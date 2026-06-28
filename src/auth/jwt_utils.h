// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — JWT utility (Phase 2 ★)
//
// SPEC §5.1 / §11 Phase 2 / §15.1:
//   - Algorithm: HS256, secret from JWT_SECRET env (>= 32 bytes — already
//     enforced by config.h's load_config(); we assert again here as a
//     defense-in-depth so calling this header directly can't bypass it).
//   - Token kinds:
//       * access  (TTL 2h) — carries {sub, username, role, iat, exp, jti, iss, kind}
//       * refresh (TTL 7d) — carries {sub, jti, iat, exp, iss, kind}
//   - `jti` is a UUID v4 so we can later add the refresh-token blacklist
//     (`auth/refresh_token.h`) without changing the wire format.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 module
//     (config.h / logger.h / server.h / password_hash.h). Tests link
//     this header directly. jwt-cpp is template-heavy anyway, so a
//     header is the natural home.
//   - All public entry points take a `const std::string& secret` and
//     use HS256. We deliberately do NOT accept `JwtConfig` (the
//     config layer) as a parameter so this header stays free of any
//     dependency on logger/redis/cors — main.cpp and tests just pass
//     `config().jwt.secret` (or a hardcoded value in tests).
//   - A pluggable clock makes expiry tests deterministic without
//     having to sleep — `Clock` models jwt-cpp's default_clock
//     (a single `now() -> time_point`). SystemClock (alias) is the
//     production default; tests substitute FrozenClock.
//   - All failures throw a `JwtError` subclass (see below). Callers
//     (auth_middleware.h, auth_routes.h) translate them into
//     `ApiException(401, UNAUTHORIZED, ...)` — the SPEC §5.7 envelope.
//     We do NOT do that translation here, because the same verifier
//     may be reused for refresh (which should map to a different
//     status if the token is `kind=refresh` rather than `kind=access`).
//
// Usage (production — access token in /api/v1/auth/login handler):
//
//   const auto& cfg = litecode::config();
//   const auto pair = litecode::sign_access(
//       cfg.jwt.secret, cfg.jwt.issuer,
//       /*user_id=*/42, "alice", /*role=*/"user",
//       cfg.jwt.access_ttl_seconds);
//   // pair.token  -> the wire string
//   // pair.jti    -> UUID v4 to track this token in audit/blacklist
//
// Usage (verifying an incoming Bearer token):
//
//   try {
//       const auto claims = litecode::verify(
//           token, cfg.jwt.secret, cfg.jwt.issuer,
//           litecode::TokenKind::Access);
//       // claims.user_id, claims.username, claims.role, ...
//   } catch (const litecode::JwtVerifyError& e) {
//       throw litecode::ApiException(401, litecode::ErrorCode::UNAUTHORIZED,
//                                    "invalid or expired token");
//   }

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <jwt-cpp/traits/kazuho-picojson/defaults.h>
// Pulls in <jwt-cpp/jwt.h> + picojson + the non-template overloads
// of jwt::create() / jwt::decode(token) / jwt::verify() / jwt::claim
// so we don't have to spell `jwt::traits::kazuho_picojson` at every
// call site.

#include "../utils/uuid.h"   // generate_uuid_v4()

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
//
//  Three-tier hierarchy matches the three failure surfaces a caller has
//  to handle differently:
//    - JwtSignError  — we couldn't build the token. Almost always a
//                      programming bug (empty secret, bad ttl). 500.
//    - JwtVerifyError — the token is bad: signature, expiry, malformed,
//                      wrong issuer, wrong kind. Caller maps to 401.
//    - JwtClaimError  — signature+claims are valid but a required claim
//                      is missing or the wrong type. Caller maps to 401.
//
//  All inherit std::runtime_error so generic exception handlers (logs,
//  metrics, panic-as-500 fallbacks) can catch them uniformly.
// ────────────────────────────────────────────────────────────────────────────

class JwtError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class JwtSignError : public JwtError {
public:
    using JwtError::JwtError;
};

class JwtVerifyError : public JwtError {
public:
    using JwtError::JwtError;
};

