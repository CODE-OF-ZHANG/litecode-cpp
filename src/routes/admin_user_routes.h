// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — admin user management routes (Phase 6 ★)
//
// SPEC §5.5 / §11 Phase 6 / §15.2 / §15.6 / A22, A24, A27 acceptance:
//   - GET  /api/v1/admin/users              (🔒 admin, 60/min/user)
//       Paginated user list with optional role + q (username search)
//       filters. Drives the /admin/users.html page (v1.2.33).
//   - PUT  /api/v1/admin/users/:id/role     (🔒 admin, 10/min/user)
//       Change a user's role between "user" and "admin". Writes one
//       row to `audit_logs` (kActionUserRoleChange). Drives the
//       "降为普通用户 / 提升为管理员" buttons on the same page.
//
// Both endpoints follow the canonical admin 6-step pipeline:
//   1) require_admin(...)    — 401 envelope if no / bad token,
//                              403 if the authenticated user isn't
//                              an admin
//   2) consume_rate_limit(...)  — 429 envelope on deny
//   3) parse query / path / body + validate (400 on shape / value)
//   4) dispatch to user_repo
//   5) audit_log_repo::record (strict) on the role-change path
//   6) respond 200/201 + the unified success envelope
//
// Wire shapes:
//
//   GET /api/v1/admin/users?role=user&admin&q=alice&page=2&limit=20
//     200 + {
//       "data": {
//         "items": [
//           { "id": 42, "username": "alice", "email": "..." | null,
//             "role": "user", "created_at": "...",
//             "last_login":    "..." | null,
//             "last_login_ip": "..." | null,
//             "submission_count": 17 },
//           ...
//         ],
//         "total":  123,
//         "limit":  20,
//         "offset": 20,
//       },
//       "request_id": "..."
//     }
//
//   PUT /api/v1/admin/users/:id/role
//     body:  {"role": "user" | "admin"}
//     200 + {
//       "data": {
//         "id": 42, "username": "alice", "email": "..." | null,
//         "role": "admin", "created_at": "...",
//         "last_login": "..." | null,
//         "last_login_ip": "..." | null
//       },
//       "request_id": "..."
//     }
//
// Audit log payload for user.role_change:
//   { "username":  "alice",
//     "old_role":  "user",
//     "new_role":  "admin" }
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3/4/5
//     module. Tests link this header directly and instantiate the
//     route set with a real ConnectionPool + RateLimiter + JwtConfig.
//   - We surface the list response WITHOUT password_hash (admin
//     can see role, email, last_login_ip, etc., but never the
//     password material — even an admin shouldn't be able to
//     grab the bcrypt hash from a single GET).
//   - Rate limits: the list is 60/min/user (a healthy cap for the
//     /admin/users.html page) and the role change is 10/min/user
//     (deliberate, since role changes are destructive). The two
//     have separate bucket names ("admin.users.list" vs
//     "admin.users.role") so a busy operator scripting role
//     changes can't starve the list view.
//   - Self-protection: an admin CAN demote themselves, but
//     we don't add a server-side guard for it. The front-end
//     shows a "当前账号" placeholder for the current admin's
//     own row (v1.2.33), and we let the API surface the action
//     so a future "team accountability" workflow can audit
//     self-demotions. The audit row will record who did what
//     to whom (admin_id = demotee in this case).
//   - ODR caveat: this header transitively pulls user_repo.h +
//     audit_log_repo.h, both of which define helpers in
//     `user_repo::detail` / `audit_log_repo::detail` (NOT
//     `litecode::detail`) to dodge the cross-repo ODR collision
//     the Phase 3 test_*_repo binaries documented. main.cpp
//     does NOT smoke-register this header for the same reason
//     it doesn't register admin_problem_routes /
//     admin_bulk_import_routes / submission_routes /
//     problem_routes. End-to-end coverage is owned by
//     tests/unit/test_admin_users.cpp.
//   - The user_repo.h extension (UserListFilter / count_users /
//     list_users / UserListRow) lives there, not here, because
//     the list / count primitives are independent of HTTP and
//     could be reused by a future CLI / background job.
//
// Usage (production, from main.cpp):
//
//   litecode::HttpServer     server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter    limiter;
//   litecode::register_admin_user_routes(server, pool, limiter,
//                                        cfg.rate_limit, cfg.jwt);
//   server.listen_blocking();
//
// Usage (test, from gtest):
//
//   litecode::HttpServer     server(dev_server(), dev_cors());
//   litecode::ConnectionPool pool(test_db_config());
//   litecode::RateLimiter    limiter;
//   litecode::JwtConfig      jwt_cfg = test_jwt_config();
//   litecode::register_admin_user_routes(server, pool, limiter,
//                                        lax_rate_limit(), jwt_cfg);
//   auto h = start_server(&server);
//   auto r = h.client->Get("/api/v1/admin/users?q=alice", admin_token);

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "../config.h"                          // RateLimitConfig / JwtConfig
#include "../db/audit_log_repo.h"               // audit_log_repo::record + kActionUserRoleChange
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/user_repo.h"                    // user_repo::list / count / find_by_id / update_role
#include "../logger.h"                          // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../middleware/admin_middleware.h"     // require_admin
#include "../middleware/rate_limit.h"           // consume_rate_limit / admin_users_*_quota / extract_client_ip
#include "../routes/error_handler.h"            // parse_json_body / ErrorCode / send_error
#include "../server.h"                          // HttpServer / send_success / send_error

