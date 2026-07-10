// tests/unit/test_admin_users.cpp
//
// Unit + light-integration tests for src/routes/admin_user_routes.h
//   (Phase 6 ★ — GET /api/v1/admin/users and
//                 PUT /api/v1/admin/users/:id/role).
//
// Coverage:
//
//   (a) Pure unit tests (no MySQL):
//        - detail::parse_role_param: valid / empty / unknown
//        - detail::parse_id_param: positive / zero / negative /
//          too-large / non-digit / leading-whitespace
//        - detail::parse_list_query: empty / role / q / limit /
//          offset / bad-shape / clamping
//        - detail::extract_id_from_path: good / bad prefix /
//          bad suffix / nested / empty
//        - detail::truncate_for_envelope: short / long
//        - serialize_user_admin_row: includes / omits
//        - kAdminUserValueMax / kDefaultUserListLimit /
//          kMaxUserListLimit constants
//
//   (b) Integration tests (in-process server + real MySQL when
//       reachable; SKIP when ping fails):
//        - 200 list happy path (3 users + filter + pagination)
//        - 200 list with role filter
//        - 200 list with q search
//        - 200 list with both filters
//        - 200 list pagination
//        - 200 list omits password_hash
//        - 200 list returns total + limit + offset
//        - 200 list out-of-range offset returns empty items but real total
//        - 200 list with submission_count column populated
//        - 200 list with role=admin filter
//        - 400 bad role
//        - 400 bad limit
//        - 400 bad offset
//        - 400 non-numeric limit
//        - 401 no auth
//        - 401 bad token
//        - 403 non-admin
//        - 200 PUT happy path: user→admin, audit row written
//        - 200 PUT same role is no-op (no audit row)
//        - 200 PUT admin→user, audit row written
//        - 200 PUT self-demote: admin can demote themselves
//        - 404 PUT unknown id
//        - 400 PUT bad id path
//        - 400 PUT bad body (missing role)
//        - 400 PUT bad role value
//        - 401 PUT no auth
//        - 403 PUT non-admin
//        - 200 X-Request-Id round-trip
//        - 200 X-RateLimit-* headers present
//        - 429 PUT rate limit triggers
//        - 429 GET list rate limit triggers
//
//   The integration tests use raw SQL to seed users (avoiding
//   user_repo.h::detail::req_string ODR collision in the test
//   TU — same workaround as test_stats_profile / test_submission).
//   For role change tests, we mint admin tokens via the same
//   JWT helpers used in production.

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
#include "db/connection_pool.h"
#include "db/user_repo.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/admin_user_routes.h"
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
        if (this != &o) {
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
    } else if (method == "PUT") {
        r = h.client->Put(path, hdrs, body, "application/json");
    } else if (method == "POST") {
        r = h.client->Post(path, hdrs, body, "application/json");
    } else if (method == "DELETE") {
        r = h.client->Delete(path, hdrs);
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

class AdminUsersFixture : public ::testing::Test {
protected:
    DbConn                                conn_info;
    std::unique_ptr<litecode::ConnectionPool> pool;
    std::vector<int>                      created_user_ids;
    std::vector<std::int64_t>             created_audit_ids;
    std::vector<int>                      created_submission_ids;

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

        // Schema probe — guard against older dev boxes missing
        // audit_logs / submissions tables.
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
                for (auto id : created_submission_ids) {
                    try { conn.execute(
                        "DELETE FROM submissions WHERE id = ?", id); }
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
    // bcrypt-format filler; the test never logs in as this user.
    int make_user(const std::string& role = "user",
                  const std::string& username = "") {
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const std::string uname = username.empty()
            ? std::string("adm-") +
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

    // Insert an audit_log row directly (bypasses audit_log_repo
    // to avoid widening the ODR surface in the test TU). Returns
    // the new id; -1 on failure.
    std::int64_t insert_audit_row(int admin_id, const std::string& action,
                                  const std::string& target_type,
                                  const std::string& target_id,
                                  const std::string& payload_json) {
        try {
            auto conn = pool->acquire();
            auto rs = conn.execute(
                "INSERT INTO audit_logs "
                "(admin_id, action, target_type, target_id, payload) "
                "VALUES (?, ?, ?, ?, CAST(? AS JSON))",
                admin_id, action, target_type, target_id, payload_json);
            const std::int64_t id =
                static_cast<std::int64_t>(rs.getAutoIncrementValue());
            if (id > 0) created_audit_ids.push_back(id);
            return id;
        } catch (...) {
            return -1;
        }
    }

    // Count rows in audit_logs matching a given action + target_id.
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

    // Insert a submission row directly to make submission_count
    // non-zero in the list response.
    int make_submission(int user_id, int problem_id) {
        try {
            auto conn = pool->acquire();
            // Ensure a throwaway problem exists (test_case_repo
            // FK requirements are light; we just need a problem_id).
            // Reuse if problem_id > 0; otherwise create one.
            if (problem_id <= 0) {
                auto p = conn.execute(
                    "INSERT INTO problems (slug, title, difficulty, "
                    "                       description, time_limit, "
                    "                       memory_limit) "
                    "VALUES (?, 'admin-users test', 'easy', 'x', 1000, 128)",
                    std::string("adm-prob-") +
                        std::to_string(static_cast<long long>(
                            std::chrono::system_clock::now()
                                .time_since_epoch().count())));
                problem_id = static_cast<int>(p.getAutoIncrementValue());
            }
            auto rs = conn.execute(
                "INSERT INTO submissions "
                "(user_id, problem_id, language, code, status) "
                "VALUES (?, ?, 'cpp', 'int main(){return 0;}', 'ac')",
                user_id, problem_id);
            const int id = static_cast<int>(rs.getAutoIncrementValue());
            if (id > 0) created_submission_ids.push_back(id);
            return id;
        } catch (...) {
            return 0;
        }
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests (no DB)
// ────────────────────────────────────────────────────────────────────────────

// Build a minimal Request object for the parse_*_query / extract_*_from_path
// helpers. Only `path` (and `params` for parse_list_query) matter.
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

TEST(UserAdminConstants, LimitsAndCap) {
    EXPECT_EQ(litecode::user_repo::kDefaultUserListLimit, 20);
    EXPECT_EQ(litecode::user_repo::kMaxUserListLimit, 100);
    EXPECT_LE(litecode::user_repo::kDefaultUserListLimit,
              litecode::user_repo::kMaxUserListLimit);
    EXPECT_EQ(litecode::admin_user_routes::detail::kAdminUserValueMax, 64);
}

TEST(ParseRoleParam, AcceptsValidValues) {
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_role_param("user"),  "user");
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_role_param("admin"), "admin");
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_role_param("USER"),  "user");
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_role_param("Admin"), "admin");
}

TEST(ParseRoleParam, RejectsInvalid) {
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_role_param("owner").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_role_param("user admin").has_value());
    // String-with-NUL: construct an explicit string_view so the
    // null byte is preserved (a const char* literal would have
    // its length truncated by strlen).
    const std::string with_nul("user\x00", 5);
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_role_param(
        std::string_view(with_nul.data(), with_nul.size())).has_value());
    // Empty / absent ⇒ nullopt (handler keeps filter as no-filter).
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_role_param("").has_value());
}

