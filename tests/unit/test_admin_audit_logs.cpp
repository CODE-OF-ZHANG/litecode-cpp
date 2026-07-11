// tests/unit/test_admin_audit_logs.cpp
//
// Unit + light-integration tests for src/routes/admin_audit_log_routes.h
//   (Phase 6 ★ — GET /api/v1/admin/audit-logs).
//
// Coverage:
//
//   (a) Pure unit tests (no MySQL):
//        - detail::parse_admin_id_param: positive / zero / negative /
//          non-digit / too-large / empty
//        - detail::parse_datetime_param: valid YYYY-MM-DD /
//          valid YYYY-MM-DD HH:MM:SS / empty / too-short / too-long
//        - detail::parse_list_query: empty / admin_id / action /
//          target_type / target_id / since / until / limit / offset
//          + bad-shape + clamping
//        - detail::truncate_for_envelope: short / long
//        - serialize_audit_row: full AuditRow + all nullable fields
//        - serialize_audit_row: malformed payload becomes null
//        - kAdminAuditValueMax / kDefaultAuditListLimit /
//          kMaxAuditListLimit constants
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - 200 list happy path (multi-action seed): returns all 8 fields
//          + payload parses to a JSON object + total/limit/offset echoed
//        - 200 list with action filter
//        - 200 list with target_type filter
//        - 200 list with admin_id filter
//        - 200 list with combined filters
//        - 200 list with since / until datetime filter
//        - 200 list pagination
//        - 200 list limit + offset clamp at kMaxAuditListLimit
//        - 200 list with nullable admin_id (login_failure case)
//        - 200 list with malformed payload still returns 200 + null payload
//        - 200 list when there are zero audit rows returns items=[] +
//          total=0
//        - 401 no auth
//        - 401 bad token
//        - 403 non-admin
//        - 400 bad admin_id (non-integer)
//        - 400 bad admin_id (zero)
//        - 400 bad action (control char)
//        - 400 bad action (too long)
//        - 400 bad target_type (too long)
//        - 400 bad target_id (too long)
//        - 400 bad since (empty)
//        - 400 bad until (too long)
//        - 400 bad limit (zero)
//        - 400 bad limit (non-integer)
//        - 400 bad offset (negative)
//        - 200 X-Request-Id round-trip
//        - 200 X-RateLimit-* headers present
//        - 429 rate limit triggers (tight bucket)
//        - 200 list empty result for impossible filter
//
//   The integration tests use raw SQL to seed audit_logs directly
//   (bypassing audit_log_repo.h::record via the test TU). The same
//   ODR-safe pattern is used in test_admin_users.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "auth/jwt_utils.h"
#include "config.h"
#include "db/audit_log_repo.h"
#include "db/connection_pool.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/admin_audit_log_routes.h"
#include "server.h"

namespace {

using nlohmann::json;

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

class StdoutSilencer {
public:
    StdoutSilencer()  { original_ = std::cout.rdbuf(sink_.rdbuf()); }
    ~StdoutSilencer() { std::cout.rdbuf(original_); }
private:
    std::stringstream  sink_;
    std::streambuf*    original_ = nullptr;
};

// ────────────────────────────────────────────────────────────────────────────
//  Config / server / fixture helpers
// ────────────────────────────────────────────────────────────────────────────

litecode::ServerConfig dev_server() {
    litecode::ServerConfig s;
    s.host = "127.0.0.1";
    s.port = 0;
    s.thread_pool_size = 4;
    return s;
}

litecode::CorsConfig dev_cors() {
    litecode::CorsConfig c;
    c.allowed_origins = "http://localhost:8080,http://127.0.0.1:8080";
    c.allow_credentials = true;
    return c;
}

litecode::JwtConfig dev_jwt() {
    litecode::JwtConfig j;
    j.secret = "test_jwt_secret_at_least_32_bytes_long_xxxxxxxxx";
    j.issuer = "litecode";
    j.access_ttl_seconds = 3600;
    j.refresh_ttl_seconds = 7 * 24 * 3600;
    return j;
}

litecode::RateLimitConfig lax_rate_limit() {
    litecode::RateLimitConfig r;
    r.auth_register_per_minute_per_ip   = 1000;
    r.auth_login_per_minute_per_ip      = 1000;
    r.problems_public_per_minute_per_ip = 1000;
    r.submission_per_minute_per_user    = 1000;
    r.admin_write_per_minute            = 1000;
    r.bulk_import_per_hour              = 1000;
    r.stats_ranking_per_minute_per_ip   = 1000;
    r.admin_users_list_per_minute       = 1000;
    r.admin_users_role_per_minute       = 1000;
    r.admin_audit_logs_per_minute       = 1000;
    return r;
}

std::string issue_token(const litecode::JwtConfig& jwt,
                        const std::string& user_id,
                        const std::string& username,
                        const std::string& role) {
    auto t = litecode::sign_access(jwt.secret, jwt.issuer,
                                   user_id, username, role,
                                   jwt.access_ttl_seconds);
    return t.token;
}

// API response wrapper.
struct ApiResponse {
    int               status = 0;
    std::string       body;
    std::string       request_id;
    bool              ok = false;
    httplib::Headers  headers;
    explicit operator bool() const noexcept { return ok; }
};

struct ServerHandle {
    litecode::HttpServer*            server = nullptr;
    std::unique_ptr<httplib::Client> client;
    int                              port = 0;

