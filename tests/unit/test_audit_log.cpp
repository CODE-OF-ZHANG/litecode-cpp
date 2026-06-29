// tests/unit/test_audit_log.cpp
//
// Unit + light integration tests for src/db/audit_log_repo.h.
//
// Two layers, matching the test_problem / test_tag pattern:
//
//   1) Pure unit tests (no MySQL required):
//        - validate_action: length bounds + control-char rejection
//        - validate_target_type / validate_target_id: length + ctrl char
//        - validate_ip: empty / oversized / control chars
//        - validate_datetime: length + ctrl char
//        - clamp_list_filter: pagination clamping
//        - AuditRow / AuditEntry / AuditListFilter default values
//        - Action constants pin the grep-friendly strings
//        - kMin/kMax length & limit constants pin SPEC §4.2d contract
//        - exception type hierarchy (AuditLogNotFoundError derives
//          from AuditLogRepoError)
//
//   2) Integration tests (require a reachable MySQL):
//        Writers:
//          - record() inserts a fully-populated row, returns id
//          - record() inserts a row with NULL fields (admin_id / ip /
//            target_type / target_id all unset)
//          - record() rejects bad action length (validator surfaces as
//            AuditLogRepoError)
//          - record_login_failure() round-trips through the table
//            with the Phase-2-shaped payload
//          - record_best_effort() does not throw even when the input
//            is bad (it LOG_WARNs and returns 0)
//        Readers:
//          - find_by_id round-trips the inserted row
//          - find_by_id returns nullopt for an unknown id
//          - list() returns paginated rows newest-first
//          - list() filters by admin_id / action / target_type /
//            target_id
//          - list() filters by since/until (created_at range)
//          - list() pagination clamps limit to kMaxAuditListLimit
//          - list() total matches count() for the same filter
//          - count() returns 0 for a filter that matches nothing
//        FK behavior:
//          - ON DELETE SET NULL: deleting the admin user keeps the
//            row, admin_id becomes NULL
//        Payload JSON:
//          - payload round-trips as JSON text (parseable back to
//            nlohmann::json with the same value)
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box (root/123456/litecode@127.0.0.1).
// If ping() fails, those tests are SKIPPED — the binary still passes on a
// machine without MySQL, which is what we want for CI lint jobs.
//
// Each integration test creates a throwaway admin user (so admin_id
// has a valid FK target — the FK is what enforces referential
// integrity for the audit_logs.admin_id column). Rows are left in
// place on purpose — the tests don't own the lifecycle and "cleanup
// on failure" would mask any real FK issue that surfaces later.

#include <gtest/gtest.h>

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
#include <nlohmann/json.hpp>

#include "config.h"
#include "db/connection_pool.h"
#include "db/audit_log_repo.h"

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
        // Lazy global logger bootstrap. record_best_effort() goes
        // through LOG_WARN on its error path; the logger pulls
        // JWT_SECRET from the env on first use, so we have to
        // seed it before the first audit-write happens. Mirrors
        // the pattern in test_auth_login.cpp.
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

// Generate a username unique to this test run. The username validator
// (referenced via user_repo) accepts [a-zA-Z0-9_]; we use a 'al-'
// (audit_log) prefix so the cleanup query can scope to our rows.
std::string fresh_username(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::string safe_tag(tag);
    for (char& c : safe_tag) {
        if (c == '-') c = '_';
    }
    return std::string("al-") + safe_tag + "_" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "_" + std::to_string(n);
}

// Best-effort cleanup of every audit row we created during a test,
// plus the throwaway admin user. We don't fail the test if cleanup
// fails — the next test's fresh_username avoids collisions anyway.
struct CleanupTracker {
    litecode::ConnectionPool*       pool = nullptr;
    std::vector<std::int64_t>       created_row_ids;
    std::vector<std::string>        created_admin_usernames;

    void add_row(std::int64_t id)            { created_row_ids.push_back(id); }
    void add_admin(std::string username)     { created_admin_usernames.push_back(std::move(username)); }
};

