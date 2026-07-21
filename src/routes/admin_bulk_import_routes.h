// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — admin bulk-import routes (Phase 3 ★)
//
// SPEC §5.2 / §8.2 / §11 Phase 3 / §15.6 / A17, A21, A27 acceptance:
//   - POST /api/v1/admin/problems/import    (🔒 admin, 5/hour/admin)
//       Multipart/form-data upload of JSON problem files matching the
//       SPEC §8.1 schema. Hard limits from §8.2:
//         * max 50 files per request
//         * max 10 MB total per request
//       Query parameter ?on_duplicate=skip (default) | overwrite:
//         * skip       — collision on slug → record "skipped" in the
//                        response and move on (no UPDATE applied).
//         * overwrite  — collision on slug → full overwrite (titles,
//                        tags, samples, judge test cases); a previously
//                        soft-deleted row comes back to life (the
//                        problem_repo::upsert helper flips is_deleted
//                        to FALSE inside the same statement).
//       Single audit_logs row per batch (action=problem.bulk_import,
//       payload carries the full summary; per-file failures land in
//       payload.failures[]).
//
// Wire shape (request):
//   POST /api/v1/admin/problems/import[?on_duplicate=skip|overwrite]
//   Authorization: Bearer <admin-jwt>
//   Content-Type:  multipart/form-data; boundary=...
//
//   --boundary
//   Content-Disposition: form-data; name="files"; filename="two-sum.json"
//   Content-Type: application/json
//
//   { "slug": "two-sum", "title": "...", "difficulty": "easy",
//     "description": "...", "tags": [...], "samples": [...],
//     "test_cases": [...] }
//   --boundary
//   ...
//
// Wire shape (response, 200):
//   { "data": {
//       "summary":  { total_files, imported, skipped, overwritten, failed,
//                     duration_ms, on_duplicate },
//       "imported": [ { filename, slug, id, title, action,
//                       sample_count, test_case_count, tag_names[] } ],
//       "failures": [ { filename, stage, reason, details } ]
//     },
//     "request_id": "..." }
//
// Failure isolation:
//   - A bad JSON in file #3 of 5 files is NOT an abort — we record
//     file #3 in `failures[]` and continue. HTTP stays 200.
//   - The only conditions that abort the whole batch with 400 / 500
//     are header-level: 0 files, >50 files, >10MB, malformed
//     on_duplicate, infra-level DB error.
//
// Error envelopes (SPEC §5.7):
//   - 400 INVALID_INPUT  — 0 files / >50 files / >10MB / bad on_duplicate
//   - 401 UNAUTHORIZED   — require_admin
//   - 403 FORBIDDEN      — non-admin caller
//   - 429 RATE_LIMITED   — bulk_import_quota (5/hour/admin)
//   - 500 INTERNAL_ERROR — uncaught infra-level error (driver down, etc.)
//
// Audit log payload (SPEC §8.2 — single row per batch):
//   {
//     "on_duplicate":  "skip" | "overwrite",
//     "total_files":   N,
//     "imported":      N,
//     "skipped":       N,
//     "overwritten":   N,
//     "failed":        N,
//     "duration_ms":   N,
//     "failures":      [{ filename, stage, reason }]
//   }
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 / 3 module.
//     Tests link this header directly and instantiate the route set
//     with a real ConnectionPool + a (lax) rate limiter + a real
//     JwtConfig.
//   - The parser helpers (require_string / parse_tags_array /
//     parse_samples_array / parse_test_cases_array /
//     optional_int_field / validate_problem_patch / apply_problem_patch)
//     live inside `litecode::bulk_import::detail` so they don't collide
//     with the identically-named helpers in `litecode::detail` (which
//     admin_problem_routes.h owns) when both headers are included in
//     the same TU. This is the same convention problem_repo::detail /
//     tag_repo::detail / test_case_repo::detail follow — see the
//     comment at the top of tag_repo.h.
//   - The single-batch audit row is written STRICTLY (record(), not
//     record_best_effort) — a lost audit row on a destructive admin
//     action is a security trail gap (SPEC §15.6). If the row fails to
//     write the whole batch response surfaces a 500 envelope.
//   - Because of the ODR-collision risk between
//     `litecode::detail::*` (defined identically in
//     admin_problem_routes.h) and `litecode::bulk_import::detail::*`
//     (defined identically in this header), main.cpp does NOT smoke-
//     register this header. End-to-end coverage is owned by
//     tests/unit/test_admin_bulk_import.cpp. The same constraint
//     already excludes problem_routes.h / admin_problem_routes.h /
//     tag_routes.h from main.cpp — see the comment at the top of
//     admin_problem_routes.h for the long-form rationale.
//   - We rely on cpp-httplib's multipart parser for the request side.
//     `req.files` is `httplib::MultipartFormDataMap` (a multimap
//     keyed by form-field name); we accept any file posted under the
//     `files` field. The number of files and total byte budget are
//     enforced at the route boundary so a hostile client can't keep
//     us busy parsing unbounded input.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                          // RateLimitConfig / JwtConfig
#include "../db/audit_log_repo.h"               // audit_log_repo::record + kActionProblemBulkImport
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/problem_repo.h"                 // problem_repo::create / upsert / slug_exists / find_by_*
#include "../db/tag_repo.h"                     // tag_repo::find_or_create_many / replace
#include "../db/test_case_repo.h"               // test_case_repo::list_samples_for_problem / replace_for_problem
#include "../logger.h"                          // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../middleware/admin_middleware.h"     // require_admin
#include "../middleware/rate_limit.h"           // consume_rate_limit / bulk_import_quota / extract_client_ip
#include "../routes/error_handler.h"            // parse_json_body / ErrorCode / send_error
#include "../server.h"                          // HttpServer / send_success

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Hard limits from SPEC §8.2
// ────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kBulkImportMaxFiles = 50;
inline constexpr std::size_t kBulkImportMaxTotalBytes = 10 * 1024 * 1024;  // 10 MB

