// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — admin problem CRUD routes (Phase 3 ★)
//
// SPEC §5.2 / §11 Phase 3 / §15.2 / §15.6 / A18, A19, A20, A27 acceptance:
//   - POST   /api/v1/admin/problems        (🔒 admin, 30/min/user) — create
//   - PUT    /api/v1/admin/problems/:slug  (🔒 admin, 30/min/user) — full replace
//   - DELETE /api/v1/admin/problems/:slug  (🔒 admin, 30/min/user) — soft delete
//
// All three endpoints:
//   1) require_admin(...)  — 401 envelope if no / bad token, 403 if
//                            the authenticated user isn't an admin
//   2) consume_rate_limit(... admin_write_quota(...)) — 429 envelope
//   3) parse JSON body + validate fields (400 envelope on shape / value errors)
//   4) dispatch to problem_repo / tag_repo / test_case_repo
//   5) write one audit_logs row via audit_log_repo::record (strict —
//      lost audit row on a destructive action is a security trail gap)
//   6) respond 201/200/204 + the unified success envelope
//
// Wire shape (request):
//   POST / PUT body:
//     {
//       "slug":          "two-sum",          // required, lowercase [a-z0-9-]
//       "title":         "Two Sum",          // required, 1..200 chars
//       "difficulty":    "easy",             // required, easy|medium|hard
//       "description":   "# ...",            // required, Markdown
//       "time_limit_ms": 1000,               // optional, default 1000
//       "memory_limit_mb": 256,              // optional, default 256
//       "tags":          ["array", "hash"],  // optional, defaults []
//       "samples":       [                   // optional, defaults []
//         { "input": "...", "output": "...",
//           "judge_type": "exact" (default), "order_num": 0 (auto) }
//       ]
//     }
//
// Wire shape (response):
//   201 / 200 + {data: <full problem detail incl. tags + samples>, request_id}
//   204 No Content for DELETE
//
// Soft-delete (SPEC §4.2): the DELETE path sets is_deleted = TRUE
// rather than removing the row. The list / detail public endpoints
// already filter soft-deleted rows out by default, so the row
// immediately disappears from public view but stays around for
// audit / future restore. test_cases of soft-deleted problems are
// preserved (SPEC §4.3 — they may carry the row's maintenance
// counter history).
//
// Audit log payloads:
//   problem.create: {slug, title, difficulty, time_limit, memory_limit,
//                    tag_names[], sample_count}
//   problem.update: {old_slug, slug, title, difficulty, time_limit,
//                    memory_limit, tag_names[], sample_count}
//   problem.delete: {slug, title, hard_delete: false}
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 / 3 module
//     (server.h / error_handler.h / problem_routes.h / tag_routes.h).
//     Tests link this header directly and instantiate the route set
//     with a real ConnectionPool + a (lax) rate limiter + a real
//     JwtConfig.
//   - The handler is the single funnel for each of the three verbs.
//     Each one follows the canonical 6-step pattern from problem_routes.h.
//   - The sample-rewrite path on POST / PUT runs as two separate
//     repo calls (delete_for_problem + per-row insert). Strict
//     atomicity across samples + tags + problem row is enforced by
//     wrapping the whole block in a try/catch and emitting a 500
//     envelope on any partial failure; the worst case is "tag set
//     replaced, samples partially rewritten", which the admin can
//     recover from by re-issuing PUT.
//   - Because of the ODR-collision risk between
//     `litecode::detail::req_string` (defined identically in
//     problem_repo / tag_repo / test_case_repo / audit_log_repo),
//     main.cpp does NOT smoke-register this header. End-to-end
//     coverage is provided by tests/unit/test_admin_problem_crud.cpp.
//     The same constraint already excludes problem_routes.h from
//     main.cpp; documented in main.cpp's comment at lines 192-201.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                          // RateLimitConfig / JwtConfig
#include "../db/audit_log_repo.h"               // audit_log_repo::record + kActionProblem*
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/problem_repo.h"                 // problem_repo::create / update / soft_delete / find_by_*
#include "../db/special_judge_repo.h"           // special_judge_repo::find_by_problem_id / upsert / remove_by_problem_id (v1.3.1)
#include "../db/tag_repo.h"                     // tag_repo::find_or_create_many / replace / list_tags_for_problem
#include "../db/test_case_repo.h"               // test_case_repo::list_samples_for_problem / replace_for_problem
#include "../db/problem_revisions_repo.h"      // problem_revisions_repo::record_best_effort (v1.2.12)
#include "../logger.h"                          // LOG_INFO / LOG_WARN
#include "../middleware/admin_middleware.h"     // require_admin
#include "../middleware/rate_limit.h"           // consume_rate_limit / admin_write_quota / extract_client_ip
#include "../routes/error_handler.h"            // parse_json_body / ErrorCode / send_error
#include "../routes/problem_routes.h"           // serialize_problem_detail (reused for response shape)
#include "../server.h"                          // HttpServer / send_success / send_created / send_no_content
#include "../utils/security.h"                  // security::validate_path_component_len / has_* (Phase 6 ★ v1.2.45)

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Body-shape parsing helpers
//
//  The admin POST / PUT body is a flat JSON object with required
//  scalar fields, optional numeric fields, and two array-typed
//  fields (tags / samples). We do all shape + value validation
//  here so the handler body is one straight-line repo dispatch.
//
//  Anti-enumeration: every validation failure carries the field
//  name and the rejected value in the envelope's `details`, so the
//  admin UI can highlight the offending input without re-running
//  validation client-side. We do NOT echo arbitrary user bytes —
//  the value is truncated to 64 chars to keep the error body
//  bounded.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline constexpr std::size_t kAdminDetailValueMax = 64;

inline std::string truncate_for_envelope(const std::string& v) {
    if (v.size() <= kAdminDetailValueMax) return v;
    return v.substr(0, kAdminDetailValueMax) + "...";
}

// require_string — extract a required string field from the body.
// Returns the value on success. On missing / wrong-type, writes a
// 400 envelope and returns std::nullopt so the caller can `if (!v) return`.
inline std::optional<std::string> require_string(const nlohmann::json& body,
                                                 httplib::Response& res,
                                                 const char* field) {
    if (!body.contains(field) || !body[field].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("missing or non-string field '") + field + "'",
                   {{"field", field}});
        return std::nullopt;
    }
    return body[field].get<std::string>();
}

// optional_string — extract an OPTIONAL string field from the body.
// Same shape as require_string, but absent / null → std::nullopt
// (caller falls back to the empty/default value). Non-string when
// present is a 400 envelope. Used by v1.3.2's `template` field.
inline std::optional<std::string> optional_string(const nlohmann::json& body,
                                                   httplib::Response& res,
                                                   const char* field) {
    if (!body.contains(field) || body[field].is_null()) {
        return std::nullopt;
    }
    if (!body[field].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field +
                   "' must be a string when present",
                   {{"field", field}});
        return std::nullopt;
    }
    return body[field].get<std::string>();
}

// Per SPEC §4.2 — template body is MEDIUMTEXT but the admin route
// applies a practical 64 KB cap so a runaway copy-paste from a
// 700-page book doesn't blow the request body. The repo accepts
// whatever comes through; this is a request-shape guard, not a DB
// constraint. SPJ source uses 256 KB (SPEC §4.4 — kMaxSpjSourceLenAdmin).
inline constexpr std::size_t kMaxProblemTemplateAdmin = 64 * 1024;

