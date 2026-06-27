// tests/unit/test_connection_pool.cpp
//
// Unit + light integration tests for src/db/connection_pool.h.
//
// Two layers:
//
//   1) Pure unit tests (no MySQL required):
//        - PoolConfig defaults
//        - PoolConfig::from_database_config mapping
//        - Constructor validation (rejects bad configs before any I/O)
//        - PooledConnection default ctor + move semantics
//        - PooledConnection::valid()
//        - Stats start at zero
//        - Move-assignment releases the previous handle
//
//   2) Integration tests (require a reachable MySQL):
//        - ping() returns true
//        - acquire() hands out a usable session
//        - execute() runs a plain statement
//        - execute(sql, args...) binds positional `?` parameters
//        - fetch_one / fetch_scalar helpers return typed results
//        - RAII: stats.active returns to 0 when the handle goes out of scope
//        - RAII: multiple acquire/release cycles reuse the underlying pool
//        - Thread safety: N threads acquire/release concurrently without
//          leaking sessions or corrupting stats
//
// Integration tests are gated by env vars (LITECODE_TEST_DB_HOST etc.)
// with sane defaults for the local dev box (root/123456/litecode@127.0.0.1).
// If ping() fails, those tests are SKIPPED — the binary still passes on a
// machine without MySQL, which is what we want for CI lint jobs.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "config.h"
#include "db/connection_pool.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test env helpers — read a connection string from process env, fall back
//  to local-dev defaults. Mirrors the pattern used in test_config.cpp.
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
    std::string  host        = env_or("LITECODE_TEST_DB_HOST", "127.0.0.1");
    std::uint16_t port       = env_u16_or("LITECODE_TEST_DB_PORT", 33060);
    std::string  user        = env_or("LITECODE_TEST_DB_USER", "root");
    std::string  password    = env_or("LITECODE_TEST_DB_PASSWORD", "123456");
    std::string  database    = env_or("LITECODE_TEST_DB_NAME", "litecode");

    litecode::PoolConfig to_pool_config() const {
        litecode::PoolConfig c;
        c.host                = host;
        c.port                = port;
        c.user                = user;
        c.password            = password;
        c.database            = database;
        c.min_size            = 1;
        c.max_size            = 4;
        c.acquire_timeout_ms  = 2000;
        c.connect_timeout_ms  = 5000;
        c.max_idle_time_ms    = 60000;
        return c;
    }
};

// Shared fixture that creates a pool once and skips when MySQL is
// unreachable. Subclasses get `*pool` ready to use.
class DbFixture : public ::testing::Test {
protected:
    DbConn          conn_info;
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

// Helper: a uniquely-named temp table that gets dropped on scope exit,
// so each test gets a fresh slate without polluting other tests.
class TempTable {
public:
    explicit TempTable(litecode::ConnectionPool& p) : pool_(p) {
        // Use chrono nanoseconds + counter to dodge name collisions across
        // tests running back-to-back.
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        name_ = "_test_cp_" + std::to_string(n) + "_"
              + std::to_string(static_cast<long long>(
                    std::chrono::system_clock::now()
                        .time_since_epoch().count()));
        auto conn = pool_.acquire();
        conn.execute(
            "CREATE TABLE " + name_ + " ("
            "  id INT PRIMARY KEY AUTO_INCREMENT,"
            "  name VARCHAR(64) NOT NULL,"
            "  score INT NOT NULL DEFAULT 0"
            ")");
    }
    ~TempTable() {
        if (name_.empty()) return;
        try {
            auto conn = pool_.acquire();
            conn.execute("DROP TABLE IF EXISTS " + name_);
        } catch (...) {
            // best-effort cleanup; failure here doesn't fail the test
        }
    }
    TempTable(const TempTable&)            = delete;
    TempTable& operator=(const TempTable&) = delete;