// On-duplicate policy. Default is skip — the SPEC §8.2 contract.
enum class OnDuplicate {
    Skip,
    Overwrite,
};

// Wire string ↔ OnDuplicate. Case-insensitive. Used by the route
// handler when reading ?on_duplicate=... and by the response shape.
inline std::string_view on_duplicate_name(OnDuplicate v) noexcept {
    switch (v) {
        case OnDuplicate::Skip:      return "skip";
        case OnDuplicate::Overwrite: return "overwrite";
    }
    return "skip";   // unreachable; keeps -Wreturn-type happy
}

// File-level action — what happened to one row in the batch. Used by
// the response shape and by the `imported[]` array (skip + overwrite
// + created are all surfaced as "what happened to this file").
enum class ImportAction {
    Created,
    Overwritten,
    Skipped,
    Failed,
};

inline std::string_view import_action_name(ImportAction a) noexcept {
    switch (a) {
        case ImportAction::Created:     return "created";
        case ImportAction::Overwritten: return "overwritten";
        case ImportAction::Skipped:     return "skipped";
        case ImportAction::Failed:      return "failed";
    }
    return "failed";
}

// ImportFileResult — one row in the response's `imported` and
// `failures` arrays (the two arrays share this same struct; the
// `action` field discriminates them on the wire — `failed` items
// also have `stage` + `reason` + `details` filled in).
struct ImportFileResult {
    std::string             filename;       // original multipart filename ("" if absent)
    std::string             slug;           // the slug from the JSON (or first 64 chars of failure)
    std::string             title;          // the title from the JSON (or "")
    ImportAction            action = ImportAction::Failed;
    int                     problem_id = 0;
    int                     sample_count = 0;
    int                     test_case_count = 0;
    std::vector<std::string> tag_names;

    // Failure surface — populated only when `action == Failed`.
    std::string             stage;          // "parse" | "validate" | "repo"
    std::string             reason;
    nlohmann::json          details = nullptr;
};

// ImportSummary — the top-of-response counters plus the chosen policy.
struct ImportSummary {
    std::size_t total_files   = 0;
    std::size_t imported      = 0;   // created + overwritten
    std::size_t skipped       = 0;
    std::size_t overwritten   = 0;
    std::size_t failed        = 0;
    long long   duration_ms   = 0;
    OnDuplicate on_duplicate  = OnDuplicate::Skip;
};

// ────────────────────────────────────────────────────────────────────────────
//  Bulk-import parsing helpers
//
//  Mirrors the shape of admin_problem_routes.h::detail::* but lives
//  in its own namespace to dodge ODR collisions when a TU pulls in
//  both headers. The error envelope contract (400 INVALID_INPUT
//  with `details.field` / `details.value` / `details.index`) is the
//  same as the admin CRUD path; the audit-friendly truncation is
//  identical too (64-char cap with "..." suffix).
// ────────────────────────────────────────────────────────────────────────────

