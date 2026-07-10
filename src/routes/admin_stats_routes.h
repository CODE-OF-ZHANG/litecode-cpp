// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — admin system statistics routes (Phase 6 ★)
//
// SPEC §5.5 / §11 Phase 6 / §5.6 / §11 Phase 1-4 / §12.1 / §16.1:
//   - GET /api/v1/admin/stats            (🔒 admin, no rate limit)
//
//       System-wide overview for the /admin/dashboard.html page
//       (v1.2.40+ delivers the page; the API shape lives here).
//       Returns the rows + counts the dashboard renders, in one
//       envelope so the page can paint a single fetch:
//
//         1. Repository counts
//            - users.total
//            - users.admins (subset, role='admin')
//            - problems.total      (live: is_deleted=FALSE)
//            - problems.deleted    (is_deleted=TRUE)
//            - problems.by_difficulty.{easy, medium, hard}
//            - tags.total
//            - submissions.total
//            - submissions.by_status.{pending, running, ac, wa,
//                                     re, tle, mle, ole, pe, ce, se}
//            - submissions.recent_24h
//            - submissions.recent_24h_ac
//            - audit_logs.total
//
//         2. Activity (last 24h window, UTC by created_at)
//            - submissions_24h
//            - ac_24h
//            - new_users_24h
//
//         3. Language distribution
//            - submissions.by_language.{cpp, ...}
//            (unordered; the front-end sorts client-side)
//
//         4. Judge subsystem (Phase 4 ★ + v1.2.42 collated here)
//            - queue.{size, running, max_concurrent, scheduler_running}
//              When no scheduler is wired in (dev box, test
//              fixture) every numeric field is 0 and
//              `scheduler_running` is false. SPEC §5.5 row 3
//              promises "队列长度" — size is the canonical queue
//              depth metric, with running/max_concurrent/running
//              filling in the dashboard's sparkline / gauge.
//            - warm_pool.{size, target, running}
//              When no pool is wired: 0 / 0 / false.
//              When the pool was shut down: 0 / target / false.
//            - docker.{ok: bool, detail: string|null}
//              Bridges the docker_client probe (v1.2.14) so the
//              dashboard can show "judge subsystem down" in red.
//
//         5. System health snapshot
//            - db.{ok: bool}
//            - docker.{ok: bool, detail: string|null}
//            - uptime_seconds
//
// Wire shape (response, 200):
//   {
//     "data": {
//        "users":   { "total": N, "admins": M },
//        "problems":{ "total": N, "live": ..., "deleted": ...,
//                     "by_difficulty": {"easy":..,"medium":..,"hard":..} },
//        "tags":    { "total": N },
//        "submissions": { "total": N, "recent_24h": X, "recent_24h_ac": Y,
//                         "by_status": {11 enum keys},
//                         "by_language": {...} },
//        "audit_logs": { "total": N },
//        "activity": { "submissions_24h": N, "ac_24h": N, "new_users_24h": N },
//        "judge": {
//          "queue":     {"size":N,"running":N,"max_concurrent":N,
//                        "scheduler_running":bool},
//          "warm_pool": {"size":N,"target":N,"running":bool},
//          "docker":    {"ok":bool, "detail":"..."|null}
//        },
//        "db":          { "ok": bool },
//        "uptime_seconds": 12345
//     },
//     "request_id": "..."
//   }
//
// Failure modes:
//   - No / bad access token            → 401 UNAUTHORIZED
//   - Token valid but role != "admin"  → 403 FORBIDDEN
//   - Any repo throw                   → 500 INTERNAL_ERROR
//   - Judge/warm_pool probes throwing  → those fields fall back to
//                                        safe defaults (0/false),
//                                        the rest of the response
//                                        still goes out — partial
//                                        degradation, never a 500
//                                        (a config bug cannot page
//                                        an operator)
//
// Design notes:
//   - Header-only + inline: matches every other Phase 2/3/4/5/6
//     route module. Tests link this header directly; production
//     ownership is `main.cpp` (or, given the ODR caveat below,
//     nothing — same as the other admin route modules).
//   - We deliberately do NOT write to `audit_logs`. SPEC §11
//     reserves audit_logs for "key admin operations" (changes that
//     mutate data) — read-only dashboard polls would otherwise
//     flood the table with one row per page refresh.
//   - We deliberately do NOT call consume_rate_limit(). The
//     dashboard polls this endpoint every ~5 seconds (front-end
//     `setTimeout`); a per-admin 60/min cap would page the
//     operator with 429 every time the auto-refresh tick fires.
//     SPEC §5.5 row 3 lists no quota for /admin/stats for
//     exactly this reason.
//   - We deliberately tolerate scheduler/pool/docker == nullptr.
//     Dev machines don't have docker; the test fixture doesn't
//     spin up the scheduler; the response is still a valid
//     200 envelope so the page renders an "offline" badge instead
//     of a 500 page. The dashboard stub (v1.2.40) reads the
//     same wire fields.
//   - Repos are pulled via header-only SELECTs into pool (no need
//     to add new methods to user_repo / problem_repo / etc.). A
//     future refactor can promote each helper to the repo if
//     it earns multiple callers; for now it keeps the ODR
//     surface narrow.
//   - ODR caveat: this header transitively pulls in user_repo.h,
//     submission_repo.h, problem_repo.h, audit_log_repo.h. To
//     sidestep the cross-repo collision on
//     `litecode::detail::req_string`, this header follows the
//     same shape used by stats_routes.h: it adds an
//     admin_stats_routes::detail namespace for local helpers
//     that don't collide with anything else, and uses bare SELECTs
//     instead of going through user_repo.h's bulk row mapper
//     (so user_repo.h's `req_string` / `req_int` are never
//     instantiated in this TU). End-to-end coverage is owned by
//     tests/unit/test_admin_stats.cpp; main.cpp does NOT
//     smoke-register this header for the same reason it doesn't
//     register admin_problem_routes / admin_bulk_import_routes /
//     admin_user_routes / submission_routes / problem_routes.
//   - Judge subsystem pointers:
//       judge_scheduler = JudgeScheduler* (nullable)
//       warm_pool       = WarmPool*         (nullable)
//       docker_probe    = HealthService::Probe (a std::function)
//     We accept the last one as a std::function so the route
//     doesn't have to include `docker_client.h` directly (the
//     include graph would force every TU that already uses
//     HealthService through the docker surface area; this keeps
//     admin_stats_routes.h header-light).
//
// Usage (production, from main.cpp):
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
//   litecode::register_admin_stats_routes(
//       server, pool, cfg.jwt,
//       /*scheduler=*/ &sched,
//       /*warm_pool=*/  &pool_docker,
//       /*docker_probe=*/ litecode::docker::make_docker_probe(docker_client.get()));
//
// Usage (test, from gtest):
//
//   litecode::register_admin_stats_routes(
//       server, pool, jwt_cfg,
//       /*scheduler=*/ nullptr,    // dev box has no scheduler
//       /*warm_pool=*/  nullptr,    // dev box has no warm pool
//       /*docker_probe=*/ nullptr); // dev box has no docker

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "../config.h"                                // JwtConfig
#include "../db/connection_pool.h"                    // ConnectionPool
#include "../logger.h"                                // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../middleware/admin_middleware.h"           // require_admin + Claims
#include "../middleware/rate_limit.h"                 // extract_client_ip (used only for logs)
#include "../routes/error_handler.h"                  // send_error / ErrorCode / ApiException
#include "../routes/system_routes.h"                  // HealthService::Probe / ProbeResult + uptime
//
// IMPORTANT: We deliberately do NOT include `user_repo.h` /
// `problem_repo.h` / `submission_repo.h` here. Each of them
// defines helpers in `litecode::detail` (req_string / req_int /
// row_to_user), and pulling more than one into the same TU
// under MSVC's strict One-Definition-Rule triggers C2084
// ("function already has a body"). The Phase 3 test_admin_user
// etc. documented this same collision; the workaround has been
// to do bare SQL via `mysqlx::SqlResult` here instead of going
// through the repo layer. The handful of SELECTs in detail::
// helpers below mirror the user_repo / problem_repo /
// submission_repo count functions one-for-one but live in their
// own namespace.
//
// Tests that need both user_repo.h AND problem_repo.h in the
// same TU (e.g. test_admin_users.cpp / test_stats_*.cpp) all
// seed via raw SQL for the same reason. See their preamble.

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Judge subsystem forward declarations.
//
//  We deliberately do NOT include `judge/judge_scheduler.h` or
//  `judge/warm_pool.h` here — those headers drag in docker_client.h +
//  system_routes.h + nlohmann_json + std::thread + std::filesystem,
//  which is a heavy graph for a route header to force on every
//  translation unit. The route handler accepts
//      JudgeScheduler*  /  WarmPool*
//  by pointer and calls only the public, noexcept accessors we list
//  below. Any future judge-internal change is invisible to this header
//  as long as those accessors keep their signatures.
//
//  We DO list the public accessors we use as free functions
//  (`admin_stats_judge_*`) so the test binary can stub them without
//  pulling in the real scheduler / pool (tests cover both the
//  "scheduler wired" and "scheduler not wired" cases — see
//  tests/unit/test_admin_stats.cpp).
// ────────────────────────────────────────────────────────────────────────────

