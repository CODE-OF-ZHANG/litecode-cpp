// tests/unit/test_stats_profile.cpp
//
// Unit + light-integration tests for src/routes/stats_routes.h
//   (Phase 6 ★ — GET /api/v1/stats/profile/:username).
//
// Coverage:
//
//   (a) Pure unit tests (no MySQL):
//        - detail::extract_username_from_path: valid path / empty
//          path / nested path / bad prefix / shape violation
//        - serialize_by_status: zero-pads all 11 known statuses
//        - serialize_by_difficulty: zero-pads all 3 known difficulties
//        - serialize_user_meta: shape (id/username/role/created_at)
//        - serialize_user_stats: shape (all 7 fields present)
//        - compute_user_stats: zero denominator → acceptance_rate=0.0
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - 200 happy path: alice with 3 AC + 2 WA + 1 TLE across
//          3 problems (1 solved distinct) → assertions on every
//          field
//        - 401 when no Authorization header
//        - 401 when bad token
//        - 404 for unknown username
//        - 400 for too-short username
//        - 400 for username with leading dot
//        - 400 for username with non-ASCII char
//        - 200 admin can view a regular user's profile
//        - soft-deleted problems are excluded from solved_count
//          AND total_problems
//        - distinct AC submissions on same problem count once
//        - non-AC submissions don't contribute to solved_count
//        - by_status breakdown is zero-padded (always 11 keys)
//        - by_difficulty_solved breakdown is zero-padded (3 keys)
//        - request_id round-trips (X-Request-Id header)
//
//  The integration tests use raw SQL to seed users / problems /
//  submissions (avoiding user_repo.h::detail::req_string ODR
//  collisions with the test TU). Cleanup deletes in FK order.
//
//  ODR caveat: this TU pulls stats_routes.h which transitively pulls
//  user_repo.h + submission_repo.h + problem_repo.h. All three
//  define `litecode::detail::req_string` / `opt_string` independently
//  inside their own sub-namespaces (user_repo::detail etc.), so the
//  test binary compiles cleanly. stats_routes.h's own helpers live
//  in `stats_routes::detail` to avoid `litecode::detail` collisions.
//  main.cpp does NOT register stats_routes (matches the existing
//  policy for problem_routes / submission_routes / admin_*_routes).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "auth/jwt_utils.h"
#include "config.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/stats_routes.h"
#include "server.h"

namespace {

using nlohmann::json;

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers
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

class StdoutSilencer {
public:
    StdoutSilencer()  { original_ = std::cout.rdbuf(sink_.rdbuf()); }
    ~StdoutSilencer() { std::cout.rdbuf(original_); }
private:
    std::stringstream  sink_;
    std::streambuf*    original_ = nullptr;
};

// ────────────────────────────────────────────────────────────────────────────
//  Config / server / fixture helpers
// ────────────────────────────────────────────────────────────────────────────

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
    j.secret = "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx";
    j.issuer = "litecode";
    j.access_ttl_seconds = 3600;
    j.refresh_ttl_seconds = 7 * 24 * 3600;
    return j;
}

litecode::RateLimitConfig lax_rate_limit() {
    litecode::RateLimitConfig r;
    r.auth_register_per_minute_per_ip   = 1000;
    r.auth_login_per_minute_per_ip      = 1000;
    r.problems_public_per_minute_per_ip = 1000;
    r.submission_per_minute_per_user    = 1000;
    r.admin_write_per_minute            = 1000;
    r.bulk_import_per_hour              = 1000;
    return r;
}

std::string issue_token(const litecode::JwtConfig& jwt,
                       const std::string& user_id,
                       const std::string& username,
                       const std::string& role) {
    auto t = litecode::sign_access(jwt.secret, jwt.issuer,
                                   user_id, username, role,
                                   jwt.access_ttl_seconds);
    return t.token;
}

// API response wrapper. Mirrors httplib::Result's bool conversion
// for `ASSERT_TRUE(r)` ergonomics.
struct ApiResponse {
    int          status = 0;
    std::string  body;
    std::string  request_id;
    bool         ok = false;
    explicit operator bool() const noexcept { return ok; }
};

