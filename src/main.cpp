#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "config.h"
#include "logger.h"

#if defined(_WIN32)
inline void setenv_local(const char* k, const char* v) { _putenv_s(k, v); }
#else
inline void setenv_local(const char* k, const char* v) { setenv(k, v, 1); }
#endif

// Verify all dependencies are linkable
#include <httplib.h>           // cpp-httplib
#include <jwt-cpp/jwt.h>      // jwt-cpp
#include <nlohmann/json.hpp>   // nlohmann/json
#include <zlib.h>              // zlib
#include <openssl/ssl.h>       // OpenSSL
#include <mysqlx/xdevapi.h>    // mysql-connector-c++

// Verify bcrypt wrapper
#include "auth/password_hash.h"

namespace {

// Smoke-test the config layer end-to-end:
//   1. Inject a known-good env into the process (smoke build has no secrets).
//   2. init_config() succeeds.
//   3. Reject too-short JWT_SECRET.
//   4. Reject bad LOG_LEVEL.
int run_config_smoke() {
    // 64-char random-looking secret so we exercise the >= 32 byte path.
    setenv_local("JWT_SECRET",
                 "smoketest_smoketest_smoketest_smoketest_smoketest_smoketest_");
    setenv_local("DB_PASSWORD", "smoketest_pw");

    const litecode::AppConfig& cfg = litecode::init_config(".env");
    std::cout << "[config] " << litecode::redacted_dump(cfg) << std::endl;

    // Spec §5.1: short JWT_SECRET must be rejected.
    setenv_local("JWT_SECRET", "short");
    bool threw_on_short = false;
    try {
        litecode::load_config(".env");
    } catch (const litecode::ConfigError& e) {
        threw_on_short = true;
        std::cout << "[config] short-secret rejected: " << e.what() << std::endl;
    }
    if (!threw_on_short) {
        std::cerr << "[config] FAIL: short JWT_SECRET was accepted" << std::endl;
        return 1;
    }

    // Restore for the rest of the smoke test.
    setenv_local("JWT_SECRET",
                 "smoketest_smoketest_smoketest_smoketest_smoketest_smoketest_");

    // Bad log level.
    setenv_local("LOG_LEVEL", "LOUD");
    bool threw_on_log = false;
    try {
        litecode::load_config(".env");
    } catch (const litecode::ConfigError& e) {
        threw_on_log = true;
        std::cout << "[config] bad LOG_LEVEL rejected: " << e.what() << std::endl;
    }
    if (!threw_on_log) {
        std::cerr << "[config] FAIL: bad LOG_LEVEL was accepted" << std::endl;
        return 1;
    }
    setenv_local("LOG_LEVEL", "INFO");

    std::cout << "[config] PASS" << std::endl;
    return 0;
}

} // namespace

int main() {
    std::cout << "LiteCode-CPP starting..." << std::endl;

    if (int rc = run_config_smoke(); rc != 0) return rc;

    // ── Logger smoke test ──────────────────────────────────────────────────
    // Boot the process-wide logger from the loaded config and emit a few
    // lines so a human eyeballing `docker logs litecode-web` sees the new
    // structured format. JSON goes to stdout, optionally to a file under
    // LOG_FILE (see .env.example).
    {
        const auto& cfg = litecode::config();
        litecode::init_logger(cfg.logging);
        LOG_INFO ("boot complete",  {{"host",  cfg.server.host},
                                    {"port",  std::to_string(cfg.server.port)},
                                    {"log_level", cfg.logging.level}});
        {
            // Per-thread request_id stamp — pretend we're inside a request handler.
            litecode::RequestIdScope rid("boot-smoke-001");
            LOG_INFO ("logger smoke", {{"stage", "ready"}});
        }
        // Outside the scope: request_id is gone, but the next line is still
        // logged with whatever was there before (or nothing).
        LOG_DEBUG("debug suppressed at INFO level");
        LOG_WARN ("fallback: JWT secret came from insecure defaults",
                  {{"recommendation", "set JWT_SECRET in production"}});
    }

    // Verify bcrypt works
    std::string hash = litecode::password_hash("test123");
    bool ok = litecode::password_verify("test123", hash);
    std::cout << "bcrypt test: " << (ok ? "PASS" : "FAIL") << std::endl;

    // Verify jwt-cpp works
    auto token = jwt::create()
        .set_issuer("litecode")
        .set_subject("test")
        .sign(jwt::algorithm::hs256{"secret"});
    std::cout << "jwt-cpp test: token generated" << std::endl;

    // Verify nlohmann/json works
    nlohmann::json j = {{"status", "ok"}};
    std::cout << "json test: " << j.dump() << std::endl;

    // Verify zlib version
    std::cout << "zlib version: " << zlibVersion() << std::endl;

    // Verify OpenSSL version
    std::cout << "OpenSSL version: " << OpenSSL_version_num() << std::endl;

    std::cout << "All dependency checks passed." << std::endl;
    return 0;
}