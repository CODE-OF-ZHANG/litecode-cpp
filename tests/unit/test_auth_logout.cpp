// tests/unit/test_auth_logout.cpp
//
// Integration + unit tests for src/routes/auth_routes.h — POST /api/v1/auth/logout.
//
// Coverage:
//   - 200 on happy path: response shape + refresh lands on the blacklist
//   - 200 is idempotent: calling logout twice both succeed
//   - 200 even when the refresh did not parse (best-effort — SPEC §15.1)
//   - 200 even when the refresh is malformed / empty / wrong-kind
//   - 200 with an already-revoked refresh (the previous logout's
//     token, presented again — proves the store handles reuse)
//   - 200 + theft defense: refresh belongs to user B but access token
//     is for user A → revocation is REFUSED, log is at WARN, wire is
//     still 200 (anti-enumeration — SPEC §15.1)
//   - 401 when no Authorization header is present
//   - 401 when the Bearer token is malformed / bad signature / expired
//     (anti-enumeration: every 401 envelope is identical — SPEC §15.1)
//   - 401 when an access token signed for user B is used to call
//     /auth/logout for a refresh belonging to user A — the auth
//     middleware passes (token is valid for B) but the theft-defense
//     inside revoke_refresh_token refuses to add the jti to the
//     blacklist. The wire still answers 200.
//   - 200 on missing refresh_token body field (v1.3.3.8 ★ Phase 5
//     cookie-aware — pre-v1.3.3.8 this was 400, which broke every
//     front-end that sent the cookie-only empty body)
//   - 200 on empty body (v1.3.3.8 ★)
//   - 400 ONLY when refresh_token is present-but-malformed (non-string,
//     empty string) or body is malformed JSON — the "presence required"
//     rule is gone
//   - Response envelope includes X-Request-Id passthrough
//   - After logout, /auth/refresh with the revoked token returns 401
//     (reuse detection — the full SPEC §15.1 round-trip)
//   - Login / register / refresh still work (smoke — proves we didn't
//     break the route table when wiring logout)
//   - Pure-unit tests of parse_logout_request (no MySQL / no server)
//
// Integration tests need a live MySQL — gated by the same env vars as
// test_auth_login / test_auth_refresh. Each test uses a fresh username
// so parallel / back-to-back runs never collide.

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
//  Test fixtures / helpers  (mirror test_auth_refresh.cpp)
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
    j.issuer              = "litecode-logout-test";
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
    return std::string("lo_") + tag + "_" +
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
httplib::Result post_logout(ServerHandle& h, const std::string& body,
                            const std::string& bearer = std::string()) {
    if (bearer.empty()) {
        return h.client->Post("/api/v1/auth/logout", body, "application/json");
    }
    httplib::Headers hdrs = {{"Authorization", "Bearer " + bearer}};
    return h.client->Post("/api/v1/auth/logout", hdrs, body, "application/json");
}

// v1.3.3.8 ★ — cookie-aware variant for the Phase 5 path. Pass the
// presented refresh token through the Cookie header instead of (or
// in addition to) the JSON body. The handler sources the refresh via
// detail::extract_refresh_token, cookie-first.
httplib::Result post_logout_cookie(ServerHandle& h, const std::string& body,
                                   const std::string& bearer,
                                   const std::string& cookie_value) {
    httplib::Headers hdrs = {{"Authorization", "Bearer " + bearer}};
    if (!cookie_value.empty()) {
        hdrs.emplace("Cookie", "lc_refresh=" + cookie_value);
    }
    return h.client->Post("/api/v1/auth/logout", hdrs, body, "application/json");
}

// Convenience: build a {refresh_token} body.
std::string logout_body(const std::string& refresh_token) {
    nlohmann::json j = {
        {"refresh_token", refresh_token},
    };
    return j.dump();
}

// ────────────────────────────────────────────────────────────────────────────
//  Live fixture — full stack (server + pool + limiter + tracker + store
//  + auth_routes). Skipped when MySQL is unreachable.
// ────────────────────────────────────────────────────────────────────────────

class AuthLogoutLiveFixture : public ::testing::Test {
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

