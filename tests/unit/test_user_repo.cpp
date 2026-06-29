// tests/unit/test_user_repo.cpp
//
// Unit + light integration tests for src/db/user_repo.h.
//
// Two layers, matching the test_connection_pool pattern:
//
//   1) Pure unit tests (no MySQL required):
//        - validate_username shape, length, charset rules
//        - validate_email shape, length, single-@, dot-in-domain
//        - is_valid_username_char ASCII predicate
//        - kMin/kMax length constants pin SPEC §4.1 contract
//
//   2) Integration tests (require a reachable MySQL):
//        - create_user inserts a row, returns the new id
//        - create_user returns 0 on duplicate username (409 path)
//        - create_user returns 0 on duplicate email (409 path)
//        - username_exists / email_exists round-trip
//        - find_by_username returns the inserted row with all columns
//        - find_by_id matches find_by_username
//        - find_by_username returns nullopt for an unknown user
//        - update_role flips role + returns true on hit, false on miss
//        - update_last_login stamps last_login + last_login_ip
//        - the optional email column is stored as NULL when omitted
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box (root/123456/litecode@127.0.0.1).
// If ping() fails, those tests are SKIPPED — the binary still passes on a
// machine without MySQL, which is what we want for CI lint jobs.
//
// Each integration test uses a unique username (timestamp + counter)
// to dodge collisions across parallel test runs.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <mysqlx/xdevapi.h>

#include "auth/password_hash.h"
#include "config.h"
#include "db/connection_pool.h"
#include "db/user_repo.h"

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

// Generate a username unique to this test run. Counts up so re-running
// a single test in isolation doesn't collide with itself, and tags with
// "ur_" (user_repo) so it's obvious in the DB which test left the row
// (we leave rows on purpose — the tests don't own the lifecycle).
std::string fresh_username(const char* tag) {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return std::string("ur_") + tag + "_" +
           std::to_string(static_cast<long long>(
               std::chrono::system_clock::now()
                   .time_since_epoch().count())) +
           "_" + std::to_string(n);
}

// Best-effort cleanup of every username created during the test. We
// don't fail the test if cleanup fails — the next test's fresh_username
// avoids collisions anyway.
void cleanup_usernames(litecode::ConnectionPool& pool,
                       const std::vector<std::string>& usernames) {
    if (usernames.empty()) return;
    try {
        auto conn = pool.acquire();
        for (const auto& u : usernames) {
            try { conn.execute("DELETE FROM users WHERE username = ?", u); }
            catch (...) {}
        }
    } catch (...) {}
}

// RAII helper that records the usernames a test created and drops
// them in TearDown. Tests append via add().
class UsernameTracker {
public:
    explicit UsernameTracker(litecode::ConnectionPool* p) : pool_(p) {}
    ~UsernameTracker() { if (pool_) cleanup_usernames(*pool_, created_); }

    void add(std::string u) { created_.push_back(std::move(u)); }

private:
    litecode::ConnectionPool*       pool_;
    std::vector<std::string>        created_;
};

} // namespace

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — no DB I/O
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateUsername, AcceptsAllowedShapes) {
    EXPECT_TRUE (litecode::validate_username("alice"));
    EXPECT_TRUE (litecode::validate_username("Alice_42"));
    EXPECT_TRUE (litecode::validate_username("bob.smith"));
    EXPECT_TRUE (litecode::validate_username("user-007"));
    EXPECT_TRUE (litecode::validate_username("a_b"));
    EXPECT_TRUE (litecode::validate_username("abc-123.xyz"));
}

TEST(ValidateUsername, RejectsBadLength) {
    std::string err;
    EXPECT_FALSE(litecode::validate_username("",        &err)); EXPECT_FALSE(err.empty());
    EXPECT_FALSE(litecode::validate_username("ab",       &err));
    EXPECT_FALSE(litecode::validate_username(std::string(51, 'a'), &err));
    EXPECT_TRUE (litecode::validate_username(std::string(50, 'a')));
}

TEST(ValidateUsername, RejectsLeadingOrTrailingDotOrHyphen) {
    EXPECT_FALSE(litecode::validate_username(".alice"));
    EXPECT_FALSE(litecode::validate_username("alice."));
    EXPECT_FALSE(litecode::validate_username("-alice"));
    EXPECT_FALSE(litecode::validate_username("alice-"));
}

TEST(ValidateUsername, RejectsInvalidCharacters) {
    EXPECT_FALSE(litecode::validate_username("alice@bob"));
    EXPECT_FALSE(litecode::validate_username("alice bob"));
    EXPECT_FALSE(litecode::validate_username("alice/bob"));
    EXPECT_FALSE(litecode::validate_username("alice!"));
    EXPECT_FALSE(litecode::validate_username("alicè"));   // non-ASCII
}

TEST(ValidateEmail, AcceptsAllowedShapes) {
    EXPECT_TRUE (litecode::validate_email("alice@x.io"));
    EXPECT_TRUE (litecode::validate_email("a.b.c@example.co.uk"));
    EXPECT_TRUE (litecode::validate_email("user+tag@sub.example.com"));
}

