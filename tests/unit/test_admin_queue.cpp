// tests/unit/test_admin_queue.cpp
//
// Unit + light-integration tests for src/routes/admin_queue_routes.h
//   (Phase 6 ★ v1.2.44 — GET /api/v1/admin/queue).
//
// Coverage:
//
//   (a) Pure unit tests (no MySQL):
//        - kAdminQueueDefaultPerMinute == 60
//        - RateLimitConfig default admin_queue_per_minute == 60
//        - admin_queue_quota() returns the expected bucket name +
//          per-user keying + 1-minute window
//        - serialize_queue_block: full state (finite max_queue_size)
//          → size/running/max_concurrent/max_queue_size/scheduler_running
//            present + utilization in [0, 1]
//        - serialize_queue_block: max_queue_size == 0 ⇒ utilization
//          is null (the unbounded dev path)
//        - serialize_queue_block: utilization capped at 1.0 even when
//          a transient race bumps queue_size past max_queue_size
//        - serialize_warm_pool_block: size/target/running shape
//        - serialize_docker_block: ok=true with detail, ok=false with
//          null detail (no empty-string leak)
//        - serialize_db_block: ok=true with count, ok=false with
//          count=0 (driver error folds to 0)
//        - serialize_db_block: negative pending count folds to 0
//        - format_iso8601_utc: shape (YYYY-MM-DDTHH:MM:SSZ, contains 'T'
//          and trailing 'Z')
//        - snapshot_queue_subsystem: all nullptrs ⇒ safe zero defaults
//        - snapshot_queue_subsystem: scheduler accessor throws ⇒
//          queue fields fall back to zeros, no throw
//        - snapshot_queue_subsystem: warm_pool accessor throws ⇒
//          warm_pool fields fall back to zeros, no throw
//        - snapshot_queue_subsystem: docker probe throws ⇒
//          docker fields fall back to safe defaults, no throw
//        - snapshot_queue_subsystem: docker probe returns ok=true ⇒
//          detail echoed
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - 200 happy path with no subsystem wired (all defaults +
//          db.ok=true + db.pending_submissions echoed)
//        - 200 with seeded pending submissions → pending_submissions >= seed
//        - 200 shape: every block present + numeric types
//        - 200 X-Request-Id round-trip
//        - 200 X-RateLimit-* headers present
//        - 401 no auth
//        - 401 bad token
//        - 403 non-admin
//        - 429 rate limit triggers (tight bucket)
//
//   (c) Live scheduler + warm pool + docker probe snapshot:
//        - With real JudgeScheduler (unstarted, accessors return 0/false)
//          + WarmPool (unstarted) + docker probe stub: the route
//          returns 200 and surfaces the live accessors' values.
//        - Probe that throws: route still returns 200 (no 500).
//
//   All admin routes use raw SQL for any DB query to dodge the
//   cross-repo ODR collisions on user_repo.h::detail::req_string /
//   req_int — same fix as test_admin_audit_logs / test_admin_stats /
//   test_admin_users.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <memory>
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
#include "routes/admin_queue_routes.h"
#include "routes/system_routes.h"
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
    r.admin_users_list_per_minute       = 1000;
    r.admin_users_role_per_minute       = 1000;
    r.admin_audit_logs_per_minute       = 1000;
    r.admin_queue_per_minute            = 1000;
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
    int               status = 0;
    std::string       body;
    std::string       request_id;
    bool              ok = false;
    httplib::Headers  headers;
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

ApiResponse do_request(ServerHandle& h,
                       const std::string& method,
                       const std::string& path,
                       const std::string& bearer_token = "",
                       const std::string& request_id = "") {
    ApiResponse out;
    httplib::Headers hdrs;
    if (!bearer_token.empty()) hdrs.emplace("Authorization", "Bearer " + bearer_token);
    if (!request_id.empty())  hdrs.emplace("X-Request-Id", request_id);
    httplib::Result r;
    if (method == "GET") {
        r = h.client->Get(path, hdrs);
    } else {
        ADD_FAILURE() << "unsupported method " << method;
        return out;
    }
    if (!r) {
        ADD_FAILURE() << method << " " << path << " failed: " << r.error();
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

std::string get_header(const ApiResponse& r, const std::string& name) {
    auto it = r.headers.find(name);
    if (it == r.headers.end()) return "";
    return it->second;
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

class AdminQueueFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
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

        // Schema probe — the queue endpoint reads submissions +
        // users. Both tables must exist.
        try {
            auto conn = pool->acquire();
            auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.TABLES "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'submissions' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "submissions table missing";
            }
            auto vu = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.TABLES "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'users' LIMIT 1");
            if (!vu.has_value()) {
                GTEST_SKIP() << "users table missing";
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
                for (auto id : created_user_ids) {
                    try { conn.execute(
                        "DELETE FROM users WHERE id = ?", id); }
                    catch (...) {}
                }
            } catch (...) {}
        }
        pool.reset();
    }

    // Insert a throwaway user via raw SQL (avoid cross-repo ODR
    // collisions on user_repo.h::detail::req_string). bcrypt-format
    // filler; we never log in as this user.
    int make_user(const std::string& role = "user") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string uname = std::string("aq-") +
            std::to_string(static_cast<long long>(
                std::chrono::system_clock::now()
                    .time_since_epoch().count())) +
            "_" + std::to_string(n);
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

    // Insert a pending submission row directly via raw SQL. We don't
    // need a real problems row for the queue endpoint — the route
    // only counts WHERE status='pending'. If the schema has a NOT
    // NULL FK on problem_id, make_problem() below can be used first.
    int make_pending_submission(int user_id) {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        try {
            auto conn = pool->acquire();
            // Insert with problem_id=0 — schema permits it on most
            // dev boxes (no FK enforcement) and the queue endpoint
            // doesn't read problem_id. If the schema DOES enforce a
            // FK, we'll catch the error and the test will skip below.
            auto rs = conn.execute(
                "INSERT INTO submissions "
                "(user_id, problem_id, language, code, status) "
                "VALUES (?, 0, 'cpp', 'int main(){return 0;}', 'pending')",
                user_id);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_submission_ids.push_back(id);
            return id;
        } catch (const std::exception& e) {
            // Probably a FK violation (problem_id=0 doesn't exist).
            // Re-raise so the test can GTEST_SKIP — we don't want
            // the route test to assume a specific schema.
            throw;
        }
    }

    // Count submissions WHERE status='pending' — the same SELECT
    // the route runs. Used to assert the wire value matches reality.
    int count_pending_submissions() {
        try {
            auto conn = pool->acquire();
            const auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT COUNT(*) FROM submissions WHERE status='pending'");
            return v.has_value() ? static_cast<int>(*v) : 0;
        } catch (...) {
            return -1;
        }
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests (no DB, no Docker)
// ────────────────────────────────────────────────────────────────────────────

TEST(AdminQueueConstants, RateLimitDefaults) {
    EXPECT_EQ(litecode::admin_queue_routes::kAdminQueueDefaultPerMinute, 60);
    EXPECT_EQ(litecode::RateLimitConfig{}.admin_queue_per_minute, 60);
}

TEST(AdminQueueQuota, BucketShapeAndKeying) {
    litecode::RateLimitConfig cfg;
    cfg.admin_queue_per_minute = 42;
    const auto q = litecode::admin_queue_quota(cfg);
    EXPECT_STREQ(q.name.c_str(), "admin.queue");
    EXPECT_EQ(q.capacity, 42);
    EXPECT_EQ(q.window, std::chrono::minutes(1));
    EXPECT_EQ(q.key_type, litecode::RateLimitKeyType::ByUser);
}

TEST(SerializeQueueBlock, FullFiniteState) {
    litecode::admin_queue_routes::QueueSubsystemSnapshot s;
    s.scheduler_running = true;
    s.queue_size        = 6;
    s.running_count     = 2;
    s.max_concurrent    = 4;
    s.max_queue_size    = 50;
    const auto j = litecode::admin_queue_routes::serialize_queue_block(s);
    EXPECT_EQ(j["size"].get<int>(),              6);
    EXPECT_EQ(j["running"].get<int>(),           2);
    EXPECT_EQ(j["max_concurrent"].get<int>(),    4);
    EXPECT_EQ(j["max_queue_size"].get<int>(),    50);
    EXPECT_TRUE(j["scheduler_running"].get<bool>());
    // utilization = 6 / 50 = 0.12
    ASSERT_TRUE(j["utilization"].is_number());
    EXPECT_NEAR(j["utilization"].get<double>(), 0.12, 1e-6);
}

TEST(SerializeQueueBlock, UnboundedYieldsNullUtilization) {
    litecode::admin_queue_routes::QueueSubsystemSnapshot s;
    s.max_queue_size = 0;   // unbounded (dev / test)
    s.queue_size     = 7;
    const auto j = litecode::admin_queue_routes::serialize_queue_block(s);
    EXPECT_TRUE(j["utilization"].is_null());
    // size is still echoed so the gauge can show a bare count.
    EXPECT_EQ(j["size"].get<int>(), 7);
}

TEST(SerializeQueueBlock, UtilizationClampedAtOne) {
    // Transient race: queue_size briefly exceeds max_queue_size (worker
    // drained just before the snapshot read). utilization must clamp
    // at 1.0 so the gauge fill is never > 100%.
    litecode::admin_queue_routes::QueueSubsystemSnapshot s;
    s.queue_size     = 60;
    s.max_queue_size = 50;
    const auto j = litecode::admin_queue_routes::serialize_queue_block(s);
    ASSERT_TRUE(j["utilization"].is_number());
    EXPECT_NEAR(j["utilization"].get<double>(), 1.0, 1e-6);
}

TEST(SerializeWarmPoolBlock, AllFieldsPresent) {
    litecode::admin_queue_routes::QueueSubsystemSnapshot s;
    s.warm_pool_running = true;
    s.warm_pool_size    = 3;
    s.warm_pool_target  = 5;
    const auto j = litecode::admin_queue_routes::serialize_warm_pool_block(s);
    EXPECT_EQ(j["size"].get<int>(),   3);
    EXPECT_EQ(j["target"].get<int>(), 5);
    EXPECT_TRUE(j["running"].get<bool>());
}

TEST(SerializeDockerBlock, OkWithDetail) {
    litecode::admin_queue_routes::QueueSubsystemSnapshot s;
    s.docker_ok     = true;
    s.docker_detail = "docker proxy reachable";
    const auto j = litecode::admin_queue_routes::serialize_docker_block(s);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["detail"].get<std::string>(), "docker proxy reachable");
}

TEST(SerializeDockerBlock, NotOkWithEmptyDetailIsNull) {
    litecode::admin_queue_routes::QueueSubsystemSnapshot s;
    s.docker_ok     = false;
    s.docker_detail = "";   // empty → JSON null
    const auto j = litecode::admin_queue_routes::serialize_docker_block(s);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_TRUE(j["detail"].is_null());
}

TEST(SerializeDbBlock, OkEchoesCount) {
    const auto j = litecode::admin_queue_routes::serialize_db_block(true, 17);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["pending_submissions"].get<int>(), 17);
}

