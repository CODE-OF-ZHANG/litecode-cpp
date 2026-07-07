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
// Failure modes:
//   - No / bad access token        → 401 UNAUTHORIZED
//   - Username path component has
//     bad shape (regex / length)   → 400 INVALID_INPUT with details.field=username
//   - User does not exist          → 404 NOT_FOUND with details.username
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

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "../config.h"                          // RateLimitConfig / JwtConfig
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/problem_repo.h"                 // problem_repo::count (live problems)
#include "../db/submission_repo.h"              // submission_repo::count
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
                                        RateLimiter&             /*limiter*/,
                                        const RateLimitConfig&   /*rate_cfg*/,
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

    return server;
}

}  // namespace litecode