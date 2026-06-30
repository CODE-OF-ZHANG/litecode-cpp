// tests/unit/test_tag_list.cpp
//
// Integration + unit tests for src/routes/tag_routes.h -
// GET /api/v1/tags (SPEC §5.2, Phase 3 *).
//
// Coverage:
//   - Pure unit tests (no MySQL required):
//       * serialize_tag_with_count - shape (id / name / problem_count),
//         omits timestamps / internal fields
//
//   - Integration tests (require a reachable MySQL):
//       * 200 happy path on a populated tags table - items[] shape,
//         total matches the underlying repo call, request_id
//         passthrough
//       * 200 on a freshly-seeded tag - the new tag appears in
//         items[] with the right name + problem_count
//       * 200 ordering - name ASC (matches repo contract)
//       * 200 problem_count reflects live problems only -
//         soft-deleted problems do NOT contribute to the count
//       * 200 UTF-8 round-trip - Chinese tag name (e.g.
//         "\xe6\x95\xb0\xe7\xbb\x84" / "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"
//         per SPEC §4.2b) round-trips intact
//       * 200 no rate-limit headers (X-RateLimit-Limit /
//         X-RateLimit-Remaining are absent — SPEC §5.2 puts no
//         quota on /api/v1/tags)
//       * 200 X-Request-Id echo on success and failure envelopes
//       * 404 NOT_FOUND for unrouted paths under /api/v1/tags/*
//         (the server's default error handler shapes them into the
//         unified envelope)
//       * Method-not-allowed (POST to /api/v1/tags - no route
//         registered; cpp-httplib's 404 with our error handler
//         envelope)
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
#include <set>
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
#include "routes/tag_routes.h"
#include "server.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers (mirrors test_problem_list.cpp / test_problem_detail.cpp)
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

// Unique-per-call tag name generator. The tag name validator
// accepts UTF-8 but rejects control chars and whitespace at the
// edges; we use a stable, collision-free ASCII prefix + timestamp
// + sequence suffix that survives utf8mb4_unicode_ci round-trips.
std::string fresh_tag_name(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("tl-tag-") + tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

// Unique-per-call slug generator. The slug validator only accepts
// [a-z0-9-], so we replace underscores with hyphens in the tag.
std::string fresh_slug(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '_') c = '-';
    }
    return std::string("tl-") + safe_tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

// Tag tracker - best-effort cleanup in TearDown. We delete
// problem_tags FK rows first (via the tag_id lookup), then the
// tag row itself. We don't fail the test if cleanup fails; the
// next test's fresh_tag_name avoids collisions.
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

// Slug tracker - mirrors test_problem_list.cpp's SlugTracker.
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

TEST(SerializeTagWithCount, IncludesIdNameProblemCount) {
    litecode::TagWithCount twc;
    twc.tag.id          = 42;
    twc.tag.name        = "Hash";
    twc.problem_count   = 7;
    const auto j = litecode::serialize_tag_with_count(twc);
    EXPECT_EQ(j["id"].get<int>(),            42);
    EXPECT_EQ(j["name"],                       "Hash");
    EXPECT_EQ(j["problem_count"].get<int>(), 7);
    // No timestamps or internal fields leak.
    EXPECT_FALSE(j.contains("created_at"));
    EXPECT_FALSE(j.contains("updated_at"));
    EXPECT_FALSE(j.contains("is_deleted"));
}

TEST(SerializeTagWithCount, Utf8RoundTrip) {
    litecode::TagWithCount twc;
    twc.tag.id        = 1;
    twc.tag.name      = "\xe6\x95\xb0\xe7\xbb\x84";      // 数组
    twc.problem_count = 3;
    const auto j = litecode::serialize_tag_with_count(twc);
    EXPECT_EQ(j["name"], "\xe6\x95\xb0\xe7\xbb\x84");
    EXPECT_EQ(j["problem_count"].get<int>(), 3);
}

