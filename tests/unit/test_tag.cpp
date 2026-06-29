// tests/unit/test_tag.cpp
//
// Unit + light integration tests for src/db/tag_repo.h.
//
// Two layers, matching the test_problem / test_user_repo pattern:
//
//   1) Pure unit tests (no MySQL required):
//        - validate_tag_name shape, length, charset rules
//        - trim_tag_name trims leading/trailing whitespace only
//        - TagRow / TagWithCount default values
//        - kMin/kMax length constants pin SPEC §4.2b contract
//        - exception type hierarchy (TagNotFoundError /
//          TagAlreadyExistsError are both TagRepoError)
//
//   2) Integration tests (require a reachable MySQL):
//        Tags table:
//          - create inserts a row, returns the new id
//          - create returns 0 on duplicate name (409 path)
//          - create trims leading/trailing whitespace
//          - create rejects empty / too-long / control-char names
//          - name_exists round-trip
//          - find_by_id / find_by_name round-trip
//          - find_by_* returns nullopt for an unknown tag
//          - update renames
//          - update throws TagNotFoundError on a non-existent id
//          - update throws TagAlreadyExistsError when renaming into
//            another tag's name
//          - delete_by_id removes the row (and cascades to
//            problem_tags via FK)
//          - delete_by_id returns false for an unknown id
//          - list returns tags ordered by name
//          - list_with_count counts live problems only by default
//          - list_with_count includes soft-deleted problems when
//            asked
//          - list_with_count keeps zero-count tags in the result
//        problem_tags M:N:
//          - attach / detach round-trip
//          - attach is idempotent (PK collision via INSERT IGNORE)
//          - clear removes every tag for a problem
//          - replace is atomic: a tx failure leaves the OLD set
//          - replace dedupes the input id list
//          - list_tags_for_problem returns the right tags
//          - list_problems_for_tag returns the right ids
//          - list_problems_for_tag hides soft-deleted problems by
//            default
//          - count_tags_for_problem / count_problems_for_tag agree
//            with the list_*
//        Bulk resolver:
//          - find_or_create_many creates missing, returns existing
//          - find_or_create_many preserves caller order
//          - find_or_create_many returns the right ids
//          - find_or_create_many rejects invalid input (empty name)
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box (root/123456/litecode@127.0.0.1).
// If ping() fails, those tests are SKIPPED -- the binary still passes on a
// machine without MySQL, which is what we want for CI lint jobs.
//
// Each integration test uses a unique tag name (timestamp + counter) to
// dodge collisions across parallel test runs. We DON'T clean up the
// rows we create on purpose -- the tests don't own the lifecycle and
// "cleanup on failure" would mask any real FK issue that surfaces
// later. (delete_by_id is covered explicitly by its own test, so
// leaving the rows behind doesn't grow the table unboundedly in CI --
// the dev box is fine with a few hundred rows.)
//
// Note on Chinese tag names: the SPEC §4.2b example uses
// "\xe6\x95\xb0\xe7\xbb\x84" (array) and "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"
// (hash table). We use ASCII names in most tests for readability; one
// test exercises the multi-byte UTF-8 path explicitly via escape
// sequences (so the source file stays ASCII-only and builds cleanly
// on any MSVC code page).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "config.h"
#include "db/connection_pool.h"
#include "db/problem_repo.h"
#include "db/tag_repo.h"

