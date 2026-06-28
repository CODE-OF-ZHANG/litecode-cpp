// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Unified error envelope (SPEC §5.7 / §11 Phase 1 ★)
//
// This is the SINGLE place every API error response is shaped.
//
// SPEC §5.7 mandates:
//   - 8 error codes: INVALID_INPUT / UNAUTHORIZED / FORBIDDEN / NOT_FOUND
//     / RATE_LIMITED / CONFLICT / INTERNAL_ERROR / SERVICE_UNAVAILABLE
//   - JSON envelope shape:
//       {
//         "code":       "INVALID_INPUT",
//         "message":    "用户名长度必须在 3-50 之间",
//         "details":    { "field": "username" },
//         "request_id": "550e8400-e29b-41d4-a716-446655440000"
//       }
//   - The same envelope appears at:
//       * every explicit handler call site (send_error(...))
//       * the 404/500 catch-all in server.h's wire_middleware()
//       * every 401/403 thrown by Phase 2 auth/admin middleware
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 module
//     (config.h / logger.h / server.h). Tests link this header
//     directly.
//   - Self-contained: does NOT include server.h. server.h re-exports
//     our symbols with a one-line `#include` + namespace block. This
//     keeps the dependency graph acyclic and lets future modules
//     (middleware/admin_middleware.h, routes/auth_routes.h, ...) use
//     the envelope without dragging in the whole HTTP framework.
//   - request_id is sourced from logger.h's per-thread
//     current_request_id() — the same token the X-Request-Id response
//     header carries. When no scope is active (e.g. inside a worker
//     thread that hasn't stamped one), the field is omitted, never
//     blank.
//   - We intentionally keep `error_handler.h` free of HTTP status
//     mapping tables outside the one `default_error_for_status()`
//     helper; that helper exists for the catch-all handler in
//     server.h only. Route handlers must pick status + code together
//     and pass both to send_error().
//   - The `ApiException` class lets handlers `throw` instead of
//     calling send_error + return. server.h's per-request wrap catches
//     the exception, calls send_error(), and never re-throws.
//     This is what Phase 2's auth_middleware will use to abort early
//     from deep in the stack (e.g. inside a JWT verifier) without
//     turning every helper into an `int` error code.
//
// Usage from a route handler:
//   void login_handler(const Request& req, Response& res) {
//       auto j = parse_json_body(req, res);
//       if (!j) return;                     // 400 already sent
//       try {
//           authenticate_user(*j);          // may throw ApiException
//       } catch (const ApiException& e) {
//           e.respond(res);                 // unified envelope
//           return;
//       }
//       send_success(res, {{"user_id", 42}});
//   }
//
// Or, with the throw-and-catch convenience:
//   throw ApiException(401, ErrorCode::UNAUTHORIZED, "token expired");

#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../logger.h"   // current_request_id() / current_request_id_present()

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  ErrorCode — the SPEC §5.7 catalog.
//
//  One enumerator per wire string. Adding a new code is a two-line edit:
//  1) add the enumerator here,
//  2) add its case in error_code_name().
//  Anything else (handlers, tests, dashboards) reads off the wire string
//  via error_code_name(), so we never silently drift.
// ────────────────────────────────────────────────────────────────────────────

enum class ErrorCode {
    INVALID_INPUT,
    UNAUTHORIZED,
    FORBIDDEN,
    NOT_FOUND,
    RATE_LIMITED,
    CONFLICT,
    INTERNAL_ERROR,
    SERVICE_UNAVAILABLE,
};

// error_code_name — stable wire string for each enumerator.
// Returned as string_view to keep the call site zero-copy; the literals
// have static storage so the view is safe everywhere.
inline std::string_view error_code_name(ErrorCode c) {
    switch (c) {
        case ErrorCode::INVALID_INPUT:       return "INVALID_INPUT";
        case ErrorCode::UNAUTHORIZED:        return "UNAUTHORIZED";
        case ErrorCode::FORBIDDEN:           return "FORBIDDEN";
        case ErrorCode::NOT_FOUND:           return "NOT_FOUND";
        case ErrorCode::RATE_LIMITED:        return "RATE_LIMITED";
        case ErrorCode::CONFLICT:            return "CONFLICT";
        case ErrorCode::INTERNAL_ERROR:      return "INTERNAL_ERROR";
        case ErrorCode::SERVICE_UNAVAILABLE: return "SERVICE_UNAVAILABLE";
    }
    // Unreachable in well-formed code; keeps -Wreturn-type happy.
    return "INTERNAL_ERROR";
}

