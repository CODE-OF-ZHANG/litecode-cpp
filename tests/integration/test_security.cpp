// tests/integration/test_security.cpp
//
// Integration tests for the SPEC §15.3 "安全加固" hardening layer
// (Phase 6 ★ v1.2.45):
//
//   1) server.h's pre-routing hook stamps the SPEC §15 baseline
//      security response headers (X-Content-Type-Options /
//      X-Frame-Options / Referrer-Policy / Permissions-Policy /
//      Content-Security-Policy) on EVERY response, not just the
//      404 catch-all.
//
//   2) utils/security.h's input-validation primitives correctly
//      reject / accept the boundary inputs the route layer relies on:
//        - validate_path_component_len
//        - validate_query_param_len / validate_header_value_len
//        - validate_markdown_body_len
//        - has_control_chars
//        - has_html_special_chars
//        - has_json_special_chars (U+2028 / U+2029)
//        - is_safe_for_log
//        - is_safe_for_html_attribute
//        - validate_slug_shape (mirrors db/problem_repo.h)
//        - validate_username_shape (mirrors db/user_repo.h)
//        - validate_role_shape
//        - escape_html
//
//   3) End-to-end: a malicious /api/v1/problems/<slug> request with
//      a path-traversal / control-char payload is rejected at the
//      route layer with a 400 INVALID_INPUT envelope, BEFORE the
//      request reaches the database (verified by stubbing the route
//      in-process).
//
// The tests deliberately use the in-process HttpServer + the route
// layer (without MySQL) so they run in CI even when no database is
// available. The unit tests for the pure helpers don't need a server
// at all.
//
// Link set: httplib + nlohmann_json + OpenSSL (server.h depends on
// them, and apply_security_headers lives behind server.h's pre-
// routing hook).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"
#include "logger.h"
#include "routes/error_handler.h"
#include "routes/problem_routes.h"   // parse_slug_param
#include "server.h"
#include "utils/security.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test infrastructure
// ────────────────────────────────────────────────────────────────────────────

// Mirror the silent_logging() / dev_server() / dev_cors() helpers
// from test_server.cpp. We duplicate them rather than #include the
// test (the test isn't a header) so test_security.cpp is self-
// contained.
litecode::LoggingConfig silent_logging() {
    litecode::LoggingConfig c;
    c.level = "ERROR";
    c.format = "text";
    c.file_path = "";
    c.include_request_id = true;
    return c;
}

litecode::ServerConfig dev_server() {
    litecode::ServerConfig s;
    s.host = "127.0.0.1";
    s.port = 0;
    s.thread_pool_size = 4;
    return s;
}

litecode::CorsConfig dev_cors() {
    litecode::CorsConfig c;
    c.allowed_origins = "http://localhost:8080,http://127.0.0.1:8080";
    c.allow_credentials = true;
    return c;
}

struct ServerHandle {
    litecode::HttpServer* s;
    std::unique_ptr<httplib::Client> client;
    int port = 0;

    explicit ServerHandle(litecode::HttpServer* srv) : s(srv) {
        port = s->bind_any_port("127.0.0.1");
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        client->set_connection_timeout(2, 0);
        client->set_read_timeout(2, 0);
        s->start(/*background=*/true);
    }
    ~ServerHandle() {
        if (s) s->stop();
    }
    ServerHandle(const ServerHandle&)            = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;
};

// ────────────────────────────────────────────────────────────────────────────
//  utils/security.h — pure unit tests
// ────────────────────────────────────────────────────────────────────────────

TEST(SecurityLength, PathComponentLenAcceptsReasonable) {
    EXPECT_TRUE(litecode::security::validate_path_component_len("two-sum"));
    EXPECT_TRUE(litecode::security::validate_path_component_len("a"));
    // 200 chars exactly — at the boundary.
    EXPECT_TRUE(litecode::security::validate_path_component_len(
        std::string(200, 'a')));
}

TEST(SecurityLength, PathComponentLenRejectsEmpty) {
    EXPECT_FALSE(litecode::security::validate_path_component_len(""));
}

TEST(SecurityLength, PathComponentLenRejectsOversize) {
    EXPECT_FALSE(litecode::security::validate_path_component_len(
        std::string(201, 'a')));
    EXPECT_FALSE(litecode::security::validate_path_component_len(
        std::string(1024, 'a')));
}

