// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — tags + problem_tags repository (Phase 3 ★)
//
// SPEC §4.2b / §4.2c / §4.5 / §5.2 / §11 Phase 3 / §15 / §11 Phase 6 (admin)
// / A4, A18, A20 acceptance (the underlying M:N machinery the
// problem_routes / problem_repo filters rely on):
//   - `tags` table schema (V001):
//       id INT AUTO_INCREMENT PK, name VARCHAR(50) UNIQUE NOT NULL
//       (utf8mb4_unicode_ci -- UNIQUE is case-insensitive and
//        accent-insensitive, so "Hash" and "hash" collide)
//   - `problem_tags` M:N table (V001):
//       (problem_id, tag_id) PK
//       FK problem_id -> problems(id) ON DELETE CASCADE
//       FK tag_id     -> tags(id)     ON DELETE CASCADE
//   - The repo offers a Phase-3-shaped surface that the public
//     list-tags endpoint, the problem detail/list endpoints (for
//     resolving problem->tags), and the admin problem CRUD / bulk
//     import endpoints (for tag resolution + "auto-create missing
//     tags" per SPEC §5.2) all share:
//       Tags table:
//         * create             -- INSERT a new tag row, return new id
//                                 (0 on duplicate name; the auto-create
//                                 path on problem create is a
//                                 INSERT IGNORE + re-SELECT, not a
//                                 collision path, so this 0 is for
//                                 callers that explicitly want to know)
//         * find_by_id         -- fetch one row by primary key
//         * find_by_name       -- same idea, keyed by the display name
//         * name_exists        -- boolean pre-check used by the admin
//                                 "rename into existing" path
//         * update             -- rename a tag; throws NotFoundError
//                                 / AlreadyExistsError so the route can
//                                 map to 404 / 409
//         * delete_by_id       -- hard DELETE; cascades to problem_tags
//                                 via FK ON DELETE CASCADE. Reserved
//                                 for admin cleanup / tests; the public
//                                 API never deletes tags.
//         * list               -- full list of tags ordered by name
//         * list_with_count    -- same, plus a per-tag problem count
//                                 (live vs include-deleted problem
//                                 controlled by `count_live_problems`)
//       problem_tags M:N:
//         * attach             -- INSERT (problem_id, tag_id); idempotent
//                                 via INSERT IGNORE so a duplicate call
//                                 is a no-op success
//         * detach             -- DELETE one (problem_id, tag_id) row
//         * clear              -- DELETE every tag for a given problem
//         * replace            -- clear + attach the given set
//                                 atomically (BEGIN/COMMIT) so the
//                                 problem's tag set is never observed
//                                 in a half-applied state
//         * list_tags_for_problem       -- tags attached to a problem
//         * list_problems_for_tag       -- problem_ids attached to a tag
//                                         (live-only by default; the
//                                         admin path opts in to
//                                         tombstone visibility)
//         * count_tags_for_problem      -- COUNT(*) for a problem
//         * count_problems_for_tag      -- COUNT(*) for a tag (live
//                                         vs include-deleted same as
//                                         list_problems_for_tag)
//       Bulk resolver used by admin problem create / bulk import:
//         * find_or_create_many         -- given a vector of names,
//                                         INSERT IGNORE the missing
//                                         ones, then SELECT id for
//                                         every name; returns
//                                         vector<TagRow> ordered by
//                                         the caller's input order
//                                         so the route can use the
//                                         resolved IDs directly
//                                         without reordering
//
//   - All writes use parameterized SQL (`?` placeholders) -- SPEC §15.2
//     forbids string concatenation. mysqlx::SqlStatement::bind() handles
//     the binding; user-supplied data never reaches the wire string.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3 module
//     (config.h / logger.h / server.h / user_repo.h / problem_repo.h
//     / audit_log_repo.h). The repo is essentially a thin set of
//     free functions over a ConnectionPool reference; no internal
//     state to unit-test.
//   - Returned rows are exposed via a `TagRow` struct (not the raw
//     mysqlx::Row) so callers don't depend on mysqlx::Value's
//     semantics. `std::optional<TagRow>` for "not found".
//   - We deliberately do NOT model an ORM-style change-tracking
//     `Tag` object. Hand-written SQL with bind() is fine for our
//     surface and avoids the C++ ORM overhead (SPEC §9 calls this out).
//   - The M:N resolution is split into two layers:
//       (a) tag-row CRUD (create / find / update / delete / list) --
//           operates on `tags` alone
//       (b) association helpers (attach / detach / replace / clear /
//           list_*) -- operate on `problem_tags` and join through to
//           `tags` / `problems` only at the edges (i.e. when callers
//           ask for TagRow projections). This keeps the write paths
//           single-table, and keeps the SQL boundary between "rows
//           of tags" and "ids that participate in the M:N" clean.
//   - `replace` is the only multi-statement helper --
//     it wraps clear + N attaches in a single transaction. The
//     problem update route calls this once per edit; admin bulk
//     import calls it once per imported problem. The single
//     transaction guarantees a reader never sees a problem with
//     half its tags flipped.
//   - The detail helpers (req_string, req_int, opt_int, row_to_tag)
//     live in `litecode::tag_repo::detail` (NOT in the shared
//     `litecode::detail`) so they don't collide with
//     `problem_repo::detail`'s identically-named helpers when both
//     headers are included in the same TU (e.g. in test_tag.cpp).
//   - Concurrency: every public method acquires a fresh
//     PooledConnection from the pool, runs the SQL, releases. The
//     pool is thread-safe; individual methods do not need their
//     own locks. `replace` holds the connection for the
//     duration of the transaction -- that's fine because the
//     pool grows up to max_size concurrent connections.
//
// Usage (public list, future problem_routes.h / tag_routes.h):
//
//   const auto tags = litecode::tag_repo::list(pool);
//   // tags is std::vector<TagRow>, ordered by name ASC
//
// Usage (admin create problem with "auto-create missing tags"):
//
//   std::vector<std::string> names = {"\xe6\x95\xb0\xe7\xbb\x84", "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"};
//   const auto resolved = litecode::tag_repo::find_or_create_many(
//       pool, names);
//   // resolved is std::vector<TagRow> in the SAME order as `names`,
//   // with `id` populated for every name.
//
// Usage (admin update problem -- replace tag set in one tx):
//
//   std::vector<int> new_tag_ids = {1, 2, 3};
//   litecode::tag_repo::replace(pool, problem_id, new_tag_ids);
//   // clears old + attaches new atomically
//
// Usage (public problem detail -- read attached tags):
//
//   const auto tags = litecode::tag_repo::list_tags_for_problem(
//       pool, problem_id);

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"             // LOG_WARN (best-effort non-fatal DB hiccups)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ---------------------------------------------------------------------------
//  TagRow
//
//  Plain-data projection of a row from the `tags` table. All string
//  fields are std::string for safe lifetime -- callers can copy, move,
//  or store the row across async boundaries.
//
//  Field semantics (mirrors SPEC §4.2b + V001):
//    - id: the row's primary key; 0 => "not yet inserted"
//    - name: required, never empty once loaded, UNIQUE in the DB
//            (UNIQUE is utf8mb4_unicode_ci -- case- AND
//            accent-insensitive, so "Hash" and "hash" collide).
//            SPEC §4.2b example: "\xe6\x95\xb0\xe7\xbb\x84" (array),
//            "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8" (hash table);
//            Chinese is fine -- we deliberately do NOT restrict
//            the charset.
// ---------------------------------------------------------------------------

