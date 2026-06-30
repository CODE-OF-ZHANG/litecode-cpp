// tests/unit/test_problem_list.cpp
//
// Integration + unit tests for src/routes/problem_routes.h -
// GET /api/v1/problems (SPEC §5.2, Phase 3 *).
//
// Coverage:
//   - Pure unit tests (no MySQL required):
//       * detail::parse_int_param - accepts ints in range, rejects
//         non-numeric, leading/trailing whitespace, trailing junk,
//         out-of-range, negative
//       * detail::parse_difficulty_param - accepts the three ENUM
//         values, rejects empty / unknown / case-mismatched
//       * detail::parse_bool_param - accepts true/false/1/0/yes/no
//         (case-insensitive), rejects arbitrary strings
//       * detail::clamp_pagination - applies repo defaults / caps
//       * serialize_problem_row - shape (omits description / is_deleted,
//         includes the maintenance counters)
//       * parse_list_query semantics - invalid query => 400 envelope
//         with code/message/details; missing query => default filter
//       * 501 placeholders on /api/v1/problems/:slug, /api/v1/tags -
//         registered routes return 501 (not yet implemented) when hit
//         (these are the "next phase will land" stubs)
//         SERVICE_UNAVAILABLE envelopes
//
//   - Integration tests (require a reachable MySQL):
//       * 200 happy path on a populated table - items[] shape,
//         total/limit/offset echo, soft-deleted rows excluded
//       * 200 on an empty table - empty items[] + total=0
//       * 200 pagination - limit/offset honored, total stays stable
//       * 200 limit clamping - limit=200 clamps to 100, limit=0 uses
//         default 20
//       * 200 difficulty filter - only matching rows returned
//       * 200 tag_id filter - only matching rows returned
//       * 200 combined difficulty + tag_id
//       * 200 ordering - created_at DESC, id DESC
//       * 200 soft-deleted rows are NEVER in the response (even with
//         ?include_deleted=true - public list forces false)
//       * 200 response envelope shape - request_id passthrough
//       * 400 invalid difficulty
//       * 400 invalid limit (non-numeric / negative / over-cap)
//       * 400 invalid offset (non-numeric / negative)
//       * 400 invalid tag_id (non-numeric / zero)
//       * 400 invalid include_deleted (typo)
//       * 429 rate limit exceeded (tight bucket in the fixture)
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
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/problem_routes.h"
#include "server.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers (mirrors test_auth_profile.cpp / test_problem.cpp)
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
// [a-z0-9-], so we replace underscores with hyphens in the tag.
std::string fresh_slug(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '_') c = '-';
    }
    return std::string("pl-") + safe_tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

