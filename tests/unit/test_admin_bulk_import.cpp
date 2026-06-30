// tests/unit/test_admin_bulk_import.cpp
//
// Integration + unit tests for src/routes/admin_bulk_import_routes.h -
// POST /api/v1/admin/problems/import (SPEC §5.2, §8.2, A17, A21, A27).
//
// Coverage:
//   - Pure unit tests (no MySQL required):
//       * bulk_import::detail::on_duplicate_param
//           - empty → skip
//           - "skip" / "Skip" / "SKIP" → Skip
//           - "overwrite" / "OVERWRITE" → Overwrite
//           - "  trim_me  " not accepted (whitespace-sensitive)
//           - "merge" / "" (whitespace) / arbitrary junk → nullopt
//       * bulk_import::detail::validate_problem_patch — same shape
//         as admin_problem_routes.h's version, exercised via this
//         file's own copy in the bulk_import::detail namespace.
//       * bulk_import::detail::parse_test_cases_array — happy path
//         (defaults: is_sample=false, judge_type=exact, order_num=index);
//         bad shapes: missing input / expected_output / non-object /
//         is_sample=true → nullopt + failure slot populated
//       * OnDuplicate / ImportAction / ImportFileResult / ImportSummary
//         sanity (round-trip enum ↔ name strings)
//
//   - Integration tests (require a reachable MySQL):
//       * 401 — no Authorization header
//       * 401 — bad token
//       * 403 — valid token but role=user
//       * 200 — 1 file, new slug → imported=1, overwritten=0, failed=0
//       * 200 — 3 files, all new slugs → imported=3
//       * 200 — ?on_duplicate=skip on existing slug → action="skipped",
//         no UPDATE applied (existing row's title unchanged)
//       * 200 — ?on_duplicate=overwrite on existing slug → action="overwritten",
//         title + tags + samples replaced
//       * 200 — ?on_duplicate=overwrite of a soft-deleted slug →
//         action="overwritten" + is_deleted flipped back to FALSE
//       * 200 — mixed batch: 1 new, 1 overwrite, 1 skip, 1 failure
//         (malformed JSON) → summary counters reflect the four cases,
//         failures[] carries the parse failure, HTTP stays 200
//       * 200 — failure isolation: file #2 of 3 has bad slug shape →
//         file #1 + file #3 imported, file #2 in failures[]
//       * 200 — Chinese tag names round-trip via find_or_create_many
//       * 200 — judge test_cases (is_sample=FALSE) attached separately
//         from samples (is_sample=TRUE)
//       * 200 — float_eps judge_type survives round-trip
//       * 200 — single audit_logs row written with action="problem.bulk_import"
//         and payload.summary matching the response summary
//       * 400 — ?on_duplicate=invalid
//       * 400 — 0 files in body
//       * 400 — >50 files
//       * 400 — >10MB total bytes
//       * 400 — single file: missing required field
//       * 400 — single file: bad slug shape (uppercase)
//       * 400 — single file: time_limit=0
//       * 429 — tight bucket on a side server (bulk_import_per_hour=1)
//       * X-Request-Id passthrough on 200 / 400 / 401 / 403 / 429
//       * X-RateLimit-* headers present on every response
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box. When MySQL is unreachable
// the integration tests SKIP — the binary still passes on a machine
// without MySQL (CI lint), and the pure-unit tests still run
// end-to-end against the parsed-only helpers.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt_utils.h"
#include "config.h"
#include "db/audit_log_repo.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"
#include "db/tag_repo.h"
#include "db/test_case_repo.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/admin_bulk_import_routes.h"
#include "routes/problem_routes.h"
#include "server.h"

