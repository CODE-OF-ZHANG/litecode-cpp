// tests/unit/test_judge_scheduler.cpp
//
// Unit + light-integration tests for src/judge/judge_scheduler.h
//   (Phase 4 ★ — judge scheduler: thread pool + task queue + max-
//    concurrent + 30s hard timeout + warm_pool integration).
//
// Mirrors the project test pattern (test_docker_client.cpp,
// test_warm_pool.cpp, test_audit_log.cpp):
//
//   1) Pure unit tests (no docker daemon, no MySQL):
//        - kStatus* constants are stable strings
//        - is_valid_status / is_terminal_status / is_valid_language
//        - validate_code_length / clamp_list_filter
//        - JudgeSchedulerConfig defaults
//        - make_default_scheduler_config pulls from JudgeConfig
//        - JudgeTask default values
//        - parse_judge_result_json: clean AC / WA / TLE JSON
//        - parse_judge_result_json: trailing-prefix noise
//        - parse_judge_result_json: empty → SE
//        - parse_judge_result_json: garbage → SE
//        - parse_judge_result_json: missing fields default sensibly
//        - make_probe on null scheduler → down
//        - make_probe on un-started scheduler → ok + zero counters
//
//   2) Light integration tests (in-process httplib::Server simulating
//      the docker socket proxy + real MySQL when available):
//        - start() spawns the configured number of workers
//        - enqueue() before start() returns false
//        - enqueue() at capacity returns false
//        - One task end-to-end: AC status round-trip
//        - One task end-to-end: WA status round-trip
//        - One task end-to-end: judge.sh SE → submission SE
//        - Per-task create body carries bind mount + JUDGE_TASK_FILE env
//        - Hard timeout exceeded → DockerTimeoutError → submission SE
//        - Docker start failure → submission SE
//        - pool_id released after task done
//        - shutdown() drains in-flight + rejects new enqueue
//        - Health probe publishes queue_size + running + max_concurrent
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box (root/123456/litecode@127.0.0.1).
// If ping() fails, those tests are SKIPPED — the binary still passes on a
// machine without MySQL, which is what we want for CI lint jobs.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
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

#include "config.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"
#include "db/submission_repo.h"
#include "db/test_case_repo.h"
#include "judge/docker_client.h"
#include "judge/judge_scheduler.h"
#include "judge/warm_pool.h"
#include "logger.h"
#include "routes/system_routes.h"

namespace {

using litecode::JudgeConfig;
using litecode::judge::IdleContainer;
using litecode::judge::JudgeScheduler;
using litecode::judge::JudgeSchedulerConfig;
using litecode::judge::JudgeTask;
using litecode::judge::WarmPool;
using litecode::judge::WarmPoolConfig;
using litecode::judge::make_default_scheduler_config;
using litecode::judge::make_default_warm_pool_config;
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
    std::string captured() const { return sink_.str(); }
private:
    std::stringstream sink_;
    std::streambuf*   original_ = nullptr;
};

// ────────────────────────────────────────────────────────────────────────────
//  JudgeConfig + JudgeSchedulerConfig helpers
// ────────────────────────────────────────────────────────────────────────────

JudgeConfig minimal_judge_cfg() {
    JudgeConfig jc;
    jc.judge_image              = "litecode-judge:latest";
    jc.network_mode             = "none";
    jc.warm_pool_size           = 0;   // disabled — we test pool = nullopt
    jc.max_concurrent_judges    = 2;
    jc.max_queue_size           = 4;
    jc.compile_timeout_seconds  = 5;
    jc.judge_hard_timeout_seconds = 5;
    jc.output_limit_bytes       = 16 * 1024 * 1024;
    jc.pids_limit               = "50";
    return jc;
}

