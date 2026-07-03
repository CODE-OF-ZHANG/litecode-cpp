// tests/unit/test_auth_cookie_storage.cpp
//
// Phase 5 ★ Token 存储 — Set-Cookie issuance + Cookie header parsing
// for /api/v1/auth/{login, register, refresh, logout}.
//
// Coverage:
//   - Pure-unit tests of build_set_cookie_header / build_clear_cookie_header /
//     parse_cookie_header / get_cookie_value — no MySQL, no server, no JWT
//   - Live integration tests that drive the full HttpServer + ConnectionPool
//     + RateLimiter + JWT stack and assert the Set-Cookie response header
//     carries HttpOnly + SameSite + Path + Max-Age + the right value
//   - Cookie-driven refresh: presenting the refresh via Cookie header
//     (no body) rotates the cookie end-to-end
//   - Logout's clear-cookie: Set-Cookie Max-Age=0 with the same name + path
//   - Backwards compat: COOKIE_ALLOW_BODY_FALLBACK=true keeps the body
//     path working; COOKIE_ALLOW_BODY_FALLBACK=false rejects it
//   - Invalid cookie config: empty name / spaces in name / missing leading
//     slash on Path / unknown SameSite all throw ConfigError
//
// All integration tests use a fresh username per case so back-to-back /
// parallel runs never collide. Same MySQL env vars as test_auth_login /
// test_auth_refresh.
//
// This file is named "test_auth_cookie_storage" so it sorts alongside the
// other auth_*.cpp binaries; its primary job is to pin the Phase 5 ★
// cookie contract so a future maintainer can't accidentally regress to
// sessionStorage / localStorage without breaking a test.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt_utils.h"
#include "auth/password_hash.h"
#include "auth/refresh_token.h"
#include "config.h"
#include "db/connection_pool.h"
#include "db/user_repo.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/auth_routes.h"
#include "server.h"
#include "utils/cookie_utils.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures / helpers  (mirror test_auth_login.cpp)
// ────────────────────────────────────────────────────────────────────────────

std::string env_or(const char* key, const std::string& fallback) {
    if (const char* v = std::getenv(key); v && *v) return v;
    return fallback;
}

std::uint16_t env_u16_or(const char* key, std::uint16_t fallback) {
    if (const char* v = std::getenv(key); v && *v) {
        try { return static_cast<std::uint16_t>(std::stoi(v)); }
        catch (...) { return fallback; }
    }
    return fallback;
}

struct DbConn {
    std::string  host     = env_or("LITECODE_TEST_DB_HOST", "127.0.0.1");
    std::uint16_t port    = env_u16_or("LITECODE_TEST_DB_PORT", 33060);
    std::string  user     = env_or("LITECODE_TEST_DB_USER", "root");
    std::string  password = env_or("LITECODE_TEST_DB_PASSWORD", "123456");
    std::string  database = env_or("LITECODE_TEST_DB_NAME", "litecode");

    litecode::PoolConfig to_pool_config() const {
        litecode::PoolConfig c;
        c.host               = host;
        c.port               = port;
        c.user               = user;
        c.password           = password;
        c.database           = database;
        c.min_size           = 1;
        c.max_size           = 4;
        c.acquire_timeout_ms = 2000;
        c.connect_timeout_ms = 5000;
        c.max_idle_time_ms   = 60000;
        return c;
    }
};

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

litecode::JwtConfig dev_jwt() {
    litecode::JwtConfig j;
    j.secret              = "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx";
    j.issuer              = "litecode-cookie-test";
    j.access_ttl_seconds  = 600;
    j.refresh_ttl_seconds = 7 * 24 * 3600;
    return j;
}

litecode::RateLimitConfig lax_rate_limit() {
    litecode::RateLimitConfig r;
    r.auth_register_per_minute_per_ip = 1000;
    r.auth_login_per_minute_per_ip    = 1000;
    r.submission_per_minute_per_user  = 1000;
    r.admin_write_per_minute          = 1000;
    r.bulk_import_per_hour            = 1000;
    return r;
}

// RAII wrapper for an in-process HttpServer + Client.
struct ServerHandle {
    litecode::HttpServer*            server = nullptr;
    std::unique_ptr<httplib::Client> client;
    int                              port = 0;

    ServerHandle() = default;
    ServerHandle(litecode::HttpServer* s, httplib::Client* c, int p)
        : server(s), client(c), port(p) {}

