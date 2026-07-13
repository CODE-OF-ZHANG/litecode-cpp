// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext auth factory (v1.2.48)
//
// Defines litecode::build_auth_deps(). Pulls in LoginFailureTracker
// (which lives in src/routes/auth_routes.h alongside the route
// handlers) and the in-memory refresh token store. Because this is
// the ONLY TU that includes auth_routes.h + refresh_token.h, the
// inline helpers req_string/req_int/truncate_for_envelope defined
// there get emitted exactly once — no ODR collision when main.cpp
// is later linked against this object file alongside the per-route
// .cpp files.

#include "app_context_deps.h"
#include "auth/refresh_token.h"
#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/auth_routes.h"   // LoginFailureTracker

namespace litecode {

AuthDeps build_auth_deps(const LoginLockoutConfig& lockout_cfg) {
    AuthDeps out;
    out.limiter       = std::make_unique<RateLimiter>();
    out.login_tracker = std::make_unique<LoginFailureTracker>(lockout_cfg);
    out.refresh_store =
        std::make_unique<InMemoryRefreshTokenStore>(100000);
    return out;
}

} // namespace litecode