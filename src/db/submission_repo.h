// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — submissions repository (Phase 4 ★)
//
// SPEC §4.4 / §4.5 / §5.3 / §11 Phase 4 / §12.2 / A6–A8 / A14 / A25 / A29–A30:
//   - `submissions` table schema (V001 + V005 + V006 + V008):
//       id, user_id, problem_id, language ENUM('c','cpp'), code LONGTEXT,
//       status ENUM('pending','running','ac','wa','re','tle','mle','ole','pe','ce','se')
//              NOT NULL DEFAULT 'pending',
//       time_used INT, memory_used INT, error_message TEXT,
//       created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
//       finished_at DATETIME NULL,
//       FK user_id    -> users(id)    ON DELETE CASCADE,
//       FK problem_id -> problems(id) ON DELETE CASCADE,
//       INDEX (user_id, problem_id, created_at DESC),
//       INDEX (problem_id, status, created_at),
//       INDEX (status, created_at),
//       INDEX (created_at),
//       INDEX (finished_at).
//
//   - The repo offers a Phase-4-shaped surface used by the submission
//     routes AND the judge_scheduler worker pool:
//       * SubmissionRow         — plain-data projection (mirrors §4.4)
//       * SubmissionListFilter  — list / count input
//       * SubmissionListResult  — paginated output
//       * create                — INSERT a new row with status='pending'
//                                 (called from POST /api/v1/submissions),
//                                 returns the new id (>0) or 0 on FK error
//       * find_by_id            — load one row (used by GET /:id)
//       * list / count          — paginated history (user_id × problem_id ×
//                                 include_unfinished filter)
//       * mark_running          — UPDATE status='running'; called by a
//                                 worker right before it picks up the
//                                 container from the warm pool (SPEC §7.1
//                                 step 2b). The `where status='pending'`
//                                 guard makes the transition atomic: a
//                                 crashed worker that already flipped to
//                                 'running' will not re-flip on restart
//                                 (a future recovery job can requeue).
//       * mark_finished         — UPDATE status, time_used, memory_used,
//                                 error_message, finished_at=NOW(); the
//                                 single-statement update avoids a torn
//                                 write where a polling client sees
//                                 finished_at set but status still
//                                 'running' (or vice versa). Called by
//                                 the worker on every terminal verdict.
//       * requeue_stuck_running — UPDATE status='pending' for rows
//                                 stuck in 'running' older than a
//                                 threshold. Reserved for a future
//                                 crash-recovery sweep; today the
//                                 scheduler handles a worker crash by
//                                 flipping `shutting_down_` and
//                                 trusting the per-row `started_at`
//                                 bookkeeping to recover.
//
//   - All writes use parameterized SQL (`?` placeholders) — SPEC §15.2
//     forbids string concatenation.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 2/3/4 module.
//   - Returned rows are exposed via a `SubmissionRow` struct (not the
//     raw mysqlx::Row) so callers don't depend on mysqlx::Value
//     semantics. `std::optional<SubmissionRow>` for "not found".
//   - Concurrency: every public method acquires a fresh
//     PooledConnection from the pool, runs the SQL, releases. The pool
//     is thread-safe; individual methods do not need their own locks.
//   - We deliberately do NOT expose `language` validation here — the
//     route layer normalizes it to "c" / "cpp" before calling create().
//   - Mark_finished() is the *only* writer that touches the terminal
//     fields (time/mem/error/finished_at). Every other status change
//     goes through mark_running() / requeue_stuck_running() / create().
//     Keeping the writer contract narrow makes it easy to audit the
//     "this row is in a terminal state" assertion downstream — it's
//     just `WHERE status IN ('ac','wa','re','tle','mle','ole','pe','ce','se')`
//     and the absence of any later update.
//
// Usage (POST /api/v1/submissions handler):
//   litecode::SubmissionRow s;
//   s.user_id    = claims.user_id;
//   s.problem_id = body.problem_id;
//   s.language   = body.language;
//   s.code       = body.code;
//   const int id = litecode::submission_repo::create(pool, s);
//   scheduler.enqueue({.submission_id = id, ...});
//
// Usage (worker callback after judge.sh returns):
//   litecode::submission_repo::mark_finished(pool, id,
//       result.status, result.time_used_ms, result.memory_used_kb,
//       result.error_message);

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"             // LOG_WARN (best-effort non-fatal DB hiccups)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Status constants
//
//  Mirror the MySQL ENUM exactly. Centralized so callers (route layer,
//  judge_scheduler, audit_logs) don't string-literal "running" all over
//  the codebase. The route layer's grep-ability hinges on every status
//  compare going through these symbols.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr const char* kStatusPending = "pending";
inline constexpr const char* kStatusRunning = "running";
inline constexpr const char* kStatusAC      = "ac";
inline constexpr const char* kStatusWA      = "wa";
inline constexpr const char* kStatusRE      = "re";
inline constexpr const char* kStatusTLE     = "tle";
inline constexpr const char* kStatusMLE     = "mle";
inline constexpr const char* kStatusOLE     = "ole";
inline constexpr const char* kStatusPE      = "pe";
inline constexpr const char* kStatusCE      = "ce";
inline constexpr const char* kStatusSE      = "se";

