// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — configuration management
//
// Phase 1 基础设施 / 配置管理
//   - All tunables (DB / port / JWT / judge / Redis / logging / CORS /
//     rate-limit / admin-bootstrap) live in this single header.
//   - Values come from environment variables; every field has a sensible
//     default so the service can boot in dev with zero env set.
//   - On startup we also read a `.env` file (path configurable) so that
//     docker-compose / k8s secrets can stay out of the working tree.
//     Already-set env vars win over `.env` values (12-factor convention).
//   - `load_config()` validates every required field and throws
//     `ConfigError` on the first problem — fail fast at boot.
//
// Usage:
//   int main() {
//       try {
//           const auto& cfg = litecode::init_config(".env");
//           // ... start server, logger, db pool using cfg ...
//       } catch (const litecode::ConfigError& e) {
//           std::fprintf(stderr, "config error: %s\n", e.what());
//           return 1;
//       }
//   }
//
//   // Anywhere downstream (no globals-with-side-effects):
//   const auto& cfg = litecode::config();

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Section: value types
// ────────────────────────────────────────────────────────────────────────────

struct ServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
    int thread_pool_size = 8;
};

struct DatabaseConfig {
    std::string host = "127.0.0.1";          // ignored if socket_path is set
    std::uint16_t port = 33060;              // mysql-connector-c++ X Protocol default
    std::string user = "litecode";
    std::string password;                    // no default — must be supplied in non-dev
    std::string database = "litecode";
    std::string socket_path;                 // optional unix/named socket
    int pool_min_size = 4;
    int pool_max_size = 16;
    int connection_timeout_seconds = 10;
};

struct JwtConfig {
    std::string secret;                      // REQUIRED, >= 32 bytes (SPEC §5.1)
    std::string issuer = "litecode";
    int access_ttl_seconds = 2 * 3600;       // 2h
    int refresh_ttl_seconds = 7 * 24 * 3600; // 7d
};

struct JudgeConfig {
    // Per-test resource limits (match SPEC §7.3, §7.4)
    int default_time_limit_ms = 1000;
    int default_memory_limit_mb = 256;
    int compile_timeout_seconds = 10;        // SPEC §7.3 — anti compile-bomb
    int run_timeout_seconds = 30;            // judge.sh hard timeout
    int judge_hard_timeout_seconds = 30;     // web-side outer timeout

    // Pooling (SPEC §3.2)
    int max_concurrent_judges = 4;
    int warm_pool_size = 2;
    int max_queue_size = 50;

    // Output limits (SPEC §7.4)
    int output_limit_bytes = 16 * 1024 * 1024; // 16 MB → OLE

    // Docker
    std::string docker_socket_url;           // e.g. tcp://docker-proxy:2375
    std::string judge_image = "litecode-judge:latest";
    std::string network_mode = "none";       // SPEC §7.3 — full network isolation
    std::string pids_limit = "50";           // SPEC §7.3

    // Compile flags (SPEC §7.1)
    std::string cpp_compile_flags =
        "-O2 -std=c++17 -pipe "
        "-fstack-protector-strong "
        "-D_FORTIFY_SOURCE=2 "
        "-Wformat -Wformat-security "
        "-Wl,-z,now -Wl,-z,relro";

    // CE / RE error truncation (SPEC §7.4)
    int compile_error_truncate_bytes = 4 * 1024;
    int runtime_error_truncate_bytes = 2 * 1024;
};

struct RedisConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string password;                    // empty → no AUTH
    int db = 0;
    int connection_timeout_seconds = 5;
    bool enabled = false;                    // MVP: in-memory; flip when Redis is up
};

struct LoggingConfig {
    std::string level = "INFO";              // TRACE/DEBUG/INFO/WARN/ERROR
    std::string format = "json";             // json | text
    std::string file_path;                   // empty → stdout only
    std::string rotation_max_size = "10M";
    int rotation_max_files = 3;
    bool include_request_id = true;          // SPEC §15 — every log line carries it
};

struct CorsConfig {
    std::string allowed_origins = "http://localhost:8080,http://127.0.0.1:8080";
    bool allow_credentials = true;
};