struct TagRow {
    int         id   = 0;
    std::string name;
};

// ---------------------------------------------------------------------------
//  TagWithCount
//
//  TagRow + a per-tag problem count. Used by the admin tag list
//  endpoint and by any "I want to know which tags are popular"
//  query. `problem_count` is the count of (live or all) problems
//  attached to the tag, depending on the call-site flag -- see
//  list_with_count() for the contract.
// ---------------------------------------------------------------------------

struct TagWithCount {
    TagRow tag;
    int    problem_count = 0;
};

// ---------------------------------------------------------------------------
//  TagRepoError -- surface every repo-layer failure as a typed exception
//
//  Three tiers, mirroring the rest of Phase 3 (problem_repo.h):
//    - TagRepoError           -- generic failure (driver error, etc.)
//    - TagNotFoundError       -- update / delete target not present;
//                                 caught by the route handler and folded
//                                 into 404
//    - TagAlreadyExistsError  -- name uniqueness collision on create
//                                 (via 0 return) or on update (rename
//                                 into an existing name); caught by the
//                                 route handler and folded into 409
//
//  We deliberately do NOT surface duplicate-detection via a magic
//  error code from create(); the boolean returns are simpler and let
//  the handler compose "tag already exists" cleanly. NotFound, by
//  contrast, is rare on the write path and worth a typed exception
//  because the handler maps it directly to a 404 envelope.
// ---------------------------------------------------------------------------

class TagRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TagNotFoundError : public TagRepoError {
public:
    using TagRepoError::TagRepoError;
};

class TagAlreadyExistsError : public TagRepoError {
public:
    using TagRepoError::TagRepoError;
};

