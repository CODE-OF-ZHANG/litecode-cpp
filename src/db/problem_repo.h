// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — problems repository (Phase 3 ★)
//
// SPEC §4.2 / §4.5 / §5.2 / §11 Phase 3 / §15 / A4, A18, A19, A20 acceptance:
//   - `problems` table schema (post-V004):
//       id, slug UNIQUE, title, difficulty ENUM('easy','medium','hard'),
//       description MEDIUMTEXT, time_limit (ms), memory_limit (MB),
//       accepted_count, submission_count, is_deleted (BOOLEAN),
//       created_at, updated_at
//   - The repo offers a Phase-3-shaped surface that the public list /
//     detail endpoints and the admin CRUD endpoints will all share:
//       * create            — INSERT a new row, return the new id (0 on
//                              duplicate slug, mapped to 409 by the
//                              route handler)
//       * find_by_id        — fetch one row (admin paths include
//                              is_deleted; public paths filter it out)
//       * find_by_slug      — same idea, but keyed by the URL slug
//       * slug_exists       — boolean pre-check, matches find_by_slug
//       * update            — PUT path; touches every mutable column
//       * soft_delete       — DELETE path; UPDATE is_deleted = TRUE
//       * restore           — admin recovery; UPDATE is_deleted = FALSE
//       * count             — total row count for a filter (pagination
//                              total); respects include_deleted flag
//       * list              — paginated, filterable list (difficulty,
//                              include_deleted, limit/offset) — used by
//                              GET /api/v1/problems
//   - All writes use parameterized SQL (`?` placeholders) — SPEC §15.2
//     forbids string concatenation. mysqlx::SqlStatement::bind() handles
//     the binding; user-supplied data never reaches the wire string.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3 module
//     (config.h / logger.h / server.h / user_repo.h / audit_log_repo.h).
//     The repo is essentially a thin set of free functions over a
//     ConnectionPool reference; no internal state to unit-test.
//   - Returned rows are exposed via a `ProblemRow` struct (not the raw
//     mysqlx::Row) so callers don't depend on mysqlx::Value's semantics.
//     `std::optional<ProblemRow>` for "not found".
//   - We deliberately do NOT model an ORM-style change-tracking
//     `Problem` object. Hand-written SQL with bind() is fine for our
//     surface and avoids the C++ ORM overhead (SPEC §9 calls this out).
//   - The list filter is intentionally a small struct instead of a
//     builder API. Each field is independently `std::optional` so the
//     route handler can translate a JSON query string into a filter
//     without touching SQL.
//   - The `tag_id` filter lives here even though `tags` is a separate
//     table — it's a list-filter concern (one of the "筛选" SPEC §2.1
//     axes) and lets us avoid joining tags in every row materialization.
//     The full M:N resolution belongs to tag_repo.h (next Phase 3 step).
//   - Concurrency: every public method acquires a fresh PooledConnection
//     from the pool, runs the SQL, releases. The pool is thread-safe;
//     individual methods do not need their own locks.
//
// Usage (public list, future problem_routes.h):
//
//   litecode::ProblemListFilter f;
//   f.limit  = 20;
//   f.offset = 0;
//   // difficulty / tag_id left nullopt ⇒ no filter
//   const auto page = litecode::problem_repo::list(pool, f);
//   // page.items is std::vector<ProblemRow>; page.total is the COUNT(*)
//
// Usage (admin create, future problem_routes.h):
//
//   litecode::ProblemRow p;
//   p.slug        = "two-sum";
//   p.title       = "两数之和";
//   p.difficulty  = "easy";
//   p.description = "# ...";
//
//   const int new_id = litecode::problem_repo::create(pool, p);
//   if (new_id == 0) {
//       // slug collision — 409
//   }
//
// Usage (admin soft-delete):
//
//   if (!litecode::problem_repo::soft_delete(pool, "two-sum")) {
//       // slug not found — 404
//   }

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
//  ProblemRow
//
//  Plain-data projection of a row from the `problems` table. All string
//  fields are std::string for safe lifetime — callers can copy, move,
//  or store the row across async boundaries.
//
//  Field semantics (mirrors SPEC §4.2 + V004 + V012):
//    - id: the row's primary key; 0 ⇒ "not yet inserted"
//    - slug: required, never empty once loaded, UNIQUE in the DB
//    - title: required, never empty
//    - difficulty: "easy" | "medium" | "hard" (matches the MySQL ENUM)
//    - description: Markdown body (NOT NULL, MEDIUMTEXT in the DB)
//    - template_ : v1.3.2 — per-problem code template (MEDIUMTEXT NULL).
//                  problem.html injects this into the CodeMirror editor on
//                  load; NULL/empty ⇒ fallback to the built-in C++/C
//                  skeleton in web/js/editor.js. Routed through admin
//                  POST/PUT and admin bulk-import as an optional field.
//    - time_limit: ms; SPEC §2.2 default 1000
//    - memory_limit: MB; SPEC §2.2 default 256
//    - accepted_count, submission_count: maintenance counters; updated
//      by the (future) judge flow. Read by Phase 5 list cards; not
//      authoritative for ranking — SPEC §4.2 spells that out.
//    - is_deleted: true when the row has been soft-deleted (SPEC §4.2).
//      Public read paths filter this out at the SQL level.
//    - created_at, updated_at: ISO-8601 strings. Kept as text for
//      forward-compat with JSON serialization; DATE_FORMAT()'d in
//      the SELECT for the same mysql-connector 9.x reason spelled
//      out in user_repo.h.
// ────────────────────────────────────────────────────────────────────────────