struct RateLimitConfig {
    // SPEC §5.1 / §5.2 / §15.2 quotas
    int auth_register_per_minute_per_ip    = 5;
    int auth_login_per_minute_per_ip       = 10;
    int problems_public_per_minute_per_ip  = 60;   // GET /api/v1/problems, /api/v1/problems/:slug
    int submission_per_minute_per_user     = 30;
    int admin_write_per_minute             = 30;
    int bulk_import_per_hour               = 5;
};

struct AdminBootstrapConfig {
    // When both username + password are set, the bootstrap routine creates
    // an admin account at startup if one with that username doesn't exist.
    std::string username;
    std::string password;
    std::string email;
    bool enabled = false;
};

struct AppConfig {
    ServerConfig           server;
    DatabaseConfig         database;
    JwtConfig              jwt;
    JudgeConfig            judge;
    RedisConfig            redis;
    LoggingConfig          logging;
    CorsConfig             cors;
    RateLimitConfig        rate_limit;
    AdminBootstrapConfig   admin_bootstrap;

    std::string env_file_path = ".env";      // last-loaded file (for diagnostics)
    bool env_override = false;               // true ⇒ .env wins over process env
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: exceptions
// ────────────────────────────────────────────────────────────────────────────

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: env helpers  (header-private)
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::optional<std::string> getenv_opt(const char* key) {
    if (key == nullptr) return std::nullopt;
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

inline std::string getenv_or(const char* key, const std::string& fallback) {
    return getenv_opt(key).value_or(fallback);
}

template <typename T>
inline T getenv_int_or(const char* key, T fallback) {
    auto v = getenv_opt(key);
    if (!v) return fallback;
    try {
        long long parsed = std::stoll(*v);
        return static_cast<T>(parsed);
    } catch (...) {
        return fallback;
    }
}

inline bool getenv_bool_or(const char* key, bool fallback) {
    auto v = getenv_opt(key);
    if (!v) return fallback;
    std::string s;
    s.reserve(v->size());
    for (char c : *v) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (s == "1" || s == "true" || s == "yes" || s == "on"  || s == "y") return true;
    if (s == "0" || s == "false" || s == "no"  || s == "off" || s == "n") return false;
    return fallback;
}

inline std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

inline std::string strip_inline_comment(std::string s) {
    // # starts a comment only when not inside quotes (we keep parsing simple
    // and require quotes around any value containing '#').
    bool in_single = false, in_double = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        else if (c == '#' && !in_single && !in_double) {
            s.erase(i);
            break;
        }
    }
    return s;
}

// Parses a single .env line "KEY=VALUE" into a (key,value) pair.
// Returns false if the line is blank / comment / malformed.
inline bool parse_env_line(const std::string& raw,
                           std::string& key, std::string& value) {
    std::string line = trim(strip_inline_comment(raw));
    if (line.empty()) return false;

    auto eq = line.find('=');
    if (eq == std::string::npos) return false;

    key   = trim(line.substr(0, eq));
    value = trim(line.substr(eq + 1));

    // Strip a single matching pair of surrounding quotes.
    if (value.size() >= 2) {
        char a = value.front(), b = value.back();
        if ((a == '"'  && b == '"') ||
            (a == '\'' && b == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return !key.empty();
}

// Loads KEY=VALUE pairs from a .env file into the process environment.
// If override_env is false, existing process env wins over file values.
// Returns the number of variables actually set from the file.
inline std::size_t load_env_file(const std::string& path, bool override_env) {
    std::ifstream in(path);
    if (!in.is_open()) return 0;

    std::size_t applied = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::string key, value;
        if (!parse_env_line(line, key, value)) continue;

        if (!override_env && std::getenv(key.c_str()) != nullptr) continue;

#if defined(_WIN32)
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), override_env ? 1 : 0);
#endif
        ++applied;
    }
    return applied;
}

inline std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Section: load_config
// ────────────────────────────────────────────────────────────────────────────

// Build an AppConfig from environment + (optional) .env file.
// Throws ConfigError on any validation failure.
inline AppConfig load_config(const std::string& env_file_path = ".env",
                             bool override_env = false) {
    if (!env_file_path.empty()) {
        detail::load_env_file(env_file_path, override_env);
    }

    AppConfig cfg;
    cfg.env_file_path = env_file_path;
    cfg.env_override  = override_env;

    // ── Server ──────────────────────────────────────────────────────────────
    cfg.server.host = detail::getenv_or("SERVER_HOST", cfg.server.host);
    cfg.server.port = detail::getenv_int_or<std::uint16_t>("SERVER_PORT", cfg.server.port);
    cfg.server.thread_pool_size =
        detail::getenv_int_or<int>("SERVER_THREAD_POOL_SIZE", cfg.server.thread_pool_size);
    if (cfg.server.port == 0)               throw ConfigError("SERVER_PORT must be 1..65535");
    if (cfg.server.thread_pool_size < 1)    throw ConfigError("SERVER_THREAD_POOL_SIZE must be >= 1");

    // ── Database ────────────────────────────────────────────────────────────
    cfg.database.host     = detail::getenv_or("DB_HOST", cfg.database.host);
    cfg.database.port     = detail::getenv_int_or<std::uint16_t>("DB_PORT", cfg.database.port);
    cfg.database.user     = detail::getenv_or("DB_USER", cfg.database.user);
    cfg.database.password = detail::getenv_or("DB_PASSWORD", cfg.database.password);
    cfg.database.database = detail::getenv_or("DB_NAME", cfg.database.database);
    cfg.database.socket_path = detail::getenv_or("DB_SOCKET", cfg.database.socket_path);
    cfg.database.pool_min_size =
        detail::getenv_int_or<int>("DB_POOL_MIN", cfg.database.pool_min_size);
    cfg.database.pool_max_size =
        detail::getenv_int_or<int>("DB_POOL_MAX", cfg.database.pool_max_size);
    cfg.database.connection_timeout_seconds =
        detail::getenv_int_or<int>("DB_CONN_TIMEOUT_SECONDS",
                                   cfg.database.connection_timeout_seconds);

    if (cfg.database.host.empty() && cfg.database.socket_path.empty())
        throw ConfigError("DB_HOST (or DB_SOCKET) must be set");
    if (cfg.database.user.empty())
        throw ConfigError("DB_USER must be set");
    if (cfg.database.database.empty())
        throw ConfigError("DB_NAME must be set");
    // NOTE: the three empty-checks above are defense-in-depth. Because
    // getenv_opt() treats empty env values as "missing" and falls back to
    // the struct defaults (which are non-empty), these checks currently
    // only fire if a future change clears the struct defaults.
    if (cfg.database.pool_min_size < 1)
        throw ConfigError("DB_POOL_MIN must be >= 1");
    if (cfg.database.pool_max_size < cfg.database.pool_min_size)
        throw ConfigError("DB_POOL_MAX (" + std::to_string(cfg.database.pool_max_size) +
                          ") must be >= DB_POOL_MIN (" +
                          std::to_string(cfg.database.pool_min_size) + ")");
    if (cfg.database.connection_timeout_seconds < 1)
        throw ConfigError("DB_CONN_TIMEOUT_SECONDS must be >= 1");

    // ── JWT (SPEC §5.1) ─────────────────────────────────────────────────────
    cfg.jwt.secret              = detail::getenv_or("JWT_SECRET", "");
    cfg.jwt.issuer              = detail::getenv_or("JWT_ISSUER", cfg.jwt.issuer);
    cfg.jwt.access_ttl_seconds  = detail::getenv_int_or<int>("JWT_ACCESS_TTL_SECONDS",
                                                            cfg.jwt.access_ttl_seconds);
    cfg.jwt.refresh_ttl_seconds = detail::getenv_int_or<int>("JWT_REFRESH_TTL_SECONDS",
                                                             cfg.jwt.refresh_ttl_seconds);

    // In dev we accept an auto-generated placeholder so the smoke build still
    // runs without secrets. Production must set JWT_SECRET explicitly.
    if (cfg.jwt.secret.empty()) {
        if (detail::getenv_bool_or("LITECODE_ALLOW_INSECURE_DEFAULTS", false)) {
            cfg.jwt.secret =
                "INSECURE_DEV_ONLY_auto_generated_DO_NOT_USE_IN_PRODUCTION_";
            cfg.jwt.secret += std::string(32, '!');
        } else {
            throw ConfigError(
                "JWT_SECRET must be set (>= 32 bytes). Generate with "
                "`openssl rand -base64 48`. For local dev only, set "
                "LITECODE_ALLOW_INSECURE_DEFAULTS=1 to use an ephemeral "
                "placeholder secret.");
        }
    }
    if (cfg.jwt.secret.size() < 32) {
        throw ConfigError("JWT_SECRET must be at least 32 bytes (got " +
                          std::to_string(cfg.jwt.secret.size()) + ")");
    }
    if (cfg.jwt.access_ttl_seconds < 60)
        throw ConfigError("JWT_ACCESS_TTL_SECONDS must be >= 60");
    if (cfg.jwt.refresh_ttl_seconds < cfg.jwt.access_ttl_seconds)
        throw ConfigError("JWT_REFRESH_TTL_SECONDS must be >= JWT_ACCESS_TTL_SECONDS");

    // ── Judge ───────────────────────────────────────────────────────────────
    cfg.judge.default_time_limit_ms =
        detail::getenv_int_or<int>("JUDGE_DEFAULT_TIME_LIMIT_MS",
                                   cfg.judge.default_time_limit_ms);
    cfg.judge.default_memory_limit_mb =
        detail::getenv_int_or<int>("JUDGE_DEFAULT_MEMORY_LIMIT_MB",
                                   cfg.judge.default_memory_limit_mb);
    cfg.judge.compile_timeout_seconds =
        detail::getenv_int_or<int>("JUDGE_COMPILE_TIMEOUT_SECONDS",
                                   cfg.judge.compile_timeout_seconds);
    cfg.judge.run_timeout_seconds =
        detail::getenv_int_or<int>("JUDGE_RUN_TIMEOUT_SECONDS",
                                   cfg.judge.run_timeout_seconds);
    cfg.judge.judge_hard_timeout_seconds =
        detail::getenv_int_or<int>("JUDGE_HARD_TIMEOUT_SECONDS",
                                   cfg.judge.judge_hard_timeout_seconds);
    cfg.judge.max_concurrent_judges =
        detail::getenv_int_or<int>("JUDGE_MAX_CONCURRENT",
                                   cfg.judge.max_concurrent_judges);
    cfg.judge.warm_pool_size =
        detail::getenv_int_or<int>("JUDGE_WARM_POOL_SIZE",
                                   cfg.judge.warm_pool_size);
    cfg.judge.max_queue_size =
        detail::getenv_int_or<int>("JUDGE_MAX_QUEUE_SIZE",
                                   cfg.judge.max_queue_size);
    cfg.judge.output_limit_bytes =
        detail::getenv_int_or<int>("JUDGE_OUTPUT_LIMIT_BYTES",
                                   cfg.judge.output_limit_bytes);
    cfg.judge.docker_socket_url =
        detail::getenv_or("DOCKER_SOCKET_URL", cfg.judge.docker_socket_url);
    cfg.judge.judge_image =
        detail::getenv_or("JUDGE_IMAGE", cfg.judge.judge_image);
    cfg.judge.network_mode =
        detail::getenv_or("JUDGE_NETWORK_MODE", cfg.judge.network_mode);
    cfg.judge.pids_limit =
        detail::getenv_or("JUDGE_PIDS_LIMIT", cfg.judge.pids_limit);
    cfg.judge.cpp_compile_flags =
        detail::getenv_or("JUDGE_CPP_COMPILE_FLAGS", cfg.judge.cpp_compile_flags);
    cfg.judge.compile_error_truncate_bytes =
        detail::getenv_int_or<int>("JUDGE_CE_TRUNCATE_BYTES",
                                   cfg.judge.compile_error_truncate_bytes);
    cfg.judge.runtime_error_truncate_bytes =
        detail::getenv_int_or<int>("JUDGE_RE_TRUNCATE_BYTES",
                                   cfg.judge.runtime_error_truncate_bytes);

    if (cfg.judge.max_concurrent_judges < 1)
        throw ConfigError("JUDGE_MAX_CONCURRENT must be >= 1");
    if (cfg.judge.warm_pool_size < 0 ||
        cfg.judge.warm_pool_size > cfg.judge.max_concurrent_judges)
        throw ConfigError("JUDGE_WARM_POOL_SIZE must be in [0, JUDGE_MAX_CONCURRENT]");
    if (cfg.judge.max_queue_size < 1)
        throw ConfigError("JUDGE_MAX_QUEUE_SIZE must be >= 1");
    if (cfg.judge.compile_timeout_seconds < 1 || cfg.judge.compile_timeout_seconds > 60)
        throw ConfigError("JUDGE_COMPILE_TIMEOUT_SECONDS must be in [1, 60]");
    if (cfg.judge.run_timeout_seconds < 1)
        throw ConfigError("JUDGE_RUN_TIMEOUT_SECONDS must be >= 1");
    if (cfg.judge.default_time_limit_ms < 1)
        throw ConfigError("JUDGE_DEFAULT_TIME_LIMIT_MS must be >= 1");
    if (cfg.judge.default_memory_limit_mb < 16)
        throw ConfigError("JUDGE_DEFAULT_MEMORY_LIMIT_MB must be >= 16");
    if (cfg.judge.output_limit_bytes < 1024)
        throw ConfigError("JUDGE_OUTPUT_LIMIT_BYTES must be >= 1024");

    // ── Redis ───────────────────────────────────────────────────────────────
    cfg.redis.host     = detail::getenv_or("REDIS_HOST", cfg.redis.host);
    cfg.redis.port     = detail::getenv_int_or<std::uint16_t>("REDIS_PORT", cfg.redis.port);
    cfg.redis.password = detail::getenv_or("REDIS_PASSWORD", cfg.redis.password);
    cfg.redis.db       = detail::getenv_int_or<int>("REDIS_DB", cfg.redis.db);
    cfg.redis.connection_timeout_seconds =
        detail::getenv_int_or<int>("REDIS_CONN_TIMEOUT_SECONDS",
                                   cfg.redis.connection_timeout_seconds);
    cfg.redis.enabled  = detail::getenv_bool_or("REDIS_ENABLED", cfg.redis.enabled);

    // ── Logging ─────────────────────────────────────────────────────────────
    cfg.logging.level             = detail::getenv_or("LOG_LEVEL", cfg.logging.level);
    cfg.logging.format            = detail::getenv_or("LOG_FORMAT", cfg.logging.format);
    cfg.logging.file_path         = detail::getenv_or("LOG_FILE", cfg.logging.file_path);
    cfg.logging.rotation_max_size = detail::getenv_or("LOG_ROTATION_MAX_SIZE",
                                                     cfg.logging.rotation_max_size);
    cfg.logging.rotation_max_files = detail::getenv_int_or<int>("LOG_ROTATION_MAX_FILES",
                                                               cfg.logging.rotation_max_files);
    cfg.logging.include_request_id = detail::getenv_bool_or("LOG_INCLUDE_REQUEST_ID",
                                                            cfg.logging.include_request_id);

    {
        const std::string lvl = detail::upper(cfg.logging.level);
        if (lvl != "TRACE" && lvl != "DEBUG" && lvl != "INFO" &&
            lvl != "WARN"  && lvl != "ERROR")
            throw ConfigError("LOG_LEVEL must be one of TRACE/DEBUG/INFO/WARN/ERROR");
        cfg.logging.level = lvl;
    }
    {
        const std::string fmt = detail::upper(cfg.logging.format);
        if (fmt != "JSON" && fmt != "TEXT")
            throw ConfigError("LOG_FORMAT must be JSON or TEXT");
        cfg.logging.format = fmt;
    }

    // ── CORS (SPEC §5.7 / §15.2) ────────────────────────────────────────────
    cfg.cors.allowed_origins    = detail::getenv_or("CORS_ALLOWED_ORIGINS",
                                                   cfg.cors.allowed_origins);
    cfg.cors.allow_credentials  = detail::getenv_bool_or("CORS_ALLOW_CREDENTIALS",
                                                        cfg.cors.allow_credentials);

    // ── Rate limit (SPEC §5.1 / §15.2) ──────────────────────────────────────
    cfg.rate_limit.auth_register_per_minute_per_ip =
        detail::getenv_int_or<int>("RATE_LIMIT_REGISTER_PER_MIN",
                                   cfg.rate_limit.auth_register_per_minute_per_ip);
    cfg.rate_limit.auth_login_per_minute_per_ip =
        detail::getenv_int_or<int>("RATE_LIMIT_LOGIN_PER_MIN",
                                   cfg.rate_limit.auth_login_per_minute_per_ip);
    cfg.rate_limit.submission_per_minute_per_user =
        detail::getenv_int_or<int>("RATE_LIMIT_SUBMIT_PER_MIN",
                                   cfg.rate_limit.submission_per_minute_per_user);
    cfg.rate_limit.admin_write_per_minute =
        detail::getenv_int_or<int>("RATE_LIMIT_ADMIN_WRITE_PER_MIN",
                                   cfg.rate_limit.admin_write_per_minute);
    cfg.rate_limit.bulk_import_per_hour =
        detail::getenv_int_or<int>("RATE_LIMIT_BULK_IMPORT_PER_HOUR",
                                   cfg.rate_limit.bulk_import_per_hour);

    for (int q : {cfg.rate_limit.auth_register_per_minute_per_ip,
                  cfg.rate_limit.auth_login_per_minute_per_ip,
                  cfg.rate_limit.submission_per_minute_per_user,
                  cfg.rate_limit.admin_write_per_minute,
                  cfg.rate_limit.bulk_import_per_hour}) {
        if (q < 1)
            throw ConfigError("rate-limit quotas must be >= 1");
    }

    // ── Admin bootstrap (SPEC §4.1 — first admin via env) ──────────────────
    cfg.admin_bootstrap.username = detail::getenv_or("ADMIN_USERNAME", "");
    cfg.admin_bootstrap.password = detail::getenv_or("ADMIN_PASSWORD", "");
    cfg.admin_bootstrap.email    = detail::getenv_or("ADMIN_EMAIL", "");
    cfg.admin_bootstrap.enabled  =
        !cfg.admin_bootstrap.username.empty() &&
        !cfg.admin_bootstrap.password.empty();

    return cfg;
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: process-wide singleton accessor
//
//  Thread-safe init under a single mutex; the slot itself is a function-
//  local static std::unique_ptr so destruction is well-defined at program
//  exit. We deliberately do NOT use std::call_once because once_flag
//  cannot be reset, which would make the singleton un-testable (and
//  would leave a dangling pointer after reset_config_for_testing()).
// ────────────────────────────────────────────────────────────────────────────

namespace detail {
inline std::unique_ptr<AppConfig>& config_slot() {
    static std::unique_ptr<AppConfig> slot;
    return slot;
}
inline std::mutex& config_mutex() {
    static std::mutex m;
    return m;
}
} // namespace detail

// Eagerly initialize the process-wide config. Call once from main().
// Subsequent calls with the same arguments are no-ops; calls with
// different arguments are ignored (the first init wins). To re-read
// env after the first init, call reset_config_for_testing() first
// (tests only).
inline const AppConfig& init_config(const std::string& env_file_path = ".env",
                                    bool override_env = false) {
    std::lock_guard<std::mutex> g(detail::config_mutex());
    auto& slot = detail::config_slot();
    if (!slot) {
        slot = std::make_unique<AppConfig>(load_config(env_file_path, override_env));
    }
    return *slot;
}

// Returns the process-wide config, initializing it lazily with defaults
// if init_config() was never called. Use init_config() in main() so that
// configuration errors are caught at startup rather than at first use.
inline const AppConfig& config() {
    // Fast path — already initialized, no lock needed.
    auto& slot = detail::config_slot();
    if (slot) return *slot;

    std::lock_guard<std::mutex> g(detail::config_mutex());
    if (!slot) {
        slot = std::make_unique<AppConfig>(load_config());
    }
    return *slot;
}

// Resets the singleton — only intended for tests. After this call the
// next init_config() / config() will re-read the environment from scratch.
inline void reset_config_for_testing() {
    std::lock_guard<std::mutex> g(detail::config_mutex());
    detail::config_slot().reset();
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: redacted dump (for boot logs / /api/v1/admin/stats)
// ────────────────────────────────────────────────────────────────────────────

inline std::string redacted_dump(const AppConfig& cfg) {
    std::ostringstream os;
    os << "AppConfig{\n"
       << "  server.host="            << cfg.server.host
       << " server.port="             << cfg.server.port
       << " thread_pool_size="        << cfg.server.thread_pool_size << "\n"
       << "  database.host="          << cfg.database.host
       << " database.port="           << cfg.database.port
       << " database.user="           << cfg.database.user
       << " database.name="           << cfg.database.database
       << " database.socket="         << (cfg.database.socket_path.empty() ? "<tcp>" : cfg.database.socket_path)
       << " pool=["                   << cfg.database.pool_min_size << ".."
                                        << cfg.database.pool_max_size << "]\n"
       << "  database.password=***    "
       << "db.conn_timeout="          << cfg.database.connection_timeout_seconds << "s\n"
       << "  jwt.issuer="             << cfg.jwt.issuer
       << " jwt.secret_len="          << cfg.jwt.secret.size() << "B (***)\n"
       << "  jwt.access_ttl="         << cfg.jwt.access_ttl_seconds << "s"
       << " jwt.refresh_ttl="         << cfg.jwt.refresh_ttl_seconds << "s\n"
       << "  judge.time_limit="       << cfg.judge.default_time_limit_ms << "ms"
       << " mem_limit="               << cfg.judge.default_memory_limit_mb << "MB"
       << " compile_timeout="         << cfg.judge.compile_timeout_seconds << "s\n"
       << "  judge.max_concurrent="   << cfg.judge.max_concurrent_judges
       << " warm_pool="               << cfg.judge.warm_pool_size
       << " queue="                   << cfg.judge.max_queue_size
       << " output_limit="            << cfg.judge.output_limit_bytes << "B\n"
       << "  judge.docker_socket="    << (cfg.judge.docker_socket_url.empty() ? "<default>" : cfg.judge.docker_socket_url)
       << " judge_image="             << cfg.judge.judge_image
       << " network="                 << cfg.judge.network_mode
       << " pids_limit="              << cfg.judge.pids_limit << "\n"
       << "  redis.enabled="          << (cfg.redis.enabled ? "true" : "false")
       << " redis.host="              << cfg.redis.host
       << " redis.port="              << cfg.redis.port
       << " redis.password="          << (cfg.redis.password.empty() ? "<none>" : "***") << "\n"
       << "  log.level="              << cfg.logging.level
       << " log.format="              << cfg.logging.format
       << " log.file="                << (cfg.logging.file_path.empty() ? "<stdout>" : cfg.logging.file_path)
       << " request_id="              << (cfg.logging.include_request_id ? "yes" : "no") << "\n"
       << "  cors.origins="           << cfg.cors.allowed_origins
       << " creds="                   << (cfg.cors.allow_credentials ? "yes" : "no") << "\n"
       << "  rate_limit.register="    << cfg.rate_limit.auth_register_per_minute_per_ip << "/min"
       << " login="                   << cfg.rate_limit.auth_login_per_minute_per_ip << "/min"
       << " submit="                  << cfg.rate_limit.submission_per_minute_per_user << "/min"
       << " admin="                   << cfg.rate_limit.admin_write_per_minute << "/min"
       << " import="                  << cfg.rate_limit.bulk_import_per_hour << "/hour\n"
       << "  admin_bootstrap="        << (cfg.admin_bootstrap.enabled ? cfg.admin_bootstrap.username : "<disabled>") << "\n"
       << "  env_file="               << (cfg.env_file_path.empty() ? "<none>" : cfg.env_file_path) << "\n"
       << "}";
    return os.str();
}

} // namespace litecode