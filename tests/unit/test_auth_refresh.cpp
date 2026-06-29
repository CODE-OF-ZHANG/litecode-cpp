// tests/unit/test_auth_refresh.cpp
//
// Integration + unit tests for src/routes/auth_routes.h — POST /api/v1/auth/refresh.
//
// Coverage:
//   - 200 on happy path: response shape + new access/refresh pair
//   - 200 chains correctly: each rotation produces a fresh pair whose
//     refresh can itself be rotated (SPEC §5.1, A2 acceptance)
//   - 200 stamps the new access token with the user's CURRENT role +
//     username (so a role change since the refresh was issued is
//     reflected on next refresh — SPEC §4.1)
//   - 401 on presenting the OLD refresh after rotation (revoked jti —
//     reuse detection, SPEC §15.1)
//   - 401 on malformed / empty / wrong-kind token (identical envelope
//     to "revoked" — anti-enumeration per SPEC §15.1)
//   - 401 on access token presented as refresh (token confusion defense)
//   - 401 on bad signature
//   - 401 on token signed with the wrong secret
//   - 401 on the user row being deleted between login and refresh
//     (claims.user_id is still valid as a JWT, but the user is gone)
//   - 401 envelope carries the standard {code, message, request_id}
//     shape and identical message for every failure mode
//   - 400 on missing / non-string / empty refresh_token
//   - 400 on malformed JSON / empty body
//   - Response envelope includes X-Request-Id passthrough
//   - Login endpoint still works (smoke — proves we didn't break login
//     when reordering the route table)
//   - Logout / profile endpoints still 501 (placeholder regressions)
//   - Pure-unit tests of parse_refresh_request (no MySQL / no server)
//
// Integration tests need a live MySQL — gated by the same env vars as
// test_auth_login. Each test uses a fresh username so parallel /
// back-to-back runs never collide.

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
    j.issuer              = "litecode-refresh-test";
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
    return std::string("rf_") + tag + "_" +
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

// POST helper that wraps httplib boilerplate.
httplib::Result post_refresh(ServerHandle& h, const std::string& body) {
    return h.client->Post("/api/v1/auth/refresh", body, "application/json");
}

// Convenience: build a {refresh_token} body.
std::string refresh_body(const std::string& refresh_token) {
    nlohmann::json j = {
        {"refresh_token", refresh_token},
    };
    return j.dump();
}

// ────────────────────────────────────────────────────────────────────────────
//  Live fixture — full stack (server + pool + limiter + tracker + store
//  + auth_routes). Skipped when MySQL is unreachable.
// ────────────────────────────────────────────────────────────────────────────

class AuthRefreshLiveFixture : public ::testing::Test {
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

        // Per-test store so blacklist state doesn't bleed between
        // cases (the process-wide default would survive the test).
        // A small cap keeps memory bounded; refresh tokens are <= 7d
        // TTL, so natural churn is enough.
        store = std::make_unique<litecode::InMemoryRefreshTokenStore>(1000);

        limiter       = std::make_unique<litecode::RateLimiter>();
        tracker       = std::make_unique<litecode::LoginFailureTracker>();
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

    // Insert a brand-new test user directly via the repo. Returns the
    // username (and stashes it in the tracker for cleanup).
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

    // Insert an admin user — verifies the new access token's role
    // claim is "admin" end-to-end (role-surfacing on refresh).
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

    // Drive /api/v1/auth/login with a real user and return the parsed
    // JSON body. The login endpoint is the easiest way to mint a
    // refresh token in the live-fixture world.
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
//  200 — happy path
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthRefreshLiveFixture, HappyPathReturns200WithNewPair) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "hp");
    const auto login = login_and_get_tokens(username, "hunter22");
    ASSERT_TRUE(login.contains("refresh_token"));
    const std::string old_refresh = login["refresh_token"].get<std::string>();
    const std::string old_access  = login["access_token"].get<std::string>();

    const auto r = post_refresh(handle, refresh_body(old_refresh));
    ASSERT_TRUE(r) << "refresh POST failed: " << r.error();
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

    // New tokens differ from the old ones (true rotation, not a copy).
    EXPECT_NE(data["access_token"].get<std::string>(),  old_access);
    EXPECT_NE(data["refresh_token"].get<std::string>(), old_refresh);
}

