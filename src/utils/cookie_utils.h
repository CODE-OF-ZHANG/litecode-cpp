// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Cookie helpers (Phase 5 ★)
//
// SPEC §6.3 / §15.1 / §15.3 token storage.
//
// The refresh token is delivered to the browser as an HttpOnly cookie
// so document-side JavaScript (and any injected <script> via XSS) can
// never read it; only the Web server's /api/v1/auth/refresh endpoint
// sees it via the Cookie request header. The access token, in
// contrast, stays in JavaScript memory only — short-lived (default 2h)
// and tolerable to lose on tab close.
//
// This header provides the THREE primitives the auth routes need:
//
//   1. build_set_cookie_header(cfg, value, max_age_seconds)
//        Returns the wire form of a Set-Cookie header value — the
//        cookie name, the value, Max-Age, Path, HttpOnly, Secure,
//        SameSite. NOT including the "Set-Cookie:" prefix; callers
//        do `res.set_header("Set-Cookie", value)`.
//
//   2. parse_cookie_header(req)
//        Parses the incoming Cookie request header into a flat
//        vector of (name, value) pairs. Tolerant of:
//          - leading/trailing whitespace
//          - empty / absent header (returns empty vector)
//          - the cookie name spelled in any case (we case-fold on
//            comparison so HttpOnly lower-case isn't required)
//        Deliberately NOT case-folding NAMES — RFC 6265 §4.1.1 says
//        the browser sends the name verbatim from Set-Cookie. We
//        just trim leading/trailing whitespace and split on ";".
//        Quoted values are unquoted; backslash escapes ("\\" /
//        "\"") are left intact since the only consumer (refresh
//        token) never quotes or escapes in our codepath.
//
//   3. get_cookie_value(req, name)
//        Convenience wrapper around (2) that returns the FIRST
//        matching value or empty-string.
//
// Design notes:
//
//   - Header-only + inline: matches every other Phase 1/2/5 module
//     in the project (utils/uuid.h, auth/jwt_utils.h, etc.). Tests
//     instantiate the helpers directly without a separate library.
//
//   - We deliberately do NOT use cpp-httplib's set_header overload
//     that takes a (key,val) pair because httplib 0.18.3 doesn't
//     expose a typed Cookie API. Setting `Set-Cookie` as a generic
//     header is well-supported (httplib writes it as-is), and the
//     browser parses the value correctly as long as the value is
//     a single Set-Cookie string per call.
//
//   - Max-Age vs Expires: we emit Max-Age only (seconds-from-now).
//     Max-Age is preferred per RFC 6265 §5.2.2 because it's
//     wall-clock-independent; the browser computes the absolute
//     expiry locally. Expires would require us to emit a fixed
//     timestamp that drifts if the server's clock drifts.
//
//   - URL-encoding: refresh tokens are JWTs which are base64url-
//     encoded by jwt-cpp, so the value never contains spaces,
//     semicolons, commas, equals signs, or quotes. We DO NOT
//     percent-encode the value because (a) the JWT alphabet
//     doesn't need it and (b) percent-encoding the dots inside
//     a JWT would break the standard tooling on the consumer side.
//     If a future token format gains characters outside the
//     cookie-octet range, add percent-encoding HERE — never on
//     the consumer side.
//
//   - Multi-cookie support: when both access and refresh were
//     stored as cookies, we'd want a way to set them in one
//     response. We DON'T do that — the access token stays in
//     JavaScript memory, so only the refresh cookie needs
//     Set-Cookie. /logout sets a second Set-Cookie with Max-Age=0
//     to delete it; httplib concatenates successive `set_header`
//     calls with the same key into multiple headers, which is
//     exactly what browsers want for cookie management.
//
// Usage (from auth_routes.h):
//
//   // Issue a refresh cookie on login/register.
//   res.set_header("Set-Cookie",
//       build_set_cookie_header(cfg.cookie, tokens.refresh_token,
//                               cfg.jwt.refresh_ttl_seconds));
//
//   // Read the cookie (or fall back to the body for dev/test).
//   const std::string cookie_refresh =
//       get_cookie_value(req, cfg.cookie.name);
//   auto body_refresh = parse_refresh_request(j, res);  // 400 if shape bad
//   const std::string presented = !cookie_refresh.empty()
//       ? cookie_refresh
//       : (cfg.cookie.allow_body_fallback && body_refresh
//              ? body_refresh->refresh_token
//              : "");
//
//   // Clear on logout.
//   res.set_header("Set-Cookie",
//       build_clear_cookie_header(cfg.cookie));

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../config.h"             // CookieConfig

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Cookie-pair return type — see parse_cookie_header().
// ────────────────────────────────────────────────────────────────────────────

using CookiePair = std::pair<std::string, std::string>;

// ────────────────────────────────────────────────────────────────────────────
//  build_set_cookie_header — produce the value part of a Set-Cookie header.
//
//  Example output (Secure + HttpOnly + SameSite=Strict + Path=/api/v1/auth):
//    lc_refresh=eyJhbGc...; Max-Age=604800; Path=/api/v1/auth; HttpOnly;
//    Secure; SameSite=Strict
//
//  Note: the value goes in VERBATIM. The Set-Cookie value MUST NOT be
//  double-encoded — the browser only understands the RFC 6265 wire form.
// ────────────────────────────────────────────────────────────────────────────

