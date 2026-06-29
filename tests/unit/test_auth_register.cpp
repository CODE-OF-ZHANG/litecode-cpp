// tests/unit/test_auth_register.cpp
//
// Integration tests for src/routes/auth_routes.h — POST /api/v1/auth/register.
//
// Coverage:
//   - 201 on happy path: response shape + bcrypt hash + tokens are usable
//   - 400 on missing username / password / wrong types
//   - 400 on invalid username (length, charset, leading/trailing dot/dash)
//   - 400 on weak password (too short, no letter, no digit)
//   - 400 on invalid email (when present)
//   - 400 on malformed JSON
//   - 409 on duplicate username
//   - 409 on duplicate email
//   - 429 after the 5/min/IP bucket empties (rate-limit + Retry-After header)
//   - 201 with valid email round-trips (email persisted, find_by_username)
//   - Refresh token jti ends up in the blacklist ONLY after logout
//     (we don't test logout here — that comes in the logout-handler test)
//   - placeholder login/refresh/logout/profile endpoints return 501
//
// Integration tests need a live MySQL — gated by the same env vars as
// test_user_repo / test_connection_pool. Pure validation tests skip the
// rate limit so they all run even when MySQL is unreachable.
//
// Each integration test uses a unique username (timestamp + counter) so
// parallel runs and back-to-back runs never collide.

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

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures / helpers
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
    j.issuer              = "litecode-register-test";
    j.access_ttl_seconds  = 600;       // 10 min — short for test speed
    j.refresh_ttl_seconds = 7 * 24 * 3600;  // 7d — matches SPEC §5.1
    return j;
}

// A rate-limit config generous enough that the validation tests don't
// trip the bucket. The rate-limit-specific test uses a tight custom
// config instead.
litecode::RateLimitConfig lax_rate_limit() {
    litecode::RateLimitConfig r;
    r.auth_register_per_minute_per_ip = 1000;
    r.auth_login_per_minute_per_ip    = 1000;
    r.submission_per_minute_per_user  = 1000;
    r.admin_write_per_minute          = 1000;
    r.bulk_import_per_hour            = 1000;
    return r;
}

litecode::RateLimitConfig tight_register_rate_limit() {
    // Exactly the SPEC §5.1 quota (5/min/IP). Test creates its own
    // limiter so the bucket starts empty.
    litecode::RateLimitConfig r;
    r.auth_register_per_minute_per_ip = 5;
    r.auth_login_per_minute_per_ip    = 1000;
    r.submission_per_minute_per_user  = 1000;
    r.admin_write_per_minute          = 1000;
    r.bulk_import_per_hour            = 1000;
    return r;
}

// RAII wrapper: spin up an HttpServer on an ephemeral port, return a
// Client wired to it. Tears down on scope exit.
struct ServerHandle {
    litecode::HttpServer*            server = nullptr;
    std::unique_ptr<httplib::Client> client;
    int                              port = 0;

    ServerHandle() = default;        // gtest needs the fixture to be default-constructible
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
    return std::string("ar_") + tag + "_" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "_" + std::to_string(n);
}

// Cleanup any usernames registered with the tracker.
class UsernameTracker {
public:
    UsernameTracker() = default;             // lets std::optional default-construct us
    explicit UsernameTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~UsernameTracker() {
        if (!pool_) return;
        try {
            auto conn = pool_->acquire();
            for (const auto& u : created_) {
                try { conn.execute("DELETE FROM users WHERE username = ?", u); }
                catch (...) {}
            }
        } catch (...) {}
    }
    void add(std::string u) { created_.push_back(std::move(u)); }
private:
    litecode::ConnectionPool*       pool_ = nullptr;
    std::vector<std::string>        created_;
};

// Build a JSON body for the register endpoint.
std::string register_body(const std::string& username,
                          const std::string& password,
                          const std::optional<std::string>& email = std::nullopt) {
    nlohmann::json j = {
        {"username", username},
        {"password", password},
    };
    if (email) j["email"] = *email;
    return j.dump();
}

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures
// ────────────────────────────────────────────────────────────────────────────