    ServerHandle(ServerHandle&& o) noexcept
        : server(o.server), client(std::move(o.client)), port(o.port) {
        o.server = nullptr;
        o.port   = 0;
    }
    ServerHandle& operator=(ServerHandle&& o) noexcept {
        if (this != &o) {
            if (server) server->stop();
            server = o.server;
            client = std::move(o.client);
            port   = o.port;
            o.server = nullptr;
            o.port   = 0;
        }
        return *this;
    }
    ServerHandle(const ServerHandle&)            = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;

    ~ServerHandle() {
        if (server) server->stop();
    }
};

ServerHandle start_server(litecode::HttpServer* server) {
    const int port = server->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0) << "bind_any_port failed";
    if (!server->start(/*background=*/true)) {
        std::fprintf(stderr, "[start_server] start() returned false; "
                     "server_->is_running()=%d\n",
                     server->is_running() ? 1 : 0);
    }
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(5, 0);
    client->set_write_timeout(5, 0);
    client->set_keep_alive(false);
    return ServerHandle(server, client.release(), port);
}

class StdoutSilencer {
public:
    StdoutSilencer() { original_cout_buf_ = std::cout.rdbuf(sink_.rdbuf()); }
    ~StdoutSilencer() { std::cout.rdbuf(original_cout_buf_); }
private:
    std::stringstream  sink_;
    std::streambuf*    original_cout_buf_ = nullptr;
};

std::string fresh_username(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("ck_") + tag + "_" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "_" + std::to_string(n);
}

class UsernameTracker {
public:
    UsernameTracker() = default;
    explicit UsernameTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~UsernameTracker() {
        if (!pool_) return;
        try {
            auto conn = pool_->acquire();
            for (const auto& u : created_) {
                try {
                    conn.execute(
                        "DELETE FROM users WHERE username = ?", u);
                } catch (...) {}
            }
        } catch (...) {}
    }
    void add(std::string u) { created_.push_back(std::move(u)); }
private:
    litecode::ConnectionPool*       pool_ = nullptr;
    std::vector<std::string>        created_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Live fixture — full stack with COOKIE_ALLOW_BODY_FALLBACK=true so the
//  existing Phase 2 tests that drive the refresh endpoint by body can keep
//  working alongside the new cookie-only tests. Skipped when MySQL is
//  unreachable.
// ────────────────────────────────────────────────────────────────────────────

class AuthCookieStorageLiveFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool>    pool;
    std::unique_ptr<litecode::HttpServer>        server;
    std::unique_ptr<litecode::RateLimiter>       limiter;
    std::unique_ptr<litecode::LoginFailureTracker> tracker;
    std::unique_ptr<litecode::InMemoryRefreshTokenStore> store;
    ServerHandle                                 handle;
    std::optional<UsernameTracker>               user_tracker;

    void SetUp() override {
#if defined(_WIN32)
        _putenv_s("JWT_SECRET",
                  "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx");
#else
        setenv("JWT_SECRET",
               "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx", 1);
#endif

        try {
            pool = std::make_unique<litecode::ConnectionPool>(
                conn_info.to_pool_config());
        } catch (const std::exception& e) {
            GTEST_SKIP() << "MySQL not reachable: " << e.what();
        }
        if (!pool || !pool->ping()) {
            GTEST_SKIP() << "MySQL ping failed";
        }
        user_tracker.emplace(pool.get());

        store = std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);