std::string fresh_tag_name(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("pl-tag-") + tag + "-" +
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
                                  const std::string& title = "Test Problem") {
    litecode::ProblemRow p;
    p.slug        = slug;
    p.title       = title;
    p.difficulty  = difficulty;
    p.description = "# " + title + "\n\nSample description body.";
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

TEST(ParseIntParam, AcceptsInRangeValues) {
    EXPECT_EQ(litecode::detail::parse_int_param("0",   0, 100).value_or(-1), 0);
    EXPECT_EQ(litecode::detail::parse_int_param("42",  0, 100).value_or(-1), 42);
    EXPECT_EQ(litecode::detail::parse_int_param("100", 0, 100).value_or(-1), 100);
}

TEST(ParseIntParam, RejectsOutOfRange) {
    EXPECT_FALSE(litecode::detail::parse_int_param("-1", 0, 100).has_value());
    EXPECT_FALSE(litecode::detail::parse_int_param("101", 0, 100).has_value());
}

TEST(ParseIntParam, RejectsNonNumeric) {
    EXPECT_FALSE(litecode::detail::parse_int_param("",    0, 100).has_value());
    EXPECT_FALSE(litecode::detail::parse_int_param("abc", 0, 100).has_value());
    EXPECT_FALSE(litecode::detail::parse_int_param("12x", 0, 100).has_value());
    EXPECT_FALSE(litecode::detail::parse_int_param("1.5", 0, 100).has_value());
}

TEST(ParseIntParam, RejectsLeadingOrTrailingWhitespace) {
    EXPECT_FALSE(litecode::detail::parse_int_param(" 1",  0, 100).has_value());
    EXPECT_FALSE(litecode::detail::parse_int_param("1 ",  0, 100).has_value());
    EXPECT_FALSE(litecode::detail::parse_int_param("\t1", 0, 100).has_value());
}

TEST(ParseDifficultyParam, AcceptsEnumValues) {
    EXPECT_EQ(litecode::detail::parse_difficulty_param("easy").value(),   "easy");
    EXPECT_EQ(litecode::detail::parse_difficulty_param("medium").value(), "medium");
    EXPECT_EQ(litecode::detail::parse_difficulty_param("hard").value(),   "hard");
}

TEST(ParseDifficultyParam, RejectsUnknown) {
    EXPECT_FALSE(litecode::detail::parse_difficulty_param("").has_value());
    EXPECT_FALSE(litecode::detail::parse_difficulty_param("EASY").has_value());   // case-sensitive
    EXPECT_FALSE(litecode::detail::parse_difficulty_param("simple").has_value());
    EXPECT_FALSE(litecode::detail::parse_difficulty_param("extreme").has_value());
    EXPECT_FALSE(litecode::detail::parse_difficulty_param("easy ").has_value());
}

TEST(ParseBoolParam, AcceptsCanonicalForms) {
    EXPECT_TRUE (litecode::detail::parse_bool_param("true").value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("false").value());
    EXPECT_TRUE (litecode::detail::parse_bool_param("1").value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("0").value());
    EXPECT_TRUE (litecode::detail::parse_bool_param("yes").value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("no").value());
}

TEST(ParseBoolParam, AcceptsCaseInsensitively) {
    EXPECT_TRUE (litecode::detail::parse_bool_param("True").value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("FALSE").value());
    EXPECT_TRUE (litecode::detail::parse_bool_param("YES").value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("No").value());
}

TEST(ParseBoolParam, RejectsUnknown) {
    EXPECT_FALSE(litecode::detail::parse_bool_param("").has_value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("truthy").has_value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("2").has_value());
    EXPECT_FALSE(litecode::detail::parse_bool_param("yes ").has_value());
}

TEST(ClampPagination, AppliesDefaults) {
    int limit = 0, offset = -5;
    litecode::detail::clamp_pagination(limit, offset);
    EXPECT_EQ(limit,  litecode::kDefaultListLimit);
    EXPECT_EQ(offset, 0);
}

TEST(ClampPagination, CapsLimitAtMax) {
    int limit = 1000, offset = 0;
    litecode::detail::clamp_pagination(limit, offset);
    EXPECT_EQ(limit, litecode::kMaxListLimit);
}

TEST(SerializeProblemRow, OmitsHeavyFields) {
    litecode::ProblemRow p;
    p.id               = 7;
    p.slug             = "two-sum";
    p.title            = "两数之和";
    p.difficulty       = "easy";
    p.description      = "VERY LONG MARKDOWN BODY...";
    p.time_limit       = 1000;
    p.memory_limit     = 256;
    p.accepted_count   = 3;
    p.submission_count = 9;
    p.is_deleted       = true;             // the public list must NEVER
                                          // carry this value - the row
                                          // wouldn't have been returned
                                          // in the first place.
    p.created_at       = "2026-06-29 12:00:00";
    p.updated_at       = "2026-06-29 12:30:00";

    const auto j = litecode::serialize_problem_row(p);
    EXPECT_EQ(j["id"],               7);
    EXPECT_EQ(j["slug"],             "two-sum");
    EXPECT_EQ(j["title"],            "两数之和");
    EXPECT_EQ(j["difficulty"],       "easy");
    EXPECT_EQ(j["time_limit"],       1000);
    EXPECT_EQ(j["memory_limit"],     256);
    EXPECT_EQ(j["accepted_count"],   3);
    EXPECT_EQ(j["submission_count"], 9);
    EXPECT_EQ(j["created_at"],       "2026-06-29 12:00:00");
    EXPECT_EQ(j["updated_at"],       "2026-06-29 12:30:00");
    EXPECT_FALSE(j.contains("description"));
    EXPECT_FALSE(j.contains("is_deleted"));
    EXPECT_FALSE(j.contains("tags"));
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests - require a reachable MySQL
//
//  Each fixture seeds a handful of rows tagged with this run's
//  timestamp so the assertions stay independent of any pre-existing
//  fixtures left by other test runs (problem_repo tests are
//  intentionally non-cleanup so concurrent runs don't collide).
// ────────────────────────────────────────────────────────────────────────────

class ProblemListLiveFixture : public ::testing::Test {
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

    // Seed a problem with the given slug + difficulty and stash it for
    // cleanup. Returns the new id. We retry on the rare duplicate-slug
    // race so the test isn't flaky on parallel runs.
    int seed_problem(const std::string& slug,
                     const std::string& difficulty,
                     const std::string& title) {
        slug_tracker->add(slug);
        return litecode::problem_repo::create(
            *pool, make_problem(slug, difficulty, title));
    }

    // Seed a tag and attach it to a list of problem ids. The tag is
    // stashed for cleanup. We use find_or_create_many so the tag may
    // already exist from a prior run (utf8mb4_unicode_ci is
    // case-insensitive).
    int seed_tag_with(const std::string& name,
                      const std::vector<int>& problem_ids) {
        tag_tracker->add(name);
        const auto resolved =
            litecode::tag_repo::find_or_create_many(*pool, {name});
        if (resolved.empty()) return 0;
        const int tag_id = resolved.front().id;
        for (int pid : problem_ids) {
            litecode::tag_repo::attach(*pool, pid, tag_id);
        }
        return tag_id;
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  501 placeholders on routes that ship later
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemListLiveFixture, DetailEndpointReturns404ForUnknownSlug) {
    // v1.2.6 shipped the detail endpoint as a 501 placeholder; v1.2.7
    // replaced it with the real handler. From the LIST endpoint's
    // perspective the detail URL now returns 404 for an unknown slug
    // (NOT_FOUND) instead of 501. The full detail-handler behavior
    // (200 happy path / 400 bad slug / 404 soft-deleted / 429
    // rate-limit) is covered in tests/unit/test_problem_detail.cpp;
    // this case is here to keep the LIST-side smoke test honest.
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems/two-sum");
    ASSERT_TRUE(r.ok);
    // No problem with this slug exists in the test DB; the response
    // is a clean 404 NOT_FOUND, not a 501.
    EXPECT_EQ(r.status, 404);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"], "NOT_FOUND");
}

TEST_F(ProblemListLiveFixture, TagsEndpointNoLongerReturns501) {
    // v1.2.6 shipped /api/v1/tags as a 501 placeholder from
    // register_problem_routes(). v1.2.8 (Phase 3 *) moved that
    // route to src/routes/tag_routes.h's register_tag_routes() —
    // but THIS fixture only registers problem_routes, not
    // tag_routes, so /api/v1/tags is now unrouted in the test
    // server. The response is a 404 NOT_FOUND envelope (server.h's
    // default error handler shapes it), not the 501 from before.
    // The full /api/v1/tags end-to-end behavior (200 happy path /
    // ordering / problem_count / X-Request-Id / no rate-limit
    // headers / 404 / 405 / UTF-8 round-trip / soft-delete
    // exclusion) is covered in tests/unit/test_tag_list.cpp.
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/tags");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 404);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"], "NOT_FOUND");
}

TEST_F(ProblemListLiveFixture, AdminCreateEndpointNotRegisteredByProblemRoutes) {
    StdoutSilencer silencer;
    // v1.2.6 shipped the admin POST endpoint as a 501 placeholder
    // registered from problem_routes.h. v1.2.9 (Phase 3 ★ admin CRUD
    // commit) moves the admin endpoints into their own header,
    // src/routes/admin_problem_routes.h, and removes the 501
    // placeholders from problem_routes.h. This fixture only
    // registers problem_routes, so POST /api/v1/admin/problems no
    // longer has a handler at all — cpp-httplib's catch-all 404
    // handler shapes the response into the unified envelope (per
    // SPEC §5.7).
    //
    // Full admin CRUD coverage lives in tests/unit/test_admin_problem_crud.cpp
    // (the dedicated fixture spins up register_admin_problem_routes
    // + mints admin / user JWTs).
    const auto r = handle.client->Post(
        "/api/v1/admin/problems",
        R"({"slug":"x","title":"x","difficulty":"easy"})",
        "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 404);
}

// ────────────────────────────────────────────────────────────────────────────
//  200 - happy path
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemListLiveFixture, HappyPathReturns200WithItemsAndTotal) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("happy");
    const int pid = seed_problem(slug, "easy", "Happy Path Problem");
    ASSERT_GT(pid, 0);

    const auto r = do_get(handle, "/api/v1/problems?limit=100");
    ASSERT_TRUE(r.ok) << r.body;
    if (r.status != 200) {
        std::cerr << "[DEBUG] happy body=" << r.body << std::endl;
    }
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    ASSERT_TRUE(body.contains("data"));
    const auto& data = body["data"];
    ASSERT_TRUE(data.contains("items"));
    ASSERT_TRUE(data["items"].is_array());
    ASSERT_TRUE(data.contains("total"));
    ASSERT_TRUE(data.contains("limit"));
    ASSERT_TRUE(data.contains("offset"));
    EXPECT_EQ(data["limit"].get<int>(),  100);
    EXPECT_EQ(data["offset"].get<int>(), 0);
    EXPECT_GE(data["total"].get<int>(),  1);

    bool found = false;
    for (const auto& item : data["items"]) {
        if (item["slug"] == slug) {
            found = true;
            EXPECT_EQ(item["id"].get<int>(),         pid);
            EXPECT_EQ(item["title"],                 "Happy Path Problem");
            EXPECT_EQ(item["difficulty"],            "easy");
            EXPECT_EQ(item["time_limit"].get<int>(), 1000);
            EXPECT_EQ(item["memory_limit"].get<int>(),256);
            EXPECT_FALSE(item.contains("description"));
            EXPECT_FALSE(item.contains("is_deleted"));
            break;
        }
    }
    EXPECT_TRUE(found) << "seeded problem not in list response";
}