// ---------------------------------------------------------------------------
//  Validation helpers (used by both repo + route layer)
//
//  Matches SPEC §4.2b column types:
//    - name: 1..50 chars (DB column is VARCHAR(50)). We deliberately
//            do NOT restrict the charset -- the migration sets
//            utf8mb4_unicode_ci, and SPEC §4.2b explicitly shows
//            Chinese names like "\xe6\x95\xb0\xe7\xbb\x84" and
//            "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8". A future locale
//            extension (e.g. Cyrillic, Arabic) works without
//            validator changes.
//    - The validator also rejects leading/trailing whitespace --
//            the UNIQUE index is whitespace-sensitive ("foo" and
//            " foo" are different rows), and a leading-space typo
//            shows up in the UI as a mystery " tag". We trim
//            implicitly on insert; callers that want to preserve
//            the exact bytes can use raw SQL via the connection
//            pool.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMinTagNameLength = 1;
inline constexpr std::size_t kMaxTagNameLength = 50;

inline bool validate_tag_name(std::string_view name,
                              std::string* error_out = nullptr) {
    if (name.size() < kMinTagNameLength || name.size() > kMaxTagNameLength) {
        if (error_out) {
            *error_out = "tag name length must be between " +
                         std::to_string(kMinTagNameLength) + " and " +
                         std::to_string(kMaxTagNameLength);
        }
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ' ||
        name.front() == '\t' || name.back() == '\t') {
        if (error_out) {
            *error_out = "tag name must not start or end with whitespace";
        }
        return false;
    }
    // Reject ASCII control characters (NUL, newline, tab, etc.) but
    // accept everything else (multi-byte UTF-8 stays valid because
    // the high bit of continuation bytes is always 1; we test each
    // byte individually against the 0x20..0x7E printable range OR
    // accept a non-ASCII byte as part of a multi-byte sequence).
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "tag name must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

// Trim leading/trailing ASCII whitespace from a tag name. Internal
// whitespace is left intact ("linked  list" stays "linked  list") so
// multi-word tags round-trip exactly. UTF-8 multibyte sequences are
// safe because every byte of a continuation byte has the high bit
// set and never matches a single-byte space/tab.
inline std::string trim_tag_name(std::string_view name) {
    std::size_t begin = 0;
    std::size_t end   = name.size();
    while (begin < end &&
           (name[begin] == ' ' || name[begin] == '\t')) {
        ++begin;
    }
    while (end > begin &&
           (name[end - 1] == ' ' || name[end - 1] == '\t')) {
        --end;
    }
    return std::string(name.substr(begin, end - begin));
}

// ---------------------------------------------------------------------------
//  Public API -- `tags` table
// ---------------------------------------------------------------------------

namespace tag_repo {

// Row materialization helpers live INSIDE `tag_repo::detail` (NOT
// in the shared `litecode::detail`) so they don't collide with
// `problem_repo::detail`'s identically-named helpers when both
// headers are included in the same TU (e.g. in test_tag.cpp).
namespace detail {

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw TagRepoError(std::string("tag_repo: required field '") +
                           field + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw TagRepoError(std::string("tag_repo: required field '") +
                           field + "' is not an int: " + e.what());
    }
}

inline int opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return 0;
    try {
        return static_cast<int>(v.get<std::int64_t>());
    } catch (const std::exception&) {
        return 0;
    }
}

// Column order for the basic `tags` projection:
//
//   0 id
//   1 name
inline constexpr const char* kTagSelectColumns = "id, name";

inline TagRow row_to_tag(const mysqlx::Row& row) {
    TagRow t;
    t.id   = req_int   (row, 0, "id");
    t.name = req_string(row, 1, "name");
    return t;
}

}  // namespace detail