namespace judge {
class JudgeScheduler;
class WarmPool;
}

namespace admin_stats_routes {

// ────────────────────────────────────────────────────────────────────────────
//  Count helpers (namespace `admin_stats_routes::detail` to dodge the
//  cross-repo ODR collisions on `litecode::detail::req_string` /
//  `req_int` that other admin route modules documented).
//
//  Each helper is a single COUNT(*) SELECT — no JOINs, no GROUP BYs
//  — so each one is a small, predictable MySQL round-trip. Five
//  different DB counters? Five round-trips? That's well under the
//  SPEC §12.2 < 200ms budget on a LAN MySQL 8.x (each round-trip is
//  ~3-5 ms with the connection pool warm).
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::size_t safe_int_to_size(std::int64_t v) noexcept {
    return v < 0 ? 0u : static_cast<std::size_t>(v);
}

inline int count_scalar(ConnectionPool& pool, const std::string& sql) {
    auto conn = pool.acquire();
    try {
        const auto v = conn.fetch_scalar<std::int64_t>(sql);
        return v.has_value() ? static_cast<int>(*v) : 0;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(std::string("admin_stats: ") + e.what());
    }
}

inline int count_users_total(ConnectionPool& pool) {
    return count_scalar(pool, "SELECT COUNT(*) FROM users");
}

inline int count_users_admins(ConnectionPool& pool) {
    return count_scalar(pool,
        "SELECT COUNT(*) FROM users WHERE role = 'admin'");
}

inline int count_tags_total(ConnectionPool& pool) {
    return count_scalar(pool, "SELECT COUNT(*) FROM tags");
}

inline int count_problems_total(ConnectionPool& pool) {
    // includes soft-deleted (matches "what's in the table")
    return count_scalar(pool, "SELECT COUNT(*) FROM problems");
}

inline int count_problems_live(ConnectionPool& pool) {
    return count_scalar(pool,
        "SELECT COUNT(*) FROM problems WHERE is_deleted = FALSE");
}

inline int count_problems_deleted(ConnectionPool& pool) {
    return count_scalar(pool,
        "SELECT COUNT(*) FROM problems WHERE is_deleted = TRUE");
}

// count_problems_by_difficulty — returns {easy, medium, hard}
// counts of LIVE problems, zero-padded to all three keys. The
// front-end's distribution chart relies on every key being
// present (a 0-count still gets a bar in the chart for visual
// parity — designers want a stable layout across deployments).
inline std::unordered_map<std::string, int>
count_problems_by_difficulty(ConnectionPool& pool) {
    std::unordered_map<std::string, int> out;
    auto conn = pool.acquire();
    try {
        mysqlx::SqlResult rs = conn.execute(
            "SELECT difficulty, COUNT(*) AS cnt "
            "FROM problems "
            "WHERE is_deleted = FALSE "
            "  AND difficulty IN ('easy','medium','hard') "
            "GROUP BY difficulty");
        for (auto row : rs) {
            try {
                const std::string diff = row[0].get<std::string>();
                const std::int64_t cnt  = row[1].get<std::int64_t>();
                out[diff] = static_cast<int>(cnt);
            } catch (const std::exception&) {
                // Skip malformed rows defensively so one bad
                // column doesn't tank the whole distribution.
            }
        }
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("admin_stats: count_problems_by_difficulty: ")
            + e.what());
    }
    // Zero-pad so the response carries all three keys.
    const std::string easy_str   = "easy";
    const std::string medium_str = "medium";
    const std::string hard_str   = "hard";
    if (out.find(easy_str)   == out.end()) out[easy_str]   = 0;
    if (out.find(medium_str) == out.end()) out[medium_str] = 0;
    if (out.find(hard_str)   == out.end()) out[hard_str]   = 0;
    return out;
}