struct ProblemRow {
    int                 id              = 0;
    std::string         slug;
    std::string         title;
    std::string         difficulty;         // "easy" | "medium" | "hard"
    std::string         description;       // MEDIUMTEXT (Markdown)
    std::string         template_;         // v1.3.2: MEDIUMTEXT NULL (per-problem code template)
                                           // C++ reserved-word shadow — the trailing underscore is
                                           // intentional and matches the project's C++ style.
    int                 time_limit     = 1000;
    int                 memory_limit   = 256;
    int                 accepted_count = 0;
    int                 submission_count = 0;
    bool                is_deleted     = false;
    std::string         created_at;
    std::string         updated_at;
};

// ────────────────────────────────────────────────────────────────────────────
//  ProblemListFilter — input to list() / count()
//
//  All filter fields are independently optional so the route handler
//  can translate a JSON query string into a filter without touching
//  SQL. `include_deleted` is the only "boolean, not optional" field:
//  the public list endpoint wants the strict default (false), the
//  admin list endpoint wants the lenient default (true). The handler
//  sets it explicitly so we never silently leak soft-deleted rows.
//
//  `tag_id` is part of the filter even though `tags` lives in its own
//  table — it's a public-list filter axis (SPEC §5.2 "按难度/标签筛选")
//  and we implement it via an EXISTS subquery against problem_tags so
//  the result row materialization stays a flat projection.
// ────────────────────────────────────────────────────────────────────────────

struct ProblemListFilter {
    bool                    include_deleted = false;
    std::optional<std::string> difficulty;     // "easy" | "medium" | "hard"
    std::optional<int>      tag_id;
    int                     limit           = 20;   // 1..100
    int                     offset          = 0;    // >= 0
};

// ────────────────────────────────────────────────────────────────────────────
//  ProblemListResult — output of list()
//
//  `total` is the COUNT(*) over the same filter, with the same
//  include_deleted / difficulty / tag_id predicates applied, so the
//  front-end can render "page 3 of 12" without a second round-trip.
//  `items` is the page of rows for the requested offset/limit.
// ────────────────────────────────────────────────────────────────────────────

