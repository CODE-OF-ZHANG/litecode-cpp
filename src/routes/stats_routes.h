// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — stats routes (Phase 6 ★)
//
// SPEC §5.4 / §11 Phase 6 / §12.1 / A15 acceptance:
//   - GET /api/v1/stats/profile/:username — auth required, no rate limit
//
//       Returns a user's "做题统计" (problem-solving statistics) for the
//       /profile/:username page. The response shape is the same one
//       profile.html (v1.2.30) renders client-side, so a Phase 5 page
//       rewrite to point at this endpoint is a one-line fetch switch.
//
//   - GET /api/v1/stats/ranking — public, 30/min/IP
//
//       Returns the all-time leaderboard, sorted by
//       `solved_count DESC, submission_count ASC, user_id ASC`. The
//       first tier is "who solved the most distinct live problems";
//       the second tier is "who reached that count with the fewest
//       attempts" (a tiebreaker that rewards efficiency, the
//       Codeforces/LeetCode convention); the third tier is a
//       deterministic `user_id ASC` so the rank is stable across
//       page refreshes (otherwise two users with identical stats
//       could swap places on every request, confusing the
//       "current user" highlight on /ranking.html).
//
// Wire shape (response, 200):
//   {
//     "user": {
//       "id":         42,
//       "username":   "alice",
//       "role":       "user" | "admin",
//       "created_at": "2026-07-01 12:34:56"
//     },
//     "stats": {
//       "total_submissions":   100,         // ALL submissions (incl. pending/running)
//       "solved_count":         25,         // DISTINCT problems with at least one 'ac'
//       "total_problems":       50,         // live (not soft-deleted) problem count
//       "attempted_count":      40,         // DISTINCT problems the user has submitted to
//       "acceptance_rate":      50.0,       // 0.0 .. 100.0; solved_count / total_problems * 100
//       "by_status": {
//         "pending": 0, "running": 1,
//         "ac": 50, "wa": 30, "re": 5, "tle": 5, "mle": 2,
//         "ole": 0, "pe": 0, "ce": 5, "se": 2
//       },
//       "by_difficulty_solved": {
//         "easy":   15,
//         "medium": 8,
//         "hard":   2
//       }
//     },
//     "request_id": "..."
//   }
//
//   /api/v1/stats/ranking wire shape (200):
//   {
//     "items": [
//       {
//         "rank":             1,
//         "user":  { "id": 42, "username": "alice", "role": "user" },
//         "solved_count":     25,
//         "submission_count": 80,
//         "acceptance_rate":  31.25
//       },
//       ...
//     ],
//     "total":   17,    // # of users with ≥ 1 AC on a live problem
//     "limit":   100,
//     "offset":  0,
//     "request_id": "..."
//   }
//
// Failure modes (profile):
//   - No / bad access token        → 401 UNAUTHORIZED
//   - Username path component has
//     bad shape (regex / length)   → 400 INVALID_INPUT with details.field=username
//   - User does not exist          → 404 NOT_FOUND with details.username
//   - Any repo throw               → 500 INTERNAL_ERROR
//
// Failure modes (ranking):
//   - Bad limit/offset             → 400 INVALID_INPUT with details.field
//   - 30/min/IP rate limit exceeded→ 429 RATE_LIMITED (+ Retry-After + X-RateLimit-* headers)
//   - Any repo throw               → 500 INTERNAL_ERROR
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3/4/5 route
//     module (auth_routes.h / problem_routes.h / submission_routes.h).
//   - The 5-step pipeline (require_authentication → path-parse + validate
//     → user_repo::find_by_username → aggregate 4 repo queries →
//     serialize) matches Phase 3/4 admin endpoints but stops short of
//     write + audit_logs because stats reads are not auditable actions
//     (SPEC §5.6 / §11 — audit_logs is for "key admin operations").
//   - The by_difficulty_solved query JOINs submissions with problems so
//     we get the difficulty only for problems that are still live
//     (is_deleted = FALSE). A soft-deleted problem that the user AC'd
//     does NOT count in the difficulty breakdown — the public
//     difficulty list never shows tombstones, and the per-user
//     breakdown follows the same rule.
//   - The leaderboard query is implemented in a single SQL with a
//     derived-table aggregation (`submissions GROUP BY user_id`) JOINed
//     back to `users`. We then `WHERE` on `solved_count > 0` so a
//     user who has submitted but never AC'd doesn't show up at the
//     bottom with rank=N+1. This matches the SPEC §11 F11
//     "按解题数/通过率排名" semantic — solving zero problems is
//     not a "rank" position.
//   - We deliberately do NOT include `code` / `password_hash` /
//     `last_login_ip` in the response. The profile page has no need
//     for them, and surfacing them is a confidentiality regression
//     compared to the v1.2.30 client-side aggregation (which never
//     had access to them in the first place).
//   - ODR caveat (same as problem_routes.h / submission_routes.h):
//     this header transitively pulls in user_repo.h / submission_repo.h
//     / problem_repo.h. main.cpp does NOT smoke-register this header
//     because the existing main.cpp smoke stack already pulls in
//     problem_repo.h via admin_problem_routes.h / admin_bulk_import_routes.h
//     and adding this header would collide on
//     `litecode::detail::req_string` / `opt_string` across the three
//     repos. End-to-end coverage is provided by
//     tests/unit/test_stats_profile.cpp. When the Phase 6 endpoint
//     collection stabilizes, the ODR issue can be cleaned up by
//     moving each repo's `detail` symbols into per-repo namespaces
//     (e.g. `user_repo::detail::opt_string`).
//
// Usage (production, from main.cpp):
//
//   litecode::HttpServer     server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter    limiter;
//   litecode::register_stats_routes(server, pool, limiter, cfg.rate_limit, cfg.jwt);
//   server.listen_blocking();
//
// Usage (test, from gtest):
//
//   litecode::HttpServer     server(dev_server(), dev_cors());
//   litecode::ConnectionPool pool(test_db_config());
//   litecode::RateLimiter    limiter;
//   litecode::JwtConfig      jwt_cfg = test_jwt_config();
//   litecode::register_stats_routes(server, pool, limiter, lax_rate_limit(), jwt_cfg);
//   auto h = start_server(&server);
//   auto r = h.client->Get("/api/v1/stats/profile/alice");
//   auto r2 = h.client->Get("/api/v1/stats/ranking?limit=20");

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "../config.h"                          // RateLimitConfig / JwtConfig
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/problem_repo.h"                 // problem_repo::count (live problems)
#include "../db/submission_repo.h"              // submission_repo::count
#include "../middleware/rate_limit.h"           // consume_rate_limit / RateLimitQuota
#include "../logger.h"                          // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../middleware/auth_middleware.h"      // require_authentication / Claims
#include "../routes/error_handler.h"            // ApiException / ErrorCode / send_error
#include "../server.h"                          // HttpServer / send_success

