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
#include <filesystem>
#include <fstream>
#include <iostream>
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
    // v1.3.4 PR 2 — restore bounded concurrency.
    //   - 8 was a placeholder when we ran on a SyncTaskQueue (single
    //     request at a time, the queue size never mattered).
    //   - 16 reserves enough worker threads so a PR 3 sample-run
    //     request (up to 3s of stuck-in-while-loop CPU inside a
    //     worker) can sit idle without starving login / health /
    //     ranking reads. With 16 workers, even if 2 sample-runs are
    //     in-flight (gated by `sample_max_concurrent`), there are
    //     still 14 free workers for the rest of the API.
    //   - Hard upper bound is whatever the operator passes via
    //     `SERVER_THREAD_POOL_SIZE`; recommended ceiling 32.
    int thread_pool_size = 16;
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
    int access_ttl_seconds = 60 * 60;        // v1.3.4 PR 10: 1h(SPEC §5.1)
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
    std::filesystem::path task_dir_parent;   // v1.2.50: where task.json is
                                             // staged inside the web container
                                             // (must be a path the same docker
                                             //  volume is mounted at, otherwise
                                             //  the judge container can't see
                                             //  the file). When empty the
                                             //  scheduler falls back to
                                             //  std::filesystem::temp_directory_path().
    std::string            task_volume_name; // v1.2.50: docker named-volume name
                                             // that BOTH the web container's
                                             // task_dir_parent mount AND the
                                             // judge container's task.json mount
                                             // resolve to. Lets the judge
                                             // container read task.json from
                                             // the same backing storage without
                                             // any host-path translation
                                             // (works on Docker Desktop too).
                                             // When empty the scheduler falls
                                             // back to the bind-mount path
                                             // (legacy / Linux-host dev).

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

    // v1.3.4 PR 3 — synchronous run-samples endpoint tunables.
    // Hard caps enforced at the SampleRunner layer; env vars can dial
    // DOWN but never UP (a careless operator can't widen the attack
    // surface by setting SAMPLE_MAX_CASES=999).
    int sample_max_cases            = 3;        // hard cap 4 (tested at handler)
    int sample_case_timeout_ms      = 3000;     // per-case wall clock
    int sample_max_concurrent       = 2;        // semaphore size; > concurrent runners
    int sample_compile_timeout_ms   = 10'000;   // compile bomb guard (matches async)
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
    int stats_ranking_per_minute_per_ip    = 30;   // GET /api/v1/stats/ranking (SPEC §5.4)
    int admin_users_list_per_minute        = 60;   // GET /api/v1/admin/users (SPEC §5.5)
    int admin_users_role_per_minute        = 10;   // PUT /api/v1/admin/users/:id/role (SPEC §5.5)
    int admin_audit_logs_per_minute        = 60;   // GET /api/v1/admin/audit-logs (SPEC §5.5, §15.6)
    int admin_queue_per_minute             = 60;   // GET /api/v1/admin/queue (SPEC §5.5, §11 Phase 6 ★ v1.2.44)
    // v1.3.4 PR 3 — synchronous run-samples rate cap. Per-user, generous
    // default (60/min) so a LeetCode-style "iterate-on-a-problem"
    // session isn't punished; tighten in CI / exam scenarios via env.
    int sample_run_per_minute_per_user     = 60;
};