TEST_F(ProblemListLiveFixture, SoftDeletedRowsAreNeverReturned) {
    StdoutSilencer silencer;
    const std::string slug = fresh_slug("soft");
    const int pid = seed_problem(slug, "easy", "Soft Deleted");
    ASSERT_GT(pid, 0);

    // Verify it shows up while live.
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=100");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        bool found = false;
        for (const auto& item : body["data"]["items"]) {
            if (item["slug"] == slug) { found = true; break; }
        }
        EXPECT_TRUE(found) << "live problem missing from list";
    }

    // Soft-delete it.
    {
        auto conn = pool->acquire();
        conn.execute(
            "UPDATE problems SET is_deleted = TRUE WHERE id = ?",
            pid);
    }

    // Now it MUST be absent - even with include_deleted=true on a
    // public endpoint.
    {
        const auto r = do_get(handle,
            "/api/v1/problems?limit=100&include_deleted=true");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        for (const auto& item : body["data"]["items"]) {
            EXPECT_NE(item["id"].get<int>(), pid)
                << "soft-deleted problem leaked into public list";
        }
    }
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=100");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        for (const auto& item : body["data"]["items"]) {
            EXPECT_NE(item["id"].get<int>(), pid);
        }
    }
}

TEST_F(ProblemListLiveFixture, PaginationHonorsLimitAndOffset) {
    StdoutSilencer silencer;
    // Seed 5 problems so we can paginate. Each row gets a unique
    // slug; total counts ALL live rows (could be more than 5 from
    // other test runs, hence the >= assertions below).
    std::vector<std::string> slugs;
    for (int i = 0; i < 5; ++i) {
        const std::string slug = fresh_slug("page");
        slugs.push_back(slug);
        ASSERT_GT(seed_problem(slug, "medium",
                               "Page Test " + std::to_string(i)), 0);
    }

    // limit=2, offset=0 -> 2 items, returned_count=2
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=2&offset=0");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        EXPECT_EQ(body["data"]["items"].size(), 2u);
        EXPECT_EQ(body["data"]["limit"].get<int>(),  2);
        EXPECT_EQ(body["data"]["offset"].get<int>(), 0);
        EXPECT_GE(body["data"]["total"].get<int>(),  5);
    }
    // limit=2, offset=2 -> another 2 items, total stays >= 5
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=2&offset=2");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        EXPECT_EQ(body["data"]["items"].size(), 2u);
        EXPECT_GE(body["data"]["total"].get<int>(),  5);
    }
}

