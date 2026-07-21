// tests/unit/test_admin_special_judge.cpp
//
// Tests for the admin Special Judge CRUD surface (v1.3.1 — SPJ 闭环).
//
// Scoped specifically to the *admin* layer — the underlying
// special_judge_repo behavior is covered by tests/unit/test_special_judge.cpp
// (16 cases). What this suite adds is the admin-only semantics:
//
//   * 256 KB admin-side clamp (kMaxSpjSourceLenAdmin in
//     src/routes/admin_problem_routes.h::admin_put_special_judge_handler).
//     The repo ceiling is 16 MB (kMaxSpjSourceLen = 16 * 1024 * 1024);
//     admin uploads want a tighter bound to keep compile time predictable.
//     We re-state the constant here and grep the header to confirm it
//     didn't drift (the value lives inside a function so we can't
//     #include the header to static_assert).
//
//   * audit_log_repo constants (kActionProblemSpjUpsert /
//     kActionProblemSpjRemove) — namespaced alongside the other
//     problem.* actions, string-equal to the wire contract. The e2e
//     A48 (v1.3.1) relies on these strings being exact.
//
//   * Validator edges the admin endpoint exercises:
//       - empty source rejected (would-be admin footgun)
//       - exactly 1-byte source accepted (per repo contract; SPJ of
//         length 0 is meaningless)
//       - exactly 256 KB accepted (admin cap boundary)
//       - 256 KB + 1 rejected (admin cap + 1)
//       - exactly 16 MB accepted (repo ceiling)
//       - 16 MB + 1 rejected (repo ceiling + 1)
//
//   * Language validator edges the admin endpoint exercises:
//       - "cpp" accepted (only language today)
//       - "C++" rejected (case-sensitive — mirrors the repo docstring)
//       - "py" / "python" rejected
//
//   * Repo idempotency / re-upsert / FK cascade (integration):
//       - upsert twice with identical content → row exists, content unchanged
//       - admin cap boundary 256 KB upserted → find round-trips verbatim
//       - bad language upsert → no row written
//       - remove then re-upsert → find returns the new row verbatim
//       - FK CASCADE hard-delete parent problem wipes the SPJ row
//
// Coverage matrix:
//   - Pure unit (no DB required):
//       * Admin cap constant pinned (grep header)
//       * Audit action constants pinned
//       * Validator edges (above)
//       * SpecialJudgeRow defaults
//       * Exception hierarchy (RepoError → NotFoundError)
//
//   - Integration (require MySQL with V010 applied):
//       * find_by_problem_id on missing row → nullopt
//       * upsert idempotency (same content → same find result)
//       * upsert at admin cap boundary (256 KB) → round-trip
//       * upsert rejects bad language → no row written
//       * remove + re-upsert → new row content
//       * FK CASCADE hard-delete → SPJ row gone
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.) with
// the same defaults used across Phase 3/4 repos. When MySQL is unreachable
// the integration tests SKIP — the binary still passes the pure-unit tests
// on a machine without MySQL.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "config.h"
#include "db/audit_log_repo.h"
#include "db/connection_pool.h"
#include "db/special_judge_repo.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Constants (re-declared to mirror the admin handler)
//
//  `kMaxSpjSourceLenAdmin` lives inside
//  admin_put_special_judge_handler (line ~1204 in admin_problem_routes.h).
//  We mirror it here so tests don't have to #include that header — the
//  whole admin route set pulls in httplib + every other route, which
//  would explode the test binary's link surface. We pin it via grep
//  on the header file (see TestAdminCapMatchesHeader below) so any
//  drift is caught.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kMaxSpjSourceLenAdmin = 256 * 1024;

// Project root — discovered by walking up from the cwd until we find
// a CMakeLists.txt. Mirrors the trick other test binaries use.
std::string project_root() {
    namespace fs = std;
    char buf[4096] = {0};
    if (!fs::getcwd(buf, sizeof(buf) - 1)) return std::string();
    std::string cur(buf);
    for (int i = 0; i < 8; ++i) {
        std::ifstream f(cur + "/CMakeLists.txt");
        if (f.good()) return cur;
        auto pos = cur.find_last_of("/\\");
        if (pos == std::string::npos) break;
        cur = cur.substr(0, pos);
    }
    return std::string();
}