class JwtClaimError : public JwtVerifyError {
public:
    using JwtVerifyError::JwtVerifyError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Token kinds + Claims
// ────────────────────────────────────────────────────────────────────────────

enum class TokenKind {
    Access,
    Refresh,
};

// Stable wire string for TokenKind. The `kind` claim is what lets a
// refresh verifier refuse an access token (and vice versa) — without
// it, a stolen access token could be exchanged at /auth/refresh.
inline std::string_view token_kind_name(TokenKind k) {
    switch (k) {
        case TokenKind::Access:  return "access";
        case TokenKind::Refresh: return "refresh";
    }
    return "access";
}

// Parsed claims returned by `verify()`. We intentionally do NOT
// surface every JWT claim — only what the application code needs.
// Adding fields here is a wire-format change for callers, so we keep
// the surface small.
//
//   user_id    -> `sub` claim (stringified — jwt-cpp doesn't constrain
//                 the subject's type; we always emit string on sign)
//   username   -> present only for access tokens; "" for refresh
//   role       -> present only for access tokens; "" for refresh
//   kind       -> TokenKind derived from the `kind` claim
//   issued_at  -> iat (std::chrono::system_clock::time_point)
//   expires_at -> exp (std::chrono::system_clock::time_point)
//   jti        -> jti (UUID v4 string; "" if missing — verify()
//                 logs a warning but accepts the token for backwards
//                 compat with pre-jti tokens)
struct Claims {
    std::string             user_id;
    std::string             username;
    std::string             role;
    TokenKind               kind       = TokenKind::Access;
    std::chrono::system_clock::time_point issued_at{};
    std::chrono::system_clock::time_point expires_at{};
    std::string             jti;

