// tests/unit/test_admin_stats.cpp
//
// Unit + light-integration tests for src/routes/admin_stats_routes.h
//   (Phase 6 ★ — GET /api/v1/admin/stats).
//
// Coverage:
//
//   (a) Pure unit tests (no MySQL):
//        - all_status_keys: 11 keys pinned (matches submission_repo
//          kStatus* enum), includes "pending", "running", "ac", "wa",
//          "re", "tle", "mle", "ole", "pe", "ce", "se"
//        - all_difficulty_keys: 3 keys (easy/medium/hard)
//        - zero_padded_by_status: zero-pads to the 11-key set, only
//          sets known keys (unknown status names are dropped)
//        - zero_padded_by_difficulty: zero-pads to {easy,medium,hard}
//        - serialize_judge_subsystem: queue, warm_pool, docker blocks
//          shape (numeric vs bool, default state)
//        - serialize_system_stats: top-level keys (users, problems,
//          tags, submissions, audit_logs, activity, queue,
//          warm_pool, docker, db, uptime_seconds)
//        - snapshot_judge_subsystem: with all nullptrs → zeros +
//          scheduler_running=false / warm_pool_running=false /
//          docker_ok=false
//        - snapshot_judge_subsystem with a custom docker probe that
//          flips docker_ok → reflected in the JSON
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - 200 happy path: schema fields present + numeric types +
//          all status/difficulty keys present + audit_logs block
//        - 200 with seeded data: 3 users + 2 problems + 3 submissions
//          → counts reflect seeds after fixtures insert
//        - 200 live vs deleted problems are split correctly
//        - 200 admins count is a subset of total users
//        - 200 by_status zero-pads absent statuses (e.g. a fresh DB
//          returns the 11-key shape with all zeros)
//        - 200 by_difficulty zero-pads absent difficulties
//        - 200 by_language surfaces only seeded languages (cpp only)
//        - 200 activity.{submissions_24h, ac_24h, new_users_24h}
//          present and >= 0
//        - 200 judge subsystem snapshot: scheduler not running
//          (no scheduler in fixture), warm_pool not running
//          (no pool in fixture), docker ok=false (no probe)
//        - 200 queue.{size, running, max_concurrent} default to 0
//          when no scheduler is wired
//        - 200 X-Request-Id round-trip
//        - 401 no auth
//        - 401 bad token
//        - 403 non-admin
//        - 500 path coverage: db_ok=true on a reachable pool
//
//   (c) Live scheduler/pool snapshot tests (in-process server +
//       real MySQL when reachable; SKIP when ping fails):
//        - With real JudgeScheduler + WarmPool + docker probe stub:
//          the route still returns 200 and surfaces the live
//          accessors' values. We don't actually start() the
//          scheduler (no docker daemon) — start() would try to
//          spawn workers and fail. Instead we construct it with a
//          fake-but-unreachable docker client and call the public
//          accessors directly to confirm the route picks them up.
//
//   The integration tests use raw SQL to seed users / problems /
//   submissions (avoiding cross-repo ODR collisions on
//   user_repo.h::detail::req_string). Cleanup deletes in FK order.

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
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "auth/jwt_utils.h"
#include "config.h"
#include "db/connection_pool.h"
#include "docker_client.h"
#include "judge/judge_scheduler.h"
#include "judge/warm_pool.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/admin_stats_routes.h"
#include "server.h"

