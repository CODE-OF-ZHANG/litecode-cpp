// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — users repository (Phase 2 ★)
//
// SPEC §4.1 / §5.1 / §11 Phase 2 / §15.1 / A1 acceptance:
//   - `users` table schema:
//       id, username UNIQUE, password_hash, role ENUM('user','admin'),
//       email (UNIQUE NULLS NOT DISTINCT on MySQL 8.0.19+),
//       avatar, created_at, last_login, last_login_ip
//   - The repo offers a small, project-shaped surface that every
//     Phase 2 / Phase 6 endpoint can share:
//       * create_user  — INSERT a new row, return the new id (0 on dup)
//       * username_exists — boolean pre-check for the 409 path
//       * email_exists — same for email (optional field)
//       * find_by_username / find_by_id — fetch a full row (login + profile)
//       * update_last_login — set last_login = NOW(), last_login_ip = ?
//       * update_role — admin-only role change
//   - All writes use parameterized SQL (`?` placeholders) — SPEC §15.2
//     forbids string concatenation. mysqlx::SqlStatement::bind() handles
//     the binding; user-supplied data never reaches the wire string.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (config.h / logger.h / server.h / jwt_utils.h / connection_pool.h).
//     The repo is essentially a thin set of free functions over a
//     ConnectionPool reference; no internal state to unit-test.
//   - Returned rows are exposed via a `UserRow` struct (not the raw
//     mysqlx::Row) so callers don't depend on mysqlx::Value's semantics.
//     `std::optional<UserRow>` for "not found"; empty / default
//     fields when the row had a NULL.
//   - We deliberately do NOT model an ORM-style change-tracking
//     `User` object. Hand-written SQL with bind() is fine for our
//     surface and avoids the C++ ORM overhead (SPEC §9 calls this out).
//   - Concurrency: every public method acquires a fresh PooledConnection
//     from the pool, runs the SQL, releases. The pool is thread-safe;
//     individual methods do not need their own locks.
//
// Usage (registration, in auth_routes.h):
//
//   litecode::UserRow row;
//   row.username      = "alice";
//   row.password_hash = litecode::hash_password(req.password);
//   row.role          = "user";
//   row.email         = req.email;   // std::nullopt ⇒ column becomes NULL
//
//   const int new_id = litecode::user_repo::create_user(pool, row);
//   if (new_id == 0) {
//       // username (or email) collided — 409
//   } else {
//       row.id = new_id;
//       // issue access+refresh, return envelope
//   }
//
// Usage (login, future Phase 2):
//
//   const auto row = litecode::user_repo::find_by_username(pool, "alice");
//   if (!row || !litecode::verify_password(req.password, row->password_hash)) {
//       throw litecode::ApiException(401, litecode::ErrorCode::UNAUTHORIZED,
//           "invalid username or password");
//   }

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"             // LOG_WARN (used for non-fatal DB hiccups)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  UserRow
//
//  Plain-data projection of a row from the `users` table. All string
//  fields are std::string for safe lifetime — callers can copy, move,
//  or store the row across async boundaries.
//
//  Field semantics:
//    - id: the row's primary key; 0 ⇒ "not yet inserted"
//    - username: required, never empty once loaded
//    - password_hash: bcrypt string (see auth/password_hash.h)
//    - role: "user" | "admin" (matches the MySQL ENUM)
//    - email: optional; nullopt when NULL in the DB
//    - avatar: optional; nullopt when NULL
//    - created_at: ISO-8601 string ("YYYY-MM-DD HH:MM:SS") — kept as
//      text for forward-compat with JSON serialization
//    - last_login: optional; nullopt when never logged in
//    - last_login_ip: optional IPv4 / IPv6 string; nullopt when never set
//
//  Not exposing mysqlx::Value at this boundary keeps the surface free
//  of mysql-connector-c++ types — handlers can be tested without the
//  driver in scope.
// ────────────────────────────────────────────────────────────────────────────

