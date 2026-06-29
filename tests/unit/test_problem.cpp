// tests/unit/test_problem.cpp
//
// Unit + light integration tests for src/db/problem_repo.h.
//
// Two layers, matching the test_user_repo / test_connection_pool pattern:
//
//   1) Pure unit tests (no MySQL required):
//        - validate_slug shape, length, charset rules
//        - validate_difficulty accepts the three ENUM values, rejects
//          everything else
//        - validate_title length bounds
//        - validate_time_limit / validate_memory_limit numeric ranges
//        - is_valid_slug_char ASCII predicate
//        - is_valid_difficulty predicate
//        - clamp_list_filter pagination clamping
//        - kMin/kMax length & limit constants pin SPEC §4.2 contract
//        - exception type hierarchy (ProblemNotFoundError /
//          ProblemAlreadyExistsError are both ProblemRepoError)
//
//   2) Integration tests (require a reachable MySQL):
//        - create inserts a row, returns the new id
//        - create returns 0 on duplicate slug (409 path)
//        - create with default time_limit / memory_limit honors 1000 / 256
//        - slug_exists round-trip (default vs include_deleted)
//        - find_by_id / find_by_slug round-trip the inserted row
//        - find_by_* returns nullopt for an unknown problem
//        - find_by_* hides soft-deleted rows by default but exposes
//          them when include_deleted=true
//        - update modifies every mutable column; renaming works
//        - update throws ProblemNotFoundError on a non-existent slug
//        - update throws ProblemAlreadyExistsError when renaming into
//          another row's slug
//        - soft_delete flips is_deleted; the row is hidden from
//          default finds; restore brings it back
//        - soft_delete is idempotent (second call returns false)
//        - restore is a no-op on a live row (returns false)
//        - list returns paginated, filterable rows; total matches count
//        - list with include_deleted=true returns tombstones
//        - list pagination clamps limit to kMaxListLimit
//        - list ordering: created_at DESC, id DESC
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box (root/123456/litecode@127.0.0.1).
// If ping() fails, those tests are SKIPPED — the binary still passes on a
// machine without MySQL, which is what we want for CI lint jobs.
//
// Each integration test uses a unique slug (timestamp + counter) to
// dodge collisions across parallel test runs. Rows are left in place
// on purpose — the tests don't own the lifecycle and "cleanup on
// failure" would mask any real FK issue that surfaces later.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "config.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"

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

class DbFixture : public ::testing::Test {
protected:
    DbConn                          conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;

    void SetUp() override {
        try {
            pool = std::make_unique<litecode::ConnectionPool>(
                conn_info.to_pool_config());
        } catch (const std::exception& e) {
            GTEST_SKIP() << "MySQL not reachable, skipping integration test: "
                         << e.what();
        }
        if (!pool || !pool->ping()) {
            GTEST_SKIP() << "MySQL ping failed, skipping integration test";
        }
    }

    void TearDown() override {
        pool.reset();
    }
};

// Generate a slug unique to this test run. Counts up so re-running a
// single test in isolation doesn't collide with itself, and tags with
// "pr-" (problem_repo) so it's obvious in the DB which test left
// the row (we leave rows on purpose — the tests don't own the
// lifecycle).
//
// IMPORTANT: the slug validator only accepts [a-z0-9-]. We replace
// '_' with '-' in the tag so callers can write natural test names
// like "create_defaults" without tripping the validator.
std::string fresh_slug(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '_') c = '-';
    }
    return std::string("pr-") + safe_tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