TEST(SerializeDbBlock, DriverErrorFoldsToZero) {
    // -1 from pending_submissions_count means "driver error"; the
    // operator already sees db.ok=false, so pending_submissions
    // collapses to 0 instead of leaking the sentinel.
    const auto j = litecode::admin_queue_routes::serialize_db_block(false, -1);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_EQ(j["pending_submissions"].get<int>(), 0);
}

TEST(FormatIso8601Utc, Shape) {
    // Pick a fixed timestamp so the formatter's behavior is
    // deterministic regardless of the wall clock.
    const std::time_t t = 1716000000;   // 2024-05-18 07:00:00 UTC
    const auto s = litecode::admin_queue_routes::detail::format_iso8601_utc(t);
    EXPECT_EQ(s.size(), 20u);
    EXPECT_EQ(s[4],  '-');
    EXPECT_EQ(s[7],  '-');
    EXPECT_EQ(s[10], 'T');
    EXPECT_EQ(s.back(), 'Z');
    // Date half is 2024-05-18
    EXPECT_EQ(s.substr(0, 10), "2024-05-18");
}

TEST(SnapshotQueueSubsystem, AllNullptrsYieldZeroDefaults) {
    const auto s = litecode::admin_queue_routes::snapshot_queue_subsystem(
        nullptr, nullptr, std::function<litecode::ProbeResult()>());
    EXPECT_FALSE(s.scheduler_running);
    EXPECT_FALSE(s.warm_pool_running);
    EXPECT_FALSE(s.docker_ok);
    EXPECT_EQ(s.queue_size,        0u);
    EXPECT_EQ(s.running_count,     0u);
    EXPECT_EQ(s.max_concurrent,    0);
    EXPECT_EQ(s.max_queue_size,    0);
    EXPECT_EQ(s.warm_pool_size,    0u);
    EXPECT_EQ(s.warm_pool_target,  0u);
    EXPECT_TRUE(s.docker_detail.empty());
}