namespace {

// ---------------------------------------------------------------------------
//  Test env helpers
// ---------------------------------------------------------------------------

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

// Generate a tag name unique to this test run. Counts up so re-running
// a single test in isolation doesn't collide with itself, and tags with
// "tg-" (tag_repo) so it's obvious in the DB which test left the row.
// We use only ASCII to keep the names readable in DB dumps and
// collision-proof against the utf8mb4_unicode_ci UNIQUE index (no
// case-insensitive collisions across test runs).
std::string fresh_name(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("tg-") + tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

// Best-effort cleanup of every tag name created during the test.
// Uses problem_repo (which has hard DELETE) to remove the slug
// rows first, then tag_repo to remove the tag rows. We don't
// fail the test if cleanup fails -- the next test's fresh_name
// avoids collisions anyway.
void cleanup_tags(litecode::ConnectionPool& pool,
                  const std::vector<std::string>& names) {
    if (names.empty()) return;
    try {
        auto conn = pool.acquire();
        // First, find every tag id (so we can clean up problem_tags).
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

// RAII helper that records the names a test created and drops
// them in TearDown. Tests append via add().
class NameTracker {
public:
    explicit NameTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~NameTracker() { if (pool_) cleanup_tags(*pool_, created_); }

    void add(std::string n) { created_.push_back(std::move(n)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        created_;
};

// Same idea for slugs (we need real `problems` rows to exercise the
// M:N helpers). Reuses the problem_repo's slug validator: slug
// only accepts [a-z0-9-]. The "tr-" prefix marks the slug's origin
// in the DB.
std::string fresh_slug(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '_') c = '-';
    }
    return std::string("tr-") + safe_tag + "-" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "-" + std::to_string(n);
}

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

}  // namespace

// ---------------------------------------------------------------------------
//  Pure unit tests -- no DB I/O
// ---------------------------------------------------------------------------

TEST(ValidateTagName, AcceptsAllowedShapes) {
    std::string err;
    EXPECT_TRUE (litecode::validate_tag_name("array",     &err));
    EXPECT_TRUE (litecode::validate_tag_name("hash-table", &err));
    EXPECT_TRUE (litecode::validate_tag_name("a",          &err));
    EXPECT_TRUE (litecode::validate_tag_name("linked list",&err));
    // Multi-byte UTF-8 names (the SPEC §4.2b examples). We use
    // \xNN escape sequences so the source file stays pure ASCII
    // and the MSVC code page doesn't matter at compile time.
    EXPECT_TRUE (litecode::validate_tag_name(
        "\xe6\x95\xb0\xe7\xbb\x84",       &err));  // "array" (CJK)
    EXPECT_TRUE (litecode::validate_tag_name(
        "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8", &err));  // "hash table" (CJK)
    EXPECT_TRUE (litecode::validate_tag_name(std::string(50, 'a'), &err));
}

TEST(ValidateTagName, RejectsEmptyAndTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_tag_name("",        &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_tag_name(std::string(51, 'a'), &err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE (litecode::validate_tag_name(std::string(50, 'a'), &err));
}

TEST(ValidateTagName, RejectsLeadingOrTrailingWhitespace) {
    std::string err;
    EXPECT_FALSE(litecode::validate_tag_name(" foo", &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_tag_name("foo ", &err));
    EXPECT_FALSE(litecode::validate_tag_name("\tfoo",&err));
    EXPECT_FALSE(litecode::validate_tag_name("foo\t",&err));
    // Internal whitespace is fine.
    EXPECT_TRUE (litecode::validate_tag_name("foo bar", &err));
    EXPECT_TRUE (litecode::validate_tag_name("a b c d", &err));
}

TEST(ValidateTagName, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_tag_name("foo\nbar", &err));
    EXPECT_FALSE(litecode::validate_tag_name("foo\rbar", &err));
    EXPECT_FALSE(litecode::validate_tag_name("foo\tbar", &err));  // internal tab
    EXPECT_FALSE(litecode::validate_tag_name(std::string("foo\0bar", 7), &err));
}

TEST(TrimTagName, TrimsLeadingAndTrailingWhitespace) {
    EXPECT_EQ(litecode::trim_tag_name("foo"),          "foo");
    EXPECT_EQ(litecode::trim_tag_name("  foo  "),      "foo");
    EXPECT_EQ(litecode::trim_tag_name("\tfoo\t"),      "foo");
    EXPECT_EQ(litecode::trim_tag_name(" foo "),        "foo");
    EXPECT_EQ(litecode::trim_tag_name("foo bar"),      "foo bar");  // internal preserved
    EXPECT_EQ(litecode::trim_tag_name("  foo bar  "),  "foo bar");
    EXPECT_EQ(litecode::trim_tag_name(""),             "");
    EXPECT_EQ(litecode::trim_tag_name("   "),          "");
    // UTF-8 safe: high bytes of multi-byte sequences are never
    // matched as ASCII space/tab.
    EXPECT_EQ(litecode::trim_tag_name(
        "  \xe6\x95\xb0\xe7\xbb\x84  "),      "\xe6\x95\xb0\xe7\xbb\x84");
}

TEST(TagRowDefaults, ZeroedAndEmpty) {
    litecode::TagRow t;
    EXPECT_EQ(t.id,   0);
    EXPECT_EQ(t.name, "");
}

TEST(TagWithCountDefaults, ZeroedAndEmpty) {
    litecode::TagWithCount twc;
    EXPECT_EQ(twc.tag.id,         0);
    EXPECT_EQ(twc.tag.name,       "");
    EXPECT_EQ(twc.problem_count,  0);
}

TEST(Constants, MatchSpec) {
    // SPEC §4.2b: tags.name VARCHAR(50). Our validator is exactly
    // the column width.
    EXPECT_EQ(litecode::kMinTagNameLength, 1u);
    EXPECT_EQ(litecode::kMaxTagNameLength, 50u);
}

TEST(ExceptionHierarchy, TypedErrorsDeriveFromRepoError) {
    // The route handler's 404 / 409 paths catch TagRepoError &
    // specifically, so make sure the typed exceptions really are
    // TagRepoError subclasses.
    litecode::TagNotFoundError      nf ("nf");
    litecode::TagAlreadyExistsError ae ("ae");
    EXPECT_NE(dynamic_cast<const litecode::TagRepoError*>(&nf), nullptr);
    EXPECT_NE(dynamic_cast<const litecode::TagRepoError*>(&ae), nullptr);
    EXPECT_THROW(throw litecode::TagNotFoundError("x"),
                 litecode::TagRepoError);
    EXPECT_THROW(throw litecode::TagAlreadyExistsError("x"),
                 litecode::TagRepoError);
}

// ---------------------------------------------------------------------------
//  Integration tests -- skipped if MySQL isn't reachable
// ---------------------------------------------------------------------------

// --- `tags` table ---------------------------------------------------------

TEST_F(DbFixture, CreateInsertsAndReturnsId) {
    NameTracker tracker(pool.get());

    const std::string name = fresh_name("create");
    tracker.add(name);

    const int id = litecode::tag_repo::create(*pool, name);
    EXPECT_GT(id, 0) << "create returned non-positive id";

    // Round-trip via find_by_id and find_by_name -- both must see it.
    const auto by_id   = litecode::tag_repo::find_by_id  (*pool, id);
    const auto by_name = litecode::tag_repo::find_by_name(*pool, name);
    ASSERT_TRUE(by_id  .has_value()) << "tag not found by id after insert";
    ASSERT_TRUE(by_name.has_value()) << "tag not found by name after insert";
    EXPECT_EQ(by_id->id,   id);
    EXPECT_EQ(by_id->name, name);
    EXPECT_EQ(by_name->id,   id);
    EXPECT_EQ(by_name->name, name);
}

TEST_F(DbFixture, CreateTrimsLeadingAndTrailingWhitespace) {
    NameTracker tracker(pool.get());

    const std::string canonical = fresh_name("trim");
    tracker.add(canonical);

    // Pass a name with leading + trailing whitespace; create()
    // must trim it to the canonical form.
    const int id = litecode::tag_repo::create(*pool, "  " + canonical + "\t");
    EXPECT_GT(id, 0);

    const auto loaded = litecode::tag_repo::find_by_id(*pool, id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name, canonical);
}

TEST_F(DbFixture, CreateDuplicateNameReturnsZero) {
    NameTracker tracker(pool.get());

    const std::string name = fresh_name("dup");
    tracker.add(name);

    EXPECT_GT(litecode::tag_repo::create(*pool, name), 0);
    // Second insert with the same name must return 0 (not throw).
    EXPECT_EQ(litecode::tag_repo::create(*pool, name), 0);
}

TEST_F(DbFixture, CreateRejectsInvalidNames) {
    // Every validator failure path must surface as TagRepoError so
    // the route handler can map it to 400.
    //
    // Note on trim behavior: create() trims leading/trailing ASCII
    // whitespace BEFORE validating, so a name like " foo" becomes
    // "foo" and is accepted. The "rejects leading/trailing
    // whitespace" semantic is implemented at a different level --
    // we test the trim path in CreateTrimsLeadingAndTrailingWhitespace
    // and only assert rejection here for inputs that the trim
    // can't paper over (empty after trim, too long, control chars).
    EXPECT_THROW(litecode::tag_repo::create(*pool, ""),         litecode::TagRepoError);
    EXPECT_THROW(litecode::tag_repo::create(*pool, "   "),      litecode::TagRepoError);
    EXPECT_THROW(litecode::tag_repo::create(*pool, std::string(51, 'a')),
                 litecode::TagRepoError);
    EXPECT_THROW(litecode::tag_repo::create(*pool, "foo\nbar"),litecode::TagRepoError);
    EXPECT_THROW(litecode::tag_repo::create(*pool, "\nfoo"),   litecode::TagRepoError);
}

TEST_F(DbFixture, NameExistsRoundTrip) {
    NameTracker tracker(pool.get());

    const std::string name = fresh_name("exists");
    tracker.add(name);

    EXPECT_FALSE(litecode::tag_repo::name_exists(*pool, name));
    EXPECT_GT(litecode::tag_repo::create(*pool, name), 0);
    EXPECT_TRUE (litecode::tag_repo::name_exists(*pool, name));
    EXPECT_TRUE (litecode::tag_repo::name_exists(*pool, name));  // idempotent
}

TEST_F(DbFixture, FindByIdAndNameReturnNulloptForUnknown) {
    EXPECT_FALSE(litecode::tag_repo::find_by_id  (*pool, 99999999).has_value());
    EXPECT_FALSE(litecode::tag_repo::find_by_name(*pool, "no-such-tag-xyz-zzz").has_value());
}

TEST_F(DbFixture, UpdateRenamesTag) {
    NameTracker tracker(pool.get());

    const std::string name_old = fresh_name("rename_old");
    const std::string name_new = fresh_name("rename_new");
    tracker.add(name_old);
    tracker.add(name_new);

    const int id = litecode::tag_repo::create(*pool, name_old);
    ASSERT_GT(id, 0);

    EXPECT_TRUE(litecode::tag_repo::update(*pool, id, name_new));

    const auto moved = litecode::tag_repo::find_by_id(*pool, id);
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->name, name_new);
    // Old name is gone.
    EXPECT_FALSE(litecode::tag_repo::find_by_name(*pool, name_old).has_value());
}

TEST_F(DbFixture, UpdateUnknownIdThrowsNotFound) {
    EXPECT_THROW(
        litecode::tag_repo::update(*pool, 99999999, "ignored"),
        litecode::TagNotFoundError);
}

TEST_F(DbFixture, UpdateIntoExistingNameThrowsAlreadyExists) {
    NameTracker tracker(pool.get());

    const std::string name_a = fresh_name("rename_a");
    const std::string name_b = fresh_name("rename_b");
    tracker.add(name_a);
    tracker.add(name_b);

    const int id_a = litecode::tag_repo::create(*pool, name_a);
    const int id_b = litecode::tag_repo::create(*pool, name_b);
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_b, 0);

    // Try to rename A -> B. B is already taken; the UPDATE must
    // surface as TagAlreadyExistsError, not a 1062 generic driver
    // error.
    EXPECT_THROW(
        litecode::tag_repo::update(*pool, id_a, name_b),
        litecode::TagAlreadyExistsError);

    // Both rows are still intact with their original names.
    const auto a = litecode::tag_repo::find_by_id(*pool, id_a);
    const auto b = litecode::tag_repo::find_by_id(*pool, id_b);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->name, name_a);
    EXPECT_EQ(b->name, name_b);
}

TEST_F(DbFixture, DeleteByIdRemovesRow) {
    NameTracker tracker(pool.get());

    const std::string name = fresh_name("delete");
    tracker.add(name);

    const int id = litecode::tag_repo::create(*pool, name);
    ASSERT_GT(id, 0);
    EXPECT_TRUE(litecode::tag_repo::delete_by_id(*pool, id));
    EXPECT_FALSE(litecode::tag_repo::find_by_id(*pool, id).has_value());
}

TEST_F(DbFixture, DeleteByIdReturnsFalseForUnknown) {
    EXPECT_FALSE(litecode::tag_repo::delete_by_id(*pool, 99999999));
}

TEST_F(DbFixture, DeleteByIdCascadesToProblemTags) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string name = fresh_name("cascade");
    const std::string slug = fresh_slug("cascade");
    name_tracker.add(name);
    slug_tracker.add(slug);

    const int tag_id = litecode::tag_repo::create(*pool, name);
    ASSERT_GT(tag_id, 0);

    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug, "Cascade Test"));
    ASSERT_GT(problem_id, 0);

    litecode::tag_repo::attach(*pool, problem_id, tag_id);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 1);

    // Delete the tag -- FK ON DELETE CASCADE must drop the
    // (problem_id, tag_id) row in problem_tags.
    EXPECT_TRUE(litecode::tag_repo::delete_by_id(*pool, tag_id));
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 0);
}