// require_judge_type — validate against the SPEC §4.3 ENUM. Empty
// (absent / null) maps to "exact" to match the column DEFAULT. Any
// non-empty value outside the enum is a 400.
inline std::optional<std::string> require_judge_type(
        const nlohmann::json& body,
        httplib::Response& res,
        const char* field,
        const std::string& default_value = "exact") {
    if (!body.contains(field) || body[field].is_null()) {
        return default_value;
    }
    if (!body[field].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field +
                   "' must be a string when present",
                   {{"field", field}});
        return std::nullopt;
    }
    const std::string v = body[field].get<std::string>();
    if (v.empty()) return default_value;
    if (v != "exact" && v != "ignore_trailing" &&
        v != "float_eps" && v != "special" &&
        v != "ignore_case" && v != "ignore_all_whitespace") {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "judge_type must be one of: exact, ignore_trailing, "
                   "float_eps, special, ignore_case, "
                   "ignore_all_whitespace",
                   {{"field", field},
                    {"value", truncate_for_envelope(v)}});
        return std::nullopt;
    }
    return v;
}

// Parse the optional `time_limit_ms` / `memory_limit_mb` numeric
// fields. Absent → std::nullopt (the repo falls back to SPEC §2.2
// defaults). Present but wrong type / out of range → 400 envelope.
inline std::optional<int> optional_int_field(const nlohmann::json& body,
                                             httplib::Response& res,
                                             const char* field,
                                             int min_inclusive,
                                             int max_inclusive) {
    if (!body.contains(field) || body[field].is_null()) {
        return std::nullopt;
    }
    if (!body[field].is_number_integer()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field +
                   "' must be an integer when present",
                   {{"field", field}});
        return std::nullopt;
    }
    const int v = body[field].get<int>();
    if (v < min_inclusive || v > max_inclusive) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field + "' out of range",
                   {{"field", field},
                    {"value", std::to_string(v)},
                    {"min",   std::to_string(min_inclusive)},
                    {"max",   std::to_string(max_inclusive)}});
        return std::nullopt;
    }
    return v;
}

// parse_tags_array — extract the optional `tags` array. Each entry
// must be a non-empty string (1..50 chars after trimming). On any
// shape failure, writes a 400 envelope and returns std::nullopt.
// The returned vector is the canonicalized (trimmed) list — never
// the raw user bytes — so downstream find_or_create_many / replace
// can hand it straight to the repo.
inline std::optional<std::vector<std::string>> parse_tags_array(
        const nlohmann::json& body,
        httplib::Response& res) {
    std::vector<std::string> out;
    if (!body.contains("tags") || body["tags"].is_null()) {
        return out;   // absent / null → empty
    }
    if (!body["tags"].is_array()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "tags must be an array of strings",
                   {{"field", "tags"}});
        return std::nullopt;
    }
    for (std::size_t i = 0; i < body["tags"].size(); ++i) {
        const auto& el = body["tags"][i];
        if (!el.is_string()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "tags entries must be strings",
                       {{"field", "tags"},
                        {"index", std::to_string(i)}});
            return std::nullopt;
        }
        const std::string raw = el.get<std::string>();
        // Per-element validation: empty string is rejected so we
        // don't blow up later in tag_repo::validate_tag_name. The
        // trimmed form is what the repo will see.
        const std::string trimmed = trim_tag_name(raw);
        std::string err;
        if (trimmed.empty() || !validate_tag_name(trimmed, &err)) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       std::string("invalid tag name at index ") +
                                   std::to_string(i) + ": " + err,
                       {{"field", "tags"},
                        {"index", std::to_string(i)},
                        {"value", truncate_for_envelope(raw)}});
            return std::nullopt;
        }
        out.push_back(trimmed);
    }
    return out;
}

// parse_samples_array — extract the optional `samples` array. Each
// entry must be an object with required `input` / `output` strings;
// `judge_type` defaults to "exact"; `order_num` defaults to the
// array index. On any shape failure, writes a 400 envelope and
// returns std::nullopt. The returned vector's order matches the
// caller-supplied array, so the route can hand it straight to
// replace_for_problem (which inserts in iteration order).
inline std::optional<std::vector<litecode::SampleCaseRow>> parse_samples_array(
        const nlohmann::json& body,
        httplib::Response& res) {
    std::vector<litecode::SampleCaseRow> out;
    if (!body.contains("samples") || body["samples"].is_null()) {
        return out;
    }
    if (!body["samples"].is_array()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "samples must be an array of objects",
                   {{"field", "samples"}});
        return std::nullopt;
    }
    for (std::size_t i = 0; i < body["samples"].size(); ++i) {
        const auto& el = body["samples"][i];
        if (!el.is_object()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "samples entries must be objects",
                       {{"field", "samples"},
                        {"index", std::to_string(i)}});
            return std::nullopt;
        }
        // input (required)
        if (!el.contains("input") || !el["input"].is_string()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "samples[].input must be a string",
                       {{"field", "samples"},
                        {"index", std::to_string(i)},
                        {"subfield", "input"}});
            return std::nullopt;
        }
        // output (required)
        if (!el.contains("output") || !el["output"].is_string()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "samples[].output must be a string",
                       {{"field", "samples"},
                        {"index", std::to_string(i)},
                        {"subfield", "output"}});
            return std::nullopt;
        }
        SampleCaseRow s;
        s.input           = el["input"].get<std::string>();
        s.expected_output = el["output"].get<std::string>();
        // judge_type (optional, defaults to "exact")
        const auto jt = require_judge_type(el, res, "judge_type");
        if (!jt.has_value()) {
            // require_judge_type already wrote a 400 envelope.
            return std::nullopt;
        }
        s.judge_type = *jt;
        // order_num (optional, defaults to the array index)
        s.order_num = static_cast<int>(i);
        if (el.contains("order_num") && !el["order_num"].is_null()) {
            if (!el["order_num"].is_number_integer()) {
                send_error(res, 400, ErrorCode::INVALID_INPUT,
                           "samples[].order_num must be an integer",
                           {{"field", "samples"},
                            {"index", std::to_string(i)},
                            {"subfield", "order_num"}});
                return std::nullopt;
            }
            s.order_num = el["order_num"].get<int>();
        }
        out.push_back(std::move(s));
    }
    return out;
}