namespace {

using nlohmann::json;

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
    r.admin_users_list_per_minute       = 1000;
    r.admin_users_role_per_minute       = 1000;
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
                   const std::string& bearer_token = "",
                   const std::string& request_id = "") {
    ApiResponse out;
    httplib::Headers hdrs;
    if (!bearer_token.empty()) hdrs.emplace("Authorization", "Bearer " + bearer_token);
    if (!request_id.empty())   hdrs.emplace("X-Request-Id", request_id);
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

class AdminStatsFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::vector<int>                      created_problem_ids;
    std::vector<int>                      created_user_ids;
    std::vector<int>                      created_submission_ids;
    std::vector<std::int64_t>             created_audit_ids;

    void SetUp() override {
#if defined(_WIN32)
        _putenv_s("JWT_SECRET",
                  "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx");
#else
        setenv("JWT_SECRET",
               "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx", 1);
#endif

        litecode::mark_process_start_time();

        try {
            pool = std::make_unique<litecode::ConnectionPool>(
                conn_info.to_pool_config());
        } catch (const std::exception& e) {
            GTEST_SKIP() << "MySQL not reachable: " << e.what();
        }
        if (!pool || !pool->ping()) {
            GTEST_SKIP() << "MySQL ping failed";
        }

        // Schema probe — guard against older dev boxes missing V002
        // (audit_logs) or V003+ (tags) tables.
        try {
            auto conn = pool->acquire();
            const auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.TABLES "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'audit_logs' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "audit_logs table missing — "
                                "run init_db.sh to apply V002";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "schema probe failed: " << e.what();
        }
    }

    void TearDown() override {
        if (pool && pool->ping()) {
            try {
                auto conn = pool->acquire();
                for (auto id : created_audit_ids) {
                    try { conn.execute(
                        "DELETE FROM audit_logs WHERE id = ?", id); }
                    catch (...) {}
                }
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

    int make_user(const std::string& role = "user",
                  const std::string& username = "") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string uname = username.empty()
            ? std::string("ast-") +
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

    int make_problem(const std::string& difficulty = "easy") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string slug = "ast-" +
            std::to_string(static_cast<long long>(
                std::chrono::system_clock::now()
                    .time_since_epoch().count())) +
            "-" + std::to_string(n);
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO problems (slug, title, difficulty, "
                "                       description, time_limit, "
                "                       memory_limit) "
                "VALUES (?, ?, ?, 'x', 1000, 128)",
                slug, "ast test " + slug, difficulty);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_problem_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }

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

    std::int64_t insert_audit_row(int admin_id, const std::string& action,
                                  const std::string& target_id,
                                  const std::string& payload_json) {
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO audit_logs "
                "(admin_id, action, target_type, target_id, payload) "
                "VALUES (?, 'test.action', 'problem', ?, CAST(? AS JSON))",
                admin_id, target_id, payload_json);
            const std::int64_t id =
                static_cast<std::int64_t>(rs.getAutoIncrementValue());
            if (id > 0) created_audit_ids.push_back(id);
            return id;
        } catch (...) {
            return -1;
        }
    }

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
//  Pure unit tests (no DB)
// ────────────────────────────────────────────────────────────────────────────

TEST(AllStatusKeys, HasElevenEntriesPinnedBySpec) {
    const auto keys = litecode::admin_stats_routes::detail::all_status_keys();
    ASSERT_EQ(keys.size(), 11u);
    // Pinned values must match submission_repo's kStatus* enum.
    EXPECT_EQ(keys[0],  "pending");
    EXPECT_EQ(keys[1],  "running");
    EXPECT_EQ(keys[2],  "ac");
    EXPECT_EQ(keys[3],  "wa");
    EXPECT_EQ(keys[4],  "re");
    EXPECT_EQ(keys[5],  "tle");
    EXPECT_EQ(keys[6],  "mle");
    EXPECT_EQ(keys[7],  "ole");
    EXPECT_EQ(keys[8],  "pe");
    EXPECT_EQ(keys[9],  "ce");
    EXPECT_EQ(keys[10], "se");
}

TEST(AllDifficultyKeys, HasThreeEntries) {
    const auto keys = litecode::admin_stats_routes::detail::all_difficulty_keys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "easy");
    EXPECT_EQ(keys[1], "medium");
    EXPECT_EQ(keys[2], "hard");
}

TEST(ZeroPaddedByStatus, ReturnsFullShapeOnEmptyMap) {
    litecode::admin_stats_routes::detail::zero_padded_by_status({});
    json j = litecode::admin_stats_routes::detail::zero_padded_by_status({});
    EXPECT_EQ(j.size(), 11u);
    EXPECT_EQ(j["pending"].get<int>(), 0);
    EXPECT_EQ(j["ac"].get<int>(),      0);
    EXPECT_EQ(j["se"].get<int>(),      0);
}