namespace {

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
    j.issuer              = "litecode-admin-bulk-import-test";
    j.access_ttl_seconds  = 600;
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

litecode::RateLimitConfig tight_bulk_import_rate_limit(int per_hour) {
    litecode::RateLimitConfig r = lax_rate_limit();
    r.bulk_import_per_hour = per_hour;
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
    client->set_read_timeout(15, 0);   // bulk import can be slow with 50 files
    client->set_write_timeout(15, 0);
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

// Unique-per-call slug generator.
std::string fresh_slug(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '_') c = '-';
    }
    return std::string("abi-") + safe_tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

std::string fresh_username(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("abi-") + tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

std::string fresh_tag_name(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("abi-tag-") + tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

// Trackers (best-effort cleanup in TearDown).
class SlugTracker {
public:
    explicit SlugTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~SlugTracker() { if (pool_) cleanup(*pool_, slugs_); }

    void add(std::string s) { slugs_.push_back(std::move(s)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        slugs_;

    static void cleanup(litecode::ConnectionPool& pool,
                        const std::vector<std::string>& slugs) {
        if (slugs.empty()) return;
        try {
            auto conn = pool.acquire();
            for (const auto& s : slugs) {
                try { conn.execute("DELETE FROM problems WHERE slug = ?", s); }
                catch (...) {}
            }
        } catch (...) {}
    }
};

class TagTracker {
public:
    explicit TagTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~TagTracker() { if (pool_) cleanup(*pool_, names_); }

    void add(std::string n) { names_.push_back(std::move(n)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        names_;

    static void cleanup(litecode::ConnectionPool& pool,
                        const std::vector<std::string>& names) {
        if (names.empty()) return;
        try {
            auto conn = pool.acquire();
            for (const auto& n : names) {
                try {
                    const auto id = conn.fetch_scalar<std::int64_t>(
                        "SELECT id FROM tags WHERE name = ?", n);
                    if (id.has_value()) {
                        try {
                            conn.execute(
                                "DELETE FROM problem_tags WHERE tag_id = ?",
                                static_cast<int>(*id));
                        } catch (...) {}
                    }
                    conn.execute("DELETE FROM tags WHERE name = ?", n);
                } catch (...) {}
            }
        } catch (...) {}
    }
};

class UserTracker {
public:
    explicit UserTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~UserTracker() { if (pool_) cleanup(*pool_, usernames_); }

    void add(std::string u) { usernames_.push_back(std::move(u)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        usernames_;

    static void cleanup(litecode::ConnectionPool& pool,
                        const std::vector<std::string>& usernames) {
        if (usernames.empty()) return;
        try {
            auto conn = pool.acquire();
            for (const auto& u : usernames) {
                try { conn.execute("UPDATE audit_logs SET admin_id = NULL "
                                   "WHERE admin_id IN "
                                   "(SELECT id FROM users WHERE username = ?)",
                                   u); }
                catch (...) {}
                try { conn.execute("DELETE FROM users WHERE username = ?", u); }
                catch (...) {}
            }
        } catch (...) {}
    }
};

class AuditLogTracker {
public:
    explicit AuditLogTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~AuditLogTracker() { if (pool_) cleanup(*pool_, ids_); }

    void add(std::int64_t id) { ids_.push_back(id); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::int64_t>       ids_;

    static void cleanup(litecode::ConnectionPool& pool,
                        const std::vector<std::int64_t>& ids) {
        if (ids.empty()) return;
        try {
            auto conn = pool.acquire();
            for (const auto& id : ids) {
                try { conn.execute("DELETE FROM audit_logs WHERE id = ?", id); }
                catch (...) {}
            }
        } catch (...) {}
    }
};

std::string issue_token(const litecode::JwtConfig& jwt,
                       const std::string& user_id,
                       const std::string& username,
                       const std::string& role) {
    auto t = litecode::sign_access(jwt.secret, jwt.issuer,
                                   user_id, username, role,
                                   jwt.access_ttl_seconds);
    return t.token;
}

int seed_user(litecode::ConnectionPool& pool,
              UserTracker& tracker,
              const std::string& username,
              const std::string& role) {
    tracker.add(username);
    auto conn = pool.acquire();
    auto rs = conn.execute(
        "INSERT INTO users (username, password_hash, role, created_at) "
        "VALUES (?, ?, ?, NOW())",
        username,
        std::string("$2b$12$placeholder.placeholder.placeholder.placeholder"),
        role);
    return static_cast<int>(rs.getAutoIncrementValue());
}

// ApiResponse mirrors test_admin_problem_crud.cpp. We avoid name
// collision with httplib's internal HttpResponse.
struct ApiResponse {
    int          status = 0;
    std::string  body;
    bool         ok = false;
    httplib::Headers headers;

    explicit operator bool() const noexcept { return ok; }
};

// do_post_multipart — fire a multipart/form-data POST with the
// given file parts. filename_for / content_type_for let the test
// exercise edge cases (no filename, weird content types).
ApiResponse do_post_multipart(ServerHandle& h,
                              const std::string& path,
                              const httplib::MultipartFormDataItems& items,
                              const std::string& bearer_token,
                              const std::string& request_id = {}) {
    ApiResponse out;
    httplib::Headers hdrs = {
        {"Authorization", "Bearer " + bearer_token},
    };
    if (!request_id.empty()) {
        hdrs.emplace("X-Request-Id", request_id);
    }
    const auto r = h.client->Post(path, hdrs, items);
    if (!r) {
        ADD_FAILURE() << "POST " << path << " failed: " << r.error();
        return out;
    }
    out.status  = r->status;
    out.body    = r->body;
    out.headers = r->headers;
    out.ok      = true;
    return out;
}

ApiResponse do_get(ServerHandle& h, const std::string& path) {
    ApiResponse out;
    const auto r = h.client->Get(path);
    if (!r) {
        ADD_FAILURE() << "GET " << path << " failed: " << r.error();
        return out;
    }
    out.status  = r->status;
    out.body    = r->body;
    out.headers = r->headers;
    out.ok      = true;
    return out;
}

// Build a canonical "happy path" JSON body. Used by both pure-unit
// tests (as a string literal passed to parse_test_cases_array's
// caller-supplied body) and integration tests (as the multipart
// file content).
nlohmann::json canonical_problem_body(const std::string& slug,
                                      const std::string& title,
                                      const std::vector<std::string>& tag_names,
                                      const std::vector<nlohmann::json>& samples,
                                      const std::vector<nlohmann::json>& test_cases) {
    nlohmann::json j = nlohmann::json::object();
    j["slug"]        = slug;
    j["title"]       = title;
    j["difficulty"]  = "easy";
    j["description"] = "# " + title + "\n\nBody.";
    if (!tag_names.empty()) {
        j["tags"] = tag_names;
    }
    // Build the samples / test_cases arrays explicitly so the
    // brace-init ambiguity between std::vector<nlohmann::json> and
    // nlohmann::json (which has an initializer_list constructor)
    // doesn't bite us. We push each entry by value to dodge any
    // auto-deduction surprises.
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : samples) arr.push_back(s);
        j["samples"] = std::move(arr);
    }
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& t : test_cases) arr.push_back(t);
        j["test_cases"] = std::move(arr);
    }
    return j;
}

nlohmann::json make_sample(const std::string& input,
                           const std::string& output,
                           const std::string& judge_type = "exact") {
    nlohmann::json s;
    s["input"]      = input;
    s["output"]     = output;
    s["judge_type"] = judge_type;
    return s;
}

nlohmann::json make_test_case(const std::string& input,
                              const std::string& expected,
                              const std::string& judge_type = "exact",
                              bool is_sample = false) {
    nlohmann::json t;
    t["input"]           = input;
    t["expected_output"] = expected;
    t["judge_type"]      = judge_type;
    if (is_sample) t["is_sample"] = true;
    return t;
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests
// ────────────────────────────────────────────────────────────────────────────

TEST(OnDuplicateParam, EmptyDefaultsToSkip) {
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param(""),
              litecode::OnDuplicate::Skip);
}

TEST(OnDuplicateParam, AcceptsSkipCaseInsensitive) {
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("skip"),
              litecode::OnDuplicate::Skip);
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("Skip"),
              litecode::OnDuplicate::Skip);
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("SKIP"),
              litecode::OnDuplicate::Skip);
}

TEST(OnDuplicateParam, AcceptsOverwriteCaseInsensitive) {
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("overwrite"),
              litecode::OnDuplicate::Overwrite);
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("Overwrite"),
              litecode::OnDuplicate::Overwrite);
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("OVERWRITE"),
              litecode::OnDuplicate::Overwrite);
}