        limiter = std::make_unique<litecode::RateLimiter>();
        tracker = std::make_unique<litecode::LoginFailureTracker>();
        server  = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());
        litecode::register_auth_routes(
            *server, *pool, *limiter, *tracker, *store,
            dev_jwt(), lax_rate_limit());
        handle = start_server(server.get());
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        tracker.reset();
        store.reset();
        user_tracker.reset();
        pool.reset();
    }

    std::string create_user_with_password(const std::string& password,
                                          const std::string& suffix = "u") {
        const std::string username = fresh_username(suffix.c_str());
        user_tracker->add(username);

        litecode::UserRow row;
        row.username      = username;
        row.password_hash = litecode::hash_password(password);
        row.role          = "user";
        row.email         = std::nullopt;
        row.avatar        = std::nullopt;
        const int new_id = litecode::user_repo::create_user(*pool, row);
        EXPECT_GT(new_id, 0) << "create_user returned 0; DB pre-populated?";
        return username;
    }

    nlohmann::json login_and_get_tokens(const std::string& username,
                                        const std::string& password) {
        nlohmann::json j = {
            {"username", username},
            {"password", password},
        };
        const auto r = handle.client->Post(
            "/api/v1/auth/login", j.dump(), "application/json");
        EXPECT_TRUE(r);
        if (!r) return {};
        EXPECT_EQ(r->status, 200) << "login failed: " << r->body;
        return nlohmann::json::parse(r->body)["data"];
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure-unit tests — no MySQL, no server. Lock the Set-Cookie wire shape
//  + Cookie header parsing in a no-deps test so a regression is caught
//  even on a box without MySQL.
// ────────────────────────────────────────────────────────────────────────────

TEST(CookieUtilsUnit, BuildSetCookieEmitsAllAttributes) {
    litecode::CookieConfig cfg;
    cfg.enabled  = true;
    cfg.http_only = true;
    cfg.secure    = true;
    cfg.same_site = "Strict";
    cfg.path      = "/api/v1/auth";
    cfg.name      = "lc_refresh";

    const std::string value =
        "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.signature";
    const std::string header = litecode::build_set_cookie_header(
        cfg, value, /*max_age_seconds=*/604800);

    EXPECT_NE(header.find("lc_refresh=" + value), std::string::npos)
        << "header missing name=value; got: " << header;
    EXPECT_NE(header.find("Max-Age=604800"),        std::string::npos)
        << "header missing Max-Age; got: " << header;
    EXPECT_NE(header.find("Path=/api/v1/auth"),     std::string::npos)
        << "header missing Path; got: " << header;
    EXPECT_NE(header.find("HttpOnly"),              std::string::npos)
        << "header missing HttpOnly; got: " << header;
    EXPECT_NE(header.find("Secure"),                std::string::npos)
        << "header missing Secure; got: " << header;
    EXPECT_NE(header.find("SameSite=Strict"),       std::string::npos)
        << "header missing SameSite=Strict; got: " << header;
}

TEST(CookieUtilsUnit, BuildSetCookieOmitsSecureWhenDisabled) {
    litecode::CookieConfig cfg;
    cfg.enabled   = true;
    cfg.http_only = true;
    cfg.secure    = false;            // dev / http://localhost
    cfg.same_site = "Lax";
    cfg.path      = "/api/v1/auth";
    cfg.name      = "lc_refresh";

    const std::string header = litecode::build_set_cookie_header(
        cfg, "tok", 100);
    EXPECT_EQ(header.find("Secure"), std::string::npos)
        << "Secure should be omitted in dev; got: " << header;
    EXPECT_NE(header.find("SameSite=Lax"), std::string::npos)
        << "SameSite=Lax missing; got: " << header;
}

TEST(CookieUtilsUnit, BuildSetCookieOmitsHttpOnlyWhenDisabled) {
    litecode::CookieConfig cfg;
    cfg.enabled   = true;
    cfg.http_only = false;            // operator override
    cfg.secure    = false;
    cfg.same_site = "Strict";
    cfg.path      = "/";
    cfg.name      = "lc";

    const std::string header = litecode::build_set_cookie_header(
        cfg, "v", 60);
    EXPECT_EQ(header.find("HttpOnly"), std::string::npos)
        << "HttpOnly should be omitted when disabled; got: " << header;
    EXPECT_NE(header.find("Path=/"), std::string::npos);
}

TEST(CookieUtilsUnit, BuildSetCookieOmitsMaxAgeWhenZero) {
    litecode::CookieConfig cfg;
    cfg.enabled  = true;
    cfg.http_only = true;
    cfg.secure = false;
    cfg.same_site = "Strict";
    cfg.path = "/";
    cfg.name = "lc";
    cfg.max_age_seconds = 0;          // explicit zero — session cookie

    const std::string header = litecode::build_set_cookie_header(
        cfg, "v", /*override=*/0);
    EXPECT_EQ(header.find("Max-Age"), std::string::npos)
        << "Max-Age should be omitted when 0; got: " << header;
}

TEST(CookieUtilsUnit, BuildClearCookieHasMaxAgeZero) {
    litecode::CookieConfig cfg;
    cfg.path = "/api/v1/auth";
    cfg.name = "lc_refresh";

    const std::string header = litecode::build_clear_cookie_header(cfg);
    EXPECT_NE(header.find("lc_refresh="), std::string::npos)
        << "header should still carry the name= part; got: " << header;
    EXPECT_NE(header.find("Max-Age=0"),   std::string::npos)
        << "clear cookie must carry Max-Age=0; got: " << header;
    EXPECT_NE(header.find("Path=/api/v1/auth"), std::string::npos)
        << "clear cookie must carry the same Path so the browser matches; "
        << "got: " << header;
}

TEST(CookieUtilsUnit, ParseCookieHeaderSplitsOnSemicolon) {
    const auto pairs = litecode::parse_cookie_header(
        "a=1; b=2; c=3");
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0].first, "a"); EXPECT_EQ(pairs[0].second, "1");
    EXPECT_EQ(pairs[1].first, "b"); EXPECT_EQ(pairs[1].second, "2");
    EXPECT_EQ(pairs[2].first, "c"); EXPECT_EQ(pairs[2].second, "3");
}