struct UserRow {
    int                          id           = 0;
    std::string                  username;
    std::optional<std::string>   display_name;   // v1.3.4 PR 9 — 昵称(可空,空时回退到 username)
    std::string                  password_hash;
    std::string                  role;
    int                          token_version = 0;  // v1.3.4 PR 9 — JWT 缓存失效钩子(预留,本 PR 不强制)
    std::optional<std::string>   email;
    std::optional<std::string>   school;         // v1.3.4 PR 9 — 学校(0..100, 可空)
    std::optional<std::string>   bio;            // v1.3.4 PR 9 — 简介(0..500, 可空)
    std::optional<std::string>   avatar;
    std::string                  created_at;
    std::optional<std::string>   last_login;
    std::optional<std::string>   last_login_ip;
    std::optional<std::string>   username_changed_at;  // v1.3.4 PR 9 — 改名频率限制
};

// ────────────────────────────────────────────────────────────────────────────
//  UserRepoError — surface every repo-layer failure as a typed exception
//
//  Two tiers, mirroring the rest of Phase 2:
//    - UserRepoError         — generic failure (driver error, etc.)
//    - UserAlreadyExistsError — username or email uniqueness collision;
//                              caught by the route handler and folded
//                              into 409 CONFLICT.
//
//  We deliberately do NOT surface duplicate-detection via a magic
//  error code from create_user; the boolean returns are simpler and
//  let the handler compose "username taken" vs "email taken" cleanly.
// ────────────────────────────────────────────────────────────────────────────

class UserRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UserAlreadyExistsError : public UserRepoError {
public:
    using UserRepoError::UserRepoError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Validation helpers (used by both repo + route layer)
//
//  Username: 3..50 chars, ASCII letters / digits / underscore / hyphen / dot.
//  This matches the SPEC §5.7 example message ("用户名长度必须在 3-50 之间")
//  and is intentionally permissive enough to allow `alice_42`,
//  `bob.smith`, `user-007` without opening the door to whitespace or
//  quote characters that would need escaping in URLs / log lines.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kMinUsernameLength = 3;
inline constexpr std::size_t kMaxUsernameLength = 50;
inline constexpr std::size_t kMinEmailLength    = 3;
inline constexpr std::size_t kMaxEmailLength    = 100;

inline bool is_valid_username_char(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return (uc >= 'a' && uc <= 'z') ||
           (uc >= 'A' && uc <= 'Z') ||
           (uc >= '0' && uc <= '9') ||
           uc == '_' || uc == '-' || uc == '.';
}

inline bool validate_username(std::string_view username,
                              std::string* error_out = nullptr) {
    if (username.size() < kMinUsernameLength ||
        username.size() > kMaxUsernameLength) {
        if (error_out) {
            *error_out = "username must be between " +
                         std::to_string(kMinUsernameLength) + " and " +
                         std::to_string(kMaxUsernameLength) + " characters";
        }
        return false;
    }
    if (username.front() == '.' || username.front() == '-' ||
        username.back()  == '.' || username.back()  == '-') {
        // Forbid leading / trailing dot / hyphen — they cause awkward
        // display in lists and URLs.
        if (error_out) {
            *error_out = "username must not start or end with '.' or '-'";
        }
        return false;
    }
    for (char c : username) {
        if (!is_valid_username_char(c)) {
            if (error_out) {
                *error_out = "username may only contain letters, digits, '_', '-', '.'";
            }
            return false;
        }
    }
    return true;
}

// Lightweight email shape check — we deliberately do NOT regex against
// RFC 5322 (overkill + endless corner cases). The pattern is "has a
// non-empty local part, exactly one '@', a non-empty domain with at
// least one dot, and no whitespace". The DB still owns the UNIQUE
// constraint; this is a friendlier-failure pre-check.
inline bool validate_email(std::string_view email,
                           std::string* error_out = nullptr) {
    if (email.size() < kMinEmailLength || email.size() > kMaxEmailLength) {
        if (error_out) {
            *error_out = "email must be between " +
                         std::to_string(kMinEmailLength) + " and " +
                         std::to_string(kMaxEmailLength) + " characters";
        }
        return false;
    }
    const auto at = email.find('@');
    if (at == std::string_view::npos || at == 0 || at == email.size() - 1) {
        if (error_out) {
            *error_out = "email must be of the form local@domain";
        }
        return false;
    }
    const std::string_view domain = email.substr(at + 1);
    if (domain.find('@') != std::string_view::npos) {
        if (error_out) {
            *error_out = "email must contain exactly one '@'";
        }
        return false;
    }
    if (domain.find('.') == std::string_view::npos ||
        domain.front() == '.' || domain.back() == '.') {
        if (error_out) {
            *error_out = "email domain must contain a dot and not start/end with one";
        }
        return false;
    }
    for (char c : email) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (error_out) {
                *error_out = "email must not contain whitespace";
            }
            return false;
        }
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
//
//  Reads from a mysqlx::Row into a UserRow. Centralized here so the
//  find_* helpers stay small. NULL columns come back as mysqlx::Value{}
//  (isNull()==true); everything else is read as std::string via
//  get<std::string>() which handles VARCHAR / TEXT uniformly.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::optional<std::string> opt_string(const mysqlx::Row& row,
                                             std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try {
        return v.get<std::string>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw UserRepoError(std::string("user_repo: required field '") +
                            field + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw UserRepoError(std::string("user_repo: required field '") +
                            field + "' is not an int: " + e.what());
    }
}

inline UserRow row_to_user(const mysqlx::Row& row) {
    UserRow u;
    u.id            = req_int   (row, 0, "id");
    u.username      = req_string(row, 1, "username");
    u.display_name  = opt_string(row, 2);
    u.password_hash = req_string(row, 3, "password_hash");
    u.role          = req_string(row, 4, "role");
    u.token_version = req_int   (row, 5, "token_version");
    u.email         = opt_string(row, 6);
    u.school        = opt_string(row, 7);
    u.bio           = opt_string(row, 8);
    u.avatar        = opt_string(row, 9);
    u.created_at    = req_string(row, 10, "created_at");
    u.last_login    = opt_string(row, 11);
    u.last_login_ip = opt_string(row, 12);
    u.username_changed_at = opt_string(row, 13);
    return u;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace user_repo {

// create_user — INSERT a new user row.
//
// Returns:
//   - new user id (>0) on success
//   - 0 on username or email uniqueness collision (no exception thrown;
//     the route handler maps 0 → 409 CONFLICT)
//
// Throws UserRepoError on driver / SQL errors (handler → 500).
//
// Notes:
//   - The INSERT lists every column explicitly so we never accidentally
//     write NULL into a NOT NULL column (e.g. password_hash) and so a
//     future schema addition doesn't get a surprise default.
//   - email / avatar default to NULL when not provided.
//   - created_at is set by the DB DEFAULT CURRENT_TIMESTAMP so two
//     users created in the same second still get distinct ids (auto
//     increment is the source of truth for ordering).
//   - We deliberately do NOT pre-check username_exists() before INSERT;
//     the UNIQUE constraint on `username` (and `email`) is the
//     authoritative gate, and the INSERT-then-check pattern is one
//     round-trip. The race window between "I checked, it's free" and
//     "I inserted" is closed by the UNIQUE constraint at INSERT time.
inline int create_user(ConnectionPool& pool, const UserRow& row) {
    auto conn = pool.acquire();

    // mysqlx doesn't have implicit conversion from std::optional into
    // a NULL bind, so we lift the optional columns into mysqlx::Value
    // explicitly. Required fields stay std::string (they get converted
    // to mysqlx::Value by bind()).
    const std::string role = row.role.empty() ? std::string("user") : row.role;
    mysqlx::Value email_val = row.email
        ? mysqlx::Value(*row.email)
        : mysqlx::Value(nullptr);
    mysqlx::Value avatar_val = row.avatar
        ? mysqlx::Value(*row.avatar)
        : mysqlx::Value(nullptr);

    try {
        auto rs = conn.execute(
            "INSERT INTO users "
            "(username, password_hash, role, email, avatar) "
            "VALUES (?, ?, ?, ?, ?)",
            row.username, row.password_hash, role, email_val, avatar_val);
        // mysqlx surfaces the auto-increment value via getAutoIncrement().
        const auto id = rs.getAutoIncrementValue();
        return static_cast<int>(id);
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        // MySQL surfaces a uniqueness violation with errno 1062. We
        // match on the message text rather than a version-specific
        // symbol so the code keeps working across connector versions.
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            return 0;
        }
        throw UserRepoError(std::string("user_repo::create_user: ") + what);
    }
}

// username_exists — quick boolean pre-check used by the register handler
// to give a precise "username taken" message (vs "email taken") before
// hitting INSERT. Optional — the INSERT path already returns 0 on
// collision — but it lets the route surface a clearer error.
inline bool username_exists(ConnectionPool& pool, std::string_view username) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM users WHERE username = ? LIMIT 1",
        std::string(username));
    return row.has_value();
}

