// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — audit_logs repository (Phase 3 ★)
//
// SPEC §4.2d / §11 Phase 3 / §11 Phase 6 / §15.6 / A17, A18, A19, A20, A27:
//   - `audit_logs` table schema (V002):
//       id BIGINT AUTO_INCREMENT PK,
//       admin_id INT NULL FK -> users(id) ON DELETE SET NULL,
//       action VARCHAR(50) NOT NULL,
//       target_type VARCHAR(50) NULL,
//       target_id VARCHAR(100) NULL,
//       payload JSON NULL,
//       ip VARCHAR(45) NULL,
//       created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
//       INDEX idx_audit_admin_time (admin_id, created_at DESC),
//       INDEX idx_audit_action_time (action, created_at)
//   - The repo offers the surface that every Phase 6 admin endpoint
//     and the Phase 2 login-failure tracker share:
//       Writers (used by admin_routes + auth_routes):
//         * record              — strict INSERT; throws on driver error.
//                                Phase 6 callers (admin write paths)
//                                want a hard failure: a lost audit row
//                                on a destructive action is a security
//                                trail gap and must surface as 500.
//         * record_best_effort  — INSERT that LOG_WARNs and swallows on
//                                driver error. Phase 2 login-failure
//                                uses this; losing one row is not
//                                worth failing the login (which has
//                                already returned 401 to the client).
//         * record_login_failure — thin best-effort wrapper that
//                                builds the Phase-2-shaped payload
//                                (action="auth.login_failure",
//                                payload={"consecutive_failures": N})
//                                and calls record_best_effort. Kept
//                                here verbatim because auth_routes.h
//                                already calls it.
//       Readers (used by Phase 6 admin /audit-logs endpoint):
//         * find_by_id          — fetch one row by PK (admin detail
//                                 view; admin "look up the exact row
//                                 that broke the system" UI)
//         * list                — paginated, filterable page of rows
//                                 (admin audit-logs list view)
//         * count               — COUNT(*) for the same filter, used
//                                 to fill `total` in list()'s result
//                                 so the front-end can render
//                                 "page 3 of 12" without a second
//                                 round-trip
//       Action constants:
//         * kActionProblemCreate / kActionProblemUpdate /
//           kActionProblemDelete / kActionProblemRestore /
//           kActionProblemBulkImport — problem module writes
//         * kActionUserRoleChange / kActionUserPasswordChange
//                                 — user module writes
//         * kActionLoginFailure  — anonymous login-failure tracker
//                                 (Phase 2; kept here for grep-ability
//                                 and so admin log filters can
//                                 surface auth events)
//
//   - All writes use parameterized SQL (`?` placeholders) — SPEC §15.2
//     forbids string concatenation. mysqlx::SqlStatement::bind() handles
//     the binding; user-supplied data never reaches the wire string.
//   - `created_at` is DATE_FORMAT'd to 'YYYY-MM-DD HH:MM:SS' on every
//     SELECT — same mysql-connector 9.x packed-binary issue that
//     problem_repo.h / user_repo.h work around (DATETIME columns come
//     back as raw bytes that `get<std::string>()` can't decode).
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3 module
//     (config.h / logger.h / server.h / user_repo.h / problem_repo.h
//     / tag_repo.h). The repo is essentially a thin set of free
//     functions over a ConnectionPool reference; no internal state to
//     unit-test.
//   - Returned rows are exposed via an `AuditRow` struct (not the raw
//     mysqlx::Row) so callers don't depend on mysqlx::Value's
//     semantics. `std::optional<AuditRow>` for "not found".
//   - `payload` round-trips as `std::string` (JSON text). The route
//     handler parses it back into nlohmann::json at the response
//     boundary; the repo never owns a parsed JSON object because the
//     shape is opaque (each action writes a different payload
//     schema) and forcing a parse would lose fidelity on
//     extensions like null / nested arrays.
//   - `record_best_effort` and the strict `record` diverge ONLY in
//     error handling — both build and execute the same INSERT, both
//     return the new id when they return at all. The split exists so
//     the Phase-2 login-failure path (which can't fail the request
//     because the request has already been answered 401) doesn't drag
//     a try/catch into every Phase-6 admin route.
//   - `record_login_failure` keeps its Phase-2 signature so
//     auth_routes.h continues to link unchanged; the implementation
//     is now a thin wrapper that builds the same AuditEntry the new
//     surface expects.
//   - The list filter is a small struct (same shape as
//     ProblemListFilter). Each filter field is independently
//     `std::optional` so the route handler can translate a JSON
//     query string into a filter without touching SQL.
//   - list() / count() build the WHERE clause + bind() chain
//     dynamically via `conn.session().sql(...)` and chained
//     `.bind(...)` calls — one round-trip per query, no per-pred-count
//     dispatch table.
//   - Concurrency: every public method acquires a fresh
//     PooledConnection from the pool, runs the SQL, releases. The
//     pool is thread-safe; individual methods do not need their
//     own locks.
//   - Row materialization helpers (req_int64, req_int, opt_int,
//     opt_string, req_string, row_to_audit) live inside
//     `audit_log_repo::detail` (NOT in the shared `litecode::detail`)
//     so they don't collide with `problem_repo::detail`'s
//     identically-named helpers when both headers are included in
//     the same TU (e.g. in tests or in admin_routes.h when it
//     eventually includes both).
//
// Usage (Phase 6 admin delete problem, future admin_routes.h):
//
//   litecode::AuditEntry e;
//   e.admin_id   = jwt_claims.user_id;
//   e.action     = litecode::audit_log_repo::kActionProblemDelete;
//   e.target_type= "problem";
//   e.target_id  = slug;
//   e.payload    = {{"slug", slug},
//                   {"title", row.title},
//                   {"hard_delete", false}};
//   e.ip         = req.remote_addr;
//   const std::int64_t new_id =
//       litecode::audit_log_repo::record(pool, e);
//   // route handler proceeds with the soft-delete UPDATE.
//
// Usage (Phase 6 admin audit-logs list view, future admin_routes.h):
//
//   litecode::AuditListFilter f;
//   f.limit       = 50;
//   f.offset      = 0;
//   f.admin_id    = std::optional<int>(5);     // filter to one admin
//   f.action      = std::optional<std::string>(
//                       litecode::audit_log_repo::kActionProblemDelete);
//   const auto page = litecode::audit_log_repo::list(pool, f);
//   // page.items is std::vector<AuditRow>; page.total is the COUNT(*)

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>