// All error codes — useful for tests that want to assert the catalog
// size matches SPEC §5.7 and nothing has been added/removed by accident.
inline constexpr std::size_t kErrorCodeCount = 8;

// ────────────────────────────────────────────────────────────────────────────
//  status_for_error / default_error_for_status
//
//  Phase 2 / 3 route handlers must pick (status, code, message) together
//  and pass all three to send_error(). The two helpers here are for the
//  ONE remaining case where the framework has only an HTTP status code
//  to work with — the catch-all handler in server.h's wire_middleware()
//  that catches the 404 / 500 / etc. that cpp-httplib emits on its own
//  before any of our code runs.
//
//  We keep them here so the SPEC §5.7 status↔code mapping lives next to
//  the catalog itself. The handler does NOT call this for any case it
//  itself reports; it's the "we missed this one" safety net.
// ────────────────────────────────────────────────────────────────────────────

inline ErrorCode default_error_for_status(int status) {
    switch (status) {
        case 400: return ErrorCode::INVALID_INPUT;
        case 401: return ErrorCode::UNAUTHORIZED;
        case 403: return ErrorCode::FORBIDDEN;
        case 404: return ErrorCode::NOT_FOUND;
        case 409: return ErrorCode::CONFLICT;
        case 429: return ErrorCode::RATE_LIMITED;
        case 503: return ErrorCode::SERVICE_UNAVAILABLE;
        default:  return ErrorCode::INTERNAL_ERROR;
    }
}

inline std::string_view default_message_for_status(int status) {
    switch (status) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 429: return "Too Many Requests";
        case 503: return "Service Unavailable";
        default:  return "Internal Server Error";
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Envelope builders
//
//  The two builders below are the SINGLE source of truth for the wire
//  format. Every error path in the codebase must funnel through one of
//  them. Bypassing them (e.g. by writing the JSON manually) is a bug:
//  tests assert the field order / types / presence of `request_id`.
// ────────────────────────────────────────────────────────────────────────────

// make_error_envelope — builds the SPEC §5.7 error object.
//
// Field policy:
//   - code:    always present, equals error_code_name(code).
//   - message: always present; never blank if caller passed a non-empty
//              string. An empty message is a programming error and we
//              substitute the canonical default so clients always have
//              something to display.
//   - details: omitted when null. Pass a JSON object to surface extra
//              context (e.g. {"field": "username"} for validation).
//   - request_id: omitted when no per-thread scope is active. We never
//              emit an empty string — that's worse than absent because
//              some clients render "" as a broken UI token.
inline nlohmann::json make_error_envelope(ErrorCode code,
                                          std::string_view message,
                                          const nlohmann::json& details = nullptr) {
    nlohmann::json j = {
        {"code",    std::string(error_code_name(code))},
        {"message", message.empty()
                        ? std::string(default_message_for_status(
                              // We don't know the HTTP status here;
                              // fall back to 500 message for any code.
                              500))
                        : std::string(message)},
    };
    if (!details.is_null()) {
        j["details"] = details;
    }
    if (current_request_id_present()) {
        j["request_id"] = current_request_id();
    }
    return j;
}

// make_success_envelope — the symmetric success shape used by
// send_success() / send_created(). The success envelope is NOT part of
// the SPEC §5.7 contract — the spec only mandates the error shape —
// but we centralize the builder here so the two envelopes stay in
// lock-step (same Content-Type, same charset, same request_id policy).
inline nlohmann::json make_success_envelope(const nlohmann::json& data) {
    nlohmann::json body = {{"data", data}};
    if (current_request_id_present()) {
        body["request_id"] = current_request_id();
    }
    return body;
}

// ────────────────────────────────────────────────────────────────────────────
//  Response helpers — sugar on top of httplib::Response.
//
//  Free functions (instead of a Response subclass) because cpp-httplib
//  passes `Response&` by reference and a wrapper would force every
//  existing handler signature to dereference. Free functions keep the
//  void handler(const Request&, Response&) signature.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// Compact JSON serialization. We deliberately omit pretty-printing:
// the API is consumed by JS, and smaller payloads = lower P95.
inline std::string to_json_string(const nlohmann::json& v) {
    return v.dump();
}

// Shared Content-Type for every JSON response. Charset is mandatory
// per RFC 8259 §11; we always serve UTF-8 (SPEC §7.4 enforces LF + UTF-8
// on user-supplied test data).
inline constexpr const char* kJsonContentType =
    "application/json; charset=utf-8";

} // namespace detail