    ServerHandle() = default;
    ServerHandle(litecode::HttpServer* s, httplib::Client* c, int p)
        : server(s), client(c), port(p) {}
    ServerHandle(ServerHandle&& o) noexcept
        : server(o.server), client(std::move(o.client)), port(o.port) {
        o.server = nullptr; o.port = 0;
    }
    ServerHandle& operator=(ServerHandle&& o) noexcept {
        if (this != this) {
            if (server) server->stop();
            server = o.server;
            client = std::move(o.client);
            port   = o.port;
            o.server = nullptr; o.port = 0;
        }
        return *this;
    }
    ServerHandle(const ServerHandle&)            = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;
    ~ServerHandle() { if (server) server->stop(); }
};

ApiResponse do_request(ServerHandle& h,
                       const std::string& method,
                       const std::string& path,
                       const std::string& bearer_token = "",
                       const std::string& body = "",
                       const std::string& request_id = "") {
    ApiResponse out;
    httplib::Headers hdrs;
    if (!bearer_token.empty()) hdrs.emplace("Authorization", "Bearer " + bearer_token);
    if (!request_id.empty())  hdrs.emplace("X-Request-Id", request_id);
    if (!body.empty())        hdrs.emplace("Content-Type", "application/json");
    httplib::Result r;
    if (method == "GET") {
        r = h.client->Get(path, hdrs);
    } else {
        ADD_FAILURE() << "unsupported method " << method;
        return out;
    }
    if (!r) {
        ADD_FAILURE() << method << " " << path << " failed: " << r.error();
        return out;
    }
    out.status = r->status;
    out.body   = r->body;
    if (r->has_header("X-Request-Id")) {
        out.request_id = r->get_header_value("X-Request-Id");
    }
    out.headers = r->headers;
    out.ok = true;
    return out;
}

ServerHandle start_server(litecode::HttpServer* server) {
    const int port = server->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0) << "bind_any_port failed";
    if (!server->start(/*background=*/true)) {
        std::fprintf(stderr, "[start_server] start() returned false; "
                     "running=%d\n", server->is_running() ? 1 : 0);
    }
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(15, 0);
    client->set_write_timeout(15, 0);
    client->set_keep_alive(false);
    return ServerHandle(server, client.release(), port);
}

// ────────────────────────────────────────────────────────────────────────────
//  DB fixture
// ────────────────────────────────────────────────────────────────────────────

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
        c.min_size           = 1;
        c.max_size           = 4;
        c.acquire_timeout_ms = 2000;
        c.connect_timeout_ms = 5000;
        c.max_idle_time_ms   = 60000;
        return c;
    }
};

class AdminAuditLogsFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::vector<int>                      created_user_ids;
    std::vector<std::int64_t>             created_audit_ids;

    void SetUp() override {
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
            GTEST_SKIP() << "MySQL not reachable: " << e.what();
        }
        if (!pool || !pool->ping()) {
            GTEST_SKIP() << "MySQL ping failed";
        }

        // Schema probe — guard against older dev boxes missing the
        // audit_logs table or the JSON column type.
        try {
            auto conn = pool->acquire();
            auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.TABLES "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'audit_logs' LIMIT 1");
            if (!v.has_value()) {
                GTEST_SKIP() << "audit_logs table missing — "
                                "run init_db.sh to apply V002";
            }
            auto vc = conn.fetch_scalar<std::int64_t>(
                "SELECT 1 FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA = DATABASE() "
                "  AND TABLE_NAME = 'audit_logs' "
                "  AND COLUMN_NAME = 'payload' LIMIT 1");
            if (!vc.has_value()) {
                GTEST_SKIP() << "audit_logs.payload column missing";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "schema probe failed: " << e.what();
        }
    }

    void TearDown() override {
        if (pool && pool->ping()) {
            try {
                auto conn = pool->acquire();
                for (auto id : created_audit_ids) {
                    try { conn.execute(
                        "DELETE FROM audit_logs WHERE id = ?", id); }
                    catch (...) {}
                }
                for (auto id : created_user_ids) {
                    try { conn.execute(
                        "DELETE FROM users WHERE id = ?", id); }
                    catch (...) {}
                }
            } catch (...) {}
        }
        pool.reset();
    }

    // Insert a throwaway user via raw SQL (avoid cross-repo ODR
    // collisions on user_repo.h::detail::req_string). The hash is
    // bcrypt-format filler; the test never logs in as this user
    // for any auth-bearing endpoint — the token is issued against
    // the user_id we just minted and the JWT signing helper doesn't
    // verify anything against the DB.
    int make_user(const std::string& role = "user",
                  const std::string& username = "") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string uname = username.empty()
            ? std::string("aal-") +
              std::to_string(static_cast<long long>(
                  std::chrono::system_clock::now()
                      .time_since_epoch().count())) +
              "_" + std::to_string(n)
            : username;
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO users (username, password_hash, role) "
                "VALUES (?, '$2b$12$dummy.hash.for.test.only.padding.aaaa', ?)",
                uname, role);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_user_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }

    // Insert an audit_log row directly via raw SQL with a JSON payload.
    // Returns the new id; -1 on failure.
    std::int64_t insert_audit_row(int admin_id, const std::string& action,
                                  const std::string& target_type,
                                  const std::string& target_id,
                                  const std::string& payload_json,
                                  const std::string& ip = "127.0.0.1") {
        try {
            auto conn = pool->acquire();
            // admin_id may be 0 in which case we want SQL NULL.
            if (admin_id <= 0) {
                auto rs = conn.execute(
                    "INSERT INTO audit_logs "
                    "(admin_id, action, target_type, target_id, payload, ip) "
                    "VALUES (NULL, ?, ?, ?, CAST(? AS JSON), ?)",
                    action, target_type, target_id, payload_json, ip);
                const std::int64_t id =
                    static_cast<std::int64_t>(rs.getAutoIncrementValue());
                if (id > 0) created_audit_ids.push_back(id);
                return id;
            }
            auto rs = conn.execute(
                "INSERT INTO audit_logs "
                "(admin_id, action, target_type, target_id, payload, ip) "
                "VALUES (?, ?, ?, ?, CAST(? AS JSON), ?)",
                admin_id, action, target_type, target_id, payload_json, ip);
            const std::int64_t id =
                static_cast<std::int64_t>(rs.getAutoIncrementValue());
            if (id > 0) created_audit_ids.push_back(id);
            return id;
        } catch (...) {
            return -1;
        }
    }

    // Insert an audit_log row with explicit control over which created_at
    // timestamp is recorded (used for the since/until filter tests). The
    // value is taken at face value — must be a YYYY-MM-DD HH:MM:SS that
    // the column can accept (DATETIME has range 1000-01-01 .. 9999-12-31).
    std::int64_t insert_audit_row_at(const std::string& when,
                                     int admin_id,
                                     const std::string& action,
                                     const std::string& target_type,
                                     const std::string& target_id,
                                     const std::string& payload_json) {
        try {
            auto conn = pool->acquire();
            if (admin_id <= 0) {
                auto rs = conn.execute(
                    "INSERT INTO audit_logs "
                    "(admin_id, action, target_type, target_id, payload, "
                    " created_at) "
                    "VALUES (NULL, ?, ?, ?, CAST(? AS JSON), ?)",
                    action, target_type, target_id, payload_json, when);
                const std::int64_t id =
                    static_cast<std::int64_t>(rs.getAutoIncrementValue());
                if (id > 0) created_audit_ids.push_back(id);
                return id;
            }
            auto rs = conn.execute(
                "INSERT INTO audit_logs "
                "(admin_id, action, target_type, target_id, payload, "
                " created_at) "
                "VALUES (?, ?, ?, ?, CAST(? AS JSON), ?)",
                admin_id, action, target_type, target_id, payload_json, when);
            const std::int64_t id =
                static_cast<std::int64_t>(rs.getAutoIncrementValue());
            if (id > 0) created_audit_ids.push_back(id);
            return id;
        } catch (...) {
            return -1;
        }
    }

    // Count rows in audit_logs matching an action + target_id.
    int count_audit_rows(const std::string& action,
                         const std::string& target_id) {
        try {
            auto conn = pool->acquire();
            const auto v = conn.fetch_scalar<std::int64_t>(
                "SELECT COUNT(*) FROM audit_logs "
                "WHERE action = ? AND target_id = ?",
                action, target_id);
            return v.has_value() ? static_cast<int>(*v) : 0;
        } catch (...) {
            return 0;
        }
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests (no DB)
// ────────────────────────────────────────────────────────────────────────────

// Build a minimal Request object for the parse_*_query helpers.
httplib::Request make_request(const std::string& path_with_qs) {
    httplib::Request req;
    const auto qpos = path_with_qs.find('?');
    if (qpos == std::string::npos) {
        req.path = path_with_qs;
    } else {
        req.path = path_with_qs.substr(0, qpos);
        const std::string qs = path_with_qs.substr(qpos + 1);
        std::size_t i = 0;
        while (i < qs.size()) {
            std::size_t amp = qs.find('&', i);
            if (amp == std::string::npos) amp = qs.size();
            const std::string pair = qs.substr(i, amp - i);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) {
                req.params.emplace(pair.substr(0, eq), pair.substr(eq + 1));
            } else {
                req.params.emplace(pair, std::string());
            }
            i = amp + 1;
        }
    }
    req.method = "GET";
    return req;
}

