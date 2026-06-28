// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — JWT authentication middleware (Phase 2 ★)
//
// SPEC §5.1 / §11 Phase 2 / §15.1 / A3 acceptance:
//   - Pulls a Bearer token out of the `Authorization` request header.
//   - Verifies it via auth/jwt_utils.h against the configured
//     secret + issuer, expecting TokenKind::Access.
//   - Returns the parsed Claims to the route handler so it can act
//     as `user_id` / `username` / `role` without re-parsing.
//   - Translates every failure mode (no header, wrong scheme, malformed
//     token, bad signature, expired, wrong issuer, wrong kind, missing
//     claims) into the SPEC §5.7 unified error envelope:
//       401 UNAUTHORIZED  →  invalid or missing token
//     via ApiException, which server.h's per-request wrap turns into
//     the right JSON body.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (config.h / logger.h / server.h / jwt_utils.h / request_id.h).
//     The middleware has no state of its own; tests instantiate a
//     JwtConfig and call require_authentication() directly without
//     needing a live socket.
//   - We do NOT use the per-request `httplib::Server::HandlerWithResponse`
//     pre-routing hook (server.h exposes one) because:
//       1. it would have to know the route table up-front, and
//       2. handlers want the Claims they just verified, not a
//          side-channel global.
//     Instead, the middleware exports a *callable guard* the handler
//     invokes on entry:
//         void handler(const Request& req, Response& res) {
//             const auto claims = litecode::require_authentication(
//                 req, litecode::config().jwt);
//             // claims.user_id, claims.username, claims.role are valid
//             ...
//         }
//     The throw-on-failure contract pairs with server.h's
//     wrap()/wrap_resp() so the unified envelope is emitted without
//     a single line of error handling at the call site.
//   - We do NOT consult the refresh-token blacklist here. The
//     blacklist belongs to /api/v1/auth/refresh (Phase 2 ★ refresh
//     token mechanism) and only applies to kind=refresh tokens. An
//     access token that lives past its `exp` is rejected by the
//     verifier directly; revoking live access tokens is out of
//     MVP scope (SPEC §15.1).
//   - Token extraction is permissive about the scheme case
//     ("Bearer" / "bearer" / "BEARER") and the surrounding
//     whitespace; everything else is strict. The token body is
//     passed verbatim to jwt-cpp — we do not URL-decode, base64-
//     decode, or otherwise massage it (jwt-cpp's parser handles
//     the wire format).

#pragma once

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <httplib.h>

#include "../auth/jwt_utils.h"        // verify / Claims / TokenKind
#include "../config.h"               // JwtConfig
#include "../logger.h"               // LOG_WARN
#include "../routes/error_handler.h" // ApiException / ErrorCode::UNAUTHORIZED / FORBIDDEN

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Authorization scheme
// ────────────────────────────────────────────────────────────────────────────

// HTTP "Authorization" header is case-insensitive on the header name
// and on the auth-scheme token (RFC 7235 §2.1). cpp-httplib exposes
// headers via get_header_value() which is itself case-insensitive on
// the name, so we only need to canonicalize the scheme prefix.
//
// Returning a string_view (not a string) keeps the API zero-copy; the
// returned view aliases into the input buffer the caller already owns.
inline constexpr std::string_view kBearerScheme       = "Bearer";
inline constexpr std::string_view kBearerSchemeLower  = "bearer";

// case-insensitive string equality, ASCII only.
// We don't use std::equal on ranges with case_fold predicates because
// httplib/openssl are compiled with locales that occasionally disagree
// on `std::tolower(0xDF)` etc. A bounded ASCII compare sidesteps the
// locale question entirely.
inline bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

// extract_bearer_token — pull the JWT out of an Authorization header.
//
// Accepts any of:
//     "Bearer eyJ..."
//     "bearer eyJ..."
//     "BEARER    eyJ..."
//     "Bearer\teyJ..."
//
// Rejects (returns empty string_view) when:
//   - header is empty
//   - scheme is not "Bearer" (case-insensitive)
//   - the part after the scheme is empty
//   - the part after the scheme contains whitespace mid-token
//     (defense-in-depth — the wire format is three base64url
//     segments joined by '.', and a stray space would corrupt the
//     signature verification; we surface the failure as 401, not
//     let jwt-cpp throw a more obscure error)
//
// The returned view aliases into the caller's `header_value` buffer.
// No allocation.
inline std::string_view extract_bearer_token(std::string_view header_value) {
    // Skip leading whitespace (RFC 7235 allows OWS around the auth-param).
    while (!header_value.empty()
           && std::isspace(static_cast<unsigned char>(header_value.front()))) {
        header_value.remove_prefix(1);
    }
    if (header_value.empty()) return {};

    // Find end of scheme token (the next whitespace or end of string).
    const std::size_t scheme_end = header_value.find_first_of(" \t");
    const std::string_view scheme = header_value.substr(0, scheme_end);

    if (!iequals_ascii(scheme, kBearerScheme)) {
        return {};
    }

    // Skip the OWS between scheme and token.
    std::size_t pos = scheme_end;
    while (pos < header_value.size()
           && std::isspace(static_cast<unsigned char>(header_value[pos]))) {
        ++pos;
    }
    if (pos >= header_value.size()) return {};

    // The token itself — until the next whitespace, if any.
    const std::size_t token_end = header_value.find_first_of(" \t", pos);
    const std::string_view token = header_value.substr(
        pos, token_end == std::string_view::npos ? std::string_view::npos
                                                  : token_end - pos);

    if (token.empty()) return {};

    // Reject embedded whitespace inside the token (defensive — a
    // well-formed JWT never contains spaces).
    if (token.find_first_of(" \t\r\n") != std::string_view::npos) {
        return {};
    }

    return token;
}

