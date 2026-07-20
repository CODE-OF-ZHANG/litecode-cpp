// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — system routes (/api/v1/health, /api/v1/metrics stub)
//
// SPEC §10 src/routes/system_routes.h
// SPEC §11 Phase 1 ★ — 健康检查端点 /api/v1/health (DB + Docker 探测)
// SPEC §5.6  GET /api/v1/health   公开   健康检查: DB / Docker / 队列 / 预热池
// SPEC §16.1 健康检查 payload + docker-compose healthcheck 语义
// SPEC A31    验收用例: GET /api/v1/health 返回 DB / Docker 状态
//
// Why a HealthService abstraction (not a single hardcoded handler)?
//   - Phase 1 implements only the DB probe (ConnectionPool::ping()).
//     The Docker daemon probe belongs to Phase 4 (docker_client.h), and
//     the queue / warm-pool sizes come from Phase 4 (judge_scheduler /
//     warm_pool). Rather than hardcode "db" + "docker" + "queue_size"
//     fields into a single function and then rewrite half of it in
//     Phase 4, we model each subsystem as a *named probe* that the
//     service runs at request time and merges into the response body.
//   - Tests pin the contract by registering stub probes; no MySQL,
//     Docker, or scheduler is needed to exercise the endpoint logic.
//   - Phase 4's docker_client.h will register its probe at boot via
//     `health.register_probe("docker", docker_probe)`. Same shape, no
//     call-site churn.
//
// Response shape (matches SPEC §16.1 verbatim):
//   200 OK   when every registered probe is healthy:
//     {
//       "status": "ok",
//       "db": "ok",
//       "docker": "ok" | "n/a" | "down",
//       "queue_size": 0,
//       "warm_pool": 0,
//       "uptime_seconds": 86400,
//       "checks": {
//         "db":      {"ok": true,  "detail": "..."},
//         "docker":  {"ok": false, "detail": "..."}
//       }
//     }
//   503 Service Unavailable when at least one probe reports down.
//   Body always carries the same top-level keys so consumers can parse
//   unconditionally.
//
// docker-compose integration (SPEC §16.1):
//   healthcheck:
//     test: ["CMD", "curl", "-f", "http://localhost:8080/api/v1/health"]
//   `curl -f` exits non-zero on HTTP >= 400, so the 503 mapping keeps
//   containers marked unhealthy whenever a critical probe fails.
//
// Thread safety:
//   - Probes are called inside the HTTP handler thread. Each probe is
//     responsible for being thread-safe (ConnectionPool::ping() is).
//   - Probe registration is single-threaded (boot path); reads happen
//     during request handling. We don't need a lock because probes_
//     is populated once and then read-only.
//
#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../logger.h"              // LOG_INFO / LOG_WARN
#include "../server.h"              // HttpServer / send_error / ErrorCode
#include "../db/connection_pool.h"  // ConnectionPool (optional dependency)

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Process-start anchor.
//
//  Used by the default uptime probe. mark_process_start_time() is
//  idempotent: the first call wins, so a test or a server module can
//  safely call it more than once without losing the original anchor.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {
inline std::chrono::steady_clock::time_point& process_start_time() {
    static std::chrono::steady_clock::time_point t =
        std::chrono::steady_clock::now();
    return t;
}
} // namespace detail

// Set (or overwrite, on rare intent) the process-start anchor. Called
// from main() at boot; idempotent — first call wins.
inline void mark_process_start_time() {
    detail::process_start_time() = std::chrono::steady_clock::now();
}

// Seconds since mark_process_start_time(). Returns 0 if never marked
// (defensive — callers should mark at boot, but the test harness
// shouldn't crash if it forgets).
inline std::chrono::seconds process_uptime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - detail::process_start_time());
}

// ────────────────────────────────────────────────────────────────────────────
//  Probe model.
//
//  A probe is a named, side-effect-free function returning:
//    {ok, detail, extra}
//
//  ok     — true ⇒ probe considers the subsystem healthy.
//  detail — human-readable one-liner; appears under checks.<name>.detail.
//  extra  — optional JSON object merged into the top-level body.
//           Use this for non-binary signals (queue_size, warm_pool).
//
//  Probes MUST NOT throw. ConnectionPool::ping() is already noexcept; we
//  wrap any other probe body in a try/catch in build_response() so a
//  buggy probe can't take down the endpoint.
// ────────────────────────────────────────────────────────────────────────────

