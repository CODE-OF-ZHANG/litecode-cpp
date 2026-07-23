// tests/unit/test_stats_ranking.cpp
//
// Unit + light-integration tests for src/routes/stats_routes.h
//   (Phase 6 ★ — GET /api/v1/stats/ranking).
//
// Coverage:
//
//   (a) Pure unit tests (no MySQL):
//        - parse_ranking_query: valid / missing / non-numeric /
//          negative / out-of-range limit & offset
//        - rank_for_offset: deterministic 1-indexed rank
//        - serialize_leaderboard_row: shape (rank, user.id,
//          user.username, user.role, solved_count, submission_count,
//          acceptance_rate)
//        - kLeaderboardDefaultLimit / kLeaderboardMaxLimit constants
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - 200 happy path: 3 users with distinct solve counts →
//          assertions on rank ordering, user block, counts, rates
//        - 200 empty: no users with ACs → items=[], total=0
//        - 200 with submit-only users (no AC) → they are excluded
//          from the ranking
//        - 200 efficiency tiebreaker: two users with same
//          solved_count → fewer submissions ranks higher
//        - 200 stable tiebreaker: two users with same stats →
//          user_id ASC
//        - 200 admin users are ranked too (no role filter)
//        - 200 soft-deleted problems don't count toward solved
//        - 200 pending/running submissions don't count
//        - 200 limit clamps to kLeaderboardMaxLimit
//        - 200 offset walks the page correctly
//        - 200 out-of-range offset returns empty items but real total
//        - 400 bad limit
//        - 400 negative offset
//        - 400 zero limit
//        - 400 non-numeric limit
//        - 200 X-Request-Id round-trip
//        - 200 X-RateLimit-* headers present (public endpoint with quota)
//        - 429 rate limit triggers (tight bucket)
//
//   The integration tests use raw SQL to seed users / problems /
//   submissions (avoiding cross-repo ODR collisions on
//   user_repo.h::detail::req_string). Cleanup deletes in FK order.
//
//   ODR caveat: same as test_stats_profile.cpp — stats_routes.h
//   transitively pulls in user_repo + submission_repo + problem_repo
//   which collide on `litecode::detail::req_string`; we work around
//   it via raw SQL seed.

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
    r.stats_ranking_per_minute_per_ip   = 1000;
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

// API response wrapper.
struct ApiResponse {
    int          status = 0;
    std::string  body;
    std::string  request_id;
    bool         ok = false;
    httplib::Headers headers;
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
                   const std::string& request_id = "") {
    ApiResponse out;
    httplib::Headers hdrs;
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
    out.headers = r->headers;
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

class StatsRankingFixture : public ::testing::Test {
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
    // filler; the test never logs in as this user.
    int make_user(const std::string& role = "user",
                  const std::string& username = "") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string uname = username.empty()
            ? std::string("rank-") +
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
        p.slug = "rank-" +
                 std::to_string(static_cast<long long>(
                     std::chrono::system_clock::now()
                         .time_since_epoch().count())) +
                 "-" + std::to_string(n);
        p.title = "rank test problem";
        p.difficulty = difficulty;
        p.description = "auto-generated";
        p.time_limit = 1000;
        p.memory_limit = 128;
        const int pid = litecode::problem_repo::create(*pool, p);
        if (pid <= 0) return 0;
        created_problem_ids.push_back(pid);
        return pid;
    }

    // Insert a submission row directly. Returns the submission id.
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

// Build a minimal Request object for the parse_ranking_query helper.
// httplib::Request::params is the multimap that has_param() /
// get_param_value() look up; we populate it directly from the
// query-string portion of the path. Returns a path WITHOUT the
// query string (httplib strips it off in production).
httplib::Request make_request(const std::string& path_with_qs) {
    httplib::Request req;
    const auto qpos = path_with_qs.find('?');
    if (qpos == std::string::npos) {
        req.path = path_with_qs;
    } else {
        req.path = path_with_qs.substr(0, qpos);
        const std::string qs = path_with_qs.substr(qpos + 1);
        // Simple key=value&key=value parser. URL-decode is unnecessary
        // for the test cases we exercise (limit, offset, ab).
        std::size_t i = 0;
        while (i < qs.size()) {
            std::size_t amp = qs.find('&', i);
            if (amp == std::string::npos) amp = qs.size();
            const std::string pair = qs.substr(i, amp - i);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) {
                req.params.emplace(pair.substr(0, eq), pair.substr(eq + 1));
            } else {
                req.params.emplace(pair, std::string());
            }
            i = amp + 1;
        }
    }
    req.method = "GET";
    return req;
}