// Best-effort cleanup of every slug created during the test. We
// don't fail the test if cleanup fails — the next test's fresh_slug
// avoids collisions anyway.
void cleanup_slugs(litecode::ConnectionPool& pool,
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

// RAII helper that records the slugs a test created and drops them
// in TearDown. Tests append via add().
class SlugTracker {
public:
    explicit SlugTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~SlugTracker() { if (pool_) cleanup_slugs(*pool_, created_); }

    void add(std::string s) { created_.push_back(std::move(s)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        created_;
};

// Build a fully-populated ProblemRow with sane defaults. Tests
// override individual fields when they need to exercise an edge
// case (empty description, default time_limit, etc.).
litecode::ProblemRow make_basic(const std::string& slug,
                                const std::string& title = "Test Problem",
                                const std::string& difficulty = "easy") {
    litecode::ProblemRow p;
    p.slug        = slug;
    p.title       = title;
    p.difficulty  = difficulty;
    p.description = "# " + title + "\n\nSample description body.";
    p.time_limit   = 1000;
    p.memory_limit = 256;
    return p;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — no DB I/O
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateSlug, AcceptsAllowedShapes) {
    EXPECT_TRUE (litecode::validate_slug("two-sum"));
    EXPECT_TRUE (litecode::validate_slug("a"));
    EXPECT_TRUE (litecode::validate_slug("abc-123"));
    EXPECT_TRUE (litecode::validate_slug("foo-bar-baz-qux"));
    EXPECT_TRUE (litecode::validate_slug("01-02-03"));
    EXPECT_TRUE (litecode::validate_slug("x9"));
}

TEST(ValidateSlug, RejectsBadLength) {
    std::string err;
    EXPECT_FALSE(litecode::validate_slug("",        &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_slug(std::string(101, 'a'), &err));
    EXPECT_TRUE (litecode::validate_slug(std::string(100, 'a')));
    EXPECT_TRUE (litecode::validate_slug("a"));
}

TEST(ValidateSlug, RejectsLeadingOrTrailingHyphen) {
    EXPECT_FALSE(litecode::validate_slug("-two-sum"));
    EXPECT_FALSE(litecode::validate_slug("two-sum-"));
}

TEST(ValidateSlug, RejectsInvalidCharacters) {
    EXPECT_FALSE(litecode::validate_slug("Two-Sum"));        // uppercase
    EXPECT_FALSE(litecode::validate_slug("two_sum"));        // underscore
    EXPECT_FALSE(litecode::validate_slug("two.sum"));        // dot
    EXPECT_FALSE(litecode::validate_slug("two sum"));        // space
    EXPECT_FALSE(litecode::validate_slug("two/sum"));        // slash
    EXPECT_FALSE(litecode::validate_slug("two!sum"));        // punctuation
    EXPECT_FALSE(litecode::validate_slug("twosum!"));        // trailing punctuation
    EXPECT_FALSE(litecode::validate_slug("twosùm"));         // non-ASCII
}

TEST(IsValidSlugChar, AcceptsAllowedCharacters) {
    for (char c : {'a','z','0','9','-'}) {
        EXPECT_TRUE(litecode::is_valid_slug_char(c))
            << "char '" << c << "' should be valid";
    }
}

TEST(IsValidSlugChar, RejectsEverythingElse) {
    for (char c : {'A','Z','_','.',' ','/','\\',':','!','@','\t','\n','\0'}) {
        EXPECT_FALSE(litecode::is_valid_slug_char(c))
            << "char " << static_cast<int>(static_cast<unsigned char>(c))
            << " should be invalid";
    }
}

TEST(ValidateDifficulty, AcceptsAllowedValues) {
    EXPECT_TRUE(litecode::validate_difficulty("easy"));
    EXPECT_TRUE(litecode::validate_difficulty("medium"));
    EXPECT_TRUE(litecode::validate_difficulty("hard"));
}

TEST(ValidateDifficulty, RejectsUnknownValues) {
    std::string err;
    EXPECT_FALSE(litecode::validate_difficulty("EASY",   &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_difficulty("simple", &err));
    EXPECT_FALSE(litecode::validate_difficulty("",       &err));
    EXPECT_FALSE(litecode::validate_difficulty("easy ",  &err));
    EXPECT_FALSE(litecode::validate_difficulty("extreme",&err));
}

TEST(IsValidDifficulty, AcceptsAllowedValues) {
    EXPECT_TRUE (litecode::is_valid_difficulty("easy"));
    EXPECT_TRUE (litecode::is_valid_difficulty("medium"));
    EXPECT_TRUE (litecode::is_valid_difficulty("hard"));
    EXPECT_FALSE(litecode::is_valid_difficulty(""));
    EXPECT_FALSE(litecode::is_valid_difficulty("EASY"));
    EXPECT_FALSE(litecode::is_valid_difficulty("extreme"));
}

TEST(ValidateTitle, AcceptsAllowedLength) {
    std::string err;
    EXPECT_TRUE (litecode::validate_title("A",          &err));
    EXPECT_TRUE (litecode::validate_title("两数之和",   &err));
    EXPECT_TRUE (litecode::validate_title(std::string(200, 'A'), &err));
    EXPECT_FALSE(litecode::validate_title("",            &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_title(std::string(201, 'A'), &err));
}

TEST(ValidateTimeLimit, AcceptsInRange) {
    std::string err;
    EXPECT_TRUE (litecode::validate_time_limit(1,     &err));
    EXPECT_TRUE (litecode::validate_time_limit(1000,  &err));
    EXPECT_TRUE (litecode::validate_time_limit(60'000,&err));
    EXPECT_FALSE(litecode::validate_time_limit(0,     &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_time_limit(-1,    &err));
    EXPECT_FALSE(litecode::validate_time_limit(60'001,&err));
    EXPECT_FALSE(litecode::validate_time_limit(999'999,&err));
}

TEST(ValidateMemoryLimit, AcceptsInRange) {
    std::string err;
    EXPECT_TRUE (litecode::validate_memory_limit(1,     &err));
    EXPECT_TRUE (litecode::validate_memory_limit(256,   &err));
    EXPECT_TRUE (litecode::validate_memory_limit(1024,  &err));
    EXPECT_FALSE(litecode::validate_memory_limit(0,     &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_memory_limit(-1,    &err));
    EXPECT_FALSE(litecode::validate_memory_limit(1025,  &err));
    EXPECT_FALSE(litecode::validate_memory_limit(99'999,&err));
}

TEST(ClampListFilter, ClampsBadInputs) {
    litecode::ProblemListFilter f;

    // Default values are fine.
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  litecode::kDefaultListLimit);
    EXPECT_EQ(f.offset, 0);

    // Negative / zero limit → default.
    f.limit  = 0;
    f.offset = 0;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultListLimit);

    f.limit  = -7;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultListLimit);

    // Excessive limit → kMaxListLimit.
    f.limit  = 9999;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kMaxListLimit);

    // Negative offset → 0.
    f.limit  = 20;
    f.offset = -10;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.offset, 0);

    // Sane input passes through.
    f.limit  = 50;
    f.offset = 100;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  50);
    EXPECT_EQ(f.offset, 100);
}

TEST(Constants, MatchSpec) {
    // SPEC §4.2: slug VARCHAR(100), title VARCHAR(200), time/memory defaults
    // 1000ms / 256MB. We pick slightly looser inner ranges so admins
    // can tighten a problem (1ms) or loosen it (60s) without hitting
    // the validator; the DB column types are the hard upper bound.
    EXPECT_EQ(litecode::kMinSlugLength,    1u);
    EXPECT_EQ(litecode::kMaxSlugLength,    100u);
    EXPECT_EQ(litecode::kMinTitleLength,   1u);
    EXPECT_EQ(litecode::kMaxTitleLength,   200u);
    EXPECT_EQ(litecode::kMinTimeLimitMs,   1);
    EXPECT_EQ(litecode::kMaxTimeLimitMs,   60'000);
    EXPECT_EQ(litecode::kMinMemoryLimitMb, 1);
    EXPECT_EQ(litecode::kMaxMemoryLimitMb, 1024);
    EXPECT_EQ(litecode::kDefaultListLimit, 20);
    EXPECT_EQ(litecode::kMaxListLimit,     100);
}

TEST(ExceptionHierarchy, TypedErrorsDeriveFromRepoError) {
    // The route handler's 404 / 409 paths catch ProblemRepoError &
    // specifically, so make sure the typed exceptions really are
    // ProblemRepoError subclasses.
    litecode::ProblemNotFoundError      nf ("nf");
    litecode::ProblemAlreadyExistsError ae ("ae");
    EXPECT_NE(dynamic_cast<const litecode::ProblemRepoError*>(&nf), nullptr);
    EXPECT_NE(dynamic_cast<const litecode::ProblemRepoError*>(&ae), nullptr);
    EXPECT_THROW(throw litecode::ProblemNotFoundError("x"),
                 litecode::ProblemRepoError);
    EXPECT_THROW(throw litecode::ProblemAlreadyExistsError("x"),
                 litecode::ProblemRepoError);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — skipped if MySQL isn't reachable
// ────────────────────────────────────────────────────────────────────────────

TEST_F(DbFixture, CreateInsertsAndReturnsId) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("create");
    tracker.add(slug);
    auto row = make_basic(slug);

    const int id = litecode::problem_repo::create(*pool, row);
    EXPECT_GT(id, 0) << "create returned non-positive id";
    row.id = id;

    // Round-trip via find_by_slug — every column we care about
    // should match what we inserted.
    const auto loaded = litecode::problem_repo::find_by_slug(
        *pool, slug);
    ASSERT_TRUE(loaded.has_value()) << "problem not found after insert";
    EXPECT_EQ(loaded->id,              id);
    EXPECT_EQ(loaded->slug,            row.slug);
    EXPECT_EQ(loaded->title,           row.title);
    EXPECT_EQ(loaded->difficulty,      row.difficulty);
    EXPECT_EQ(loaded->description,     row.description);
    EXPECT_EQ(loaded->time_limit,      1000);
    EXPECT_EQ(loaded->memory_limit,    256);
    EXPECT_EQ(loaded->accepted_count,  0);
    EXPECT_EQ(loaded->submission_count,0);
    EXPECT_FALSE(loaded->is_deleted);
    EXPECT_FALSE(loaded->created_at.empty());
    EXPECT_FALSE(loaded->updated_at.empty());
}

TEST_F(DbFixture, CreateHonorsDefaultLimits) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("create_defaults");
    tracker.add(slug);
    litecode::ProblemRow row;
    row.slug         = slug;
    row.title        = "Defaults";
    row.difficulty   = "medium";
    row.description  = "Body";
    // time_limit and memory_limit explicitly set to 0 — should fall
    // back to 1000 / 256 (SPEC §2.2 defaults) inside create().
    row.time_limit   = 0;
    row.memory_limit = 0;

    const int id = litecode::problem_repo::create(*pool, row);
    ASSERT_GT(id, 0);

    const auto loaded = litecode::problem_repo::find_by_slug(*pool, slug);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->time_limit,   1000);
    EXPECT_EQ(loaded->memory_limit, 256);
}

TEST_F(DbFixture, CreateDuplicateSlugReturnsZero) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("dup");
    tracker.add(slug);

    litecode::ProblemRow a = make_basic(slug, "first", "easy");
    ASSERT_GT(litecode::problem_repo::create(*pool, a), 0);

    // Second insert with the same slug must return 0 (not throw).
    litecode::ProblemRow b = make_basic(slug, "second", "hard");
    EXPECT_EQ(litecode::problem_repo::create(*pool, b), 0);
}

TEST_F(DbFixture, CreateRejectsBadInputs) {
    // All five validators must reject their bad input. create() must
    // surface each as a ProblemRepoError.
    EXPECT_THROW({
        litecode::ProblemRow r;
        r.slug = "";                  // empty slug
        r.title = "x";
        r.difficulty = "easy";
        r.description = "x";
        litecode::problem_repo::create(*pool, r);
    }, litecode::ProblemRepoError);

    EXPECT_THROW({
        litecode::ProblemRow r;
        r.slug = "ok-slug";
        r.title = "";                 // empty title
        r.difficulty = "easy";
        r.description = "x";
        litecode::problem_repo::create(*pool, r);
    }, litecode::ProblemRepoError);

    EXPECT_THROW({
        litecode::ProblemRow r;
        r.slug = "ok-slug";
        r.title = "ok";
        r.difficulty = "extreme";     // bad difficulty
        r.description = "x";
        litecode::problem_repo::create(*pool, r);
    }, litecode::ProblemRepoError);

    EXPECT_THROW({
        litecode::ProblemRow r;
        r.slug = "ok-slug";
        r.title = "ok";
        r.difficulty = "easy";
        r.description = "x";
        r.time_limit = -1;            // negative time_limit
        litecode::problem_repo::create(*pool, r);
    }, litecode::ProblemRepoError);

    EXPECT_THROW({
        litecode::ProblemRow r;
        r.slug = "ok-slug";
        r.title = "ok";
        r.difficulty = "easy";
        r.description = "x";
        r.memory_limit = -1;          // negative memory_limit
        litecode::problem_repo::create(*pool, r);
    }, litecode::ProblemRepoError);
}

TEST_F(DbFixture, SlugExistsDefaultHidesSoftDeleted) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("exists");
    tracker.add(slug);

    // Before insert: false (default + include_deleted=false).
    EXPECT_FALSE(litecode::problem_repo::slug_exists(*pool, slug));
    EXPECT_FALSE(litecode::problem_repo::slug_exists(*pool, slug, true));

    litecode::ProblemRow row = make_basic(slug);
    ASSERT_GT(litecode::problem_repo::create(*pool, row), 0);

    EXPECT_TRUE (litecode::problem_repo::slug_exists(*pool, slug));
    EXPECT_TRUE (litecode::problem_repo::slug_exists(*pool, slug, true));

    // Soft-delete and re-check both flags.
    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug));
    EXPECT_FALSE(litecode::problem_repo::slug_exists(*pool, slug));         // default hides
    EXPECT_TRUE (litecode::problem_repo::slug_exists(*pool, slug, true));   // include_deleted sees
}

TEST_F(DbFixture, FindByIdAndSlugRoundTrip) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("findboth");
    tracker.add(slug);
    litecode::ProblemRow row = make_basic(slug, "Round Trip", "medium");
    row.description = "# Round Trip\n\nWith **markdown** body.";
    row.time_limit   = 2500;
    row.memory_limit = 512;
    const int id = litecode::problem_repo::create(*pool, row);
    ASSERT_GT(id, 0);

    const auto by_id   = litecode::problem_repo::find_by_id  (*pool, id);
    const auto by_slug = litecode::problem_repo::find_by_slug(*pool, slug);
    ASSERT_TRUE(by_id  .has_value());
    ASSERT_TRUE(by_slug.has_value());
    EXPECT_EQ(by_id->id,            by_slug->id);
    EXPECT_EQ(by_id->slug,          by_slug->slug);
    EXPECT_EQ(by_id->title,         by_slug->title);
    EXPECT_EQ(by_id->difficulty,    by_slug->difficulty);
    EXPECT_EQ(by_id->description,   by_slug->description);
    EXPECT_EQ(by_id->time_limit,    2500);
    EXPECT_EQ(by_id->memory_limit,  512);
    EXPECT_EQ(by_id->accepted_count, 0);
    EXPECT_EQ(by_id->submission_count, 0);
}

TEST_F(DbFixture, FindByUnknownReturnsNullopt) {
    EXPECT_FALSE(litecode::problem_repo::find_by_id  (*pool, 99999999).has_value());
    EXPECT_FALSE(litecode::problem_repo::find_by_slug(*pool, "no-such-slug-xyz-zzz").has_value());
}

TEST_F(DbFixture, SoftDeleteHidesFromDefaultFindsButNotFromIncludeDeleted) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("softdel");
    tracker.add(slug);
    litecode::ProblemRow row = make_basic(slug);
    const int id = litecode::problem_repo::create(*pool, row);
    ASSERT_GT(id, 0);

    // Sanity: visible everywhere.
    ASSERT_TRUE(litecode::problem_repo::find_by_id  (*pool, id).has_value());
    ASSERT_TRUE(litecode::problem_repo::find_by_slug(*pool, slug).has_value());

    // Soft-delete.
    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug));

    // Default find: hidden.
    EXPECT_FALSE(litecode::problem_repo::find_by_id  (*pool, id  ).has_value());
    EXPECT_FALSE(litecode::problem_repo::find_by_slug(*pool, slug).has_value());
    // include_deleted: visible, is_deleted=true.
    const auto admin_view = litecode::problem_repo::find_by_id(*pool, id, /*include_deleted=*/true);
    ASSERT_TRUE(admin_view.has_value());
    EXPECT_TRUE(admin_view->is_deleted);
}

TEST_F(DbFixture, SoftDeleteIsIdempotent) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("softdel_idem");
    tracker.add(slug);
    litecode::ProblemRow row = make_basic(slug);
    ASSERT_GT(litecode::problem_repo::create(*pool, row), 0);

    // First delete: true.
    EXPECT_TRUE (litecode::problem_repo::soft_delete(*pool, slug));
    // Second delete on the same slug: false (no live row to flip).
    EXPECT_FALSE(litecode::problem_repo::soft_delete(*pool, slug));
    // Unknown slug: false.
    EXPECT_FALSE(litecode::problem_repo::soft_delete(*pool, "no-such-slug-xyz"));
}

