// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Request-ID middleware
//
// SPEC §10 src/middleware/request_id.h / §11 Phase 1 ★
// SPEC §5.7 global conventions / §15.6 logging / §16.6 logging policy
//
// This middleware does the two things SPEC §5.7 mandates:
//
//   1. Attach an X-Request-Id to every HTTP response.
//      - Client sent a valid X-Request-Id → echo it back so the caller
//        can correlate across services
//      - Client sent nothing / sent garbage → server falls back to UUID v4
//
//   2. Stash the same id in the per-request log scope (RequestIdScope,
//      in src/logger.h) so the access log line and every LOG_INFO /
//      LOG_ERROR emitted inside the handler share one grep-able token.
//
// Design notes:
//   - Single header, zero extra dependencies (UUID comes from
//     utils/uuid.h)
//   - server.h's pre-routing hook + per-handler wrap are just thin
//     wrappers around apply_request_id_header() here, so future Phase 2
//     middleware (rate-limit / auth) can reuse the same resolution
//     policy without copy-pasting
//   - is_valid_request_id() only accepts [A-Za-z0-9_.-]: CRLF, colons
//     and other characters that would pollute HTTP headers or log grep
//     output are rejected outright (defense against log injection)

#pragma once

#include <string>
#include <string_view>

#include "../utils/uuid.h"   // generate_uuid_v4()

namespace litecode {

// Header constant: SPEC §5.7 / §16.6 specifies "X-Request-Id" for
// distributed tracing. Returned as a constexpr const char* function
// instead of being declared as `inline const char* const` to sidestep
// an ODR-resolution issue seen on some MSVC versions with pointer-
// literal-type header constants.
inline constexpr const char* request_id_header_name() {
    return "X-Request-Id";
}

// Request-ID upper bound: refuse anything longer to keep a malicious
// client from sending a 1MB id and blowing up our response header /
// log file. 128 leaves room for a UUID v4 (36) plus a meaningful
// prefix.
inline constexpr std::size_t kMaxRequestIdLength = 128;

// is_valid_request_id — check whether a client-supplied X-Request-Id
// is safe to echo back.
//
// Rules:
//   - Non-empty, length <= 128
//   - Characters ∈ [A-Za-z0-9_.-]
//
// Rejection reasons:
//   - Empty / overlong   → header abuse
//   - CRLF / whitespace  → header injection / log injection
//   - colon / other       → breaks HTTP header escaping, pollutes logs

inline bool is_valid_request_id(std::string_view id) {
    if (id.empty() || id.size() > kMaxRequestIdLength) return false;
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok =
            (u >= '0' && u <= '9') ||
            (u >= 'a' && u <= 'z') ||
            (u >= 'A' && u <= 'Z') ||
            u == '-' || u == '_' || u == '.';
        if (!ok) return false;
    }
    return true;
}

// resolve_request_id — pick the X-Request-Id we'll actually use.
//
// Differs from a bare generate_uuid_v4() call: this function prefers
// to reuse a valid client-supplied id and only falls back to a fresh
// UUID when the inbound value is missing or invalid.
//
// Return value: never empty. The caller can write it to a header
// without a null-check.
inline std::string resolve_request_id(std::string_view inbound) {
    if (!inbound.empty() && is_valid_request_id(inbound)) {
        return std::string(inbound);
    }
    return generate_uuid_v4();
}

// apply_request_id_header — single entry point used by server.h (and
// any future middleware that wants to stamp the header, e.g. a 429
// response from the rate limiter).
//
// Behaviour:
//   1. Pick the id (resolve_request_id)
//   2. Write it to res[request_id_header_name()]
//   3. Return the id so the caller can push it into RequestIdScope
//      for the per-thread log correlation

template <class Response>
inline std::string apply_request_id_header(std::string_view inbound, Response& res) {
    const std::string rid = resolve_request_id(inbound);
    res.set_header(request_id_header_name(), rid);
    return rid;
}

} // namespace litecode