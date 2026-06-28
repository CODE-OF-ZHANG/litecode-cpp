// tests/unit/test_request_id.cpp
//
// Unit tests for src/middleware/request_id.h — the request-id middleware
// primitives that src/server.h wires into the pre-routing hook.
//
// End-to-end coverage (full HttpServer round-trip) lives in
// test_server.cpp's ServerRequestId.* group. This file exercises the
// standalone helpers so future Phase 2 middleware (rate-limit / auth)
// can reuse them without needing a live socket.
//
// Coverage:
//   - is_valid_request_id: accepts UUID v4 / alnum / .-_, rejects CRLF,
//     spaces, colons, empty, oversize
//   - resolve_request_id: echoes valid input, generates UUID v4 on
//     missing/empty/garbage
//   - apply_request_id_header: writes the resolved id to the response
//     and returns it (so the caller can stash it into RequestIdScope)
//   - kMaxRequestIdLength constant / request_id_header_name() function
//     match SPEC §5.7 ("X-Request-Id", 128)

#include <gtest/gtest.h>

#include <httplib.h>
#include <string>

#include "middleware/request_id.h"
#include "utils/uuid.h"   // for shape assertions

namespace {

void expect_uuid_v4_shape(const std::string& id) {
    ASSERT_EQ(id.size(), 36u);
    EXPECT_EQ(id[8],  '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');
    EXPECT_EQ(id[14], '4');
    EXPECT_TRUE(id[19] == '8' || id[19] == '9'
             || id[19] == 'a' || id[19] == 'b');
}

// ────────────────────────────────────────────────────────────────────────────
//  is_valid_request_id
// ────────────────────────────────────────────────────────────────────────────

TEST(RequestIdValidator, AcceptsUuidV4) {
    const auto id = litecode::generate_uuid_v4();
    EXPECT_TRUE(litecode::is_valid_request_id(id));
}

TEST(RequestIdValidator, AcceptsAlnumAndSafePunct) {
    EXPECT_TRUE(litecode::is_valid_request_id("abc-123"));
    EXPECT_TRUE(litecode::is_valid_request_id("ABC.123"));
    EXPECT_TRUE(litecode::is_valid_request_id("a_b-c.d"));
    EXPECT_TRUE(litecode::is_valid_request_id("0123456789"));
    EXPECT_TRUE(litecode::is_valid_request_id("request-42"));
}

TEST(RequestIdValidator, RejectsControlChars) {
    EXPECT_FALSE(litecode::is_valid_request_id("a\nb"));   // LF
    EXPECT_FALSE(litecode::is_valid_request_id("a\rb"));   // CR
    EXPECT_FALSE(litecode::is_valid_request_id("a\tb"));   // TAB
    EXPECT_FALSE(litecode::is_valid_request_id(std::string("\0x", 2)));
}

TEST(RequestIdValidator, RejectsSpaceAndColon) {
    EXPECT_FALSE(litecode::is_valid_request_id("a b"));
    EXPECT_FALSE(litecode::is_valid_request_id("a:b"));    // header-unsafe
    EXPECT_FALSE(litecode::is_valid_request_id("a;b"));    // log-grep unsafe
}

TEST(RequestIdValidator, RejectsEmptyAndOversize) {
    EXPECT_FALSE(litecode::is_valid_request_id(""));
    EXPECT_FALSE(litecode::is_valid_request_id(std::string(litecode::kMaxRequestIdLength + 1, 'a')));
    EXPECT_FALSE(litecode::is_valid_request_id(std::string(256, 'a')));
}

TEST(RequestIdValidator, AcceptsAtMaxLength) {
    EXPECT_TRUE(litecode::is_valid_request_id(std::string(litecode::kMaxRequestIdLength, 'a')));
}

// ────────────────────────────────────────────────────────────────────────────
//  resolve_request_id
// ────────────────────────────────────────────────────────────────────────────

TEST(ResolveRequestId, EmptyReturnsUuidV4) {
    const auto rid = litecode::resolve_request_id("");
    expect_uuid_v4_shape(rid);
}

TEST(ResolveRequestId, ValidInputEchoed) {
    EXPECT_EQ(litecode::resolve_request_id("client-supplied-001"), "client-supplied-001");
    EXPECT_EQ(litecode::resolve_request_id("ABC.DEF_ghi-123"),      "ABC.DEF_ghi-123");
}

TEST(ResolveRequestId, GarbageReplacedByUuid) {
    const auto rid = litecode::resolve_request_id("evil:value\r\nX-Inject: yes");
    expect_uuid_v4_shape(rid);
}

TEST(ResolveRequestId, OversizeReplacedByUuid) {
    const auto oversize = std::string(litecode::kMaxRequestIdLength + 5, 'x');
    const auto rid = litecode::resolve_request_id(oversize);
    expect_uuid_v4_shape(rid);
}

// ────────────────────────────────────────────────────────────────────────────
//  apply_request_id_header
// ────────────────────────────────────────────────────────────────────────────

TEST(ApplyRequestIdHeader, WritesHeaderAndReturnsRid) {
    httplib::Response res;
    const auto rid = litecode::apply_request_id_header("client-9", res);
    EXPECT_EQ(rid, "client-9");
    EXPECT_EQ(res.get_header_value(litecode::request_id_header_name()), "client-9");
}

TEST(ApplyRequestIdHeader, GeneratesUuidWhenMissing) {
    httplib::Response res;
    const auto rid = litecode::apply_request_id_header("", res);
    expect_uuid_v4_shape(rid);
    EXPECT_EQ(res.get_header_value(litecode::request_id_header_name()), rid);
}

TEST(ApplyRequestIdHeader, SecondCallReturnsFreshRid) {
    // Successive apply_request_id_header() invocations on the same
    // Response must each return the rid the caller asked for. The
    // concrete wire-level "which header value the server sends" is
    // httplib's concern (it stores headers in a multimap); this test
    // pins the contract our middleware offers.
    httplib::Response res;
    const auto rid1 = litecode::apply_request_id_header("first",  res);
    const auto rid2 = litecode::apply_request_id_header("second", res);
    EXPECT_EQ(rid1, "first");
    EXPECT_EQ(rid2, "second");
}

TEST(ApplyRequestIdHeader, HeaderNameMatchesSpec) {
    // SPEC §5.7 / §16.6 explicitly name "X-Request-Id". Pin it down so
    // a future rename shows up as a test failure.
    EXPECT_STREQ(litecode::request_id_header_name(), "X-Request-Id");
}

// ────────────────────────────────────────────────────────────────────────────
//  Composition: end-to-end without the HTTP server
//
//  Simulate the pre-routing hook's job — pick an id, stamp the header,
//  and confirm it survives a round-trip into the response object.
// ────────────────────────────────────────────────────────────────────────────

TEST(RequestIdMiddlewareFlow, EchoPathRoundTrip) {
    httplib::Response res;
    const std::string inbound = "01234567-89ab-4cde-9f01-23456789abcd";

    const auto rid = litecode::apply_request_id_header(inbound, res);

    EXPECT_EQ(rid, inbound);
    EXPECT_EQ(res.get_header_value(litecode::request_id_header_name()), inbound);
}

TEST(RequestIdMiddlewareFlow, FallbackPathRoundTrip) {
    httplib::Response res;
    const auto rid = litecode::apply_request_id_header("", res);
    expect_uuid_v4_shape(rid);
    // Two consecutive calls must produce DIFFERENT ids (otherwise we
    // accidentally hard-coded a constant in resolve_request_id).
    httplib::Response res2;
    const auto rid2 = litecode::apply_request_id_header("", res2);
    expect_uuid_v4_shape(rid2);
    EXPECT_NE(rid, rid2);
}

} // anonymous namespace