TEST_F(DbFixture, RestoreBringsRowBack) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("restore");
    tracker.add(slug);
    litecode::ProblemRow row = make_basic(slug);
    const int id = litecode::problem_repo::create(*pool, row);
    ASSERT_GT(id, 0);

    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug));
    EXPECT_FALSE(litecode::problem_repo::find_by_slug(*pool, slug).has_value());

    ASSERT_TRUE(litecode::problem_repo::restore(*pool, slug));
    const auto back = litecode::problem_repo::find_by_slug(*pool, slug);
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back->is_deleted);

    // Restoring a live row is a no-op (returns false).
    EXPECT_FALSE(litecode::problem_repo::restore(*pool, slug));
    // Restoring an unknown slug is a no-op (returns false).
    EXPECT_FALSE(litecode::problem_repo::restore(*pool, "no-such-slug-xyz"));
}

TEST_F(DbFixture, UpdateModifiesEveryColumn) {
    SlugTracker tracker(pool.get());

    const std::string slug = fresh_slug("update");
    tracker.add(slug);
    litecode::ProblemRow row = make_basic(slug, "Original", "easy");
    row.description = "old body";
    const int id = litecode::problem_repo::create(*pool, row);
    ASSERT_GT(id, 0);

    // Build a patch with every column changed.
    litecode::ProblemRow patch;
    patch.slug        = slug;                  // slug unchanged
    patch.title       = "Updated";
    patch.difficulty  = "hard";
    patch.description = "# Updated\n\nnew body";
    patch.time_limit   = 2000;
    patch.memory_limit = 512;

    EXPECT_TRUE(litecode::problem_repo::update(*pool, slug, patch));

    const auto loaded = litecode::problem_repo::find_by_id(*pool, id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->title,         "Updated");
    EXPECT_EQ(loaded->difficulty,    "hard");
    EXPECT_EQ(loaded->description,   "# Updated\n\nnew body");
    EXPECT_EQ(loaded->time_limit,    2000);
    EXPECT_EQ(loaded->memory_limit,  512);
}

