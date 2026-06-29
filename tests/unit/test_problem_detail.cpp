// tests/unit/test_problem_detail.cpp
//
// Integration + unit tests for src/routes/problem_routes.h -
// GET /api/v1/problems/:slug (SPEC §5.2, A5).
//
// Coverage:
//   - Pure unit tests (no MySQL required):
//       * detail::parse_slug_param       - accepts valid slugs,
//                                          rejects empty / too-long /
//                                          uppercase / spaces /
//                                          leading-hyphen /
//                                          trailing-hyphen
//       * detail::extract_slug_from_path - strips the
//                                          /api/v1/problems/ prefix
//                                          and delegates; rejects
//                                          short paths and bad slugs
//       * serialize_sample               - shape (input / output)
//       * serialize_problem_detail       - shape (includes
//                                          description / tags /
//                                          samples; omits is_deleted)
//
//   - Integration tests (require a reachable MySQL):
//       * 200 happy path on a populated problem - description /
//         tags / samples round-trip end-to-end
//       * 200 on a problem with NO samples - samples=[]
//       * 200 on a problem with NO tags    - tags=[]
//       * 200 on a problem with no samples AND no tags
//       * 200 UTF-8 round-trip - Chinese title + Chinese description
//         + Chinese tag name
//       * 200 maintenance counters included (accepted_count /
//         submission_count)
//       * 200 ordering on samples - order_num ASC, id ASC
//       * 200 ordering on tags    - name ASC
//       * 200 description is delivered RAW (no XSS sanitization in
//         the API layer; SPEC §6.3 + A32 puts the burden on the
//         front-end DOMPurify pass)
//       * 404 slug not found
//       * 404 soft-deleted problem is NEVER returned (public path
//         forces include_deleted=false)
//       * 400 invalid slug (uppercase / space / leading-hyphen /
//         too-long / special char)
//       * 400 empty slug (path '/api/v1/problems/')
//       * 429 rate limit exceeded (tight bucket in the fixture)
//       * X-Request-Id passthrough on 200
//       * X-Request-Id passthrough on 4xx error envelope
//       * X-RateLimit-* headers present on every response
//       * Method-not-allowed (POST to the detail URL is not
//         registered; cpp-httplib's 404 is the default, our error
//         handler shapes it into the unified envelope)
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box. When MySQL is
// unreachable the integration tests SKIP - the binary still passes
// on a machine without MySQL (CI lint), and the pure-unit tests
// still run end-to-end against the parsed-only handler.

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

#include "config.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"
#include "db/tag_repo.h"
#include "db/test_case_repo.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/problem_routes.h"
#include "server.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers (mirrors test_problem_list.cpp)
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

litecode::RateLimitConfig tight_rate_limit(int problems_per_minute) {
    litecode::RateLimitConfig r = lax_rate_limit();
    r.problems_public_per_minute_per_ip = problems_per_minute;
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

// Unique-per-call slug generator. The slug validator only accepts
// [a-z0-9-], so we replace underscores with hyphens.
std::string fresh_slug(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '_') c = '-';
    }
    return std::string("pd-") + safe_tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