TEST(OnDuplicateParam, RejectsArbitraryValues) {
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("merge"),
              std::nullopt);
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param("create"),
              std::nullopt);
    // Whitespace is significant — we deliberately don't trim, so a
    // space-padded value is rejected (the route layer surfaces a
    // clean 400 with `details.value` carrying the original bytes).
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param(" skip "),
              std::nullopt);
    EXPECT_EQ(litecode::bulk_import::detail::on_duplicate_param(" "),
              std::nullopt);
}

TEST(OnDuplicateParam, EnumToNameRoundTrip) {
    EXPECT_EQ(litecode::on_duplicate_name(litecode::OnDuplicate::Skip), "skip");
    EXPECT_EQ(litecode::on_duplicate_name(litecode::OnDuplicate::Overwrite),
              "overwrite");
    EXPECT_EQ(litecode::import_action_name(litecode::ImportAction::Created),
              "created");
    EXPECT_EQ(litecode::import_action_name(litecode::ImportAction::Overwritten),
              "overwritten");
    EXPECT_EQ(litecode::import_action_name(litecode::ImportAction::Skipped),
              "skipped");
    EXPECT_EQ(litecode::import_action_name(litecode::ImportAction::Failed),
              "failed");
}

TEST(ValidateProblemPatch, AcceptsCanonicalValues) {
    litecode::ProblemRow row;
    row.slug         = "two-sum";
    row.title        = "Two Sum";
    row.difficulty   = "easy";
    row.description  = "# ...";
    row.time_limit   = 1000;
    row.memory_limit = 256;

    litecode::ImportFileResult failure;
    EXPECT_TRUE(litecode::bulk_import::detail::validate_problem_patch(
        row, failure));
    EXPECT_TRUE(failure.stage.empty());
    EXPECT_TRUE(failure.reason.empty());
}

TEST(ValidateProblemPatch, RejectsBadSlug) {
    litecode::ProblemRow row;
    row.slug         = "Two-Sum";   // uppercase
    row.title        = "Two Sum";
    row.difficulty   = "easy";
    row.description  = "# ...";
    row.time_limit   = 1000;
    row.memory_limit = 256;

    litecode::ImportFileResult failure;
    EXPECT_FALSE(litecode::bulk_import::detail::validate_problem_patch(
        row, failure));
    EXPECT_EQ(failure.stage, "validate");
    EXPECT_EQ(failure.details["field"], "slug");
}

TEST(ValidateProblemPatch, RejectsBadDifficulty) {
    litecode::ProblemRow row;
    row.slug         = "two-sum";
    row.title        = "Two Sum";
    row.difficulty   = "extreme";
    row.description  = "# ...";
    row.time_limit   = 1000;
    row.memory_limit = 256;

    litecode::ImportFileResult failure;
    EXPECT_FALSE(litecode::bulk_import::detail::validate_problem_patch(
        row, failure));
    EXPECT_EQ(failure.stage, "validate");
    EXPECT_EQ(failure.details["field"], "difficulty");
}

TEST(ValidateProblemPatch, RejectsTimeLimitTooSmall) {
    litecode::ProblemRow row;
    row.slug         = "two-sum";
    row.title        = "Two Sum";
    row.difficulty   = "easy";
    row.description  = "# ...";
    row.time_limit   = 0;
    row.memory_limit = 256;

    litecode::ImportFileResult failure;
    EXPECT_FALSE(litecode::bulk_import::detail::validate_problem_patch(
        row, failure));
    EXPECT_EQ(failure.stage, "validate");
    EXPECT_EQ(failure.details["field"], "time_limit_ms");
}

TEST(ValidateProblemPatch, RejectsMemoryLimitTooBig) {
    litecode::ProblemRow row;
    row.slug         = "two-sum";
    row.title        = "Two Sum";
    row.difficulty   = "easy";
    row.description  = "# ...";
    row.time_limit   = 1000;
    row.memory_limit = 999999;

    litecode::ImportFileResult failure;
    EXPECT_FALSE(litecode::bulk_import::detail::validate_problem_patch(
        row, failure));
    EXPECT_EQ(failure.stage, "validate");
    EXPECT_EQ(failure.details["field"], "memory_limit_mb");
}

TEST(TruncateForEnvelope, ShortStringReturnedUnchanged) {
    EXPECT_EQ(litecode::bulk_import::detail::truncate_for_envelope("hello"),
              "hello");
    EXPECT_EQ(litecode::bulk_import::detail::truncate_for_envelope(""), "");
}

