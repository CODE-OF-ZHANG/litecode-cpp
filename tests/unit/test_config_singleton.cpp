// tests/unit/test_config_singleton.cpp
//
// Unit tests for the process-wide singleton accessor in src/config.h:
//   - litecode::init_config(path, override)
//   - litecode::config()                    // lazy fallback
//   - litecode::reset_config_for_testing()  // test hook
//
// Lives in its own test binary (test_config_singleton) so the singleton can
// be freely reset between cases without polluting test_config.cpp's pure
// load_config() coverage.
//
// What we cover:
//   - config() lazy-loads when init_config() was never called
//   - config() / init_config() are idempotent (same address each call)
//   - init_config() honors its env_file_path argument on first call
//   - init_config() ignores arguments on subsequent calls (first wins)
//   - reset_config_for_testing() drops the slot so re-init reads fresh env
//   - config() after reset re-creates the slot
//   - throwing during init leaves the slot null (no dangling pointer)

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "config.h"

namespace {

// ── RAII env helpers (same pattern as test_config.cpp; duplicated so this
//    binary stays self-contained). ───────────────────────────────────────────

class ScopedEnv {
public:
    ScopedEnv(const char* key, const char* value) : key_(key) {
        if (const char* prev = std::getenv(key)) {
            had_prev_ = true;
            prev_ = prev;
        }
        set_(value);
    }
    ~ScopedEnv() {
        if (had_prev_) set_(prev_.c_str());
        else           clear_();
    }
    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set_(const char* v) {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), v);
#else
        setenv(key_.c_str(), v, 1);
#endif
    }
    void clear_() {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), "");
#else
        unsetenv(key_.c_str());
#endif
    }
    std::string key_, prev_;
    bool had_prev_ = false;
};

class ScopedUnset {
public:
    explicit ScopedUnset(const char* key) : key_(key) {
        if (const char* prev = std::getenv(key)) {
            had_prev_ = true;
            prev_ = prev;
        }
        clear_();
    }
    ~ScopedUnset() {
        if (had_prev_) set_(prev_.c_str());
    }
    ScopedUnset(const ScopedUnset&)            = delete;
    ScopedUnset& operator=(const ScopedUnset&) = delete;

private:
    void set_(const char* v) {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), v);
#else
        setenv(key_.c_str(), v, 1);
#endif
    }
    void clear_() {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), "");
#else
        unsetenv(key_.c_str());
#endif
    }
    std::string key_, prev_;
    bool had_prev_ = false;
};

// Every env var read by src/config.h. Wiped in SetUp so each test starts
// from a known baseline regardless of execution order.
constexpr const char* kAllKeys[] = {
    "SERVER_HOST", "SERVER_PORT", "SERVER_THREAD_POOL_SIZE",
    "DB_HOST", "DB_PORT", "DB_USER", "DB_PASSWORD", "DB_NAME", "DB_SOCKET",
    "DB_POOL_MIN", "DB_POOL_MAX", "DB_CONN_TIMEOUT_SECONDS",
    "JWT_SECRET", "JWT_ISSUER", "JWT_ACCESS_TTL_SECONDS", "JWT_REFRESH_TTL_SECONDS",
    "LITECODE_ALLOW_INSECURE_DEFAULTS",
    "JUDGE_DEFAULT_TIME_LIMIT_MS", "JUDGE_DEFAULT_MEMORY_LIMIT_MB",
    "JUDGE_COMPILE_TIMEOUT_SECONDS", "JUDGE_RUN_TIMEOUT_SECONDS",
    "JUDGE_HARD_TIMEOUT_SECONDS", "JUDGE_MAX_CONCURRENT", "JUDGE_WARM_POOL_SIZE",
    "JUDGE_MAX_QUEUE_SIZE", "JUDGE_OUTPUT_LIMIT_BYTES",
    "DOCKER_SOCKET_URL", "JUDGE_IMAGE", "JUDGE_NETWORK_MODE", "JUDGE_PIDS_LIMIT",
    "JUDGE_CPP_COMPILE_FLAGS", "JUDGE_CE_TRUNCATE_BYTES", "JUDGE_RE_TRUNCATE_BYTES",
    "REDIS_HOST", "REDIS_PORT", "REDIS_PASSWORD", "REDIS_DB",
    "REDIS_CONN_TIMEOUT_SECONDS", "REDIS_ENABLED",
    "LOG_LEVEL", "LOG_FORMAT", "LOG_FILE", "LOG_ROTATION_MAX_SIZE",
    "LOG_ROTATION_MAX_FILES", "LOG_INCLUDE_REQUEST_ID",
    "CORS_ALLOWED_ORIGINS", "CORS_ALLOW_CREDENTIALS",
    "RATE_LIMIT_REGISTER_PER_MIN", "RATE_LIMIT_LOGIN_PER_MIN",
    "RATE_LIMIT_SUBMIT_PER_MIN", "RATE_LIMIT_ADMIN_WRITE_PER_MIN",
    "RATE_LIMIT_BULK_IMPORT_PER_HOUR",
    "ADMIN_USERNAME", "ADMIN_PASSWORD", "ADMIN_EMAIL",
};