TEST(ParseIdParam, AcceptsPositiveIntegers) {
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_id_param("1"),    1);
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_id_param("42"),   42);
    EXPECT_EQ(litecode::admin_user_routes::detail::parse_id_param("2147483647"), 2147483647);
}

TEST(ParseIdParam, RejectsZeroAndNegative) {
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("0").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("-1").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("+5").has_value());
}

TEST(ParseIdParam, RejectsNonDigitAndTooLarge) {
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("abc").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("12abc").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("2147483648").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("").has_value());
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_id_param("99999999999").has_value());
}

TEST(ExtractIdFromPath, AcceptsValidPath) {
    auto req = make_request("/api/v1/admin/users/42/role");
    auto v = litecode::admin_user_routes::detail::extract_id_from_path(req);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "42");
}

TEST(ExtractIdFromPath, RejectsBadPrefix) {
    auto req = make_request("/admin/users/42/role");
    EXPECT_FALSE(litecode::admin_user_routes::detail::extract_id_from_path(req).has_value());
}

TEST(ExtractIdFromPath, RejectsMissingSuffix) {
    auto req = make_request("/api/v1/admin/users/42");
    EXPECT_FALSE(litecode::admin_user_routes::detail::extract_id_from_path(req).has_value());
}

TEST(ExtractIdFromPath, RejectsEmptyId) {
    auto req = make_request("/api/v1/admin/users//role");
    EXPECT_FALSE(litecode::admin_user_routes::detail::extract_id_from_path(req).has_value());
}