// ────────────────────────────────────────────────────────────────────────────
//  LoginLockoutConfig (Phase 6 ☆ v1.2.46 — SPEC §15.1 失败登录锁定)
//
//  Throttles credential-stuffing / brute-force probes at the per-USERNAME
//  layer (the per-IP rate-limit on /api/v1/auth/login covers a single
//  attacker IP, but a botnet of N IPs can together hammer one account
//  and slip under the per-IP bucket). The state machine:
//
//      1) Every failed login (bad password OR unknown username) bumps
//         a per-username counter.
//      2) When the counter crosses `threshold` within
//         `window_seconds` of the FIRST failure in the current window,
//         the username is locked for `lockout_duration_seconds`.
//      3) During the lockout, /api/v1/auth/login returns 423 Locked +
//         the same "invalid username or password" envelope a normal
//         bad-password attempt would (anti-enumeration: the wire never
//         reveals whether the account is real). The Retry-After
//         header carries the remaining lockout time.
//      4) A successful login clears the counter AND any active lockout
//         so a one-off typo doesn't chain into a self-lockout.
//      5) On natural expiry the counter and lockout state reset
//         implicitly (entries older than `window_seconds` count as
//         fresh attempts).
//
//  Tuning rationale (defaults match SPEC §15.1):
//    threshold = 5           — every 5th failure already writes an
//                               audit row, so a lockout threshold of 5
//                               aligns the operator-visible event with
//                               the user-visible block.
//    window_seconds = 900    — 15 min (SPEC §15.1 explicit).
//    lockout_duration_seconds = 900 — 15 min (SPEC §15.1 explicit).
//    enabled = true          — flip to false in tests / a paranoid
//                               dev box that wants to disable the
//                               feature without code edits.
//
//  Storage is in-memory inside LoginFailureTracker (Phase 2 ★). The
//  state is intentionally process-local; a server restart wipes the
//  tracker, which is a documented trade-off (the alternative —
//  persisting lockout state to the DB — makes a coordinated multi-IP
//  attack slightly harder but adds a hot-path write to every failed
//  login). SPEC §9 calls for a Redis-backed tracker on multi-instance
//  deploys; v1.2.46 keeps the MVP single-process semantics.
//
//  Env knobs:
//      LOGIN_LOCKOUT_ENABLED                   (1/0/true/false)
//      LOGIN_LOCKOUT_THRESHOLD                 (int >= 1)
//      LOGIN_LOCKOUT_WINDOW_SECONDS            (int >= 1)
//      LOGIN_LOCKOUT_DURATION_SECONDS          (int >= 1)
// ────────────────────────────────────────────────────────────────────────────

struct LoginLockoutConfig {
    bool enabled                   = true;
    int  threshold                 = 5;       // 5 failed attempts → lockout
    int  window_seconds            = 900;     // 15 min sliding window
    int  lockout_duration_seconds  = 900;     // 15 min lockout
};

// ────────────────────────────────────────────────────────────────────────────
//  CookieConfig (Phase 5 ★ — SPEC §6.3, §15.1, §15.3 token storage)
//
//  The refresh token is delivered to the browser as an HttpOnly cookie
//  so document-side JavaScript (and any injected <script> via XSS) can
//  never read it; only the Web server's /auth/refresh endpoint sees it
//  via the Cookie header. The access token, in contrast, stays in
//  JavaScript memory only — short-lived (default 2h) and tolerable to
//  lose on tab close.
//
//  The attributes enforced here are the SPEC §6.3 + §15.3 baseline:
//      HttpOnly   — set true always; the JS layer can't reach the
//                   refresh token regardless of XSS
//      Secure     — true in production (HTTPS only); default false in
//                   dev so a plain http://localhost flow can still
//                   receive the cookie. Auto-flipped when the env
//                   detects a `production` / `prod` flag, so an
//                   operator who forgets to set SECURE_COOKIES in a
//                   prod-style deployment gets the safer default.
//      SameSite   — "Strict" per SPEC §6.3; "Lax" / "None" are
//                   supported for operators who want the refresh to
//                   survive an OAuth-style cross-site bounce.
//      Path       — "/api/v1/auth" by default so the cookie is only
//                   attached to /api/v1/auth/* requests; safer surface
//                   than the site root and matches the SPEC §15.1
//                   "refresh + blacklist" endpoint cluster.
//      Max-Age    — refresh_ttl_seconds by default (7d) so the cookie
//                   dies in lock-step with the JWT exp. The /refresh
//                   rotation issues a fresh Set-Cookie with the new
//                   pair's TTL.
//      Name       — "lc_refresh" by default; short, project-scoped,
//                   avoids clashing with anything else on the host.
// ────────────────────────────────────────────────────────────────────────────

struct CookieConfig {
    bool        enabled             = true;   // master switch (dev can flip to false)
    bool        http_only           = true;   // SPEC §6.3 / §15.3 — never readable from JS
    bool        secure              = false;  // true in prod (HTTPS); false in dev (HTTP localhost)
    std::string same_site           = "Strict"; // SPEC §6.3
    std::string path                = "/api/v1/auth";
    std::string name                = "lc_refresh";
    int         max_age_seconds     = 0;     // 0 ⇒ "follow refresh_ttl_seconds at handler time"
    bool        allow_body_fallback = true;  // /refresh also accepts refresh_token in body (dev/test)