inline bool is_valid_status(std::string_view s) noexcept {
    return s == "pending" || s == "running" ||
           s == "ac" || s == "wa" || s == "re" || s == "tle" ||
           s == "mle" || s == "ole" || s == "pe" || s == "ce" ||
           s == "se";
}

inline bool is_terminal_status(std::string_view s) noexcept {
    // Anything except the two transient states. Single source of truth
    // so the worker pool / route layer / audit code all agree on
    // "this row is done".
    return s != "pending" && s != "running";
}

inline bool is_valid_language(std::string_view s) noexcept {
    return s == "c" || s == "cpp";
}

// ────────────────────────────────────────────────────────────────────────────
//  SubmissionRow
//
//  Plain-data projection of a row from the `submissions` table. All
//  string fields are std::string for safe lifetime — callers can copy,
//  move, or store the row across async boundaries.
//
//  Field semantics (mirrors SPEC §4.4):
//    - id: the row's primary key; 0 ⇒ "not yet inserted".
//    - user_id, problem_id: FK targets.
//    - language: "c" | "cpp".
//    - code: full source (LONGTEXT, ≤ 16 MB; the route layer clamps).
//    - status: one of the kStatus* constants.
//    - time_used: ms (NULL when not yet known → optional<>).
//    - memory_used: KB (NULL when not yet known → optional<>).
//    - error_message: NULL when no error → optional<>.
//    - created_at, finished_at: ISO-8601 strings (DATE_FORMAT'd in
//      every SELECT to dodge the mysql-connector 9.x packed-binary
//      datetime read bug, same pattern as problem_repo.h / user_repo.h).
//      finished_at is nullopt when the row hasn't finished yet
//      (status = pending / running) or finished_at = created_at when
//      finished in the same SQL statement.
// ────────────────────────────────────────────────────────────────────────────

struct SubmissionRow {
    int         id          = 0;
    int         user_id     = 0;
    int         problem_id  = 0;
    std::string language;
    std::string code;
    std::string status;       // kStatus* constants
    std::optional<int> time_used;     // ms
    std::optional<int> memory_used;   // KB
    std::optional<std::string> error_message;
    std::string created_at;
    std::optional<std::string> finished_at;
};

// ────────────────────────────────────────────────────────────────────────────
//  SubmissionListFilter — input to list() / count()
//
//  `user_id` / `problem_id` filters are independently optional so the
//  same shape serves:
//    - "my own submissions to problem X"  (user_id set, problem_id set)
//    - "all my submissions"               (user_id set)
//    - "submissions to problem X"         (problem_id set; admin only)
//    - "everything"                       (both nullopt; admin only)
//
//  `include_unfinished` controls whether pending/running rows appear.
//  Default true: a poll endpoint called by the judge client wants to see
//  its own pending row immediately. Admin tooling sets it false to scan
//  only terminal results.
//
//  `limit` / `offset` are clamped inside the repo (defense in depth)
//  so callers can't accidentally request unbounded pages.
// ────────────────────────────────────────────────────────────────────────────

struct SubmissionListFilter {
    std::optional<int> user_id;
    std::optional<int> problem_id;
    std::optional<std::string> status;            // one of kStatus* values
    bool               include_unfinished = true;
    int                limit              = 20;    // 1..100
    int                offset             = 0;     // >= 0
};

