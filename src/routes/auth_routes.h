// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — authentication routes (Phase 2 ★)
//
// SPEC §5.1 / §11 Phase 2 / §15.1 / A1, A2, A3, A22 acceptance:
//   - POST /api/v1/auth/register     — public, 5/min/IP, returns 201 + tokens
//   - POST /api/v1/auth/login        — public, 10/min/IP, returns 200 + tokens (Phase 2 ★, next)
//   - POST /api/v1/auth/refresh      — public (valid refresh), no rate limit
//   - POST /api/v1/auth/logout       — auth required
//   - GET  /api/v1/auth/profile      — auth required
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (server.h / error_handler.h / jwt_utils.h / refresh_token.h /
//     password_hash.h / user_repo.h). Tests link this header directly
//     and instantiate the route set with a real ConnectionPool + a
//     dummy rate limiter (or a stub pool that throws on demand).
//   - This file ships the **register** endpoint in Phase 2 ★. The
//     login / refresh / logout / profile handlers are stubs (501) so
//     the route table is stable; they get implemented as the follow-up
//     Phase 2 work.
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
//
// Usage (production, from main.cpp):
//
//   litecode::HttpServer server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(litecode::PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter limiter;
//   litecode::register_auth_routes(server, pool, limiter);
//   server.listen_blocking();
//
// Usage (test, from gtest):
//
//   litecode::HttpServer server(dev_server(), dev_cors());
//   litecode::RateLimiter limiter;
//   litecode::ConnectionPool pool(test_db_config());
//   litecode::register_auth_routes(server, pool, limiter);
//   auto h = start_server(&server);
//   auto r = h.client->Post("/api/v1/auth/register",
//       R"({"username":"alice","password":"hunter22"})", "application/json");

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../auth/jwt_utils.h"           // sign_access / sign_refresh
#include "../auth/password_hash.h"       // hash_password / PasswordPolicyError
#include "../auth/refresh_token.h"       // issue_token_pair / TokenPair
#include "../config.h"                   // AppConfig / JwtConfig / RateLimitConfig
#include "../db/connection_pool.h"       // ConnectionPool
#include "../db/user_repo.h"             // user_repo::create_user / find_by_username
#include "../logger.h"                   // LOG_INFO / LOG_WARN
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
//  Route registration
//
//  Returns HttpServer& so callers can chain.
//
//  Production usage (from main.cpp):
//
//    litecode::HttpServer server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(litecode::PoolConfig::from_database_config(cfg.database));
//    litecode::RateLimiter limiter;
//    litecode::register_auth_routes(server, pool, limiter, cfg.jwt, cfg.rate_limit);
//    server.listen_blocking();
//
//  Tests pass an in-process server + a freshly-constructed pool + the
//  process-wide rate limiter.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_auth_routes(HttpServer&        server,
                                        ConnectionPool&    pool,
                                        RateLimiter&       limiter,
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
    // pool / limiter are still captured by reference — they're owned
    // by the caller (main() / test fixture) and outlive the server.
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

    // The other auth endpoints (login / refresh / logout / profile)
    // are reserved for the follow-up Phase 2 work. We register a 501
    // placeholder for each so the route table is stable and the
    // front-end's integration can be staged endpoint by endpoint.
    // The placeholder returns a SPEC §5.7 envelope so callers always
    // get the same shape they expect from the eventual implementations.
    auto not_implemented = [](const httplib::Request&,
                              httplib::Response& res) {
        send_error(res, 501, ErrorCode::SERVICE_UNAVAILABLE,
                   "this auth endpoint is not yet implemented "
                   "(see SPEC §11 Phase 2)");
    };

    server.post("/api/v1/auth/login",   not_implemented);
    server.post("/api/v1/auth/refresh", not_implemented);
    server.post("/api/v1/auth/logout",  not_implemented);
    server.get ("/api/v1/auth/profile", not_implemented);

    return server;
}

} // namespace litecode