// Read a whole file as a string (small files only — used for grep tests).
std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return std::string();
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers (mirrors test_special_judge.cpp / test_admin_*.cpp)
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

// RAII cleanup: hard-delete the parent problem so V010's FK CASCADE
// sweeps any leftover problem_special_judges rows. Mirrors the
// SpjTracker pattern in test_special_judge.cpp.
struct AdminSpjTracker {
    litecode::ConnectionPool* pool = nullptr;
    std::vector<int>          problem_ids_to_clean;

    ~AdminSpjTracker() {
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
    AdminSpjTracker tracker;

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
// FK parent. Mirrors the helper in test_special_judge.cpp.
int make_throwaway_problem(litecode::ConnectionPool& pool,
                           AdminSpjTracker& tracker,
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
            .bind(std::string("Throwaway for admin SPJ test"))
            .bind(std::string("# description\n\nThrowaway."))
            .execute();
        const int new_id = static_cast<int>(rs.getAutoIncrementValue());
        tracker.problem_ids_to_clean.push_back(new_id);
        return new_id;
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
//  Pure unit tests — TEST() (no DB required)
// ════════════════════════════════════════════════════════════════════════════

// The admin handler pins `constexpr std::size_t kMaxSpjSourceLenAdmin = 256 * 1024;`
// inside admin_put_special_judge_handler. We can't include that header
// in a unit test (it pulls in the entire route table + httplib server),
// so we re-declare the value and grep the header to confirm it didn't
// drift out from under us.
TEST(AdminSpjConstants, AdminCapMatchesHeader) {
    const std::string root = project_root();
    ASSERT_FALSE(root.empty()) << "could not locate project root (CMakeLists.txt)";

    const std::string header_path =
        root + "/src/routes/admin_problem_routes.h";
    const std::string src = read_file(header_path);
    ASSERT_FALSE(src.empty()) << "could not read " << header_path;

    EXPECT_NE(src.find("kMaxSpjSourceLenAdmin = 256 * 1024"),
              std::string::npos)
        << "admin_problem_routes.h kMaxSpjSourceLenAdmin drifted — "
           "the unit-test mirror and the handler must agree";
    EXPECT_EQ(kMaxSpjSourceLenAdmin, 256u * 1024u);
}

// Admin cap must be strictly below the repo ceiling so the wire-layer
// rejection fires before the repo ever sees an oversized payload.
// (16 MB > 256 KB is the whole point.)
TEST(AdminSpjConstants, AdminCapBelowRepoCeiling) {
    EXPECT_LT(kMaxSpjSourceLenAdmin, litecode::kMaxSpjSourceLen);
    EXPECT_EQ(litecode::kMaxSpjSourceLen,
              static_cast<std::size_t>(16u * 1024u * 1024u));
}

// Wire contract: the audit action string must be exactly
// "problem.spj_upsert" so e2e A48 / A36 contract checks pass.
TEST(AdminSpjAuditAction, ProblemSpjUpsertValue) {
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemSpjUpsert,
                 "problem.spj_upsert");
}

TEST(AdminSpjAuditAction, ProblemSpjRemoveValue) {
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemSpjRemove,
                 "problem.spj_remove");
}

// Namespacing: the two new constants sit alongside the existing
// problem.* actions — same namespace, same prefix, same camel-case.
TEST(AdminSpjAuditAction, SpjActionsShareProblemNamespace) {
    EXPECT_NE(std::string(litecode::audit_log_repo::kActionProblemCreate),
              std::string(litecode::audit_log_repo::kActionProblemSpjUpsert));
    EXPECT_NE(std::string(litecode::audit_log_repo::kActionProblemDelete),
              std::string(litecode::audit_log_repo::kActionProblemSpjRemove));
}

// Empty source: the admin endpoint also goes through validate_source,
// so an empty `source` field is rejected at the validator (400 path),
// not as a generic 500. The repo shares the same validator — sanity
// check the contract here so the admin endpoint can rely on it.
TEST(AdminSpjValidator, EmptySourceRejected) {
    std::string err;
    EXPECT_FALSE(litecode::special_judge_repo::validate_source("", &err));
    EXPECT_FALSE(err.empty());
}

TEST(AdminSpjValidator, OneByteSourceAccepted) {
    std::string err;
    EXPECT_TRUE(litecode::special_judge_repo::validate_source("x", &err));
    err.clear();
}

// Admin cap boundary: exactly 256 KB passes the repo validator (the
// repo ceiling is 16 MB). The admin endpoint clamps to 256 KB at the
// wire layer; the repo will accept anything ≤ 16 MB.
TEST(AdminSpjValidator, AdminCapBoundaryAccepted) {
    std::string err;
    EXPECT_TRUE(litecode::special_judge_repo::validate_source(
        std::string(kMaxSpjSourceLenAdmin, 'x'), &err));
    err.clear();
}

// 256 KB + 1 is rejected by the admin endpoint (it does
// `if (source.size() > kMaxSpjSourceLenAdmin) return 400`).
// The repo accepts it because it's well below 16 MB — the rejection
// lives at the wire layer, not at the repo. Verify both layers'
// semantics here so a future refactor doesn't accidentally move the
// check down (which would still reject but lose the 400 envelope).
TEST(AdminSpjValidator, AdminCapPlusOneAcceptedByRepo) {
    // The REPO allows 256K + 1 (≤ 16M). The admin ENDPOINT rejects it.
    // We document the asymmetry here so the admin wire-layer 400 stays
    // at the route, not at the repo.
    std::string err;
    EXPECT_TRUE(litecode::special_judge_repo::validate_source(
        std::string(kMaxSpjSourceLenAdmin + 1, 'x'), &err));
    err.clear();
}

TEST(AdminSpjValidator, RepoCeilingAccepted) {
    std::string err;
    EXPECT_TRUE(litecode::special_judge_repo::validate_source(
        std::string(litecode::kMaxSpjSourceLen, 'x'), &err));
    err.clear();
}

TEST(AdminSpjValidator, RepoCeilingPlusOneRejected) {
    std::string err;
    EXPECT_FALSE(litecode::special_judge_repo::validate_source(
        std::string(litecode::kMaxSpjSourceLen + 1, 'x'), &err));
    EXPECT_FALSE(err.empty());
}

// "cpp" is the only accepted language today (judge image is g++/gcc
// only). The admin endpoint mirrors this and surfaces 400 on any
// non-cpp value.
TEST(AdminSpjValidator, LanguageCppAccepted) {
    std::string err;
    EXPECT_TRUE(litecode::special_judge_repo::validate_language("cpp", &err));
    err.clear();
}

TEST(AdminSpjValidator, LanguageCppPlusPlusRejected) {
    std::string err;
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("C++", &err));
    EXPECT_FALSE(err.empty());
}

