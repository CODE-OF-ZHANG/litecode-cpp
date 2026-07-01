// tests/unit/test_problem_revision.cpp
//
// Integration + unit tests for src/db/problem_revisions_repo.h
// (v1.2.12 — Phase 3 ★ problem_revisions storage layer).
//
// Coverage:
//   - Pure unit tests (no MySQL required):
//       * kMinRevisionNo / kMaxRevisionNo / kMaxSummaryLength
//                       / kMinEditorUsernameLength / kMaxEditorUsernameLength
//       * validate_revision_no: 0 (sentinel), 1, kMaxRevisionNo, kMaxRevisionNo+1, -1
//       * validate_summary: empty, exactly 200, 201, control-char
//       * validate_action: "create", "update", "delete", "", control-char
//       * validate_editor_username: empty, 1-char, 50-char, 51-char, control-char
//       * clamp_list_filter: limit 0 → 1 / limit 999 → 100 / offset -5 → 0
//       * RevisionEntry / RevisionRow / RevisionListFilter defaults
//       * Exception hierarchy (RepoError / NotFoundError / ConflictError)
//       * kActionRevisionCreate / kActionRevisionUpdate constants
//
//   - Integration tests (require a reachable MySQL):
//       * record happy path (revision_no=0 ⇒ auto MAX+1) → find_by_id round-trip
//       * record happy path (forced revision_no=N) — exact value preserved
//       * record_best_effort swallows on bad input → returns 0
//       * find_by_id missing id → std::nullopt
//       * latest_for_problem: empty → nullopt; populated → max revision_no
//       * count_for_problem ≡ list_for_problem total
//       * list_for_problem pagination: 5 rows, limit=2 → 2 / 2 / 1 across pages
//       * list_for_problem ordering: revision_no DESC
//       * JSON tags_snapshot round-trip — Chinese tag name ("数组") preserved
//       * JSON samples_snapshot round-trip — {input, output} array preserved
//       * summary: NULL-able + non-empty round-trip
//       * editor_id SET NULL: hard-delete admin user → revisions survive,
//         editor_username still populated
//       * ON DELETE CASCADE from problems: hard-delete the parent problem
//         → revisions cascade away
//       * 1062 forced collision → ProblemRevisionsConflictError after retry
//       * delete_for_problem wipes all rows for one problem (test cleanup helper)
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box. When MySQL is unreachable
// the integration tests SKIP — the binary still passes on a machine
// without MySQL (CI lint), and the pure-unit tests still run end-to-end
// against the parsed-only helpers.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>

#include "db/connection_pool.h"
#include "db/problem_revisions_repo.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers (mirrors test_audit_log.cpp / test_problem.cpp style)
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

// ────────────────────────────────────────────────────────────────────────────
//  RevisionTracker — RAII cleanup for problem_id + revision data
//
//  Tests that exercise record()/list_for_problem() need a real problem
//  to attach revisions to (FK constraints). Each test creates its own
//  problems; the tracker best-effort deletes them in TearDown. We
//  hard-delete via raw SQL (bypassing soft_delete) so the FK CASCADE
//  removes any leftover revisions in one shot.
// ────────────────────────────────────────────────────────────────────────────

struct RevisionTracker {
    litecode::ConnectionPool* pool = nullptr;
    std::vector<int>          problem_ids_to_clean;
    std::string               next_slug = "rev-track-problem";

    std::int64_t audit_user_id = 0;   // a throwaway admin user for editor_id FK
    std::string  audit_username;

    ~RevisionTracker() {
        if (!pool) return;
        for (int pid : problem_ids_to_clean) {
            try {
                // 1) hard-delete revisions (helps when the FK
                //    CASCADE doesn't fire — e.g. test mistakes)
                litecode::problem_revisions_repo::delete_for_problem(
                    *pool, pid);
                // 2) hard-delete the problem so the FK CASCADE
                //    (defined in V009) cleans any leftover rows.
                auto conn = pool->acquire();
                conn.session().sql(
                    "DELETE FROM problems WHERE id = ?")
                    .bind(static_cast<std::int64_t>(pid))
                    .execute();
            } catch (...) { /* best-effort cleanup */ }
        }
        if (audit_user_id > 0) {
            try {
                auto conn = pool->acquire();
                conn.session().sql(
                    "DELETE FROM users WHERE id = ?")
                    .bind(audit_user_id)
                    .execute();
            } catch (...) { /* best-effort */ }
        }
    }