namespace bulk_import {

namespace detail {

inline constexpr std::size_t kBulkImportDetailValueMax = 64;

inline std::string truncate_for_envelope(const std::string& v) {
    if (v.size() <= kBulkImportDetailValueMax) return v;
    return v.substr(0, kBulkImportDetailValueMax) + "...";
}

// require_string — extract a required string field from the body.
// Returns the value on success. On missing / wrong-type, writes a
// 400 envelope into `failure` (a slot the caller passes in to record
// the per-file failure) and returns std::nullopt. The route handler
// short-circuits the per-file loop on std::nullopt.
inline std::optional<std::string> require_string(const nlohmann::json& body,
                                                 ImportFileResult& failure) {
    if (!body.contains("slug") || !body["slug"].is_string()) {
        failure.stage   = "validate";
        failure.reason  = "missing or non-string field 'slug'";
        failure.details = {{"field", "slug"}};
        return std::nullopt;
    }
    return body["slug"].get<std::string>();
}

inline std::optional<std::string> require_title(const nlohmann::json& body,
                                                ImportFileResult& failure) {
    if (!body.contains("title") || !body["title"].is_string()) {
        failure.stage   = "validate";
        failure.reason  = "missing or non-string field 'title'";
        failure.details = {{"field", "title"}};
        return std::nullopt;
    }
    return body["title"].get<std::string>();
}

inline std::optional<std::string> require_difficulty(const nlohmann::json& body,
                                                     ImportFileResult& failure) {
    if (!body.contains("difficulty") || !body["difficulty"].is_string()) {
        failure.stage   = "validate";
        failure.reason  = "missing or non-string field 'difficulty'";
        failure.details = {{"field", "difficulty"}};
        return std::nullopt;
    }
    return body["difficulty"].get<std::string>();
}

inline std::optional<std::string> require_description(const nlohmann::json& body,
                                                      ImportFileResult& failure) {
    if (!body.contains("description") || !body["description"].is_string()) {
        failure.stage   = "validate";
        failure.reason  = "missing or non-string field 'description'";
        failure.details = {{"field", "description"}};
        return std::nullopt;
    }
    return body["description"].get<std::string>();
}

// require_judge_type — validate against SPEC §4.3 ENUM. Absent /
// null maps to "exact" (matches the column DEFAULT). A non-empty
// value outside the enum is a 400-equivalent failure.
inline std::optional<std::string> require_judge_type(
        const nlohmann::json& body,
        ImportFileResult& failure,
        const char* field,
        const std::string& default_value = "exact") {
    if (!body.contains(field) || body[field].is_null()) {
        return default_value;
    }
    if (!body[field].is_string()) {
        failure.stage   = "validate";
        failure.reason  = std::string("field '") + field +
                          "' must be a string when present";
        failure.details = {{"field", field}};
        return std::nullopt;
    }
    const std::string v = body[field].get<std::string>();
    if (v.empty()) return default_value;
    if (v != "exact" && v != "ignore_trailing" &&
        v != "float_eps" && v != "special" &&
        v != "ignore_case" && v != "ignore_all_whitespace") {
        failure.stage   = "validate";
        failure.reason  = "judge_type must be one of: exact, ignore_trailing, "
                          "float_eps, special, ignore_case, "
                          "ignore_all_whitespace";
        failure.details = {{"field", field},
                           {"value", truncate_for_envelope(v)}};
        return std::nullopt;
    }
    return v;
}

// optional_int_field — extract an optional integer field with a
// range guard. Absent / null → std::nullopt (repo applies the SPEC
// defaults). Present-and-malformed → fills `failure` and returns
// std::nullopt. The route's per-file loop bails on std::nullopt.
inline std::optional<int> optional_int_field(const nlohmann::json& body,
                                             ImportFileResult& failure,
                                             const char* field,
                                             int min_inclusive,
                                             int max_inclusive) {
    if (!body.contains(field) || body[field].is_null()) {
        return std::nullopt;
    }
    if (!body[field].is_number_integer()) {
        failure.stage   = "validate";
        failure.reason  = std::string("field '") + field +
                          "' must be an integer when present";
        failure.details = {{"field", field}};
        return std::nullopt;
    }
    const int v = body[field].get<int>();
    if (v < min_inclusive || v > max_inclusive) {
        failure.stage   = "validate";
        failure.reason  = std::string("field '") + field + "' out of range";
        failure.details = {{"field", field},
                           {"value", std::to_string(v)},
                           {"min",   std::to_string(min_inclusive)},
                           {"max",   std::to_string(max_inclusive)}};
        return std::nullopt;
    }
    return v;
}

// parse_tags_array — optional, defaults to []. Each entry must be
// a non-empty string (1..50 chars after trimming). Bad shapes fill
// `failure` and return std::nullopt. Returns the trimmed canonical
// list (NOT the raw user bytes) so find_or_create_many can hand it
// straight to the repo.
inline std::optional<std::vector<std::string>> parse_tags_array(
        const nlohmann::json& body,
        ImportFileResult& failure) {
    std::vector<std::string> out;
    if (!body.contains("tags") || body["tags"].is_null()) {
        return out;
    }
    if (!body["tags"].is_array()) {
        failure.stage   = "validate";
        failure.reason  = "tags must be an array of strings";
        failure.details = {{"field", "tags"}};
        return std::nullopt;
    }
    for (std::size_t i = 0; i < body["tags"].size(); ++i) {
        const auto& el = body["tags"][i];
        if (!el.is_string()) {
            failure.stage   = "validate";
            failure.reason  = "tags entries must be strings";
            failure.details = {{"field", "tags"},
                               {"index", std::to_string(i)}};
            return std::nullopt;
        }
        const std::string raw = el.get<std::string>();
        const std::string trimmed = trim_tag_name(raw);
        std::string err;
        if (trimmed.empty() || !validate_tag_name(trimmed, &err)) {
            failure.stage   = "validate";
            failure.reason  = std::string("invalid tag name at index ") +
                              std::to_string(i) + ": " + err;
            failure.details = {{"field", "tags"},
                               {"index", std::to_string(i)},
                               {"value", truncate_for_envelope(raw)}};
            return std::nullopt;
        }
        out.push_back(trimmed);
    }
    return out;
}

// parse_samples_array — optional, defaults to []. Each entry must
// be an object with required `input` / `output` strings;
// `judge_type` defaults to "exact"; `order_num` defaults to the
// array index.
inline std::optional<std::vector<SampleCaseRow>> parse_samples_array(
        const nlohmann::json& body,
        ImportFileResult& failure) {
    std::vector<SampleCaseRow> out;
    if (!body.contains("samples") || body["samples"].is_null()) {
        return out;
    }
    if (!body["samples"].is_array()) {
        failure.stage   = "validate";
        failure.reason  = "samples must be an array of objects";
        failure.details = {{"field", "samples"}};
        return std::nullopt;
    }
    for (std::size_t i = 0; i < body["samples"].size(); ++i) {
        const auto& el = body["samples"][i];
        if (!el.is_object()) {
            failure.stage   = "validate";
            failure.reason  = "samples entries must be objects";
            failure.details = {{"field", "samples"},
                               {"index", std::to_string(i)}};
            return std::nullopt;
        }
        if (!el.contains("input") || !el["input"].is_string()) {
            failure.stage   = "validate";
            failure.reason  = "samples[].input must be a string";
            failure.details = {{"field", "samples"},
                               {"index", std::to_string(i)},
                               {"subfield", "input"}};
            return std::nullopt;
        }
        if (!el.contains("output") || !el["output"].is_string()) {
            failure.stage   = "validate";
            failure.reason  = "samples[].output must be a string";
            failure.details = {{"field", "samples"},
                               {"index", std::to_string(i)},
                               {"subfield", "output"}};
            return std::nullopt;
        }
        SampleCaseRow s;
        s.input           = el["input"].get<std::string>();
        s.expected_output = el["output"].get<std::string>();
        const auto jt = require_judge_type(el, failure, "judge_type");
        if (!jt.has_value()) return std::nullopt;
        s.judge_type = *jt;
        s.order_num  = static_cast<int>(i);
        if (el.contains("order_num") && !el["order_num"].is_null()) {
            if (!el["order_num"].is_number_integer()) {
                failure.stage   = "validate";
                failure.reason  = "samples[].order_num must be an integer";
                failure.details = {{"field", "samples"},
                                   {"index", std::to_string(i)},
                                   {"subfield", "order_num"}};
                return std::nullopt;
            }
            s.order_num = el["order_num"].get<int>();
        }
        out.push_back(std::move(s));
    }
    return out;
}

// parse_test_cases_array — the §8.1 `test_cases` array, which is
// the judge-only counterpart of `samples`. Defaults to []. Each
// entry MUST have `input` + `expected_output`; `is_sample` defaults
// to false (this is the discriminator vs `samples`); `judge_type`
// defaults to "exact"; `order_num` defaults to the array index.
//
// `float_epsilon` is accepted on the wire (forward-compat with the
// §8.1 schema) but discarded — the underlying SQL still binds a
// fixed default (0.00000001) for float_eps cases. A future
// schema change can plumb it through once the SampleCaseRow /
// replace_for_problem path grows a real float_epsilon slot.
inline std::optional<std::vector<SampleCaseRow>> parse_test_cases_array(
        const nlohmann::json& body,
        ImportFileResult& failure) {
    std::vector<SampleCaseRow> out;
    if (!body.contains("test_cases") || body["test_cases"].is_null()) {
        return out;
    }
    if (!body["test_cases"].is_array()) {
        failure.stage   = "validate";
        failure.reason  = "test_cases must be an array of objects";
        failure.details = {{"field", "test_cases"}};
        return std::nullopt;
    }
    for (std::size_t i = 0; i < body["test_cases"].size(); ++i) {
        const auto& el = body["test_cases"][i];
        if (!el.is_object()) {
            failure.stage   = "validate";
            failure.reason  = "test_cases entries must be objects";
            failure.details = {{"field", "test_cases"},
                               {"index", std::to_string(i)}};
            return std::nullopt;
        }
        if (!el.contains("input") || !el["input"].is_string()) {
            failure.stage   = "validate";
            failure.reason  = "test_cases[].input must be a string";
            failure.details = {{"field", "test_cases"},
                               {"index", std::to_string(i)},
                               {"subfield", "input"}};
            return std::nullopt;
        }
        if (!el.contains("expected_output") || !el["expected_output"].is_string()) {
            failure.stage   = "validate";
            failure.reason  = "test_cases[].expected_output must be a string";
            failure.details = {{"field", "test_cases"},
                               {"index", std::to_string(i)},
                               {"subfield", "expected_output"}};
            return std::nullopt;
        }
        // is_sample — optional. The SPEC §8.1 schema puts the judge
        // cases in `test_cases` and the samples in `samples`, so the
        // default for this array is FALSE. We still honor an
        // explicit true/false in case the caller conflates the two
        // arrays (and we record a flag mismatch in `details`).
        if (el.contains("is_sample") && !el["is_sample"].is_null()
            && el["is_sample"].is_boolean() && el["is_sample"].get<bool>()) {
            failure.stage   = "validate";
            failure.reason  = "test_cases[].is_sample must be false or omitted "
                              "(use the 'samples' array for public-facing cases)";
            failure.details = {{"field", "test_cases"},
                               {"index", std::to_string(i)},
                               {"subfield", "is_sample"}};
            return std::nullopt;
        }
        SampleCaseRow t;
        t.input           = el["input"].get<std::string>();
        t.expected_output = el["expected_output"].get<std::string>();
        const auto jt = require_judge_type(el, failure, "judge_type");
        if (!jt.has_value()) return std::nullopt;
        t.judge_type = *jt;
        t.order_num  = static_cast<int>(i);
        if (el.contains("order_num") && !el["order_num"].is_null()) {
            if (!el["order_num"].is_number_integer()) {
                failure.stage   = "validate";
                failure.reason  = "test_cases[].order_num must be an integer";
                failure.details = {{"field", "test_cases"},
                                   {"index", std::to_string(i)},
                                   {"subfield", "order_num"}};
                return std::nullopt;
            }
            t.order_num = el["order_num"].get<int>();
        }
        out.push_back(std::move(t));
    }
    return out;
}

// validate_problem_patch — same shape as
// admin_problem_routes.h::detail::validate_problem_patch; we keep
// our own copy in the bulk_import::detail namespace to dodge the
// ODR collision risk (see the comment at the top of this header).
inline bool validate_problem_patch(const ProblemRow& row,
                                   ImportFileResult& failure) {
    std::string err;
    if (!validate_slug(row.slug, &err)) {
        failure.stage   = "validate";
        failure.reason  = err;
        failure.details = {{"field", "slug"},
                           {"value", truncate_for_envelope(row.slug)}};
        return false;
    }
    if (!validate_title(row.title, &err)) {
        failure.stage   = "validate";
        failure.reason  = err;
        failure.details = {{"field", "title"}};
        return false;
    }
    if (!validate_difficulty(row.difficulty, &err)) {
        failure.stage   = "validate";
        failure.reason  = err;
        failure.details = {{"field", "difficulty"},
                           {"value", truncate_for_envelope(row.difficulty)}};
        return false;
    }
    if (!validate_time_limit(row.time_limit, &err)) {
        failure.stage   = "validate";
        failure.reason  = err;
        failure.details = {{"field", "time_limit_ms"},
                           {"value", std::to_string(row.time_limit)}};
        return false;
    }
    if (!validate_memory_limit(row.memory_limit, &err)) {
        failure.stage   = "validate";
        failure.reason  = err;
        failure.details = {{"field", "memory_limit_mb"},
                           {"value", std::to_string(row.memory_limit)}};
        return false;
    }
    return true;
}

// apply_problem_patch — the shared "after the problem row is in
// place" step. Resolves tag names via find_or_create_many, replaces
// the problem's tag set via tag_repo::replace, and writes the
// supplied samples + judge test cases via test_case_repo::
// replace_for_problem (each call atomic on its own connection).
//
// Throws on repo failure; the caller maps to a stage="repo"
// failure record.
//
// `samples` may be empty (means "delete all samples" or "no
// samples to add"). Same for `test_cases`.
inline void apply_problem_patch(ConnectionPool& pool,
                                int problem_id,
                                const std::vector<std::string>& tag_names,
                                const std::vector<SampleCaseRow>& samples,
                                const std::vector<SampleCaseRow>& test_cases) {
    if (!tag_names.empty()) {
        const auto resolved = tag_repo::find_or_create_many(pool, tag_names);
        std::vector<int> tag_ids;
        tag_ids.reserve(resolved.size());
        for (const auto& t : resolved) tag_ids.push_back(t.id);
        tag_repo::replace(pool, problem_id, tag_ids);
    } else {
        tag_repo::replace(pool, problem_id, {});
    }

    test_case_repo::replace_for_problem(
        pool, problem_id, samples,
        /*is_sample_for_all_rows=*/true);
    test_case_repo::replace_for_problem(
        pool, problem_id, test_cases,
        /*is_sample_for_all_rows=*/false);
}

// on_duplicate_param — translate a query-param string to the enum.
// Empty → OnDuplicate::Skip (SPEC §8.2 default). Case-insensitive.
// "skip" / "overwrite" accepted; anything else → std::nullopt (the
// caller emits a 400 envelope).
inline std::optional<OnDuplicate> on_duplicate_param(std::string_view raw) {
    if (raw.empty()) return OnDuplicate::Skip;
    // Case-insensitive compare — same defensive pattern as
    // problem_routes.h::detail::parse_bool_param.
    auto ci_eq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    if (ci_eq(raw, "skip"))      return OnDuplicate::Skip;
    if (ci_eq(raw, "overwrite")) return OnDuplicate::Overwrite;
    return std::nullopt;
}

// process_one_file — the inner per-file import loop body. The
// caller (bulk_import_files) provides the parsed JSON + the
// preferred file `filename`, and we do parse → validate → repo
// dispatch in one place.
//
// On any infra-level exception (DB driver error, FK violation,
// etc.) we record stage="repo" with the exception's `what()` so
// the audit row can carry the diagnostic. Per SPEC §5.2/§8.2 the
// batch is NOT aborted — we surface the failure into the result
// and let the loop continue.
inline ImportFileResult process_one_file(ConnectionPool& pool,
                                         const httplib::MultipartFormData& file,
                                         OnDuplicate on_duplicate) {
    ImportFileResult result;
    result.filename = file.filename;
    // We carry the slug into the failure record (truncated) so the
    // admin can correlate the failure row back to the input file
    // even when the JSON was malformed. Empty until we know it.
    result.action   = ImportAction::Failed;

    // 1) Parse JSON body.
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(file.content);
    } catch (const std::exception& e) {
        result.stage   = "parse";
        result.reason  = std::string("invalid JSON: ") + e.what();
        result.details = nullptr;
        result.slug    = file.filename;   // for the failure record
        return result;
    }