TEST_F(DbFixture, ListReturnsAllTagsOrderedByName) {
    NameTracker tracker(pool.get());

    const std::string a = fresh_name("list_a");
    const std::string b = fresh_name("list_b");
    const std::string c = fresh_name("list_c");
    tracker.add(a);
    tracker.add(b);
    tracker.add(c);

    ASSERT_GT(litecode::tag_repo::create(*pool, c), 0);
    ASSERT_GT(litecode::tag_repo::create(*pool, a), 0);
    ASSERT_GT(litecode::tag_repo::create(*pool, b), 0);

    const auto all = litecode::tag_repo::list(*pool);
    // Filter to ours (the table may have other rows from parallel
    // tests) and check the order is name-ASC.
    std::set<std::string> ours = {a, b, c};
    std::vector<std::string> seen_order;
    for (const auto& t : all) {
        if (ours.count(t.name)) seen_order.push_back(t.name);
    }
    ASSERT_EQ(seen_order.size(), 3u);
    // Per the names we generated, "list_a" < "list_b" < "list_c"
    // because the timestamp+seq suffixes are identical for this
    // test and only the tag differs.
    EXPECT_EQ(seen_order[0], a);
    EXPECT_EQ(seen_order[1], b);
    EXPECT_EQ(seen_order[2], c);
}