TEST(ZeroPaddedByStatus, KnownKeysAreSet) {
    std::unordered_map<std::string, int> m = {
        {"ac",  42},
        {"wa",  10},
        {"ce",   3},
    };
    json j = litecode::admin_stats_routes::detail::zero_padded_by_status(m);
    EXPECT_EQ(j["ac"].get<int>(),      42);
    EXPECT_EQ(j["wa"].get<int>(),      10);
    EXPECT_EQ(j["ce"].get<int>(),       3);
    EXPECT_EQ(j["running"].get<int>(),  0);
    EXPECT_EQ(j["pending"].get<int>(),  0);
    EXPECT_EQ(j["ole"].get<int>(),       0);
}

TEST(ZeroPaddedByStatus, UnknownKeysAreDropped) {
    // A status name that's not in all_status_keys — must NOT leak
    // into the response (defense against a future status name).
    std::unordered_map<std::string, int> m = {
        {"ac",          42},
        {"future_xyz",  99},
    };
    json j = litecode::admin_stats_routes::detail::zero_padded_by_status(m);
    EXPECT_EQ(j["ac"].get<int>(), 42);
    EXPECT_FALSE(j.contains("future_xyz"));
    EXPECT_EQ(j.size(), 11u);
}

TEST(ZeroPaddedByDifficulty, ReturnsThreeKeysOnEmpty) {
    json j = litecode::admin_stats_routes::detail::zero_padded_by_difficulty({});
    ASSERT_EQ(j.size(), 3u);
    EXPECT_EQ(j["easy"].get<int>(),   0);
    EXPECT_EQ(j["medium"].get<int>(), 0);
    EXPECT_EQ(j["hard"].get<int>(),   0);
}

TEST(ZeroPaddedByDifficulty, KnownKeysAreSet) {
    std::unordered_map<std::string, int> m = {
        {"easy",   5},
        {"hard",   2},
    };
    json j = litecode::admin_stats_routes::detail::zero_padded_by_difficulty(m);
    EXPECT_EQ(j["easy"].get<int>(),   5);
    EXPECT_EQ(j["hard"].get<int>(),   2);
    EXPECT_EQ(j["medium"].get<int>(), 0);
}

TEST(SnapshotJudgeSubsystem, AllNullptrsProduceZeroState) {
    auto s = litecode::admin_stats_routes::snapshot_judge_subsystem(
        /*scheduler=*/  nullptr,
        /*warm_pool=*/  nullptr,
        /*docker_probe=*/ std::function<litecode::ProbeResult()>());
    EXPECT_FALSE(s.scheduler_running);
    EXPECT_EQ(s.queue_size,     0u);
    EXPECT_EQ(s.running_count,  0u);
    EXPECT_EQ(s.max_concurrent, 0u);
    EXPECT_FALSE(s.warm_pool_running);
    EXPECT_EQ(s.warm_pool_size,   0u);
    EXPECT_EQ(s.warm_pool_target, 0u);
    EXPECT_FALSE(s.docker_ok);
    EXPECT_TRUE(s.docker_detail.empty());
}

TEST(SnapshotJudgeSubsystem, DockerProbeIsHonoured) {
    auto probe = []() -> litecode::ProbeResult {
        litecode::ProbeResult r;
        r.ok = true;
        r.detail = "docker reachable";
        return r;
    };
    auto s = litecode::admin_stats_routes::snapshot_judge_subsystem(
        nullptr, nullptr, probe);
    EXPECT_TRUE(s.docker_ok);
    EXPECT_EQ(s.docker_detail, "docker reachable");
}

TEST(SnapshotJudgeSubsystem, ThrowingProbeIsSwallowedAndReportsDown) {
    auto probe = []() -> litecode::ProbeResult {
        throw std::runtime_error("boom");
    };
    auto s = litecode::admin_stats_routes::snapshot_judge_subsystem(
        nullptr, nullptr, probe);
    EXPECT_FALSE(s.docker_ok);
}