// IMPORTANT: We deliberately do NOT include "../db/user_repo.h" here.
// user_repo.h defines `litecode::detail::req_string` and
// `litecode::detail::req_int`, which collide with the same names in
// problem_repo.h when both headers are pulled into the same TU under
// MSVC's strict One-Definition-Rule. Since problem_repo.h is required
// for `problem_repo::count`, we re-implement the user lookup + the
// username validator inline below (both ~30 lines and identical to the
// upstream versions) so stats_routes.h compiles cleanly alongside
// problem_repo.h. This is the same workaround used by test_submission
// (see its comment: "Insert a throwaway user via raw SQL to dodge
// the litecode::detail::req_string ODR collision").

namespace litecode {
namespace stats_routes {
namespace detail {

// ────────────────────────────────────────────────────────────────────────────
//  Inline username validation
//
//  Mirrors user_repo::validate_username (3..50 chars, ASCII letters /
//  digits / '_' / '-' / '.', no leading / trailing '.' or '-'). We
//  re-declare the constants here so the regex stays in lock-step with
//  the registration form; a future commit can centralize this in a
//  common header if more modules need it.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kStatsMinUsernameLength = 3;
inline constexpr std::size_t kStatsMaxUsernameLength = 50;

inline bool stats_is_valid_username_char(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return (uc >= 'a' && uc <= 'z') ||
           (uc >= 'A' && uc <= 'Z') ||
           (uc >= '0' && uc <= '9') ||
           uc == '_' || uc == '-' || uc == '.';
}

inline bool stats_validate_username(std::string_view username,
                                    std::string* error_out = nullptr) {
    if (username.size() < kStatsMinUsernameLength ||
        username.size() > kStatsMaxUsernameLength) {
        if (error_out) {
            *error_out = "username must be between " +
                         std::to_string(kStatsMinUsernameLength) + " and " +
                         std::to_string(kStatsMaxUsernameLength) + " characters";
        }
        return false;
    }
    if (username.front() == '.' || username.front() == '-' ||
        username.back()  == '.' || username.back()  == '-') {
        if (error_out) {
            *error_out = "username must not start or end with '.' or '-'";
        }
        return false;
    }
    for (char c : username) {
        if (!stats_is_valid_username_char(c)) {
            if (error_out) {
                *error_out = "username may only contain letters, digits, '_', '-', '.'";
            }
            return false;
        }
    }
    return true;
}

// UserProfileRow — the slice of `users` we surface in the response.
// Deliberately a local struct (not user_repo::UserRow) so this header
// compiles without user_repo.h. The fields are 1:1 with the SELECT
// columns below; keep them in lock-step.
struct UserProfileRow {
    int                id         = 0;
    std::string        username;
    std::string        role;
    std::string        created_at;
};

inline std::optional<UserProfileRow> find_user_for_stats(
        ConnectionPool& pool, const std::string& username) {
    auto conn = pool.acquire();
    try {
        const auto row = conn.fetch_one(
            "SELECT id, username, role, "
            "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
            "         AS created_at "
            "FROM users WHERE username = ? LIMIT 1",
            username);
        if (!row) return std::nullopt;
        UserProfileRow u;
        const auto& r = *row;
        u.id         = static_cast<int>(r[0].get<std::int64_t>());
        u.username   = r[1].get<std::string>();
        u.role       = r[2].get<std::string>();
        u.created_at = r[3].get<std::string>();
        return u;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("find_user_for_stats: ") + e.what());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Helpers (namespace `stats_routes::detail` — avoids the cross-repo ODR
//  collision that would happen if we used the bare `litecode::detail`
//  shared with problem_routes.h / submission_routes.h / etc.)
//
//  extract_username_from_path — strip the
//  "/api/v1/stats/profile/" prefix from req.path and hand the remainder
//  to validate_username. Returns the username (unchanged) on success, or
//  std::nullopt on any shape failure (bad prefix / empty tail / regex
//  mismatch / length out of range).
//
//  We deliberately use the user_repo::validate_username regex (3..50
//  ASCII letters / digits / '_' / '-' / '.') so the path component
//  matches the same constraints as a registration form. A username
//  that would fail registration is rejected at the URL layer too —
//  this prevents an attacker from enumerating usernames with
//  case-variants or unicode (the regex is strict).
// ────────────────────────────────────────────────────────────────────────────

inline std::optional<std::string> extract_username_from_path(
        const httplib::Request& req) {
    static constexpr std::string_view kPrefix = "/api/v1/stats/profile/";
    const std::string& path = req.path;
    if (path.size() <= kPrefix.size()) return std::nullopt;
    if (path.compare(0, kPrefix.size(), kPrefix) != 0) return std::nullopt;
    const std::string_view tail =
        std::string_view(path).substr(kPrefix.size());
    if (tail.empty()) return std::nullopt;
    if (tail.find('/') != std::string_view::npos) return std::nullopt;

    const std::string username(tail);
    std::string err;
    if (!stats_validate_username(username, &err)) {
        return std::nullopt;
    }
    return username;
}

// count_user_submissions — total submissions rows for `user_id`.
// Includes pending/running (matches the "total_submissions" semantics
// in the profile page: "all the things I clicked Submit on").
// Returns 0 when the user has none (a fresh user, or a user who
// registered but never submitted).
inline int count_user_submissions(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    try {
        const auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT COUNT(*) FROM submissions WHERE user_id = ?",
            user_id);
        return v.has_value() ? static_cast<int>(*v) : 0;
    } catch (const mysqlx::Error& e) {
        // Bubble up as a generic exception so the route handler can
        // fold it into a 500 envelope. We don't expose the raw
        // driver message to the client.
        throw std::runtime_error(std::string("count_user_submissions: ") + e.what());
    }
}

// count_user_solved_problems — DISTINCT problem_ids the user has at
// least one 'ac' submission for. Soft-deleted problems are excluded
// from the count (the user can still have an AC row pointing at one,
// but it doesn't count toward "已解决").
//
// Implementation note: we JOIN submissions → problems so the
// is_deleted check is in the same query — keeps the SELECT a single
// round-trip and lets the (user_id, problem_id, created_at) index
// on submissions carry the predicate.
inline int count_user_solved_problems(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    try {
        const auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT COUNT(DISTINCT s.problem_id) "
            "FROM submissions s "
            "JOIN problems p ON p.id = s.problem_id "
            "WHERE s.user_id = ? "
            "  AND s.status = 'ac' "
            "  AND p.is_deleted = FALSE",
            user_id);
        return v.has_value() ? static_cast<int>(*v) : 0;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("count_user_solved_problems: ") + e.what());
    }
}

// count_user_attempted_problems — DISTINCT problem_ids the user has
// at least one submission for (any status). Excludes soft-deleted
// problems so a submitted-to-a-tombstone row doesn't bloat the count.
// Used to render "尝试过 N 道题" on the profile page.
inline int count_user_attempted_problems(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    try {
        const auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT COUNT(DISTINCT s.problem_id) "
            "FROM submissions s "
            "JOIN problems p ON p.id = s.problem_id "
            "WHERE s.user_id = ? "
            "  AND p.is_deleted = FALSE",
            user_id);
        return v.has_value() ? static_cast<int>(*v) : 0;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("count_user_attempted_problems: ") + e.what());
    }
}