// create -- INSERT a new tag row.
//
// Returns:
//   - new tag id (>0) on success
//   - 0 on name uniqueness collision (no exception thrown; the
//     route handler maps 0 -> 409 CONFLICT)
//
// Throws TagRepoError on driver / SQL errors (handler -> 500).
// Throws TagRepoError (via validate_*) on invalid field values --
// the caller is expected to have validated first, but we re-validate
// as a defense in depth.
//
// Notes:
//   - We trim leading/trailing ASCII whitespace from `name` before
//     insert. The validator rejects already-trimmed names that are
//     pure whitespace, but a leading space the user didn't notice
//     would still get past the validator; trim() collapses it.
//   - The INSERT lists every column explicitly so we never
//     accidentally write NULL into a NOT NULL column and so a future
//     schema addition doesn't get a surprise default.
//   - We deliberately do NOT pre-check name_exists() before INSERT;
//     the UNIQUE constraint on `name` is the authoritative gate, and
//     the INSERT-then-check pattern is one round-trip. The race
//     window between "I checked, it's free" and "I inserted" is
//     closed by the UNIQUE constraint at INSERT time.
//   - The UNIQUE index is utf8mb4_unicode_ci, so "Hash" and "hash"
//     collide. The "Duplicate entry" path in the catch below fires
//     for that case the same as for an exact match; the caller can't
//     distinguish and shouldn't need to.
inline int create(ConnectionPool& pool, std::string_view name) {
    const std::string trimmed = trim_tag_name(name);

    {
        std::string err;
        if (!validate_tag_name(trimmed, &err)) {
            throw TagRepoError("create: " + err);
        }
    }

    auto conn = pool.acquire();

    try {
        auto rs = conn.execute(
            "INSERT INTO tags (name) VALUES (?)",
            trimmed);
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
        throw TagRepoError(std::string("tag_repo::create: ") + what);
    }
}

// name_exists -- quick boolean pre-check. Matches find_by_name's
// collation behavior: the UNIQUE index is utf8mb4_unicode_ci, so a
// case-only difference ("Hash" vs "hash") is treated as a match.
inline bool name_exists(ConnectionPool& pool, std::string_view name) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM tags WHERE name = ? LIMIT 1",
        std::string(name));
    return row.has_value();
}

// find_by_id -- load a full row by primary key. Returns std::nullopt
// when no such row exists.
inline std::optional<TagRow> find_by_id(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        std::string("SELECT ") + detail::kTagSelectColumns +
        " FROM tags WHERE id = ? LIMIT 1",
        id);
    if (!row) return std::nullopt;
    return detail::row_to_tag(*row);
}

// find_by_name -- load a full row by display name. Collation-sensitive
// matching (utf8mb4_unicode_ci): "Hash" matches "hash" but "Hash"
// does NOT match "Hash " (trailing space -- those are different
// bytes). The route handler should pre-trim if it wants lenient
// matching.
inline std::optional<TagRow> find_by_name(ConnectionPool& pool,
                                          std::string_view name) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        std::string("SELECT ") + detail::kTagSelectColumns +
        " FROM tags WHERE name = ? LIMIT 1",
        std::string(name));
    if (!row) return std::nullopt;
    return detail::row_to_tag(*row);
}

// update -- rename a tag, identified by id. The UNIQUE constraint
// means rename can fail with errno 1062 if another row already
// has the new name (live or soft-deleted). The catch below surfaces
// that as TagAlreadyExistsError so the route can return 409 with
// a clear "tag name taken" message instead of a 500.
//
// Throws TagNotFoundError when no row matches `id`; the route
// handler maps that to 404.
//
// The new name is trimmed of leading/trailing whitespace and
// re-validated; we don't trust the caller to have trimmed.
inline bool update(ConnectionPool& pool,
                   int id,
                   std::string_view new_name) {
    const std::string trimmed = trim_tag_name(new_name);

    {
        std::string err;
        if (!validate_tag_name(trimmed, &err)) {
            throw TagRepoError("update: " + err);
        }
    }

    auto conn = pool.acquire();

    // First, confirm the row exists. Doing this BEFORE the UPDATE
    // gives us a clean 404 path even when the new name collides
    // with a different row -- the driver error from a UNIQUE
    // violation is opaque and we don't want to leak "1062" to the
    // caller.
    {
        const auto exists = conn.fetch_scalar<int>(
            "SELECT 1 FROM tags WHERE id = ? LIMIT 1",
            id);
        if (!exists.has_value()) {
            throw TagNotFoundError(
                std::string("tag_repo::update: id ") +
                std::to_string(id) + " not found");
        }
    }

    try {
        auto rs = conn.execute(
            "UPDATE tags SET name = ? WHERE id = ?",
            trimmed, id);
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            // The new name collides with a different row.
            throw TagAlreadyExistsError(
                std::string("tag_repo::update: name '") +
                trimmed + "' already exists");
        }
        throw TagRepoError(std::string("tag_repo::update: ") + what);
    }
}

// delete_by_id -- hard DELETE a tag row. FK ON DELETE CASCADE on
// problem_tags drops every (problem_id, tag_id) row that points at
// this tag, so the M:N table stays consistent automatically. The
// public /api/v1/* API never calls this; it's reserved for admin
// cleanup and for tests.
//
// Returns true if a row was actually removed; false when no row
// matched `id` (already deleted, or never existed). The route
// handler can map false -> 404.
inline bool delete_by_id(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "DELETE FROM tags WHERE id = ?",
            id);
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::delete_by_id: ") + e.what());
    }
}