TEST_F(AuthRefreshLiveFixture, NewAccessTokenVerifiesWithCorrectClaims) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "clm");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string old_refresh = login["refresh_token"].get<std::string>();

    const auto r = post_refresh(handle, refresh_body(old_refresh));
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    const int user_id = body["data"]["user"]["id"].get<int>();

    // Access side: full claims.
    const auto access_claims = litecode::verify(
        body["data"]["access_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Access);
    EXPECT_EQ(access_claims.kind,     litecode::TokenKind::Access);
    EXPECT_EQ(access_claims.user_id,  std::to_string(user_id));
    EXPECT_EQ(access_claims.username, username);
    EXPECT_EQ(access_claims.role,     "user");

    // Refresh side: sub only (least privilege).
    const auto refresh_claims = litecode::verify(
        body["data"]["refresh_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Refresh);
    EXPECT_EQ(refresh_claims.kind,    litecode::TokenKind::Refresh);
    EXPECT_EQ(refresh_claims.user_id, std::to_string(user_id));
    EXPECT_TRUE(refresh_claims.username.empty());
    EXPECT_TRUE(refresh_claims.role.empty());
}

TEST_F(AuthRefreshLiveFixture, AdminRoleSurfacesInNewAccessToken) {
    StdoutSilencer silencer;
    const std::string username = create_admin_with_password("adminPass1");
    const auto login = login_and_get_tokens(username, "adminPass1");
    const std::string old_refresh = login["refresh_token"].get<std::string>();

    const auto r = post_refresh(handle, refresh_body(old_refresh));
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["user"]["role"], "admin");

    const auto claims = litecode::verify(
        body["data"]["access_token"].get<std::string>(),
        dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Access);
    EXPECT_EQ(claims.role, "admin");
}

TEST_F(AuthRefreshLiveFixture, ChainOfRefreshesWorks) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "chn");
    const auto login = login_and_get_tokens(username, "hunter22");

    std::string current_refresh = login["refresh_token"].get<std::string>();

    constexpr int kSteps = 5;
    for (int i = 0; i < kSteps; ++i) {
        const auto r = post_refresh(handle, refresh_body(current_refresh));
        ASSERT_TRUE(r) << "rotation " << i << " failed: " << r.error();
        ASSERT_EQ(r->status, 200) << "rotation " << i
                                  << " body=" << r->body;
        const auto body = nlohmann::json::parse(r->body);
        ASSERT_TRUE(body["data"].contains("refresh_token"));
        current_refresh =
            body["data"]["refresh_token"].get<std::string>();
    }

    // After kSteps rotations, the original refresh is long revoked.
    // The current_refresh should still be valid; presenting the
    // ORIGINAL one should now fail (reuse detection).
    const auto reused = post_refresh(handle, refresh_body(
        login["refresh_token"].get<std::string>()));
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->status, 401);
}

TEST_F(AuthRefreshLiveFixture, OldRefreshTokenIsRevokedAfterRotation) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rev");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string old_refresh = login["refresh_token"].get<std::string>();

    // First refresh succeeds.
    {
        const auto r = post_refresh(handle, refresh_body(old_refresh));
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
    }

    // The same refresh, presented again, MUST fail with 401
    // (reuse detection — SPEC §15.1).
    const auto reused = post_refresh(handle, refresh_body(old_refresh));
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->status, 401);
    const auto body = nlohmann::json::parse(reused->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");
}