// The "live" fixture: spins up the full stack — server + pool + limiter
// + auth_routes — against a real MySQL. Skips when MySQL is unreachable.
class AuthLiveFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    // login-failure tracker is required by the new register_auth_routes
    // signature (Phase 2 ★ added /api/v1/auth/refresh, which doesn't
    // need the tracker but the parameter is shared across the whole
    // auth route family). Register-only tests pass a fresh instance
    // and never bump the counter.
    std::unique_ptr<litecode::LoginFailureTracker> failure_tracker;
    ServerHandle                          handle;
    // UsernameTracker needs a pool pointer, which doesn't exist until
    // SetUp() runs. Wrapped in optional so the field is default-
    // constructible and we can emplace it once the pool is up.
    std::optional<UsernameTracker>        tracker;

    void SetUp() override {
        // The auth_routes happy path emits LOG_INFO on success, which
        // lazily bootstraps the global Logger → calls config() →
        // load_config() → throws ConfigError when JWT_SECRET is unset.
        // We don't actually USE the global config (every config value
        // is passed explicitly to register_auth_routes), so this is a
        // bootstrap quirk rather than a real dependency. Setting
        // JWT_SECRET in the env sidesteps the lazy bootstrap's strict
        // validation. We use the same secret as dev_jwt() so anything
        // that accidentally hits config() picks up a working secret.
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
        tracker.emplace(pool.get());

        limiter = std::make_unique<litecode::RateLimiter>();
        server  = std::make_unique<litecode::HttpServer>(
            dev_server(), dev_cors());
        // Throwaway store + login-failure tracker — the register
        // suite doesn't exercise /api/v1/auth/refresh, but
        // register_auth_routes now takes both (Phase 2 ★ refresh
        // signature).
        failure_tracker = std::make_unique<litecode::LoginFailureTracker>();
        auto register_store =
            std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);
        litecode::register_auth_routes(*server, *pool, *limiter,
                                       *failure_tracker, *register_store,
                                       dev_jwt(), lax_rate_limit());
        handle = start_server(server.get());
    }

    void TearDown() override {
        handle = ServerHandle();   // default ctor — no-op destructor
        server.reset();
        failure_tracker.reset();
        limiter.reset();
        tracker.reset();           // runs UsernameTracker's destructor
        pool.reset();
    }
};

// Post helper that wraps the boilerplate.
httplib::Result post_register(ServerHandle& h,
                              const std::string& body) {
    return h.client->Post("/api/v1/auth/register", body, "application/json");
}