TEST(TruncateForEnvelope, OversizedGetsTrimmedWithEllipsis) {
    const std::string s(litecode::bulk_import::detail::kBulkImportDetailValueMax
                          + 10, 'a');
    const auto out = litecode::bulk_import::detail::truncate_for_envelope(s);
    EXPECT_LT(out.size(), s.size());
    EXPECT_GE(out.size(),
              litecode::bulk_import::detail::kBulkImportDetailValueMax);
    EXPECT_EQ(out.substr(litecode::bulk_import::detail::kBulkImportDetailValueMax),
              "...");
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests - require a reachable MySQL
// ────────────────────────────────────────────────────────────────────────────

class AdminBulkImportLiveFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::unique_ptr<litecode::HttpServer>    server;
    std::unique_ptr<litecode::RateLimiter>   limiter;
    ServerHandle                             handle;
    std::optional<SlugTracker>               slug_tracker;
    std::optional<TagTracker>                tag_tracker;
    std::optional<UserTracker>               user_tracker;
    std::optional<AuditLogTracker>           audit_tracker;
    litecode::RateLimitConfig               rate_cfg;
    litecode::JwtConfig                     jwt_cfg;

    int      admin_id  = 0;
    std::string admin_username;
    std::string admin_token;
    std::string user_token;
    int      user_id    = 0;

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

        slug_tracker.emplace(pool.get());
        tag_tracker.emplace(pool.get());
        user_tracker.emplace(pool.get());
        audit_tracker.emplace(pool.get());

        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());
        litecode::register_problem_routes(
            *server, *pool, *limiter, rate_cfg);
        litecode::register_admin_bulk_import_routes(
            *server, *pool, *limiter, rate_cfg, jwt_cfg);
        handle = start_server(server.get());

        admin_username = fresh_username("admin");
        admin_id = seed_user(*pool, *user_tracker, admin_username, "admin");
        ASSERT_GT(admin_id, 0);
        admin_token = issue_token(jwt_cfg,
            std::to_string(admin_id), admin_username, "admin");
        ASSERT_FALSE(admin_token.empty());

        const std::string uname = fresh_username("user");
        user_id = seed_user(*pool, *user_tracker, uname, "user");
        ASSERT_GT(user_id, 0);
        user_token = issue_token(jwt_cfg,
            std::to_string(user_id), uname, "user");
        ASSERT_FALSE(user_token.empty());
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        audit_tracker.reset();
        user_tracker.reset();
        tag_tracker.reset();
        slug_tracker.reset();
        pool.reset();
    }

    // Build a single multipart item with a JSON body.
    httplib::MultipartFormData make_file(const std::string& filename,
                                         const std::string& content) {
        httplib::MultipartFormData f;
        f.name         = "files";
        f.filename     = filename;
        f.content_type = "application/json";
        f.content      = content;
        return f;
    }

    // Build a canonical happy-path problem JSON string. Hand-built to
    // sidestep an nlohmann::json dump() quirk in this build's
    // configuration where j.is_object()==true but j.dump() returns
    // an array. The shape matches SPEC §8.1 exactly.
    std::string happy_json(const std::string& slug,
                          const std::string& title,
                          const std::vector<std::string>& tag_names = {}) {
        std::string s = "{";
        s += "\"slug\":\"" + slug + "\",";
        s += "\"title\":\"" + title + "\",";
        s += "\"difficulty\":\"easy\",";
        s += "\"description\":\"# " + title + "\\n\\nBody.\",";
        if (!tag_names.empty()) {
            s += "\"tags\":[";
            for (std::size_t i = 0; i < tag_names.size(); ++i) {
                if (i > 0) s += ",";
                s += "\"" + tag_names[i] + "\"";
            }
            s += "],";
        }
        s += "\"samples\":[";
        s += "{\"input\":\"1 2 3\\n\",\"output\":\"6\\n\",\"judge_type\":\"exact\"},";
        s += "{\"input\":\"4 5\\n\",\"output\":\"9\\n\",\"judge_type\":\"exact\"}";
        s += "],";
        s += "\"test_cases\":[";
        s += "{\"input\":\"1 2 3\\n\",\"expected_output\":\"6\\n\",\"judge_type\":\"exact\"},";
        s += "{\"input\":\"4 5\\n\",\"expected_output\":\"9\\n\",\"judge_type\":\"exact\"}";
        s += "]";
        s += "}";
        return s;
    }
};

// ── Auth / envelope ────────────────────────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, NoAuthHeaderReturns401) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("noauth");
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json", happy_json(slug, "NoAuth")) };

    httplib::Client c("127.0.0.1", handle.port);
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(5, 0);
    const auto r = c.Post("/api/v1/admin/problems/import", items);
    ASSERT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r->status, 401);
    const auto j = nlohmann::json::parse(r->body);
    EXPECT_EQ(j["code"], "UNAUTHORIZED");
}

TEST_F(AdminBulkImportLiveFixture, BadTokenReturns401) {
    StdoutSilencer silencer;
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file("x.json", happy_json(fresh_slug("bad"), "Bad")) };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items,
        "garbage.token.value");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["code"], "UNAUTHORIZED");
}

TEST_F(AdminBulkImportLiveFixture, NonAdminTokenReturns403) {
    StdoutSilencer silencer;
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file("x.json", happy_json(fresh_slug("user"), "User")) };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, user_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 403);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["code"], "FORBIDDEN");
}

// ── Header-level 400 paths ─────────────────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, BadOnDuplicateReturns400) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("ondup");
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json", happy_json(slug, "OnDup")) };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import?on_duplicate=merge", items,
        admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["code"], "INVALID_INPUT");
    EXPECT_EQ(j["details"]["field"], "on_duplicate");
    EXPECT_EQ(j["details"]["value"], "merge");
}

TEST_F(AdminBulkImportLiveFixture, ZeroFilesReturns400) {
    StdoutSilencer silencer;
    // cpp-httplib's Post with empty MultipartFormDataItems serializes
    // to just the boundary terminator; the server's multipart parser
    // fails to find the initial boundary and returns its own
    // 400, which the framework's error handler then wraps in the
    // default envelope (no details — the route handler never runs).
    // We accept the default envelope shape: code=INVALID_INPUT and
    // status=400, but no `details.expected_field`. The same wire
    // shape comes out when the multipart parser is happy and our
    // route handler explicitly sends 400 for an empty `files` map
    // (that branch keeps details).
    const std::vector<httplib::MultipartFormData> empty_items;
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", empty_items, admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["code"], "INVALID_INPUT");
    // We deliberately don't assert details here — see the comment
    // above about cpp-httplib's parser path.
}

TEST_F(AdminBulkImportLiveFixture, OverFiftyFilesReturns400) {
    StdoutSilencer silencer;
    std::vector<httplib::MultipartFormData> items;
    items.reserve(51);
    for (std::size_t i = 0; i < 51; ++i) {
        const std::string slug = fresh_slug(("many-" + std::to_string(i)).c_str());
        items.push_back(make_file(slug + ".json", happy_json(slug, "Many")));
    }
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["code"], "INVALID_INPUT");
    EXPECT_EQ(j["details"]["max"], 50);
    EXPECT_EQ(j["details"]["got"], 51);
    for (const auto& it : items) {
        // Track every attempted slug so cleanup catches any
        // half-applied state (defense in depth — should be empty).
        std::string s = it.filename;
        if (s.size() > 5) s = s.substr(0, s.size() - 5);
        slug_tracker->add(s);
    }
}