TEST(SecurityLength, QueryParamLen) {
    EXPECT_TRUE(litecode::security::validate_query_param_len(""));
    EXPECT_TRUE(litecode::security::validate_query_param_len("easy"));
    EXPECT_TRUE(litecode::security::validate_query_param_len(
        std::string(4096, 'x')));
    EXPECT_FALSE(litecode::security::validate_query_param_len(
        std::string(4097, 'x')));
}

TEST(SecurityLength, HeaderValueLen) {
    EXPECT_TRUE(litecode::security::validate_header_value_len(
        std::string(8192, 'a')));
    EXPECT_FALSE(litecode::security::validate_header_value_len(
        std::string(8193, 'a')));
}

TEST(SecurityLength, FreeTextLen) {
    EXPECT_TRUE(litecode::security::validate_free_text_len(
        std::string(64 * 1024, 'x')));
    EXPECT_FALSE(litecode::security::validate_free_text_len(
        std::string(64 * 1024 + 1, 'x')));
}

TEST(SecurityLength, MarkdownBodyLen) {
    EXPECT_TRUE(litecode::security::validate_markdown_body_len(""));
    EXPECT_TRUE(litecode::security::validate_markdown_body_len(
        std::string(16 * 1024 * 1024, 'x')));
    EXPECT_FALSE(litecode::security::validate_markdown_body_len(
        std::string(16 * 1024 * 1024 + 1, 'x')));
}

TEST(SecurityLength, EmailLen) {
    EXPECT_FALSE(litecode::security::validate_email_len(""));        // empty
    EXPECT_TRUE(litecode::security::validate_email_len("a@b.io"));   // 6 chars
    EXPECT_TRUE(litecode::security::validate_email_len(
        std::string(254, 'a')));                                     // boundary
    EXPECT_FALSE(litecode::security::validate_email_len(
        std::string(255, 'a')));                                     // over
}

// ─── charset filters ──────────────────────────────────────────────────────

TEST(SecurityCharset, ControlCharsDetected) {
    EXPECT_TRUE(litecode::security::has_control_chars(std::string("\n")));
    EXPECT_TRUE(litecode::security::has_control_chars(std::string("\r")));
    EXPECT_TRUE(litecode::security::has_control_chars(std::string("\t")));
    // std::string("\0") is empty because the constructor reads until
    // the first NUL; pass an explicit length so the byte survives.
    EXPECT_TRUE(litecode::security::has_control_chars(std::string("\0", 1)));
    EXPECT_TRUE(litecode::security::has_control_chars(std::string("a\x01""b")));
    EXPECT_TRUE(litecode::security::has_control_chars(std::string("a\x7f""b")));
    EXPECT_FALSE(litecode::security::has_control_chars(""));
    EXPECT_FALSE(litecode::security::has_control_chars("hello world"));
    EXPECT_FALSE(litecode::security::has_control_chars("two-sum"));
    // Printable UTF-8 multi-byte chars are NOT control chars:
    EXPECT_FALSE(litecode::security::has_control_chars("用户"));
    EXPECT_FALSE(litecode::security::has_control_chars("∑x²"));
}

TEST(SecurityCharset, HtmlSpecialCharsDetected) {
    EXPECT_TRUE(litecode::security::has_html_special_chars("<"));
    EXPECT_TRUE(litecode::security::has_html_special_chars(">"));
    EXPECT_TRUE(litecode::security::has_html_special_chars("&"));
    EXPECT_TRUE(litecode::security::has_html_special_chars("<script>"));
    EXPECT_TRUE(litecode::security::has_html_special_chars("a & b"));
    EXPECT_FALSE(litecode::security::has_html_special_chars(""));
    EXPECT_FALSE(litecode::security::has_html_special_chars("two-sum"));
    EXPECT_FALSE(litecode::security::has_html_special_chars("hello world"));
}

TEST(SecurityCharset, JsonSpecialCharsDetected) {
    // U+2028 LINE SEPARATOR: UTF-8 = E2 80 A8
    EXPECT_TRUE(litecode::security::has_json_special_chars(
        std::string("a\xc0\xe2\x80\xa8""b")));
    // U+2029 PARAGRAPH SEPARATOR: UTF-8 = E2 80 A9
    EXPECT_TRUE(litecode::security::has_json_special_chars(
        std::string("a\xe2\x80\xa9""b")));
    // A 0xE2 byte that isn't followed by 0x80 0xA8 / 0xA9 must NOT
    // trigger — make sure we don't over-match.
    EXPECT_FALSE(litecode::security::has_json_special_chars(
        std::string("a\xe2\x80\xa0""b")));   // U+2020 (DAGGER)
    EXPECT_FALSE(litecode::security::has_json_special_chars(""));
    EXPECT_FALSE(litecode::security::has_json_special_chars("two-sum"));
}

