// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — HTTP server framework (Phase 1 ★)
//
// SPEC §11 Phase 1 / §10 server.h
//   Thin, project-shaped wrapper around `cpp-httplib::Server` that
//   provides everything Phase 1 promises:
//     - Routing registration with a typed JSON body parser
//     - CORS preflight + headers driven by CorsConfig
//     - Unified JSON response helpers (success / error envelope) —
//       re-exported from src/routes/error_handler.h, the SPEC §5.7
//       single source of truth. Handlers should keep `#include
//       "server.h"` and the existing call sites stay unchanged.
//     - Multi-threaded request handling via httplib::ThreadPool
//       (size pulled from ServerConfig::thread_pool_size)
//     - Request-ID middleware (echo or generate UUID v4 →
//       X-Request-Id header + per-thread log correlation)
//     - Per-request structured access logging via litecode::logger()
//     - Static file serving via set_mount_point() (used by Phase 5 web/)
//     - Pre-routing hook for future rate-limit / auth middleware
//     - 404 / 500 catch-all that uses error_handler.h's envelope
//       builder so unrouted requests and unhandled exceptions both
//       come back in the unified format.
//
// The wrapper is deliberately header-only: every other Phase 1 module is
// a single header, and adding a server.cpp + a separate library target
// for a thin wrapper adds friction with no payoff. Tests link it directly.
//
// Usage (from src/main.cpp, Phase 1 boot path):
//   int main() {
//       const auto& cfg = litecode::init_config(".env");
//       litecode::init_logger(cfg.logging);
//
//       litecode::HttpServer server(cfg.server, cfg.cors);
//       server.get ("/api/v1/health", health_handler);
//       server.post("/api/v1/auth/register", register_handler);
//       // ...more routes...
//       return server.listen_blocking();
//   }
//
// Usage (from gtest, in-process — no real network listener):
//   litecode::HttpServer server({.port = 0}, /*cors=*/{});
//   server.get("/echo", [](auto& req, auto& res) {
//       res.json(200, {{"path", req.path}});
//   });
//   auto port = server.bind_any_port();           // binds ephemeral port
//   server.start(/*background=*/true);
//   httplib::Client c("127.0.0.1", port);
//   auto r = c.Get("/echo");
//   EXPECT_EQ(r->status, 200);
//   server.stop();

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"                          // ServerConfig / CorsConfig
#include "logger.h"                          // RequestIdScope / LOG_*
#include "middleware/request_id.h"           // apply_request_id_header / request_id_header_name
#include "routes/error_handler.h"            // ErrorCode / send_error / make_error_envelope / ApiException
#include "utils/security.h"                  // apply_security_headers (SPEC §15 — Phase 6 ★ v1.2.45)
#include "utils/uuid.h"                      // generate_uuid_v4 (via request_id.h)

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
//
//  HttpServerError is the framework-internal exception (e.g. thrown from
//  the constructor when set_mount_point() fails). The user-facing error
//  model — unified envelope, SPEC §5.7 codes — lives in
//  routes/error_handler.h and is re-exported below via the include at
//  the top of this file.
// ────────────────────────────────────────────────────────────────────────────

class HttpServerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Re-export of the unified error envelope (SPEC §5.7)
//
//  The catalog and helpers live in routes/error_handler.h. Including
//  server.h pulls them into litecode:: automatically, so existing
//  call sites like `litecode::send_error(res, 400, ErrorCode::INVALID_INPUT,
//  "...")` keep compiling without churn. The two re-exports below add
//  documentation breadcrumbs that pin this contract to the file callers
//  are most likely to read.
// ────────────────────────────────────────────────────────────────────────────

// make_error_envelope — see routes/error_handler.h. Pulled in via
// `#include "routes/error_handler.h"` above; do NOT redefine here.
// (No-op alias left as a comment so future readers know where to look.)

// send_error / send_success / send_created / send_no_content /
// parse_json_body — also from routes/error_handler.h. Do NOT redefine.

// ────────────────────────────────────────────────────────────────────────────
//  CORS helpers (SPEC §5.7)
//
//  We allow configured origins explicitly. The preflight response caches
//  for 1 day; credentials are allowed when the config says so.
// ────────────────────────────────────────────────────────────────────────────