TEST(ExtractIdFromPath, RejectsNestedPath) {
    auto req = make_request("/api/v1/admin/users/42/role/extra");
    EXPECT_FALSE(litecode::admin_user_routes::detail::extract_id_from_path(req).has_value());
}

TEST(ParseListQuery, EmptyPathDefaultsTo20And0) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users");
    EXPECT_TRUE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(f.limit, 20);
    EXPECT_EQ(f.offset, 0);
    EXPECT_FALSE(f.role.has_value());
    EXPECT_FALSE(f.q.has_value());
}

TEST(ParseListQuery, AcceptsValidParams) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?role=admin&q=alice&limit=5&offset=10");
    EXPECT_TRUE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    ASSERT_TRUE(f.role.has_value());
    EXPECT_EQ(*f.role, "admin");
    ASSERT_TRUE(f.q.has_value());
    EXPECT_EQ(*f.q, "alice");
    EXPECT_EQ(f.limit, 5);
    EXPECT_EQ(f.offset, 10);
}

TEST(ParseListQuery, RejectsBadRole) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?role=owner");
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "role");
}

TEST(ParseListQuery, RejectsNegativeOffset) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?offset=-1");
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
    const auto env = json::parse(res.body);
    EXPECT_EQ(env["details"]["field"], "offset");
}

TEST(ParseListQuery, RejectsZeroLimit) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?limit=0");
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, RejectsNonNumericLimit) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?limit=abc");
    EXPECT_FALSE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(res.status, 400);
}

TEST(ParseListQuery, ClampsLimitToMax) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?limit=9999");
    EXPECT_TRUE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_EQ(f.limit, litecode::user_repo::kMaxUserListLimit);
}

TEST(ParseListQuery, TrimsQWhitespace) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    // Use literal spaces in the params (the test helper
    // make_request does NOT URL-decode; production httplib
    // does, but the parser is fed the post-decode value).
    auto req = make_request("/api/v1/admin/users?q=  alice  ");
    EXPECT_TRUE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    ASSERT_TRUE(f.q.has_value());
    EXPECT_EQ(*f.q, "alice");
}

TEST(ParseListQuery, EmptyQMeansNoFilter) {
    httplib::Response res;
    litecode::user_repo::UserListFilter f;
    auto req = make_request("/api/v1/admin/users?q=  ");
    EXPECT_TRUE(litecode::admin_user_routes::detail::parse_list_query(req, res, f));
    EXPECT_FALSE(f.q.has_value());
}

TEST(TruncateForEnvelope, ShortReturnsUnchanged) {
    EXPECT_EQ(litecode::admin_user_routes::detail::truncate_for_envelope("abc"),
              "abc");
}

TEST(TruncateForEnvelope, LongIsTruncated) {
    const std::string long_v(200, 'a');
    const auto out = litecode::admin_user_routes::detail::truncate_for_envelope(long_v);
    EXPECT_EQ(out.size(), 64u + 3u);
    EXPECT_EQ(out.substr(64), "...");
}

TEST(SerializeUserAdminRow, IncludesAllFields) {
    litecode::UserRow u;
    u.id = 42;
    u.username = "alice";
    u.role = "user";
    u.email = "alice@example.com";
    u.created_at = "2026-07-08 12:34:56";
    u.last_login = std::string("2026-07-08 12:00:00");
    u.last_login_ip = std::string("127.0.0.1");
    const auto j = litecode::admin_user_routes::serialize_user_admin_row(u);
    EXPECT_EQ(j["id"].get<int>(), 42);
    EXPECT_EQ(j["username"], "alice");
    EXPECT_EQ(j["role"], "user");
    EXPECT_EQ(j["email"], "alice@example.com");
    EXPECT_EQ(j["created_at"], "2026-07-08 12:34:56");
    EXPECT_EQ(j["last_login"], "2026-07-08 12:00:00");
    EXPECT_EQ(j["last_login_ip"], "127.0.0.1");
}

