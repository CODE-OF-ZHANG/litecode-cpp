// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — authentication routes (Phase 2 ★)
//
// SPEC §5.1 / §11 Phase 2 / §15.1 / A1, A2, A3, A22 acceptance:
//   - POST /api/v1/auth/register     — public, 5/min/IP, returns 201 + tokens
//                                       (refresh in HttpOnly cookie,
//                                       access in body)
//   - POST /api/v1/auth/login        — public, 10/min/IP, returns 200 + tokens
//                                       (refresh in HttpOnly cookie,
//                                       access in body) + per-username
//                                       failure audit (every 5th attempt
//                                       → audit_logs)
//   - POST /api/v1/auth/refresh      — public (valid refresh), no rate limit.
//                                       Reads refresh from HttpOnly cookie
//                                       (preferred, SPEC §6.3) OR from body
//                                       when COOKIE_ALLOW_BODY_FALLBACK=true
//                                       (dev/test back-compat). Always sets
//                                       a fresh Set-Cookie on rotation.
//   - POST /api/v1/auth/logout       — auth required; revokes the presented
//                                       refresh + clears the cookie
//                                       (Set-Cookie Max-Age=0)
//   - GET  /api/v1/auth/profile      — auth required
//
// Phase 5 ★ token storage (this file, on top of Phase 2 ★):
//   - register_handler / login_handler / refresh_handler now stamp a
//     Set-Cookie response header carrying the refresh token. The
//     cookie attributes come from CookieConfig (HttpOnly always;
//     Secure + SameSite=Strict configurable via env: COOKIE_SECURE /
//     COOKIE_SAME_SITE / COOKIE_PATH / COOKIE_NAME). The access
//     token stays in the JSON response body so the JS layer can
//     hold it in memory only.
//   - refresh_handler now reads the refresh from the Cookie header
//     first; if empty AND CookieConfig::allow_body_fallback is true
//     (default in dev for back-compat with the Phase 2 test suite),
//     it falls back to the body's `refresh_token` field. In prod
//     we recommend COOKIE_ALLOW_BODY_FALLBACK=false so the refresh
//     never crosses the wire in a readable field.
//   - logout_handler clears the cookie (Set-Cookie ...; Max-Age=0)
//     in addition to revoking the presented refresh. The browser
//     drops the cookie on receipt; the server blacklist guarantees
//     even an attacker who captured the value can no longer rotate.
//
// Phase 2 ★ refresh coverage (this file):
//   - refresh_handler — verify+rotate via rotate_token_pair (blacklist check
//     + revoke-old + sign-new lives in refresh_token.h). Looks up the
//     user by `sub` so the new access token carries the current
//     username/role (a user who got renamed or role-changed since the
//     refresh was issued will see the up-to-date values).
//   - Wire /api/v1/auth/refresh in register_auth_routes. No rate limit
//     per SPEC §5.1; the per-IP bucket is reserved for register/login
//     to stop credential-stuffing probes.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (server.h / error_handler.h / jwt_utils.h / refresh_token.h /
//     password_hash.h / user_repo.h). Tests link this header directly
//     and instantiate the route set with a real ConnectionPool + a
//     dummy rate limiter (or a stub pool that throws on demand).
//   - Phase 2 ★ ships **register** + **login** + **refresh** + **logout** +
//     **profile**. The last is the SPEC §5.1 "已登录" GET that returns
//     the current user's row from the `users` table (id / username /
//     role / email / avatar / created_at / last_login) — the "who am I"
//     companion to /auth/login used by the front-end's profile page +
//     nav-bar hydration on every page load.
//   - Every handler follows the same shape:
//       1) consume_rate_limit() — bucket + 429 envelope on deny
//       2) parse_json_body() — 400 envelope on bad JSON
//       3) field-level validation — 400 INVALID_INPUT envelope on bad shape
//       4) repo call — throws → 500 envelope
//       5) token issuance + 201/200 envelope
//     The throw-on-failure style (PasswordPolicyError, ApiException)
//     pairs with server.h's wrap()/wrap_resp() so callers never write
//     a try/catch themselves.
//   - The pool is passed by reference so the route handler can be
//     wired against a fresh ConnectionPool in tests without the
//     process-wide singleton. main() wires the production pool.
//   - login_handler additionally takes a `LoginFailureTracker&` for
//     the per-username failure counter that drives the audit row
//     every kAuditLogEvery attempts (SPEC §15.1). The tracker lives
//     outside the request thread and is shared by all logins, the
//     way `RateLimiter` is.
//
// Usage (production, from main.cpp):
//
//   litecode::HttpServer       server(cfg.server, cfg.cors);
//   litecode::ConnectionPool   pool(litecode::PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter      limiter;
//   litecode::LoginFailureTracker tracker;
//   litecode::RefreshTokenStore& store = *litecode::default_refresh_token_store();
//   litecode::register_auth_routes(server, pool, limiter, tracker, store,
//                                  cfg.jwt, cfg.rate_limit);
//   server.listen_blocking();
//
// Usage (test, from gtest):
//
//   litecode::HttpServer       server(dev_server(), dev_cors());
//   litecode::RateLimiter      limiter;
//   litecode::LoginFailureTracker tracker;
//   litecode::ConnectionPool   pool(test_db_config());
//   litecode::InMemoryRefreshTokenStore store;
//   litecode::register_auth_routes(server, pool, limiter, tracker, store,
//                                  dev_jwt(), lax_rate_limit());
//   auto h = start_server(&server);
//   auto r = h.client->Post("/api/v1/auth/login",
//       R"({"username":"alice","password":"hunter22"})", "application/json");

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../auth/jwt_utils.h"           // sign_access / sign_refresh
#include "../auth/password_hash.h"       // hash_password / PasswordPolicyError
#include "../auth/refresh_token.h"       // issue_token_pair / TokenPair
#include "../config.h"                   // AppConfig / JwtConfig / RateLimitConfig / CookieConfig
#include "../db/audit_log_repo.h"        // audit_log_repo::record_login_failure
#include "../db/connection_pool.h"       // ConnectionPool
#include "../db/user_repo.h"             // user_repo::create_user / find_by_username
#include "../logger.h"                   // LOG_INFO / LOG_WARN
#include "../middleware/auth_middleware.h" // require_authentication (Phase 2 ★ logout)
#include "../middleware/rate_limit.h"    // consume_rate_limit / RateLimiter / auth_register_quota
#include "../server.h"                   // HttpServer / send_error / send_created / ErrorCode
#include "../utils/cookie_utils.h"       // build_set_cookie_header / get_cookie_value (Phase 5 ★)
#include "../utils/uuid.h"               // (kept for parity with future logout handler)

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Request payload parsing
//
//  We parse strictly: missing required fields ⇒ 400. Surplus fields are
//  ignored (so the front-end can send extra telemetry without breaking).
//  Email is optional; when present it must pass validate_email().
//
//  Field validation rules (mirrors the JS front-end policy so users get
//  the same error message before AND after the request leaves the
//  browser — see SPEC §4.1):
//    username : 3..50, [a-zA-Z0-9_.-], not starting/ending with . or -
//    password : 8..72, must contain letters AND digits (delegated to
//               password_hash.h's validate_password_strength)
//    email    : optional; when present, must pass validate_email()
//    role     : optional; rejected if not "user" or "admin" (we don't
//               expose a way to self-register as admin via the API)
//
//  Validation messages are surfaced verbatim in the response body's
//  `message` field (SPEC §5.7). The error envelope's `details.field`
//  tells the front-end which input was at fault so it can highlight
//  the right form input.
// ────────────────────────────────────────────────────────────────────────────

struct RegisterRequest {
    std::string                username;
    std::string                password;
    std::optional<std::string> email;
};