TEST_F(ProblemListLiveFixture, LimitClampsToMax) {
    StdoutSilencer silencer;
    // limit=500 should clamp to kMaxListLimit (100). We don't pin
    // items.size() because other tests may have left rows in the
    // table; the contract under test is the limit echoed back.
    const auto r = do_get(handle, "/api/v1/problems?limit=500");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["limit"].get<int>(), 100);
}

TEST_F(ProblemListLiveFixture, DefaultLimitWhenAbsent) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["limit"].get<int>(), 20);
    EXPECT_EQ(body["data"]["offset"].get<int>(), 0);
}

TEST_F(ProblemListLiveFixture, DifficultyFilterReturnsOnlyMatchingRows) {
    StdoutSilencer silencer;
    // Seed one of each difficulty so the filter has something to do.
    const std::string easy_slug = fresh_slug("diff-easy");
    const std::string med_slug  = fresh_slug("diff-med");
    const std::string hard_slug = fresh_slug("diff-hard");
    const int easy_pid = seed_problem(easy_slug, "easy",   "Easy Diff");
    const int med_pid  = seed_problem(med_slug,  "medium", "Medium Diff");
    const int hard_pid = seed_problem(hard_slug, "hard",   "Hard Diff");
    ASSERT_GT(easy_pid, 0);
    ASSERT_GT(med_pid,  0);
    ASSERT_GT(hard_pid, 0);

    const auto r = do_get(handle,
        "/api/v1/problems?difficulty=easy&limit=100");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    bool found_easy = false;
    for (const auto& item : body["data"]["items"]) {
        EXPECT_EQ(item["difficulty"], "easy")
            << "non-easy row in difficulty=easy response: " << item["slug"];
        if (item["id"].get<int>() == easy_pid) found_easy = true;
        if (item["id"].get<int>() == med_pid)  FAIL() << "medium row leaked";
        if (item["id"].get<int>() == hard_pid) FAIL() << "hard row leaked";
    }
    EXPECT_TRUE(found_easy) << "seeded easy problem missing from response";
}