struct ProblemListResult {
    std::vector<ProblemRow> items;
    int                     total  = 0;
    int                     limit  = 0;
    int                     offset = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  ProblemRepoError — surface every repo-layer failure as a typed exception
//
//  Three tiers, mirroring the rest of Phase 2/3:
//    - ProblemRepoError           — generic failure (driver error, etc.)
//    - ProblemNotFoundError       — UPDATE / soft_delete / restore target
//                                   not present; caught by the route
//                                   handler and folded into 404.
//    - ProblemAlreadyExistsError  — slug uniqueness collision on create
//                                   (via 0 return) or on update (rename
//                                   into an existing slug); caught by
//                                   the route handler and folded into
//                                   409.
//
//  We deliberately do NOT surface duplicate-detection via a magic
//  error code from create(); the boolean returns are simpler and let
//  the handler compose "slug taken" cleanly. NotFound, by contrast,
//  is rare on the write path and worth a typed exception because the
//  handler maps it directly to a 404 envelope.
// ────────────────────────────────────────────────────────────────────────────

class ProblemRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ProblemNotFoundError : public ProblemRepoError {
public:
    using ProblemRepoError::ProblemRepoError;
};

class ProblemAlreadyExistsError : public ProblemRepoError {
public:
    using ProblemRepoError::ProblemRepoError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Validation helpers (used by both repo + route layer)
//
//  These match SPEC §4.2 column types and §2.2 default limits:
//    - slug: 1..100 chars, lowercase ASCII letters / digits / hyphen,
//            not starting or ending with a hyphen (the latter would
//            produce awkward URLs and is forbidden by the spec's URL
//            identifier convention).
//    - title: 1..200 chars (DB column is VARCHAR(200)); we accept any
//            non-NUL byte — the title is rendered with Markdown
//            escaping in the front-end, so charset is intentionally
//            permissive.
//    - difficulty: exactly one of "easy" / "medium" / "hard".
//    - time_limit: 1..60_000 ms (1ms..60s); a problem with
//            time_limit <= 0 is nonsense and we reject it.
//    - memory_limit: 1..1024 MB (1MB..1GB); the upper bound matches
//            typical container memory caps and keeps the judge within
//            sane hardware limits.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kMinSlugLength      = 1;
inline constexpr std::size_t kMaxSlugLength      = 100;
inline constexpr std::size_t kMinTitleLength     = 1;
inline constexpr std::size_t kMaxTitleLength     = 200;
inline constexpr int         kMinTimeLimitMs     = 1;
inline constexpr int         kMaxTimeLimitMs     = 60'000;
inline constexpr int         kMinMemoryLimitMb   = 1;
inline constexpr int         kMaxMemoryLimitMb   = 1024;
inline constexpr int         kDefaultListLimit   = 20;
inline constexpr int         kMaxListLimit       = 100;

inline bool is_valid_slug_char(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return (uc >= 'a' && uc <= 'z') ||
           (uc >= '0' && uc <= '9') ||
           uc == '-';
}

inline bool validate_slug(std::string_view slug,
                          std::string* error_out = nullptr) {
    if (slug.size() < kMinSlugLength || slug.size() > kMaxSlugLength) {
        if (error_out) {
            *error_out = "slug length must be between " +
                         std::to_string(kMinSlugLength) + " and " +
                         std::to_string(kMaxSlugLength);
        }
        return false;
    }
    if (slug.front() == '-' || slug.back() == '-') {
        if (error_out) {
            *error_out = "slug must not start or end with '-'";
        }
        return false;
    }
    for (char c : slug) {
        if (!is_valid_slug_char(c)) {
            if (error_out) {
                *error_out = "slug may only contain lowercase letters, digits, and '-'";
            }
            return false;
        }
    }
    return true;
}

inline bool is_valid_difficulty(std::string_view d) noexcept {
    return d == "easy" || d == "medium" || d == "hard";
}

inline bool validate_difficulty(std::string_view difficulty,
                                std::string* error_out = nullptr) {
    if (!is_valid_difficulty(difficulty)) {
        if (error_out) {
            *error_out = "difficulty must be one of: easy, medium, hard";
        }
        return false;
    }
    return true;
}

inline bool validate_title(std::string_view title,
                           std::string* error_out = nullptr) {
    if (title.size() < kMinTitleLength || title.size() > kMaxTitleLength) {
        if (error_out) {
            *error_out = "title length must be between " +
                         std::to_string(kMinTitleLength) + " and " +
                         std::to_string(kMaxTitleLength);
        }
        return false;
    }
    return true;
}

inline bool validate_time_limit(int ms, std::string* error_out = nullptr) {
    if (ms < kMinTimeLimitMs || ms > kMaxTimeLimitMs) {
        if (error_out) {
            *error_out = "time_limit must be between " +
                         std::to_string(kMinTimeLimitMs) + " and " +
                         std::to_string(kMaxTimeLimitMs) + " ms";
        }
        return false;
    }
    return true;
}

inline bool validate_memory_limit(int mb, std::string* error_out = nullptr) {
    if (mb < kMinMemoryLimitMb || mb > kMaxMemoryLimitMb) {
        if (error_out) {
            *error_out = "memory_limit must be between " +
                         std::to_string(kMinMemoryLimitMb) + " and " +
                         std::to_string(kMaxMemoryLimitMb) + " MB";
        }
        return false;
    }
    return true;
}

// Clamp the pagination fields to safe ranges. We do this in the repo
// (not just the handler) so any caller — including a future bulk
// import path — gets sane behavior by default.
inline void clamp_list_filter(ProblemListFilter& f) {
    if (f.limit  <= 0)              f.limit  = kDefaultListLimit;
    if (f.limit  > kMaxListLimit)   f.limit  = kMaxListLimit;
    if (f.offset < 0)               f.offset = 0;
}

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
//
//  Reads from a mysqlx::Row into a ProblemRow. Centralized here so the
//  find_* / list helpers stay small. NULL columns come back as
//  mysqlx::Value{} (isNull()==true); everything else is read as
//  std::string via get<std::string>() which handles VARCHAR / TEXT /
//  MEDIUMTEXT uniformly. BOOLEAN columns come back as INT64 (0/1) and
//  are cast to bool explicitly.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw ProblemRepoError(std::string("problem_repo: required field '") +
                               field + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw ProblemRepoError(std::string("problem_repo: required field '") +
                               field + "' is not an int: " + e.what());
    }
}

