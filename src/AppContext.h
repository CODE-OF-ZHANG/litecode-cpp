// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext (v1.2.48)
//
// Long-lived dependencies owned by main() and shared by every
// register_X_routes(server, ...) call.
//
// ODR-safety: AppContext.h is forward-declaration only. It MUST NOT
// `#include` any route header or db repo header, otherwise main.cpp
// pulls in multiple copies of the inline helpers (req_string /
// req_int / truncate_for_envelope) defined in those headers and
// hits ODR violations (see memory `reference-odr-collision-msvc`).
//
// Each concrete type is defined in its per-route .cpp file
// (registered via CMakeLists.txt) and pulled in by litecode_server
// at link time.

#pragma once

#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

namespace litecode {

// Forward declarations for everything heavy. The actual definitions
// live in the headers pulled in by the per-route / per-deps .cpp
// files in src/routes/ and src/.
struct AppConfig;                        // src/config.h (via app_context_db.cpp + main.cpp)
struct ProbeResult;                      // src/routes/system_routes.h (via app_context_health.cpp + system_routes.cpp)
class ConnectionPool;                    // src/db/connection_pool.h
class RateLimiter;                       // src/middleware/rate_limit.h
class LoginFailureTracker;               // src/routes/auth_routes.h
class RefreshTokenStore;                 // src/auth/refresh_token.h
class HealthService;                     // src/routes/system_routes.h

namespace docker { class Client; }       // src/judge/docker_client.h
namespace judge {
class WarmPool;
class JudgeScheduler;
class JudgeNotifier;
}

struct AppContext {
    AppConfig*                                config           = nullptr;

    // ── Auth / rate-limit / token-store ──────────────────────────────────
    std::unique_ptr<ConnectionPool>                db_pool;
    std::unique_ptr<RateLimiter>                   limiter;
    std::unique_ptr<LoginFailureTracker>           login_tracker;
    std::unique_ptr<RefreshTokenStore>             refresh_store;

    // ── Judge subsystem ──────────────────────────────────────────────────
    std::unique_ptr<docker::Client>                docker_client;
    std::unique_ptr<judge::WarmPool>               warm_pool;
    std::unique_ptr<judge::JudgeScheduler>         scheduler;
    std::unique_ptr<judge::JudgeNotifier>          notifier;

    // ── Health ───────────────────────────────────────────────────────────
    std::unique_ptr<HealthService>                 health;
    std::function<ProbeResult()>                   docker_probe;
};

} // namespace litecode