// list -- full list of tags, ordered by name ASC (the natural order
// for /api/v1/tags dropdowns / filter chips). Used by the public
// list-tags endpoint.
//
// We deliberately do NOT paginate the tags list. SPEC §5.2 says
// "all tags" and tags are bounded (handful of dozen for a learning
// OJ); pagination would only complicate the front-end filter UI.
inline std::vector<TagRow> list(ConnectionPool& pool) {
    auto conn = pool.acquire();
    std::vector<TagRow> out;
    try {
        const std::string sql = std::string("SELECT ") +
                                detail::kTagSelectColumns +
                                " FROM tags ORDER BY name ASC, id ASC";
        auto rs = conn.execute(sql);
        out.reserve(16);  // small bounded list; pre-reserve to dodge a few reallocs
        for (auto row : rs) {
            out.push_back(detail::row_to_tag(row));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::list: ") + e.what());
    }
}

// list_with_count -- same as list(), plus a per-tag problem count.
//
// `count_live_problems=true` (default) counts only problems with
// `is_deleted = FALSE`. This matches the public read path
// (problems listed on the site are the live ones) and keeps the
// "Hash" tag from showing count=12 just because 12 soft-deleted
// problems used to carry it.
//
// `count_live_problems=false` counts every problem that has ever
// been attached to the tag (live + soft-deleted). This is what the
// admin tag-management view wants when auditing history.
//
// The LEFT JOIN keeps tags with zero problems in the result with
// count=0, instead of dropping them (INNER JOIN would silently
// hide them). The route handler can render "Hash (0)" or hide the
// row based on the call-site.
inline std::vector<TagWithCount> list_with_count(ConnectionPool& pool,
                                                 bool count_live_problems = true) {
    auto conn = pool.acquire();
    std::vector<TagWithCount> out;
    try {
        // 0 t.id
        // 1 t.name
        // 2 problem_count
        //
        // We fully qualify the SELECT columns (`t.id`, `t.name`)
        // because both `tags` and `problem_tags` carry an `id`
        // column, and `problem_tags` also has a `name` indirectly
        // through the join. Without the prefix, MySQL raises
        // "Column 'id' in field list is ambiguous".
        //
        // For the live-only case, we LEFT JOIN to `problems` and
        // use COUNT(CASE WHEN p.is_deleted = FALSE THEN 1 END).
        // COUNT counts only non-NULL values, so the CASE returns
        // 1 for live rows and NULL for soft-deleted/missing
        // problems -- the result is the count of LIVE problems.
        // Tags with zero problems (LEFT JOIN produced a single
        // row with p=NULL, CASE=NULL) keep a 0 count, so the
        // LEFT JOIN shape preserves them in the result.
        //
        // Note: SUM(CASE ... 1/0) would also work logically, but
        // mysql-connector-c++ 9.x reports SUM-of-INTEGER as a
        // DECIMAL value that the driver can't auto-convert via
        // get<int64>() ("Can not convert to integer value" at
        // runtime). COUNT returns a plain integer and the driver
        // handles it cleanly.
        std::string sql =
            "SELECT t.id, t.name, "
            "COUNT(pt.problem_id) AS problem_count "
            "FROM tags t "
            "LEFT JOIN problem_tags pt ON pt.tag_id = t.id";
        if (count_live_problems) {
            sql += " LEFT JOIN problems p ON p.id = pt.problem_id";
            sql = "SELECT t.id, t.name, "
                  "COUNT(CASE WHEN p.is_deleted = FALSE THEN 1 END) "
                  "    AS problem_count "
                  "FROM tags t "
                  "LEFT JOIN problem_tags pt ON pt.tag_id = t.id "
                  "LEFT JOIN problems p ON p.id = pt.problem_id "
                  "GROUP BY t.id, t.name";
        } else {
            sql += " GROUP BY t.id, t.name";
        }
        sql += " ORDER BY t.name ASC, t.id ASC";

        auto rs = conn.execute(sql);
        out.reserve(16);
        for (auto row : rs) {
            TagWithCount twc;
            twc.tag           = detail::row_to_tag(row);
            twc.problem_count = detail::opt_int(row, 2);
            out.push_back(std::move(twc));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::list_with_count: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
//  Public API -- `problem_tags` M:N
// ---------------------------------------------------------------------------

// attach -- INSERT one (problem_id, tag_id) row. Idempotent: a
// duplicate call is a no-op success (we use INSERT IGNORE so the
// PK collision is swallowed instead of raising 1062).
//
// `replace` is the right entry point for "set the
// problem's full tag list"; attach() is for "add one more tag to
// a problem" calls (e.g. a future "tag this problem" UI action).
inline void attach(ConnectionPool& pool, int problem_id, int tag_id) {
    auto conn = pool.acquire();
    try {
        conn.execute(
            "INSERT IGNORE INTO problem_tags (problem_id, tag_id) "
            "VALUES (?, ?)",
            problem_id, tag_id);
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::attach: ") + e.what());
    }
}

// detach -- DELETE one (problem_id, tag_id) row. Returns true if a
// row was actually removed; false when no row matched (already
// detached, or never attached). The route handler can map false ->
// 404 if it cares; most call sites don't.
inline bool detach(ConnectionPool& pool, int problem_id, int tag_id) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "DELETE FROM problem_tags "
            "WHERE problem_id = ? AND tag_id = ?",
            problem_id, tag_id);
        return rs.getAffectedItemsCount() > 0;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::detach: ") + e.what());
    }
}

// clear -- DELETE every tag for a given problem. Returns the number
// of rows removed. Used by replace and by the future
// admin "untag everything" path.
//
// Note: this does NOT cascade to `tags` -- the tag rows themselves
// stay. They're not orphaned; other problems can still reference
// them. (FK direction is `problem_tags -> tags`, so deleting tag
// rows is the cascade direction, not the other way around.)
inline int clear(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "DELETE FROM problem_tags WHERE problem_id = ?",
            problem_id);
        return static_cast<int>(rs.getAffectedItemsCount());
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::clear: ") + e.what());
    }
}