// extract_bearer_token convenience overload — pulls the header off an
// httplib::Request directly. Wraps extract_bearer_token() so handlers
// don't have to spell `req.get_header_value("Authorization")`.
inline std::string_view extract_bearer_token(const httplib::Request& req) {
    // CRITICAL: get_header_value() returns a std::string BY VALUE
    // (httplib.h:5679). Constructing a string_view from a temporary
    // would dangle as soon as the full expression ends — the
    // std::string is destroyed and our view points at freed memory.
    // We hold the value in a local std::string first, then alias
    // a string_view into it. The local lives for the entire function,
    // so the returned view is well-defined.
    const std::string header_value = req.get_header_value("Authorization");
    return extract_bearer_token(std::string_view(header_value));
}

// ────────────────────────────────────────────────────────────────────────────
//  require_authentication — the main guard
//
//  Verifies the request's Bearer token and returns the parsed Claims.
//  Throws ApiException(401, UNAUTHORIZED) on any failure mode so the
//  unified envelope is emitted by server.h's per-request wrap.
//
//  Failure mode  →  status 401 + ErrorCode::UNAUTHORIZED + a message
//  ----------------------------------------------------------------
//  no Authorization header                  "missing Authorization header"
//  scheme ≠ Bearer                          "Authorization scheme must be Bearer"
//  empty / whitespace-only token            "empty bearer token"
//  malformed JWT / bad signature / expired  "invalid or expired token"
//  wrong issuer / wrong kind                "invalid or expired token"
//  missing required claim (sub/username/role) "invalid or expired token"
//
//  The detailed reason is logged at WARN (with the request_id from the
//  active RequestIdScope) but NOT echoed in the response body — leaking
//  "wrong signature" vs "wrong issuer" is a small but real user-
//  enumeration / probing oracle (SPEC §15.1). All authentication
//  failures present the same "invalid or expired token" wall to the
//  client.
//
//  Returns a Claims by value. The user_id/username/role strings inside
//  are copies — handlers are free to use them across awaits/threads
//  without worrying about lifetime.
// ────────────────────────────────────────────────────────────────────────────

inline Claims require_authentication(const httplib::Request& req,
                                     const JwtConfig&        jwt_cfg) {
    // CRITICAL: get_header_value() returns a std::string BY VALUE.
    // We must hold the value in a local std::string first, then alias
    // a string_view into it. A direct string_view-from-temporary
    // construction dangles as soon as the expression ends.
    const std::string header_storage = req.get_header_value("Authorization");
    const std::string_view header = std::string_view(header_storage);

    if (header.empty()) {
        LOG_WARN("auth: missing Authorization header",
                 {{"path", req.path}, {"method", req.method}});
        throw ApiException(401, ErrorCode::UNAUTHORIZED,
                           "missing Authorization header");
    }

    // Pre-compute "did the caller at least say Bearer?" so the
    // 401 message is actionable (the front-end uses it to fix its
    // bug) without leaking verifier internals.
    auto looks_like_bearer = [&]() -> bool {
        std::string_view h = header;
        while (!h.empty()
               && std::isspace(static_cast<unsigned char>(h.front()))) {
            h.remove_prefix(1);
        }
        const std::size_t end = h.find_first_of(" \t");
        return iequals_ascii(h.substr(0, end), kBearerScheme);
    };

    const std::string_view token = extract_bearer_token(header);
    if (token.empty()) {
        const bool has_bearer_scheme = looks_like_bearer();
        LOG_WARN("auth: malformed Authorization header",
                 {{"path",             req.path},
                  {"method",           req.method},
                  {"has_bearer_scheme", has_bearer_scheme ? "true" : "false"}});
        throw ApiException(401, ErrorCode::UNAUTHORIZED,
                           has_bearer_scheme
                               ? "empty bearer token"
                               : "Authorization scheme must be Bearer");
    }

    // Verify the token. We deliberately do NOT forward jwt_utils's
    // exception message to the client (user-enumeration defense;
    // see header comment). Operators get the detail in logs; clients
    // get a single wall.
    try {
        return verify(std::string(token),
                      jwt_cfg.secret,
                      jwt_cfg.issuer,
                      TokenKind::Access);
    } catch (const JwtError& e) {
        LOG_WARN("auth: token verification failed",
                 {{"path",   req.path},
                  {"method", req.method},
                  {"reason", e.what()}});
        throw ApiException(401, ErrorCode::UNAUTHORIZED,
                           "invalid or expired token");
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  require_role — role check
//
//  Takes an already-verified Claims and refuses anything other than
//  the expected role. SPEC §4.1 enumerates exactly two roles: "user"
//  and "admin"; jwt_utils already rejects unknown role values at
//  verify() time, so a Claims reaching this function is guaranteed
//  to have role ∈ {user, admin}.
//
//  Throws ApiException(403, FORBIDDEN) on mismatch.
//
//  Lives in auth_middleware.h (not admin_middleware.h) because
//  SPEC §5.2/§5.3 list "已登录" / "🔒 admin" as orthogonal axes —
//  future endpoints may require other roles. Putting the helper
//  here lets Phase 2 / Phase 6 add `require_role("user")`-style
//  endpoints without re-rolling the 403 plumbing.
// ────────────────────────────────────────────────────────────────────────────

inline void require_role(const Claims& claims, std::string_view expected_role) {
    if (claims.role != expected_role) {
        LOG_WARN("auth: role check failed",
                 {{"user_id",  claims.user_id},
                  {"role",     claims.role},
                  {"expected", std::string(expected_role)}});
        throw ApiException(403, ErrorCode::FORBIDDEN,
                           "insufficient privileges");
    }
}

} // namespace litecode
