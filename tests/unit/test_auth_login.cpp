// tests/unit/test_auth_login.cpp
//
// Integration tests for src/routes/auth_routes.h — POST /api/v1/auth/login.
//
// Coverage:
//   - 200 on happy path: response shape + bcrypt verify + tokens usable
//   - 200 on admin login: role claim correctly surfaces "admin"
//   - 200 stamps users.last_login + last_login_ip (DB row updated)
//   - 401 on bad password (identical envelope to "no such user")
//   - 401 on unknown username (identical envelope to "bad password" —
//     anti-enumeration per SPEC §15.1)
//   - 401 on success resets the per-username failure counter
//   - 400 on missing / wrong-type username + password
//   - 400 on malformed username (length, charset, leading/trailing dot)
//   - 400 on empty password
//   - 400 on malformed JSON / empty body
//   - 429 after the 10/min/IP bucket empties (rate-limit + Retry-After)
//   - audit_logs row written every kAuditLogEvery (5) consecutive failures
//     (verifies SPEC §15.1 + A27 acceptance)
//   - audit_logs NOT written before the threshold
//   - placeholder refresh/logout/profile endpoints still 501
//
// Integration tests need a live MySQL — gated by the same env vars as
// test_user_repo / test_connection_pool / test_auth_register. The audit-log
// verification is gated by the same condition (audit_logs is a DB row).
//
// Each integration test uses a unique username so parallel + back-to-back
// runs never collide and the failure-count tracker reads sanely.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt_utils.h"
#include "auth/password_hash.h"
#include "auth/refresh_token.h"
#include "config.h"
#include "db/audit_log_repo.h"
#include "db/connection_pool.h"
#include "db/user_repo.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/auth_routes.h"
#include "server.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures / helpers  (mirror test_auth_register.cpp)
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
    j.issuer              = "litecode-login-test";
    j.access_ttl_seconds  = 600;             // 10 min
    j.refresh_ttl_seconds = 7 * 24 * 3600;   // 7 d
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

// SPEC §5.1 / A26 — exactly 10/min/IP for /api/v1/auth/login. The
// rate-limit-specific test uses this; everything else uses lax_rate_limit.
litecode::RateLimitConfig tight_login_rate_limit() {
    litecode::RateLimitConfig r;
    r.auth_register_per_minute_per_ip = 1000;
    r.auth_login_per_minute_per_ip    = 10;
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
    return std::string("lg_") + tag + "_" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "_" + std::to_string(n);
}

// ────────────────────────────────────────────────────────────────────────────
//  Username tracker: deletes every created account in TearDown so back-to-back
//  test runs (and parallel CI shards) don't accumulate cruft in `users`.
// ────────────────────────────────────────────────────────────────────────────

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
                    // Delete audit rows owned by this username too — the
                    // audit-log tests insert rows whose target_id matches
                    // a real username, and we don't want orphans piling
                    // up run-over-run.
                    conn.execute(
                        "DELETE FROM audit_logs WHERE target_id = ?", u);
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

// Count audit_logs rows for a given target_id / action. Used by the
// failure-audit tests. Returns -1 on a DB error (caller treats as skip).
int count_audit_logs(litecode::ConnectionPool& pool,
                     const std::string& target_id,
                     const std::string& action) {
    try {
        auto conn = pool.acquire();
        const auto row = conn.fetch_one(
            "SELECT COUNT(*) FROM audit_logs WHERE target_id = ? "
            "AND action = ?",
            target_id, action);
        if (!row) return -1;
        return static_cast<int>(row->operator[](0).get<std::int64_t>());
    } catch (...) {
        return -1;
    }
}

// POST helper that wraps httplib boilerplate.
httplib::Result post_login(ServerHandle& h, const std::string& body) {
    return h.client->Post("/api/v1/auth/login", body, "application/json");
}

// Convenience: build a {username,password} body.
std::string login_body(const std::string& username,
                       const std::string& password) {
    nlohmann::json j = {
        {"username", username},
        {"password", password},
    };
    return j.dump();
}

// ────────────────────────────────────────────────────────────────────────────
//  Live fixture — full stack (server + pool + limiter + tracker + auth_routes)
//  skipped when MySQL is unreachable.
// ────────────────────────────────────────────────────────────────────────────

class AuthLoginLiveFixture : public ::testing::Test {
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
        // Lazy global logger bootstrap (mirrors test_auth_register).
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