inline bool req_bool(const mysqlx::Row& row, std::size_t idx,
                     const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>()) != 0;
    } catch (const std::exception& e) {
        throw ProblemRepoError(std::string("problem_repo: required field '") +
                               field + "' is not a bool: " + e.what());
    }
}

// opt_string — read a possibly-NULL string column as std::optional<std::string>.
// Returns std::nullopt when the cell is SQL NULL; throws ProblemRepoError
// on non-string cells (mirrors req_string's contract but for nullable
// columns — typically MEDIUMTEXT NULL). Used by v1.3.2's ProblemRow.template_;
// raw `row[idx].get<std::string>()` throws on NULL, which would mask our
// "fall back to editor skeleton" contract on the public detail endpoint.
inline std::optional<std::string> opt_string(const mysqlx::Row& row,
                                              std::size_t idx,
                                              const char* field) {
    try {
        const auto& v = row[idx];
        if (v.isNull()) return std::nullopt;
        return v.get<std::string>();
    } catch (const std::exception& e) {
        throw ProblemRepoError(std::string("problem_repo: optional field '") +
                               field + "' is not a string: " + e.what());
    }
}

// Column order, used by every SELECT in this file. Centralized so a
// schema change here is a one-liner.
//
//   0  id
//   1  slug
//   2  title
//   3  difficulty
//   4  description
//   5  template          ← v1.3.2: per-problem code template (NULL allowed MEDIUMTEXT)
//   6  time_limit        ← was 5 before v1.3.2 (shifted by +1 after template)
//   7  memory_limit      ← was 6
//   8  accepted_count    ← was 7   ← v1.3.5: 从 problems 表的冗余列
//                                       改为相关子查询实时从 submissions
//                                       聚合,消除"judge 流程未维护
//                                       problems.accepted_count 导致
//                                       problems_list.html 永远 0/0"的
//                                       根因(详见 commit log)。
//                                       V005 idx_submissions_problem_status
//                                       索引覆盖,O(log N) per row。
//   9  submission_count  ← was 8   ← 同上,实时聚合
//  10  is_deleted        ← was 9
//  11  created_at        (DATE_FORMAT'd → text)
//  12  updated_at        (DATE_FORMAT'd → text)
inline constexpr const char* kProblemSelectColumns =
    "id, slug, title, difficulty, description, template, time_limit, memory_limit, "
    // v1.3.5: 实时聚合,COALESCE 保证 NULL→0(LEFT JOIN / subquery 边界)
    "(SELECT COUNT(*) FROM submissions s "
    " WHERE s.problem_id = problems.id AND s.status = 'ac') AS accepted_count, "
    "(SELECT COUNT(*) FROM submissions s "
    " WHERE s.problem_id = problems.id "
    "   AND s.status NOT IN ('pending','running')) AS submission_count, "
    "is_deleted, "
    "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at, "
    "DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s') AS updated_at";