// count_submissions_by_status — returns a map status -> count for
// every status that has ≥1 row for the user. Used to render the
// "by_status" breakdown on the profile page.
//
// We deliberately do NOT zero-pad absent statuses here. The route
// handler does that so the response shape stays stable regardless of
// which statuses the user has hit (a fresh user has no rows; an
// intermediate user may have skipped 'ole' entirely; both render
// the same JSON).
inline std::unordered_map<std::string, int> count_submissions_by_status(
        ConnectionPool& pool, int user_id) {
    std::unordered_map<std::string, int> out;
    auto conn = pool.acquire();
    try {
        mysqlx::SqlResult rs = conn.execute(
            "SELECT status, COUNT(*) AS cnt "
            "FROM submissions "
            "WHERE user_id = ? "
            "GROUP BY status",
            user_id);
        for (auto row : rs) {
            try {
                const std::string status = row[0].get<std::string>();
                const std::int64_t cnt   = row[1].get<std::int64_t>();
                out[status] = static_cast<int>(cnt);
            } catch (const std::exception&) {
                // Skip individual malformed rows so one bad row doesn't
                // tank the whole breakdown.
            }
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("count_submissions_by_status: ") + e.what());
    }
}

// count_solved_by_difficulty — returns a map difficulty -> count
// for the DISTINCT problems (live only) the user has AC'd, grouped
// by difficulty. Used to render the "by_difficulty_solved" bar
// (easy / medium / hard).
//
// Implementation note: same JOIN shape as count_user_solved_problems
// so the optimizer can reuse work. We always GROUP BY difficulty so
// the map has at most 3 entries (one per ENUM value); absent
// difficulties are zero-padded in the route handler.
inline std::unordered_map<std::string, int> count_solved_by_difficulty(
        ConnectionPool& pool, int user_id) {
    std::unordered_map<std::string, int> out;
    auto conn = pool.acquire();
    try {
        mysqlx::SqlResult rs = conn.execute(
            "SELECT p.difficulty, COUNT(DISTINCT s.problem_id) AS solved "
            "FROM submissions s "
            "JOIN problems p ON p.id = s.problem_id "
            "WHERE s.user_id = ? "
            "  AND s.status = 'ac' "
            "  AND p.is_deleted = FALSE "
            "  AND p.difficulty IN ('easy','medium','hard') "
            "GROUP BY p.difficulty",
            user_id);
        for (auto row : rs) {
            try {
                const std::string diff = row[0].get<std::string>();
                const std::int64_t cnt = row[1].get<std::int64_t>();
                out[diff] = static_cast<int>(cnt);
            } catch (const std::exception&) {
                // Skip malformed rows defensively.
            }
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("count_solved_by_difficulty: ") + e.what());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Leaderboard helpers (Phase 6 ★ GET /api/v1/stats/ranking)
//
//  The leaderboard is one page-worth of users with at least 1 AC on
//  a live problem, sorted by `solved_count DESC, submission_count
//  ASC, user_id ASC`. Each row carries enough to render a row in
//  /ranking.html: rank, user (id/username/role), solved_count,
//  submission_count, acceptance_rate.
//
//  Implementation note: the SQL is one statement with a derived
//  table. The derived table aggregates per-user counts once, then
//  we JOIN it back to `users` for username / role / id and apply
//  the WHERE / ORDER BY / LIMIT in the outer SELECT. This shape
//  matches what MySQL 8.x's optimizer expects (a `GROUP BY` over
//  submissions with a non-correlated subquery in WHERE is the
//  canonical "rank users by solve count" pattern).
//
//  Soft-deleted problems are excluded (same rule as the per-user
//  stats). Admin users appear in the ranking if they have
//  submissions too — there is no "admin role" filter because the
//  front-end wants to show the entire community.
// ────────────────────────────────────────────────────────────────────────────

// LeaderboardRow — a single row in the leaderboard, projection of
// (user, aggregate) JOIN. Same shape as the response items[]. Used
// by the route handler and the unit tests.
struct LeaderboardRow {
    int                user_id           = 0;
    std::string        username;
    std::string        role;
    int                solved_count      = 0;
    int                submission_count  = 0;
    double             acceptance_rate   = 0.0;
};

// kLeaderboardDefaultLimit — SPEC §5.4 "默认 100 名". Surfaced as a
// constant so the front-end (`/ranking.html`) and the route handler
// agree on the default without cross-referencing headers.
inline constexpr int kLeaderboardDefaultLimit = 100;
// kLeaderboardMaxLimit — defense-in-depth upper bound. SPEC §5.4
// doesn't pin a hard cap, but unbounded LIMITs on a per-IP
// 30/min quota are still a DoS surface (an attacker can request
// 10M rows in one shot). 200 is generous for an OJ and keeps the
// page payload under ~50KB JSON.
inline constexpr int kLeaderboardMaxLimit     = 200;

// count_ranked_users — returns the total # of users that appear in
// the leaderboard.
//
// v1.3.4: 排行榜改成"全员上榜"(从 users 出发 LEFT JOIN submissions
// 聚合)。零提交 / 零 AC 的用户也会出现,排在尾部。这里返回 users
// 总数,与 list_leaderboard() 的查询语义保持一致 — 否则分页 total
// 字段会与列表不一致。
inline int count_ranked_users(ConnectionPool& pool) {
    auto conn = pool.acquire();
    try {
        const auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT COUNT(*) FROM users");
        return v.has_value() ? static_cast<int>(*v) : 0;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("count_ranked_users: ") + e.what());
    }
}

// list_leaderboard — returns one page of LeaderboardRow, ordered
// solved_count DESC, submission_count ASC, user_id ASC.
//
// v1.3.4: 从 `users` 出发 LEFT JOIN 聚合,**所有用户都上榜**;零提交
// / 零 AC 用户排在尾部。原来的 HAVING solved_count > 0 会过滤掉所有
// 没 AC 的用户,导致 zhangxu 这种还在解题的注册用户从不出现 — 这是
// 用户报"排行榜全是匿名用户"的独立根因(另一个根因是前端嵌套字段
// 读取 bug,在 web/ranking.html:194 修)。
//
// The query:
//   SELECT u.id, u.username, u.role,
//          COALESCE(agg.solved_count, 0)     AS solved_count,
//          COALESCE(agg.submission_count, 0) AS submission_count
//   FROM users u
//   LEFT JOIN (
//     SELECT s.user_id,
//            COUNT(DISTINCT CASE WHEN s.status = 'ac' AND p.is_deleted = FALSE
//                                THEN s.problem_id END) AS solved_count,
//            COUNT(CASE WHEN p.is_deleted = FALSE THEN 1 END) AS submission_count
//     FROM submissions s
//     LEFT JOIN problems p ON p.id = s.problem_id
//     GROUP BY s.user_id
//   ) agg ON agg.user_id = u.id
//   ORDER BY solved_count DESC, submission_count ASC, u.id ASC
//   LIMIT ? OFFSET ?
//
// COALESCE 让 LEFT JOIN 出来的 NULL 转成 0,前端不用处理 null;
// `ORDER BY u.id ASC` 在 solved_count/submission_count 都相同时
// 保证分页结果确定性。
inline std::vector<LeaderboardRow> list_leaderboard(
        ConnectionPool& pool, int limit, int offset) {
    std::vector<LeaderboardRow> out;
    auto conn = pool.acquire();
    try {
        mysqlx::SqlResult rs = conn.execute(
            "SELECT u.id, u.username, u.role, "
            "       COALESCE(agg.solved_count, 0), "
            "       COALESCE(agg.submission_count, 0) "
            "FROM users u "
            "LEFT JOIN ( "
            "  SELECT s.user_id, "
            "         COUNT(DISTINCT CASE WHEN s.status = 'ac' "
            "                              AND p.is_deleted = FALSE "
            "                              THEN s.problem_id END) "
            "           AS solved_count, "
            "         COUNT(CASE WHEN p.is_deleted = FALSE "
            "                    THEN 1 END) "
            "           AS submission_count "
            "  FROM submissions s "
            "  LEFT JOIN problems p ON p.id = s.problem_id "
            "  GROUP BY s.user_id "
            ") agg ON agg.user_id = u.id "
            "ORDER BY solved_count DESC, "
            "         submission_count ASC, "
            "         u.id ASC "
            "LIMIT ? OFFSET ?",
            limit, offset);
        for (auto row : rs) {
            LeaderboardRow r;
            try {
                r.user_id          = static_cast<int>(row[0].get<std::int64_t>());
                r.username         = row[1].get<std::string>();
                r.role             = row[2].get<std::string>();
                r.solved_count     = static_cast<int>(row[3].get<std::int64_t>());
                r.submission_count = static_cast<int>(row[4].get<std::int64_t>());
            } catch (const std::exception&) {
                // Skip individual malformed rows so one bad row
                // doesn't tank the whole page.
                continue;
            }
            // v1.3.4: 修正 acceptance_rate 语义。之前的实现算的是
            //   submission_count * 100 / solved_count
            // 注释把它叫做"average attempts per AC",但 acceptance
            // rate 顾名思义是"通过率",应该是
            //   solved_count * 100 / submission_count
            // 数值 0..100 百分比。前端展示时除以 100 显示百分比。
            // 零提交用户固定 0.0(无法计算)。
            r.acceptance_rate =
                (r.submission_count > 0)
                    ? (static_cast<double>(r.solved_count) * 100.0 /
                       static_cast<double>(r.submission_count))
                    : 0.0;
            out.push_back(std::move(r));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw std::runtime_error(
            std::string("list_leaderboard: ") + e.what());
    }
}

// rank_for_offset — given the offset of a row, return its
// absolute rank (= offset + 1). Used by the handler when
// assigning `rank` to each item. We compute it on the server side
// because the client only sees one page at a time, so it can't
// trivially rank itself; doing it here also means the rank
// always reflects the actual ordering, not the page-local
// order (which can have gaps if the caller is paginating).
inline int rank_for_offset(int offset, int page_index) {
    return offset + page_index + 1;
}

}  // namespace detail
}  // namespace stats_routes