#include "../logger.h"             // LOG_WARN (best-effort writers)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  AuditRow
//
//  Plain-data projection of a row from the `audit_logs` table. All string
//  fields are std::string for safe lifetime — callers can copy, move,
//  or store the row across async boundaries.
//
//  Field semantics (mirrors SPEC §4.2d + V002):
//    - id: BIGINT PK; 0 ⇒ "not yet inserted". Stored as int64_t because
//          the audit table is append-only and grows forever (no
//          INT rollover risk).
//    - admin_id: optional. NULL when the action is system / anonymous
//                (Phase 2 login-failure uses NULL). ON DELETE SET NULL
//                in V002 means deleting an admin doesn't break the FK;
//                the historical row stays, just orphaned. Phase 6
//                routes that resolve `admin_username` from this id
//                should treat NULL as "system event".
//    - action: required, never empty, ≤ 50 chars.
//              Examples: "problem.create", "problem.delete",
//              "user.role_change", "auth.login_failure".
//    - target_type: optional; NULL when the action doesn't target a
//                   specific object (rare; reserved for future global
//                   events like "system.maintenance").
//    - target_id: optional; VARCHAR(100) so a problem slug (≤ 100),
//                 a user id (numeric), or any other opaque identifier
//                 fits. Stored as text so we don't have to encode the
//                 "what kind of id is this" semantics into the
//                 column type.
//    - payload: optional JSON text. NULL when no payload was supplied.
//               The route layer parses it back into nlohmann::json at
//               the response boundary.
//    - ip: optional IPv4 / IPv6 (≤ 45 chars per V002). Empty when
//          unknown (e.g. an internal scheduled job).
//    - created_at: ISO-8601 string ("YYYY-MM-DD HH:MM:SS"). DATE_FORMAT'd
//                  in the SELECT to dodge the mysql-connector 9.x
//                  packed-binary DATETIME bug.
// ────────────────────────────────────────────────────────────────────────────

struct AuditRow {
    std::int64_t                       id          = 0;
    std::optional<int>                 admin_id;
    std::string                        action;
    std::optional<std::string>         target_type;
    std::optional<std::string>         target_id;
    std::optional<std::string>         payload;     // JSON text
    std::optional<std::string>         ip;
    std::string                        created_at;
};

// ────────────────────────────────────────────────────────────────────────────
//  AuditEntry — input to record()
//
//  What the caller wants to write. Used by both the strict
//  `record()` (Phase 6 admin writes) and the best-effort
//  `record_best_effort()` (Phase 2 login-failure). `record_login_failure`
//  is itself a thin wrapper that fills this struct in.
//
//  `payload` defaults to an empty object so callers that don't care
//  about details don't have to construct one. The INSERT path
//  serializes it back to JSON text via `.dump()`.
// ────────────────────────────────────────────────────────────────────────────