TEST_F(DbFixture, UpdateRenamesSlug) {
    SlugTracker tracker(pool.get());

    const std::string slug_old = fresh_slug("rename_old");
    const std::string slug_new = fresh_slug("rename_new");
    tracker.add(slug_old);
    tracker.add(slug_new);

    litecode::ProblemRow row = make_basic(slug_old);
    const int id = litecode::problem_repo::create(*pool, row);
    ASSERT_GT(id, 0);

    litecode::ProblemRow patch = make_basic(slug_new);
    EXPECT_TRUE(litecode::problem_repo::update(*pool, slug_old, patch));

    // Old slug is gone (default find); new slug is live.
    EXPECT_FALSE(litecode::problem_repo::find_by_slug(*pool, slug_old).has_value());
    const auto moved = litecode::problem_repo::find_by_slug(*pool, slug_new);
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->id, id);
}

TEST_F(DbFixture, UpdateUnknownSlugThrowsNotFound) {
    EXPECT_THROW(
        litecode::problem_repo::update(
            *pool, "no-such-slug-xyz",
            make_basic("ignored", "x")),
        litecode::ProblemNotFoundError);
}

TEST_F(DbFixture, UpdateIntoExistingSlugThrowsAlreadyExists) {
    SlugTracker tracker(pool.get());

    const std::string slug_a = fresh_slug("rename_a");
    const std::string slug_b = fresh_slug("rename_b");
    tracker.add(slug_a);
    tracker.add(slug_b);

    ASSERT_GT(litecode::problem_repo::create(*pool, make_basic(slug_a)), 0);
    ASSERT_GT(litecode::problem_repo::create(*pool, make_basic(slug_b)), 0);

    // Try to rename A → B. Slug B is already taken; the UPDATE must
    // surface as ProblemAlreadyExistsError, not a 1062 generic driver
    // error.
    EXPECT_THROW(
        litecode::problem_repo::update(*pool, slug_a, make_basic(slug_b)),
        litecode::ProblemAlreadyExistsError);

    // Both rows are still intact.
    EXPECT_TRUE(litecode::problem_repo::find_by_slug(*pool, slug_a).has_value());
    EXPECT_TRUE(litecode::problem_repo::find_by_slug(*pool, slug_b).has_value());
}