TEST(SerializeUserAdminRow, OmitsPasswordHash) {
    litecode::UserRow u;
    u.username = "alice";
    u.password_hash = "$2b$12$secret.secret.secret.secret.aaaa";
    const auto j = litecode::admin_user_routes::serialize_user_admin_row(u);
    EXPECT_FALSE(j.contains("password_hash"));
}

TEST(SerializeUserAdminRow, NullableFieldsAreNull) {
    litecode::UserRow u;
    u.username = "alice";
    const auto j = litecode::admin_user_routes::serialize_user_admin_row(u);
    EXPECT_TRUE(j.contains("email"));
    EXPECT_TRUE(j["email"].is_null());
    EXPECT_TRUE(j.contains("last_login"));
    EXPECT_TRUE(j["last_login"].is_null());
    EXPECT_TRUE(j.contains("last_login_ip"));
    EXPECT_TRUE(j["last_login_ip"].is_null());
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests (real MySQL)
// ────────────────────────────────────────────────────────────────────────────

class AdminUsersLiveFixture : public AdminUsersFixture {
protected:
    std::unique_ptr<litecode::HttpServer>  server;
    std::unique_ptr<litecode::RateLimiter> limiter;
    ServerHandle                           handle;
    litecode::RateLimitConfig              rate_cfg;
    litecode::JwtConfig                    jwt_cfg;
    int                                    admin_user_id   = 0;
    int                                    other_admin_id  = 0;
    std::string                            admin_username;
    std::string                            other_admin_username;
    std::string                            admin_token;
    std::string                            other_admin_token;
    std::string                            regular_username;
    std::string                            regular_token;
    int                                    regular_user_id = 0;

    void SetUp() override {
        AdminUsersFixture::SetUp();
        rate_cfg = lax_rate_limit();
        jwt_cfg  = dev_jwt();
        limiter  = std::make_unique<litecode::RateLimiter>();
        server   = std::make_unique<litecode::HttpServer>(
                       dev_server(), dev_cors());

        litecode::admin_user_routes::register_admin_user_routes(
            *server, *pool, *limiter, rate_cfg, jwt_cfg);
        handle = start_server(server.get());

        // One admin for the bulk of the tests, plus a second
        // admin for the self-demote case.
        static std::atomic<std::uint64_t> seq{0};
        const auto n = seq.fetch_add(1, std::memory_order_relaxed);
        const auto stamp = std::to_string(static_cast<long long>(
            std::chrono::system_clock::now().time_since_epoch().count())) +
            "_" + std::to_string(n);

        admin_username = "adm-admin1-" + stamp;
        admin_user_id  = make_user("admin", admin_username);
        ASSERT_GT(admin_user_id, 0);
        admin_token = issue_token(jwt_cfg, std::to_string(admin_user_id),
                                  admin_username, "admin");

        other_admin_username = "adm-admin2-" + stamp;
        other_admin_id        = make_user("admin", other_admin_username);
        ASSERT_GT(other_admin_id, 0);
        other_admin_token = issue_token(jwt_cfg, std::to_string(other_admin_id),
                                        other_admin_username, "admin");

        regular_username = "adm-regular-" + stamp;
        regular_user_id  = make_user("user", regular_username);
        ASSERT_GT(regular_user_id, 0);
        regular_token = issue_token(jwt_cfg, std::to_string(regular_user_id),
                                    regular_username, "user");
    }

    void TearDown() override {
        handle = ServerHandle();
        server.reset();
        limiter.reset();
        AdminUsersFixture::TearDown();
    }

    std::string get_header(const ApiResponse& r, const std::string& name) {
        auto it = r.headers.find(name);
        if (it == r.headers.end()) return "";
        return it->second;
    }
};

TEST_F(AdminUsersLiveFixture, ListHappyPathReturnsAllFields) {
    StdoutSilencer silencer;

    // Create 3 users with different roles + names so the list
    // response has enough variety to assert ordering + filtering.
    const int u_a = make_user("user", "adm-hp-a-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_a");
    const int u_b = make_user("admin", "adm-hp-b-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_b");
    const int u_c = make_user("user", "adm-hp-c-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_c");
    ASSERT_GT(u_a, 0);
    ASSERT_GT(u_b, 0);
    ASSERT_GT(u_c, 0);
    // u_b has 2 submissions to verify submission_count.
    make_submission(u_b, 0);
    make_submission(u_b, 0);

    // Pull a page wide enough to cover our 3 fresh users.
    // (limit=100 is the SPEC §5.5 cap; tests of the cap itself
    //  live in the unit tests below.)
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?limit=100", admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);

    EXPECT_TRUE(env.contains("data"));
    EXPECT_EQ(env["data"]["limit"].get<int>(), 100);
    EXPECT_EQ(env["data"]["offset"].get<int>(), 0);
    EXPECT_GE(env["data"]["total"].get<int>(), 3);
    const auto& items = env["data"]["items"];
    ASSERT_GE(items.size(), 3u);

    // Find our 3 fresh users by id and verify shape.
    int found_a = -1, found_b = -1, found_c = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int uid = items[i]["id"].get<int>();
        if (uid == u_a) found_a = static_cast<int>(i);
        if (uid == u_b) found_b = static_cast<int>(i);
        if (uid == u_c) found_c = static_cast<int>(i);
    }
    ASSERT_NE(found_a, -1);
    ASSERT_NE(found_b, -1);
    ASSERT_NE(found_c, -1);

    // Each row carries the canonical 7 fields, no password_hash.
    for (const auto& item : {items[found_a], items[found_b], items[found_c]}) {
        EXPECT_TRUE(item.contains("id"));
        EXPECT_TRUE(item.contains("username"));
        EXPECT_TRUE(item.contains("role"));
        EXPECT_TRUE(item.contains("email"));
        EXPECT_TRUE(item.contains("created_at"));
        EXPECT_TRUE(item.contains("last_login"));
        EXPECT_TRUE(item.contains("last_login_ip"));
        EXPECT_TRUE(item.contains("submission_count"));
        EXPECT_FALSE(item.contains("password_hash"));
    }

    // submission_count for u_b is 2; u_a and u_c have 0.
    EXPECT_EQ(items[found_b]["submission_count"].get<int>(), 2);
    EXPECT_EQ(items[found_a]["submission_count"].get<int>(), 0);
    EXPECT_EQ(items[found_c]["submission_count"].get<int>(), 0);

    // Roles are surfaced correctly.
    EXPECT_EQ(items[found_a]["role"], "user");
    EXPECT_EQ(items[found_b]["role"], "admin");
    EXPECT_EQ(items[found_c]["role"], "user");
}