    static CookieConfig insecure_dev_defaults() {
        CookieConfig c;
        c.enabled  = true;
        c.http_only = true;
        c.secure    = false;             // http://localhost doesn't need Secure
        c.same_site = "Strict";
        c.path      = "/api/v1/auth";
        c.name      = "lc_refresh";
        c.max_age_seconds = 0;           // follow refresh_ttl_seconds
        c.allow_body_fallback = true;
        return c;
    }
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
    LoginLockoutConfig     login_lockout;          // Phase 6 ☆ v1.2.46
    CookieConfig           cookie;
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
    // v1.2.50: optional override for the tempdir where task.json is
    // staged. Must be a directory inside the same docker volume that
    // the judge container will mount to read it back (use the
    // shared named volume in docker-compose). When empty the
    // scheduler falls back to std::filesystem::temp_directory_path().
    {
        std::string tdp = detail::getenv_or("JUDGE_TASK_DIR_PARENT", "");
        if (!tdp.empty()) cfg.judge.task_dir_parent = std::filesystem::path(tdp);
    }
    // v1.2.50: docker named-volume name that BOTH the web container's
    // task_dir_parent mount and the judge container's task.json mount
    // resolve to. Lets the judge container read task.json from the
    // same backing storage without any host-path translation. Default
    // empty → scheduler uses a bind mount (legacy / Linux-host dev).
    cfg.judge.task_volume_name =
        detail::getenv_or("JUDGE_TASK_VOLUME_NAME",
                           cfg.judge.task_volume_name);
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