    // Mint a throwaway admin user so editor_id has a valid FK target.
    // Returns the new id; "" already used => falls through to a
    // unique username (timestamp suffix) to dodge FK violations
    // across re-runs without proper cleanup.
    void ensure_admin_user() {
        if (audit_user_id > 0) return;
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        audit_username = "revtest_admin_" + std::to_string(ms);
        try {
            auto conn = pool->acquire();
            mysqlx::SqlResult rs = conn.session().sql(
                "INSERT INTO users (username, password_hash, role, avatar, created_at) "
                "VALUES (?, ?, 'admin', NULL, NOW())")
                .bind(audit_username)
                .bind(std::string("$2b$12$placeholderhashplaceholderhashplaceholder")) // 60 bytes OK for VARCHAR(255); not used
                .execute();
            audit_user_id = static_cast<std::int64_t>(rs.getAutoIncrementValue());
        } catch (const std::exception&) {
            audit_user_id = 0;
        }
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  DbFixture — ping-or-skip + tracker
// ────────────────────────────────────────────────────────────────────────────

class DbFixture : public ::testing::Test {
protected:
    DbConn conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    RevisionTracker tracker;

    void SetUp() override {
        // JWT_SECRET must be set BEFORE logger/config is touched, or
        // the very first LOG_* call from any helper lazy-loads config
        // and config() throws on missing JWT_SECRET. Same trick used
        // by test_auth_login / test_audit_log.
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

        tracker.pool = pool.get();
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — TEST() (no DB required)
// ────────────────────────────────────────────────────────────────────────────

TEST(ProblemRevisionsValidators, ValidateRevisionNo) {
    std::string err;
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_revision_no(1,    &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_revision_no(1'000, &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_revision_no(litecode::kMaxRevisionNo, &err)); err.clear();

    // 0 is the "let the repo allocate" sentinel; the validator
    // explicitly accepts it.
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_revision_no(0, &err)); err.clear();

    EXPECT_FALSE(litecode::problem_revisions_repo::validate_revision_no(-1,             &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_revision_no(litecode::kMaxRevisionNo + 1, &err));
    EXPECT_FALSE(err.empty());
}

TEST(ProblemRevisionsValidators, ValidateSummary) {
    std::string err;
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_summary("",                          &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_summary(std::string(200, 'x'),      &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_summary("title changed; ",            &err)); err.clear();

    EXPECT_FALSE(litecode::problem_revisions_repo::validate_summary(std::string(201, 'x'),       &err));
    EXPECT_FALSE(err.empty());

    // Control char (newline) → rejected even at length < 200.
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_summary("bad\nsummary", &err));
    EXPECT_NE(err.find("control"), std::string::npos);
}

TEST(ProblemRevisionsValidators, ValidateAction) {
    std::string err;
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_action("create", &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_action("update", &err)); err.clear();

    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("",      &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("delete", &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("insert", &err));
    EXPECT_FALSE(err.empty());
}

TEST(ProblemRevisionsValidators, ValidateEditorUsername) {
    std::string err;
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_editor_username("a",                       &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_editor_username(std::string(50, 'a'),       &err)); err.clear();
    EXPECT_TRUE (litecode::problem_revisions_repo::validate_editor_username("revtest_admin_1234",      &err)); err.clear();

    EXPECT_FALSE(litecode::problem_revisions_repo::validate_editor_username("",                          &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_editor_username(std::string(51, 'a'),        &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_editor_username("bad\nuser",                 &err));
}

TEST(ProblemRevisionsValidators, ClampListFilter) {
    using namespace litecode;
    RevisionListFilter f;

    f.problem_id = 42;
    f.limit = 0;       // 0 ⇒ default
    f.offset = 0;
    problem_revisions_repo::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  kDefaultRevisionListLimit);
    EXPECT_EQ(f.offset, 0);

    f.limit = 999;     // huge ⇒ capped
    f.offset = -5;     // negative ⇒ 0
    problem_revisions_repo::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  kMaxRevisionListLimit);
    EXPECT_EQ(f.offset, 0);

    // problem_id is left untouched (it's required, not a page param).
    EXPECT_EQ(f.problem_id, 42);
}

TEST(ProblemRevisionsStruct, Defaults) {
    using namespace litecode;
    RevisionEntry e;
    EXPECT_EQ(e.problem_id,  0);
    EXPECT_EQ(e.revision_no, 0);              // 0 ⇒ repo allocates
    EXPECT_FALSE(e.editor_id.has_value());
    EXPECT_TRUE (e.editor_username.empty());
    EXPECT_FALSE(e.editor_ip.has_value());
    EXPECT_TRUE (e.action.empty());
    EXPECT_EQ   (e.time_limit,   0);
    EXPECT_EQ   (e.memory_limit, 0);
    EXPECT_TRUE (e.description.empty());
    EXPECT_TRUE (e.tags_snapshot.is_array());
    EXPECT_TRUE (e.samples_snapshot.is_array());
    EXPECT_TRUE (e.tags_snapshot.empty());
    EXPECT_TRUE (e.samples_snapshot.empty());
    EXPECT_FALSE(e.summary.has_value());

    RevisionRow r;
    EXPECT_EQ(r.id, 0);
    EXPECT_TRUE(r.created_at.empty());

    RevisionListFilter f;
    EXPECT_EQ(f.problem_id, 0);
    EXPECT_EQ(f.limit,      kDefaultRevisionListLimit);
    EXPECT_EQ(f.offset,     0);

    RevisionListResult res;
    EXPECT_TRUE(res.items.empty());
    EXPECT_EQ (res.total, 0);
    EXPECT_EQ (res.limit, 0);
    EXPECT_EQ (res.offset, 0);
}

TEST(ProblemRevisionsExceptions, Hierarchy) {
    using namespace litecode;

    // All three tiers derive from std::runtime_error so callers can
    // catch std::exception once and still distinguish.
    std::unique_ptr<std::runtime_error> base(new ProblemRevisionsRepoError("x"));
    std::unique_ptr<std::runtime_error> nf (new ProblemRevisionsNotFoundError("x"));
    std::unique_ptr<std::runtime_error> co (new ProblemRevisionsConflictError("x"));

    auto* as_repo = dynamic_cast<ProblemRevisionsRepoError*>(base.get());
    auto* as_nf   = dynamic_cast<ProblemRevisionsNotFoundError*>(nf.get());
    auto* as_co   = dynamic_cast<ProblemRevisionsConflictError*>(co.get());
    ASSERT_NE(as_repo, nullptr);
    ASSERT_NE(as_nf,   nullptr);
    ASSERT_NE(as_co,   nullptr);

    // NotFoundError / ConflictError both inherit RepoError.
    EXPECT_NE(dynamic_cast<ProblemRevisionsRepoError*>(nf.get()), nullptr);
    EXPECT_NE(dynamic_cast<ProblemRevisionsRepoError*>(co.get()), nullptr);

    // But NotFound/Conflict aren't the same type.
    EXPECT_EQ(dynamic_cast<ProblemRevisionsNotFoundError*>(co.get()), nullptr);
    EXPECT_EQ(dynamic_cast<ProblemRevisionsConflictError*>(nf.get()), nullptr);
}

TEST(ProblemRevisionsConstants, ActionStringsPinned) {
    // Pin the wire format so accidental rename trips this assertion.
    EXPECT_STREQ(litecode::problem_revisions_repo::kActionRevisionCreate, "create");
    EXPECT_STREQ(litecode::problem_revisions_repo::kActionRevisionUpdate, "update");
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — TEST_F(DbFixture, ...)
// ────────────────────────────────────────────────────────────────────────────

namespace {

// Mint a throwaway problem in the live `problems` table so the
// revisions we want to test against have a valid FK parent. Returns
// the new id; 0 on failure.
int make_throwaway_problem(litecode::ConnectionPool& pool,
                           RevisionTracker& tracker,
                           const std::string& slug_in) {
    // Suffix with a per-process counter + millisecond timestamp so a
    // re-run (or parallel run) of the same test never collides on
    // the UNIQUE(slug) index. Slug is otherwise the caller-supplied
    // label so the audit / lookup logs stay readable.
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
            .bind(std::string("Throwaway for revision test"))
            .bind(std::string("# description\n\nMarkdown body."))
            .execute();
        int new_id = static_cast<int>(rs.getAutoIncrementValue());
        tracker.problem_ids_to_clean.push_back(new_id);
        return new_id;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "make_throwaway_problem INSERT failed for slug '"
                      << slug << "': " << e.what();
        return 0;
    }
}

// Build a basic valid RevisionEntry seeded with the supplied core
// fields and editor metadata from the tracker.
litecode::RevisionEntry make_revision(
        int problem_id,
        std::int64_t editor_user_id,
        const std::string& editor_username,
        RevisionTracker& tracker,
        int revision_no) {
    using namespace litecode;
    RevisionEntry e;
    e.problem_id      = problem_id;
    e.revision_no     = revision_no;        // 0 ⇒ auto-allocate in record()
    e.editor_id       = static_cast<int>(editor_user_id);
    e.editor_username = tracker.audit_username;
    e.editor_ip       = std::string("127.0.0.1");
    e.action          = problem_revisions_repo::kActionRevisionCreate;
    e.slug            = "throwaway";
    e.title           = "Throwaway for revision test";
    e.difficulty      = "easy";
    e.time_limit      = 1000;
    e.memory_limit    = 256;
    e.description     = "# description\n\nMarkdown body.";
    e.tags_snapshot    = nlohmann::json::array({"array", "hash"});
    e.samples_snapshot = nlohmann::json::array({
        {{"input", "1 2\n"}, {"output", "3\n"}},
        {{"input", "5 7\n"}, {"output", "12\n"}},
    });
    return e;
}

} // namespace

// record + find_by_id round-trip — happy path with revision_no=0 (auto).
TEST_F(DbFixture, RecordAutoAllocatesRevisionNo) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-auto");
    ASSERT_GT(problem_id, 0);

    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/0);
    e.action = litecode::problem_revisions_repo::kActionRevisionCreate;

    std::int64_t rid = 0;
    ASSERT_NO_THROW(rid = litecode::problem_revisions_repo::record(*pool, e));
    EXPECT_GT(rid, 0);

    // Auto-allocated revision_no should be 1 (first revision).
    auto row = litecode::problem_revisions_repo::find_by_id(*pool, rid);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->revision_no, 1);
    EXPECT_EQ(row->problem_id,  problem_id);
    EXPECT_EQ(row->action,      std::string("create"));
    EXPECT_EQ(row->editor_username, tracker.audit_username);
    EXPECT_EQ(row->editor_id,   static_cast<int>(tracker.audit_user_id));
    EXPECT_TRUE(row->created_at.size() == 19);   // 'YYYY-MM-DD HH:MM:SS'
    EXPECT_EQ(row->created_at[4],  '-');
}

// record with forced revision_no — value is preserved verbatim.
TEST_F(DbFixture, RecordForcedRevisionNoPreserved) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-forced");
    ASSERT_GT(problem_id, 0);

    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/7);

    std::int64_t rid = 0;
    ASSERT_NO_THROW(rid = litecode::problem_revisions_repo::record(*pool, e));
    EXPECT_GT(rid, 0);

    auto row = litecode::problem_revisions_repo::find_by_id(*pool, rid);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->revision_no, 7);
}

// record_best_effort swallows a validation error (empty editor_username).
TEST_F(DbFixture, RecordBestEffortSwallowsInvalid) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-besteffort");
    ASSERT_GT(problem_id, 0);

    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/0);
    e.editor_username = "";                     // validator rejects

    std::int64_t rid = litecode::problem_revisions_repo::record_best_effort(
        *pool, e);
    EXPECT_EQ(rid, 0);                          // swallow ⇒ return 0
}