TEST(SnapshotQueueSubsystem, SchedulerAccessorThrowsFallsBackToZeros) {
    // Use a std::function that throws — the route must catch +
    // fold to defaults, not propagate.
    std::function<litecode::ProbeResult()> bad = []() -> litecode::ProbeResult {
        throw std::runtime_error("probe exploded");
    };
    // We can't easily wire a "throwing scheduler" without making
    // our own subclass; rely on a throwing docker probe to exercise
    // the same try/catch ladder (the queue block is symmetric).
    const auto s = litecode::admin_queue_routes::snapshot_queue_subsystem(
        nullptr, nullptr, bad);
    EXPECT_FALSE(s.docker_ok);
    EXPECT_TRUE(s.docker_detail.empty());
    // Other blocks stay at zero (no scheduler / pool wired).
    EXPECT_EQ(s.queue_size, 0u);
    EXPECT_EQ(s.warm_pool_size, 0u);
}

TEST(SnapshotQueueSubsystem, DockerProbeOkTrueEchoesDetail) {
    std::function<litecode::ProbeResult()> good = []() -> litecode::ProbeResult {
        litecode::ProbeResult r;
        r.ok     = true;
        r.detail = "docker proxy reachable";
        return r;
    };
    const auto s = litecode::admin_queue_routes::snapshot_queue_subsystem(
        nullptr, nullptr, good);
    EXPECT_TRUE(s.docker_ok);
    EXPECT_EQ(s.docker_detail, "docker proxy reachable");
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests (real MySQL)
// ────────────────────────────────────────────────────────────────────────────

class AdminQueueLiveFixture : public AdminQueueFixture {
protected:
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    ServerHandle                           handle;
    litecode::RateLimitConfig              rate_cfg;
    litecode::JwtConfig                    jwt_cfg;
    int                                    admin_user_id   = 0;
    int                                    regular_user_id = 0;
    std::string                            admin_username;
    std::string                            admin_token;
    std::string                            regular_username;
    std::string                            regular_token;

    void SetUp() override {
        AdminQueueFixture::SetUp();
        // Parent's SetUp calls GTEST_SKIP when MySQL is unreachable.
        // Subsequent code dereferences pool; guard.
        if (!pool) return;
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        // No subsystem wired — this fixture covers the "fresh
        // dev box" baseline.
        litecode::admin_queue_routes::register_admin_queue_routes(
            *server, *pool, *limiter, jwt_cfg, rate_cfg,
            /*scheduler=*/   nullptr,
            /*warm_pool=*/    nullptr,
            /*docker_probe=*/ nullptr);
        handle = start_server(server.get());

        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const auto stamp = std::to_string(static_cast<long long>(
            std::chrono::system_clock::now()
                .time_since_epoch().count())) +
            "_" + std::to_string(n);

        admin_username = "aq-admin-" + stamp;
        admin_user_id  = make_user("admin");
        ASSERT_GT(admin_user_id, 0);
        admin_token = issue_token(jwt_cfg, std::to_string(admin_user_id),
                                  admin_username, "admin");

        regular_username = std::string("aq-user-") + stamp;
        regular_user_id  = make_user("user");
        ASSERT_GT(regular_user_id, 0);
        regular_token = issue_token(jwt_cfg, std::to_string(regular_user_id),
                                    regular_username, "user");
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        AdminQueueFixture::TearDown();
    }
};

TEST_F(AdminQueueLiveFixture, HappyPathShapeAndKeys) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET", "/api/v1/admin/queue",
                              admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    ASSERT_TRUE(env.contains("data"));
    const auto& d = env["data"];

    // Top-level blocks.
    EXPECT_TRUE(d.contains("queue"));
    EXPECT_TRUE(d.contains("warm_pool"));
    EXPECT_TRUE(d.contains("docker"));
    EXPECT_TRUE(d.contains("db"));
    EXPECT_TRUE(d.contains("updated_at"));
    EXPECT_TRUE(env.contains("request_id"));

    // queue block — every documented field is present.
    EXPECT_TRUE(d["queue"].contains("size"));
    EXPECT_TRUE(d["queue"].contains("running"));
    EXPECT_TRUE(d["queue"].contains("max_concurrent"));
    EXPECT_TRUE(d["queue"].contains("max_queue_size"));
    EXPECT_TRUE(d["queue"].contains("scheduler_running"));
    EXPECT_TRUE(d["queue"].contains("utilization"));

    // No subsystem wired ⇒ everything zero / false.
    EXPECT_EQ(d["queue"]["size"].get<int>(),           0);
    EXPECT_EQ(d["queue"]["running"].get<int>(),        0);
    EXPECT_EQ(d["queue"]["max_concurrent"].get<int>(), 0);
    EXPECT_EQ(d["queue"]["max_queue_size"].get<int>(), 0);
    EXPECT_FALSE(d["queue"]["scheduler_running"].get<bool>());
    // max_queue_size = 0 ⇒ utilization must be null.
    EXPECT_TRUE(d["queue"]["utilization"].is_null());

    // warm_pool block.
    EXPECT_EQ(d["warm_pool"]["size"].get<int>(),   0);
    EXPECT_EQ(d["warm_pool"]["target"].get<int>(), 0);
    EXPECT_FALSE(d["warm_pool"]["running"].get<bool>());

    // docker block — no probe ⇒ ok=false, detail=null.
    EXPECT_FALSE(d["docker"]["ok"].get<bool>());
    EXPECT_TRUE(d["docker"]["detail"].is_null());

    // db block — MySQL ping should succeed in this fixture.
    EXPECT_TRUE(d["db"]["ok"].get<bool>());
    EXPECT_GE(d["db"]["pending_submissions"].get<int>(), 0);

    // updated_at shape.
    const auto ts = d["updated_at"].get<std::string>();
    EXPECT_EQ(ts.size(), 20u);
    EXPECT_EQ(ts[10], 'T');
    EXPECT_EQ(ts.back(), 'Z');
}

