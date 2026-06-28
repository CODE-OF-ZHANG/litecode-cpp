// tests/unit/test_server.cpp
//
// Unit tests for src/server.h — the Phase 1 HTTP service framework.
//
// Tests are written against an in-process HttpServer bound to an
// ephemeral port; we then drive it with httplib::Client the same way
// production traffic would. No real socket is exposed.
//
// Coverage:
//   - Routing registration (GET / POST / PUT / PATCH / DELETE)
//   - Uniform JSON response envelope (send_success / send_created /
//     send_error) and content-type charset
//   - X-Request-Id middleware: generated when missing, echoed when
//     valid, replaced when client sent garbage (CRLF injection guard)
//   - Per-thread request_id scope matches the response header
//   - Unified 404 envelope returned by the error handler
//   - CORS: preflight OPTIONS short-circuit, allowed origin headers,
//     disallowed origin suppressed, credentials on/off
//   - Multi-threaded handling: N concurrent requests don't tear /
//     every request sees its own request_id
//   - JSON body parser: empty body → 400, malformed → 400,
//     valid → parsed object
//   - Static mount_point serves a real file from disk
//   - generate_uuid_v4: shape + RFC 4122 v4 version/variant bits
//   - is_valid_request_id: rejects CRLF, oversize, empty
//   - error_code_name: pins SPEC §5.7 catalog

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>     // _getpid
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"   // ServerConfig / CorsConfig
#include "logger.h"   // RequestIdScope
#include "server.h"   // HttpServer / send_* / parse_json_body / ...

namespace {

inline int test_pid() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures / helpers
// ────────────────────────────────────────────────────────────────────────────

// A tiny logging config that writes nowhere — every test should be silent
// on stdout so `ctest -V` stays readable. Tests that DO want to inspect
// log lines can use the helper directly.
litecode::LoggingConfig silent_logging() {
    litecode::LoggingConfig c;
    c.level = "ERROR";          // drop INFO/WARN, keep ERROR visible if something fails
    c.format = "text";
    c.file_path = "";           // stdout-only — but we'll silence stdout per test
    c.include_request_id = true;
    return c;
}

litecode::ServerConfig dev_server() {
    litecode::ServerConfig s;
    s.host = "127.0.0.1";
    s.port = 0;                  // bind_any_port() will pick an ephemeral one
    s.thread_pool_size = 4;
    return s;
}

litecode::CorsConfig dev_cors() {
    litecode::CorsConfig c;
    c.allowed_origins = "http://localhost:8080,http://127.0.0.1:8080";
    c.allow_credentials = true;
    return c;
}

// RAII wrapper: spin up an HttpServer on an ephemeral port, return a
// Client wired to it. Tears down on scope exit.
//
// The server is a NON-OWNING pointer — the test owns the HttpServer
// on its stack. We must not take ownership here, otherwise the stack
// HttpServer and ServerHandle both delete the same object.
struct ServerHandle {
    litecode::HttpServer*                server = nullptr;
    std::unique_ptr<httplib::Client>     client;
    int                                  port = 0;

    ServerHandle(litecode::HttpServer* s, httplib::Client* c, int p)
        : server(s), client(c), port(p) {}

    ServerHandle(ServerHandle&& o) noexcept
        : server(o.server), client(std::move(o.client)), port(o.port) {
        o.server = nullptr;
        o.port   = 0;
    }
    ServerHandle& operator=(ServerHandle&&) = delete;
    ServerHandle(const ServerHandle&)            = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;

    ~ServerHandle() {
        if (server) server->stop();
    }
};

ServerHandle start_server(litecode::HttpServer* server) {
    const int port = server->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0) << "bind_any_port failed";
    EXPECT_TRUE(server->start(/*background=*/true));
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(5, 0);
    client->set_write_timeout(5, 0);
    // Keep-alive OFF: the server doesn't auto-set Content-Length for
    // raw-body responses, so a keep-alive client would hang waiting
    // for the connection to close. Connection-close per request is
    // fine for in-process tests.
    client->set_keep_alive(false);
    return ServerHandle(server, client.release(), port);
}

// Silence stdout chatter from per-request access logs while a test is
// running. Restores the original streams on scope exit.
class StdoutSilencer {
public:
    StdoutSilencer() {
        original_cout_buf_ = std::cout.rdbuf(sink_.rdbuf());
    }
    ~StdoutSilencer() {
        std::cout.rdbuf(original_cout_buf_);
    }