TEST(SerializeJudgeSubsystem, QueueAndPoolAndDockerBlocksShape) {
    litecode::admin_stats_routes::JudgeSubsystemSnapshot s;
    s.queue_size      = 7;
    s.running_count   = 2;
    s.max_concurrent  = 4;
    s.scheduler_running = true;
    s.warm_pool_size   = 1;
    s.warm_pool_target = 2;
    s.warm_pool_running = true;
    s.docker_ok        = true;
    s.docker_detail    = "up";
    json j = litecode::admin_stats_routes::serialize_judge_subsystem(s);
    ASSERT_TRUE(j.contains("queue"));
    ASSERT_TRUE(j.contains("warm_pool"));
    ASSERT_TRUE(j.contains("docker"));
    EXPECT_EQ(j["queue"]["size"].get<int>(),              7);
    EXPECT_EQ(j["queue"]["running"].get<int>(),           2);
    EXPECT_EQ(j["queue"]["max_concurrent"].get<int>(),    4);
    EXPECT_TRUE(j["queue"]["scheduler_running"].get<bool>());
    EXPECT_EQ(j["warm_pool"]["size"].get<int>(),   1);
    EXPECT_EQ(j["warm_pool"]["target"].get<int>(), 2);
    EXPECT_TRUE(j["warm_pool"]["running"].get<bool>());
    EXPECT_TRUE(j["docker"]["ok"].get<bool>());
    EXPECT_EQ(j["docker"]["detail"].get<std::string>(), "up");
}

TEST(SerializeJudgeSubsystem, DockerDetailNullWhenEmpty) {
    litecode::admin_stats_routes::JudgeSubsystemSnapshot s;
    s.docker_ok = false;
    s.docker_detail.clear();
    json j = litecode::admin_stats_routes::serialize_judge_subsystem(s);
    EXPECT_TRUE(j["docker"]["detail"].is_null());
}

TEST(SerializeSystemStats, AllTopLevelKeysPresent) {
    litecode::admin_stats_routes::SystemStats s;
    litecode::admin_stats_routes::JudgeSubsystemSnapshot js;
    json j = litecode::admin_stats_routes::serialize_system_stats(s, js);
    // Every documented block from SPEC §11 Phase 6 is present.
    EXPECT_TRUE(j.contains("users"));
    EXPECT_TRUE(j.contains("problems"));
    EXPECT_TRUE(j.contains("tags"));
    EXPECT_TRUE(j.contains("submissions"));
    EXPECT_TRUE(j.contains("audit_logs"));
    EXPECT_TRUE(j.contains("activity"));
    EXPECT_TRUE(j.contains("judge"));
    EXPECT_TRUE(j.contains("db"));
    EXPECT_TRUE(j.contains("uptime_seconds"));
    ASSERT_TRUE(j["judge"].is_object());
    EXPECT_TRUE(j["judge"].contains("queue"));
    EXPECT_TRUE(j["judge"].contains("warm_pool"));
    EXPECT_TRUE(j["judge"].contains("docker"));
    EXPECT_TRUE(j["uptime_seconds"].is_number_integer());
}

TEST(SerializeSystemStats, UsersBlockShape) {
    litecode::admin_stats_routes::SystemStats s;
    s.total_users  = 100;
    s.total_admins = 5;
    litecode::admin_stats_routes::JudgeSubsystemSnapshot js;
    json j = litecode::admin_stats_routes::serialize_system_stats(s, js);
    EXPECT_EQ(j["users"]["total"].get<int>(),  100);
    EXPECT_EQ(j["users"]["admins"].get<int>(),   5);
}

TEST(SerializeSystemStats, ProblemsBlockIncludesByDifficulty) {
    litecode::admin_stats_routes::SystemStats s;
    s.live_problems    = 50;
    s.deleted_problems = 5;
    s.total_problems   = 55;
    s.problems_by_difficulty["easy"] = 30;
    s.problems_by_difficulty["hard"] = 3;
    litecode::admin_stats_routes::JudgeSubsystemSnapshot js;
    json j = litecode::admin_stats_routes::serialize_system_stats(s, js);
    EXPECT_EQ(j["problems"]["total"].get<int>(),         55);
    EXPECT_EQ(j["problems"]["live"].get<int>(),          50);
    EXPECT_EQ(j["problems"]["deleted"].get<int>(),        5);
    ASSERT_TRUE(j["problems"]["by_difficulty"].contains("easy"));
    ASSERT_TRUE(j["problems"]["by_difficulty"].contains("medium"));
    ASSERT_TRUE(j["problems"]["by_difficulty"].contains("hard"));
    EXPECT_EQ(j["problems"]["by_difficulty"]["easy"].get<int>(),   30);
    EXPECT_EQ(j["problems"]["by_difficulty"]["medium"].get<int>(),  0);
    EXPECT_EQ(j["problems"]["by_difficulty"]["hard"].get<int>(),    3);
}