inline int count_submissions_total(ConnectionPool& pool) {
    return count_scalar(pool, "SELECT COUNT(*) FROM submissions");
}

inline int count_submissions_24h(ConnectionPool& pool) {
    return count_scalar(pool,
        "SELECT COUNT(*) FROM submissions "
        "WHERE created_at >= (NOW() - INTERVAL 1 DAY)");
}

inline int count_submissions_24h_ac(ConnectionPool& pool) {
    return count_scalar(pool,
        "SELECT COUNT(*) FROM submissions "
        "WHERE created_at >= (NOW() - INTERVAL 1 DAY) "
        "  AND status = 'ac'");
}

inline int count_audit_logs_total(ConnectionPool& pool) {
    return count_scalar(pool, "SELECT COUNT(*) FROM audit_logs");
}

inline int count_new_users_24h(ConnectionPool& pool) {
    return count_scalar(pool,
        "SELECT COUNT(*) FROM users "
        "WHERE created_at >= (NOW() - INTERVAL 1 DAY)");
}

// count_submissions_by_status — map status → count, zero-padded
// to every enum value defined by submission_repo::kStatus*. The
// handler stitches the 11-key enum into `submissions.by_status` so
// the dashboard can render 11 colored bars in a fixed layout.
inline std::unordered_map<std::string, int>
count_submissions_by_status(ConnectionPool& pool) {
    std::unordered_map<std::string, int> out;
    auto conn = pool.acquire();
    try {
        mysqlx::SqlResult rs = conn.execute(
            "SELECT status, COUNT(*) AS cnt "
            "FROM submissions GROUP BY status");
        for (auto row : rs) {
            try {
                const std::string s   = row[0].get<std::string>();
                const std::int64_t cnt = row[1].get<std::int64_t>();
                out[s] = static_cast<int>(cnt);
            } catch (const std::exception&) {
                // Skip malformed rows defensively.
            }
        }
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("admin_stats: count_submissions_by_status: ")
            + e.what());
    }
    return out;
}

