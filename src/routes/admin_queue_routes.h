// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — admin judge-queue status route (Phase 6 ★)
//
// SPEC §5.5 row 4 / §11 Phase 6 / §12.1 A28 / §15.5 / §16.1:
//   - GET /api/v1/admin/queue            (🔒 admin, 60/min)
//
//       Read-only snapshot of the judge queue subsystem. Powers the
//       dedicated operator view (and a future /admin/dashboard.html
//       widget) with everything needed to spot back-pressure before
//       users start seeing 503s on /api/v1/submissions:
//
//         1. Queue (JudgeScheduler public accessors — v1.2.15 ★)
//            - size               pending tasks waiting for a worker
//            - running            tasks currently in a worker
//            - max_concurrent     worker thread ceiling (cfg.max_concurrent)
//            - max_queue_size     queue capacity (cfg.max_queue_size; 0 ⇒ unbounded)
//            - scheduler_running  start() succeeded AND not shut down
//            - utilization        size / max_queue_size (or null when unbounded)
//
//         2. Warm pool (WarmPool public accessors — v1.2.13/14 ★)
//            - size               idle containers currently in the pool
//            - target             K (the configured refill target)
//            - running            pool.start() succeeded AND not shut down
//
//         3. Docker daemon (caller-supplied probe — mirrors
//            admin_stats_routes.h's docker_probe argument). The probe
//            is a std::function<ProbeResult()> so this header doesn't
//            have to drag in docker_client.h (which transitively pulls
//            nlohmann_json + a curl handle). When the probe is empty
//            we report ok=false / detail=null — the dashboard already
//            paints "offline" for missing subsystems.
//
//         4. DB status (one tiny raw SELECT against the pool)
//            - ok                 ConnectionPool::ping()
//            - pending_submissions COUNT(*) WHERE status='pending'
//              The "pending in DB" count is intentionally separate
//              from the in-memory queue size: after a crash the DB
//              still has rows in 'pending' that the worker has lost
//              track of (a requeue sweep is a Phase 9 cron concern).
//              Surfacing both numbers lets an operator spot the
//              mismatch ("pending_in_db=12, queue_size=2") without
//              guessing.
//
//         5. updated_at (UTC ISO 8601)
//            Stamped by the handler — the only piece the route synthesizes.
//
// Wire shape (response, 200):
//   {
//     "data": {
//       "queue": {
//         "size": 3,
//         "running": 2,
//         "max_concurrent": 4,
//         "max_queue_size": 50,
//         "scheduler_running": true,
//         "utilization": 0.06
//       },
//       "warm_pool": {"size": 2, "target": 4, "running": true},
//       "docker":    {"ok": false, "detail": "..." | null},
//       "db":        {"ok": true, "pending_submissions": 5},
//       "updated_at": "2026-07-11 12:34:56"
//     },
//     "request_id": "..."
//   }
//
//   utilization is null when max_queue_size == 0 (the unbounded
//   dev-mode path used by tests; production always sets a finite
//   limit per SPEC §15.5).
//
// Failure modes:
//   - No / bad access token       → 401 UNAUTHORIZED
//   - Token valid but != admin    → 403 FORBIDDEN
//   - Rate limit tripped          → 429 RATE_LIMITED
//   - DB ping/SELECT throw        → the queue/warm_pool/docker fields
//                                   still go out (those are non-DB
//                                   probes) but db.ok flips to false
//                                   and db.pending_submissions is 0.
//                                   A 500 here would be worse than
//                                   a partial degradation — the
//                                   operator needs the queue signal
//                                   even when the DB is having a
//                                   bad day.
//   - Scheduler/pool accessor throws → those fields fall back to
//                                   0/false (matches admin_stats
//                                   policy). Probe exception ⇒
//                                   docker.{ok=false, detail=null}.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 6 admin route
//     (admin_user_routes / admin_problem_routes / admin_stats_routes /
//     admin_audit_log_routes). Tests link this header directly;
//     production ownership is `main.cpp` (or, given the ODR caveat
//     below, nothing — same as the other admin route modules).
//   - We deliberately do NOT include `user_repo.h` / `submission_repo.h` /
//     `problem_repo.h` here. Each of them defines helpers in
//     `litecode::detail` (req_string / req_int / row_to_user), and
//     pulling more than one into the same TU under MSVC's strict
//     One-Definition-Rule triggers C2084 ("function already has a
//     body"). The handful of SELECTs in detail:: helpers below
//     mirror submission_repo / connection_pool counts but live in
//     their own namespace.
//   - We deliberately do NOT include `judge/judge_scheduler.h` or
//     `judge/warm_pool.h` here — those headers drag in docker_client.h
//     + nlohmann_json + std::thread + std::filesystem, which is a
//     heavy graph for a route header to force on every translation
//     unit. We forward-declare `JudgeScheduler` + `WarmPool` and call
//     only the public noexcept accessors the accessors list below.
//   - We deliberately do NOT write to `audit_logs`. SPEC §11
//     reserves audit_logs for "key admin operations" (changes that
//     mutate data). A read-only status poll fires every few seconds
//     when wired into the dashboard — flooding audit_logs would be
//     noise that buries the real operator events.
//   - ODR caveat: this header is not registered from main.cpp for
//     the same reason admin_user_routes.h / admin_stats_routes.h /
//     admin_problem_routes.h / admin_bulk_import_routes.h /
//     admin_audit_log_routes.h aren't. End-to-end coverage is owned
//     by tests/unit/test_admin_queue.cpp.
//
// Usage (production, from main.cpp — once the cross-route ODR problem
// gets unwound, this becomes the canonical registration pattern):
//
//   litecode::HttpServer     server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter    limiter;
//   auto docker_client = litecode::docker::make_client_from_config(cfg.judge);
//   litecode::judge::WarmPool pool_docker(docker_client.get());
//   pool_docker.start(litecode::judge::make_default_warm_pool_config(cfg.judge));
//   litecode::judge::JudgeScheduler sched(
//       docker_client.get(), &pool_docker, &pool, sc);
//   sched.start();
//   litecode::register_admin_queue_routes(
//       server, pool, limiter, cfg.jwt, cfg.rate_limit,
//       /*scheduler=*/  &sched,
//       /*warm_pool=*/   &pool_docker,
//       /*docker_probe=*/ litecode::docker::make_docker_probe(docker_client.get()));
//
// Usage (test, from gtest):
//
//   litecode::register_admin_queue_routes(
//       server, pool, limiter, jwt_cfg, lax_rate_limit(),
//       /*scheduler=*/  nullptr,    // dev box has no scheduler
//       /*warm_pool=*/   nullptr,    // dev box has no warm pool
//       /*docker_probe=*/ nullptr);  // dev box has no docker

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <typeinfo>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "../config.h"                                // JwtConfig / RateLimitConfig
#include "../db/connection_pool.h"                    // ConnectionPool
#include "../logger.h"                                // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../middleware/admin_middleware.h"           // require_admin
#include "../middleware/rate_limit.h"                 // consume_rate_limit / admin_queue_quota
#include "../routes/error_handler.h"                  // send_error / ErrorCode / ApiException
#include "../routes/system_routes.h"                  // HealthService::ProbeResult

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Judge subsystem forward declarations.
//
//  We deliberately do NOT include `judge/judge_scheduler.h` or
//  `judge/warm_pool.h` here — those headers drag in docker_client.h +
//  nlohmann_json + std::thread + std::filesystem. The route handler
//  accepts JudgeScheduler* / WarmPool* by pointer and calls only the
//  public, noexcept accessors we list below. Any future judge-internal
//  change is invisible to this header as long as those accessors keep
//  their signatures.
// ────────────────────────────────────────────────────────────────────────────

