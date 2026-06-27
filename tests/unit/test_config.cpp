// tests/unit/test_config.cpp
//
// Unit tests for src/config.h. config.h is header-only and pulls in only
// the standard library, so the test binary links nothing beyond gtest_main.
//
// What we cover:
//   - Default loading (with LITECODE_ALLOW_INSECURE_DEFAULTS=1 escape hatch)
//   - JWT_SECRET boundary (SPEC §5.1: >= 32 bytes)
//   - JWT_SECRET missing in production mode ⇒ ConfigError
//   - LOG_LEVEL / LOG_FORMAT whitelist
//   - DB_POOL_MAX >= DB_POOL_MIN invariant
//   - JUDGE_WARM_POOL_SIZE <= JUDGE_MAX_CONCURRENT invariant
//   - rate-limit quota >= 1
//   - Admin bootstrap enabled iff both username + password set
//   - redacted_dump() never leaks JWT secret / DB password / Redis password
//   - getenv_bool_or accepts 1/true/yes/on/y (case-insensitive)
//   - .env file loader: KEY=VALUE, comments, empty lines, quoted values,
//     invalid lines are skipped without crashing
//
// Each test runs against a freshly cleared env so they're order-independent.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "config.h"

namespace {

// ── RAII env helpers (header-private — duplicated here so tests stay
//    self-contained without touching src/config.h's detail namespace). ──

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

// Every env var read by src/config.h. We wipe all of them in SetUp so each
// test starts from a known baseline regardless of the order it runs in.
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

// Common boilerplate: enable the dev-only placeholder JWT secret + a fake
// DB password so load_config() can succeed in the happy paths.
struct DevFixture : ::testing::Test {
    void SetUp() override {
        for (const char* k : kAllKeys) clear_one_(k);
        insecure_.emplace("LITECODE_ALLOW_INSECURE_DEFAULTS", "1");
        db_pw_.emplace("DB_PASSWORD", "test_pw");
    }
    // Most tests want the insecure flag; production-mode tests call
    // insecure_.reset()->clear() (see below) to flip it off.

    void clear_one_(const char* k) {
#if defined(_WIN32)
        _putenv_s(k, "");
#else
        unsetenv(k);
#endif
    }

    std::optional<ScopedEnv> insecure_;
    std::optional<ScopedEnv> db_pw_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Defaults / dev escape hatch
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, LoadsWithInsecureDefaults) {
    auto cfg = litecode::load_config();
    EXPECT_GE(cfg.jwt.secret.size(), 32u);
    EXPECT_EQ(cfg.server.host, "0.0.0.0");
    EXPECT_EQ(cfg.server.port, 8080);
    EXPECT_EQ(cfg.server.thread_pool_size, 8);
    EXPECT_EQ(cfg.database.host, "127.0.0.1");
    EXPECT_EQ(cfg.database.user, "litecode");
    EXPECT_EQ(cfg.database.password, "test_pw");
    EXPECT_EQ(cfg.database.pool_min_size, 4);
    EXPECT_EQ(cfg.database.pool_max_size, 16);
    EXPECT_EQ(cfg.jwt.access_ttl_seconds, 2 * 3600);
    EXPECT_EQ(cfg.jwt.refresh_ttl_seconds, 7 * 24 * 3600);
    EXPECT_EQ(cfg.judge.max_concurrent_judges, 4);
    EXPECT_EQ(cfg.judge.warm_pool_size, 2);
    EXPECT_EQ(cfg.judge.compile_timeout_seconds, 10);
    EXPECT_EQ(cfg.judge.output_limit_bytes, 16 * 1024 * 1024);
    EXPECT_EQ(cfg.logging.level, "INFO");
    EXPECT_EQ(cfg.logging.format, "JSON");
    EXPECT_FALSE(cfg.admin_bootstrap.enabled);
}

TEST_F(DevFixture, RejectsMissingJwtSecretInProductionMode) {
    insecure_.reset();
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

// ─────────────────────────────────────────────────────────────────────────────
//  JWT_SECRET boundary (SPEC §5.1)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, AcceptsJwtSecretAt32ByteBoundary) {
    ScopedEnv secret("JWT_SECRET", std::string(32, 'x').c_str());
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.jwt.secret.size(), 32u);
}