// count_submissions_by_language — map language → count. The map
// has at most 2 entries today (cpp / c) but the response shape is
// a free-form object so adding rust/python later costs nothing.
// The dashboard sorts by count DESC client-side.
inline std::unordered_map<std::string, int>
count_submissions_by_language(ConnectionPool& pool) {
    std::unordered_map<std::string, int> out;
    auto conn = pool.acquire();
    try {
        mysqlx::SqlResult rs = conn.execute(
            "SELECT language, COUNT(*) AS cnt "
            "FROM submissions GROUP BY language");
        for (auto row : rs) {
            try {
                const std::string lang = row[0].get<std::string>();
                const std::int64_t cnt = row[1].get<std::int64_t>();
                out[lang] = static_cast<int>(cnt);
            } catch (const std::exception&) {
                // Skip malformed rows defensively.
            }
        }
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("admin_stats: count_submissions_by_language: ")
            + e.what());
    }
    return out;
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

// Soft-default 11-status enum from SPEC §4.4 / submission_repo.h.
// We re-declare here so the zero-pad set is constant across
// callers; it's the SAME 11 keys submission_repo.h pins in
// kStatus*, but we keep the literal strings here so we don't
// have to include submission_repo's full namespace.
inline const std::vector<std::string>& all_status_keys() noexcept {
    static const std::vector<std::string> keys = {
        "pending", "running",
        "ac", "wa", "re", "tle", "mle",
        "ole", "pe", "ce", "se",
    };
    return keys;
}