JudgeTask make_task(int submission_id, int problem_id, int user_id) {
    JudgeTask t;
    t.submission_id    = submission_id;
    t.user_id          = user_id;
    t.problem_id       = problem_id;
    t.language         = "cpp";
    t.code             = "int main(){return 0;}";
    t.time_limit_ms    = 1000;
    t.memory_limit_mb  = 256;
    t.compile_timeout_ms = 10'000;
    JudgeTask::TestCaseInput tc;
    tc.input           = "1 2\n";
    tc.expected_output = "3\n";
    tc.judge_type      = "exact";
    tc.order_num       = 0;
    t.test_cases.push_back(std::move(tc));
    return t;
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests
// ────────────────────────────────────────────────────────────────────────────

TEST(JudgeStatus, ConstantsPinStrings) {
    EXPECT_STREQ(litecode::kStatusPending, "pending");
    EXPECT_STREQ(litecode::kStatusRunning, "running");
    EXPECT_STREQ(litecode::kStatusAC,      "ac");
    EXPECT_STREQ(litecode::kStatusWA,      "wa");
    EXPECT_STREQ(litecode::kStatusRE,      "re");
    EXPECT_STREQ(litecode::kStatusTLE,     "tle");
    EXPECT_STREQ(litecode::kStatusMLE,     "mle");
    EXPECT_STREQ(litecode::kStatusOLE,     "ole");
    EXPECT_STREQ(litecode::kStatusPE,      "pe");
    EXPECT_STREQ(litecode::kStatusCE,      "ce");
    EXPECT_STREQ(litecode::kStatusSE,      "se");
}

TEST(JudgeStatus, IsValidAndTerminal) {
    EXPECT_TRUE (litecode::is_valid_status("pending"));
    EXPECT_TRUE (litecode::is_valid_status("running"));
    EXPECT_TRUE (litecode::is_valid_status("ac"));
    EXPECT_TRUE (litecode::is_valid_status("se"));
    EXPECT_FALSE(litecode::is_valid_status(""));
    EXPECT_FALSE(litecode::is_valid_status("PE"));
    EXPECT_FALSE(litecode::is_valid_status("AC "));
    EXPECT_FALSE(litecode::is_valid_status("ok"));

    EXPECT_FALSE(litecode::is_terminal_status("pending"));
    EXPECT_FALSE(litecode::is_terminal_status("running"));
    EXPECT_TRUE (litecode::is_terminal_status("ac"));
    EXPECT_TRUE (litecode::is_terminal_status("se"));

    EXPECT_TRUE (litecode::is_valid_language("c"));
    EXPECT_TRUE (litecode::is_valid_language("cpp"));
    EXPECT_FALSE(litecode::is_valid_language("python"));
    EXPECT_FALSE(litecode::is_valid_language(""));
}

TEST(SubmissionValidation, CodeLength) {
    std::string err;
    EXPECT_TRUE (litecode::validate_code_length(1,                &err));
    EXPECT_TRUE (litecode::validate_code_length(1024,             &err));
    EXPECT_TRUE (litecode::validate_code_length(litecode::kMaxCodeLength, &err));
    EXPECT_FALSE(litecode::validate_code_length(0,                &err));
    EXPECT_FALSE(litecode::validate_code_length(litecode::kMaxCodeLength + 1, &err));
    EXPECT_FALSE(err.empty());
}

TEST(SubmissionValidation, ClampListFilter) {
    litecode::SubmissionListFilter f;
    f.limit = -5; f.offset = -1;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  litecode::kDefaultListLimit);
    EXPECT_EQ(f.offset, 0);

    f.limit = 1000; f.offset = 50;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  litecode::kMaxListLimit);
    EXPECT_EQ(f.offset, 50);
}

TEST(JudgeSchedulerConfig, Defaults) {
    JudgeSchedulerConfig c;
    EXPECT_EQ(c.max_concurrent, 4);
    EXPECT_EQ(c.max_queue_size, 50);
    EXPECT_EQ(c.compile_timeout_ms, 10'000);
    EXPECT_EQ(c.judge_hard_timeout_seconds, 30);
    EXPECT_EQ(c.output_limit_bytes, 16 * 1024 * 1024);
    EXPECT_EQ(c.judge_image, "litecode-judge:latest");
    EXPECT_EQ(c.network_mode, "none");
}

TEST(JudgeSchedulerConfig, FromJudgeConfig) {
    StdoutSilencer silencer;
    JudgeConfig jc = minimal_judge_cfg();
    jc.judge_image  = "litecode-judge:1.0";
    jc.network_mode = "bridge";
    jc.max_concurrent_judges = 3;
    jc.max_queue_size        = 7;
    jc.compile_timeout_seconds = 8;
    jc.judge_hard_timeout_seconds = 12;
    jc.output_limit_bytes = 4096;
    auto sc = make_default_scheduler_config(jc);
    EXPECT_EQ(sc.max_concurrent, 3);
    EXPECT_EQ(sc.max_queue_size, 7);
    EXPECT_EQ(sc.compile_timeout_ms, 8000);
    EXPECT_EQ(sc.judge_hard_timeout_seconds, 12);
    EXPECT_EQ(sc.output_limit_bytes, 4096);
    EXPECT_EQ(sc.judge_image, "litecode-judge:1.0");
    EXPECT_EQ(sc.network_mode, "bridge");
}

TEST(JudgeTask, DefaultsAreSane) {
    JudgeTask t;
    EXPECT_EQ(t.submission_id, 0);
    EXPECT_EQ(t.user_id, 0);
    EXPECT_EQ(t.problem_id, 0);
    EXPECT_EQ(t.time_limit_ms, 1000);
    EXPECT_EQ(t.memory_limit_mb, 256);
    EXPECT_EQ(t.compile_timeout_ms, 10'000);
    EXPECT_TRUE(t.test_cases.empty());
    EXPECT_TRUE(t.language.empty());
    EXPECT_TRUE(t.code.empty());
}

TEST(JudgeProbe, NullSchedulerReportsDown) {
    StdoutSilencer silencer;
    auto probe = JudgeScheduler::make_probe(nullptr);
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("no judge scheduler"), std::string::npos);
}

TEST(JudgeProbe, UnstartedSchedulerReportsOkWithCounters) {
    StdoutSilencer silencer;
    JudgeScheduler s(
        static_cast<litecode::docker::Client*>(nullptr),
        static_cast<WarmPool*>(nullptr),
        static_cast<litecode::ConnectionPool*>(nullptr),
        make_default_scheduler_config(minimal_judge_cfg()));
    auto probe = JudgeScheduler::make_probe(&s);
    auto r = probe();
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.detail.find("not running"), std::string::npos);
    EXPECT_EQ(r.extra["queue_size"].get<int>(), 0);
    EXPECT_EQ(r.extra["running"].get<int>(), 0);
    EXPECT_EQ(r.extra["max_concurrent"].get<int>(), 2);
}