TEST(SerializeTagWithCount, ZeroCountIsKept) {
    // A brand-new tag with no problems must still appear in the
    // response with problem_count=0, NOT omitted.
    litecode::TagWithCount twc;
    twc.tag.id        = 99;
    twc.tag.name      = "NewTag";
    twc.problem_count = 0;
    const auto j = litecode::serialize_tag_with_count(twc);
    EXPECT_EQ(j["problem_count"].get<int>(), 0);
    EXPECT_TRUE(j.contains("problem_count"));
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests - require a reachable MySQL
//
//  The fixture seeds a handful of throwaway tags + problems with
//  this run's timestamp so the assertions stay independent of any
//  pre-existing rows left by other test runs (tag_repo / problem_repo
//  tests are intentionally non-cleanup so concurrent runs don't
//  collide).
//
//  Total-row-count assertions are not pinned; we instead compare
//  the API response against `litecode::tag_repo::list_with_count`
//  directly (both queries see the same DB state, so they must
//  agree exactly).
// ────────────────────────────────────────────────────────────────────────────

class TagListLiveFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::unique_ptr<litecode::HttpServer>    server;
    std::unique_ptr<litecode::RateLimiter>   limiter;
    ServerHandle                             handle;
    std::optional<TagTracker>                tag_tracker;
    std::optional<SlugTracker>               slug_tracker;
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
        tag_tracker.emplace(pool.get());
        slug_tracker.emplace(pool.get());

        rate_cfg = lax_rate_limit();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());
        litecode::register_tag_routes(
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

    // Seed a tag via the public repo. Returns the new id (>0) on
    // success. We don't pre-check name_exists() because the repo's
    // INSERT IGNORE path is what the production admin create uses
    // anyway, and uniqueness conflicts are surfaced through
    // TagAlreadyExistsError.
    int seed_tag(const std::string& name) {
        tag_tracker->add(name);
        const auto resolved =
            litecode::tag_repo::find_or_create_many(*pool, {name});
        if (resolved.empty()) return 0;
        return resolved.front().id;
    }

    // Seed a problem (slug + difficulty + title). Returns the new id.
    int seed_problem(const std::string& slug,
                     const std::string& difficulty,
                     const std::string& title) {
        slug_tracker->add(slug);
        return litecode::problem_repo::create(
            *pool, make_problem(slug, difficulty, title));
    }

    // Attach a tag_id to a problem_id. Best-effort; the FK is
    // INSERT IGNORE in the repo.
    void attach_tag(int problem_id, int tag_id) {
        litecode::tag_repo::attach(*pool, problem_id, tag_id);
    }

    // Direct DB write to soft-delete a problem (the public
    // soft-delete endpoint isn't shipped yet). We use this to
    // exercise the "soft-deleted problems excluded from
    // problem_count" contract.
    void soft_delete_problem(int problem_id) {
        auto conn = pool->acquire();
        conn.execute(
            "UPDATE problems SET is_deleted = TRUE WHERE id = ?",
            problem_id);
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  200 - happy path
// ────────────────────────────────────────────────────────────────────────────

TEST_F(TagListLiveFixture, HappyPathReturns200WithItemsAndTotal) {
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/tags");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);
    ASSERT_TRUE(body.contains("data"));
    const auto& data = body["data"];
    ASSERT_TRUE(data.contains("items"));
    ASSERT_TRUE(data["items"].is_array());
    ASSERT_TRUE(data.contains("total"));
    EXPECT_EQ(data["items"].size(),
              static_cast<std::size_t>(data["total"].get<int>()));

    // Cross-check against the repo directly. Both queries see the
    // same DB state at the same instant, so the totals must match
    // exactly (defends against hidden filters or pagination in the
    // route that diverged from the repo contract).
    const auto repo_rows = litecode::tag_repo::list_with_count(*pool, true);
    EXPECT_EQ(data["total"].get<int>(), static_cast<int>(repo_rows.size()));
    EXPECT_EQ(data["items"].size(),     repo_rows.size());
}

TEST_F(TagListLiveFixture, NewlySeededTagAppearsInResponse) {
    StdoutSilencer silencer;
    const std::string name = fresh_tag_name("newtag");
    const int tag_id = seed_tag(name);
    ASSERT_GT(tag_id, 0);

    const auto r = do_get(handle, "/api/v1/tags");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    bool found = false;
    for (const auto& item : body["data"]["items"]) {
        if (item["id"].get<int>() == tag_id) {
            found = true;
            EXPECT_EQ(item["name"], name);
            EXPECT_EQ(item["problem_count"].get<int>(), 0);
            EXPECT_TRUE(item.contains("id"));
            EXPECT_TRUE(item.contains("name"));
            EXPECT_TRUE(item.contains("problem_count"));
            break;
        }
    }
    EXPECT_TRUE(found) << "seeded tag missing from response";
}

TEST_F(TagListLiveFixture, NamesAreOrderedAscending) {
    StdoutSilencer silencer;
    // Seed a few tags that the existing table is unlikely to have
    // already (the "tl-tag-order-..." prefix combined with a fresh
    // timestamp is collision-safe across concurrent test runs).
    const std::string n1 = fresh_tag_name("ord-aaa");
    const std::string n2 = fresh_tag_name("ord-zzz");
    const std::string n3 = fresh_tag_name("ord-mmm");
    ASSERT_GT(seed_tag(n1), 0);
    ASSERT_GT(seed_tag(n2), 0);
    ASSERT_GT(seed_tag(n3), 0);

    const auto r = do_get(handle, "/api/v1/tags");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    // Walk items and assert non-decreasing order on `name`. We use
    // string comparison (the repo's contract is ORDER BY name ASC,
    // id ASC; utf8mb4_unicode_ci gives a deterministic ordering
    // that's stable enough for the assertion below).
    const auto& items = body["data"]["items"];
    ASSERT_GE(items.size(), 3u);
    std::string prev;
    for (const auto& item : items) {
        const std::string cur = item["name"];
        if (!prev.empty()) {
            EXPECT_LE(prev, cur)
                << "name ordering violated at: '" << prev << "' > '" << cur << "'";
        }
        prev = cur;
    }
}

TEST_F(TagListLiveFixture, ProblemCountReflectsAttachedLiveProblems) {
    StdoutSilencer silencer;
    // Seed a tag and attach it to 3 live problems; problem_count
    // for that tag must equal 3.
    const std::string tag_name = fresh_tag_name("count-live");
    const int tag_id = seed_tag(tag_name);
    ASSERT_GT(tag_id, 0);

    std::vector<int> problem_ids;
    for (int i = 0; i < 3; ++i) {
        const int pid = seed_problem(fresh_slug("count-live"),
                                     "easy",
                                     "Count Live " + std::to_string(i));
        ASSERT_GT(pid, 0);
        problem_ids.push_back(pid);
        attach_tag(pid, tag_id);
    }

    const auto r = do_get(handle, "/api/v1/tags");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    int observed_count = -1;
    for (const auto& item : body["data"]["items"]) {
        if (item["id"].get<int>() == tag_id) {
            observed_count = item["problem_count"].get<int>();
            break;
        }
    }
    EXPECT_EQ(observed_count, 3)
        << "tag's problem_count must equal the number of attached LIVE problems";
}

TEST_F(TagListLiveFixture, SoftDeletedProblemsAreExcludedFromCount) {
    StdoutSilencer silencer;
    // Seed a tag and attach it to 2 problems; soft-delete one of
    // them; the live-only count for that tag must be 1.
    const std::string tag_name = fresh_tag_name("count-soft");
    const int tag_id = seed_tag(tag_name);
    ASSERT_GT(tag_id, 0);

    const int pid_live = seed_problem(fresh_slug("soft-live"),
                                      "easy", "Soft Live");
    const int pid_dead = seed_problem(fresh_slug("soft-dead"),
                                      "easy", "Soft Dead");
    ASSERT_GT(pid_live, 0);
    ASSERT_GT(pid_dead, 0);
    attach_tag(pid_live, tag_id);
    attach_tag(pid_dead, tag_id);

    // Sanity: before soft-delete, the count should be 2.
    {
        const auto r = do_get(handle, "/api/v1/tags");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        int observed_count = -1;
        for (const auto& item : body["data"]["items"]) {
            if (item["id"].get<int>() == tag_id) {
                observed_count = item["problem_count"].get<int>();
                break;
            }
        }
        EXPECT_EQ(observed_count, 2);
    }

    // Soft-delete one problem.
    soft_delete_problem(pid_dead);

    // Now the live-only count must be 1.
    {
        const auto r = do_get(handle, "/api/v1/tags");
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.status, 200);
        const auto body = nlohmann::json::parse(r.body);
        int observed_count = -1;
        for (const auto& item : body["data"]["items"]) {
            if (item["id"].get<int>() == tag_id) {
                observed_count = item["problem_count"].get<int>();
                break;
            }
        }
        EXPECT_EQ(observed_count, 1)
            << "soft-deleted problems must NOT contribute to problem_count";
    }
}

TEST_F(TagListLiveFixture, Utf8TagNameRoundTrips) {
    StdoutSilencer silencer;
    // SPEC §4.2b explicitly calls out Chinese names like
    // "\xe6\x95\xb0\xe7\xbb\x84" and "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8".
    // We seed one of each (with a unique-per-run prefix so a
    // collision with prior runs doesn't matter) and assert the
    // bytes round-trip exactly through the API.
    const std::string cn1 = "\xe6\x95\xb0\xe7\xbb\x84-" + fresh_tag_name("utf8-1");
    const std::string cn2 = "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8-" + fresh_tag_name("utf8-2");
    const int id1 = seed_tag(cn1);
    const int id2 = seed_tag(cn2);
    ASSERT_GT(id1, 0);
    ASSERT_GT(id2, 0);

    const auto r = do_get(handle, "/api/v1/tags");
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.status, 200);
    const auto body = nlohmann::json::parse(r.body);

    std::set<int> seen_ids;
    for (const auto& item : body["data"]["items"]) {
        const int id = item["id"].get<int>();
        if (id == id1) {
            EXPECT_EQ(item["name"], cn1);
            seen_ids.insert(id);
        } else if (id == id2) {
            EXPECT_EQ(item["name"], cn2);
            seen_ids.insert(id);
        }
    }
    EXPECT_EQ(seen_ids.size(), 2u)
        << "Chinese-named tags missing or malformed in response";
}