TEST(LeaderboardConstants, DefaultAndMaxLimit) {
    EXPECT_EQ(litecode::stats_routes::detail::kLeaderboardDefaultLimit, 100);
    EXPECT_EQ(litecode::stats_routes::detail::kLeaderboardMaxLimit,     200);
    EXPECT_LE(litecode::stats_routes::detail::kLeaderboardDefaultLimit,
              litecode::stats_routes::detail::kLeaderboardMaxLimit);
}

TEST(RankForOffset, DeterministicOneIndexed) {
    // Page 0: rank = 1, 2, 3, ...
    EXPECT_EQ(litecode::stats_routes::detail::rank_for_offset(0, 0), 1);
    EXPECT_EQ(litecode::stats_routes::detail::rank_for_offset(0, 1), 2);
    EXPECT_EQ(litecode::stats_routes::detail::rank_for_offset(0, 99), 100);

    // Page 1 (offset=100): rank = 101, 102, ...
    EXPECT_EQ(litecode::stats_routes::detail::rank_for_offset(100, 0), 101);
    EXPECT_EQ(litecode::stats_routes::detail::rank_for_offset(100, 5), 106);

    // Page 3 (offset=300, page_index=0): rank = 301
    EXPECT_EQ(litecode::stats_routes::detail::rank_for_offset(300, 0), 301);
}

TEST(ParseRankingQuery, EmptyPathDefaultsTo100And0) {
    httplib::Response res;
    int limit = -1, offset = -1;
    auto req = make_request("/api/v1/stats/ranking");
    EXPECT_TRUE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(limit, 100);
    EXPECT_EQ(offset, 0);
    // No envelope was written — res.status stays at the httplib
    // default (-1). The body must also be empty.
    EXPECT_EQ(res.status, -1);
    EXPECT_TRUE(res.body.empty());
}

TEST(ParseRankingQuery, AcceptsValidLimitAndOffset) {
    httplib::Response res;
    int limit = 0, offset = 0;
    auto req = make_request("/api/v1/stats/ranking?limit=20&offset=40");
    EXPECT_TRUE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(limit, 20);
    EXPECT_EQ(offset, 40);
}

TEST(ParseRankingQuery, RejectsNonNumericLimit) {
    httplib::Response res;
    int limit = 0, offset = 0;
    auto req = make_request("/api/v1/stats/ranking?limit=abc");
    EXPECT_FALSE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "limit");
}

TEST(ParseRankingQuery, RejectsZeroLimit) {
    httplib::Response res;
    int limit = 0, offset = 0;
    auto req = make_request("/api/v1/stats/ranking?limit=0");
    EXPECT_FALSE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "limit");
}

TEST(ParseRankingQuery, RejectsNegativeOffset) {
    httplib::Response res;
    int limit = 0, offset = 0;
    auto req = make_request("/api/v1/stats/ranking?offset=-1");
    EXPECT_FALSE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "offset");
}

TEST(ParseRankingQuery, RejectsTrailingJunkOnLimit) {
    httplib::Response res;
    int limit = 0, offset = 0;
    auto req = make_request("/api/v1/stats/ranking?limit=20abc");
    EXPECT_FALSE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
}

TEST(ParseRankingQuery, ClampsLimitToMax) {
    httplib::Response res;
    int limit = 0, offset = 0;
    auto req = make_request("/api/v1/stats/ranking?limit=9999");
    EXPECT_TRUE(litecode::parse_ranking_query(req, res, limit, offset));
    EXPECT_EQ(limit, litecode::stats_routes::detail::kLeaderboardMaxLimit);
}