TEST_F(DbFixture, ListReturnsPaginatedFilterableRows) {
    SlugTracker tracker(pool.get());

    // Create 5 problems, 3 easy + 2 hard.
    std::vector<std::string> easy_slugs, hard_slugs;
    for (int i = 0; i < 3; ++i) {
        const std::string s = fresh_slug("list_easy");
        tracker.add(s);
        easy_slugs.push_back(s);
        litecode::ProblemRow r = make_basic(s, "Easy "  + std::to_string(i), "easy");
        ASSERT_GT(litecode::problem_repo::create(*pool, r), 0);
    }
    for (int i = 0; i < 2; ++i) {
        const std::string s = fresh_slug("list_hard");
        tracker.add(s);
        hard_slugs.push_back(s);
        litecode::ProblemRow r = make_basic(s, "Hard " + std::to_string(i), "hard");
        ASSERT_GT(litecode::problem_repo::create(*pool, r), 0);
    }

    // No filter: at least 5 results in the page.
    litecode::ProblemListFilter f;
    f.limit = 100;
    f.offset = 0;
    const auto page_all = litecode::problem_repo::list(*pool, f);
    EXPECT_GE(page_all.items.size(), 5u);
    EXPECT_GE(page_all.total,         5);
    EXPECT_EQ(page_all.limit,         100);
    EXPECT_EQ(page_all.offset,        0);

    // Filter by difficulty=easy: at least our 3.
    litecode::ProblemListFilter f_easy;
    f_easy.limit = 100;
    f_easy.difficulty = std::string("easy");
    const auto page_easy = litecode::problem_repo::list(*pool, f_easy);
    EXPECT_GE(page_easy.items.size(), 3u);
    EXPECT_GE(page_easy.total,         3);
    for (const auto& p : page_easy.items) {
        EXPECT_EQ(p.difficulty, "easy");
    }

    // Filter by difficulty=hard: at least our 2.
    litecode::ProblemListFilter f_hard;
    f_hard.limit = 100;
    f_hard.difficulty = std::string("hard");
    const auto page_hard = litecode::problem_repo::list(*pool, f_hard);
    EXPECT_GE(page_hard.items.size(), 2u);
    EXPECT_GE(page_hard.total,         2);
    for (const auto& p : page_hard.items) {
        EXPECT_EQ(p.difficulty, "hard");
    }
}