    std::string captured() const { return sink_.str(); }

private:
    std::stringstream sink_;
    std::streambuf*   original_cout_buf_ = nullptr;
};

// ────────────────────────────────────────────────────────────────────────────
//  Routing & response envelope
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerRouting, RegisterHandlersForAllMethods) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    s.get   ("/items", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res, {{"verb", "GET"}});
    });
    s.post  ("/items", [](const httplib::Request&, httplib::Response& res){
        litecode::send_created(res, {{"verb", "POST"}});
    });
    s.put   ("/items/:id", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res, {{"verb", "PUT"}});
    });
    s.patch ("/items/:id", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res, {{"verb", "PATCH"}});
    });
    s.del   ("/items/:id", [](const httplib::Request&, httplib::Response& res){
        litecode::send_no_content(res);
    });

    auto h = start_server(&s);

    auto r_get  = h.client->Get("/items");
    auto r_post = h.client->Post("/items", "{}", "application/json");
    auto r_put  = h.client->Put("/items/42", "{}", "application/json");
    auto r_pat  = h.client->Patch("/items/42", "{}", "application/json");
    auto r_del  = h.client->Delete("/items/42");

    ASSERT_TRUE(r_get)  << "GET failed: "    << r_get.error();
    ASSERT_TRUE(r_post) << "POST failed: "   << r_post.error();
    ASSERT_TRUE(r_put)  << "PUT failed: "    << r_put.error();
    ASSERT_TRUE(r_pat)  << "PATCH failed: "  << r_pat.error();
    ASSERT_TRUE(r_del)  << "DELETE failed: " << r_del.error();

    EXPECT_EQ(r_get->status,  200);
    EXPECT_EQ(r_post->status, 201);
    EXPECT_EQ(r_put->status,  200);
    EXPECT_EQ(r_pat->status,  200);
    EXPECT_EQ(r_del->status,  204);

    // The verbs came back in the body.
    EXPECT_EQ(r_get->body,  R"({"data":{"verb":"GET"},"request_id":")" + r_get->get_header_value("X-Request-Id") + R"("})");
    EXPECT_EQ(r_post->body, R"({"data":{"verb":"POST"},"request_id":")" + r_post->get_header_value("X-Request-Id") + R"("})");
    EXPECT_EQ(r_put->body,  R"({"data":{"verb":"PUT"},"request_id":")"  + r_put->get_header_value("X-Request-Id")  + R"("})");
    EXPECT_EQ(r_pat->body,  R"({"data":{"verb":"PATCH"},"request_id":")"+ r_pat->get_header_value("X-Request-Id")  + R"("})");
}

TEST(ServerRouting, SendErrorUsesUnifiedEnvelope) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    s.get("/forbidden", [](const httplib::Request&, httplib::Response& res){
        litecode::send_error(res, 403, litecode::ErrorCode::FORBIDDEN,
                             "you shall not pass",
                             {{"resource", "/secret"}});
    });

    auto h = start_server(&s);
    auto r = h.client->Get("/forbidden");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_NE(r->body.find("\"code\":\"FORBIDDEN\""), std::string::npos);
    EXPECT_NE(r->body.find("\"message\":\"you shall not pass\""), std::string::npos);
    EXPECT_NE(r->body.find("\"resource\":\"/secret\""), std::string::npos);
    EXPECT_NE(r->body.find("\"request_id\":"), std::string::npos);
}

TEST(ServerRouting, JsonResponseContentTypeIsUtf8) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res, {{"k", "v"}});
    });

    auto h = start_server(&s);
    auto r = h.client->Get("/x");
    ASSERT_TRUE(r);
    const auto ct = r->get_header_value("Content-Type");
    EXPECT_NE(ct.find("application/json"), std::string::npos);
    EXPECT_NE(ct.find("charset=utf-8"),     std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  Request-Id middleware
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerRequestId, GeneratedWhenMissing) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    auto r = h.client->Get("/x");
    ASSERT_TRUE(r);

    const auto rid = r->get_header_value("X-Request-Id");
    EXPECT_EQ(rid.size(), 36u);                       // UUID v4 length
    EXPECT_EQ(rid[14],   '4');                        // version nibble
    EXPECT_TRUE(rid[19] == '8' || rid[19] == '9' ||
                rid[19] == 'a' || rid[19] == 'b');   // variant nibble
}

