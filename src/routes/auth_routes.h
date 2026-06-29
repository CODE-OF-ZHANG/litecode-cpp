// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — authentication routes (Phase 2 ★)
//
// SPEC §5.1 / §11 Phase 2 / §15.1 / A1, A2, A3, A22 acceptance:
//   - POST /api/v1/auth/register     — public, 5/min/IP, returns 201 + tokens
//   - POST /api/v1/auth/login        — public, 10/min/IP, returns 200 + tokens
//                                       + per-username failure audit
//                                       (every 5th attempt → audit_logs)
//   - POST /api/v1/auth/refresh      — public (valid refresh), no rate limit
//   - POST /api/v1/auth/logout       — auth required
//   - GET  /api/v1/auth/profile      — auth required
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
//   - Phase 2 ★ ships **register** + **login** + **refresh** + **logout**.
//     Profile still returns 501 — it's the follow-up Phase 2 work
//     and its stub keeps the route table stable.
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
#include "../config.h"                   // AppConfig / JwtConfig / RateLimitConfig
#include "../db/audit_log_repo.h"        // audit_log_repo::record_login_failure
#include "../db/connection_pool.h"       // ConnectionPool
#include "../db/user_repo.h"             // user_repo::create_user / find_by_username
#include "../logger.h"                   // LOG_INFO / LOG_WARN
#include "../middleware/auth_middleware.h" // require_authentication (Phase 2 ★ logout)
#include "../middleware/rate_limit.h"    // consume_rate_limit / RateLimiter / auth_register_quota
#include "../server.h"                   // HttpServer / send_error / send_created / ErrorCode
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

    send_created(res, {
        {"user", {
            {"id",         new_id},
            {"username",   row.username},
            {"role",       row.role},
            {"email",      row.email ? nlohmann::json(*row.email)
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
//  Login-failure tracker
//
//  In-memory, per-username failure counter used by /api/v1/auth/login
//  to satisfy SPEC §15.1 ("失败登录 5 次 → 写 audit_logs"):
//
//    - record_failure() bumps the counter for `username` and returns
//      the new count. The caller then writes to audit_logs when the
//      count crosses the kAuditLogEvery threshold (5, 10, 15, ...).
//    - reset() is called after a successful login so a legitimate
//      user who fat-fingered their password twice isn't penalized
//      forever.
//    - `max_entries` caps the map at 100k distinct usernames — defense
//      against an attacker poking with random usernames to grow RSS.
//      When the cap is hit, the entry with the highest count (most
//      evidence of misbehavior) is dropped.
//
//  Thread safety: a single mutex guards the map. record_failure() and
//  reset() are O(1) amortized.
//
//  Tests reset the process-wide tracker between cases so state from
//  a prior test doesn't bleed in.
// ────────────────────────────────────────────────────────────────────────────

class LoginFailureTracker {
public:
    static constexpr int kAuditLogEvery = 5;   // threshold for audit_logs
    explicit LoginFailureTracker(std::size_t max_entries = 100000)
        : max_entries_(max_entries) {}

    struct Outcome {
        int  count = 0;                          // new failure count for username
        bool should_audit = false;               // true iff this attempt crossed a multiple of kAuditLogEvery
    };

    Outcome record_failure(std::string_view username) {
        Outcome out;
        if (username.empty()) {
            return out;
        }
        std::lock_guard<std::mutex> g(mu_);

        // Opportunistic cap enforcement: if we're at the cap, drop the
        // entry with the largest count (it's the most evidence we'd be
        // throwing away — fresh attackers with random usernames are
        // cheap to re-track, but a long-banned user losing their
        // counter is fine because a fresh failure re-creates it).
        if (max_entries_ > 0 && failures_.size() >= max_entries_) {
            auto victim = failures_.begin();
            for (auto it = failures_.begin(); it != failures_.end(); ++it) {
                if (it->second.count > victim->second.count) victim = it;
            }
            failures_.erase(victim);
        }

        auto& entry = failures_[std::string(username)];
        ++entry.count;
        out.count        = entry.count;
        out.should_audit = (entry.count > 0)
                        && (entry.count % kAuditLogEvery == 0);

        // We deliberately do NOT decay / expire entries by wall clock
        // here. SPEC §15.1 mentions "15 分钟内" as a lockout window
        // (Phase 6 ★), but counter decay is a separate concern from
        // the audit-log trigger — Phase 6 will introduce its own
        // lockout state machine. Keeping this tracker pure-count keeps
        // the Phase 2 surface small.
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

    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        failures_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return failures_.size();
    }

    std::size_t max_entries() const { return max_entries_; }

private:
    struct Entry {
        int count = 0;
    };

    mutable std::mutex                       mu_;
    std::unordered_map<std::string, Entry>   failures_;
    std::size_t                              max_entries_;
};

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/auth/login   — Phase 2 ★  (SPEC §5.1, §15.1, §16.4, A2)
//
//  Wire flow:
//    1. consume_rate_limit()           — 10/min/IP; 429 envelope on deny
//    2. parse_json_body()              — 400 INVALID_INPUT on bad JSON
//    3. detail::parse_login_request()  — 400 on missing fields / bad
//                                        username shape
//    4. user_repo::find_by_username()  — 401 if no such user
//    5. verify_password()              — 401 if mismatch
//    6. audit_log (every 5th failure)  — fire-and-forget row insert
//    7. tracker.reset()                — successful login clears the
//                                        counter for this username
//    8. user_repo::update_last_login() — best-effort last_login stamp
//    9. issue_token_pair()             — access (2h) + refresh (7d)
//   10. send_success()                 — 200 + {user, tokens, ...}
//
//  Anti-enumeration (SPEC §15.1): the 401 message is identical for
//  "no such user" and "wrong password" — "invalid username or
//  password". The route handler returns 401 in either case via a
//  single throw so the wire body is byte-for-byte the same.
//
//  Failure-audit (SPEC §15.1): each failed login bumps an in-memory
//  counter keyed by username. At every kAuditLogEvery (5) crossings
//  we INSERT a row into audit_logs with action="auth.login_failure".
//  Counts are per-username so a brute-force probe targeting one
//  account is captured cleanly without polluting other accounts' logs.
//
//  Why we DON'T do rate-limit-by-username here: SPEC §5.1 only
//  budgets login at 10/min/IP. The per-username counter is for
//  audit, not throttling — Phase 6 will introduce a separate
//  "lockout" state machine if SPEC §6.6 marks it as required.
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
        LOG_WARN("auth: login failed",
                 {{"username",                  username_for_audit},
                  {"consecutive_failures",      std::to_string(outcome.count)},
                  {"should_audit",              outcome.should_audit ? "true" : "false"}});
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
    auto j = parse_json_body(req, res);
    if (!j) return;                                  // 400 already on the wire

    auto parsed = detail::parse_refresh_request(*j, res);
    if (!parsed) return;                             // 400 already on the wire

    // 3) Verify the refresh token to recover the sub + jti + exp.
    //    We do this BEFORE the user lookup so a malformed / wrong-kind
    //    token fails fast (no DB hit) and gets the same 401 envelope
    //    a revoked token would (anti-enumeration).
    Claims claims;
    try {
        claims = verify(parsed->refresh_token,
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
            parsed->refresh_token,
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
              {"role",     row->role}});

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
    send_success(res, {
        {"logged_out", true},
        {"revoked",    outcome.revoked},
    });
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

    // The remaining auth endpoints (logout / profile) are reserved
    // for the follow-up Phase 2 work. We register a 501 placeholder
    // for each so the route table is stable and the front-end's
    // integration can be staged endpoint by endpoint. The placeholder
    // returns a SPEC §5.7 envelope so callers always get the same
    // shape they expect from the eventual implementations.
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

    server.get ("/api/v1/auth/profile", not_implemented);

    return server;
}

} // namespace litecode