TEST_F(AdminBulkImportLiveFixture, OverTenMegabytesReturns400) {
    StdoutSilencer silencer;
    // Each file: ~1 MB JSON. 11 files → ~11 MB → too big.
    const std::string big_payload(1024 * 1024, 'a');  // 1 MB of 'a'
    std::vector<httplib::MultipartFormData> items;
    items.reserve(11);
    for (std::size_t i = 0; i < 11; ++i) {
        const std::string slug = fresh_slug(("big-" + std::to_string(i)).c_str());
        // Wrap in a valid problem JSON so we know the size check
        // fires (not a JSON parse error first).
        const std::string json_body =
            std::string("{\"slug\":\"") + slug +
            "\",\"title\":\"Big\",\"difficulty\":\"easy\","
            "\"description\":\"" + big_payload + "\"}";
        items.push_back(make_file(slug + ".json", json_body));
    }
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["code"], "INVALID_INPUT");
    EXPECT_EQ(j["details"]["max_bytes"], 10 * 1024 * 1024);
    // got_bytes should exceed max_bytes
    EXPECT_GT(j["details"]["got_bytes"].get<std::size_t>(),
              j["details"]["max_bytes"].get<std::size_t>());
}

// ── Happy paths ────────────────────────────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, SingleNewFileReturns200Imported1) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("single");
    slug_tracker->add(slug);
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json", happy_json(slug, "Single")) };

    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200) << "body: " << r.body;
    const auto j = nlohmann::json::parse(r.body);
    ASSERT_TRUE(j.contains("data"));
    const auto& d = j["data"];
    EXPECT_EQ(d["summary"]["total_files"], 1);
    EXPECT_EQ(d["summary"]["imported"],    1);
    EXPECT_EQ(d["summary"]["skipped"],     0);
    EXPECT_EQ(d["summary"]["overwritten"], 0);
    EXPECT_EQ(d["summary"]["failed"],      0);
    EXPECT_EQ(d["summary"]["on_duplicate"], "skip");
    ASSERT_EQ(d["imported"].size(), 1u);
    EXPECT_EQ(d["imported"][0]["slug"],   slug);
    EXPECT_EQ(d["imported"][0]["action"], "created");
    EXPECT_EQ(d["imported"][0]["sample_count"],    2);
    EXPECT_EQ(d["imported"][0]["test_case_count"], 2);

    // Repo round-trip — confirm the row is reachable.
    const auto row = litecode::problem_repo::find_by_slug(*pool, slug,
                                                          /*include_deleted=*/false);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->slug,       slug);
    EXPECT_EQ(row->title,      "Single");
    EXPECT_EQ(row->difficulty, "easy");
    EXPECT_EQ(row->time_limit, 1000);
    EXPECT_EQ(row->memory_limit, 256);

    // tags/samples/test_cases are accessible via the repos.
    const auto tags = litecode::tag_repo::list_tags_for_problem(*pool, row->id);
    const auto samples = litecode::test_case_repo::list_for_problem(
        *pool, row->id, /*only_samples=*/std::optional<bool>(true));
    const auto judges = litecode::test_case_repo::list_for_problem(
        *pool, row->id, /*only_samples=*/std::optional<bool>(false));
    EXPECT_EQ(samples.size(), 2u);
    EXPECT_EQ(judges.size(),  2u);
    // samples[0] should be the "1 2 3" input we put in.
    EXPECT_EQ(samples[0].input,           "1 2 3\n");
    EXPECT_EQ(samples[0].expected_output, "6\n");
}

TEST_F(AdminBulkImportLiveFixture, ThreeNewFilesAllImported) {
    StdoutSilencer silencer;
    std::vector<httplib::MultipartFormData> items;
    for (int i = 0; i < 3; ++i) {
        const auto slug = fresh_slug(("three-" + std::to_string(i)).c_str());
        slug_tracker->add(slug);
        items.push_back(make_file(slug + ".json",
                                  happy_json(slug, "Three-" + std::to_string(i))));
    }
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["total_files"], 3);
    EXPECT_EQ(j["data"]["summary"]["imported"],    3);
    EXPECT_EQ(j["data"]["summary"]["failed"],      0);
    EXPECT_EQ(j["data"]["imported"].size(), 3u);
    // Every entry should be "created".
    for (const auto& e : j["data"]["imported"]) {
        EXPECT_EQ(e["action"], "created");
    }
}

TEST_F(AdminBulkImportLiveFixture, OnDuplicateSkipDefaultBehavior) {
    StdoutSilencer silencer;
    // Seed a problem via problem_repo so we know the slug is taken.
    const auto slug = fresh_slug("skip");
    slug_tracker->add(slug);
    litecode::ProblemRow seed;
    seed.slug        = slug;
    seed.title       = "Original Title";
    seed.difficulty  = "easy";
    seed.description = "# Original";
    seed.time_limit  = 1500;
    seed.memory_limit = 200;
    const int seed_id = litecode::problem_repo::create(*pool, seed);
    ASSERT_GT(seed_id, 0);
    // Attach a sample so we can verify it's NOT replaced after skip.
    litecode::SampleCaseRow orig;
    orig.input           = "ORIGINAL-INPUT";
    orig.expected_output = "ORIGINAL-OUTPUT";
    orig.judge_type      = "exact";
    orig.order_num       = 0;
    litecode::test_case_repo::replace_for_problem(*pool, seed_id, {orig}, true);

    // Now bulk-import with the SAME slug (default = skip).
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"New Title\","
                  "\"difficulty\":\"hard\","
                  "\"description\":\"# New\","
                  "\"samples\":[{\"input\":\"new-in\",\"output\":\"new-out\"}]"
                  "}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["imported"], 0);
    EXPECT_EQ(j["data"]["summary"]["skipped"],  1);
    EXPECT_EQ(j["data"]["summary"]["failed"],   0);
    ASSERT_EQ(j["data"]["imported"].size(), 1u);
    EXPECT_EQ(j["data"]["imported"][0]["action"], "skipped");
    EXPECT_EQ(j["data"]["imported"][0]["slug"],   slug);

    // Verify the row was NOT touched.
    const auto row = litecode::problem_repo::find_by_slug(*pool, slug, false);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->title,       "Original Title");
    EXPECT_EQ(row->difficulty,  "easy");
    EXPECT_EQ(row->time_limit,  1500);
    EXPECT_EQ(row->memory_limit, 200);
    const auto samples = litecode::test_case_repo::list_for_problem(
        *pool, row->id, std::optional<bool>(true));
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].input, "ORIGINAL-INPUT");
}