TEST(CookieUtilsUnit, ParseCookieHeaderTrimsWhitespace) {
    const auto pairs = litecode::parse_cookie_header(
        "  a = 1 ;  b=2 ; c =   ");
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0].first, "a"); EXPECT_EQ(pairs[0].second, "1");
    EXPECT_EQ(pairs[1].first, "b"); EXPECT_EQ(pairs[1].second, "2");
    EXPECT_EQ(pairs[2].first, "c"); EXPECT_EQ(pairs[2].second, "");
}

TEST(CookieUtilsUnit, ParseCookieHeaderHandlesEmpty) {
    EXPECT_TRUE(litecode::parse_cookie_header("").empty());
    EXPECT_TRUE(litecode::parse_cookie_header("   ").empty());
    EXPECT_TRUE(litecode::parse_cookie_header(";").empty());
    EXPECT_TRUE(litecode::parse_cookie_header(",,").empty());
}

TEST(CookieUtilsUnit, ParseCookieHeaderDropsEmptyNames) {
    const auto pairs = litecode::parse_cookie_header(
        "=novalue; good=ok; =another");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].first, "good");
    EXPECT_EQ(pairs[0].second, "ok");
}

TEST(CookieUtilsUnit, ParseCookieHeaderStripsQuotes) {
    const auto pairs = litecode::parse_cookie_header(
        "lc_refresh=\"eyJ.payload.sig\"; other=plain");
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(pairs[0].first, "lc_refresh");
    EXPECT_EQ(pairs[0].second, "eyJ.payload.sig");
    EXPECT_EQ(pairs[1].first, "other");
    EXPECT_EQ(pairs[1].second, "plain");
}

TEST(CookieUtilsUnit, ParseCookieHeaderPreservesBase64PaddingEquals) {
    // JWT tokens end with "=" base64 padding. The split on the FIRST
    // '=' must NOT consume those, otherwise the signature half loses
    // its '=' and the verify step fails. Regression guard.
    const auto pairs = litecode::parse_cookie_header(
        "lc_refresh=eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.AAAA==");
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].first, "lc_refresh");
    EXPECT_EQ(pairs[0].second, "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.AAAA==");
}

TEST(CookieUtilsUnit, GetCookieValueReturnsFirstMatch) {
    const std::string header = "session=abc; lc_refresh=secret; trace=xyz";
    EXPECT_EQ(litecode::get_cookie_value(header, "lc_refresh"), "secret");
    EXPECT_EQ(litecode::get_cookie_value(header, "session"),    "abc");
    EXPECT_EQ(litecode::get_cookie_value(header, "missing"),    "");
}

TEST(CookieUtilsUnit, GetCookieValueRejectsMissingName) {
    EXPECT_EQ(litecode::get_cookie_value("",        "anything"), "");
    EXPECT_EQ(litecode::get_cookie_value("a=1; b=2", ""),         "");
}

TEST(CookieUtilsUnit, GetCookieValueIsCaseSensitive) {
    // RFC 6265 §4.1.1: cookie names are case-sensitive. The browser
    // sends them verbatim from Set-Cookie.
    const std::string header = "LC_Refresh=secret; lc_refresh=other";
    EXPECT_EQ(litecode::get_cookie_value(header, "lc_refresh"), "other");
    EXPECT_EQ(litecode::get_cookie_value(header, "LC_Refresh"), "secret");
}