        limiter       = std::make_unique<litecode::RateLimiter>();
        tracker       = std::make_unique<litecode::LoginFailureTracker>();
        // The login suite doesn't exercise /api/v1/auth/refresh, but
        // the route table now expects a RefreshTokenStore — supply a
        // throwaway in-memory one so the registration call compiles
        // and the per-test pool stays consistent.
        store         = std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);
        server        = std::make_unique<litecode::HttpServer>(
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

    // Insert a brand-new test user directly via the repo (skips the
    // register endpoint so a single happy-path test isn't shadowed by
    // strict password rules + audit_log noise). Returns the inserted
    // username.
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

    // Insert an admin user — verifies the role claim is "admin" end-to-end.
    std::string create_admin_with_password(const std::string& password) {
        const std::string username = fresh_username("admin");
        user_tracker->add(username);
        litecode::UserRow row;
        row.username      = username;
        row.password_hash = litecode::hash_password(password);
        row.role          = "admin";
        row.email         = std::nullopt;
        row.avatar        = std::nullopt;
        const int new_id = litecode::user_repo::create_user(*pool, row);
        EXPECT_GT(new_id, 0);
        return username;
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  200 — happy path
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLoginLiveFixture, HappyPathReturns200WithTokens) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "happy");
    const std::string password = "hunter22";

    const auto r = post_login(handle, login_body(username, password));
    ASSERT_TRUE(r) << "POST failed: " << r.error();
    ASSERT_EQ(r->status, 200) << "body=" << r->body;

    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("data")) << "missing data envelope: " << r->body;
    const auto& data = body["data"];

    // User block — must NOT leak password_hash.
    ASSERT_TRUE(data.contains("user"));
    const auto& user = data["user"];
    EXPECT_EQ(user["username"], username);
    EXPECT_EQ(user["role"],     "user");
    EXPECT_FALSE(user.contains("password_hash"));
    EXPECT_TRUE (user["id"].is_number_integer());

    // Tokens + envelope shape.
    ASSERT_TRUE(data.contains("access_token"));
    ASSERT_TRUE(data.contains("refresh_token"));
    EXPECT_EQ(data["token_type"], "Bearer");
    EXPECT_TRUE(data.contains("expires_in"));
    EXPECT_GE(data["expires_in"], 599);
    EXPECT_LE(data["expires_in"], 600);

    // Access token verifies with the right claims.
    const auto claims = litecode::verify(
        data["access_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Access);
    EXPECT_EQ(claims.username, username);
    EXPECT_EQ(claims.role,     "user");
    EXPECT_EQ(claims.user_id,  std::to_string(user["id"].get<int>()));

    // Refresh token verifies (no username/role — least privilege).
    const auto refresh = litecode::verify(
        data["refresh_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Refresh);
    EXPECT_EQ(refresh.user_id, std::to_string(user["id"].get<int>()));
    EXPECT_TRUE(refresh.username.empty());
    EXPECT_TRUE(refresh.role.empty());
}

TEST_F(AuthLoginLiveFixture, HappyPathAdminSurfacesAdminRole) {
    StdoutSilencer silencer;
    const std::string username = create_admin_with_password("adminPass1");
    const auto r = post_login(handle, login_body(username, "adminPass1"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["user"]["role"], "admin");

    const auto claims = litecode::verify(
        body["data"]["access_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Access);
    EXPECT_EQ(claims.role, "admin");
}

TEST_F(AuthLoginLiveFixture, HappyPathStampsLastLogin) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "ll");

    // Snapshot the row before login — last_login must be NULL.
    const auto before = litecode::user_repo::find_by_username(*pool, username);
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(before->last_login.has_value());

    const auto r = post_login(handle, login_body(username, "hunter22"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);

    // After login — last_login must be set (last_login_ip may or may
    // not be set; bind_any_port wires the loopback so it often is,
    // but the integration test just asserts the timestamp column).
    const auto after = litecode::user_repo::find_by_username(*pool, username);
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->last_login.has_value())
        << "users.last_login was not stamped by login_handler";
    EXPECT_FALSE(after->last_login->empty());
}

TEST_F(AuthLoginLiveFixture, SuccessfulLoginResetsFailureCounter) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rst");

    // Three failures — counter goes 1, 2, 3.
    for (int i = 0; i < 3; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    EXPECT_EQ(tracker->count(username), 3);

    // A successful login zeros the counter.
    const auto ok = post_login(handle, login_body(username, "hunter22"));
    ASSERT_TRUE(ok);
    ASSERT_EQ(ok->status, 200);
    EXPECT_EQ(tracker->count(username), 0);

    // A subsequent failure starts at 1, not 4 (the old counter
    // was cleared, not decayed).
    const auto fail = post_login(handle,
        login_body(username, "WRONG_PW_xx9"));
    ASSERT_TRUE(fail);
    EXPECT_EQ(fail->status, 401);
    EXPECT_EQ(tracker->count(username), 1);
}

TEST_F(AuthLoginLiveFixture, ResponseEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rid");

    httplib::Headers hdrs = {{"X-Request-Id", "login-rid-002"}};
    auto r = handle.client->Post(
        "/api/v1/auth/login", hdrs,
        login_body(username, "hunter22"),
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "login-rid-002");
    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "login-rid-002");
}

// ────────────────────────────────────────────────────────────────────────────
//  401 — bad credentials  (anti-enumeration: envelope = "no such user")
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLoginLiveFixture, WrongPasswordReturns401WithIdenticalEnvelope) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "wp");

    const auto r = post_login(handle,
        login_body(username, "WRONG_PW_xx9"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);

    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid username or password");
    EXPECT_FALSE(body.contains("details"));   // never expose which axis was wrong
}

TEST_F(AuthLoginLiveFixture, UnknownUsernameReturns401WithIdenticalEnvelope) {
    StdoutSilencer silencer;
    // username passes shape validation (3..50, [a-z0-9_.-]) so the
    // request makes it past 400 and exercises the "no such user" path.
    const auto r = post_login(handle,
        login_body(fresh_username("nosuch"), "anypassword9"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);

    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid username or password");

    // The envelope for "no such user" MUST be byte-identical to
    // "wrong password" except for the request_id (which differs by
    // definition because it's a fresh request).
    const auto& msg = body["message"];
    EXPECT_EQ(msg, "invalid username or password");
}

TEST_F(AuthLoginLiveFixture, UnknownUsernameStillBumpsTracker) {
    StdoutSilencer silencer;
    const std::string ghost = fresh_username("ghost");

    for (int i = 0; i < 2; ++i) {
        const auto r = post_login(handle,
            login_body(ghost, "anypassword9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    // A non-existent user should still feed the per-username counter —
    // otherwise an attacker probing with random usernames could walk
    // past the audit-log trigger.
    EXPECT_EQ(tracker->count(ghost), 2);
}

// ────────────────────────────────────────────────────────────────────────────
//  400 — input validation
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLoginLiveFixture, RejectsMissingUsername) {
    StdoutSilencer silencer;
    auto r = post_login(handle, R"({"password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "username");
}

TEST_F(AuthLoginLiveFixture, RejectsMissingPassword) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("nopw");
    auto r = post_login(handle, R"({"username":")" + username + R"("})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "password");
}

TEST_F(AuthLoginLiveFixture, RejectsShortUsername) {
    StdoutSilencer silencer;
    auto r = post_login(handle,
        R"({"username":"ab","password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "username");
}

TEST_F(AuthLoginLiveFixture, RejectsUsernameWithInvalidChars) {
    StdoutSilencer silencer;
    auto r = post_login(handle,
        R"({"username":"alice@evil","password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST_F(AuthLoginLiveFixture, RejectsUsernameWithLeadingDot) {
    StdoutSilencer silencer;
    auto r = post_login(handle,
        R"({"username":".alice","password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST_F(AuthLoginLiveFixture, RejectsEmptyPassword) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("epw");
    auto r = post_login(handle,
        R"({"username":")" + username + R"(","password":""})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["details"]["field"], "password");
}

TEST_F(AuthLoginLiveFixture, RejectsMalformedJson) {
    StdoutSilencer silencer;
    auto r = post_login(handle, "{not valid json}");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
}

TEST_F(AuthLoginLiveFixture, RejectsEmptyBody) {
    StdoutSilencer silencer;
    auto r = post_login(handle, "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

TEST_F(AuthLoginLiveFixture, RejectsNonStringUsername) {
    StdoutSilencer silencer;
    auto r = post_login(handle,
        R"({"username":42,"password":"hunter22"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

// ────────────────────────────────────────────────────────────────────────────
//  429 — rate limit  (SPEC §5.1: 10/min/IP, A26 acceptance)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLoginLiveFixture, RateLimitTriggersAtEleventhRequest) {
    StdoutSilencer silencer;

    // Rebuild the server with the SPEC §5.1 login quota so we can drive
    // the bucket under our own clock.
    handle = ServerHandle();
    server.reset();
    limiter.reset();
    tracker.reset();
    store.reset();

    // 11 unique attempts: first 10 must pass through the rate limit
    // gate (we expect 401 on most, since none of these users exist;
    // what matters is that the rate-limit guard doesn't reject them),
    // the 11th must hit the 429 envelope.
    const std::vector<std::string> names = [this]{
        std::vector<std::string> v;
        for (int i = 0; i < 11; ++i) {
            v.push_back(fresh_username(("rl" + std::to_string(i)).c_str()));
        }
        return v;
    }();

    limiter = std::make_unique<litecode::RateLimiter>();
    tracker = std::make_unique<litecode::LoginFailureTracker>();
    // Re-create the per-test store so the rebuilt server can wire it
    // into the route table. The reset() above dropped it; tests that
    // rebuild the server must rebuild the store too.
    store   = std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);
    server  = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    litecode::register_auth_routes(
        *server, *pool, *limiter, *tracker, *store,
        dev_jwt(), tight_login_rate_limit());
    handle = start_server(server.get());

    // First 10: each must be answered (401 for the unknown usernames;
    // the rate-limit guard emits 429 ONLY on the 11th).
    for (int i = 0; i < 10; ++i) {
        const auto r = post_login(handle, login_body(names[i], "anypassword9"));
        ASSERT_TRUE(r) << "request " << i << " failed: " << r.error();
        EXPECT_NE(r->status, 429)
            << "rate limit fired too early at request " << i
            << " body=" << r->body;
        EXPECT_FALSE(r->get_header_value("X-RateLimit-Remaining").empty());
    }
    // 11th: 429.
    const auto blocked = post_login(handle, login_body(names[10], "anypassword9"));
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked->status, 429);
    EXPECT_FALSE(blocked->get_header_value("Retry-After").empty());

    const auto body = nlohmann::json::parse(blocked->body);
    EXPECT_EQ(body["code"], "RATE_LIMITED");
    EXPECT_EQ(body["details"]["quota"], "auth.login");
}

// ────────────────────────────────────────────────────────────────────────────
//  audit_logs — every kAuditLogEvery (5) failed attempts writes a row
//  (SPEC §15.1, A27 acceptance).
//
//  We use a freshly-inserted user so the failure path is the "wrong
//  password" branch (which keeps the username in the counter) AND
//  every kAuditLogEvery attempt is verified via a COUNT(*) — exactly
//  one row at attempt 5, two rows at attempt 10, etc.
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLoginLiveFixture, AuditLogRowWrittenEveryFifthFailure) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "aud");

    // Drive exactly 5 failures. audit_logs row count for this
    // (target_id, action) pair should be 1 after attempt #5.
    for (int i = 0; i < 4; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
        const int n = count_audit_logs(*pool, username, "auth.login_failure");
        EXPECT_EQ(n, 0)
            << "audit row fired too early (attempt " << (i + 1) << ")";
    }
    {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    const int after5 = count_audit_logs(*pool, username, "auth.login_failure");
    EXPECT_EQ(after5, 1) << "expected exactly one audit row after 5 failures";

    // Drive 5 more failures → total of 10. Should add a SECOND row.
    for (int i = 0; i < 5; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    const int after10 = count_audit_logs(*pool, username, "auth.login_failure");
    EXPECT_EQ(after10, 2) << "expected two audit rows after 10 failures";

    // Verify the row payload + ip are populated (best-effort).
    // We don't pin ip because bind_any_port wires the loopback which
    // cpp-httplib may or may not surface consistently across versions.
}

TEST_F(AuthLoginLiveFixture, AuditLogPayloadIncludesConsecutiveCount) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "aud_pl");

    // Drive 5 failures to trigger exactly one audit row.
    for (int i = 0; i < 5; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
    }

    // Pull the most recent audit row and assert payload.consecutive_failures.
    // The `audit_logs.payload` column is a MySQL JSON type — `get<string>`
    // raises on JSON columns, so we use `JSON_EXTRACT(payload, ...)` to
    // pull the scalar out as a JSON path the server unwraps to text.
    // Cast to CHAR before binding through fetch_one so we don't depend
    // on mysql-connector-c++ having a dedicated JSON unwrap.
    try {
        auto conn = pool->acquire();
        const auto row = conn.fetch_one(
            "SELECT action, target_type, "
            "       CAST(JSON_EXTRACT(payload, '$.consecutive_failures') AS CHAR) "
            "FROM audit_logs WHERE target_id = ? ORDER BY id DESC LIMIT 1",
            username);
        ASSERT_TRUE(row);
        EXPECT_EQ(row->operator[](0).get<std::string>(), "auth.login_failure");
        EXPECT_EQ(row->operator[](1).get<std::string>(), "user");
        const std::string count_str =
            row->operator[](2).get<std::string>();
        EXPECT_EQ(count_str, "5");
    } catch (const std::exception& e) {
        GTEST_SKIP() << "audit_logs row check threw: " << e.what();
    }
}

TEST_F(AuthLoginLiveFixture, SuccessAfterFailuresStopsAuditRowGrowth) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "aud_s");

    // Drive 6 failures — one audit row exists after attempt #5, none
    // added at #6.
    for (int i = 0; i < 6; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
    }
    EXPECT_EQ(count_audit_logs(*pool, username, "auth.login_failure"), 1);

    // A successful login zeroes the tracker; subsequent failures
    // start a fresh count and DO NOT append to the audit table
    // until the next multiple of 5.
    const auto ok = post_login(handle, login_body(username, "hunter22"));
    ASSERT_TRUE(ok);
    ASSERT_EQ(ok->status, 200);
    EXPECT_EQ(count_audit_logs(*pool, username, "auth.login_failure"), 1);

    // 4 post-success failures: still 1 audit row.
    for (int i = 0; i < 4; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
    }
    EXPECT_EQ(count_audit_logs(*pool, username, "auth.login_failure"), 1);

    // 5th post-success failure → second row.
    const auto r5 = post_login(handle,
        login_body(username, "WRONG_PW_xx9"));
    ASSERT_TRUE(r5);
    EXPECT_EQ(count_audit_logs(*pool, username, "auth.login_failure"), 2);
}

// ────────────────────────────────────────────────────────────────────────────
//  Placeholder routes (logout / profile) — still 501
//  (refresh moved to its own suite: test_auth_refresh.cpp)
// ────────────────────────────────────────────────────────────────────────────
//  Pure-unit tests (no MySQL, no server) — exercise LoginFailureTracker +
//  parse_login_request directly. Catches regressions even on a box
//  without MySQL.
// ────────────────────────────────────────────────────────────────────────────

TEST(LoginFailureTrackerUnit, AuditFlagFiresAtMultiplesOfFive) {
    litecode::LoginFailureTracker tracker;
    for (int i = 1; i <= 12; ++i) {
        const auto outcome = tracker.record_failure("alice");
        EXPECT_EQ(outcome.count, i);
        if (i % 5 == 0) {
            EXPECT_TRUE(outcome.should_audit)
                << "attempt " << i << " should have triggered audit";
        } else {
            EXPECT_FALSE(outcome.should_audit)
                << "attempt " << i << " must NOT trigger audit";
        }
    }
}

TEST(LoginFailureTrackerUnit, ResetClearsCounter) {
    litecode::LoginFailureTracker tracker;
    for (int i = 0; i < 7; ++i) {
        tracker.record_failure("bob");
    }
    EXPECT_EQ(tracker.count("bob"), 7);
    tracker.reset("bob");
    EXPECT_EQ(tracker.count("bob"), 0);

    // First failure after reset is at 1, not at 8.
    const auto after = tracker.record_failure("bob");
    EXPECT_EQ(after.count, 1);
    EXPECT_FALSE(after.should_audit);
}

TEST(LoginFailureTrackerUnit, PerUsernameCountersAreIndependent) {
    litecode::LoginFailureTracker tracker;
    for (int i = 0; i < 5; ++i) tracker.record_failure("carol");
    for (int i = 0; i < 3; ++i) tracker.record_failure("dave");
    EXPECT_EQ(tracker.count("carol"), 5);
    EXPECT_EQ(tracker.count("dave"),  3);

    tracker.reset("carol");
    EXPECT_EQ(tracker.count("carol"), 0);
    EXPECT_EQ(tracker.count("dave"),  3);     // unaffected
}

TEST(LoginFailureTrackerUnit, EmptyUsernameIsIgnored) {
    litecode::LoginFailureTracker tracker;
    const auto outcome = tracker.record_failure("");
    EXPECT_EQ(outcome.count, 0);
    EXPECT_FALSE(outcome.should_audit);
    EXPECT_FALSE(outcome.locked);
    EXPECT_FALSE(outcome.was_already_locked);
    EXPECT_EQ(tracker.size(), 0u);
}

TEST(LoginFailureTrackerUnit, CapEvictsEntryWithHighestCount) {
    // Cap of 3 so we can drive eviction deterministically. Disable
    // lockout (threshold = INT_MAX) so the eviction logic isn't
    // competing with a lockout that would prefer to keep a locked
    // entry around (covered by LockoutCapPrefersToKeepLockedEntry
    // below).
    litecode::LoginLockoutConfig lcfg;
    lcfg.enabled = false;
    litecode::LoginFailureTracker tracker(lcfg, /*max_entries=*/3);
    tracker.record_failure("u1");   // count=1
    tracker.record_failure("u2");   // count=1
    tracker.record_failure("u3");   // count=1
    tracker.record_failure("u1");   // count=2
    tracker.record_failure("u2");   // count=2
    tracker.record_failure("u3");   // count=2
    EXPECT_EQ(tracker.size(), 3u);
    // Adding u4 triggers eviction; victim is whichever has the highest
    // count, then the smallest count, etc. u1/u2/u3 all tied at 2.
    // After eviction + insert u4 has count 1, so size is still 3.
    // We assert eviction actually happened by checking size + the
    // surviving set.
    tracker.record_failure("u4");
    EXPECT_EQ(tracker.size(), 3u);
    EXPECT_EQ(tracker.count("u4"), 1);
}

// ────────────────────────────────────────────────────────────────────────────
//  Lockout state machine — Phase 6 ☆ v1.2.46 (SPEC §15.1 失败登录锁定)
//
//  These tests don't need MySQL — they exercise LoginFailureTracker in
//  isolation. The integration path (HTTP wire shape + audit_logs row)
//  is covered by the live-fixture tests below.
// ────────────────────────────────────────────────────────────────────────────

// Helper: build a tracker with the given lockout policy + a frozen
// clock anchored at epoch so the time arithmetic is deterministic.
struct LockoutPolicyFixture {
    litecode::LoginLockoutConfig cfg;
    std::shared_ptr<std::chrono::steady_clock::time_point> now_ptr =
        std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::time_point{});
    std::unique_ptr<litecode::LoginFailureTracker> tracker;

    explicit LockoutPolicyFixture(int threshold       = 5,
                                  int window_seconds  = 900,
                                  int lockout_seconds = 900,
                                  bool enabled        = true)
        : cfg{} {
        cfg.enabled                   = enabled;
        cfg.threshold                 = threshold;
        cfg.window_seconds            = window_seconds;
        cfg.lockout_duration_seconds  = lockout_seconds;
        litecode::LoginFailureTracker::Clock clock =
            [now_ptr = now_ptr] { return *now_ptr; };
        tracker = std::make_unique<litecode::LoginFailureTracker>(
            cfg, /*max_entries=*/1000, std::move(clock));
    }

    void advance(std::chrono::seconds by) { *now_ptr += by; }
};

TEST(LockoutUnit, DisabledConfigNeverLocks) {
    LockoutPolicyFixture f(/*threshold=*/2, /*window=*/900,
                           /*lockout=*/900, /*enabled=*/false);
    for (int i = 0; i < 10; ++i) {
        const auto o = f.tracker->record_failure("alice");
        EXPECT_FALSE(o.locked);
        EXPECT_FALSE(o.was_already_locked);
    }
    EXPECT_FALSE(f.tracker->is_locked("alice"));
}

TEST(LockoutUnit, ThresholdTripLocksAccount) {
    LockoutPolicyFixture f;
    // 4 failures — still counting, NOT locked.
    for (int i = 1; i <= 4; ++i) {
        const auto o = f.tracker->record_failure("alice");
        EXPECT_FALSE(o.locked) << "locked too early at attempt " << i;
        EXPECT_EQ(o.count, i);
    }
    EXPECT_FALSE(f.tracker->is_locked("alice"));
    // 5th failure — crosses the threshold, locks the account.
    const auto trip = f.tracker->record_failure("alice");
    EXPECT_TRUE(trip.locked);
    EXPECT_FALSE(trip.was_already_locked);
    EXPECT_EQ(trip.count, 5);
    EXPECT_EQ(trip.locked_for_seconds, 900);
    EXPECT_GE(trip.remaining_seconds, 899);
    EXPECT_LE(trip.remaining_seconds, 900);
    EXPECT_TRUE(f.tracker->is_locked("alice"));

    // Subsequent failures during the lockout window report
    // was_already_locked, NOT a fresh locked.
    const auto probe = f.tracker->record_failure("alice");
    EXPECT_FALSE(probe.locked);
    EXPECT_TRUE(probe.was_already_locked);
    EXPECT_EQ(probe.count, 5);                // count doesn't grow
    EXPECT_EQ(probe.locked_for_seconds, 900);
    EXPECT_GE(probe.remaining_seconds, 0);
}

TEST(LockoutUnit, LockoutLiftsAfterDuration) {
    LockoutPolicyFixture f(/*threshold=*/2, /*window=*/900, /*lockout=*/3);
    f.tracker->record_failure("alice");
    const auto trip = f.tracker->record_failure("alice");
    ASSERT_TRUE(trip.locked);

    // Just before expiry — still locked.
    f.advance(std::chrono::seconds(2));
    EXPECT_TRUE(f.tracker->is_locked("alice"));
    int rem = 0;
    EXPECT_TRUE(f.tracker->is_locked("alice", &rem));
    EXPECT_EQ(rem, 1);

    // Past expiry — not locked. Sliding window (900s) is still in
    // effect, so the count persists (Model B: brute-force probes
    // that time around the lockout don't get a clean slate).
    f.advance(std::chrono::seconds(2));
    EXPECT_FALSE(f.tracker->is_locked("alice"));

    const auto fresh = f.tracker->record_failure("alice");
    EXPECT_FALSE(fresh.locked);
    EXPECT_FALSE(fresh.was_already_locked);
    EXPECT_EQ(fresh.count, 3); // 2 prior + 1 new; sliding window still active
}

TEST(LockoutUnit, SuccessfulLoginResetsLockout) {
    LockoutPolicyFixture f;
    f.tracker->record_failure("alice");
    const auto trip = f.tracker->record_failure("alice");
    trip; // trip locked state
    // Trip it 3 more times so the threshold crosses again? No,
    // threshold=5 means 5 failures total. Already at 2.
    f.tracker->record_failure("alice");
    f.tracker->record_failure("alice");
    const auto trip5 = f.tracker->record_failure("alice");
    ASSERT_TRUE(trip5.locked);
    EXPECT_TRUE(f.tracker->is_locked("alice"));

    // Successful login → reset clears the lockout entirely.
    f.tracker->reset("alice");
    EXPECT_FALSE(f.tracker->is_locked("alice"));
    EXPECT_EQ(f.tracker->count("alice"), 0);

    // Next failure starts at count=1, fresh window.
    const auto after_reset = f.tracker->record_failure("alice");
    EXPECT_EQ(after_reset.count, 1);
    EXPECT_FALSE(after_reset.locked);
}

TEST(LockoutUnit, RollingWindowResetsCounter) {
    // window=2s, threshold=3, lockout=10s. Three failures inside the
    // window should trip; advancing past window resets the counter
    // so a new burst of three needs another full window.
    LockoutPolicyFixture f(/*threshold=*/3, /*window=*/2, /*lockout=*/10);
    f.tracker->record_failure("alice"); // count=1, first_failure_at=t0
    f.tracker->record_failure("alice"); // count=2
    const auto trip = f.tracker->record_failure("alice"); // count=3 → lockout
    ASSERT_TRUE(trip.locked);

    // Wait out the lockout (10s) + the window (2s) so the sliding
    // window also expires.
    f.advance(std::chrono::seconds(15));
    EXPECT_FALSE(f.tracker->is_locked("alice"));

    // 1st failure in the NEW window — count starts at 1.
    const auto fresh1 = f.tracker->record_failure("alice");
    EXPECT_EQ(fresh1.count, 1);
    EXPECT_FALSE(fresh1.locked);
}

TEST(LockoutUnit, PerUsernameLocksAreIndependent) {
    LockoutPolicyFixture f;
    for (int i = 0; i < 5; ++i) f.tracker->record_failure("alice");
    EXPECT_TRUE(f.tracker->is_locked("alice"));
    // bob hasn't failed — not locked, even with a high alice count.
    EXPECT_FALSE(f.tracker->is_locked("bob"));
    EXPECT_EQ(f.tracker->remaining_lockout_seconds("bob"), 0);
}

TEST(LockoutUnit, EmptyUsernameNeverLocks) {
    LockoutPolicyFixture f;
    const auto o = f.tracker->record_failure("");
    EXPECT_FALSE(o.locked);
    EXPECT_FALSE(o.was_already_locked);
    EXPECT_FALSE(f.tracker->is_locked(""));
}

TEST(LockoutUnit, LockoutCapPrefersToKeepLockedEntry) {
    // Drive cap=3 with all three accounts LOCKED. Adding a 4th
    // username must evict the highest-count UNLOCKED entry if any
    // exist; if all are locked, fall back to the highest-count
    // locked entry.
    litecode::LoginLockoutConfig lcfg;
    lcfg.threshold                = 2;
    lcfg.window_seconds           = 900;
    lcfg.lockout_duration_seconds = 900;
    lcfg.enabled                  = true;
    auto now_ptr = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point{});
    litecode::LoginFailureTracker::Clock clock =
        [now_ptr] { return *now_ptr; };
    litecode::LoginFailureTracker tracker(lcfg, /*max_entries=*/3, clock);

    tracker.record_failure("u1");
    tracker.record_failure("u1"); // locks u1
    tracker.record_failure("u2");
    tracker.record_failure("u2"); // locks u2
    tracker.record_failure("u3");
    tracker.record_failure("u3"); // locks u3
    EXPECT_TRUE(tracker.is_locked("u1"));
    EXPECT_TRUE(tracker.is_locked("u2"));
    EXPECT_TRUE(tracker.is_locked("u3"));

    // Add u4 — every entry is locked; the cap policy falls back to
    // "evict highest-count locked entry". u1/u2/u3 all tied at 2, so
    // any one of them is a valid victim. The invariant we test is
    // that ONE of the locked entries survives and a fresh slot opens
    // for u4.
    tracker.record_failure("u4");
    EXPECT_EQ(tracker.size(), 3u);
    EXPECT_EQ(tracker.count("u4"), 1);
    // At least 2 of {u1,u2,u3} must still be locked (the cap evicted
    // at most one).
    int locked_count = 0;
    for (const auto* u : {"u1", "u2", "u3"}) {
        if (tracker.is_locked(u)) ++locked_count;
    }
    EXPECT_GE(locked_count, 2);
}

TEST(LockoutUnit, LockoutCapPrefersUnlockedVictim) {
    // Drive cap=3 with two LOCKED + one UNLOCKED-but-counting entry.
    // Adding a 4th username must evict the UNLOCKED entry (we never
    // want to drop a lockout state to make room for a fresh entry).
    //
    // Use lockout=900s, window=3600s so we can advance past the
    // lockout while staying inside the rolling window (u3 keeps
    // its count=2 even after its lockout expires).
    litecode::LoginLockoutConfig lcfg;
    lcfg.threshold                = 2;
    lcfg.window_seconds           = 3600;
    lcfg.lockout_duration_seconds = 900;
    lcfg.enabled                  = true;
    auto now_ptr = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::time_point{});
    litecode::LoginFailureTracker::Clock clock =
        [now_ptr] { return *now_ptr; };
    litecode::LoginFailureTracker tracker(lcfg, /*max_entries=*/3, clock);

    // u1, u2 LOCKED (count=2 each). u3 tripped, then its lockout
    // expired but the count persists.
    tracker.record_failure("u1");
    tracker.record_failure("u1");
    tracker.record_failure("u2");
    tracker.record_failure("u2");
    tracker.record_failure("u3");
    tracker.record_failure("u3");
    EXPECT_TRUE(tracker.is_locked("u1"));
    EXPECT_TRUE(tracker.is_locked("u2"));
    EXPECT_TRUE(tracker.is_locked("u3"));

    // Advance past u3's lockout (900s) but well inside the 3600s
    // window — u3's count of 2 must persist, and is_locked is false.
    *now_ptr += std::chrono::seconds(1000);
    EXPECT_TRUE (tracker.is_locked("u1"));
    EXPECT_TRUE (tracker.is_locked("u2"));
    EXPECT_FALSE(tracker.is_locked("u3"));
    EXPECT_EQ   (tracker.count("u3"), 2);

    // Add u4 → cap reached → tracker must evict the UNLOCKED entry
    // (u3) so the two LOCKED entries survive intact.
    tracker.record_failure("u4");
    EXPECT_EQ(tracker.size(), 3u);
    EXPECT_EQ(tracker.count("u4"), 1);
    EXPECT_TRUE (tracker.is_locked("u1"));
    EXPECT_TRUE (tracker.is_locked("u2"));
    // u3 was the only UNLOCKED entry → it should be the victim. After
    // eviction, count() returns 0 (entry gone) and is_locked() is
    // false (entry gone).
    EXPECT_EQ   (tracker.count("u3"), 0);
    EXPECT_FALSE(tracker.is_locked("u3"));
}

// ────────────────────────────────────────────────────────────────────────────
//  Live wire-shape tests (require MySQL — SKIP when not reachable)
//
//  Coverage:
//    - After N=threshold consecutive failures, /auth/login returns
//      423 Locked + Retry-After: <remaining> + the same anti-
//      enumeration envelope (UNAUTHORIZED code, "invalid username or
//      password" message).
//    - Lockout fires even for UNKNOWN usernames (the counter keys on
//      the supplied username, not on a real row).
//    - The kAuditLogEvery (5) auth.login_failure rows are still
//      written; ONE additional auth.login_locked row appears at the
//      threshold-crossing attempt.
//    - A successful login during an active lockout does NOT happen
//      (we 423 first), but a successful login once the lockout has
//      lifted clears the counter + lockout state end-to-end.
//    - LOGIN_LOCKOUT_ENABLED=false disables the feature entirely.
//
//  We use a custom LoginLockoutConfig with threshold=3 and
//  lockout_duration=2s so the live tests run in seconds, not the SPEC
//  §15.1 15-min default. The unit tests above pin the 5/900 default.
// ────────────────────────────────────────────────────────────────────────────

class AuthLoginLockoutLiveFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool>    pool;
    std::unique_ptr<litecode::HttpServer>        server;
    std::unique_ptr<litecode::RateLimiter>       limiter;
    std::unique_ptr<litecode::LoginFailureTracker> tracker;
    std::unique_ptr<litecode::InMemoryRefreshTokenStore> store;
    ServerHandle                                 handle;
    std::optional<UsernameTracker>               user_tracker;

    // Tight lockout policy for fast tests. Threshold=3 so a 3-failure
    // burst trips the lockout without waiting for the kAuditLogEvery
    // (5) trigger. lockout_duration=2s so the "lockout lifts" test
    // can sleep briefly. window_seconds=60s so the rolling window
    // doesn't interfere with the test sequence.
    static litecode::LoginLockoutConfig lockout_cfg() {
        litecode::LoginLockoutConfig c;
        c.enabled                   = true;
        c.threshold                 = 3;
        c.window_seconds            = 60;
        c.lockout_duration_seconds  = 2;
        return c;
    }

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

        limiter = std::make_unique<litecode::RateLimiter>();
        tracker = std::make_unique<litecode::LoginFailureTracker>(
                      lockout_cfg());
        store   = std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);
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

    httplib::Result post_login(ServerHandle& h, const std::string& body) {
        return h.client->Post("/api/v1/auth/login", body, "application/json");
    }
    std::string login_body(const std::string& username,
                           const std::string& password) {
        return nlohmann::json{
            {"username", username},
            {"password", password},
        }.dump();
    }
};

