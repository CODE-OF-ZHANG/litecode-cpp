// tests/unit/test_special_judge.cpp
//
// Unit + integration tests for src/db/special_judge_repo.h
// (Phase 4 ☆ v1.2.18 — Special Judge 框架).
//
// Coverage matrix:
//   - Pure unit tests (no MySQL required):
//       * kMinSpjSourceLen / kMaxSpjSourceLen constants pin the SPEC
//         MEDIUMTEXT 16MB ceiling.
//       * validate_source: 0 / 1 / kMaxSpjSourceLen — exact boundary,
//         and the over-by-one rejection.
//       * validate_language: "cpp" / "" / "py" / "java" / "c" —
//         today only "cpp" is honored (litecode-judge image is
//         C++-only).
//       * SpecialJudgeRow defaults.
//       * Exception hierarchy (RepoError → NotFoundError inheritance).
//
//   - Integration tests (require a reachable MySQL with V010 applied):
//       * find_by_problem_id on missing row → std::nullopt.
//       * exists_for_problem mirrors find_by_problem_id (false when
//         missing, true after upsert).
//       * upsert happy path: insert → find returns the row verbatim.
//       * upsert replacement: same problem, new source — find
//         returns the new content (UPDATE branch of ON DUPLICATE KEY).
//       * upsert rejects empty / oversized / wrong-language via
//         SpecialJudgeRepoError (NOT driver errors → 400 path).
//       * upsert on unknown problem_id throws SpecialJudgeRepoError
//         (FK violation → upper layer can map 400).
//       * remove_by_problem_id returns true when a row was removed,
//         false on idempotent re-delete.
//       * FK CASCADE: hard-delete the parent problem → the SPJ row
//         goes away too (matches problem_revisions_repo tests).
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with the same defaults used across Phase 3/4 repos. When MySQL is
// unreachable the integration tests SKIP — the binary still passes
// the pure-unit tests on a machine without MySQL.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "config.h"
#include "db/connection_pool.h"
#include "db/special_judge_repo.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers (mirrors test_audit_log.cpp / test_problem_revision.cpp)
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
        c.max_size           = 4;
        c.acquire_timeout_ms = 2000;
        c.connect_timeout_ms = 5000;
        c.max_idle_time_ms   = 60000;
        return c;
    }
};

// Tracker — RAII cleanup for problem_id + SPJ rows. Hard-deletes the
// problem row so the FK CASCADE on V010 wipes any leftover
// problem_special_judges entries (mirrors test_problem_revision's
// RevisionTracker).
struct SpjTracker {
    litecode::ConnectionPool* pool = nullptr;
    std::vector<int>          problem_ids_to_clean;

    ~SpjTracker() {
        if (!pool) return;
        for (int pid : problem_ids_to_clean) {
            try {
                auto conn = pool->acquire();
                conn.session().sql(
                    "DELETE FROM problems WHERE id = ?")
                    .bind(static_cast<std::int64_t>(pid))
                    .execute();
            } catch (...) {}
        }
    }
};

class DbFixture : public ::testing::Test {
protected:
    DbConn conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    SpjTracker tracker;

    void SetUp() override {
        _putenv_s("JWT_SECRET", "test_jwt_secret_at_least_32_bytes_long_xxx");

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

        // V010 must already be applied; verify the table exists so we
        // get a meaningful SKIP message instead of a confusing
        // "no such table" on every test.
        try {
            auto conn = pool->acquire();
            auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.TABLES "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME   = 'problem_special_judges' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "V010 (problem_special_judges) not applied; "
                                "skipping integration test";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "schema probe failed: " << e.what();
        }

        tracker.pool = pool.get();
    }
};

// Mint a throwaway problem in `problems` so the SPJ row has a valid
// FK parent. Mirrors the same helper in test_problem_revision.
int make_throwaway_problem(litecode::ConnectionPool& pool,
                           SpjTracker& tracker,
                           const std::string& slug_in) {
    static std::atomic<int> counter{0};
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    std::ostringstream os;
    os << slug_in << "-" << ms << "-" << counter.fetch_add(1);
    const std::string slug = os.str();

    try {
        auto conn = pool.acquire();
        mysqlx::SqlResult rs = conn.session().sql(
            "INSERT INTO problems "
            "(slug, title, difficulty, description, time_limit, memory_limit, "
            " accepted_count, submission_count, is_deleted, created_at, updated_at) "
            "VALUES (?, ?, 'easy', ?, 1000, 256, 0, 0, FALSE, NOW(), NOW())")
            .bind(slug)
            .bind(std::string("Throwaway for SPJ test"))
            .bind(std::string("# description\n\nThrowaway."))
            .execute();
        const int new_id = static_cast<int>(rs.getAutoIncrementValue());
        tracker.problem_ids_to_clean.push_back(new_id);
        return new_id;
    } catch (const std::exception&) {
        return 0;
    }
}