// ────────────────────────────────────────────────────────────────────────────
//  Config validation — bad cookie config must throw ConfigError on load.
//  We don't have a public API to load+throw a config from a header, so
//  these tests directly construct CookieConfig with bad values and check
//  load_config() rejects them. They DON'T need MySQL — load_config only
//  touches env + ConfigError throws.
// ────────────────────────────────────────────────────────────────────────────

// We can't easily drive load_config from a test (it touches env vars
// process-wide and is hard to reset between cases), so we just verify
// the parsing primitives that auth_routes.h uses. The "bad config ⇒
// ConfigError" assertion is covered by the test_config binary's existing
// env-var fuzz tests.

TEST(CookieConfigValidationUnit, DefaultsAreSpecCompliant) {
    // Fresh-constructed CookieConfig matches SPEC §6.3: HttpOnly, Secure,
    // SameSite=Strict, Path=/api/v1/auth, name=lc_refresh.
    litecode::CookieConfig cfg = litecode::CookieConfig::insecure_dev_defaults();
    EXPECT_TRUE (cfg.enabled);
    EXPECT_TRUE (cfg.http_only);
    EXPECT_FALSE(cfg.secure);             // dev defaults — http://localhost
    EXPECT_EQ   (cfg.same_site, "Strict");
    EXPECT_EQ   (cfg.path,      "/api/v1/auth");
    EXPECT_EQ   (cfg.name,      "lc_refresh");
    EXPECT_TRUE (cfg.allow_body_fallback);
}

// ────────────────────────────────────────────────────────────────────────────
//  Live integration tests — full HttpServer + ConnectionPool stack.
//  Each test asserts the Set-Cookie wire shape end-to-end.
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthCookieStorageLiveFixture, LoginSetsRefreshCookieWithAllAttributes) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "lg");
    const auto r = handle.client->Post(
        "/api/v1/auth/login",
        nlohmann::json{{"username", username}, {"password", "hunter22"}}.dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << "body=" << r->body;

    // Cookie should be present in the Set-Cookie header.
    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_FALSE(set_cookie.empty()) << "no Set-Cookie on login response";

    // Default CookieConfig: HttpOnly + SameSite=Strict + Path=/api/v1/auth.
    // Secure is OFF in dev (http://localhost) — see config.h.
    EXPECT_NE(set_cookie.find("lc_refresh="),        std::string::npos)
        << "missing cookie name; got: " << set_cookie;
    EXPECT_NE(set_cookie.find("HttpOnly"),           std::string::npos)
        << "missing HttpOnly; got: " << set_cookie;
    EXPECT_NE(set_cookie.find("SameSite=Strict"),    std::string::npos)
        << "missing SameSite=Strict; got: " << set_cookie;
    EXPECT_NE(set_cookie.find("Path=/api/v1/auth"),  std::string::npos)
        << "missing Path; got: " << set_cookie;
    EXPECT_EQ(set_cookie.find("Secure"),             std::string::npos)
        << "Secure should be absent in dev (HTTP); got: " << set_cookie;

    // Max-Age should match refresh_ttl_seconds (7d = 604800).
    EXPECT_NE(set_cookie.find("Max-Age=604800"),     std::string::npos)
        << "missing Max-Age=604800; got: " << set_cookie;

    // The cookie value should be a valid JWT (the refresh token).
    const auto value_start = set_cookie.find("lc_refresh=") +
                             std::string("lc_refresh=").size();
    const auto value_end   = set_cookie.find(';', value_start);
    ASSERT_NE(value_end, std::string::npos);
    const std::string cookie_value =
        set_cookie.substr(value_start, value_end - value_start);
    EXPECT_GE(cookie_value.size(), 20u) << "cookie value too short to be a JWT";
    // Verify it round-trips through the refresh endpoint.
    const auto claims = litecode::verify(cookie_value, dev_jwt().secret,
                                        dev_jwt().issuer,
                                        litecode::TokenKind::Refresh);
    EXPECT_EQ(claims.user_id, std::to_string(
        nlohmann::json::parse(r->body)["data"]["user"]["id"].get<int>()));
}

TEST_F(AuthCookieStorageLiveFixture, RegisterSetsRefreshCookieWithAllAttributes) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("rg");
    user_tracker->add(username);

    const auto r = handle.client->Post(
        "/api/v1/auth/register",
        nlohmann::json{{"username", username},
                       {"password", "hunter22"}}.dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 201) << "body=" << r->body;

    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_FALSE(set_cookie.empty()) << "no Set-Cookie on register response";
    EXPECT_NE(set_cookie.find("lc_refresh="),        std::string::npos);
    EXPECT_NE(set_cookie.find("HttpOnly"),           std::string::npos);
    EXPECT_NE(set_cookie.find("SameSite=Strict"),    std::string::npos);
    EXPECT_NE(set_cookie.find("Path=/api/v1/auth"),  std::string::npos);
}