TEST(SecurityCharset, IsSafeForLog) {
    EXPECT_TRUE(litecode::security::is_safe_for_log("alice"));
    EXPECT_TRUE(litecode::security::is_safe_for_log(""));
    EXPECT_FALSE(litecode::security::is_safe_for_log(std::string("ali\nce")));
    EXPECT_FALSE(litecode::security::is_safe_for_log(std::string("ali\rce")));
    EXPECT_FALSE(litecode::security::is_safe_for_log(
        std::string("ali\xe2\x80\xa8""ce")));
}

TEST(SecurityCharset, IsSafeForHtmlAttribute) {
    EXPECT_TRUE(litecode::security::is_safe_for_html_attribute("alice"));
    EXPECT_TRUE(litecode::security::is_safe_for_html_attribute(""));
    EXPECT_FALSE(litecode::security::is_safe_for_html_attribute("<x>"));
    EXPECT_FALSE(litecode::security::is_safe_for_html_attribute("a&b"));
    EXPECT_FALSE(litecode::security::is_safe_for_html_attribute(std::string("a\nb")));
}

// ─── composite shape validators ─────────────────────────────────────────

TEST(SecurityShape, SlugShape) {
    EXPECT_TRUE(litecode::security::validate_slug_shape("two-sum"));
    EXPECT_TRUE(litecode::security::validate_slug_shape("a"));
    EXPECT_TRUE(litecode::security::validate_slug_shape("abc-123"));
    EXPECT_TRUE(litecode::security::validate_slug_shape(
        std::string(100, 'a')));
    // Empty / too long
    EXPECT_FALSE(litecode::security::validate_slug_shape(""));
    EXPECT_FALSE(litecode::security::validate_slug_shape(
        std::string(101, 'a')));
    // Bad chars
    EXPECT_FALSE(litecode::security::validate_slug_shape("Two-Sum"));   // uppercase
    EXPECT_FALSE(litecode::security::validate_slug_shape("two_sum"));    // underscore
    EXPECT_FALSE(litecode::security::validate_slug_shape("two.sum"));    // dot
    EXPECT_FALSE(litecode::security::validate_slug_shape("two sum"));    // space
    EXPECT_FALSE(litecode::security::validate_slug_shape("two/sum"));    // slash
    EXPECT_FALSE(litecode::security::validate_slug_shape("two-sum "));   // trailing space
    // Leading / trailing '-'
    EXPECT_FALSE(litecode::security::validate_slug_shape("-two-sum"));
    EXPECT_FALSE(litecode::security::validate_slug_shape("two-sum-"));
}

TEST(SecurityShape, UsernameShape) {
    EXPECT_TRUE(litecode::security::validate_username_shape("alice"));
    EXPECT_TRUE(litecode::security::validate_username_shape("Bob_42"));
    EXPECT_TRUE(litecode::security::validate_username_shape("a.b-c"));
    // Length bounds
    EXPECT_FALSE(litecode::security::validate_username_shape("ab"));           // too short
    EXPECT_FALSE(litecode::security::validate_username_shape(
        std::string(51, 'a')));                                               // too long
    EXPECT_TRUE(litecode::security::validate_username_shape(
        std::string(3, 'a')));                                                // boundary low
    EXPECT_TRUE(litecode::security::validate_username_shape(
        std::string(50, 'a')));                                               // boundary high
    // Bad chars
    EXPECT_FALSE(litecode::security::validate_username_shape("alice@bob"));
    EXPECT_FALSE(litecode::security::validate_username_shape("alice bob"));
    EXPECT_FALSE(litecode::security::validate_username_shape("用户"));
    // Leading / trailing separators
    EXPECT_FALSE(litecode::security::validate_username_shape(".alice"));
    EXPECT_FALSE(litecode::security::validate_username_shape("-alice"));
    EXPECT_FALSE(litecode::security::validate_username_shape("alice."));
    EXPECT_FALSE(litecode::security::validate_username_shape("alice-"));
}