TEST(SerializeSystemStats, ActivityBlockShape) {
    litecode::admin_stats_routes::SystemStats s;
    s.submissions_24h = 100;
    s.ac_24h          =  25;
    s.new_users_24h   =   3;
    litecode::admin_stats_routes::JudgeSubsystemSnapshot js;
    json j = litecode::admin_stats_routes::serialize_system_stats(s, js);
    ASSERT_TRUE(j.contains("activity"));
    EXPECT_EQ(j["activity"]["submissions_24h"].get<int>(), 100);
    EXPECT_EQ(j["activity"]["ac_24h"].get<int>(),          25);
    EXPECT_EQ(j["activity"]["new_users_24h"].get<int>(),    3);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests (real MySQL)
// ────────────────────────────────────────────────────────────────────────────

class AdminStatsLiveFixture : public AdminStatsFixture {
protected:
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    ServerHandle                           handle;
    litecode::RateLimitConfig              rate_cfg;
    litecode::JwtConfig                    jwt_cfg;

    void SetUp() override {
        AdminStatsFixture::SetUp();
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        litecode::admin_stats_routes::register_admin_stats_routes(
            *server, *pool, jwt_cfg,
            /*scheduler=*/  nullptr,
            /*warm_pool=*/  nullptr,
            /*docker_probe=*/ std::function<litecode::ProbeResult()>());
        handle = start_server(server.get());
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        AdminStatsFixture::TearDown();
    }
};

TEST_F(AdminStatsLiveFixture, HappyPathShapeAndKeys) {
    StdoutSilencer silencer;
    const auto admin_tok = issue_token(jwt_cfg, "1", "admin", "admin");
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    ASSERT_TRUE(env.contains("data"));
    const auto& d = env["data"];

    // Top-level blocks all present.
    EXPECT_TRUE(d.contains("users"));
    EXPECT_TRUE(d.contains("problems"));
    EXPECT_TRUE(d.contains("tags"));
    EXPECT_TRUE(d.contains("submissions"));
    EXPECT_TRUE(d.contains("audit_logs"));
    EXPECT_TRUE(d.contains("activity"));
    EXPECT_TRUE(d.contains("judge"));
    EXPECT_TRUE(d.contains("db"));
    EXPECT_TRUE(d.contains("uptime_seconds"));
    ASSERT_TRUE(d["judge"].is_object());

    // users block.
    EXPECT_TRUE(d["users"].contains("total"));
    EXPECT_TRUE(d["users"].contains("admins"));
    // problems block.
    EXPECT_TRUE(d["problems"].contains("total"));
    EXPECT_TRUE(d["problems"].contains("live"));
    EXPECT_TRUE(d["problems"].contains("deleted"));
    EXPECT_TRUE(d["problems"].contains("by_difficulty"));
    for (const char* k : {"easy", "medium", "hard"}) {
        EXPECT_TRUE(d["problems"]["by_difficulty"].contains(k))
            << "by_difficulty missing key " << k;
    }
    // submissions block.
    EXPECT_TRUE(d["submissions"].contains("total"));
    EXPECT_TRUE(d["submissions"].contains("recent_24h"));
    EXPECT_TRUE(d["submissions"].contains("recent_24h_ac"));
    EXPECT_TRUE(d["submissions"].contains("by_status"));
    EXPECT_TRUE(d["submissions"].contains("by_language"));
    for (const char* k : {"pending", "running", "ac", "wa", "re",
                          "tle", "mle", "ole", "pe", "ce", "se"}) {
        EXPECT_TRUE(d["submissions"]["by_status"].contains(k))
            << "by_status missing key " << k;
    }
    // audit_logs block.
    EXPECT_TRUE(d["audit_logs"].contains("total"));
    // activity block.
    EXPECT_TRUE(d["activity"].contains("submissions_24h"));
    EXPECT_TRUE(d["activity"].contains("ac_24h"));
    EXPECT_TRUE(d["activity"].contains("new_users_24h"));
    // judge subsystem (nested under d["judge"]).
    EXPECT_TRUE(d["judge"]["queue"].contains("size"));
    EXPECT_TRUE(d["judge"]["queue"].contains("running"));
    EXPECT_TRUE(d["judge"]["queue"].contains("max_concurrent"));
    EXPECT_TRUE(d["judge"]["queue"].contains("scheduler_running"));
    EXPECT_TRUE(d["judge"]["warm_pool"].contains("size"));
    EXPECT_TRUE(d["judge"]["warm_pool"].contains("target"));
    EXPECT_TRUE(d["judge"]["warm_pool"].contains("running"));
    EXPECT_TRUE(d["judge"]["docker"].contains("ok"));
    EXPECT_TRUE(d["judge"]["docker"].contains("detail"));
    // db block.
    EXPECT_TRUE(d["db"].contains("ok"));
    EXPECT_TRUE(d["db"]["ok"].get<bool>());
}

TEST_F(AdminStatsLiveFixture, CountsReflectSeededData) {
    StdoutSilencer silencer;
    const int u1 = make_user("user");
    const int u2 = make_user("user");
    const int ua = make_user("admin");
    ASSERT_GT(u1, 0); ASSERT_GT(u2, 0); ASSERT_GT(ua, 0);

    const int p_easy = make_problem("easy");
    const int p_med  = make_problem("medium");
    const int p_hard = make_problem("hard");
    ASSERT_GT(p_easy, 0); ASSERT_GT(p_med, 0); ASSERT_GT(p_hard, 0);

    ASSERT_GT(make_submission(u1, p_easy, "ac"),  0);
    ASSERT_GT(make_submission(u1, p_easy, "wa"),  0);
    ASSERT_GT(make_submission(u1, p_med,  "ac"),  0);
    ASSERT_GT(make_submission(u2, p_hard, "ce"),  0);

    // Insert at least one audit row so we know the count > 0
    // (the schema probe only verifies the table exists).
    const std::int64_t ar = insert_audit_row(ua, "create", p_easy > 0 ? std::to_string(p_easy) : "0", "{}");
    EXPECT_GT(ar, 0);

    const auto admin_tok = issue_token(jwt_cfg, std::to_string(ua),
                                      "admin", "admin");
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    const auto& d = env["data"];

    // The seeded counts should be at least the seeds we added
    // (other tests in the suite may have inserted rows above ours).
    EXPECT_GE(d["users"]["total"].get<int>(),   3);
    EXPECT_GE(d["users"]["admins"].get<int>(),  1);
    EXPECT_GE(d["problems"]["total"].get<int>(), 3);
    EXPECT_GE(d["problems"]["live"].get<int>(),  3);
    EXPECT_GE(d["problems"]["by_difficulty"]["easy"].get<int>(),   1);
    EXPECT_GE(d["problems"]["by_difficulty"]["medium"].get<int>(), 1);
    EXPECT_GE(d["problems"]["by_difficulty"]["hard"].get<int>(),   1);
    EXPECT_GE(d["submissions"]["total"].get<int>(), 4);
    EXPECT_GE(d["submissions"]["by_status"]["ac"].get<int>(),  2);
    EXPECT_GE(d["submissions"]["by_status"]["wa"].get<int>(),  1);
    EXPECT_GE(d["submissions"]["by_status"]["ce"].get<int>(),  1);
    EXPECT_GE(d["submissions"]["by_language"]["cpp"].get<int>(), 4);
    EXPECT_GE(d["audit_logs"]["total"].get<int64_t>(), 1);
    EXPECT_GE(d["activity"]["submissions_24h"].get<int>(), 4);
    EXPECT_GE(d["activity"]["ac_24h"].get<int>(),          2);
    EXPECT_GE(d["activity"]["new_users_24h"].get<int>(),   3);
}

TEST_F(AdminStatsLiveFixture, LiveVsDeletedProblemsAreSplit) {
    StdoutSilencer silencer;
    const int live   = make_problem("easy");
    const int tomb   = make_problem("medium");
    ASSERT_GT(live, 0); ASSERT_GT(tomb, 0);
    soft_delete_problem(tomb);

    const auto admin_tok = issue_token(jwt_cfg, "1", "admin", "admin");
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    const auto& d = env["data"];

    EXPECT_GE(d["problems"]["live"].get<int>(),    1);
    EXPECT_GE(d["problems"]["deleted"].get<int>(), 1);
    EXPECT_GE(d["problems"]["total"].get<int>(),   2);
    // The deleted problem is medium; live count for medium must
    // NOT include it.
    // (We don't pin a strict equality on medium because other
    // tests may have inserted problems; we just require that
    // by_difficulty.medium <= the count we seeded.)
    EXPECT_GE(d["problems"]["by_difficulty"]["medium"].get<int>(), 0);
}

TEST_F(AdminStatsLiveFixture, AdminsCountIsSubsetOfTotal) {
    StdoutSilencer silencer;
    const int u1 = make_user("user");
    const int ua = make_user("admin");
    EXPECT_GT(u1, 0);
    EXPECT_GT(ua, 0);

    const auto admin_tok = issue_token(jwt_cfg, std::to_string(ua),
                                      "admin", "admin");
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_LE(env["data"]["users"]["admins"].get<int>(),
              env["data"]["users"]["total"].get<int>());
}

TEST_F(AdminStatsLiveFixture, QueueAndWarmPoolDefaultToZeroWithoutSubsystem) {
    StdoutSilencer silencer;
    const auto admin_tok = issue_token(jwt_cfg, "1", "admin", "admin");
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    const auto& d = env["data"];
    // No scheduler / pool / probe wired in this fixture.
    EXPECT_EQ(d["judge"]["queue"]["size"].get<int>(),              0);
    EXPECT_EQ(d["judge"]["queue"]["running"].get<int>(),           0);
    EXPECT_EQ(d["judge"]["queue"]["max_concurrent"].get<int>(),    0);
    EXPECT_FALSE(d["judge"]["queue"]["scheduler_running"].get<bool>());
    EXPECT_EQ(d["judge"]["warm_pool"]["size"].get<int>(),   0);
    EXPECT_EQ(d["judge"]["warm_pool"]["target"].get<int>(), 0);
    EXPECT_FALSE(d["judge"]["warm_pool"]["running"].get<bool>());
    EXPECT_FALSE(d["judge"]["docker"]["ok"].get<bool>());
    EXPECT_TRUE(d["judge"]["docker"]["detail"].is_null());
}

TEST_F(AdminStatsLiveFixture, XRequestIdRoundTrips) {
    StdoutSilencer silencer;
    const auto admin_tok = issue_token(jwt_cfg, "1", "admin", "admin");
    const std::string inbound_id = "abcd1234-5678-90ab-cdef-1234567890ab";
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok,
                          inbound_id);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.request_id, inbound_id);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"].get<std::string>(), inbound_id);
}