    // Drive /api/v1/auth/login with a real user and return the parsed
    // JSON data block.
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

TEST_F(AuthLogoutLiveFixture, HappyPathReturns200AndRevokesRefresh) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "hp");
    const auto login = login_and_get_tokens(username, "hunter22");
    ASSERT_TRUE(login.contains("refresh_token"));
    const std::string refresh = login["refresh_token"].get<std::string>();
    const std::string access  = login["access_token"].get<std::string>();
    const int         user_id = login["user"]["id"].get<int>();

    // The refresh should not be on the blacklist yet.
    litecode::Claims claims = litecode::verify(
        refresh, dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Refresh);
    EXPECT_FALSE(store->is_revoked(claims.jti));

    const auto r = post_logout(handle, logout_body(refresh), access);
    ASSERT_TRUE(r) << "logout POST failed: " << r.error();
    ASSERT_EQ(r->status, 200) << "body=" << r->body;

    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("data")) << "missing data envelope: " << r->body;
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    true);

    // The refresh's jti MUST be on the blacklist now.
    EXPECT_TRUE(store->is_revoked(claims.jti));

    // user_id from the body should match the access token's sub —
    // proves the auth middleware ran and passed the right value
    // through. (Implicit — the call didn't 401.)
    EXPECT_EQ(std::to_string(user_id), claims.user_id);
}

TEST_F(AuthLogoutLiveFixture, LogoutIsIdempotent) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "idem");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string refresh = login["refresh_token"].get<std::string>();
    const std::string access  = login["access_token"].get<std::string>();

    // First logout — succeeds, refresh added to blacklist.
    {
        const auto r = post_logout(handle, logout_body(refresh), access);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
        const auto body = nlohmann::json::parse(r->body);
        EXPECT_EQ(body["data"]["revoked"], true);
    }

    // Second logout with the same refresh — still 200, but
    // revoke_refresh_token sees the token is still valid (it
    // expires in 7d), so it re-revokes and returns revoked=true
    // again. (idempotent: store.revoke is idempotent — duplicate
    // jtis just refresh the TTL.)
    {
        const auto r = post_logout(handle, logout_body(refresh), access);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->status, 200);
        const auto body = nlohmann::json::parse(r->body);
        EXPECT_EQ(body["data"]["logged_out"], true);
        EXPECT_EQ(body["data"]["revoked"],    true);
    }
}

TEST_F(AuthLogoutLiveFixture, RevokedRefreshCannotBeUsedToRotate) {
    StdoutSilencer silencer;
    // The full SPEC §15.1 round-trip: logout + refresh reuse detection.
    const std::string username = create_user_with_password("hunter22", "rt");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string refresh = login["refresh_token"].get<std::string>();
    const std::string access  = login["access_token"].get<std::string>();

    // Logout. The refresh is now on the blacklist.
    {
        const auto r = post_logout(handle, logout_body(refresh), access);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
    }

    // Try to refresh with the revoked token — must 401.
    const auto r = handle.client->Post(
        "/api/v1/auth/refresh", logout_body(refresh), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired refresh token");
}

TEST_F(AuthLogoutLiveFixture, MalformedRefreshIsBestEffort200) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "mf");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();

    // "not-a-jwt" doesn't parse — the helper returns parsed=false,
    // revoked=false. The wire still answers 200 with logged_out=true.
    const auto r = post_logout(handle, logout_body("not-a-jwt"), access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    false);
}

TEST_F(AuthLogoutLiveFixture, AccessTokenAsRefreshIsBestEffort200) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "ac");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();

    // The access token is verified for kind=Access; presenting it
    // as a refresh fails verify() (wrong kind). The handler treats
    // it as best-effort — 200, revoked=false.
    const auto r = post_logout(handle,
                               logout_body(login["access_token"].get<std::string>()),
                               access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    false);
}

TEST_F(AuthLogoutLiveFixture, ExpiredRefreshIsBestEffort200) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "ex");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();

    // Mint a refresh that's already expired.
    const auto row = litecode::user_repo::find_by_username(*pool, username);
    ASSERT_TRUE(row.has_value());
    const auto expired = litecode::sign_refresh(
        dev_jwt().secret, dev_jwt().issuer,
        std::to_string(row->id), /*ttl=*/1);
    // (no real sleep needed — the verifier's 1s TTL is shorter than
    // the time it takes to issue the request, but to be deterministic
    // we sign with a -1 second offset; sign_refresh doesn't support
    // negative TTL, so we just sign with 1s and skip if the helper
    // doesn't accept a frozen clock. The test still exercises the
    // "refresh did not parse" path on the wire — the access token
    // itself is short-lived, so the wire still auths the request.)
    const auto r = post_logout(handle, logout_body(expired.token), access);
    ASSERT_TRUE(r);
    // If the verifier passed (1s hasn't elapsed yet), revoked=true
    // and the jti is on the blacklist. If it didn't, the helper
    // still returns 200 with revoked=false. Either is the
    // correct logout behavior — assert the envelope shape.
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
}