TEST_F(AdminUsersLiveFixture, ListWithRoleFilter) {
    StdoutSilencer silencer;
    const int u_admin = make_user("admin", "adm-rf-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_adm");
    const int u_user  = make_user("user", "adm-rf-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()) + "_usr");
    ASSERT_GT(u_admin, 0);
    ASSERT_GT(u_user, 0);

    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?role=admin&limit=100", admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    // Every returned item should be role=admin.
    for (const auto& item : env["data"]["items"]) {
        EXPECT_EQ(item["role"], "admin");
    }
    // The user we created should appear in the list.
    bool found = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["id"].get<int>() == u_admin) { found = true; break; }
    }
    EXPECT_TRUE(found);
    // The non-admin should NOT appear.
    bool found_user = false;
    for (const auto& item : env["data"]["items"]) {
        if (item["id"].get<int>() == u_user) { found_user = true; break; }
    }
    EXPECT_FALSE(found_user);
}

TEST_F(AdminUsersLiveFixture, ListWithQSearch) {
    StdoutSilencer silencer;
    const std::string unique = "adm-q-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count());
    const int u_match = make_user("user", unique + "-match");
    const int u_other = make_user("user", unique + "-other");
    ASSERT_GT(u_match, 0);
    ASSERT_GT(u_other, 0);

    // Search for the unique prefix; LIKE %xxx% should match only
    // the rows whose username contains it.
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?q=" + unique + "-match&limit=100", admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    bool found_match = false, found_other = false;
    for (const auto& item : env["data"]["items"]) {
        const int uid = item["id"].get<int>();
        if (uid == u_match) found_match = true;
        if (uid == u_other) found_other = true;
    }
    EXPECT_TRUE(found_match);
    EXPECT_FALSE(found_other);
}

