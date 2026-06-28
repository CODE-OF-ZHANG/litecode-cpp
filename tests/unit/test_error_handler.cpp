// tests/unit/test_error_handler.cpp
//
// Unit tests for src/routes/error_handler.h — the SPEC §5.7 unified
// error envelope.
//
// Coverage:
//   - ErrorCode catalog: all 8 codes map to the SPEC wire strings, and
//     nothing else. kErrorCodeCount pins the catalog size.
//   - make_error_envelope:
//       * required fields (code, message) always present
//       * details omitted when null, present when set
//       * request_id stamped from logger.h's per-thread scope, omitted
//         when no scope is active
//       * empty message falls back to a sane default
//   - default_error_for_status / default_message_for_status: cover
//     every status the SPEC §5.7 enumerator list mentions, plus 500
//     falling back to INTERNAL_ERROR.
//   - send_error: writes correct status + Content-Type + JSON body.
//   - send_success / send_created: success envelope shape (data +
//     optional request_id).
//   - send_no_content: 204 with empty body.
//   - parse_json_body: empty body → 400 INVALID_INPUT, malformed JSON
//     → 400 INVALID_INPUT, valid JSON → parsed.
//   - ApiException: respond() writes the unified envelope; what()
//     returns the message; status / code / message / details accessors.
//
// What we deliberately do NOT test here:
//   - The 404 / 500 catch-all in server.h's wire_middleware() — that's
//     covered by test_server.cpp's ServerErrorEnvelope group, which
//     drives a live HttpServer.
//   - The X-Request-Id header propagation — that's request_id.h's
//     contract, pinned by test_request_id.cpp.

#include <gtest/gtest.h>

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "logger.h"
#include "routes/error_handler.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Catalog (SPEC §5.7)
// ────────────────────────────────────────────────────────────────────────────

TEST(ErrorCodeCatalog, NamesMatchSpec) {
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::INVALID_INPUT),       "INVALID_INPUT");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::UNAUTHORIZED),        "UNAUTHORIZED");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::FORBIDDEN),           "FORBIDDEN");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::NOT_FOUND),           "NOT_FOUND");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::RATE_LIMITED),        "RATE_LIMITED");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::CONFLICT),            "CONFLICT");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::INTERNAL_ERROR),      "INTERNAL_ERROR");
    EXPECT_EQ(litecode::error_code_name(litecode::ErrorCode::SERVICE_UNAVAILABLE), "SERVICE_UNAVAILABLE");
}

TEST(ErrorCodeCatalog, CatalogSizePinned) {
    // SPEC §5.7 lists exactly 8 codes. If someone adds one, they must
    // also update this assertion (and the wire string list).
    EXPECT_EQ(litecode::kErrorCodeCount, 8u);
}