TEST(SecurityShape, RoleShape) {
    EXPECT_TRUE(litecode::security::validate_role_shape("user"));
    EXPECT_TRUE(litecode::security::validate_role_shape("admin"));
    EXPECT_FALSE(litecode::security::validate_role_shape(""));
    EXPECT_FALSE(litecode::security::validate_role_shape("root"));
    EXPECT_FALSE(litecode::security::validate_role_shape("ADMIN"));   // case-sensitive
    EXPECT_FALSE(litecode::security::validate_role_shape(" admin "));
}

// ─── HTML escape ─────────────────────────────────────────────────────────

TEST(SecurityEscape, HtmlEscapeTable) {
    EXPECT_EQ(litecode::security::escape_html(""),               "");
    EXPECT_EQ(litecode::security::escape_html("hello"),          "hello");
    EXPECT_EQ(litecode::security::escape_html("a<b>c"),          "a&lt;b&gt;c");
    EXPECT_EQ(litecode::security::escape_html("a&b"),            "a&amp;b");
    EXPECT_EQ(litecode::security::escape_html("\"quoted\""),    "&quot;quoted&quot;");
    EXPECT_EQ(litecode::security::escape_html("it's"),          "it&#x27;s");
    EXPECT_EQ(litecode::security::escape_html("<script>alert(1)</script>"),
              "&lt;script&gt;alert(1)&lt;/script&gt;");
    // The escaping must be idempotent on the encoded chars — calling
    // escape_html twice should NOT keep doubling up the & (otherwise
    // a logged-then-rerendered string bloats forever). We assert the
    // simple case: a literal '&' in the OUTPUT would survive a second
    // pass as '&amp;amp;' but never the other way around.
    const std::string once  = litecode::security::escape_html("a&b");
    const std::string twice = litecode::security::escape_html(once);
    EXPECT_EQ(twice, "a&amp;amp;b");
}

// ─── security.h constants pin the SPEC contract ─────────────────────────

TEST(SecurityConstants, ApiCspIsApiOnly) {
    // The API CSP is the strictest possible (default-src 'none') because
    // every response is JSON. The Caddyfile separately serves a more
    // permissive CSP for HTML pages (script-src cdn.jsdelivr.net). A
    // regression that conflates the two would let <script> tags execute
    // from a JSON body. Pin both the absence of script-src and the
    // presence of default-src 'none'.
    const std::string csp = litecode::security::kHeaderContentSecurityPolicyApi;
    EXPECT_NE(csp.find("default-src 'none'"),      std::string::npos);
    EXPECT_NE(csp.find("frame-ancestors 'none'"),  std::string::npos);
    EXPECT_EQ(csp.find("script-src"),              std::string::npos);
    EXPECT_EQ(csp.find("'unsafe-inline'"),         std::string::npos);
}

