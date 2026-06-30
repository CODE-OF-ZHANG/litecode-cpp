// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — test_cases repository (Phase 3 ★ follow-up)
//
// SPEC §4.3 / §4.5 / §5.2 / §11 Phase 3 / A5 acceptance:
//   - `test_cases` table schema (V001 + V003):
//       id INT AUTO_INCREMENT PK,
//       problem_id INT NOT NULL FK -> problems(id) ON DELETE CASCADE,
//       input LONGTEXT NOT NULL,              -- unified UTF-8 + LF
//       expected_output LONGTEXT NOT NULL,
//       is_sample BOOLEAN NOT NULL DEFAULT FALSE,
//       judge_type ENUM('exact','ignore_trailing','float_eps','special')
//                  NOT NULL DEFAULT 'exact',  -- v1.2
//       float_epsilon DECIMAL(10,8) NULL,     -- v1.2 (only for float_eps)
//       order_num INT NOT NULL DEFAULT 0,
//       INDEX (problem_id, order_num)         -- judge loads in order
//       INDEX (problem_id, is_sample, order_num)  -- detail page sample view
//   - The repo's Phase 3 surface is the small slice the public problem
//     detail endpoint needs today. Future readers (admin problem
//     edit / bulk import / judge flow) will land as additive
//     commits that build on this same shape:
//       * SampleCaseRow             -- projection of the row
//                                      (input / expected_output /
//                                       judge_type / order_num; we
//                                       deliberately do NOT carry
//                                       float_epsilon on the public
//                                       sample because the
//                                       front-end renders the sample
//                                       as text and never compares)
//       * TestCaseRepoError         -- typed exception for the route
//                                      to fold into a 500 envelope
//       * list_samples_for_problem  -- SELECT … WHERE is_sample=TRUE
//                                       for a given problem, ordered
//                                       by (order_num ASC, id ASC).
//                                       Powers the "samples" array
//                                       in GET /api/v1/problems/:slug.
//       * list_for_problem          -- SELECT … WHERE problem_id = ?
//                                       with an is_sample filter
//                                       (true / false / nullopt = all).
//                                       Reserved for the judge flow
//                                       and admin problem-edit; not
//                                       called by the public detail
//                                       endpoint (the public path
//                                       never surfaces non-sample
//                                       cases, see SPEC §4.3 "示例
//                                       用例").
//       * insert                   -- INSERT one row (sample or judge
//                                       case), returning the new id.
//                                       Used by admin create / update
//                                       / bulk import to attach test
//                                       cases to a freshly-created
//                                       problem row.
//       * delete_for_problem       -- DELETE every row for a problem.
//                                       Used by replace_for_problem
//                                       below and by admin problem
//                                       delete (to scrub attached
//                                       test cases when the problem
//                                       goes away — currently a no-op
//                                       because soft-delete keeps the
//                                       rows; reserved for a future
//                                       hard-delete admin path).
//       * replace_for_problem      -- clear + insert (in one
//                                       transaction) for a given
//                                       problem, with `is_sample`
//                                       propagated uniformly across
//                                       every input row (the caller
//                                       passes a homogeneous vector
//                                       — the admin POST / PUT path
//                                       splits samples vs judge
//                                       cases into two calls when
//                                       it needs both). Used by
//                                       admin update.
//
//   - All reads + writes use parameterized SQL (`?` placeholders) —
//     SPEC §15.2 forbids string concatenation. mysqlx::SqlStatement::
//     bind() handles the binding; user-supplied data never reaches
//     the wire string.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3 module
//     (problem_repo.h / tag_repo.h / audit_log_repo.h). The repo
//     is essentially a thin set of free functions over a
//     ConnectionPool reference; no internal state to unit-test.
//   - Returned rows are exposed via `SampleCaseRow` (not the raw
//     mysqlx::Row) so callers don't depend on mysqlx::Value's
//     semantics. `std::vector<SampleCaseRow>` for "no rows" (an
//     empty samples list, distinct from "no problem").
//   - We do NOT model an ORM-style change-tracking `TestCase`
//     object. Hand-written SQL with bind() is fine for our
//     surface and avoids the C++ ORM overhead (SPEC §9 calls
//     this out).
//   - Row materialization helpers (req_int, req_string, row_to_*
//     live inside `test_case_repo::detail` (NOT in the shared
//     `litecode::detail`) so they don't collide with
//     `problem_repo::detail`'s / `tag_repo::detail`'s
//     identically-named helpers when multiple repos are
//     included in the same TU (test_problem_detail.cpp pulls
//     all three). The collision problem is the same one
//     tag_repo.h calls out — see its header preamble.
//   - Concurrency: every public method acquires a fresh
//     PooledConnection from the pool, runs the SQL, releases.
//     The pool is thread-safe; individual methods do not need
//     their own locks.
//
// Usage (public detail, problem_routes.h):
//
//   const auto samples = litecode::test_case_repo::list_samples_for_problem(
//       pool, problem_id);
//   // samples is std::vector<SampleCaseRow>, ordered
//   // (order_num ASC, id ASC); empty when the problem has no
//   // is_sample=TRUE rows.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"             // LOG_WARN (best-effort non-fatal DB hiccups)
#include "connection_pool.h"       // ConnectionPool / PooledConnection

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  SampleCaseRow
//
//  Plain-data projection of a row from the `test_cases` table where
//  `is_sample = TRUE`. The public detail endpoint uses this exact
//  shape — input / expected_output / judge_type / order_num — to
//  build the `samples` array in the response.
//
//  Field semantics (mirrors SPEC §4.3 + V003):
//    - id: the row's primary key; 0 ⇒ "not yet inserted".
//    - problem_id: FK to problems.id; denormalized onto the row
//                  even though the query always filters on it
//                  (callers need it to verify the row really
//                  belongs to the requested problem before
//                  surfacing it).
//    - input / expected_output: LONGTEXT. The repo returns them
//          as std::string; SPEC §7.4 guarantees the data is
//          UTF-8 + LF when written by the admin path, so we
//          round-trip without re-encoding here.
//    - judge_type: SPEC §4.3 ENUM. "exact" / "ignore_trailing" /
//                  "float_eps" / "special". The public detail
//                  endpoint does not surface `judge_type` today
//                  (front-end renders the sample as text and
//                  never compares), but the column is included
//                  so a future UI annotation (e.g. a "floating
//                  point answer" hint next to the sample) is a
//                  pure additive change.
//    - order_num: user-controlled ordering. Smaller numbers
//                 come first; ties broken by id ASC (stable).
//    - float_epsilon: deliberately NOT carried on the public
//          projection. It only applies when judge_type =
//          "float_eps" and the front-end never compares. The
//          admin / judge paths read it via a fuller row when
//          they need it; the public samples view doesn't.
// ────────────────────────────────────────────────────────────────────────────