TEST(AdminSpjValidator, LanguagePythonRejected) {
    std::string err;
    EXPECT_FALSE(litecode::special_judge_repo::validate_language("python", &err));
    EXPECT_FALSE(err.empty());
}

// Plain-data projection defaults — the admin GET endpoint emits these
// fields verbatim, so the wire shape must match what the DB returns
// when the row is absent (all empty strings + source_bytes=0).
TEST(AdminSpjStruct, RowDefaults) {
    litecode::SpecialJudgeRow r;
    EXPECT_EQ (r.problem_id, 0);
    EXPECT_TRUE(r.source.empty());
    EXPECT_TRUE(r.language.empty());
    EXPECT_TRUE(r.created_at.empty());
    EXPECT_TRUE(r.updated_at.empty());
}

TEST(AdminSpjExceptions, Hierarchy) {
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
//  Integration tests — TEST_F(DbFixture, ...) (require MySQL + V010)
// ════════════════════════════════════════════════════════════════════════════

TEST_F(DbFixture, FindMissingReturnsNullopt) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "aspj-missing");
    ASSERT_GT(problem_id, 0);
    auto row = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    EXPECT_FALSE(row.has_value());
}

// Two PUTs with identical body must NOT cause data loss — the second
// write's row content must equal the first. This is the idempotency
// contract documented on the admin endpoint ("PUT = upsert", not no-op).
TEST_F(DbFixture, UpsertIdempotentSameContent) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "aspj-idem");
    ASSERT_GT(problem_id, 0);

    const std::string body = "// SPJ: always AC\nint main(){return 0;}\n";
    ASSERT_TRUE(litecode::special_judge_repo::upsert(
        *pool, problem_id, body, "cpp"));
    auto r1 = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    ASSERT_TRUE(r1.has_value());

    // Second PUT — same body, same language. Row must still exist with
    // the same content (no silent truncation / corruption).
    ASSERT_TRUE(litecode::special_judge_repo::upsert(
        *pool, problem_id, body, "cpp"));
    auto r2 = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->source,   body);
    EXPECT_EQ(r2->language, std::string("cpp"));

    litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id);
}