// Wipe one env var unconditionally.
inline void clear_one_(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

// Reset the singleton + clear all env before each test. Set dev-mode
// defaults so most init_config() / config() calls succeed.
struct SingletonFixture : ::testing::Test {
    void SetUp() override {
        litecode::reset_config_for_testing();
        for (const char* k : kAllKeys) clear_one_(k);
        insecure_.emplace("LITECODE_ALLOW_INSECURE_DEFAULTS", "1");
        db_pw_.emplace("DB_PASSWORD", "test_pw");
    }
    void TearDown() override {
        // Belt-and-braces: don't leak a singleton between tests.
        litecode::reset_config_for_testing();
    }

    std::optional<ScopedEnv> insecure_;
    std::optional<ScopedEnv> db_pw_;
};

// ── Lazy load ────────────────────────────────────────────────────────────────

TEST_F(SingletonFixture, ConfigLazilyCreatesDefault) {
    // init_config was never called; config() should still produce a valid cfg.
    const litecode::AppConfig& cfg = litecode::config();
    EXPECT_EQ(cfg.server.port, 8080);
    EXPECT_EQ(cfg.database.host, "127.0.0.1");
    EXPECT_GE(cfg.jwt.secret.size(), 32u);
}

TEST_F(SingletonFixture, ConfigCalledTwiceReturnsSameRef) {
    const litecode::AppConfig& a = litecode::config();
    const litecode::AppConfig& b = litecode::config();
    EXPECT_EQ(&a, &b);
}

// ── init_config ─────────────────────────────────────────────────────────────

TEST_F(SingletonFixture, InitConfigReturnsValidConfig) {
    const litecode::AppConfig& cfg = litecode::init_config();
    EXPECT_EQ(cfg.server.port, 8080);
    EXPECT_EQ(cfg.database.host, "127.0.0.1");
    EXPECT_EQ(cfg.database.password, "test_pw");
}

TEST_F(SingletonFixture, InitConfigCalledTwiceReturnsSameRef) {
    const litecode::AppConfig& a = litecode::init_config();
    const litecode::AppConfig& b = litecode::init_config();
    EXPECT_EQ(&a, &b);
}

TEST_F(SingletonFixture, InitConfigHonorsEnvFilePath) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_singleton_init.env";
    {
        std::ofstream f(path);
        f << "DB_PASSWORD=singleton_pw\n"
          << "JWT_SECRET=this_is_a_valid_secret_with_at_least_32_bytes_xx\n"
          << "SERVER_PORT=7777\n";
    }
    const litecode::AppConfig& cfg = litecode::init_config(path.string(), true);
    EXPECT_EQ(cfg.database.password, "singleton_pw");
    EXPECT_EQ(cfg.server.port, 7777);
    fs::remove(path);
}

