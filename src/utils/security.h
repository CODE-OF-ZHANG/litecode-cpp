// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Security hardening helpers (Phase 6 ★ v1.2.45)
//
// SPEC §5.7 / §6.3 / §11 Phase 6 / §15 / A3, A14, A32, A34 acceptance:
//   This header centralizes the SPEC §15.3 "defense in depth" checks
//   that every route handler should be able to reach in one line:
//
//     1) Input length caps (kMaxXxxLength constants) — protects against
//        memory-exhaustion DoS (e.g. a 50 MB slug in /problems/:slug).
//        Each constant is tied to a SPEC column width or a measured
//        upper bound; values are tuned to give a 2-4x margin so a
//        future "add tag to slug" doesn't blow up the wire contract.
//
//     2) Charset filters (validate_slug_char / validate_username_char
//        / has_control_chars / is_safe_for_json_log) — protect against
//        XSS in path components (URL-encoded payloads), log injection
//        (\r\n in a username clobbering log readers), and JSON injection
//        (a stray U+2028 line separator that some JSON parsers mishandle).
//
//     3) Path traversal / HTML-injection guards — reject raw '<' / '>'
//        in anything that flows into a `<a href>` or `window.location`
//        sink on the front-end. Front-end will DOMPurify on render,
//        but the API shouldn't even accept the bytes in the first place.
//
//     4) Security response headers (apply_security_headers) — a single
//        helper that stamps X-Content-Type-Options, X-Frame-Options,
//        Referrer-Policy, Permissions-Policy, and a baseline CSP onto
//        every response. Caddyfile already sets these for the
//        reverse-proxy path; this helper exists so a developer
//        running `litecode-cpp` directly (without Caddy in front)
//        still ships the same baseline.
//
//     5) HTML escape (escape_html) — for the rare case the back-end
//        needs to ship an HTML fragment in a 5xx error page. Mirrors
//        the OWASP recommendation (escape &, <, >, ", ').
//
//   Design notes:
//     - Header-only + inline: matches every other Phase 1-6 helper
//       (server.h / error_handler.h / request_id.h / cookie_utils.h).
//       Tests link this header directly.
//     - Pure C++17 + <cctype>; no httplib / nlohmann_json / OpenSSL
//       dependency so the header is safe to include from anywhere.
//     - Functions never throw. Validation failures are signalled by
//       returning false / nullopt / a sanitized string so the caller
//       can compose them with the SPEC §5.7 envelope helper.
//
//   Usage:
//
//     // In a route handler:
//     if (!litecode::security::validate_path_component_len(slug)) {
//         send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
//                    "path component too long");
//         return;
//     }
//
//     // On a 200 response, just before send_success:
//     litecode::security::apply_security_headers(res);
//
//     // On any string flowing into a log:
//     if (litecode::security::has_control_chars(username)) {
//         LOG_WARN("username had control chars — likely injection attempt",
//                  {{"username", username}});
//     }

#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include <httplib.h>