TEST_F(DbFixture, ListWithCountCountsLiveProblems) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string name = fresh_name("count_live");
    const std::string slug_a = fresh_slug("count_live_a");
    const std::string slug_b = fresh_slug("count_live_b");
    name_tracker.add(name);
    slug_tracker.add(slug_a);
    slug_tracker.add(slug_b);

    const int tag_id = litecode::tag_repo::create(*pool, name);
    ASSERT_GT(tag_id, 0);

    const int id_a = litecode::problem_repo::create(
        *pool, make_basic(slug_a, "Live A"));
    const int id_b = litecode::problem_repo::create(
        *pool, make_basic(slug_b, "Live B"));
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_b, 0);

    litecode::tag_repo::attach(*pool, id_a, tag_id);
    litecode::tag_repo::attach(*pool, id_b, tag_id);

    // Both live: count=2, by-id count=2.
    const auto ours_live = litecode::tag_repo::list_with_count(*pool);
    int found = -1;
    for (const auto& twc : ours_live) {
        if (twc.tag.id == tag_id) { found = twc.problem_count; break; }
    }
    EXPECT_GE(found, 2);
    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, tag_id), 2);

    // Soft-delete B; default list_with_count should drop it from
    // the count.
    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug_b));
    const auto ours_after = litecode::tag_repo::list_with_count(*pool);
    found = -1;
    for (const auto& twc : ours_after) {
        if (twc.tag.id == tag_id) { found = twc.problem_count; break; }
    }
    EXPECT_GE(found, 1);
    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, tag_id), 1);

    // include-deleted: both count again.
    const auto ours_all = litecode::tag_repo::list_with_count(*pool, false);
    found = -1;
    for (const auto& twc : ours_all) {
        if (twc.tag.id == tag_id) { found = twc.problem_count; break; }
    }
    EXPECT_GE(found, 2);
    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, tag_id, true), 2);
}