TEST_F(SingletonFixture, SecondInitConfigIgnoresNewArgs) {
    // First init reads env file A; second init with file B must NOT reload.
    namespace fs = std::filesystem;
    auto path_a = fs::temp_directory_path() / "litecode_test_singleton_a.env";
    auto path_b = fs::temp_directory_path() / "litecode_test_singleton_b.env";
    {
        std::ofstream f(path_a);
        f << "DB_PASSWORD=first_init_pw\n"
          << "JWT_SECRET=this_is_a_valid_secret_with_at_least_32_bytes_xx\n"
          << "SERVER_PORT=1111\n";
    }
    {
        std::ofstream f(path_b);
        f << "DB_PASSWORD=second_init_pw\n"
          << "JWT_SECRET=this_is_a_valid_secret_with_at_least_32_bytes_xx\n"
          << "SERVER_PORT=2222\n";
    }
    const litecode::AppConfig& a = litecode::init_config(path_a.string(), true);
    EXPECT_EQ(a.database.password, "first_init_pw");
    EXPECT_EQ(a.server.port, 1111);

    // Second call with a different file must not change anything.
    const litecode::AppConfig& b = litecode::init_config(path_b.string(), true);
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(b.database.password, "first_init_pw");
    EXPECT_EQ(b.server.port, 1111);

    fs::remove(path_a);
    fs::remove(path_b);
}

// ── config() after init_config() ────────────────────────────────────────────

TEST_F(SingletonFixture, ConfigAfterInitReturnsSameRef) {
    const litecode::AppConfig& init = litecode::init_config();
    const litecode::AppConfig& lazy = litecode::config();
    EXPECT_EQ(&init, &lazy);
}

// ── reset + re-init ─────────────────────────────────────────────────────────

TEST_F(SingletonFixture, ResetAllowsReinitWithNewEnv) {
    ScopedEnv port("SERVER_PORT", "3000");
    const litecode::AppConfig& a = litecode::init_config();
    EXPECT_EQ(a.server.port, 3000);

    litecode::reset_config_for_testing();
    ScopedEnv port2("SERVER_PORT", "4000");
    const litecode::AppConfig& b = litecode::init_config();
    EXPECT_EQ(b.server.port, 4000);
    // We deliberately don't compare &a vs &b — after reset() the old object
    // is freed and the allocator is free to reuse the same heap address.
    // The port-value comparison above is what actually proves reset worked.
}

TEST_F(SingletonFixture, ResetThenConfigReloadsFromEnv) {
    ScopedEnv port("SERVER_PORT", "3001");
    const litecode::AppConfig& a = litecode::init_config();
    EXPECT_EQ(a.server.port, 3001);

    litecode::reset_config_for_testing();
    ScopedEnv port2("SERVER_PORT", "3002");
    const litecode::AppConfig& b = litecode::config();
    EXPECT_EQ(b.server.port, 3002);
    // Same allocator-reuse caveat as ResetAllowsReinitWithNewEnv.
}

TEST_F(SingletonFixture, ResetIsIdempotent) {
    litecode::init_config();
    litecode::reset_config_for_testing();
    litecode::reset_config_for_testing(); // second reset must not crash
    SUCCEED();
}

// ── Error propagation ──────────────────────────────────────────────────────

TEST_F(SingletonFixture, InitConfigThrowLeavesSlotNull) {
    // Force a config error: production mode requires JWT_SECRET.
    insecure_.reset();
    EXPECT_THROW(litecode::init_config(), litecode::ConfigError);

    // If load_config threw mid-construction, the slot must NOT hold a
    // half-built AppConfig — otherwise config() would happily return a
    // dangling/broken reference. We verify by observing that the next
    // call also throws (it goes through the lazy path and tries again).
    EXPECT_THROW(litecode::config(), litecode::ConfigError);
}

} // namespace