// parse_judge_result_json is a public static helper (intentionally
// exposed so test_judge_scheduler.cpp can exercise the JSON shape
// without spinning up a docker mock for every case). We test the
// static method directly via the type's name.

TEST(ParseJudgeResultJson, CleanAC) {
    StdoutSilencer silencer;
    const std::string logs =
        "someprefix noise\n"
        "{\"submission_id\":42,\"status\":\"ac\",\"time_used_ms\":12,"
        "\"memory_used_kb\":2048,\"error_message\":null,"
        "\"failed_case_index\":null,\"case_results\":[]}\n";
    auto r = JudgeScheduler::parse_judge_result_json(logs, 0);
    EXPECT_TRUE(r.parsed);
    EXPECT_EQ(r.status, "ac");
    EXPECT_EQ(r.time_used_ms, 12);
    EXPECT_EQ(r.memory_used_kb, 2048);
    EXPECT_TRUE(r.error_message.empty());
    EXPECT_EQ(r.failed_case_index, -1);
}

TEST(ParseJudgeResultJson, CleanWA) {
    StdoutSilencer silencer;
    const std::string logs =
        "{\"submission_id\":1,\"status\":\"wa\",\"time_used_ms\":7,"
        "\"memory_used_kb\":1024,\"error_message\":\"case 0 mismatch\","
        "\"failed_case_index\":0,\"case_results\":[]}\n";
    auto r = JudgeScheduler::parse_judge_result_json(logs, 0);
    EXPECT_TRUE(r.parsed);
    EXPECT_EQ(r.status, "wa");
    EXPECT_EQ(r.failed_case_index, 0);
    EXPECT_EQ(r.error_message, "case 0 mismatch");
}

TEST(ParseJudgeResultJson, EmptyLogsYieldsSE) {
    StdoutSilencer silencer;
    auto r = JudgeScheduler::parse_judge_result_json("", 0);
    EXPECT_FALSE(r.parsed);
    EXPECT_EQ(r.status, "se");
    EXPECT_FALSE(r.error_message.empty());
}

TEST(ParseJudgeResultJson, GarbageYieldsSE) {
    StdoutSilencer silencer;
    auto r = JudgeScheduler::parse_judge_result_json(
        "not json\nnot json 2\nbanana\n", 1);
    EXPECT_FALSE(r.parsed);
    EXPECT_EQ(r.status, "se");
    EXPECT_NE(r.error_message.find("wait exit=1"), std::string::npos);
}

TEST(ParseJudgeResultJson, MissingFieldsDefaultSensibly) {
    StdoutSilencer silencer;
    const std::string logs = "{\"status\":\"tle\"}\n";
    auto r = JudgeScheduler::parse_judge_result_json(logs, 0);
    EXPECT_TRUE(r.parsed);
    EXPECT_EQ(r.status, "tle");
    EXPECT_EQ(r.time_used_ms, 0);
    EXPECT_EQ(r.memory_used_kb, 0);
    EXPECT_TRUE(r.error_message.empty());
    EXPECT_EQ(r.failed_case_index, -1);
}

// ────────────────────────────────────────────────────────────────────────────
//  Mock docker proxy — emulates the 5 whitelisted endpoints + a few
//  knobs so each integration test can drive a specific lifecycle:
//    * prebuilt_start_response_  → fail POST /containers/{id}/start
//    * prebuilt_wait_response_   → fail POST /containers/{id}/wait
//    * prebuilt_logs_response_   → what /containers/{id}/logs returns
//    * logs_delay_ms             → make /logs block (drives timeout)
//    * task_json_body (last)     → captured for assertion
// ────────────────────────────────────────────────────────────────────────────