namespace litecode {

namespace admin_user_routes {

// ────────────────────────────────────────────────────────────────────────────
//  Parsing helpers — local namespace to dodge cross-route ODR collisions.
//  Mirrors the pattern used in problem_routes.h / admin_problem_routes.h.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline constexpr std::size_t kAdminUserValueMax = 64;

// truncate_for_envelope — keeps the value field in 400/422 envelopes
// bounded to 64 chars (a hostile value could otherwise be multi-MB
// and balloon the error body). The "+..." suffix is appended to make
// the truncation obvious to the admin reading the error in the UI.
inline std::string truncate_for_envelope(const std::string& v) {
    if (v.size() <= kAdminUserValueMax) return v;
    return v.substr(0, kAdminUserValueMax) + "...";
}

// parse_role_param — accept only "user" or "admin" (case-insensitive).
// Empty / absent ⇒ nullopt (handler keeps filter as nullopt, no
// filtering). Any other value ⇒ nullopt (handler writes a 400).
inline std::optional<std::string> parse_role_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    // Lowercase the input without allocating via std::string.
    std::string lc;
    lc.reserve(raw.size());
    for (char c : raw) {
        lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lc == "user"  || lc == "admin") return lc;
    return std::nullopt;
}

// parse_list_query — translate req.{has_param, get_param_value}
// into a user_repo::UserListFilter. Recognized query params:
//   - role  (optional; "user" | "admin")
//   - q     (optional; username substring; trimmed)
//   - limit (optional; default 20, range [1, 100])
//   - offset (optional; default 0, >= 0)
//
// Bad shape writes a 400 envelope and returns false; success returns
// true and populates `out`.
inline bool parse_list_query(const httplib::Request&      req,
                             httplib::Response&          res,
                             user_repo::UserListFilter&  out) {
    out = user_repo::UserListFilter{};

    if (req.has_param("role")) {
        const std::string raw = req.get_param_value("role");
        const auto v = parse_role_param(raw);
        if (!v.has_value()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "role must be one of: user, admin",
                       {{"field", "role"},
                        {"value", detail::truncate_for_envelope(raw)}});
            return false;
        }
        out.role = *v;
    }
    if (req.has_param("q")) {
        const std::string raw = req.get_param_value("q");
        // Trim leading / trailing whitespace; the trim is
        // intentionally simple (no Unicode whitespace) because
        // the username charset is ASCII-only.
        std::size_t a = 0, b = raw.size();
        while (a < b && std::isspace(static_cast<unsigned char>(raw[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(raw[b - 1]))) --b;
        const std::string trimmed = raw.substr(a, b - a);
        if (!trimmed.empty()) {
            // Cap search length so a hostile q=AAAA...AAAA can't
            // pin the LIKE pattern's left-anchored wildcard.
            // 50 chars matches the username max so the search
            // term can fully span a username.
            if (trimmed.size() > kMaxUsernameLength) {
                send_error(res, 400, ErrorCode::INVALID_INPUT,
                           "q is too long",
                           {{"field", "q"},
                            {"value", detail::truncate_for_envelope(trimmed)}});
                return false;
            }
            out.q = trimmed;
        }
    }
    if (req.has_param("limit")) {
        const std::string raw = req.get_param_value("limit");
        if (raw.empty()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
        try {
            std::size_t consumed = 0;
            const int v = std::stoi(raw, &consumed);
            if (consumed != raw.size() || v < 1) {
                send_error(res, 400, ErrorCode::INVALID_INPUT,
                           "limit must be a positive integer",
                           {{"field", "limit"},
                            {"value", raw}});
                return false;
            }
            out.limit = v;
        } catch (const std::exception&) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
    }
    if (req.has_param("offset")) {
        const std::string raw = req.get_param_value("offset");
        if (raw.empty()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
        try {
            std::size_t consumed = 0;
            const int v = std::stoi(raw, &consumed);
            if (consumed != raw.size() || v < 0) {
                send_error(res, 400, ErrorCode::INVALID_INPUT,
                           "offset must be a non-negative integer",
                           {{"field", "offset"},
                            {"value", raw}});
                return false;
            }
            out.offset = v;
        } catch (const std::exception&) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
    }
    // Clamp at the end so a future caller can read back the
    // effective limit / offset from the filter struct.
    user_repo::clamp_user_list_filter(out);
    return true;
}

// parse_id_param — extract the user id from a path like
// "/api/v1/admin/users/123/role" (the regex in register_admin_user_routes
// has already captured the id substring).
//
// We accept only ASCII digits, no leading sign, no whitespace,
// no nested path. Returns nullopt on any shape failure (the
// handler maps to 400). Range is [1, INT_MAX] (a 0 or negative
// id is rejected up front so we don't bother the repo with a
// pathological query).
inline std::optional<int> parse_id_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    if (raw.size() > 11) return std::nullopt;   // INT_MAX = 2147483647 (10 digits)
    for (char c : raw) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        const long v = std::stol(std::string(raw));
        if (v < 1 || v > 2147483647L) return std::nullopt;
        return static_cast<int>(v);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// extract_id_from_path — strip the "/api/v1/admin/users/" prefix
// and the "/role" suffix, returning the captured id string (still
// text — the handler validates + converts to int).
inline std::optional<std::string_view> extract_id_from_path(
        const httplib::Request& req) {
    static constexpr std::string_view kPrefix = "/api/v1/admin/users/";
    static constexpr std::string_view kSuffix = "/role";
    const std::string& path = req.path;
    if (path.size() <= kPrefix.size() + kSuffix.size()) return std::nullopt;
    if (path.compare(0, kPrefix.size(), kPrefix) != 0)  return std::nullopt;
    if (path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return std::nullopt;
    }
    const std::string_view body = std::string_view(path).substr(
        kPrefix.size(),
        path.size() - kPrefix.size() - kSuffix.size());
    if (body.empty()) return std::nullopt;
    if (body.find('/') != std::string_view::npos) return std::nullopt;
    return body;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Row -> JSON serializers
//
//  We deliberately do NOT include `password_hash` in any response
//  — an admin reading /api/v1/admin/users shouldn't see the bcrypt
//  material (defense in depth: a stolen admin token doesn't grant
//  the attacker the user table's password hashes).
//
//  We DO include `last_login_ip` because the admin UI surfaces it
//  (v1.2.33) to help spot "an account I don't recognize logged in
//  from 1.2.3.4 at 03:00" — but the route never returns a raw
//  password hash, even to admin.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_user_admin_row(const litecode::UserRow& u) {
    nlohmann::json j = {
        {"id",         u.id},
        {"username",   u.username},
        {"role",       u.role},
        {"created_at", u.created_at},
    };
    if (u.email.has_value())        j["email"]         = *u.email;
    else                            j["email"]         = nullptr;
    if (u.last_login.has_value())   j["last_login"]    = *u.last_login;
    else                            j["last_login"]    = nullptr;
    if (u.last_login_ip.has_value()) j["last_login_ip"] = *u.last_login_ip;
    else                            j["last_login_ip"] = nullptr;
    return j;
}

inline nlohmann::json serialize_user_list_item(
        const litecode::user_repo::UserListRow& r) {
    nlohmann::json j = serialize_user_admin_row(r.user);
    j["submission_count"] = r.submission_count;
    return j;
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/admin/users   (SPEC §5.5, A22)
//
//  Wire flow:
//    1) require_admin(...)                  — 401 / 403
//    2) consume_rate_limit(admin_users_list_quota) — 429
//    3) parse_list_query                    — 400 on bad shape
//    4) user_repo::count_users + list_users (clamp limit / offset)
//       - throws on driver error → 500
//    5) serialize + send_success(200)
//
//  Authorization: admin only. Anonymous → 401, non-admin → 403
//  (handled by require_admin in one call).
// ────────────────────────────────────────────────────────────────────────────

inline void list_admin_users_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    // 1) Admin gate. require_admin throws ApiException(401, ...) or
    //    ApiException(403, ...); server.h's per-request wrap emits
    //    the envelope. We don't need to capture the returned Claims
    //    because the list endpoint doesn't audit-log (read paths
    //    aren't auditable per SPEC §5.6 / §15.6).
    require_admin(req, jwt_cfg);

    // 2) Rate limit (admin.users.list bucket, keyed by user_id).
    consume_rate_limit(res, req, limiter, admin_users_list_quota(rate_cfg));

    // 3) Parse query.
    user_repo::UserListFilter f;
    if (!detail::parse_list_query(req, res, f)) {
        return;  // 400 already on the wire
    }

    // 4) count + list. We do them as two round-trips; the front-end
    //    needs `total` to render pagination. The cost is one extra
    //    SELECT, but admin lists are infrequent (a few per minute
    //    per admin) so this is well under the SPEC §12.2 budget.
    int total = 0;
    std::vector<user_repo::UserListRow> rows;
    try {
        total = user_repo::count_users(pool, f);
        if (total > 0) {
            // Skip the list query when total=0 (no rows means the
            // pagination window is empty regardless of offset).
            rows = user_repo::list_users(pool, f);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("admin_users_list: repo threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 5) Serialize.
    nlohmann::json items = nlohmann::json::array();
    for (const auto& row : rows) {
        items.push_back(serialize_user_list_item(row));
    }

    LOG_INFO("admin_users_list: served",
             {{"total",    std::to_string(total)},
              {"returned", std::to_string(items.size())},
              {"limit",    std::to_string(f.limit)},
              {"offset",   std::to_string(f.offset)},
              {"has_role", f.role.has_value() ? "true" : "false"},
              {"has_q",    f.q.has_value()    ? "true" : "false"}});

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  total},
        {"limit",  f.limit},
        {"offset", f.offset},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  PUT /api/v1/admin/users/:id/role   (SPEC §5.5, A24, A27)
//
//  Wire flow:
//    1) require_admin(...)                   — 401 / 403
//    2) consume_rate_limit(admin_users_role_quota) — 429
//    3) extract_id_from_path + parse_id_param — 400 on bad shape
//    4) parse_json_body + per-field validation — 400 on bad shape
//    5) user_repo::find_by_id                — 404 if no such user
//    6) user_repo::update_role               — 0 rows affected → 404
//                                              (race: deleted between
//                                               find and update)
//    7) audit_log_repo::record (strict)      — 500 on log failure
//    8) re-fetch + send_success(200)
//
//  Authorization: admin only.
//
//  Self-demote: a logged-in admin CAN demote themselves (we don't
//  add a server-side guard for it; the front-end hides the
//  button for the current admin's own row at v1.2.33). If an
//  admin demotes themselves, their live access token still
//  works for the rest of its 2h TTL — that's by design (revoking
//  live access tokens is out of MVP scope per SPEC §15.1). The
//  audit row records `admin_id == target_id` so the security
//  trail stays intact.
//
//  Audit log payload:
//    {
//      "username": "<target username>",
//      "old_role": "user" | "admin",
//      "new_role": "user" | "admin"
//    }
//
//  What we DON'T do here:
//    - We don't write a row when the role is unchanged (idempotent
//      no-op). The UPDATE returns affected=0 (mysql) but the
//      repo's `update_role` reports true if the row matched; we
//      detect the "same role" case in the handler by comparing
//      the old role string to the new one and skipping both the
//      repo write AND the audit row when they match. An operator
//      who clicks "提升为管理员" twice gets one audit row, not
//      two. The response is 200 with the (unchanged) user row
//      in either case.
// ────────────────────────────────────────────────────────────────────────────

inline void change_user_role_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    // 1) Admin gate. We capture the Claims so we can use the
    //    admin's user_id / username in the audit row.
    const Claims admin_claims = require_admin(req, jwt_cfg);

    // 2) Rate limit (admin.users.role bucket — 10/min/user, the
    //    tightest of all admin write paths).
    consume_rate_limit(res, req, limiter, admin_users_role_quota(rate_cfg));

    // 3) Path → id.
    const auto id_str_opt = detail::extract_id_from_path(req);
    if (!id_str_opt.has_value()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "user id path component is invalid",
                   {{"field", "id"}});
        return;
    }
    const auto id_opt = detail::parse_id_param(*id_str_opt);
    if (!id_opt.has_value()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "user id must be a positive integer",
                   {{"field", "id"},
                    {"value", detail::truncate_for_envelope(std::string(*id_str_opt))}});
        return;
    }
    const int target_id = *id_opt;

    // 4) Body parse + role validation. Only "user" / "admin" are
    //    accepted; the user_repo::update_role precondition throws
    //    on anything else, but we surface a friendlier 400 first
    //    so the operator knows which field was wrong.
    const auto body = parse_json_body(req, res);
    if (!body) return;

    if (!body->contains("role") || !(*body)["role"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string field 'role'",
                   {{"field", "role"}});
        return;
    }
    const std::string new_role = (*body)["role"].get<std::string>();
    if (new_role != "user" && new_role != "admin") {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "role must be one of: user, admin",
                   {{"field", "role"},
                    {"value", detail::truncate_for_envelope(new_role)}});
        return;
    }

    // ★ v1.3.4 PR 11 — admin 禁止自降权(server-side guard)。
    // 之前只有前端在 users.html:455-460 / 614-618 做 client-side guard,
    // 直接 curl PUT 就能 bypass 自降,破坏唯一超级管理员不变量。本 PR
    // 在 server 层加 403 兜底,前端保留(双层防御)。
    // 例外:admin 把自己的 role 设为 admin(no-op,前面已经早退);admin
    // 互相提权也是合法的(由其他 admin 的 token 调),所以仅禁止自降。
    int admin_user_id = 0;
    try {
        admin_user_id = std::stoi(admin_claims.user_id);
    } catch (const std::exception&) {
        // user_id 解析失败,继续走下面 (target_id != admin_user_id)
        // 必然成立,守卫自然 fail-open;不抛错避免影响合法路径。
        admin_user_id = -1;
    }
    if (new_role == "user" && target_id == admin_user_id) {
        LOG_WARN("admin_users_role: self-demote denied",
                 {{"target_id",   std::to_string(target_id)},
                  {"admin_id",    admin_claims.user_id},
                  {"admin_uname", admin_claims.username}});
        send_error(res, 403, ErrorCode::FORBIDDEN,
                   "管理员账户不可自降为普通用户",
                   {{"reason", "admin_self_demote_forbidden"},
                    {"admin_id", std::to_string(admin_user_id)}});
        return;
    }

    // 5) Find the target user (404 if missing). We need the
    //    username for the audit log payload, and the current role
    //    so we can detect the no-op case below.
    std::optional<UserRow> target;
    try {
        target = user_repo::find_by_id(pool, target_id);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_users_role: find_by_id threw",
                  {{"user_id", std::to_string(target_id)},
                   {"type",    typeid(e).name()},
                   {"reason",  e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!target.has_value()) {
        send_error(res, 404, ErrorCode::NOT_FOUND,
                   "user not found",
                   {{"user_id", target_id}});
        return;
    }

    const std::string old_role = target->role;
    const std::string target_username = target->username;

    // 6) No-op short-circuit: if the role is unchanged, skip both
    //    the UPDATE and the audit row. The response is 200 with
    //    the existing user row.
    if (old_role == new_role) {
        LOG_INFO("admin_users_role: no-op (role unchanged)",
                 {{"user_id",     std::to_string(target_id)},
                  {"username",    target_username},
                  {"role",        new_role},
                  {"admin_id",    admin_claims.user_id},
                  {"admin_uname", admin_claims.username}});
        send_success(res, serialize_user_admin_row(*target));
        return;
    }

    // 7) UPDATE the role. `update_role` returns true iff a row was
    //    actually changed. A false return after a successful
    //    find_by_id means a concurrent deletion race — surface as
    //    404 (the row really is gone now).
    bool updated = false;
    try {
        updated = user_repo::update_role(pool, target_id, new_role);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_users_role: update_role threw",
                  {{"user_id",  std::to_string(target_id)},
                   {"old_role", old_role},
                   {"new_role", new_role},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!updated) {
        send_error(res, 404, ErrorCode::NOT_FOUND,
                   "user not found",
                   {{"user_id", target_id}});
        return;
    }

    // 8) Audit log — strict insert. A lost audit row on a
    //    destructive admin action is a security trail gap and
    //    must surface as 500 (the operator can retry the request;
    //    the UPDATE is idempotent because step 7 already happened).
    try {
        AuditEntry e;
        e.admin_id    = std::stoi(admin_claims.user_id);
        e.action      = audit_log_repo::kActionUserRoleChange;
        e.target_type = "user";
        e.target_id   = std::to_string(target_id);
        e.payload     = {
            {"username", target_username},
            {"old_role", old_role},
            {"new_role", new_role},
        };
        e.ip          = extract_client_ip(req);
        audit_log_repo::record(pool, e);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_users_role: audit_log_repo::record threw",
                  {{"user_id",  std::to_string(target_id)},
                   {"old_role", old_role},
                   {"new_role", new_role},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("audit log write failed: ") + e.what());
        return;
    }

    LOG_INFO("admin_users_role: served",
             {{"user_id",     std::to_string(target_id)},
              {"username",    target_username},
              {"old_role",    old_role},
              {"new_role",    new_role},
              {"admin_id",    admin_claims.user_id},
              {"admin_uname", admin_claims.username}});

    // 9) Re-fetch and respond. The re-fetch gets the updated
    //    row and the latest created_at / last_login; if a future
    //    commit adds columns to users (e.g. role_changed_at) we
    //    want them visible here.
    try {
        const auto refreshed = user_repo::find_by_id(pool, target_id);
        if (refreshed.has_value()) {
            send_success(res, serialize_user_admin_row(*refreshed));
        } else {
            // Should be impossible — we just updated the row.
            // Surface as 500.
            throw std::runtime_error("admin_users_role: row "
                                     + std::to_string(target_id)
                                     + " missing after update");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("admin_users_role: find_by_id after update threw",
                  {{"user_id", std::to_string(target_id)},
                   {"type",    typeid(e).name()},
                   {"reason",  e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Returns HttpServer& so callers can chain. jwt_cfg is captured by
//  reference; the caller (main() / test fixture) owns it. rate_cfg
//  is captured by value (defends against a temporary RateLimitConfig
//  going out of scope — same defensive pattern as problem_routes.h /
//  admin_problem_routes.h).
//
//  Production usage (from main.cpp):
//
//    litecode::register_admin_user_routes(
//        server, pool, limiter, cfg.rate_limit, cfg.jwt);
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter (so bucket state is per-test, even though
//  the role-change path is the only one we test the bucket on).
//
//  ODR caveat (see header preamble): this header is not registered
//  from main.cpp for the same reason admin_problem_routes.h /
//  admin_bulk_import_routes.h / submission_routes.h / problem_routes.h
//  aren't. End-to-end coverage is owned by the test binary.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_admin_user_routes(
        HttpServer&            server,
        ConnectionPool&        pool,
        RateLimiter&           limiter,
        const RateLimitConfig& rate_cfg,
        const JwtConfig&       jwt_cfg) {

    // GET /api/v1/admin/users — list (SPEC §5.5, A22)
    server.get("/api/v1/admin/users",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                list_admin_users_handler(res, req, pool, limiter,
                                          rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_users_list: handler threw",
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

    // PUT /api/v1/admin/users/:id/role — change role
    // (SPEC §5.5, A24, A27). The regex matches a positive integer
    // id followed by "/role"; nested paths and trailing slashes
    // are rejected by the handler's extract_id_from_path + parse_id_param.
    server.put(R"(/api/v1/admin/users/([^/]+)/role)",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                change_user_role_handler(res, req, pool, limiter,
                                          rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_users_role: handler threw",
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

}  // namespace admin_user_routes

}  // namespace litecode