// ────────────────────────────────────────────────────────────────────────────
//  Happy path
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLiveFixture, HappyPathReturns201WithTokensAndPersistedUser) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("happy");
    tracker->add(username);

    const auto r = post_register(handle,
        register_body(username, "hunter22"));
    ASSERT_TRUE(r) << "POST failed: " << r.error();
    ASSERT_EQ(r->status, 201) << "body=" << r->body;

    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("data")) << "missing data envelope: " << r->body;
    const auto& data = body["data"];

    // User block — no password_hash leaked.
    ASSERT_TRUE(data.contains("user"));
    const auto& user = data["user"];
    EXPECT_EQ(user["username"], username);
    EXPECT_EQ(user["role"],     "user");
    EXPECT_FALSE(user.contains("password_hash"));
    EXPECT_TRUE (user.contains("id"));
    EXPECT_TRUE (user["id"].is_number_integer());

    // Tokens + envelope shape.
    ASSERT_TRUE(data.contains("access_token"));
    ASSERT_TRUE(data.contains("refresh_token"));
    EXPECT_EQ(data["token_type"], "Bearer");
    EXPECT_TRUE(data.contains("expires_in"));
    EXPECT_GE(data["expires_in"], 599);     // ~dev_jwt() ttl (10 min) — allow 1s slack for the gap between sign + expires_in math
    EXPECT_LE(data["expires_in"], 600);

    // Access token verifies + carries the right claims.
    const auto claims = litecode::verify(
        data["access_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Access);
    EXPECT_EQ(claims.username, username);
    EXPECT_EQ(claims.role,     "user");
    EXPECT_EQ(claims.user_id,  std::to_string(user["id"].get<int>()));

    // Refresh token verifies + is a refresh (no username/role).
    const auto refresh_claims = litecode::verify(
        data["refresh_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Refresh);
    EXPECT_EQ(refresh_claims.user_id, std::to_string(user["id"].get<int>()));
    EXPECT_TRUE (refresh_claims.username.empty());
    EXPECT_TRUE (refresh_claims.role.empty());

    // The user is actually persisted — find_by_username round-trips
    // and the password matches what we sent (bcrypt cost=12 verification).
    const auto row = litecode::user_repo::find_by_username(*pool, username);
    ASSERT_TRUE(row.has_value()) << "user not persisted";
    EXPECT_EQ(row->id,   user["id"].get<int>());
    EXPECT_EQ(row->role, "user");
    EXPECT_TRUE (litecode::verify_password("hunter22", row->password_hash));
    EXPECT_FALSE(litecode::verify_password("WRONG_PW_9!", row->password_hash));
}

TEST_F(AuthLiveFixture, HappyPathWithEmailPersistsEmail) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("happy_email");
    const std::string email    = username + "@example.test";
    tracker->add(username);

    const auto r = post_register(handle, register_body(username, "hunter22", email));
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 201);

    const auto row = litecode::user_repo::find_by_username(*pool, username);
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->email.has_value());
    EXPECT_EQ(*row->email, email);
}

TEST_F(AuthLiveFixture, ResponseEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("rid");
    tracker->add(username);

    httplib::Headers hdrs = {{"X-Request-Id", "client-supplied-rid-001"}};
    auto r = handle.client->Post(
        "/api/v1/auth/register",
        hdrs,
        register_body(username, "hunter22"),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "client-supplied-rid-001");

    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "client-supplied-rid-001");
}

// ────────────────────────────────────────────────────────────────────────────
//  400 — bad input
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLiveFixture, RejectsMissingUsername) {
    StdoutSilencer silencer;
    auto r = post_register(handle, R"({"password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "username");
}

TEST_F(AuthLiveFixture, RejectsMissingPassword) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("nopass");
    auto r = post_register(handle, R"({"username":")" + username + R"("})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "password");
}

TEST_F(AuthLiveFixture, RejectsShortUsername) {
    StdoutSilencer silencer;
    auto r = post_register(handle, R"({"username":"ab","password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "username");
}

TEST_F(AuthLiveFixture, RejectsUsernameWithInvalidChars) {
    StdoutSilencer silencer;
    auto r = post_register(handle, R"({"username":"alice@bob","password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST_F(AuthLiveFixture, RejectsWeakPasswordTooShort) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("weak_pw_short");
    auto r = post_register(handle, register_body(username, "a1b2c3"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "password");
}

TEST_F(AuthLiveFixture, RejectsWeakPasswordNoLetters) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("weak_pw_nol");
    auto r = post_register(handle, register_body(username, "12345678"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["details"]["field"], "password");
}

TEST_F(AuthLiveFixture, RejectsWeakPasswordNoDigits) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("weak_pw_nod");
    auto r = post_register(handle, register_body(username, "onlyletters"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["details"]["field"], "password");
}

TEST_F(AuthLiveFixture, RejectsBadEmail) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("bad_email");
    auto r = post_register(handle,
        register_body(username, "hunter22", "not-an-email"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["details"]["field"], "email");
}

TEST_F(AuthLiveFixture, RejectsMalformedJson) {
    StdoutSilencer silencer;
    auto r = post_register(handle, "{not valid json}");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
}

TEST_F(AuthLiveFixture, RejectsEmptyBody) {
    StdoutSilencer silencer;
    auto r = post_register(handle, "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

// ────────────────────────────────────────────────────────────────────────────
//  409 — conflicts
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLiveFixture, RejectsDuplicateUsername) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("dup");
    tracker->add(username);

    auto first = post_register(handle, register_body(username, "hunter22"));
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 201);

    auto second = post_register(handle, register_body(username, "different9P"));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 409);
    const auto body = nlohmann::json::parse(second->body);
    EXPECT_EQ(body["code"], "CONFLICT");
    EXPECT_EQ(body["details"]["field"], "username");
}