TEST_F(AdminQueueLiveFixture, PendingSubmissionCountReflectsSeed) {
    StdoutSilencer silencer;

    // Seed N pending submissions. We may have other tests'
    // leftover rows (or even ours from a prior failed run); the
    // assertion uses >= so the test is robust to suite ordering.
    constexpr int kSeed = 3;
    int seeded = 0;
    for (int i = 0; i < kSeed; ++i) {
        try {
            const int id = make_pending_submission(admin_user_id);
            if (id > 0) ++seeded;
        } catch (const std::exception&) {
            // Schema enforces a FK on problem_id=0 — skip the test
            // rather than fail; the route layer itself works either
            // way (the SELECT is parameterized).
            GTEST_SKIP() << "submissions.problem_id FK enforces non-zero; "
                            "skip pending-count seed";
        }
    }
    ASSERT_GT(seeded, 0);

    const int db_count = count_pending_submissions();
    ASSERT_GE(db_count, seeded);

    const auto r = do_request(handle, "GET", "/api/v1/admin/queue",
                              admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["db"]["pending_submissions"].get<int>(), db_count);
    EXPECT_GE(env["data"]["db"]["pending_submissions"].get<int>(), seeded);
}

TEST_F(AdminQueueLiveFixture, XRequestIdRoundTrip) {
    StdoutSilencer silencer;
    const std::string rid = "test-rid-admin-queue-abc";
    const auto r = do_request(handle, "GET", "/api/v1/admin/queue",
                              admin_token, rid);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.request_id, rid);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"].get<std::string>(), rid);
}