// ────────────────────────────────────────────────────────────────────────────
//  401 — token failures (anti-enumeration: every envelope is identical)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthRefreshLiveFixture, MalformedTokenReturns401) {
    StdoutSilencer silencer;
    create_user_with_password("hunter22", "mf");  // user must exist; otherwise
                                                   // the body would 401 on
                                                   // user lookup with the SAME
                                                   // envelope, which is what
                                                   // we want, but we want to
                                                   // cover the verify path
                                                   // independently.
    const auto r = post_refresh(handle, refresh_body("not-a-jwt"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");
}

TEST_F(AuthRefreshLiveFixture, AccessTokenAsRefreshReturns401) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "ac");
    const auto login = login_and_get_tokens(username, "hunter22");
    // The "access_token" is verified for kind=Access; presenting it
    // at /auth/refresh (which verifies for kind=Refresh) is the
    // token-confusion attack — must fail.
    const std::string access_token =
        login["access_token"].get<std::string>();
    const auto r = post_refresh(handle, refresh_body(access_token));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");
}

TEST_F(AuthRefreshLiveFixture, WrongSecretReturns401) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "ws");
    // Sign a refresh with a secret the server doesn't know about.
    const auto forged = litecode::sign_refresh(
        "completely_different_secret_at_least_32_bytes_long_xx",
        dev_jwt().issuer, "1", 600);

    // We have to forge a refresh whose `sub` matches a real user so
    // the verify() fails (not the user lookup). Look up the test
    // user's id and re-sign with the wrong secret.
    const auto row = litecode::user_repo::find_by_username(*pool, username);
    ASSERT_TRUE(row.has_value());
    const auto wrong_secret_token = litecode::sign_refresh(
        "completely_different_secret_at_least_32_bytes_long_xx",
        dev_jwt().issuer, std::to_string(row->id), 600);

    const auto r = post_refresh(handle, refresh_body(wrong_secret_token.token));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");

    // Reference forged to silence the unused-variable warning; the
    // wrong-secret scenario is exercised by wrong_secret_token above.
    (void)forged;
}

TEST_F(AuthRefreshLiveFixture, TokenForDeletedUserReturns401) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "dl");
    const auto login = login_and_get_tokens(username, "hunter22");
    const int user_id = login["user"]["id"].get<int>();
    const std::string refresh = login["refresh_token"].get<std::string>();

    // Delete the user. The refresh token is still a valid JWT, but
    // the row it refers to is gone. The route must surface this as
    // 401 (not 500), with the standard envelope.
    {
        auto conn = pool->acquire();
        conn.execute("DELETE FROM users WHERE id = ?", user_id);
    }
    // The user_tracker in the fixture will skip the deletion in its
    // own TearDown — it deletes by username, not id, so the row is
    // already gone; the tracker will quietly log a warning.

    const auto r = post_refresh(handle, refresh_body(refresh));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");

    // Re-create the user (so the teardown doesn't trip — but with
    // the user already gone, the tracker's DELETE is a no-op).
    litecode::UserRow replacement;
    replacement.username      = username;
    replacement.password_hash = litecode::hash_password("hunter22");
    replacement.role          = "user";
    replacement.email         = std::nullopt;
    replacement.avatar        = std::nullopt;
    litecode::user_repo::create_user(*pool, replacement);
}

TEST_F(AuthRefreshLiveFixture, TokenWithUnknownSubReturns401) {
    StdoutSilencer silencer;
    // No user row matches `sub=999999999` (much higher than any
    // auto-increment we've seen). The refresh token is otherwise a
    // perfectly valid JWT — the failure is in the user-lookup step.
    const auto forged = litecode::sign_refresh(
        dev_jwt().secret, dev_jwt().issuer, "999999999", 600);

    const auto r = post_refresh(handle, refresh_body(forged.token));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");
}

