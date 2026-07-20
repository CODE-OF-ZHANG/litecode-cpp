// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext factory declarations (v1.2.48)
//
// To avoid pulling 12+ route headers and 7+ db repo headers into
// main.cpp (which would trigger ODR collisions on the inline
// req_string / req_int / truncate_for_envelope helpers — see memory
// `reference-odr-collision-msvc`), the AppContext is built
// piecewise via per-component factory functions. Each factory is
// defined in its own .cpp file so it includes only the headers it
// actually needs:
//
//   app_context_db.cpp     → src/db/connection_pool.h
//   app_context_auth.cpp   → src/routes/auth_routes.h,
//                            src/middleware/rate_limit.h,
//                            src/auth/refresh_token.h
//   app_context_judge.cpp  → src/judge/{docker_client,
//                            warm_pool, judge_scheduler,
//                            judge_notifier}.h
//   app_context_health.cpp → src/routes/system_routes.h,
//                            src/judge/* (for make_docker_probe)
//
// main.cpp includes only this header (forward declarations + struct
// shapes) plus AppContext.h and route_registry.h.

#pragma once

#include <functional>
#include <memory>

#include "config.h"   // DatabaseConfig, LoginLockoutConfig, JudgeConfig

namespace litecode {

class ConnectionPool;
class RateLimiter;
class LoginFailureTracker;
class RefreshTokenStore;
class HealthService;
class MetricsService;                    // Phase 9 ★ v1.2.68

namespace docker { class Client; }       // src/judge/docker_client.h
namespace judge {
class WarmPool;
class JudgeScheduler;
class JudgeNotifier;
}

// Pulled in via AppContext.h's forward declaration; AppContext.h
// defines `ProbeResult` itself to keep system_routes.h out of
// this header (which is the ODR hotspot).
struct ProbeResult;

struct DbDeps {
    std::unique_ptr<ConnectionPool> pool;
};
DbDeps build_db_deps(const DatabaseConfig& cfg);

struct AuthDeps {
    std::unique_ptr<RateLimiter>        limiter;
    std::unique_ptr<LoginFailureTracker> login_tracker;
    std::unique_ptr<RefreshTokenStore>   refresh_store;
};
AuthDeps build_auth_deps(const LoginLockoutConfig& lockout_cfg);

struct JudgeDeps {
    std::unique_ptr<docker::Client>        docker_client;
    std::unique_ptr<judge::WarmPool>       warm_pool;
    std::unique_ptr<judge::JudgeScheduler> scheduler;
    std::unique_ptr<judge::JudgeNotifier>  notifier;
};
// v1.2.50: takes the db_pool so the scheduler can be started
// here (pre-v1.2.50 the scheduler was constructed with db=nullptr
// and never started, so submissions got 503 "judge queue is full").
JudgeDeps build_judge_deps(const JudgeConfig& cfg,
                           ConnectionPool*   db);

// Returns a HealthService with probes wired against the supplied
// deps. db_pool may be null (probe skipped); docker_client may be
// null (probe skipped); scheduler + warm_pool may be null (their
// probes skipped). Caller owns the returned service.
struct HealthDeps {
    std::unique_ptr<HealthService> health;
};
HealthDeps build_health_deps(const DbDeps&         db,
                             const JudgeDeps&      judge,
                             std::function<ProbeResult()> docker_probe);

// Returns a fully-populated MetricsService with the
// SPEC §16.4 metric families registered:
//   - litecode_submissions_total       (counter / status)
//   - litecode_judge_duration_seconds  (histogram)
//   - litecode_judge_queue_size        (gauge, sampled from sched)
//   - litecode_judge_running_count     (gauge, sampled from sched)
//   - litecode_judge_warm_pool_size    (gauge, sampled from warm_pool)
//   - litecode_judge_warm_pool_target  (gauge, sampled from warm_pool)
//   - litecode_db_pool_active          (gauge, sampled from db.stats())
//
// All gauge providers are null-safe — when a subsystem is unconfigured
// (db==null, sched==null, etc.) the gauge renders "0" instead of
// crashing the scrape.
struct MetricsDeps {
    std::unique_ptr<MetricsService> metrics;
};
MetricsDeps build_metrics_deps(const DbDeps&    db,
                                const JudgeDeps& judge);

} // namespace litecode