inline std::string build_set_cookie_header(const CookieConfig& cfg,
                                           std::string_view value,
                                           int max_age_seconds) {
    std::string out;
    out.reserve(cfg.name.size() + value.size() + 96);
    out.append(cfg.name);
    out.push_back('=');
    out.append(value);

    // Max-Age: prefer the explicit override, fall back to the configured
    // max_age, then to zero (which means session cookie).
    int max_age = max_age_seconds > 0 ? max_age_seconds : cfg.max_age_seconds;
    if (max_age > 0) {
        out.append("; Max-Age=");
        out.append(std::to_string(max_age));
    }

    // Path: required for the cookie to be sent back on refresh; we
    // restrict to /api/v1/auth so the cookie never leaves the auth
    // surface (less attack surface than "/").
    if (!cfg.path.empty()) {
        out.append("; Path=");
        out.append(cfg.path);
    }

    if (cfg.http_only) out.append("; HttpOnly");
    if (cfg.secure)    out.append("; Secure");
    if (!cfg.same_site.empty()) {
        out.append("; SameSite=");
        out.append(cfg.same_site);
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  build_clear_cookie_header — produce a Set-Cookie that asks the browser
//  to delete the cookie. RFC 6265 §4.1.1 says you clear a cookie by
//  sending it again with Max-Age=0 (or an Expires in the past) and the
//  same Path / Domain that issued it. SameSite isn't required for the
//  clear because the browser only cares about the name + path match.
//
//  We mirror the issuance attributes exactly except: no Max-Age (we set
//  Max-Age=0 instead), keep Path (mandatory for delete to take effect),
//  drop HttpOnly/Secure/SameSite because they're irrelevant once the
//  cookie is gone. The browser still removes the cookie if it matches
//  by name + path, regardless of the other attributes — keeping them
//  identical to the issuance set just makes the intent obvious in logs.
// ────────────────────────────────────────────────────────────────────────────

inline std::string build_clear_cookie_header(const CookieConfig& cfg) {
    std::string out;
    out.reserve(cfg.name.size() + 32);
    out.append(cfg.name);
    out.append("=; Max-Age=0");
    if (!cfg.path.empty()) {
        out.append("; Path=");
        out.append(cfg.path);
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  parse_cookie_header — split the incoming "Cookie: a=b; c=d; e=f"
//  header into a vector of (name, value) pairs.
//
//  Rules implemented (RFC 6265 §5.2 + §5.4):
//    - Trim leading/trailing whitespace from the header
//    - Split on ';' (NOT on ','; commas separate cookies in Set-Cookie
//      but in Cookie they're just whitespace equivalents)
//    - Each chunk is split on the FIRST '=' (avoids breaking values
//      that contain '=' — e.g. base64 padding)
//    - Names and values have leading/trailing OWS stripped
//    - If a value is wrapped in DQUOTE, the surrounding quotes are
//      stripped. Backslash escapes inside quoted values are NOT
//      processed (we're not implementing full RFC 6265 quoted-string;
//      refresh tokens are never quoted by the browser anyway).
//    - Empty names are dropped
//
//  Case sensitivity: RFC 6265 §4.1.1 says cookie names are
//  case-sensitive. We preserve the name verbatim — callers compare
//  with cfg.cookie.name verbatim too (set by us).
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// OWS = optional whitespace, RFC 7230 §3.2.3.
inline void trim_ows(std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    if (a == 0 && b == s.size()) return;
    s = s.substr(a, b - a);
}

} // namespace detail

inline std::vector<CookiePair> parse_cookie_header(std::string_view header) {
    std::vector<CookiePair> out;
    if (header.empty()) return out;
    out.reserve(4);

    std::size_t i = 0;
    const std::size_t n = header.size();
    while (i < n) {
        // Skip whitespace between cookies.
        while (i < n && (header[i] == ' ' || header[i] == '\t' ||
                         header[i] == ';' || header[i] == ',')) {
            ++i;
        }
        if (i >= n) break;

        // Find the end of this name=value chunk.
        std::size_t j = i;
        while (j < n && header[j] != ';') ++j;

        // Split into name and value at the first '='.
        std::string chunk(header.substr(i, j - i));
        std::string name;
        std::string value;

        const auto eq = chunk.find('=');
        if (eq == std::string::npos) {
            // No '=', treat the whole chunk as a name with empty value.
            // RFC 6265 §5.2 step 3 says ignore such cookies.
            i = j + 1;
            continue;
        }
        name  = chunk.substr(0, eq);
        value = chunk.substr(eq + 1);

        detail::trim_ows(name);
        detail::trim_ows(value);

        if (name.empty()) {
            i = j + 1;
            continue;
        }

        // Strip DQUOTE pair if both ends are quoted.
        if (value.size() >= 2 &&
            value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        out.emplace_back(std::move(name), std::move(value));
        i = j + 1;
    }

    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  get_cookie_value — first match (case-sensitive) by name, or empty.
//
//  Most browsers only send one cookie per name anyway (later Set-Cookie
//  overrides), so "first match" is the same as "the only one".
// ────────────────────────────────────────────────────────────────────────────

// Free-standing version — same logic but takes the header string directly.
// This avoids forcing cookie_utils.h to know about httplib::Request; callers
// extract the header via `req.get_header_value("Cookie")` themselves.
inline std::string get_cookie_value(std::string_view header,
                                    std::string_view name) {
    if (header.empty() || name.empty()) return std::string();
    const auto pairs = parse_cookie_header(header);
    for (const auto& p : pairs) {
        if (p.first == name) return p.second;
    }
    return std::string();
}

} // namespace litecode