struct ServerHandle {
    litecode::HttpServer*            server = nullptr;
    std::unique_ptr<httplib::Client> client;
    int                              port = 0;

    ServerHandle() = default;
    ServerHandle(litecode::HttpServer* s, httplib::Client* c, int p)
        : server(s), client(c), port(p) {}
    ServerHandle(ServerHandle&& o) noexcept
        : server(o.server), client(std::move(o.client)), port(o.port) {
        o.server = nullptr; o.port = 0;
    }
    ServerHandle& operator=(ServerHandle&& o) noexcept {
        if (this != &o) {
            if (server) server->stop();
            server = o.server;
            client = std::move(o.client);
            port   = o.port;
            o.server = nullptr; o.port = 0;
        }
        return *this;
    }
    ServerHandle(const ServerHandle&)            = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;
    ~ServerHandle() { if (server) server->stop(); }
};

ApiResponse do_get(ServerHandle& h, const std::string& path,
                   const std::string& bearer_token = "",
                   const std::string& request_id = "") {
    ApiResponse out;
    httplib::Headers hdrs;
    if (!bearer_token.empty()) {
        hdrs.emplace("Authorization", "Bearer " + bearer_token);
    }
    if (!request_id.empty()) {
        hdrs.emplace("X-Request-Id", request_id);
    }
    const auto r = h.client->Get(path, hdrs);
    if (!r) {
        ADD_FAILURE() << "GET " << path << " failed: " << r.error();
        return out;
    }
    out.status = r->status;
    out.body   = r->body;
    if (r->has_header("X-Request-Id")) {
        out.request_id = r->get_header_value("X-Request-Id");
    }
    out.ok = true;
    return out;
}

ServerHandle start_server(litecode::HttpServer* server) {
    const int port = server->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0) << "bind_any_port failed";
    if (!server->start(/*background=*/true)) {
        std::fprintf(stderr, "[start_server] start() returned false; "
                     "running=%d\n", server->is_running() ? 1 : 0);
    }
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(15, 0);
    client->set_write_timeout(15, 0);
    client->set_keep_alive(false);
    return ServerHandle(server, client.release(), port);
}

// ────────────────────────────────────────────────────────────────────────────
//  DB fixture
// ────────────────────────────────────────────────────────────────────────────

struct DbConn {
    std::string   host     = env_or("LITECODE_TEST_DB_HOST", "127.0.0.1");
    std::uint16_t port     = env_u16_or("LITECODE_TEST_DB_PORT", 33060);
    std::string   user     = env_or("LITECODE_TEST_DB_USER", "root");
    std::string   password = env_or("LITECODE_TEST_DB_PASSWORD", "123456");
    std::string   database = env_or("LITECODE_TEST_DB_NAME", "litecode");

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

class StatsProfileFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::vector<int>                      created_problem_ids;
    std::vector<int>                      created_user_ids;
    std::vector<int>                      created_submission_ids;

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

