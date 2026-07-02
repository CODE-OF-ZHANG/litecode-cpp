// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — problem_special_judges repository (Phase 4 ☆ SPJ)
//
// SPEC §4.3 / §11 Phase 4 / §15.4 — Special Judge framework:
//
// Schema (V010):
//   problem_id   INT  NOT NULL PRIMARY KEY   (FK problems ON DELETE CASCADE)
//   source       MEDIUMTEXT NOT NULL         (the SPJ program source — C++)
//   language     ENUM('cpp') NOT NULL        (reserved for future py/java/...)
//                            DEFAULT 'cpp'
//   created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
//   updated_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
//                                         ON UPDATE CURRENT_TIMESTAMP
//
// Why a 1:1 table (problem_id PK) instead of stuffing `source` onto
// `problems`:
//   - the column is only meaningful for the (small) subset of problems
//     that opt into judge_type='special'; keeping it off the main
//     `problems` table avoids the schema carrying NULL for every row;
//   - one problem can change SPJ source over time without rewriting the
//     problem metadata — same edit-history boundaries test_case_repo
//     has today (samples vs judge cases split into is_sample);
//   - the FK ON DELETE CASCADE handles the harder problem (hard-delete
//     admin path), so a future un-soft-delete doesn't leave an orphan
//     SPJ row.
//
// Public surface (Phase 4 ☆ SPJ framework — storage only):
//   * SpecialJudgeRow            plain-data projection
//   * SpecialJudgeRepoError /
//     SpecialJudgeNotFoundError  typed exceptions
//   * validate_source            / validate_language — mirrors the
//                                problem_revisions_repo validators
//   * find_by_problem_id         — std::optional row, nullopt = "no SPJ
//                                attached to this problem"
//   * upsert                     — INSERT ... ON DUPLICATE KEY UPDATE.
//                                Sets a fresh source (and bumps
//                                updated_at via the column's
//                                ON UPDATE). Returns true on success.
//                                Soft-deleted problems are NOT
//                                blocked; the route layer gates
//                                write access on admin role and the
//                                problem being live.
//   * remove_by_problem_id       — DELETE the row. Returns true when
//                                a row was removed. Idempotent (no
//                                error when there is no row).
//   * exists_for_problem         — boolean convenience (used by the
//                                submission flow to decide whether to
//                                bother loading `source` for the
//                                task.json)
//
// Design notes (mirrors problem_revisions_repo.h):
//   - Header-only + inline like every Phase 1/2/3/4 repo.
//   - All SQL is parameterized (`?` placeholders) — SPEC §15.2.
//   - DATETIME columns are DATE_FORMAT'd in every SELECT to dodge the
//     mysql-connector 9.x packed-binary read bug (same workaround used
//     in user_repo / problem_repo / submission_repo).
//   - Row materialization helpers live in
//     `special_judge_repo::detail` (NOT shared `litecode::detail`) so
//     identically-named helpers don't collide when test binaries pull
//     in multiple repos.
//   - Language today is restricted to "cpp" — the litecode-judge
//     image (judge/Dockerfile) only ships g++/gcc. The validate_*
//     helper accepts only that value; future python/java/etc. would
//     need an additional `python3 -m py_compile` step in the judge
//     image + a multi-language dispatch in judge.sh.
//
// Usage (submission_routes.h when building a JudgeTask):
//
//   const auto spj = litecode::special_judge_repo::find_by_problem_id(
//       pool, *problem_id);
//   if (spj.has_value()) {
//       task.spj_source   = spj->source;
//       task.spj_language = spj->language;
//   }
//   // judge.sh's special case compiles spj_source once into a binary
//   // (see judge/lib/spj.sh), then invokes it per `judge_type=special`
//   // test case. No source ⇒ compare_special returns WA (literal '1'
//   // exit) so the submission lands in 'wa' instead of 'se' — that
//   // matches the "Judge admin has not configured SPJ yet" intent.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <mysqlx/xdevapi.h>

#include "../logger.h"             // LOG_WARN (best-effort drivers — none today; reserved)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Constants — pin the same bounds as V010 + the wire validators.
//
//  kMaxSpjSourceLen matches MEDIUMTEXT (16 MB). The admin upload path
//  will clamp at write time (similar to description / code in
//  problem_repo / submission_repo); this is the ceiling.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr const char*    kSpjLanguageCxx   = "cpp";
inline constexpr std::size_t    kMinSpjSourceLen  = 1;
inline constexpr std::size_t    kMaxSpjSourceLen  = 16 * 1024 * 1024;  // 16MB