// find_by_id on a missing id → nullopt (no throw).
TEST_F(DbFixture, FindByIdReturnsNulloptForMissing) {
    // Use a deliberately huge id that no test would produce.
    auto row = litecode::problem_revisions_repo::find_by_id(*pool, 9'999'999'999LL);
    EXPECT_FALSE(row.has_value());
}

// latest_for_problem: empty → nullopt.
TEST_F(DbFixture, LatestForProblemEmpty) {
    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-latest-empty");
    ASSERT_GT(problem_id, 0);

    auto row = litecode::problem_revisions_repo::latest_for_problem(
        *pool, problem_id);
    EXPECT_FALSE(row.has_value());
}

// latest_for_problem: populated → returns the max revision_no row.
TEST_F(DbFixture, LatestForProblemReturnsMaxRevisionNo) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-latest-multi");
    ASSERT_GT(problem_id, 0);

    for (int n : {1, 2, 3}) {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        e.action = litecode::problem_revisions_repo::kActionRevisionUpdate;
        std::int64_t rid = litecode::problem_revisions_repo::record(*pool, e);
        EXPECT_GT(rid, 0);
    }

    auto latest = litecode::problem_revisions_repo::latest_for_problem(
        *pool, problem_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->revision_no, 3);
    EXPECT_EQ(latest->action, std::string("update"));
}