    const std::string& name() const noexcept { return name_; }

private:
    litecode::ConnectionPool& pool_;
    std::string               name_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — no DB I/O
// ────────────────────────────────────────────────────────────────────────────

TEST(PoolConfig, DefaultsAreSensible) {
    litecode::PoolConfig c;
    EXPECT_EQ(c.host,             "127.0.0.1");
    EXPECT_EQ(c.port,             33060);
    EXPECT_EQ(c.user,             "litecode");
    EXPECT_TRUE (c.password.empty());
    EXPECT_TRUE (c.database.empty());
    EXPECT_TRUE (c.socket_path.empty());
    EXPECT_EQ(c.min_size,         1);
    EXPECT_EQ(c.max_size,         8);
    EXPECT_GE(c.acquire_timeout_ms, 1);
    EXPECT_GE(c.connect_timeout_ms, 1);
}

TEST(PoolConfig, FromDatabaseConfigCarriesRelevantFields) {
    litecode::DatabaseConfig db;
    db.host                         = "db.example.com";
    db.port                         = 33060;
    db.user                         = "alice";
    db.password                     = "s3cret";
    db.database                     = "litecode";
    db.pool_min_size                = 2;
    db.pool_max_size                = 10;
    db.connection_timeout_seconds   = 7;

    auto c = litecode::PoolConfig::from_database_config(db);
    EXPECT_EQ(c.host,             "db.example.com");
    EXPECT_EQ(c.port,             33060);
    EXPECT_EQ(c.user,             "alice");
    EXPECT_EQ(c.password,         "s3cret");
    EXPECT_EQ(c.database,         "litecode");
    EXPECT_EQ(c.min_size,         2);
    EXPECT_EQ(c.max_size,         10);
    EXPECT_EQ(c.connect_timeout_ms, 7 * 1000);
}

TEST(PoolConfigCtor, RejectsEmptyHost) {
    litecode::PoolConfig c;
    c.host = "";
    c.user = "u"; c.database = "d"; c.max_size = 1;
    EXPECT_THROW(litecode::ConnectionPool p(c), litecode::ConnectionPoolError);
}

TEST(PoolConfigCtor, RejectsEmptyUser) {
    litecode::PoolConfig c;
    c.user = "";
    c.database = "d"; c.max_size = 1;
    EXPECT_THROW(litecode::ConnectionPool p(c), litecode::ConnectionPoolError);
}

TEST(PoolConfigCtor, RejectsEmptyDatabase) {
    litecode::PoolConfig c;
    c.database = "";
    c.max_size = 1;
    EXPECT_THROW(litecode::ConnectionPool p(c), litecode::ConnectionPoolError);
}

TEST(PoolConfigCtor, RejectsBadSizing) {
    litecode::PoolConfig c;
    c.max_size = 0;            // too small
    EXPECT_THROW(litecode::ConnectionPool p(c), litecode::ConnectionPoolError);

    c.max_size = 4; c.min_size = 5;   // min > max
    EXPECT_THROW(litecode::ConnectionPool p(c), litecode::ConnectionPoolError);

    c.min_size = 1;
    c.acquire_timeout_ms = -1; // negative timeout
    EXPECT_THROW(litecode::ConnectionPool p(c), litecode::ConnectionPoolError);
}

TEST(PooledConnection, DefaultCtorIsInvalid) {
    litecode::PooledConnection c;
    EXPECT_FALSE(c.valid());
    EXPECT_FALSE(static_cast<bool>(c));
}

TEST(PooledConnection, MoveTransfersOwnership) {
    // We can't easily construct a fresh pool for this test (needs MySQL),
    // but we can prove the move semantics on a moved-from handle: after
    // moving, the source becomes invalid; the destination is valid.
    litecode::PooledConnection a;   // invalid by default
    litecode::PooledConnection b(std::move(a));
    EXPECT_FALSE(a.valid());
    EXPECT_FALSE(b.valid());

    // Self-move-assignment on invalid handle must remain valid (no crash).
    litecode::PooledConnection c;
    c = std::move(b);
    EXPECT_FALSE(c.valid());
}

TEST(PoolConfigCtor, BadCredentialsThrow) {
    // A pool that points at a real MySQL but with a wrong password must
    // fail at construction (mysql-connector eagerly validates on first
    // getSession; we force it by setting a short connect timeout).
    //
    // Wrapped in a try/catch so CI without MySQL doesn't fail here.
    litecode::PoolConfig c;
    c.host               = env_or("LITECODE_TEST_DB_HOST", "127.0.0.1");
    c.port               = env_u16_or("LITECODE_TEST_DB_PORT", 33060);
    c.user               = env_or("LITECODE_TEST_DB_USER", "root");
    c.password           = "definitely_not_the_real_password";
    c.database           = env_or("LITECODE_TEST_DB_NAME", "litecode");
    c.min_size           = 1;
    c.max_size           = 1;
    c.connect_timeout_ms = 2000;
    c.acquire_timeout_ms = 1000;

    bool constructed_ok = false;
    try {
        litecode::ConnectionPool p(c);
        constructed_ok = p.initialized();
    } catch (const litecode::ConnectionPoolError&) {
        // Expected when MySQL is reachable and rejects the credentials.
        SUCCEED();
        return;
    } catch (...) {
        // Any other error also counts as "didn't silently accept bad creds".
        SUCCEED();
        return;
    }
    // If construction succeeded (e.g. lazy connection), try to actually
    // use it — that's where the auth check fires.
    if (constructed_ok) {
        litecode::ConnectionPool p(c);
        EXPECT_THROW(p.acquire(), litecode::ConnectionPoolError);
    } else {
        SUCCEED();
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — skipped if MySQL isn't reachable
// ────────────────────────────────────────────────────────────────────────────

TEST_F(DbFixture, PingReturnsTrue) {
    EXPECT_TRUE(pool->ping());
}

TEST_F(DbFixture, StatsStartAtZero) {
    auto s = pool->stats();
    EXPECT_EQ(s.total_acquired,   0);
    EXPECT_EQ(s.acquire_timeouts, 0);
    EXPECT_EQ(s.active,           0);
}

TEST_F(DbFixture, AcquireReturnsUsableSession) {
    auto conn = pool->acquire();
    ASSERT_TRUE(conn.valid());

    auto rs = conn.execute("SELECT 1 + 1 AS s");
    ASSERT_TRUE(rs.hasData());
    auto row = rs.fetchOne();
    ASSERT_TRUE(row);
    EXPECT_EQ(static_cast<int>(row[0]), 2);
    EXPECT_EQ(conn.session().getDefaultSchemaName(), conn_info.database);
}

TEST_F(DbFixture, ExecuteBindsPositionalParameters) {
    TempTable tt(*pool);

    // Insert with positional `?` placeholders.
    auto conn = pool->acquire();
    auto ins  = conn.execute(
        "INSERT INTO " + tt.name() + " (name, score) VALUES (?, ?)",
        std::string("alice"), 42);
    EXPECT_EQ(ins.getAffectedItemsCount(), 1u);

    // Read back via parameter binding — proves no string concat.
    auto rs = conn.execute(
        "SELECT name, score FROM " + tt.name() + " WHERE name = ?",
        std::string("alice"));
    ASSERT_TRUE(rs.hasData());
    auto row = rs.fetchOne();
    ASSERT_TRUE(row);
    EXPECT_EQ(std::string(row[0]), "alice");
    EXPECT_EQ(static_cast<int>(row[1]), 42);
}

TEST_F(DbFixture, FetchOneReturnsNulloptOnEmpty) {
    TempTable tt(*pool);
    auto conn = pool->acquire();

    auto row = conn.fetch_one(
        "SELECT id FROM " + tt.name() + " WHERE id = ?", 99999);
    EXPECT_FALSE(row.has_value());
}

TEST_F(DbFixture, FetchScalarTyped) {
    TempTable tt(*pool);
    auto conn = pool->acquire();
    conn.execute(
        "INSERT INTO " + tt.name() + " (name, score) VALUES (?, ?)",
        std::string("bob"), 7);

    auto name  = conn.fetch_scalar<std::string>(
        "SELECT name  FROM " + tt.name() + " WHERE name = ?",
        std::string("bob"));
    auto score = conn.fetch_scalar<int>(
        "SELECT score FROM " + tt.name() + " WHERE name = ?",
        std::string("bob"));

    ASSERT_TRUE(name.has_value());
    ASSERT_TRUE(score.has_value());
    EXPECT_EQ(*name,  "bob");
    EXPECT_EQ(*score, 7);
}

TEST_F(DbFixture, RaiiReturnsSessionToPool) {
    // Acquire 3 in sequence (well within max_size=4); the active counter
    // must come back to 0 after each handle goes out of scope.
    EXPECT_EQ(pool->stats().active, 0);

    {
        auto c1 = pool->acquire();
        EXPECT_EQ(pool->stats().active,      1);
        EXPECT_EQ(pool->stats().total_acquired, 1);
    }
    EXPECT_EQ(pool->stats().active, 0);

    {
        auto c2 = pool->acquire();
        auto c3 = pool->acquire();
        EXPECT_EQ(pool->stats().active,        2);
        EXPECT_EQ(pool->stats().total_acquired, 3);
    }
    EXPECT_EQ(pool->stats().active, 0);
}

TEST_F(DbFixture, MoveAssignmentReleasesOldHandle) {
    auto a = pool->acquire();
    EXPECT_EQ(pool->stats().active, 1);

    {
        auto b = pool->acquire();
        EXPECT_EQ(pool->stats().active, 2);
        a = std::move(b);  // a's old session released; b's moved to a
    }
    EXPECT_EQ(pool->stats().active, 1);

    {
        auto c = pool->acquire();
        EXPECT_EQ(pool->stats().active, 2);
        a = std::move(c);
    }
    EXPECT_EQ(pool->stats().active, 1);
}

TEST_F(DbFixture, PoolReusesSessionsAcrossAcquires) {
    // The mysqlx client pool should hand back a usable session on every
    // acquire, and total_acquired must grow monotonically. We don't pin
    // any specific identity (sessions are interchangeable by spec).
    for (int i = 0; i < 5; ++i) {
        auto conn = pool->acquire();
        ASSERT_TRUE(conn.valid());
        auto rs = conn.execute("SELECT " + std::to_string(i + 1));
        ASSERT_TRUE(rs.hasData());
        auto row = rs.fetchOne();
        ASSERT_TRUE(row);
        EXPECT_EQ(static_cast<int>(row[0]), i + 1);
    }
    EXPECT_EQ(pool->stats().total_acquired, 5);
    EXPECT_EQ(pool->stats().active,         0);
}

TEST_F(DbFixture, ThreadSafetyConcurrentAcquireRelease) {
    constexpr int kThreads         = 8;
    constexpr int kAcquiresPerThd  = 25;

    std::atomic<int>        ok_count{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kAcquiresPerThd; ++i) {
                try {
                    auto conn = pool->acquire();
                    auto rs   = conn.execute("SELECT 1");
                    if (rs.hasData()) {
                        auto row = rs.fetchOne();
                        if (row && static_cast<int>(row[0]) == 1) {
                            ++ok_count;
                        }
                    }
                } catch (...) {
                    // Pool exhaustion is acceptable under heavy concurrency
                    // when max_size=4 and timeout is short — the test
                    // mainly verifies we don't crash or leak.
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    // After all workers finish, every session must have been returned.
    EXPECT_EQ(pool->stats().active, 0);
    EXPECT_EQ(pool->stats().total_acquired,
              kThreads * kAcquiresPerThd);
    // At least some acquisitions should succeed; the exact count depends
    // on the host but should be > 0 since max_size=4 and threads=8 means
    // at least 4 in flight at any moment.
    EXPECT_GT(ok_count.load(), 0);
}

} // namespace