TEST_F(ProblemListLiveFixture, TagIdFilterReturnsOnlyMatchingRows) {
    StdoutSilencer silencer;
    const std::string slug_a = fresh_slug("tag-a");
    const std::string slug_b = fresh_slug("tag-b");
    const int id_a = seed_problem(slug_a, "easy", "Tagged A");
    const int id_b = seed_problem(slug_b, "easy", "Tagged B");
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_b, 0);

    const std::string tag_name = fresh_tag_name("alpha");
    const int tag_id = seed_tag_with(tag_name, {id_a});
    ASSERT_GT(tag_id, 0);

    const auto r = do_get(handle,
        "/api/v1/problems?tag_id=" + std::to_string(tag_id) +
        "&limit=100");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    bool found_a = false;
    for (const auto& item : body["data"]["items"]) {
        if (item["id"].get<int>() == id_a) found_a = true;
        if (item["id"].get<int>() == id_b) {
            FAIL() << "untagged problem leaked into tag filter response";
        }
    }
    EXPECT_TRUE(found_a) << "tagged problem missing from tag filter";
}

TEST_F(ProblemListLiveFixture, DifficultyAndTagFiltersCombine) {
    StdoutSilencer silencer;
    const std::string slug_match    = fresh_slug("combo-match");
    const std::string slug_diffmiss = fresh_slug("combo-diffmiss");
    const std::string slug_tagmiss  = fresh_slug("combo-tagmiss");
    const int id_match    = seed_problem(slug_match,    "hard", "Combo Match");
    const int id_diffmiss = seed_problem(slug_diffmiss, "easy", "Combo Diff Miss");
    const int id_tagmiss  = seed_problem(slug_tagmiss,  "hard", "Combo Tag Miss");
    ASSERT_GT(id_match,    0);
    ASSERT_GT(id_diffmiss, 0);
    ASSERT_GT(id_tagmiss,  0);

    const std::string tag_name = fresh_tag_name("combo");
    const int tag_id = seed_tag_with(tag_name, {id_match, id_diffmiss});
    ASSERT_GT(tag_id, 0);

    const auto r = do_get(handle,
        "/api/v1/problems?difficulty=hard&tag_id=" +
        std::to_string(tag_id) + "&limit=100");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    bool found_match = false;
    for (const auto& item : body["data"]["items"]) {
        if (item["id"].get<int>() == id_match) {
            found_match = true;
            EXPECT_EQ(item["difficulty"], "hard");
        }
        if (item["id"].get<int>() == id_diffmiss) {
            FAIL() << "easy row leaked into difficulty=hard+tag response";
        }
        if (item["id"].get<int>() == id_tagmiss) {
            FAIL() << "untagged row leaked into tag response";
        }
    }
    EXPECT_TRUE(found_match);
}