void cleanup_audit(CleanupTracker& tracker) {
    if (!tracker.pool) return;
    try {
        auto conn = tracker.pool->acquire();
        for (auto id : tracker.created_row_ids) {
            try { conn.execute("DELETE FROM audit_logs WHERE id = ?", id); }
            catch (...) {}
        }
        for (const auto& u : tracker.created_admin_usernames) {
            try { conn.execute("DELETE FROM users WHERE username = ?", u); }
            catch (...) {}
        }
    } catch (...) {}
}

// RAII helper that records the audit rows + admin usernames a test
// created and drops them in TearDown. Tests append via add_row /
// add_admin.
class AuditTracker {
public:
    explicit AuditTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~AuditTracker() {
        tracker_.pool = pool_;
        cleanup_audit(tracker_);
    }

    void add_row(std::int64_t id)         { tracker_.add_row(id); }
    void add_admin(std::string username)  { tracker_.add_admin(std::move(username)); }

private:
    litecode::ConnectionPool*       pool_;
    CleanupTracker                  tracker_;
};

// Create a throwaway admin user with the given username and a
// pre-baked password_hash that's NEVER verified (the column is just
// a placeholder so the FK from audit_logs.admin_id can resolve).
// Returns the new user id (>= 1) on success, throws on failure.
//
// We deliberately use INSERT directly instead of going through
// user_repo::create_user to keep this test independent of user_repo's
// bcrypt requirement — user_repo would call hash_password() which is
// a slow bcrypt cost=12 op; that's irrelevant to what we're
// testing here.
int insert_admin_user(litecode::ConnectionPool& pool,
                      const std::string& username,
                      const std::string& role) {
    auto conn = pool.acquire();
    // We rely on the V007__add_unique_email migration making the
    // email column nullable+unique (NULLS NOT DISTINCT). To dodge
    // any per-test collision on email, we leave it NULL.
    auto rs = conn.execute(
        "INSERT INTO users (username, password_hash, role) "
        "VALUES (?, ?, ?)",
        username,
        // bcrypt cost=12 hash of "x" — never verified in these tests
        "$2b$12$abcdefghijklmnopqrstuOuqZ0vJ5nQ6c6eX8aN8w3y0vW7iV2aO4y",
        role);
    return static_cast<int>(rs.getAutoIncrementValue());
}

// Build a fully-populated AuditEntry with sane defaults. Tests
// override individual fields when they need to exercise an edge
// case (no admin_id, no payload, etc.).
litecode::AuditEntry make_entry(int admin_id,
                                const std::string& action,
                                const std::string& target_type = "problem",
                                const std::string& target_id   = "two-sum") {
    litecode::AuditEntry e;
    e.admin_id   = admin_id;
    e.action     = action;
    e.target_type= target_type;
    e.target_id  = target_id;
    e.payload    = {{"slug", target_id},
                    {"note", "integration-test"}};
    e.ip         = std::string("127.0.0.1");
    return e;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — no DB I/O
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateAction, AcceptsAllowedLength) {
    std::string err;
    EXPECT_TRUE (litecode::validate_action("problem.create",  &err));
    EXPECT_TRUE (litecode::validate_action("a",               &err));
    EXPECT_TRUE (litecode::validate_action(std::string(50, 'x'), &err));
    EXPECT_FALSE(litecode::validate_action("",                &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_action(std::string(51, 'x'), &err)); EXPECT_FALSE(err.empty());
}

TEST(ValidateAction, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_action("problem\ncreate",   &err));
    EXPECT_FALSE(litecode::validate_action("problem\tcreate",   &err));
    // The std::string explicit-length ctor is required: the plain
    // std::string("a\0b") form would stop at the embedded NUL
    // (strlen), producing a 1-char string instead of the 3-char
    // "a\0b" we want to assert against.
    EXPECT_FALSE(litecode::validate_action(std::string("a\0b", 3), &err));
}