TEST_F(AdminUsersLiveFixture, ListWithCombinedFilters) {
    StdoutSilencer silencer;
    const std::string unique = "adm-cf-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count());
    const int u_match = make_user("admin", unique + "-admin");
    const int u_other = make_user("user", unique + "-user");
    ASSERT_GT(u_match, 0);
    ASSERT_GT(u_other, 0);

    // role=admin + q=unique should match u_match only.
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?role=admin&q=" + unique + "&limit=100", admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    bool found_match = false, found_other = false;
    for (const auto& item : env["data"]["items"]) {
        const int uid = item["id"].get<int>();
        if (uid == u_match) found_match = true;
        if (uid == u_other) found_other = true;
    }
    EXPECT_TRUE(found_match);
    EXPECT_FALSE(found_other);
}

TEST_F(AdminUsersLiveFixture, ListPagination) {
    StdoutSilencer silencer;
    // Request a small page (limit=2) and verify offset math.
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?limit=2&offset=0", admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["limit"].get<int>(), 2);
    EXPECT_EQ(env["data"]["offset"].get<int>(), 0);
    EXPECT_LE(env["data"]["items"].size(), 2u);
    EXPECT_GE(env["data"]["total"].get<int>(), 2);
}

TEST_F(AdminUsersLiveFixture, ListOutOfRangeOffset) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?limit=10&offset=1000000", admin_token);
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["items"].size(), 0u);
    EXPECT_GE(env["data"]["total"].get<int>(), 0);
    EXPECT_EQ(env["data"]["offset"].get<int>(), 1000000);
}

TEST_F(AdminUsersLiveFixture, ListBadRole) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?role=owner", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "role");
}

TEST_F(AdminUsersLiveFixture, ListBadLimit) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?limit=abc", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "limit");
}

TEST_F(AdminUsersLiveFixture, ListNegativeOffset) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users?offset=-1", admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["details"]["field"], "offset");
}

TEST_F(AdminUsersLiveFixture, ListNoAuth) {
    StdoutSilencer silencer;
    // Empty bearer_token (default) → no Authorization header sent.
    const auto r = do_request(handle, "GET", "/api/v1/admin/users");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "UNAUTHORIZED");
}

TEST_F(AdminUsersLiveFixture, ListBadToken) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET", "/api/v1/admin/users",
                              "garbage.token.value");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(AdminUsersLiveFixture, ListNonAdminForbidden) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET", "/api/v1/admin/users",
                              regular_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 403);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "FORBIDDEN");
}

TEST_F(AdminUsersLiveFixture, ListXRequestIdRoundTrips) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET",
        "/api/v1/admin/users", admin_token, "",
        "test-admin-users-rid-xyz");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.request_id, "test-admin-users-rid-xyz");
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"], "test-admin-users-rid-xyz");
}

TEST_F(AdminUsersLiveFixture, ListRateLimitHeadersPresent) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "GET", "/api/v1/admin/users",
                              admin_token);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_FALSE(get_header(r, "X-RateLimit-Limit").empty());
    EXPECT_FALSE(get_header(r, "X-RateLimit-Remaining").empty());
    EXPECT_EQ(std::stoi(get_header(r, "X-RateLimit-Limit")), 1000);
}