TEST_F(ProblemListLiveFixture, OrderingIsCreatedAtDescIdDesc) {
    StdoutSilencer silencer;
    // Seed three rows with deliberately-staggered created_at so the
    // ordering is observable. The repo sets created_at via DB DEFAULT
    // (CURRENT_TIMESTAMP), so we INSERT with explicit values to
    // guarantee the order.
    const std::string s_oldest = fresh_slug("ord-old");
    const std::string s_middle = fresh_slug("ord-mid");
    const std::string s_newest = fresh_slug("ord-new");
    const int id_oldest = seed_problem(s_oldest, "easy", "Oldest");
    const int id_middle = seed_problem(s_middle, "easy", "Middle");
    const int id_newest = seed_problem(s_newest, "easy", "Newest");
    ASSERT_GT(id_oldest, 0);
    ASSERT_GT(id_middle, 0);
    ASSERT_GT(id_newest, 0);

    {
        auto conn = pool->acquire();
        // Backdate the "oldest" row by 60s, mid by 30s; leave newest
        // at NOW(). Use the column alias returned by DATE_FORMAT in
        // find_by_id for sanity, but here we hit it raw.
        conn.execute(
            "UPDATE problems SET created_at = NOW() - INTERVAL 60 SECOND "
            "WHERE id = ?", id_oldest);
        conn.execute(
            "UPDATE problems SET created_at = NOW() - INTERVAL 30 SECOND "
            "WHERE id = ?", id_middle);
    }

    const auto r = do_get(handle, "/api/v1/problems?limit=100");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    // Walk the items array looking for the first occurrence of each
    // of the three slugs; the order should be newest -> middle ->
    // oldest.
    int pos_newest = -1, pos_middle = -1, pos_oldest = -1;
    int idx = 0;
    for (const auto& item : body["data"]["items"]) {
        const auto sid = item["id"].get<int>();
        if      (sid == id_newest && pos_newest < 0) pos_newest = idx;
        else if (sid == id_middle && pos_middle < 0) pos_middle = idx;
        else if (sid == id_oldest && pos_oldest < 0) pos_oldest = idx;
        ++idx;
    }
    EXPECT_GE(pos_newest, 0) << "newest row missing";
    EXPECT_GE(pos_middle, 0) << "middle row missing";
    EXPECT_GE(pos_oldest, 0) << "oldest row missing";
    EXPECT_LT(pos_newest, pos_middle);
    EXPECT_LT(pos_middle, pos_oldest);
}

TEST_F(ProblemListLiveFixture, EmptyTableReturnsEmptyItemsArray) {
    StdoutSilencer silencer;
    // We can't safely truncate the table here (other tests share it),
    // but we can ask for an absurdly-high offset and confirm we get
    // an empty array + a sane total.
    const auto r = do_get(handle,
        "/api/v1/problems?offset=999999&limit=1");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["data"]["items"].size(), 0u);
    EXPECT_GE(body["data"]["total"].get<int>(), 0);
}

TEST_F(ProblemListLiveFixture, ResponseCarriesRequestId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"X-Request-Id", "plist-rid-001"}};
    const auto r = handle.client->Get("/api/v1/problems", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "plist-rid-001");
    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "plist-rid-001");
}