    // 2) Pull + validate the scalar fields. Each helper fills
    //    result.stage/reason/details on failure and returns nullopt.
    const auto slug_v        = require_string      (body, result);
    if (!slug_v) { result.slug = ""; return result; }
    const auto title_v       = require_title       (body, result);
    if (!title_v) { result.slug = *slug_v; return result; }
    const auto difficulty_v  = require_difficulty  (body, result);
    if (!difficulty_v) { result.slug = *slug_v; return result; }
    const auto description_v = require_description (body, result);
    if (!description_v) { result.slug = *slug_v; return result; }
    const auto tags_v        = parse_tags_array    (body, result);
    if (!tags_v) { result.slug = *slug_v; return result; }
    const auto samples_v     = parse_samples_array (body, result);
    if (!samples_v) { result.slug = *slug_v; return result; }
    const auto test_cases_v  = parse_test_cases_array(body, result);
    if (!test_cases_v) { result.slug = *slug_v; return result; }

    ProblemRow row;
    row.slug        = *slug_v;
    row.title       = *title_v;
    row.difficulty  = *difficulty_v;
    row.description = *description_v;
    // optional_int_field returns std::nullopt in TWO cases:
    //   (1) field is absent / null → no value (use SPEC defaults);
    //   (2) field is present but malformed → 400-equivalent failure.
    // We have to disambiguate by checking contains() FIRST — if
    // the field is absent, std::nullopt is a benign "use default";
    // if the field is present and we got nullopt, the helper
    // already filled result.stage/reason and we bail.
    {
        if (body.contains("time_limit_ms") && !body["time_limit_ms"].is_null()) {
            const auto t = optional_int_field(body, result, "time_limit_ms",
                                              kMinTimeLimitMs, kMaxTimeLimitMs);
            if (!t) { result.slug = *slug_v; return result; }
            row.time_limit = *t;
        }
    }
    {
        if (body.contains("memory_limit_mb") && !body["memory_limit_mb"].is_null()) {
            const auto m = optional_int_field(body, result, "memory_limit_mb",
                                              kMinMemoryLimitMb, kMaxMemoryLimitMb);
            if (!m) { result.slug = *slug_v; return result; }
            row.memory_limit = *m;
        }
    }
    if (!validate_problem_patch(row, result)) {
        result.slug = row.slug;
        return result;
    }