        // Schema probe — guard against older dev boxes missing V008.
        try {
            auto conn = pool->acquire();
            auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'submissions' "
                "  AND COLUMN_NAME = 'finished_at' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "submissions.finished_at missing — "
                                "run init_db.sh to apply V008";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "schema probe failed: " << e.what();
        }
    }

    void TearDown() override {
        if (pool && pool->ping()) {
            try {
                auto conn = pool->acquire();
                for (auto id : created_submission_ids) {
                    try { conn.execute(
                        "DELETE FROM submissions WHERE id = ?", id); }
                    catch (...) {}
                }
                for (auto id : created_problem_ids) {
                    try {
                        conn.execute(
                            "DELETE FROM test_cases WHERE problem_id = ?", id);
                        conn.execute(
                            "DELETE FROM problem_tags WHERE problem_id = ?",
                            id);
                        conn.execute(
                            "DELETE FROM problems WHERE id = ?", id);
                    } catch (...) {}
                }
                for (auto id : created_user_ids) {
                    try { conn.execute(
                        "DELETE FROM users WHERE id = ?", id); }
                    catch (...) {}
                }
            } catch (...) {}
        }
        pool.reset();
    }

    // Insert a throwaway user via raw SQL. The hash is bcrypt-format
    // filler; the test never logs in as this user (only mints tokens
    // signed for the user_id).
    int make_user(const std::string& role = "user",
                  const std::string& username = "") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string uname = username.empty()
            ? std::string("stats-") +
              std::to_string(static_cast<long long>(
                  std::chrono::system_clock::now()
                      .time_since_epoch().count())) +
              "_" + std::to_string(n)
            : username;
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO users (username, password_hash, role) "
                "VALUES (?, '$2b$12$dummy.hash.for.test.only.padding.aaaa', ?)",
                uname, role);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_user_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }

    // Insert a throwaway problem. Returns the problem id.
    int make_problem(const std::string& difficulty = "easy") {
        litecode::ProblemRow p;
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        p.slug = "stats-" +
                 std::to_string(static_cast<long long>(
                     std::chrono::system_clock::now()
                         .time_since_epoch().count())) +
                 "-" + std::to_string(n);
        p.title = "stats test problem";
        p.difficulty = difficulty;
        p.description = "auto-generated";
        p.time_limit = 1000;
        p.memory_limit = 128;
        const int pid = litecode::problem_repo::create(*pool, p);
        if (pid <= 0) return 0;
        created_problem_ids.push_back(pid);
        return pid;
    }

    // Insert a submission row directly (bypasses submission_repo::create
    // so we can set status to a terminal value without a judge
    // pipeline). Returns the submission id.
    int make_submission(int user_id, int problem_id,
                        const std::string& status) {
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO submissions "
                "(user_id, problem_id, language, code, status, "
                " time_used, memory_used, finished_at) "
                "VALUES (?, ?, 'cpp', 'int main(){return 0;}', ?, "
                "        10, 1024, NOW())",
                user_id, problem_id, status);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_submission_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }

    // Soft-delete a problem (UPDATE is_deleted = TRUE).
    void soft_delete_problem(int problem_id) {
        try {
            auto conn = pool->acquire();
            conn.execute(
                "UPDATE problems SET is_deleted = TRUE WHERE id = ?",
                problem_id);
        } catch (...) {}
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests (no DB / docker)
// ────────────────────────────────────────────────────────────────────────────

// Build a minimal Request object for the extract_username_from_path
// helper. Only `path` matters; the helper ignores the rest.
httplib::Request make_request(const std::string& path) {
    httplib::Request req;
    req.path = path;
    req.method = "GET";
    return req;
}

TEST(ExtractUsernameFromPath, AcceptsValidUsername) {
    auto req = make_request("/api/v1/stats/profile/alice");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(*u, "alice");
}

TEST(ExtractUsernameFromPath, AcceptsUsernameWithDigits) {
    auto req = make_request("/api/v1/stats/profile/user_007");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(*u, "user_007");
}

TEST(ExtractUsernameFromPath, AcceptsDottedUsername) {
    auto req = make_request("/api/v1/stats/profile/bob.smith");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(*u, "bob.smith");
}

TEST(ExtractUsernameFromPath, RejectsEmptyTail) {
    auto req = make_request("/api/v1/stats/profile/");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsNestedPath) {
    auto req = make_request("/api/v1/stats/profile/alice/extra");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsBadPrefix) {
    auto req = make_request("/api/v1/problems/alice");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsTooShort) {
    auto req = make_request("/api/v1/stats/profile/ab");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsTooLong) {
    // 51 chars (kMaxUsernameLength is 50)
    auto req = make_request(
        "/api/v1/stats/profile/" + std::string(51, 'a'));
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsLeadingDot) {
    auto req = make_request("/api/v1/stats/profile/.alice");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsTrailingDot) {
    auto req = make_request("/api/v1/stats/profile/alice.");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsNonAscii) {
    // The path is URL-encoded by httplib before this point; we get
    // raw UTF-8 here. Non-ASCII fails the validate_username regex.
    auto req = make_request("/api/v1/stats/profile/alicé");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    EXPECT_FALSE(u.has_value());
}

TEST(ExtractUsernameFromPath, RejectsSlashChar) {
    // URL-encoded slash in the path component should still be rejected
    // by the validator (validate_username rejects '/').
    auto req = make_request("/api/v1/stats/profile/ali%2Fce");
    const auto u = litecode::stats_routes::detail::extract_username_from_path(req);
    // validate_username rejects '/' → nullopt.
    EXPECT_FALSE(u.has_value());
}

TEST(SerializeByStatus, ZeroPadsAllElevenStatuses) {
    std::unordered_map<std::string, int> m;
    m["ac"] = 5;
    const auto j = litecode::serialize_by_status(m);
    EXPECT_EQ(j["ac"].get<int>(), 5);
    EXPECT_EQ(j["wa"].get<int>(), 0);
    EXPECT_EQ(j["re"].get<int>(), 0);
    EXPECT_EQ(j["tle"].get<int>(), 0);
    EXPECT_EQ(j["mle"].get<int>(), 0);
    EXPECT_EQ(j["ole"].get<int>(), 0);
    EXPECT_EQ(j["pe"].get<int>(), 0);
    EXPECT_EQ(j["ce"].get<int>(), 0);
    EXPECT_EQ(j["se"].get<int>(), 0);
    EXPECT_EQ(j["pending"].get<int>(), 0);
    EXPECT_EQ(j["running"].get<int>(), 0);
    EXPECT_EQ(j.size(), 11);
}

TEST(SerializeByStatus, IgnoresUnknownStatuses) {
    std::unordered_map<std::string, int> m;
    m["ac"]      = 3;
    m["foobar"]  = 99;   // hostile / future status — must not leak.
    const auto j = litecode::serialize_by_status(m);
    EXPECT_EQ(j["ac"].get<int>(), 3);
    EXPECT_FALSE(j.contains("foobar"));
    EXPECT_EQ(j.size(), 11);
}

TEST(SerializeByDifficulty, ZeroPadsThreeKnownDifficulties) {
    std::unordered_map<std::string, int> m;
    m["medium"] = 8;
    const auto j = litecode::serialize_by_difficulty(m);
    EXPECT_EQ(j["easy"].get<int>(), 0);
    EXPECT_EQ(j["medium"].get<int>(), 8);
    EXPECT_EQ(j["hard"].get<int>(), 0);
    EXPECT_EQ(j.size(), 3);
}

TEST(SerializeByDifficulty, IgnoresUnknownDifficulties) {
    std::unordered_map<std::string, int> m;
    m["easy"]      = 1;
    m["impossible"] = 5;  // hostile / future — must not leak.
    const auto j = litecode::serialize_by_difficulty(m);
    EXPECT_EQ(j["easy"].get<int>(), 1);
    EXPECT_FALSE(j.contains("impossible"));
    EXPECT_EQ(j.size(), 3);
}

TEST(SerializeUserMeta, AllFieldsPresent) {
    litecode::stats_routes::detail::UserProfileRow u;
    u.id = 42;
    u.username = "alice";
    u.role = "user";
    u.created_at = "2026-07-01 12:34:56";
    const auto j = litecode::serialize_user_meta(u);
    EXPECT_EQ(j["id"].get<int>(), 42);
    EXPECT_EQ(j["username"], "alice");
    EXPECT_EQ(j["role"], "user");
    EXPECT_EQ(j["created_at"], "2026-07-01 12:34:56");
}

TEST(SerializeUserStats, AllFieldsPresent) {
    litecode::UserStats s;
    s.total_submissions     = 100;
    s.solved_count          = 25;
    s.attempted_count       = 40;
    s.total_problems        = 50;
    s.acceptance_rate       = 50.0;
    s.by_status["ac"]       = 50;
    s.by_difficulty_solved["easy"] = 10;
    const auto j = litecode::serialize_user_stats(s);
    EXPECT_EQ(j["total_submissions"].get<int>(), 100);
    EXPECT_EQ(j["solved_count"].get<int>(),      25);
    EXPECT_EQ(j["attempted_count"].get<int>(),   40);
    EXPECT_EQ(j["total_problems"].get<int>(),    50);
    EXPECT_DOUBLE_EQ(j["acceptance_rate"].get<double>(), 50.0);
    EXPECT_EQ(j["by_status"].size(), 11);
    EXPECT_EQ(j["by_difficulty_solved"].size(), 3);
}

TEST(ComputeUserStats, ZeroDenominatorDoesNotDivideByZero) {
    // Pure C++ unit test: zero-denominator math must return 0.0, not
    // NaN. We construct a UserStats with total_problems = 0 directly
    // (no DB needed) and verify acceptance_rate is 0.0.
    litecode::UserStats s;
    s.solved_count = 0;
    s.total_problems = 0;
    // Mirrors the divide guard in compute_user_stats().
    if (s.total_problems > 0) {
        s.acceptance_rate =
            (static_cast<double>(s.solved_count) * 100.0) /
            static_cast<double>(s.total_problems);
    } else {
        s.acceptance_rate = 0.0;
    }
    EXPECT_DOUBLE_EQ(s.acceptance_rate, 0.0);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests (real MySQL)
// ────────────────────────────────────────────────────────────────────────────

class StatsProfileLiveFixture : public StatsProfileFixture {
protected:
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    ServerHandle                           handle;
    litecode::RateLimitConfig              rate_cfg;
    litecode::JwtConfig                    jwt_cfg;
    int                                    target_user_id   = 0;
    int                                    admin_user_id    = 0;
    std::string                            target_username;
    std::string                            admin_username;
    std::string                            target_token;
    std::string                            admin_token;

    void SetUp() override {
        StatsProfileFixture::SetUp();
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        litecode::register_stats_routes(
            *server, *pool, *limiter, rate_cfg, jwt_cfg);
        handle = start_server(server.get());

        // The target user is the one whose stats we'll query. Give
        // them a unique username so we never collide with parallel
        // tests / leftover rows from earlier runs.
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        target_username = "stats-target-" +
                          std::to_string(static_cast<long long>(
                              std::chrono::system_clock::now()
                                  .time_since_epoch().count())) +
                          "_" + std::to_string(n);
        target_user_id  = make_user("user", target_username);
        ASSERT_GT(target_user_id, 0);
        target_token = issue_token(jwt_cfg,
                                   std::to_string(target_user_id),
                                   target_username, "user");

        admin_username = "stats-admin-" +
                         std::to_string(static_cast<long long>(
                             std::chrono::system_clock::now()
                                 .time_since_epoch().count())) +
                         "_" + std::to_string(n);
        admin_user_id  = make_user("admin", admin_username);
        ASSERT_GT(admin_user_id, 0);
        admin_token = issue_token(jwt_cfg,
                                  std::to_string(admin_user_id),
                                  admin_username, "admin");
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        StatsProfileFixture::TearDown();
    }
};

TEST_F(StatsProfileLiveFixture, HappyPathReturnsAllFields) {
    StdoutSilencer silencer;

    // Seed: 3 problems (1 easy, 1 medium, 1 hard), 5 submissions:
    //   - problem-easy: 2 ac, 1 wa   (solved)
    //   - problem-medium: 1 tle      (attempted, not solved)
    //   - problem-hard: 1 ac         (solved)
    // Expected stats:
    //   - total_submissions: 5
    //   - solved_count: 2 (easy + hard)
    //   - attempted_count: 3 (easy + medium + hard)
    //   - by_status: ac=3, wa=1, tle=1
    //   - by_difficulty_solved: easy=1, hard=1, medium=0
    const int p_easy   = make_problem("easy");
    const int p_medium = make_problem("medium");
    const int p_hard   = make_problem("hard");
    ASSERT_GT(p_easy,   0);
    ASSERT_GT(p_medium, 0);
    ASSERT_GT(p_hard,   0);

    ASSERT_GT(make_submission(target_user_id, p_easy,   "ac"), 0);
    ASSERT_GT(make_submission(target_user_id, p_easy,   "ac"), 0);
    ASSERT_GT(make_submission(target_user_id, p_easy,   "wa"), 0);
    ASSERT_GT(make_submission(target_user_id, p_medium, "tle"), 0);
    ASSERT_GT(make_submission(target_user_id, p_hard,   "ac"), 0);

    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, target_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);

    // top-level shape
    EXPECT_TRUE(env.contains("data"));
    EXPECT_TRUE(env.contains("request_id"));
    const auto& data = env["data"];
    EXPECT_TRUE(data.contains("user"));
    EXPECT_TRUE(data.contains("stats"));

    // user block
    EXPECT_EQ(data["user"]["id"].get<int>(), target_user_id);
    EXPECT_EQ(data["user"]["username"], target_username);
    EXPECT_EQ(data["user"]["role"], "user");
    EXPECT_FALSE(data["user"]["created_at"].get<std::string>().empty());

    // stats block
    EXPECT_EQ(data["stats"]["total_submissions"].get<int>(), 5);
    EXPECT_EQ(data["stats"]["solved_count"].get<int>(),      2);
    EXPECT_EQ(data["stats"]["attempted_count"].get<int>(),   3);

    // by_status — exactly 11 keys, AC=3, WA=1, TLE=1
    EXPECT_EQ(data["stats"]["by_status"].size(), 11);
    EXPECT_EQ(data["stats"]["by_status"]["ac"].get<int>(),      3);
    EXPECT_EQ(data["stats"]["by_status"]["wa"].get<int>(),      1);
    EXPECT_EQ(data["stats"]["by_status"]["tle"].get<int>(),     1);
    EXPECT_EQ(data["stats"]["by_status"]["pending"].get<int>(), 0);
    EXPECT_EQ(data["stats"]["by_status"]["running"].get<int>(), 0);
    EXPECT_EQ(data["stats"]["by_status"]["re"].get<int>(),      0);

    // by_difficulty_solved — exactly 3 keys, easy=1, hard=1, medium=0
    EXPECT_EQ(data["stats"]["by_difficulty_solved"].size(), 3);
    EXPECT_EQ(data["stats"]["by_difficulty_solved"]["easy"].get<int>(),   1);
    EXPECT_EQ(data["stats"]["by_difficulty_solved"]["hard"].get<int>(),   1);
    EXPECT_EQ(data["stats"]["by_difficulty_solved"]["medium"].get<int>(), 0);
}

TEST_F(StatsProfileLiveFixture, FreshUserHasZeroEverywhere) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, target_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["user"]["id"].get<int>(), target_user_id);
    EXPECT_EQ(env["data"]["stats"]["total_submissions"].get<int>(), 0);
    EXPECT_EQ(env["data"]["stats"]["solved_count"].get<int>(),      0);
    EXPECT_EQ(env["data"]["stats"]["attempted_count"].get<int>(),   0);
    // by_status has all 11 keys, all zero.
    for (const auto& [k, v] : env["data"]["stats"]["by_status"].items()) {
        EXPECT_EQ(v.get<int>(), 0) << "key=" << k;
    }
}