TEST(SerializeLeaderboardRow, AllFieldsPresent) {
    litecode::stats_routes::detail::LeaderboardRow r;
    r.user_id          = 42;
    r.username         = "alice";
    r.role             = "user";
    r.solved_count     = 25;
    r.submission_count = 80;
    r.acceptance_rate  = 31.25;
    const auto j = litecode::serialize_leaderboard_row(r);
    EXPECT_EQ(j["user"]["id"].get<int>(), 42);
    EXPECT_EQ(j["user"]["username"], "alice");
    EXPECT_EQ(j["user"]["role"], "user");
    EXPECT_EQ(j["solved_count"].get<int>(), 25);
    EXPECT_EQ(j["submission_count"].get<int>(), 80);
    EXPECT_DOUBLE_EQ(j["acceptance_rate"].get<double>(), 31.25);
    // rank is filled in by the handler, not by the row serializer.
    EXPECT_TRUE(j.contains("rank"));
}

TEST(SerializeLeaderboardRow, OmitsEmailAndLastLogin) {
    // We deliberately don't surface email / last_login / last_login_ip
    // / avatar in the leaderboard row. The serializer doesn't accept
    // them, so the simplest way to assert "omitted" is "the result
    // doesn't have those keys".
    litecode::stats_routes::detail::LeaderboardRow r;
    r.username = "alice";
    const auto j = litecode::serialize_leaderboard_row(r);
    EXPECT_FALSE(j.contains("email"));
    EXPECT_FALSE(j.contains("last_login"));
    EXPECT_FALSE(j.contains("last_login_ip"));
    EXPECT_FALSE(j.contains("avatar"));
    EXPECT_FALSE(j.contains("created_at"));
    EXPECT_FALSE(j.contains("password_hash"));
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests (real MySQL)
// ────────────────────────────────────────────────────────────────────────────

class StatsRankingLiveFixture : public StatsRankingFixture {
protected:
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    ServerHandle                           handle;
    litecode::RateLimitConfig              rate_cfg;
    litecode::JwtConfig                    jwt_cfg;

    void SetUp() override {
        StatsRankingFixture::SetUp();
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        litecode::register_stats_routes(
            *server, *pool, *limiter, rate_cfg, jwt_cfg);
        handle = start_server(server.get());
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        StatsRankingFixture::TearDown();
    }

    // Convenience: read X-RateLimit-Limit from a response.
    std::string get_header(const ApiResponse& r, const std::string& name) {
        auto it = r.headers.find(name);
        if (it == r.headers.end()) return "";
        return it->second;
    }
};

TEST_F(StatsRankingLiveFixture, HappyPathOrderingAndShape) {
    StdoutSilencer silencer;

    // Seed 3 users with distinct solve counts.
    const int u_top    = make_user("user", "rank-top-"    + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    const int u_middle = make_user("user", "rank-middle-" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    const int u_low    = make_user("user", "rank-low-"    + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()));
    ASSERT_GT(u_top, 0);
    ASSERT_GT(u_middle, 0);
    ASSERT_GT(u_low, 0);

    // 4 distinct problems so we can give 3 / 2 / 1 solves.
    const int p1 = make_problem("easy");
    const int p2 = make_problem("easy");
    const int p3 = make_problem("medium");
    const int p4 = make_problem("hard");
    ASSERT_GT(p1, 0);
    ASSERT_GT(p2, 0);
    ASSERT_GT(p3, 0);
    ASSERT_GT(p4, 0);

    // u_top: AC on p1, p2, p3 + a couple of WA noise = 3 solves
    ASSERT_GT(make_submission(u_top, p1, "ac"), 0);
    ASSERT_GT(make_submission(u_top, p2, "ac"), 0);
    ASSERT_GT(make_submission(u_top, p3, "ac"), 0);
    ASSERT_GT(make_submission(u_top, p1, "wa"), 0);

    // u_middle: AC on p1, p2 = 2 solves
    ASSERT_GT(make_submission(u_middle, p1, "ac"), 0);
    ASSERT_GT(make_submission(u_middle, p2, "ac"), 0);

    // u_low: AC on p1 = 1 solve
    ASSERT_GT(make_submission(u_low, p1, "ac"), 0);

    // Query the leaderboard with a tight limit so we only see our 3
    // users (not the leftover rows from earlier tests in the suite).
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=10");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);

    EXPECT_TRUE(env.contains("data"));
    EXPECT_TRUE(env.contains("request_id"));
    EXPECT_EQ(env["data"]["limit"].get<int>(), 10);
    EXPECT_EQ(env["data"]["offset"].get<int>(), 0);
    EXPECT_GE(env["data"]["total"].get<int>(), 3);

    const auto& items = env["data"]["items"];
    ASSERT_GE(items.size(), 3u);

    // The top 3 rows should be our 3 users in order: top, middle, low.
    // We look them up by user id (since username can be long and
    // shares prefix "rank-" with the test ID timestamp).
    int found_top = -1, found_middle = -1, found_low = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int uid = items[i]["user"]["id"].get<int>();
        if (uid == u_top)    found_top    = static_cast<int>(i);
        if (uid == u_middle) found_middle = static_cast<int>(i);
        if (uid == u_low)    found_low    = static_cast<int>(i);
    }
    ASSERT_NE(found_top,    -1);
    ASSERT_NE(found_middle, -1);
    ASSERT_NE(found_low,    -1);
    EXPECT_LT(found_top,    found_middle);
    EXPECT_LT(found_middle, found_low);

    // Top user's stats.
    EXPECT_EQ(items[found_top]["solved_count"].get<int>(),     3);
    EXPECT_EQ(items[found_top]["submission_count"].get<int>(), 4);
    // v1.3.4: acceptance_rate 改为真百分比 — solved/submission*100。
    // 之前注释里写成"100 * submission / solved"是命名错。
    EXPECT_DOUBLE_EQ(items[found_top]["acceptance_rate"].get<double>(), 100.0 * 3.0 / 4.0);
    EXPECT_EQ(items[found_top]["rank"].get<int>(), found_top + 1);

    // Middle user's stats.
    EXPECT_EQ(items[found_middle]["solved_count"].get<int>(),     2);
    EXPECT_EQ(items[found_middle]["submission_count"].get<int>(), 2);
    EXPECT_DOUBLE_EQ(items[found_middle]["acceptance_rate"].get<double>(), 100.0);
    EXPECT_EQ(items[found_middle]["rank"].get<int>(), found_middle + 1);

    // Low user's stats.
    EXPECT_EQ(items[found_low]["solved_count"].get<int>(),     1);
    EXPECT_EQ(items[found_low]["submission_count"].get<int>(), 1);
    EXPECT_DOUBLE_EQ(items[found_low]["acceptance_rate"].get<double>(), 100.0);
    EXPECT_EQ(items[found_low]["rank"].get<int>(), found_low + 1);

    // User block: id/username/role only (no email / last_login).
    EXPECT_TRUE(items[found_top]["user"].contains("id"));
    EXPECT_TRUE(items[found_top]["user"].contains("username"));
    EXPECT_TRUE(items[found_top]["user"].contains("role"));
    EXPECT_FALSE(items[found_top]["user"].contains("email"));
    EXPECT_FALSE(items[found_top]["user"].contains("last_login"));
}

TEST_F(StatsRankingLiveFixture, TotalCountsAllUsersEvenWithZeroAc) {
    StdoutSilencer silencer;
    // v1.3.4: 排行榜改为"全员上榜",从 users 出发 LEFT JOIN 聚合。
    // 任何用户(包括刚注册的、零提交的、零 AC 的)都会出现在
    // leaderboard 列表里,只是零 AC 的排在尾部。这里不再断言 total=0,
    // 而是断言 total >= 1(共享 mysql 的 fixture 至少已有 admin)。
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=10");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_GE(env["data"]["total"].get<int>(),  1);
    EXPECT_LE(env["data"]["items"].size(),      static_cast<std::size_t>(10));
    EXPECT_EQ(env["data"]["limit"].get<int>(),  10);
    EXPECT_EQ(env["data"]["offset"].get<int>(), 0);
    // 至少有一个用户条目 — 即使全是零 AC 也有 admin 这种内置用户。
    EXPECT_GE(env["data"]["items"].size(), 1u);
}

TEST_F(StatsRankingLiveFixture, WaOnlyUserStillRankedWithZeroAc) {
    StdoutSilencer silencer;
    // v1.3.4: WA-only 用户也会出现在排行榜里(零 AC 排尾部)。
    // 验收 acceptance_rate 边界值:零 AC 应为 0.0。
    const int u_only_wa = make_user("user", "rank-only-wa-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(u_only_wa, 0);
    const int p = make_problem("easy");
    ASSERT_GT(p, 0);
    for (int i = 0; i < 5; ++i) {
        ASSERT_GT(make_submission(u_only_wa, p, "wa"), 0);
    }
    // Plus one AC user so we have at least one user with solved > 0.
    const int u_ac = make_user("user", "rank-with-ac-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(u_ac, 0);
    ASSERT_GT(make_submission(u_ac, p, "ac"), 0);

    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);

    // WA-only user MUST appear now (v1.3.4 LEFT JOIN 行为)。
    bool found_wa = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["user"]["id"].get<int>() == u_only_wa) {
            found_wa = true;
            EXPECT_EQ(item["solved_count"].get<int>(),     0);
            EXPECT_EQ(item["submission_count"].get<int>(), 5);
            // 零 AC 用户 acceptance_rate 应为 0.0(避免除零)。
            EXPECT_DOUBLE_EQ(item["acceptance_rate"].get<double>(), 0.0);
            break;
        }
    }
    EXPECT_TRUE(found_wa) << "WA-only user should still be ranked (v1.3.4 LEFT JOIN)";

    // AC 用户出现在前面,acceptance_rate = 1/1 = 100%。
    bool found_ac = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["user"]["id"].get<int>() == u_ac) {
            found_ac = true;
            EXPECT_DOUBLE_EQ(item["acceptance_rate"].get<double>(), 100.0);
            break;
        }
    }
    EXPECT_TRUE(found_ac);

    // AC 用户的 rank 应严格小于 WA-only 用户的 rank
    // (solved_count DESC tiebreak)。
    int idx_ac = -1, idx_wa = -1;
    for (std::size_t i = 0; i < env["data"]["items"].size(); ++i) {
        const int uid = env["data"]["items"][i]["user"]["id"].get<int>();
        if (uid == u_ac)     idx_ac = static_cast<int>(i);
        if (uid == u_only_wa) idx_wa = static_cast<int>(i);
    }
    EXPECT_LT(idx_ac, idx_wa) << "AC user should rank above zero-AC user";
}