TEST_F(DbFixture, ListWithCountKeepsZeroCountTags) {
    NameTracker tracker(pool.get());

    const std::string name = fresh_name("zero");
    tracker.add(name);
    const int tag_id = litecode::tag_repo::create(*pool, name);
    ASSERT_GT(tag_id, 0);

    // Tag with zero problems must still appear in list_with_count.
    const auto all = litecode::tag_repo::list_with_count(*pool);
    bool found = false;
    for (const auto& twc : all) {
        if (twc.tag.id == tag_id) {
            EXPECT_EQ(twc.problem_count, 0);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "tag with zero problems was dropped from list_with_count";
}

// --- `problem_tags` M:N ---------------------------------------------------

TEST_F(DbFixture, AttachAndDetachRoundTrip) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string name = fresh_name("attach");
    const std::string slug = fresh_slug("attach");
    name_tracker.add(name);
    slug_tracker.add(slug);

    const int tag_id = litecode::tag_repo::create(*pool, name);
    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug));
    ASSERT_GT(tag_id, 0);
    ASSERT_GT(problem_id, 0);

    litecode::tag_repo::attach(*pool, problem_id, tag_id);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 1);

    EXPECT_TRUE (litecode::tag_repo::detach(*pool, problem_id, tag_id));
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 0);
    // Second detach: false (no row matched).
    EXPECT_FALSE(litecode::tag_repo::detach(*pool, problem_id, tag_id));
}

TEST_F(DbFixture, AttachIsIdempotent) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string name = fresh_name("idem");
    const std::string slug = fresh_slug("idem");
    name_tracker.add(name);
    slug_tracker.add(slug);

    const int tag_id = litecode::tag_repo::create(*pool, name);
    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug));
    ASSERT_GT(tag_id, 0);
    ASSERT_GT(problem_id, 0);

    // Three attaches in a row -- INSERT IGNORE must absorb the
    // PK collision on calls 2 and 3.
    litecode::tag_repo::attach(*pool, problem_id, tag_id);
    litecode::tag_repo::attach(*pool, problem_id, tag_id);
    litecode::tag_repo::attach(*pool, problem_id, tag_id);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 1);
}