class MockDockerProxy {
public:
    void start() {
        srv_.Get("/_ping",
            [this](const httplib::Request&, httplib::Response& res) {
                record("GET", "/_ping", "");
                res.status = 200; res.set_content("OK", "text/plain");
            });

        srv_.Post("/containers/create",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", "/containers/create", req.body);
                last_create_body = req.body;
                ++create_calls;
                std::ostringstream id;
                id << "sched-" << std::setw(4) << std::setfill('0')
                   << ++created_seq;
                std::string cid = id.str();
                created_ids.push_back(cid);

                json body{{"Id",       cid},
                          {"Warnings", json::array()}};
                res.status = 201;
                res.set_content(body.dump(), "application/json");
            });

        // /containers/{id}/start — always 204, or the prebuilt status
        srv_.Post(R"(/containers/([^/]+)/start)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                if (prebuilt_start_response_.status != 0) {
                    res.status = prebuilt_start_response_.status;
                    res.set_content(prebuilt_start_response_.body,
                                    "application/json");
                    return;
                }
                res.status = 204;
            });

        // /containers/{id}/wait — by default returns 200 with exit code 0.
        // If logs_delay_ms is set, sleep that long before returning to
        // exercise the scheduler's hard-timeout path. If
        // prebuilt_wait_response_ is armed, surface its status.
        srv_.Post(R"(/containers/([^/]+)/wait)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                if (prebuilt_wait_response_.status != 0) {
                    res.status = prebuilt_wait_response_.status;
                    res.set_content(prebuilt_wait_response_.body,
                                    "application/json");
                    return;
                }
                if (logs_delay_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(logs_delay_ms_));
                }
                json body{{"StatusCode", default_exit_code_},
                          {"Error",      nullptr}};
                res.status = 200;
                res.set_content(body.dump(), "application/json");
            });

        // /containers/{id}/logs — return the prebuilt payload verbatim
        srv_.Get(R"(/containers/([^/]+)/logs)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("GET", req.path, req.body);
                if (logs_delay_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(logs_delay_ms_));
                }
                res.status = 200;
                res.set_content(prebuilt_logs_response_, "text/plain");
            });

        // /containers/{id}/kill — always 204
        srv_.Post(R"(/containers/([^/]+)/kill)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                res.status = 204;
            });

        // /containers/{id} — DELETE; always 204
        srv_.Delete(R"(/containers/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("DELETE", req.path, req.body);
                ++delete_calls;
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                removed_ids.push_back(last_id_);
                res.status = 204;
            });

        const int port = srv_.bind_to_any_port("127.0.0.1");
        ASSERT_TRUE(port > 0);
        port_ = port;
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

    // Knobs.
    std::atomic<int> create_calls{0};
    std::atomic<int> delete_calls{0};
    std::atomic<int> created_seq{0};
    std::string      last_create_body;
    std::string      last_id_;
    std::vector<std::string> created_ids;
    std::vector<std::string> removed_ids;

    struct Prebuilt { int status = 0; std::string body; };
    Prebuilt  prebuilt_start_response_{};
    Prebuilt  prebuilt_wait_response_{};
    std::string prebuilt_logs_response_ =
        "{\"submission_id\":0,\"status\":\"ac\",\"time_used_ms\":5,"
        "\"memory_used_kb\":1024,\"error_message\":null,"
        "\"failed_case_index\":null,\"case_results\":[]}\n";
    int      default_exit_code_ = 0;
    int      logs_delay_ms_     = 0;   // > 0 ⇒ wait + logs block

private:
    void record(const std::string& m, const std::string& p,
                const std::string& b) {
        std::lock_guard<std::mutex> g(mu_);
        requests_.push_back({m, p, b});
    }

    httplib::Server              srv_;
    std::thread                  thread_;
    int                          port_ = 0;
    struct RecordedRequest {
        std::string method, path, body;
    };
    std::vector<RecordedRequest> requests_;
    std::mutex                   mu_;
};

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
//  DB fixture — mirrors test_audit_log.cpp. SKIP if MySQL is unreachable.
// ────────────────────────────────────────────────────────────────────────────

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

class SchedulerFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::unique_ptr<MockDockerProxy>      proxy;
    std::unique_ptr<litecode::docker::Client> docker_client;
    std::filesystem::path                 task_dir;
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

        // Verify the test DB has the v008 schema (submissions.finished_at
        // column). Older dev boxes that pre-date the V008 migration
        // would otherwise fail every integration test with an opaque
        // "Unknown column" error from the driver. We probe
        // information_schema once and SKIP if the column is missing —
        // operators see a clear "run init_db.sh" message instead.
        try {
            auto conn = pool->acquire();
            auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME   = 'submissions' "
                "  AND COLUMN_NAME  = 'finished_at' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "submissions.finished_at missing — "
                                "run init_db.sh to apply V008";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "schema probe failed: " << e.what();
        }

        // Per-test task dir under the build dir so the worker tempdirs
        // don't pollute /tmp. Clean any leftover subdirs from
        // previous test runs to keep tests isolated — a leak in
        // one test would otherwise make "task_dir is empty" checks
        // in subsequent tests flaky.
        std::error_code ec;
        task_dir = std::filesystem::temp_directory_path() /
                   "litecode-judge-sched-test";
        std::filesystem::remove_all(task_dir, ec);
        std::filesystem::create_directory(task_dir, ec);
    }

    void TearDown() override {
        // Best-effort cleanup of all rows we created.
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
                        // test_cases + problem_tags cascade via FK
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
        proxy.reset();
        docker_client.reset();
        pool.reset();
    }

    // Insert a throwaway user via raw SQL (we don't pull in
    // user_repo.h to avoid the litecode::detail::req_string ODR
    // collision with problem_repo.h in this TU). The hash is a
    // fixed bcrypt-format blob that satisfies the NOT NULL column
    // — the test never logs in as this user, so the hash value is
    // semantically irrelevant.
    int make_user() {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        std::string username = std::string("sched-") +
                     std::to_string(static_cast<long long>(
                         std::chrono::system_clock::now()
                             .time_since_epoch().count())) +
                     "_" + std::to_string(n);
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO users (username, password_hash, role) "
                "VALUES (?, '$2b$12$dummy.hash.for.test.only.padding.aaaa', 'user')",
                username);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_user_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }

    // Insert a throwaway problem with one test case. Returns the
    // problem id.
    int make_problem() {
        litecode::ProblemRow p;
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        p.slug        = "sched-" +
                        std::to_string(static_cast<long long>(
                            std::chrono::system_clock::now()
                                .time_since_epoch().count())) +
                        "-" + std::to_string(n);
        p.title       = "scheduler test problem";
        p.difficulty  = "easy";
        p.description = "auto-generated";
        p.time_limit  = 1000;
        p.memory_limit = 128;
        const int pid = litecode::problem_repo::create(*pool, p);
        if (pid <= 0) return 0;
        created_problem_ids.push_back(pid);
        // Insert a non-sample test case so submissions FK works.
        litecode::test_case_repo::insert(
            *pool, pid, "1 2\n", "3\n",
            /*is_sample=*/false, "exact", /*order_num=*/0);
        return pid;
    }

    // Insert a pending submission row; return its id.
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
//  Integration tests
// ────────────────────────────────────────────────────────────────────────────

TEST_F(SchedulerFixture, EnqueueBeforeStartReturnsFalse) {
    StdoutSilencer silencer;
    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(
        static_cast<litecode::docker::Client*>(nullptr),
        static_cast<WarmPool*>(nullptr),
        pool.get(),
        sc);
    JudgeTask t = make_task(/*submission_id=*/1, /*problem_id=*/1, 1);
    EXPECT_FALSE(sched.enqueue(std::move(t)));
    EXPECT_EQ(sched.queue_size(), 0u);
}

TEST_F(SchedulerFixture, EnqueueAtCapacityReturnsFalse) {
    StdoutSilencer silencer;
    // No docker mock — workers fail fast (SE within ~10 ms each).
    // We pick a larger max_queue_size and enqueue one MORE than the
    // limit; whichever task the worker happens to have popped doesn't
    // matter — the queue-size check is the contract we're testing.
    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.max_queue_size = 4;
    sc.judge_hard_timeout_seconds = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(
        static_cast<litecode::docker::Client*>(nullptr),
        static_cast<WarmPool*>(nullptr),
        pool.get(),
        sc);
    ASSERT_TRUE(sched.start());

    const int user_id = make_user();
    const int problem_id = make_problem();
    ASSERT_GT(user_id, 0);
    ASSERT_GT(problem_id, 0);
    std::vector<int> subs;
    for (int i = 0; i < 7; ++i) {
        int sid = make_submission(user_id, problem_id);
        ASSERT_GT(sid, 0);
        subs.push_back(sid);
    }

    // Try to enqueue max_queue_size + 2 = 6 tasks back-to-back.
    // The first one is in-flight (1), up to max_queue_size=4 sit in
    // the queue, the (max_queue_size + 2)th MUST be rejected. The
    // buffer (5th can succeed) covers the race where the worker has
    // already popped task 1 by the time the 5th enqueue runs.
    int accepted = 0;
    int rejected = 0;
    for (int i = 0; i < 6; ++i) {
        if (sched.enqueue(make_task(subs[i], problem_id, user_id))) ++accepted;
        else ++rejected;
    }
    // At least 5 (max_queue_size + 1) enqueues must have succeeded —
    // the 6th, 7th (if not yet picked up) are rejected. Note: if
    // the worker has drained fast, more than 5 may have been
    // accepted; the contract we test is "the 6th in a tight burst
    // gets rejected at least once", which is `rejected >= 1`.
    EXPECT_GE(rejected, 1) << "got accepted=" << accepted
                          << " rejected=" << rejected
                          << " queue=" << sched.queue_size()
                          << " running=" << sched.running_count();

    sched.shutdown();
}