namespace detail {

// parse_register_request — extract a RegisterRequest from a parsed JSON
// object. Returns std::nullopt and emits a 400 envelope when the input
// doesn't satisfy the schema. Throws on password-strength failure so
// the handler can map to a 400 with the password-specific message.
//
// `pool` is consulted to disambiguate "username taken" vs "email taken"
// on the 409 path. The handler does the actual pre-check; this helper
// only validates shape + password strength.
inline std::optional<RegisterRequest>
parse_register_request(const nlohmann::json& j,
                       httplib::Response&    res,
                       ConnectionPool&       pool) {
    (void)pool; // pool is consulted by the caller; reserved for future
                // "email already in use" check that wants a single repo
                // call. Keep the parameter so the signature stays
                // symmetric with the eventual `parse_login_request`.
    RegisterRequest out;

    // username (required, string)
    if (!j.contains("username") || !j["username"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string `username`",
                   {{"field", "username"}});
        return std::nullopt;
    }
    out.username = j["username"].get<std::string>();

    std::string username_err;
    if (!validate_username(out.username, &username_err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, username_err,
                   {{"field", "username"}});
        return std::nullopt;
    }

    // password (required, string, strength-validated)
    if (!j.contains("password") || !j["password"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string `password`",
                   {{"field", "password"}});
        return std::nullopt;
    }
    out.password = j["password"].get<std::string>();

    std::string password_err;
    if (!validate_password_strength(out.password, &password_err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, password_err,
                   {{"field", "password"}});
        return std::nullopt;
    }

    // email (optional, string)
    if (j.contains("email") && !j["email"].is_null()) {
        if (!j["email"].is_string()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "`email` must be a string when present",
                       {{"field", "email"}});
            return std::nullopt;
        }
        std::string email = j["email"].get<std::string>();
        std::string email_err;
        if (!validate_email(email, &email_err)) {
            send_error(res, 400, ErrorCode::INVALID_INPUT, email_err,
                       {{"field", "email"}});
            return std::nullopt;
        }
        out.email = std::move(email);
    }

    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  LoginRequest — smaller surface than RegisterRequest (no email, no
//  strength rule: an existing password can be weaker than the strength
//  policy because it was created under an earlier policy revision).
//  We DO still validate username shape — a malformed username is never
//  going to match a stored row, and surfacing 400 before DB hit closes
//  off a class of probing attacks.
// ────────────────────────────────────────────────────────────────────────────

struct LoginRequest {
    std::string username;
    std::string password;
};

// parse_login_request — extract a LoginRequest from JSON. Returns
// std::nullopt and writes a 400 envelope when the body is malformed.
//
// Differences from parse_register_request():
//   - email is NOT supported (server-side login is by username only;
//     the front-end's "forgot password" flow uses a separate path).
//   - password strength is NOT enforced (login must verify what was
//     stored, not what we'd re-allow today).
//   - Both fields are required and must be strings. Empty `password`
//     is rejected with a 400 because bcrypt_checkpass on an empty key
//     against an existing hash would still do ~250ms of work for no
//     useful purpose.
inline std::optional<LoginRequest>
parse_login_request(const nlohmann::json& j,
                    httplib::Response&    res) {
    LoginRequest out;

    // username (required, string, shape-validated)
    if (!j.contains("username") || !j["username"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string `username`",
                   {{"field", "username"}});
        return std::nullopt;
    }
    out.username = j["username"].get<std::string>();

    std::string username_err;
    if (!validate_username(out.username, &username_err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, username_err,
                   {{"field", "username"}});
        return std::nullopt;
    }

    // password (required, string, may be empty — empty is rejected as
    // 400 so we don't waste a bcrypt round on a blank key).
    if (!j.contains("password") || !j["password"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string `password`",
                   {{"field", "password"}});
        return std::nullopt;
    }
    out.password = j["password"].get<std::string>();
    if (out.password.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "`password` must not be empty",
                   {{"field", "password"}});
        return std::nullopt;
    }
    // login does NOT apply the kMaxPasswordLength cap from
    // password_hash.h — historic bcrypt hashes were 72-byte-truncated
    // anyway, so an over-long candidate still feeds through the same
    // truncation and answers the right way.

    return out;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  RefreshRequest — POST /api/v1/auth/refresh body shape
//
//  Only one field is required: the refresh token. The body is JSON
//  (matches the rest of /api/v1/auth/*) so the front-end doesn't need
//  a second parsing path for refresh; if it ever wants to use
//  HttpOnly cookies, the route can switch on `req.get_header_value(
//  "Cookie")` without changing the contract.
// ────────────────────────────────────────────────────────────────────────────

struct RefreshRequest {
    std::string refresh_token;
};

namespace detail {

// ────────────────────────────────────────────────────────────────────────────
//  Cookie plumbing (Phase 5 ★ — SPEC §6.3 / §15.1 / §15.3 token storage)
//
//  set_refresh_cookie(res, jwt_cfg, value)
//      Stamps Set-Cookie on `res` for the refresh token, using
//      jwt_cfg.refresh_ttl_seconds as the Max-Age and the configured
//      CookieConfig attributes from config().cookie.
//
//      Behavior when config().cookie.enabled is false: no-op.
//      Useful for a dev console that prefers localStorage flow.
//
//  clear_refresh_cookie(res)
//      Sets a Set-Cookie ...; Max-Age=0 with the same name + path so
//      the browser drops the cookie on receipt. Used by /auth/logout.
//
//  extract_refresh_token(req, cfg, body_refresh)
//      Returns the refresh token to verify, in this priority:
//          1) HttpOnly cookie (the canonical Phase 5 path)
//          2) If cfg.allow_body_fallback AND body is present:
//             body.refresh_token (legacy / curl)
//          3) Empty string
//
//  All three live in the SAME `namespace detail` block as
//  parse_refresh_request / parse_logout_request so the auth_routes
//  translation unit only has to include "routes/auth_routes.h" and
//  nothing else needs to know about the cookie plumbing.
// ────────────────────────────────────────────────────────────────────────────

// Append a Set-Cookie header (httplib concatenates successive set_header
// calls with the same key into multiple Set-Cookie headers, which is the
// behavior we want when /logout needs both clear-cookie + (none here)).
inline void set_refresh_cookie(httplib::Response& res,
                               const JwtConfig&   jwt_cfg,
                               std::string_view   refresh_token) {
    if (refresh_token.empty()) return;
    const auto& cfg = config().cookie;
    if (!cfg.enabled) return;
    res.set_header("Set-Cookie",
        build_set_cookie_header(cfg, refresh_token, jwt_cfg.refresh_ttl_seconds));
}

inline void clear_refresh_cookie(httplib::Response& res) {
    const auto& cfg = config().cookie;
    if (!cfg.enabled) return;
    res.set_header("Set-Cookie", build_clear_cookie_header(cfg));
}

// Look up the refresh in the Cookie header first; fall back to body
// only if the cookie is empty AND the dev-mode allow_body_fallback is on.
// Returns the presented refresh (empty if neither is present).
inline std::string extract_refresh_token(const httplib::Request& req,
                                         const CookieConfig&      cfg,
                                         const std::optional<
                                             RefreshRequest>&     body) {
    const std::string cookie_name = cfg.name;
    // req.get_header_value returns a const char* (httplib API).
    const std::string cookie_value =
        get_cookie_value(req.get_header_value("Cookie"), cookie_name);
    if (!cookie_value.empty()) return cookie_value;
    if (cfg.allow_body_fallback && body.has_value()) return body->refresh_token;
    return std::string();
}

// parse_refresh_request — extract a RefreshRequest from JSON. Returns
// std::nullopt and writes a 400 envelope when the body is malformed.
//
// The refresh token's content is opaque at this layer — we don't
// try to peek inside to distinguish "this is an access token" from
// "this is a refresh token" or "this is a JWT at all". The verify()
// call inside refresh_handler is authoritative; trying to be clever
// here would just duplicate the verifier's checks (and risk drifting
// from them).
inline std::optional<RefreshRequest>
parse_refresh_request(const nlohmann::json& j,
                      httplib::Response&    res) {
    RefreshRequest out;

    // refresh_token (required, string, non-empty)
    if (!j.contains("refresh_token") || !j["refresh_token"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string `refresh_token`",
                   {{"field", "refresh_token"}});
        return std::nullopt;
    }
    out.refresh_token = j["refresh_token"].get<std::string>();
    if (out.refresh_token.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "`refresh_token` must not be empty",
                   {{"field", "refresh_token"}});
        return std::nullopt;
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  LogoutRequest — POST /api/v1/auth/logout body shape
//
//  Same shape as RefreshRequest: only the refresh token is required.
//  We keep the request-body shape symmetric with /auth/refresh so the
//  front-end can reuse its existing JSON plumbing; the difference is
//  in what we DO with the token (revoke-and-forget vs. rotate).
// ────────────────────────────────────────────────────────────────────────────

struct LogoutRequest {
    std::string refresh_token;
};

// parse_logout_request — extract a LogoutRequest from JSON. Returns
// std::nullopt and writes a 400 envelope when the body is malformed.
//
// Rules mirror parse_refresh_request exactly:
//   - `refresh_token` is required, must be a string, must be non-empty.
//   - Surplus fields are ignored (front-end telemetry, device_id, etc.).
//
// The refresh token's content is opaque at this layer — same reason
// as parse_refresh_request: we don't try to peek inside to distinguish
// "this is an access token" from "this is a refresh token" or "this
// is a JWT at all". The revoke_refresh_token() call inside
// logout_handler is authoritative; trying to be clever here would
// just duplicate the verifier's checks.
//
// Note: this helper lives in the SAME `namespace detail` block that
// hosts parse_refresh_request (see line 294) — there's no new
// namespace open here. The single closing brace on line 376 closes
// the entire block opened back at line 294.
inline std::optional<LogoutRequest>
parse_logout_request(const nlohmann::json& j,
                     httplib::Response&    res) {
    LogoutRequest out;

    if (!j.contains("refresh_token") || !j["refresh_token"].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing or non-string `refresh_token`",
                   {{"field", "refresh_token"}});
        return std::nullopt;
    }
    out.refresh_token = j["refresh_token"].get<std::string>();
    if (out.refresh_token.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "`refresh_token` must not be empty",
                   {{"field", "refresh_token"}});
        return std::nullopt;
    }
    return out;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/auth/register
//
//  Wire flow:
//    1. consume_rate_limit()       — 5/min/IP token bucket; 429 on deny
//    2. parse_json_body()          — 400 INVALID_INPUT on bad JSON
//    3. detail::parse_register_request() — 400 on shape / strength failure
//    4. user_repo::create_user()   — 0 ⇒ 409 CONFLICT, throws ⇒ 500
//    5. issue_token_pair()         — 201 + access + refresh + user payload
//
//  Returns 201 with the new user + tokens. The `user` block deliberately
//  omits `password_hash` — never echo that back, even a one-way value.
// ────────────────────────────────────────────────────────────────────────────

inline void register_handler(httplib::Response&     res,
                             const httplib::Request& req,
                             ConnectionPool&       pool,
                             const JwtConfig&      jwt_cfg) {
    auto j = parse_json_body(req, res);
    if (!j) return;                                  // 400 already on the wire

    auto parsed = detail::parse_register_request(*j, res, pool);
    if (!parsed) return;                             // 400 already on the wire

    // 4) Insert. The UNIQUE constraint on username (and email when
    //    present) is the authoritative gate; create_user() returns 0
    //    on collision so we don't need a separate pre-check + race.
    UserRow row;
    row.username      = parsed->username;
    row.password_hash = hash_password(parsed->password); // throws → 500
    row.role          = "user";                     // SPEC §4.1 — never admin via API
    row.email         = parsed->email;
    row.avatar        = std::nullopt;              // SPEC §6 — /assets/img/default-avatar.svg
                                                   //           served by the front-end,
                                                   //           not stored in the column.

    int new_id = 0;
    try {
        new_id = user_repo::create_user(pool, row);
    } catch (const std::exception& e) {
        // Catch std::exception (not just UserRepoError) so a
        // mysqlx::Error escaping from create_user doesn't fall through
        // to a generic 500 — log it with the type name so we can
        // tighten the catch list once the surface is pinned.
        LOG_ERROR("register: create_user threw",
                  {{"username", row.username},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("failed to create user: ") + e.what());
        return;
    }
    if (new_id == 0) {
        // Collision. Disambiguate via a single SELECT so the front-end
        // can highlight the right form field (username vs email).
        // The cost is one extra round-trip — fine for a 5/min endpoint.
        const bool username_taken = user_repo::username_exists(
            pool, row.username);
        const bool email_taken = row.email
            ? user_repo::email_exists(pool, *row.email)
            : false;
        if (username_taken) {
            send_error(res, 409, ErrorCode::CONFLICT,
                       "username already taken",
                       {{"field", "username"}});
        } else if (email_taken) {
            send_error(res, 409, ErrorCode::CONFLICT,
                       "email already in use",
                       {{"field", "email"}});
        } else {
            // Both checks came back free — race with a concurrent
            // INSERT that won the race but was already gone by the
            // time we asked. Just report a generic 409.
            send_error(res, 409, ErrorCode::CONFLICT,
                       "user already exists");
        }
        return;
    }

    // 5) Mint tokens. role is hard-coded "user" — admin accounts go
    //    through scripts/create_admin.sql (SPEC §4.1).
    TokenPair tokens;
    try {
        tokens = issue_token_pair(
            jwt_cfg.secret,
            jwt_cfg.issuer,
            std::to_string(new_id),
            row.username,
            row.role,
            jwt_cfg.access_ttl_seconds,
            jwt_cfg.refresh_ttl_seconds);
    } catch (const std::exception& e) {
        // We just inserted the row but couldn't mint a session.
        // That's bad but recoverable — the user can log in. Don't
        // 500 with "registration failed" because the account IS
        // created. Log loudly so operators know to investigate.
        LOG_ERROR("register: token issuance failed AFTER user insert",
                  {{"user_id",  std::to_string(new_id)},
                   {"username", row.username},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("user created but session could not be issued: ") + e.what());
        return;
    }

    LOG_INFO("auth: registered",
             {{"user_id",  std::to_string(new_id)},
              {"username", row.username}});

    // Phase 5 ★: deliver the refresh token via HttpOnly cookie so
    // JavaScript can never read it (SPEC §6.3 + §15.3). The access
    // token is intentionally NOT in a cookie — the JS layer holds it
    // in memory only and a reload triggers /auth/refresh (cookie path)
    // to mint a fresh access token.
    detail::set_refresh_cookie(res, jwt_cfg, tokens.refresh_token);

    send_created(res, {
        {"user", {
            {"id",         new_id},
            {"username",   row.username},
            {"role",       row.role},
            {"email",      row.email ? nlohmann::json(*row.email)
                                     : nlohmann::json(nullptr)},
        }},
        // access_token stays in the body — the JS layer stores it in
        // memory only (refresh lost = re-login, not silent reissue).
        {"access_token",  tokens.access_token},
        // refresh_token stays in the body too, for two reasons:
        //   1) COOKIE_ALLOW_BODY_FALLBACK (dev/test back-compat) lets
        //      callers without cookie support still rotate.
        //   2) The body value is the same one we just stamped into the
        //      cookie — convenient for clients that want to verify the
        //      cookie path by diffing.
        {"refresh_token", tokens.refresh_token},
        {"token_type",    "Bearer"},
        {"expires_in",    tokens.access_expires_in_seconds(
                              std::chrono::system_clock::now())},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  Login-failure tracker
//
//  In-memory, per-username failure counter used by /api/v1/auth/login
//  to satisfy SPEC §15.1 ("失败登录 5 次 → 写 audit_logs") and the
//  Phase 6 ☆ v1.2.46 lockout state machine ("失败登录锁定 — 连续 N
//  次失败 15 分钟内禁止该用户名登录"):
//
//    - record_failure() bumps the counter for `username` and returns
//      the new count. The caller then writes to audit_logs when the
//      count crosses the kAuditLogEvery threshold (5, 10, 15, ...).
//    - When `LoginLockoutConfig::enabled` is true and the counter
//      crosses `LoginLockoutConfig::threshold` within the rolling
//      `window_seconds`, the username is locked for
//      `lockout_duration_seconds`. record_failure returns
//      `locked=true` on the attempt that crosses the threshold so the
//      caller can write a `auth.login_locked` audit row.
//    - is_locked() / remaining_lockout_seconds() are the gate that
//      login_handler consults BEFORE running bcrypt. While locked,
//      every login attempt is short-circuited with the anti-
//      enumeration envelope and a Retry-After header.
//    - reset() is called after a successful login so a legitimate
//      user who fat-fingered their password twice isn't penalized
//      forever. It also clears any active lockout state.
//    - `max_entries` caps the map at 100k distinct usernames — defense
//      against an attacker poking with random usernames to grow RSS.
//      When the cap is hit, we prefer to evict UNLOCKED entries with
//      the largest count (the most-evidence accounts that aren't
//      currently being throttled). Locked entries are evicted last —
//      dropping a lockout would let the attacker back in.
//
//  Thread safety: a single mutex guards the map. record_failure() and
//  reset() are O(1) amortized.
//
//  Clock injection: tests can pass a custom `Clock` callable that
//  returns a steady_clock::time_point. Production callers leave the
//  default (std::chrono::steady_clock::now) in place.
//
//  Tests reset the process-wide tracker between cases so state from
//  a prior test doesn't bleed in.
// ────────────────────────────────────────────────────────────────────────────

class LoginFailureTracker {
public:
    static constexpr int kAuditLogEvery = 5;   // threshold for audit_logs

    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit LoginFailureTracker(
            LoginLockoutConfig lockout_cfg = LoginLockoutConfig{},
            std::size_t        max_entries = 100000,
            Clock              clock       = nullptr)
        : lockout_cfg_(lockout_cfg),
          max_entries_(max_entries),
          clock_(clock ? std::move(clock)
                       : Clock([] { return std::chrono::steady_clock::now(); })) {
        // Clamp to safe defaults so a misconfigured config (e.g. an
        // operator who sets threshold=0) doesn't disable the feature
        // silently — treat < 1 as "feature off". The same clamps live
        // in load_config() so this is defense in depth.
        if (lockout_cfg_.threshold                < 1) lockout_cfg_.threshold                = 1;
        if (lockout_cfg_.window_seconds           < 1) lockout_cfg_.window_seconds           = 1;
        if (lockout_cfg_.lockout_duration_seconds < 1) lockout_cfg_.lockout_duration_seconds = 1;
    }

    // Backward-compat overload — pre-Phase-6 callers passed only
    // `max_entries` (the cap on tracked usernames). Delegates to the
    // primary constructor with a default `LoginLockoutConfig{}` so
    // the lockout feature defaults to ON but the cap is honored.
    explicit LoginFailureTracker(std::size_t max_entries)
        : LoginFailureTracker(LoginLockoutConfig{}, max_entries, nullptr) {}

    struct Outcome {
        int  count              = 0;  // new failure count for username
        bool should_audit       = false; // true iff this attempt crossed a multiple of kAuditLogEvery
        bool locked             = false; // true iff THIS attempt triggered a fresh lockout
        bool was_already_locked = false; // true iff the account was already locked when this attempt came in
        int  locked_for_seconds = 0;  // when locked/was_already_locked: how long the lockout will last from now
        int  remaining_seconds  = 0;  // when locked/was_already_locked: how long until the lockout lifts
    };

    // record_failure — bump the counter for `username` and possibly
    // trigger a lockout. Returns the outcome so the caller can fire
    // the right audit row (login_failure vs login_locked).
    //
    // State machine (per username):
    //
    //   +-------------------+  record_failure   +---------------------+
    //   | clean (no entry)  | ----------------> | counting (count < N)|
    //   +-------------------+                   +---------------------+
    //                                                | count == N
    //                                                v
    //                                          +-------------+
    //                                          |  LOCKED     |
    //                                          |  for D sec  |
    //                                          +-------------+
    //                                                |
    //                       window expires / reset() |
    //                                                v
    //                                          +-------------+
    //                                          | clean again |
    //                                          +-------------+
    //
    // `window_seconds` is the rolling "first failure of the window"
    // rule: if a new failure arrives `window_seconds` after the FIRST
    // failure of the current window, we reset the counter to 1 (a
    // fresh attempt at a fresh user). While inside the window, every
    // failure increments.
    Outcome record_failure(std::string_view username) {
        Outcome out;
        if (username.empty()) {
            return out;
        }
        const auto now = clock_();

        std::lock_guard<std::mutex> g(mu_);

        // Opportunistic cap enforcement: prefer to drop an UNLOCKED
        // entry with the largest count. If every entry is locked
        // (unlikely — that would mean >max_entries_ accounts under
        // simultaneous lockout, which would itself mean we're being
        // hammered), fall back to dropping the highest-count locked
        // entry so the cap is enforced. Locked entries being evicted
        // is recoverable: the next failed login will re-arm the
        // lockout, and the attacker hasn't gained anything (the same
        // threshold still applies).
        if (max_entries_ > 0 && failures_.size() >= max_entries_) {
            auto victim = failures_.end();
            for (auto it = failures_.begin(); it != failures_.end(); ++it) {
                if (victim == failures_.end()) {
                    victim = it;
                    continue;
                }
                const bool victim_locked = (victim->second.locked_until > now);
                const bool it_locked     = (it->second.locked_until > now);
                if (!victim_locked && it_locked) {
                    // Keep the unlocked victim; skip this locked entry.
                    continue;
                }
                if (victim_locked && !it_locked) {
                    // Prefer the unlocked entry.
                    victim = it;
                    continue;
                }
                if (it->second.count > victim->second.count) {
                    victim = it;
                }
            }
            if (victim != failures_.end()) failures_.erase(victim);
        }

        auto& entry = failures_[std::string(username)];

        // If the entry is currently locked and we're INSIDE the
        // lockout window, do NOT count this attempt — we don't want a
        // brute-force probe to keep extending its own ban. Return the
        // existing lockout so the caller knows to short-circuit.
        if (lockout_cfg_.enabled && entry.locked_until > now) {
            out.count              = entry.count;
            out.should_audit       = false; // no NEW failure to audit
            out.was_already_locked = true;
            out.locked_for_seconds = lockout_cfg_.lockout_duration_seconds;
            out.remaining_seconds  = seconds_until(entry.locked_until, now);
            return out;
        }

        // Sliding-window reset: if the first failure of the current
        // window is older than window_seconds, start a fresh window
        // with count = 1. We use `>=` so the boundary case
        // (first_failure_at exactly window_seconds ago) resets too —
        // the "15 分钟内" rule in SPEC §15.1 reads naturally as a
        // half-open interval [now - window, now).
        //
        // Note: this does NOT wipe an active lockout — the check above
        // would have short-circuited if we were still inside the
        // lockout window. A "natural expiry" (lockout lifted, but
        // sliding window hasn't expired yet) keeps the count intact,
        // so an attacker who times their probes around lockout expiries
        // can't trivially reset their progress. Only a successful
        // login (tracker.reset) or a fully-expired sliding window
        // clears the count.
        const bool entry_is_fresh =
            entry.first_failure_at == std::chrono::steady_clock::time_point{};
        const auto window_dur = std::chrono::seconds(lockout_cfg_.window_seconds);
        if (entry_is_fresh || now - entry.first_failure_at >= window_dur) {
            entry.count             = 1;
            entry.first_failure_at  = now;
            entry.locked_until      = {};
        } else {
            ++entry.count;
        }
        out.count        = entry.count;
        out.should_audit = (entry.count > 0)
                        && (entry.count % kAuditLogEvery == 0);

        // Lockout trigger — only when enabled AND the counter just
        // hit the threshold (don't re-trigger on later failures in
        // the same window).
        if (lockout_cfg_.enabled
            && entry.count >= lockout_cfg_.threshold
            && entry.locked_until <= now) {
            entry.locked_until      = now
                + std::chrono::seconds(lockout_cfg_.lockout_duration_seconds);
            out.locked             = true;
            out.locked_for_seconds = lockout_cfg_.lockout_duration_seconds;
            out.remaining_seconds  = lockout_cfg_.lockout_duration_seconds;
        }
        return out;
    }

    void reset(std::string_view username) {
        if (username.empty()) return;
        std::lock_guard<std::mutex> g(mu_);
        failures_.erase(std::string(username));
    }

    int count(std::string_view username) const {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = failures_.find(std::string(username));
        return (it == failures_.end()) ? 0 : it->second.count;
    }

    // is_locked — true iff the username is currently within an active
    // lockout window. Returns the remaining seconds in `out_remaining`
    // (0 when not locked).
    bool is_locked(std::string_view username, int* out_remaining = nullptr) const {
        if (out_remaining) *out_remaining = 0;
        if (username.empty()) return false;
        std::lock_guard<std::mutex> g(mu_);
        const auto it = failures_.find(std::string(username));
        if (it == failures_.end()) return false;
        const auto now = clock_();
        if (it->second.locked_until > now) {
            if (out_remaining) {
                *out_remaining = seconds_until(it->second.locked_until, now);
            }
            return true;
        }
        return false;
    }

    // remaining_lockout_seconds — convenience wrapper for callers that
    // only want the integer. Returns 0 when not locked.
    int remaining_lockout_seconds(std::string_view username) const {
        int r = 0;
        is_locked(username, &r);
        return r;
    }

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        failures_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return failures_.size();
    }

    std::size_t max_entries() const { return max_entries_; }

    const LoginLockoutConfig& lockout_config() const { return lockout_cfg_; }

    // Visible-for-test: install a custom clock mid-flight. The clock
    // is captured by reference inside the lambda set up in the
    // constructor; calling this replaces it.
    void set_clock_for_testing(Clock clock) {
        std::lock_guard<std::mutex> g(mu_);
        clock_ = clock ? std::move(clock)
                       : Clock([] { return std::chrono::steady_clock::now(); });
    }

private:
    struct Entry {
        int count = 0;
        // Time of the FIRST failure in the current rolling window.
        // Defaults to the epoch so a freshly-constructed entry is
        // treated as "outside any window" (will reset on first
        // failure).
        std::chrono::steady_clock::time_point first_failure_at{};
        // Time at which the active lockout lifts. Default-constructed
        // time_point compares less-than any clock_() return value,
        // so the lockout check is naturally false on a fresh entry.
        std::chrono::steady_clock::time_point locked_until{};
    };

    // seconds_until(a, b) — ceil((a - b) / 1s), clamped at 0. We use
    // ceiling so the Retry-After header never under-reports (a value
    // of 0 would let the client retry immediately, which would race
    // against the lockout lift on the server).
    static int seconds_until(std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b) {
        if (a <= b) return 0;
        const auto diff = a - b;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(diff);
        // Round up if diff has any sub-second component past the
        // truncated `secs` (e.g. 2.5s → 3). Cast back to seconds
        // (a duration comparison works because both sides share
        // representation units after the cast).
        if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(secs)
            < diff) {
            return static_cast<int>(secs.count()) + 1;
        }
        return static_cast<int>(secs.count());
    }

    LoginLockoutConfig                       lockout_cfg_;
    mutable std::mutex                       mu_;
    std::unordered_map<std::string, Entry>   failures_;
    std::size_t                              max_entries_;
    Clock                                    clock_;
};

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/auth/login   — Phase 2 ★  (SPEC §5.1, §15.1, §16.4, A2)
//
//  Wire flow:
//    1. consume_rate_limit()           — 10/min/IP; 429 envelope on deny
//    2. parse_json_body()              — 400 INVALID_INPUT on bad JSON
//    3. detail::parse_login_request()  — 400 on missing fields / bad
//                                        username shape
//    3.5. tracker.is_locked()          — Phase 6 ☆ v1.2.46: 423 Locked
//                                        envelope when the username is
//                                        inside an active lockout window
//                                        (Retry-After: remaining seconds)
//    4. user_repo::find_by_username()  — 401 if no such user
//    5. verify_password()              — 401 if mismatch
//    6. audit_log (every 5th failure)  — fire-and-forget row insert
//    6.5. tracker.record_failure()     — Phase 6 ☆ v1.2.46: bumps the
//                                        sliding-window counter; may
//                                        trip the lockout state machine,
//                                        in which case a SEPARATE
//                                        auth.login_locked row is
//                                        written
//    7. tracker.reset()                — successful login clears the
//                                        counter AND any active lockout
//                                        for this username (Phase 6 ☆
//                                        v1.2.46)
//    8. user_repo::update_last_login() — best-effort last_login stamp
//    9. issue_token_pair()             — access (2h) + refresh (7d)
//   10. send_success()                 — 200 + {user, tokens, ...}
//
//  Anti-enumeration (SPEC §15.1): the 401 message is identical for
//  "no such user" and "wrong password" — "invalid username or
//  password". The route handler returns 401 in either case via a
//  single throw so the wire body is byte-for-byte the same. The
//  423 Locked response for an actively-locked username uses the same
//  envelope message ("invalid username or password") so a probe
//  can't distinguish "this account exists and is locked" from
//  "this account exists and you got the password wrong" from
//  "no such account" — only the Retry-After header differs, and a
//  polite client is the only one who acts on that.
//
//  Failure-audit (SPEC §15.1): each failed login bumps an in-memory
//  counter keyed by username. At every kAuditLogEvery (5) crossings
//  we INSERT a row into audit_logs with action="auth.login_failure".
//  Counts are per-username so a brute-force probe targeting one
//  account is captured cleanly without polluting other accounts' logs.
//
//  Lockout state machine (Phase 6 ☆ v1.2.46 — SPEC §15.1):
//    - threshold (default 5):   consecutive failures inside the
//                               sliding window that trip the lockout.
//    - window_seconds (900):    rolling "first-failure" anchor. A
//                               failure that arrives window_seconds
//                               after the anchor resets the counter
//                               to 1.
//    - lockout_duration (900):  how long the lockout lasts once
//                               triggered. While locked, every login
//                               attempt returns 423 Locked without
//                               even consulting the DB.
//    - Successful login:        clears the counter AND any active
//                               lockout so a one-off typo doesn't
//                               self-lockout.
//    - Config knob:             LOGIN_LOCKOUT_ENABLED=false disables
//                               the feature entirely (the counter
//                               still drives the audit_logs trigger;
//                               only the lockout trip is suppressed).
// ────────────────────────────────────────────────────────────────────────────

inline void login_handler(httplib::Response&                 res,
                          const httplib::Request&            req,
                          ConnectionPool&                    pool,
                          const JwtConfig&                   jwt_cfg,
                          LoginFailureTracker&               tracker,
                          std::string_view                   client_ip) {
    auto j = parse_json_body(req, res);
    if (!j) return;                                  // 400 already on the wire

    auto parsed = detail::parse_login_request(*j, res);
    if (!parsed) return;                             // 400 already on the wire

    // Phase 6 ☆ v1.2.46 — lockout gate. If this username is currently
    // inside an active lockout window, short-circuit BEFORE the bcrypt
    // round-trip. We use the same anti-enumeration envelope a normal
    // bad-password attempt would ("invalid username or password") so
    // the wire never reveals whether the account is real. The
    // Retry-After header carries the remaining lockout time so a
    // polite client can back off, but the body deliberately does NOT
    // — an attacker who probed a real username would otherwise see
    // "this account exists" through the difference in headers.
    //
    // We check the lockout BEFORE the per-IP rate limit budget is
    // consumed? No — actually we check AFTER consume_rate_limit so a
    // flood of probes against a locked account still trips the IP
    // bucket. The opposite (lockout short-circuit before rate-limit)
    // would let an attacker bypass the IP bucket by always probing a
    // locked account. consume_rate_limit is called in the route
    // registration lambda, so by the time we reach this line the IP
    // bucket has already been debited; here we only gate on per-
    // username lockout state.
    if (tracker.lockout_config().enabled) {
        int remaining = 0;
        if (tracker.is_locked(parsed->username, &remaining)) {
            // No record_failure here — we don't want a probe to
            // extend the ban indefinitely. The state machine itself
            // short-circuits record_failure during a lockout, but
            // skipping the call entirely saves a map lookup and
            // makes the intent obvious.
            LOG_WARN("auth: login rejected (account locked)",
                     {{"username",          parsed->username},
                      {"remaining_seconds", std::to_string(remaining)},
                      {"ip",                std::string(client_ip)}});
            // Retry-After header per RFC 7231 §7.1.3 — integer seconds
            // (the simplest form; RFC also allows HTTP-date, but no
            // client we care about uses it). Always at least 1 so a
            // 0-second remainder still produces a "wait" signal.
            res.set_header("Retry-After",
                           std::to_string(remaining > 0 ? remaining : 1));
            send_error(res, 423, ErrorCode::FORBIDDEN,
                       "invalid username or password",
                       {{"retry_after_seconds", remaining}});
            return;
        }
    }

    // Helper for the unified 401 path. Both "no such user" and "wrong
    // password" end up here — same envelope, same message, no leak.
    auto deny_login = [&](const std::string& username_for_audit) {
        // Bump the per-username counter; audit when it crosses
        // a multiple of kAuditLogEvery. username_for_audit may be
        // empty when the request had a missing username but we still
        // got past validation (it won't — parse_login_request rejects
        // missing username). Kept as a parameter so the audit row
        // stays attributable when the user really did supply a username
        // but the row doesn't exist in the DB.
        const auto outcome = tracker.record_failure(username_for_audit);
        if (outcome.should_audit) {
            try {
                audit_log_repo::record_login_failure(
                    pool, username_for_audit, client_ip, outcome.count);
            } catch (...) {
                // record_login_failure is already best-effort; this
                // catch is paranoia against a future change that
                // flips it to throwing.
            }
        }
        // Phase 6 ☆ v1.2.46 — fresh lockout triggered? Write a
        // dedicated auth.login_locked audit row so operators can
        // filter brute-force signals cleanly out of the general
        // login_failure stream.
        if (outcome.locked) {
            try {
                audit_log_repo::record_login_lockout(
                    pool,
                    username_for_audit,
                    client_ip,
                    outcome.count,
                    outcome.locked_for_seconds,
                    tracker.lockout_config().threshold);
            } catch (...) {
                // record_login_lockout is best-effort; the throwaway
                // catch matches record_login_failure above.
            }
            LOG_WARN("auth: login locked (threshold reached)",
                     {{"username",            username_for_audit},
                      {"consecutive_failures", std::to_string(outcome.count)},
                      {"locked_for_seconds",   std::to_string(outcome.locked_for_seconds)},
                      {"ip",                  std::string(client_ip)}});
        }
        LOG_WARN("auth: login failed",
                 {{"username",                  username_for_audit},
                  {"consecutive_failures",      std::to_string(outcome.count)},
                  {"should_audit",              outcome.should_audit ? "true" : "false"},
                  {"locked",                    outcome.locked ? "true" : "false"},
                  {"was_already_locked",        outcome.was_already_locked ? "true" : "false"}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid username or password");
    };

    // 4) Find the user. The repo returns nullopt for both "no row"
    //    and "DB error" — but the latter throws, which we catch.
    std::optional<UserRow> row;
    try {
        row = user_repo::find_by_username(pool, parsed->username);
    } catch (const std::exception& e) {
        LOG_ERROR("login: find_by_username threw",
                  {{"username", parsed->username},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!row) {
        // Anti-enumeration: count the failure for tracking but
        // produce the same 401 envelope a wrong password would.
        deny_login(parsed->username);
        return;
    }

    // 5) Verify the password. verify_password is noexcept + returns
    //    bool. A malformed / NULL hash in the DB still answers "false"
    //    (not 500) so a stale row can't crash the request thread.
    if (!verify_password(parsed->password, row->password_hash)) {
        deny_login(row->username);
        return;
    }

    // 7) Successful login — clear the counter so a one-off typo
    //    doesn't chain into an audit row on the user's next try.
    tracker.reset(row->username);

    // 8) Best-effort last_login stamp. The repo swallows DB errors
    //    and logs WARN — the user is authenticated regardless.
    user_repo::update_last_login(pool, row->id, client_ip);

    // 9) Mint tokens. role is the row's role — accounts whose role
    //    was changed since token issuance will get a fresh role on
    //    their next login (Phase 6 adds an admin-side invalidation
    //    hook that revokes outstanding tokens at role-change time).
    TokenPair tokens;
    try {
        tokens = issue_token_pair(
            jwt_cfg.secret,
            jwt_cfg.issuer,
            std::to_string(row->id),
            row->username,
            row->role,
            jwt_cfg.access_ttl_seconds,
            jwt_cfg.refresh_ttl_seconds);
    } catch (const std::exception& e) {
        // Password verified but token issuance blew up. Same shape
        // as the analogous register path: don't pretend we succeeded
        // because we didn't. Log loudly so operators investigate.
        LOG_ERROR("login: token issuance failed",
                  {{"user_id",  std::to_string(row->id)},
                   {"username", row->username},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("session could not be issued: ") + e.what());
        return;
    }

    LOG_INFO("auth: login",
             {{"user_id",  std::to_string(row->id)},
              {"username", row->username},
              {"role",     row->role}});

    // Phase 5 ★: deliver the refresh token via HttpOnly cookie so
    // JavaScript can never read it (SPEC §6.3 + §15.3). The access
    // token stays in the body for in-memory storage by the SPA.
    detail::set_refresh_cookie(res, jwt_cfg, tokens.refresh_token);

    send_success(res, {
        {"user", {
            {"id",       row->id},
            {"username", row->username},
            {"role",     row->role},
            {"email",    row->email ? nlohmann::json(*row->email)
                                    : nlohmann::json(nullptr)},
        }},
        {"access_token",  tokens.access_token},
        {"refresh_token", tokens.refresh_token},
        {"token_type",    "Bearer"},
        {"expires_in",    tokens.access_expires_in_seconds(
                              std::chrono::system_clock::now())},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/auth/refresh   — Phase 2 ★  (SPEC §5.1, §15.1, A2)
//
//  Wire flow:
//    1. parse_json_body()              — 400 INVALID_INPUT on bad JSON
//    2. detail::parse_refresh_request()— 400 on missing/empty/typed field
//    3. verify() (jwt_utils)           — populates Claims with sub / jti / exp
//    4. user_repo::find_by_id()        — 500 on DB error, 401 if user gone
//    5. rotate_token_pair()            — verify + blacklist-check + revoke-old
//                                        + sign-new (all in one helper)
//    6. send_success()                 — 200 + new pair + user block
//
//  Why we look up the user before rotating: rotate_token_pair needs
//  the current username + role (refresh tokens deliberately don't
//  carry them — least privilege; SPEC §5.1). Looking up by `sub`
//  also lets us catch the "user was deleted" case explicitly: a
//  refresh token is still valid as a JWT, but the user it refers to
//  is gone, so the right answer is 401 (not 500). We also use this
//  hook to surface the current role / username in the new access
//  token, so a role change since the refresh was issued is reflected
//  on next refresh.
//
//  Anti-enumeration (SPEC §15.1): the 401 envelope is identical for
//  every failure mode — bad signature, expired, revoked, kind wrong,
//  user deleted. We log the actual reason at WARN so operators can
//  investigate theft / clock-drift patterns; the wire only carries
//  "invalid or expired refresh token".
//
//  Why NO rate limit (SPEC §5.1): refresh is meant to be hit on a
//  schedule by the client (typically once per ~2h as the access
//  token expires). Capping it would make a tab that's been idle for
//  a few hours unable to come back. The blacklist + short access
//  TTL provide the actual security; a stolen refresh is dead as
//  soon as the legitimate user rotates it.
// ────────────────────────────────────────────────────────────────────────────

inline void refresh_handler(httplib::Response&                 res,
                             const httplib::Request&            req,
                             ConnectionPool&                    pool,
                             RefreshTokenStore&                 store,
                             const JwtConfig&                   jwt_cfg,
                             std::string_view                   client_ip) {
    // Phase 5 ★ cookie-aware refresh extraction.
    //
    // We always parse the JSON body when present (so a missing/empty
    // body can still 400 on a body-only caller), but the authoritative
    // source of the presented refresh is the HttpOnly cookie. The body
    // is consulted ONLY if (a) the cookie is absent AND (b)
    // CookieConfig::allow_body_fallback is true.
    //
    // The parsing deliberately stays tolerant: a request that arrives
    // with ONLY a cookie (no body) is the canonical Phase 5 path and
    // must NOT 400. parse_refresh_request requires a non-empty
    // refresh_token field, so we don't call it when the body is empty
    // or has no refresh_token.
    auto j = parse_json_body(req, res);
    // j may be std::nullopt if the body is missing or invalid JSON.
    // For the cookie path that's fine — we ignore the body entirely.

    std::optional<RefreshRequest> body_parsed;
    if (j) {
        // Only attempt to parse if the body actually contains a
        // refresh_token field — keeps an empty body or unrelated body
        // (e.g. {"foo": 1}) from triggering a 400 envelope on the
        // cookie path.
        if (j->contains("refresh_token")) {
            body_parsed = detail::parse_refresh_request(*j, res);
            if (!body_parsed) return;  // 400 already on the wire
        }
    }

    const CookieConfig& cookie_cfg = config().cookie;
    const std::string presented_refresh = detail::extract_refresh_token(
        req, cookie_cfg, body_parsed);

    if (presented_refresh.empty()) {
        // Neither cookie nor body (or body_fallback disabled). The
        // caller's intent is "rotate my refresh" — we have nothing to
        // verify. 401 with the unified anti-enumeration envelope.
        LOG_WARN("refresh: no refresh token (no cookie, no body)",
                 {{"ip", std::string(client_ip)},
                  {"body_fallback", cookie_cfg.allow_body_fallback ? "yes" : "no"}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    }

    // 3) Verify the refresh token to recover the sub + jti + exp.
    //    We do this BEFORE the user lookup so a malformed / wrong-kind
    //    token fails fast (no DB hit) and gets the same 401 envelope
    //    a revoked token would (anti-enumeration).
    Claims claims;
    try {
        claims = verify(presented_refresh,
                         jwt_cfg.secret, jwt_cfg.issuer,
                         TokenKind::Refresh);
    } catch (const JwtError& e) {
        LOG_WARN("refresh: token verification failed",
                 {{"ip",     std::string(client_ip)},
                  {"type",   typeid(e).name()},
                  {"reason", e.what()}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    }

    // 3.5) The sub claim should be a decimal string of an int — same
    //      shape we emit in sign_access / sign_refresh. Anything else
    //      is a tampered token; treat as invalid.
    int user_id = 0;
    try {
        user_id = std::stoi(claims.user_id);
    } catch (const std::exception&) {
        LOG_WARN("refresh: malformed sub claim",
                 {{"ip",  std::string(client_ip)},
                  {"sub", claims.user_id}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    }
    if (user_id <= 0) {
        LOG_WARN("refresh: non-positive sub claim",
                 {{"ip",  std::string(client_ip)},
                  {"sub", claims.user_id}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    }

    // 4) Look up the user. nullopt ⇒ the user was deleted (or never
    //    existed — which is the same as a tampered sub claim, just
    //    observed one layer deeper). Both surface as 401.
    std::optional<UserRow> row;
    try {
        row = user_repo::find_by_id(pool, user_id);
    } catch (const std::exception& e) {
        LOG_ERROR("refresh: find_by_id threw",
                  {{"user_id", claims.user_id},
                   {"ip",      std::string(client_ip)},
                   {"type",    typeid(e).name()},
                   {"reason",  e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!row) {
        LOG_WARN("refresh: user not found",
                 {{"user_id", claims.user_id},
                  {"ip",      std::string(client_ip)}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    }

    // 5) Rotate. rotate_token_pair handles blacklist check + revoke-old
    //    + sign-new in one shot. Throws RefreshTokenRevokedError if
    //    this jti is already on the blacklist (the classic "reuse
    //    detection" path — SPEC §15.1), RefreshTokenInvalidError for
    //    any other verify failure (shouldn't happen here since we
    //    already verified above, but the helper re-verifies as a
    //    safety belt). Both map to the same 401 envelope so the wire
    //    doesn't reveal which side of the check failed.
    TokenPair tokens;
    try {
        tokens = rotate_token_pair(
            store,
            jwt_cfg.secret,
            jwt_cfg.issuer,
            presented_refresh,
            row->username,
            row->role,
            jwt_cfg.access_ttl_seconds,
            jwt_cfg.refresh_ttl_seconds);
    } catch (const RefreshTokenRevokedError& e) {
        // Reuse detection. The previously-rotated token was presented
        // again — a possible theft signal. Log at WARN with the jti
        // so operators can correlate; the response stays generic.
        LOG_WARN("refresh: token reuse detected (revoked jti presented)",
                 {{"user_id", claims.user_id},
                  {"jti",     claims.jti},
                  {"ip",      std::string(client_ip)}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    } catch (const RefreshTokenInvalidError& e) {
        LOG_WARN("refresh: rotate_token_pair invalid",
                 {{"user_id", claims.user_id},
                  {"ip",      std::string(client_ip)},
                  {"reason",  e.what()}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired refresh token");
        return;
    } catch (const std::exception& e) {
        LOG_ERROR("refresh: rotate_token_pair threw",
                  {{"user_id", claims.user_id},
                   {"ip",      std::string(client_ip)},
                   {"type",    typeid(e).name()},
                   {"reason",  e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    LOG_INFO("auth: refresh",
             {{"user_id",  std::to_string(row->id)},
              {"username", row->username},
              {"role",     row->role},
              {"src",      body_parsed ? "body_or_cookie" : "cookie"}});

    // Phase 5 ★: rotate the cookie alongside the token pair. The
    // browser drops the old cookie on receipt and adopts the new one
    // with the new TTL. The body's refresh_token is unchanged in
    // shape (we still emit it for back-compat) but the Set-Cookie
    // header is the canonical delivery mechanism.
    detail::set_refresh_cookie(res, jwt_cfg, tokens.refresh_token);

    // 6) Same envelope shape as login / register so the front-end can
    //    share its "store tokens + user" code across all three.
    send_success(res, {
        {"user", {
            {"id",       row->id},
            {"username", row->username},
            {"role",     row->role},
            {"email",    row->email ? nlohmann::json(*row->email)
                                    : nlohmann::json(nullptr)},
        }},
        {"access_token",  tokens.access_token},
        {"refresh_token", tokens.refresh_token},
        {"token_type",    "Bearer"},
        {"expires_in",    tokens.access_expires_in_seconds(
                              std::chrono::system_clock::now())},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/auth/logout   — Phase 2 ★  (SPEC §5.1, §15.1, A2)
//
//  Wire flow:
//    1. require_authentication()    — 401 envelope on missing / bad /
//                                       expired Bearer access token
//                                       (SPEC §5.1: "已登录")
//    2. parse_json_body()           — 400 INVALID_INPUT on bad JSON
//    3. detail::parse_logout_request()— 400 on missing/empty refresh_token
//    4. revoke_refresh_token()      — best-effort: parses the refresh,
//                                       checks sub == claims.user_id
//                                       (theft defense), and adds the
//                                       jti to the blacklist with a TTL
//                                       equal to the token's remaining
//                                       lifetime. NEVER throws.
//    5. send_success()              — 200 + {logged_out: true}
//
//  Why the response is ALWAYS 200 on a well-formed request:
//    The goal of /auth/logout is to forget the session. A malformed /
//    expired / already-revoked / wrong-kind refresh token is already
//    useless — there's no surviving session to invalidate. Returning
//    200 with {logged_out: true} lets the front-end clear its
//    in-memory state and navigate away without conditionals.
//
//  Theft defense (SPEC §15.1):
//    The Bearer access token's claims.user_id is passed as
//    `expected_user_id` to revoke_refresh_token. If a stolen refresh
//    (signed for victim B) is presented with an access token signed
//    for attacker A, the helper detects the mismatch, refuses to
//    revoke, and returns a {user_matched: false} outcome. The handler
//    logs the mismatch at WARN so operators can spot theft patterns;
//    the wire still answers 200 because the attacker's intent (to
//    invalidate the legitimate session) was foiled — telling them
//    "wrong user" would be the opposite of what we want.
//
//  Why NO rate limit (SPEC §5.1):
//    Logout is hit once per session and is idempotent. A client that
//    spams /auth/logout is just adding rows to the blacklist with
//    shorter TTLs (refresh tokens expire on their own); the per-IP
//    bucket is reserved for register / login / submission endpoints
//    where flooding matters.
//
//  What we DON'T do:
//    - We don't clear the access token's jti (access tokens aren't
//      tracked on the server — they're verified by signature + exp
//      alone; see SPEC §15.1). The natural 2h expiry handles the
//      rest. Revoking live access tokens is out of MVP scope.
//    - We don't write to audit_logs. Logout is a routine user action,
//      not an admin operation. The INFO log line is enough to drive
//      user-facing "last logout" UI if Phase 6 wants it.
// ────────────────────────────────────────────────────────────────────────────

inline void logout_handler(httplib::Response&                 res,
                           const httplib::Request&            req,
                           RefreshTokenStore&                 store,
                           const JwtConfig&                   jwt_cfg,
                           std::string_view                   client_ip) {
    // 1) Authentication gate. A valid Bearer access token is required
    //    (SPEC §5.1: "已登录"). require_authentication throws
    //    ApiException(401, UNAUTHORIZED) on every failure mode, which
    //    server.h's per-request wrap turns into the unified envelope.
    const Claims claims = require_authentication(req, jwt_cfg);

    // 2) Body — must contain a refresh_token. Same shape as
    //    /auth/refresh so the front-end can reuse its JSON plumbing.
    auto j = parse_json_body(req, res);
    if (!j) return;                                  // 400 already on the wire

    auto parsed = detail::parse_logout_request(*j, res);
    if (!parsed) return;                             // 400 already on the wire

    // 3) Best-effort revocation. revoke_refresh_token() NEVER throws —
    //    it folds every failure mode (malformed JWT, expired, wrong
    //    kind, theft-mismatch) into the returned RevokeOutcome so the
    //    wire stays consistent at 200. The detailed reason is logged
    //    below at the appropriate level.
    const auto outcome = revoke_refresh_token(
        store,
        parsed->refresh_token,
        jwt_cfg.secret,
        jwt_cfg.issuer,
        jwt_cfg.refresh_ttl_seconds,
        /*expected_user_id=*/claims.user_id);

    if (!outcome.parsed) {
        // Malformed / expired / wrong-kind refresh. The session
        // we're trying to forget is already useless; nothing to
        // revoke. Logged at INFO because this is the common case
        // for a client that already let the refresh expire.
        LOG_INFO("auth: logout (refresh did not parse)",
                 {{"user_id",  claims.user_id},
                  {"ip",       std::string(client_ip)},
                  {"reason",   outcome.reason}});
    } else if (!outcome.user_matched) {
        // Theft signal: the refresh is for a different user than the
        // access token. Don't revoke (the legitimate user's session
        // is intact), but record it so operators can correlate.
        LOG_WARN("auth: logout theft-mismatch (refresh sub != access sub)",
                 {{"access_user_id",  claims.user_id},
                  {"refresh_user_id", outcome.jti},
                  {"reason",          outcome.reason},
                  {"ip",              std::string(client_ip)}});
    } else if (outcome.revoked) {
        LOG_INFO("auth: logout",
                 {{"user_id",  claims.user_id},
                  {"jti",      outcome.jti},
                  {"ip",       std::string(client_ip)}});
    } else {
        // Parsed + matched, but not revoked — should be unreachable
        // given the current revoke_refresh_token contract, but log it
        // defensively in case the helper grows a new failure mode.
        LOG_INFO("auth: logout (no-op)",
                 {{"user_id",  claims.user_id},
                  {"jti",      outcome.jti},
                  {"ip",       std::string(client_ip)}});
    }

    // 4) Always 200 — the front-end clears local state and moves on.
    //    The "revoked" field lets the front-end distinguish a clean
    //    logout (true) from a no-op logout (false: token was already
    //    invalid). Both are non-error outcomes from the API's POV.
    //
    // Phase 5 ★: clear the HttpOnly cookie too, so the browser drops
    // it on receipt. Even if the JS layer forgets to call clear(),
    // the cookie is gone — closing the XSS-steal window where an
    // attacker who already has a stolen refresh could keep using it
    // until natural expiry. We send the clear BEFORE the body so a
    // concurrent /refresh that raced in can't mint a cookie we then
    // immediately delete (httplib sends headers before body either way).
    detail::clear_refresh_cookie(res);

    send_success(res, {
        {"logged_out", true},
        {"revoked",    outcome.revoked},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/auth/profile   — Phase 2 ★  (SPEC §5.1, A3)
//
//  Wire flow:
//    1. require_authentication()    — 401 envelope on missing / bad /
//                                       expired Bearer access token
//                                       (SPEC §5.1: "已登录")
//    2. user_repo::find_by_id()     — 401 if the user was deleted
//                                       between token issuance and
//                                       this request; the JWT is still
//                                       signature-valid but the row is
//                                       gone, so the session is
//                                       effectively dead.
//    3. send_success()              — 200 + {user: {...}}
//
//  Response shape — the `user` block carries the same fields as the
//  login / register / refresh `user` blocks (id / username / role /
//  email) plus three extras the front-end's profile page can render
//  without a second request:
//      avatar      — null when the user has no avatar
//      created_at  — string "YYYY-MM-DD HH:MM:SS"
//      last_login  — null when the user has never logged in
//  We deliberately do NOT expose:
//      password_hash — would let a stolen access token trivially
//                      bypass re-auth on any other service that
//                      shares the same bcrypt cost factor
//      last_login_ip — session metadata, not profile display; also
//                      unnecessary surface for an attacker who has
//                      already hijacked a Bearer token
//  No rate limit (SPEC §5.1: profile row has `-` in the rate-limit
//  column). It's hit on every page load by every logged-in user, so
//  the per-IP bucket is reserved for register / login / submission
//  endpoints where flooding matters.
// ────────────────────────────────────────────────────────────────────────────

inline void profile_handler(httplib::Response&      res,
                            const httplib::Request& req,
                            ConnectionPool&         pool,
                            const JwtConfig&        jwt_cfg) {
    // 1) Authentication gate. require_authentication throws
    //    ApiException(401, UNAUTHORIZED) on every failure mode (no
    //    header / wrong scheme / empty / malformed / bad sig / expired
    //    / wrong issuer / wrong kind) — server.h's per-request wrap
    //    turns it into the unified envelope.
    const Claims claims = require_authentication(req, jwt_cfg);

    // 2) sub is a decimal string of the user's id (same shape we emit
    //    in sign_access / sign_refresh). Anything else is a tampered
    //    token; require_authentication already rejected those with a
    //    signature failure, so reaching this line with a non-numeric
    //    sub is unreachable in practice — but a defensive parse keeps
    //    std::stoi exceptions from escaping the handler.
    int user_id = 0;
    try {
        user_id = std::stoi(claims.user_id);
    } catch (const std::exception&) {
        LOG_WARN("profile: malformed sub claim",
                 {{"sub", claims.user_id}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired token");
        return;
    }
    if (user_id <= 0) {
        LOG_WARN("profile: non-positive sub claim",
                 {{"sub", claims.user_id}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "invalid or expired token");
        return;
    }

    // 3) Look up the user. nullopt ⇒ the user was deleted (or never
    //    existed — same outcome). Anti-enumeration: the JWT carries
    //    the user_id, so an attacker hitting /profile already knows
    //    who they're probing. The 401 envelope surfaces the missing
    //    row to the front-end so it can clear local state and bounce
    //    the user to /login.
    std::optional<UserRow> row;
    try {
        row = user_repo::find_by_id(pool, user_id);
    } catch (const std::exception& e) {
        LOG_ERROR("profile: find_by_id threw",
                  {{"user_id", claims.user_id},
                   {"type",    typeid(e).name()},
                   {"reason",  e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!row) {
        LOG_WARN("profile: user not found",
                 {{"user_id", claims.user_id}});
        send_error(res, 401, ErrorCode::UNAUTHORIZED,
                   "user not found");
        return;
    }

    LOG_INFO("auth: profile",
             {{"user_id",  std::to_string(row->id)},
              {"username", row->username},
              {"role",     row->role}});

    // 4) Emit the user block. Optional columns are lifted into a
    //    JSON null when the DB column is NULL — never an empty string,
    //    so the front-end can branch on `user.email === null` cleanly.
    nlohmann::json user_block = {
        {"id",         row->id},
        {"username",   row->username},
        {"role",       row->role},
        {"email",      row->email
                          ? nlohmann::json(*row->email)
                          : nlohmann::json(nullptr)},
        {"avatar",     row->avatar
                          ? nlohmann::json(*row->avatar)
                          : nlohmann::json(nullptr)},
        {"created_at", row->created_at},
        {"last_login", row->last_login
                          ? nlohmann::json(*row->last_login)
                          : nlohmann::json(nullptr)},
    };
    send_success(res, {{"user", user_block}});
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Returns HttpServer& so callers can chain.
//
//  Production usage (from main.cpp):
//
//    litecode::HttpServer server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(litecode::PoolConfig::from_database_config(cfg.database));
//    litecode::RateLimiter limiter;
//    litecode::LoginFailureTracker tracker;
//    litecode::InMemoryRefreshTokenStore store;     // (or Redis-backed later)
//    litecode::register_auth_routes(server, pool, limiter, tracker, store,
//                                   cfg.jwt, cfg.rate_limit);
//    server.listen_blocking();
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh tracker (so failure counts don't bleed between cases) +
//  a fresh InMemoryRefreshTokenStore (so blacklist state is per-test) +
//  the process-wide rate limiter.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_auth_routes(HttpServer&        server,
                                        ConnectionPool&    pool,
                                        RateLimiter&       limiter,
                                        LoginFailureTracker& tracker,
                                        RefreshTokenStore& store,
                                        const JwtConfig&   jwt_cfg,
                                        const RateLimitConfig& rate_cfg) {
    // POST /api/v1/auth/register — 5/min/IP (SPEC §5.1)
    //
    // The lambda captures jwt_cfg / rate_cfg BY VALUE. The previous
    // by-reference variant had a subtle lifetime bug: callers that
    // passed temporary JwtConfig / RateLimitConfig (e.g. `dev_jwt()`
    // returning a fresh struct) would leave the lambda with dangling
    // references once the temporary was destroyed. Capturing by value
    // costs ~30 bytes per lambda and is the safe default.
    //
    // pool / limiter / tracker are still captured by reference — they're
    // owned by the caller (main() / test fixture) and outlive the server.
    server.post("/api/v1/auth/register",
        [&pool, &limiter, jwt_cfg, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                // 1) Rate limit FIRST so a flood of malformed requests
                //    doesn't blow past validation work. consume_rate_limit
                //    throws ApiException(429, RATE_LIMITED) on deny.
                consume_rate_limit(res, req, limiter,
                                   auth_register_quota(rate_cfg));
                // 2..5) Registration pipeline.
                register_handler(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                // Anything else — log with the dynamic type so we can
                // tighten the catch list once the surface is pinned,
                // then write a generic 500 envelope.
                LOG_ERROR("register: handler threw",
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

    // POST /api/v1/auth/login — 10/min/IP (SPEC §5.1, Phase 2 ★)
    //
    // The IP passed to audit_log_repo::record_login_failure comes
    // from extract_client_ip (in rate_limit.h) — same source the per-IP
    // bucket uses, so the audit row's `ip` column matches the bucket
    // key. Keeping both on the same definition prevents the audit
    // table from showing a different "where" than the rate limiter
    // saw.
    server.post("/api/v1/auth/login",
        [&pool, &limiter, &tracker, jwt_cfg, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                consume_rate_limit(res, req, limiter,
                                   auth_login_quota(rate_cfg));
                const std::string ip =
                    extract_client_ip(req);
                login_handler(res, req, pool, jwt_cfg, tracker, ip);
            } catch (const ApiException&) {
                throw;                                  // unified envelope
            } catch (const std::exception& e) {
                LOG_ERROR("login: handler threw",
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

    // The remaining auth endpoints (profile) are reserved for the
    // follow-up Phase 2 work. We register a 501 placeholder so the
    // route table is stable and the front-end's integration can be
    // staged endpoint by endpoint. The placeholder returns a SPEC
    // §5.7 envelope so callers always get the same shape they expect
    // from the eventual implementation.
    auto not_implemented = [](const httplib::Request&,
                              httplib::Response& res) {
        send_error(res, 501, ErrorCode::SERVICE_UNAVAILABLE,
                   "this auth endpoint is not yet implemented "
                   "(see SPEC §11 Phase 2)");
    };

    // POST /api/v1/auth/refresh — Phase 2 ★  (SPEC §5.1)
    //
    // No rate limit (SPEC §5.1): refresh is meant to be hit on a
    // schedule as the access token ages out. The blacklist + short
    // access TTL provide the actual security — a stolen refresh is
    // dead the moment the legitimate user rotates it.
    //
    // The lambda captures pool / store BY REFERENCE (they outlive the
    // server, owned by main() / the test fixture) and jwt_cfg BY VALUE
    // (same reasoning as the register / login lambdas above — defends
    // against a temporary JwtConfig going out of scope).
    server.post("/api/v1/auth/refresh",
        [&pool, &store, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                const std::string ip = extract_client_ip(req);
                refresh_handler(res, req, pool, store, jwt_cfg, ip);
            } catch (const ApiException&) {
                throw;                                  // unified envelope
            } catch (const std::exception& e) {
                LOG_ERROR("refresh: handler threw",
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

    // POST /api/v1/auth/logout — Phase 2 ★  (SPEC §5.1)
    //
    // No rate limit (SPEC §5.1): logout is hit once per session and
    // is idempotent. A flood of logouts just adds rows to the blacklist
    // with shorter TTLs (refresh tokens self-expire on their own).
    //
    // The Bearer access token gates the endpoint (SPEC §5.1: "已登录").
    // The user_id from the verified claims is fed to
    // revoke_refresh_token as the theft-defense check, so a stolen
    // refresh presented with the attacker's own access token is
    // detected and refused.
    //
    // `pool` is NOT captured — logout_handler doesn't touch the DB
    // (Phase 2 ★ keeps logout out of audit_logs since it's a routine
    // user action, not an admin operation; the INFO log line is
    // enough to drive user-facing "last logout" UI if Phase 6 wants
    // it). When Phase 6 adds an admin-side session-invalidation
    // hook, it'll either route through this same endpoint with
    // admin context or open a new /api/v1/admin/sessions endpoint.
    server.post("/api/v1/auth/logout",
        [&store, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                const std::string ip = extract_client_ip(req);
                logout_handler(res, req, store, jwt_cfg, ip);
            } catch (const ApiException&) {
                throw;                                  // unified envelope
            } catch (const std::exception& e) {
                LOG_ERROR("logout: handler threw",
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

    // GET /api/v1/auth/profile — Phase 2 ★  (SPEC §5.1)
    //
    // No rate limit (SPEC §5.1). The endpoint is hit on every page
    // load by every logged-in user; the per-IP bucket is reserved for
    // register / login / submission endpoints where flooding matters.
    //
    // The Bearer access token gates the endpoint (SPEC §5.1: "已登录").
    // `store` is NOT captured — profile doesn't touch the refresh-
    // token blacklist (revoking a session is /auth/logout's job).
    // `limiter` / `tracker` / `rate_cfg` are NOT captured — profile
    // has no rate-limit budget. We capture `pool` BY REFERENCE (owned
    // by main() / test fixture) and `jwt_cfg` BY VALUE (defends
    // against a temporary JwtConfig going out of scope).
    server.get("/api/v1/auth/profile",
        [&pool, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                profile_handler(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("profile: handler threw",
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

} // namespace litecode