inline ProblemRow row_to_problem(const mysqlx::Row& row) {
    ProblemRow p;
    p.id              = req_int   (row, 0,  "id");
    p.slug            = req_string(row, 1,  "slug");
    p.title           = req_string(row, 2,  "title");
    p.difficulty      = req_string(row, 3,  "difficulty");
    p.description     = req_string(row, 4,  "description");
    {
        // v1.3.2: nullable MEDIUMTEXT — opt_string returns std::nullopt on SQL
        // NULL; we coalesce to empty so the editor skeleton fallback runs.
        auto opt_t = detail::opt_string(row, 5, "template");
        p.template_ = opt_t.value_or(std::string());
    }
    p.time_limit      = req_int   (row, 6,  "time_limit");
    p.memory_limit    = req_int   (row, 7,  "memory_limit");
    p.accepted_count  = req_int   (row, 8,  "accepted_count");
    p.submission_count= req_int   (row, 9,  "submission_count");
    p.is_deleted      = req_bool  (row, 10, "is_deleted");
    p.created_at      = req_string(row, 11, "created_at");
    p.updated_at      = req_string(row, 12, "updated_at");
    return p;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace problem_repo {

// create — INSERT a new problem row.
//
// Returns:
//   - new problem id (>0) on success
//   - 0 on slug uniqueness collision (no exception thrown; the route
//     handler maps 0 → 409 CONFLICT)
//
// Throws ProblemRepoError on driver / SQL errors (handler → 500).
// Throws ProblemRepoError (via validate_*) on invalid field values —
// the caller is expected to have validated first, but we re-validate
// the cheap ones as a defense in depth.
//
// Notes:
//   - The INSERT lists every column explicitly so we never accidentally
//     write NULL into a NOT NULL column and so a future schema addition
//     doesn't get a surprise default.
//   - time_limit / memory_limit fall back to the SPEC §2.2 defaults
//     when the caller leaves them at 0 (matching the DB DEFAULT).
//   - accepted_count / submission_count default to 0 — the judge flow
//     maintains them later.
//   - We deliberately do NOT pre-check slug_exists() before INSERT;
//     the UNIQUE constraint on `slug` is the authoritative gate, and
//     the INSERT-then-check pattern is one round-trip. The race
//     window between "I checked, it's free" and "I inserted" is closed
//     by the UNIQUE constraint at INSERT time.
inline int create(ConnectionPool& pool, const ProblemRow& row) {
    // Honor the SPEC defaults when the caller didn't override them.
    // We do this BEFORE validation so a caller who explicitly set
    // 0 (the "use default" sentinel) gets the default applied without
    // tripping the lower-bound check. Only 0 is the sentinel — any
    // negative or out-of-range value is "garbage in" and will be
    // rejected by the validator below.
    const int time_limit   = row.time_limit   == 0 ? 1000 : row.time_limit;
    const int memory_limit = row.memory_limit == 0 ? 256  : row.memory_limit;

    // Defense-in-depth validation on the EFFECTIVE values. The handler
    // is expected to have validated first; the goal here is "garbage
    // in ⇒ exception, never a corrupt row".
    {
        std::string err;
        if (!validate_slug      (row.slug,        &err)) throw ProblemRepoError("create: " + err);
        if (!validate_title     (row.title,       &err)) throw ProblemRepoError("create: " + err);
        if (!validate_difficulty(row.difficulty,  &err)) throw ProblemRepoError("create: " + err);
        if (!validate_time_limit(time_limit,     &err)) throw ProblemRepoError("create: " + err);
        if (!validate_memory_limit(memory_limit, &err)) throw ProblemRepoError("create: " + err);
    }

    auto conn = pool.acquire();

    try {
        auto rs = conn.execute(
            "INSERT INTO problems "
            "(slug, title, difficulty, description, template, time_limit, memory_limit) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            row.slug,
            row.title,
            row.difficulty,
            row.description,
            row.template_,          // v1.3.2: MEDIUMTEXT NULL — empty string is fine
            time_limit,
            memory_limit);
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
        throw ProblemRepoError(std::string("problem_repo::create: ") + what);
    }
}

// slug_exists — quick boolean pre-check. `include_deleted` matches
// find_by_slug's behavior: false (default) only counts live rows; true
// counts both live and soft-deleted. The admin recovery flow uses
// include_deleted=true so the route can decide whether a slug is
// "truly free" or "taken by a tombstone" before attempting restore.
inline bool slug_exists(ConnectionPool& pool,
                        std::string_view slug,
                        bool include_deleted = false) {
    auto conn = pool.acquire();
    if (include_deleted) {
        const auto row = conn.fetch_scalar<int>(
            "SELECT 1 FROM problems WHERE slug = ? LIMIT 1",
            std::string(slug));
        return row.has_value();
    } else {
        const auto row = conn.fetch_scalar<int>(
            "SELECT 1 FROM problems WHERE slug = ? AND is_deleted = FALSE LIMIT 1",
            std::string(slug));
        return row.has_value();
    }
}

// find_by_id — load a full row by primary key. `include_deleted=false`
// (the default) hides soft-deleted rows; the admin recovery path sets
// it to true. Returns std::nullopt when no such row exists.
inline std::optional<ProblemRow> find_by_id(ConnectionPool& pool,
                                            int id,
                                            bool include_deleted = false) {
    auto conn = pool.acquire();
    std::optional<mysqlx::Row> row;
    if (include_deleted) {
        row = conn.fetch_one(
            std::string("SELECT ") + detail::kProblemSelectColumns +
            " FROM problems WHERE id = ? LIMIT 1",
            id);
    } else {
        row = conn.fetch_one(
            std::string("SELECT ") + detail::kProblemSelectColumns +
            " FROM problems WHERE id = ? AND is_deleted = FALSE LIMIT 1",
            id);
    }
    if (!row) return std::nullopt;
    return detail::row_to_problem(*row);
}

// find_by_slug — load a full row by URL slug. Same include_deleted
// semantics as find_by_id.
inline std::optional<ProblemRow> find_by_slug(ConnectionPool& pool,
                                              std::string_view slug,
                                              bool include_deleted = false) {
    auto conn = pool.acquire();
    std::optional<mysqlx::Row> row;
    if (include_deleted) {
        row = conn.fetch_one(
            std::string("SELECT ") + detail::kProblemSelectColumns +
            " FROM problems WHERE slug = ? LIMIT 1",
            std::string(slug));
    } else {
        row = conn.fetch_one(
            std::string("SELECT ") + detail::kProblemSelectColumns +
            " FROM problems WHERE slug = ? AND is_deleted = FALSE LIMIT 1",
            std::string(slug));
    }
    if (!row) return std::nullopt;
    return detail::row_to_problem(*row);
}

// update — modify every mutable column on a row identified by its
// current slug. Slug itself is part of the body; the WHERE clause
// still pins the OLD slug, so renaming is supported atomically.
//
// Throws ProblemNotFoundError when no row matches `current_slug`
// (and is not soft-deleted, so we don't accidentally resurrect a
// tombstone by editing it). The route handler maps that to 404.
//
// Throws ProblemAlreadyExistsError when the new slug collides with
// a different row (live or soft-deleted). The route handler maps
// that to 409.
//
// Returns true when a row was actually changed (false ⇒ current_slug
// matched but every value was already the new value — same effect
// from the caller's point of view, so we collapse them here).
inline bool update(ConnectionPool& pool,
                   std::string_view current_slug,
                   const ProblemRow& patch) {
    {
        std::string err;
        if (!validate_slug      (patch.slug,        &err)) throw ProblemRepoError("update: " + err);
        if (!validate_title     (patch.title,       &err)) throw ProblemRepoError("update: " + err);
        if (!validate_difficulty(patch.difficulty,  &err)) throw ProblemRepoError("update: " + err);
        if (!validate_time_limit(patch.time_limit,  &err)) throw ProblemRepoError("update: " + err);
        if (!validate_memory_limit(patch.memory_limit, &err)) throw ProblemRepoError("update: " + err);
    }

    auto conn = pool.acquire();

    // First, confirm the row exists and is live. Doing this BEFORE
    // the UPDATE gives us a clean 404 path even when the new slug
    // collides with a different row — the driver error from a UNIQUE
    // violation is opaque and we don't want to leak "1062" to the
    // caller.
    {
        const auto exists = conn.fetch_scalar<int>(
            "SELECT 1 FROM problems WHERE slug = ? AND is_deleted = FALSE LIMIT 1",
            std::string(current_slug));
        if (!exists.has_value()) {
            throw ProblemNotFoundError(
                std::string("problem_repo::update: slug '") +
                std::string(current_slug) + "' not found");
        }
    }

    try {
        auto rs = conn.execute(
            "UPDATE problems "
            "SET slug = ?, title = ?, difficulty = ?, description = ?, "
            "    template = ?, time_limit = ?, memory_limit = ? "
            "WHERE slug = ? AND is_deleted = FALSE",
            patch.slug,
            patch.title,
            patch.difficulty,
            patch.description,
            patch.template_,         // v1.3.2: per-problem code template
            patch.time_limit,
            patch.memory_limit,
            std::string(current_slug));
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            // The new slug collides with a different row.
            // Surface as a typed exception so the route can return 409
            // with a clear "slug taken" message instead of a 500.
            throw ProblemAlreadyExistsError(
                std::string("problem_repo::update: slug '") +
                patch.slug + "' already exists");
        }
        throw ProblemRepoError(std::string("problem_repo::update: ") + what);
    }
}

// soft_delete — flip is_deleted = TRUE on the row matching `slug`.
// Returns true if a live row was actually soft-deleted; false when
// no live row matched (already deleted, or never existed).
//
// Notes:
//   - We use UPDATE … WHERE slug = ? AND is_deleted = FALSE so that
//     double-DELETE is idempotent: the second call returns false
//     instead of "affecting" the soft-deleted row's updated_at.
//   - The route handler maps false → 404 (no live row found).
inline bool soft_delete(ConnectionPool& pool, std::string_view slug) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "UPDATE problems SET is_deleted = TRUE "
            "WHERE slug = ? AND is_deleted = FALSE",
            std::string(slug));
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        throw ProblemRepoError(std::string("problem_repo::soft_delete: ") + e.what());
    }
}