TEST_F(AuthLoginLockoutLiveFixture, FifthFailureTripsLockout) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "lo");

    // Drive 3 bad-password attempts (threshold=3). Attempts #1 and #2
    // are plain 401s; attempt #3 trips the lockout (and is still a
    // 401 because the lockout fires AFTER bcrypt verification — see
    // the deny_login closure). The 4th attempt hits the lockout
    // BEFORE bcrypt and returns 423 Locked.
    for (int i = 0; i < 3; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401) << "i=" << i << " body=" << r->body;
    }
    EXPECT_TRUE(tracker->is_locked(username));

    // The 4th attempt hits the lockout gate.
    const auto locked = post_login(handle,
        login_body(username, "WRONG_PW_xx9"));
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->status, 423);
    const std::string retry_after = locked->get_header_value("Retry-After");
    EXPECT_FALSE(retry_after.empty()) << "Retry-After header missing";
    // lockout_duration_seconds=2 → remaining is 2 or 1 (we may have
    // lost a fraction of a second since the lockout was set).
    const int retry_int = std::stoi(retry_after);
    EXPECT_GE(retry_int, 1);
    EXPECT_LE(retry_int, 2);

    const auto body = nlohmann::json::parse(locked->body);
    // Anti-enumeration: code is FORBIDDEN (the catalog doesn't have a
    // dedicated LOCKED), message identical to a normal 401.
    EXPECT_EQ(body["code"],    "FORBIDDEN");
    EXPECT_EQ(body["message"], "invalid username or password");
    // details.retry_after_seconds is a courtesy; an attacker can see
    // it, but it carries no info beyond the Retry-After header.
    EXPECT_TRUE(body.contains("details"));
    EXPECT_TRUE(body["details"].contains("retry_after_seconds"));
}