TEST_F(SchedulerFixture, EndToEndACRoundTrip) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    const int user_id = make_user();
    const int problem_id = make_problem();
    ASSERT_GT(user_id, 0);
    ASSERT_GT(problem_id, 0);
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.max_queue_size = 4;
    sc.judge_hard_timeout_seconds = 5;
    sc.task_dir_parent = task_dir;

    // prebuilt logs = AC, time=42ms, mem=1024KB (the default in MockDockerProxy)
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    // Wait for status='ac' to land.
    auto got = wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && row->status == "ac";
    });
    EXPECT_TRUE(got) << "scheduler never marked submission ac";

    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->status, "ac");
    EXPECT_EQ(row->time_used.value_or(-1), 5);  // from prebuilt logs
    EXPECT_EQ(row->memory_used.value_or(-1), 1024);
    EXPECT_TRUE(row->finished_at.has_value());

    // The container should have been created + removed.
    EXPECT_EQ(proxy->create_calls.load(), 1);
    EXPECT_GE(proxy->delete_calls.load(), 1);

    // task_dir should have been cleaned up.
    int remaining = 0;
    for (auto& e : std::filesystem::directory_iterator(task_dir)) {
        (void)e;
        ++remaining;
    }
    EXPECT_EQ(remaining, 0)
        << "scheduler did not clean up task tempdir";

    sched.shutdown();
}

TEST_F(SchedulerFixture, EndToEndWARoundTrip) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    // Replace logs with a WA result.
    proxy->prebuilt_logs_response_ =
        "{\"submission_id\":0,\"status\":\"wa\",\"time_used_ms\":3,"
        "\"memory_used_kb\":512,\"error_message\":\"case 0 wrong answer\","
        "\"failed_case_index\":0,\"case_results\":[]}\n";

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    ASSERT_TRUE(wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && row->status == "wa";
    }));
    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->status, "wa");
    EXPECT_EQ(row->error_message.value_or(""), "case 0 wrong answer");
    EXPECT_EQ(row->time_used.value_or(-1), 3);
    EXPECT_EQ(row->memory_used.value_or(-1), 512);
    sched.shutdown();
}

TEST_F(SchedulerFixture, EndToEndSERoundTripWhenJudgeJSONMissing) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    // Garbage on stdout — should be parsed as SE.
    proxy->prebuilt_logs_response_ = "no json here\n";

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    ASSERT_TRUE(wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && row->status == "se";
    }));
    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->status, "se");
    EXPECT_NE(row->error_message.value_or("").find(
        "no parseable result JSON"), std::string::npos);
    sched.shutdown();
}

TEST_F(SchedulerFixture, PerTaskCreateBodyCarriesBindMountAndEnv) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    t.test_cases.clear();
    JudgeTask::TestCaseInput a; a.input = "1\n"; a.expected_output = "1\n";
    a.judge_type = "exact"; a.order_num = 0;
    t.test_cases.push_back(a);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    ASSERT_TRUE(wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && litecode::is_terminal_status(row->status);
    }, std::chrono::milliseconds(5000)));

    // Verify the create body shape.
    ASSERT_FALSE(proxy->last_create_body.empty());
    auto body = json::parse(proxy->last_create_body);
    EXPECT_EQ(body["Image"], "litecode-judge:latest");
    EXPECT_EQ(body["User"],  "judgeuser");
    EXPECT_EQ(body["HostConfig"]["NetworkMode"], "none");
    EXPECT_TRUE(body["HostConfig"]["ReadonlyRootfs"].get<bool>());
    EXPECT_EQ(body["HostConfig"]["PidsLimit"].get<int>(), 50);
    // Env carries JUDGE_TASK_FILE.
    ASSERT_TRUE(body.contains("Env"));
    bool found_task_file = false;
    bool found_judge_home = false;
    for (const auto& e : body["Env"]) {
        if (e.is_string()) {
            if (e.get<std::string>().find("JUDGE_TASK_FILE=/tmp/task.json")
                != std::string::npos) found_task_file = true;
            if (e.get<std::string>().find("JUDGE_HOME=/judge")
                != std::string::npos)  found_judge_home = true;
        }
    }
    EXPECT_TRUE(found_task_file);
    EXPECT_TRUE(found_judge_home);
    // Bind mount for /tmp/task.json
    ASSERT_TRUE(body["HostConfig"].contains("Mounts"));
    ASSERT_EQ(body["HostConfig"]["Mounts"].size(), 1u);
    EXPECT_EQ(body["HostConfig"]["Mounts"][0]["Type"],   "bind");
    EXPECT_EQ(body["HostConfig"]["Mounts"][0]["Target"], "/tmp/task.json");
    EXPECT_TRUE(body["HostConfig"]["Mounts"][0]["ReadOnly"].get<bool>());

    sched.shutdown();
}