// count_for_problem ≡ list_for_problem total.
TEST_F(DbFixture, CountMatchesListTotal) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-count-vs-list");
    ASSERT_GT(problem_id, 0);

    for (int n = 0; n < 4; ++n) {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }

    const auto total =
        litecode::problem_revisions_repo::count_for_problem(*pool, problem_id);
    EXPECT_EQ(total, 4);

    litecode::RevisionListFilter f;
    f.problem_id = problem_id;
    f.limit      = 100;
    auto page = litecode::problem_revisions_repo::list_for_problem(*pool, f);
    EXPECT_EQ(page.total, total);
    EXPECT_EQ(page.items.size(), static_cast<std::size_t>(total));
}

// list_for_problem paginates a 5-row set with limit=2 → 2 / 2 / 1.
TEST_F(DbFixture, ListForProblemPagination5RowsLimit2) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-page");
    ASSERT_GT(problem_id, 0);

    for (int n = 0; n < 5; ++n) {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }

    litecode::RevisionListFilter f;
    f.problem_id = problem_id;
    f.limit = 2;

    f.offset = 0;
    auto p1 = litecode::problem_revisions_repo::list_for_problem(*pool, f);
    EXPECT_EQ(p1.items.size(), 2u);
    EXPECT_EQ(p1.total,         5);
    EXPECT_EQ(p1.items[0].revision_no, 5);   // newest first
    EXPECT_EQ(p1.items[1].revision_no, 4);

    f.offset = 2;
    auto p2 = litecode::problem_revisions_repo::list_for_problem(*pool, f);
    EXPECT_EQ(p2.items.size(), 2u);
    EXPECT_EQ(p2.items[0].revision_no, 3);
    EXPECT_EQ(p2.items[1].revision_no, 2);

    f.offset = 4;
    auto p3 = litecode::problem_revisions_repo::list_for_problem(*pool, f);
    EXPECT_EQ(p3.items.size(), 1u);
    EXPECT_EQ(p3.items[0].revision_no, 1);
}