// replace -- clear all + attach the given set, in a single
// transaction. The transaction guarantees a reader never observes
// the problem in a half-applied tag state (e.g. "old tags gone
// but new ones not yet attached").
//
// We de-duplicate `tag_ids` before the loop so a caller passing
// [1, 1, 2, 2] doesn't generate INSERT IGNORE warnings or wasted
// round-trips. Insertion order is preserved (set semantics aside,
// each unique id is attached exactly once).
//
// Throws TagRepoError on any driver error. The transaction is
// rolled back when a mid-loop failure leaves the connection
// without commit() -- that means the problem keeps its OLD tag set
// intact, not a half-set.
inline void replace(ConnectionPool& pool,
                    int problem_id,
                    const std::vector<int>& tag_ids) {
    auto conn = pool.acquire();
    try {
        // Deduplicate while preserving the caller's order. We
        // don't sort -- the route layer might want the displayed
        // order to follow the input order, and a stable
        // round-trip is the simplest contract.
        std::vector<int> unique_ids;
        unique_ids.reserve(tag_ids.size());
        {
            std::unordered_map<int, bool> seen;
            seen.reserve(tag_ids.size() * 2);
            for (int id : tag_ids) {
                if (seen.find(id) == seen.end()) {
                    seen.emplace(id, true);
                    unique_ids.push_back(id);
                }
            }
        }

        // Transaction. mysqlx exposes the underlying Session via
        // PooledConnection::session() and supports
        // START TRANSACTION / COMMIT / ROLLBACK as ordinary SQL
        // statements. Every execute() below runs on the same
        // session, so the BEGIN here covers every INSERT/DELETE
        // in the loop.
        conn.execute("START TRANSACTION");
        try {
            conn.execute(
                "DELETE FROM problem_tags WHERE problem_id = ?",
                problem_id);
            for (int id : unique_ids) {
                // INSERT IGNORE so a stale id (e.g. one the admin
                // deleted between read and write) doesn't blow up
                // the whole replace. The admin is expected to have
                // re-validated via find_or_create_many, but a
                // race-safe guard here is cheap.
                conn.execute(
                    "INSERT IGNORE INTO problem_tags (problem_id, tag_id) "
                    "VALUES (?, ?)",
                    problem_id, id);
            }
            conn.execute("COMMIT");
        } catch (...) {
            // Best-effort rollback. If the connection is already
            // broken (the original error was a connection-level
            // fault), the rollback statement will also fail -- we
            // swallow that and let the original exception propagate.
            try { conn.execute("ROLLBACK"); } catch (...) {}
            throw;
        }
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::replace: ") + e.what());
    }
}