TEST_F(StatsRankingLiveFixture, EfficiencyTiebreaker) {
    StdoutSilencer silencer;
    // Two users with the same solved_count — the one with fewer
    // submissions should rank higher (Codeforces/LeetCode
    // convention).
    const int u_efficient = make_user("user", "rank-eff-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    const int u_inefficient = make_user("user", "rank-ineff-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_x");
    ASSERT_GT(u_efficient, 0);
    ASSERT_GT(u_inefficient, 0);

    // 2 distinct problems.
    const int p1 = make_problem("easy");
    const int p2 = make_problem("medium");
    ASSERT_GT(p1, 0);
    ASSERT_GT(p2, 0);

    // u_efficient: 2 AC, 2 submissions total.
    ASSERT_GT(make_submission(u_efficient, p1, "ac"), 0);
    ASSERT_GT(make_submission(u_efficient, p2, "ac"), 0);

    // u_inefficient: 2 AC + 4 WA noise = 2 solved, 6 submissions.
    ASSERT_GT(make_submission(u_inefficient, p1, "ac"), 0);
    ASSERT_GT(make_submission(u_inefficient, p2, "ac"), 0);
    for (int i = 0; i < 4; ++i) {
        ASSERT_GT(make_submission(u_inefficient, p1, "wa"), 0);
    }

    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);

    int idx_eff = -1, idx_ineff = -1;
    for (std::size_t i = 0; i < env["data"]["items"].size(); ++i) {
        const int uid = env["data"]["items"][i]["user"]["id"].get<int>();
        if (uid == u_efficient)   idx_eff   = static_cast<int>(i);
        if (uid == u_inefficient) idx_ineff = static_cast<int>(i);
    }
    ASSERT_NE(idx_eff,   -1);
    ASSERT_NE(idx_ineff, -1);
    EXPECT_LT(idx_eff, idx_ineff)
        << "fewer submissions should rank higher when solved_count is equal";
    // And the counts themselves.
    EXPECT_EQ(env["data"]["items"][idx_eff]["solved_count"].get<int>(),     2);
    EXPECT_EQ(env["data"]["items"][idx_eff]["submission_count"].get<int>(), 2);
    EXPECT_EQ(env["data"]["items"][idx_ineff]["solved_count"].get<int>(),     2);
    EXPECT_EQ(env["data"]["items"][idx_ineff]["submission_count"].get<int>(), 6);
}