// ────────────────────────────────────────────────────────────────────────────
//  Aggregate computation — turns the raw repo helpers into the
//  `stats` block of the response.
//
//  All five sub-queries are independent and could in principle run
//  concurrently, but ConnectionPool's API gives us a connection per
//  call and our local MySQL handles the 5-query burst well under
//  the SPEC §12.2 "< 200ms" budget. Parallelizing via std::async
//  would complicate the test fixture (which owns a single pool) for
//  marginal latency win at this scale.
// ────────────────────────────────────────────────────────────────────────────

struct UserStats {
    int total_submissions     = 0;
    int solved_count          = 0;
    int attempted_count       = 0;
    int total_problems        = 0;
    double acceptance_rate    = 0.0;
    std::unordered_map<std::string, int> by_status;
    std::unordered_map<std::string, int> by_difficulty_solved;
};

inline UserStats compute_user_stats(ConnectionPool& pool, int user_id) {
    UserStats out;
    out.total_submissions =
        stats_routes::detail::count_user_submissions(pool, user_id);
    out.solved_count =
        stats_routes::detail::count_user_solved_problems(pool, user_id);
    out.attempted_count =
        stats_routes::detail::count_user_attempted_problems(pool, user_id);

    // total_problems — uses the existing problem_repo::count() with
    // include_deleted=false so the profile page denominator matches
    // the public problem list denominator.
    {
        litecode::ProblemListFilter f;
        f.include_deleted = false;
        f.limit           = 1;   // unused; only total matters
        f.offset          = 0;
        out.total_problems = litecode::problem_repo::count(pool, f);
    }

    // acceptance_rate = solved_count / total_problems * 100.0,
    // 0.0 when total_problems == 0 (no division by zero; a freshly
    // created OJ with zero problems renders 0.0%, not NaN).
    if (out.total_problems > 0) {
        out.acceptance_rate =
            (static_cast<double>(out.solved_count) * 100.0) /
            static_cast<double>(out.total_problems);
    } else {
        out.acceptance_rate = 0.0;
    }

    out.by_status =
        stats_routes::detail::count_submissions_by_status(pool, user_id);
    out.by_difficulty_solved =
        stats_routes::detail::count_solved_by_difficulty(pool, user_id);
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  Serialization
//
//  The status / difficulty maps are zero-padded to the full enum set
//  so the client never has to guess whether a key is missing because
//  it was zero or because it was skipped — a stable shape beats a
//  sparse one for front-end rendering.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_by_status(
        const std::unordered_map<std::string, int>& m) {
    // The order matches the SubmissionRow's kStatus* constants. We
    // build the JSON object explicitly rather than dumping a map so
    // the iteration order is deterministic across compilers and
    // MySQL versions.
    nlohmann::json j = {
        {"pending", 0}, {"running", 0},
        {"ac",      0}, {"wa",      0},
        {"re",      0}, {"tle",     0},
        {"mle",     0}, {"ole",     0},
        {"pe",      0}, {"ce",      0},
        {"se",      0},
    };
    for (const auto& [k, v] : m) {
        // Defensive: only set known keys. A hostile or future
        // status name won't break the response.
        if (j.contains(k)) j[k] = v;
    }
    return j;
}