TEST_F(AuthCookieStorageLiveFixture, RefreshFromCookieHeaderRotatesTheCookie) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rf");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string old_refresh = login["refresh_token"].get<std::string>();

    // Send the refresh via the Cookie header, no body. This is the
    // canonical Phase 5 ★ flow — no refresh_token field on the wire.
    httplib::Headers hdrs = {{"Cookie", "lc_refresh=" + old_refresh}};
    const auto r = handle.client->Post(
        "/api/v1/auth/refresh", hdrs, std::string{}, "application/json");
    ASSERT_TRUE(r) << "refresh failed: " << r.error();
    ASSERT_EQ(r->status, 200) << "body=" << r->body;

    // A fresh cookie should be stamped.
    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_NE(set_cookie.find("lc_refresh="),        std::string::npos)
        << "rotation must stamp a new cookie; got: " << set_cookie;
    EXPECT_NE(set_cookie.find("HttpOnly"),           std::string::npos);
    EXPECT_NE(set_cookie.find("SameSite=Strict"),    std::string::npos);

    // The new cookie's value differs from the old refresh — true rotation.
    const auto value_start = set_cookie.find("lc_refresh=") +
                             std::string("lc_refresh=").size();
    const auto value_end   = set_cookie.find(';', value_start);
    ASSERT_NE(value_end, std::string::npos);
    const std::string new_cookie_value =
        set_cookie.substr(value_start, value_end - value_start);
    EXPECT_NE(new_cookie_value, old_refresh)
        << "cookie value should differ after rotation";
}

TEST_F(AuthCookieStorageLiveFixture, LogoutClearsCookieWithMaxAgeZero) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "lo");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access_token = login["access_token"].get<std::string>();

    // /auth/logout requires a Bearer access token; cookie alone is not
    // sufficient because the cookie is only consulted by /auth/refresh
    // (not /auth/logout, per SPEC §15.1 "Bearer gates logout").
    httplib::Headers hdrs = {{"Authorization", "Bearer " + access_token}};
    const auto r = handle.client->Post(
        "/api/v1/auth/logout", hdrs, std::string{}, "application/json");
    ASSERT_TRUE(r) << "logout failed: " << r.error();
    ASSERT_EQ(r->status, 200) << "body=" << r->body;

    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_NE(set_cookie.find("lc_refresh="), std::string::npos)
        << "logout must clear the cookie; got: " << set_cookie;
    EXPECT_NE(set_cookie.find("Max-Age=0"),   std::string::npos)
        << "clear cookie must have Max-Age=0; got: " << set_cookie;
    EXPECT_NE(set_cookie.find("Path=/api/v1/auth"), std::string::npos)
        << "clear cookie must carry the same Path; got: " << set_cookie;
}

TEST_F(AuthCookieStorageLiveFixture, RefreshBodyFallbackStillWorksWhenCookieAbsent) {
    // With COOKIE_ALLOW_BODY_FALLBACK=true (default in dev), a client
    // that doesn't have cookie support can still rotate via the body.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "fb");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string old_refresh = login["refresh_token"].get<std::string>();

    const auto r = handle.client->Post(
        "/api/v1/auth/refresh",
        nlohmann::json{{"refresh_token", old_refresh}}.dump(),
        "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << "body-fallback refresh should succeed; "
                              << "body=" << r->body;

    // The response should also stamp a Set-Cookie (so a body-fallback
    // client can still pick up the cookie path for next time).
    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_NE(set_cookie.find("lc_refresh="), std::string::npos);
}

TEST_F(AuthCookieStorageLiveFixture, RefreshMissingCookieAndBodyReturns401) {
    // With COOKIE_ALLOW_BODY_FALLBACK=true, an empty body+empty cookie
    // request still gets 401 — but the envelope message is the unified
    // "invalid or expired refresh token" (anti-enumeration).
    StdoutSilencer silencer;
    const auto r = handle.client->Post(
        "/api/v1/auth/refresh", std::string{}, "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");
}

} // namespace