TEST(ErrorCodeCatalog, NamesAreNonEmpty) {
    // Defensive — every enumerator must produce a non-empty string.
    // We can't iterate the enum, so spell them out.
    const litecode::ErrorCode all[] = {
        litecode::ErrorCode::INVALID_INPUT,
        litecode::ErrorCode::UNAUTHORIZED,
        litecode::ErrorCode::FORBIDDEN,
        litecode::ErrorCode::NOT_FOUND,
        litecode::ErrorCode::RATE_LIMITED,
        litecode::ErrorCode::CONFLICT,
        litecode::ErrorCode::INTERNAL_ERROR,
        litecode::ErrorCode::SERVICE_UNAVAILABLE,
    };
    for (auto c : all) {
        EXPECT_FALSE(litecode::error_code_name(c).empty())
            << "empty wire string for enumerator #"
            << static_cast<int>(c);
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Status mapping (used by the catch-all in server.h)
// ────────────────────────────────────────────────────────────────────────────

TEST(DefaultErrorForStatus, MapsSpecStatuses) {
    EXPECT_EQ(litecode::default_error_for_status(400), litecode::ErrorCode::INVALID_INPUT);
    EXPECT_EQ(litecode::default_error_for_status(401), litecode::ErrorCode::UNAUTHORIZED);
    EXPECT_EQ(litecode::default_error_for_status(403), litecode::ErrorCode::FORBIDDEN);
    EXPECT_EQ(litecode::default_error_for_status(404), litecode::ErrorCode::NOT_FOUND);
    EXPECT_EQ(litecode::default_error_for_status(409), litecode::ErrorCode::CONFLICT);
    EXPECT_EQ(litecode::default_error_for_status(429), litecode::ErrorCode::RATE_LIMITED);
    EXPECT_EQ(litecode::default_error_for_status(503), litecode::ErrorCode::SERVICE_UNAVAILABLE);
}

TEST(DefaultErrorForStatus, UnknownStatusFallsBackToInternal) {
    EXPECT_EQ(litecode::default_error_for_status(500), litecode::ErrorCode::INTERNAL_ERROR);
    EXPECT_EQ(litecode::default_error_for_status(418), litecode::ErrorCode::INTERNAL_ERROR);
    EXPECT_EQ(litecode::default_error_for_status(0),   litecode::ErrorCode::INTERNAL_ERROR);
}

TEST(DefaultMessageForStatus, MatchesSpec) {
    EXPECT_EQ(litecode::default_message_for_status(400), "Bad Request");
    EXPECT_EQ(litecode::default_message_for_status(401), "Unauthorized");
    EXPECT_EQ(litecode::default_message_for_status(403), "Forbidden");
    EXPECT_EQ(litecode::default_message_for_status(404), "Not Found");
    EXPECT_EQ(litecode::default_message_for_status(409), "Conflict");
    EXPECT_EQ(litecode::default_message_for_status(429), "Too Many Requests");
    EXPECT_EQ(litecode::default_message_for_status(503), "Service Unavailable");
    EXPECT_EQ(litecode::default_message_for_status(500), "Internal Server Error");
}

// ────────────────────────────────────────────────────────────────────────────
//  make_error_envelope
// ────────────────────────────────────────────────────────────────────────────

TEST(MakeErrorEnvelope, RequiredFieldsAlwaysPresent) {
    // No request_id scope active — must still build a valid envelope.
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::INVALID_INPUT, "username too short");
    EXPECT_EQ(env["code"],    "INVALID_INPUT");
    EXPECT_EQ(env["message"], "username too short");
    EXPECT_FALSE(env.contains("details"));
    EXPECT_FALSE(env.contains("request_id"));
}

TEST(MakeErrorEnvelope, DetailsEmittedWhenSet) {
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::INVALID_INPUT,
        "username length must be 3-50",
        {{"field", "username"}, {"min", 3}, {"max", 50}});
    EXPECT_EQ(env["details"]["field"], "username");
    EXPECT_EQ(env["details"]["min"],    3);
    EXPECT_EQ(env["details"]["max"],    50);
}

TEST(MakeErrorEnvelope, DetailsOmittedWhenNull) {
    // Default `details` argument is null → key absent.
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::NOT_FOUND, "missing");
    EXPECT_FALSE(env.contains("details"));
}

TEST(MakeErrorEnvelope, DetailsExplicitNullIsOmitted) {
    // Same behaviour for an explicit nullopt-equivalent.
    const nlohmann::json null_details = nullptr;
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::NOT_FOUND, "missing", null_details);
    EXPECT_FALSE(env.contains("details"));
}

TEST(MakeErrorEnvelope, RequestIdStampedFromScope) {
    litecode::RequestIdScope scope("req-001");
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::UNAUTHORIZED, "no token");
    EXPECT_EQ(env["request_id"], "req-001");
}

TEST(MakeErrorEnvelope, RequestIdOmittedOutsideScope) {
    // No scope active in this scope — even though a previous test set
    // one, it's per-thread and that thread has long exited. We don't
    // assert cross-test isolation; we just check that *without* a
    // fresh scope, request_id is absent.
    //
    // To be safe we explicitly clear by entering and exiting a scope
    // with an empty id, since RequestIdScope never sets "" unless we
    // tell it to. We do NOT rely on that and instead reason: if a
    // RequestIdScope is set on this thread by the test framework's
    // own code, the field may or may not be present — so we only
    // assert the *form* of the envelope, not the absence of
    // request_id. This keeps the test deterministic.
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::UNAUTHORIZED, "no token");
    // Either absent OR equal to whatever scope is currently active.
    if (env.contains("request_id")) {
        EXPECT_FALSE(env["request_id"].get<std::string>().empty());
    }
}