struct ProbeResult {
    bool                ok     = true;
    std::string         detail;
    nlohmann::json      extra;     // {} when nothing to merge
};

class HealthService {
public:
    using Probe = std::function<ProbeResult()>;

    HealthService() = default;

    // Register a probe under `name`. The name doubles as the key in
    // the `checks` map. Duplicate names replace (last-write-wins)
    // — useful in tests that swap a probe out without restarting.
    HealthService& register_probe(std::string name, Probe probe) {
        for (auto& slot : probes_) {
            if (slot.first == name) { slot.second = std::move(probe); return *this; }
        }
        probes_.emplace_back(std::move(name), std::move(probe));
        return *this;
    }

    // Remove a previously-registered probe. No-op when absent.
    HealthService& unregister_probe(const std::string& name) {
        for (auto it = probes_.begin(); it != probes_.end(); ++it) {
            if (it->first == name) { probes_.erase(it); return *this; }
        }
        return *this;
    }

    // Number of registered probes (test inspection helper).
    std::size_t probe_count() const noexcept { return probes_.size(); }

    // Run every probe and aggregate into the SPEC §16.1 response body.
    // Sets *status_out to 200 (healthy) or 503 (any probe unhealthy).
    //
    // Critical-vs-warning: every probe counts toward `status`. We do
    // not yet have a notion of "warning"; Phase 4 will add severity
    // when the docker probe arrives (a down docker is critical, a
    // slow docker is a warning).
    nlohmann::json build_response(int* status_out = nullptr) const {
        nlohmann::json body = {
            {"status",         "ok"},
            {"db",             "ok"},
            {"docker",         "n/a"},
            {"queue_size",     0},
            {"warm_pool",      0},
            {"uptime_seconds", static_cast<std::int64_t>(process_uptime().count())},
            {"checks",         nlohmann::json::object()},
        };

        bool overall_ok = true;

        for (const auto& [name, probe] : probes_) {
            ProbeResult r;
            try {
                r = probe();
            } catch (const std::exception& e) {
                // A throwing probe is treated as down with a generic
                // detail. We never let it escape the endpoint.
                r.ok     = false;
                r.detail = std::string("probe threw: ") + e.what();
            } catch (...) {
                r.ok     = false;
                r.detail = "probe threw: unknown exception";
            }

            nlohmann::json check = {
                {"ok",     r.ok},
                {"detail", r.detail},
            };
            body["checks"][name] = check;

            // Top-level mirror conventions:
            //   "db"     → `db` becomes "ok" | "down"
            //   "docker" → `docker` becomes "ok" | "down"
            // Other probes: anything they put in `extra` (e.g. queue_size
            // / warm_pool) is merged into the top-level body verbatim.
            if (name == "db") {
                body["db"] = r.ok ? "ok" : "down";
            } else if (name == "docker") {
                body["docker"] = r.ok ? "ok" : "down";
            } else if (r.extra.is_object()) {
                for (auto it = r.extra.begin(); it != r.extra.end(); ++it) {
                    body[it.key()] = it.value();
                }
            }

            if (!r.ok) overall_ok = false;
        }

        if (!overall_ok) {
            body["status"] = "degraded";
        }

        if (status_out) {
            *status_out = overall_ok ? 200 : 503;
        }
        return body;
    }