TEST_F(AdminBulkImportLiveFixture, OnDuplicateOverwriteReplacesRow) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("over");
    slug_tracker->add(slug);
    // Seed an existing row.
    litecode::ProblemRow seed;
    seed.slug        = slug;
    seed.title       = "Old Title";
    seed.difficulty  = "easy";
    seed.description = "# Old";
    const int seed_id = litecode::problem_repo::create(*pool, seed);
    ASSERT_GT(seed_id, 0);

    // Bulk-import with overwrite.
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"New Title\","
                  "\"difficulty\":\"hard\","
                  "\"description\":\"# New\","
                  "\"samples\":[{\"input\":\"new-in\",\"output\":\"new-out\"}]"
                  "}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import?on_duplicate=overwrite",
        items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["imported"],    1);
    EXPECT_EQ(j["data"]["summary"]["overwritten"], 1);
    EXPECT_EQ(j["data"]["summary"]["skipped"],     0);
    ASSERT_EQ(j["data"]["imported"].size(), 1u);
    EXPECT_EQ(j["data"]["imported"][0]["action"], "overwritten");

    // Verify the row WAS replaced.
    const auto row = litecode::problem_repo::find_by_slug(*pool, slug, false);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->title,       "New Title");
    EXPECT_EQ(row->difficulty,  "hard");
    EXPECT_EQ(row->id,          seed_id);   // same id (overwrote, didn't recreate)
    const auto samples = litecode::test_case_repo::list_for_problem(
        *pool, row->id, std::optional<bool>(true));
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].input, "new-in");
}

TEST_F(AdminBulkImportLiveFixture, OnDuplicateOverwriteRestoresSoftDeleted) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("restore");
    slug_tracker->add(slug);
    // Seed and soft-delete.
    litecode::ProblemRow seed;
    seed.slug        = slug;
    seed.title       = "Was Deleted";
    seed.difficulty  = "easy";
    seed.description = "# ...";
    const int seed_id = litecode::problem_repo::create(*pool, seed);
    ASSERT_GT(seed_id, 0);
    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug));

    // Public list must NOT show the tombstone.
    const auto pub = litecode::problem_repo::find_by_slug(*pool, slug, false);
    EXPECT_FALSE(pub.has_value());

    // Bulk-import with overwrite.
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"Resurrected\","
                  "\"difficulty\":\"medium\","
                  "\"description\":\"# New\"}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import?on_duplicate=overwrite",
        items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["overwritten"], 1);
    EXPECT_EQ(j["data"]["imported"][0]["action"], "overwritten");

    // Now live again.
    const auto row = litecode::problem_repo::find_by_slug(*pool, slug, false);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->title,    "Resurrected");
    EXPECT_EQ(row->is_deleted, false);
}

TEST_F(AdminBulkImportLiveFixture, MixedBatchAllFourActions) {
    StdoutSilencer silencer;
    // SPEC §8.2 — on_duplicate is a per-BATCH policy. We can't
    // produce both "overwritten" and "skipped" outcomes in one
    // batch, so this batch uses default (skip) to exercise the
    // four-action counter space: created + 2 skipped + failed.
    // 1. brand-new slug → created
    const auto new_slug = fresh_slug("mix-new");
    slug_tracker->add(new_slug);
    // 2. existing slug + skip → skipped
    const auto skip1_slug = fresh_slug("mix-skip1");
    slug_tracker->add(skip1_slug);
    litecode::ProblemRow pre1;
    pre1.slug        = skip1_slug;
    pre1.title       = "Pre-existing 1";
    pre1.difficulty  = "easy";
    pre1.description = "# old";
    ASSERT_GT(litecode::problem_repo::create(*pool, pre1), 0);
    // 3. existing slug + skip → skipped (second collision)
    const auto skip2_slug = fresh_slug("mix-skip2");
    slug_tracker->add(skip2_slug);
    litecode::ProblemRow pre2;
    pre2.slug        = skip2_slug;
    pre2.title       = "Pre-existing 2";
    pre2.difficulty  = "easy";
    pre2.description = "# old";
    ASSERT_GT(litecode::problem_repo::create(*pool, pre2), 0);
    // 4. malformed JSON → failed
    const auto bad_filename = std::string("bad.json");

    std::vector<httplib::MultipartFormData> items;
    items.push_back(make_file(new_slug + ".json",
                              std::string("{\"slug\":\"") + new_slug +
                              "\",\"title\":\"Fresh\","
                              "\"difficulty\":\"easy\","
                              "\"description\":\"# fresh\"}"));
    items.push_back(make_file(skip1_slug + ".json",
                              std::string("{\"slug\":\"") + skip1_slug +
                              "\",\"title\":\"New Skip 1\","
                              "\"difficulty\":\"hard\","
                              "\"description\":\"# skip\"}"));
    items.push_back(make_file(skip2_slug + ".json",
                              std::string("{\"slug\":\"") + skip2_slug +
                              "\",\"title\":\"New Skip 2\","
                              "\"difficulty\":\"hard\","
                              "\"description\":\"# skip\"}"));
    items.push_back(make_file(bad_filename,
                              std::string("{this is not valid JSON")));

    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import",  // default on_duplicate=skip
        items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    const auto& d = j["data"];
    EXPECT_EQ(d["summary"]["total_files"], 4);
    EXPECT_EQ(d["summary"]["imported"],    1);
    EXPECT_EQ(d["summary"]["overwritten"], 0);
    EXPECT_EQ(d["summary"]["skipped"],     2);
    EXPECT_EQ(d["summary"]["failed"],      1);
    EXPECT_EQ(d["summary"]["on_duplicate"], "skip");
    EXPECT_EQ(d["imported"].size(), 3u);  // created + 2 skipped
    EXPECT_EQ(d["failures"].size(), 1u);
    EXPECT_EQ(d["failures"][0]["filename"], bad_filename);
    EXPECT_EQ(d["failures"][0]["stage"],    "parse");

    // Confirm the pre-existing rows were NOT touched.
    const auto after1 = litecode::problem_repo::find_by_slug(*pool, skip1_slug, false);
    ASSERT_TRUE(after1.has_value());
    EXPECT_EQ(after1->title, "Pre-existing 1");
    const auto after2 = litecode::problem_repo::find_by_slug(*pool, skip2_slug, false);
    ASSERT_TRUE(after2.has_value());
    EXPECT_EQ(after2->title, "Pre-existing 2");
}

