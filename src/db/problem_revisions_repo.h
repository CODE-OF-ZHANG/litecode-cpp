// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — problem_revisions repository (v1.2.12)
//
// SPEC §11 Phase 3 last open item (originally tagged "v1.3 考虑",
// pulled forward to v1.2.12 for the storage layer). The v1.3
// reader / diff / restore endpoints will sit on top of these
// rows; for now we only record them automatically when an admin
// creates or updates a problem.
//
// Schema (V009):
//   id                BIGINT AUTO_INCREMENT PRIMARY KEY
//   problem_id        INT  NOT NULL   (FK problems ON DELETE CASCADE)
//   revision_no       INT  NOT NULL   (per-problem 1..N)
//   editor_id         INT  NULL       (FK users    ON DELETE SET NULL)
//   editor_username   VARCHAR(50)     (snapshot — survives user deletion)
//   editor_ip         VARCHAR(45)     (same as audit_logs.ip)
//   action            VARCHAR(20)     ('create' | 'update')
//   slug              VARCHAR(100)    (the slug at that version)
//   title             VARCHAR(200)
//   difficulty        VARCHAR(10)
//   time_limit        INT
//   memory_limit      INT
//   description       MEDIUMTEXT      (snapshot — v1.3 reader replays from this)
//   tags_snapshot     JSON            (array of tag-name strings)
//   samples_snapshot  JSON            (array of {input, output} objects)
//   summary           VARCHAR(200)    (nullable human-readable "what changed")
//   created_at        DATETIME        (default CURRENT_TIMESTAMP)
//
//   UNIQUE KEY (problem_id, revision_no)
//   3 idx_*  : (problem_id, revision_no DESC), (problem_id, created_at DESC),
//              (editor_id, created_at DESC)
//
// Public surface:
//   Writers (Phase 3 ★, used by admin_problem_routes.h):
//     * record              — strict INSERT; throws ProblemRevisionsRepoError
//                            on driver / validation error. Throws
//                            ProblemRevisionsConflictError after a 1062
//                            retry exhausted. revision_no=0 ⇒ repo
//                            allocates MAX+1 atomically (single round-trip
//                            via INSERT...SELECT); revision_no>0 ⇒ caller
//                            is forcing a specific version (used by tests
//                            that want to deterministically probe 1062).
//     * record_best_effort  — same path, but LOG_WARN + swallow, returning 0
//                            on failure. Used by the route handler where a
//                            revision write failure must NOT escalate to
//                            500 (audit_logs is the durable security trail;
//                            problem_revisions is the content-history layer
//                            on top of it).
//   Readers (Phase 6 admin reader / v1.3 endpoint):
//     * find_by_id            — fetch one row by BIGINT PK; nullopt if missing
//     * latest_for_problem    — most-recent revision of one problem
//     * list_for_problem      — paginated + total, ORDER BY revision_no DESC
//     * count_for_problem     — COUNT(*) over a single problem_id
//   Validators (used by record() + by tests):
//     * validate_revision_no / validate_summary / validate_action /
//       validate_editor_username / validate_description
//     * clamp_list_filter     — pagination bounds (same shape as audit_log)
//   Action constants:
//     * kActionRevisionCreate / kActionRevisionUpdate
//
// Design notes (mirrors audit_log_repo.h §0):
//   - Header-only + inline like every Phase 1/2/3 repo.
//   - All writers use parameterized SQL (`?` placeholders) — SPEC §15.2.
//   - `created_at` is DATE_FORMAT'd to 'YYYY-MM-DD HH:MM:SS' on every
//     SELECT — mysql-connector 9.x DATETIME packed-binary workaround.
//   - `tags_snapshot` / `samples_snapshot` JSON columns are CAST AS CHAR
//     in every SELECT — same 9.x-driver JSON-binary bug workaround.
//   - Row materialization helpers live in `problem_revisions_repo::detail`
//     (NOT in shared `litecode::detail`) so they don't collide with
//     audit_log_repo::detail's identically-named helpers when both
//     headers are pulled into one TU (e.g. admin_problem_routes.h).
//   - We don't need an AlreadyExistsError tier because version numbers
//     are caller-allocated; any duplicate is a 1062 ⇒ ProblemRevisionsConflictError
//     so the upper layer can distinguish it from generic driver errors.
//
// Usage (Phase 3 admin write paths):
//
//   litecode::RevisionEntry e;
//   e.problem_id      = problem_id_from_db;
//   e.revision_no     = 0;                  // 0 ⇒ repo allocates MAX+1
//   e.editor_id       = std::stoi(claims.user_id);
//   e.editor_username = claims.username;
//   e.editor_ip       = litecode::extract_client_ip(req);
//   e.action          = litecode::problem_revisions_repo::kActionRevisionUpdate;
//   e.slug            = patch.slug;
//   e.title           = patch.title;
//   ...
//   const std::int64_t rev_id =
//       litecode::problem_revisions_repo::record_best_effort(pool, e);
//   // rev_id > 0 ⇒ stored; rev_id == 0 ⇒ LOG_WARN already emitted inside.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>