    // 3) Repo dispatch. Check existence FIRST so the skip vs
    //    overwrite branch is unambiguous. include_deleted=false so
    //    a soft-deleted row counts as "free" and gets re-created
    //    by the same INSERT path. (?on_duplicate=overwrite restores
    //    a soft-deleted row via the upsert path below; ?on_duplicate=skip
    //    leaves the tombstone in place and reports the slug as
    //    "skipped" against the live count — that's the safer
    //    default per SPEC §8.2.)
    bool existed_live = false;
    try {
        existed_live = problem_repo::slug_exists(pool, row.slug,
                                                 /*include_deleted=*/false);
    } catch (const std::exception& e) {
        result.stage   = "repo";
        result.reason  = std::string("problem_repo::slug_exists threw: ") + e.what();
        result.slug    = row.slug;
        return result;
    }

    if (existed_live && on_duplicate == OnDuplicate::Skip) {
        // No-op. We deliberately don't run apply_problem_patch on
        // skipped rows so the existing tags / samples / judge
        // cases stay intact. The response carries the slug +
        // action="skipped" so the admin UI can mark the file as
        // "unchanged".
        result.action       = ImportAction::Skipped;
        result.slug         = row.slug;
        result.title        = row.title;
        result.sample_count = 0;
        result.test_case_count = 0;
        // tag_names are not loaded for skipped rows — that would
        // require a SELECT IN-clause per skipped file and the
        // caller asked for "skip", not "diff". A future
        // admin-tools commit can surface this if needed.
        result.tag_names    = {};
        return result;
    }