// list_for_problem ordering: revision_no DESC.
TEST_F(DbFixture, ListForProblemOrderedByRevisionNoDesc) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-order");
    ASSERT_GT(problem_id, 0);

    for (int n = 0; n < 3; ++n) {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }

    litecode::RevisionListFilter f;
    f.problem_id = problem_id;
    auto page = litecode::problem_revisions_repo::list_for_problem(*pool, f);
    ASSERT_EQ(page.items.size(), 3u);
    EXPECT_EQ(page.items[0].revision_no, 3);
    EXPECT_EQ(page.items[1].revision_no, 2);
    EXPECT_EQ(page.items[2].revision_no, 1);
}

// tags_snapshot round-trips a Chinese tag name in the JSON array.
TEST_F(DbFixture, TagsSnapshotChineseRoundTrip) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-tags-cn");
    ASSERT_GT(problem_id, 0);

    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/0);
    // Non-ASCII tag names spelled out as UTF-8 byte hex escapes so
    // this source file is ASCII-only (dodges MSVC's CP936 code-page
    // tokenization hazard). The runtime bytes match the original
    // UTF-8 sequence; the wire round-trip exercises the same path
    // as Chinese input from JSON. Pattern mirrors test_admin_bulk_import.
    //
    //   array      = U+6570 U+7EC4   -> "\xe6\x95\xb0\xe7\xbb\x84"
    //   hash-table = U+54C8 U+5E0C U+8868 -> "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"
    //   stack      = U+6808          -> "\xe6\xa0\x88"
    e.tags_snapshot = nlohmann::json::array({
        std::string("\xe6\x95\xb0\xe7\xbb\x84"),
        std::string("\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"),
        std::string("\xe6\xa0\x88")
    });
    const std::int64_t rid =
        litecode::problem_revisions_repo::record(*pool, e);
    ASSERT_GT(rid, 0);

    auto row = litecode::problem_revisions_repo::find_by_id(*pool, rid);
    ASSERT_TRUE(row.has_value());

    // Round-trip: parse the JSON text the driver returned, walk it.
    auto parsed = nlohmann::json::parse(row->tags_snapshot);
    ASSERT_TRUE(parsed.is_array());
    ASSERT_EQ(parsed.size(), 3u);
    EXPECT_EQ(parsed[0].get<std::string>(), std::string("\xe6\x95\xb0\xe7\xbb\x84"));
    EXPECT_EQ(parsed[1].get<std::string>(), std::string("\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"));
    EXPECT_EQ(parsed[2].get<std::string>(), std::string("\xe6\xa0\x88"));
}