// email_exists — same idea for the optional email column. Skipped when
// the request didn't provide one (caller checks `email.has_value()`).
inline bool email_exists(ConnectionPool& pool, std::string_view email) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM users WHERE email = ? LIMIT 1",
        std::string(email));
    return row.has_value();
}

// find_by_username — load a full row (login + profile use this).
// Returns std::nullopt when no such user exists.
//
// Note on the DATE_FORMAT casts: mysql-connector-c++ 9.x returns
// DATETIME columns as a 5–8-byte packed binary when read via
// get<std::string>() — NOT a formatted string like "2026-06-29
// 09:48:04". To get a stable wire-shape across the driver, server,
// and timezone, we cast to CHAR in the SELECT itself. The format
// pattern matches MySQL's default ISO-8601 rendering so the field
// round-trips as text everywhere (audit_logs, JSON, logs). NULL
// columns stay NULL — opt_string() still reports them as std::nullopt.
inline std::optional<UserRow> find_by_username(ConnectionPool& pool,
                                               std::string_view username) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT id, username, display_name, password_hash, role, token_version, "
        "       email, school, bio, avatar, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at, "
        "       DATE_FORMAT(last_login,  '%Y-%m-%d %H:%i:%s') AS last_login, "
        "       last_login_ip, "
        "       DATE_FORMAT(username_changed_at, '%Y-%m-%d %H:%i:%s') AS username_changed_at "
        "FROM users WHERE username = ? LIMIT 1",
        std::string(username));
    if (!row) return std::nullopt;
    return detail::row_to_user(*row);
}

// find_by_id — same as above but keyed by primary key. See the
// DATE_FORMAT rationale on find_by_username.
inline std::optional<UserRow> find_by_id(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT id, username, display_name, password_hash, role, token_version, "
        "       email, school, bio, avatar, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at, "
        "       DATE_FORMAT(last_login,  '%Y-%m-%d %H:%i:%s') AS last_login, "
        "       last_login_ip, "
        "       DATE_FORMAT(username_changed_at, '%Y-%m-%d %H:%i:%s') AS username_changed_at "
        "FROM users WHERE id = ? LIMIT 1",
        id);
    if (!row) return std::nullopt;
    return detail::row_to_user(*row);
}

// update_last_login — set last_login = NOW() and the IP. Called by the
// future login handler after a successful password verify. The IP is
// optional (X-Forwarded-For may be absent); an empty string is stored
// as NULL so the column stays "clean" when no IP is known.
inline void update_last_login(ConnectionPool& pool,
                              int id,
                              std::string_view ip) {
    auto conn = pool.acquire();
    try {
        if (ip.empty()) {
            conn.execute(
                "UPDATE users SET last_login = NOW(), last_login_ip = NULL "
                "WHERE id = ?",
                id);
        } else {
            conn.execute(
                "UPDATE users SET last_login = NOW(), last_login_ip = ? "
                "WHERE id = ?",
                std::string(ip), id);
        }
    } catch (const mysqlx::Error& e) {
        // Best-effort: a failure to stamp last_login should NOT fail
        // the login. Log and move on — the user is still authenticated.
        LOG_WARN("user_repo::update_last_login failed",
                 {{"user_id", std::to_string(id)},
                  {"reason",  e.what()}});
    }
}

// update_role — admin write path. Returns true iff a row was actually
// changed (false ⇒ no such user). Throws on driver errors.
inline bool update_role(ConnectionPool& pool, int id, std::string_view role) {
    if (role != "user" && role != "admin") {
        throw UserRepoError("user_repo::update_role: role must be 'user' or 'admin'");
    }
    auto conn = pool.acquire();
    auto rs = conn.execute(
        "UPDATE users SET role = ? WHERE id = ?",
        std::string(role), id);
    return rs.getAffectedItemsCount() > 0;
}