    // v1.3.4 PR 3 — synchronous run-samples tunables. Defaults are
    // sized for the "instant feedback while iterating" UX without
    // letting a careless operator widen the attack surface.
    cfg.judge.sample_max_cases =
        detail::getenv_int_or<int>("SAMPLE_MAX_CASES",
                                   cfg.judge.sample_max_cases);
    cfg.judge.sample_case_timeout_ms =
        detail::getenv_int_or<int>("SAMPLE_CASE_TIMEOUT_MS",
                                   cfg.judge.sample_case_timeout_ms);
    cfg.judge.sample_max_concurrent =
        detail::getenv_int_or<int>("SAMPLE_MAX_CONCURRENT",
                                   cfg.judge.sample_max_concurrent);
    cfg.judge.sample_compile_timeout_ms =
        detail::getenv_int_or<int>("SAMPLE_COMPILE_TIMEOUT_MS",
                                   cfg.judge.sample_compile_timeout_ms);

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
    // Sample-runner range guards. The hard upper bound on
    // sample_max_cases is 4 (matches what LeetCode shows per problem
    // — more than that, and the front-end result panel becomes
    // unreadable). The per-case timeout has no hard upper bound but
    // anything > 30 s defeats the "synchronous" UX.
    if (cfg.judge.sample_max_cases < 1 || cfg.judge.sample_max_cases > 4)
        throw ConfigError("SAMPLE_MAX_CASES must be in [1, 4]");
    if (cfg.judge.sample_case_timeout_ms < 100 ||
        cfg.judge.sample_case_timeout_ms > 30'000)
        throw ConfigError("SAMPLE_CASE_TIMEOUT_MS must be in [100, 30000]");
    if (cfg.judge.sample_max_concurrent < 1 ||
        cfg.judge.sample_max_concurrent > 8)
        throw ConfigError("SAMPLE_MAX_CONCURRENT must be in [1, 8]");
    if (cfg.judge.sample_compile_timeout_ms < 1000)
        throw ConfigError("SAMPLE_COMPILE_TIMEOUT_MS must be >= 1000");

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
    cfg.rate_limit.stats_ranking_per_minute_per_ip =
        detail::getenv_int_or<int>("RATE_LIMIT_RANKING_PER_MIN",
                                   cfg.rate_limit.stats_ranking_per_minute_per_ip);
    cfg.rate_limit.admin_users_list_per_minute =
        detail::getenv_int_or<int>("RATE_LIMIT_ADMIN_USERS_LIST_PER_MIN",
                                   cfg.rate_limit.admin_users_list_per_minute);
    cfg.rate_limit.admin_users_role_per_minute =
        detail::getenv_int_or<int>("RATE_LIMIT_ADMIN_USERS_ROLE_PER_MIN",
                                   cfg.rate_limit.admin_users_role_per_minute);
    cfg.rate_limit.admin_audit_logs_per_minute =
        detail::getenv_int_or<int>("RATE_LIMIT_ADMIN_AUDIT_LOGS_PER_MIN",
                                   cfg.rate_limit.admin_audit_logs_per_minute);
    cfg.rate_limit.admin_queue_per_minute =
        detail::getenv_int_or<int>("RATE_LIMIT_ADMIN_QUEUE_PER_MIN",
                                   cfg.rate_limit.admin_queue_per_minute);
    // v1.3.4 PR 3 — synchronous run-samples rate cap.
    cfg.rate_limit.sample_run_per_minute_per_user =
        detail::getenv_int_or<int>("SAMPLE_RUN_RATE_PER_MINUTE_PER_USER",
                                   cfg.rate_limit.sample_run_per_minute_per_user);

    // ── Login lockout (Phase 6 ☆ v1.2.46 — SPEC §15.1) ──────────────────
    cfg.login_lockout.enabled =
        detail::getenv_bool_or("LOGIN_LOCKOUT_ENABLED",
                               cfg.login_lockout.enabled);
    cfg.login_lockout.threshold =
        detail::getenv_int_or<int>("LOGIN_LOCKOUT_THRESHOLD",
                                   cfg.login_lockout.threshold);
    cfg.login_lockout.window_seconds =
        detail::getenv_int_or<int>("LOGIN_LOCKOUT_WINDOW_SECONDS",
                                   cfg.login_lockout.window_seconds);
    cfg.login_lockout.lockout_duration_seconds =
        detail::getenv_int_or<int>("LOGIN_LOCKOUT_DURATION_SECONDS",
                                   cfg.login_lockout.lockout_duration_seconds);
    if (cfg.login_lockout.threshold < 1)
        throw ConfigError("LOGIN_LOCKOUT_THRESHOLD must be >= 1");
    if (cfg.login_lockout.window_seconds < 1)
        throw ConfigError("LOGIN_LOCKOUT_WINDOW_SECONDS must be >= 1");
    if (cfg.login_lockout.lockout_duration_seconds < 1)
        throw ConfigError("LOGIN_LOCKOUT_DURATION_SECONDS must be >= 1");

    for (int q : {cfg.rate_limit.auth_register_per_minute_per_ip,
                  cfg.rate_limit.auth_login_per_minute_per_ip,
                  cfg.rate_limit.submission_per_minute_per_user,
                  cfg.rate_limit.admin_write_per_minute,
                  cfg.rate_limit.bulk_import_per_hour,
                  cfg.rate_limit.stats_ranking_per_minute_per_ip,
                  cfg.rate_limit.admin_users_list_per_minute,
                  cfg.rate_limit.admin_users_role_per_minute,
                  cfg.rate_limit.admin_audit_logs_per_minute,
                  cfg.rate_limit.admin_queue_per_minute,
                  cfg.rate_limit.sample_run_per_minute_per_user}) {
        if (q < 1)
            throw ConfigError("rate-limit quotas must be >= 1");
    }

    // ── Cookie (Phase 5 ★ — SPEC §6.3 / §15.1 / §15.3) ────────────────────
    //
    // Defaults follow SPEC §6.3: HttpOnly; Secure; SameSite=Strict.
    // We honor an explicit COOKIE_SECURE flag, but auto-flip Secure=true
    // in `production` environments so an operator who forgets to set
    // COOKIE_SECURE in a prod deploy still gets the safer default
    // (their browser will silently drop the cookie over plain HTTP, which
    // is the right failure mode — better than sending it in the clear).
    cfg.cookie.enabled    = detail::getenv_bool_or("COOKIE_ENABLED", true);
    cfg.cookie.http_only  = detail::getenv_bool_or("COOKIE_HTTP_ONLY", true);
    cfg.cookie.secure     = detail::getenv_bool_or(
        "COOKIE_SECURE",
        detail::getenv_or("LITECODE_ENV", "") == "production");
    cfg.cookie.same_site  = detail::getenv_or("COOKIE_SAME_SITE", cfg.cookie.same_site);
    cfg.cookie.path       = detail::getenv_or("COOKIE_PATH",      cfg.cookie.path);
    cfg.cookie.name       = detail::getenv_or("COOKIE_NAME",      cfg.cookie.name);
    cfg.cookie.max_age_seconds =
        detail::getenv_int_or<int>("COOKIE_MAX_AGE_SECONDS", 0);
    cfg.cookie.allow_body_fallback =
        detail::getenv_bool_or("COOKIE_ALLOW_BODY_FALLBACK", true);

    // Normalize SameSite to its three legal spellings (case-insensitive).
    {
        std::string ss;
        ss.reserve(cfg.cookie.same_site.size());
        for (char c : cfg.cookie.same_site)
            ss.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (ss != "strict" && ss != "lax" && ss != "none")
            throw ConfigError("COOKIE_SAME_SITE must be Strict, Lax, or None");
        if (ss == "none" && !cfg.cookie.secure)
            throw ConfigError("COOKIE_SAME_SITE=None requires COOKIE_SECURE=true "
                              "(browsers reject SameSite=None without Secure)");
        cfg.cookie.same_site = ss;
        // Capitalize for HTTP header friendliness.
        cfg.cookie.same_site[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(cfg.cookie.same_site[0])));
    }
    if (cfg.cookie.path.empty() || cfg.cookie.path[0] != '/')
        throw ConfigError("COOKIE_PATH must start with '/'");
    if (cfg.cookie.name.empty() ||
        cfg.cookie.name.find_first_of(" ;,=\"") != std::string::npos)
        throw ConfigError("COOKIE_NAME must be a non-empty token "
                          "(no spaces / ; , = or quotes)");
    if (cfg.cookie.max_age_seconds < 0)
        throw ConfigError("COOKIE_MAX_AGE_SECONDS must be >= 0 (0 means: "
                          "follow refresh_ttl_seconds at handler time)");

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

// Boot-time smoke check: load + validate the config and print a
// short summary. Returns 0 on success, non-zero on failure. Both
// litecode_server (main.cpp) and lit_smoke_check (smoke_check.cpp)
// call this at startup so a misconfigured container fails loudly
// rather than getting a 503 from every API.
//
// The dump line intentionally avoids `std::cout << config()` —
// AppConfig doesn't have a streaming operator (only a custom
// formatter used inside the AppConfig struct itself).
inline int run_config_smoke(const std::string& env_file_path = ".env",
                            bool override_env = false) {
    try {
        (void)init_config(env_file_path, override_env);
    } catch (const std::exception& e) {
        std::cerr << "[config] FAIL: " << e.what() << std::endl;
        return 1;
    }
    const auto& c = config();
    std::cout << "[config] PASS host=" << c.server.host
              << " port=" << c.server.port
              << " db=" << c.database.host << ":" << c.database.port
              << " log_level=" << c.logging.level << std::endl;
    return 0;
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
       << " import="                  << cfg.rate_limit.bulk_import_per_hour << "/hour"
       << " ranking="                 << cfg.rate_limit.stats_ranking_per_minute_per_ip << "/min"
       << " users_list="              << cfg.rate_limit.admin_users_list_per_minute << "/min"
       << " users_role="              << cfg.rate_limit.admin_users_role_per_minute << "/min"
       << " audit_logs="              << cfg.rate_limit.admin_audit_logs_per_minute << "/min"
       << " queue="                   << cfg.rate_limit.admin_queue_per_minute << "/min\n"
       << "  login_lockout="          << (cfg.login_lockout.enabled ? "on" : "off")
       << " threshold="               << cfg.login_lockout.threshold
       << " window="                  << cfg.login_lockout.window_seconds << "s"
       << " duration="                << cfg.login_lockout.lockout_duration_seconds << "s\n"
       << "  cookie.enabled="         << (cfg.cookie.enabled ? "true" : "false")
       << " cookie.name="             << cfg.cookie.name
       << " cookie.path="             << cfg.cookie.path
       << " HttpOnly="                << (cfg.cookie.http_only ? "yes" : "no")
       << " Secure="                  << (cfg.cookie.secure ? "yes" : "no")
       << " SameSite="                << cfg.cookie.same_site
       << " Max-Age="                 << (cfg.cookie.max_age_seconds == 0
                                            ? "<follow refresh_ttl>"
                                            : std::to_string(cfg.cookie.max_age_seconds) + "s")
       << " body_fallback="           << (cfg.cookie.allow_body_fallback ? "yes" : "no") << "\n"
       << "  admin_bootstrap="        << (cfg.admin_bootstrap.enabled ? cfg.admin_bootstrap.username : "<disabled>") << "\n"
       << "  env_file="               << (cfg.env_file_path.empty() ? "<none>" : cfg.env_file_path) << "\n"
       << "}";
    return os.str();
}

} // namespace litecode