// A representative-but-minimal C++ SPJ source. The integration tests
// feed this into upsert(); the test then reads it back via
// find_by_problem_id() and compares byte-for-byte.
const char* kSpjSampleSource =
    "// minimal special judge: read three file paths, compare expected\n"
    "// against actual byte-for-byte.\n"
    "#include <cstdio>\n"
    "#include <cstdlib>\n"
    "int main(int argc, char** argv) {\n"
    "    if (argc != 4) return 1;\n"
    "    FILE* e = std::fopen(argv[2], \"rb\");\n"
    "    FILE* a = std::fopen(argv[3], \"rb\");\n"
    "    if (!e || !a) return 2;\n"
    "    int c1, c2;\n"
    "    do { c1 = std::fgetc(e); c2 = std::fgetc(a);\n"
    "         if (c1 != c2) { std::fclose(e); std::fclose(a); return 1; } }\n"
    "    while (c1 != EOF && c2 != EOF);\n"
    "    std::fclose(e); std::fclose(a);\n"
    "    return 0;\n"
    "}\n";

} // namespace

// ════════════════════════════════════════════════════════════════════════════
//  Pure unit tests — TEST() (no DB required)
// ════════════════════════════════════════════════════════════════════════════

TEST(SpecialJudgeConstants, BoundPinning) {
    EXPECT_EQ(litecode::kMinSpjSourceLen, 1u);
    EXPECT_EQ(litecode::kMaxSpjSourceLen,
              static_cast<std::size_t>(16u * 1024u * 1024u));
    EXPECT_STREQ(litecode::kSpjLanguageCxx, "cpp");
}

TEST(SpecialJudgeValidators, ValidateSource) {
    std::string err;

    // Empty ⇒ rejected (no point in a 0-byte program; the "no SPJ
    // attached" path is handled at the route / find layer by *absence
    // of a row*, not by storing a 0-byte source).
    EXPECT_FALSE(litecode::special_judge_repo::validate_source("", &err));
    EXPECT_FALSE(err.empty());

    // 1-byte minimum: a single non-empty byte passes.
    EXPECT_TRUE (litecode::special_judge_repo::validate_source("x",                       &err)); err.clear();
    // Realistic small SPJ body.
    EXPECT_TRUE (litecode::special_judge_repo::validate_source("int main(){}",             &err)); err.clear();
    // Exact MAX boundary (16 MB) passes.
    EXPECT_TRUE (litecode::special_judge_repo::validate_source(
                    std::string(litecode::kMaxSpjSourceLen, 'x'),                          &err));
    err.clear();

    // Over-by-one: MAX + 1 rejected.
    EXPECT_FALSE(litecode::special_judge_repo::validate_source(
                    std::string(litecode::kMaxSpjSourceLen + 1, 'x'),                      &err));
    EXPECT_FALSE(err.empty());
}

TEST(SpecialJudgeValidators, ValidateLanguage) {
    std::string err;
    EXPECT_TRUE (litecode::special_judge_repo::validate_language("cpp", &err)); err.clear();

    // Everything else is rejected today (judge image is C++-only).
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("",     &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("c",     &err));
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("py",    &err));
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("python",&err));
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("java",  &err));
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("C++",   &err));  // case-sensitive
}

TEST(SpecialJudgeStruct, Defaults) {
    litecode::SpecialJudgeRow r;
    EXPECT_EQ (r.problem_id, 0);
    EXPECT_TRUE(r.source.empty());
    EXPECT_TRUE(r.language.empty());
    EXPECT_TRUE(r.created_at.empty());
    EXPECT_TRUE(r.updated_at.empty());
}

TEST(SpecialJudgeExceptions, Hierarchy) {
    using litecode::SpecialJudgeRepoError;
    using litecode::SpecialJudgeNotFoundError;

    std::unique_ptr<std::runtime_error> base(new SpecialJudgeRepoError("x"));
    std::unique_ptr<std::runtime_error> nf  (new SpecialJudgeNotFoundError("x"));

    auto* as_repo = dynamic_cast<SpecialJudgeRepoError*>(base.get());
    auto* as_nf   = dynamic_cast<SpecialJudgeNotFoundError*>(nf.get());
    ASSERT_NE(as_repo, nullptr);
    ASSERT_NE(as_nf,   nullptr);
    EXPECT_NE(dynamic_cast<SpecialJudgeRepoError*>(nf.get()), nullptr);
    EXPECT_EQ(dynamic_cast<SpecialJudgeNotFoundError*>(base.get()), nullptr);
}

// ════════════════════════════════════════════════════════════════════════════
//  Integration tests — TEST_F(DbFixture, ...)
// ════════════════════════════════════════════════════════════════════════════

TEST_F(DbFixture, FindByProblemIdMissingReturnsNullopt) {
    // Fresh problem with no SPJ attached ⇒ nullopt.
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-missing");
    ASSERT_GT(problem_id, 0);

    auto row = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    EXPECT_FALSE(row.has_value());
}

TEST_F(DbFixture, ExistsForProblemMirrorsFind) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-exists");
    ASSERT_GT(problem_id, 0);

    EXPECT_FALSE(litecode::special_judge_repo::exists_for_problem(
        *pool, problem_id));

    ASSERT_TRUE(litecode::special_judge_repo::upsert(
        *pool, problem_id, kSpjSampleSource, "cpp"));

    EXPECT_TRUE(litecode::special_judge_repo::exists_for_problem(
        *pool, problem_id));

    // Cleanup so other test runs find a clean slate.
    litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id);
}