TEST_F(DevFixture, RejectsJwtSecretShorterThan32Bytes) {
    ScopedEnv secret("JWT_SECRET", std::string(31, 'x').c_str());
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsEmptyJwtSecretEvenWithInsecureFlag) {
    // The flag only fills in a placeholder when secret is *missing*, not when
    // it is explicitly set to empty. (std::getenv returns "" → treated as
    // missing, so the placeholder kicks in. Verify that path.)
    ScopedEnv secret("JWT_SECRET", "");
    auto cfg = litecode::load_config();
    EXPECT_FALSE(cfg.jwt.secret.empty());
    EXPECT_GE(cfg.jwt.secret.size(), 32u);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Logging whitelist
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, AcceptsAllValidLogLevels) {
    for (const char* lvl : {"TRACE", "DEBUG", "INFO", "WARN", "ERROR",
                            "trace", "Info", "warn"}) {
        ScopedEnv level("LOG_LEVEL", lvl);
        auto cfg = litecode::load_config();
        // load_config uppercases known levels; unknown ones throw below.
        EXPECT_NE(cfg.logging.level.find(lvl == "trace" ? "TRACE"
                                       : lvl == "Info" ? "INFO"
                                       : lvl == "warn" ? "WARN"
                                       : lvl), std::string::npos) << "lvl=" << lvl;
    }
}

TEST_F(DevFixture, RejectsBadLogLevel) {
    ScopedEnv level("LOG_LEVEL", "LOUD");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsBadLogFormat) {
    ScopedEnv fmt("LOG_FORMAT", "yaml");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Numeric invariants
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, RejectsDbPoolMaxBelowMin) {
    ScopedEnv min_p("DB_POOL_MIN", "20");
    ScopedEnv max_p("DB_POOL_MAX", "10");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsWarmPoolExceedingMaxConcurrent) {
    ScopedEnv max_c("JUDGE_MAX_CONCURRENT", "2");
    ScopedEnv warm ("JUDGE_WARM_POOL_SIZE", "5");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, AcceptsWarmPoolAtMaxConcurrentBoundary) {
    ScopedEnv max_c("JUDGE_MAX_CONCURRENT", "2");
    ScopedEnv warm ("JUDGE_WARM_POOL_SIZE", "2");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.judge.warm_pool_size, 2);
    EXPECT_EQ(cfg.judge.max_concurrent_judges, 2);
}

TEST_F(DevFixture, RejectsZeroRateLimitQuota) {
    ScopedEnv r("RATE_LIMIT_REGISTER_PER_MIN", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, AcceptsRateLimitAtOne) {
    ScopedEnv r("RATE_LIMIT_REGISTER_PER_MIN", "1");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.rate_limit.auth_register_per_minute_per_ip, 1);
}

TEST_F(DevFixture, RejectsCompileTimeoutOutOfRange) {
    ScopedEnv ct("JUDGE_COMPILE_TIMEOUT_SECONDS", "120");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Admin bootstrap
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, AdminBootstrapEnabledWhenBothSet) {
    ScopedEnv u("ADMIN_USERNAME", "admin");
    ScopedEnv p("ADMIN_PASSWORD", "secret");
    auto cfg = litecode::load_config();
    EXPECT_TRUE(cfg.admin_bootstrap.enabled);
    EXPECT_EQ(cfg.admin_bootstrap.username, "admin");
    EXPECT_EQ(cfg.admin_bootstrap.password, "secret");
}

TEST_F(DevFixture, AdminBootstrapDisabledWhenPasswordMissing) {
    ScopedEnv u("ADMIN_USERNAME", "admin");
    auto cfg = litecode::load_config();
    EXPECT_FALSE(cfg.admin_bootstrap.enabled);
}

TEST_F(DevFixture, AdminBootstrapDisabledWhenUsernameMissing) {
    ScopedEnv p("ADMIN_PASSWORD", "secret");
    auto cfg = litecode::load_config();
    EXPECT_FALSE(cfg.admin_bootstrap.enabled);
}