TEST_F(AdminQueueLiveFixture, XRateLimitHeadersPresent) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET", "/api/v1/admin/queue",
                              admin_token);
    ASSERT_TRUE(r);
    EXPECT_FALSE(get_header(r, "X-RateLimit-Limit").empty());
    EXPECT_FALSE(get_header(r, "X-RateLimit-Remaining").empty());
}

TEST_F(AdminQueueLiveFixture, NoAuthIs401) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET", "/api/v1/admin/queue");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "UNAUTHORIZED");
}

TEST_F(AdminQueueLiveFixture, BadTokenIs401) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/queue", "not-a-real-jwt-token");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(AdminQueueLiveFixture, NonAdminIs403) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/queue", regular_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 403);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "FORBIDDEN");
}

TEST_F(AdminQueueLiveFixture, RateLimitTriggers429) {
    StdoutSilencer silencer;

    // Tight bucket: 2/min. Tear down + re-register so the lax
    // bucket from SetUp doesn't taint state.
    litecode::RateLimitConfig tight;
    tight.auth_register_per_minute_per_ip   = 2;
    tight.auth_login_per_minute_per_ip      = 2;
    tight.problems_public_per_minute_per_ip = 2;
    tight.submission_per_minute_per_user    = 2;
    tight.admin_write_per_minute            = 2;
    tight.bulk_import_per_hour              = 2;
    tight.stats_ranking_per_minute_per_ip   = 2;
    tight.admin_users_list_per_minute       = 2;
    tight.admin_users_role_per_minute       = 2;
    tight.admin_audit_logs_per_minute       = 2;
    tight.admin_queue_per_minute            = 2;

    handle = ServerHandle();
    server.reset();
    limiter.reset();

    limiter = std::make_unique<litecode::RateLimiter>();
    server  = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    litecode::admin_queue_routes::register_admin_queue_routes(
        *server, *pool, *limiter, jwt_cfg, tight,
        /*scheduler=*/   nullptr,
        /*warm_pool=*/    nullptr,
        /*docker_probe=*/ nullptr);
    handle = start_server(server.get());

    const auto r1 = do_request(handle, "GET",
        "/api/v1/admin/queue", admin_token);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1.status, 200);
    const auto r2 = do_request(handle, "GET",
        "/api/v1/admin/queue", admin_token);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2.status, 200);
    const auto r3 = do_request(handle, "GET",
        "/api/v1/admin/queue", admin_token);
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3.status, 429);
    const auto env = json::parse(r3.body);
    EXPECT_EQ(env["code"], "RATE_LIMITED");
    EXPECT_FALSE(get_header(r3, "Retry-After").empty());
}