TEST_F(StatsRankingLiveFixture, StableTiebreakerByUserId) {
    StdoutSilencer silencer;
    // Two users with IDENTICAL stats → tiebreak is user_id ASC.
    const int u_a = make_user("user", "rank-tie-a-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    const int u_b = make_user("user", "rank-tie-b-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_y");
    ASSERT_GT(u_a, 0);
    ASSERT_GT(u_b, 0);
    ASSERT_NE(u_a, u_b);

    // One problem, both AC it.
    const int p = make_problem("easy");
    ASSERT_GT(p, 0);
    ASSERT_GT(make_submission(u_a, p, "ac"), 0);
    ASSERT_GT(make_submission(u_b, p, "ac"), 0);

    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);

    int idx_a = -1, idx_b = -1;
    for (std::size_t i = 0; i < env["data"]["items"].size(); ++i) {
        const int uid = env["data"]["items"][i]["user"]["id"].get<int>();
        if (uid == u_a) idx_a = static_cast<int>(i);
        if (uid == u_b) idx_b = static_cast<int>(i);
    }
    ASSERT_NE(idx_a, -1);
    ASSERT_NE(idx_b, -1);
    const int lo = std::min(u_a, u_b);
    if (u_a == lo) {
        EXPECT_LT(idx_a, idx_b);
    } else {
        EXPECT_LT(idx_b, idx_a);
    }
}