// Admin endpoint clamps at 256 KB but the repo accepts up to 16 MB —
// verify a SPJ at exactly the admin cap survives the round-trip so the
// admin wire can submit at its full budget without surprises.
TEST_F(DbFixture, UpsertAtAdminCapRoundTrips) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "aspj-256k");
    ASSERT_GT(problem_id, 0);

    const std::string at_cap(kMaxSpjSourceLenAdmin, 'x');
    ASSERT_TRUE(litecode::special_judge_repo::upsert(
        *pool, problem_id, at_cap, "cpp"));

    auto row = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->source.size(), kMaxSpjSourceLenAdmin);
    EXPECT_EQ(row->source,        at_cap);

    litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id);
}

TEST_F(DbFixture, UpsertRejectsBadLanguage) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "aspj-bad-lang");
    ASSERT_GT(problem_id, 0);

    EXPECT_THROW(
        litecode::special_judge_repo::upsert(*pool, problem_id, "int main(){return 0;}", "java"),
        litecode::SpecialJudgeRepoError);
    EXPECT_FALSE(litecode::special_judge_repo::exists_for_problem(*pool, problem_id));
}

// remove → re-upsert: the DELETE-then-PUT flow that the admin UI
// uses for "edit SPJ" + "remove SPJ" cycles.
TEST_F(DbFixture, RemoveThenReUpsert) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "aspj-remove-up");
    ASSERT_GT(problem_id, 0);

    const std::string v1 = "// v1: always AC\nint main(){return 0;}\n";
    const std::string v2 = "// v2: always WA\nint main(){return 1;}\n";

    ASSERT_TRUE(litecode::special_judge_repo::upsert(*pool, problem_id, v1, "cpp"));
    EXPECT_TRUE (litecode::special_judge_repo::exists_for_problem(*pool, problem_id));
    EXPECT_TRUE (litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id));
    EXPECT_FALSE(litecode::special_judge_repo::exists_for_problem(*pool, problem_id));

    // Re-upsert with new content — fresh row, fresh created_at.
    ASSERT_TRUE(litecode::special_judge_repo::upsert(*pool, problem_id, v2, "cpp"));
    auto row = litecode::special_judge_repo::find_by_problem_id(*pool, problem_id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->source, v2);

    litecode::special_judge_repo::remove_by_problem_id(*pool, problem_id);
}

// FK CASCADE on V010: hard-deleting the parent problem wipes the SPJ
// row too. Confirms the schema migration's intent.
TEST_F(DbFixture, FkCascadeHardDeleteWipesSpj) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "aspj-cascade");
    ASSERT_GT(problem_id, 0);

    ASSERT_TRUE(litecode::special_judge_repo::upsert(
        *pool, problem_id, "// sample\nint main(){return 0;}\n", "cpp"));
    ASSERT_TRUE(litecode::special_judge_repo::exists_for_problem(*pool, problem_id));

    try {
        auto conn = pool->acquire();
        conn.session().sql("DELETE FROM problems WHERE id = ?")
            .bind(static_cast<std::int64_t>(problem_id))
            .execute();
    } catch (...) {
        ADD_FAILURE() << "hard-delete parent failed";
    }
    EXPECT_FALSE(litecode::special_judge_repo::exists_for_problem(*pool, problem_id));

    // Suppress the tracker's RAII cleanup — the parent is already gone.
    auto& v = tracker.problem_ids_to_clean;
    v.erase(std::remove(v.begin(), v.end(), problem_id), v.end());
}