TEST_F(DbFixture, ListPaginationClampsAndPages) {
    SlugTracker tracker(pool.get());

    // Insert 3 rows with the same difficulty; we'll paginate over them.
    for (int i = 0; i < 3; ++i) {
        const std::string s = fresh_slug("page");
        tracker.add(s);
        litecode::ProblemRow r = make_basic(s, "Page", "medium");
        ASSERT_GT(litecode::problem_repo::create(*pool, r), 0);
    }

    // limit > kMaxListLimit gets clamped to kMaxListLimit.
    litecode::ProblemListFilter huge;
    huge.limit = 9999;
    const auto clamped = litecode::problem_repo::list(*pool, huge);
    EXPECT_EQ(clamped.limit, litecode::kMaxListLimit);

    // Page through the medium-only filter.
    litecode::ProblemListFilter p1;
    p1.limit = 1;
    p1.offset = 0;
    p1.difficulty = std::string("medium");
    const auto page1 = litecode::problem_repo::list(*pool, p1);
    EXPECT_LE(page1.items.size(), 1u);
    EXPECT_GE(page1.total,         3);

    litecode::ProblemListFilter p2;
    p2.limit = 1;
    p2.offset = 1;
    p2.difficulty = std::string("medium");
    const auto page2 = litecode::problem_repo::list(*pool, p2);
    EXPECT_LE(page2.items.size(), 1u);
    EXPECT_EQ(page2.total, page1.total);
    // Page 1 and page 2 should not collide on id.
    if (!page1.items.empty() && !page2.items.empty()) {
        EXPECT_NE(page1.items[0].id, page2.items[0].id);
    }
}