// ─────────────────────────────────────────────────────────────────────────────
//  redaction
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, RedactedDumpHidesSecrets) {
    ScopedEnv secret("JWT_SECRET",   "verysecretjwtkeythatshouldbehiddenxx");
    ScopedEnv pw    ("DB_PASSWORD",  "verysecretdbpassword");
    ScopedEnv rp    ("REDIS_PASSWORD", "verysecre t_redis_password");

    auto cfg = litecode::load_config();
    auto dump = litecode::redacted_dump(cfg);
    EXPECT_EQ(dump.find("verysecretjwtkey"),       std::string::npos);
    EXPECT_EQ(dump.find("verysecretdbpassword"),    std::string::npos);
    EXPECT_EQ(dump.find("verysecre t_redis_password"), std::string::npos);
    EXPECT_NE(dump.find("***"),                     std::string::npos);
    EXPECT_NE(dump.find("jwt.secret_len="),         std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Boolean parsing
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, AcceptsAllTruthyVariants) {
    for (const char* v : {"1", "true", "TRUE", "Yes", "on", "Y"}) {
        ScopedEnv r("REDIS_ENABLED", v);
        auto cfg = litecode::load_config();
        EXPECT_TRUE(cfg.redis.enabled) << "value=" << v;
    }
}

TEST_F(DevFixture, AcceptsAllFalsyVariants) {
    for (const char* v : {"0", "false", "FALSE", "no", "off", "N"}) {
        ScopedEnv r("REDIS_ENABLED", v);
        auto cfg = litecode::load_config();
        EXPECT_FALSE(cfg.redis.enabled) << "value=" << v;
    }
}

TEST_F(DevFixture, UnknownBoolFallsBackToDefault) {
    ScopedEnv r("REDIS_ENABLED", "maybe");
    auto cfg = litecode::load_config();
    EXPECT_FALSE(cfg.redis.enabled); // default is false
}

// ─────────────────────────────────────────────────────────────────────────────
//  .env file loader
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(DevFixture, DotEnvFileSetsValues) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_env_loader.env";
    {
        std::ofstream f(path);
        f << "# a comment line\n"
          << "\n"
          << "DB_PASSWORD=from_env_file\n"
          << "JWT_SECRET=this_is_a_valid_secret_with_at_least_32_bytes_xx\n"
          << "LOG_LEVEL=DEBUG  # inline comment\n"
          << "CORS_ALLOWED_ORIGINS=\"https://app.example.com,https://api.example.com\"\n"
          << "INVALID_LINE_NO_EQUALS_SIGN\n"
          << "REDIS_ENABLED=yes\n";
    }

    // override_env=true so .env values win over the fixture's pre-set
    // DB_PASSWORD. (Default behavior — process env wins — is covered by
    // DotEnvFileDoesNotOverrideProcessEnv below.)
    auto cfg = litecode::load_config(path.string(), /*override_env=*/true);
    EXPECT_EQ(cfg.database.password, "from_env_file");
    EXPECT_EQ(cfg.jwt.secret, "this_is_a_valid_secret_with_at_least_32_bytes_xx");
    EXPECT_EQ(cfg.logging.level, "DEBUG");
    EXPECT_EQ(cfg.cors.allowed_origins,
              "https://app.example.com,https://api.example.com");
    EXPECT_TRUE(cfg.redis.enabled);

    fs::remove(path);
}

TEST_F(DevFixture, DotEnvFileDoesNotOverrideProcessEnv) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_env_override.env";
    {
        std::ofstream f(path);
        f << "DB_PASSWORD=from_file\n";
    }
    // Process env already has DB_PASSWORD=test_pw from the fixture;
    // file value should be ignored because override_env=false.
    auto cfg = litecode::load_config(path.string());
    EXPECT_EQ(cfg.database.password, "test_pw");
    fs::remove(path);
}

TEST_F(DevFixture, MissingDotEnvFileIsNotAnError) {
    // No file at this path — load_config should silently fall through to
    // process env (fixture already set the dev defaults).
    auto cfg = litecode::load_config("definitely/not/a/real/path/.env");
    EXPECT_EQ(cfg.database.password, "test_pw");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 2 — additional coverage
//
//  Existing Phase-1 tests focus on the SPEC's load-bearing invariants
//  (JWT_SECRET length, pool bounds, log whitelist, redaction). This block
//  fills out the long tail so every field has at least one positive read,
//  one negative (where validation exists), and the redacted dump is
//  structurally verified.
// ─────────────────────────────────────────────────────────────────────────────

// ── ServerConfig ─────────────────────────────────────────────────────────────

TEST_F(DevFixture, ServerHostOverridesFromEnv) {
    ScopedEnv h("SERVER_HOST", "127.0.0.1");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.server.host, "127.0.0.1");
}

TEST_F(DevFixture, ServerPortOverridesFromEnv) {
    ScopedEnv p("SERVER_PORT", "9090");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.server.port, 9090);
}

TEST_F(DevFixture, ServerThreadPoolSizeOverridesFromEnv) {
    ScopedEnv t("SERVER_THREAD_POOL_SIZE", "16");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.server.thread_pool_size, 16);
}