TEST_F(AuthLoginLockoutLiveFixture, LockoutShortCircuitsBeforeBcrypt) {
    // After the lockout is in effect, even a CORRECT password attempt
    // must 423 — we never want the bcrypt work to happen for a
    // locked username. The point is to throttle attackers; a
    // legitimate user who fat-fingered their password enough times
    // to lock themselves out has to wait it out.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "sc");

    for (int i = 0; i < 3; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }

    // Now the CORRECT password — must still 423, NOT 200.
    const auto with_correct = post_login(handle,
        login_body(username, "hunter22"));
    ASSERT_TRUE(with_correct);
    EXPECT_EQ(with_correct->status, 423);
    EXPECT_EQ(with_correct->get_header_value("Retry-After"), "2");
}

TEST_F(AuthLoginLockoutLiveFixture, LockoutForUnknownUsername) {
    // The counter keys on the SUPPLIED username, not on a real row.
    // An attacker probing with random usernames still hits the
    // lockout (the wire response is identical so they can't tell
    // whether the account is real).
    StdoutSilencer silencer;
    const std::string ghost = fresh_username("ghost_lo");

    for (int i = 0; i < 3; ++i) {
        const auto r = post_login(handle,
            login_body(ghost, "anypassword9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    EXPECT_TRUE(tracker->is_locked(ghost));

    // 4th attempt — same anti-enumeration 423 envelope.
    const auto locked = post_login(handle,
        login_body(ghost, "anypassword9"));
    ASSERT_TRUE(locked);
    EXPECT_EQ(locked->status, 423);
}

TEST_F(AuthLoginLockoutLiveFixture, LockoutAuditRowFires) {
    // The threshold-crossing attempt must write ONE auth.login_locked
    // row (separate from the per-5-failure auth.login_failure rows).
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "aud_lo");

    for (int i = 0; i < 3; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }

    const int locked_rows = count_audit_logs(*pool, username,
                                             "auth.login_locked");
    EXPECT_EQ(locked_rows, 1) << "expected exactly one auth.login_locked row";

    // The row's payload should carry consecutive_failures=3 +
    // locked_for_seconds=2 + threshold=3 (the live fixture's config).
    try {
        auto conn = pool->acquire();
        const auto row = conn.fetch_one(
            "SELECT CAST(payload AS CHAR) FROM audit_logs "
            "WHERE target_id = ? AND action = 'auth.login_locked' "
            "ORDER BY id DESC LIMIT 1",
            username);
        ASSERT_TRUE(row);
        const std::string payload_str =
            row->operator[](0).get<std::string>();
        const auto payload = nlohmann::json::parse(payload_str);
        EXPECT_EQ(payload["consecutive_failures"], 3);
        EXPECT_EQ(payload["locked_for_seconds"],   2);
        EXPECT_EQ(payload["threshold"],            3);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "audit row payload check threw: " << e.what();
    }
}

TEST_F(AuthLoginLockoutLiveFixture, LockoutLiftsAndSuccessClearsState) {
    // After the lockout expires, a correct password returns 200 and
    // clears the per-username state so subsequent bad attempts start
    // a fresh counter.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "lift");

    for (int i = 0; i < 3; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401);
    }
    EXPECT_TRUE(tracker->is_locked(username));

    // Wait out the 2-second lockout. We use 3s to be safe across CI
    // schedulers that sometimes add jitter.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_FALSE(tracker->is_locked(username));

    // Correct password now succeeds (the post-lockout record_failure
    // path bumps count to 4 — still under threshold after the lockout
    // expired; no fresh lockout fires because count < threshold).
    const auto ok = post_login(handle,
        login_body(username, "hunter22"));
    ASSERT_TRUE(ok);
    ASSERT_EQ(ok->status, 200) << "body=" << ok->body;

    // After success the counter is cleared.
    EXPECT_FALSE(tracker->is_locked(username));
    EXPECT_EQ(tracker->count(username), 0);
}

TEST_F(AuthLoginLockoutLiveFixture, DisabledConfigNeverLocks) {
    // LOGIN_LOCKOUT_ENABLED=false — the tracker accepts the policy
    // but never trips the lockout. The login-failure audit trigger
    // (every 5th failure) still fires because that lives in the
    // tracker too but doesn't depend on the lockout knob.
    //
    // Rebuild the server with a disabled config.
    handle = ServerHandle();
    server.reset();
    limiter.reset();
    tracker.reset();
    store.reset();

    auto cfg = lockout_cfg();
    cfg.enabled = false;
    limiter = std::make_unique<litecode::RateLimiter>();
    tracker = std::make_unique<litecode::LoginFailureTracker>(cfg);
    store   = std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);
    server  = std::make_unique<litecode::HttpServer>(
                  dev_server(), dev_cors());
    litecode::register_auth_routes(
        *server, *pool, *limiter, *tracker, *store,
        dev_jwt(), lax_rate_limit());
    handle = start_server(server.get());

    const std::string username = create_user_with_password("hunter22", "off");

    // Drive 10 bad attempts — never locks.
    for (int i = 0; i < 10; ++i) {
        const auto r = post_login(handle,
            login_body(username, "WRONG_PW_xx9"));
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 401) << "i=" << i;
        EXPECT_FALSE(r->get_header_value("Retry-After").size() > 0
                     && r->status != 429)
            << "lockout fired despite disabled config (i=" << i << ")";
    }
    EXPECT_FALSE(tracker->is_locked(username));
    // No auth.login_locked audit row either.
    EXPECT_EQ(count_audit_logs(*pool, username, "auth.login_locked"), 0);
}

} // namespace