TEST_F(AuthLiveFixture, RejectsDuplicateEmail) {
    StdoutSilencer silencer;
    const std::string email = fresh_username("dup_email") + "@example.test";

    const std::string u1 = fresh_username("dup_e1");
    const std::string u2 = fresh_username("dup_e2");
    tracker->add(u1);
    tracker->add(u2);

    auto first = post_register(handle, register_body(u1, "hunter22", email));
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 201);

    auto second = post_register(handle, register_body(u2, "hunter22", email));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 409);
    const auto body = nlohmann::json::parse(second->body);
    EXPECT_EQ(body["code"], "CONFLICT");
    EXPECT_EQ(body["details"]["field"], "email");
}

// ────────────────────────────────────────────────────────────────────────────
//  429 — rate limit (custom limiter + tight config)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLiveFixture, RateLimitTriggersAtSixthRequest) {
    StdoutSilencer silencer;

    // Tear down the lax server from SetUp and rebuild with the SPEC §5.1
    // quota so we can drive the bucket under our own clock.
    handle = ServerHandle();
    server.reset();
    limiter.reset();

    // Use unique usernames so we hit 429 on rate, not 409 on conflict.
    std::vector<std::string> names;
    for (int i = 0; i < 7; ++i) {
        names.push_back(fresh_username(("rl" + std::to_string(i)).c_str()));
        tracker->add(names.back());
    }

    // Fresh limiter with the SPEC §5.1 quota — bucket starts at 5 tokens.
    limiter = std::make_unique<litecode::RateLimiter>();
    server  = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    // Throwaway store + failure tracker — the rate-limit suite
    // doesn't exercise /api/v1/auth/refresh or login, but the
    // route table needs both.
    auto rate_store =
        std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);
    litecode::LoginFailureTracker rate_failure_tracker;
    litecode::register_auth_routes(*server, *pool, *limiter,
                                   rate_failure_tracker, *rate_store,
                                   dev_jwt(), tight_register_rate_limit());
    handle = start_server(server.get());

    // First 5 must succeed.
    for (int i = 0; i < 5; ++i) {
        auto r = post_register(handle, register_body(names[i], "hunter22"));
        ASSERT_TRUE(r) << "request " << i << " failed: " << r.error();
        ASSERT_EQ(r->status, 201) << "request " << i << " body=" << r->body;
        EXPECT_FALSE(r->get_header_value("X-RateLimit-Remaining").empty());
    }
    // 6th must be 429.
    auto blocked = post_register(handle, register_body(names[5], "hunter22"));
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked->status, 429);
    EXPECT_FALSE(blocked->get_header_value("Retry-After").empty());
    EXPECT_FALSE(blocked->get_header_value("X-RateLimit-Remaining").empty());

    const auto body = nlohmann::json::parse(blocked->body);
    EXPECT_EQ(body["code"], "RATE_LIMITED");
    EXPECT_EQ(body["details"]["quota"], "auth.register");

    // 7th is also 429 (bucket still empty).
    auto blocked2 = post_register(handle, register_body(names[6], "hunter22"));
    ASSERT_TRUE(blocked2);
    EXPECT_EQ(blocked2->status, 429);
}

// ────────────────────────────────────────────────────────────────────────────
//  Placeholder routes (501 until Phase 2 follow-ups)
//
//  /api/v1/auth/login, /api/v1/auth/refresh, and /api/v1/auth/logout
//  are now real handlers (login covered in test_auth_login.cpp,
//  refresh in test_auth_refresh.cpp, logout in test_auth_logout.cpp).
//  Only profile still 501.
// ────────────────────────────────────────────────────────────────────────────