// ────────────────────────────────────────────────────────────────────────────
//  200 - response envelope shape / maintenance
// ────────────────────────────────────────────────────────────────────────────

TEST_F(TagListLiveFixture, ResponseCarriesRequestId) {
    StdoutSilencer silencer;
    httplib::Headers hdrs = {{"X-Request-Id", "tlist-rid-001"}};
    const auto r = handle.client->Get("/api/v1/tags", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "tlist-rid-001");
    const auto body = nlohmann::json::parse(r->body);
    ASSERT_TRUE(body.contains("request_id"));
    EXPECT_EQ(body["request_id"], "tlist-rid-001");
}

TEST_F(TagListLiveFixture, NoRateLimitHeadersOnTagList) {
    // SPEC §5.2 puts NO quota on /api/v1/tags. The route handler
    // does NOT call consume_rate_limit(), so X-RateLimit-Limit /
    // X-RateLimit-Remaining MUST be absent. (compare against the
    // problem_list endpoint, which DOES call consume_rate_limit
    // and ships X-RateLimit-* headers — see test_problem_list.cpp.)
    StdoutSilencer silencer;
    const auto r = handle.client->Get("/api/v1/tags");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(r->get_header_value("X-RateLimit-Limit").empty())
        << "tag_list is ungated per SPEC §5.2; X-RateLimit-Limit must be absent";
    EXPECT_TRUE(r->get_header_value("X-RateLimit-Remaining").empty())
        << "tag_list is ungated per SPEC §5.2; X-RateLimit-Remaining must be absent";
}