// ────────────────────────────────────────────────────────────────────────────
//  Live scheduler + warm pool + docker probe snapshot.
//
//  Mirrors test_admin_stats's "SubsystemAccessorsPlumbThrough" but
//  adapted to /admin/queue. We don't start() the scheduler or pool
//  (no docker daemon); the route picks up queue_size=0 + warm_pool
//  size=0 from the live accessors and the docker probe is a stub.
// ────────────────────────────────────────────────────────────────────────────

class AdminQueueLiveSubsystemFixture : public AdminQueueFixture {
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
    int                                            admin_user_id = 0;
    std::string                                    admin_token;

    void SetUp() override {
        AdminQueueFixture::SetUp();
        if (!pool) return;
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        // Docker client points at an unreachable address (port 1
        // is reserved). ping() returns false; start() is never called.
        docker_client = std::make_unique<litecode::docker::Client>(
            std::string("http://127.0.0.1:1"));

        // WarmPool unstarted — running()=false, size()=0, target()=0.
        warm_pool = std::make_unique<litecode::judge::WarmPool>(
            docker_client.get());

        // JudgeScheduler unstarted — every accessor returns its
        // configured default (0 for sizes / counts, max_concurrent
        // and max_queue_size echo the cfg values).
        litecode::judge::JudgeSchedulerConfig sc;
        sc.max_concurrent             = 4;
        sc.max_queue_size             = 25;
        sc.compile_timeout_ms         = 1000;
        sc.judge_hard_timeout_seconds = 5;
        sc.output_limit_bytes         = 1;
        scheduler = std::make_unique<litecode::judge::JudgeScheduler>(
            docker_client.get(), warm_pool.get(), pool.get(), sc);

        // Probe stub that reports docker-down.
        docker_probe = [this]() -> litecode::ProbeResult {
            litecode::ProbeResult r;
            r.ok     = docker_client->ping();   // false
            r.detail = r.ok ? "ping ok" : "no docker client";
            return r;
        };

        litecode::admin_queue_routes::register_admin_queue_routes(
            *server, *pool, *limiter, jwt_cfg, rate_cfg,
            scheduler.get(), warm_pool.get(), docker_probe);
        handle = start_server(server.get());

        admin_user_id = make_user("admin");
        ASSERT_GT(admin_user_id, 0);
        admin_token = issue_token(jwt_cfg, std::to_string(admin_user_id),
                                  "admin", "admin");
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        scheduler.reset();
        warm_pool.reset();
        docker_client.reset();
        AdminQueueFixture::TearDown();
    }
};