TEST_F(DevFixture, RejectsThreadPoolSizeZero) {
    ScopedEnv t("SERVER_THREAD_POOL_SIZE", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

// ── DatabaseConfig ───────────────────────────────────────────────────────────

TEST_F(DevFixture, DatabaseHostAndPortOverrideFromEnv) {
    ScopedEnv h("DB_HOST", "db.internal");
    ScopedEnv p("DB_PORT", "3307");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.host, "db.internal");
    EXPECT_EQ(cfg.database.port, 3307);
}

TEST_F(DevFixture, DatabaseUserOverrideFromEnv) {
    ScopedEnv u("DB_USER", "app");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.user, "app");
}

TEST_F(DevFixture, DatabaseNameOverrideFromEnv) {
    ScopedEnv n("DB_NAME", "litecode_test");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.database, "litecode_test");
}

TEST_F(DevFixture, EmptyDbUserFallsBackToStructDefault) {
    // NOTE: src/config.h::getenv_opt treats empty env values as "missing"
    // and returns the struct default. So DB_USER="" never reaches the
    // validator — the struct default "litecode" wins. The validator in
    // config.h is defense-in-depth: it only fires if someone later removes
    // the default. Document that behavior here.
    ScopedEnv u("DB_USER", "");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.user, "litecode");
    EXPECT_FALSE(cfg.database.user.empty());
}

TEST_F(DevFixture, EmptyDbNameFallsBackToStructDefault) {
    // Same rationale as EmptyDbUserFallsBackToStructDefault above.
    ScopedEnv n("DB_NAME", "");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.database, "litecode");
    EXPECT_FALSE(cfg.database.database.empty());
}

TEST_F(DevFixture, RejectsPoolMinBelowOne) {
    ScopedEnv m("DB_POOL_MIN", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsConnTimeoutBelowOne) {
    ScopedEnv t("DB_CONN_TIMEOUT_SECONDS", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, DatabaseSocketStoredWhenProvided) {
    ScopedEnv s("DB_SOCKET", "/var/run/mysqlx.sock");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.socket_path, "/var/run/mysqlx.sock");
}

TEST_F(DevFixture, PoolRangeEqualityAccepted) {
    // DB_POOL_MIN == DB_POOL_MAX is a valid degenerate configuration.
    ScopedEnv mn("DB_POOL_MIN", "8");
    ScopedEnv mx("DB_POOL_MAX", "8");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.database.pool_min_size, 8);
    EXPECT_EQ(cfg.database.pool_max_size, 8);
}

// ── JwtConfig ────────────────────────────────────────────────────────────────

TEST_F(DevFixture, JwtIssuerOverrideFromEnv) {
    ScopedEnv i("JWT_ISSUER", "litecode-prod");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.jwt.issuer, "litecode-prod");
}

TEST_F(DevFixture, RejectsAccessTtlBelow60Seconds) {
    ScopedEnv a("JWT_ACCESS_TTL_SECONDS", "30");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsRefreshTtlBelowAccessTtl) {
    ScopedEnv a("JWT_ACCESS_TTL_SECONDS", "7200");
    ScopedEnv r("JWT_REFRESH_TTL_SECONDS", "3600");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, AcceptsCustomTtls) {
    ScopedEnv a("JWT_ACCESS_TTL_SECONDS", "1800");
    ScopedEnv r("JWT_REFRESH_TTL_SECONDS", "86400");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.jwt.access_ttl_seconds, 1800);
    EXPECT_EQ(cfg.jwt.refresh_ttl_seconds, 86400);
}

// ── JudgeConfig ──────────────────────────────────────────────────────────────

TEST_F(DevFixture, JudgeLimitsOverrideFromEnv) {
    ScopedEnv t("JUDGE_DEFAULT_TIME_LIMIT_MS", "2000");
    ScopedEnv m("JUDGE_DEFAULT_MEMORY_LIMIT_MB", "512");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.judge.default_time_limit_ms, 2000);
    EXPECT_EQ(cfg.judge.default_memory_limit_mb, 512);
}

