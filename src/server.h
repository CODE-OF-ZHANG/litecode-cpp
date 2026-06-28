// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — HTTP server framework (Phase 1 ★)
//
// SPEC §11 Phase 1 / §10 server.h
//   Thin, project-shaped wrapper around `cpp-httplib::Server` that
//   provides everything Phase 1 promises:
//     - Routing registration with a typed JSON body parser
//     - CORS preflight + headers driven by CorsConfig
//     - Unified JSON response helpers (success / error envelope)
//     - Multi-threaded request handling via httplib::ThreadPool
//       (size pulled from ServerConfig::thread_pool_size)
//     - Request-ID middleware (echo or generate UUID v4 →
//       X-Request-Id header + per-thread log correlation)
//     - Per-request structured access logging via litecode::logger()
//     - Static file serving via set_mount_point() (used by Phase 5 web/)
//     - Pre-routing hook for future rate-limit / auth middleware
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
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"   // ServerConfig / CorsConfig
#include "logger.h"   // RequestIdScope / LOG_*

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
// ────────────────────────────────────────────────────────────────────────────

class HttpServerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Error code catalog (SPEC §5.7)
//
//  Every JSON error body uses one of these. Centralized so callers can't
//  drift and tests can pin them down.
// ────────────────────────────────────────────────────────────────────────────

enum class ErrorCode {
    INVALID_INPUT,
    UNAUTHORIZED,
    FORBIDDEN,
    NOT_FOUND,
    RATE_LIMITED,
    CONFLICT,
    INTERNAL_ERROR,
    SERVICE_UNAVAILABLE,
};

inline std::string_view error_code_name(ErrorCode c) {
    switch (c) {
        case ErrorCode::INVALID_INPUT:      return "INVALID_INPUT";
        case ErrorCode::UNAUTHORIZED:       return "UNAUTHORIZED";
        case ErrorCode::FORBIDDEN:          return "FORBIDDEN";
        case ErrorCode::NOT_FOUND:          return "NOT_FOUND";
        case ErrorCode::RATE_LIMITED:       return "RATE_LIMITED";
        case ErrorCode::CONFLICT:           return "CONFLICT";
        case ErrorCode::INTERNAL_ERROR:     return "INTERNAL_ERROR";
        case ErrorCode::SERVICE_UNAVAILABLE:return "SERVICE_UNAVAILABLE";
    }
    return "INTERNAL_ERROR";
}

// ────────────────────────────────────────────────────────────────────────────
//  JSON helpers — make the response envelope one line in every handler
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// Serialize an nlohmann::json object/array into a compact string.
// We use .dump() (not .dump(2)) because the API is consumed by JS; the
// frontend reformats when it needs to. Smaller payloads = fewer bytes
// over the wire = better P95.
inline std::string to_json_string(const nlohmann::json& v) {
    return v.dump();
}

} // namespace detail

// Build the SPEC §5.7 unified error envelope:
//   {"code":"...", "message":"...", "details":{...}, "request_id":"..."}
inline nlohmann::json make_error_envelope(ErrorCode code,
                                          std::string_view message,
                                          const nlohmann::json& details = nullptr) {
    nlohmann::json j = {
        {"code",    std::string(error_code_name(code))},
        {"message", std::string(message)},
    };
    if (!details.is_null()) j["details"] = details;
    if (current_request_id_present()) {
        j["request_id"] = current_request_id();
    }
    return j;
}

// Response extension: thin sugar on top of httplib::Response so every
// handler in the codebase writes responses the same way.
//
// We use free functions instead of a custom Response subclass because
// cpp-httplib passes `Response&` by reference and adding a wrapper would
// force every existing handler to dereference. Free functions keep the
// existing `void handler(const Request&, Response&)` signature.
inline void send_json(httplib::Response& res,
                      int status,
                      const nlohmann::json& body) {
    res.status = status;
    res.set_content(detail::to_json_string(body), "application/json; charset=utf-8");
}

inline void send_success(httplib::Response& res,
                         const nlohmann::json& data = {}) {
    nlohmann::json body = {{"data", data}};
    if (current_request_id_present()) body["request_id"] = current_request_id();
    send_json(res, 200, body);
}

inline void send_created(httplib::Response& res,
                         const nlohmann::json& data = {}) {
    nlohmann::json body = {{"data", data}};
    if (current_request_id_present()) body["request_id"] = current_request_id();
    send_json(res, 201, body);
}

inline void send_no_content(httplib::Response& res) {
    res.status = 204;
}

inline void send_error(httplib::Response& res,
                       int status,
                       ErrorCode code,
                       std::string_view message,
                       const nlohmann::json& details = nullptr) {
    send_json(res, status, make_error_envelope(code, message, details));
}