TEST(ValidateTargetType, AcceptsAllowedLength) {
    std::string err;
    EXPECT_TRUE (litecode::validate_target_type("problem",   &err));
    EXPECT_TRUE (litecode::validate_target_type("",          &err));   // NULL-able
    EXPECT_TRUE (litecode::validate_target_type(std::string(50, 'a'), &err));
    EXPECT_FALSE(litecode::validate_target_type(std::string(51, 'a'), &err)); EXPECT_FALSE(err.empty());
}

TEST(ValidateTargetType, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_target_type("prob\nlem", &err));
}

TEST(ValidateTargetId, AcceptsAllowedLength) {
    std::string err;
    EXPECT_TRUE (litecode::validate_target_id("two-sum",       &err));
    EXPECT_TRUE (litecode::validate_target_id("12345",         &err));
    EXPECT_TRUE (litecode::validate_target_id("",              &err));   // NULL-able
    EXPECT_TRUE (litecode::validate_target_id(std::string(100, 'x'), &err));
    EXPECT_FALSE(litecode::validate_target_id(std::string(101, 'x'), &err)); EXPECT_FALSE(err.empty());
}

TEST(ValidateTargetId, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_target_id("abc\n",     &err));
    EXPECT_FALSE(litecode::validate_target_id(std::string("a\0b", 3), &err));
}

TEST(ValidateIp, AcceptsCommonAddresses) {
    std::string err;
    EXPECT_TRUE (litecode::validate_ip("127.0.0.1",             &err));
    EXPECT_TRUE (litecode::validate_ip("::1",                   &err));
    EXPECT_TRUE (litecode::validate_ip("2001:db8::1",           &err));
    EXPECT_TRUE (litecode::validate_ip(std::string(45, 'a'),     &err));
    EXPECT_FALSE(litecode::validate_ip("",                      &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_ip(std::string(46, 'a'),    &err)); EXPECT_FALSE(err.empty());
}

TEST(ValidateIp, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_ip("127.0.0.1\n",  &err));
    EXPECT_FALSE(litecode::validate_ip(std::string("a\0b", 3), &err));
}

TEST(ValidateDatetime, AcceptsAllowedLength) {
    std::string err;
    EXPECT_TRUE (litecode::validate_datetime("2026-06-29",                  &err));
    EXPECT_TRUE (litecode::validate_datetime("2026-06-29 12:34:56",          &err));
    EXPECT_FALSE(litecode::validate_datetime("",                             &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_datetime("2026-06-29 12:34:56.789",      &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_datetime(std::string(20, 'x'),           &err)); EXPECT_FALSE(err.empty());
}

TEST(ValidateDatetime, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_datetime("2026-06-29\n",   &err));
}

TEST(ClampListFilter, ClampsBadInputs) {
    litecode::AuditListFilter f;

    // Default values are fine.
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  litecode::kDefaultAuditListLimit);
    EXPECT_EQ(f.offset, 0);

    // Negative / zero limit → default.
    f.limit  = 0;
    f.offset = 0;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultAuditListLimit);

    f.limit  = -7;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultAuditListLimit);

    // Excessive limit → kMaxAuditListLimit.
    f.limit  = 9999;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kMaxAuditListLimit);

    // Negative offset → 0.
    f.limit  = 50;
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
    // SPEC §4.2d: action VARCHAR(50), target_type VARCHAR(50),
    // target_id VARCHAR(100), ip VARCHAR(45). The kMax constants
    // must match these column widths exactly.
    EXPECT_EQ(litecode::kMinActionLength,       1u);
    EXPECT_EQ(litecode::kMaxActionLength,       50u);
    EXPECT_EQ(litecode::kMaxTargetTypeLength,   50u);
    EXPECT_EQ(litecode::kMaxTargetIdLength,     100u);
    EXPECT_EQ(litecode::kMaxIpLength,           45u);
    EXPECT_EQ(litecode::kDefaultAuditListLimit, 20);
    EXPECT_EQ(litecode::kMaxAuditListLimit,     100);
}