TEST_F(TagListLiveFixture, RetryAfterHeaderAbsent) {
    StdoutSilencer silencer;
    const auto r = handle.client->Get("/api/v1/tags");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(r->get_header_value("Retry-After").empty());
}

// ────────────────────────────────────────────────────────────────────────────
//  404 - unrouted paths / methods
// ────────────────────────────────────────────────────────────────────────────

TEST_F(TagListLiveFixture, TagsSlugPathReturns404) {
    // /api/v1/tags/123 is not registered (the spec only lists
    // GET /api/v1/tags). The server's default error handler
    // shapes it into the unified envelope with NOT_FOUND.
    StdoutSilencer silencer;
    const auto r = do_get(handle, "/api/v1/tags/123");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.status, 404);
    const auto body = nlohmann::json::parse(r.body);
    EXPECT_EQ(body["code"], "NOT_FOUND");
}

TEST_F(TagListLiveFixture, PostToTagsReturns404) {
    // POST /api/v1/tags is not registered. cpp-httplib's default
    // 405 (Method Not Allowed) is folded into our error handler
    // envelope; the route table has no POST handler for this URL.
    StdoutSilencer silencer;
    const auto r = handle.client->Post(
        "/api/v1/tags", "", "application/json");
    ASSERT_TRUE(r);
    // cpp-httplib's default behavior for an unrouted method on an
    // otherwise-routed path is 404 ("Not Found") rather than 405.
    // Either way the body must be the unified envelope.
    EXPECT_GE(r->status, 400);
    EXPECT_FALSE(r->body.empty());
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_TRUE(body.contains("code"));
    EXPECT_TRUE(body.contains("message"));
    EXPECT_TRUE(body.contains("request_id"));
}

TEST_F(TagListLiveFixture, OptionsToTagsReturnsCorsResponse) {
    // CORS preflight. The server's pre_routing handler / cors
    // configuration owns this; the tag_list endpoint itself is
    // GET-only. We don't pin the exact status (204 without an
    // Origin header can come back as 403, depending on the cors
    // config; either way the response is NOT a 404 to the
    // underlying GET handler). The relevant contract under test is
    // "OPTIONS does not leak the GET handler's body shape".
    StdoutSilencer silencer;
    const auto r = handle.client->Options("/api/v1/tags");
    ASSERT_TRUE(r);
    // 2xx (preflight accepted), 403 (no Origin header), or
    // 405/404 (method not allowed) are all acceptable here; the
    // failure mode we're guarding against is a 200 with the
    // GET handler's JSON body.
    EXPECT_NE(r->status, 200);
    EXPECT_FALSE(r->body.empty());
    // Body should NOT be a JSON envelope from the tag_list handler
    // (no `"data"` key, no items array).
    const auto body = nlohmann::json::parse(r->body, nullptr, false);
    const bool looks_like_envelope =
        body.is_object() &&
        (body.contains("data") || body.contains("items"));
    EXPECT_FALSE(looks_like_envelope)
        << "OPTIONS should not surface the GET handler's body shape";
}

} // namespace