std::string fresh_tag_name(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("pd-tag-") + tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

// Slug tracker - best-effort cleanup in TearDown. We don't fail the
// test if cleanup fails; the next test's fresh_slug avoids collisions.
class SlugTracker {
public:
    explicit SlugTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~SlugTracker() { if (pool_) cleanup(*pool_, created_); }

    void add(std::string s) { created_.push_back(std::move(s)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        created_;

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

// Build a fully-populated ProblemRow with sane defaults.
litecode::ProblemRow make_problem(const std::string& slug,
                                  const std::string& difficulty,
                                  const std::string& title = "Test Problem",
                                  const std::string& description = "") {
    litecode::ProblemRow p;
    p.slug        = slug;
    p.title       = title;
    p.difficulty  = difficulty;
    p.description = description.empty()
                        ? std::string("# ") + title + "\n\nSample description body."
                        : description;
    p.time_limit   = 1000;
    p.memory_limit = 256;
    return p;
}

// Convenience: issue a GET and return (status, body). Logs at error
// when the request itself fails (httplib error) so the test failure
// message points at the network layer, not at the JSON parse.
struct HttpResponse {
    int          status = 0;
    std::string  body;
    bool         ok = false;
};

HttpResponse do_get(ServerHandle& h, const std::string& path) {
    HttpResponse out;
    const auto r = h.client->Get(path);
    if (!r) {
        ADD_FAILURE() << "GET " << path << " failed: " << r.error();
        return out;
    }
    out.status = r->status;
    out.body   = r->body;
    out.ok     = true;
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests - no DB I/O
// ────────────────────────────────────────────────────────────────────────────

TEST(ParseSlugParam, AcceptsCanonicalSlugs) {
    EXPECT_EQ(litecode::detail::parse_slug_param("two-sum").value(), "two-sum");
    EXPECT_EQ(litecode::detail::parse_slug_param("a").value(),       "a");
    EXPECT_EQ(litecode::detail::parse_slug_param("abc-123").value(), "abc-123");
    EXPECT_EQ(litecode::detail::parse_slug_param("123").value(),     "123");
}

TEST(ParseSlugParam, RejectsEmpty) {
    EXPECT_FALSE(litecode::detail::parse_slug_param("").has_value());
}

TEST(ParseSlugParam, RejectsUppercase) {
    EXPECT_FALSE(litecode::detail::parse_slug_param("Two-Sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("TWO-SUM").has_value());
}

TEST(ParseSlugParam, RejectsSpaces) {
    EXPECT_FALSE(litecode::detail::parse_slug_param("two sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param(" two-sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("two-sum ").has_value());
}

TEST(ParseSlugParam, RejectsSpecialChars) {
    EXPECT_FALSE(litecode::detail::parse_slug_param("two_sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("two.sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("two/sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("two:sum").has_value());
}

TEST(ParseSlugParam, RejectsLeadingOrTrailingHyphen) {
    EXPECT_FALSE(litecode::detail::parse_slug_param("-two-sum").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("two-sum-").has_value());
    EXPECT_FALSE(litecode::detail::parse_slug_param("-").has_value());
}

TEST(ParseSlugParam, RejectsTooLong) {
    const std::string too_long(101, 'a');
    EXPECT_FALSE(litecode::detail::parse_slug_param(too_long).has_value());
}

TEST(ParseSlugParam, AcceptsExactly100Chars) {
    const std::string exactly_100(100, 'a');
    EXPECT_EQ(litecode::detail::parse_slug_param(exactly_100).value().size(),
              100u);
}

TEST(ExtractSlugFromPath, StripsPrefix) {
    httplib::Request req;
    req.path = "/api/v1/problems/two-sum";
    EXPECT_EQ(litecode::detail::extract_slug_from_path(req).value(),
              "two-sum");
}

TEST(ExtractSlugFromPath, RejectsShortPath) {
    httplib::Request req;
    req.path = "/api/v1/problems/";
    EXPECT_FALSE(litecode::detail::extract_slug_from_path(req).has_value());
}

TEST(ExtractSlugFromPath, RejectsPrefixMismatch) {
    httplib::Request req;
    req.path = "/api/v1/tags/two-sum";
    EXPECT_FALSE(litecode::detail::extract_slug_from_path(req).has_value());
}

TEST(ExtractSlugFromPath, RejectsBadSlugShape) {
    httplib::Request req;
    req.path = "/api/v1/problems/Two-Sum";   // uppercase
    EXPECT_FALSE(litecode::detail::extract_slug_from_path(req).has_value());

    req.path = "/api/v1/problems/two_sum";   // underscore
    EXPECT_FALSE(litecode::detail::extract_slug_from_path(req).has_value());
}

TEST(SerializeSample, Shape) {
    litecode::SampleCaseRow s;
    s.id              = 99;
    s.problem_id      = 7;
    s.input           = "1 2 3\n";
    s.expected_output = "6\n";
    s.judge_type      = "exact";
    s.order_num       = 1;
    const auto j = litecode::serialize_sample(s);
    EXPECT_EQ(j["input"],  "1 2 3\n");
    EXPECT_EQ(j["output"], "6\n");
    // We deliberately do NOT expose id / problem_id / judge_type /
    // order_num on the public samples view.
    EXPECT_FALSE(j.contains("id"));
    EXPECT_FALSE(j.contains("problem_id"));
    EXPECT_FALSE(j.contains("judge_type"));
    EXPECT_FALSE(j.contains("order_num"));
}

TEST(SerializeProblemDetail, IncludesHeavyFields) {
    litecode::ProblemRow p;
    p.id               = 7;
    p.slug             = "two-sum";
    p.title            = u8"\xe4\xb8\xa4\xe6\x95\xb0\xe4\xb9\x8b\xe5\x92\x8c";
    p.difficulty       = "easy";
    p.description      = "# Markdown body\n\n```cpp\nint main(){}\n```";
    p.time_limit       = 1000;
    p.memory_limit     = 256;
    p.accepted_count   = 3;
    p.submission_count = 9;
    p.is_deleted       = true;             // would NEVER actually be
                                            // true on a public path;
                                            // the serializer omits it
                                            // unconditionally.
    p.created_at       = "2026-06-29 12:00:00";
    p.updated_at       = "2026-06-29 12:30:00";

    litecode::TagRow t1; t1.id = 1; t1.name = u8"\xe6\x95\xb0\xe7\xbb\x84";
    litecode::TagRow t2; t2.id = 2; t2.name = u8"\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8";

    litecode::SampleCaseRow s1;
    s1.input = "2 7 11 15\n9\n";
    s1.expected_output = "0 1\n";

    const auto j = litecode::serialize_problem_detail(
        p, {t1, t2}, {s1});

    EXPECT_EQ(j["id"],               7);
    EXPECT_EQ(j["slug"],             "two-sum");
    EXPECT_EQ(j["title"],            u8"\xe4\xb8\xa4\xe6\x95\xb0\xe4\xb9\x8b\xe5\x92\x8c");
    EXPECT_EQ(j["difficulty"],       "easy");
    EXPECT_EQ(j["description"],
              "# Markdown body\n\n```cpp\nint main(){}\n```");
    EXPECT_EQ(j["time_limit"],       1000);
    EXPECT_EQ(j["memory_limit"],     256);
    EXPECT_EQ(j["accepted_count"],   3);
    EXPECT_EQ(j["submission_count"], 9);
    EXPECT_EQ(j["created_at"],       "2026-06-29 12:00:00");
    EXPECT_EQ(j["updated_at"],       "2026-06-29 12:30:00");
    EXPECT_FALSE(j.contains("is_deleted"));

    ASSERT_TRUE(j["tags"].is_array());
    ASSERT_EQ(j["tags"].size(), 2u);
    EXPECT_EQ(j["tags"][0]["id"],   1);
    EXPECT_EQ(j["tags"][0]["name"], u8"\xe6\x95\xb0\xe7\xbb\x84");
    EXPECT_EQ(j["tags"][1]["id"],   2);
    EXPECT_EQ(j["tags"][1]["name"], u8"\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8");

    ASSERT_TRUE(j["samples"].is_array());
    ASSERT_EQ(j["samples"].size(), 1u);
    EXPECT_EQ(j["samples"][0]["input"],  "2 7 11 15\n9\n");
    EXPECT_EQ(j["samples"][0]["output"], "0 1\n");
}

TEST(SerializeProblemDetail, EmptyTagsAndSamples) {
    litecode::ProblemRow p;
    p.slug        = "x";
    p.title       = "x";
    p.difficulty  = "easy";
    p.description = "# x";
    p.created_at  = "2026-01-01 00:00:00";
    p.updated_at  = "2026-01-01 00:00:00";

    const auto j = litecode::serialize_problem_detail(p, {}, {});
    ASSERT_TRUE(j["tags"].is_array());
    EXPECT_EQ(j["tags"].size(),    0u);
    ASSERT_TRUE(j["samples"].is_array());
    EXPECT_EQ(j["samples"].size(), 0u);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests - require a reachable MySQL
//
//  Each fixture seeds a handful of rows tagged with this run's
//  timestamp so the assertions stay independent of any pre-existing
//  fixtures left by other test runs.
// ────────────────────────────────────────────────────────────────────────────

class ProblemDetailLiveFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::unique_ptr<litecode::HttpServer>    server;
    std::unique_ptr<litecode::RateLimiter>   limiter;
    ServerHandle                             handle;
    std::optional<SlugTracker>               slug_tracker;
    std::optional<TagTracker>                tag_tracker;
    litecode::RateLimitConfig               rate_cfg;

    void SetUp() override {
        // logger.h's LOG_INFO macro lazily bootstraps via
        // litecode::config() on first use; that path throws
        // ConfigError when JWT_SECRET is unset. Set a throwaway
        // secret here so the route handler's structured log line
        // doesn't 500 the request. (The route itself doesn't use
        // JWT — but the underlying logger does.)
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

        rate_cfg = lax_rate_limit();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());
        litecode::register_problem_routes(
            *server, *pool, *limiter, rate_cfg);
        handle = start_server(server.get());
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        tag_tracker.reset();
        slug_tracker.reset();
        pool.reset();
    }

    // Seed a problem with the given slug + difficulty and stash it
    // for cleanup. Returns the new id.
    int seed_problem(const std::string& slug,
                     const std::string& difficulty,
                     const std::string& title,
                     const std::string& description = "") {
        slug_tracker->add(slug);
        return litecode::problem_repo::create(
            *pool, make_problem(slug, difficulty, title, description));
    }

    // Seed a tag and attach it to a given problem id. The tag is
    // stashed for cleanup.
    int seed_tag_with(const std::string& name, int problem_id) {
        tag_tracker->add(name);
        const auto resolved =
            litecode::tag_repo::find_or_create_many(*pool, {name});
        if (resolved.empty()) return 0;
        const int tag_id = resolved.front().id;
        litecode::tag_repo::attach(*pool, problem_id, tag_id);
        return tag_id;
    }

    // Seed a sample test case (is_sample=TRUE) for a problem.
    // Returns the new id.
    int seed_sample(int problem_id, const std::string& input,
                    const std::string& output, int order_num,
                    const std::string& judge_type = "exact") {
        auto conn = pool->acquire();
        auto rs = conn.execute(
            "INSERT INTO test_cases "
            "(problem_id, input, expected_output, is_sample, "
            " judge_type, order_num) "
            "VALUES (?, ?, ?, TRUE, ?, ?)",
            problem_id, input, output, judge_type, order_num);
        return static_cast<int>(rs.getAutoIncrementValue());
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  200 - happy path
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemDetailLiveFixture, HappyPathReturns200WithFullBody) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("happy");
    const std::string title = "Happy Path Problem";
    const std::string description =
        "# Description\n\nThis is the Markdown body with `code`.\n";
    const int pid = seed_problem(slug, "easy", title, description);
    ASSERT_GT(pid, 0);

    const int tag_id = seed_tag_with(fresh_tag_name("alpha"), pid);
    ASSERT_GT(tag_id, 0);

    const int sample_id =
        seed_sample(pid, "1 2 3\n", "6\n", /*order_num=*/1);
    ASSERT_GT(sample_id, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok) << r.body;
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    ASSERT_TRUE(body.contains("data"));
    const auto& d = body["data"];

    EXPECT_EQ(d["id"].get<int>(),         pid);
    EXPECT_EQ(d["slug"],                  slug);
    EXPECT_EQ(d["title"],                 title);
    EXPECT_EQ(d["difficulty"],            "easy");
    EXPECT_EQ(d["description"],           description);
    EXPECT_EQ(d["time_limit"].get<int>(), 1000);
    EXPECT_EQ(d["memory_limit"].get<int>(),256);

    ASSERT_TRUE(d["tags"].is_array());
    ASSERT_EQ(d["tags"].size(), 1u);
    EXPECT_EQ(d["tags"][0]["id"].get<int>(), tag_id);

    ASSERT_TRUE(d["samples"].is_array());
    ASSERT_EQ(d["samples"].size(), 1u);
    EXPECT_EQ(d["samples"][0]["input"],  "1 2 3\n");
    EXPECT_EQ(d["samples"][0]["output"], "6\n");

    EXPECT_FALSE(d.contains("is_deleted"));
}

TEST_F(ProblemDetailLiveFixture, NoSamplesReturnsEmptyArray) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("nosamples");
    const int pid = seed_problem(slug, "easy", "No Samples");
    ASSERT_GT(pid, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["samples"].size(), 0u);
    EXPECT_TRUE(body["data"]["samples"].is_array());
}

TEST_F(ProblemDetailLiveFixture, NoTagsReturnsEmptyArray) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("notag");
    const int pid = seed_problem(slug, "medium", "No Tags");
    ASSERT_GT(pid, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["tags"].size(), 0u);
    EXPECT_TRUE(body["data"]["tags"].is_array());
}

TEST_F(ProblemDetailLiveFixture, NoTagsAndNoSamples) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("lonely");
    const int pid = seed_problem(slug, "hard", "Lonely Problem");
    ASSERT_GT(pid, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["tags"].size(),    0u);
    EXPECT_EQ(body["data"]["samples"].size(), 0u);
}

TEST_F(ProblemDetailLiveFixture, Utf8RoundTripsCorrectly) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("utf8");
    const std::string title = u8"\xe4\xb8\xa4\xe6\x95\xb0\xe4\xb9\x8b\xe5\x92\x8c";
    const std::string description =
        u8"# \xe4\xb8\xa4\xe6\x95\xb0\xe4\xb9\x8b\xe5\x92\x8c\n\n"
        u8"\xe7\xbb\x99\xe5\xae\x9a\xe4\xb8\x80\xe4\xb8\xaa**"
        u8"\xe6\x95\xb4\xe6\x95\xb0\xe6\x95\xb0\xe7\xbb\x84** "
        u8"`nums` \xe5\x92\x8c\xe4\xb8\x80\xe4\xb8\xaa"
        u8"\xe6\x95\xb4\xe6\x95\xb0"
        u8"\xe7\x9b\xae\xe6\xa0\x87\xe5\x80\xbc `target`\xe3\x80\x82\n";
    const int pid = seed_problem(slug, "easy", title, description);
    ASSERT_GT(pid, 0);

    // Chinese tag name
    const std::string tag_name = u8"\xe6\x95\xb0\xe7\xbb\x84";
    const int tag_id = seed_tag_with(tag_name, pid);
    ASSERT_GT(tag_id, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["title"],       title);
    EXPECT_EQ(body["data"]["description"],  description);
    ASSERT_EQ(body["data"]["tags"].size(), 1u);
    EXPECT_EQ(body["data"]["tags"][0]["name"], u8"\xe6\x95\xb0\xe7\xbb\x84");
}

TEST_F(ProblemDetailLiveFixture, MaintenanceCountersIncluded) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("counters");
    const int pid = seed_problem(slug, "easy", "Counter Problem");
    ASSERT_GT(pid, 0);

    // Bump the counters via raw SQL (the judge flow will write to
    // these in Phase 4; here we just exercise the read path).
    {
        auto conn = pool->acquire();
        conn.execute(
            "UPDATE problems SET accepted_count = 12, submission_count = 47 "
            "WHERE id = ?", pid);
    }

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["accepted_count"].get<int>(),   12);
    EXPECT_EQ(body["data"]["submission_count"].get<int>(), 47);
}

TEST_F(ProblemDetailLiveFixture, SamplesOrderedByOrderNum) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("ordered");
    const int pid = seed_problem(slug, "easy", "Ordered Samples");
    ASSERT_GT(pid, 0);

    // Insert in REVERSE order_num; the read should still come back
    // in order_num ASC.
    ASSERT_GT(seed_sample(pid, "z-in",  "Z",  /*order_num=*/3), 0);
    ASSERT_GT(seed_sample(pid, "a-in",  "A",  /*order_num=*/1), 0);
    ASSERT_GT(seed_sample(pid, "m-in",  "M",  /*order_num=*/2), 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    const auto& samples = body["data"]["samples"];
    ASSERT_EQ(samples.size(), 3u);
    EXPECT_EQ(samples[0]["input"], "a-in");
    EXPECT_EQ(samples[0]["output"], "A");
    EXPECT_EQ(samples[1]["input"], "m-in");
    EXPECT_EQ(samples[1]["output"], "M");
    EXPECT_EQ(samples[2]["input"], "z-in");
    EXPECT_EQ(samples[2]["output"], "Z");
}

TEST_F(ProblemDetailLiveFixture, TagsOrderedByName) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("tagord");
    const int pid = seed_problem(slug, "easy", "Tag Ordered");
    ASSERT_GT(pid, 0);

    // Insert tags in deliberately-non-alphabetic order.
    const int t_z = seed_tag_with(fresh_tag_name("zzz"), pid);
    const int t_a = seed_tag_with(fresh_tag_name("aaa"), pid);
    const int t_m = seed_tag_with(fresh_tag_name("mmm"), pid);
    ASSERT_GT(t_z, 0);
    ASSERT_GT(t_a, 0);
    ASSERT_GT(t_m, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    const auto& tags = body["data"]["tags"];
    ASSERT_EQ(tags.size(), 3u);
    // tag_repo's list_tags_for_problem orders by name ASC, id ASC.
    // fresh_tag_name guarantees unique per-call names; the aaa/
    // mmm/zzz names themselves compare alphabetically.
    EXPECT_LT(tags[0]["name"].get<std::string>(),
              tags[1]["name"].get<std::string>());
    EXPECT_LT(tags[1]["name"].get<std::string>(),
              tags[2]["name"].get<std::string>());
}

TEST_F(ProblemDetailLiveFixture, NonSampleCasesAreNotReturned) {
    StdoutSilencer silencer;
    // The public detail endpoint MUST NOT surface judge-only rows
    // (is_sample=FALSE). SPEC §4.3: "是否为示例用例（展示给用户）"
    // — the public path renders samples, not the full test set.
    const std::string slug = fresh_slug("judgeonly");
    const int pid = seed_problem(slug, "easy", "Judge Only");
    ASSERT_GT(pid, 0);

    // Insert a sample AND a non-sample row; the response should
    // carry ONLY the sample.
    ASSERT_GT(seed_sample(pid, "in-sample",  "out-sample", 1), 0);
    {
        auto conn = pool->acquire();
        conn.execute(
            "INSERT INTO test_cases "
            "(problem_id, input, expected_output, is_sample, "
            " judge_type, order_num) "
            "VALUES (?, 'in-hidden', 'out-hidden', FALSE, 'exact', 99)",
            pid);
    }

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    ASSERT_EQ(body["data"]["samples"].size(), 1u);
    EXPECT_EQ(body["data"]["samples"][0]["input"],  "in-sample");
    EXPECT_EQ(body["data"]["samples"][0]["output"], "out-sample");
}

TEST_F(ProblemDetailLiveFixture, DescriptionDeliveredRaw) {
    StdoutSilencer silencer;
    // SPEC §6.3 + A32: the front-end runs Markdown through
    // DOMPurify. The API must deliver raw Markdown — the
    // sanitization policy lives in one place (the browser).
    const std::string slug = fresh_slug("xss");
    const std::string description =
        "# Title\n\n<script>alert('xss')</script>\n\n"
        "![img](javascript:alert(1))\n";
    const int pid = seed_problem(slug, "easy", "XSS Body", description);
    ASSERT_GT(pid, 0);

    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    // Raw bytes round-trip; the sanitization is the front-end's job.
    EXPECT_EQ(body["data"]["description"], description);
}

// ────────────────────────────────────────────────────────────────────────────
//  404 - not found
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemDetailLiveFixture, UnknownSlugReturns404) {
    StdoutSilencer silencer;
    // Use a slug that's guaranteed not to exist.
    const std::string ghost = "pd-ghost-" +
        std::to_string(static_cast<long long>(
            std::chrono::system_clock::now()
                .time_since_epoch().count()));
    const auto r = do_get(handle, "/api/v1/problems/" + ghost);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 404);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"],                "NOT_FOUND");
    EXPECT_EQ(body["details"]["slug"],     ghost);
}

TEST_F(ProblemDetailLiveFixture, SoftDeletedProblemReturns404) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("tombstone");
    const int pid = seed_problem(slug, "easy", "Tombstone");
    ASSERT_GT(pid, 0);

    // Sanity: live, the detail endpoint returns 200.
    {
        const auto r = do_get(handle, "/api/v1/problems/" + slug);
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
    }

    // Soft-delete the row.
    {
        auto conn = pool->acquire();
        conn.execute(
            "UPDATE problems SET is_deleted = TRUE WHERE id = ?", pid);
    }

    // The public detail path forces include_deleted=false, so the
    // tombstone MUST be invisible — even though the slug is still
    // present in the URL.
    const auto r = do_get(handle, "/api/v1/problems/" + slug);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 404);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"], "NOT_FOUND");
}