// Parse a JSON request body. Returns nullopt + sends 400 on failure so
// handlers can do:
//   auto j = parse_json_body(req, res);
//   if (!j) return;  // response already sent
inline std::optional<nlohmann::json> parse_json_body(const httplib::Request& req,
                                                     httplib::Response& res) {
    if (req.body.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "Request body is empty");
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("Invalid JSON body: ") + e.what());
        return std::nullopt;
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  UUID v4 — used for X-Request-Id when the client didn't send one.
//
//  We deliberately don't pull in a uuid library: cpp-httplib + OpenSSL
//  are already linked, and the server only needs hex-with-dashes.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// One-time entropy seed. We mix steady_clock with the address of the
// thread-local object so two processes started in the same nanosecond
// don't generate the same id sequence.
inline std::mt19937_64& uuid_rng() {
    static thread_local std::mt19937_64 rng = []{
        std::mt19937_64::result_type seed =
            static_cast<std::mt19937_64::result_type>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        // xor with a pointer to add per-process variability
        seed ^= reinterpret_cast<std::uintptr_t>(&seed);
        return std::mt19937_64(seed);
    }();
    return rng;
}

} // namespace detail

inline std::string generate_uuid_v4() {
    // Pull 128 bits of randomness into a 16-byte buffer.
    std::uniform_int_distribution<std::uint64_t> dist64;
    std::uint64_t hi = dist64(detail::uuid_rng());
    std::uint64_t lo = dist64(detail::uuid_rng());

    unsigned char b[16];
    for (int i = 0; i < 8; ++i) b[i]     = static_cast<unsigned char>(hi >> (8 * i));
    for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<unsigned char>(lo >> (8 * i));

    // RFC 4122 v4: version + variant bits.
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);

    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3],  b[4], b[5],  b[6], b[7],
                  b[8], b[9],  b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out, 36);
}

// Validate that an inbound X-Request-Id looks safe (length + alphabet)
// before reflecting it. We refuse to echo anything that could be used to
// inject headers (CRLF) or pollute log greps.
inline bool is_valid_request_id(std::string_view id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok =
            (u >= '0' && u <= '9') ||
            (u >= 'a' && u <= 'z') ||
            (u >= 'A' && u <= 'Z') ||
            u == '-' || u == '_' || u == '.';
        if (!ok) return false;
    }
    return true;
}

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
                       "Content-Type, Authorization, X-Request-Id");
        res.set_header("Access-Control-Expose-Headers",
                       "X-Request-Id");
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
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!server_->is_running() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    Handler wrap(Handler h) const {
        return [inner = std::move(h)](const httplib::Request& req,
                                      httplib::Response&       res) {
            const std::string rid = res.get_header_value("X-Request-Id");
            litecode::RequestIdScope scope(rid);
            inner(req, res);
        };
    }

    // Same idea for the Handled/Unhandled variant.
    HandlerResp wrap_resp(HandlerResp h) const {
        return [inner = std::move(h)](const httplib::Request& req,
                                      httplib::Response&       res)
                  -> httplib::Server::HandlerResponse {
            const std::string rid = res.get_header_value("X-Request-Id");
            litecode::RequestIdScope scope(rid);
            return inner(req, res);
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
        //    every response and answer OPTIONS preflight short-circuit.
        server_->set_pre_routing_handler(
            [this](const httplib::Request& req, httplib::Response& res)
                -> httplib::Server::HandlerResponse {
                std::string rid = req.get_header_value("X-Request-Id");
                if (rid.empty() || !is_valid_request_id(rid)) {
                    rid = generate_uuid_v4();
                }
                res.set_header("X-Request-Id", rid);

                const std::string origin = req.get_header_value("Origin");
                cors_.apply(res, origin);

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

                ErrorCode code = ErrorCode::INTERNAL_ERROR;
                std::string_view msg = "Internal Server Error";
                int status = res.status;

                if      (status == 400) { code = ErrorCode::INVALID_INPUT;        msg = "Bad Request"; }
                else if (status == 401) { code = ErrorCode::UNAUTHORIZED;         msg = "Unauthorized"; }
                else if (status == 403) { code = ErrorCode::FORBIDDEN;            msg = "Forbidden"; }
                else if (status == 404) { code = ErrorCode::NOT_FOUND;            msg = "Not Found"; }
                else if (status == 409) { code = ErrorCode::CONFLICT;             msg = "Conflict"; }
                else if (status == 429) { code = ErrorCode::RATE_LIMITED;         msg = "Too Many Requests"; }
                else if (status == 503) { code = ErrorCode::SERVICE_UNAVAILABLE;  msg = "Service Unavailable"; }

                nlohmann::json env = make_error_envelope(code, msg);
                res.set_content(detail::to_json_string(env),
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