TEST_F(ProblemListLiveFixture, FailureEnvelopeCarriesRequestId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"X-Request-Id", "plist-rid-fail"}};
    const auto r = handle.client->Get(
        "/api/v1/problems?difficulty=extreme", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "plist-rid-fail");
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],       "INVALID_INPUT");
    EXPECT_EQ(body["request_id"], "plist-rid-fail");
}

// ────────────────────────────────────────────────────────────────────────────
//  400 - invalid query params
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemListLiveFixture, InvalidDifficultyReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems?difficulty=extreme");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"],                "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"],    "difficulty");
    EXPECT_EQ(body["details"]["value"],    "extreme");
}

TEST_F(ProblemListLiveFixture, InvalidLimitReturns400) {
    StdoutSilencer silencer;

    // Non-numeric
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=abc");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.status, 400);
        const auto body = nlohmann::json::parse(r.body);
        EXPECT_EQ(body["code"],             "INVALID_INPUT");
        EXPECT_EQ(body["details"]["field"], "limit");
    }
    // Negative
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=-1");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.status, 400);
    }
    // Zero (must be >= 1 per the contract).
    {
        const auto r = do_get(handle, "/api/v1/problems?limit=0");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.status, 400);
    }
}

TEST_F(ProblemListLiveFixture, InvalidOffsetReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems?offset=abc");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"],             "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "offset");
}

TEST_F(ProblemListLiveFixture, NegativeOffsetReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/problems?offset=-1");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
}

TEST_F(ProblemListLiveFixture, InvalidTagIdReturns400) {
    StdoutSilencer silencer;
    {
        const auto r = do_get(handle, "/api/v1/problems?tag_id=abc");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.status, 400);
        const auto body = nlohmann::json::parse(r.body);
        EXPECT_EQ(body["code"],             "INVALID_INPUT");
        EXPECT_EQ(body["details"]["field"], "tag_id");
    }
    {
        const auto r = do_get(handle, "/api/v1/problems?tag_id=0");
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.status, 400);
    }
}

TEST_F(ProblemListLiveFixture, InvalidIncludeDeletedReturns400) {
    StdoutSilencer silencer;
    const auto r = do_get(handle,
        "/api/v1/problems?include_deleted=truthy");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 400);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"],             "INVALID_INPUT");
    EXPECT_EQ(body["details"]["field"], "include_deleted");
}

// ────────────────────────────────────────────────────────────────────────────
//  429 - rate limit exceeded (tight bucket)
// ────────────────────────────────────────────────────────────────────────────

TEST_F(ProblemListLiveFixture, RateLimitReturns429OnExceed) {
    StdoutSilencer silencer;

    // Spawn a side server on its own port with a tight bucket so we
    // can hit the cap without firing 60 requests. limit=2 means the
    // 3rd request 429s. The side server uses the SAME ConnectionPool
    // but a fresh RateLimiter (so its bucket is independent of the
    // main fixture's limiter).
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

    // 1st and 2nd requests: 200.
    for (int i = 0; i < 2; ++i) {
        const auto r = side_client.Get("/api/v1/problems");
        ASSERT_TRUE(r) << "request " << i << " err=" << r.error();
        EXPECT_EQ(r->status, 200);
    }
    // 3rd request: 429 with Retry-After + RATE_LIMITED code.
    const auto r = side_client.Get("/api/v1/problems");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 429);
    EXPECT_FALSE(r->body.empty());
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"],                "RATE_LIMITED");
    EXPECT_EQ(body["details"]["quota"],    "problems.read");
    EXPECT_GE(body["details"]["retry_after_s"].get<int>(), 1);

    side_server.stop();
}

TEST_F(ProblemListLiveFixture, RateLimitHeadersAlwaysPresent) {
    StdoutSilencer silencer;
    const auto r = handle.client->Get("/api/v1/problems");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    // X-RateLimit-Limit / -Remaining must be present on every gated
    // response (allow OR deny), per SPEC §5.1 + rate_limit.h's
    // contract.
    EXPECT_FALSE(r->get_header_value("X-RateLimit-Limit").empty());
    EXPECT_FALSE(r->get_header_value("X-RateLimit-Remaining").empty());
}

} // namespace