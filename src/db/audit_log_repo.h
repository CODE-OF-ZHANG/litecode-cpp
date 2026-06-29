// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — audit_logs repository (Phase 2 ★, login-failure subset)
//
// SPEC §4.2d / §11 Phase 2 / §15.6 / A27 acceptance:
//   The full audit_logs surface (admin CRUD + bulk-import + role-change)
//   lands with Phase 6 admin_routes. Phase 2 only needs the **login
//   failure** write path so /api/v1/auth/login can satisfy §15.1
//   ("失败登录 5 次 → 写 audit_logs") without dragging in the entire
//   Phase 6 surface.
//   The schema is:
//       audit_logs(id, admin_id NULL, action, target_type,
//                  target_id, payload JSON, ip, created_at)
//   and is created by db/migrations/V002__add_audit_logs.sql. We do
//   NOT depend on the schema here — we just insert the columns that
//   exist today. Adding more writers (admin_routes, etc.) later
//   expands this header; it's a deliberately small Phase-2-shaped
//   surface that doesn't need a class hierarchy yet.
//
// Design notes:
//   - Header-only + inline: matches the rest of the Phase 2 data
//     layer (user_repo.h, connection_pool.h). The single write is
//     small enough that inlining the SQL is the right shape.
//   - Parameterized SQL (`?` placeholders) per SPEC §15.2 — user
//     input never reaches the wire string.
//   - Best-effort semantics: a transient insert failure must not
//     fail the login itself. We log a WARN and swallow. The login
//     flow has already returned 401 to the client by the time we
//     get here.
//
// Usage:
//   litecode::audit_log_repo::record_login_failure(
//       pool,
//       /*username=*/"alice",
//       /*ip=*/      "1.2.3.4",
//       /*consecutive_failures=*/5);

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>

#include "../logger.h"             // LOG_WARN
#include "connection_pool.h"       // ConnectionPool

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace audit_log_repo {

// record_login_failure — INSERT a row into audit_logs capturing that
// `username` failed to authenticate for the Nth consecutive time from
// `ip`. The row is shaped:
//
//     action       = "auth.login_failure"
//     target_type  = "user"
//     target_id    = username                 (best-effort — we don't
//                                              know the user_id when
//                                              the username is wrong)
//     admin_id     = NULL                      (login is anonymous-ish;
//                                              Phase 6 will reuse this
//                                              column for admin writes)
//     payload      = { "consecutive_failures": N }
//     ip           = client IP                (may be empty)
//
// `consecutive_failures` is the count BEFORE this attempt. The route
// handler tracks its own per-username counter (in-memory) and decides
// when to call this — currently at every 5th consecutive failure, so
// the audit table only sees things the operator cares about.
//
// Throws: only on driver errors we can't interpret. Callers should
// wrap the call in try/catch and treat failure as "audit lost, login
// still answered 401 normally".
inline void record_login_failure(ConnectionPool&        pool,
                                 std::string_view       username,
                                 std::string_view       ip,
                                 int                    consecutive_failures) {
    auto conn = pool.acquire();
    try {
        // mysqlx requires explicit nullable binders — std::optional
        // doesn't auto-convert, so we lift the admin_id into a NULL
        // mysqlx::Value. The other optionals (payload, ip) are
        // pre-serialized to std::string / empty before binding.
        const std::string username_str(username);
        const std::string action      = "auth.login_failure";
        const std::string target_type = "user";
        const std::string target_id(username);
        const nlohmann::json payload = {
            {"consecutive_failures", consecutive_failures},
        };
        const std::string payload_str = payload.dump();
        const std::string ip_str(ip);   // may be empty; driver stores empty string

        const mysqlx::Value admin_id_val(nullptr);
        const mysqlx::Value ip_val     = ip.empty()
            ? mysqlx::Value(nullptr)
            : mysqlx::Value(ip_str);

        conn.execute(
            "INSERT INTO audit_logs "
            "(admin_id, action, target_type, target_id, payload, ip) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            admin_id_val,
            action,
            target_type,
            target_id,
            payload_str,
            ip_val);
    } catch (const std::exception& e) {
        // Best-effort: losing one audit row is not worth failing the
        // login (which has already been answered 401 by the caller).
        LOG_WARN("audit_log_repo::record_login_failure failed",
                 {{"username", std::string(username)},
                  {"reason",   e.what()}});
    }
}

} // namespace audit_log_repo
} // namespace litecode