TEST_F(DbFixture, ListExcludesSoftDeletedByDefaultAndIncludesWhenAsked) {
    SlugTracker tracker(pool.get());

    const std::string slug_keep = fresh_slug("list_keep");
    const std::string slug_drop = fresh_slug("list_drop");
    tracker.add(slug_keep);
    tracker.add(slug_drop);

    // Both created with the same difficulty so the filter is fair
    // — we want to detect "did the soft-deleted row leak into the
    // page" without confusing it with a difficulty mismatch.
    ASSERT_GT(litecode::problem_repo::create(
        *pool, make_basic(slug_keep, "Keep", "medium")), 0);
    ASSERT_GT(litecode::problem_repo::create(
        *pool, make_basic(slug_drop, "Drop", "medium")), 0);

    // Capture the live count for medium BEFORE soft-deleting.
    litecode::ProblemListFilter before;
    before.limit = 1000;
    before.difficulty = std::string("medium");
    const auto page_before = litecode::problem_repo::list(*pool, before);
    const int medium_count_before = page_before.total;

    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug_drop));

    // Default: the soft-deleted row drops out of the count and out
    // of the page.
    const auto page_after = litecode::problem_repo::list(*pool, before);
    EXPECT_EQ(page_after.total, medium_count_before - 1);
    for (const auto& p : page_after.items) {
        EXPECT_NE(p.slug, slug_drop);
    }

    // include_deleted=true: the soft-deleted row comes back into the
    // page (and total).
    litecode::ProblemListFilter with_deleted;
    with_deleted.limit = 1000;
    with_deleted.difficulty = std::string("medium");
    with_deleted.include_deleted = true;
    const auto page_with = litecode::problem_repo::list(*pool, with_deleted);
    EXPECT_EQ(page_with.total, medium_count_before);
    bool seen_drop = false;
    for (const auto& p : page_with.items) {
        if (p.slug == slug_drop) {
            EXPECT_TRUE(p.is_deleted);
            seen_drop = true;
        }
    }
    EXPECT_TRUE(seen_drop);
}