inline nlohmann::json serialize_by_difficulty(
        const std::unordered_map<std::string, int>& m) {
    nlohmann::json j = {
        {"easy",   0},
        {"medium", 0},
        {"hard",   0},
    };
    for (const auto& [k, v] : m) {
        if (j.contains(k)) j[k] = v;
    }
    return j;
}

inline nlohmann::json serialize_user_stats(const UserStats& s) {
    return nlohmann::json{
        {"total_submissions",    s.total_submissions},
        {"solved_count",         s.solved_count},
        {"attempted_count",      s.attempted_count},
        {"total_problems",       s.total_problems},
        // Front-end formats with .toFixed(1); we trust IEEE 754 here
        // because the value range is bounded (0..100).
        {"acceptance_rate",      s.acceptance_rate},
        {"by_status",            serialize_by_status(s.by_status)},
        {"by_difficulty_solved", serialize_by_difficulty(s.by_difficulty_solved)},
    };
}

inline nlohmann::json serialize_user_meta(
        const litecode::stats_routes::detail::UserProfileRow& u) {
    return nlohmann::json{
        {"id",         u.id},
        {"username",   u.username},
        {"role",       u.role},
        {"created_at", u.created_at},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Leaderboard row serializer (Phase 6 ★ GET /api/v1/stats/ranking)
//
//  Mirrors the v1.2.31 front-end contract (`/ranking.html` already
//  has a normalizeRankItem() helper that's tolerant to multiple
//  field names — solved/solved_count, submissions/submission_count
//  — but the canonical wire shape we own here is unambiguous):
//
//      {
//        "rank":             1,
//        "user":  { "id": 42, "username": "alice", "role": "user" },
//        "solved_count":     25,
//        "submission_count": 80,
//        "acceptance_rate":  31.25
//      }
//
//  We deliberately OMIT `email` / `avatar` / `created_at` /
//  `last_login` / `last_login_ip` — none are needed for the
//  leaderboard row, and surfacing them is a confidentiality
//  regression (a /ranking scraper would otherwise get every
//  user's email in plain text). The /profile/:username endpoint
//  owns the "full user block" path.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_leaderboard_row(
        const litecode::stats_routes::detail::LeaderboardRow& r) {
    return nlohmann::json{
        {"rank",             0 /* filled in by handler */},
        {"user",             nlohmann::json{
                                  {"id",       r.user_id},
                                  {"username", r.username},
                                  {"role",     r.role},
                              }},
        {"solved_count",     r.solved_count},
        {"submission_count", r.submission_count},
        {"acceptance_rate",  r.acceptance_rate},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Query-string parser for GET /api/v1/stats/ranking
//
//  Recognized params:
//    - limit  (optional, default 100, range [1, kLeaderboardMaxLimit])
//    - offset (optional, default 0,  >= 0)
//
//  Anything else is ignored (future /ranking.html may add
//  ?difficulty=easy to filter — we just don't yet). Bad shape
//  returns false and writes a 400 envelope; success returns true
//  and populates `out`.
// ────────────────────────────────────────────────────────────────────────────

inline bool parse_ranking_query(const httplib::Request& req,
                                httplib::Response&     res,
                                int& limit, int& offset) {
    limit  = litecode::stats_routes::detail::kLeaderboardDefaultLimit;
    offset = 0;

    if (req.has_param("limit")) {
        const std::string raw = req.get_param_value("limit");
        // Mirror the parse_int_param() shape used in problem_routes.h
        // so the error envelope looks identical across endpoints.
        if (raw.empty()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
        try {
            std::size_t consumed = 0;
            const int v = std::stoi(raw, &consumed);
            if (consumed != raw.size() || v < 1) {
                send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                           "limit must be a positive integer",
                           {{"field", "limit"},
                            {"value", raw}});
                return false;
            }
            if (v > litecode::stats_routes::detail::kLeaderboardMaxLimit) {
                // Clamp, don't reject — same lenient policy as the
                // public problem list (a future caller shouldn't
                // have to know the cap; clamping is friendlier
                // and the cap is a server-side safety, not a
                // contract).
                limit = litecode::stats_routes::detail::kLeaderboardMaxLimit;
            } else {
                limit = v;
            }
        } catch (const std::exception&) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
    }
    if (req.has_param("offset")) {
        const std::string raw = req.get_param_value("offset");
        if (raw.empty()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
        try {
            std::size_t consumed = 0;
            const int v = std::stoi(raw, &consumed);
            if (consumed != raw.size() || v < 0) {
                send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                           "offset must be a non-negative integer",
                           {{"field", "offset"},
                            {"value", raw}});
                return false;
            }
            offset = v;
        } catch (const std::exception&) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/stats/ranking — Phase 6 ★
//  (SPEC §5.4, §11 Phase 6, F11)
//
//  Wire flow:
//    1) consume_rate_limit()             — 30/min/IP (stats.ranking bucket)
//                                           429 + Retry-After on deny
//    2) parse_ranking_query()            — limit + offset, 400 on bad shape
//    3) count_ranked_users()             — single COUNT for the `total` field
//    4) list_leaderboard(limit, offset)  — page of rows, throws → 500
//    5) serialize + send_success()       — 200 + {items, total, limit, offset}
//
//  Authorization: NONE. SPEC §5.4 row 2 reads "公开" (public).
//  No require_authentication() call. A bearer token (if present)
//  is ignored — the response shape is identical whether the caller
//  is logged in or not. The /ranking.html front-end uses
//  `litecode.api.auth` solely to highlight the "current user" row
//  with the `.lc-rank-row--me` class; the data itself never depends
//  on who you are.
//
//  Rate limit: 30/min/IP via stats_ranking_quota(rate_cfg). The
//  response carries X-RateLimit-* headers via consume_rate_limit().
//
//  Soft delete: ALWAYS applied to the problem JOIN. A user with
//  only ACs against tombstones does not appear in the ranking
//  (this mirrors the per-user stats semantics).
//
//  What we DON'T do here:
//    - We don't write to audit_logs. Public reads are not
//      auditable actions.
//    - We don't surface email / last_login / last_login_ip /
//      avatar. The /profile/:username endpoint owns that.
//    - We don't apply a `role` filter. Admin users appear in the
//      ranking if they have submissions — the front-end wants the
//      full community view.
// ────────────────────────────────────────────────────────────────────────────

inline void get_ranking_handler(
        httplib::Response&               res,
        const httplib::Request&          req,
        litecode::ConnectionPool&        pool,
        litecode::RateLimiter&           limiter,
        const litecode::RateLimitConfig& rate_cfg) {

    // 1) Rate limit. consume_rate_limit() throws ApiException(429,
    //    RATE_LIMITED) on deny (the wrap() in server.h catches it
    //    and emits the unified envelope, with Retry-After +
    //    X-RateLimit-* already stamped on res). On success it
    //    stamps the X-RateLimit-* headers and we keep going.
    consume_rate_limit(res, req, limiter,
                       litecode::stats_ranking_quota(rate_cfg));

    // 2) Parse query.
    int limit  = 0;
    int offset = 0;
    if (!parse_ranking_query(req, res, limit, offset)) {
        return;  // 400 already on the wire
    }

    // 3) Total — for the `total` field of the response. Done in a
    //    separate query so a 0-row result still gives the client the
    //    correct denominator. (We could fold this into the page
    //    query with a `SQL_CALC_FOUND_ROWS`-style trick, but that
    //    is deprecated in MySQL 8.x and adds a round-trip anyway.)
    int total = 0;
    try {
        total = stats_routes::detail::count_ranked_users(pool);
    } catch (const std::exception& e) {
        LOG_ERROR("stats_ranking: count_ranked_users threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 4) Page. Skip the SQL when total==0 (avoids a meaningless
    //    round-trip) and when offset >= total (out-of-range page
    //    returns empty items, total still says the real total —
    //    the front-end can detect "we walked off the end" and stop
    //    paginating).
    std::vector<stats_routes::detail::LeaderboardRow> rows;
    if (total > 0 && offset < total) {
        try {
            rows = stats_routes::detail::list_leaderboard(
                pool, limit, offset);
        } catch (const std::exception& e) {
            LOG_ERROR("stats_ranking: list_leaderboard threw",
                      {{"type",   typeid(e).name()},
                       {"reason", e.what()},
                       {"limit",  std::to_string(limit)},
                       {"offset", std::to_string(offset)}});
            send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                       std::string("internal error: ") + e.what());
            return;
        }
    }

    // 5) Serialize. rank = offset + page_index + 1 so it
    //    represents the absolute position (1-indexed) — the
    //    front-end's medal table (🥇🥈🥉) keys off this.
    nlohmann::json items = nlohmann::json::array();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        nlohmann::json item = serialize_leaderboard_row(rows[i]);
        item["rank"] =
            stats_routes::detail::rank_for_offset(offset,
                                                   static_cast<int>(i));
        items.push_back(std::move(item));
    }

    LOG_INFO("stats_ranking: served",
             {{"total",    std::to_string(total)},
              {"returned", std::to_string(items.size())},
              {"limit",    std::to_string(limit)},
              {"offset",   std::to_string(offset)}});

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  total},
        {"limit",  limit},
        {"offset", offset},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/stats/profile/:username — Phase 6 ★
//  (SPEC §5.4, §11 Phase 6, A15)
//
//  Wire flow:
//    1) require_authentication()         — 401 envelope on no / bad token
//    2) extract_username_from_path()     — 400 on bad path / bad shape
//    3) user_repo::find_by_username()    — nullopt → 404 envelope
//    4) compute_user_stats()             — 5 repo queries; throws → 500
//    5) serialize + send_success()       — 200 + {user, stats, request_id}
//
//  Authorization: requires a valid access token (SPEC §5.4 "已登录").
//  Non-admin and admin alike can view anyone else's profile — this
//  endpoint exposes only publicly-meaningful counters (no email,
//  no IP, no role-based gating). A future phase may add a
//  self-only toggle.
//
//  Rate limit: NONE today (SPEC §5.4 leaves the cell blank). The
//  handler therefore does NOT call consume_rate_limit() and the
//  response carries no X-RateLimit-* headers. If a quota lands on
//  this endpoint later, the line to add is:
//
//      consume_rate_limit(res, req, limiter,
//                         stats_profile_quota(rate_cfg));
//
//  and the test binary must change to assert X-RateLimit-* presence.
//
//  Why we don't paginate: the stats response is bounded (11 statuses
//  + 3 difficulties + a handful of scalar fields) and always small.
//  The "expensive" call is the 5 repo queries; pagination wouldn't
//  reduce that.
//
//  What we DON'T do here:
//    - We don't write to audit_logs. Stats reads are not auditable
//      actions (SPEC §11 Phase 6 audit_logs is for "key admin
//      operations" — read paths are not in that set).
//    - We don't expose the user's email / avatar / last_login /
//      last_login_ip. The v1.2.30 client-side aggregator didn't
//      have them either; the profile page reads them from a
//      separate `/auth/profile` call when needed.
//    - We don't sanitize the username in the response — it goes
//      through JSON serialization which already escapes `<` / `>`
//      / `&` / `"`. XSS safety is the front-end's job (the
//      v1.2.30 profile.html already escapes via textContent).
// ────────────────────────────────────────────────────────────────────────────

inline void get_user_profile_stats_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        litecode::ConnectionPool&      pool,
        const litecode::JwtConfig&     jwt_cfg) {

    // 1) Auth. Throws ApiException(401, UNAUTHORIZED) on failure;
    //    server.h's per-request wrap turns it into the unified
    //    envelope. Anti-enumeration: a missing / bad token gets the
    //    same wall as a bad token — we don't leak "this username is
    //    valid" by varying the 401 timing.
    const Claims claims = require_authentication(req, jwt_cfg);

    // 2) Path → username. 400 on bad shape (empty / too short / too
    //    long / disallowed chars). The handler maps std::nullopt to
    //    a 400 envelope with details.field=username; we deliberately
    //    don't echo the rejected raw value (it can be hostile).
    const auto username_opt =
        stats_routes::detail::extract_username_from_path(req);
    if (!username_opt.has_value()) {
        send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                   "username path component is invalid",
                   {{"field", "username"}});
        return;
    }
    const std::string username = *username_opt;

    // 3) Repo dispatch. find_user_for_stats() returns nullopt
    //    when no such user exists. The handler folds that into a 404
    //    envelope with details.username so the front-end can show a
    //    precise "user not found" message.
    std::optional<litecode::stats_routes::detail::UserProfileRow> user_row;
    try {
        user_row = litecode::stats_routes::detail::find_user_for_stats(
            pool, username);
    } catch (const std::exception& e) {
        LOG_ERROR("stats_profile: find_user_for_stats threw",
                  {{"username", username},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!user_row.has_value()) {
        LOG_INFO("stats_profile: user not found",
                 {{"username", username}});
        send_error(res, 404, litecode::ErrorCode::NOT_FOUND,
                   "user not found",
                   {{"username", username}});
        return;
    }

    // 4) Compute. Five repo queries; any failure bubbles up as
    //    std::runtime_error and the catch below folds it into 500.
    UserStats stats;
    try {
        stats = compute_user_stats(pool, user_row->id);
    } catch (const std::exception& e) {
        LOG_ERROR("stats_profile: compute_user_stats threw",
                  {{"user_id", std::to_string(user_row->id)},
                   {"type",    typeid(e).name()},
                   {"reason",  e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    LOG_INFO("stats_profile: served",
             {{"username",          username},
              {"user_id",           std::to_string(user_row->id)},
              {"total_submissions", std::to_string(stats.total_submissions)},
              {"solved_count",      std::to_string(stats.solved_count)},
              {"total_problems",    std::to_string(stats.total_problems)}});

    // 5) Serialize + send.
    send_success(res, nlohmann::json{
        {"user",  serialize_user_meta(*user_row)},
        {"stats", serialize_user_stats(stats)},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Returns HttpServer& so callers can chain. The JWT config is
//  captured by reference; the caller (main() / test fixture) owns
//  it. rate_cfg is captured by value (defends against a temporary
//  RateLimitConfig going out of scope - same defensive pattern as
//  problem_routes.h).
//
//  Production usage (from main.cpp):
//
//    litecode::HttpServer     server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//    litecode::RateLimiter    limiter;
//    litecode::register_stats_routes(
//        server, pool, limiter, cfg.rate_limit, cfg.jwt);
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter + a test JwtConfig. The shared rate_cfg
//  uses lax limits so test bodies can fire many requests without
//  hitting 429 (this endpoint doesn't consume a bucket today, so
//  rate_cfg is reserved for the future "add quota" commit).
//
//  ODR caveat (see header preamble): this header is not registered
//  from main.cpp for the same reason submission_routes.h /
//  problem_routes.h / admin_problem_routes.h / admin_bulk_import_routes.h
//  aren't. End-to-end coverage is owned by the test binary.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_stats_routes(HttpServer&              server,
                                        ConnectionPool&          pool,
                                        RateLimiter&             limiter,
                                        const RateLimitConfig&   rate_cfg,
                                        const JwtConfig&         jwt_cfg) {

    // GET /api/v1/stats/profile/:username — Phase 6 ★
    // (SPEC §5.4, A15). The regex captures anything that isn't a
    // '/'; the handler validates the captured username shape via
    // user_repo::validate_username so a hostile path component is a
    // 400 envelope, not a 500.
    server.get(R"(/api/v1/stats/profile/([^/]+))",
        [&pool, &jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                get_user_profile_stats_handler(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("stats_profile: handler threw",
                          {{"type",   typeid(e).name()},
                           {"reason", e.what()}});
                if (res.body.empty()) {
                    send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                               std::string("internal error: ") + e.what());
                } else {
                    throw;
                }
            }
        });

    // GET /api/v1/stats/ranking — Phase 6 ★
    // (SPEC §5.4, F11). Public, 30/min/IP. Query params: ?limit (default
    // 100, max 200), ?offset (default 0). The leaderboard is
    // hard-sorted by `solved_count DESC, submission_count ASC,
    // user_id ASC`; the `rank` field on each item is computed from
    // (offset + page_index + 1) so the front-end gets absolute
    // positions without a separate "compute rank" round-trip.
    //
    // We register the limiter / rate_cfg capture BY REFERENCE so the
    // server's lifetime extends over the test fixture's lifetime
    // (and `rate_cfg` BY VALUE to defend against a temporary going
    // out of scope — same defensive pattern as problem_routes.h).
    server.get("/api/v1/stats/ranking",
        [&pool, &limiter, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                get_ranking_handler(res, req, pool, limiter, rate_cfg);
            } catch (const ApiException&) {
                // Already an envelope — let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("stats_ranking: handler threw",
                          {{"type",   typeid(e).name()},
                           {"reason", e.what()}});
                if (res.body.empty()) {
                    send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                               std::string("internal error: ") + e.what());
                } else {
                    throw;
                }
            }
        });

    return server;
}

}  // namespace litecode