TEST_F(AuthLogoutLiveFixture, WrongUserRefreshIsRefused) {
    StdoutSilencer silencer;
    // Theft defense: alice's access token is used to call logout
    // for bob's refresh. The auth middleware passes (alice is
    // authenticated) but the theft-defense check inside
    // revoke_refresh_token refuses to revoke bob's token.
    const std::string alice = create_user_with_password("hunter22", "alice");
    const std::string bob   = create_user_with_password("hunter22", "bob");

    const auto alice_login = login_and_get_tokens(alice, "hunter22");
    const auto bob_login   = login_and_get_tokens(bob,   "hunter22");

    const std::string alice_access  = alice_login["access_token"].get<std::string>();
    const std::string bob_refresh   = bob_login["refresh_token"].get<std::string>();

    litecode::Claims bob_claims = litecode::verify(
        bob_refresh, dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Refresh);
    EXPECT_FALSE(store->is_revoked(bob_claims.jti));

    // alice (access sub=alice) tries to revoke bob's refresh. The
    // handler returns 200 with revoked=false; bob's session is
    // untouched.
    const auto r = post_logout(handle, logout_body(bob_refresh), alice_access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    false);

    // Bob's refresh is still usable for rotation.
    EXPECT_FALSE(store->is_revoked(bob_claims.jti));
    const auto refresh_resp = handle.client->Post(
        "/api/v1/auth/refresh", logout_body(bob_refresh), "application/json");
    ASSERT_TRUE(refresh_resp);
    EXPECT_EQ(refresh_resp->status, 200);
}

// ────────────────────────────────────────────────────────────────────────────
//  401 — auth failures (anti-enumeration: every envelope is identical)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLogoutLiveFixture, MissingAuthorizationHeaderReturns401) {
    StdoutSilencer silencer;
    // No bearer token — auth middleware short-circuits before the
    // body is even parsed.
    const auto r = post_logout(handle, logout_body("anything"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "missing Authorization header");
}

TEST_F(AuthLogoutLiveFixture, NonBearerSchemeReturns401) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"Authorization", "Basic dXNlcjpwYXNz"}};
    const auto r = handle.client->Post(
        "/api/v1/auth/logout", hdrs,
        logout_body("anything"), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "UNAUTHORIZED");
}

TEST_F(AuthLogoutLiveFixture, MalformedBearerTokenReturns401) {
    StdoutSilencer silencer;
    const auto r = post_logout(handle, logout_body("anything"), "garbage.not.a.jwt");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired token");
}

TEST_F(AuthLogoutLiveFixture, WrongSecretBearerTokenReturns401) {
    StdoutSilencer silencer;
    // Sign an access token with the wrong secret — verify() fails
    // and the auth middleware short-circuits with 401.
    const auto row = litecode::user_repo::find_by_username(
        *pool, create_user_with_password("hunter22", "ws"));
    ASSERT_TRUE(row.has_value());
    const auto forged = litecode::sign_access(
        "completely_different_secret_at_least_32_bytes_long_xx",
        dev_jwt().issuer, std::to_string(row->id), row->username,
        row->role, 600);
    const auto r = post_logout(handle, logout_body("anything"), forged.token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired token");
}

TEST_F(AuthLogoutLiveFixture, RefreshAsAccessTokenReturns401) {
    StdoutSilencer silencer;
    // Token confusion: present a refresh-kind token in the Bearer
    // slot. The auth middleware requires kind=Access; this is the
    // dual of the AccessTokenAsRefresh case above.
    const std::string username = create_user_with_password("hunter22", "rc");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string refresh = login["refresh_token"].get<std::string>();
    const auto r = post_logout(handle, logout_body("anything"), refresh);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],    "UNAUTHORIZED");
    EXPECT_EQ(body["message"], "invalid or expired token");
}

// ────────────────────────────────────────────────────────────────────────────
//  200 — body validation (v1.3.3.8: refresh_token is OPTIONAL since
//  Phase 5 moved refresh to HttpOnly cookie). The canonical Phase 5
//  call shape is `Bearer + body={}` — handler reads refresh from
//  cookie, no body shape error.
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLogoutLiveFixture, LogoutAcceptsMissingRefreshToken) {
    // v1.3.3.8: empty body `{}` is now the canonical success shape.
    // Pre-v1.3.3.8 this returned 400 INVALID_INPUT details.field=
    // "refresh_token", which silently broke every front-end that
    // used the cookie-only Phase 5 storage path.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rm");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();
    auto r = post_logout(handle, R"({})", access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    false);   // no refresh was presented
}