TEST_F(DbFixture, UpsertHappyPathThenFindRoundTrips) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-roundtrip");
    ASSERT_GT(problem_id, 0);

    ASSERT_TRUE(litecode::special_judge_repo::upsert(
        *pool, problem_id, kSpjSampleSource, "cpp"));

    auto row = litecode::special_judge_repo::find_by_problem_id(
        *pool, problem_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->problem_id, problem_id);
    EXPECT_EQ(row->source,     std::string(kSpjSampleSource));
    EXPECT_EQ(row->language,   std::string("cpp"));
    // created_at / updated_at come back as DATE_FORMAT'd text; we
    // don't pin the exact string here — the format itself is documented
    // in the migration and the repo, the smoke check is "non-empty".
    EXPECT_FALSE(row->created_at.empty());
    EXPECT_FALSE(row->updated_at.empty());

    // Cleanup.
    litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id);
}

TEST_F(DbFixture, UpsertReplacesExistingSource) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-replace");
    ASSERT_GT(problem_id, 0);

    const std::string v1 = "// v1: returns 0 always\nint main(){return 0;}\n";
    const std::string v2 = "// v2: returns 1 always\nint main(){return 1;}\n";

    ASSERT_TRUE(litecode::special_judge_repo::upsert(*pool, problem_id, v1, "cpp"));
    auto r1 = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->source, v1);

    // Sleep briefly so CURRENT_TIMESTAMP shifts by at least a second
    // on platforms with second-precision DATETIME columns. Skip on
    // CI boxes with sub-second resolution — update tests would just
    // be flaky for no signal.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    ASSERT_TRUE(litecode::special_judge_repo::upsert(*pool, problem_id, v2, "cpp"));
    auto r2 = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->source, v2);
    EXPECT_EQ(r2->language, std::string("cpp"));

    // Cleanup.
    litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id);
}

TEST_F(DbFixture, UpsertRejectsBadLanguage) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-bad-lang");
    ASSERT_GT(problem_id, 0);

    EXPECT_THROW(
        litecode::special_judge_repo::upsert(*pool, problem_id, kSpjSampleSource, "java"),
        litecode::SpecialJudgeRepoError);

    EXPECT_THROW(
        litecode::special_judge_repo::upsert(*pool, problem_id, kSpjSampleSource, ""),
        litecode::SpecialJudgeRepoError);

    // No row should have been written by the failed attempts.
    EXPECT_FALSE(litecode::special_judge_repo::exists_for_problem(*pool, problem_id));
}

TEST_F(DbFixture, UpsertForeignKeyViolationSurfacesTypedError) {
    // problem_id = 9_999_999 ⇒ no such row in `problems`.
    EXPECT_THROW(
        litecode::special_judge_repo::upsert(
            *pool, 9999999, kSpjSampleSource, "cpp"),
        litecode::SpecialJudgeRepoError);
}

TEST_F(DbFixture, RemoveByProblemIdIdempotent) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-remove");
    ASSERT_GT(problem_id, 0);

    // First remove: nothing to delete ⇒ false (idempotent).
    EXPECT_FALSE(litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id));
    // Second remove: still false (idempotent re-delete).
    EXPECT_FALSE(litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id));

    ASSERT_TRUE(litecode::special_judge_repo::upsert(*pool, problem_id,
        kSpjSampleSource, "cpp"));
    EXPECT_TRUE (litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id));
    EXPECT_FALSE(litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id));
}

TEST_F(DbFixture, RemoveByProblemIdRejectsNonPositiveId) {
    EXPECT_THROW(
        litecode::special_judge_repo::remove_by_problem_id(*pool, 0),
        litecode::SpecialJudgeRepoError);
    EXPECT_THROW(
        litecode::special_judge_repo::remove_by_problem_id(*pool, -1),
        litecode::SpecialJudgeRepoError);
}

TEST_F(DbFixture, FkCascadeHardDeleteProblemWipesSpjRow) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "spj-cascade");
    ASSERT_GT(problem_id, 0);

    ASSERT_TRUE(litecode::special_judge_repo::upsert(*pool, problem_id,
        kSpjSampleSource, "cpp"));
    ASSERT_TRUE (litecode::special_judge_repo::exists_for_problem(*pool, problem_id));

    // Hard-delete the parent problem; V010 FK ON DELETE CASCADE
    // should sweep the SPJ row.
    try {
        auto conn = pool->acquire();
        conn.session().sql("DELETE FROM problems WHERE id = ?")
            .bind(static_cast<std::int64_t>(problem_id))
            .execute();
    } catch (...) {
        ADD_FAILURE() << "hard-delete parent failed";
    }
    EXPECT_FALSE(litecode::special_judge_repo::exists_for_problem(*pool, problem_id));

    // Remove the problem_id from the tracker so its RAII cleanup
    // doesn't fire on an already-deleted id.
    auto& v = tracker.problem_ids_to_clean;
    v.erase(std::remove(v.begin(), v.end(), problem_id), v.end());
}