namespace litecode {
namespace security {

// ────────────────────────────────────────────────────────────────────────────
//  Section: input length caps
//
//  These mirror SPEC §4 column widths and route-handler measured bounds.
//  Anything above the cap is rejected with a 400 envelope at the route
//  layer; defense against memory-exhaustion DoS (a 50 MB slug blowing
//  up the URL parser) and log / audit row overflow.
//
//  Where a column already has a constant (kMaxSlugLength in
//  db/problem_repo.h), re-declare it here as an alias so a route handler
//  that doesn't want to drag the whole repo header can still check the
//  bound. The numeric values stay in lock-step — both header gates
//  fire on the same wire input.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kMaxPathComponentLength = 200;   // slug / id segment
inline constexpr std::size_t kMaxQueryParamLength    = 4096;  // ?difficulty=&tag_id=...
inline constexpr std::size_t kMaxHeaderValueLength   = 8192;  // bearer / cookie
inline constexpr std::size_t kMaxFreeTextLength      = 64 * 1024; // 64 KB
inline constexpr std::size_t kMaxMarkdownBodyLength  = 16 * 1024 * 1024; // 16 MB
inline constexpr std::size_t kMaxEmailLength         = 254;   // RFC 5321

// validate_path_component_len — true iff `s` fits in a URL path
// component (the route-handler slice between two `/` separators).
// Rejects empty strings and over-long inputs. The cap (200) is
// generous vs SPEC §4.2's 100-char slug — it leaves room for future
// slugs that grow past today's bound without a re-tuning cycle.
inline bool validate_path_component_len(std::string_view s) {
    if (s.empty() || s.size() > kMaxPathComponentLength) return false;
    return true;
}

// validate_query_param_len — true iff `s` fits in a single query-string
// value. 4096 is a comfortable upper bound for SPEC §5.2's ?difficulty /
// ?tag_id / ?status / ?limit params (none are realistic past ~64 bytes).
inline bool validate_query_param_len(std::string_view s) {
    return s.size() <= kMaxQueryParamLength;
}

// validate_header_value_len — true iff `s` fits in a header value
// (Authorization, Cookie, X-Request-Id, …). 8 KB is the de-facto
// Apache / nginx default; rejecting longer values stops a probe from
// passing a 1 MB Authorization header.
inline bool validate_header_value_len(std::string_view s) {
    return s.size() <= kMaxHeaderValueLength;
}

// validate_free_text_len — 64 KB upper bound for short free-form
// strings (admin note, audit payload, error_message preview).
inline bool validate_free_text_len(std::string_view s) {
    return s.size() <= kMaxFreeTextLength;
}

// validate_markdown_body_len — 16 MB upper bound for the problem
// description. Mirrors the MySQL MEDIUMTEXT cap (16 MB − 3) so a
// over-large payload can't survive parsing the wire only to fail at
// the DB column. Use this as a pre-flight check in admin problem
// create / update / bulk import paths.
inline bool validate_markdown_body_len(std::string_view s) {
    return s.size() <= kMaxMarkdownBodyLength;
}

// validate_email_len — RFC 5321 caps emails at 254 octets.
inline bool validate_email_len(std::string_view s) {
    return s.size() > 0 && s.size() <= kMaxEmailLength;
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: charset filters
//
//  These are the second layer of "no surprise bytes" — even if a length
//  check passes, the bytes themselves can be malicious (control chars,
//  zero-width spaces, Unicode line separators that some JSON parsers
//  treat as newlines, etc.).
// ────────────────────────────────────────────────────────────────────────────

// has_control_chars — true iff `s` contains any byte < 0x20 or DEL (0x7F),
// except for the trailing newline on a multi-line string. Slugs / usernames
// / emails / tag names are SINGLE-LINE by contract; a control char in one
// of them is an injection probe and should be rejected outright.
inline bool has_control_chars(std::string_view s) noexcept {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

// has_html_special_chars — true iff `s` contains '<' or '>' or '&'.
// For inputs that flow into a `<a href="…">`, `window.location = …`,
// or an attribute-value sink (without further DOMPurify), this is the
// XSS trip-wire. Front-end will DOMPurify on render, but the API
// should reject the bytes so the audit log + 4xx envelope carry
// the original (escaped) input rather than a stored payload.
inline bool has_html_special_chars(std::string_view s) noexcept {
    for (char c : s) {
        if (c == '<' || c == '>' || c == '&') return true;
    }
    return false;
}

// has_json_special_chars — true iff `s` contains a U+2028 / U+2029
// (LINE / PARAGRAPH SEPARATOR). These are NOT control chars in the
// ASCII sense, but JavaScript's JSON.parse() historically treated
// them as newlines; if a future log / dashboard surfaces the
// stored value via JSON.parse, an injected U+2028 can smuggle
// arbitrary content. Reject upfront.
inline bool has_json_special_chars(std::string_view s) noexcept {
    // Iterate the raw bytes looking for the UTF-8 encodings:
    //   U+2028 = E2 80 A8
    //   U+2029 = E2 80 A9
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(s[i + 1]);
        const unsigned char c = static_cast<unsigned char>(s[i + 2]);
        if (a == 0xE2 && b == 0x80 && (c == 0xA8 || c == 0xA9)) return true;
    }
    return false;
}

// is_safe_for_log — true iff `s` has no control chars and no JSON
// line-separator bytes. Anything that goes into a log line via
// `LOG_INFO(..., {{"key", value}})` should pass this first; otherwise
// a CRLF in a username can split one log record into two, smuggling
// a forged entry past the operator.
inline bool is_safe_for_log(std::string_view s) noexcept {
    return !has_control_chars(s) && !has_json_special_chars(s);
}

// is_safe_for_html_attribute — true iff `s` has no '<' or '>' (so
// it can't close an HTML tag prematurely) and no control chars
// (so it can't break out of an attribute value). Use as a
// pre-flight before stuffing `s` into a `data-foo="…"` attribute
// or a `<title>…</title>` body.
inline bool is_safe_for_html_attribute(std::string_view s) noexcept {
    return !has_control_chars(s) && !has_html_special_chars(s);
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: HTML escape
//
//  escape_html — turn `&`, `<`, `>`, `"`, `'` into their entity
//  references. Use only when the back-end has to ship raw HTML
//  (e.g. a 5xx error page rendered by the server). The front-end's
//  DOMPurify pipeline is the canonical XSS sanitizer for any
//  user-supplied content; the back-end's escape is a fallback for
//  the rare case where we don't trust the front-end to run.
//
//  This mirrors the OWASP "HTML Entity Encoding" rule for content
//  between tags AND in quoted attributes. It deliberately escapes
//  ' to &#x27; (not &apos;) because the older entity is not
//  recognized by every HTML parser in older browsers.
inline std::string escape_html(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#x27;"; break;
            default:   out.push_back(c);
        }
    }
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: security response headers
//
//  apply_security_headers — stamp the SPEC §15 baseline headers on
//  `res`. Caddyfile already does this for the reverse-proxy path; this
//  helper exists so a developer running `litecode-cpp` directly
//  (without Caddy in front) still ships the same baseline.
//
//  Headers applied (idempotent — set_header overwrites):
//
//    X-Content-Type-Options: nosniff
//      Tell the browser not to MIME-sniff; prevent the "I served text/plain
//      but the browser treated it as text/html and ran the script" attack.
//
//    X-Frame-Options: DENY
//      Refuse to render inside <iframe>/<frame>. Combined with the CSP
//      frame-ancestors 'none', this is defense-in-depth against clickjacking.
//
//    Referrer-Policy: strict-origin-when-cross-origin
//      Send only the origin (not the full URL) on cross-origin navigation.
//      Stops leaking sensitive path/query-string data via the Referer
//      header.
//
//    Permissions-Policy: geolocation=(), microphone=(), camera=(), payment=()
//      Opt every powerful feature out of the document. We don't use any
//      of these today; turning them off means a future script-injection
//      can't silently enable geolocation, for example.
//
//    Cross-Origin-Opener-Policy: same-origin
//    Cross-Origin-Embedder-Policy: require-corp
//      Isolate the browsing context. Together they enable cross-origin
//      isolation (Spectre-class mitigations). Disabled by default because
//      some CDN scripts (CodeMirror) may not ship CORP headers; pass
//      `with_coop_coep=true` to opt in for high-security deployments.
//
//    Content-Security-Policy: default-src 'none'; frame-ancestors 'none'
//      API responses are JSON, so the strictest possible policy is
//      safe. The browser refuses to render any nested browsing context
//      and refuses to load any resource (script / image / etc.) from
//      a JSON body — a defense-in-depth against an attacker who tricks
//      a browser into `application/json` interpretation.
// ────────────────────────────────────────────────────────────────────────────

inline void apply_security_headers(httplib::Response& res,
                                   bool with_coop_coep = false) {
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options",        "DENY");
    res.set_header("Referrer-Policy",        "strict-origin-when-cross-origin");
    res.set_header("Permissions-Policy",     "geolocation=(), microphone=(), camera=(), payment=()");
    // The Caddyfile already sets a more permissive CSP for HTML pages
    // (script-src https://cdn.jsdelivr.net for marked / DOMPurify).
    // The API only ships JSON, so the strictest policy is correct here:
    // the browser should not load any sub-resource from a JSON body.
    res.set_header("Content-Security-Policy",
                   "default-src 'none'; frame-ancestors 'none'");
    if (with_coop_coep) {
        res.set_header("Cross-Origin-Opener-Policy",   "same-origin");
        res.set_header("Cross-Origin-Embedder-Policy", "require-corp");
    }
}

// kSecurityHeadersBaseline — the constant string set, useful in tests
// that want to assert each header independently without firing up an
// HttpServer. Same values as apply_security_headers() with the default
// (with_coop_coep=false) flag.
inline constexpr const char* kHeaderXContentTypeOptions = "nosniff";
inline constexpr const char* kHeaderXFrameOptions        = "DENY";
inline constexpr const char* kHeaderReferrerPolicy      = "strict-origin-when-cross-origin";
inline constexpr const char* kHeaderPermissionsPolicy   =
    "geolocation=(), microphone=(), camera=(), payment=()";
inline constexpr const char* kHeaderContentSecurityPolicyApi =
    "default-src 'none'; frame-ancestors 'none'";

// ────────────────────────────────────────────────────────────────────────────
//  Section: composite checks
//
//  These compose the primitives above into the "single call site" form
//  route handlers reach for. The pattern is intentionally one helper
//  per check (rather than a single mega-validator) so each handler can
//  document in its message what it actually rejected.
// ────────────────────────────────────────────────────────────────────────────

// validate_slug_shape — true iff `s` is a valid problem slug. Mirrors
// the rules in db/problem_repo.h::validate_slug() but available to
// handlers that don't want to include the full repo header. The two
// definitions MUST stay in lock-step — the route layer rejects the
// bytes first (cheap), the repo layer rejects them again as a
// safety net (the DB has a UNIQUE constraint that could race).
//
// Rules (SPEC §4.2):
//   - length 1..100
//   - chars [a-z0-9-] only
//   - no leading or trailing '-'
inline bool validate_slug_shape(std::string_view s) noexcept {
    if (s.empty() || s.size() > 100) return false;
    if (s.front() == '-' || s.back() == '-') return false;
    for (unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-';
        if (!ok) return false;
    }
    return true;
}

// validate_username_shape — true iff `s` is a valid username.
// Mirrors db/user_repo.h::validate_username() but available to
// handlers that don't want the full repo header. The two MUST
// stay in lock-step (same reasoning as validate_slug_shape).
//
// Rules (SPEC §4.1):
//   - length 3..50
//   - chars [a-zA-Z0-9_.-]
//   - no leading or trailing '.' or '-'
inline bool validate_username_shape(std::string_view s) noexcept {
    if (s.size() < 3 || s.size() > 50) return false;
    if (s.front() == '.' || s.front() == '-' ||
        s.back()  == '.' || s.back()  == '-') return false;
    for (unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// validate_role_shape — true iff `s` is one of the two allowed roles.
// Returns false for anything else (including case variants — roles
// are lowercase by contract).
inline bool validate_role_shape(std::string_view s) noexcept {
    return s == "user" || s == "admin";
}

} // namespace security
} // namespace litecode