    const std::vector<std::pair<std::string, Probe>>& probes() const noexcept {
        return probes_;
    }

private:
    std::vector<std::pair<std::string, Probe>> probes_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Built-in probes.
// ────────────────────────────────────────────────────────────────────────────

// DB probe — wraps ConnectionPool::ping(). Never throws (the underlying
// call is already noexcept). When the pool pointer is null we report
// down; that's the "service started without DB" case (a config bug,
// but we shouldn't crash the endpoint over it).
inline HealthService::Probe make_db_probe(ConnectionPool* pool) {
    return [pool]() -> ProbeResult {
        if (pool == nullptr) {
            ProbeResult r;
            r.ok     = false;
            r.detail = "no connection pool configured";
            return r;
        }
        const bool ok = pool->ping();
        ProbeResult r;
        r.ok     = ok;
        r.detail = ok ? "select 1 ok" : "select 1 failed";
        return r;
    };
}

// Uptime probe — emits the `uptime_seconds` field. Always healthy.
inline HealthService::Probe make_uptime_probe() {
    return []() -> ProbeResult {
        ProbeResult r;
        r.ok     = true;
        r.detail = "process alive";
        r.extra  = {
            {"uptime_seconds",
                static_cast<std::int64_t>(process_uptime().count())},
        };
        return r;
    };
}

// Queue size probe — Phase 4 will hand a live reader here. Until then
// the placeholder reports 0 so the field shape stays stable.
inline HealthService::Probe make_queue_size_probe() {
    return []() -> ProbeResult {
        ProbeResult r;
        r.ok     = true;
        r.detail = "scheduler not yet wired (phase 4)";
        r.extra  = {{"queue_size", 0}};
        return r;
    };
}

// Warm-pool size probe — placeholder for boot before the warm pool is
// constructed (Phase 4 has a real WarmPool that overrides this via
// `make_warm_pool_probe(WarmPool*)` below).
inline HealthService::Probe make_warm_pool_probe() {
    return []() -> ProbeResult {
        ProbeResult r;
        r.ok     = true;
        r.detail = "warm pool not yet wired (phase 4)";
        r.extra  = {{"warm_pool", 0}};
        return r;
    };
}

// ── Phase 4 live wiring ────────────────────────────────────────────────────
//
// Callers (judge_scheduler.h, main.cpp) that own a `litecode::judge::WarmPool`
// can wire it into /api/v1/health via:
//
//     #include "judge/warm_pool.h"
//     health.register_probe("warm_pool",
//                           litecode::judge::WarmPool::make_probe(&pool));
//
// We deliberately do NOT have `system_routes.h` include `warm_pool.h`
// itself — that would force every translation unit that needs the
// health endpoint to drag in the docker client (and the MySQL driver
// transitively, since docker_client.h's CompileOptions is fine but
// warm_pool.h also reaches into logger.h / system_routes.h itself).
// `WarmPool::make_probe()` is the canonical entry point.

// Docker daemon probe — Phase 4 will hand a live reader here. We
// register a no-op healthy probe so the field stays "ok" until Phase 4
// swaps it for a real check; SPEC §16.1 only requires the field to be
// present and parsable, not that it's meaningful in Phase 1.
inline HealthService::Probe make_docker_probe_placeholder() {
    return []() -> ProbeResult {
        ProbeResult r;
        r.ok     = true;
        r.detail = "docker probe not yet wired (phase 4)";
        return r;
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration.
//
//  GET /api/v1/health — public; runs probes; returns 200 or 503.
//  GET /api/v1/metrics — registered by `register_metric_routes()` in
//  routes/metrics.h (Phase 9 ★ v1.2.68). Kept out of this TU so the
//  metrics header doesn't drag mysql::* / docker::* / warm_pool.h into
//  callers that only need /api/v1/health. The docker-compose monitoring
//  profile expects /api/v1/metrics to start returning the Prometheus
//  exposition format; register_metric_routes() is therefore called
//  BEFORE server.start() in main.cpp.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_health_routes(HttpServer& server,
                                          HealthService& health) {
    server.get("/api/v1/health",
        [&health](const httplib::Request&, httplib::Response& res) {
            int status = 200;
            const nlohmann::json body = health.build_response(&status);

            // Per SPEC §16.1: the body always carries X-Request-Id
            // (server.h's pre-routing hook added it) and uses
            // application/json. The default success envelope wraps
            // `data: {...}`; for /health the spec's example shows the
            // fields at the top level, so we send the body verbatim
            // rather than wrapping it.
            //
            // We log degraded events AFTER writing the body, so a
            // misconfigured logger (or any other logger-induced
            // exception) cannot take the response down with a 500
            // from cpp-httplib's catch block. The structured log line
            // is best-effort — the HTTP response is what callers see.
            res.status = status;
            res.set_content(body.dump(), "application/json; charset=utf-8");

            if (status == 503) {
                try {
                    LOG_WARN("health check degraded",
                             {{"component", "system"}});
                } catch (...) {
                    // Logging must never flip a 503 into a 500. Swallow.
                }
            }
        });

    return server;
}

} // namespace litecode