#include "../logger.h"             // LOG_WARN (best-effort writers)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Constants — pin the same bounds as V009 + the wire validators
// ────────────────────────────────────────────────────────────────────────────

inline constexpr int          kMinRevisionNo              = 1;
inline constexpr int          kMaxRevisionNo              = 100000;
inline constexpr std::size_t  kMaxSummaryLength           = 200;
inline constexpr std::size_t  kMinEditorUsernameLength    = 1;
inline constexpr std::size_t  kMaxEditorUsernameLength    = 50;
inline constexpr std::size_t  kMinRevisionDescriptionLen  = 1;
inline constexpr std::size_t  kMaxRevisionDescriptionLen  = 16 * 1024 * 1024;   // 16MB
inline constexpr std::size_t  kMinRevisionSlugLength      = 1;
inline constexpr std::size_t  kMaxRevisionSlugLength      = 100;
inline constexpr std::size_t  kMinRevisionTitleLength     = 1;
inline constexpr std::size_t  kMaxRevisionTitleLength     = 200;
inline constexpr int          kMinRevisionTimeLimitMs     = 1;
inline constexpr int          kMaxRevisionTimeLimitMs     = 60000;
inline constexpr int          kMinRevisionMemoryLimitMb   = 1;
inline constexpr int          kMaxRevisionMemoryLimitMb   = 1024;
inline constexpr std::size_t  kMinRevisionDifficultyLen   = 3;   // "easy"
inline constexpr std::size_t  kMaxRevisionDifficultyLen   = 6;   // "medium"
inline constexpr int          kDefaultRevisionListLimit   = 20;
inline constexpr int          kMaxRevisionListLimit       = 100;

// ────────────────────────────────────────────────────────────────────────────
//  Validators
//
//  Each validator has signature `bool validate_xxx(T v, std::string* err)`,
//  writing a human-readable reason into *err when it returns false, and
//  returning false on length or control-character violations. The route
//  layer is expected to have validated first; these exist as defense in
//  depth so garbage in the table ⇒ an exception, never a corrupt row.
// ────────────────────────────────────────────────────────────────────────────