struct AuditEntry {
    std::optional<int>                 admin_id;
    std::string                        action;
    std::optional<std::string>         target_type;
    std::optional<std::string>         target_id;
    nlohmann::json                     payload     = nlohmann::json::object();
    std::optional<std::string>         ip;
};

// ────────────────────────────────────────────────────────────────────────────
//  AuditListFilter — input to list() / count()
//
//  Each filter field is independently `std::optional` so the route
//  handler can translate a JSON query string into a filter without
//  touching SQL. Pagination is clamped via clamp_list_filter() (below)
//  so any caller — including a future bulk-import replay tool — gets
//  sane defaults.
//
//  `since` / `until` are DATETIME strings ('YYYY-MM-DD HH:MM:SS'). They
//  filter `created_at` inclusively at the lower bound and exclusively
//  at the upper bound (the half-open interval convention used by most
//  range APIs). NULL on either side ⇒ no bound.
// ────────────────────────────────────────────────────────────────────────────

struct AuditListFilter {
    std::optional<int>                 admin_id;
    std::optional<std::string>         action;
    std::optional<std::string>         target_type;
    std::optional<std::string>         target_id;
    std::optional<std::string>         since;       // created_at >= since
    std::optional<std::string>         until;       // created_at <  until
    int                                limit        = 20;   // 1..100
    int                                offset       = 0;    // >= 0
};

// ────────────────────────────────────────────────────────────────────────────
//  AuditListResult — output of list()
//
//  `total` is the COUNT(*) over the same filter, with the same
//  admin_id / action / target_type / target_id / since / until
//  predicates applied, so the front-end can render pagination UI
//  without a second round-trip. `items` is the page of rows for the
//  requested offset/limit, ordered newest-first (matches the
//  idx_audit_admin_time DESC direction in V002).
// ────────────────────────────────────────────────────────────────────────────