// ────────────────────────────────────────────────────────────────────
//  v1.3.4 PR 9 ★ 个人资料编辑 + 用户名可改
//  SPEC §5.1 / §5.2 + 新功能
// ────────────────────────────────────────────────────────────────────
//
// update_profile — 写入 display_name / school / bio / email。
// 调用方负责长度校验(前端 50/100/500/100 字符上限),这里只负责
// 类型安全的 optional 绑定。email 通过 SET email = NULL 也允许
// 清空邮箱。返回 true iff 至少 1 行受影响(用户存在)。
inline bool update_profile(ConnectionPool& pool, int id,
                           std::optional<std::string> display_name,
                           std::optional<std::string> school,
                           std::optional<std::string> bio,
                           std::optional<std::string> email) {
    auto conn = pool.acquire();
    mysqlx::Value display_name_val = display_name
        ? mysqlx::Value(*display_name) : mysqlx::Value(nullptr);
    mysqlx::Value school_val = school
        ? mysqlx::Value(*school) : mysqlx::Value(nullptr);
    mysqlx::Value bio_val = bio
        ? mysqlx::Value(*bio) : mysqlx::Value(nullptr);
    mysqlx::Value email_val = email
        ? mysqlx::Value(*email) : mysqlx::Value(nullptr);
    try {
        auto rs = conn.execute(
            "UPDATE users SET display_name = ?, school = ?, bio = ?, email = ? "
            "WHERE id = ?",
            display_name_val, school_val, bio_val, email_val, id);
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        // 邮箱 UNIQUE 冲突(V007 加的 uq_users_email)
        const std::string what = e.what();
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            return false;  // 调用方应该再走 email_exists 区分
        }
        throw UserRepoError(std::string("user_repo::update_profile: ") + what);
    }
}

// update_avatar — 单字段写头像 URL(由上传路由裁剪后写入的相对路径
// /uploads/avatars/{user_id}.jpg)。NULL 不允许(由前端传 null 时
// 也写入 NULL,即"恢复默认")。
inline bool update_avatar(ConnectionPool& pool, int id,
                          std::optional<std::string> avatar_url) {
    auto conn = pool.acquire();
    mysqlx::Value avatar_val = avatar_url
        ? mysqlx::Value(*avatar_url) : mysqlx::Value(nullptr);
    auto rs = conn.execute(
        "UPDATE users SET avatar = ? WHERE id = ?",
        avatar_val, id);
    return rs.getAffectedItemsCount() > 0;
}

// update_username — 改名核心:
//   1) 写 user_username_history(user_id, old_username, NOW())
//   2) UPDATE users SET username = ?, username_changed_at = NOW()
//
// 频率限制(1 天 1 次)由 route 层负责,本函数无条件执行;route 层
// 先 SELECT users.username_changed_at 比对当前时间 < 1 day 则拒绝。
// UNIQUE 冲突(新名已存在)由 route 层先 SELECT username_exists()
// 检查,这里仍然 catch 兜底返回 false 区分"用户不存在"和"重名"。
inline bool update_username(ConnectionPool& pool, int id,
                            std::string_view new_username) {
    auto conn = pool.acquire();
    try {
        // 先读旧 username(用于写 history)
        const auto old_row = conn.fetch_scalar<std::string>(
            "SELECT username FROM users WHERE id = ? LIMIT 1", id);
        if (!old_row) return false;
        const std::string old_username = *old_row;
        if (old_username == new_username) return true;  // 幂等

        // 1) 写 history(UNIQUE 兜底)
        conn.execute(
            "INSERT INTO user_username_history (user_id, old_username) "
            "VALUES (?, ?)",
            id, old_username);

        // 2) UPDATE users
        auto rs = conn.execute(
            "UPDATE users SET username = ?, username_changed_at = NOW() "
            "WHERE id = ?",
            std::string(new_username), id);
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            return false;  // 新名已存在
        }
        throw UserRepoError(std::string("user_repo::update_username: ") + what);
    }
}