namespace problem_revisions_repo {

inline bool validate_revision_no(int v,
                                 std::string* error_out = nullptr) {
    // 0 is the "let the repo allocate MAX+1" sentinel — accepted.
    // Anything <0 is rejected outright (negative revision_no is
    // never meaningful). For v in [kMinRevisionNo, kMaxRevisionNo]
    // we enforce the SPEC §4.2e bound.
    if (v < 0) {
        if (error_out) {
            *error_out = "revision_no must not be negative";
        }
        return false;
    }
    if (v == 0) return true;        // sentinel
    if (v < kMinRevisionNo || v > kMaxRevisionNo) {
        if (error_out) {
            *error_out = "revision_no must be in [1, " +
                         std::to_string(kMaxRevisionNo) +
                         "] or 0 to let the repo allocate MAX+1";
        }
        return false;
    }
    return true;
}

inline bool validate_summary(std::string_view s,
                             std::string* error_out = nullptr) {
    if (s.size() > kMaxSummaryLength) {
        if (error_out) {
            *error_out = "summary length must be <= " +
                         std::to_string(kMaxSummaryLength);
        }
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "summary must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_action(std::string_view action,
                            std::string* error_out = nullptr) {
    // Tighter than audit_log_repo::validate_action: we restrict to the
    // two values we actually write today. Future 'delete' / 'restore'
    // would widen this — update V009 + this validator + admin write
    // paths in one PR. We don't accept 'insert' / 'set' synonyms.
    //
    // We deliberately hardcode "create"/"update" here instead of
    // referencing kActionRevisionCreate / kActionRevisionUpdate, because
    // those constants are declared further down in the same namespace
    // (so the validator block stays self-contained). Keep the literal
    // and the constant in lock-step — there's a pin test that asserts
    // both share the same value.
    if (action != "create" && action != "update") {
        if (error_out) {
            *error_out = "action must be 'create' or 'update'";
        }
        return false;
    }
    return true;
}

inline bool validate_editor_username(std::string_view s,
                                     std::string* error_out = nullptr) {
    if (s.size() < kMinEditorUsernameLength ||
        s.size() > kMaxEditorUsernameLength) {
        if (error_out) {
            *error_out = "editor_username length must be between " +
                         std::to_string(kMinEditorUsernameLength) + " and " +
                         std::to_string(kMaxEditorUsernameLength);
        }
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "editor_username must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_slug(std::string_view s,
                          std::string* error_out = nullptr) {
    if (s.size() < kMinRevisionSlugLength || s.size() > kMaxRevisionSlugLength) {
        if (error_out) {
            *error_out = "slug length must be between " +
                         std::to_string(kMinRevisionSlugLength) + " and " +
                         std::to_string(kMaxRevisionSlugLength);
        }
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "slug must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_title(std::string_view s,
                           std::string* error_out = nullptr) {
    if (s.size() < kMinRevisionTitleLength || s.size() > kMaxRevisionTitleLength) {
        if (error_out) {
            *error_out = "title length must be between " +
                         std::to_string(kMinRevisionTitleLength) + " and " +
                         std::to_string(kMaxRevisionTitleLength);
        }
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "title must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_difficulty(std::string_view d,
                                std::string* error_out = nullptr) {
    if (d != "easy" && d != "medium" && d != "hard") {
        if (error_out) {
            *error_out = "difficulty must be one of: easy, medium, hard";
        }
        return false;
    }
    return true;
}

inline bool validate_time_limit(int ms,
                                std::string* error_out = nullptr) {
    if (ms < kMinRevisionTimeLimitMs || ms > kMaxRevisionTimeLimitMs) {
        if (error_out) {
            *error_out = "time_limit must be between " +
                         std::to_string(kMinRevisionTimeLimitMs) + " and " +
                         std::to_string(kMaxRevisionTimeLimitMs);
        }
        return false;
    }
    return true;
}

inline bool validate_memory_limit(int mb,
                                  std::string* error_out = nullptr) {
    if (mb < kMinRevisionMemoryLimitMb || mb > kMaxRevisionMemoryLimitMb) {
        if (error_out) {
            *error_out = "memory_limit must be between " +
                         std::to_string(kMinRevisionMemoryLimitMb) + " and " +
                         std::to_string(kMaxRevisionMemoryLimitMb);
        }
        return false;
    }
    return true;
}

inline bool validate_description(std::string_view s,
                                 std::string* error_out = nullptr) {
    if (s.size() < kMinRevisionDescriptionLen ||
        s.size() > kMaxRevisionDescriptionLen) {
        if (error_out) {
            *error_out = "description length must be between " +
                         std::to_string(kMinRevisionDescriptionLen) + " and " +
                         std::to_string(kMaxRevisionDescriptionLen);
        }
        return false;
    }
    for (unsigned char c : s) {
        // Markdown descriptions legitimately contain \n / \r / \t;
        // reject only NUL / DEL and the truly broken control chars
        // (0x01-0x08, 0x0B-0x0C, 0x0E-0x1F) that can wreck a
        // terminal / log viewer.
        if (c == 0x00 || c == 0x7F
            || (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D)) {
            if (error_out) {
                *error_out = "description must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

inline bool validate_ip(std::string_view ip,
                        std::string* error_out = nullptr) {
    if (ip.empty()) {
        // IP is optional in this repo (mirrors audit_logs.ip); an empty
        // string means "no editor IP captured" rather than invalid.
        return true;
    }
    if (ip.size() > 45) {
        if (error_out) {
            *error_out = "editor_ip length must be <= 45";
        }
        return false;
    }
    for (unsigned char c : ip) {
        if (c < 0x20 || c == 0x7F) {
            if (error_out) {
                *error_out = "editor_ip must not contain control characters";
            }
            return false;
        }
    }
    return true;
}

// (clamp_list_filter is declared in the inline section after
//  RevisionListFilter is fully defined below; placing it before
//  RevisionListFilter would force a forward-declared type that
//  shadows the real one and breaks .limit / .offset access.)

} // namespace problem_revisions_repo

// ────────────────────────────────────────────────────────────────────────────
//  Action constants — grep-friendly strings for the well-known actions.
//  These live inside `problem_revisions_repo` so callers write
//  `problem_revisions_repo::kActionRevisionCreate`. The validator
//  (`validate_action` above) intentionally uses literal "create" /
//  "update" instead of these constants because the constants are
//  defined after the validator block in the source — keep the
//  literal/constant pair in lockstep if you ever add a new action.
// ────────────────────────────────────────────────────────────────────────────

namespace problem_revisions_repo {

inline constexpr const char* kActionRevisionCreate = "create";
inline constexpr const char* kActionRevisionUpdate = "update";

} // namespace problem_revisions_repo

// ────────────────────────────────────────────────────────────────────────────
//  RevisionEntry — input to record()
//
//  All snapshot fields are populated by the route handler from the
//  in-memory `row` / `tags_v` / `samples_v` / pre-patch state. We
//  deliberately do NOT issue a DB round-trip to materialize the
//  snapshot — the request payload is the canonical input.
//
//  `revision_no = 0` is the "let the repo allocate MAX+1" sentinel;
//  any value >0 forces a specific version (used by tests that want
//  to deterministically probe the UNIQUE 1062 path).
// ────────────────────────────────────────────────────────────────────────────

struct RevisionEntry {
    int                          problem_id       = 0;
    int                          revision_no      = 0;       // 0 ⇒ repo allocates MAX+1
    std::optional<int>           editor_id;
    std::string                  editor_username;
    std::optional<std::string>   editor_ip;
    std::string                  action;                    // "create" | "update"
    std::string                  slug;
    std::string                  title;
    std::string                  difficulty;                // "easy" | "medium" | "hard"
    int                          time_limit       = 0;
    int                          memory_limit     = 0;
    std::string                  description;
    nlohmann::json               tags_snapshot    = nlohmann::json::array();
    nlohmann::json               samples_snapshot = nlohmann::json::array();
    std::optional<std::string>   summary;                    // ≤ 200 chars, no control
};

// ────────────────────────────────────────────────────────────────────────────
//  RevisionRow — projection of one row, materialized by row_to_revision()
//
//  Mirrors RevisionEntry plus the BIGINT PK `id` and the DATE_FORMAT'd
//  `created_at` text. The two JSON snapshot fields are returned as raw
//  std::string (JSON text) so callers can re-parse via
//  nlohmann::json::parse at the route response boundary.
// ────────────────────────────────────────────────────────────────────────────

struct RevisionRow {
    std::int64_t                 id               = 0;
    int                          problem_id       = 0;
    int                          revision_no      = 0;
    std::optional<int>           editor_id;
    std::string                  editor_username;
    std::optional<std::string>   editor_ip;
    std::string                  action;
    std::string                  slug;
    std::string                  title;
    std::string                  difficulty;
    int                          time_limit       = 0;
    int                          memory_limit     = 0;
    std::string                  description;
    std::string                  tags_snapshot;             // JSON text
    std::string                  samples_snapshot;          // JSON text
    std::optional<std::string>   summary;
    std::string                  created_at;                // 'YYYY-MM-DD HH:MM:SS'
};

// ────────────────────────────────────────────────────────────────────────────
//  RevisionListFilter / RevisionListResult — list_for_problem() inputs/outputs
// ────────────────────────────────────────────────────────────────────────────

struct RevisionListFilter {
    int                          problem_id       = 0;       // required
    int                          limit            = kDefaultRevisionListLimit;
    int                          offset           = 0;
};

struct RevisionListResult {
    std::vector<RevisionRow>     items;
    std::int64_t                 total            = 0;
    int                          limit            = 0;
    int                          offset           = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
// ────────────────────────────────────────────────────────────────────────────

class ProblemRevisionsRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ProblemRevisionsNotFoundError : public ProblemRevisionsRepoError {
public:
    using ProblemRevisionsRepoError::ProblemRevisionsRepoError;
};

class ProblemRevisionsConflictError : public ProblemRevisionsRepoError {
public:
    using ProblemRevisionsRepoError::ProblemRevisionsRepoError;
};

namespace problem_revisions_repo {

// ────────────────────────────────────────────────────────────────────────────
//  detail:: row materialization helpers + kRevisionSelectColumns
//
//  Same shape as audit_log_repo::detail — namespaced here to dodge ODR
//  collisions when both headers land in the same TU.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::int64_t req_int64(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::int64_t>();
    } catch (const std::exception& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo: required field '") +
            field + "' is not an int64: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo: required field '") +
            field + "' is not an int: " + e.what());
    }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try {
        return static_cast<int>(v.get<std::int64_t>());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

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
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo: required field '") +
            field + "' is not a string: " + e.what());
    }
}

// Column order for the problem_revisions projection. Centralized so
// a V009 schema change is a one-liner.
//
// `tags_snapshot` / `samples_snapshot` are JSON-typed and use the same
// CAST AS CHAR workaround as audit_log_repo for mysql-connector 9.x.
// `created_at` is DATE_FORMAT'd to dodge the 9.x DATETIME bug.
//
//   0  id               (BIGINT)
//   1  problem_id       (INT)
//   2  revision_no      (INT)
//   3  editor_id        (INT NULL)
//   4  editor_username  (VARCHAR(50))
//   5  editor_ip        (VARCHAR(45) NULL)
//   6  action           (VARCHAR(20))
//   7  slug             (VARCHAR(100))
//   8  title            (VARCHAR(200))
//   9  difficulty       (VARCHAR(10))
//  10  time_limit       (INT)
//  11  memory_limit     (INT)
//  12  description      (MEDIUMTEXT)
//  13  tags_snapshot    (JSON → CAST AS CHAR)
//  14  samples_snapshot (JSON → CAST AS CHAR)
//  15  summary          (VARCHAR(200) NULL)
//  16  created_at       (DATETIME → DATE_FORMAT string)
inline constexpr const char* kRevisionSelectColumns =
    "id, problem_id, revision_no, editor_id, editor_username, editor_ip, "
    "action, slug, title, difficulty, time_limit, memory_limit, "
    "description, "
    "CAST(tags_snapshot AS CHAR)    AS tags_snapshot, "
    "CAST(samples_snapshot AS CHAR) AS samples_snapshot, "
    "summary, "
    "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at";

inline RevisionRow row_to_revision(const mysqlx::Row& row) {
    RevisionRow r;
    r.id              = req_int64 (row, 0, "id");
    r.problem_id      = req_int   (row, 1, "problem_id");
    r.revision_no     = req_int   (row, 2, "revision_no");
    r.editor_id       = opt_int   (row, 3);
    r.editor_username = req_string(row, 4, "editor_username");
    r.editor_ip       = opt_string(row, 5);
    r.action          = req_string(row, 6, "action");
    r.slug            = req_string(row, 7, "slug");
    r.title           = req_string(row, 8, "title");
    r.difficulty      = req_string(row, 9, "difficulty");
    r.time_limit      = req_int   (row, 10, "time_limit");
    r.memory_limit    = req_int   (row, 11, "memory_limit");
    r.description     = req_string(row, 12, "description");
    r.tags_snapshot     = req_string(row, 13, "tags_snapshot");
    r.samples_snapshot  = req_string(row, 14, "samples_snapshot");
    r.summary         = opt_string(row, 15);
    r.created_at      = req_string(row, 16, "created_at");
    return r;
}

// Heuristic: does this mysqlx::Error look like a UNIQUE 1062?
// We can't reliably downcast; the mysql-connector-c++ 9.x Error
// message embeds "Duplicate entry" and/or "1062" for that case.
inline bool looks_like_dup_key_error(const std::string& what) {
    return what.find("Duplicate entry") != std::string::npos
        || what.find("duplicate")       != std::string::npos
        || what.find("1062")            != std::string::npos;
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  clamp_list_filter — out-of-line to keep the header readable
// ────────────────────────────────────────────────────────────────────────────

inline void clamp_list_filter(RevisionListFilter& f) {
    if (f.limit  <= 0)                  f.limit  = kDefaultRevisionListLimit;
    if (f.limit  > kMaxRevisionListLimit) f.limit  = kMaxRevisionListLimit;
    if (f.offset < 0)                   f.offset = 0;
}

// ────────────────────────────────────────────────────────────────────────────
//  record — strict INSERT into problem_revisions
//
//  * Validates every field as defense in depth (the route handler is
//    expected to have validated first; this is belt + suspenders).
//  * revision_no = 0 ⇒ repo allocates MAX+1 atomically via a single
//    INSERT ... SELECT COALESCE(MAX(revision_no),0)+1 ... WHERE
//    problem_id=?. UNIQUE (problem_id, revision_no) is the safety net
//    for concurrent writers; on 1062 we retry once; a second 1062
//    escalates to ProblemRevisionsConflictError.
//  * revision_no > 0 ⇒ caller is forcing a specific version (used by
//    the 1062 forced-collision test). We still pass it through verbatim;
//    on a 1062 we retry once, then throw Conflict.
//
//  Returns the new BIGINT id (>0) on success. Throws:
//    - ProblemRevisionsRepoError      (driver / validation / generic)
//    - ProblemRevisionsConflictError  (1062 exhausted)
// ────────────────────────────────────────────────────────────────────────────

inline std::int64_t record(ConnectionPool& pool, const RevisionEntry& e) {
    // 1) Defense-in-depth validation
    {
        std::string err;
        if (!validate_action         (e.action,                          &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_slug           (e.slug,                            &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_title          (e.title,                           &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_difficulty     (e.difficulty,                      &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_time_limit     (e.time_limit,                      &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_memory_limit   (e.memory_limit,                    &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_editor_username(e.editor_username,                 &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (!validate_description    (e.description,                     &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (e.editor_ip.has_value() &&
            !validate_ip             (*e.editor_ip,                      &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (e.summary.has_value() &&
            !validate_summary        (*e.summary,                        &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
        if (e.revision_no != 0 &&
            !validate_revision_no    (e.revision_no,                     &err)) {
            throw ProblemRevisionsRepoError("record: " + err);
        }
    }

    // 2) Materialize bind-side strings.
    //    `revision_no=0` in the entry maps to SQL `MAX(revision_no)+1`
    //    via the INSERT...SELECT below, so we don't need to pre-compute
    //    it here. We bind a sentinel -1 to the placeholder only when
    //    the caller forced a specific version.
    const int         forced_rev_no = e.revision_no;   // 0 ⇒ use MAX+1
    const std::string tags_j        = e.tags_snapshot.dump();
    const std::string samples_j     = e.samples_snapshot.dump();
    const std::string summary_s     = e.summary.value_or(std::string());
    const std::string ip_s          = e.editor_ip.value_or(std::string());

    // Sanity check on JSON snapshot lengths (mysqlx TEXT/VARCHAR limit).
    if (tags_j.size() > kMaxRevisionDescriptionLen) {
        throw ProblemRevisionsRepoError(
            "record: tags_snapshot serialization exceeds 16MB");
    }
    if (samples_j.size() > kMaxRevisionDescriptionLen) {
        throw ProblemRevisionsRepoError(
            "record: samples_snapshot serialization exceeds 16MB");
    }

    auto conn = pool.acquire();
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            // Two SQL paths — one for caller-forced revision_no
            // (used by the 1062 forced-collision integration test)
            // and one for the common "let the repo allocate
            // MAX+1" path. We split them into two separate
            // execute() blocks rather than try to share bind()
            // chains across both shapes, because the bind count
            // is the same (15) but the SELECT-branch's WHERE
            // placeholder reuses problem_id.
            if (forced_rev_no > 0) {
                // VALUES path — 15 placeholders, all bound below.
                mysqlx::SqlStatement stmt = conn.session().sql(
                    "INSERT INTO problem_revisions "
                    "(problem_id, revision_no, editor_id, editor_username, editor_ip, "
                    " action, slug, title, difficulty, time_limit, memory_limit, "
                    " description, tags_snapshot, samples_snapshot, summary) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
                stmt.bind(static_cast<std::int64_t>(e.problem_id));   // 1
                stmt.bind(static_cast<std::int64_t>(forced_rev_no)); // 2
                // 3 editor_id (optional)
                if (e.editor_id.has_value())
                    stmt.bind(static_cast<std::int64_t>(*e.editor_id));
                else
                    stmt.bind(mysqlx::Value(nullptr));
                stmt.bind(e.editor_username);                         // 4
                // 5 editor_ip (optional)
                if (e.editor_ip.has_value()) stmt.bind(ip_s); else stmt.bind(mysqlx::Value(nullptr));
                stmt.bind(e.action);                                  // 6
                stmt.bind(e.slug);                                    // 7
                stmt.bind(e.title);                                   // 8
                stmt.bind(e.difficulty);                              // 9
                stmt.bind(e.time_limit);                              // 10
                stmt.bind(e.memory_limit);                            // 11
                stmt.bind(e.description);                             // 12
                stmt.bind(tags_j);                                    // 13
                stmt.bind(samples_j);                                 // 14
                // 15 summary (optional)
                if (e.summary.has_value()) stmt.bind(summary_s); else stmt.bind(mysqlx::Value(nullptr));

                mysqlx::SqlResult rs = stmt.execute();
                return rs.getAutoIncrementValue();
            }

            // INSERT...SELECT path — let the SQL compute MAX(revision_no)+1.
            // The SELECT's column list is 14 placeholders (col 2 is the
            // computed MAX+1 — not bound). The trailing WHERE has 1
            // additional placeholder reusing problem_id. Total = 15.
            mysqlx::SqlStatement stmt = conn.session().sql(
                "INSERT INTO problem_revisions "
                "(problem_id, revision_no, editor_id, editor_username, editor_ip, "
                " action, slug, title, difficulty, time_limit, memory_limit, "
                " description, tags_snapshot, samples_snapshot, summary) "
                "SELECT ?, COALESCE(MAX(revision_no),0)+1, "
                "       ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
                "FROM problem_revisions WHERE problem_id = ?");
            stmt.bind(static_cast<std::int64_t>(e.problem_id));   // 1 → problem_id col
            // 2 → revision_no computed by SELECT (MAX+1) — not bound
            // 3 → editor_id (optional)
            if (e.editor_id.has_value())
                stmt.bind(static_cast<std::int64_t>(*e.editor_id));
            else
                stmt.bind(mysqlx::Value(nullptr));
            stmt.bind(e.editor_username);                         // 4
            // 5 → editor_ip (optional)
            if (e.editor_ip.has_value()) stmt.bind(ip_s); else stmt.bind(mysqlx::Value(nullptr));
            stmt.bind(e.action);                                  // 6
            stmt.bind(e.slug);                                    // 7
            stmt.bind(e.title);                                   // 8
            stmt.bind(e.difficulty);                              // 9
            stmt.bind(e.time_limit);                              // 10
            stmt.bind(e.memory_limit);                            // 11
            stmt.bind(e.description);                             // 12
            stmt.bind(tags_j);                                    // 13
            stmt.bind(samples_j);                                 // 14
            // 15 → summary (optional)
            if (e.summary.has_value()) stmt.bind(summary_s); else stmt.bind(mysqlx::Value(nullptr));
            // 16 → WHERE problem_id (re-uses e.problem_id, 16th placeholder)
            stmt.bind(static_cast<std::int64_t>(e.problem_id));

            mysqlx::SqlResult rs = stmt.execute();
            return rs.getAutoIncrementValue();
        } catch (const ProblemRevisionsRepoError&) {
            throw;
        } catch (const mysqlx::Error& ex) {
            const std::string what = ex.what();
            if (detail::looks_like_dup_key_error(what)) {
                if (attempt == 0) continue;
                throw ProblemRevisionsConflictError(
                    std::string("problem_revisions_repo::record: "
                                "revision_no collision after retry: ") + what);
            }
            throw ProblemRevisionsRepoError(
                std::string("problem_revisions_repo::record: ") + what);
        } catch (const std::exception& ex) {
            throw ProblemRevisionsRepoError(
                std::string("problem_revisions_repo::record: ") + ex.what());
        }
    }
    // Loop exits only via return or throw; this is for static analysis.
    throw ProblemRevisionsRepoError(
        "problem_revisions_repo::record: retry exhausted (unreachable)");
}

// ────────────────────────────────────────────────────────────────────────────
//  record_best_effort — LOG_WARN + swallow; returns 0 on failure
// ────────────────────────────────────────────────────────────────────────────

inline std::int64_t record_best_effort(ConnectionPool& pool,
                                       const RevisionEntry& e) {
    try {
        return record(pool, e);
    } catch (const std::exception& ex) {
        LOG_WARN("problem_revisions_repo::record_best_effort failed",
                 {{"problem_id", std::to_string(e.problem_id)},
                  {"action",     e.action},
                  {"reason",     ex.what()}});
        return 0;
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  find_by_id — fetch a single revision by BIGINT PK. nullopt if not found.
// ────────────────────────────────────────────────────────────────────────────

inline std::optional<RevisionRow> find_by_id(ConnectionPool& pool,
                                            std::int64_t id) {
    auto conn = pool.acquire();
    try {
        std::optional<mysqlx::Row> row = conn.fetch_one(
            std::string("SELECT ") + detail::kRevisionSelectColumns +
            " FROM problem_revisions WHERE id = ?",
            static_cast<std::int64_t>(id));
        if (!row) return std::nullopt;
        return detail::row_to_revision(*row);
    } catch (const mysqlx::Error& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo::find_by_id: ") + e.what());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  latest_for_problem — most-recent revision of one problem.
//
//  Used by the v1.3 reader to render "what does this problem look like
//  right now?". Returns nullopt when the problem has zero revisions
//  (which today would only happen for problems that slipped through the
//  V009 cutover — every create / update after v1.2.12 lands a row).
// ────────────────────────────────────────────────────────────────────────────

inline std::optional<RevisionRow> latest_for_problem(ConnectionPool& pool,
                                                     int problem_id) {
    auto conn = pool.acquire();
    try {
        std::optional<mysqlx::Row> row = conn.fetch_one(
            std::string("SELECT ") + detail::kRevisionSelectColumns +
            " FROM problem_revisions WHERE problem_id = ?"
            " ORDER BY revision_no DESC LIMIT 1",
            static_cast<std::int64_t>(problem_id));
        if (!row) return std::nullopt;
        return detail::row_to_revision(*row);
    } catch (const mysqlx::Error& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo::latest_for_problem: ")
            + e.what());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  list_for_problem — paginated revisions of one problem, newest first.
//
//  `filter.problem_id` is required; `limit` / `offset` are clamped.
//  Returns total via COUNT(*) over the same predicate so the v1.3
//  reader's pagination UI has a "page N of M" without a second
//  round-trip.
// ────────────────────────────────────────────────────────────────────────────

inline RevisionListResult list_for_problem(ConnectionPool& pool,
                                           RevisionListFilter filter) {
    clamp_list_filter(filter);
    RevisionListResult result;
    result.limit  = filter.limit;
    result.offset = filter.offset;

    auto conn = pool.acquire();
    try {
        // total
        {
            std::optional<mysqlx::Row> row = conn.fetch_one(
                "SELECT COUNT(*) AS n FROM problem_revisions"
                " WHERE problem_id = ?",
                static_cast<std::int64_t>(filter.problem_id));
            if (row) {
                result.total = (*row)[0].get<std::int64_t>();
            }
        }
        // page — drive the result via mysqlx::SqlResult's range-for,
        // the same pattern audit_log_repo::list uses. PooledConnection
        // does not expose a fetch_all surface; we go through the
        // session's bind/execute path.
        {
            mysqlx::SqlStatement stmt = conn.session().sql(
                std::string("SELECT ") + detail::kRevisionSelectColumns +
                " FROM problem_revisions"
                " WHERE problem_id = ?"
                " ORDER BY revision_no DESC, id DESC"
                " LIMIT ? OFFSET ?");
            stmt.bind(static_cast<std::int64_t>(filter.problem_id));
            stmt.bind(filter.limit);
            stmt.bind(filter.offset);
            mysqlx::SqlResult rs = stmt.execute();
            // Reasonable upper bound; we cap by kMaxRevisionListLimit.
            result.items.reserve(static_cast<std::size_t>(filter.limit));
            for (auto row : rs) {
                result.items.push_back(detail::row_to_revision(row));
            }
        }
        return result;
    } catch (const mysqlx::Error& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo::list_for_problem: ")
            + e.what());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  count_for_problem — single count, used by tests + future "edit count"
//  pill on the admin dashboard.
// ────────────────────────────────────────────────────────────────────────────

inline std::int64_t count_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        std::optional<mysqlx::Row> row = conn.fetch_one(
            "SELECT COUNT(*) AS n FROM problem_revisions WHERE problem_id = ?",
            static_cast<std::int64_t>(problem_id));
        if (!row) return 0;
        return (*row)[0].get<std::int64_t>();
    } catch (const mysqlx::Error& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo::count_for_problem: ")
            + e.what());
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  delete_for_problem — best-effort hard-delete used by tests for cleanup.
//
//  Real production paths NEVER call this: rows are append-only and live
//  until the problem itself is hard-deleted (FK CASCADE) or someone
//  runs the future v1.3 "scrub revisions older than N years" job.
//  Exposed only so test fixtures can wipe revisions between cases.
// ────────────────────────────────────────────────────────────────────────────

inline void delete_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        conn.session().sql(
            "DELETE FROM problem_revisions WHERE problem_id = ?")
            .bind(static_cast<std::int64_t>(problem_id))
            .execute();
    } catch (const mysqlx::Error& e) {
        throw ProblemRevisionsRepoError(
            std::string("problem_revisions_repo::delete_for_problem: ")
            + e.what());
    }
}

} // namespace problem_revisions_repo

} // namespace litecode