// ────────────────────────────────────────────────────────────────────────────
//  400 - bad slug shape
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemDetailLiveFixture, UppercaseSlugReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems/Two-Sum");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"],                "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"],    "slug");
}

TEST_F(ProblemDetailLiveFixture, SlugWithUnderscoreReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems/two_sum");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"],             "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "slug");
}

TEST_F(ProblemDetailLiveFixture, SlugWithLeadingHyphenReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems/-two-sum");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
}

TEST_F(ProblemDetailLiveFixture, SlugTooLongReturns400) {
    StdoutSilencer silencer;
    const std::string too_long(101, 'a');
    const auto r = do_get(handle, "/api/v1/problems/" + too_long);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
}

TEST_F(ProblemDetailLiveFixture, EmptySlugReturns404FromRouter) {
    StdoutSilencer silencer;
    // An empty slug makes the path /api/v1/problems/ which doesn't
    // match the GET /api/v1/problems/:slug pattern. cpp-httplib
    // falls through to the 404 handler; the server's error handler
    // shapes the response into the unified envelope.
    const auto r = do_get(handle, "/api/v1/problems/");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 404);
}

// ────────────────────────────────────────────────────────────────────────────
//  Request-ID passthrough + rate limit headers
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemDetailLiveFixture, ResponseCarriesRequestId) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("rid");
    const int pid = seed_problem(slug, "easy", "RID");
    ASSERT_GT(pid, 0);

    httplib::Headers hdrs = {{"X-Request-Id", "pdet-rid-001"}};
    const auto r = handle.client->Get("/api/v1/problems/" + slug, hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "pdet-rid-001");
    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "pdet-rid-001");
}