namespace judge {
class JudgeScheduler;
class WarmPool;
}

namespace admin_queue_routes {

// ────────────────────────────────────────────────────────────────────────────
//  Constants — pinned here so the test suite can verify the
//  RateLimitConfig defaults without depending on the field order in
//  config.h.
// ────────────────────────────────────────────────────────────────────────────

// Default cap mirrored from RateLimitConfig::admin_queue_per_minute.
// SPEC §5.5 row 4 leaves the rate-limit cell blank ("-" meaning "no
// policy stated"); we adopt 60/min to match the other operator-facing
// read endpoints (admin_users_list / admin_audit_logs). The bucket
// name "admin.queue" is independent of those buckets so busy
// operators hammering one endpoint can't starve another.
inline constexpr int kAdminQueueDefaultPerMinute = 60;

// ────────────────────────────────────────────────────────────────────────────
//  Count helpers (namespace `admin_queue_routes::detail` to dodge the
//  cross-repo ODR collisions on `litecode::detail::req_string` /
//  `req_int` that other admin route modules documented).
//
//  The DB block of the response carries a single integer:
//      submissions WHERE status='pending'
//  Surfacing this separately from the in-memory queue size lets an
//  operator spot a backlog mismatch (e.g. after a crash the DB has
//  rows in 'pending' that the worker has lost track of — a requeue
//  sweep is a Phase 9 cron concern).
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// pending_submissions_count — single COUNT(*) against submissions
// where status='pending'. Returns -1 on driver error so the caller
// can distinguish "no pending rows" (0) from "couldn't read the
// table" (negative — caller maps to db.ok=false and force-zeroes
// the count). We deliberately don't throw here — a DB blip must
// not 500 the entire route.
inline int pending_submissions_count(ConnectionPool& pool) noexcept {
    try {
        auto conn = pool.acquire();
        const auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT COUNT(*) FROM submissions WHERE status = 'pending'");
        if (!v.has_value()) return 0;
        return v.value() < 0 ? 0 : static_cast<int>(v.value());
    } catch (const std::exception& e) {
        try {
            LOG_WARN("admin_queue: pending_submissions_count threw",
                     {{"type",   typeid(e).name()},
                      {"reason", e.what()}});
        } catch (...) {}
        return -1;
    } catch (...) {
        // swallow — a driver-level exception must not escape
    }
    return -1;
}

// db_ok — pure SELECT 1; same shape as HealthService::make_db_probe.
inline bool db_ok(ConnectionPool* pool) noexcept {
    if (pool == nullptr) return false;
    try {
        return pool->ping();
    } catch (...) {
        return false;
    }
}

// format_iso8601_utc — synthesize an updated_at timestamp in UTC.
// We don't pull in <date.h> or Howard Hinnant date; the format is
// trivial (YYYY-MM-DDTHH:MM:SSZ) and the handler stamps one entry
// per request. Tests pin the *shape* (must contain 'T' and a
// trailing 'Z'), not the wall-clock value, so timezone semantics
// never bite us.
inline std::string format_iso8601_utc(std::time_t t) {
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Judge subsystem snapshot (Phase 4 ★ bridge to v1.2.15 scheduler +
//  v1.2.14 warm pool + v1.2.13 docker_client).
//
//  Mirrors the shape admin_stats_routes::snapshot_judge_subsystem
//  uses (we could share, but admin_stats folds warm_pool + docker
//  into a single "judge" block under data.judge.* and this route
//  hoists them to top-level data.warm_pool / data.docker so the
//  dedicated /admin/queue endpoint has cleaner semantics for
//  Prometheus scrapers that may eventually consume it).
//
//  Pointer args are nullable; a nullptr is rendered as the zero
//  state (size=0, running=false) — never a 500. This matches the
//  policy on /api/v1/health (a missing subsystem doesn't page the
//  operator; the page shows "offline").
// ────────────────────────────────────────────────────────────────────────────

struct QueueSubsystemSnapshot {
    // Scheduler
    bool          scheduler_running   = false;
    std::size_t   queue_size          = 0;
    std::size_t   running_count       = 0;
    int           max_concurrent      = 0;
    int           max_queue_size      = 0;   // 0 ⇒ unbounded (dev / test)

    // Warm pool
    bool          warm_pool_running   = false;
    std::size_t   warm_pool_size      = 0;
    std::size_t   warm_pool_target    = 0;

    // Docker
    bool          docker_ok           = false;
    std::string   docker_detail;
};

inline QueueSubsystemSnapshot snapshot_queue_subsystem(
        const judge::JudgeScheduler* scheduler,
        const judge::WarmPool*      warm_pool,
        const std::function<ProbeResult()>& docker_probe) {
    QueueSubsystemSnapshot s;

    // ── Scheduler (queue) ─────────────────────────────────────────
    if (scheduler != nullptr) {
        try {
            s.scheduler_running   = scheduler->running();
            s.queue_size         = scheduler->queue_size();
            s.running_count      = scheduler->running_count();
            s.max_concurrent     = scheduler->max_concurrent();
            s.max_queue_size     = scheduler->max_queue_size();
        } catch (const std::exception& e) {
            try {
                LOG_WARN("admin_queue: scheduler probe threw",
                         {{"type",   typeid(e).name()},
                          {"reason", e.what()}});
            } catch (...) {}
        } catch (...) {
            // swallow; a buggy accessor must not page the operator
        }
    }

    // ── Warm pool ──────────────────────────────────────────────────
    if (warm_pool != nullptr) {
        try {
            s.warm_pool_running   = warm_pool->running();
            s.warm_pool_size     = warm_pool->size();
            s.warm_pool_target   = warm_pool->target();
        } catch (const std::exception& e) {
            try {
                LOG_WARN("admin_queue: warm_pool probe threw",
                         {{"type",   typeid(e).name()},
                          {"reason", e.what()}});
            } catch (...) {}
        } catch (...) {
            // swallow
        }
    }

    // ── Docker ─────────────────────────────────────────────────────
    if (docker_probe) {
        try {
            auto r = docker_probe();
            s.docker_ok     = r.ok;
            s.docker_detail = r.detail;
        } catch (const std::exception& e) {
            try {
                LOG_WARN("admin_queue: docker probe threw",
                         {{"type",   typeid(e).name()},
                          {"reason", e.what()}});
            } catch (...) {}
        } catch (...) {
            // swallow
        }
    }

    return s;
}

// ────────────────────────────────────────────────────────────────────────────
//  JSON serializers.
//
//  We split the serializer into per-block helpers so a future field
//  addition (e.g. Prometheus-style min/max latency) is one new helper
//  + one new top-level key, not a string of diff hunks in a monolithic
//  function.
//
//  utilization is null when max_queue_size <= 0 (the unbounded path
//  used by tests). A finite max_queue_size with size=0 yields 0.0
//  (the empty-but-healthy state — not null).
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_queue_block(const QueueSubsystemSnapshot& s) {
    nlohmann::json q = {
        {"size",              static_cast<std::int64_t>(s.queue_size)},
        {"running",           static_cast<std::int64_t>(s.running_count)},
        {"max_concurrent",    static_cast<std::int64_t>(s.max_concurrent)},
        {"max_queue_size",    static_cast<std::int64_t>(s.max_queue_size)},
        {"scheduler_running", s.scheduler_running},
    };
    if (s.max_queue_size > 0) {
        // size / max_queue_size, rounded to 4 decimals. A host
        // numeric field is friendlier than a string for clients that
        // graph the gauge server-side (Prometheus / Grafana scrape
        // would multiply by 100 to render "%").
        const double u = static_cast<double>(s.queue_size)
                       / static_cast<double>(s.max_queue_size);
        // Cap at 1.0 — a transient race (worker drained a task
        // between queue_size() and max_queue_size()) could otherwise
        // report > 100% and confuse a gauge fill.
        q["utilization"] = u > 1.0 ? 1.0 : u;
    } else {
        q["utilization"] = nullptr;
    }
    return q;
}

inline nlohmann::json serialize_warm_pool_block(const QueueSubsystemSnapshot& s) {
    return nlohmann::json{
        {"size",    static_cast<std::int64_t>(s.warm_pool_size)},
        {"target",  static_cast<std::int64_t>(s.warm_pool_target)},
        {"running", s.warm_pool_running},
    };
}

inline nlohmann::json serialize_docker_block(const QueueSubsystemSnapshot& s) {
    nlohmann::json d = {{"ok", s.docker_ok}};
    if (!s.docker_detail.empty()) d["detail"] = s.docker_detail;
    else                          d["detail"] = nullptr;
    return d;
}

inline nlohmann::json serialize_db_block(bool db_ok, int pending_submissions) {
    // pending_submissions < 0 ⇒ driver error; surface as 0 so the
    // operator sees a stable shape (db.ok already conveys the
    // degraded state).
    const int p = (pending_submissions < 0) ? 0 : pending_submissions;
    return nlohmann::json{
        {"ok",                  db_ok},
        {"pending_submissions", static_cast<std::int64_t>(p)},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/admin/queue   (SPEC §5.5 row 4, §11 Phase 6)
//
//  Wire flow:
//    1) require_admin(...)                       — 401 / 403 envelope
//    2) consume_rate_limit(admin_queue_quota)     — 429 envelope
//    3) snapshot_queue_subsystem(...)             — nullable probes;
//                                                   never throws
//                                                   (every accessor
//                                                   exception is
//                                                   caught + logged)
//    4) detail::db_ok + detail::pending_submissions_count
//       — DB exceptions fold into db.ok=false and
//         pending_submissions=0 (a 500 here would be worse than
//         a partial degradation — the operator needs the queue
//         signal even when the DB is having a bad day)
//    5) serialize + send_success(200)
//
//  Authorization: admin only.
//  Audit log:      NOT written (read path — see preamble).
//  Rate limit:     admin.queue bucket, 60/min by default.
// ────────────────────────────────────────────────────────────────────────────

inline void get_admin_queue_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg,
        const judge::JudgeScheduler*   scheduler,
        const judge::WarmPool*         warm_pool,
        const std::function<ProbeResult()>& docker_probe) {

    // 1) Admin gate.
    require_admin(req, jwt_cfg);

    // 2) Rate limit. Keyed by user_id; bucket name "admin.queue".
    consume_rate_limit(res, req, limiter, admin_queue_quota(rate_cfg));

    // 3) Subsystem snapshot. snapshot_queue_subsystem is
    //    noexcept-friendly — it catches every probe exception and
    //    logs at WARN. So this call cannot fail.
    const QueueSubsystemSnapshot snap =
        snapshot_queue_subsystem(scheduler, warm_pool, docker_probe);

    // 4) DB block. detail::db_ok is noexcept. detail::pending_submissions_count
    //    is noexcept — returns -1 on driver error, which the serializer
    //    folds to 0. So this block is also noexcept in practice.
    const bool db_ok = detail::db_ok(&pool);
    const int pending_in_db = db_ok
        ? detail::pending_submissions_count(pool)
        : -1;

    LOG_INFO("admin_queue: served",
             {{"scheduler_running",    snap.scheduler_running ? "true" : "false"},
              {"queue_size",           std::to_string(snap.queue_size)},
              {"running",              std::to_string(snap.running_count)},
              {"max_concurrent",       std::to_string(snap.max_concurrent)},
              {"max_queue_size",       std::to_string(snap.max_queue_size)},
              {"warm_pool_running",    snap.warm_pool_running ? "true" : "false"},
              {"warm_pool_size",       std::to_string(snap.warm_pool_size)},
              {"warm_pool_target",     std::to_string(snap.warm_pool_target)},
              {"docker_ok",            snap.docker_ok ? "true" : "false"},
              {"db_ok",                db_ok ? "true" : "false"},
              {"pending_submissions",  std::to_string(
                  pending_in_db < 0 ? 0 : pending_in_db)}});

    // 5) Serialize + send. The top-level data object is built
    //    step-by-step (operator[] rather than a single initializer_list)
    //    because mixing a bare json helper call into a top-level
    //    { {"key",val}, ... } initializer silently falls back to
    //    ARRAY construction — see the nlohmann initializer-list
    //    pitfall the project documented.
    nlohmann::json out = nlohmann::json::object();
    out["queue"]      = serialize_queue_block(snap);
    out["warm_pool"]  = serialize_warm_pool_block(snap);
    out["docker"]     = serialize_docker_block(snap);
    out["db"]         = serialize_db_block(db_ok, pending_in_db);
    out["updated_at"] = detail::format_iso8601_utc(
                              std::chrono::system_clock::to_time_t(
                                  std::chrono::system_clock::now()));
    send_success(res, out);
}

inline HttpServer& register_admin_queue_routes(
        HttpServer&            server,
        ConnectionPool&        pool,
        RateLimiter&           limiter,
        const JwtConfig&       jwt_cfg,
        const RateLimitConfig& rate_cfg,
        const judge::JudgeScheduler*   scheduler = nullptr,
        const judge::WarmPool*         warm_pool  = nullptr,
        const std::function<ProbeResult()>& docker_probe =
            std::function<ProbeResult()>()) {

    // GET /api/v1/admin/queue — single endpoint, SPEC §5.5 row 4.
    server.get("/api/v1/admin/queue",
        [&pool, &limiter, jwt_cfg, rate_cfg, scheduler, warm_pool, docker_probe]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                get_admin_queue_handler(res, req, pool, limiter,
                                        rate_cfg, jwt_cfg,
                                        scheduler, warm_pool, docker_probe);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_queue: handler threw",
                          {{"type",   typeid(e).name()},
                           {"reason", e.what()}});
                if (res.body.empty()) {
                    send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                               std::string("internal error: ") + e.what());
                } else {
                    throw;
                }
            }
        });
    return server;
}

}  // namespace admin_queue_routes

}  // namespace litecode