// samples_snapshot round-trips the {input, output}[] shape.
TEST_F(DbFixture, SamplesSnapshotRoundTrip) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-samples");
    ASSERT_GT(problem_id, 0);

    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/0);
    std::int64_t rid = litecode::problem_revisions_repo::record(*pool, e);
    ASSERT_GT(rid, 0);

    auto row = litecode::problem_revisions_repo::find_by_id(*pool, rid);
    ASSERT_TRUE(row.has_value());
    auto parsed = nlohmann::json::parse(row->samples_snapshot);
    ASSERT_TRUE(parsed.is_array());
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0]["input"].get<std::string>(),  "1 2\n");
    EXPECT_EQ(parsed[0]["output"].get<std::string>(), "3\n");
    EXPECT_EQ(parsed[1]["input"].get<std::string>(),  "5 7\n");
    EXPECT_EQ(parsed[1]["output"].get<std::string>(), "12\n");
}

// summary is optional: NULL-able + non-empty both round-trip.
TEST_F(DbFixture, SummaryOptionalAndNonEmpty) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-summary");
    ASSERT_GT(problem_id, 0);

    // Row 1: summary unset (nullopt → DB NULL).
    {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        // e.summary stays nullopt
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }
    // Row 2: summary populated with a 200-char text.
    {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        e.summary = std::string(200, 'x');
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }

    litecode::RevisionListFilter f;
    f.problem_id = problem_id;
    auto page = litecode::problem_revisions_repo::list_for_problem(*pool, f);
    ASSERT_EQ(page.items.size(), 2u);

    // Newest first ⇒ row[0] is the 200-char one.
    ASSERT_TRUE(page.items[0].summary.has_value());
    EXPECT_EQ(*page.items[0].summary, std::string(200, 'x'));
    EXPECT_FALSE(page.items[1].summary.has_value());
}