inline const std::vector<std::string>& all_difficulty_keys() noexcept {
    static const std::vector<std::string> keys = {
        "easy", "medium", "hard",
    };
    return keys;
}

inline nlohmann::json zero_padded_by_status(
        const std::unordered_map<std::string, int>& m) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& k : all_status_keys()) {
        j[k] = 0;
    }
    for (const auto& [k, v] : m) {
        // Defensive: only set known keys. A future status name
        // (e.g. "internal_error") that's not in all_status_keys
        // won't sneak into the response.
        if (j.contains(k)) j[k] = v;
    }
    return j;
}

inline nlohmann::json zero_padded_by_difficulty(
        const std::unordered_map<std::string, int>& m) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& k : all_difficulty_keys()) {
        j[k] = 0;
    }
    for (const auto& [k, v] : m) {
        if (j.contains(k)) j[k] = v;
    }
    return j;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Judge subsystem snapshot (Phase 4 ★ bridge to v1.2.14 docker_client).
//
//  We surface queue_size / running_count / max_concurrent /
//  warm_pool size / target / docker probe result as a tiny "judge"
//  block so the dashboard can render the queue sparkline without
//  hitting /api/v1/health + /api/v1/admin/queue separately.
//
//  Pointer args are nullable; a nullptr is rendered as the zero
//  state (`size=0, running=false`) — never a 500. This matches
//  the Policy on /api/v1/health (a missing subsystem doesn't
//  page the operator; the dashboard shows "offline").
// ────────────────────────────────────────────────────────────────────────────

struct JudgeSubsystemSnapshot {
    bool   scheduler_running    = false;
    std::size_t queue_size      = 0;
    std::size_t running_count   = 0;
    std::size_t max_concurrent  = 0;

    bool   warm_pool_running    = false;
    std::size_t warm_pool_size  = 0;
    std::size_t warm_pool_target = 0;

    bool   docker_ok              = false;
    std::string docker_detail;
};

inline JudgeSubsystemSnapshot snapshot_judge_subsystem(
        const judge::JudgeScheduler* scheduler,
        const judge::WarmPool*      warm_pool,
        const std::function<litecode::ProbeResult()>& docker_probe) {
    JudgeSubsystemSnapshot s;

    // ── Scheduler (queue) ─────────────────────────────────────────
    if (scheduler != nullptr) {
        try {
            s.scheduler_running   = scheduler->running();
            s.queue_size         = scheduler->queue_size();
            s.running_count      = scheduler->running_count();
            s.max_concurrent     = static_cast<std::size_t>(
                                       scheduler->max_concurrent());
        } catch (const std::exception& e) {
            try {
                LOG_WARN("admin_stats: scheduler probe threw",
                         {{"type",   typeid(e).name()},
                          {"reason", e.what()}});
            } catch (...) {}
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
                LOG_WARN("admin_stats: warm_pool probe threw",
                         {{"type",   typeid(e).name()},
                          {"reason", e.what()}});
            } catch (...) {}
        }
    }

    // ── Docker (v1.2.14 ★ probe) ───────────────────────────────────
    if (docker_probe) {
        try {
            auto r = docker_probe();
            s.docker_ok     = r.ok;
            s.docker_detail = r.detail;
        } catch (const std::exception& e) {
            try {
                LOG_WARN("admin_stats: docker probe threw",
                         {{"type",   typeid(e).name()},
                          {"reason", e.what()}});
            } catch (...) {}
        } catch (...) {
            // swallow; a buggy probe must not page the operator
        }
    }

    return s;
}