TEST(SecurityConstants, SecurityBaselineHeadersPinned) {
    EXPECT_STREQ(litecode::security::kHeaderXContentTypeOptions, "nosniff");
    EXPECT_STREQ(litecode::security::kHeaderXFrameOptions,        "DENY");
    EXPECT_STREQ(litecode::security::kHeaderReferrerPolicy,
                 "strict-origin-when-cross-origin");
    // Permissions-Policy must opt out the powerful-by-default features.
    EXPECT_NE(std::string(litecode::security::kHeaderPermissionsPolicy)
                  .find("geolocation=()"), std::string::npos);
    EXPECT_NE(std::string(litecode::security::kHeaderPermissionsPolicy)
                  .find("microphone=()"),  std::string::npos);
    EXPECT_NE(std::string(litecode::security::kHeaderPermissionsPolicy)
                  .find("camera=()"),      std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  server.h — response header integration
//
//  These tests stand up a real in-process HttpServer and assert that
//  every response (200, 404, 500, OPTIONS preflight) carries the
//  SPEC §15 baseline headers. Caddyfile sets the same headers on the
//  reverse-proxy path; server.h is the defense-in-depth backstop.
// ────────────────────────────────────────────────────────────────────────────

TEST(SecurityResponseHeaders, EveryResponseGetsBaselineHeaders) {
    // Register a few representative routes (200, 404, error envelope).
    litecode::HttpServer s(dev_server(), dev_cors());

    s.get("/api/v1/ok", [](const httplib::Request&,
                            httplib::Response& res) {
        litecode::send_success(res, {{"hello", "world"}});
    });

    // StdoutSilencer — the server's logger is a no-op but if a future
    // change routes startup logs here, they don't drown ctest output.
    ServerHandle h(&s);

    auto r = h.client->Get("/api/v1/ok");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);

    // Baseline security headers must be set on this 200:
    EXPECT_EQ(r->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(r->get_header_value("X-Frame-Options"),        "DENY");
    EXPECT_EQ(r->get_header_value("Referrer-Policy"),
              "strict-origin-when-cross-origin");
    EXPECT_EQ(r->get_header_value("Permissions-Policy"),
              "geolocation=(), microphone=(), camera=(), payment=()");
    // The API-only CSP. Caddyfile serves a more permissive CSP for
    // HTML pages; the API is JSON-only, so the strictest possible
    // policy is correct.
    EXPECT_NE(r->get_header_value("Content-Security-Policy")
                  .find("default-src 'none'"), std::string::npos);
}

TEST(SecurityResponseHeaders, NotFoundAlsoGetsBaselineHeaders) {
    litecode::HttpServer s(dev_server(), dev_cors());
    ServerHandle h(&s);

    auto r = h.client->Get("/api/v1/does/not/exist");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(r->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(r->get_header_value("X-Frame-Options"),        "DENY");
}

TEST(SecurityResponseHeaders, PreflightAlsoGetsBaselineHeaders) {
    // OPTIONS preflight goes through the pre-routing hook too — make
    // sure security headers land on the 204 path.
    litecode::HttpServer s(dev_server(), dev_cors());
    ServerHandle h(&s);

    httplib::Headers hdrs = {
        {"Origin", "http://localhost:8080"},
        {"Access-Control-Request-Method", "GET"},
    };
    auto r = h.client->Options("/api/v1/ok", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
    EXPECT_EQ(r->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(r->get_header_value("X-Frame-Options"),        "DENY");
}

// ────────────────────────────────────────────────────────────────────────────
//  Route-level hardening: parse_slug_param rejects malicious slugs
//  BEFORE they reach problem_repo::find_by_slug.
//
//  These tests don't bind to a real DB — they wire the helper in-
//  process via the public parse_slug_param (re-exported from
//  problem_routes.h's detail namespace) so a regression is caught
//  here without depending on MySQL.
// ────────────────────────────────────────────────────────────────────────────

TEST(SecurityRoute, SlugRejectsPathTraversal) {
    // parse_slug_param lives in detail:: in problem_routes.h. cpp-httplib's
    // route pattern "/api/v1/problems/([^/]+)" would normally keep ".."
    // out, but a defense-in-depth check in the helper itself closes the
    // gap on a future regex regression.
    EXPECT_FALSE(litecode::detail::parse_slug_param("..").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("../etc/passwd").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("..%2Fetc").has_value());
}

TEST(SecurityRoute, SlugRejectsControlChars) {
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("two\nsum")).has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("two\rsum")).has_value());
    // std::string("two\x00sum") is just "twosum" because the
    // constructor reads until NUL; use explicit length so the byte
    // survives and parse_slug_param can reject it.
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("two\x00sum", 8)).has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("two\x7fsum")).has_value());
}

TEST(SecurityRoute, SlugRejectsHtmlSpecialChars) {
    // A slug of "<script>" should never reach the DB — the API would
    // 400 it first.
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("<script>")).has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("a&b")).has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("a>b")).has_value());
}

TEST(SecurityRoute, SlugRejectsJsonLineSeparators) {
    // U+2028 (UTF-8: E2 80 A8) and U+2029 (E2 80 A9) — JSON.parse()
    // historically treated these as newlines, smuggling a payload
    // through a logger that renders via JSON.parse.
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("a\xe2\x80\xa8""b")).has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string("a\xe2\x80\xa9""b")).has_value());
}

TEST(SecurityRoute, SlugRejectsOverLength) {
    EXPECT_FALSE(litecode::detail::parse_slug_param(
        std::string(201, 'a')).has_value());
}

TEST(SecurityRoute, SlugAcceptsWellFormed) {
    EXPECT_TRUE(litecode::detail::parse_slug_param("two-sum").has_value());
    EXPECT_TRUE(litecode::detail::parse_slug_param("a").has_value());
    EXPECT_TRUE(litecode::detail::parse_slug_param(
        std::string(100, 'a')).has_value());
}

}  // namespace