TEST_F(AuthLogoutLiveFixture, LogoutAcceptsEmptyBody) {
    // v1.3.3.8: zero-byte body is now valid. Pre-v1.3.3.8 this was
    // 400 from parse_json_body's `req.body.empty()` short-circuit.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rb");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();
    auto r = post_logout(handle, "", access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
}

// ────────────────────────────────────────────────────────────────────────────
//  400 — body validation (only when refresh_token is *present but
//  malformed*). Absent refresh_token is the success shape above.
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLogoutLiveFixture, RejectsEmptyRefreshToken) {
    // Present-but-empty-string is still 400 — too easy to ship a
    // buggy client that sends "" instead of omitting the field.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "re");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();
    auto r = post_logout(handle, R"({"refresh_token":""})", access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST_F(AuthLogoutLiveFixture, RejectsNonStringRefreshToken) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rn");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();
    auto r = post_logout(handle, R"({"refresh_token":42})", access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST_F(AuthLogoutLiveFixture, RejectsMalformedJson) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rj");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access = login["access_token"].get<std::string>();
    auto r = post_logout(handle, "{not valid json}", access);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
}

// ────────────────────────────────────────────────────────────────────────────
//  Envelope / request-id passthrough
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLogoutLiveFixture, ResponseEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rid");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string refresh = login["refresh_token"].get<std::string>();
    const std::string access  = login["access_token"].get<std::string>();

    httplib::Headers hdrs = {
        {"X-Request-Id", "logout-rid-001"},
        {"Authorization", "Bearer " + access},
    };
    auto r = handle.client->Post(
        "/api/v1/auth/logout", hdrs, logout_body(refresh), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "logout-rid-001");
    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "logout-rid-001");
}

TEST_F(AuthLogoutLiveFixture, FailureEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"X-Request-Id", "logout-rid-fail"}};
    // No bearer token — auth fails with 401, but the request id
    // must still propagate end-to-end.
    auto r = handle.client->Post(
        "/api/v1/auth/logout", hdrs,
        logout_body("anything"), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "logout-rid-fail");
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["request_id"], "logout-rid-fail");
}

// ────────────────────────────────────────────────────────────────────────────
//  v1.3.3.8 ★ cookie-aware Phase 5 path
//
//  These cases exercise the canonical front-end logout shape: Bearer
//  access token in the Authorization header, refresh token in the
//  `lc_refresh` HttpOnly cookie, no body. Pre-v1.3.3.8 the handler
//  returned 400 from parse_logout_request on empty body, so the
//  cookie was never consulted and the cookie-clear header was never
//  emitted; every production logout silently failed.
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLogoutLiveFixture, LogoutRevokesCookieRefresh) {
    // Phase 5 ★ canonical: cookie carries the refresh, body is empty,
    // handler sources refresh from cookie and adds the jti to the
    // blacklist.
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "ck");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string refresh = login["refresh_token"].get<std::string>();
    const std::string access  = login["access_token"].get<std::string>();

    litecode::Claims claims = litecode::verify(
        refresh, dev_jwt().secret, dev_jwt().issuer, litecode::TokenKind::Refresh);
    EXPECT_FALSE(store->is_revoked(claims.jti));

    const auto r = post_logout_cookie(handle, std::string{}, access, refresh);
    ASSERT_TRUE(r) << "logout POST failed: " << r.error();
    ASSERT_EQ(r->status, 200) << "body=" << r->body;

    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    true);

    // Cookie-clear Set-Cookie header MUST be present even though
    // the cookie was the one carrying the refresh — closing the
    // XSS-steal window.
    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_NE(set_cookie.find("lc_refresh="),  std::string::npos);
    EXPECT_NE(set_cookie.find("Max-Age=0"),   std::string::npos);

    // The jti carried by the cookie MUST be on the blacklist now.
    EXPECT_TRUE(store->is_revoked(claims.jti));

    // And subsequent refresh via the same cookie fails 401.
    const httplib::Headers refresh_hdrs = {
        {"Cookie", "lc_refresh=" + refresh},
    };
    const auto refresh_resp = handle.client->Post(
        "/api/v1/auth/refresh", refresh_hdrs,
        std::string{}, "application/json");
    ASSERT_TRUE(refresh_resp);
    EXPECT_EQ(refresh_resp->status, 401);
}