// restore — admin recovery: flip is_deleted = FALSE on a row matching
// `slug`. Returns true if a soft-deleted row was actually restored;
// false when no soft-deleted row matched (already live, or never
// existed). The route handler maps false → 404.
//
// The UNIQUE(slug) constraint means restore can fail with errno 1062
// if another LIVE row has already taken the slug (e.g. someone
// recreated it under the same key after the original was deleted).
// We don't try to recover from that — the admin has to rename one
// of the two rows. The error propagates as ProblemAlreadyExistsError
// via the catch below.
inline bool restore(ConnectionPool& pool, std::string_view slug) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "UPDATE problems SET is_deleted = FALSE "
            "WHERE slug = ? AND is_deleted = TRUE",
            std::string(slug));
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            throw ProblemAlreadyExistsError(
                std::string("problem_repo::restore: slug '") +
                std::string(slug) + "' is already in use by another live row");
        }
        throw ProblemRepoError(std::string("problem_repo::restore: ") + what);
    }
}

// UpsertResult — return value of upsert().
//
//   - id       : the problem row's primary key after the call. On a
//                fresh insert this is the new AUTO_INCREMENT id; on
//                overwrite this is the existing row's id.
//   - created  : true ⇔ a new row was inserted; false ⇔ the existing
//                row was overwritten. The caller (route layer) uses
//                this to label the response entry as "created" vs
//                "overwritten".
struct UpsertResult {
    int  id      = 0;
    bool created = false;
};