struct SubmissionListResult {
    std::vector<SubmissionRow> items;
    int                        total  = 0;
    int                        limit  = 0;
    int                        offset = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  SubmissionRepoError — typed exception surface
//
//  Two tiers, mirroring the rest of Phase 2/3/4:
//    - SubmissionRepoError        — generic failure (driver error, etc.)
//    - SubmissionNotFoundError    — UPDATE / requeue target not present;
//                                   caught by the route handler and
//                                   folded into 404.
//  Foreign-key violations on create() surface as a boolean return
//  (id == 0) rather than a typed exception, matching problem_repo's
//  shape so the route layer can compose "no such problem" cleanly.
// ────────────────────────────────────────────────────────────────────────────

class SubmissionRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SubmissionNotFoundError : public SubmissionRepoError {
public:
    using SubmissionRepoError::SubmissionRepoError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Validation helpers
//
//  Length / shape caps for the user-controllable fields. Mirrors
//  problem_repo / user_repo conventions: the route layer validates
//  first and these are defense in depth.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kMinCodeLength   = 1;
// 16 MB matches MEDIUMTEXT; we clamp just below so a 16 MB body
// doesn't go through and trip a "Data too long" error in the wire.
inline constexpr std::size_t kMaxCodeLength   = 15 * 1024 * 1024;
inline constexpr int         kSubmissionDefaultListLimit = 20;
inline constexpr int         kSubmissionMaxListLimit     = 100;

inline bool validate_code_length(std::size_t len,
                                 std::string* error_out = nullptr) {
    if (len < kMinCodeLength || len > kMaxCodeLength) {
        if (error_out) {
            *error_out = "code length must be between " +
                         std::to_string(kMinCodeLength) + " and " +
                         std::to_string(kMaxCodeLength) + " bytes";
        }
        return false;
    }
    return true;
}

inline void clamp_list_filter(SubmissionListFilter& f) {
    if (f.limit  <= 0)            f.limit  = kSubmissionDefaultListLimit;
    if (f.limit  > kSubmissionMaxListLimit) f.limit  = kSubmissionMaxListLimit;
    if (f.offset < 0)             f.offset = 0;
}

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
//
//  Centralized here so the find_* / list helpers stay small. Every
//  SELECT DATE_FORMATS the two DATETIME columns so a mysql-connector
//  9.x client gets a text value instead of packed binary (the same
//  bug user_repo.h / problem_repo.h document). BOOLEAN columns come
//  back as INT64; not used in this table but the helper is kept for
//  consistency.
// ────────────────────────────────────────────────────────────────────────────

namespace submission_repo {
namespace detail {

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw SubmissionRepoError(std::string("submission_repo: required field '") +
                                 field + "' is not a string: " + e.what());
    }
}

inline std::optional<std::string> opt_string(const mysqlx::Row& row,
                                             std::size_t idx) {
    try {
        const auto& v = row[idx];
        if (v.isNull()) return std::nullopt;
        return v.get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw SubmissionRepoError(std::string("submission_repo: required field '") +
                                 field + "' is not an int: " + e.what());
    }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    try {
        const auto& v = row[idx];
        if (v.isNull()) return std::nullopt;
        return static_cast<int>(v.get<std::int64_t>());
    } catch (...) {
        return std::nullopt;
    }
}

// Column order used by every SELECT in this file. Centralized so a
// schema change here is a one-liner.
//
//   0 id
//   1 user_id
//   2 problem_id
//   3 language
//   4 code
//   5 status
//   6 time_used     (NULL when not yet known)
//   7 memory_used   (NULL when not yet known)
//   8 error_message (NULL when no error)
//   9 created_at    (DATE_FORMAT'd → text)
//  10 finished_at   (DATE_FORMAT'd → text; NULL when not yet finished)
inline constexpr const char* kSubmissionSelectColumns =
    "id, user_id, problem_id, language, code, status, "
    "time_used, memory_used, error_message, "
    "DATE_FORMAT(created_at,  '%Y-%m-%d %H:%i:%s') AS created_at, "
    "DATE_FORMAT(finished_at, '%Y-%m-%d %H:%i:%s') AS finished_at";

inline SubmissionRow row_to_submission(const mysqlx::Row& row) {
    SubmissionRow s;
    s.id            = req_int     (row, 0,  "id");
    s.user_id       = req_int     (row, 1,  "user_id");
    s.problem_id    = req_int     (row, 2,  "problem_id");
    s.language      = req_string  (row, 3,  "language");
    s.code          = req_string  (row, 4,  "code");
    s.status        = req_string  (row, 5,  "status");
    s.time_used     = opt_int     (row, 6);
    s.memory_used   = opt_int     (row, 7);
    s.error_message = opt_string  (row, 8);
    s.created_at    = req_string  (row, 9,  "created_at");
    s.finished_at   = opt_string  (row, 10);
    return s;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

// create — INSERT a new submission row with status='pending'. The
// caller (POST /api/v1/submissions) has already validated that
// `user_id` / `problem_id` reference real rows; the FK is the
// authoritative gate, and we surface an FK failure as `id == 0` so
// the route handler can return 404 / 400 without a try/catch ladder.
//
// Returns:
//   - new submission id (> 0) on success
//   - 0 on FK violation (user_id or problem_id unknown). The route
//     handler maps 0 → 400 / 404.
//
// Throws SubmissionRepoError on driver / SQL errors (handler → 500).
//
// Field semantics mirror SPEC §4.4:
//   - user_id, problem_id: FK targets
//   - language: "c" | "cpp"
//   - code: source bytes (≤ 15 MB; the route layer clamps)
//   - status: NOT touched on insert — the column DEFAULT 'pending'
//             handles the initial value
//
// We deliberately do NOT pre-check user_id / problem_id existence
// before INSERT; the FK constraint is the authoritative gate and the
// INSERT-then-check pattern is one round-trip.
inline int create(ConnectionPool& pool, const SubmissionRow& row) {
    // Defense-in-depth validation on the cheap fields.
    if (!is_valid_language(row.language)) {
        throw SubmissionRepoError(
            std::string("create: language must be 'c' or 'cpp', got '") +
            row.language + "'");
    }
    {
        std::string err;
        if (!validate_code_length(row.code.size(), &err)) {
            throw SubmissionRepoError("create: " + err);
        }
    }
    if (row.user_id    <= 0) throw SubmissionRepoError("create: user_id must be > 0");
    if (row.problem_id <= 0) throw SubmissionRepoError("create: problem_id must be > 0");

    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "INSERT INTO submissions "
            "(user_id, problem_id, language, code) "
            "VALUES (?, ?, ?, ?)",
            row.user_id,
            row.problem_id,
            row.language,
            row.code);
        return static_cast<int>(rs.getAutoIncrementValue());
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        // MySQL surfaces an FK violation as errno 1452 ("Cannot add or
        // update a child row: a foreign key constraint fails"). We match
        // on the message text rather than a version-specific symbol so
        // the code keeps working across connector versions.
        if (what.find("foreign key") != std::string::npos ||
            what.find("FOREIGN KEY") != std::string::npos ||
            what.find("1452")        != std::string::npos) {
            return 0;
        }
        throw SubmissionRepoError(
            std::string("submission_repo::create: ") + what);
    }
}

// find_by_id — load a full row by primary key. Returns std::nullopt
// when no such row exists. The route handler maps that to 404.
inline std::optional<SubmissionRow> find_by_id(ConnectionPool& pool,
                                              int id) {
    auto conn = pool.acquire();
    try {
        auto row = conn.fetch_one(
            std::string("SELECT ") + detail::kSubmissionSelectColumns +
            " FROM submissions WHERE id = ? LIMIT 1",
            id);
        if (!row) return std::nullopt;
        return detail::row_to_submission(*row);
    } catch (const mysqlx::Error& e) {
        throw SubmissionRepoError(
            std::string("submission_repo::find_by_id: ") + e.what());
    }
}

// count — total rows matching `filter`. Used by list() to fill the
// `total` field on the result.
inline int count(ConnectionPool& pool, const SubmissionListFilter& filter) {
    SubmissionListFilter f = filter;
    clamp_list_filter(f);

    std::string sql = "SELECT COUNT(*) FROM submissions WHERE 1 = 1";
    if (f.user_id.has_value())    sql += " AND user_id = ?";
    if (f.problem_id.has_value()) sql += " AND problem_id = ?";
    if (f.status.has_value())     sql += " AND status = ?";
    if (!f.include_unfinished) {
        sql += " AND status NOT IN ('pending','running')";
    }

    auto conn = pool.acquire();
    try {
        // Dispatch on the (user_id × problem_id × status) shape to
        // keep the bind() argument count exactly right — mysqlx
        // requires positional binding to match the placeholder count.
        const bool has_u = f.user_id.has_value();
        const bool has_p = f.problem_id.has_value();
        const bool has_s = f.status.has_value();
        if (has_u && has_p && has_s) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.user_id, *f.problem_id, *f.status).value_or(0));
        } else if (has_u && has_p) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.user_id, *f.problem_id).value_or(0));
        } else if (has_u && has_s) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.user_id, *f.status).value_or(0));
        } else if (has_p && has_s) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.problem_id, *f.status).value_or(0));
        } else if (has_u) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.user_id).value_or(0));
        } else if (has_p) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.problem_id).value_or(0));
        } else if (has_s) {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql, *f.status).value_or(0));
        } else {
            return static_cast<int>(conn.fetch_scalar<std::int64_t>(
                sql).value_or(0));
        }
    } catch (const mysqlx::Error& e) {
        throw SubmissionRepoError(
            std::string("submission_repo::count: ") + e.what());
    }
}