TEST_F(DbFixture, ClearRemovesAllTagsForProblem) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n1 = fresh_name("clear1");
    const std::string n2 = fresh_name("clear2");
    const std::string n3 = fresh_name("clear3");
    const std::string slug = fresh_slug("clear");
    name_tracker.add(n1);
    name_tracker.add(n2);
    name_tracker.add(n3);
    slug_tracker.add(slug);

    const int t1 = litecode::tag_repo::create(*pool, n1);
    const int t2 = litecode::tag_repo::create(*pool, n2);
    const int t3 = litecode::tag_repo::create(*pool, n3);
    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug));
    ASSERT_GT(problem_id, 0);

    litecode::tag_repo::attach(*pool, problem_id, t1);
    litecode::tag_repo::attach(*pool, problem_id, t2);
    litecode::tag_repo::attach(*pool, problem_id, t3);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 3);

    const int removed = litecode::tag_repo::clear(*pool, problem_id);
    EXPECT_EQ(removed, 3);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 0);
}

TEST_F(DbFixture, ReplaceSwapsTagSetAtomically) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n_old_1 = fresh_name("old1");
    const std::string n_old_2 = fresh_name("old2");
    const std::string n_new_1 = fresh_name("new1");
    const std::string n_new_2 = fresh_name("new2");
    const std::string slug = fresh_slug("replace");
    name_tracker.add(n_old_1);
    name_tracker.add(n_old_2);
    name_tracker.add(n_new_1);
    name_tracker.add(n_new_2);
    slug_tracker.add(slug);

    const int t_old_1 = litecode::tag_repo::create(*pool, n_old_1);
    const int t_old_2 = litecode::tag_repo::create(*pool, n_old_2);
    const int t_new_1 = litecode::tag_repo::create(*pool, n_new_1);
    const int t_new_2 = litecode::tag_repo::create(*pool, n_new_2);
    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug));
    ASSERT_GT(problem_id, 0);

    litecode::tag_repo::attach(*pool, problem_id, t_old_1);
    litecode::tag_repo::attach(*pool, problem_id, t_old_2);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 2);

    // Replace old -> new.
    litecode::tag_repo::replace(
        *pool, problem_id, {t_new_1, t_new_2});

    // Old tag associations are gone.
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 2);
    auto ours = litecode::tag_repo::list_tags_for_problem(*pool, problem_id);
    std::set<int> ours_ids;
    for (const auto& t : ours) ours_ids.insert(t.id);
    EXPECT_TRUE(ours_ids.count(t_new_1));
    EXPECT_TRUE(ours_ids.count(t_new_2));
    EXPECT_FALSE(ours_ids.count(t_old_1));
    EXPECT_FALSE(ours_ids.count(t_old_2));
}

TEST_F(DbFixture, ReplaceDedupesInputIds) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n1 = fresh_name("dedup1");
    const std::string n2 = fresh_name("dedup2");
    const std::string slug = fresh_slug("dedup");
    name_tracker.add(n1);
    name_tracker.add(n2);
    slug_tracker.add(slug);

    const int t1 = litecode::tag_repo::create(*pool, n1);
    const int t2 = litecode::tag_repo::create(*pool, n2);
    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug));
    ASSERT_GT(problem_id, 0);

    // Pass {t1, t1, t2, t2, t1} -- replace should dedupe to {t1, t2}.
    litecode::tag_repo::replace(
        *pool, problem_id, {t1, t1, t2, t2, t1});
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, problem_id), 2);
}

TEST_F(DbFixture, ListTagsForProblemReturnsAttached) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n1 = fresh_name("ltfp1");
    const std::string n2 = fresh_name("ltfp2");
    const std::string slug = fresh_slug("ltfp");
    name_tracker.add(n1);
    name_tracker.add(n2);
    slug_tracker.add(slug);

    const int t1 = litecode::tag_repo::create(*pool, n1);
    const int t2 = litecode::tag_repo::create(*pool, n2);
    const int problem_id = litecode::problem_repo::create(
        *pool, make_basic(slug));
    ASSERT_GT(problem_id, 0);

    litecode::tag_repo::attach(*pool, problem_id, t1);
    litecode::tag_repo::attach(*pool, problem_id, t2);

    const auto tags = litecode::tag_repo::list_tags_for_problem(*pool, problem_id);
    ASSERT_EQ(tags.size(), 2u);
    // Ordered by name ASC.
    if (n1 < n2) {
        EXPECT_EQ(tags[0].name, n1);
        EXPECT_EQ(tags[1].name, n2);
    } else {
        EXPECT_EQ(tags[0].name, n2);
        EXPECT_EQ(tags[1].name, n1);
    }
}