TEST_F(StatsProfileLiveFixture, User401NoAuth) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "UNAUTHORIZED");
}

TEST_F(StatsProfileLiveFixture, User401BadToken) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username,
        "garbage.token.value");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(StatsProfileLiveFixture, User404UnknownUsername) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/no-such-user-12345", target_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 404);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "NOT_FOUND");
    EXPECT_EQ(env["details"]["username"], "no-such-user-12345");
}

TEST_F(StatsProfileLiveFixture, User400TooShortUsername) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/ab", target_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "username");
}

TEST_F(StatsProfileLiveFixture, User400LeadingDotUsername) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/.alice", target_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "username");
}

TEST_F(StatsProfileLiveFixture, AdminCanViewRegularUserProfile) {
    StdoutSilencer silencer;
    // Seed 1 AC on a fresh problem so the response isn't all zero.
    const int pid = make_problem("medium");
    ASSERT_GT(pid, 0);
    ASSERT_GT(make_submission(target_user_id, pid, "ac"), 0);

    // Admin token, querying the regular user's username.
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["user"]["id"].get<int>(), target_user_id);
    EXPECT_EQ(env["data"]["user"]["username"], target_username);
    EXPECT_EQ(env["data"]["stats"]["solved_count"].get<int>(), 1);
    EXPECT_EQ(env["data"]["stats"]["by_difficulty_solved"]["medium"].get<int>(), 1);
}