    // 4) Insert (new slug) or upsert (overwrite path). upsert
    //    handles both fresh inserts AND overwrites via
    //    ON DUPLICATE KEY UPDATE — for an existed_live + overwrite
    //    case it returns {existing_id, created=false}; for a
    //    new slug it returns {new_id, created=true}. For a
    //    soft-deleted slug + skip path the slug_exists returned
    //    false (we excluded soft-deleted), so we'd INSERT — but
    //    the UNIQUE constraint would collide with the tombstone.
    //    We don't hit that branch because skip falls through
    //    above; here the only callers are create-new or
    //    overwrite-live.
    litecode::problem_repo::UpsertResult upserted{0, false};
    try {
        if (existed_live) {
            // Overwrite an existing live row. upsert()'s
            // ON DUPLICATE KEY UPDATE branch handles this AND
            // also restores a soft-deleted row (by setting
            // is_deleted=FALSE inside the same statement) — a
            // free side-effect.
            upserted = problem_repo::upsert(pool, row);
        } else {
            // No live row. INSERT a fresh one. create() returns 0
            // on UNIQUE collision (e.g. a soft-deleted row
            // already owns this slug); in that case we fall
            // through to upsert() which restores the tombstone.
            const int new_id = problem_repo::create(pool, row);
            if (new_id > 0) {
                upserted = litecode::problem_repo::UpsertResult{new_id, /*created=*/true};
            } else {
                // Soft-deleted slug — restore via upsert. We
                // preserve the bulk-import "created vs overwritten"
                // distinction by checking whether a soft-deleted
                // row existed pre-restore.
                const bool had_tombstone = problem_repo::slug_exists(
                    pool, row.slug, /*include_deleted=*/true);
                upserted = problem_repo::upsert(pool, row);
                if (had_tombstone) {
                    upserted.created = false;   // report as overwrite
                }
            }
        }
    } catch (const std::exception& e) {
        result.stage   = "repo";
        result.reason  = std::string("problem_repo::create/upsert threw: ") + e.what();
        result.slug    = row.slug;
        return result;
    }

    // 5) Tags + samples + judge test cases.
    try {
        apply_problem_patch(pool, upserted.id, *tags_v, *samples_v, *test_cases_v);
    } catch (const std::exception& e) {
        // The problem row is in place but its associations failed.
        // We surface stage="repo" and the admin can re-PUT the row
        // to repair the tag/sample set. The HTTP response stays
        // 200 — partial success is part of the §8.2 contract.
        result.stage   = "repo";
        result.reason  = std::string("apply_problem_patch threw: ") + e.what();
        result.slug    = row.slug;
        result.title   = row.title;
        result.problem_id = upserted.id;
        return result;
    }