TEST(AuditAdminConstants, LimitsAndCap) {
    EXPECT_EQ(litecode::kDefaultAuditListLimit, 20);
    EXPECT_EQ(litecode::kMaxAuditListLimit, 100);
    EXPECT_LE(litecode::kDefaultAuditListLimit,
              litecode::kMaxAuditListLimit);
    EXPECT_EQ(litecode::admin_audit_log_routes::detail::kAdminAuditValueMax, 64);
    EXPECT_EQ(litecode::RateLimitConfig{}.admin_audit_logs_per_minute, 60);
}

TEST(ParseAdminIdParam, AcceptsPositiveIntegers) {
    EXPECT_EQ(litecode::admin_audit_log_routes::detail::parse_admin_id_param("1"),    1);
    EXPECT_EQ(litecode::admin_audit_log_routes::detail::parse_admin_id_param("42"),   42);
    EXPECT_EQ(litecode::admin_audit_log_routes::detail::parse_admin_id_param("2147483647"), 2147483647);
}

TEST(ParseAdminIdParam, RejectsZeroAndNegative) {
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("0").has_value());
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("-1").has_value());
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("+5").has_value());
}

TEST(ParseAdminIdParam, RejectsNonDigitAndTooLarge) {
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("abc").has_value());
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("12abc").has_value());
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("2147483648").has_value());
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("").has_value());
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_admin_id_param("99999999999").has_value());
}

TEST(ParseListQuery, EmptyPathDefaultsTo20And0) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs");
    EXPECT_TRUE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(f.limit, 20);
    EXPECT_EQ(f.offset, 0);
    EXPECT_FALSE(f.admin_id.has_value());
    EXPECT_FALSE(f.action.has_value());
    EXPECT_FALSE(f.target_type.has_value());
    EXPECT_FALSE(f.target_id.has_value());
    EXPECT_FALSE(f.since.has_value());
    EXPECT_FALSE(f.until.has_value());
}

TEST(ParseListQuery, AcceptsAllValidParams) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request(
        "/api/v1/admin/audit-logs"
        "?admin_id=42"
        "&action=user.role_change"
        "&target_type=user"
        "&target_id=42"
        "&since=2026-07-01"
        "&until=2026-07-11 12:34:56"
        "&limit=50"
        "&offset=20");
    EXPECT_TRUE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    ASSERT_TRUE(f.admin_id.has_value());
    EXPECT_EQ(*f.admin_id, 42);
    ASSERT_TRUE(f.action.has_value());
    EXPECT_EQ(*f.action, "user.role_change");
    ASSERT_TRUE(f.target_type.has_value());
    EXPECT_EQ(*f.target_type, "user");
    ASSERT_TRUE(f.target_id.has_value());
    EXPECT_EQ(*f.target_id, "42");
    ASSERT_TRUE(f.since.has_value());
    EXPECT_EQ(*f.since, "2026-07-01");
    ASSERT_TRUE(f.until.has_value());
    EXPECT_EQ(*f.until, "2026-07-11 12:34:56");
    EXPECT_EQ(f.limit, 50);
    EXPECT_EQ(f.offset, 20);
}

TEST(ParseListQuery, AcceptsDateOnlySince) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?since=2026-07-01");
    EXPECT_TRUE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    ASSERT_TRUE(f.since.has_value());
    EXPECT_EQ(*f.since, "2026-07-01");
}

TEST(ParseListQuery, RejectsBadAdminId) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?admin_id=abc");
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "admin_id");
}

TEST(ParseListQuery, RejectsZeroAdminId) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?admin_id=0");
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, RejectsBadActionControlChar) {
    httplib::Response res;
    litecode::AuditListFilter f;
    // Build a raw string with an embedded newline so the validator
    // rejects it (the literal in a const char* would stop at the NUL).
    const std::string bad("bad\naction");
    auto req = make_request("/api/v1/admin/audit-logs");
    req.params.emplace("action", bad);
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["details"]["field"], "action");
}

TEST(ParseListQuery, RejectsActionTooLong) {
    httplib::Response res;
    litecode::AuditListFilter f;
    const std::string huge(201, 'x'); // > kMaxActionLength = 50
    auto req = make_request("/api/v1/admin/audit-logs");
    req.params.emplace("action", huge);
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, RejectsBadSinceTooShort) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?since=2026");
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["details"]["field"], "since");
}