TEST(ServerRequestId, EchoedWhenValid) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    httplib::Headers hdrs = { {"X-Request-Id", "client-supplied-12345"} };
    auto r = h.client->Get("/x", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "client-supplied-12345");
}

TEST(ServerRequestId, ReplacedWhenClientSendsGarbage) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    // Garbage that IS valid HTTP header bytes but contains characters
    // our validator rejects (colon is not in the safe alphabet). We
    // expect the server to refuse to echo it and substitute a fresh
    // UUID v4.
    httplib::Headers bad_hdrs = { {"X-Request-Id", "evil:value:here"} };
    auto r = h.client->Get("/x", bad_hdrs);
    ASSERT_TRUE(r);
    const auto rid = r->get_header_value("X-Request-Id");
    EXPECT_EQ(rid.size(), 36u) << "rid=" << rid;
    EXPECT_EQ(rid[14], '4');
}

TEST(ServerRequestId, VisibleInsideHandler) {
    // The RequestIdScope set by the pre-routing hook must be visible to
    // the body of the handler — this is what lets request handlers emit
    // log lines that correlate with the access log line.
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    std::string observed_rid;
    s.get("/x", [&observed_rid](const httplib::Request&, httplib::Response& res){
        observed_rid = litecode::current_request_id();
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    httplib::Headers hdrs = { {"X-Request-Id", "abc-123"} };
    auto r = h.client->Get("/x", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(observed_rid, "abc-123");
}

// ────────────────────────────────────────────────────────────────────────────
//  404 envelope
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerErrorEnvelope, NotFoundReturnsUnifiedBody) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    auto h = start_server(&s);
    auto r = h.client->Get("/this/does/not/exist");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_NE(r->body.find("\"code\":\"NOT_FOUND\""),  std::string::npos);
    EXPECT_NE(r->body.find("\"message\":\"Not Found\""), std::string::npos);
    EXPECT_NE(r->body.find("\"request_id\":"),         std::string::npos);
    EXPECT_NE(r->get_header_value("Content-Type").find("application/json"), std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  CORS
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerCors, AllowedOriginGetsHeaders) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    httplib::Headers hdrs = { {"Origin", "http://localhost:8080"} };
    auto r = h.client->Get("/x", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->get_header_value("Access-Control-Allow-Origin"), "http://localhost:8080");
    EXPECT_EQ(r->get_header_value("Access-Control-Allow-Credentials"), "true");
    EXPECT_EQ(r->get_header_value("Vary"), "Origin");
}

TEST(ServerCors, DisallowedOriginGetsNoCorsHeaders) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    httplib::Headers hdrs = { {"Origin", "https://evil.example.com"} };
    auto r = h.client->Get("/x", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->get_header_value("Access-Control-Allow-Origin"), "");
}

TEST(ServerCors, PreflightFromAllowedOriginReturns204) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    auto h = start_server(&s);
    httplib::Headers hdrs = {
        {"Origin", "http://localhost:8080"},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "Content-Type"},
    };
    auto r = h.client->Options("/api/v1/auth/register", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
    EXPECT_EQ(r->get_header_value("Access-Control-Allow-Origin"), "http://localhost:8080");
    EXPECT_NE(r->get_header_value("Access-Control-Allow-Methods").find("POST"), std::string::npos);
    EXPECT_NE(r->get_header_value("Access-Control-Allow-Headers").find("Content-Type"), std::string::npos);
}

TEST(ServerCors, PreflightFromDisallowedOriginReturns403) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    auto h = start_server(&s);
    httplib::Headers hdrs = {
        {"Origin", "https://evil.example.com"},
        {"Access-Control-Request-Method", "POST"},
    };
    auto r = h.client->Options("/api/v1/auth/register", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
}

TEST(ServerCors, NoOriginMeansSameOriginNoCorsHeaders) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.get("/x", [](const httplib::Request&, httplib::Response& res){
        litecode::send_success(res);
    });

    auto h = start_server(&s);
    auto r = h.client->Get("/x");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->get_header_value("Access-Control-Allow-Origin"), "");
}