TEST(ValidateEmail, RejectsBadLength) {
    EXPECT_FALSE(litecode::validate_email(""));
    EXPECT_FALSE(litecode::validate_email("a@b"));                    // no dot
    EXPECT_FALSE(litecode::validate_email(std::string(101, 'a') + "@b.io"));
}

TEST(ValidateEmail, RejectsMissingOrExtraAt) {
    EXPECT_FALSE(litecode::validate_email("alice.example.com"));
    EXPECT_FALSE(litecode::validate_email("@example.com"));
    EXPECT_FALSE(litecode::validate_email("alice@"));
    EXPECT_FALSE(litecode::validate_email("alice@@example.com"));
}

TEST(ValidateEmail, RejectsWhitespace) {
    EXPECT_FALSE(litecode::validate_email("alice @example.com"));
    EXPECT_FALSE(litecode::validate_email("alice@exa mple.com"));
}

TEST(IsValidUsernameChar, AcceptsAllowedCharacters) {
    for (char c : {'a','z','A','Z','0','9','_','-','.'}) {
        EXPECT_TRUE(litecode::is_valid_username_char(c))
            << "char '" << c << "' should be valid";
    }
}

TEST(IsValidUsernameChar, RejectsEverythingElse) {
    for (char c : {'@',' ','!','/','\\',':','\t','\n','\0'}) {
        EXPECT_FALSE(litecode::is_valid_username_char(c))
            << "char " << static_cast<int>(static_cast<unsigned char>(c))
            << " should be invalid";
    }
}

TEST(Constants, MatchSpec) {
    // SPEC §4.1: username VARCHAR(50), password 8..72 (bcrypt cap).
    EXPECT_EQ(litecode::kMinUsernameLength, 3u);
    EXPECT_EQ(litecode::kMaxUsernameLength, 50u);
    EXPECT_EQ(litecode::kMinEmailLength,    3u);
    EXPECT_EQ(litecode::kMaxEmailLength,   100u);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — skipped if MySQL isn't reachable
// ────────────────────────────────────────────────────────────────────────────

TEST_F(DbFixture, CreateUserInsertsAndReturnsId) {
    UsernameTracker tracker(pool.get());

    litecode::UserRow row;
    row.username      = fresh_username("create_basic");
    tracker.add(row.username);
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    row.email         = std::nullopt;     // exercises the NULL bind path
    row.avatar        = std::nullopt;

    const int id = litecode::user_repo::create_user(*pool, row);
    EXPECT_GT(id, 0) << "create_user returned non-positive id";
    row.id = id;

    // Round-trip via find_by_username — every column we care about
    // should match what we inserted.
    const auto loaded = litecode::user_repo::find_by_username(
        *pool, row.username);
    ASSERT_TRUE(loaded.has_value()) << "user not found after insert";
    EXPECT_EQ(loaded->id,            id);
    EXPECT_EQ(loaded->username,      row.username);
    EXPECT_EQ(loaded->password_hash, row.password_hash);
    EXPECT_EQ(loaded->role,          "user");
    EXPECT_FALSE(loaded->email.has_value());
    EXPECT_FALSE(loaded->avatar.has_value());
    // mysql-connector returns DATETIME as a string; the exact format
    // varies with MySQL server config / column type, so we accept any
    // non-empty value (we round-trip a row that the DB populated).
    EXPECT_FALSE(loaded->created_at.empty());
    EXPECT_FALSE(loaded->last_login.has_value());
}

TEST_F(DbFixture, CreateUserWithEmailStoresEmail) {
    UsernameTracker tracker(pool.get());

    litecode::UserRow row;
    row.username      = fresh_username("create_email");
    tracker.add(row.username);
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    row.email         = std::string(row.username + "@example.test");
    row.avatar        = std::nullopt;

    const int id = litecode::user_repo::create_user(*pool, row);
    ASSERT_GT(id, 0);

    const auto loaded = litecode::user_repo::find_by_username(
        *pool, row.username);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->email.has_value());
    EXPECT_EQ(*loaded->email, *row.email);
}

TEST_F(DbFixture, CreateUserDuplicateUsernameReturnsZero) {
    UsernameTracker tracker(pool.get());

    litecode::UserRow row;
    row.username      = fresh_username("create_dup");
    tracker.add(row.username);
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    row.email         = std::nullopt;

    ASSERT_GT(litecode::user_repo::create_user(*pool, row), 0);

    // Second insert with the same username must return 0 (not throw).
    litecode::UserRow dup;
    dup.username      = row.username;
    dup.password_hash = litecode::hash_password("another9P");
    dup.role          = "user";
    EXPECT_EQ(litecode::user_repo::create_user(*pool, dup), 0);
}