// extract_slug_from_admin_path — strip the
//   GET/PUT/DELETE /api/v1/admin/problems/<slug>
// prefix from req.path and hand the remainder to problem_repo's
// validate_slug(). Returns std::nullopt on any shape failure;
// the caller emits a 400 envelope.
//
// We deliberately don't reuse litecode::detail::extract_slug_from_path
// because that helper hardcodes the "/api/v1/problems/" prefix
// (it's used by the public detail endpoint, where the path is
// shorter). Sharing one helper across the two routes would force
// the public detail path to grow a branch it doesn't need.
//
// Phase 6 ★ v1.2.45 hardening (SPEC §15.2):
//   - Length cap (security::validate_path_component_len)
//   - Path-traversal trip-wire ("..")
//   - Control-char / HTML-special-char / U+2028-2029 rejection
//
//   These mirror the rules in problem_routes.h's
//   detail::parse_slug_param so a "GET /api/v1/problems/<slug>" and
//   a "PUT /api/v1/admin/problems/<slug>" can never disagree on what
//   counts as a well-formed slug. The check is cheap (~20 ns) so
//   doing it twice (route + repo) is acceptable.
inline std::optional<std::string> extract_slug_from_admin_path(
        const httplib::Request& req,
        std::string_view suffix = "") {
    static constexpr std::string_view kPrefix =
        "/api/v1/admin/problems/";
    const std::string& path = req.path;
    if (path.size() <= kPrefix.size()) return std::nullopt;
    if (path.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    std::string_view rest(path.data() + kPrefix.size(),
                          path.size() - kPrefix.size());
    // v1.3.1: SPJ 三端点（PUT/GET/DELETE /admin/problems/:slug/special-judge）
    // 的 path 比标准 admin/problems/:slug 多了 "/special-judge" 后缀。
    // 这里可选地把后缀剥掉,让 SPJ handler 复用同一份 slug validator。
    if (!suffix.empty()) {
        if (rest.size() <= suffix.size()) return std::nullopt;
        if (rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return std::nullopt;
        }
        rest = std::string_view(rest.data(), rest.size() - suffix.size());
    }
    if (rest.empty()) return std::nullopt;
    if (!litecode::security::validate_path_component_len(rest)) return std::nullopt;
    if (rest.find("..") != std::string_view::npos) return std::nullopt;
    if (litecode::security::has_control_chars(rest)) return std::nullopt;
    if (litecode::security::has_html_special_chars(rest)) return std::nullopt;
    if (litecode::security::has_json_special_chars(rest)) return std::nullopt;
    std::string err;
    const std::string s(rest);
    if (!validate_slug(s, &err)) return std::nullopt;
    return s;
}

// validate_problem_patch — shared validator used by both POST and
// PUT. The handler has already pulled slug / title / difficulty /
// description / time_limit_ms / memory_limit_mb out of the body and
// built a ProblemRow-shaped `row`. We run the cheap problem_repo
// validators here so we never reach the repo with a malformed
// value (the repo re-validates as a defense in depth, but surfacing
// the failure at the route boundary gives a cleaner envelope).
//
// On any failure, writes a 400 envelope and returns false. The
// caller bails on `false`.
inline bool validate_problem_patch(const ProblemRow& row,
                                   httplib::Response& res) {
    std::string err;
    if (!validate_slug(row.slug, &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, err,
                   {{"field", "slug"},
                    {"value", truncate_for_envelope(row.slug)}});
        return false;
    }
    if (!validate_title(row.title, &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, err,
                   {{"field", "title"}});
        return false;
    }
    if (!validate_difficulty(row.difficulty, &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, err,
                   {{"field", "difficulty"},
                    {"value", truncate_for_envelope(row.difficulty)}});
        return false;
    }
    if (!validate_time_limit(row.time_limit, &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, err,
                   {{"field", "time_limit_ms"},
                    {"value", std::to_string(row.time_limit)}});
        return false;
    }
    if (!validate_memory_limit(row.memory_limit, &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, err,
                   {{"field", "memory_limit_mb"},
                    {"value", std::to_string(row.memory_limit)}});
        return false;
    }
    return true;
}

// apply_tag_and_sample_patch — the shared "after the problem row is
// in place" step. Resolves tag names via find_or_create_many,
// replaces the problem's tag set via tag_repo::replace, and writes
// the supplied samples via test_case_repo::replace_for_problem
// (which is atomic on its own connection).
//
// Throws on repo failure; the handler maps to a 500 envelope.
//
// `samples` may be empty (means "delete all samples" on PUT or
// "no samples to add" on POST).
inline void apply_tag_and_sample_patch(
        ConnectionPool& pool,
        int problem_id,
        const std::vector<std::string>& tag_names,
        const std::vector<SampleCaseRow>& samples) {
    // 1) Tags. find_or_create_many is a no-op when the input is
    //    empty (returns an empty vector). For a non-empty list it
    //    resolves / creates every name and returns rows in input
    //    order, which we hand straight to tag_repo::replace.
    if (!tag_names.empty()) {
        const auto resolved = tag_repo::find_or_create_many(pool, tag_names);
        std::vector<int> tag_ids;
        tag_ids.reserve(resolved.size());
        for (const auto& t : resolved) tag_ids.push_back(t.id);
        tag_repo::replace(pool, problem_id, tag_ids);
    } else {
        // Empty list → clear every tag. (Tagset becomes empty.)
        tag_repo::replace(pool, problem_id, {});
    }

    // 2) Samples. replace_for_problem is atomic (BEGIN/COMMIT on
    //    one connection), so a partial failure rolls back cleanly
        //    and the problem keeps its old samples intact.
    test_case_repo::replace_for_problem(
        pool, problem_id, samples,
        /*is_sample_for_all_rows=*/true);
}

// build_response_payload — read the just-written row back from the
// DB and project it through serialize_problem_detail. Centralized
// so POST and PUT return the same shape (and the same detail
// endpoint's projection — the admin and public clients should
// never see two different shapes for the same row).
//
// Throws on repo failure.
inline nlohmann::json build_response_payload(ConnectionPool& pool,
                                             int problem_id) {
    // We need a ProblemRow to feed serialize_problem_detail; the
    // repo's find_by_id returns the full row when the parent exists.
    // We use the include_deleted=true form so the admin can read
    // back the row even if a race elsewhere flips is_deleted
    // (admin write paths own the lifecycle).
    auto row = problem_repo::find_by_id(pool, problem_id,
                                       /*include_deleted=*/true);
    if (!row.has_value()) {
        // Should be impossible: we just wrote this id. Surface a
        // generic INTERNAL_ERROR — the handler maps to 500.
        throw std::runtime_error(
            "admin_problem_routes: row " + std::to_string(problem_id) +
            " disappeared between write and readback");
    }
    const auto tags    = tag_repo::list_tags_for_problem(pool, problem_id);
    const auto samples = test_case_repo::list_samples_for_problem(
        pool, problem_id);
    return serialize_problem_detail(*row, tags, samples);
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/admin/problems   (SPEC §5.2, A18)
//
//  Wire flow:
//    1) require_admin(...)   — 401 / 403 envelope on auth failure
//    2) consume_rate_limit(... admin_write_quota) — 429 envelope
//    3) parse_json_body + per-field validation   — 400 envelope
//    4) tag_repo::find_or_create_many + tag_repo::replace
//    5) problem_repo::create
//    6) test_case_repo::replace_for_problem
//    7) audit_log_repo::record (strict)
//    8) send_created(201) + serialize_problem_detail
//
//  Failure paths that are NOT 400:
//    - slug already exists → 409 CONFLICT
//    - any repo / DB error → 500 INTERNAL_ERROR
// ────────────────────────────────────────────────────────────────────────────

inline void admin_create_problem_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    // 1) Auth gate. 401 on missing / bad token; 403 on non-admin.
    const auto claims = require_admin(req, jwt_cfg);

    // 2) Rate limit (admin.write bucket, keyed by user_id).
    consume_rate_limit(res, req, limiter, admin_write_quota(rate_cfg));

    // 3) Body parse + per-field validation. Any failure has already
    //    written a 400 envelope; we bail on `nullopt`.
    const auto body = parse_json_body(req, res);
    if (!body) return;

    const auto slug_v        = detail::require_string(*body, res, "slug");
    if (!slug_v)        return;
    const auto title_v       = detail::require_string(*body, res, "title");
    if (!title_v)       return;
    const auto difficulty_v  = detail::require_string(*body, res, "difficulty");
    if (!difficulty_v)  return;
    const auto description_v = detail::require_string(*body, res, "description");
    if (!description_v) return;
    // v1.3.2: optional per-problem code template, capped at 64 KB.
    // Absent / null / "" → empty row.template_, which the public
    // detail endpoint surfaces as JSON "" and problem.html treats as
    // "fall back to the built-in C++/C skeleton".
    const auto template_v    = detail::optional_string(*body, res, "template");
    if (!template_v)    return;
    if (template_v->size() > detail::kMaxProblemTemplateAdmin) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "template exceeds 64 KB ceiling",
                   {{"field", "template"},
                    {"size",  std::to_string(template_v->size())}});
        return;
    }
    const auto tags_v        = detail::parse_tags_array(*body, res);
    if (!tags_v)        return;
    const auto samples_v     = detail::parse_samples_array(*body, res);
    if (!samples_v)     return;

    ProblemRow row;
    row.slug        = *slug_v;
    row.title       = *title_v;
    row.difficulty  = *difficulty_v;
    row.description = *description_v;
    row.template_   = *template_v;          // v1.3.2: per-problem code template
    // time_limit / memory_limit: optional in the body. The repo
    // applies the SPEC defaults (1000 / 256) when the field is 0,
    // so we leave the row at 0 when the body omitted the field.
    {
        const auto t = detail::optional_int_field(
            *body, res, "time_limit_ms",
            kMinTimeLimitMs, kMaxTimeLimitMs);
        if (req.body.find("time_limit_ms") != std::string::npos && !t) {
            // optional_int_field wrote a 400 envelope when the
            // field is present-and-malformed; bail.
            return;
        }
        if (t) row.time_limit = *t;
    }
    {
        const auto m = detail::optional_int_field(
            *body, res, "memory_limit_mb",
            kMinMemoryLimitMb, kMaxMemoryLimitMb);
        if (req.body.find("memory_limit_mb") != std::string::npos && !m) {
            return;
        }
        if (m) row.memory_limit = *m;
    }

    if (!detail::validate_problem_patch(row, res)) return;

    // 4) Create the row. On slug collision → 409. On driver error
    //    → 500.
    int new_id = 0;
    try {
        new_id = problem_repo::create(pool, row);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_create: problem_repo::create threw",
                  {{"slug",   row.slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (new_id == 0) {
        // problem_repo maps Duplicate entry (errno 1062) to 0.
        // Slug uniqueness collision → 409 (SPEC §5.7).
        send_error(res, 409, ErrorCode::CONFLICT,
                   "slug already exists",
                   {{"field", "slug"},
                    {"value", row.slug}});
        return;
    }

    // 5) Tags + samples. Wrapped in try/catch so any repo hiccup
    //    after the problem row was written still maps to a clean
    //    500 envelope (the row is left in place — admin can re-issue
    //    with corrected body to attach tags/samples; or delete and
    //    recreate).
    try {
        detail::apply_tag_and_sample_patch(pool, new_id, *tags_v, *samples_v);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_create: tag/sample patch threw",
                  {{"problem_id", std::to_string(new_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 6) Problem-revision snapshot (best-effort; v1.2.12). Failure
    //    here is logged and swallowed — it does NOT escalate to 500.
    //    audit_logs (the next step) is the durable security trail;
    //    problem_revisions is the content-history layer on top of it,
    //    and a missing revision on a single CREATE is recoverable
    //    (the next PUT will land a new revision_no=2 row, and the
    //    "what was the problem at version 1?" gap is small). The
    //    revision_id is captured for the audit payload so future
    //    investigators can correlate the security row with the
    //    content row by hand.
    std::int64_t revision_id_for_audit = 0;
    {
        RevisionEntry re;
        re.problem_id      = new_id;
        re.revision_no     = 0;                                // 0 ⇒ repo allocates MAX+1
        re.editor_id       = std::stoi(claims.user_id);
        re.editor_username = claims.username;
        re.editor_ip       = extract_client_ip(req);
        re.action          = problem_revisions_repo::kActionRevisionCreate;
        re.slug            = row.slug;
        re.title           = row.title;
        re.difficulty      = row.difficulty;
        re.time_limit      = row.time_limit;
        re.memory_limit    = row.memory_limit;
        re.description     = row.description;
        re.tags_snapshot   = *tags_v;                          // vector<string> → nlohmann::json
        re.samples_snapshot = nlohmann::json::array();
        for (const auto& s : *samples_v) {
            re.samples_snapshot.push_back({
                {"input",  s.input},
                {"output", s.expected_output},
            });
        }
        re.summary = std::nullopt;                            // CREATE doesn't have a "before"
        try {
            revision_id_for_audit =
                problem_revisions_repo::record_best_effort(pool, re);
        } catch (const std::exception& e) {
            // record_best_effort already LOG_WARNs; the belt-and-
            // suspenders catch is for the case where a future
            // hardening pass changes it to strict.
            LOG_WARN("admin_problem_create: revision snapshot threw",
                     {{"problem_id", std::to_string(new_id)},
                      {"type",       typeid(e).name()},
                      {"reason",     e.what()}});
            revision_id_for_audit = 0;
        }
        if (revision_id_for_audit > 0) {
            LOG_INFO("admin_problem_create: revision recorded",
                     {{"problem_id",  std::to_string(new_id)},
                      {"revision_id", std::to_string(revision_id_for_audit)}});
        }
        // revision_id_for_audit = 0 ⇒ missing snapshot; the audit
        // payload below carries `revision_id = 0` so the audit UI
        // can flag the row as "(no revision recorded)".
    }

    // 7) Audit log (strict). Failure here MUST surface as 500 —
    //    a lost audit row on a destructive admin action is a
    //    security trail gap.
    try {
        AuditEntry ae;
        ae.admin_id    = std::stoi(claims.user_id);
        ae.action      = audit_log_repo::kActionProblemCreate;
        ae.target_type = "problem";
        ae.target_id   = row.slug;
        ae.payload     = {
            {"slug",           row.slug},
            {"title",          row.title},
            {"difficulty",     row.difficulty},
            {"time_limit_ms",  row.time_limit},
            {"memory_limit_mb", row.memory_limit},
            {"tag_names",      *tags_v},
            {"sample_count",   static_cast<int>(samples_v->size())},
            {"revision_id",    revision_id_for_audit},
        };
        ae.ip           = extract_client_ip(req);
        audit_log_repo::record(pool, ae);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_create: audit_log::record threw",
                  {{"problem_id", std::to_string(new_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 8) Read-back + serialize. We deliberately read the row back
    //    so the response carries the canonical `created_at` /
    //    `updated_at` strings instead of fabricating them from
    //    request bytes.
    nlohmann::json payload;
    try {
        payload = detail::build_response_payload(pool, new_id);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_create: build_response_payload threw",
                  {{"problem_id", std::to_string(new_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    LOG_INFO("admin_problem_create: created",
             {{"problem_id", std::to_string(new_id)},
              {"slug",       row.slug},
              {"admin_id",   claims.user_id}});

    send_created(res, payload);
}

// ────────────────────────────────────────────────────────────────────────────
//  PUT /api/v1/admin/problems/:slug   (SPEC §5.2, A19)
//
//  Full replace: the request body carries every mutable field. Slug
//  may be renamed (the URL's `:slug` is the OLD slug; the body's
//  `slug` is the NEW slug). Tags + samples are replaced wholesale.
//
//  Wire flow:
//    1) require_admin
//    2) consume_rate_limit (admin.write)
//    3) extract_slug_from_path (400 envelope on bad shape)
//    4) parse_json_body + validate fields (400)
//    5) problem_repo::update (404 → no-such-slug; 409 → slug collision)
//    6) tag_repo::replace + test_case_repo::replace_for_problem
//    7) audit_log_repo::record (action=problem.update)
//    8) send_success(200) + serialize_problem_detail
// ────────────────────────────────────────────────────────────────────────────

inline void admin_update_problem_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    const auto claims = require_admin(req, jwt_cfg);
    consume_rate_limit(res, req, limiter, admin_write_quota(rate_cfg));

    // The slug-from-path uses the admin-specific prefix-strip helper
    // (the public detail endpoint's helper hardcodes
    // "/api/v1/problems/" which doesn't match this route).
    const auto old_slug_v = detail::extract_slug_from_admin_path(req);
    if (!old_slug_v) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "slug must be 1-100 chars of [a-z0-9-], not starting "
                   "or ending with '-'",
                   {{"field", "slug"},
                    {"value", std::string(req.path)}});
        return;
    }
    const std::string old_slug = *old_slug_v;

    const auto body = parse_json_body(req, res);
    if (!body) return;

    const auto slug_v        = detail::require_string(*body, res, "slug");
    if (!slug_v)        return;
    const auto title_v       = detail::require_string(*body, res, "title");
    if (!title_v)       return;
    const auto difficulty_v  = detail::require_string(*body, res, "difficulty");
    if (!difficulty_v)  return;
    const auto description_v = detail::require_string(*body, res, "description");
    if (!description_v) return;
    // v1.3.2: optional per-problem code template, capped at 64 KB.
    // PUT is "full replace" — absent ⇒ "" (overwrites any previous
    // value with the empty string). The front-end admin form always
    // round-trips the existing template from GET /problems/:slug,
    // so on a stable edit path the value comes back unchanged.
    // A null template here would break the "last write wins" contract.
    const auto template_v    = detail::optional_string(*body, res, "template");
    if (!template_v)    return;
    if (template_v->size() > detail::kMaxProblemTemplateAdmin) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "template exceeds 64 KB ceiling",
                   {{"field", "template"},
                    {"size",  std::to_string(template_v->size())}});
        return;
    }
    const auto tags_v        = detail::parse_tags_array(*body, res);
    if (!tags_v)        return;
    const auto samples_v     = detail::parse_samples_array(*body, res);
    if (!samples_v)     return;

    ProblemRow patch;
    patch.slug        = *slug_v;
    patch.title       = *title_v;
    patch.difficulty  = *difficulty_v;
    patch.description = *description_v;
    patch.template_   = *template_v;          // v1.3.2: full-replace, "" if absent
    {
        const auto t = detail::optional_int_field(
            *body, res, "time_limit_ms",
            kMinTimeLimitMs, kMaxTimeLimitMs);
        if (req.body.find("time_limit_ms") != std::string::npos && !t) return;
        if (t) patch.time_limit = *t;
    }
    {
        const auto m = detail::optional_int_field(
            *body, res, "memory_limit_mb",
            kMinMemoryLimitMb, kMaxMemoryLimitMb);
        if (req.body.find("memory_limit_mb") != std::string::npos && !m) return;
        if (m) patch.memory_limit = *m;
    }

    if (!detail::validate_problem_patch(patch, res)) return;

    // Capture the pre-update title for the audit log. We fetch the
    // row BEFORE the update (with include_deleted=true so a row
    // that's mid-restore still surfaces its title).
    std::string pre_title;
    try {
        const auto pre = problem_repo::find_by_slug(
            pool, old_slug, /*include_deleted=*/true);
        if (pre.has_value()) pre_title = pre->title;
    } catch (const std::exception&) {
        // Best-effort. If the SELECT fails (e.g. the row truly
        // doesn't exist) the UPDATE below will surface a 404 and
        // the title stays empty in the audit row.
    }

    // Update. The repo throws ProblemNotFoundError on no match
    // (-> 404) and ProblemAlreadyExistsError on slug collision
    // (-> 409). Anything else is a 500.
    bool updated = false;
    try {
        updated = problem_repo::update(pool, old_slug, patch);
    } catch (const ProblemNotFoundError& e) {
        LOG_INFO("admin_problem_update: not found",
                 {{"slug", old_slug}});
        send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found",
                   {{"slug", old_slug}});
        return;
    } catch (const ProblemAlreadyExistsError& e) {
        send_error(res, 409, ErrorCode::CONFLICT,
                   "slug already exists",
                   {{"field", "slug"},
                    {"value", patch.slug}});
        return;
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_update: problem_repo::update threw",
                  {{"old_slug", old_slug},
                   {"new_slug", patch.slug},
                   {"type",     typeid(e).name()},
                   {"reason",   e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    // updated=false means "row matched but every column was already
    // the new value" — that's still a successful 200. We don't 404
    // on that case.
    (void)updated;

    // Look up the new id (might be the same as the old slug's id).
    int new_id = 0;
    try {
        const auto row_after = problem_repo::find_by_slug(
            pool, patch.slug, /*include_deleted=*/true);
        if (!row_after.has_value()) {
            // Should be impossible — we just updated it. Surface
            // the surprise as 500.
            throw std::runtime_error("admin_problem_update: row "
                                     + patch.slug + " missing after update");
        }
        new_id = row_after->id;
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_update: find_by_slug after update threw",
                  {{"slug", patch.slug},
                   {"type", typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // Tag + sample rewrite.
    try {
        detail::apply_tag_and_sample_patch(pool, new_id, *tags_v, *samples_v);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_update: tag/sample patch threw",
                  {{"problem_id", std::to_string(new_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // Problem-revision snapshot (best-effort; v1.2.12). Mirror of
    // the CREATE step. revision_no=0 lets the repo allocate
    // COALESCE(MAX(revision_no),0)+1, so a PUT after N previous
    // revisions lands at N+1. `summary` is a short human-readable
    // line listing what changed (≤200 chars). revision_id is fed
    // into the audit payload below.
    std::int64_t revision_id_for_audit = 0;
    {
        RevisionEntry re;
        re.problem_id      = new_id;
        re.revision_no     = 0;                                // 0 ⇒ repo allocates MAX+1
        re.editor_id       = std::stoi(claims.user_id);
        re.editor_username = claims.username;
        re.editor_ip       = extract_client_ip(req);
        re.action          = problem_revisions_repo::kActionRevisionUpdate;
        re.slug            = patch.slug;                       // NEW slug (post-rename)
        re.title           = patch.title;
        re.difficulty      = patch.difficulty;
        re.time_limit      = patch.time_limit;
        re.memory_limit    = patch.memory_limit;
        re.description     = patch.description;
        re.tags_snapshot   = *tags_v;
        re.samples_snapshot = nlohmann::json::array();
        for (const auto& s : *samples_v) {
            re.samples_snapshot.push_back({
                {"input",  s.input},
                {"output", s.expected_output},
            });
        }

        // Compact one-liner. Only fields that changed get a
        // mention; falls back to "updated" when nothing visibly
        // differs (PUT body matched the current row byte-for-byte).
        std::string summary;
        if (old_slug != patch.slug) {
            summary += "slug: " + old_slug + " -> " + patch.slug + "; ";
        }
        if (!pre_title.empty() && pre_title != patch.title) {
            summary += "title changed; ";
        }
        if (summary.empty()) {
            summary = "updated";
        }
        if (summary.size() > 200) summary.resize(200);
        re.summary = summary;

        try {
            revision_id_for_audit =
                problem_revisions_repo::record_best_effort(pool, re);
        } catch (const std::exception& e) {
            LOG_WARN("admin_problem_update: revision snapshot threw",
                     {{"problem_id", std::to_string(new_id)},
                      {"type",       typeid(e).name()},
                      {"reason",     e.what()}});
            revision_id_for_audit = 0;
        }
        if (revision_id_for_audit > 0) {
            LOG_INFO("admin_problem_update: revision recorded",
                     {{"problem_id",  std::to_string(new_id)},
                      {"revision_id", std::to_string(revision_id_for_audit)},
                      {"old_slug",    old_slug},
                      {"new_slug",    patch.slug}});
        }
    }

    // Audit log.
    try {
        AuditEntry ae;
        ae.admin_id    = std::stoi(claims.user_id);
        ae.action      = audit_log_repo::kActionProblemUpdate;
        ae.target_type = "problem";
        ae.target_id   = patch.slug;            // NEW slug
        ae.payload     = {
            {"old_slug",        old_slug},
            {"slug",            patch.slug},
            {"old_title",       pre_title},
            {"title",           patch.title},
            {"difficulty",      patch.difficulty},
            {"time_limit_ms",   patch.time_limit},
            {"memory_limit_mb", patch.memory_limit},
            {"tag_names",       *tags_v},
            {"sample_count",    static_cast<int>(samples_v->size())},
            {"revision_id",     revision_id_for_audit},
        };
        ae.ip           = extract_client_ip(req);
        audit_log_repo::record(pool, ae);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_update: audit_log::record threw",
                  {{"problem_id", std::to_string(new_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    nlohmann::json payload;
    try {
        payload = detail::build_response_payload(pool, new_id);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_update: build_response_payload threw",
                  {{"problem_id", std::to_string(new_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    LOG_INFO("admin_problem_update: updated",
             {{"problem_id", std::to_string(new_id)},
              {"old_slug",   old_slug},
              {"new_slug",   patch.slug},
              {"admin_id",   claims.user_id}});

    send_success(res, payload);
}

// ────────────────────────────────────────────────────────────────────────────
//  DELETE /api/v1/admin/problems/:slug   (SPEC §5.2, A20)
//
//  Soft-delete: UPDATE is_deleted = TRUE. Idempotent — a second
//  DELETE on the same slug returns 404 (no live row matches).
//  test_cases of the soft-deleted row are intentionally preserved
//  so a future restore keeps them intact; the public detail endpoint
//  already filters soft-deleted rows out.
//
//  Wire flow:
//    1) require_admin
//    2) consume_rate_limit (admin.write)
//    3) extract_slug_from_path (400 on bad shape)
//    4) fetch pre-delete title for the audit payload (best-effort;
//       if it fails because the row truly doesn't exist, we still
//       hit the soft_delete below which will 404)
//    5) problem_repo::soft_delete
//         - returns false (no live row) → 404
//         - throws → 500
//    6) audit_log_repo::record (action=problem.delete)
//
//    NOTE (v1.2.12): unlike POST/PUT, DELETE does NOT write a
//    problem_revisions row. Rationale: a soft-delete only flips
//    `is_deleted = TRUE`; the existing revisions remain the truthful
//    snapshot history of the problem's content. Writing a
//    "deleted" revision here would only duplicate the audit row
//    without new content. If a future restore-cycle re-publishes
//    the problem, the next CREATE / UPDATE will land at MAX(revision_no)+1
//    so the deletion is implicit via the gap in revision_no / slug
//    history.
//
//    7) send_no_content(204)
// ────────────────────────────────────────────────────────────────────────────

inline void admin_delete_problem_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    const auto claims = require_admin(req, jwt_cfg);
    consume_rate_limit(res, req, limiter, admin_write_quota(rate_cfg));

    const auto slug_v = detail::extract_slug_from_admin_path(req);
    if (!slug_v) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "slug must be 1-100 chars of [a-z0-9-], not starting "
                   "or ending with '-'",
                   {{"field", "slug"},
                    {"value", std::string(req.path)}});
        return;
    }
    const std::string slug = *slug_v;

    // Pre-fetch the title for the audit payload (best-effort).
    std::string title;
    try {
        const auto row = problem_repo::find_by_slug(
            pool, slug, /*include_deleted=*/true);
        if (row.has_value()) title = row->title;
    } catch (const std::exception&) {
        // Swallow — soft_delete will surface a clean 404 / 500 below.
    }

    bool deleted = false;
    try {
        deleted = problem_repo::soft_delete(pool, slug);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_delete: problem_repo::soft_delete threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!deleted) {
        // No live row matched — either the slug never existed or
        // it was already soft-deleted. Either way: 404 (SPEC §5.2
        // contract; second-DELETE is idempotent at 404).
        LOG_INFO("admin_problem_delete: not found",
                 {{"slug", slug}});
        send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found",
                   {{"slug", slug}});
        return;
    }

    // Audit log.
    try {
        AuditEntry ae;
        ae.admin_id    = std::stoi(claims.user_id);
        ae.action      = audit_log_repo::kActionProblemDelete;
        ae.target_type = "problem";
        ae.target_id   = slug;
        ae.payload     = {
            {"slug",        slug},
            {"title",       title},
            {"hard_delete", false},   // SPEC §4.2 — soft delete only
        };
        ae.ip           = extract_client_ip(req);
        audit_log_repo::record(pool, ae);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_problem_delete: audit_log::record threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    LOG_INFO("admin_problem_delete: soft-deleted",
             {{"slug",     slug},
              {"admin_id", claims.user_id}});

    send_no_content(res);
}

// ────────────────────────────────────────────────────────────────────────────
//  PUT /api/v1/admin/problems/:slug/special-judge   (v1.3.1 — SPJ 闭环)
//
//  SPEC §11 Phase 4 ☆ + §7.1 judge_type='special':
//    admin uploads the C++ source that implements the problem's
//    custom comparison. The source is stored in
//    `problem_special_judges` (V010) keyed by problem_id (PK + FK
//    ON DELETE CASCADE). judge.sh reads task.json's
//    `special_judge_source` and compiles / invokes it per case.
//
//  Wire flow:
//    1) require_admin
//    2) consume_rate_limit (admin.write)
//    3) extract_slug_from_admin_path (400 on bad shape)
//    4) parse_json_body → require_string(source) + optional language
//       - source: 1..256KB (kMaxSpjSourceLenAdmin — tighter than the
//         repo's 16MB ceiling because admin uploads are typically
//         tiny; 256KB caps compile time at g++ < 5s comfortably under
//         the 10s compile_timeout)
//       - language: defaults to "cpp" (only C++ today; the judge
//         image is g++/gcc-only)
//    5) problem_repo::find_by_slug → 404 if not live
//    6) special_judge_repo::upsert(problem_id, source, language)
//       - throws on FK violation (problem gone) → 404
//       - throws on validation → 400
//    7) audit_log_repo::record (action=problem.spj_upsert,
//       payload={source_bytes, language})
//    8) send_success 200 + {problem_id, source_bytes, language,
//       updated_at}
//
//  Why we re-fetch the row after upsert: the repo doesn't return
//  updated_at (the ON UPDATE CURRENT_TIMESTAMP write is fire-and-forget).
//  We issue a follow-up find_by_problem_id to surface the post-update
//  timestamp in the response — gives the admin UI a stable cursor to
//  detect re-saves.
//
//  Idempotency: PUT twice with the same body is two writes, not a no-op.
//  The route layer matches the standard "PUT = upsert" contract; a
//  future client that wants true idempotency can compare updated_at.
// ────────────────────────────────────────────────────────────────────────────

inline void admin_put_special_judge_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    const auto claims = require_admin(req, jwt_cfg);
    consume_rate_limit(res, req, limiter, admin_write_quota(rate_cfg));

    const auto slug_v = detail::extract_slug_from_admin_path(req, "/special-judge");
    if (!slug_v) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "slug must be 1-100 chars of [a-z0-9-], not starting "
                   "or ending with '-'",
                   {{"field", "slug"},
                    {"value", std::string(req.path)}});
        return;
    }
    const std::string slug = *slug_v;

    // Parse body.
    const auto body = parse_json_body(req, res);
    if (!body) return;  // parse_json_body already emitted 400
    const auto& j = *body;  // alias so the rest reads naturally

    const auto source_v = detail::require_string(j, res, "source");
    if (!source_v) return;
    const std::string& source = *source_v;

    // Admin-side clamp: 256 KB. The repo ceiling is 16 MB; admin UI
    // uploads want a tighter bound to keep compile time predictable.
    constexpr std::size_t kMaxSpjSourceLenAdmin = 256 * 1024;
    if (source.size() > kMaxSpjSourceLenAdmin) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "special judge source exceeds 256KB admin cap "
                   "(repo ceiling is 16MB; tighten the source or open "
                   "a follow-up to raise the admin cap)",
                   {{"field", "source"},
                    {"size",  std::to_string(source.size())},
                    {"max",   std::to_string(kMaxSpjSourceLenAdmin)}});
        return;
    }

    // language — defaults to cpp; reject anything else with 400 (the
    // repo would do the same, but surfacing here keeps the error
    // path consistent with the rest of the admin handler set).
    std::string language = litecode::kSpjLanguageCxx;
    if (j.contains("language") && !j["language"].is_null()) {
        if (!j["language"].is_string()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "field 'language' must be a string when present",
                       {{"field", "language"}});
            return;
        }
        language = j["language"].get<std::string>();
        std::string verr;
        if (!litecode::special_judge_repo::validate_language(language, &verr)) {
            send_error(res, 400, ErrorCode::INVALID_INPUT, verr,
                       {{"field", "language"},
                        {"value", detail::truncate_for_envelope(language)}});
            return;
        }
    }

    // Look up problem_id (live only; soft-deleted problems can hold
    // their SPJ in the table but the public detail already 404s them,
    // so we keep the same gate here for symmetry).
    std::optional<int> problem_id;
    try {
        const auto row = problem_repo::find_by_slug(
            pool, slug, /*include_deleted=*/false);
        if (!row.has_value()) {
            send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found",
                       {{"slug", slug}});
            return;
        }
        problem_id = row->id;
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_upsert: find_by_slug threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // Upsert the SPJ row.
    try {
        litecode::special_judge_repo::upsert(pool, *problem_id, source, language);
    } catch (const std::exception& e) {
        // FK violation lands here too; surface as 404 to match the
        // "no such problem" contract (the race window between
        // find_by_slug and upsert is small but real under load).
        const std::string what = e.what();
        if (what.find("problem_id does not exist") != std::string::npos) {
            send_error(res, 404, ErrorCode::NOT_FOUND,
                       "problem not found (FK violation on upsert)",
                       {{"slug", slug}});
            return;
        }
        LOG_ERROR("admin_spj_upsert: special_judge_repo::upsert threw",
                  {{"problem_id", std::to_string(*problem_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     what}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + what);
        return;
    }

    // Audit log.
    try {
        AuditEntry ae;
        ae.admin_id    = std::stoi(claims.user_id);
        ae.action      = audit_log_repo::kActionProblemSpjUpsert;
        ae.target_type = "problem";
        ae.target_id   = slug;
        ae.payload     = {
            {"source_bytes", source.size()},
            {"language",     language},
        };
        ae.ip = extract_client_ip(req);
        audit_log_repo::record(pool, ae);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_upsert: audit_log::record threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // Re-fetch the row so the response carries the post-update
    // updated_at (set by MySQL's ON UPDATE CURRENT_TIMESTAMP).
    std::optional<litecode::SpecialJudgeRow> after;
    try {
        after = litecode::special_judge_repo::find_by_problem_id(
            pool, *problem_id);
    } catch (const std::exception&) {
        // Best-effort; the row IS there (upsert succeeded), we just
        // can't read it back right now. Surface a 200 with the values
        // we have; clients retrying the GET will pick up updated_at
        // on the next call.
    }

    nlohmann::json payload = {
        {"problem_id",   *problem_id},
        {"slug",         slug},
        {"source_bytes", source.size()},
        {"language",     language},
        {"updated_at",   after.has_value() ? after->updated_at : ""},
    };

    LOG_INFO("admin_spj_upsert: saved",
             {{"problem_id",   std::to_string(*problem_id)},
              {"slug",         slug},
              {"source_bytes", std::to_string(source.size())},
              {"admin_id",     claims.user_id}});

    send_success(res, payload);
}

// ────────────────────────────────────────────────────────────────────────────
//  DELETE /api/v1/admin/problems/:slug/special-judge   (v1.3.1)
//
//  Idempotent: removes the problem_special_judges row if present; 204
//  either way (matches the soft-delete pattern for the problem itself).
//  judge_type='special' test cases then fall back to "no SPJ" → judge.sh
//  flips every case to WA (SPEC §7.1 contract: empty spj_source ⇒ WA
//  for every special case, not SE — admins get immediate feedback when
//  they detach an SPJ).
//
//  Wire flow:
//    1) require_admin
//    2) consume_rate_limit (admin.write)
//    3) extract_slug_from_admin_path
//    4) resolve problem_id (404 if no live row — keep semantics
//       symmetric with PUT, which also 404s on a missing slug)
//    5) special_judge_repo::remove_by_problem_id
//    6) audit_log_repo::record (action=problem.spj_remove)
//    7) send_no_content(204)
// ────────────────────────────────────────────────────────────────────────────

inline void admin_delete_special_judge_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    const auto claims = require_admin(req, jwt_cfg);
    consume_rate_limit(res, req, limiter, admin_write_quota(rate_cfg));

    const auto slug_v = detail::extract_slug_from_admin_path(req, "/special-judge");
    if (!slug_v) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "slug must be 1-100 chars of [a-z0-9-], not starting "
                   "or ending with '-'",
                   {{"field", "slug"},
                    {"value", std::string(req.path)}});
        return;
    }
    const std::string slug = *slug_v;

    std::optional<int> problem_id;
    try {
        const auto row = problem_repo::find_by_slug(
            pool, slug, /*include_deleted=*/false);
        if (!row.has_value()) {
            send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found",
                       {{"slug", slug}});
            return;
        }
        problem_id = row->id;
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_remove: find_by_slug threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    bool removed = false;
    try {
        removed = litecode::special_judge_repo::remove_by_problem_id(
            pool, *problem_id);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_remove: special_judge_repo::remove threw",
                  {{"problem_id", std::to_string(*problem_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // Audit (even when no row was removed — the admin GET-then-DELETE
    // flow is observable, and "DELETE on no row" is still a
    // security-trail event).
    try {
        AuditEntry ae;
        ae.admin_id    = std::stoi(claims.user_id);
        ae.action      = audit_log_repo::kActionProblemSpjRemove;
        ae.target_type = "problem";
        ae.target_id   = slug;
        ae.payload     = {
            {"removed",   removed},    // false ⇒ no row was attached
            {"problem_id", *problem_id},
        };
        ae.ip = extract_client_ip(req);
        audit_log_repo::record(pool, ae);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_remove: audit_log::record threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    LOG_INFO("admin_spj_remove: detached",
             {{"problem_id", std::to_string(*problem_id)},
              {"slug",       slug},
              {"had_row",    removed ? "1" : "0"},
              {"admin_id",   claims.user_id}});

    send_no_content(res);
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/admin/problems/:slug/special-judge   (v1.3.1)
//
//  Returns the SPJ row (source + language + timestamps) for the admin
//  editor to prefill the textarea on edit. Public detail intentionally
//  surfaces ONLY `has_special_judge` (boolean) — the source itself is
//  admin-only (matches the existing admin-only attachment pattern).
//
//  Wire shape (200):
//    {
//      "exists":       bool,
//      "language":     "cpp" (when exists=true; "" when false),
//      "source":       "..."  (when exists=true; "" when false),
//      "source_bytes": N     (when exists=true; 0 when false),
//      "created_at":   "YYYY-MM-DD HH:MM:SS",
//      "updated_at":   "YYYY-MM-DD HH:MM:SS"
//    }
//
//  404 when the problem slug itself doesn't exist (live).
// ────────────────────────────────────────────────────────────────────────────

inline void admin_get_special_judge_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {
    const auto claims = require_admin(req, jwt_cfg);
    consume_rate_limit(res, req, limiter, admin_write_quota(rate_cfg));

    const auto slug_v = detail::extract_slug_from_admin_path(req, "/special-judge");
    if (!slug_v) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "slug must be 1-100 chars of [a-z0-9-], not starting "
                   "or ending with '-'",
                   {{"field", "slug"},
                    {"value", std::string(req.path)}});
        return;
    }
    const std::string slug = *slug_v;

    std::optional<int> problem_id;
    try {
        const auto row = problem_repo::find_by_slug(
            pool, slug, /*include_deleted=*/false);
        if (!row.has_value()) {
            send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found",
                       {{"slug", slug}});
            return;
        }
        problem_id = row->id;
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_get: find_by_slug threw",
                  {{"slug",   slug},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    std::optional<litecode::SpecialJudgeRow> spj;
    try {
        spj = litecode::special_judge_repo::find_by_problem_id(
            pool, *problem_id);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_spj_get: find_by_problem_id threw",
                  {{"problem_id", std::to_string(*problem_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    nlohmann::json payload = {
        {"problem_id",   *problem_id},
        {"slug",         slug},
        {"exists",       spj.has_value()},
        {"language",     spj.has_value() ? spj->language : std::string("")},
        {"source",       spj.has_value() ? spj->source   : std::string("")},
        {"source_bytes", spj.has_value() ? static_cast<std::int64_t>(spj->source.size()) : static_cast<std::int64_t>(0)},
        {"created_at",   spj.has_value() ? spj->created_at : std::string("")},
        {"updated_at",   spj.has_value() ? spj->updated_at : std::string("")},
    };

    send_success(res, payload);
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Mirrors the pattern from problem_routes.h / tag_routes.h. The
//  registration function returns HttpServer& so callers can chain.
//
//  Production usage (from main.cpp — see the ODR-collision comment
//  at the top of this file; main.cpp does NOT smoke-register this
//  header, only test binaries do):
//
//    litecode::HttpServer     server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(...);
//    litecode::RateLimiter    limiter;
//    litecode::register_admin_problem_routes(
//        server, pool, limiter, cfg.rate_limit, cfg.jwt);
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter (so bucket state is per-test).
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_admin_problem_routes(
        HttpServer&            server,
        ConnectionPool&        pool,
        RateLimiter&           limiter,
        const RateLimitConfig& rate_cfg,
        const JwtConfig&       jwt_cfg) {
    // POST /api/v1/admin/problems — create (SPEC §5.2, A18)
    server.post("/api/v1/admin/problems",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_create_problem_handler(res, req, pool, limiter,
                                              rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;   // already an envelope — let server.h emit it
            } catch (const std::exception& e) {
                LOG_ERROR("admin_problem_create: handler threw",
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

    // PUT /api/v1/admin/problems/:slug — full replace (SPEC §5.2, A19)
    server.put(R"(/api/v1/admin/problems/([^/]+))",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_update_problem_handler(res, req, pool, limiter,
                                              rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_problem_update: handler threw",
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

    // DELETE /api/v1/admin/problems/:slug — soft delete (SPEC §5.2, A20)
    server.del(R"(/api/v1/admin/problems/([^/]+))",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_delete_problem_handler(res, req, pool, limiter,
                                              rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_problem_delete: handler threw",
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

    // ──── v1.3.1 — Special Judge CRUD ────────────────────────────────
    // PUT    /api/v1/admin/problems/:slug/special-judge  (admin, 30/min)
    // GET    /api/v1/admin/problems/:slug/special-judge  (admin, 30/min)
    // DELETE /api/v1/admin/problems/:slug/special-judge  (admin, 30/min)

    server.put(R"(/api/v1/admin/problems/([^/]+)/special-judge)",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_put_special_judge_handler(res, req, pool, limiter,
                                                rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_spj_put: handler threw",
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

    server.del(R"(/api/v1/admin/problems/([^/]+)/special-judge)",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_delete_special_judge_handler(res, req, pool, limiter,
                                                   rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_spj_delete: handler threw",
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

    server.get(R"(/api/v1/admin/problems/([^/]+)/special-judge)",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                admin_get_special_judge_handler(res, req, pool, limiter,
                                                rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_spj_get: handler threw",
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

} // namespace litecode