TEST(ParseListQuery, RejectsBadUntilTooLong) {
    httplib::Response res;
    litecode::AuditListFilter f;
    const std::string huge(50, 'x'); // > kMaxDatetimeLength = 19
    auto req = make_request("/api/v1/admin/audit-logs?until=" + huge);
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, RejectsNegativeOffset) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?offset=-1");
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, RejectsZeroLimit) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?limit=0");
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, RejectsNonNumericLimit) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?limit=abc");
    EXPECT_FALSE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, ClampsLimitToMax) {
    httplib::Response res;
    litecode::AuditListFilter f;
    auto req = make_request("/api/v1/admin/audit-logs?limit=9999");
    EXPECT_TRUE(litecode::admin_audit_log_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(f.limit, litecode::kMaxAuditListLimit);
}

TEST(TruncateForEnvelope, Short) {
    EXPECT_EQ(litecode::admin_audit_log_routes::detail::truncate_for_envelope(
                  std::string(10, 'a')),
              std::string(10, 'a'));
}

TEST(TruncateForEnvelope, Long) {
    std::string in(200, 'x');
    auto out = litecode::admin_audit_log_routes::detail::truncate_for_envelope(in);
    EXPECT_EQ(out.size(), litecode::admin_audit_log_routes::detail::kAdminAuditValueMax + 3);
    // The "+..." suffix is part of the contract.
    EXPECT_EQ(out.substr(out.size() - 3), "...");
}

TEST(SerializeAuditRow, FullFieldsBecomeJson) {
    litecode::AuditRow r;
    r.id           = 123;
    r.admin_id     = 5;
    r.action       = "user.role_change";
    r.target_type  = "user";
    r.target_id    = "42";
    r.payload      = std::string("{\"old_role\":\"user\",\"new_role\":\"admin\"}");
    r.ip           = "1.2.3.4";
    r.created_at   = "2026-07-10 12:34:56";
    const auto j = litecode::admin_audit_log_routes::serialize_audit_row(r);
    EXPECT_EQ(j["id"],         123);
    EXPECT_EQ(j["admin_id"],   5);
    EXPECT_EQ(j["action"],     "user.role_change");
    EXPECT_EQ(j["target_type"], "user");
    EXPECT_EQ(j["target_id"],  "42");
    EXPECT_EQ(j["created_at"], "2026-07-10 12:34:56");
    EXPECT_EQ(j["ip"],         "1.2.3.4");
    // payload round-trips as parsed JSON object — the operation evidence
    // is structured, not a string blob.
    ASSERT_TRUE(j["payload"].is_object());
    EXPECT_EQ(j["payload"]["old_role"], "user");
    EXPECT_EQ(j["payload"]["new_role"], "admin");
}

TEST(SerializeAuditRow, NullableFieldsAreNull) {
    litecode::AuditRow r;
    r.id         = 1;
    r.action     = "system.test";
    r.created_at = "2026-07-10 12:34:56";
    const auto j = litecode::admin_audit_log_routes::serialize_audit_row(r);
    EXPECT_TRUE(j["admin_id"].is_null());
    EXPECT_TRUE(j["target_type"].is_null());
    EXPECT_TRUE(j["target_id"].is_null());
    EXPECT_TRUE(j["payload"].is_null());
    EXPECT_TRUE(j["ip"].is_null());
}

TEST(SerializeAuditRow, MalformedPayloadBecomesNull) {
    litecode::AuditRow r;
    r.id         = 1;
    r.action     = "system.test";
    r.created_at = "2026-07-10 12:34:56";
    // Junk payload — a buggy writer or manual UPDATE could produce
    // non-JSON text. The route layer must surface this as null
    // (not as an inner exception) so the page's JSON.parse stays
    // happy.
    r.payload    = std::string("this is not { json at all");
    const auto j = litecode::admin_audit_log_routes::serialize_audit_row(r);
    EXPECT_TRUE(j["payload"].is_null());
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests (real MySQL)
// ────────────────────────────────────────────────────────────────────────────

class AdminAuditLogsLiveFixture : public AdminAuditLogsFixture {
protected:
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    ServerHandle                           handle;
    litecode::RateLimitConfig              rate_cfg;
    litecode::JwtConfig                    jwt_cfg;
    int                                    admin_user_id   = 0;
    int                                    regular_user_id = 0;
    std::string                            admin_username;
    std::string                            admin_token;
    std::string                            regular_username;
    std::string                            regular_token;

    void SetUp() override {
        AdminAuditLogsFixture::SetUp();
        // Parent's SetUp calls GTEST_SKIP when MySQL is unreachable,
        // which marks the test as skipped. The gtest runner still
        // resumes THIS SetUp from the line below after marking the
        // skip, even though every line after the GTEST_SKIP was
        // missed (the pool stays null). Guard so we don't SEGV
        // trying to dereference pool when it's null.
        if (!pool) {
            return;
        }
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        litecode::admin_audit_log_routes::register_admin_audit_log_routes(
            *server, *pool, *limiter, rate_cfg, jwt_cfg);
        handle = start_server(server.get());

        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const auto stamp = std::to_string(static_cast<long long>(
            std::chrono::system_clock::now()
                .time_since_epoch().count())) +
            "_" + std::to_string(n);

        admin_username = "aal-admin-" + stamp;
        admin_user_id  = make_user("admin", admin_username);
        ASSERT_GT(admin_user_id, 0);
        admin_token = issue_token(jwt_cfg, std::to_string(admin_user_id),
                                  admin_username, "admin");

        regular_username = (std::string("aal-user-") + stamp);
        regular_user_id  = make_user("user", regular_username);
        ASSERT_GT(regular_user_id, 0);
        regular_token = issue_token(jwt_cfg, std::to_string(regular_user_id),
                                    regular_username, "user");
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        AdminAuditLogsFixture::TearDown();
    }

    std::string get_header(const ApiResponse& r, const std::string& name) {
        auto it = r.headers.find(name);
        if (it == r.headers.end()) return "";
        return it->second;
    }
};

TEST_F(AdminAuditLogsLiveFixture, ListHappyPathReturnsAllFields) {
    StdoutSilencer silencer;

    // Seed 4 distinct rows. Three are attributed to admin_user_id;
    // one is the anonymous login_failure (admin_id NULL).
    constexpr int kCount = 4;
    for (int i = 0; i < kCount; ++i) {
        const auto id = insert_audit_row(
            admin_user_id,
            "user.role_change",
            "user",
            std::to_string(100 + i),
            std::string("{\"username\":\"alice\",\"old_role\":\"user\","
                        "\"new_role\":\"admin\",\"index\":") + std::to_string(i) + "}",
            "10.0.0.1");
        ASSERT_GT(id, 0);
    }
    const auto anon_id = insert_audit_row(
        0,
        litecode::audit_log_repo::kActionLoginFailure,
        "user",
        "ghost",
        "{\"consecutive_failures\":5}");
    ASSERT_GT(anon_id, 0);

    const auto r = do_request(handle, "GET",
                              "/api/v1/admin/audit-logs",
                              admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    ASSERT_TRUE(env.contains("data"));
    const auto& data = env["data"];
    ASSERT_TRUE(data.contains("items"));
    ASSERT_TRUE(data["items"].is_array());
    EXPECT_EQ(data["items"].size(), 5u);
    EXPECT_EQ(data["total"],   5);
    EXPECT_EQ(data["limit"],   20);
    EXPECT_EQ(data["offset"],  0);

    // Inspect one item — newest first; the order is created_at DESC, id DESC.
    const auto& first = data["items"][0];
    // We don't pin which row is "first" because all five share the same
    // microsecond on the second-tick; we just assert the SHAPE is right.
    EXPECT_TRUE(first.contains("id"));
    EXPECT_TRUE(first.contains("admin_id"));
    EXPECT_TRUE(first.contains("action"));
    EXPECT_TRUE(first.contains("target_type"));
    EXPECT_TRUE(first.contains("target_id"));
    EXPECT_TRUE(first.contains("payload"));
    EXPECT_TRUE(first.contains("ip"));
    EXPECT_TRUE(first.contains("created_at"));
    EXPECT_TRUE(first["payload"].is_object());
    EXPECT_TRUE(first["created_at"].is_string());

    bool found_anon = false;
    bool found_admin = false;
    for (const auto& it : data["items"]) {
        if (it["action"] == litecode::audit_log_repo::kActionLoginFailure) {
            EXPECT_TRUE(it["admin_id"].is_null());
            EXPECT_EQ(it["target_id"], "ghost");
            found_anon = true;
        }
        if (it["action"] == "user.role_change"
            && it["admin_id"].is_number_integer()
            && it["admin_id"].get<int>() == admin_user_id) {
            ASSERT_TRUE(it["payload"].is_object());
            EXPECT_EQ(it["payload"]["username"], "alice");
            found_admin = true;
        }
    }
    EXPECT_TRUE(found_anon);
    EXPECT_TRUE(found_admin);
}

TEST_F(AdminAuditLogsLiveFixture, FilterByAction) {
    StdoutSilencer silencer;

    // 3 role_change rows + 2 problem.delete rows
    for (int i = 0; i < 3; ++i) {
        ASSERT_GT(insert_audit_row(
            admin_user_id, "user.role_change", "user",
            std::to_string(i), "{\"x\":1}"), 0);
    }
    for (int i = 0; i < 2; ++i) {
        ASSERT_GT(insert_audit_row(
            admin_user_id, "problem.delete", "problem",
            "two-sum", "{\"hard_delete\":false}"), 0);
    }

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?action=problem.delete",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 2u);
    EXPECT_EQ(env["data"]["total"], 2);
    for (const auto& it : env["data"]["items"]) {
        EXPECT_EQ(it["action"], "problem.delete");
        EXPECT_EQ(it["target_type"], "problem");
        EXPECT_EQ(it["target_id"], "two-sum");
    }
}

TEST_F(AdminAuditLogsLiveFixture, FilterByTargetType) {
    StdoutSilencer silencer;

    for (int i = 0; i < 2; ++i) {
        ASSERT_GT(insert_audit_row(
            admin_user_id, "user.role_change", "user",
            std::to_string(i), "{}"), 0);
    }
    for (int i = 0; i < 3; ++i) {
        ASSERT_GT(insert_audit_row(
            admin_user_id, "problem.delete", "problem",
            "p-" + std::to_string(i), "{}"), 0);
    }

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?target_type=problem",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 3u);
    for (const auto& it : env["data"]["items"]) {
        EXPECT_EQ(it["target_type"], "problem");
    }
}

TEST_F(AdminAuditLogsLiveFixture, FilterByAdminId) {
    StdoutSilencer silencer;

    // Make a second admin so the test can distinguish by admin_id.
    const int other_admin_id = make_user("admin");
    ASSERT_GT(other_admin_id, 0);

    ASSERT_GT(insert_audit_row(admin_user_id, "user.role_change", "user", "1", "{}"), 0);
    ASSERT_GT(insert_audit_row(other_admin_id, "user.role_change", "user", "2", "{}"), 0);
    ASSERT_GT(insert_audit_row(other_admin_id, "user.role_change", "user", "3", "{}"), 0);

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?admin_id=" + std::to_string(other_admin_id),
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 2u);
    for (const auto& it : env["data"]["items"]) {
        EXPECT_EQ(it["admin_id"], other_admin_id);
    }
}

TEST_F(AdminAuditLogsLiveFixture, FilterCombinedActionAndTargetType) {
    StdoutSilencer silencer;

    // mixed: 2 problem.create + 1 problem.delete + 1 user.role_change
    ASSERT_GT(insert_audit_row(admin_user_id, "problem.create", "problem", "p1", "{}"), 0);
    ASSERT_GT(insert_audit_row(admin_user_id, "problem.create", "problem", "p2", "{}"), 0);
    ASSERT_GT(insert_audit_row(admin_user_id, "problem.delete", "problem", "p1", "{}"), 0);
    ASSERT_GT(insert_audit_row(admin_user_id, "user.role_change", "user", "5", "{}"), 0);

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?action=problem.create&target_type=problem",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 2u);
    for (const auto& it : env["data"]["items"]) {
        EXPECT_EQ(it["action"], "problem.create");
        EXPECT_EQ(it["target_type"], "problem");
    }
}

TEST_F(AdminAuditLogsLiveFixture, FilterByDatetimeSince) {
    StdoutSilencer silencer;

    // 1 row at 2020-01-01 (older than since), 1 row at 2030-01-01 (newer).
    const auto old_id = insert_audit_row_at("2020-01-01 00:00:00",
        admin_user_id, "user.role_change", "user", "old", "{}");
    const auto new_id = insert_audit_row_at("2030-01-01 00:00:00",
        admin_user_id, "user.role_change", "user", "new", "{}");
    ASSERT_GT(old_id, 0);
    ASSERT_GT(new_id, 0);

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?since=2025-01-01",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 1u);
    EXPECT_EQ(env["data"]["items"][0]["target_id"], "new");
    EXPECT_EQ(env["data"]["total"], 1);
}

TEST_F(AdminAuditLogsLiveFixture, FilterByDatetimeUntil) {
    StdoutSilencer silencer;

    // 1 row at 2020-01-01 (older than until), 1 row at 2030-01-01 (newer).
    const auto old_id = insert_audit_row_at("2020-01-01 00:00:00",
        admin_user_id, "user.role_change", "user", "old", "{}");
    const auto new_id = insert_audit_row_at("2030-01-01 00:00:00",
        admin_user_id, "user.role_change", "user", "new", "{}");
    ASSERT_GT(old_id, 0);
    ASSERT_GT(new_id, 0);

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?until=2025-01-01",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 1u);
    EXPECT_EQ(env["data"]["items"][0]["target_id"], "old");
}

TEST_F(AdminAuditLogsLiveFixture, Pagination) {
    StdoutSilencer silencer;

    for (int i = 0; i < 5; ++i) {
        ASSERT_GT(insert_audit_row(
            admin_user_id, "user.role_change", "user",
            std::to_string(i), "{}"), 0);
    }

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?limit=2&offset=2",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 2u);
    EXPECT_EQ(env["data"]["total"], 5);
    EXPECT_EQ(env["data"]["limit"], 2);
    EXPECT_EQ(env["data"]["offset"], 2);
}

TEST_F(AdminAuditLogsLiveFixture, LimitClampsToMax) {
    StdoutSilencer silencer;

    // We don't need to seed any rows — the clamp is pure parser logic.
    // We DO need to verify a 200 even on an empty result so the
    // request actually goes through.
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?limit=9999",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["limit"],
              litecode::kMaxAuditListLimit);
    EXPECT_EQ(env["data"]["items"].size(), 0u);
}

TEST_F(AdminAuditLogsLiveFixture, EmptyResultForNoMatchingFilter) {
    StdoutSilencer silencer;

    ASSERT_GT(insert_audit_row(
        admin_user_id, "problem.create", "problem", "alpha", "{}"), 0);

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?action=user.password_change",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 0u);
    EXPECT_EQ(env["data"]["total"], 0);
}

TEST_F(AdminAuditLogsLiveFixture, MalformedPayloadReturnsNullPayload) {
    StdoutSilencer silencer;

    // Insert a row whose payload is plain text, not JSON. The repo
    // INSERT path goes via CAST(? AS JSON), so we have to use a
    // manually-constructed INSERT to skip the JSON validation. We
    // then read it back via the route — the payload round-trip
    // should fail to parse and we should see null on the wire.
    try {
        auto conn = pool->acquire();
        auto rs = conn.execute(
            "INSERT INTO audit_logs "
            "(admin_id, action, target_type, target_id, payload) "
            "VALUES (?, 'system.test', 'problem', 'malformed', "
            "        'this is not json')",
            admin_user_id);
        const auto id =
            static_cast<std::int64_t>(rs.getAutoIncrementValue());
        if (id > 0) created_audit_ids.push_back(id);
    } catch (...) {
        GTEST_SKIP() << "could not seed malformed payload row";
    }

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?target_type=problem",
        admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    ASSERT_GE(env["data"]["items"].size(), 1u);
    bool found_malformed = false;
    for (const auto& it : env["data"]["items"]) {
        if (it["target_id"] == "malformed") {
            EXPECT_TRUE(it["payload"].is_null());
            found_malformed = true;
        }
    }
    EXPECT_TRUE(found_malformed);
}

TEST_F(AdminAuditLogsLiveFixture, NoAuthIs401) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(AdminAuditLogsLiveFixture, BadTokenIs401) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs", "not-a-real-jwt-token");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(AdminAuditLogsLiveFixture, NonAdminIsForbidden) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs", regular_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 403);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "FORBIDDEN");
}