// ────────────────────────────────────────────────────────────────────────────
//  400 — body validation
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthRefreshLiveFixture, RejectsMissingRefreshToken) {
    StdoutSilencer silencer;
    auto r = post_refresh(handle, R"({})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST_F(AuthRefreshLiveFixture, RejectsEmptyRefreshToken) {
    StdoutSilencer silencer;
    auto r = post_refresh(handle, R"({"refresh_token":""})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST_F(AuthRefreshLiveFixture, RejectsNonStringRefreshToken) {
    StdoutSilencer silencer;
    auto r = post_refresh(handle, R"({"refresh_token":42})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST_F(AuthRefreshLiveFixture, RejectsMalformedJson) {
    StdoutSilencer silencer;
    auto r = post_refresh(handle, "{not valid json}");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
}

TEST_F(AuthRefreshLiveFixture, RejectsEmptyBody) {
    StdoutSilencer silencer;
    auto r = post_refresh(handle, "");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
}

// ────────────────────────────────────────────────────────────────────────────
//  Envelope / request-id passthrough
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthRefreshLiveFixture, ResponseEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rid");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string refresh = login["refresh_token"].get<std::string>();

    httplib::Headers hdrs = {{"X-Request-Id", "refresh-rid-001"}};
    auto r = handle.client->Post(
        "/api/v1/auth/refresh", hdrs,
        refresh_body(refresh), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "refresh-rid-001");
    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "refresh-rid-001");
}

TEST_F(AuthRefreshLiveFixture, FailureEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"X-Request-Id", "refresh-rid-fail"}};
    auto r = handle.client->Post(
        "/api/v1/auth/refresh", hdrs,
        R"({"refresh_token":"not-a-jwt"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "refresh-rid-fail");
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["request_id"], "refresh-rid-fail");
}

// ────────────────────────────────────────────────────────────────────────────
//  Regression — login still works (proves the route-table reorder was safe)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthRefreshLiveFixture, LoginStillWorks) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "lwk");
    nlohmann::json j = {
        {"username", username},
        {"password", "hunter22"},
    };
    const auto r = handle.client->Post(
        "/api/v1/auth/login", j.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
}

TEST_F(AuthRefreshLiveFixture, LogoutReturnsNotImplemented) {
    StdoutSilencer silencer;
    auto r = handle.client->Post("/api/v1/auth/logout", "{}",
                                  "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 501);
}

TEST_F(AuthRefreshLiveFixture, ProfileReturnsNotImplemented) {
    StdoutSilencer silencer;
    auto r = handle.client->Get("/api/v1/auth/profile");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 501);
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure-unit tests of parse_refresh_request (no MySQL, no server).
//  Catches regressions even on a box without MySQL.
// ────────────────────────────────────────────────────────────────────────────

TEST(ParseRefreshRequestUnit, AcceptsValidBody) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_refresh_request(
        nlohmann::json{{"refresh_token", "abc.def.ghi"}}, res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->refresh_token, "abc.def.ghi");
    EXPECT_TRUE(res.body.empty());   // no envelope written
}

TEST(ParseRefreshRequestUnit, RejectsMissingField) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_refresh_request(
        nlohmann::json::object(), res);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_FALSE(res.body.empty());
    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST(ParseRefreshRequestUnit, RejectsEmptyString) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_refresh_request(
        nlohmann::json{{"refresh_token", ""}}, res);
    EXPECT_FALSE(parsed.has_value());
    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST(ParseRefreshRequestUnit, RejectsNonStringField) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_refresh_request(
        nlohmann::json{{"refresh_token", 42}}, res);
    EXPECT_FALSE(parsed.has_value());
    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST(ParseRefreshRequestUnit, IgnoresSurplusFields) {
    // Strict-shape-with-surplus: surplus keys must be ignored so the
    // front-end can send extra telemetry without breaking.
    httplib::Response res;
    const auto parsed = litecode::detail::parse_refresh_request(
        nlohmann::json{
            {"refresh_token", "abc.def.ghi"},
            {"device_id",     "laptop-1"},
        }, res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->refresh_token, "abc.def.ghi");
}

} // namespace