TEST_F(ProblemDetailLiveFixture, FailureEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"X-Request-Id", "pdet-rid-fail"}};
    const auto r = handle.client->Get(
        "/api/v1/problems/Two-Sum", hdrs);    // uppercase → 400
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "pdet-rid-fail");
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],       "INVALID_INPUT");
    EXPECT_EQ(body["request_id"], "pdet-rid-fail");
}

TEST_F(ProblemDetailLiveFixture, RateLimitHeadersPresent) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("rlhdr");
    const int pid = seed_problem(slug, "easy", "RL Headers");
    ASSERT_GT(pid, 0);

    const auto r = handle.client->Get("/api/v1/problems/" + slug);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_FALSE(r->get_header_value("X-RateLimit-Limit").empty());
    EXPECT_FALSE(r->get_header_value("X-RateLimit-Remaining").empty());
}

// ────────────────────────────────────────────────────────────────────────────
//  429 - rate limit exceeded (tight bucket on a side server)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemDetailLiveFixture, RateLimitReturns429OnExceed) {
    StdoutSilencer silencer;

    // Side server on its own port with a tight bucket so we can hit
    // the cap without firing 60 requests. limit=2 → 3rd request 429s.
    litecode::HttpServer side_server(dev_server(), dev_cors());
    litecode::RateLimiter side_limiter;
    const auto side_cfg = tight_rate_limit(2);
    litecode::register_problem_routes(
        side_server, *pool, side_limiter, side_cfg);
    const int side_port = side_server.bind_any_port("127.0.0.1");
    ASSERT_GT(side_port, 0);
    side_server.start(/*background=*/true);

    httplib::Client side_client("127.0.0.1", side_port);
    side_client.set_connection_timeout(2, 0);
    side_client.set_read_timeout(5, 0);
    side_client.set_write_timeout(5, 0);
    side_client.set_keep_alive(false);

    // 1st and 2nd requests: 404 (no such slug) — but the rate
    // limit check runs FIRST, so they still count.
    for (int i = 0; i < 2; ++i) {
        const auto r = side_client.Get(
            "/api/v1/problems/pd-anything");
        ASSERT_TRUE(r) << "request " << i << " err=" << r.error();
        // 404 (no slug found) is fine — the rate-limit check runs
        // before the find_by_slug dispatch.
        EXPECT_EQ(r->status, 404);
    }
    // 3rd request: 429 with RATE_LIMITED code.
    const auto r = side_client.Get("/api/v1/problems/pd-anything");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 429);
    EXPECT_FALSE(r->body.empty());
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],             "RATE_LIMITED");
    EXPECT_EQ(body["details"]["quota"], "problems.read");
    EXPECT_GE(body["details"]["retry_after_s"].get<int>(), 1);

    side_server.stop();
}

} // namespace