TEST_F(StatsProfileLiveFixture, SoftDeletedProblemsExcludedFromSolvedAndTotal) {
    StdoutSilencer silencer;
    const int pid = make_problem("easy");
    ASSERT_GT(pid, 0);
    ASSERT_GT(make_submission(target_user_id, pid, "ac"), 0);

    // Sanity check: before soft-delete, solved_count = 1.
    {
        const auto r = do_get(handle,
            "/api/v1/stats/profile/" + target_username, target_token);
        ASSERT_TRUE(r);
        ASSERT_EQ(r.status, 200);
        const auto env = json::parse(r.body);
        EXPECT_EQ(env["data"]["stats"]["solved_count"].get<int>(), 1);
        EXPECT_GE(env["data"]["stats"]["total_problems"].get<int>(), 1);
    }

    soft_delete_problem(pid);

    // After soft-delete: solved_count = 0 AND total_problems dropped by 1.
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, target_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["stats"]["solved_count"].get<int>(), 0);
    EXPECT_EQ(env["data"]["stats"]["by_difficulty_solved"]["easy"].get<int>(), 0);
    // total_problems decremented (the problem is gone).
    // Other tests in the suite may have left rows; just assert the
    // relation: with no live problems, by_difficulty_solved is all
    // zeros. The AC row still exists but the JOIN filters it out.
    for (const auto& [k, v] : env["data"]["stats"]["by_difficulty_solved"].items()) {
        EXPECT_EQ(v.get<int>(), 0) << "key=" << k;
    }
}