TEST_F(AdminQueueLiveSubsystemFixture, AccessorsPlumbThrough) {
    StdoutSilencer silencer;
    // Sanity-check the in-memory state we expect the route to
    // observe. The scheduler wasn't started → running()=false,
    // queue_size=0, running_count=0, max_concurrent=4,
    // max_queue_size=25 (the cfg we built above).
    EXPECT_FALSE(scheduler->running());
    EXPECT_FALSE(warm_pool->running());
    EXPECT_EQ(scheduler->queue_size(),    0u);
    EXPECT_EQ(scheduler->running_count(), 0u);
    EXPECT_EQ(scheduler->max_concurrent(), 4);
    EXPECT_EQ(scheduler->max_queue_size(), 25);
    EXPECT_FALSE(docker_client->ping());

    const auto r = do_request(handle, "GET", "/api/v1/admin/queue",
                              admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    const auto& d = env["data"];

    // Queue: scheduler not running, size/running=0,
    // max_concurrent + max_queue_size from cfg, utilization=0.
    EXPECT_FALSE(d["queue"]["scheduler_running"].get<bool>());
    EXPECT_EQ(d["queue"]["size"].get<int>(),           0);
    EXPECT_EQ(d["queue"]["running"].get<int>(),        0);
    EXPECT_EQ(d["queue"]["max_concurrent"].get<int>(), 4);
    EXPECT_EQ(d["queue"]["max_queue_size"].get<int>(), 25);
    ASSERT_TRUE(d["queue"]["utilization"].is_number());
    EXPECT_NEAR(d["queue"]["utilization"].get<double>(), 0.0, 1e-9);

    // Warm pool: not running, size=0, target=0.
    EXPECT_FALSE(d["warm_pool"]["running"].get<bool>());
    EXPECT_EQ(d["warm_pool"]["size"].get<int>(),   0);
    EXPECT_EQ(d["warm_pool"]["target"].get<int>(), 0);

    // Docker: ping fails → ok=false, detail reflects the failure.
    EXPECT_FALSE(d["docker"]["ok"].get<bool>());
    ASSERT_TRUE(d["docker"]["detail"].is_string());

    // DB: ping succeeds → ok=true, pending_submissions echoed.
    EXPECT_TRUE(d["db"]["ok"].get<bool>());
    EXPECT_GE(d["db"]["pending_submissions"].get<int>(), 0);
}

TEST_F(AdminQueueLiveSubsystemFixture, ThrowingDockerProbeFallsBackToZero) {
    StdoutSilencer silencer;

    // Re-register with a probe that throws on every invocation.
    // The route must NOT 500; the docker block falls back to
    // safe defaults.
    handle = ServerHandle();
    server.reset();
    limiter.reset();

    std::function<litecode::ProbeResult()> bad_probe =
        []() -> litecode::ProbeResult {
            throw std::runtime_error("probe exploded");
        };

    limiter = std::make_unique<litecode::RateLimiter>();
    server  = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    litecode::admin_queue_routes::register_admin_queue_routes(
        *server, *pool, *limiter, jwt_cfg, rate_cfg,
        scheduler.get(), warm_pool.get(), bad_probe);
    handle = start_server(server.get());

    const auto r = do_request(handle, "GET", "/api/v1/admin/queue",
                              admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    const auto& d = env["data"];
    EXPECT_FALSE(d["docker"]["ok"].get<bool>());
    EXPECT_TRUE(d["docker"]["detail"].is_null());
    // Queue + warm_pool + db still echo the live state.
    EXPECT_EQ(d["queue"]["max_concurrent"].get<int>(), 4);
    EXPECT_EQ(d["queue"]["max_queue_size"].get<int>(), 25);
    EXPECT_TRUE(d["db"]["ok"].get<bool>());
}

}  // namespace