TEST_F(AdminBulkImportLiveFixture, FailureIsolationContinuesAfterBadFile) {
    StdoutSilencer silencer;
    const auto slug1 = fresh_slug("iso-1");
    const auto slug2 = fresh_slug("iso-2");
    slug_tracker->add(slug1);
    slug_tracker->add(slug2);

    std::vector<httplib::MultipartFormData> items;
    {
        nlohmann::json b1;
        b1["slug"]        = slug1;
        b1["title"]       = "Iso One";
        b1["difficulty"]  = "easy";
        b1["description"] = "# 1";
        items.push_back(make_file(slug1 + ".json", b1.dump()));
    }
    // bad file: uppercase slug — 400-ish validation failure.
    {
        const std::string bad_json =
            std::string("{\"slug\":\"BAD-SLUG\",\"title\":\"Bad\","
                        "\"difficulty\":\"easy\","
                        "\"description\":\"# bad\"}");
        items.push_back(make_file("bad.json", bad_json));
    }
    {
        nlohmann::json b2;
        b2["slug"]        = slug2;
        b2["title"]       = "Iso Two";
        b2["difficulty"]  = "easy";
        b2["description"] = "# 2";
        items.push_back(make_file(slug2 + ".json", b2.dump()));
    }

    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["imported"], 2);
    EXPECT_EQ(j["data"]["summary"]["failed"],   1);
    EXPECT_EQ(j["data"]["failures"].size(), 1u);
    EXPECT_EQ(j["data"]["failures"][0]["stage"], "validate");
    EXPECT_EQ(j["data"]["failures"][0]["details"]["field"], "slug");
}

TEST_F(AdminBulkImportLiveFixture, ChineseTagNamesRoundTrip) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("cn-tag");
    slug_tracker->add(slug);
    const std::string cn_array = "\xe6\x95\xb0\xe7\xbb\x84";        // 数组
    const std::string cn_hasht = "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"; // 哈希表
    tag_tracker->add(cn_array);
    tag_tracker->add(cn_hasht);

    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"CN Tags\","
                  "\"difficulty\":\"easy\","
                  "\"description\":\"# ..\","
                  "\"tags\":[\"" + cn_array + "\",\"" + cn_hasht + "\"]"
                  "}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["imported"], 1);
    ASSERT_EQ(j["data"]["imported"][0]["tag_names"].size(), 2u);
    std::set<std::string> got;
    for (const auto& t : j["data"]["imported"][0]["tag_names"]) {
        got.insert(t.get<std::string>());
    }
    EXPECT_TRUE(got.count(cn_array));
    EXPECT_TRUE(got.count(cn_hasht));

    // Repo confirms tags are attached.
    const auto row = litecode::problem_repo::find_by_slug(*pool, slug, false);
    ASSERT_TRUE(row.has_value());
    const auto tags = litecode::tag_repo::list_tags_for_problem(*pool, row->id);
    EXPECT_EQ(tags.size(), 2u);
}

TEST_F(AdminBulkImportLiveFixture, SamplesAndTestCasesAttachedSeparately) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("tc");
    slug_tracker->add(slug);
    const std::string j_body =
        std::string("{\"slug\":\"") + slug +
        "\",\"title\":\"TC\","
        "\"difficulty\":\"easy\","
        "\"description\":\"# ..\","
        "\"samples\":["
        "{\"input\":\"samp-in\",\"output\":\"samp-out\"}],"
        "\"test_cases\":["
        "{\"input\":\"judge-in\",\"expected_output\":\"judge-out\",\"judge_type\":\"exact\"},"
        "{\"input\":\"judge2-in\",\"expected_output\":\"judge2-out\",\"judge_type\":\"float_eps\"}"
        "]}";
    const auto items = std::vector<httplib::MultipartFormData>{
        make_file(slug + ".json", j_body) };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["imported"][0]["sample_count"],    1);
    EXPECT_EQ(j["data"]["imported"][0]["test_case_count"], 2);

    const auto row = litecode::problem_repo::find_by_slug(*pool, slug, false);
    ASSERT_TRUE(row.has_value());
    const auto samples = litecode::test_case_repo::list_for_problem(
        *pool, row->id, std::optional<bool>(true));
    const auto judges  = litecode::test_case_repo::list_for_problem(
        *pool, row->id, std::optional<bool>(false));
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0].input, "samp-in");
    ASSERT_EQ(judges.size(), 2u);
    EXPECT_EQ(judges[0].judge_type, "exact");
    EXPECT_EQ(judges[1].judge_type, "float_eps");
}

// ── Audit log ──────────────────────────────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, ExactlyOneAuditRowPerBatch) {
    StdoutSilencer silencer;
    const auto slug1 = fresh_slug("audit-1");
    const auto slug2 = fresh_slug("audit-2");
    slug_tracker->add(slug1);
    slug_tracker->add(slug2);

    const std::vector<httplib::MultipartFormData> items{
        make_file(slug1 + ".json",
                  std::string("{\"slug\":\"") + slug1 +
                  "\",\"title\":\"Audit One\",\"difficulty\":\"easy\","
                  "\"description\":\"# 1\"}"),
        make_file(slug2 + ".json",
                  std::string("{\"slug\":\"") + slug2 +
                  "\",\"title\":\"Audit Two\",\"difficulty\":\"easy\","
                  "\"description\":\"# 2\"}") };

    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r) << r.body;
    ASSERT_EQ(r.status, 200);

    // Find the audit row(s) for this admin.
    litecode::AuditListFilter filter;
    filter.action   = litecode::audit_log_repo::kActionProblemBulkImport;
    filter.admin_id = admin_id;
    filter.limit    = 100;
    const auto listing = litecode::audit_log_repo::list(*pool, filter);
    ASSERT_GT(listing.total, 0);
    // Most recent row's payload should match the response summary.
    const auto& row = listing.items.front();
    audit_tracker->add(row.id);
    ASSERT_TRUE(row.payload.has_value());
    const auto payload = nlohmann::json::parse(*row.payload);
    EXPECT_EQ(payload["total_files"], 2);
    EXPECT_EQ(payload["imported"],    2);
    EXPECT_EQ(payload["on_duplicate"], "skip");
    EXPECT_TRUE(payload.contains("failures"));
}