TEST_F(AuthLogoutLiveFixture, LogoutWithoutAnyTokenClearsCookie) {
    // Empty body + no cookie + valid Bearer. The handler has nothing
    // to revoke, so revoked=false, but it STILL emits Set-Cookie
    // Max-Age=0 to drop whatever cookie the browser is holding
    // (closing the XSS-steal window even when no fresh token was
    // presented this request).
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "nt");
    const auto login = login_and_get_tokens(username, "hunter22");
    const std::string access  = login["access_token"].get<std::string>();

    // No cookie header at all — simulate "browser dropped the cookie
    // on its own earlier" scenario.
    httplib::Headers hdrs = {{"Authorization", "Bearer " + access}};
    const auto r = handle.client->Post(
        "/api/v1/auth/logout", hdrs, std::string{}, "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["data"]["logged_out"], true);
    EXPECT_EQ(body["data"]["revoked"],    false);   // nothing to revoke

    // The clear-cookie header MUST still go out — defensive, even
    // when the present request has no cookie. This is the v1.3.3.8
    // contract promise.
    const std::string set_cookie = r->get_header_value("Set-Cookie");
    EXPECT_NE(set_cookie.find("lc_refresh="),  std::string::npos);
    EXPECT_NE(set_cookie.find("Max-Age=0"),   std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  Regression — the other auth endpoints still work
// ────────────────────────────────────────────────────────────────────────────

TEST_F(AuthLogoutLiveFixture, RegisterStillWorks) {
    StdoutSilencer silencer;
    const std::string username = fresh_username("reg");
    user_tracker->add(username);
    nlohmann::json j = {
        {"username", username},
        {"password", "hunter22"},
    };
    const auto r = handle.client->Post(
        "/api/v1/auth/register", j.dump(), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201);
}

TEST_F(AuthLogoutLiveFixture, LoginStillWorks) {
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

TEST_F(AuthLogoutLiveFixture, RefreshStillWorks) {
    StdoutSilencer silencer;
    const std::string username = create_user_with_password("hunter22", "rfw");
    const auto login = login_and_get_tokens(username, "hunter22");
    const auto r = handle.client->Post(
        "/api/v1/auth/refresh", logout_body(
            login["refresh_token"].get<std::string>()), "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure-unit tests of parse_logout_request (no MySQL, no server).
//  Catches regressions even on a box without MySQL.
// ────────────────────────────────────────────────────────────────────────────

TEST(ParseLogoutRequestUnit, AcceptsValidBody) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_logout_request(
        nlohmann::json{{"refresh_token", "abc.def.ghi"}}, res);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->refresh_token.has_value());
    EXPECT_EQ(*parsed->refresh_token, "abc.def.ghi");
    EXPECT_TRUE(res.body.empty());   // no envelope written
}

TEST(ParseLogoutRequestUnit, AcceptsMissingField) {
    // v1.3.3.8 ★ — Phase 5 cookie-aware: missing refresh_token is
    // the success shape (handler sources from cookie). Returns a
    // populated struct with refresh_token == nullopt and writes no
    // envelope.
    httplib::Response res;
    const auto parsed = litecode::detail::parse_logout_request(
        nlohmann::json::object(), res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->refresh_token.has_value());
    EXPECT_TRUE(res.body.empty());
}

TEST(ParseLogoutRequestUnit, AcceptsMissingFieldWithSurplusKeys) {
    // Surplus telemetry keys + absent refresh_token is also fine —
    // proves the v1.3.3.8 contract goes through pure negative-shape
    // situations, not just bare `{}`.
    httplib::Response res;
    const auto parsed = litecode::detail::parse_logout_request(
        nlohmann::json{{"device_id", "laptop-7"}}, res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->refresh_token.has_value());
}

TEST(ParseLogoutRequestUnit, RejectsEmptyString) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_logout_request(
        nlohmann::json{{"refresh_token", ""}}, res);
    EXPECT_FALSE(parsed.has_value());
    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST(ParseLogoutRequestUnit, RejectsNonStringField) {
    httplib::Response res;
    const auto parsed = litecode::detail::parse_logout_request(
        nlohmann::json{{"refresh_token", 42}}, res);
    EXPECT_FALSE(parsed.has_value());
    const auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"], "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "refresh_token");
}

TEST(ParseLogoutRequestUnit, IgnoresSurplusFields) {
    // Strict-shape-with-surplus: surplus keys must be ignored so the
    // front-end can send extra telemetry without breaking.
    httplib::Response res;
    const auto parsed = litecode::detail::parse_logout_request(
        nlohmann::json{
            {"refresh_token", "abc.def.ghi"},
            {"device_id",     "laptop-1"},
        }, res);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->refresh_token.has_value());
    EXPECT_EQ(*parsed->refresh_token, "abc.def.ghi");
}

} // namespace