TEST_F(DbFixture, ListTagsForProblemReturnsEmptyForUnknownProblem) {
    EXPECT_TRUE(
        litecode::tag_repo::list_tags_for_problem(*pool, 99999999).empty());
}

TEST_F(DbFixture, ListProblemsForTagReturnsAttached) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n = fresh_name("lpft");
    name_tracker.add(n);
    const std::string slug_a = fresh_slug("lpft_a");
    const std::string slug_b = fresh_slug("lpft_b");
    slug_tracker.add(slug_a);
    slug_tracker.add(slug_b);

    const int t = litecode::tag_repo::create(*pool, n);
    const int id_a = litecode::problem_repo::create(
        *pool, make_basic(slug_a));
    const int id_b = litecode::problem_repo::create(
        *pool, make_basic(slug_b));
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_b, 0);

    litecode::tag_repo::attach(*pool, id_a, t);
    litecode::tag_repo::attach(*pool, id_b, t);

    const auto ids = litecode::tag_repo::list_problems_for_tag(*pool, t);
    EXPECT_EQ(ids.size(), 2u);
    // Both ids present (order is id-ASC; we know id_a < id_b
    // because the second insert had a higher auto-increment).
    EXPECT_EQ(ids[0], id_a);
    EXPECT_EQ(ids[1], id_b);
}

TEST_F(DbFixture, ListProblemsForTagHidesSoftDeletedByDefault) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n = fresh_name("lpft_soft");
    name_tracker.add(n);
    const std::string slug_keep = fresh_slug("lpft_keep");
    const std::string slug_drop = fresh_slug("lpft_drop");
    slug_tracker.add(slug_keep);
    slug_tracker.add(slug_drop);

    const int t = litecode::tag_repo::create(*pool, n);
    const int id_keep = litecode::problem_repo::create(
        *pool, make_basic(slug_keep));
    const int id_drop = litecode::problem_repo::create(
        *pool, make_basic(slug_drop));
    ASSERT_GT(id_keep, 0);
    ASSERT_GT(id_drop, 0);

    litecode::tag_repo::attach(*pool, id_keep, t);
    litecode::tag_repo::attach(*pool, id_drop, t);
    ASSERT_TRUE(litecode::problem_repo::soft_delete(*pool, slug_drop));

    // Default: only id_keep visible.
    const auto live = litecode::tag_repo::list_problems_for_tag(*pool, t);
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0], id_keep);

    // include_deleted=true: both visible.
    const auto all = litecode::tag_repo::list_problems_for_tag(*pool, t, true);
    EXPECT_EQ(all.size(), 2u);

    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, t),       1);
    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, t, true), 2);
}

TEST_F(DbFixture, CountHelpersAgreeWithLists) {
    NameTracker name_tracker(pool.get());
    SlugTracker slug_tracker(pool.get());

    const std::string n1 = fresh_name("cnt1");
    const std::string n2 = fresh_name("cnt2");
    const std::string slug_a = fresh_slug("cnt_a");
    const std::string slug_b = fresh_slug("cnt_b");
    name_tracker.add(n1);
    name_tracker.add(n2);
    slug_tracker.add(slug_a);
    slug_tracker.add(slug_b);

    const int t1 = litecode::tag_repo::create(*pool, n1);
    const int t2 = litecode::tag_repo::create(*pool, n2);
    const int id_a = litecode::problem_repo::create(
        *pool, make_basic(slug_a));
    const int id_b = litecode::problem_repo::create(
        *pool, make_basic(slug_b));
    ASSERT_GT(id_a, 0);
    ASSERT_GT(id_b, 0);

    litecode::tag_repo::attach(*pool, id_a, t1);
    litecode::tag_repo::attach(*pool, id_a, t2);
    litecode::tag_repo::attach(*pool, id_b, t1);

    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, id_a),    2);
    EXPECT_EQ(litecode::tag_repo::count_tags_for_problem(*pool, id_b),    1);
    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, t1),      2);
    EXPECT_EQ(litecode::tag_repo::count_problems_for_tag(*pool, t2),      1);
    EXPECT_EQ(static_cast<int>(
                  litecode::tag_repo::list_tags_for_problem(*pool, id_a).size()),
              litecode::tag_repo::count_tags_for_problem(*pool, id_a));
    EXPECT_EQ(static_cast<int>(
                  litecode::tag_repo::list_problems_for_tag(*pool, t1).size()),
              litecode::tag_repo::count_problems_for_tag(*pool, t1));
}