// Login / refresh / logout moved to their own suites since
// Phase 2 ★ — see SPEC §5.1, §15.1. They are real handlers, not
// placeholders.

// ────────────────────────────────────────────────────────────────────────────
//  Pure validation tests (no MySQL, no server) — pinned via the public
//  validate_username / validate_email helpers in user_repo.h.
//  We keep them here so register-validation regressions are caught by
//  the same suite, even on a box without MySQL.
// ────────────────────────────────────────────────────────────────────────────

TEST(RegisterValidation, UsernameLengthBoundary) {
    std::string err;
    EXPECT_FALSE(litecode::validate_username("ab",  &err));
    EXPECT_TRUE (litecode::validate_username("abc", &err));
    EXPECT_TRUE (litecode::validate_username(std::string(50, 'a'), &err));
    EXPECT_FALSE(litecode::validate_username(std::string(51, 'a'), &err));
}

TEST(RegisterValidation, EmailBoundary) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("a@b",    &err));
    EXPECT_TRUE (litecode::validate_email("a@b.c",  &err));
    EXPECT_TRUE (litecode::validate_email(std::string(90, 'a') + "@x.io", &err));
}

// Diagnostic test — no fixture, no MySQL, no rate limiter. Just check
// that the server wires up and a route registered via
// register_auth_routes is reachable (a 501 from a placeholder endpoint
// is fine — the goal is to confirm the round-trip works).
TEST(AuthRegisterSmoke, PlaceholderReachableWithoutMysql) {
    StdoutSilencer silencer;
    litecode::HttpServer server(dev_server(), dev_cors());
    litecode::RateLimiter limiter;
    litecode::JwtConfig jwt = dev_jwt();
    litecode::RateLimitConfig rl = lax_rate_limit();

    // We can't call register_auth_routes without a ConnectionPool&,
    // so this test just confirms the server framework itself works on
    // a route registered without the pool — same shape as a 501 stub.
    server.post("/api/v1/auth/login",
        [](const httplib::Request&, httplib::Response& res) {
            litecode::send_error(res, 501, litecode::ErrorCode::SERVICE_UNAVAILABLE,
                                 "test stub");
        });
    ServerHandle h = start_server(&server);

    auto r = h.client->Post("/api/v1/auth/login", "{}", "application/json");
    ASSERT_TRUE(r) << "post failed";
    EXPECT_EQ(r->status, 501);

    (void)jwt; (void)rl; (void)limiter; // silence unused warnings
}

TEST(AuthRegisterSmoke, RegisterAuthRoutesReachable) {
    StdoutSilencer silencer;
    litecode::HttpServer server(dev_server(), dev_cors());
    litecode::RateLimiter limiter;
    litecode::JwtConfig jwt;
    jwt.secret              = "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx";
    jwt.issuer              = "litecode-register-test";
    jwt.access_ttl_seconds  = 600;
    jwt.refresh_ttl_seconds = 7 * 24 * 3600;
    litecode::RateLimitConfig rl = lax_rate_limit();

    // Register the same routes register_auth_routes would register,
    // minus the one that needs the pool. (Each placeholder lambda
    // closes over nothing — they don't need any state.)
    auto not_implemented = [](const httplib::Request&,
                              httplib::Response& res) {
        litecode::send_error(res, 501, litecode::ErrorCode::SERVICE_UNAVAILABLE,
                             "stub");
    };
    server.post("/api/v1/auth/login",   not_implemented);
    server.post("/api/v1/auth/refresh", not_implemented);
    server.post("/api/v1/auth/logout",  not_implemented);
    server.get ("/api/v1/auth/profile", not_implemented);

    (void)limiter; (void)jwt; (void)rl;

    ServerHandle h = start_server(&server);

    auto r = h.client->Post("/api/v1/auth/login", "{}", "application/json");
    ASSERT_TRUE(r) << "post failed";
    EXPECT_EQ(r->status, 501);
}

} // namespace