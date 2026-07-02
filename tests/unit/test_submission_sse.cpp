// tests/unit/test_submission_sse.cpp
//
// Unit + light-integration tests for Phase 4 ★ SSE push on submission
// results. Covers:
//   (a) Pure unit tests (no DB / docker):
//        - detail::extract_sse_id_from_path shape
//        - constants (kSseContentType / kSseRetryIntervalMs /
//          kSseWaitTimeoutMs) are pinned to the documented values
//        - format_sse_event / format_sse_error_event shape
//        - JudgeNotifier publish / subscribe / unsubscribe / wait_for
//          in isolation (no JudgeScheduler wired)
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - GET /api/v1/submissions/sse/:id 401 when no auth
//        - GET .../sse/abc 400 when id is not numeric
//        - GET .../sse/ 400 when no id tail
//        - GET .../sse/99999 404 when row is unknown
//        - GET .../sse/<id> 403 when non-admin watches another
//          user's submission
//        - GET .../sse/<id> 200 admin watches any submission
//        - GET .../sse/<id> 200 + Content-Type: text/event-stream
//          when row is already terminal (fast path: no
//          wait_for)
//        - GET .../sse/<id> 200 + "result" event after the
//          JudgeScheduler worker calls publish() (async round-
//          trip: SSE handler subscribes to notifier; worker
//          calls publish; SSE handler unblocks with the row)
//
//  Mirrors the test_submission.cpp / test_judge_scheduler.cpp
//  pattern: real MySQL when reachable, in-process mock proxy
//  for the docker side, raw SQL for user/problem seeding
//  (avoids user_repo.h's litecode::detail::req_string ODR
//  collision across multiple repos in the same TU).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "auth/jwt_utils.h"
#include "config.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"
#include "db/submission_repo.h"
#include "db/test_case_repo.h"
#include "judge/judge_notifier.h"
#include "judge/judge_scheduler.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/submission_routes.h"
#include "server.h"