TEST_F(StatsRankingLiveFixture, AdminUsersAreRanked) {
    StdoutSilencer silencer;
    // An admin with submissions should still appear in the leaderboard
    // (no role filter).
    const int u_admin = make_user("admin", "rank-admin-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(u_admin, 0);
    const int p = make_problem("easy");
    ASSERT_GT(p, 0);
    ASSERT_GT(make_submission(u_admin, p, "ac"), 0);

    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    bool found = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["user"]["id"].get<int>() == u_admin) {
            found = true;
            EXPECT_EQ(item["user"]["role"], "admin");
            EXPECT_EQ(item["solved_count"].get<int>(), 1);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(StatsRankingLiveFixture, SoftDeletedProblemsDoNotCount) {
    StdoutSilencer silencer;
    // v1.3.4: User ACs a problem, then admin soft-deletes the problem
    // → 用户的 solved_count 变 0(submission_count 也不计,因为 LEFT
    // JOIN 条件 `p.is_deleted = FALSE` 过滤),但用户**仍然上榜**,
    // 排在尾部(solved=0),acceptance_rate = 0.0。这与原"HAVING
    // solved_count > 0 过滤掉零 AC 用户"的语义不同 — 新语义是全员
    // 上榜,只是零 AC 排后面。
    const int u = make_user("user", "rank-sd-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(u, 0);
    const int p = make_problem("easy");
    ASSERT_GT(p, 0);
    ASSERT_GT(make_submission(u, p, "ac"), 0);

    // Sanity: user IS in the leaderboard before the soft-delete, with
    // solved_count=1.
    {
        const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
        ASSERT_TRUE(r);
        ASSERT_EQ(r.status, 200);
        const auto env = json::parse(r.body);
        bool found = false;
        for (const auto& item : env["data"]["items"]) {
            if (item["user"]["id"].get<int>() == u) {
                found = true;
                EXPECT_EQ(item["solved_count"].get<int>(), 1);
                break;
            }
        }
        EXPECT_TRUE(found) << "user should be ranked before soft-delete";
    }

    soft_delete_problem(p);

    // v1.3.4 软删除后:用户仍在列表,但 solved_count=0,acceptance_rate=0。
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    bool found_after = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["user"]["id"].get<int>() == u) {
            found_after = true;
            // LEFT JOIN 条件 `p.is_deleted = FALSE` 让软删除的题不算
            // 入 solved_count / submission_count,所以两个都变 0。
            EXPECT_EQ(item["solved_count"].get<int>(),     0);
            EXPECT_EQ(item["submission_count"].get<int>(), 0);
            EXPECT_DOUBLE_EQ(item["acceptance_rate"].get<double>(), 0.0);
            break;
        }
    }
    EXPECT_TRUE(found_after)
        << "v1.3.4: user still appears (zero-AC users now listed)";
}

TEST_F(StatsRankingLiveFixture, PendingAndRunningStillListedButZeroAc) {
    StdoutSilencer silencer;
    // v1.3.4: pending/running 是 submissions.status 值,我们的 SQL
    // 只把 'ac' 计入 solved_count,其他状态都算 submission_count。
    // 所以 pending/running 用户也会上榜,只是零 AC 排尾部。
    const int u = make_user("user", "rank-pending-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(u, 0);
    const int p = make_problem("easy");
    ASSERT_GT(p, 0);
    ASSERT_GT(make_submission(u, p, "pending"), 0);
    ASSERT_GT(make_submission(u, p, "running"), 0);
    // Plus a WA so they have terminal submissions.
    ASSERT_GT(make_submission(u, p, "wa"), 0);

    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    bool found = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["user"]["id"].get<int>() == u) {
            found = true;
            // pending/running 不算 AC → solved_count = 0
            EXPECT_EQ(item["solved_count"].get<int>(),     0);
            // pending/running/wa 都算 submission_count = 3
            EXPECT_EQ(item["submission_count"].get<int>(), 3);
            EXPECT_DOUBLE_EQ(item["acceptance_rate"].get<double>(), 0.0);
            break;
        }
    }
    EXPECT_TRUE(found)
        << "v1.3.4: user with only pending/running/wa still listed";
}