// list_tags_for_problem -- return the full set of tags attached to
// a given problem, ordered by name ASC (stable, matches list()).
// Returns an empty vector when the problem has no tags OR when
// the problem doesn't exist (callers that need to distinguish
// "no problem" from "problem with zero tags" should check
// problem_repo::find_by_id first).
inline std::vector<TagRow> list_tags_for_problem(ConnectionPool& pool,
                                                  int problem_id) {
    auto conn = pool.acquire();
    std::vector<TagRow> out;
    try {
        const std::string sql = std::string("SELECT ") +
                                detail::kTagSelectColumns +
                                " FROM tags t "
                                "INNER JOIN problem_tags pt "
                                "    ON pt.tag_id = t.id "
                                "WHERE pt.problem_id = ? "
                                "ORDER BY t.name ASC, t.id ASC";
        auto rs = conn.execute(sql, problem_id);
        for (auto row : rs) {
            out.push_back(detail::row_to_tag(row));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::list_tags_for_problem: ") + e.what());
    }
}

// list_problems_for_tag -- return all problem_ids attached to a
// given tag. Live-only by default (public detail/listing path);
// admin paths set include_deleted_problems=true.
//
// The `id` projection (not full ProblemRow) keeps this helper
// cheap and lets the caller decide what to do with the ids
// (paginate, batch-load, etc.). For full rows, callers can
// join through to problem_repo::list() with the id set.
inline std::vector<int> list_problems_for_tag(ConnectionPool& pool,
                                              int tag_id,
                                              bool include_deleted_problems = false) {
    auto conn = pool.acquire();
    std::vector<int> out;
    try {
        std::string sql = "SELECT pt.problem_id "
                          "FROM problem_tags pt "
                          "INNER JOIN problems p ON p.id = pt.problem_id "
                          "WHERE pt.tag_id = ?";
        if (!include_deleted_problems) {
            sql += " AND p.is_deleted = FALSE";
        }
        sql += " ORDER BY pt.problem_id ASC";
        auto rs = conn.execute(sql, tag_id);
        for (auto row : rs) {
            out.push_back(detail::req_int(row, 0, "problem_id"));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::list_problems_for_tag: ") + e.what());
    }
}

// count_tags_for_problem -- number of tags attached to a given
// problem. Used by the admin problem-edit view to show "this
// problem has 3 tags".
inline int count_tags_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        const auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT COUNT(*) FROM problem_tags WHERE problem_id = ?",
            problem_id);
        return v.value_or(0);
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::count_tags_for_problem: ") + e.what());
    }
}