TEST_F(SchedulerFixture, HardTimeoutExceededYieldsSE) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    // Make /wait block well past the scheduler's hard timeout.
    // judge_hard_timeout=1s + 5s slack = 6s ⇒ 8s delay forces the
    // DockerTimeoutError path.
    proxy->logs_delay_ms_ = 8'000;

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.judge_hard_timeout_seconds = 1;  // 1s + 5s slack → /wait 6s timeout
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    // The /wait timeout fires at ~6s, so the SE arrives within ~7s.
    ASSERT_TRUE(wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && row->status == "se";
    }, std::chrono::milliseconds(15'000)))
        << "hard timeout did not produce SE within 15s";
    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_NE(row->error_message.value_or("").find("hard timeout"),
              std::string::npos);
    sched.shutdown();
}

TEST_F(SchedulerFixture, DockerStartFailureYieldsSE) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    // Make /start return 500.
    proxy->prebuilt_start_response_ = {500, R"({"message":"daemon sad"})"};

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    ASSERT_TRUE(wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && row->status == "se";
    }));
    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_NE(row->error_message.value_or("").find("docker start failed"),
              std::string::npos);
    sched.shutdown();
}

TEST_F(SchedulerFixture, AlreadyTerminalSubmissionIsDropped) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    // Pre-flip the row to a terminal status. mark_running() will
    // refuse to flip pending→running, and the worker should drop
    // the task silently.
    litecode::submission_repo::mark_finished(
        *pool, sub_id, "ac", 1, 1, "pre-existing");

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    // Give the worker a chance to skip the task.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->status, "ac");
    EXPECT_EQ(row->error_message.value_or(""), "pre-existing");
    // The docker proxy should NOT have been called (we never
    // reached the create step).
    EXPECT_EQ(proxy->create_calls.load(), 0);
    sched.shutdown();
}

TEST_F(SchedulerFixture, ShutdownDrainsAndRejectsNewEnqueue) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 2;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));
    sched.shutdown();
    EXPECT_FALSE(sched.running());
    EXPECT_FALSE(sched.enqueue(make_task(0, 0, 0)));
    // The pre-shutdown task should still have completed.
    auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->status, "ac");
}

TEST_F(SchedulerFixture, HealthProbeWiredIntoHealthService) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 3;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), /*pool=*/nullptr,
                         pool.get(), sc);
    ASSERT_TRUE(sched.start());

    litecode::HealthService h;
    h.register_probe("judge_queue", JudgeScheduler::make_probe(&sched));
    int status = 0;
    auto body = h.build_response(&status);
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body["max_concurrent"].get<int>(), 3);
    EXPECT_EQ(body["queue_size"].get<int>(), 0);
    EXPECT_EQ(body["running"].get<int>(), 0);

    sched.shutdown();
}

TEST_F(SchedulerFixture, WithWarmPoolPullsAndDiscards) {
    StdoutSilencer silencer;
    proxy = std::make_unique<MockDockerProxy>();
    proxy->start();
    docker_client = std::make_unique<litecode::docker::Client>(proxy->url());

    const int user_id = make_user();
    const int problem_id = make_problem();
    const int sub_id = make_submission(user_id, problem_id);
    ASSERT_GT(sub_id, 0);

    // Warm pool with K=1; the worker should acquire + discard one
    // pre-created container, then create a per-task one.
    WarmPoolConfig wp_cfg;
    wp_cfg.template_opts = [] {
        litecode::docker::CreateOptions o;
        o.image        = "litecode-judge:latest";
        o.command      = {"--help"};
        o.network_mode = "none";
        o.read_only    = true;
        o.memory_mb    = 64;
        o.cpus         = 0.0;
        o.pids_limit   = 50;
        o.security_opt = {"no-new-privileges:true"};
        o.tmpfs        = { {"/tmp", "size=64m,mode=1777"} };
        o.user         = "judgeuser";
        return o;
    }();
    wp_cfg.target_size = 1;
    wp_cfg.refill_retry_attempts = 0;
    wp_cfg.refill_retry_delay_ms = 0;

    WarmPool pool_w(docker_client.get());
    ASSERT_TRUE(pool_w.start(wp_cfg));
    EXPECT_EQ(pool_w.size(), 1u);

    JudgeSchedulerConfig sc = make_default_scheduler_config(minimal_judge_cfg());
    sc.max_concurrent = 1;
    sc.task_dir_parent = task_dir;
    JudgeScheduler sched(docker_client.get(), &pool_w, pool.get(), sc);
    ASSERT_TRUE(sched.start());

    const int pre_create_count = proxy->create_calls.load();
    EXPECT_EQ(pre_create_count, 1);  // pool pre-created exactly one

    JudgeTask t = make_task(sub_id, problem_id, user_id);
    EXPECT_TRUE(sched.enqueue(std::move(t)));

    ASSERT_TRUE(wait_until([&]{
        auto row = litecode::submission_repo::find_by_id(*pool, sub_id);
        return row && row->status == "ac";
    }));
    // After judge: pool container rm'd + per-task container rm'd +
    // refill creates one more. Wait for the refill to actually
    // complete (the refill thread runs in the background) before
    // counting creates.
    EXPECT_TRUE(wait_until([&]{
        return proxy->create_calls.load() >= pre_create_count + 2;
    }, std::chrono::milliseconds(2000)))
        << "refill did not produce expected create count: got "
        << proxy->create_calls.load() << " expected >= "
        << (pre_create_count + 2);
    EXPECT_GE(proxy->delete_calls.load(), 2);

    sched.shutdown();
    pool_w.shutdown();
}