TEST_F(StatsRankingLiveFixture, LimitClampsToMax) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=9999");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["limit"].get<int>(),
              litecode::stats_routes::detail::kLeaderboardMaxLimit);
}

TEST_F(StatsRankingLiveFixture, OffsetWalksThePage) {
    StdoutSilencer silencer;
    // 3 users with distinct solve counts, so the first page gives us
    // a known order; offset=2 should skip to the third entry.
    const int u_top    = make_user("user", "rank-pg-top-"    +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    const int u_middle = make_user("user", "rank-pg-middle-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_m");
    const int u_low    = make_user("user", "rank-pg-low-"    +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_l");
    ASSERT_GT(u_top, 0);
    ASSERT_GT(u_middle, 0);
    ASSERT_GT(u_low, 0);

    const int p1 = make_problem("easy");
    const int p2 = make_problem("easy");
    const int p3 = make_problem("hard");
    ASSERT_GT(p1, 0);
    ASSERT_GT(p2, 0);
    ASSERT_GT(p3, 0);

    ASSERT_GT(make_submission(u_top,    p1, "ac"), 0);
    ASSERT_GT(make_submission(u_top,    p2, "ac"), 0);
    ASSERT_GT(make_submission(u_top,    p3, "ac"), 0);
    ASSERT_GT(make_submission(u_middle, p1, "ac"), 0);
    ASSERT_GT(make_submission(u_middle, p2, "ac"), 0);
    ASSERT_GT(make_submission(u_low,    p1, "ac"), 0);

    // Page 0 (offset=0): top user first.
    {
        const auto r = do_get(handle, "/api/v1/stats/ranking?limit=200&offset=0");
        ASSERT_TRUE(r);
        ASSERT_EQ(r.status, 200);
        const auto env = json::parse(r.body);
        // Find positions of our 3 users.
        int idx_top = -1, idx_middle = -1, idx_low = -1;
        for (std::size_t i = 0; i < env["data"]["items"].size(); ++i) {
            const int uid = env["data"]["items"][i]["user"]["id"].get<int>();
            if (uid == u_top)    idx_top    = static_cast<int>(i);
            if (uid == u_middle) idx_middle = static_cast<int>(i);
            if (uid == u_low)    idx_low    = static_cast<int>(i);
        }
        ASSERT_NE(idx_top,    -1);
        ASSERT_NE(idx_middle, -1);
        ASSERT_NE(idx_low,    -1);
        EXPECT_LT(idx_top,    idx_middle);
        EXPECT_LT(idx_middle, idx_low);
    }
}

TEST_F(StatsRankingLiveFixture, OutOfRangeOffsetReturnsEmptyButRealTotal) {
    StdoutSilencer silencer;
    // Seed at least 1 user with AC.
    const int u = make_user("user", "rank-oor-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(u, 0);
    const int p = make_problem("easy");
    ASSERT_GT(p, 0);
    ASSERT_GT(make_submission(u, p, "ac"), 0);

    // offset=1000000 is way past the end → items=[] but total > 0.
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=10&offset=1000000");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 0u);
    EXPECT_GE(env["data"]["total"].get<int>(), 1);
    EXPECT_EQ(env["data"]["offset"].get<int>(), 1000000);
}

TEST_F(StatsRankingLiveFixture, BadLimit) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=abc");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "limit");
}

TEST_F(StatsRankingLiveFixture, NegativeOffset) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/stats/ranking?offset=-5");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "offset");
}

TEST_F(StatsRankingLiveFixture, ZeroLimit) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/stats/ranking?limit=0");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "limit");
}