TEST(ActionConstants, MatchSpec) {
    // The action constants are grep-friendly surface strings; pin
    // them so a typo in the constant surfaces as a failing test
    // rather than as a security-trail gap.
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemCreate,
                 "problem.create");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemUpdate,
                 "problem.update");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemDelete,
                 "problem.delete");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemRestore,
                 "problem.restore");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemBulkImport,
                 "problem.bulk_import");
    EXPECT_STREQ(litecode::audit_log_repo::kActionUserRoleChange,
                 "user.role_change");
    EXPECT_STREQ(litecode::audit_log_repo::kActionUserPasswordChange,
                 "user.password_change");
    EXPECT_STREQ(litecode::audit_log_repo::kActionLoginFailure,
                 "auth.login_failure");
}

TEST(AuditRowDefaults, AreZeroOrEmpty) {
    litecode::AuditRow r;
    EXPECT_EQ(r.id, 0);
    EXPECT_FALSE(r.admin_id.has_value());
    EXPECT_TRUE (r.action.empty());
    EXPECT_FALSE(r.target_type.has_value());
    EXPECT_FALSE(r.target_id.has_value());
    EXPECT_FALSE(r.payload.has_value());
    EXPECT_FALSE(r.ip.has_value());
    EXPECT_TRUE (r.created_at.empty());

    litecode::AuditEntry e;
    EXPECT_FALSE(e.admin_id.has_value());
    EXPECT_TRUE (e.action.empty());
    EXPECT_FALSE(e.target_type.has_value());
    EXPECT_FALSE(e.target_id.has_value());
    // payload defaults to an empty JSON object so callers that
    // don't care about details don't have to construct one.
    EXPECT_TRUE(e.payload.is_object());
    EXPECT_TRUE(e.payload.empty());
    EXPECT_FALSE(e.ip.has_value());

    litecode::AuditListFilter f;
    EXPECT_FALSE(f.admin_id.has_value());
    EXPECT_FALSE(f.action.has_value());
    EXPECT_FALSE(f.target_type.has_value());
    EXPECT_FALSE(f.target_id.has_value());
    EXPECT_FALSE(f.since.has_value());
    EXPECT_FALSE(f.until.has_value());
    EXPECT_EQ   (f.limit,  20);
    EXPECT_EQ   (f.offset, 0);
}