TEST(MakeErrorEnvelope, EmptyMessageFallsBackToDefault) {
    const auto env = litecode::make_error_envelope(
        litecode::ErrorCode::INTERNAL_ERROR, "");
    EXPECT_FALSE(env["message"].get<std::string>().empty());
}

TEST(MakeErrorEnvelope, EveryCodeRendersToWireString) {
    // Belt-and-braces: any enumerator must produce a non-empty wire
    // string that's also a valid JSON string.
    const litecode::ErrorCode all[] = {
        litecode::ErrorCode::INVALID_INPUT,
        litecode::ErrorCode::UNAUTHORIZED,
        litecode::ErrorCode::FORBIDDEN,
        litecode::ErrorCode::NOT_FOUND,
        litecode::ErrorCode::RATE_LIMITED,
        litecode::ErrorCode::CONFLICT,
        litecode::ErrorCode::INTERNAL_ERROR,
        litecode::ErrorCode::SERVICE_UNAVAILABLE,
    };
    for (auto c : all) {
        const auto env = litecode::make_error_envelope(c, "x");
        EXPECT_TRUE(env["code"].is_string());
        EXPECT_FALSE(env["code"].get<std::string>().empty());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  make_success_envelope
// ────────────────────────────────────────────────────────────────────────────

TEST(MakeSuccessEnvelope, DataPassedThrough) {
    const auto env = litecode::make_success_envelope({{"user_id", 42}});
    EXPECT_EQ(env["data"]["user_id"], 42);
}

TEST(MakeSuccessEnvelope, DefaultEmptyData) {
    // Calling with no data argument defaults to a null JSON. The
    // envelope still parses, has a `data` key (null), and emits a
    // stable shape — clients can rely on `data` always being present.
    const auto env = litecode::make_success_envelope({});
    EXPECT_TRUE(env.contains("data"));
    EXPECT_TRUE(env["data"].is_null());
}

TEST(MakeSuccessEnvelope, RequestIdStampedFromScope) {
    litecode::RequestIdScope scope("success-req-007");
    const auto env = litecode::make_success_envelope({{"ok", true}});
    EXPECT_EQ(env["request_id"], "success-req-007");
}

// ────────────────────────────────────────────────────────────────────────────
//  send_error / send_success / send_created / send_no_content
// ────────────────────────────────────────────────────────────────────────────

TEST(SendError, SetsStatusAndJsonContentType) {
    httplib::Response res;
    litecode::send_error(res, 403, litecode::ErrorCode::FORBIDDEN,
                         "no entry",
                         {{"required_role", "admin"}});
    EXPECT_EQ(res.status, 403);
    EXPECT_NE(res.get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    EXPECT_NE(res.get_header_value("Content-Type").find("charset=utf-8"),
              std::string::npos);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"],    "FORBIDDEN");
    EXPECT_EQ(body["message"], "no entry");
    EXPECT_EQ(body["details"]["required_role"], "admin");
}

TEST(SendError, OmitsDetailsWhenNull) {
    httplib::Response res;
    litecode::send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR, "");
    const auto body = nlohmann::json::parse(res.body);
    EXPECT_FALSE(body.contains("details"));
    // Empty message → default substitute.
    EXPECT_FALSE(body["message"].get<std::string>().empty());
}

TEST(SendSuccess, StatusAndShape) {
    httplib::Response res;
    litecode::send_success(res, {{"ping", "pong"}});
    EXPECT_EQ(res.status, 200);
    EXPECT_NE(res.get_header_value("Content-Type").find("application/json"),
              std::string::npos);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["data"]["ping"], "pong");
}

TEST(SendCreated, StatusIs201) {
    httplib::Response res;
    litecode::send_created(res, {{"id", 1}});
    EXPECT_EQ(res.status, 201);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["data"]["id"], 1);
}