inline nlohmann::json serialize_judge_subsystem(
        const JudgeSubsystemSnapshot& s) {
    nlohmann::json q = {
        {"size",              static_cast<std::int64_t>(s.queue_size)},
        {"running",           static_cast<std::int64_t>(s.running_count)},
        {"max_concurrent",    static_cast<std::int64_t>(s.max_concurrent)},
        {"scheduler_running", s.scheduler_running},
    };
    nlohmann::json wp = {
        {"size",    static_cast<std::int64_t>(s.warm_pool_size)},
        {"target",  static_cast<std::int64_t>(s.warm_pool_target)},
        {"running", s.warm_pool_running},
    };
    nlohmann::json dk = {
        {"ok",     s.docker_ok},
    };
    if (!s.docker_detail.empty()) dk["detail"] = s.docker_detail;
    else                          dk["detail"] = nullptr;

    return nlohmann::json{
        {"queue",     q},
        {"warm_pool", wp},
        {"docker",    dk},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Top-level aggregate — the single big JSON blob the route returns.
//
//  Each block is built from its own pool of helpers; failures of
//  individual helpers are caught and folded into INTERNAL_ERROR
//  (the dashboard MUST surface the failure, not paint zeros — a
//  silent zero would be worse than a 500). Judge / docker probes
//  are explicitly tolerated because they may legitimately be
//  absent (dev box, test fixture, configuration bug).
// ────────────────────────────────────────────────────────────────────────────

struct SystemStats {
    int total_users                       = 0;
    int total_admins                      = 0;
    int total_problems                    = 0;
    int live_problems                     = 0;
    int deleted_problems                  = 0;
    std::unordered_map<std::string, int> problems_by_difficulty;
    int total_tags                        = 0;
    int total_submissions                 = 0;
    std::unordered_map<std::string, int> submissions_by_status;
    std::unordered_map<std::string, int> submissions_by_language;
    int total_audit_logs                  = 0;
    int submissions_24h                   = 0;
    int ac_24h                            = 0;
    int new_users_24h                     = 0;
    bool db_ok                            = false;
};

inline SystemStats compute_system_stats(ConnectionPool& pool) {
    SystemStats s;
    s.db_ok                        = detail::db_ok(&pool);
    s.total_users                  = detail::count_users_total(pool);
    s.total_admins                 = detail::count_users_admins(pool);
    s.total_problems               = detail::count_problems_total(pool);
    s.live_problems                = detail::count_problems_live(pool);
    s.deleted_problems             = detail::count_problems_deleted(pool);
    s.problems_by_difficulty       = detail::count_problems_by_difficulty(pool);
    s.total_tags                   = detail::count_tags_total(pool);
    s.total_submissions            = detail::count_submissions_total(pool);
    s.submissions_by_status        = detail::count_submissions_by_status(pool);
    s.submissions_by_language      = detail::count_submissions_by_language(pool);
    s.total_audit_logs             = detail::count_audit_logs_total(pool);
    s.submissions_24h              = detail::count_submissions_24h(pool);
    s.ac_24h                       = detail::count_submissions_24h_ac(pool);
    s.new_users_24h                = detail::count_new_users_24h(pool);
    return s;
}

inline nlohmann::json serialize_system_stats(
        const SystemStats& s,
        const JudgeSubsystemSnapshot& j) {
    // nlohmann::json's initializer_list ctor builds an OBJECT only
    // when every element is a 2-element `{key, value}` pair. A bare
    // json value (e.g. the result of serialize_judge_subsystem(j))
    // makes the whole list fall back to ARRAY construction — the
    // keys after it become numeric indices and disappear from
    // `data`. We work around this with a step-by-step build:
    //   1) assemble the inner objects,
    //   2) combine them via operator[] rather than a single
    //      initializer_list. The shape is identical (object
    //      semantically), but the ctor sees no mixed element.
    nlohmann::json out = nlohmann::json::object();
    out["users"] = {
        {"total",  s.total_users},
        {"admins", s.total_admins},
    };
    out["problems"] = {
        {"total",         s.total_problems},
        {"live",          s.live_problems},
        {"deleted",       s.deleted_problems},
        {"by_difficulty", detail::zero_padded_by_difficulty(
                                s.problems_by_difficulty)},
    };
    out["tags"] = {
        {"total", s.total_tags},
    };
    out["submissions"] = {
        {"total",          s.total_submissions},
        {"recent_24h",     s.submissions_24h},
        {"recent_24h_ac",  s.ac_24h},
        {"by_status",      detail::zero_padded_by_status(
                                s.submissions_by_status)},
        {"by_language",    s.submissions_by_language},
    };
    out["audit_logs"] = {
        {"total", s.total_audit_logs},
    };
    out["activity"] = {
        {"submissions_24h", s.submissions_24h},
        {"ac_24h",          s.ac_24h},
        {"new_users_24h",   s.new_users_24h},
    };
    // Judge subsystem is a separate object — paste it in as a
    // named "judge" block rather than spreading its keys at the
    // top level (matches the dashboard's mental model and keeps
    // each subsystem grouped for log/inspect purposes).
    out["judge"] = serialize_judge_subsystem(j);
    out["db"] = {{"ok", s.db_ok}};
    out["uptime_seconds"] =
        static_cast<std::int64_t>(process_uptime().count());
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/admin/stats    (SPEC §5.5, §5.6, §11 Phase 6)
//
//  Wire flow:
//    1) require_admin(...)                  — 401 / 403 envelope
//    2) (NO consume_rate_limit — see header preamble)
//    3) compute_system_stats(pool)          — single MySQL burst;
//                                              throws → 500
//    4) snapshot_judge_subsystem(...)       — nullable probes; never
//                                              throws (catches every
//                                              exception, logs)
//    5) serialize + send_success(200)
//
//  Authorization: admin only.
//  Audit log:      NOT written (read path, polled every ~5s).
//  Rate limit:     NOT enforced (SPEC §5.5 row 3 leaves the cell
//                  empty; the front-end auto-refresh fires every
//                  5s and a 60/min cap would always trip).
// ────────────────────────────────────────────────────────────────────────────

inline void get_admin_stats_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        const JwtConfig&               jwt_cfg,
        const judge::JudgeScheduler*   scheduler,
        const judge::WarmPool*         warm_pool,
        const std::function<ProbeResult()>& docker_probe) {

    // 1) Admin gate. We don't capture the Claims — this read
    //    endpoint doesn't audit_log.
    require_admin(req, jwt_cfg);

    // 2) (no rate limit — see preamble)

    // 3) Repo counts. Any failure → 500. We deliberately don't
    //    try/partial-succeed because the dashboard renders from
    //    every block; silently zeroing one block would mislead
    //    the operator.
    SystemStats s;
    try {
        s = compute_system_stats(pool);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_stats: compute_system_stats threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 4) Judge subsystem snapshot. snapshot_judge_subsystem is
    //    noexcept-friendly — it catches every probe exception and
    //    logs at WARN. So this call cannot fail.
    const JudgeSubsystemSnapshot js =
        snapshot_judge_subsystem(scheduler, warm_pool, docker_probe);

    LOG_INFO("admin_stats: served",
             {{"users",      std::to_string(s.total_users)},
              {"problems_live", std::to_string(s.live_problems)},
              {"submissions",std::to_string(s.total_submissions)},
              {"scheduler_running", js.scheduler_running ? "true" : "false"},
              {"warm_pool_running", js.warm_pool_running ? "true" : "false"}});

    // 5) Serialize + send.
    send_success(res, serialize_system_stats(s, js));
}

inline HttpServer& register_admin_stats_routes(
        HttpServer&            server,
        ConnectionPool&        pool,
        const JwtConfig&       jwt_cfg,
        const judge::JudgeScheduler*   scheduler = nullptr,
        const judge::WarmPool*         warm_pool  = nullptr,
        const std::function<ProbeResult()>& docker_probe =
            std::function<ProbeResult()>()) {

    // GET /api/v1/admin/stats — single endpoint, SPEC §5.5 row 3.
    server.get("/api/v1/admin/stats",
        [&pool, &jwt_cfg, scheduler, warm_pool, docker_probe]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                get_admin_stats_handler(res, req, pool, jwt_cfg,
                                          scheduler, warm_pool,
                                          docker_probe);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_stats: handler threw",
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

}  // namespace admin_stats_routes

}  // namespace litecode