// count_problems_for_tag -- number of problems attached to a given
// tag. Live-only by default; admin paths set include_deleted_problems=true.
//
// This is the M:N mirror of list_with_count()'s `problem_count` and
// is exposed separately because it's a much cheaper query (no
// GROUP BY over all tags).
inline int count_problems_for_tag(ConnectionPool& pool,
                                  int tag_id,
                                  bool include_deleted_problems = false) {
    auto conn = pool.acquire();
    try {
        std::int64_t v = 0;
        if (include_deleted_problems) {
            v = conn.fetch_scalar<std::int64_t>(
                "SELECT COUNT(*) FROM problem_tags WHERE tag_id = ?",
                tag_id).value_or(0);
        } else {
            v = conn.fetch_scalar<std::int64_t>(
                "SELECT COUNT(*) FROM problem_tags pt "
                "INNER JOIN problems p ON p.id = pt.problem_id "
                "WHERE pt.tag_id = ? AND p.is_deleted = FALSE",
                tag_id).value_or(0);
        }
        return static_cast<int>(v);
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::count_problems_for_tag: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
//  Public API -- bulk resolver
// ---------------------------------------------------------------------------

// find_or_create_many -- resolve a list of names to TagRow values,
// creating missing tags along the way. Returns the rows in the
// SAME order as `names`, so the route handler can use the
// resolved `id`s directly without reordering.
//
// Used by:
//   - admin problem create (POST /api/v1/admin/problems) -- the
//     request body carries `tags: ["\xe6\x95\xb0\xe7\xbb\x84",
//     "\xe5\x93\x88\xe5\xb8\x8c\xe8\xa1\xa8"]`; this helper
//     makes sure both rows exist before the problem's tag
//     associations are written
//   - admin problem update (PUT /api/v1/admin/problems/:slug) --
//     same idea, replaces the previous tag set
//   - admin bulk import (POST /api/v1/admin/problems/import) --
//     called once per imported problem
//
// Behavior:
//   - All names are trimmed and validated. A name that fails
//     validation throws TagRepoError and the whole batch is
//     rejected (we don't partial-apply).
//   - INSERT IGNORE inserts only the rows that don't already
//     exist; the UNIQUE collision is silently absorbed.
//   - After the INSERT IGNORE, we SELECT id, name for every
//     name in the input list (one IN-clause SELECT, regardless
//     of how many were pre-existing). The collation-sensitive
//     match (utf8mb4_unicode_ci) means a caller passing both
//     "Hash" and "hash" will get the same row -- the
//     route layer is expected to dedupe by id if it cares.
//   - Duplicate names in the input list (e.g. ["a","a"])
//     surface as duplicate TagRow entries in the result. The
//     caller can dedupe by id.
//
// Throws TagRepoError on any driver error or invalid input.
inline std::vector<TagRow> find_or_create_many(ConnectionPool& pool,
                                               const std::vector<std::string>& names) {
    if (names.empty()) {
        return {};
    }

    // Trim + validate every name up front. We don't want to half-
    // create a batch and then bail on the fifth name because of a
    // typo in the input.
    std::vector<std::string> trimmed;
    trimmed.reserve(names.size());
    for (const auto& n : names) {
        const std::string t = trim_tag_name(n);
        std::string err;
        if (!validate_tag_name(t, &err)) {
            throw TagRepoError(
                std::string("find_or_create_many: '") + n + "': " + err);
        }
        trimmed.push_back(t);
    }

    auto conn = pool.acquire();
    try {
        // INSERT IGNORE for every name in one multi-row INSERT.
        // The IGNORE swallows 1062 collisions on existing names,
        // so this is safe to run regardless of which names are
        // new. mysqlx binds parameters positionally, so we build
        // a "?, ?, ..." string with the same count as
        // `trimmed.size()`.
        //
        // The variadic execute() used elsewhere in this codebase
        // doesn't accept a runtime-sized list, so we go through
        // Session::sql() + repeated bind() -- the underlying
        // wire protocol is still a single multi-row INSERT.
        std::string placeholders;
        placeholders.reserve(trimmed.size() * 2);
        for (std::size_t i = 0; i < trimmed.size(); ++i) {
            if (i > 0) placeholders += ", ";
            placeholders += "(?)";
        }
        const std::string insert_sql =
            "INSERT IGNORE INTO tags (name) VALUES " + placeholders;

        mysqlx::SqlStatement insert_stmt = conn.session().sql(insert_sql);
        for (const auto& t : trimmed) {
            insert_stmt.bind(t);
        }
        insert_stmt.execute();

        // Now SELECT id, name for every name in the input list.
        // One IN-clause SELECT, one round-trip.
        std::string in_placeholders;
        in_placeholders.reserve(trimmed.size() * 2);
        for (std::size_t i = 0; i < trimmed.size(); ++i) {
            if (i > 0) in_placeholders += ", ";
            in_placeholders += "?";
        }
        const std::string select_sql =
            std::string("SELECT ") + detail::kTagSelectColumns +
            " FROM tags WHERE name IN (" + in_placeholders + ")";

        mysqlx::SqlStatement select_stmt = conn.session().sql(select_sql);
        for (const auto& t : trimmed) {
            select_stmt.bind(t);
        }
        mysqlx::SqlResult rs = select_stmt.execute();

        // Materialize to name -> TagRow, then re-order to match
        // the caller's input order. The IN-clause returns rows
        // in arbitrary order (no ORDER BY is reliable for
        // collation match results), so we MUST reorder
        // client-side.
        //
        // Note: a caller passing "Hash" while the row stored is
        // "hash" (collation-equivalent but bytewise different)
        // will MISS in this bytewise map. The route layer is
        // expected to pre-trim and to pass the canonical form
        // (case-sensitive). Mismatches are documented at the
        // top of this function.
        std::unordered_map<std::string, TagRow> by_name;
        by_name.reserve(trimmed.size() * 2);
        for (auto row : rs) {
            const TagRow t = detail::row_to_tag(row);
            by_name.emplace(t.name, t);
        }

        std::vector<TagRow> out;
        out.reserve(trimmed.size());
        for (const auto& n : trimmed) {
            auto it = by_name.find(n);
            if (it == by_name.end()) {
                throw TagRepoError(
                    std::string("find_or_create_many: failed to resolve '") +
                    n + "' after INSERT IGNORE -- collation mismatch "
                    "between input and stored value is the most likely "
                    "cause (try pre-trimming and matching the canonical "
                    "case stored on disk)");
            }
            out.push_back(it->second);
        }
        return out;
    } catch (const TagRepoError&) {
        throw;
    } catch (const mysqlx::Error& e) {
        throw TagRepoError(std::string("tag_repo::find_or_create_many: ") + e.what());
    } catch (const std::exception& e) {
        throw TagRepoError(std::string("tag_repo::find_or_create_many: ") + e.what());
    }
}

}  // namespace tag_repo
}  // namespace litecode