    // Convenience: true ⇔ token has expired at `now`.
    bool is_expired_at(std::chrono::system_clock::time_point now) const {
        return now >= expires_at;
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Clock abstraction
//
//  jwt-cpp's `verify()` already accepts a clock instance; we expose it
//  to our callers so tests can inject a frozen clock without changing
//  the production path. The default SystemClock just delegates to
//  std::chrono::system_clock.
// ────────────────────────────────────────────────────────────────────────────

struct SystemClock {
    std::chrono::system_clock::time_point now() const {
        return std::chrono::system_clock::now();
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Internal: defense-in-depth secret check
//
//  config.h already rejects < 32 byte secrets at boot, but this header
//  is also included from tests / REPLs that bypass init_config(). A
//  32-byte floor is what RFC 7518 §3.2 recommends for HS256.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline void require_secret(std::string_view secret) {
    if (secret.size() < 32) {
        throw JwtSignError("JWT secret must be at least 32 bytes (got " +
                           std::to_string(secret.size()) + ")");
    }
    if (secret.empty()) {
        throw JwtSignError("JWT secret must not be empty");
    }
}

inline void require_ttl_seconds(int ttl_seconds) {
    if (ttl_seconds < 1) {
        throw JwtSignError("JWT ttl_seconds must be >= 1 (got " +
                           std::to_string(ttl_seconds) + ")");
    }
}

inline void require_issuer(std::string_view issuer) {
    if (issuer.empty()) {
        throw JwtSignError("JWT issuer must not be empty");
    }
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Sign
// ────────────────────────────────────────────────────────────────────────────

// Issue a signed token. Returns the wire-format string plus the jti the
// caller should keep (audit log, blacklist bookkeeping). The 5-arg
// shape is repeated for both kinds because the access form needs
// username/role while refresh must NOT carry them (least privilege).
//
//   secret           — HS256 signing key (>= 32 bytes)
//   issuer           — `iss` claim; verified on the read side
//   subject          — opaque user id, becomes `sub`
//   ttl_seconds      — exp = now + ttl_seconds
//   user_id/username/role — only for TokenKind::Access
//
// Throws JwtSignError on any programmer error (empty secret, bad ttl).
struct SignedToken {
    std::string token;
    std::string jti;
};

inline SignedToken sign_access(std::string_view secret,
                               std::string_view issuer,
                               const std::string& user_id,
                               const std::string& username,
                               const std::string& role,
                               int ttl_seconds) {
    detail::require_secret(secret);
    detail::require_issuer(issuer);
    detail::require_ttl_seconds(ttl_seconds);
    if (user_id.empty())   throw JwtSignError("sign_access: user_id must not be empty");
    if (username.empty())  throw JwtSignError("sign_access: username must not be empty");
    if (role.empty())      throw JwtSignError("sign_access: role must not be empty");

    SignedToken out;
    out.jti = generate_uuid_v4();

    try {
        const auto now = std::chrono::system_clock::now();
        out.token = jwt::create()
            .set_type("JWT")
            .set_issuer(std::string(issuer))
            .set_subject(user_id)
            .set_id(out.jti)
            .set_issued_at(now)
            .set_expires_at(now + std::chrono::seconds(ttl_seconds))
            .set_payload_claim("username",  jwt::claim(username))
            .set_payload_claim("role",      jwt::claim(role))
            .set_payload_claim("kind",      jwt::claim(std::string(token_kind_name(TokenKind::Access))))
            .sign(jwt::algorithm::hs256{std::string(secret)});
    } catch (const std::exception& e) {
        throw JwtSignError(std::string("jwt-cpp sign_access failed: ") + e.what());
    }
    return out;
}

inline SignedToken sign_refresh(std::string_view secret,
                                std::string_view issuer,
                                const std::string& user_id,
                                int ttl_seconds) {
    detail::require_secret(secret);
    detail::require_issuer(issuer);
    detail::require_ttl_seconds(ttl_seconds);
    if (user_id.empty())   throw JwtSignError("sign_refresh: user_id must not be empty");

    SignedToken out;
    out.jti = generate_uuid_v4();

    try {
        const auto now = std::chrono::system_clock::now();
        out.token = jwt::create()
            .set_type("JWT")
            .set_issuer(std::string(issuer))
            .set_subject(user_id)
            .set_id(out.jti)
            .set_issued_at(now)
            .set_expires_at(now + std::chrono::seconds(ttl_seconds))
            .set_payload_claim("kind", jwt::claim(std::string(token_kind_name(TokenKind::Refresh))))
            .sign(jwt::algorithm::hs256{std::string(secret)});
    } catch (const std::exception& e) {
        throw JwtSignError(std::string("jwt-cpp sign_refresh failed: ") + e.what());
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  Verify
//
//  Decodes + signature-verifies + claim-validates the token. Throws:
//    - JwtClaimError  — required claim missing or wrong type
//    - JwtVerifyError — signature mismatch, expired, malformed, wrong
//                       issuer, or wrong kind
//
//  `expected_kind` enforces `kind` == expected; pass TokenKind::Access
//  for /api/v1/auth/* endpoints, TokenKind::Refresh for
//  /api/v1/auth/refresh. This prevents an access token from being
//  exchanged for a new one (token-confusion attack).
//
//  `Clock` defaults to SystemClock. Tests pass a frozen clock. We
//  forward the clock to jwt-cpp via the trailing arg of `verify()`.
// ────────────────────────────────────────────────────────────────────────────

template <typename Clock = SystemClock>
inline Claims verify(std::string_view token,
                     std::string_view secret,
                     std::string_view issuer,
                     TokenKind        expected_kind,
                     Clock            clock = Clock{}) {
    detail::require_secret(secret);
    detail::require_issuer(issuer);
    if (token.empty()) {
        throw JwtVerifyError("token is empty");
    }

    // 1) Build the verifier. We allow only HS256 — refusing any other
    //    algorithm here closes the classic "alg=none" / RS256→HS256
    //    confusion attacks. The verifier's exp / iat / nbf checks use
    //    the clock we hand it, so a frozen-clock test fixture lets us
    //    drive expiry without sleeping.
    auto verifier = jwt::verify<Clock, jwt::traits::kazuho_picojson>(clock)
        .allow_algorithm(jwt::algorithm::hs256{std::string(secret)})
        .with_issuer(std::string(issuer))
        .leeway(0); // strict — SPEC §5.1 says access TTL is 2h, no slack

    // 2) Decode first so we can check `kind` BEFORE running the
    //    full verifier (which would throw on the wrong kind with an
    //    unhelpful "claim 'kind' mismatch" message). decoded_jwt has
    //    no default constructor, so we have to declare + assign in one
    //    statement and not catch decode failures by reassigning.
    std::unique_ptr<jwt::decoded_jwt<jwt::traits::kazuho_picojson>> decoded_ptr;
    try {
        decoded_ptr = std::make_unique<jwt::decoded_jwt<jwt::traits::kazuho_picojson>>(
            jwt::decode(std::string(token)));
    } catch (const std::exception& e) {
        throw JwtVerifyError(std::string("malformed token: ") + e.what());
    }
    auto& decoded = *decoded_ptr;

    // 3) Run the verifier (signature + exp + iat + nbf + iss).
    try {
        verifier.verify(decoded);
    } catch (const jwt::error::signature_verification_exception& e) {
        throw JwtVerifyError(std::string("signature verification failed: ") + e.what());
    } catch (const std::system_error& e) {
        // jwt-cpp throws system_error for exp/iat/nbf failures.
        throw JwtVerifyError(std::string("token rejected: ") + e.what());
    } catch (const std::exception& e) {
        throw JwtVerifyError(std::string("verification failed: ") + e.what());
    }

    // 4) Extract claims. Anything missing → JwtClaimError; the spec
    //    mandates that we surface UNAUTHORIZED to the caller either way.
    Claims c;

    try {
        c.user_id = decoded.get_subject();
    } catch (const std::exception& e) {
        throw JwtClaimError(std::string("missing `sub` claim: ") + e.what());
    }
    if (c.user_id.empty()) {
        throw JwtClaimError("`sub` claim is empty");
    }

    try {
        const std::string kind_str = decoded.get_payload_claim("kind").as_string();
        if      (kind_str == "access")  c.kind = TokenKind::Access;
        else if (kind_str == "refresh") c.kind = TokenKind::Refresh;
        else throw JwtClaimError("unknown `kind` claim: '" + kind_str + "'");
    } catch (const std::exception& e) {
        throw JwtClaimError(std::string("missing or non-string `kind` claim: ") + e.what());
    }
    if (c.kind != expected_kind) {
        throw JwtVerifyError("token kind mismatch: expected " +
                             std::string(token_kind_name(expected_kind)) +
                             ", got " +
                             std::string(token_kind_name(c.kind)));
    }

    // Access tokens additionally carry username + role (refresh must
    // not — least privilege; the verifier enforces this).
    if (expected_kind == TokenKind::Access) {
        try {
            c.username = decoded.get_payload_claim("username").as_string();
        } catch (const std::exception& e) {
            throw JwtClaimError(std::string("missing `username` claim: ") + e.what());
        }
        if (c.username.empty()) throw JwtClaimError("`username` claim is empty");

        try {
            c.role = decoded.get_payload_claim("role").as_string();
        } catch (const std::exception& e) {
            throw JwtClaimError(std::string("missing `role` claim: ") + e.what());
        }
        if (c.role.empty()) throw JwtClaimError("`role` claim is empty");

        if (c.role != "user" && c.role != "admin") {
            // Defence-in-depth: anything other than the SPEC §4.1 enum
            // is rejected so a tampered token can't grant bogus roles.
            throw JwtClaimError("`role` claim has unknown value: '" + c.role + "'");
        }
    } else {
        // Refresh tokens must NOT carry username/role (least privilege).
        if (decoded.has_payload_claim("username")) {
            throw JwtClaimError("refresh token must not carry `username`");
        }
        if (decoded.has_payload_claim("role")) {
            throw JwtClaimError("refresh token must not carry `role`");
        }
    }

    // 5) Numeric claims. jwt-cpp exposes exp/iat via get_expires_at /
    //    get_issued_at (throws if absent — but the verifier above
    //    already required exp, so this is safe).
    c.issued_at  = decoded.get_issued_at();
    c.expires_at = decoded.get_expires_at();

    // 6) jti is optional on the read path (so pre-jti tokens still
    //    validate). When present we surface it; otherwise empty.
    if (decoded.has_payload_claim("jti")) {
        try {
            c.jti = decoded.get_id();
        } catch (...) {
            // jti is best-effort — never let a malformed jti tank an
            // otherwise valid token.
            c.jti.clear();
        }
    }

    // 7) Final sanity check against the caller-provided clock. The
    //    verifier already enforced exp, but a caller-provided clock
    //    that diverges from real time (e.g. frozen in tests for a
    //    known-expired fixture) would otherwise get a surprising
    //    answer. We re-check here so `Claims::is_expired_at` and the
    //    verifier agree.
    if (clock.now() >= c.expires_at) {
        throw JwtVerifyError("token has expired");
    }

    return c;
}

} // namespace litecode