// find_by_old_username — alias 查找。profile?username=oldname 时,
// users 表 miss 后查 history 表,如果 oldname 命中 → 跳当前 user_id。
// 返回当前 user_id + 当前 username(给前端做 301 跳的 target)。
struct OldUsernameHit {
    int          user_id;
    std::string  current_username;
};
inline std::optional<OldUsernameHit> find_by_old_username(
        ConnectionPool& pool, std::string_view old_username) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT h.user_id, u.username "
        "FROM user_username_history h "
        "JOIN users u ON u.id = h.user_id "
        "WHERE h.old_username = ? LIMIT 1",
        std::string(old_username));
    if (!row) return std::nullopt;
    OldUsernameHit h;
    // fetch_one 返回 std::optional<mysqlx::Row>,row[0] 实际是
    // (*row)[0](optional 本身没 operator[]);mysqlx::Row 有 operator[]。
    h.user_id         = static_cast<int>((*row)[0].get<std::int64_t>());
    h.current_username = (*row)[1].get<std::string>();
    return h;
}

// ────────────────────────────────────────────────────────────────────────────
//  Admin list / count (Phase 6 ★)
//
//  These back GET /api/v1/admin/users. We deliberately keep the search
//  and role filters as a small struct (same shape as ProblemListFilter)
//  so the route layer can translate a JSON query string into a filter
//  without touching SQL.
//
//  Note: the route handler `admin_user_routes.h` joins `submissions`
//  via a subquery to compute `submission_count` per row (one extra
//  GROUP BY per page). The list itself stays on the `users` table so
//  the simple cases (empty / few users) stay fast — we don't want
//  the admin list to block on a full `users × submissions` join.
//
//  Pagination: limit defaults to kDefaultListLimit (20) and is
//  clamped to kMaxListLimit (100) by clamp_list_filter() so a
//  careless client can't ask for 10M rows in one shot.
// ────────────────────────────────────────────────────────────────────────────

struct UserListFilter {
    std::optional<std::string>  role;       // exact match: "user" | "admin"
    std::optional<std::string>  q;          // substring search on username
    int                         limit       = 20;
    int                         offset      = 0;
};

inline constexpr int kDefaultUserListLimit = 20;
inline constexpr int kMaxUserListLimit     = 100;

inline void clamp_user_list_filter(UserListFilter& f) {
    if (f.limit  <= 0)            f.limit  = kDefaultUserListLimit;
    if (f.limit  > kMaxUserListLimit) f.limit = kMaxUserListLimit;
    if (f.offset < 0)             f.offset = 0;
}

// count — total number of users matching the filter (used as the
// `total` field of the list response so the front-end can paginate
// without a second round-trip).
inline int count_users(ConnectionPool& pool, const UserListFilter& f_in) {
    UserListFilter f = f_in;
    // limit / offset don't affect COUNT but clamp anyway so a
    // single filter struct can be shared.
    clamp_user_list_filter(f);
    auto conn = pool.acquire();
    try {
        // We build the WHERE clause + bind chain dynamically so a
        // missing filter doesn't add a useless `AND role = ?`
        // predicate. The pattern is the same as
        // audit_log_repo::list() — one chain, no dispatch table.
        // Build the FINAL sql string before constructing the
        // SqlStatement; see comment in list_users() for why
        // incremental .sql() + .append() silently produces
        // "Too many arguments" at execute() time.
        std::string sql = "SELECT COUNT(*) FROM users WHERE 1=1";
        std::vector<std::string> str_binds;
        if (f.role.has_value()) {
            sql += " AND role = ?";
            str_binds.push_back(*f.role);
        }
        if (f.q.has_value() && !f.q->empty()) {
            // MySQL's LIKE is case-insensitive by default for
            // utf8mb4_general_ci / utf8mb4_0900_ai_ci, so a
            // substring search "alice" matches "Alice" too.
            // We escape LIKE metacharacters (% and _) so a
            // search for "100%" doesn't act as a wildcard.
            std::string escaped = *f.q;
            for (auto& c : escaped) {
                if (c == '%' || c == '_') c = '\\';
            }
            sql += " AND username LIKE ?";
            str_binds.push_back("%" + escaped + "%");
        }
        mysqlx::SqlStatement stmt = conn.session().sql(sql);
        for (const auto& s : str_binds) stmt.bind(s);
        auto rs = stmt.execute();
        for (auto row : rs) {
            return static_cast<int>(row[0].get<std::int64_t>());
        }
        return 0;
    } catch (const mysqlx::Error& e) {
        throw UserRepoError(std::string("user_repo::count_users: ") + e.what());
    }
}