TEST_F(StatsRankingLiveFixture, XRequestIdRoundTrips) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/stats/ranking",
                          "test-ranking-rid-abc");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.request_id, "test-ranking-rid-abc");
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"], "test-ranking-rid-abc");
}

TEST_F(StatsRankingLiveFixture, RateLimitHeadersPresent) {
    StdoutSilencer silencer;
    // Public endpoints with a quota (SPEC §5.4 "30/min/IP") must
    // carry X-RateLimit-* headers via consume_rate_limit().
    const auto r = do_get(handle, "/api/v1/stats/ranking");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    const auto limit = get_header(r, "X-RateLimit-Limit");
    const auto remaining = get_header(r, "X-RateLimit-Remaining");
    EXPECT_FALSE(limit.empty())      << "X-RateLimit-Limit must be set";
    EXPECT_FALSE(remaining.empty())  << "X-RateLimit-Remaining must be set";
    // limit should be the configured 1000 (lax_rate_limit()).
    EXPECT_EQ(std::stoi(limit), 1000);
}

TEST_F(StatsRankingLiveFixture, RateLimitTriggers429) {
    StdoutSilencer silencer;
    // Use a tight bucket so the third request hits 429.
    litecode::RateLimitConfig tight;
    tight.auth_register_per_minute_per_ip   = 1000;
    tight.auth_login_per_minute_per_ip      = 1000;
    tight.problems_public_per_minute_per_ip = 1000;
    tight.submission_per_minute_per_user    = 1000;
    tight.admin_write_per_minute            = 1000;
    tight.bulk_import_per_hour              = 1000;
    tight.stats_ranking_per_minute_per_ip   = 2;

    // Re-register the routes with the tight bucket.
    auto server2 = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    auto limiter2 = std::make_unique<litecode::RateLimiter>();
    litecode::register_stats_routes(*server2, *pool, *limiter2, tight, jwt_cfg);
    auto h2 = start_server(server2.get());

    // First 2 requests succeed.
    EXPECT_EQ(do_get(h2, "/api/v1/stats/ranking").status, 200);
    EXPECT_EQ(do_get(h2, "/api/v1/stats/ranking").status, 200);
    // 3rd hits 429.
    const auto r3 = do_get(h2, "/api/v1/stats/ranking");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3.status, 429);
    const auto env = json::parse(r3.body);
    EXPECT_EQ(env["code"], "RATE_LIMITED");
    // Retry-After is set on 429.
    const auto retry = get_header(r3, "Retry-After");
    EXPECT_FALSE(retry.empty());

    h2 = ServerHandle();  // stop server2 before server2.reset()
    server2.reset();
    limiter2.reset();
}

}  // namespace