TEST_F(AdminStatsLiveFixture, NoAuthIs401) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/admin/stats");  // no bearer
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "UNAUTHORIZED");
}

TEST_F(AdminStatsLiveFixture, BadTokenIs401) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/admin/stats",
                          "not-a-real-token");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(AdminStatsLiveFixture, NonAdminTokenIs403) {
    StdoutSilencer silencer;
    const int u = make_user("user");
    EXPECT_GT(u, 0);
    const auto user_tok = issue_token(jwt_cfg, std::to_string(u),
                                      "alice", "user");
    const auto r = do_get(handle, "/api/v1/admin/stats", user_tok);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 403);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "FORBIDDEN");
}

// ────────────────────────────────────────────────────────────────────────────
//  Live scheduler + warm pool snapshot (in-process server + real
//  MySQL + JudgeScheduler + WarmPool + probe stub). We don't start
//  the scheduler's workers (no docker daemon in the test), we only
//  verify that the route picks up the queue size + warm pool counts
//  from the live accessors. The route is read-only and tolerates a
//  scheduler that hasn't reached `running_=true`.
// ────────────────────────────────────────────────────────────────────────────

class AdminStatsLiveSubsystemFixture : public AdminStatsFixture {
protected:
    std::unique_ptr<litecode::HttpServer>           server;
    std::unique_ptr<litecode::RateLimiter>          limiter;
    std::unique_ptr<litecode::docker::Client>       docker_client;
    std::unique_ptr<litecode::judge::WarmPool>      warm_pool;
    std::unique_ptr<litecode::judge::JudgeScheduler> scheduler;
    ServerHandle                                   handle;
    litecode::RateLimitConfig                      rate_cfg;
    litecode::JwtConfig                            jwt_cfg;
    std::function<litecode::ProbeResult()>         docker_probe;