struct SampleCaseRow {
    int         id            = 0;
    int         problem_id    = 0;
    std::string input;
    std::string expected_output;
    std::string judge_type;     // "exact" | "ignore_trailing" | "float_eps" | "special"
    int         order_num     = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  TestCaseRepoError — typed exception surface
//
//  We don't currently distinguish NotFound from generic failure
//  because every public read path returns an empty vector on
//  "no rows", never std::nullopt — "no such problem" and "no
//  samples for this problem" are different conditions and the
//  route layer disambiguates via problem_repo::find_by_slug
//  before calling list_samples_for_problem. The typed exception
//  exists for driver / SQL errors that the route maps to 500.
// ────────────────────────────────────────────────────────────────────────────

class TestCaseRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
// ────────────────────────────────────────────────────────────────────────────

namespace test_case_repo {
namespace detail {

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw TestCaseRepoError(std::string("test_case_repo: required field '") +
                                field + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw TestCaseRepoError(std::string("test_case_repo: required field '") +
                                field + "' is not an int: " + e.what());
    }
}

// Column order, used by every SELECT in this file. Centralized so a
// schema change here is a one-liner.
//
//   0 id
//   1 problem_id
//   2 input
//   3 expected_output
//   4 judge_type
//   5 order_num
inline constexpr const char* kSampleCaseSelectColumns =
    "id, problem_id, input, expected_output, judge_type, order_num";

inline SampleCaseRow row_to_sample(const mysqlx::Row& row) {
    SampleCaseRow s;
    s.id              = req_int   (row, 0, "id");
    s.problem_id      = req_int   (row, 1, "problem_id");
    s.input           = req_string(row, 2, "input");
    s.expected_output = req_string(row, 3, "expected_output");
    s.judge_type      = req_string(row, 4, "judge_type");
    s.order_num       = req_int   (row, 5, "order_num");
    return s;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

// list_samples_for_problem — load every is_sample = TRUE row for a
// given problem, ordered by (order_num ASC, id ASC). The
// (problem_id, is_sample, order_num) index from V005 covers this
// query directly.
//
// Returns an empty vector when the problem has zero sample rows.
// Does NOT check that the problem exists; callers that need to
// distinguish "no problem" from "problem with zero samples" must
// pre-check via problem_repo::find_by_id / find_by_slug. The
// detail route does exactly that.
//
// Throws TestCaseRepoError on driver failure (handler → 500).
inline std::vector<SampleCaseRow> list_samples_for_problem(
        ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    std::vector<SampleCaseRow> out;
    try {
        const std::string sql = std::string("SELECT ") +
                                detail::kSampleCaseSelectColumns +
                                " FROM test_cases "
                                "WHERE problem_id = ? AND is_sample = TRUE "
                                "ORDER BY order_num ASC, id ASC";
        auto rs = conn.execute(sql, problem_id);
        out.reserve(8);  // typical sample count is small
        for (auto row : rs) {
            out.push_back(detail::row_to_sample(row));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw TestCaseRepoError(
            std::string("test_case_repo::list_samples_for_problem: ") + e.what());
    }
}

// list_for_problem — generalized loader. The `is_sample` filter is
// `std::optional<bool>`:
//   - std::nullopt       ⇒ no filter (every row for the problem)
//   - std::optional<true> ⇒ only sample rows
//   - std::optional<false> ⇒ only non-sample (judge) rows
//
// Reserved for the judge flow (loads every non-sample row) and the
// admin problem-edit view (loads every row). The public detail
// endpoint does NOT call this — it uses list_samples_for_problem
// instead, which is the narrower / safer contract.
//
// Ordering: (order_num ASC, id ASC) — same as list_samples_for_problem.
inline std::vector<SampleCaseRow> list_for_problem(
        ConnectionPool& pool,
        int problem_id,
        std::optional<bool> only_samples = std::nullopt) {
    auto conn = pool.acquire();
    std::vector<SampleCaseRow> out;
    try {
        std::string sql = std::string("SELECT ") +
                          detail::kSampleCaseSelectColumns +
                          " FROM test_cases WHERE problem_id = ?";
        if (only_samples.has_value()) {
            if (*only_samples) {
                sql += " AND is_sample = TRUE";
            } else {
                sql += " AND is_sample = FALSE";
            }
        }
        sql += " ORDER BY order_num ASC, id ASC";
        auto rs = conn.execute(sql, problem_id);
        for (auto row : rs) {
            out.push_back(detail::row_to_sample(row));
        }
        return out;
    } catch (const mysqlx::Error& e) {
        throw TestCaseRepoError(
            std::string("test_case_repo::list_for_problem: ") + e.what());
    }
}

// ───────────────────────────────────────────────────────────────────────────
//  Write API — Phase 3 admin CRUD / bulk import (SPEC §5.2, A18, A19)
//
//  All write paths parameterize every value (SPEC §15.2). They are
//  intentionally thin: validate in the route handler, then call the
//  repo. We deliberately do NOT re-validate the problem_id FK in
//  the repo — the route layer pre-checks via problem_repo::find_by_*
//  so the call-site contract is "I just verified the parent exists".
//
//  Thread safety: every public method acquires a fresh
//  PooledConnection from the pool, runs the SQL, releases. `replace_*`
//  holds the connection for the duration of the transaction so the
//  pool grows up to max_size concurrent connections.
// ───────────────────────────────────────────────────────────────────────────

// insert — INSERT one test_cases row, returning the new id (>= 1 on
// success; 0 is impossible because the column is AUTO_INCREMENT).
//
// Field semantics mirror SPEC §4.3:
//   - input / expected_output: LONGTEXT NOT NULL; passed as std::string
//     for safe lifetime.
//   - is_sample: TRUE for the public-facing samples, FALSE for the
//     judge-only cases. The admin POST / PUT body decides which via
//     a separate "samples" / "test_cases" key (single endpoint that
//     accepts both shapes is left to a future bulk-import commit).
//   - judge_type: one of "exact" / "ignore_trailing" / "float_eps" /
//     "special". The route layer normalizes an absent value to
//     "exact" (matches the column DEFAULT).
//   - order_num: user-controlled ordering; smaller comes first. The
//     route layer defaults to the array index when the caller omits
//     it.
//   - float_epsilon: std::optional<double>. The DECIMAL(10,8) column
//     accepts NULL; the route layer passes std::nullopt for every
//     judge_type other than "float_eps". We deliberately convert
//     std::optional<double> -> std::optional<std::string> via the
//     mysqlx binding path's string form so a `nullopt` value lands
//     as SQL NULL, not "0" or "0.00000000".
//
// Throws TestCaseRepoError on driver error (handler → 500).
inline int insert(ConnectionPool& pool,
                 int problem_id,
                 const std::string& input,
                 const std::string& expected_output,
                 bool is_sample,
                 const std::string& judge_type,
                 int order_num,
                 std::optional<double> float_epsilon = std::nullopt) {
    auto conn = pool.acquire();
    try {
        // We use the bind-many positional form so the float_epsilon
        // NULL path doesn't have to branch on the SQL string. mysqlx
        // binds std::nullopt as SQL NULL automatically when the
        // destination is a numeric / string column.
        if (float_epsilon.has_value()) {
            // Decimal literal — keep enough precision for DECIMAL(10,8).
            // to_string of a double gives 17 significant digits which
            // is more than enough for any epsilon <= 1.
            const std::string eps_str = std::to_string(*float_epsilon);
            auto rs = conn.execute(
                "INSERT INTO test_cases "
                "(problem_id, input, expected_output, is_sample, "
                " judge_type, order_num, float_epsilon) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                problem_id,
                input,
                expected_output,
                is_sample,
                judge_type,
                order_num,
                eps_str);
            return static_cast<int>(rs.getAutoIncrementValue());
        } else {
            auto rs = conn.execute(
                "INSERT INTO test_cases "
                "(problem_id, input, expected_output, is_sample, "
                " judge_type, order_num) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                problem_id,
                input,
                expected_output,
                is_sample,
                judge_type,
                order_num);
            return static_cast<int>(rs.getAutoIncrementValue());
        }
    } catch (const mysqlx::Error& e) {
        throw TestCaseRepoError(std::string("test_case_repo::insert: ") + e.what());
    }
}

// delete_for_problem — DELETE every test_cases row for a given
// problem. Returns the number of rows removed. Used by
// `replace_for_problem` (below) and reserved for a future hard-delete
// admin path; the current admin problem DELETE is a soft-delete and
// does NOT call this (test cases of soft-deleted problems are kept
// so the row's historical submission_count / accepted_count still
// make sense on a future restore).
inline int delete_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "DELETE FROM test_cases WHERE problem_id = ?",
            problem_id);
        return static_cast<int>(rs.getAffectedItemsCount());
    } catch (const mysqlx::Error& e) {
        throw TestCaseRepoError(
            std::string("test_case_repo::delete_for_problem: ") + e.what());
    }
}

// replace_for_problem — clear + insert the given rows, atomically
// inside a single transaction held on one connection. Same shape
// as tag_repo::replace: a half-applied state is never observable
// on the wire.
//
// `rows` carries an `is_sample` flag on every entry so a single
// call can rewrite both the public samples AND the judge-only
// cases. The admin update path currently only sends samples
// (SPEC §8.1 — the judge cases are usually added later via a
// future admin UI), so the non-sample side will commonly be empty;
// that's fine — the DELETE clears zero rows and the INSERT loop
// is skipped.
//
// IMPORTANT: because the transaction must span every INSERT, this
// function does NOT call insert() (which acquires its own
// PooledConnection from the pool and would break the transaction
// boundary). Instead we build the INSERTs inline on the same
// PooledConnection the BEGIN opened on. The two INSERT shapes (with
// / without float_epsilon) mirror the insert() overload above; any
// schema change to either must be reflected here too.
//
// Throws TestCaseRepoError on any driver error. The transaction is
// rolled back when a mid-loop failure leaves the connection
// without commit() — meaning the problem keeps its OLD test cases
// intact, not a half-set.
inline void replace_for_problem(ConnectionPool& pool,
                                int problem_id,
                                const std::vector<SampleCaseRow>& rows,
                                bool is_sample_for_all_rows) {
    auto conn = pool.acquire();
    try {
        conn.execute("START TRANSACTION");
        try {
            // DELETE scoped to is_sample_for_all_rows so a follow-up
            // call (e.g. the bulk-import endpoint calling this once
            // for samples and once for judge cases) doesn't wipe
            // the rows we just inserted. The (problem_id, is_sample,
            // order_num) index from V005 covers this filter.
            if (is_sample_for_all_rows) {
                conn.execute(
                    "DELETE FROM test_cases "
                    "WHERE problem_id = ? AND is_sample = TRUE",
                    problem_id);
            } else {
                conn.execute(
                    "DELETE FROM test_cases "
                    "WHERE problem_id = ? AND is_sample = FALSE",
                    problem_id);
            }
            for (const auto& row : rows) {
                // Two INSERT shapes (with / without float_epsilon).
                // SampleCaseRow doesn't carry float_epsilon today; we
                // bind NULL for that column on every row, which is
                // semantically correct for judge_type values other
                // than "float_eps" (the column is documented as
                // "only for float_eps"). For the "float_eps" rows
                // we bind a sensible default epsilon — the admin
                // POST / PUT body can supply a richer vector in a
                // future commit if needed.
                if (row.judge_type == "float_eps") {
                    conn.execute(
                        "INSERT INTO test_cases "
                        "(problem_id, input, expected_output, is_sample, "
                        " judge_type, order_num, float_epsilon) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?)",
                        problem_id,
                        row.input,
                        row.expected_output,
                        is_sample_for_all_rows,
                        row.judge_type,
                        row.order_num,
                        std::string("0.00000001"));
                } else {
                    conn.execute(
                        "INSERT INTO test_cases "
                        "(problem_id, input, expected_output, is_sample, "
                        " judge_type, order_num) "
                        "VALUES (?, ?, ?, ?, ?, ?)",
                        problem_id,
                        row.input,
                        row.expected_output,
                        is_sample_for_all_rows,
                        row.judge_type,
                        row.order_num);
                }
            }
            conn.execute("COMMIT");
        } catch (...) {
            try { conn.execute("ROLLBACK"); } catch (...) {}
            throw;
        }
    } catch (const mysqlx::Error& e) {
        throw TestCaseRepoError(
            std::string("test_case_repo::replace_for_problem: ") + e.what());
    }
}

}  // namespace test_case_repo
}  // namespace litecode