// list — paginated, filterable page of submissions. Returns
// {items, total, limit, offset}. `total` is the unpaginated count for
// the same filter so the front-end can render pagination UI without a
// second round-trip.
//
// Ordering is fixed: created_at DESC, id DESC. SPEC §4.5 explicitly
// lists `(user_id, problem_id, created_at DESC)` as the index for
// "个人提交历史" — the DESC order matches the "newest first" front-end
// expectation. The status/created_at index handles the admin
// "queue monitor" view, which lists pending/running first; that lives
// in the admin queue endpoint (v1.2.15) and is not this function.
inline SubmissionListResult list(ConnectionPool& pool,
                                 const SubmissionListFilter& filter) {
    SubmissionListFilter f = filter;
    clamp_list_filter(f);

    SubmissionListResult out;
    out.limit  = f.limit;
    out.offset = f.offset;
    out.total  = count(pool, f);

    std::string sql = std::string("SELECT ") +
                      detail::kSubmissionSelectColumns +
                      " FROM submissions WHERE 1 = 1";
    if (f.user_id.has_value())    sql += " AND user_id = ?";
    if (f.problem_id.has_value()) sql += " AND problem_id = ?";
    if (f.status.has_value())     sql += " AND status = ?";
    if (!f.include_unfinished) {
        sql += " AND status NOT IN ('pending','running')";
    }
    sql += " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?";

    auto conn = pool.acquire();
    try {
        const bool has_u = f.user_id.has_value();
        const bool has_p = f.problem_id.has_value();
        const bool has_s = f.status.has_value();
        mysqlx::SqlResult rs = ([&]() {
            if (has_u && has_p && has_s) {
                return conn.execute(sql, *f.user_id, *f.problem_id,
                                    *f.status, f.limit, f.offset);
            } else if (has_u && has_p) {
                return conn.execute(sql, *f.user_id, *f.problem_id,
                                    f.limit, f.offset);
            } else if (has_u && has_s) {
                return conn.execute(sql, *f.user_id, *f.status,
                                    f.limit, f.offset);
            } else if (has_p && has_s) {
                return conn.execute(sql, *f.problem_id, *f.status,
                                    f.limit, f.offset);
            } else if (has_u) {
                return conn.execute(sql, *f.user_id, f.limit, f.offset);
            } else if (has_p) {
                return conn.execute(sql, *f.problem_id, f.limit, f.offset);
            } else if (has_s) {
                return conn.execute(sql, *f.status, f.limit, f.offset);
            } else {
                return conn.execute(sql, f.limit, f.offset);
            }
        })();

        out.items.reserve(static_cast<std::size_t>(f.limit));
        for (auto row : rs) {
            out.items.push_back(detail::row_to_submission(row));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw SubmissionRepoError(
            std::string("submission_repo::list: ") + e.what());
    }
}

// mark_running — flip status='running'. Called by a worker right
// before it acquires a container from the warm pool. The
// `WHERE status='pending'` guard makes the transition atomic so a
// crashed worker that already flipped to 'running' will not be
// re-flipped by a future recovery job (or by an out-of-order replay).
//
// Returns true when the row was actually transitioned (a worker
// succeeded in claiming it); false when the row was already running
// (claimed by another worker) or terminal (somebody already finished
// it). The scheduler treats false as "drop this task silently".
inline bool mark_running(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "UPDATE submissions SET status = 'running' "
            "WHERE id = ? AND status = 'pending'",
            id);
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        throw SubmissionRepoError(
            std::string("submission_repo::mark_running: ") + e.what());
    }
}

