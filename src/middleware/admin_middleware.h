// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Admin-role guard middleware (Phase 2 ★)
//
// SPEC §5.2 / §5.5 / §11 Phase 2 / §15.2 / A3b acceptance:
//   - Composes with auth_middleware.h to gate the /api/v1/admin/* routes
//     behind role=admin.
//   - Throws ApiException(403, FORBIDDEN) for any non-admin caller,
//     and (via auth_middleware's transitive call) 401 for unauthenticated
//     callers.
//   - Returns the verified Claims to the handler so the audit log can
//     attribute the action to the admin's `user_id` / `username`.
//
// Why a separate file (not folded into auth_middleware.h)?
//   - SPEC §10 §11 put admin_middleware.h at its own path; mirroring
//     the spec keeps the directory structure greppable.
//   - auth_middleware.h is consumed by every authenticated endpoint
//     (submissions / stats / profile). admin_middleware.h is only
//     included from admin_routes.h and the few routes that take admin
//     action (e.g. /api/v1/admin/audit-logs). Keeping them separate
//     trims the #include graph for the hot path.
//
// Usage (from a Phase 2 / Phase 6 admin route):
//
//   s.get("/api/v1/admin/users", [](const Request& req, Response& res) {
//       const auto claims = litecode::require_admin(req, config().jwt);
//       // claims.role == "admin" guaranteed from here on
//       ...
//   });
//
//   s.del("/api/v1/admin/problems/:slug", [](const Request& req, Response& res) {
//       const auto claims = litecode::require_admin(req, config().jwt);
//       // audit_logs insert here, with claims.user_id
//       ...
//   });
//
// Note: this header is INTENTIONALLY a one-liner wrapper. The real
// 401/403 plumbing lives in auth_middleware.h so any change to the
// error envelope / log fields happens in exactly one place.

#pragma once

#include <httplib.h>

#include "../auth/jwt_utils.h"        // Claims
#include "../config.h"               // JwtConfig
#include "auth_middleware.h"         // require_authentication / require_role

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  require_admin — verify the access token AND assert role=admin.
//
//  Combines the two steps in the canonical order (auth first, role
//  second) so an unauthenticated probe never even learns whether the
//  route would have been admin-gated.
//
//  Returns Claims by value, exactly like require_authentication().
//
//  Throws:
//    - ApiException(401, UNAUTHORIZED)  — no / bad token
//    - ApiException(403, FORBIDDEN)     — token valid but role != "admin"
// ────────────────────────────────────────────────────────────────────────────

inline Claims require_admin(const httplib::Request& req,
                            const JwtConfig&        jwt_cfg) {
    // Step 1: verify the token. Any failure here surfaces as 401.
    const Claims claims = require_authentication(req, jwt_cfg);

    // Step 2: enforce role. Throws 403 if claims.role != "admin".
    require_role(claims, "admin");

    return claims;
}

} // namespace litecode