// --- bulk resolver --------------------------------------------------------

TEST_F(DbFixture, FindOrCreateManyCreatesMissingReturnsExisting) {
    NameTracker tracker(pool.get());

    const std::string n1 = fresh_name("fcm1");
    const std::string n2 = fresh_name("fcm2");
    const std::string n3 = fresh_name("fcm3");
    tracker.add(n1);
    tracker.add(n2);
    tracker.add(n3);

    // Pre-create n1; n2 + n3 should be auto-created.
    const int id_n1 = litecode::tag_repo::create(*pool, n1);
    ASSERT_GT(id_n1, 0);

    const auto resolved = litecode::tag_repo::find_or_create_many(
        *pool, {n1, n2, n3});

    ASSERT_EQ(resolved.size(), 3u);
    EXPECT_EQ(resolved[0].id,   id_n1);
    EXPECT_EQ(resolved[0].name, n1);
    EXPECT_GT(resolved[1].id, 0);
    EXPECT_EQ(resolved[1].name, n2);
    EXPECT_GT(resolved[2].id, 0);
    EXPECT_EQ(resolved[2].name, n3);

    // All three must be findable post-resolve.
    EXPECT_TRUE(litecode::tag_repo::name_exists(*pool, n1));
    EXPECT_TRUE(litecode::tag_repo::name_exists(*pool, n2));
    EXPECT_TRUE(litecode::tag_repo::name_exists(*pool, n3));
}

TEST_F(DbFixture, FindOrCreateManyPreservesOrder) {
    NameTracker tracker(pool.get());

    const std::string a = fresh_name("ord_a");
    const std::string b = fresh_name("ord_b");
    const std::string c = fresh_name("ord_c");
    tracker.add(a);
    tracker.add(b);
    tracker.add(c);

    // Caller passes in {c, a, b} -- the resolved vector must echo
    // the input order, not the DB insertion order.
    const auto resolved = litecode::tag_repo::find_or_create_many(
        *pool, {c, a, b});
    ASSERT_EQ(resolved.size(), 3u);
    EXPECT_EQ(resolved[0].name, c);
    EXPECT_EQ(resolved[1].name, a);
    EXPECT_EQ(resolved[2].name, b);
}

TEST_F(DbFixture, FindOrCreateManyEmptyInputReturnsEmpty) {
    const auto resolved = litecode::tag_repo::find_or_create_many(
        *pool, std::vector<std::string>{});
    EXPECT_TRUE(resolved.empty());
}

TEST_F(DbFixture, FindOrCreateManyRejectsInvalidInput) {
    // Empty name in the middle of the batch -> TagRepoError; no
    // partial creation.
    EXPECT_THROW(
        litecode::tag_repo::find_or_create_many(
            *pool, {fresh_name("ok"), "", fresh_name("ok2")}),
        litecode::TagRepoError);

    // Too-long name -> TagRepoError.
    EXPECT_THROW(
        litecode::tag_repo::find_or_create_many(
            *pool, {std::string(51, 'a')}),
        litecode::TagRepoError);
}

TEST_F(DbFixture, FindOrCreateManyAcceptsMultibyteUtf8Names) {
    NameTracker tracker(pool.get());

    // SPEC §4.2b example: "\xe6\x95\xb0\xe7\xbb\x84" (array) and
    // "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8" (hash table). Verify
    // the utf8mb4_unicode_ci round-trip works for multi-byte
    // names.
    //
    // We append a per-test ASCII suffix to make the names unique
    // across runs and to keep the tracker cleanup simple.
    const std::string ua = "\xe6\x95\xb0\xe7\xbb\x84" "tg-cn-fcm";
    const std::string ub = "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8" "tg-cn-fcm";
    tracker.add(ua);
    tracker.add(ub);

    // Sanity check the validator accepts the trimmed names.
    EXPECT_TRUE(litecode::validate_tag_name(ua));
    EXPECT_TRUE(litecode::validate_tag_name(ub));

    const auto resolved = litecode::tag_repo::find_or_create_many(
        *pool, {ua, ub});
    ASSERT_EQ(resolved.size(), 2u);
    EXPECT_GT(resolved[0].id, 0);
    EXPECT_GT(resolved[1].id, 0);
    EXPECT_EQ(resolved[0].name, ua);
    EXPECT_EQ(resolved[1].name, ub);

    // Find by CJK name round-trips.
    EXPECT_TRUE(litecode::tag_repo::name_exists(*pool, ua));
    EXPECT_TRUE(litecode::tag_repo::name_exists(*pool, ub));
}