// list — one page of user rows matching the filter, ordered by
// id ASC (oldest first; admin pages want stable ordering for
// pagination). Each row carries a `submission_count` derived from
// a correlated subquery so a single SELECT covers the whole page.
//
// The submission_count is intentionally a correlated subquery
// rather than a JOIN + GROUP BY because:
//   1. Most admin pages have small result sets (a few hundred
//      users at most in MVP) — the subquery cost is bounded.
//   2. A LEFT JOIN with GROUP BY would change the row shape
//      and force us to coalesce NULLs for users with zero
//      submissions; the subquery returns 0 naturally.
//   3. MySQL's optimizer can often rewrite the subquery to a
//      derived table once the user table is small.
//
// If the admin table grows past 10k users we'll revisit this
// (denormalize submission_count into users via a trigger, or
// accept the join cost).
struct UserListRow {
    UserRow         user;            // full user row (no password_hash in response)
    int             submission_count = 0;
};

inline std::vector<UserListRow> list_users(ConnectionPool& pool,
                                            const UserListFilter& f_in) {
    UserListFilter f = f_in;
    clamp_user_list_filter(f);
    std::vector<UserListRow> out;
    auto conn = pool.acquire();
    try {
        // Build the WHERE clause + bind values first; pass the
        // FINAL sql string to conn.session().sql() so the
        // SqlStatement sees every `?` placeholder it will later
        // bind. Building sql + binds incrementally before
        // constructing SqlStatement is the canonical pattern
        // documented in audit_log_repo::list() — passing an
        // early sql string to .sql() and then appending more
        // placeholders silently produces "Too many arguments"
        // at execute() time because the SqlStatement has the
        // shorter sql cached.
        std::string sql =
            "SELECT u.id, u.username, u.display_name, u.password_hash, u.role, u.token_version, "
            "       u.email, u.school, u.bio, u.avatar, "
            "       DATE_FORMAT(u.created_at, '%Y-%m-%d %H:%i:%s') AS created_at, "
            "       DATE_FORMAT(u.last_login,  '%Y-%m-%d %H:%i:%s') AS last_login, "
            "       u.last_login_ip, "
            "       DATE_FORMAT(u.username_changed_at, '%Y-%m-%d %H:%i:%s') AS username_changed_at, "
            "       (SELECT COUNT(*) FROM submissions s WHERE s.user_id = u.id) "
            "         AS submission_count "
            "FROM users u WHERE 1=1";
        std::vector<std::string> str_binds;
        if (f.role.has_value()) {
            sql += " AND u.role = ?";
            str_binds.push_back(*f.role);
        }
        if (f.q.has_value() && !f.q->empty()) {
            std::string escaped = *f.q;
            for (auto& c : escaped) {
                if (c == '%' || c == '_') c = '\\';
            }
            sql += " AND u.username LIKE ?";
            str_binds.push_back("%" + escaped + "%");
        }
        sql += " ORDER BY u.id ASC LIMIT ? OFFSET ?";

        mysqlx::SqlStatement stmt = conn.session().sql(sql);
        for (const auto& s : str_binds) {
            stmt.bind(s);
        }
        stmt.bind(static_cast<std::int64_t>(f.limit));
        stmt.bind(static_cast<std::int64_t>(f.offset));
        mysqlx::SqlResult rs = stmt.execute();
        for (auto row : rs) {
            try {
                UserListRow r;
                r.user            = detail::row_to_user(row);
                r.submission_count = static_cast<int>(
                    row[9].get<std::int64_t>());
                out.push_back(std::move(r));
            } catch (const std::exception&) {
                // Skip malformed rows so one bad row doesn't
                // tank the whole page.
            }
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw UserRepoError(std::string("user_repo::list_users: ") + e.what());
    }
}

} // namespace user_repo
} // namespace litecode