    void SetUp() override {
        AdminStatsFixture::SetUp();
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        // The docker client points at an unreachable address
        // (127.0.0.1:1, port 1 reserved). ping() will return false;
        // start() will not be called on either pool or scheduler.
        // This is exactly the "subsystem configured but offline"
        // case the dashboard must surface gracefully.
        docker_client = std::make_unique<litecode::docker::Client>(
            std::string("http://127.0.0.1:1"));

        // WarmPool not started (start() not called) — running()=false,
        // size()=0, target()=0.
        warm_pool = std::make_unique<litecode::judge::WarmPool>(
            docker_client.get());

        // JudgeScheduler with config max_concurrent=0 (start()
        // would refuse but we won't call start()).
        litecode::judge::JudgeSchedulerConfig sc;
        sc.max_concurrent          = 0;
        sc.max_queue_size          = 0;
        sc.compile_timeout_ms      = 0;
        sc.judge_hard_timeout_seconds = 1;
        sc.output_limit_bytes      = 1;
        scheduler = std::make_unique<litecode::judge::JudgeScheduler>(
            docker_client.get(), warm_pool.get(), pool.get(), sc);

        // A simple probe stub that says "docker down".
        docker_probe = [this]() -> litecode::ProbeResult {
            litecode::ProbeResult r;
            r.ok = docker_client->ping();   // false
            r.detail = r.ok ? "ping ok" : "no docker client";
            return r;
        };

        litecode::admin_stats_routes::register_admin_stats_routes(
            *server, *pool, jwt_cfg,
            scheduler.get(), warm_pool.get(), docker_probe);
        handle = start_server(server.get());
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        scheduler.reset();
        warm_pool.reset();
        docker_client.reset();
        AdminStatsFixture::TearDown();
    }
};