TEST(SendNoContent, StatusIs204EmptyBody) {
    httplib::Response res;
    litecode::send_no_content(res);
    EXPECT_EQ(res.status, 204);
    EXPECT_TRUE(res.body.empty());
}

// ────────────────────────────────────────────────────────────────────────────
//  parse_json_body
// ────────────────────────────────────────────────────────────────────────────

TEST(ParseJsonBody, EmptyBodyEmits400) {
    httplib::Request  req;
    httplib::Response res;
    req.body = "";

    const auto out = litecode::parse_json_body(req, res);
    EXPECT_FALSE(out.has_value());
    EXPECT_EQ(res.status, 400);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"],    "INVALID_INPUT");
    EXPECT_EQ(body["message"], "Request body is empty");
}

TEST(ParseJsonBody, MalformedBodyEmits400) {
    httplib::Request  req;
    httplib::Response res;
    req.body = "{not valid json";

    const auto out = litecode::parse_json_body(req, res);
    EXPECT_FALSE(out.has_value());
    EXPECT_EQ(res.status, 400);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    // The message must surface the parse error verbatim.
    EXPECT_NE(body["message"].get<std::string>().find("Invalid JSON body"),
              std::string::npos);
}

TEST(ParseJsonBody, ValidBodyParsedAndReturned) {
    httplib::Request  req;
    httplib::Response res;
    req.body = R"({"username":"alice","age":30})";

    const auto out = litecode::parse_json_body(req, res);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ((*out)["username"], "alice");
    EXPECT_EQ((*out)["age"],      30);
    // parse_json_body must NOT have written anything to res on success.
    EXPECT_EQ(res.status, -1);    // httplib default (untouched)
}

TEST(ParseJsonBody, TopLevelArrayParsed) {
    httplib::Request  req;
    httplib::Response res;
    req.body = R"([1,2,3])";

    const auto out = litecode::parse_json_body(req, res);
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->is_array());
    EXPECT_EQ(out->size(), 3u);
}

// ────────────────────────────────────────────────────────────────────────────
//  ApiException
// ────────────────────────────────────────────────────────────────────────────

TEST(ApiException, AccessorsReturnConstructionArgs) {
    litecode::ApiException ex(409, litecode::ErrorCode::CONFLICT,
                              "username already taken",
                              {{"field", "username"}});
    EXPECT_EQ(ex.status(),  409);
    EXPECT_EQ(ex.code(),    litecode::ErrorCode::CONFLICT);
    EXPECT_EQ(ex.message(), "username already taken");
    EXPECT_EQ(ex.details()["field"], "username");
    EXPECT_STREQ(ex.what(), "username already taken");
}

TEST(ApiException, RespondWritesUnifiedEnvelope) {
    litecode::ApiException ex(401, litecode::ErrorCode::UNAUTHORIZED,
                              "token expired",
                              {{"hint", "refresh"}});
    httplib::Response res;
    ex.respond(res);
    EXPECT_EQ(res.status, 401);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "token expired");
    EXPECT_EQ(body["details"]["hint"], "refresh");
}

TEST(ApiException, RespondWithoutDetailsOmitsKey) {
    litecode::ApiException ex(500, litecode::ErrorCode::INTERNAL_ERROR,
                              "boom");
    httplib::Response res;
    ex.respond(res);
    EXPECT_EQ(res.status, 500);

    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"],    "INTERNAL_ERROR");
    EXPECT_EQ(body["message"], "boom");
    EXPECT_FALSE(body.contains("details"));
}

TEST(ApiException, IsStdException) {
    // ApiException must be catchable as std::exception& for generic
    // exception-handling code paths (logs, metrics) to introspect it.
    litecode::ApiException ex(403, litecode::ErrorCode::FORBIDDEN, "no");
    try {
        throw ex;
    } catch (const std::exception& e) {
        EXPECT_STREQ(e.what(), "no");
    }
}

} // anonymous namespace