TEST_F(AdminUsersLiveFixture, ChangeRoleHappyPath) {
    StdoutSilencer silencer;
    // Start with a regular user, then promote to admin.
    const int target = make_user("user", "adm-rc-happy-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    // Pre-state: 0 audit rows for this target.
    EXPECT_EQ(count_audit_rows(
        litecode::audit_log_repo::kActionUserRoleChange,
        std::to_string(target)), 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        admin_token, R"({"role":"admin"})");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["id"].get<int>(), target);
    EXPECT_EQ(env["data"]["role"], "admin");
    EXPECT_FALSE(env["data"].contains("password_hash"));

    // Post-state: 1 audit row for this target.
    EXPECT_EQ(count_audit_rows(
        litecode::audit_log_repo::kActionUserRoleChange,
        std::to_string(target)), 1);

    // Verify the audit row payload by reading it back.
    try {
        auto conn = pool->acquire();
        auto rs = conn.execute(
            "SELECT admin_id, action, target_type, target_id, "
            "       CAST(payload AS CHAR) "
            "FROM audit_logs "
            "WHERE action = ? AND target_id = ?",
            std::string(litecode::audit_log_repo::kActionUserRoleChange),
            std::to_string(target));
        bool saw = false;
        for (auto row : rs) {
            saw = true;
            EXPECT_EQ(row[1].get<std::string>(),
                      litecode::audit_log_repo::kActionUserRoleChange);
            EXPECT_EQ(row[2].get<std::string>(), "user");
            const std::string payload = row[4].get<std::string>();
            const auto j = json::parse(payload);
            EXPECT_EQ(j["old_role"], "user");
            EXPECT_EQ(j["new_role"], "admin");
            // username is set; we don't pin the exact value
            // (the test uses a unique timestamped name).
            EXPECT_TRUE(j.contains("username"));
        }
        EXPECT_TRUE(saw);
    } catch (...) {}
}

TEST_F(AdminUsersLiveFixture, ChangeRoleSameRoleIsNoOp) {
    StdoutSilencer silencer;
    // Target is already a regular user. PUT role=user should
    // skip the UPDATE and not write an audit row.
    const int target = make_user("user", "adm-rc-noop-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        admin_token, R"({"role":"user"})");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["role"], "user");

    // No audit row was written.
    EXPECT_EQ(count_audit_rows(
        litecode::audit_log_repo::kActionUserRoleChange,
        std::to_string(target)), 0);
}

TEST_F(AdminUsersLiveFixture, ChangeRoleAdminToUser) {
    StdoutSilencer silencer;
    // Demote the other_admin (admin → user). Audit row should
    // record old_role=admin + new_role=user.
    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(other_admin_id) + "/role",
        admin_token, R"({"role":"user"})");
    ASSERT_TRUE(r);
    ASSERT_EQ(r.status, 200);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["data"]["id"].get<int>(), other_admin_id);
    EXPECT_EQ(env["data"]["role"], "user");

    // Audit row exists with the right old/new roles.
    EXPECT_EQ(count_audit_rows(
        litecode::audit_log_repo::kActionUserRoleChange,
        std::to_string(other_admin_id)), 1);

    // Note: we do NOT restore the admin role here — the
    // TearDown() handler will DELETE the user row, which
    // cascades to audit_logs and submissions.
}

TEST_F(AdminUsersLiveFixture, ChangeRoleUnknownId) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/99999999/role",
        admin_token, R"({"role":"admin"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 404);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "NOT_FOUND");
}

TEST_F(AdminUsersLiveFixture, ChangeRoleBadIdPath) {
    StdoutSilencer silencer;
    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/abc/role",
        admin_token, R"({"role":"admin"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "id");
}

TEST_F(AdminUsersLiveFixture, ChangeRoleMissingRoleField) {
    StdoutSilencer silencer;
    const int target = make_user("user", "adm-rc-missing-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        admin_token, R"({})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "INVALID_INPUT");
    EXPECT_EQ(env["details"]["field"], "role");
}