// mark_finished — UPDATE status + time_used + memory_used +
// error_message + finished_at=NOW() in a single statement. The
// single-statement update avoids a torn write where a polling client
// sees finished_at set but status still 'running' (or vice versa).
//
// `status` MUST be a terminal status (see is_terminal_status). The
// repo does not validate this — the scheduler is the only writer and
// it always uses a terminal string — but a future caller that forgets
// would have a confusing row to debug. The throw guards against that.
//
// `time_used_ms` / `memory_used_kb` may be 0 (the schema is INT NULL;
// we pass NULL when the judge did not surface a usable measurement,
// e.g. CE before compilation). The std::optional<> carry lets the
// caller distinguish "measured zero" from "not measured" if it needs
// to — though the API surface here keeps the simpler int+has_value
// semantics.
//
// Throws SubmissionRepoError on driver error.
//
// Returns true when the row was actually updated; false when the row
// was already in a terminal status (a defensive double-write guard).
inline bool mark_finished(ConnectionPool& pool,
                          int id,
                          const std::string& status,
                          std::optional<int> time_used_ms,
                          std::optional<int> memory_used_kb,
                          const std::string& error_message) {
    if (!is_valid_status(status)) {
        throw SubmissionRepoError(
            std::string("mark_finished: invalid status '") + status + "'");
    }
    if (!is_terminal_status(status)) {
        // Defensive: refusing non-terminal statuses here makes "where
        // did this row end up at finished_at = NOW()?" impossible to
        // answer wrong.
        throw SubmissionRepoError(
            std::string("mark_finished: status '") + status +
            "' is not terminal");
    }

    auto conn = pool.acquire();
    try {
        // mysqlx has no implicit std::optional -> NULL bind, so we lift
        // the two nullable int columns into mysqlx::Value explicitly.
        // mirror the user_repo::create_user pattern (see user_repo.h).
        const mysqlx::Value time_val = time_used_ms.has_value()
            ? mysqlx::Value(static_cast<std::int64_t>(*time_used_ms))
            : mysqlx::Value(nullptr);
        const mysqlx::Value mem_val  = memory_used_kb.has_value()
            ? mysqlx::Value(static_cast<std::int64_t>(*memory_used_kb))
            : mysqlx::Value(nullptr);

        // Two UPDATE shapes (with / without error_message) so a NULL
        // error_message lands as SQL NULL, not "".
        if (error_message.empty()) {
            auto rs = conn.execute(
                "UPDATE submissions "
                "SET status = ?, time_used = ?, memory_used = ?, "
                "    error_message = NULL, finished_at = NOW() "
                "WHERE id = ? AND status IN ('pending','running')",
                status, time_val, mem_val, id);
            return rs.getAffectedItemsCount() > 0;
        } else {
            auto rs = conn.execute(
                "UPDATE submissions "
                "SET status = ?, time_used = ?, memory_used = ?, "
                "    error_message = ?, finished_at = NOW() "
                "WHERE id = ? AND status IN ('pending','running')",
                status, time_val, mem_val, error_message, id);
            return rs.getAffectedItemsCount() > 0;
        }
    } catch (const mysqlx::Error& e) {
        throw SubmissionRepoError(
            std::string("submission_repo::mark_finished: ") + e.what());
    }
}

// requeue_stuck_running — flip status='pending' for rows stuck in
// 'running' longer than `stuck_after_seconds`. Reserved for a future
// crash-recovery sweep (Phase 5 / Phase 9); today's scheduler handles
// a worker crash by flipping `shutting_down_` and trusting the
// per-row bookkeeping to recover. Kept here so the operator can run
// a one-off UPDATE without writing SQL by hand.
//
// Returns the number of rows requeued. Throws SubmissionRepoError on
// driver error.
inline int requeue_stuck_running(ConnectionPool& pool,
                                 int stuck_after_seconds) {
    if (stuck_after_seconds < 1) {
        throw SubmissionRepoError(
            "requeue_stuck_running: stuck_after_seconds must be >= 1");
    }
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "UPDATE submissions SET status = 'pending' "
            "WHERE status = 'running' "
            "  AND created_at < (NOW() - INTERVAL ? SECOND)",
            stuck_after_seconds);
        return static_cast<int>(rs.getAffectedItemsCount());
    } catch (const mysqlx::Error& e) {
        throw SubmissionRepoError(
            std::string("submission_repo::requeue_stuck_running: ") +
            e.what());
    }
}

}  // namespace submission_repo
}  // namespace litecode