// ────────────────────────────────────────────────────────────────────────────
//  Multi-threaded handling
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerThreading, ConcurrentRequestsAllReceiveDistinctIds) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    std::atomic<int> seen{0};
    s.get("/x", [&seen](const httplib::Request&, httplib::Response& res){
        // Yield to encourage the runtime to interleave threads.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto rid = litecode::current_request_id();
        // Stash the rid in a header so the test can read it back.
        res.set_header("X-Observed-Rid", rid);
        seen.fetch_add(1, std::memory_order_relaxed);
        litecode::send_success(res, {{"rid", rid}});
    });

    auto h = start_server(&s);

    constexpr int N = 50;
    std::vector<std::future<std::string>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(std::async(std::launch::async, [&]{
            auto r = h.client->Get("/x");
            if (!r) return std::string{};
            return r->get_header_value("X-Observed-Rid");
        }));
    }

    std::set<std::string> rids;
    for (auto& f : futs) {
        const auto rid = f.get();
        ASSERT_FALSE(rid.empty()) << "request failed";
        auto [_, inserted] = rids.insert(rid);
        EXPECT_TRUE(inserted) << "duplicate rid: " << rid;
    }
    EXPECT_EQ(rids.size(), static_cast<std::size_t>(N));
    EXPECT_EQ(seen.load(), N);

    // Each observed rid must also be valid (we set them in the handler
    // from current_request_id(), which came from the pre-routing hook).
    for (const auto& rid : rids) {
        EXPECT_TRUE(litecode::is_valid_request_id(rid)) << rid;
    }
}

TEST(ServerThreading, HandlerSeesOwnRequestIdUnderLoad) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    s.get("/x", [](const httplib::Request& req, httplib::Response& res){
        const auto client_rid = req.get_header_value("X-Request-Id");
        const auto thread_rid = litecode::current_request_id();
        litecode::send_success(res, {{"client", client_rid},
                                     {"thread", thread_rid}});
    });

    auto h = start_server(&s);

    constexpr int N = 30;
    std::vector<std::future<bool>> futs;
    for (int i = 0; i < N; ++i) {
        futs.push_back(std::async(std::launch::async, [&, i]{
            const std::string my_rid = "thread-test-" + std::to_string(i);
            httplib::Headers hdrs = { {"X-Request-Id", my_rid} };
            auto r = h.client->Get("/x", hdrs);
            if (!r) return false;
            auto body = nlohmann::json::parse(r->body);
            return body["data"]["client"] == my_rid
                && body["data"]["thread"] == my_rid;
        }));
    }
    for (auto& f : futs) EXPECT_TRUE(f.get());
}

// ────────────────────────────────────────────────────────────────────────────
//  JSON body parser
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerJsonBody, EmptyBodyReturns400) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.post("/x", [](const httplib::Request& req, httplib::Response& res){
        // Handlers that want body validation call parse_json_body().
        // If they don't, the framework doesn't enforce anything —
        // the test below pins down the helper's behavior.
        auto j = litecode::parse_json_body(req, res);
        if (!j) return;             // parse_json_body already wrote 400
        litecode::send_success(res, {{"echo", *j}});
    });

    auto h = start_server(&s);
    auto r = h.client->Post("/x", "", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_NE(r->body.find("\"code\":\"INVALID_INPUT\""), std::string::npos);
}

TEST(ServerJsonBody, MalformedJsonReturns400) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.post("/x", [](const httplib::Request& req, httplib::Response& res){
        auto j = litecode::parse_json_body(req, res);
        if (!j) return;
        litecode::send_success(res, {{"echo", *j}});
    });

    auto h = start_server(&s);
    auto r = h.client->Post("/x", "{not json", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_NE(r->body.find("\"code\":\"INVALID_INPUT\""), std::string::npos);
    EXPECT_NE(r->body.find("Invalid JSON body"), std::string::npos);
}

TEST(ServerJsonBody, ValidJsonIsParsed) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;

    s.post("/x", [](const httplib::Request& req, httplib::Response& res){
        auto j = litecode::parse_json_body(req, res);
        if (!j) return;
        litecode::send_success(res, {{"echo", *j}});
    });

    auto h = start_server(&s);
    auto r = h.client->Post("/x", R"({"name":"alice","n":7})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["echo"]["name"], "alice");
    EXPECT_EQ(body["data"]["echo"]["n"],    7);
}

// ────────────────────────────────────────────────────────────────────────────
//  Static file serving via mount_point
// ────────────────────────────────────────────────────────────────────────────

class StaticMountTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("litecode_static_" + std::to_string(test_pid()) + "_" +
                std::to_string(counter_++));
        std::filesystem::create_directories(dir_);

        std::ofstream(dir_ / "hello.txt") << "hi from disk";
        std::ofstream(dir_ / "index.html") << "<html>hi</html>";
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir() const { return dir_; }
private:
    std::filesystem::path dir_;
    static inline int counter_ = 0;
};

TEST_F(StaticMountTest, ServesFilesFromMountPoint) {
    litecode::HttpServer s(dev_server(), dev_cors());
    StdoutSilencer silencer;
    s.mount("/static", dir().string());

    auto h = start_server(&s);
    auto r = h.client->Get("/static/hello.txt");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->body, "hi from disk");
}

// ────────────────────────────────────────────────────────────────────────────
//  UUID v4
// ────────────────────────────────────────────────────────────────────────────

TEST(UuidV4, ShapeAndVersionBits) {
    litecode::generate_uuid_v4();    // warm the rng
    for (int i = 0; i < 1000; ++i) {
        const auto id = litecode::generate_uuid_v4();
        ASSERT_EQ(id.size(), 36u) << id;
        // Dashes at positions 8, 13, 18, 23.
        EXPECT_EQ(id[8],  '-');
        EXPECT_EQ(id[13], '-');
        EXPECT_EQ(id[18], '-');
        EXPECT_EQ(id[23], '-');
        // Version nibble.
        EXPECT_EQ(id[14], '4') << id;
        // Variant nibble (8, 9, a, b).
        const char v = id[19];
        EXPECT_TRUE(v == '8' || v == '9' || v == 'a' || v == 'b') << id;
    }
}

TEST(UuidV4, DistinctIds) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(litecode::generate_uuid_v4());
    }
    EXPECT_EQ(seen.size(), 1000u);
}

// ────────────────────────────────────────────────────────────────────────────
//  RequestId validator
// ────────────────────────────────────────────────────────────────────────────

TEST(RequestIdValidator, AcceptsAlnumAndSafePunct) {
    EXPECT_TRUE(litecode::is_valid_request_id("abc-123"));
    EXPECT_TRUE(litecode::is_valid_request_id("ABC.123"));
    EXPECT_TRUE(litecode::is_valid_request_id("a_b-c.d"));
    EXPECT_TRUE(litecode::is_valid_request_id("0123456789"));
}

TEST(RequestIdValidator, RejectsCrLfAndSpace) {
    EXPECT_FALSE(litecode::is_valid_request_id("a\nb"));
    EXPECT_FALSE(litecode::is_valid_request_id("a\rb"));
    EXPECT_FALSE(litecode::is_valid_request_id("a b"));
    EXPECT_FALSE(litecode::is_valid_request_id("a:b"));  // colon is header-unsafe
}

TEST(RequestIdValidator, RejectsEmptyAndOversize) {
    EXPECT_FALSE(litecode::is_valid_request_id(""));
    EXPECT_FALSE(litecode::is_valid_request_id(std::string(129, 'a')));
}

// ────────────────────────────────────────────────────────────────────────────
//  Error code catalog (SPEC §5.7)
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

TEST(ErrorCodeCatalog, EnvelopeMatchesSpec) {
    litecode::RequestIdScope scope("test-req-42");
    auto env = litecode::make_error_envelope(
        litecode::ErrorCode::NOT_FOUND,
        "the thing is missing",
        {{"slug", "two-sum"}});
    EXPECT_EQ(env["code"],       "NOT_FOUND");
    EXPECT_EQ(env["message"],    "the thing is missing");
    EXPECT_EQ(env["details"]["slug"], "two-sum");
    EXPECT_EQ(env["request_id"], "test-req-42");
}

// ────────────────────────────────────────────────────────────────────────────
//  Thread pool wiring
// ────────────────────────────────────────────────────────────────────────────

TEST(ServerThreadPool, SizeFromConfig) {
    litecode::ServerConfig cfg = dev_server();
    cfg.thread_pool_size = 3;
    litecode::HttpServer s(cfg, dev_cors());
    EXPECT_EQ(s.thread_pool_size(), 3u);

    cfg.thread_pool_size = 0;   // 0 falls back to default 8
    litecode::HttpServer s2(cfg, dev_cors());
    EXPECT_EQ(s2.thread_pool_size(), 8u);
}

} // anonymous namespace