// upsert — INSERT a new problem row, or overwrite an existing one
// keyed by `slug`. The behavior of the conflict branch is "full
// overwrite": title / difficulty / description / time_limit /
// memory_limit are all replaced with the new values, `is_deleted`
// is flipped to FALSE (so a previously soft-deleted row comes back
// to life — a free side-effect of the upsert contract), and
// `updated_at` is refreshed via MySQL's NOW(). The admin
// ?on_duplicate=overwrite path in the bulk-import endpoint funnels
// through here, mirroring SPEC §8.2 ("可选 query 参数
// ?on_duplicate=overwrite 覆盖").
//
// The slug column has a UNIQUE constraint, so the ON DUPLICATE KEY
// branch fires only when another row (live or soft-deleted) already
// owns `row.slug`.
//
// Returns the row id and a `created` flag. On any driver / SQL
// failure throws ProblemRepoError (handler → 500 envelope).
inline UpsertResult upsert(ConnectionPool& pool, const ProblemRow& row) {
    const int time_limit   = row.time_limit   == 0 ? 1000 : row.time_limit;
    const int memory_limit = row.memory_limit == 0 ? 256  : row.memory_limit;

    // Defense-in-depth validation on the EFFECTIVE values.
    {
        std::string err;
        if (!validate_slug      (row.slug,        &err)) throw ProblemRepoError("upsert: " + err);
        if (!validate_title     (row.title,       &err)) throw ProblemRepoError("upsert: " + err);
        if (!validate_difficulty(row.difficulty,  &err)) throw ProblemRepoError("upsert: " + err);
        if (!validate_time_limit(time_limit,     &err)) throw ProblemRepoError("upsert: " + err);
        if (!validate_memory_limit(memory_limit, &err)) throw ProblemRepoError("upsert: " + err);
    }

    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "INSERT INTO problems "
            "(slug, title, difficulty, description, template, time_limit, memory_limit) "
            "VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE "
            "title = VALUES(title), "
            "difficulty = VALUES(difficulty), "
            "description = VALUES(description), "
            "template = VALUES(template), "
            "time_limit = VALUES(time_limit), "
            "memory_limit = VALUES(memory_limit), "
            "is_deleted = FALSE, "
            "updated_at = NOW()",
            row.slug,
            row.title,
            row.difficulty,
            row.description,
            row.template_,          // v1.3.2: per-problem code template
            time_limit,
            memory_limit);
        // Disambiguate created vs overwritten via the affected-rows
        // count. MySQL's INSERT ... ON DUPLICATE KEY UPDATE returns:
        //   1 — a fresh row was inserted (created path)
        //   2 — an existing row was overwritten (ON DUPLICATE branch)
        //   0 — no change (the existing row matched every value)
        // The mysql-connector-c++ 9.x getAutoIncrementValue() is
        // NOT a reliable discriminator — it can return the next
        // AUTO_INCREMENT value even for the ON DUPLICATE branch on
        // some connector versions / pool states. Affected rows is
        // the authoritative MySQL-defined signal.
        const auto affected = rs.getAffectedItemsCount();
        // One indexed lookup; the slug index makes it a single-page
        // read. Done unconditionally so we always return the right
        // id (the AUTO_INCREMENT might not be the row id when we
        // hit the ON DUPLICATE branch — auto_increment advances
        // even on duplicate-key attempts in MySQL).
        const auto existing = conn.fetch_scalar<std::int64_t>(
            "SELECT id FROM problems WHERE slug = ? LIMIT 1",
            std::string(row.slug));
        if (!existing.has_value()) {
            throw ProblemRepoError(
                "problem_repo::upsert: row vanished after upsert for slug '"
                + row.slug + "'");
        }
        const int row_id = static_cast<int>(*existing);
        // created ⇔ affected == 1 (a real INSERT happened). Anything
        // else (0 / 2) means we hit the ON DUPLICATE branch.
        const bool created = (affected == 1);
        return UpsertResult{row_id, created};
    } catch (const ProblemRepoError&) {
        throw;
    } catch (const mysqlx::Error& e) {
        throw ProblemRepoError(std::string("problem_repo::upsert: ") + e.what());
    }
}