    // 6) Success path.
    result.action       = upserted.created ? ImportAction::Created
                                           : ImportAction::Overwritten;
    result.slug         = row.slug;
    result.title        = row.title;
    result.problem_id   = upserted.id;
    result.sample_count = static_cast<int>(samples_v->size());
    result.test_case_count = static_cast<int>(test_cases_v->size());
    result.tag_names    = *tags_v;
    return result;
}

// summarize — fold a vector of per-file results into the
// top-of-response counters. A failed item contributes to `failed`
// AND lands in the response's `failures[]`; the others contribute
// to created / overwritten / skipped exactly once.
inline ImportSummary summarize(const std::vector<ImportFileResult>& results,
                               std::size_t total_files,
                               OnDuplicate policy,
                               long long duration_ms) {
    ImportSummary s;
    s.total_files  = total_files;
    s.on_duplicate = policy;
    s.duration_ms  = duration_ms;
    for (const auto& r : results) {
        switch (r.action) {
            case ImportAction::Created:     s.imported++;    break;
            case ImportAction::Overwritten: s.overwritten++; s.imported++; break;
            case ImportAction::Skipped:     s.skipped++;     break;
            case ImportAction::Failed:      s.failed++;      break;
        }
    }
    return s;
}

}  // namespace detail

// bulk_import_files — the inner per-batch worker. Takes the
// pre-validated multipart file set (already past the count + size
// check at the route boundary), walks each one, and produces a
// parallel vector of ImportFileResult rows. Repo errors are caught
// per-file and surfaced as stage="repo" failures so the batch is
// NEVER aborted by a single bad row (SPEC §8.2 failure-isolation
// policy).
inline std::vector<ImportFileResult> bulk_import_files(
        ConnectionPool& pool,
        const std::vector<httplib::MultipartFormData>& files,
        OnDuplicate on_duplicate) {
    std::vector<ImportFileResult> out;
    out.reserve(files.size());
    for (const auto& f : files) {
        out.push_back(detail::process_one_file(pool, f, on_duplicate));
    }
    return out;
}

// count_files_and_bytes — pre-flight check for the route handler.
// The route runs this BEFORE bulk_import_files so a hostile client
// can't make us parse unbounded input. The function returns the
// count of files posted under ANY field name (we accept the
// conventional `files` name and the variant `file`); the size is
// the sum of every part's `content.size()`.
//
// We accept either field name because the SPEC §8.2 wire shape
// uses `files` but some clients (curl `-F`) elide the field name
// entirely — `req.files` still carries them in cpp-httplib. We
// DON'T try to detect "this was actually a `tags[]` part" — the
// convention is "anything posted to a multipart body under the
// admin import endpoint is a problem file".
inline std::pair<std::size_t /*count*/, std::size_t /*bytes*/>
count_files_and_bytes(const httplib::MultipartFormDataMap& parts) {
    std::size_t count = 0;
    std::size_t bytes = 0;
    for (const auto& kv : parts) {
        const auto& f = kv.second;
        // Only count entries that look like file uploads
        // (filename is non-empty). Form fields without a filename
        // are usually non-file form values and we ignore them
        // here — a defensive measure against a client that posts
        // a JSON-shaped body under the multipart field.
        if (f.filename.empty()) continue;
        ++count;
        bytes += f.content.size();
    }
    return {count, bytes};
}

inline std::vector<httplib::MultipartFormData> collect_files(
        const httplib::MultipartFormDataMap& parts) {
    std::vector<httplib::MultipartFormData> out;
    out.reserve(parts.size());
    for (const auto& kv : parts) {
        if (kv.second.filename.empty()) continue;
        out.push_back(kv.second);
    }
    return out;
}

}  // namespace bulk_import

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/admin/problems/import   (SPEC §5.2, §8.2, A17, A21, A27)
//
//  Wire flow:
//    1) require_admin(...)            — 401 / 403 envelope
//    2) consume_rate_limit(bulk_import_quota) — 5/hour/admin; 429 envelope
//    3) parse ?on_duplicate=           — 400 on bad value
//    4) header-level limits            — 400 on 0 / >50 files or >10MB
//    5) bulk_import::bulk_import_files — per-file parse + validate + repo
//    6) audit_log_repo::record(...)    — strict, single row per batch
//    7) send_success(200) + {summary, imported[], failures[]}
// ────────────────────────────────────────────────────────────────────────────