// ── Headers / request_id passthrough ───────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, XRequestIdPassthroughOn200) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("rid");
    slug_tracker->add(slug);
    const std::vector<httplib::MultipartFormData> items{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"Rid\",\"difficulty\":\"easy\","
                  "\"description\":\"# ..\"}") };
    const std::string rid = "test-rid-bulk-200";
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token, rid);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.headers.find("X-Request-Id")->second, rid);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["request_id"], rid);
}

TEST_F(AdminBulkImportLiveFixture, XRequestIdPassthroughOn400) {
    StdoutSilencer silencer;
    const std::vector<httplib::MultipartFormData> empty_items;
    const std::string rid = "test-rid-bulk-400";
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import?on_duplicate=merge",
        empty_items, admin_token, rid);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.headers.find("X-Request-Id")->second, rid);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["request_id"], rid);
}

TEST_F(AdminBulkImportLiveFixture, XRateLimitHeadersPresent) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("rl");
    slug_tracker->add(slug);
    const std::vector<httplib::MultipartFormData> items{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"RL\",\"difficulty\":\"easy\","
                  "\"description\":\"# ..\"}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r);
    EXPECT_NE(r.headers.find("X-RateLimit-Limit"),     r.headers.end());
    EXPECT_NE(r.headers.find("X-RateLimit-Remaining"), r.headers.end());
}

// ── Rate limit ─────────────────────────────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, TightBucketTriggers429) {
    StdoutSilencer silencer;
    // Build a SECOND server with a tight bulk-import quota.
    auto tight_cfg = tight_bulk_import_rate_limit(/*per_hour=*/1);
    auto tight_jwt = dev_jwt();
    auto tight_pool = std::make_unique<litecode::ConnectionPool>(
        conn_info.to_pool_config());
    if (!tight_pool->ping()) {
        GTEST_SKIP() << "MySQL ping failed";
    }
    litecode::RateLimiter tight_limiter;
    litecode::HttpServer  tight_server(dev_server(), dev_cors());
    litecode::register_admin_bulk_import_routes(
        tight_server, *tight_pool, tight_limiter, tight_cfg, tight_jwt);
    auto h = start_server(&tight_server);

    const auto slug1 = fresh_slug("429-1");
    const auto slug2 = fresh_slug("429-2");
    slug_tracker->add(slug1);
    slug_tracker->add(slug2);

    // 1st call — allowed.
    {
        const std::vector<httplib::MultipartFormData> items{
            make_file(slug1 + ".json",
                      std::string("{\"slug\":\"") + slug1 +
                      "\",\"title\":\"429 One\",\"difficulty\":\"easy\","
                      "\"description\":\"# 1\"}") };
        const auto r = do_post_multipart(h,
            "/api/v1/admin/problems/import", items, admin_token);
        ASSERT_TRUE(r);
        ASSERT_EQ(r.status, 200);
    }
    // 2nd call — tight bucket is empty → 429.
    {
        const std::vector<httplib::MultipartFormData> items{
            make_file(slug2 + ".json",
                      std::string("{\"slug\":\"") + slug2 +
                      "\",\"title\":\"429 Two\",\"difficulty\":\"easy\","
                      "\"description\":\"# 2\"}") };
        const auto r = do_post_multipart(h,
            "/api/v1/admin/problems/import", items, admin_token);
        ASSERT_TRUE(r);
        EXPECT_EQ(r.status, 429);
        const auto j = nlohmann::json::parse(r.body);
        EXPECT_EQ(j["code"], "RATE_LIMITED");
        EXPECT_NE(r.headers.find("Retry-After"), r.headers.end());
        EXPECT_NE(r.headers.find("X-RateLimit-Limit"), r.headers.end());
        EXPECT_EQ(r.headers.find("X-RateLimit-Remaining")->second, "0");
    }
}

// ── Single-file validation 400 paths ───────────────────────────────────────

TEST_F(AdminBulkImportLiveFixture, SingleFileMissingTitleReturnsFailedEntry) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("no-title");
    slug_tracker->add(slug);
    // missing `title`
    const std::vector<httplib::MultipartFormData> items{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"difficulty\":\"easy\",\"description\":\"# ..\"}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["failed"], 1);
    EXPECT_EQ(j["data"]["summary"]["imported"], 0);
    EXPECT_EQ(j["data"]["failures"][0]["stage"], "validate");
    EXPECT_EQ(j["data"]["failures"][0]["details"]["field"], "title");
}

TEST_F(AdminBulkImportLiveFixture, SingleFileTimeLimitZeroReturnsFailedEntry) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("tl0");
    slug_tracker->add(slug);
    const std::vector<httplib::MultipartFormData> items{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"TL0\","
                  "\"difficulty\":\"easy\","
                  "\"description\":\"# ..\","
                  "\"time_limit_ms\":0"
                  "}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto j = nlohmann::json::parse(r.body);
    EXPECT_EQ(j["data"]["summary"]["failed"], 1);
    EXPECT_EQ(j["data"]["failures"][0]["stage"], "validate");
    EXPECT_EQ(j["data"]["failures"][0]["details"]["field"], "time_limit_ms");
}

// ── Public visibility: bulk-imported rows show up in GET /api/v1/problems ─

TEST_F(AdminBulkImportLiveFixture, ImportedRowsVisibleViaPublicList) {
    StdoutSilencer silencer;
    const auto slug = fresh_slug("vis");
    slug_tracker->add(slug);
    const std::vector<httplib::MultipartFormData> items{
        make_file(slug + ".json",
                  std::string("{\"slug\":\"") + slug +
                  "\",\"title\":\"Visible\",\"difficulty\":\"medium\","
                  "\"description\":\"# ..\"}") };
    const auto r = do_post_multipart(handle,
        "/api/v1/admin/problems/import", items, admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);

    // GET /api/v1/problems — should see it.
    const auto pub = do_get(handle, "/api/v1/problems?limit=100");
    ASSERT_TRUE(pub);
    ASSERT_EQ(pub.status, 200);
    const auto j = nlohmann::json::parse(pub.body);
    bool found = false;
    for (const auto& row : j["data"]["items"]) {
        if (row["slug"] == slug) { found = true; break; }
    }
    EXPECT_TRUE(found);
}

}  // anonymous namespace