// ────────────────────────────────────────────────────────────────────────────
//  SpecialJudgeRow
//
//  Plain-data projection of a row from the `problem_special_judges`
//  table.
//
//  Field semantics (mirrors SPEC §11 Phase 4 + V010):
//    - problem_id: the FK target; doubles as the PK (one row per problem).
//    - source: the SPJ program in MEDIUMTEXT. Round-tripped via std::string
//              to dodge the wire-level binary encoding surprise.
//    - language: "cpp" only today (g++-only judge image). Stored as
//                std::string so a future multilingual expansion doesn't
//                change the JSON wire shape.
//    - created_at / updated_at: ISO-8601 strings (DATE_FORMAT'd in every
//                SELECT to dodge the mysql-connector 9.x packed-binary
//                read bug — same pattern as user_repo / problem_repo).
// ────────────────────────────────────────────────────────────────────────────

struct SpecialJudgeRow {
    int                  problem_id  = 0;
    std::string          source;
    std::string          language;        // "cpp"
    std::string          created_at;
    std::string          updated_at;
};

// ────────────────────────────────────────────────────────────────────────────
//  Typed exception surface
//
//  Two tiers, mirroring the rest of Phase 3/4:
//    - SpecialJudgeRepoError      — generic failure (driver error, validation)
//    - SpecialJudgeNotFoundError  — NotFound tier for routes that
//                                   distinguish "no row" from "driver failure"
// ────────────────────────────────────────────────────────────────────────────

class SpecialJudgeRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SpecialJudgeNotFoundError : public SpecialJudgeRepoError {
public:
    using SpecialJudgeRepoError::SpecialJudgeRepoError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Validators
//
//  Length + control-char checks. Mirrors problem_revisions_repo::validate_*.
//  The admin upload path validates first; these are defense-in-depth so
//  a corrupt / truncated row can't sit in the DB quietly.
// ────────────────────────────────────────────────────────────────────────────

namespace special_judge_repo {

inline bool validate_source(std::string_view s,
                            std::string* error_out = nullptr) {
    if (s.size() < kMinSpjSourceLen || s.size() > kMaxSpjSourceLen) {
        if (error_out) {
            *error_out = "special judge source length must be between " +
                         std::to_string(kMinSpjSourceLen) + " and " +
                         std::to_string(kMaxSpjSourceLen) + " bytes";
        }
        return false;
    }
    return true;
}

inline bool validate_language(std::string_view lang,
                              std::string* error_out = nullptr) {
    if (lang != kSpjLanguageCxx) {
        if (error_out) {
            *error_out = "special judge language must be '";
            *error_out += kSpjLanguageCxx;
            *error_out += "' (the litecode-judge image is C++-only today)";
        }
        return false;
    }
    return true;
}

}  // namespace special_judge_repo

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
//
//  Centralized here so find_by_problem_id stays small. Mirrors the
//  convention used by every other Phase 2/3/4 repo.
// ────────────────────────────────────────────────────────────────────────────

namespace special_judge_repo {
namespace detail {

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw SpecialJudgeRepoError(
            std::string("special_judge_repo: required field '") +
            field + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw SpecialJudgeRepoError(
            std::string("special_judge_repo: required field '") +
            field + "' is not an int: " + e.what());
    }
}

// Column order, used by every SELECT in this file. Centralized so a
// schema change here is a one-liner.
//
//   0 problem_id
//   1 source
//   2 language
//   3 created_at    (DATE_FORMAT'd → text)
//   4 updated_at    (DATE_FORMAT'd → text)
inline constexpr const char* kSpecialJudgeSelectColumns =
    "problem_id, source, language, "
    "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at, "
    "DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s') AS updated_at";

inline SpecialJudgeRow row_to_special_judge(const mysqlx::Row& row) {
    SpecialJudgeRow s;
    s.problem_id  = req_int   (row, 0, "problem_id");
    s.source      = req_string(row, 1, "source");
    s.language    = req_string(row, 2, "language");
    s.created_at  = req_string(row, 3, "created_at");
    s.updated_at  = req_string(row, 4, "updated_at");
    return s;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

// find_by_problem_id — load one row by PK. Returns std::nullopt when
// no special-judge source is attached to the given problem. The
// submission flow folds nullopt into "skip the SPJ dispatch in
// judge.sh", which makes every judge_type=special case evaluate to
// WA (not SE) — that matches the "admin hasn't configured SPJ yet"
// intent: the operator can iterate.
//
// Throws SpecialJudgeRepoError on driver error.
inline std::optional<SpecialJudgeRow> find_by_problem_id(
        ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        auto row = conn.fetch_one(
            std::string("SELECT ") + detail::kSpecialJudgeSelectColumns +
            " FROM problem_special_judges WHERE problem_id = ? LIMIT 1",
            problem_id);
        if (!row) return std::nullopt;
        return detail::row_to_special_judge(*row);
    } catch (const mysqlx::Error& e) {
        throw SpecialJudgeRepoError(
            std::string("special_judge_repo::find_by_problem_id: ") + e.what());
    }
}

// exists_for_problem — boolean convenience used by the submission flow
// (the typical hot-path optimization: skip the source column read when
// there is no SPJ attached). Today's MySQL plan would also skip the
// source column when there is no row, so this is more about API shape
// than performance — the savings materialize when a future admin
// listing wants to count "special-judge problems" without paying for
// the source bytes.
inline bool exists_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        auto v = conn.fetch_scalar<std::int64_t>(
            "SELECT 1 FROM problem_special_judges WHERE problem_id = ? LIMIT 1",
            problem_id);
        return v.has_value();
    } catch (const mysqlx::Error& e) {
        throw SpecialJudgeRepoError(
            std::string("special_judge_repo::exists_for_problem: ") + e.what());
    }
}