// count — total rows matching `filter`. Used by list() to fill the
// `total` field on the result. We keep the same include_deleted /
// difficulty / tag_id predicates as list() so the two stay in sync.
inline int count(ConnectionPool& pool, const ProblemListFilter& filter) {
    ProblemListFilter f = filter;
    clamp_list_filter(f);

    // Build the WHERE clause piece by piece. We avoid a JOIN to
    // problem_tags when no tag_id is set so the count query stays a
    // simple index scan.
    std::string sql = "SELECT COUNT(*) FROM problems";
    if (f.tag_id.has_value()) {
        sql += " WHERE id IN (SELECT problem_id FROM problem_tags WHERE tag_id = ?)";
    } else {
        sql += " WHERE 1 = 1";
    }
    if (!f.include_deleted) {
        sql += " AND is_deleted = FALSE";
    }
    if (f.difficulty.has_value()) {
        sql += " AND difficulty = ?";
    }

    auto conn = pool.acquire();
    try {
        if (f.tag_id.has_value() && f.difficulty.has_value()) {
            const auto v = conn.fetch_scalar<std::int64_t>(
                sql, *f.tag_id, *f.difficulty);
            return v.value_or(0);
        } else if (f.tag_id.has_value()) {
            const auto v = conn.fetch_scalar<std::int64_t>(
                sql, *f.tag_id);
            return v.value_or(0);
        } else if (f.difficulty.has_value()) {
            const auto v = conn.fetch_scalar<std::int64_t>(
                sql, *f.difficulty);
            return v.value_or(0);
        } else {
            const auto v = conn.fetch_scalar<std::int64_t>(sql);
            return v.value_or(0);
        }
    } catch (const mysqlx::Error& e) {
        throw ProblemRepoError(std::string("problem_repo::count: ") + e.what());
    }
}

// list — paginated, filterable page of problems. Returns
// {items, total, limit, offset}. `total` is the unpaginated count
// for the same filter, so the front-end can render pagination UI
// without a second round-trip.
//
// Ordering is fixed: created_at DESC, id DESC. SPEC §4.5 explicitly
// lists `(is_deleted, difficulty, created_at)` as the index for
// "列表分页 + 难度筛选 + 软删过滤" — created_at DESC matches the
// "newest first" expectation from the front-end.
inline ProblemListResult list(ConnectionPool& pool,
                              const ProblemListFilter& filter) {
    ProblemListFilter f = filter;
    clamp_list_filter(f);

    ProblemListResult out;
    out.limit  = f.limit;
    out.offset = f.offset;
    out.total  = count(pool, f);

    // Build the SELECT.
    std::string sql = "SELECT ";
    sql += detail::kProblemSelectColumns;
    sql += " FROM problems";
    if (f.tag_id.has_value()) {
        sql += " WHERE id IN (SELECT problem_id FROM problem_tags WHERE tag_id = ?)";
    } else {
        sql += " WHERE 1 = 1";
    }
    if (!f.include_deleted) {
        sql += " AND is_deleted = FALSE";
    }
    if (f.difficulty.has_value()) {
        sql += " AND difficulty = ?";
    }
    sql += " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?";

    auto conn = pool.acquire();
    try {
        // Dispatch on the (tag_id × difficulty) shape to keep the
        // bind() argument count exactly right — mysqlx requires
        // positional binding to match the placeholder count.
        mysqlx::SqlResult rs = ([&]() {
            if (f.tag_id.has_value() && f.difficulty.has_value()) {
                return conn.execute(sql, *f.tag_id, *f.difficulty,
                                    f.limit, f.offset);
            } else if (f.tag_id.has_value()) {
                return conn.execute(sql, *f.tag_id,
                                    f.limit, f.offset);
            } else if (f.difficulty.has_value()) {
                return conn.execute(sql, *f.difficulty,
                                    f.limit, f.offset);
            } else {
                return conn.execute(sql, f.limit, f.offset);
            }
        })();

        out.items.reserve(static_cast<std::size_t>(f.limit));
        for (auto row : rs) {
            out.items.push_back(detail::row_to_problem(row));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw ProblemRepoError(std::string("problem_repo::list: ") + e.what());
    }
}

} // namespace problem_repo
} // namespace litecode