namespace {

using nlohmann::json;
using litecode::judge::JudgeNotifier;
using litecode::judge::JudgeScheduler;
using litecode::judge::JudgeSchedulerConfig;
using litecode::judge::JudgeTask;
using litecode::judge::SubscriberScope;

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
    std::string captured() const { return sink_.str(); }
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

litecode::JudgeConfig minimal_judge_cfg() {
    litecode::JudgeConfig jc;
    jc.judge_image              = "litecode-judge:latest";
    jc.network_mode             = "none";
    jc.warm_pool_size           = 0;
    jc.max_concurrent_judges    = 1;
    jc.max_queue_size           = 50;
    jc.compile_timeout_seconds  = 5;
    jc.judge_hard_timeout_seconds = 5;
    jc.output_limit_bytes       = 16 * 1024 * 1024;
    jc.pids_limit               = "50";
    return jc;
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

// ServerHandle — RAII wrapper around an in-process HttpServer +
// Client. Mirrors test_submission.cpp's pattern.
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

ServerHandle start_server(litecode::HttpServer* server) {
    const int port = server->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0) << "bind_any_port failed";
    if (!server->start(/*background=*/true)) {
        std::fprintf(stderr, "[start_server] start() returned false; "
                     "running=%d\n", server->is_running() ? 1 : 0);
    }
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(30, 0);
    client->set_write_timeout(30, 0);
    client->set_keep_alive(false);
    return ServerHandle(server, client.release(), port);
}

template <typename Pred>
bool wait_until(Pred pred,
                std::chrono::milliseconds timeout =
                    std::chrono::milliseconds(5000),
                std::chrono::milliseconds step =
                    std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

// ────────────────────────────────────────────────────────────────────────────
//  Mock docker proxy — mirrors test_submission.cpp / test_judge_scheduler.cpp
// ────────────────────────────────────────────────────────────────────────────

class MockDockerProxy {
public:
    void start() {
        srv_.Get("/_ping",
            [this](const httplib::Request&, httplib::Response& res) {
                res.status = 200;
                res.set_content("OK", "text/plain");
            });

        srv_.Post("/containers/create",
            [this](const httplib::Request& req, httplib::Response& res) {
                ++create_calls;
                std::ostringstream id;
                id << "mock-" << std::setw(4) << std::setfill('0')
                   << ++created_seq;
                last_create_body = req.body;
                json body{{"Id", id.str()}, {"Warnings", json::array()}};
                res.status = 201;
                res.set_content(body.dump(), "application/json");
            });

        srv_.Post(R"(/containers/([^/]+)/start)",
            [](const httplib::Request&, httplib::Response& res) {
                res.status = 204;
            });

        srv_.Post(R"(/containers/([^/]+)/wait)",
            [this](const httplib::Request&, httplib::Response& res) {
                json body{{"StatusCode", 0}, {"Error", nullptr}};
                res.status = 200;
                res.set_content(body.dump(), "application/json");
            });

        srv_.Get(R"(/containers/([^/]+)/logs)",
            [this](const httplib::Request&, httplib::Response& res) {
                res.status = 200;
                res.set_content(prebuilt_logs_response_, "text/plain");
            });

        srv_.Post(R"(/containers/([^/]+)/kill)",
            [](const httplib::Request&, httplib::Response& res) {
                res.status = 204;
            });

        srv_.Delete(R"(/containers/([^/]+))",
            [this](const httplib::Request&, httplib::Response& res) {
                ++delete_calls;
                res.status = 204;
            });

        port_ = srv_.bind_to_any_port("127.0.0.1");
        ASSERT_TRUE(port_ > 0);
        thread_ = std::thread([this]{ srv_.listen_after_bind(); });
        srv_.wait_until_ready();
    }

    void stop() {
        if (port_ > 0) srv_.stop();
        if (thread_.joinable()) thread_.join();
        port_ = 0;
    }
    ~MockDockerProxy() { stop(); }

    std::string url() const {
        std::ostringstream os;
        os << "http://127.0.0.1:" << port_;
        return os.str();
    }

    std::atomic<int> create_calls{0};
    std::atomic<int> delete_calls{0};
    std::atomic<int> created_seq{0};
    std::string      last_create_body;
    std::string      prebuilt_logs_response_ =
        R"({"submission_id":0,"status":"ac","time_used_ms":7,"memory_used_kb":512,"error_message":null,"failed_case_index":null,"case_results":[]})";

private:
    httplib::Server srv_;
    std::thread     thread_;
    int             port_ = 0;
};

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

class SseFixture : public ::testing::Test {
protected:
    DbConn                                  conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::vector<int>                        created_problem_ids;
    std::vector<int>                        created_user_ids;
    std::vector<int>                        created_submission_ids;
    std::filesystem::path                   task_dir;

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
        try {
            auto conn = pool->acquire();
            auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'submissions' "
                "  AND COLUMN_NAME = 'finished_at' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "submissions.finished_at missing";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "schema probe failed: " << e.what();
        }

        std::error_code ec;
        task_dir = std::filesystem::temp_directory_path() /
                   "litecode-sse-test";
        std::filesystem::remove_all(task_dir, ec);
        std::filesystem::create_directory(task_dir, ec);
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

    int make_user(const std::string& role = "user") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string username =
            std::string("sse-") +
            std::to_string(static_cast<long long>(
                std::chrono::system_clock::now()
                    .time_since_epoch().count())) +
            "_" + std::to_string(n);
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO users (username, password_hash, role) "
                "VALUES (?, '$2b$12$dummy.hash.for.test.only.padding.aaaa', ?)",
                username, role);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_user_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }

    int make_problem() {
        litecode::ProblemRow p;
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        p.slug = "sse-" +
                 std::to_string(static_cast<long long>(
                     std::chrono::system_clock::now()
                         .time_since_epoch().count())) +
                 "-" + std::to_string(n);
        p.title = "sse test problem";
        p.difficulty = "easy";
        p.description = "auto-generated";
        p.time_limit = 1000;
        p.memory_limit = 128;
        const int pid = litecode::problem_repo::create(*pool, p);
        if (pid <= 0) return 0;
        created_problem_ids.push_back(pid);
        litecode::test_case_repo::insert(
            *pool, pid, "1 2\n", "3\n",
            /*is_sample=*/false, "exact", /*order_num=*/0);
        return pid;
    }