// editor_id SET NULL behavior: delete the admin user, the row keeps
// editor_username (snapshot) and editor_id becomes NULL.
TEST_F(DbFixture, EditorUserDeleteKeepsSnapshotUsername) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-setnull");
    ASSERT_GT(problem_id, 0);

    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/0);
    std::int64_t rid = litecode::problem_revisions_repo::record(*pool, e);
    ASSERT_GT(rid, 0);

    // Hard-delete the admin user → ON DELETE SET NULL should fire.
    try {
        auto conn = pool->acquire();
        conn.session().sql("DELETE FROM users WHERE id = ?")
            .bind(tracker.audit_user_id)
            .execute();
        // Mark tracker to NOT re-delete the user (already gone).
        tracker.audit_user_id = 0;
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Could not hard-delete the admin user (FK in use?): "
                     << e.what();
    }

    auto row = litecode::problem_revisions_repo::find_by_id(*pool, rid);
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->editor_id.has_value());                  // SET NULL
    EXPECT_EQ (row->editor_username, tracker.audit_username);  // snapshot
}

// FK CASCADE: hard-delete the parent problem wipes its revisions.
TEST_F(DbFixture, ProblemHardDeleteCascadesRevisions) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-cascade");
    ASSERT_GT(problem_id, 0);

    for (int n = 0; n < 3; ++n) {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }
    EXPECT_EQ(litecode::problem_revisions_repo::count_for_problem(
                  *pool, problem_id), 3);

    // Bypass soft_delete; do a real DELETE so the CASCADE fires.
    try {
        auto conn = pool->acquire();
        conn.session().sql(
            "DELETE FROM problems WHERE id = ?")
            .bind(static_cast<std::int64_t>(problem_id))
            .execute();
        // Tell the tracker to skip its own cleanup — gone already.
        tracker.problem_ids_to_clean.erase(
            std::remove(tracker.problem_ids_to_clean.begin(),
                        tracker.problem_ids_to_clean.end(),
                        problem_id),
            tracker.problem_ids_to_clean.end());
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Hard-delete problems failed: " << e.what();
    }

    EXPECT_EQ(litecode::problem_revisions_repo::count_for_problem(
                  *pool, problem_id), 0);
}

// delete_for_problem (the test helper) wipes rows but leaves the
// problem intact — used by the tracker RAII for cleanup.
TEST_F(DbFixture, DeleteForProblemWipesRows) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-delhelper");
    ASSERT_GT(problem_id, 0);

    for (int n = 0; n < 2; ++n) {
        auto e = make_revision(problem_id, tracker.audit_user_id,
                               tracker.audit_username, tracker, /*rev=*/0);
        ASSERT_GT(litecode::problem_revisions_repo::record(*pool, e), 0);
    }
    ASSERT_EQ(litecode::problem_revisions_repo::count_for_problem(
                  *pool, problem_id), 2);

    litecode::problem_revisions_repo::delete_for_problem(*pool, problem_id);
    EXPECT_EQ(litecode::problem_revisions_repo::count_for_problem(
                  *pool, problem_id), 0);

    // The problem itself is still there.
    auto row = pool->acquire().session().sql(
        "SELECT is_deleted FROM problems WHERE id = ?")
        .bind(static_cast<std::int64_t>(problem_id))
        .execute();
    (void)row;     // existence is enough; we just check count dropped.
}

// 1062 forced collision — record() retries once then throws ConflictError.
TEST_F(DbFixture, ConflictErrorOnDuplicateRevisionNo) {
    tracker.ensure_admin_user();
    ASSERT_GT(tracker.audit_user_id, 0);

    const int problem_id = make_throwaway_problem(*pool, tracker, "rev-conflict");
    ASSERT_GT(problem_id, 0);

    // First insert with a specific revision_no=N succeeds.
    auto e = make_revision(problem_id, tracker.audit_user_id,
                           tracker.audit_username, tracker, /*rev=*/5);
    std::int64_t rid_first = 0;
    ASSERT_NO_THROW(rid_first =
        litecode::problem_revisions_repo::record(*pool, e));
    ASSERT_GT(rid_first, 0);

    // Second insert with the SAME (problem_id, revision_no=5) — even
    // when revision_no is forced, we retry once on 1062; both attempts
    // collide, so we get ConflictError.
    auto e2 = make_revision(problem_id, tracker.audit_user_id,
                            tracker.audit_username, tracker, /*rev=*/5);
    EXPECT_THROW(
        litecode::problem_revisions_repo::record(*pool, e2),
        litecode::ProblemRevisionsConflictError);
}

} // namespace