TEST_F(DbFixture, CreateUserDuplicateEmailReturnsZero) {
    UsernameTracker tracker(pool.get());

    const std::string shared_email = fresh_username("dupemail") + "@example.test";

    litecode::UserRow a;
    a.username      = fresh_username("dup_e_a");
    tracker.add(a.username);
    a.password_hash = litecode::hash_password("hunter22");
    a.role          = "user";
    a.email         = shared_email;
    ASSERT_GT(litecode::user_repo::create_user(*pool, a), 0);

    litecode::UserRow b;
    b.username      = fresh_username("dup_e_b");
    tracker.add(b.username);
    b.password_hash = litecode::hash_password("hunter22");
    b.role          = "user";
    b.email         = shared_email;
    EXPECT_EQ(litecode::user_repo::create_user(*pool, b), 0);
}

TEST_F(DbFixture, UsernameAndEmailExists) {
    UsernameTracker tracker(pool.get());

    const std::string u = fresh_username("exists");
    tracker.add(u);
    EXPECT_FALSE(litecode::user_repo::username_exists(*pool, u));

    litecode::UserRow row;
    row.username      = u;
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    row.email         = std::nullopt;
    ASSERT_GT(litecode::user_repo::create_user(*pool, row), 0);

    EXPECT_TRUE (litecode::user_repo::username_exists(*pool, u));
    // A guaranteed-nonexistent email must report false.
    EXPECT_FALSE(litecode::user_repo::email_exists(
        *pool, "definitely_no_such_email_" + u + "@example.test"));
}

TEST_F(DbFixture, FindByIdMatchesFindByUsername) {
    UsernameTracker tracker(pool.get());

    litecode::UserRow row;
    row.username      = fresh_username("findbyid");
    tracker.add(row.username);
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    row.email         = std::nullopt;

    const int id = litecode::user_repo::create_user(*pool, row);
    ASSERT_GT(id, 0);

    const auto by_id   = litecode::user_repo::find_by_id      (*pool, id);
    const auto by_name = litecode::user_repo::find_by_username(*pool, row.username);
    ASSERT_TRUE(by_id  .has_value());
    ASSERT_TRUE(by_name.has_value());
    EXPECT_EQ(by_id->id,       by_name->id);
    EXPECT_EQ(by_id->username, by_name->username);
}

TEST_F(DbFixture, FindByUsernameReturnsNulloptForUnknown) {
    EXPECT_FALSE(litecode::user_repo::find_by_username(
        *pool, "definitely_no_such_user_xyz_zzz").has_value());
}

TEST_F(DbFixture, UpdateRoleFlipsRoleAndReturnsTrue) {
    UsernameTracker tracker(pool.get());

    litecode::UserRow row;
    row.username      = fresh_username("updrole");
    tracker.add(row.username);
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    const int id = litecode::user_repo::create_user(*pool, row);
    ASSERT_GT(id, 0);

    EXPECT_TRUE(litecode::user_repo::update_role(*pool, id, "admin"));
    const auto loaded = litecode::user_repo::find_by_id(*pool, id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->role, "admin");

    // Flipping back works too.
    EXPECT_TRUE(litecode::user_repo::update_role(*pool, id, "user"));
    const auto loaded2 = litecode::user_repo::find_by_id(*pool, id);
    ASSERT_TRUE(loaded2.has_value());
    EXPECT_EQ(loaded2->role, "user");

    // Unknown id ⇒ false.
    EXPECT_FALSE(litecode::user_repo::update_role(*pool, 99999999, "admin"));
}

TEST_F(DbFixture, UpdateRoleRejectsUnknownValues) {
    EXPECT_THROW(
        litecode::user_repo::update_role(*pool, 1, "root"),
        litecode::UserRepoError);
    EXPECT_THROW(
        litecode::user_repo::update_role(*pool, 1, ""),
        litecode::UserRepoError);
}

TEST_F(DbFixture, UpdateLastLoginStampsIpAndTimestamp) {
    UsernameTracker tracker(pool.get());

    litecode::UserRow row;
    row.username      = fresh_username("lastlogin");
    tracker.add(row.username);
    row.password_hash = litecode::hash_password("hunter22");
    row.role          = "user";
    const int id = litecode::user_repo::create_user(*pool, row);
    ASSERT_GT(id, 0);

    const auto before = litecode::user_repo::find_by_id(*pool, id);
    ASSERT_TRUE(before.has_value());
    EXPECT_FALSE(before->last_login.has_value());
    EXPECT_FALSE(before->last_login_ip.has_value());

    litecode::user_repo::update_last_login(*pool, id, "203.0.113.42");
    const auto after = litecode::user_repo::find_by_id(*pool, id);
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->last_login.has_value());
    EXPECT_EQ(*after->last_login_ip, "203.0.113.42");

    // Empty IP should clear last_login_ip.
    litecode::user_repo::update_last_login(*pool, id, "");
    const auto cleared = litecode::user_repo::find_by_id(*pool, id);
    ASSERT_TRUE(cleared.has_value());
    EXPECT_FALSE(cleared->last_login_ip.has_value());
}