    int make_submission(int user_id, int problem_id) {
        litecode::SubmissionRow s;
        s.user_id    = user_id;
        s.problem_id = problem_id;
        s.language   = "cpp";
        s.code       = "int main(){return 0;}";
        const int id = litecode::submission_repo::create(*pool, s);
        if (id > 0) created_submission_ids.push_back(id);
        return id;
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — extract_sse_id_from_path
// ────────────────────────────────────────────────────────────────────────────

TEST(ExtractSseIdFromPath, AcceptsPositiveInt) {
    httplib::Request req;
    req.path = "/api/v1/submissions/sse/42";
    const auto v = litecode::detail::extract_sse_id_from_path(req);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(ExtractSseIdFromPath, RejectsNonNumeric) {
    httplib::Request req;
    req.path = "/api/v1/submissions/sse/abc";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

TEST(ExtractSseIdFromPath, RejectsZero) {
    httplib::Request req;
    req.path = "/api/v1/submissions/sse/0";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

TEST(ExtractSseIdFromPath, RejectsNegative) {
    httplib::Request req;
    req.path = "/api/v1/submissions/sse/-1";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

TEST(ExtractSseIdFromPath, RejectsNested) {
    httplib::Request req;
    req.path = "/api/v1/submissions/sse/42/extra";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

TEST(ExtractSseIdFromPath, RejectsEmptyTail) {
    httplib::Request req;
    req.path = "/api/v1/submissions/sse/";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

TEST(ExtractSseIdFromPath, RejectsPrefixMismatch) {
    httplib::Request req;
    req.path = "/api/v1/submissions/42";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

TEST(ExtractSseIdFromPath, RejectsCompletelyWrongPath) {
    httplib::Request req;
    req.path = "/api/v1/something/else/42";
    EXPECT_FALSE(litecode::detail::extract_sse_id_from_path(req).has_value());
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — constants
// ────────────────────────────────────────────────────────────────────────────

TEST(SseConstants, ContentTypeIsTextEventStream) {
    EXPECT_STREQ(litecode::detail::kSseContentType,
                 "text/event-stream; charset=utf-8");
}

TEST(SseConstants, RetryIntervalIs3000) {
    EXPECT_EQ(litecode::detail::kSseRetryIntervalMs, 3000);
}

TEST(SseConstants, WaitTimeoutIs25000) {
    EXPECT_EQ(litecode::kSseWaitTimeoutMs, 25000);
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — format_sse_event / format_sse_error_event
// ────────────────────────────────────────────────────────────────────────────

TEST(FormatSseEvent, EmitsEventNameAndDataAndTrailingBlank) {
    const std::string out = litecode::detail::format_sse_event(
        "result", json{{"id", 7}, {"status", "ac"}});
    EXPECT_NE(out.find("event: result"), std::string::npos);
    EXPECT_NE(out.find("data: "),         std::string::npos);
    // The data line must carry the JSON payload.
    EXPECT_NE(out.find("\"id\":7"),       std::string::npos);
    EXPECT_NE(out.find("\"status\":\"ac\""), std::string::npos);
    // The frame must end with a blank line (two LF).
    ASSERT_GE(out.size(), 2u);
    EXPECT_EQ(out[out.size() - 1], '\n');
    EXPECT_EQ(out[out.size() - 2], '\n');
}

TEST(FormatSseEvent, EmptyDataIsValid) {
    const std::string out = litecode::detail::format_sse_event("ping", json::object());
    EXPECT_NE(out.find("event: ping"), std::string::npos);
    EXPECT_NE(out.find("data: {}"),   std::string::npos);
}

TEST(FormatSseErrorEvent, EmitsErrorEventAndStatus) {
    const std::string out = litecode::detail::format_sse_error_event(
        404, litecode::ErrorCode::NOT_FOUND, "submission not found");
    EXPECT_NE(out.find("event: error"), std::string::npos);
    EXPECT_NE(out.find("\"code\":\"NOT_FOUND\""), std::string::npos);
    EXPECT_NE(out.find("\"message\":\"submission not found\""), std::string::npos);
    EXPECT_NE(out.find("\"status\":404"), std::string::npos);
}

TEST(FormatSseErrorEvent, CarriesHttpStatusCode) {
    const std::string out = litecode::detail::format_sse_error_event(
        401, litecode::ErrorCode::UNAUTHORIZED, "no token");
    EXPECT_NE(out.find("\"status\":401"), std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — JudgeNotifier
// ────────────────────────────────────────────────────────────────────────────

TEST(JudgeNotifierUnit, PublishWithNoSubscribersReturnsZero) {
    JudgeNotifier n;
    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "ac";
    EXPECT_EQ(n.publish(row), 0u);
    EXPECT_EQ(n.subscriber_count_for(1), 0u);
    EXPECT_EQ(n.total_subscribers(),     0u);
}

TEST(JudgeNotifierUnit, SubscribeAndPublishDeliversRow) {
    JudgeNotifier n;
    std::optional<litecode::SubmissionRow> captured;
    std::atomic<bool> fired{false};
    (void)n.subscribe(42, [&](const litecode::SubmissionRow& r) {
        captured = r;
        fired.store(true);
    });
    EXPECT_EQ(n.subscriber_count_for(42), 1u);

    litecode::SubmissionRow row;
    row.id     = 42;
    row.status = "ac";
    row.time_used = 12;
    row.memory_used = 1024;
    const std::size_t n_recv = n.publish(row);
    EXPECT_EQ(n_recv, 1u);
    EXPECT_TRUE(fired.load());
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->id, 42);
    EXPECT_EQ(captured->status, "ac");
    EXPECT_EQ(captured->time_used.value_or(0), 12);
    // publish erases the map entry on fire.
    EXPECT_EQ(n.subscriber_count_for(42), 0u);
}

TEST(JudgeNotifierUnit, MultipleSubscribersAllReceive) {
    JudgeNotifier n;
    std::atomic<int> a{0}, b{0}, c{0};
    (void)n.subscribe(1, [&](const litecode::SubmissionRow&){ ++a; });
    (void)n.subscribe(1, [&](const litecode::SubmissionRow&){ ++b; });
    (void)n.subscribe(1, [&](const litecode::SubmissionRow&){ ++c; });
    EXPECT_EQ(n.total_subscribers(), 3u);

    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "wa";
    EXPECT_EQ(n.publish(row), 3u);
    EXPECT_EQ(a.load(), 1);
    EXPECT_EQ(b.load(), 1);
    EXPECT_EQ(c.load(), 1);
}

TEST(JudgeNotifierUnit, CallbackExceptionSwallowed) {
    JudgeNotifier n;
    std::atomic<int> after{0};
    (void)n.subscribe(1, [](const litecode::SubmissionRow&) {
        throw std::runtime_error("boom");
    });
    (void)n.subscribe(1, [&](const litecode::SubmissionRow&){ ++after; });
    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "ac";
    EXPECT_EQ(n.publish(row), 2u);
    EXPECT_EQ(after.load(), 1);
}

TEST(JudgeNotifierUnit, UnsubscribeStopsDelivery) {
    JudgeNotifier n;
    std::atomic<int> fires{0};
    const std::size_t h = n.subscribe(1, [&](const litecode::SubmissionRow&){
        ++fires;
    });
    n.unsubscribe(1, h);
    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "ac";
    n.publish(row);
    EXPECT_EQ(fires.load(), 0);
}

TEST(JudgeNotifierUnit, WaitForFastPathTerminalRow) {
    JudgeNotifier n;
    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "ac";
    const auto got = n.wait_for(1, row, std::chrono::milliseconds(200));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, 1);
    EXPECT_EQ(got->status, "ac");
}

TEST(JudgeNotifierUnit, WaitForTimeoutReturnsNullopt) {
    JudgeNotifier n;
    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "pending";   // not terminal → wait_for has to block
    const auto got = n.wait_for(1, row, std::chrono::milliseconds(50));
    EXPECT_FALSE(got.has_value());
}

TEST(JudgeNotifierUnit, WaitForWakesOnPublish) {
    JudgeNotifier n;
    std::thread pub;
    litecode::SubmissionRow row;
    row.id     = 1;
    row.status = "pending";
    std::optional<litecode::SubmissionRow> got;
    std::thread waiter([&]{
        got = n.wait_for(1, row, std::chrono::milliseconds(2000));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    litecode::SubmissionRow r2 = row;
    r2.status = "ac";
    r2.time_used = 5;
    n.publish(r2);
    waiter.join();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, "ac");
    EXPECT_EQ(got->time_used.value_or(0), 5);
}

TEST(JudgeNotifierUnit, SubscriberCountTracksLifecycle) {
    JudgeNotifier n;
    EXPECT_EQ(n.subscriber_count_for(1), 0u);
    const std::size_t h1 = n.subscribe(1, [](const litecode::SubmissionRow&){});
    const std::size_t h2 = n.subscribe(1, [](const litecode::SubmissionRow&){});
    EXPECT_EQ(n.subscriber_count_for(1), 2u);
    n.unsubscribe(1, h1);
    // unsubscribe() drops the most-recently-added callback for the
    // submission. Today's per-handle granularity is "LIFO" — a
    // future commit can promote the handle to a stable per-callback
    // id; today's N is almost always 1 (one SSE handler per
    // connection).
    EXPECT_EQ(n.subscriber_count_for(1), 1u);
    litecode::SubmissionRow row;
    row.id = 1;
    row.status = "ac";
    n.publish(row);
    EXPECT_EQ(n.subscriber_count_for(1), 0u);
    (void)h2;
}

TEST(SubscriberScope, UnsubscribesOnDestruction) {
    JudgeNotifier n;
    {
        SubscriberScope s(&n, 1, [](const litecode::SubmissionRow&){});
        EXPECT_EQ(n.subscriber_count_for(1), 1u);
    }
    // Scope destructor calls unsubscribe() which drops the
    // most-recently-added callback.
    EXPECT_EQ(n.subscriber_count_for(1), 0u);
}

TEST(SubscriberScope, ReleaseDetaches) {
    JudgeNotifier n;
    SubscriberScope s(&n, 1, [](const litecode::SubmissionRow&){});
    EXPECT_EQ(n.subscriber_count_for(1), 1u);
    s.release();
    // release() detaches the scope from the notifier without
    // calling unsubscribe — the subscription stays live until
    // either publish() fires or a future scope on the same
    // submission unsubscribes it.
    EXPECT_EQ(n.subscriber_count_for(1), 1u);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — HTTP-level
// ────────────────────────────────────────────────────────────────────────────

class SseLiveFixture : public SseFixture {
protected:
    std::unique_ptr<litecode::HttpServer>      server;
    std::unique_ptr<litecode::RateLimiter>     limiter;
    std::unique_ptr<JudgeScheduler>            scheduler;
    std::unique_ptr<JudgeNotifier>             notifier;
    std::unique_ptr<MockDockerProxy>           proxy;
    std::unique_ptr<litecode::docker::Client>  docker_client;
    ServerHandle                               handle;
    litecode::RateLimitConfig                  rate_cfg;
    litecode::JwtConfig                        jwt_cfg;
    int                                        user_id   = 0;
    int                                        admin_id  = 0;
    std::string                                user_username;
    std::string                                admin_username;
    std::string                                user_token;
    std::string                                admin_token;

    void SetUp() override {
        SseFixture::SetUp();
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        notifier = std::make_unique<JudgeNotifier>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        JudgeSchedulerConfig sc =
            litecode::judge::make_default_scheduler_config(minimal_judge_cfg());
        sc.task_dir_parent = task_dir;
        sc.max_concurrent = 1;
        sc.max_queue_size = 50;
        sc.judge_hard_timeout_seconds = 5;
        scheduler = std::make_unique<JudgeScheduler>(
            static_cast<litecode::docker::Client*>(nullptr),
            static_cast<litecode::judge::WarmPool*>(nullptr),
            pool.get(), sc);
        scheduler->set_notifier(notifier.get());
        ASSERT_TRUE(scheduler->start());

        litecode::register_submission_routes(
            *server, *pool, *limiter, rate_cfg, jwt_cfg,
            scheduler.get(), notifier.get());
        handle = start_server(server.get());

        user_id   = make_user("user");
        ASSERT_GT(user_id, 0);
        user_username = "sse-user-" + std::to_string(user_id);
        user_token = issue_token(jwt_cfg, std::to_string(user_id),
                                 user_username, "user");

        admin_id   = make_user("admin");
        ASSERT_GT(admin_id, 0);
        admin_username = "sse-admin-" + std::to_string(admin_id);
        admin_token = issue_token(jwt_cfg, std::to_string(admin_id),
                                  admin_username, "admin");
    }

    void TearDown() override {
        if (scheduler && scheduler->running()) scheduler->shutdown();
        handle = ServerHandle();
        scheduler.reset();
        notifier.reset();
        docker_client.reset();
        proxy.reset();
        server.reset();
        limiter.reset();
        SseFixture::TearDown();
    }
};

TEST_F(SseLiveFixture, Get401NoAuth) {
    StdoutSilencer silencer;
    httplib::Result r = handle.client->Get("/api/v1/submissions/sse/1");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
    // The 401 response is NOT text/event-stream — it uses the standard
    // error envelope (auth failures happen before the SSE handler runs,
    // so we never get to set the SSE content type).
    EXPECT_NE(r->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto env = json::parse(r->body);
    EXPECT_EQ(env["code"], "UNAUTHORIZED");
}

TEST_F(SseLiveFixture, Get401BadToken) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"Authorization", "Bearer garbage.token"}};
    httplib::Result r = handle.client->Get("/api/v1/submissions/sse/1", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
}

TEST_F(SseLiveFixture, Get400BadIdShape) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/abc", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    // Note: server.h's set_error_handler overwrites the body when
    // the Content-Type is not application/json. The SSE error path's
    // body is replaced with the standard error envelope. The
    // client should fall back to GET /:id on 4xx.
    EXPECT_NE(r->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto env = json::parse(r->body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
}

TEST_F(SseLiveFixture, Get400EmptyId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/", hdrs);
    ASSERT_TRUE(r);
    // cpp-httplib returns 404 for an unmatched regex; the SSE
    // route's regex `/api/v1/submissions/sse/([^/]+)` rejects
    // empty tail. This goes to the framework's 404 catch-all
    // (which renders the standard error envelope), not the SSE
    // error path. Pin the current behavior.
    EXPECT_TRUE(r->status == 400 || r->status == 404);
}

TEST_F(SseLiveFixture, Get404UnknownId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    // Use a huge id that's almost certainly unknown.
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/999999999", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
    EXPECT_NE(r->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto env = json::parse(r->body);
    EXPECT_EQ(env["code"], "NOT_FOUND");
}

TEST_F(SseLiveFixture, Get403NonAdminViewsOthers) {
    StdoutSilencer silencer;
    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int admin_sid = make_submission(admin_id, pid);
    ASSERT_GT(admin_sid, 0);

    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(admin_sid), hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    EXPECT_NE(r->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto env = json::parse(r->body);
    EXPECT_EQ(env["code"], "FORBIDDEN");
}

TEST_F(SseLiveFixture, Get200AdminViewsOthers) {
    StdoutSilencer silencer;
    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int user_sid = make_submission(user_id, pid);
    ASSERT_GT(user_sid, 0);

    // Flip the row to a terminal status so the SSE handler takes
    // the fast path (no wait_for) — the test then just verifies
    // admin can read it.
    auto conn = pool->acquire();
    conn.execute(
        "UPDATE submissions SET status = 'ac', time_used = 1, "
        "memory_used = 1, finished_at = NOW() "
        "WHERE id = ?", user_sid);

    httplib::Headers hdrs = {{"Authorization", "Bearer " + admin_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(user_sid), hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_NE(r->get_header_value("Content-Type").find("text/event-stream"),
              std::string::npos);
    // The body must contain a `result` event with the row.
    EXPECT_NE(r->body.find("event: result"), std::string::npos);
    EXPECT_NE(r->body.find("\"status\":\"ac\""), std::string::npos);
    EXPECT_NE(r->body.find("retry: 3000"), std::string::npos);
}

TEST_F(SseLiveFixture, GetFastPathAlreadyTerminal) {
    StdoutSilencer silencer;
    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int sid = make_submission(user_id, pid);
    ASSERT_GT(sid, 0);

    // Pre-flip to a terminal status. The handler should NOT call
    // wait_for (subscriber_count_for stays 0 the whole time).
    auto conn = pool->acquire();
    conn.execute(
        "UPDATE submissions SET status = 'ac', time_used = 12, "
        "memory_used = 2048, finished_at = NOW() "
        "WHERE id = ?", sid);

    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(sid), hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(notifier->subscriber_count_for(sid), 0u);
    EXPECT_NE(r->body.find("event: result"), std::string::npos);
    EXPECT_NE(r->body.find("\"time_used\":12"), std::string::npos);
    EXPECT_NE(r->body.find("\"memory_used\":2048"), std::string::npos);
}

TEST_F(SseLiveFixture, GetEventStreamShapeIncludesRetryAndResult) {
    StdoutSilencer silencer;
    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int sid = make_submission(user_id, pid);
    ASSERT_GT(sid, 0);

    auto conn = pool->acquire();
    conn.execute(
        "UPDATE submissions SET status = 'wa', error_message = 'wrong answer', "
        "finished_at = NOW() WHERE id = ?", sid);

    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(sid), hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    // Verify the exact frame structure:
    //   1) "retry: 3000\n\n" preamble
    //   2) "event: result\n"
    //   3) "data: { ... }\n\n"
    EXPECT_NE(r->body.find("retry: 3000\n\n"), std::string::npos);
    EXPECT_NE(r->body.find("event: result\n"), std::string::npos);
    EXPECT_NE(r->body.find("data: "), std::string::npos);
    EXPECT_NE(r->body.find("\"status\":\"wa\""), std::string::npos);
    EXPECT_NE(r->body.find("\"error_message\":\"wrong answer\""),
              std::string::npos);
    // The body must NOT include `code` field (SSE event omits
    // source code, same as the list endpoint).
    EXPECT_EQ(r->body.find("\"code\":"), std::string::npos);
}

TEST_F(SseLiveFixture, GetAsyncRoundTripWithNotifiedWorker) {
    StdoutSilencer silencer;
    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int sid = make_submission(user_id, pid);
    ASSERT_GT(sid, 0);

    // Simulate the JudgeScheduler worker: the row is currently
    // pending, the worker would call notifier->publish() after
    // mark_finished. We do it from a side thread to verify the
    // SSE handler unblocks via the notifier rather than via the
    // "row was already terminal" fast path.
    std::thread pub([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        litecode::SubmissionRow r;
        r.id          = sid;
        r.user_id     = user_id;
        r.problem_id  = pid;
        r.language    = "cpp";
        r.code        = "int main(){return 0;}";
        r.status      = "ac";
        r.time_used   = 7;
        r.memory_used = 512;
        notifier->publish(r);
    });

    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(sid), hdrs);
    pub.join();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_NE(r->body.find("event: result"), std::string::npos);
    EXPECT_NE(r->body.find("\"status\":\"ac\""), std::string::npos);
    EXPECT_NE(r->body.find("\"time_used\":7"), std::string::npos);
    EXPECT_NE(r->body.find("\"memory_used\":512"), std::string::npos);
}

TEST_F(SseLiveFixture, GetAsyncSubscriberCountPeaksDuringWait) {
    StdoutSilencer silencer;
    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int sid = make_submission(user_id, pid);
    ASSERT_GT(sid, 0);

    std::atomic<int> peak{0};
    std::thread watcher([&]{
        // Poll the subscriber count; the SSE handler subscribes
        // before its wait, and the row stays pending until
        // someone publishes. We sample for up to 1 second and
        // record the maximum.
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(1000);
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = static_cast<int>(
                notifier->subscriber_count_for(sid));
            if (n > peak.load()) peak.store(n);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread pub([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        litecode::SubmissionRow r;
        r.id     = sid;
        r.status = "ac";
        notifier->publish(r);
    });

    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(sid), hdrs);
    pub.join();
    watcher.join();
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_GE(peak.load(), 1) << "SSE handler never subscribed";
}

TEST_F(SseLiveFixture, GetWithoutNotifierDegradesToSnapshot) {
    StdoutSilencer silencer;
    // Spin up a second server that registers routes with a
    // nullptr notifier (the "notifier not configured" path).
    auto server2   = std::make_unique<litecode::HttpServer>(
                         dev_server(), dev_cors());
    auto limiter2  = std::make_unique<litecode::RateLimiter>();
    litecode::register_submission_routes(
        *server2, *pool, *limiter2, rate_cfg, jwt_cfg,
        scheduler.get(), /*notifier=*/nullptr);
    auto handle2 = start_server(server2.get());

    const int pid = make_problem();
    ASSERT_GT(pid, 0);
    const int sid = make_submission(user_id, pid);
    ASSERT_GT(sid, 0);
    // The row is "pending" — without a notifier, the handler
    // should emit a "result" event with the pending snapshot
    // (graceful degradation: snapshot, not error).
    httplib::Headers hdrs = {{"Authorization", "Bearer " + user_token}};
    httplib::Result r = handle2.client->Get(
        "/api/v1/submissions/sse/" + std::to_string(sid), hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_NE(r->body.find("event: result"), std::string::npos);
    EXPECT_NE(r->body.find("\"status\":\"pending\""), std::string::npos);

    handle2 = ServerHandle();
    limiter2.reset();
    server2.reset();
}

}  // namespace