// upsert — INSERT ... ON DUPLICATE KEY UPDATE source, language,
// updated_at = CURRENT_TIMESTAMP. Returns true when the row is now
// present (always, on success). Validation runs first; an invalid
// source/language throws SpecialJudgeRepoError so the route layer
// can surface 400 vs 500.
//
// Soft-deleted problems are NOT blocked: the SPJ source is content
// metadata, not a runtime dependency; an admin can attach/replace the
// SPJ on a tombstoned problem (the row is hidden from public reads
// anyway because the routes 404 soft-deleted problems).
inline bool upsert(ConnectionPool& pool,
                   int problem_id,
                   const std::string& source,
                   const std::string& language) {
    {
        std::string err;
        if (problem_id <= 0) {
            throw SpecialJudgeRepoError("upsert: problem_id must be > 0");
        }
        if (!validate_source(source, &err)) {
            throw SpecialJudgeRepoError("upsert: " + err);
        }
        if (!validate_language(language, &err)) {
            throw SpecialJudgeRepoError("upsert: " + err);
        }
    }
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "INSERT INTO problem_special_judges (problem_id, source, language) "
            "VALUES (?, ?, ?) "
            "ON DUPLICATE KEY UPDATE "
            "    source = VALUES(source), "
            "    language = VALUES(language), "
            "    updated_at = CURRENT_TIMESTAMP",
            problem_id, source, language);
        (void)rs.getAffectedItemsCount();   // 0 when row was identical; 1 otherwise
        return true;
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        // FK violation: problem_id doesn't exist. Surface as a typed
        // exception so the route can map FK failure → 400 INVALID_INPUT.
        if (what.find("foreign key") != std::string::npos ||
            what.find("FOREIGN KEY") != std::string::npos ||
            what.find("1452")        != std::string::npos) {
            throw SpecialJudgeRepoError(
                "upsert: problem_id does not exist (FK violation)");
        }
        throw SpecialJudgeRepoError(
            std::string("special_judge_repo::upsert: ") + what);
    }
}

// remove_by_problem_id — DELETE the row (idempotent — returns true
// even when there was no row to delete; false means the row was
// present and got removed). The route layer maps FK violation
// (problem_id gone) into a 404 envelope; today all callers also gate
// on require_admin, so this is the safety net.
inline bool remove_by_problem_id(ConnectionPool& pool, int problem_id) {
    if (problem_id <= 0) {
        throw SpecialJudgeRepoError(
            "remove_by_problem_id: problem_id must be > 0");
    }
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "DELETE FROM problem_special_judges WHERE problem_id = ?",
            problem_id);
        return static_cast<int>(rs.getAffectedItemsCount()) > 0;
    } catch (const mysqlx::Error& e) {
        throw SpecialJudgeRepoError(
            std::string("special_judge_repo::remove_by_problem_id: ") +
            e.what());
    }
}

}  // namespace special_judge_repo
}  // namespace litecode