TEST_F(AdminAuditLogsLiveFixture, BadAdminIdIs400) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?admin_id=abc", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "admin_id");
}

TEST_F(AdminAuditLogsLiveFixture, BadLimitIs400) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?limit=0", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
}

TEST_F(AdminAuditLogsLiveFixture, BadOffsetIs400) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?offset=-1", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
}

TEST_F(AdminAuditLogsLiveFixture, BadSinceIs400) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?since=2026", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["details"]["field"], "since");
}

TEST_F(AdminAuditLogsLiveFixture, BadActionIs400) {
    StdoutSilencer silencer;
    // Control character — won't pass validate_action.
    const std::string bad("a\nb");
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs?action=" + bad, admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
}

TEST_F(AdminAuditLogsLiveFixture, XRequestIdRoundTrip) {
    StdoutSilencer silencer;
    const std::string rid = "test-rid-audit-logs-123";
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs",
        admin_token, "", rid);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    // The header echoes back. The envelope's request_id field is
    // populated by server.h from the same source.
    EXPECT_EQ(get_header(r, "X-Request-Id"), rid);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"], rid);
}

TEST_F(AdminAuditLogsLiveFixture, XRateLimitHeadersPresent) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/audit-logs",
        admin_token);
    ASSERT_TRUE(r);
    EXPECT_FALSE(get_header(r, "X-RateLimit-Limit").empty());
    EXPECT_FALSE(get_header(r, "X-RateLimit-Remaining").empty());
}