class CorsPolicy {
public:
    explicit CorsPolicy(const CorsConfig& cfg) : cfg_(cfg) {}

    // true ⇔ `origin` is in the allow-list (case-insensitive comparison).
    bool is_allowed(std::string_view origin) const {
        ensure_parsed();
        if (origin.empty()) return false;
        for (const auto& allowed : allowed_set_) {
            if (allowed.size() == origin.size()
                && std::equal(allowed.begin(), allowed.end(),
                              origin.begin(),
                              [](char a, char b) {
                                  return std::tolower(static_cast<unsigned char>(a))
                                      == std::tolower(static_cast<unsigned char>(b));
                              })) {
                return true;
            }
        }
        return false;
    }

    // Apply response headers for an allowed origin. No-op if origin is
    // not in the allow-list, or if origin is empty (same-origin request).
    void apply(httplib::Response& res, std::string_view origin) const {
        ensure_parsed();
        if (origin.empty() || !is_allowed(origin)) return;
        res.set_header("Access-Control-Allow-Origin", std::string(origin));
        res.set_header("Vary", "Origin");
        if (cfg_.allow_credentials) {
            res.set_header("Access-Control-Allow-Credentials", "true");
        }
        res.set_header("Access-Control-Allow-Methods",
                       "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                       std::string("Content-Type, Authorization, ") + std::string(request_id_header_name()));
        res.set_header("Access-Control-Expose-Headers",
                       std::string(request_id_header_name()));
        res.set_header("Access-Control-Max-Age", "86400");
    }

    // List of parsed origins (for tests / diagnostics).
    const std::vector<std::string>& allowed_origins() const {
        ensure_parsed();
        return allowed_set_;
    }

    const CorsConfig& config() const { return cfg_; }

private:
    CorsConfig               cfg_;
    mutable std::vector<std::string> allowed_set_;
    mutable std::once_flag   parsed_;

    void ensure_parsed() const {
        std::call_once(parsed_, [this]{
            std::stringstream ss(cfg_.allowed_origins);
            std::string item;
            while (std::getline(ss, item, ',')) {
                std::size_t a = 0, b = item.size();
                while (a < b && std::isspace(static_cast<unsigned char>(item[a]))) ++a;
                while (b > a && std::isspace(static_cast<unsigned char>(item[b - 1]))) --b;
                if (a < b) allowed_set_.emplace_back(item.substr(a, b - a));
            }
        });
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  SyncTaskQueue — runs jobs inline on the listener thread.
//
//  Why not httplib::ThreadPool? Because cpp-httplib deletes the
//  TaskQueue* via unique_ptr<TaskQueue> the moment listen_internal()
//  returns. With httplib::ThreadPool that delete races against our
//  HttpServer's destructor members, and the resulting lifetime
//  juggling turned into a STATUS_HEAP_CORRUPTION (0xc0000374) at
//  teardown on Windows. A synchronous queue has no threads to join
//  and no lifetime questions, at the cost of one-request-at-a-time
//  concurrency — which is fine for tests, health probes, and any
//  single-tenant dev box. When SPEC §11 calls for real concurrency
//  we can swap this for a properly-owned pool behind the same
//  TaskQueue interface.
// ────────────────────────────────────────────────────────────────────────────

class SyncTaskQueue : public httplib::TaskQueue {
public:
    bool enqueue(std::function<void()> fn) override {
        if (fn) fn();
        return true;
    }
    void shutdown() override {}
};

class HttpServer {
public:
    // Construct from ServerConfig + CorsConfig. ThreadPool size comes
    // from ServerConfig::thread_pool_size (default 8).
    HttpServer(const ServerConfig& server_cfg, const CorsConfig& cors_cfg)
        : server_(std::make_unique<httplib::Server>()),
          cors_(cors_cfg),
          thread_pool_size_(server_cfg.thread_pool_size > 0
                              ? static_cast<std::size_t>(server_cfg.thread_pool_size)
                              : static_cast<std::size_t>(8)),
          host_(server_cfg.host),
          port_(server_cfg.port) {
        wire_middleware();
    }

    ~HttpServer() {
        stop();
        if (listen_thread_.joinable()) listen_thread_.join();
        // sync_task_queue_ is a NON-OWNING handle. cpp-httplib deletes
        // the SyncTaskQueue via its own std::unique_ptr<TaskQueue>
        // inside listen_internal() the moment listen_internal returns.
        // We deliberately DO NOT delete it here — that would be a
        // double-free and crash with STATUS_HEAP_CORRUPTION.
        sync_task_queue_ = nullptr;
    }

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // ── Route registration ────────────────────────────────────────────────
    //
    // Each returns HttpServer& so calls can be chained. Handlers are
    // wrapped in a RequestIdScope so that any LOG_* call inside them
    // — and the standard access-log line — automatically carries the
    // same X-Request-Id the response header carries.
    //
    // Handlers keep the standard cpp-httplib signature so we can swap
    // libraries later with zero call-site churn.

    using Handler     = httplib::Server::Handler;
    using HandlerResp = httplib::Server::HandlerWithResponse;

    HttpServer& get    (const std::string& pattern, Handler h) {
        server_->Get    (pattern, wrap(std::move(h))); return *this;
    }
    HttpServer& post   (const std::string& pattern, Handler h) {
        server_->Post   (pattern, wrap(std::move(h))); return *this;
    }
    HttpServer& put    (const std::string& pattern, Handler h) {
        server_->Put    (pattern, wrap(std::move(h))); return *this;
    }
    HttpServer& patch  (const std::string& pattern, Handler h) {
        server_->Patch  (pattern, wrap(std::move(h))); return *this;
    }
    HttpServer& del    (const std::string& pattern, Handler h) {
        server_->Delete (pattern, wrap(std::move(h))); return *this;
    }
    HttpServer& options(const std::string& pattern, Handler h) {
        server_->Options(pattern, wrap(std::move(h))); return *this;
    }

    HttpServer& get    (const std::string& pattern, HandlerResp h) {
        server_->Get    (pattern, wrap_resp(std::move(h))); return *this;
    }
    HttpServer& post   (const std::string& pattern, HandlerResp h) {
        server_->Post   (pattern, wrap_resp(std::move(h))); return *this;
    }
    HttpServer& put    (const std::string& pattern, HandlerResp h) {
        server_->Put    (pattern, wrap_resp(std::move(h))); return *this;
    }
    HttpServer& patch  (const std::string& pattern, HandlerResp h) {
        server_->Patch  (pattern, wrap_resp(std::move(h))); return *this;
    }
    HttpServer& del    (const std::string& pattern, HandlerResp h) {
        server_->Delete (pattern, wrap_resp(std::move(h))); return *this;
    }
    HttpServer& options(const std::string& pattern, HandlerResp h) {
        server_->Options(pattern, wrap_resp(std::move(h))); return *this;
    }

    // ── Static files (SPEC §6: web/ served from /) ────────────────────────
    HttpServer& mount(const std::string& mount_point, const std::string& dir) {
        if (!server_->set_mount_point(mount_point, dir)) {
            throw HttpServerError("HttpServer: mount_point '" + mount_point
                                  + "' → '" + dir + "' failed");
        }
        return *this;
    }

    // ── Pre-routing hook (Phase 2 will plug rate-limit here) ──────────────
    HttpServer& set_pre_routing(HandlerResp h) {
        server_->set_pre_routing_handler(std::move(h));
        return *this;
    }

    // ── Error handler (replaces default 404/500 body with our envelope) ───
    HttpServer& set_error_handler(Handler h) {
        server_->set_error_handler(std::move(h));
        return *this;
    }

    // ── Default headers (always added to every response) ─────────────────
    HttpServer& set_default_headers(httplib::Headers headers) {
        server_->set_default_headers(std::move(headers));
        return *this;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    //
    // Three boot modes:
    //
    //   bind_to_port() + listen_blocking()    — production path; blocking
    //                                            until stop().
    //   bind_any_port()   + start(true)       — test path; non-blocking
    //                                            background thread.
    //   listen(host, port, /*block=*/true)    — convenience wrapper.

    bool bind_to_port(const std::string& host, int port) {
        host_ = host;
        port_ = static_cast<std::uint16_t>(port);
        return server_->bind_to_port(host_, port_);
    }

    int bind_any_port(const std::string& host = "127.0.0.1") {
        host_ = host;
        const int p = server_->bind_to_any_port(host_);
        if (p > 0) port_ = static_cast<std::uint16_t>(p);
        return p;
    }

    bool is_running() const { return server_->is_running(); }

    // Background thread; non-blocking. Returns immediately. Spins until
    // the socket is accepting (max ~2s) so callers can immediately fire
    // requests at it without a race.
    bool start(bool background = true) {
        if (background) {
            if (listen_thread_.joinable()) listen_thread_.join();
            listen_thread_ = std::thread([this]{
                server_->listen_after_bind();
            });
            // v1.2.48: was 2s — too tight for docker-internal
            // networks where bind/listen can take 5-15s on cold
            // start (esp. when warm_pool precreates in parallel).
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!server_->is_running() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return server_->is_running();
        }
        return server_->listen_after_bind();
    }

    // Blocking listen using the host/port stashed by bind_*().
    bool listen_blocking() {
        return server_->listen(host_, port_);
    }

    // Convenience: bind + listen blocking in one call.
    bool listen(const std::string& host, int port) {
        if (!bind_to_port(host, port)) return false;
        return server_->listen_after_bind();
    }

    void stop() {
        if (server_) server_->stop();
    }

    void wait_until_ready() const {
        if (server_) server_->wait_until_ready();
    }

    // ── Inspection ────────────────────────────────────────────────────────
    const std::string& host() const { return host_; }
    std::uint16_t      port() const { return port_; }
    std::size_t        thread_pool_size() const { return thread_pool_size_; }
    const CorsPolicy&  cors() const { return cors_; }

    // Direct access for advanced use (e.g. tests that need to inspect
    // registered handlers). Prefer the typed wrappers above.
    httplib::Server&       raw()       { return *server_; }
    const httplib::Server& raw() const { return *server_; }

private:
    // Wrap a plain Handler so the per-request RequestIdScope is active
    // for the full duration of the handler call. We pull the id from
    // the X-Request-Id response header (set by the pre-routing hook).
    //
    // ApiException is caught here and converted into the unified
    // envelope via respond(). Any other exception is intentionally
    // left to propagate to cpp-httplib — the set_error_handler below
    // turns it into a 500 in our format. Catching std::exception
    // here would swallow programmer errors and obscure stack traces.
    Handler wrap(Handler h) const {
        return [inner = std::move(h)](const httplib::Request& req,
                                      httplib::Response&       res) {
            const std::string rid = res.get_header_value("X-Request-Id");
            litecode::RequestIdScope scope(rid);
            try {
                inner(req, res);
            } catch (const ApiException& e) {
                e.respond(res);
            }
        };
    }

    // Same idea for the Handled/Unhandled variant. Returning the
    // inner function's return value is safe — only ApiException (which
    // already wrote the body) takes the catch branch.
    HandlerResp wrap_resp(HandlerResp h) const {
        return [inner = std::move(h)](const httplib::Request& req,
                                      httplib::Response&       res)
                  -> httplib::Server::HandlerResponse {
            const std::string rid = res.get_header_value("X-Request-Id");
            litecode::RequestIdScope scope(rid);
            try {
                return inner(req, res);
            } catch (const ApiException& e) {
                e.respond(res);
                return httplib::Server::HandlerResponse::Handled;
            }
        };
    }

    void wire_middleware() {
        // Plug the per-request task queue. Heap-allocate the
        // SyncTaskQueue; cpp-httplib's std::unique_ptr<TaskQueue>
        // inside listen_internal() takes ownership of the returned
        // raw pointer and will delete it when listen_internal returns.
        // We must NOT also delete it from our side — that would be a
        // double-free and crash with STATUS_HEAP_CORRUPTION.
        sync_task_queue_ = new SyncTaskQueue();
        auto* queue = sync_task_queue_;
        server_->new_task_queue = [queue]{ return queue; };

        // 1. Pre-routing: stamp the per-thread request_id so every log
        //    line emitted while this request is being handled carries
        //    the same correlation token. Also attach CORS headers to
        //    every response, stamp the SPEC §15 security response
        //    headers (defense in depth alongside the Caddyfile), and
        //    answer OPTIONS preflight short-circuit.
        server_->set_pre_routing_handler(
            [this](const httplib::Request& req, httplib::Response& res)
                -> httplib::Server::HandlerResponse {
                // request_id.h owns the resolution policy (echo valid /
                // generate UUID v4 fallback) — keep the policy in one
                // place so test_request_id + test_server agree.
                const std::string rid = apply_request_id_header(
                    req.get_header_value("X-Request-Id"), res);

                const std::string origin = req.get_header_value("Origin");
                cors_.apply(res, origin);

                // SPEC §15 / Phase 6 ★ v1.2.45: baseline security response
                // headers. The Caddyfile sets the same set on the
                // reverse-proxy path; this hook covers a developer
                // running litecode-cpp without Caddy in front (e.g. the
                // dev-box `python -m http.server` style fallback). Headers
                // are idempotent: set_header overwrites a prior value, so
                // a route handler that needs a custom value can call
                // apply_security_headers(...) again or set_header(...)
                // after this hook runs.
                //
                // Only stamp the strict CSP (`default-src 'none'`) on API
                // responses — those are JSON, so the browser must not load
                // any sub-resource from them. HTML pages already declare
                // their own CSP via the <meta> tag in web/*.html; if we
                // also sent the strict CSP via the HTTP header here, the
                // browser would intersect the two policies and apply the
                // stricter one — `default-src 'none'` — which would block
                // every script/stylesheet/fetch the SPA needs and the
                // page would render with 0 items / "加载失败" forever.
                // (v1.3.1 fix: this hook used to apply uniformly, which
                // made direct :8080 access (no Caddy in front) silently
                // break the homepage.)
                if (req.path.rfind("/api/", 0) == 0) {
                    litecode::security::apply_security_headers(res);
                }

                // OPTIONS preflight short-circuit. Real route handlers
                // never see OPTIONS.
                if (req.method == "OPTIONS") {
                    res.status = cors_.is_allowed(origin) ? 204 : 403;
                    return httplib::Server::HandlerResponse::Handled;
                }

                return httplib::Server::HandlerResponse::Unhandled;
            });

        // 2. Per-request access log. Intentionally a no-op by default —
        //    tests don't want the chatter, and callers can override via
        //    server_->set_logger() (which our API exposes for power users).
        server_->set_logger(
            [this](const httplib::Request&, const httplib::Response&) {
                (void)this;
            });

        // 3. Replace 404 / 500 bodies with our unified envelope.
        //
        // cpp-httplib invokes this handler for EVERY response with
        // status >= 400 — not just for unrouted requests — so we must
        // skip the override when the route handler has already set a
        // body (e.g. send_error() inside a handler).
        //
        // The status → ErrorCode / message mapping now lives in
        // routes/error_handler.h (default_error_for_status +
        // default_message_for_status), so this block stays one line
        // of policy: "did the handler already emit our envelope? if
        // not, fall back to the default for this status."
        server_->set_error_handler(
            [this](const httplib::Request& req, httplib::Response& res)
                -> httplib::Server::HandlerResponse {
                const std::string rid = res.get_header_value("X-Request-Id");
                litecode::RequestIdScope scope(rid);

                if (!res.body.empty()
                    && res.get_header_value("Content-Type").find("application/json")
                           != std::string::npos) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }

                const int status = res.status;
                const ErrorCode code = default_error_for_status(status);
                const std::string_view msg = default_message_for_status(status);

                nlohmann::json env = make_error_envelope(code, msg);
                res.set_content(env.dump(),
                                "application/json; charset=utf-8");
                return httplib::Server::HandlerResponse::Handled;
            });
    }

    std::unique_ptr<httplib::Server>     server_;
    SyncTaskQueue*                       sync_task_queue_ = nullptr;  // non-owning
    CorsPolicy                           cors_;
    std::size_t                          thread_pool_size_;
    std::string                          host_;
    std::uint16_t                        port_;
    std::thread                          listen_thread_;
};

} // namespace litecode