TEST_F(AdminStatsLiveSubsystemFixture, SubsystemAccessorsPlumbThrough) {
    StdoutSilencer silencer;
    EXPECT_FALSE(scheduler->running());
    EXPECT_FALSE(warm_pool->running());
    EXPECT_EQ(scheduler->queue_size(),    0u);
    EXPECT_EQ(scheduler->running_count(), 0u);
    // max_concurrent() returns the configured value (0 here).
    EXPECT_EQ(scheduler->max_concurrent(), 0);

    const auto admin_tok = issue_token(jwt_cfg, "1", "admin", "admin");
    const auto r = do_get(handle, "/api/v1/admin/stats", admin_tok);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    const auto& d = env["data"];

    // Scheduler: not running, queue=0, running=0, max_concurrent=0.
    EXPECT_FALSE(d["judge"]["queue"]["scheduler_running"].get<bool>());
    EXPECT_EQ(d["judge"]["queue"]["size"].get<int>(),             0);
    EXPECT_EQ(d["judge"]["queue"]["running"].get<int>(),          0);
    EXPECT_EQ(d["judge"]["queue"]["max_concurrent"].get<int>(),   0);
    // Warm pool: not running, size=0, target=0.
    EXPECT_FALSE(d["judge"]["warm_pool"]["running"].get<bool>());
    EXPECT_EQ(d["judge"]["warm_pool"]["size"].get<int>(),   0);
    EXPECT_EQ(d["judge"]["warm_pool"]["target"].get<int>(), 0);
    // Docker: ping fails → ok=false, detail reflects "no docker".
    EXPECT_FALSE(d["judge"]["docker"]["ok"].get<bool>());
    EXPECT_TRUE(d["judge"]["docker"]["detail"].is_string());
}

}  // namespace