inline void admin_bulk_import_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    // 1) Auth gate.
    const auto claims = require_admin(req, jwt_cfg);

    // 2) Rate limit (bulk_import bucket, keyed by user_id).
    consume_rate_limit(res, req, limiter, bulk_import_quota(rate_cfg));

    // 3) ?on_duplicate= — default Skip.
    OnDuplicate on_dup = OnDuplicate::Skip;
    if (req.has_param("on_duplicate")) {
        const std::string raw = req.get_param_value("on_duplicate");
        const auto parsed = bulk_import::detail::on_duplicate_param(raw);
        if (!parsed.has_value()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "on_duplicate must be one of: skip, overwrite",
                       {{"field", "on_duplicate"},
                        {"value", raw}});
            return;
        }
        on_dup = *parsed;
    }

    // 4) Header-level limits. cpp-httplib's multipart parser has
    //    already done the heavy lifting (extracting parts,
    //    decoding boundaries); we just count + size.
    const auto [file_count, total_bytes] =
        bulk_import::count_files_and_bytes(req.files);
    if (file_count == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "no files in multipart body "
                   "(expected one or more 'files' fields)",
                   {{"expected_field", "files"}});
        return;
    }
    if (file_count > kBulkImportMaxFiles) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "too many files: max " +
                       std::to_string(kBulkImportMaxFiles) +
                       " files per request",
                   {{"max",     static_cast<int>(kBulkImportMaxFiles)},
                    {"got",     static_cast<int>(file_count)}});
        return;
    }
    if (total_bytes > kBulkImportMaxTotalBytes) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "total payload too large: max " +
                       std::to_string(kBulkImportMaxTotalBytes) +
                       " bytes",
                   {{"max_bytes", static_cast<int>(kBulkImportMaxTotalBytes)},
                    {"got_bytes", static_cast<int>(total_bytes)}});
        return;
    }

    // 5) Walk the files. bulk_import_files catches per-file repo
    //    errors internally so the loop never throws. An
    //    INFRA-level error (DB driver gone, etc.) would surface
    //    as a stage="repo" failure in the file that triggered
    //    it; subsequent files would also fail with the same
    //    error class. That's the desired behavior — the
    //    operator sees the batch is wedged and re-imports when
    //    the DB is back.
    const auto t0 = std::chrono::steady_clock::now();
    const auto files = bulk_import::collect_files(req.files);
    std::vector<ImportFileResult> results;
    try {
        results = bulk_import::bulk_import_files(pool, files, on_dup);
    } catch (const std::exception& e) {
        // Should be unreachable — bulk_import_files is designed to
        // never throw — but defense in depth: surface as 500.
        LOG_ERROR("admin_bulk_import: bulk_import_files threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long long duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 6) Build the summary. Walk the results once.
    ImportSummary summary = bulk_import::detail::summarize(
        results, file_count, on_dup, duration_ms);

    // 7) ONE audit row per batch (SPEC §8.2). Strict — a lost
    //    audit row on a destructive admin action is a security
    //    trail gap (SPEC §15.6).
    try {
        AuditEntry ae;
        ae.admin_id    = std::stoi(claims.user_id);
        ae.action      = audit_log_repo::kActionProblemBulkImport;
        ae.target_type = "problem_batch";
        ae.target_id   = std::to_string(summary.total_files) + "_files";
        // Build the failures array. Bounded to keep the payload
        // manageable — a batch with 50 broken files still has a
        // tidy audit row.
        nlohmann::json failures_j = nlohmann::json::array();
        for (const auto& r : results) {
            if (r.action != ImportAction::Failed) continue;
            nlohmann::json fj = {
                {"filename", r.filename},
                {"stage",    r.stage},
                {"reason",   r.reason},
            };
            if (!r.slug.empty()) {
                fj["slug"] = r.slug;
            }
            if (!r.details.is_null()) {
                fj["details"] = r.details;
            }
            failures_j.push_back(std::move(fj));
        }
        ae.payload = {
            {"on_duplicate", std::string(on_duplicate_name(summary.on_duplicate))},
            {"total_files",  static_cast<int>(summary.total_files)},
            {"imported",     static_cast<int>(summary.imported)},
            {"skipped",      static_cast<int>(summary.skipped)},
            {"overwritten",  static_cast<int>(summary.overwritten)},
            {"failed",       static_cast<int>(summary.failed)},
            {"duration_ms",  summary.duration_ms},
            {"failures",     std::move(failures_j)},
        };
        ae.ip = extract_client_ip(req);
        audit_log_repo::record(pool, ae);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_bulk_import: audit_log::record threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 8) Build the response payload. Two parallel arrays —
    //    `imported[]` (created + overwritten + skipped) and
    //    `failures[]` (parse / validate / repo errors). The
    //    `summary` block carries the totals.
    nlohmann::json imported_j = nlohmann::json::array();
    nlohmann::json failures_j = nlohmann::json::array();
    for (const auto& r : results) {
        if (r.action == ImportAction::Failed) {
            nlohmann::json fj = {
                {"filename", r.filename},
                {"stage",    r.stage},
                {"reason",   r.reason},
            };
            if (!r.slug.empty()) fj["slug"] = r.slug;
            if (!r.details.is_null()) fj["details"] = r.details;
            failures_j.push_back(std::move(fj));
            continue;
        }
        nlohmann::json ij = {
            {"filename",        r.filename},
            {"slug",            r.slug},
            {"id",              r.problem_id},
            {"title",           r.title},
            {"action",          std::string(import_action_name(r.action))},
            {"sample_count",    r.sample_count},
            {"test_case_count", r.test_case_count},
            {"tag_names",       r.tag_names},
        };
        imported_j.push_back(std::move(ij));
    }

    LOG_INFO("admin_bulk_import: batch complete",
             {{"admin_id",       claims.user_id},
              {"total_files",    std::to_string(summary.total_files)},
              {"imported",       std::to_string(summary.imported)},
              {"skipped",        std::to_string(summary.skipped)},
              {"overwritten",    std::to_string(summary.overwritten)},
              {"failed",         std::to_string(summary.failed)},
              {"duration_ms",    std::to_string(summary.duration_ms)},
              {"on_duplicate",   std::string(on_duplicate_name(summary.on_duplicate))}});

    send_success(res, nlohmann::json{
        {"summary",  {
            {"total_files", static_cast<int>(summary.total_files)},
            {"imported",    static_cast<int>(summary.imported)},
            {"skipped",     static_cast<int>(summary.skipped)},
            {"overwritten", static_cast<int>(summary.overwritten)},
            {"failed",      static_cast<int>(summary.failed)},
            {"duration_ms", summary.duration_ms},
            {"on_duplicate", std::string(on_duplicate_name(summary.on_duplicate))},
        }},
        {"imported", std::move(imported_j)},
        {"failures", std::move(failures_j)},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_admin_bulk_import_routes(
        HttpServer&            server,
        ConnectionPool&        pool,
        RateLimiter&           limiter,
        const RateLimitConfig& rate_cfg,
        const JwtConfig&       jwt_cfg) {
    server.post(R"(/api/v1/admin/problems/import)",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_bulk_import_handler(res, req, pool, limiter,
                                          rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_bulk_import: handler threw",
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