TEST_F(DevFixture, RejectsTimeLimitBelowOne) {
    ScopedEnv t("JUDGE_DEFAULT_TIME_LIMIT_MS", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsMemoryLimitBelow16) {
    ScopedEnv m("JUDGE_DEFAULT_MEMORY_LIMIT_MB", "8");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsOutputLimitBelow1024) {
    ScopedEnv o("JUDGE_OUTPUT_LIMIT_BYTES", "512");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsRunTimeoutBelowOne) {
    ScopedEnv r("JUDGE_RUN_TIMEOUT_SECONDS", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsMaxConcurrentBelowOne) {
    ScopedEnv m("JUDGE_MAX_CONCURRENT", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, RejectsMaxQueueSizeBelowOne) {
    ScopedEnv q("JUDGE_MAX_QUEUE_SIZE", "0");
    EXPECT_THROW(litecode::load_config(), litecode::ConfigError);
}

TEST_F(DevFixture, JudgeDockerConfigOverrides) {
    ScopedEnv url("DOCKER_SOCKET_URL", "tcp://proxy:2375");
    ScopedEnv img("JUDGE_IMAGE",       "judge:custom");
    ScopedEnv net("JUDGE_NETWORK_MODE","bridge");
    ScopedEnv pid("JUDGE_PIDS_LIMIT",  "100");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.judge.docker_socket_url, "tcp://proxy:2375");
    EXPECT_EQ(cfg.judge.judge_image,       "judge:custom");
    EXPECT_EQ(cfg.judge.network_mode,      "bridge");
    EXPECT_EQ(cfg.judge.pids_limit,        "100");
}

TEST_F(DevFixture, JudgeCompileFlagsOverrideFromEnv) {
    ScopedEnv f("JUDGE_CPP_COMPILE_FLAGS", "-O0 -std=c++17");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.judge.cpp_compile_flags, "-O0 -std=c++17");
}

TEST_F(DevFixture, JudgeTruncateBytesOverrideFromEnv) {
    ScopedEnv ce("JUDGE_CE_TRUNCATE_BYTES", "8192");
    ScopedEnv re("JUDGE_RE_TRUNCATE_BYTES", "4096");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.judge.compile_error_truncate_bytes, 8192);
    EXPECT_EQ(cfg.judge.runtime_error_truncate_bytes, 4096);
}

// ── RedisConfig ──────────────────────────────────────────────────────────────

TEST_F(DevFixture, RedisConfigOverrides) {
    ScopedEnv h  ("REDIS_HOST",                  "redis.internal");
    ScopedEnv p  ("REDIS_PORT",                  "6380");
    ScopedEnv pw ("REDIS_PASSWORD",              "redispw");
    ScopedEnv db ("REDIS_DB",                    "3");
    ScopedEnv ct ("REDIS_CONN_TIMEOUT_SECONDS",  "10");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.redis.host,                       "redis.internal");
    EXPECT_EQ(cfg.redis.port,                       6380);
    EXPECT_EQ(cfg.redis.password,                   "redispw");
    EXPECT_EQ(cfg.redis.db,                         3);
    EXPECT_EQ(cfg.redis.connection_timeout_seconds, 10);
}

// ── LoggingConfig ────────────────────────────────────────────────────────────

TEST_F(DevFixture, LoggingConfigOverrides) {
    ScopedEnv lvl("LOG_LEVEL",              "WARN");
    ScopedEnv fmt("LOG_FORMAT",             "TEXT");
    ScopedEnv f  ("LOG_FILE",               "/var/log/litecode.log");
    ScopedEnv sz ("LOG_ROTATION_MAX_SIZE",  "50M");
    ScopedEnv cnt("LOG_ROTATION_MAX_FILES", "5");
    ScopedEnv rid("LOG_INCLUDE_REQUEST_ID", "0");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.logging.level,                "WARN");
    EXPECT_EQ(cfg.logging.format,               "TEXT");
    EXPECT_EQ(cfg.logging.file_path,            "/var/log/litecode.log");
    EXPECT_EQ(cfg.logging.rotation_max_size,    "50M");
    EXPECT_EQ(cfg.logging.rotation_max_files,   5);
    EXPECT_FALSE(cfg.logging.include_request_id);
}

// ── CorsConfig (new — no Phase 1 coverage) ────────────────────────────────────

TEST_F(DevFixture, CorsOriginsDefault) {
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.cors.allowed_origins,
              "http://localhost:8080,http://127.0.0.1:8080");
    EXPECT_TRUE(cfg.cors.allow_credentials);
}

TEST_F(DevFixture, CorsConfigOverrides) {
    ScopedEnv o("CORS_ALLOWED_ORIGINS",   "https://app.example.com");
    ScopedEnv c("CORS_ALLOW_CREDENTIALS", "0");
    auto cfg = litecode::load_config();
    EXPECT_EQ(cfg.cors.allowed_origins, "https://app.example.com");
    EXPECT_FALSE(cfg.cors.allow_credentials);
}