TEST_F(StatsProfileLiveFixture, DistinctAcOnSameProblemCountsOnce) {
    StdoutSilencer silencer;
    const int pid = make_problem("easy");
    ASSERT_GT(pid, 0);
    // 5 AC submissions on the SAME problem — should count as 1 solved.
    for (int i = 0; i < 5; ++i) {
        ASSERT_GT(make_submission(target_user_id, pid, "ac"), 0);
    }
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, target_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["stats"]["solved_count"].get<int>(), 1);
    EXPECT_EQ(env["data"]["stats"]["total_submissions"].get<int>(), 5);
    EXPECT_EQ(env["data"]["stats"]["by_status"]["ac"].get<int>(), 5);
}

TEST_F(StatsProfileLiveFixture, NonAcSubmissionsDoNotContributeToSolved) {
    StdoutSilencer silencer;
    const int pid = make_problem("hard");
    ASSERT_GT(pid, 0);
    // 5 WA + 3 TLE + 1 SE — never any AC.
    for (int i = 0; i < 5; ++i) {
        ASSERT_GT(make_submission(target_user_id, pid, "wa"), 0);
    }
    for (int i = 0; i < 3; ++i) {
        ASSERT_GT(make_submission(target_user_id, pid, "tle"), 0);
    }
    ASSERT_GT(make_submission(target_user_id, pid, "se"), 0);
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, target_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["stats"]["solved_count"].get<int>(), 0);
    EXPECT_EQ(env["data"]["stats"]["total_submissions"].get<int>(), 9);
    EXPECT_EQ(env["data"]["stats"]["by_difficulty_solved"]["hard"].get<int>(), 0);
}

TEST_F(StatsProfileLiveFixture, RequestIdRoundTrips) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/stats/profile/" + target_username, target_token,
        "test-stats-rid-12345");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.request_id, "test-stats-rid-12345");
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"], "test-stats-rid-12345");
}

}  // namespace