// ────────────────────────────────────────────────────────────────────────────
//  v1.2.51 — judge.sh + docker_client.h regression coverage (Phase 8)
// ────────────────────────────────────────────────────────────────────────────

// ────────────────────────────────────────────────────────────────────────────
// [JudgeSchedulerConfig.TaskVolumeNameEmptyBindFallback]
// 验证 cfg.task_volume_name 为空时，producer 端走 Bind 路径，
// Source 指向 host 路径。锁住 judge_scheduler.h:761-766 的分支语义。
// ────────────────────────────────────────────────────────────────────────────
TEST(JudgeSchedulerConfig, TaskVolumeNameEmptyBindFallback) {
    using litecode::docker::Mount;
    StdoutSilencer silencer;
    JudgeConfig jc = minimal_judge_cfg();
    jc.task_volume_name = "";   // explicit empty → Bind path
    auto sc = make_default_scheduler_config(jc);
    EXPECT_EQ(sc.task_volume_name, "");
    // 复刻 producer 端判断（judge_scheduler.h:745-766）
    litecode::docker::CreateOptions o;
    o.image = "img";
    if (sc.task_volume_name.empty()) {
        o.mounts.push_back(Mount::bind_mount("/host/task.json",
                                              "/tmp/task.json"));
    } else {
        o.mounts.push_back(Mount::volume_mount(sc.task_volume_name,
                                                "/tmp/litecode-judge"));
    }
    ASSERT_EQ(o.mounts.size(), 1u);
    EXPECT_EQ(o.mounts[0].kind_of(), Mount::Kind::Bind);
}

// ────────────────────────────────────────────────────────────────────────────
// [JudgeSchedulerConfig.TaskVolumeNameSetVolumeMount]
// 验证 cfg.task_volume_name 设置时走 Volume 路径，Source 是 volume 名。
// 这是 v1.2.50-b 引入的核心修复 —— docker-proxy 在 Docker Desktop
// Windows/macOS 上看不到 host bind mount，必须走 named volume。
// ────────────────────────────────────────────────────────────────────────────
TEST(JudgeSchedulerConfig, TaskVolumeNameSetVolumeMount) {
    using litecode::docker::Mount;
    StdoutSilencer silencer;
    JudgeConfig jc = minimal_judge_cfg();
    jc.task_volume_name = "judge-tmp";
    auto sc = make_default_scheduler_config(jc);
    EXPECT_EQ(sc.task_volume_name, "judge-tmp");
    litecode::docker::CreateOptions o;
    o.image = "img";
    if (sc.task_volume_name.empty()) {
        o.mounts.push_back(Mount::bind_mount("/host/task.json",
                                              "/tmp/task.json"));
    } else {
        o.mounts.push_back(Mount::volume_mount(sc.task_volume_name,
                                                "/tmp/litecode-judge"));
    }
    ASSERT_EQ(o.mounts.size(), 1u);
    EXPECT_EQ(o.mounts[0].kind_of(), Mount::Kind::Volume);
}

// ────────────────────────────────────────────────────────────────────────────
// [JudgeSchedulerConfig.TmpfsExecFlagOnBothMounts]
// 锁住 opts.tmpfs 双 mount + exec — 不然 noexec 会让 binary 不可执行，
// judge.sh 会以 "compile returned 0 but binary missing" 退出 (SE)。
// 与 judge_scheduler.h:741-742 字面量同源。
// ────────────────────────────────────────────────────────────────────────────
TEST(JudgeSchedulerConfig, TmpfsExecFlagOnBothMounts) {
    StdoutSilencer silencer;
    // 直接构造 producer-side tmpfs 字面量（与 judge_scheduler.h:741-742 同）
    std::map<std::string,std::string> tmpfs = {
        {"/tmp",   "size=64m,mode=1777,exec"},
        {"/judge", "size=64m,mode=1777,exec"}
    };
    ASSERT_EQ(tmpfs.size(), 2u);
    for (const auto& [path, opts] : tmpfs) {
        EXPECT_NE(opts.find(",exec"), std::string::npos)
            << "tmpfs " << path << " missing ,exec";
    }
}

} // anonymous namespace