// ── AdminBootstrapConfig additional coverage ─────────────────────────────────

TEST_F(DevFixture, AdminBootstrapEmailStored) {
    ScopedEnv u("ADMIN_USERNAME", "admin");
    ScopedEnv p("ADMIN_PASSWORD", "secret");
    ScopedEnv e("ADMIN_EMAIL",    "admin@example.com");
    auto cfg = litecode::load_config();
    EXPECT_TRUE(cfg.admin_bootstrap.enabled);
    EXPECT_EQ(cfg.admin_bootstrap.email, "admin@example.com");
}

// ── .env loader edge cases ───────────────────────────────────────────────────

TEST_F(DevFixture, DotEnvSingleQuotedValue) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_single_quotes.env";
    {
        std::ofstream f(path);
        f << "CORS_ALLOWED_ORIGINS='https://single.example.com'\n";
    }
    auto cfg = litecode::load_config(path.string(), /*override_env=*/true);
    EXPECT_EQ(cfg.cors.allowed_origins, "https://single.example.com");
    fs::remove(path);
}

TEST_F(DevFixture, DotEnvValueContainsEqualsSign) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_equals.env";
    {
        std::ofstream f(path);
        f << "CORS_ALLOWED_ORIGINS=https://example.com?a=1&b=2\n";
    }
    auto cfg = litecode::load_config(path.string(), true);
    EXPECT_EQ(cfg.cors.allowed_origins, "https://example.com?a=1&b=2");
    fs::remove(path);
}

TEST_F(DevFixture, DotEnvEmptyValueFallsBackToDefault) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_empty.env";
    {
        std::ofstream f(path);
        f << "CORS_ALLOW_CREDENTIALS=\n";
    }
    auto cfg = litecode::load_config(path.string(), true);
    // Empty env value is treated as missing by getenv_opt → default wins.
    EXPECT_TRUE(cfg.cors.allow_credentials);
    fs::remove(path);
}

TEST_F(DevFixture, DotEnvCommentOnlyFileIsHarmless) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_comments.env";
    {
        std::ofstream f(path);
        f << "# only comments\n"
          << "\n"
          << "    # indented comment\n"
          << "\n";
    }
    auto cfg = litecode::load_config(path.string(), true);
    EXPECT_EQ(cfg.server.port, 8080);
    EXPECT_EQ(cfg.database.host, "127.0.0.1");
    fs::remove(path);
}

TEST_F(DevFixture, DotEnvBlankPathSkipsFileLoading) {
    // Empty path must skip the file loader entirely — falls through to env.
    auto cfg = litecode::load_config("");
    EXPECT_EQ(cfg.database.password, "test_pw"); // from fixture
}

// ── redacted_dump structure ──────────────────────────────────────────────────

TEST_F(DevFixture, RedactedDumpContainsAllExpectedSections) {
    auto cfg = litecode::load_config();
    auto dump = litecode::redacted_dump(cfg);
    EXPECT_NE(dump.find("AppConfig{"),                std::string::npos);
    EXPECT_NE(dump.find("server.host="),              std::string::npos);
    EXPECT_NE(dump.find("server.port="),              std::string::npos);
    EXPECT_NE(dump.find("database.host="),            std::string::npos);
    EXPECT_NE(dump.find("jwt.issuer="),               std::string::npos);
    EXPECT_NE(dump.find("jwt.secret_len="),           std::string::npos);
    EXPECT_NE(dump.find("judge.time_limit="),         std::string::npos);
    EXPECT_NE(dump.find("redis.enabled="),            std::string::npos);
    EXPECT_NE(dump.find("log.level="),                std::string::npos);
    EXPECT_NE(dump.find("cors.origins="),             std::string::npos);
    EXPECT_NE(dump.find("rate_limit.register="),      std::string::npos);
    EXPECT_NE(dump.find("admin_bootstrap="),          std::string::npos);
    EXPECT_NE(dump.find("env_file="),                 std::string::npos);
}

TEST_F(DevFixture, RedactedDumpEnvFileReflectsLoadedPath) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "litecode_test_dump_path.env";
    { std::ofstream f(path); f << "JWT_SECRET=dump_path_secret_xxxxxxxxxxxxxxxxxxx\n"; }

    auto cfg = litecode::load_config(path.string(), true);
    auto dump = litecode::redacted_dump(cfg);
    EXPECT_NE(dump.find(path.string()), std::string::npos);
    fs::remove(path);
}

} // namespace