// send_json — write a JSON body with the canonical Content-Type.
inline void send_json(httplib::Response& res,
                      int status,
                      const nlohmann::json& body) {
    res.status = status;
    res.set_content(detail::to_json_string(body), detail::kJsonContentType);
}

// send_success — 200 + {"data": ..., "request_id": ...}.
inline void send_success(httplib::Response& res,
                         const nlohmann::json& data = {}) {
    send_json(res, 200, make_success_envelope(data));
}

// send_created — 201 + {"data": ..., "request_id": ...}.
inline void send_created(httplib::Response& res,
                         const nlohmann::json& data = {}) {
    send_json(res, 201, make_success_envelope(data));
}

// send_no_content — 204 + empty body.
inline void send_no_content(httplib::Response& res) {
    res.status = 204;
}

// send_error — the central emit point for every API error.
//
// status:  the HTTP status code (400/401/.../500). Must be set
//          deliberately by the caller — the framework never overrides it.
// code:    the SPEC §5.7 enumerator; goes into body.code.
// message: human-readable string; goes into body.message.
// details: optional extra context; goes into body.details when present.
inline void send_error(httplib::Response& res,
                       int status,
                       ErrorCode code,
                       std::string_view message,
                       const nlohmann::json& details = nullptr) {
    send_json(res, status, make_error_envelope(code, message, details));
}

// parse_json_body — parse req.body as JSON; on failure, send a 400 in
// the unified envelope and return nullopt. Handlers idiomatically do:
//
//   auto j = parse_json_body(req, res);
//   if (!j) return;                  // 400 already on the wire
//   ...use *j...
//
// We co-locate parse_json_body with send_error() because they're a
// pair: a 400 from a bad JSON body is still a SPEC §5.7 error.
inline std::optional<nlohmann::json> parse_json_body(const httplib::Request& req,
                                                     httplib::Response& res) {
    if (req.body.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "Request body is empty");
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("Invalid JSON body: ") + e.what());
        return std::nullopt;
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  ApiException — "throw and let the framework turn it into an envelope".
//
//  Use when a deep helper needs to abort a request without unwinding
//  6 stack frames by hand. server.h's per-request wrap catches the
//  exception, calls respond(res). Re-throws of non-ApiException
//  types are still allowed to propagate to cpp-httplib, which turns
//  them into a 500 (caught by our envelope).
//
//  Examples:
//
//    // Deep in a JWT verifier:
//    throw ApiException(401, ErrorCode::UNAUTHORIZED, "token expired");
//
//    // In a repo layer that already knows it failed:
//    throw ApiException(409, ErrorCode::CONFLICT,
//                       "username already taken",
//                       {{"field", "username"}});
//
//  Phase 2 (auth_middleware.h, admin_middleware.h) will use this for
//  the 401 / 403 short-circuits.
// ────────────────────────────────────────────────────────────────────────────

class ApiException : public std::exception {
public:
    ApiException(int status,
                 ErrorCode code,
                 std::string message,
                 nlohmann::json details = nullptr)
        : status_(status),
          code_(code),
          message_(std::move(message)),
          details_(std::move(details)) {}

    int                   status()  const noexcept { return status_; }
    ErrorCode             code()    const noexcept { return code_; }
    const std::string&    message() const noexcept { return message_; }
    const nlohmann::json& details() const noexcept { return details_; }

    // what() returns the message — gtest's EXPECT_THAT can match on it
    // and logs stay grep-able.
    const char* what() const noexcept override {
        return message_.c_str();
    }

    // respond — write the unified envelope onto `res` exactly once.
    // Safe to call multiple times (the second call would simply
    // overwrite the body); tests rely on the single-call contract.
    void respond(httplib::Response& res) const {
        send_error(res, status_, code_, message_, details_);
    }

private:
    int            status_;
    ErrorCode      code_;
    std::string    message_;
    nlohmann::json details_;
};

} // namespace litecode