TEST_F(DbFixture, ListOrderingIsCreatedAtDescIdDesc) {
    SlugTracker tracker(pool.get());

    // Insert 3 rows in sequence. We use a dedicated prefix so we
    // can pull exactly these 3 out of a potentially-crowded table.
    std::vector<int> ids;
    for (int i = 0; i < 3; ++i) {
        const std::string s = fresh_slug("order");
        tracker.add(s);
        litecode::ProblemRow r = make_basic(s, "Order " + std::to_string(i), "hard");
        const int id = litecode::problem_repo::create(*pool, r);
        ASSERT_GT(id, 0);
        ids.push_back(id);
        // Sleep 5ms so created_at differs between rows (DATETIME has
        // 1-second resolution; without the pause two rows can share
        // a timestamp and the id-tiebreaker takes over). We only
        // need a clearly-monotonic ordering, and a tiny sleep
        // (rarely crossed at second boundaries) is still useful for
        // stressing the created_at branch of the ORDER BY.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // We can't reliably filter by the dedicated prefix alone (slug
    // has our timestamp+seq), so we extract just the 3 ids we just
    // inserted from the page (in page order) and verify they appear
    // in id-DESC order — which is what "created_at DESC, id DESC"
    // collapses to when created_at collides for back-to-back inserts.
    litecode::ProblemListFilter f;
    f.limit = litecode::kMaxListLimit;
    const auto page = litecode::problem_repo::list(*pool, f);
    ASSERT_GE(page.items.size(), 3u);

    std::vector<int> seen_ids;
    for (const auto& p : page.items) {
        if (std::find(ids.begin(), ids.end(), p.id) != ids.end()) {
            seen_ids.push_back(p.id);
        }
    }
    ASSERT_EQ(seen_ids.size(), ids.size());
    // Newest insertion (ids.back) must come first; oldest (ids.front) last.
    for (std::size_t i = 0; i < seen_ids.size(); ++i) {
        EXPECT_EQ(seen_ids[i], ids[ids.size() - 1 - i])
            << "ordering mismatch at index " << i;
    }
}