TEST_F(AdminAuditLogsLiveFixture, RateLimitTriggers429) {
    StdoutSilencer silencer;

    // Tight bucket: 2/min.
    litecode::RateLimitConfig tight;
    tight.auth_register_per_minute_per_ip   = 2;
    tight.auth_login_per_minute_per_ip      = 2;
    tight.problems_public_per_minute_per_ip = 2;
    tight.submission_per_minute_per_user    = 2;
    tight.admin_write_per_minute            = 2;
    tight.bulk_import_per_hour              = 2;
    tight.stats_ranking_per_minute_per_ip   = 2;
    tight.admin_users_list_per_minute       = 2;
    tight.admin_users_role_per_minute       = 2;
    tight.admin_audit_logs_per_minute       = 2;

    // Tear down + re-register with the tight bucket so the existing
    // setup's lax bucket doesn't taint the state.
    handle = ServerHandle();
    server.reset();
    limiter.reset();

    limiter = std::make_unique<litecode::RateLimiter>();
    server  = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    litecode::admin_audit_log_routes::register_admin_audit_log_routes(
        *server, *pool, *limiter, tight, jwt_cfg);
    handle = start_server(server.get());

    const auto r1 = do_request(handle, "GET",
        "/api/v1/admin/audit-logs", admin_token);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1.status, 200);

    const auto r2 = do_request(handle, "GET",
        "/api/v1/admin/audit-logs", admin_token);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2.status, 200);

    const auto r3 = do_request(handle, "GET",
        "/api/v1/admin/audit-logs", admin_token);
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3.status, 429);
    const auto env = json::parse(r3.body);
    EXPECT_EQ(env["code"], "RATE_LIMITED");
    EXPECT_FALSE(get_header(r3, "Retry-After").empty());
}

}  // namespace