struct AuditListResult {
    std::vector<AuditRow>  items;
    std::int64_t           total  = 0;
    int                    limit  = 0;
    int                    offset = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  AuditLogRepoError — surface every repo-layer failure as a typed exception
//
//  Two tiers, mirroring the rest of Phase 2/3:
//    - AuditLogRepoError      — generic failure (driver error, etc.).
//                              Caught by the route handler and folded
//                              into 500. The strict record() throws
//                              this on every failure; record_best_effort
//                              swallows instead.
//    - AuditLogNotFoundError  — reserved for a future admin endpoint
//                              that needs to distinguish "no such row"
//                              (404) from "DB is down" (500) without
//                              using the std::nullopt convention. The
//                              find_by_id path uses std::nullopt today.
//
//  We deliberately do NOT model an "audit row already exists" tier —
//  audit_logs has no UNIQUE constraint and is append-only, so 409 is
//  meaningless here.
// ────────────────────────────────────────────────────────────────────────────

class AuditLogRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AuditLogNotFoundError : public AuditLogRepoError {
public:
    using AuditLogRepoError::AuditLogRepoError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Validation helpers (used by both repo + route layer)
//
//  These match SPEC §4.2d column types:
//    - action:     1..50 chars  (VARCHAR(50) NOT NULL)
//    - target_type: ≤ 50 chars  (VARCHAR(50) NULL)
//    - target_id:  ≤ 100 chars  (VARCHAR(100) NULL)
//    - ip:         ≤ 45 chars   (VARCHAR(45) NULL — IPv6 max length)
//
//  None of these columns are validated for charset (UTF-8 is fine for
//  target_id / target_type / action because the audit log UI renders
//  them as plain text). `ip` IS length-bounded so a typo doesn't
//  pollute the security trail with multi-KB strings.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kMinActionLength       = 1;
inline constexpr std::size_t kMaxActionLength       = 50;
inline constexpr std::size_t kMaxTargetTypeLength   = 50;
inline constexpr std::size_t kMaxTargetIdLength     = 100;
inline constexpr std::size_t kMaxIpLength           = 45;
inline constexpr std::size_t kMinDatetimeLength     = 10;   // 'YYYY-MM-DD'
inline constexpr std::size_t kMaxDatetimeLength     = 19;   // 'YYYY-MM-DD HH:MM:SS'
inline constexpr int         kDefaultAuditListLimit = 20;
inline constexpr int         kMaxAuditListLimit     = 100;

inline bool validate_action(std::string_view action,
                            std::string* error_out = nullptr) {
    if (action.size() < kMinActionLength || action.size() > kMaxActionLength) {
        if (error_out) {
            *error_out = "action length must be between " +
                         std::to_string(kMinActionLength) + " and " +
                         std::to_string(kMaxActionLength);
        }
        return false;
    }
    for (unsigned char c : action) {
        // Reject control chars (incl. NUL / newline / tab). A newline
        // in `action` would break the row's "single line of text"
        // grep-friendly contract.
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "action must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_target_type(std::string_view target_type,
                                 std::string* error_out = nullptr) {
    if (target_type.size() > kMaxTargetTypeLength) {
        if (error_out) {
            *error_out = "target_type length must be <= " +
                         std::to_string(kMaxTargetTypeLength);
        }
        return false;
    }
    for (unsigned char c : target_type) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "target_type must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_target_id(std::string_view target_id,
                               std::string* error_out = nullptr) {
    if (target_id.size() > kMaxTargetIdLength) {
        if (error_out) {
            *error_out = "target_id length must be <= " +
                         std::to_string(kMaxTargetIdLength);
        }
        return false;
    }
    for (unsigned char c : target_id) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "target_id must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

// Best-effort IP literal check: reject anything with control chars
// or that's longer than the VARCHAR(45) IPv6 maximum. We deliberately
// do NOT try to do a full inet_pton() parse here — see comment on
// the writers below for the trade-off discussion.
inline bool validate_ip(std::string_view ip,
                        std::string* error_out = nullptr) {
    if (ip.empty()) {
        if (error_out) { *error_out = "ip must not be empty"; }
        return false;
    }
    if (ip.size() > kMaxIpLength) {
        if (error_out) {
            *error_out = "ip length must be <= " +
                         std::to_string(kMaxIpLength);
        }
        return false;
    }
    for (unsigned char c : ip) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "ip must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

// Datetime string format guard. Accepts anything in
// [kMinDatetimeLength, kMaxDatetimeLength] bytes that contains no
// control characters. A stricter check would require real regex
// parsing; for now this is enough to reject obvious garbage (empty,
// "yesterday", "2025-13-40") at the validator boundary and let
// MySQL surface anything that slips past as a parse error on
// INSERT / SELECT.
inline bool validate_datetime(std::string_view s,
                              std::string* error_out = nullptr) {
    if (s.size() < kMinDatetimeLength || s.size() > kMaxDatetimeLength) {
        if (error_out) {
            *error_out = "datetime length must be between " +
                         std::to_string(kMinDatetimeLength) + " and " +
                         std::to_string(kMaxDatetimeLength);
        }
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "datetime must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

// Clamp the pagination fields to safe ranges. Done in the repo so
// any caller — including a future admin tool that builds a filter
// from a JSON query string — gets sane defaults.
inline void clamp_list_filter(AuditListFilter& f) {
    if (f.limit  <= 0)                  f.limit  = kDefaultAuditListLimit;
    if (f.limit  > kMaxAuditListLimit)  f.limit  = kMaxAuditListLimit;
    if (f.offset < 0)                   f.offset = 0;
}

// ────────────────────────────────────────────────────────────────────────────
//  Action constants — grep-friendly strings for the well-known actions
//  (SPEC §4.2d + §15.6)
//
//  Callers should use these constants instead of string literals so
//  the audit log filter UI can present a stable enumeration and the
//  test suite can match action strings exactly. Unknown actions
//  (forward-compat from a newer admin tool) are still allowed; the
//  filter is just a string equality.
// ────────────────────────────────────────────────────────────────────────────

namespace audit_log_repo {

inline constexpr const char* kActionProblemCreate     = "problem.create";
inline constexpr const char* kActionProblemUpdate     = "problem.update";
inline constexpr const char* kActionProblemDelete     = "problem.delete";
inline constexpr const char* kActionProblemRestore    = "problem.restore";
inline constexpr const char* kActionProblemBulkImport = "problem.bulk_import";
inline constexpr const char* kActionProblemSpjUpsert  = "problem.spj_upsert";     // v1.3.1
inline constexpr const char* kActionProblemSpjRemove  = "problem.spj_remove";     // v1.3.1
inline constexpr const char* kActionUserRoleChange    = "user.role_change";
inline constexpr const char* kActionUserPasswordChange= "user.password_change";
inline constexpr const char* kActionLoginFailure      = "auth.login_failure";
inline constexpr const char* kActionLoginLockout      = "auth.login_locked";

namespace detail {

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization helpers
//
//  Named and namespaced to dodge collisions with problem_repo::detail /
//  tag_repo::detail / user_repo::detail. They all want similar
//  primitives; the duplication is intentional (each repo can change
//  its error wording + log context without touching the others).
// ────────────────────────────────────────────────────────────────────────────

inline std::int64_t req_int64(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::int64_t>();
    } catch (const std::exception& e) {
        throw AuditLogRepoError(std::string("audit_log_repo: required field '") +
                                field + "' is not an int64: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw AuditLogRepoError(std::string("audit_log_repo: required field '") +
                                field + "' is not an int: " + e.what());
    }
}

inline int opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return 0;
    try {
        return static_cast<int>(v.get<std::int64_t>());
    } catch (const std::exception&) {
        return 0;
    }
}

inline std::optional<std::string> opt_string(const mysqlx::Row& row,
                                              std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try {
        return v.get<std::string>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw AuditLogRepoError(std::string("audit_log_repo: required field '") +
                                field + "' is not a string: " + e.what());
    }
}

// Column order for the audit_logs projection. Centralized so a schema
// change here is a one-liner.
//
// `payload` is JSON-typed. mysql-connector-c++ 9.x reports JSON
// columns in a way that `get<std::string>()` can't decode directly;
// CAST AS CHAR forces the driver to surface it as a regular
// VARCHAR so the round-trip is parseable by nlohmann::json at the
// route layer. (The earlier `SELECT payload, ...` form returned
// NULL for every row, even when the column was a populated JSON
// object, because the JSON column's binary representation couldn't
// be coerced to text by `get<std::string>()` on the 9.x driver.)
//
//   0 id           (BIGINT)
//   1 admin_id     (INT NULL)
//   2 action       (VARCHAR(50))
//   3 target_type  (VARCHAR(50) NULL)
//   4 target_id    (VARCHAR(100) NULL)
//   5 payload      (JSON → CAST AS CHAR; nullable)
//   6 ip           (VARCHAR(45) NULL)
//   7 created_at   (DATE_FORMAT'd → text)
inline constexpr const char* kAuditSelectColumns =
    "id, admin_id, action, target_type, target_id, "
    "CAST(payload AS CHAR) AS payload, ip, "
    "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at";

inline AuditRow row_to_audit(const mysqlx::Row& row) {
    AuditRow r;
    r.id          = req_int64   (row, 0, "id");
    {
        const int admin_v = opt_int(row, 1);
        if (admin_v > 0) r.admin_id = admin_v;
    }
    r.action      = req_string  (row, 2, "action");
    r.target_type = opt_string  (row, 3);
    r.target_id   = opt_string  (row, 4);
    r.payload     = opt_string  (row, 5);
    r.ip          = opt_string  (row, 6);
    r.created_at  = req_string  (row, 7, "created_at");
    return r;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API — writers
// ────────────────────────────────────────────────────────────────────────────

// record — strict INSERT into audit_logs. Throws AuditLogRepoError on
// every failure (driver error, FK violation, validation error).
//
// Returns the new BIGINT id (>0). Callers should propagate this id
// back to the admin UI so the operator can reference the row in
// follow-up audit work.
//
// Validation: re-validates action / target_type / target_id / ip on
// the EFFECTIVE values as defense in depth. The route handler is
// expected to have validated first; the goal here is "garbage in ⇒
// exception, never a corrupt row".
//
// FK behavior: admin_id references users(id) ON DELETE SET NULL. We
// do NOT pre-check that admin_id exists; the FK is the authoritative
// gate. A typo in the admin_id surfaces here as a 1452 error which
// is re-thrown as AuditLogRepoError — the route handler should map
// that to 500 and (in a future hardening pass) treat it as a
// programmer error, not a user error.
//
// Parameterized SQL per SPEC §15.2. `payload` is pre-serialized to
// std::string via nlohmann::json::dump() and bound as a VARCHAR —
// MySQL parses the bound string as JSON because the column type
// is JSON.
//
// `action` is required (NOT NULL); the validator rejects an empty
// string. `payload` is optional in the schema but the AuditEntry
// defaults it to an empty JSON object so the column always has a
// value (the route layer can re-parse "{}" without special-casing).
inline std::int64_t record(ConnectionPool& pool, const AuditEntry& e) {
    {
        std::string err;
        if (!validate_action    (e.action,                       &err)) {
            throw AuditLogRepoError("record: " + err);
        }
        if (e.target_type.has_value() &&
            !validate_target_type(*e.target_type,                &err)) {
            throw AuditLogRepoError("record: " + err);
        }
        if (e.target_id.has_value() &&
            !validate_target_id  (*e.target_id,                  &err)) {
            throw AuditLogRepoError("record: " + err);
        }
        if (e.ip.has_value() &&
            !validate_ip         (*e.ip,                          &err)) {
            throw AuditLogRepoError("record: " + err);
        }
    }

    const std::string action_str    = e.action;
    const std::string payload_str   = e.payload.dump();
    const std::string target_type_s = e.target_type.value_or(std::string());
    const std::string target_id_s   = e.target_id.value_or(std::string());
    const std::string ip_s          = e.ip.value_or(std::string());

    auto conn = pool.acquire();
    try {
        // Nullable columns are bound to mysqlx::Value(nullptr)
        // when the caller didn't supply them. We use the chained-
        // bind form on session().sql(...) so the bind-arg count
        // is dynamic at runtime (instead of compile-time like
        // PooledConnection::execute's variadic template would
        // force).
        mysqlx::SqlStatement stmt = conn.session().sql(
            "INSERT INTO audit_logs "
            "(admin_id, action, target_type, target_id, payload, ip) "
            "VALUES (?, ?, ?, ?, ?, ?)");

        if (e.admin_id.has_value()) {
            stmt.bind(static_cast<std::int64_t>(*e.admin_id));
        } else {
            stmt.bind(mysqlx::Value(nullptr));
        }
        stmt.bind(action_str);
        if (e.target_type.has_value()) stmt.bind(target_type_s);
        else                           stmt.bind(mysqlx::Value(nullptr));
        if (e.target_id.has_value())   stmt.bind(target_id_s);
        else                           stmt.bind(mysqlx::Value(nullptr));
        stmt.bind(payload_str);
        if (e.ip.has_value())          stmt.bind(ip_s);
        else                           stmt.bind(mysqlx::Value(nullptr));

        mysqlx::SqlResult rs = stmt.execute();
        // mysqlx surfaces the auto-increment value via
        // getAutoIncrementValue(). For BIGINT AUTO_INCREMENT this
        // is a int64_t (matches the table's PK type).
        return rs.getAutoIncrementValue();
    } catch (const AuditLogRepoError&) {
        throw;
    } catch (const mysqlx::Error& e) {
        throw AuditLogRepoError(std::string("audit_log_repo::record: ") + e.what());
    } catch (const std::exception& e) {
        throw AuditLogRepoError(std::string("audit_log_repo::record: ") + e.what());
    }
}

// record_best_effort — same INSERT path as record(), but LOG_WARNs
// and swallows on any exception. Returns the new id when the INSERT
// succeeded; returns 0 when the INSERT failed.
//
// Used by callers that can't propagate the failure (Phase 2 login-
// failure tracker: by the time we'd write the audit row, the login
// has already returned 401 to the client, so failing here would
// leave the user with a 5xx for a security logbook glitch).
//
// The logging is intentionally cheap — just one LOG_WARN per failure
// with `action` + the error message. A future hardening pass could
// add a "audit write failures" counter for /api/v1/metrics so
// operators can spot when the security trail goes dark.
inline std::int64_t record_best_effort(ConnectionPool& pool,
                                       const AuditEntry& e) {
    try {
        return record(pool, e);
    } catch (const std::exception& ex) {
        LOG_WARN("audit_log_repo::record_best_effort failed",
                 {{"action", e.action},
                  {"reason", ex.what()}});
        return 0;
    }
}

// record_login_failure — thin best-effort wrapper for the Phase 2
// login-failure tracker. Preserves the original Phase-2 signature so
// auth_routes.h (which already calls it) doesn't have to change.
//
// Builds an AuditEntry equivalent to the original hand-coded INSERT:
//   action       = "auth.login_failure"
//   target_type  = "user"
//   target_id    = username                 (best-effort — we don't
//                                            know the user_id when the
//                                            username is wrong)
//   admin_id     = NULL                      (login is anonymous-ish)
//   payload      = { "consecutive_failures": N }
//   ip           = client IP                 (NULL if empty)
//
// `consecutive_failures` is the count BEFORE this attempt. The route
// handler tracks its own per-username counter (in-memory) and decides
// when to call this — currently at every 5th consecutive failure, so
// the audit table only sees things the operator cares about.
inline void record_login_failure(ConnectionPool&  pool,
                                 std::string_view username,
                                 std::string_view ip,
                                 int              consecutive_failures) {
    AuditEntry e;
    e.action      = kActionLoginFailure;
    e.target_type = std::string("user");
    e.target_id   = std::string(username);
    e.payload     = { {"consecutive_failures", consecutive_failures} };
    if (!ip.empty()) {
        e.ip = std::string(ip);
    }
    (void)record_best_effort(pool, e);
}

// record_login_lockout — Phase 6 ☆ v1.2.46 companion to
// record_login_failure. Fires when a username crosses the configured
// lockout threshold (SPEC §15.1: "失败登录锁定 — 连续 N 次失败 15
// 分钟内禁止该用户名登录"). Best-effort, never throws.
//
// Builds:
//   action       = "auth.login_locked"
//   target_type  = "user"
//   target_id    = username
//   admin_id     = NULL
//   payload      = {
//                    "consecutive_failures":   N,   // the count that
//                                                   // triggered the lockout
//                    "locked_for_seconds":     D,   // how long the lockout
//                                                   // will last from the
//                                                   // triggering attempt
//                    "threshold":              T,   // the configured
//                                                   // threshold (audit
//                                                   // debug-aid so an
//                                                   // operator looking at
//                                                   // the row can see the
//                                                   // policy at the time
//                                                   // of the event)
//                  }
//   ip           = client IP (NULL if empty)
//
// Why a separate action and not "auth.login_failure with a flag"?
//   The admin audit-logs UI (v1.2.35) filters by action string; a
//   dedicated `auth.login_locked` action makes the brute-force signal
//   trivially filterable without parsing the payload.
inline void record_login_lockout(ConnectionPool&  pool,
                                 std::string_view username,
                                 std::string_view ip,
                                 int              consecutive_failures,
                                 int              locked_for_seconds,
                                 int              threshold) {
    AuditEntry e;
    e.action      = kActionLoginLockout;
    e.target_type = std::string("user");
    e.target_id   = std::string(username);
    e.payload     = {
        {"consecutive_failures", consecutive_failures},
        {"locked_for_seconds",   locked_for_seconds},
        {"threshold",            threshold},
    };
    if (!ip.empty()) {
        e.ip = std::string(ip);
    }
    (void)record_best_effort(pool, e);
}

// ────────────────────────────────────────────────────────────────────────────
//  Public API — readers
// ────────────────────────────────────────────────────────────────────────────

// find_by_id — load one audit_logs row by primary key. Returns
// std::nullopt when no such row exists. The admin detail view uses
// this; the list view doesn't (list() with a tight filter is the
// common path).
//
// Throws AuditLogRepoError on driver / SQL errors. The
// AuditLogNotFoundError typed exception is reserved for a future
// admin endpoint that wants to distinguish 404 from 500 without
// using the std::nullopt convention.
inline std::optional<AuditRow> find_by_id(ConnectionPool& pool,
                                          std::int64_t    id) {
    auto conn = pool.acquire();
    try {
        const auto row = conn.fetch_one(
            std::string("SELECT ") + detail::kAuditSelectColumns +
            " FROM audit_logs WHERE id = ? LIMIT 1",
            id);
        if (!row) return std::nullopt;
        return detail::row_to_audit(*row);
    } catch (const mysqlx::Error& e) {
        throw AuditLogRepoError(std::string("audit_log_repo::find_by_id: ") + e.what());
    }
}

// build_where_clause — shared SQL builder for count() / list().
//
// Returns the WHERE clause (without the leading "WHERE " / "AND ")
// AND records the bind-arg order so the caller can chain
// `stmt.bind(...)` in the same order. Centralizing this means the
// predicate / bind lists can never get out of sync.
//
// `bound_int` / `bound_str` are filled in the order their respective
// predicates appear in the WHERE clause (admin_id, action,
// target_type, target_id, since, until). Both are empty vectors
// when the corresponding optional is unset.
namespace detail {

struct WhereClause {
    std::string text;
    // Empty when no filter is set; either way the order matches the
    // WHERE-clause predicate order.
    std::vector<int>            bound_int;
    std::vector<std::string>    bound_str;
};

inline WhereClause build_where_clause(const AuditListFilter& f,
                                     std::string* error_out) {
    WhereClause w;
    bool first = true;
    auto append = [&](const char* clause) {
        w.text += (first ? " WHERE " : " AND ");
        w.text += clause;
        first = false;
    };
    if (f.admin_id.has_value()) {
        append("admin_id = ?");
        w.bound_int.push_back(*f.admin_id);
    }
    if (f.action.has_value()) {
        append("action = ?");
        w.bound_str.push_back(*f.action);
    }
    if (f.target_type.has_value()) {
        append("target_type = ?");
        w.bound_str.push_back(*f.target_type);
    }
    if (f.target_id.has_value()) {
        append("target_id = ?");
        w.bound_str.push_back(*f.target_id);
    }
    if (f.since.has_value()) {
        std::string err;
        if (!validate_datetime(*f.since, &err)) {
            if (error_out) { *error_out = "since: " + err; }
            return w;
        }
        append("created_at >= ?");
        w.bound_str.push_back(*f.since);
    }
    if (f.until.has_value()) {
        std::string err;
        if (!validate_datetime(*f.until, &err)) {
            if (error_out) { *error_out = "until: " + err; }
            return w;
        }
        append("created_at <  ?");
        w.bound_str.push_back(*f.until);
    }
    return w;
}

} // namespace detail

// count — total rows matching `filter`. Used by list() to fill the
// `total` field on the result. Same predicates as list() so the two
// stay in sync.
//
// Note: there's no UNIQUE constraint on audit_logs, so this is a
// straightforward index scan / full scan depending on which
// predicates fire. The (admin_id, created_at DESC) and (action,
// created_at) indexes from V002 cover the common filter shapes
// (admin history / action type).
inline std::int64_t count(ConnectionPool& pool, const AuditListFilter& filter) {
    AuditListFilter f = filter;
    clamp_list_filter(f);

    detail::WhereClause w;
    {
        std::string err;
        w = detail::build_where_clause(f, &err);
        if (!err.empty()) {
            throw AuditLogRepoError("audit_log_repo::count: " + err);
        }
    }

    const std::string sql = "SELECT COUNT(*) FROM audit_logs" + w.text;
    auto conn = pool.acquire();
    try {
        // Chained bind form: each optional predicate adds one bind
        // call in the order it appears in the WHERE clause. The
        // mysqlx driver handles the positional `?` mapping.
        mysqlx::SqlStatement stmt = conn.session().sql(sql);
        for (int v : w.bound_int) stmt.bind(static_cast<std::int64_t>(v));
        for (const auto& s : w.bound_str) stmt.bind(s);
        mysqlx::SqlResult rs = stmt.execute();
        // COUNT(*) comes back as a number (int64 / decimal). The
        // driver auto-converts via get<int64_t>; we wrap in try/catch
        // because very large counts (>2^63) come back as DECIMAL.
        mysqlx::Row row = rs.fetchOne();
        try {
            return row[0].get<std::int64_t>();
        } catch (const std::exception&) {
            // Fall back to string round-trip for the DECIMAL case.
            try {
                return static_cast<std::int64_t>(std::stoll(row[0].get<std::string>()));
            } catch (...) {
                return 0;
            }
        }
    } catch (const AuditLogRepoError&) {
        throw;
    } catch (const mysqlx::Error& e) {
        throw AuditLogRepoError(std::string("audit_log_repo::count: ") + e.what());
    }
}

// list — paginated, filterable page of audit_logs rows. Returns
// {items, total, limit, offset}. `total` is the unpaginated count
// for the same filter (so the front-end renders "page 3 of 12"
// without a second round-trip).
//
// Ordering is fixed: created_at DESC, id DESC. Matches the
// idx_audit_admin_time DESC direction in V002 and the "newest
// first" expectation from the admin UI. The id DESC tie-breaker
// keeps pages stable when two rows share a created_at second (rare
// in practice but possible under burst load).
inline AuditListResult list(ConnectionPool& pool,
                            const AuditListFilter& filter) {
    AuditListFilter f = filter;
    clamp_list_filter(f);

    AuditListResult out;
    out.limit  = f.limit;
    out.offset = f.offset;
    out.total  = count(pool, f);

    detail::WhereClause w;
    {
        std::string err;
        w = detail::build_where_clause(f, &err);
        if (!err.empty()) {
            throw AuditLogRepoError("audit_log_repo::list: " + err);
        }
    }

    std::string sql = "SELECT ";
    sql += detail::kAuditSelectColumns;
    sql += " FROM audit_logs";
    sql += w.text;
    sql += " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?";

    auto conn = pool.acquire();
    try {
        mysqlx::SqlStatement stmt = conn.session().sql(sql);
        for (int v : w.bound_int) stmt.bind(static_cast<std::int64_t>(v));
        for (const auto& s : w.bound_str) stmt.bind(s);
        stmt.bind(f.limit);
        stmt.bind(f.offset);

        mysqlx::SqlResult rs = stmt.execute();
        out.items.reserve(static_cast<std::size_t>(f.limit));
        for (auto row : rs) {
            out.items.push_back(detail::row_to_audit(row));
        }
        return out;
    } catch (const AuditLogRepoError&) {
        throw;
    } catch (const mysqlx::Error& e) {
        throw AuditLogRepoError(std::string("audit_log_repo::list: ") + e.what());
    } catch (const std::exception& e) {
        throw AuditLogRepoError(std::string("audit_log_repo::list: ") + e.what());
    }
}

} // namespace audit_log_repo
} // namespace litecode