TEST(ExceptionHierarchy, TypedErrorsDeriveFromRepoError) {
    litecode::AuditLogNotFoundError nf("nf");
    EXPECT_NE(dynamic_cast<const litecode::AuditLogRepoError*>(&nf), nullptr);
    EXPECT_THROW(throw litecode::AuditLogNotFoundError("x"),
                 litecode::AuditLogRepoError);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — skipped if MySQL isn't reachable
// ────────────────────────────────────────────────────────────────────────────

TEST_F(DbFixture, RecordInsertsFullyPopulatedRow) {
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("rec_full");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    tracker.add_admin(username);

    litecode::AuditEntry e = make_entry(admin_id,
                                        litecode::audit_log_repo::kActionProblemCreate,
                                        "problem", "audit-test-create");
    e.payload = {{"slug", "audit-test-create"}, {"hard_delete", false}};

    const std::int64_t id = litecode::audit_log_repo::record(*pool, e);
    EXPECT_GT(id, 0);
    tracker.add_row(id);

    const auto loaded = litecode::audit_log_repo::find_by_id(*pool, id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, id);
    ASSERT_TRUE(loaded->admin_id.has_value());
    EXPECT_EQ(*loaded->admin_id, admin_id);
    EXPECT_EQ(loaded->action,      litecode::audit_log_repo::kActionProblemCreate);
    ASSERT_TRUE(loaded->target_type.has_value());
    EXPECT_EQ(*loaded->target_type, "problem");
    ASSERT_TRUE(loaded->target_id.has_value());
    EXPECT_EQ(*loaded->target_id,   "audit-test-create");
    ASSERT_TRUE(loaded->payload.has_value());
    // Round-trip the JSON payload — the stored text must be parseable
    // back to the same value.
    {
        const auto j = nlohmann::json::parse(*loaded->payload);
        EXPECT_EQ(j.at("slug").get<std::string>(), "audit-test-create");
        EXPECT_EQ(j.at("hard_delete").get<bool>(), false);
    }
    ASSERT_TRUE(loaded->ip.has_value());
    EXPECT_EQ(*loaded->ip, "127.0.0.1");
    EXPECT_FALSE(loaded->created_at.empty());
}

TEST_F(DbFixture, RecordInsertsRowWithNullableFieldsUnset) {
    AuditTracker tracker(pool.get());

    litecode::AuditEntry e;
    e.action = litecode::audit_log_repo::kActionLoginFailure;
    e.target_type = std::string("user");
    e.target_id   = std::string("alice");
    e.payload     = {{"consecutive_failures", 5}};
    // admin_id and ip intentionally left unset

    const std::int64_t id = litecode::audit_log_repo::record(*pool, e);
    EXPECT_GT(id, 0);
    tracker.add_row(id);

    const auto loaded = litecode::audit_log_repo::find_by_id(*pool, id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->admin_id.has_value());
    EXPECT_EQ   (loaded->action, litecode::audit_log_repo::kActionLoginFailure);
    ASSERT_TRUE(loaded->target_type.has_value());
    EXPECT_EQ   (*loaded->target_type, "user");
    ASSERT_TRUE(loaded->target_id.has_value());
    EXPECT_EQ   (*loaded->target_id,   "alice");
    EXPECT_FALSE(loaded->ip.has_value());
}

TEST_F(DbFixture, RecordRejectsBadInputs) {
    // All four validators must reject their bad input. record() must
    // surface each as an AuditLogRepoError so the route handler can
    // return 500 cleanly.
    EXPECT_THROW({
        litecode::AuditEntry e;
        e.action = "";                // empty action
        litecode::audit_log_repo::record(*pool, e);
    }, litecode::AuditLogRepoError);

    EXPECT_THROW({
        litecode::AuditEntry e;
        e.action      = "x";
        e.target_type = std::string(51, 'a');   // oversized target_type
        litecode::audit_log_repo::record(*pool, e);
    }, litecode::AuditLogRepoError);

    EXPECT_THROW({
        litecode::AuditEntry e;
        e.action    = "x";
        e.target_id = std::string(101, 'a');    // oversized target_id
        litecode::audit_log_repo::record(*pool, e);
    }, litecode::AuditLogRepoError);

    EXPECT_THROW({
        litecode::AuditEntry e;
        e.action = "x";
        e.ip     = std::string(46, 'a');          // oversized ip
        litecode::audit_log_repo::record(*pool, e);
    }, litecode::AuditLogRepoError);

    EXPECT_THROW({
        litecode::AuditEntry e;
        e.action = std::string("problem\ncreate");   // newline in action
        litecode::audit_log_repo::record(*pool, e);
    }, litecode::AuditLogRepoError);
}

TEST_F(DbFixture, RecordLoginFailureRoundTrips) {
    AuditTracker tracker(pool.get());

    litecode::audit_log_repo::record_login_failure(
        *pool, /*username=*/ "alice", /*ip=*/ "10.0.0.5",
        /*consecutive_failures=*/ 5);

    // Find the row we just wrote. We can't predict the id because
    // audit_logs is append-only and other tests may have written
    // in between — filter on the (admin_id IS NULL, action, target_id)
    // tuple that record_login_failure produces.
    litecode::AuditListFilter f;
    f.limit     = 100;
    f.action    = std::string(litecode::audit_log_repo::kActionLoginFailure);
    f.target_id = std::string("alice");

    const auto page = litecode::audit_log_repo::list(*pool, f);
    ASSERT_FALSE(page.items.empty());
    const auto& row = page.items.front();
    EXPECT_FALSE(row.admin_id.has_value());
    EXPECT_EQ   (row.action, litecode::audit_log_repo::kActionLoginFailure);
    ASSERT_TRUE(row.target_id.has_value());
    EXPECT_EQ   (*row.target_id, "alice");
    ASSERT_TRUE(row.payload.has_value());
    const auto j = nlohmann::json::parse(*row.payload);
    EXPECT_EQ(j.at("consecutive_failures").get<int>(), 5);
    ASSERT_TRUE(row.ip.has_value());
    EXPECT_EQ(*row.ip, "10.0.0.5");

    // Cleanup: drop every login_failure row we just matched. (We
    // tracked the page via the filter, not the id, so cleanup goes
    // through the row id.)
    for (const auto& r : page.items) {
        tracker.add_row(r.id);
    }
}

TEST_F(DbFixture, RecordBestEffortSwallowsErrors) {
    // record_best_effort must not throw even when the input is bad
    // (it LOG_WARNs and returns 0). The strict record() does throw
    // on the same input — see RecordRejectsBadInputs above.
    litecode::AuditEntry e;
    e.action = "";   // empty action — would throw from record()

    EXPECT_NO_THROW({
        const std::int64_t id =
            litecode::audit_log_repo::record_best_effort(*pool, e);
        EXPECT_EQ(id, 0);
    });
}

TEST_F(DbFixture, FindByIdReturnsNulloptForUnknown) {
    // Use a clearly-unreachable id (BIGINT max). No row matches.
    const auto missing = litecode::audit_log_repo::find_by_id(
        *pool, /*id=*/ static_cast<std::int64_t>(9'223'372'036'854'775'000LL));
    EXPECT_FALSE(missing.has_value());
}

TEST_F(DbFixture, ListReturnsPaginatedRowsNewestFirst) {
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("list_order");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    tracker.add_admin(username);

    // Insert 5 rows with distinct actions so we can identify them.
    std::vector<std::int64_t> ids;
    for (int i = 0; i < 5; ++i) {
        litecode::AuditEntry e = make_entry(
            admin_id,
            std::string("test.seq.") + std::to_string(i),
            "problem",
            std::string("list-order-") + std::to_string(i));
        const std::int64_t id = litecode::audit_log_repo::record(*pool, e);
        ASSERT_GT(id, 0);
        tracker.add_row(id);
        ids.push_back(id);
        // Tiny pause so the AUTO_INCREMENT id + 1-second resolution
        // created_at order matches the insertion order. MySQL
        // DATETIME is second-resolution; we need the rows to fall
        // in distinct seconds for the DESC ordering test to be
        // deterministic.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }

    // Filter by admin_id only — we want all 5 rows, then assert
    // the DESC ordering by walking the page client-side.
    litecode::AuditListFilter f;
    f.admin_id = admin_id;
    f.limit    = 100;

    const auto page = litecode::audit_log_repo::list(*pool, f);

    // Filter to just our sequence: walk the page and pick the
    // test.seq.* rows.
    std::vector<std::int64_t> our_ids_in_order;
    for (const auto& row : page.items) {
        if (row.action.rfind("test.seq.", 0) == 0) {
            our_ids_in_order.push_back(row.id);
        }
    }
    ASSERT_EQ(our_ids_in_order.size(), 5u);

    // Newest first: ids should be in DESC order. The last id we
    // inserted (ids[4]) is the newest, so it should be the first
    // element of our_ids_in_order.
    EXPECT_EQ(our_ids_in_order.front(), ids.back());
    EXPECT_EQ(our_ids_in_order.back(),  ids.front());
}

TEST_F(DbFixture, ListFiltersByActionAndTarget) {
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("list_filt");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    tracker.add_admin(username);

    // Insert two distinct actions with the same admin_id.
    litecode::AuditEntry e1 = make_entry(admin_id,
                                        litecode::audit_log_repo::kActionProblemCreate,
                                        "problem", "filt-create");
    litecode::AuditEntry e2 = make_entry(admin_id,
                                        litecode::audit_log_repo::kActionProblemDelete,
                                        "problem", "filt-delete");
    const std::int64_t id1 = litecode::audit_log_repo::record(*pool, e1);
    const std::int64_t id2 = litecode::audit_log_repo::record(*pool, e2);
    ASSERT_GT(id1, 0);
    ASSERT_GT(id2, 0);
    tracker.add_row(id1);
    tracker.add_row(id2);

    // Filter by action=problem.create, target_id=filt-create → id1 only.
    {
        litecode::AuditListFilter f;
        f.admin_id   = admin_id;
        f.action     = std::string(litecode::audit_log_repo::kActionProblemCreate);
        f.target_id  = std::string("filt-create");
        f.limit      = 100;

        const auto page = litecode::audit_log_repo::list(*pool, f);
        bool found_id1 = false, found_id2 = false;
        for (const auto& r : page.items) {
            if (r.id == id1) found_id1 = true;
            if (r.id == id2) found_id2 = true;
        }
        EXPECT_TRUE (found_id1);
        EXPECT_FALSE(found_id2);
    }

    // Filter by action=problem.delete, target_id=filt-delete → id2 only.
    {
        litecode::AuditListFilter f;
        f.admin_id   = admin_id;
        f.action     = std::string(litecode::audit_log_repo::kActionProblemDelete);
        f.target_id  = std::string("filt-delete");
        f.limit      = 100;

        const auto page = litecode::audit_log_repo::list(*pool, f);
        bool found_id1 = false, found_id2 = false;
        for (const auto& r : page.items) {
            if (r.id == id1) found_id1 = true;
            if (r.id == id2) found_id2 = true;
        }
        EXPECT_FALSE(found_id1);
        EXPECT_TRUE (found_id2);
    }

    // Filter by admin_id only — both should be in the result.
    {
        litecode::AuditListFilter f;
        f.admin_id = admin_id;
        f.limit    = 100;

        const auto page = litecode::audit_log_repo::list(*pool, f);
        bool found_id1 = false, found_id2 = false;
        for (const auto& r : page.items) {
            if (r.id == id1) found_id1 = true;
            if (r.id == id2) found_id2 = true;
        }
        EXPECT_TRUE(found_id1);
        EXPECT_TRUE(found_id2);
    }
}

TEST_F(DbFixture, ListFiltersBySinceAndUntil) {
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("list_range");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    tracker.add_admin(username);

    // Insert one row.
    litecode::AuditEntry e = make_entry(admin_id,
                                        "test.range.insert",
                                        "problem", "range-row");
    const std::int64_t id = litecode::audit_log_repo::record(*pool, e);
    ASSERT_GT(id, 0);
    tracker.add_row(id);

    // since=tomorrow → 0 matches.
    {
        litecode::AuditListFilter f;
        f.since = std::string("2999-01-01 00:00:00");
        f.limit = 100;
        const auto page = litecode::audit_log_repo::list(*pool, f);
        bool found = false;
        for (const auto& r : page.items) {
            if (r.id == id) found = true;
        }
        EXPECT_FALSE(found);
    }

    // since=yesterday, until=tomorrow → matches.
    {
        litecode::AuditListFilter f;
        f.since = std::string("2020-01-01 00:00:00");
        f.until = std::string("2999-01-01 00:00:00");
        f.limit = 100;
        const auto page = litecode::audit_log_repo::list(*pool, f);
        bool found = false;
        for (const auto& r : page.items) {
            if (r.id == id) found = true;
        }
        EXPECT_TRUE(found);
    }

    // until=yesterday → 0 matches.
    {
        litecode::AuditListFilter f;
        f.until = std::string("2020-01-01 00:00:00");
        f.limit = 100;
        const auto page = litecode::audit_log_repo::list(*pool, f);
        bool found = false;
        for (const auto& r : page.items) {
            if (r.id == id) found = true;
        }
        EXPECT_FALSE(found);
    }
}

TEST_F(DbFixture, ListPaginationClampsAndPages) {
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("list_page");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    tracker.add_admin(username);

    // Insert 3 rows.
    std::vector<std::int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        litecode::AuditEntry e = make_entry(
            admin_id,
            std::string("test.page.") + std::to_string(i),
            "problem",
            std::string("page-row-") + std::to_string(i));
        const std::int64_t id = litecode::audit_log_repo::record(*pool, e);
        ASSERT_GT(id, 0);
        tracker.add_row(id);
        ids.push_back(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }

    // limit=9999 clamps to kMaxAuditListLimit.
    {
        litecode::AuditListFilter f;
        f.admin_id = admin_id;
        f.limit    = 9999;
        const auto clamped = litecode::audit_log_repo::list(*pool, f);
        EXPECT_EQ(clamped.limit, litecode::kMaxAuditListLimit);
    }

    // Page through the rows for this admin_id only.
    litecode::AuditListFilter p1;
    p1.admin_id = admin_id;
    p1.limit    = 1;
    p1.offset   = 0;
    const auto page1 = litecode::audit_log_repo::list(*pool, p1);

    litecode::AuditListFilter p2;
    p2.admin_id = admin_id;
    p2.limit    = 1;
    p2.offset   = 1;
    const auto page2 = litecode::audit_log_repo::list(*pool, p2);

    // Both pages should have at least one row (we can't guarantee
    // exact counts because other tests might also have written
    // audit rows under different admin_ids in the same fixture run).
    EXPECT_GE(page1.items.size(), 1u);
    EXPECT_GE(page2.items.size(), 1u);

    // The first row of page1 and page2 should not collide when
    // both pages have results.
    if (!page1.items.empty() && !page2.items.empty()) {
        EXPECT_NE(page1.items.front().id, page2.items.front().id);
    }
}

TEST_F(DbFixture, CountReturnsZeroForUnmatchedFilter) {
    // A filter that can't match any row should return 0.
    litecode::AuditListFilter f;
    f.action = std::string("__definitely_not_a_real_action__");
    EXPECT_EQ(litecode::audit_log_repo::count(*pool, f), 0);
}

TEST_F(DbFixture, ListTotalMatchesCountForSameFilter) {
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("total_match");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    tracker.add_admin(username);

    // Insert 4 rows.
    for (int i = 0; i < 4; ++i) {
        litecode::AuditEntry e = make_entry(
            admin_id,
            std::string("test.total.") + std::to_string(i),
            "problem",
            std::string("total-row-") + std::to_string(i));
        const std::int64_t id = litecode::audit_log_repo::record(*pool, e);
        ASSERT_GT(id, 0);
        tracker.add_row(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }

    litecode::AuditListFilter f;
    f.admin_id = admin_id;
    f.limit    = 100;

    const std::int64_t counted = litecode::audit_log_repo::count(*pool, f);
    const auto page = litecode::audit_log_repo::list(*pool, f);
    EXPECT_EQ(page.total, counted);
}

TEST_F(DbFixture, AuditRowSurvivesAdminDeletion) {
    // ON DELETE SET NULL on audit_logs.admin_id: deleting the
    // referenced admin user keeps the row but sets admin_id = NULL.
    AuditTracker tracker(pool.get());

    const std::string username = fresh_username("fk_set_null");
    const int admin_id = insert_admin_user(*pool, username, "admin");
    ASSERT_GT(admin_id, 0);
    // Intentionally NOT adding username to tracker.add_admin — we
    // delete the user here as part of the test.
    std::int64_t row_id = 0;
    {
        litecode::AuditEntry e = make_entry(
            admin_id,
            litecode::audit_log_repo::kActionUserRoleChange,
            "user",
            std::to_string(admin_id));
        row_id = litecode::audit_log_repo::record(*pool, e);
        ASSERT_GT(row_id, 0);
        tracker.add_row(row_id);
    }

    // Delete the admin user. The FK ON DELETE SET NULL should
    // keep the audit row but set admin_id to NULL.
    {
        auto conn = pool->acquire();
        conn.execute("DELETE FROM users WHERE id = ?", admin_id);
    }

    // The audit row should still be present, with admin_id = NULL.
    const auto loaded = litecode::audit_log_repo::find_by_id(*pool, row_id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->admin_id.has_value());
    EXPECT_EQ   (loaded->action, litecode::audit_log_repo::kActionUserRoleChange);
    ASSERT_TRUE(loaded->target_id.has_value());
    EXPECT_EQ   (*loaded->target_id, std::to_string(admin_id));
}