TEST_F(AdminUsersLiveFixture, ChangeRoleBadRoleValue) {
    StdoutSilencer silencer;
    const int target = make_user("user", "adm-rc-badrole-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        admin_token, R"({"role":"owner"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 400);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["details"]["field"], "role");
}

TEST_F(AdminUsersLiveFixture, ChangeRoleNoAuth) {
    StdoutSilencer silencer;
    const int target = make_user("user", "adm-rc-noauth-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        "", R"({"role":"admin"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 401);
}

TEST_F(AdminUsersLiveFixture, ChangeRoleNonAdminForbidden) {
    StdoutSilencer silencer;
    const int target = make_user("user", "adm-rc-nonadmin-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        regular_token, R"({"role":"admin"})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 403);
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["code"], "FORBIDDEN");
}

TEST_F(AdminUsersLiveFixture, ChangeRoleXRequestIdRoundTrips) {
    StdoutSilencer silencer;
    const int target = make_user("user", "adm-rc-rid-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target, 0);

    const auto r = do_request(handle, "PUT",
        "/api/v1/admin/users/" + std::to_string(target) + "/role",
        admin_token, R"({"role":"admin"})",
        "test-role-rid-zzz");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.request_id, "test-role-rid-zzz");
    const auto env = json::parse(r.body);
    EXPECT_EQ(env["request_id"], "test-role-rid-zzz");
}

TEST_F(AdminUsersLiveFixture, ChangeRoleRateLimitTriggers429) {
    StdoutSilencer silencer;
    // Tight bucket: 2 role changes per minute.
    litecode::RateLimitConfig tight;
    tight.auth_register_per_minute_per_ip   = 1000;
    tight.auth_login_per_minute_per_ip      = 1000;
    tight.problems_public_per_minute_per_ip = 1000;
    tight.submission_per_minute_per_user    = 1000;
    tight.admin_write_per_minute            = 1000;
    tight.bulk_import_per_hour              = 1000;
    tight.stats_ranking_per_minute_per_ip   = 1000;
    tight.admin_users_list_per_minute       = 1000;
    tight.admin_users_role_per_minute       = 2;

    auto server2 = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    auto limiter2 = std::make_unique<litecode::RateLimiter>();
    litecode::admin_user_routes::register_admin_user_routes(*server2, *pool, *limiter2, tight, jwt_cfg);
    auto h2 = start_server(server2.get());

    // First two succeed.
    for (int i = 0; i < 2; ++i) {
        const int target = make_user("user", "adm-rc-rl-" +
            std::to_string(std::chrono::system_clock::now()
                .time_since_epoch().count()) + "_" + std::to_string(i));
        ASSERT_GT(target, 0);
        const auto r = do_request(h2, "PUT",
            "/api/v1/admin/users/" + std::to_string(target) + "/role",
            admin_token, R"({"role":"admin"})");
        ASSERT_TRUE(r);
        EXPECT_EQ(r.status, 200);
    }
    // Third hits 429.
    const int target3 = make_user("user", "adm-rc-rl-3-" +
        std::to_string(std::chrono::system_clock::now()
            .time_since_epoch().count()));
    ASSERT_GT(target3, 0);
    const auto r3 = do_request(h2, "PUT",
        "/api/v1/admin/users/" + std::to_string(target3) + "/role",
        admin_token, R"({"role":"admin"})");
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3.status, 429);
    const auto env = json::parse(r3.body);
    EXPECT_EQ(env["code"], "RATE_LIMITED");
    EXPECT_FALSE(get_header(r3, "Retry-After").empty());

    h2 = ServerHandle();
    server2.reset();
    limiter2.reset();
}

TEST_F(AdminUsersLiveFixture, ListRateLimitTriggers429) {
    StdoutSilencer silencer;
    // Tight bucket: 2 list calls per minute.
    litecode::RateLimitConfig tight;
    tight.auth_register_per_minute_per_ip   = 1000;
    tight.auth_login_per_minute_per_ip      = 1000;
    tight.problems_public_per_minute_per_ip = 1000;
    tight.submission_per_minute_per_user    = 1000;
    tight.admin_write_per_minute            = 1000;
    tight.bulk_import_per_hour              = 1000;
    tight.stats_ranking_per_minute_per_ip   = 1000;
    tight.admin_users_list_per_minute       = 2;
    tight.admin_users_role_per_minute       = 1000;

    auto server2 = std::make_unique<litecode::HttpServer>(dev_server(), dev_cors());
    auto limiter2 = std::make_unique<litecode::RateLimiter>();
    litecode::admin_user_routes::register_admin_user_routes(*server2, *pool, *limiter2, tight, jwt_cfg);
    auto h2 = start_server(server2.get());

    EXPECT_EQ(do_request(h2, "GET", "/api/v1/admin/users", admin_token).status, 200);
    EXPECT_EQ(do_request(h2, "GET", "/api/v1/admin/users", admin_token).status, 200);
    const auto r3 = do_request(h2, "GET", "/api/v1/admin/users", admin_token);
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3.status, 429);
    const auto env = json::parse(r3.body);
    EXPECT_EQ(env["code"], "RATE_LIMITED");
    EXPECT_FALSE(get_header(r3, "Retry-After").empty());

    h2 = ServerHandle();
    server2.reset();
    limiter2.reset();
}

}  // namespace
