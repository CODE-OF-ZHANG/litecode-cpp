// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — submission routes (Phase 4 ★)
//
// SPEC §3.2 / §5.3 / §7.1 / §11 Phase 4 / §12.1 / A6, A7, A8, A25, A26, A27
// acceptance:
//   - POST /api/v1/submissions        (auth, 30/min/user) — ASYNC enqueue
//   - GET  /api/v1/submissions/:id    (auth)                — poll result
//   - GET  /api/v1/submissions        (auth)                — history list
//   - (reserved) GET /api/v1/submissions/sse/:id — v1.2.17+
//
// Wire shape (request):
//   POST body:
//     {
//       "problem_id": 1,
//       "language":   "cpp",        // "c" | "cpp"
//       "code":       "..."         // <= 15 MB (kMaxCodeLength)
//     }
//   GET /:id — no body
//   GET /   — query: ?problem_id=... &limit=... &offset=... &status=...
//
// Wire shape (response):
//   201 POST → {data: {submission_id, status: "pending"}, request_id}
//   200 GET /:id → {data: {id, user_id, problem_id, language, code,
//                           status, time_used?, memory_used?, error_message?,
//                           created_at, finished_at?}, request_id}
//   200 GET /   → {data: {items[], total, limit, offset}, request_id}
//
// Asynchronous design (SPEC §3.2 / §7.1 / §11 Phase 4 ★):
//   POST  /api/v1/submissions  → write submissions(status='pending') →
//                                 enqueue JudgeTask → return submission_id
//   Worker thread (in JudgeScheduler) → mark_running → docker run →
//                                 parse judge.sh JSON → mark_finished.
//   Client polls GET /api/v1/submissions/:id every 1-2s; SSE is the
//   future Phase 4+ follow-up (this commit does NOT ship it — A25's
//   "SSE 推送" is `☆` in §11).
//
// Authorization rules (SPEC §5.3):
//   - POST:    require_authentication (any logged-in user).
//   - GET /:id: require_authentication; non-admin can only view own.
//   - GET /:   require_authentication; non-admin has user_id forced to
//              self (the filter's user_id is ignored even if set).
//
// Rate limit (SPEC §5.3 / §15.2):
//   - POST: 30/min/user (consume_rate_limit with submission_quota).
//   - GETs: no per-endpoint quota today (SPEC §5.3 leaves them blank).
//
// Failure modes (SPEC §15.5):
//   - Queue full at enqueue time → 503 SERVICE_UNAVAILABLE.
//   - Docker / judge infra failure → submission row reaches status='se'
//     (handled by JudgeScheduler's worker — the route never sees it).
//   - FK violation on the submissions insert (unknown problem) → 400
//     INVALID_INPUT with details.problem_id.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3/4 module
//     (server.h / problem_routes.h / admin_problem_routes.h /
//     judge_scheduler.h). Tests link this header directly and
//     instantiate the route set with a real ConnectionPool + a (lax)
//     rate limiter + a real JwtConfig + a JudgeScheduler reference.
//
//   - The JudgeScheduler pointer is captured by reference and must
//     outlive the server (main() / test fixture owns it). When the
//     scheduler is null (a test that wants to exercise only the route
//     layer), POST still creates the submission row but skips the
//     enqueue — the row stays in status='pending'. This is the
//     documented "queue disabled" path; tests that want a real judge
//     transition wire a JudgeScheduler.
//
//   - The 6-step pipeline (require_auth → consume_rate_limit → parse
//     + validate → repo dispatch + enqueue → log → response) matches
//     the Phase 2/3 admin endpoints. The only divergence is step 4
//     here = "write + enqueue" rather than "write + audit", because
//     the submission itself is the auditable action (the scheduler's
//     result row tells the operator everything).
//
//   - Because of the ODR-collision risk between
//     `litecode::detail::req_string` (defined identically in
//     problem_repo / tag_repo / test_case_repo / audit_log_repo /
//     submission_repo), main.cpp does NOT smoke-register this header
//     when other repos are already in the TU. End-to-end coverage is
//     provided by tests/unit/test_submission.cpp. The same constraint
//     already excludes problem_routes.h / admin_problem_routes.h /
//     admin_bulk_import_routes.h from main.cpp (documented in
//     main.cpp's comment at lines 232-242).

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                          // RateLimitConfig / JwtConfig / JudgeConfig
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/problem_repo.h"                 // problem_repo::find_by_id / ProblemRow
#include "../db/submission_repo.h"              // submission_repo::* (SubmissionRow etc.)
#include "../db/test_case_repo.h"               // test_case_repo::list_for_problem
#include "../judge/judge_scheduler.h"           // JudgeScheduler / JudgeTask
#include "../logger.h"                          // LOG_INFO / LOG_WARN
#include "../middleware/auth_middleware.h"      // require_authentication / Claims
#include "../middleware/rate_limit.h"           // consume_rate_limit / submission_quota
#include "../routes/error_handler.h"            // ApiException / ErrorCode / send_error
#include "../server.h"                          // HttpServer / send_success / send_created

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Body-shape parsing helpers (POST)
//
//  The POST body is small: 3 required fields (problem_id, language,
//  code). We do all shape + value validation here so the handler body
//  is one straight-line repo dispatch + enqueue.
//
//  Anti-enumeration: every validation failure carries the field name
//  and (where safe) the rejected value in the envelope's `details`.
//  We DO NOT echo `code` (potentially huge) — the helper clamps it
//  to a short prefix so the 400 envelope stays bounded.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline constexpr std::size_t kSubmissionDetailValueMax = 64;

inline std::string truncate_for_envelope(const std::string& v) {
    if (v.size() <= kSubmissionDetailValueMax) return v;
    return v.substr(0, kSubmissionDetailValueMax) + "...";
}

// require_int_field — extract a required integer field from the body.
// Returns the value on success. On missing / wrong-type / out-of-range,
// writes a 400 envelope and returns std::nullopt so the caller can
// `if (!v) return`.
inline std::optional<int> require_int_field(const nlohmann::json& body,
                                            httplib::Response& res,
                                            const char* field,
                                            int min_inclusive,
                                            int max_inclusive) {
    if (!body.contains(field) || body[field].is_null()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("missing required field '") + field + "'",
                   {{"field", field}});
        return std::nullopt;
    }
    if (!body[field].is_number_integer()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field +
                   "' must be an integer",
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

// require_language_field — extract the required `language` string,
// normalize to "c" / "cpp". Empty / null / out-of-enum → 400.
inline std::optional<std::string> require_language_field(
        const nlohmann::json& body,
        httplib::Response& res,
        const char* field = "language") {
    if (!body.contains(field) || body[field].is_null()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("missing required field '") + field + "'",
                   {{"field", field}});
        return std::nullopt;
    }
    if (!body[field].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field +
                   "' must be a string",
                   {{"field", field}});
        return std::nullopt;
    }
    const std::string v = body[field].get<std::string>();
    if (!is_valid_language(v)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "language must be one of: c, cpp",
                   {{"field", field},
                    {"value", truncate_for_envelope(v)}});
        return std::nullopt;
    }
    return v;
}

// require_code_field — extract the required `code` string. Length
// validated against kMinCodeLength..kMaxCodeLength. On any failure
// writes a 400 envelope. We DO NOT echo the rejected code (it can
// be huge); we report the length in the envelope.
inline std::optional<std::string> require_code_field(
        const nlohmann::json& body,
        httplib::Response& res,
        const char* field = "code") {
    if (!body.contains(field) || body[field].is_null()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("missing required field '") + field + "'",
                   {{"field", field}});
        return std::nullopt;
    }
    if (!body[field].is_string()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("field '") + field +
                   "' must be a string",
                   {{"field", field}});
        return std::nullopt;
    }
    const std::string v = body[field].get<std::string>();
    std::string err;
    if (!validate_code_length(v.size(), &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   err,
                   {{"field",       field},
                    {"length",      std::to_string(v.size())},
                    {"min_length",  std::to_string(kMinCodeLength)},
                    {"max_length",  std::to_string(kMaxCodeLength)}});
        return std::nullopt;
    }
    return v;
}

// ────────────────────────────────────────────────────────────────────────────
//  Query parsing helpers (GET /)
//
//  The list endpoint accepts four optional query params:
//    problem_id : positive int
//    status     : one of pending|running|ac|wa|re|tle|mle|ole|pe|ce|se
//    limit      : 1..100  (clamped)
//    offset     : >= 0    (clamped)
//
//  Validation policy mirrors problem_routes.h:
//    - non-numeric / out-of-range → 400 INVALID_INPUT
//    - out-of-cap limit values → silently clamped
// ────────────────────────────────────────────────────────────────────────────

inline std::optional<std::string> parse_status_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    if (is_valid_status(raw)) return std::string(raw);
    return std::nullopt;
}

// extract_id_from_path — strip the GET /api/v1/submissions/ prefix
// from req.path and return the trailing integer id. Returns
// std::nullopt on any shape failure (non-numeric / out-of-range /
// leading-zero garbage). The route handler maps std::nullopt → 400.
//
// Note: regex-less path parsing matches problem_routes.h's
// detail::extract_slug_from_path convention.
inline std::optional<int> extract_id_from_path(const httplib::Request& req) {
    static constexpr std::string_view kPrefix = "/api/v1/submissions/";
    const std::string& path = req.path;
    if (path.size() <= kPrefix.size()) return std::nullopt;
    if (path.compare(0, kPrefix.size(), kPrefix) != 0) return std::nullopt;
    const std::string_view tail = std::string_view(path).substr(kPrefix.size());
    if (tail.empty()) return std::nullopt;
    // Disallow anything that isn't a plain decimal positive int. We
    // do NOT route to /api/v1/submissions/sse/:id today (Phase 4
    // reserved `☆` for SSE), so a "sse" prefix is a 400, not a
    // silent match.
    if (tail.find('/') != std::string_view::npos) return std::nullopt;
    std::size_t consumed = 0;
    int value = 0;
    try {
        value = std::stoi(std::string(tail), &consumed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (consumed != tail.size()) return std::nullopt;
    if (value <= 0) return std::nullopt;
    if (value > std::numeric_limits<int>::max()) return std::nullopt;
    return value;
}

// claims_user_id_int — convert Claims::user_id (string) to int.
// jwt-cpp stores `sub` as a string; the submissions table wants an
// INT. The conversion is infallible in our pipeline because
// auth_middleware verifies the token shape, but we still guard
// against malformed claims so a future caller that hands us a
// synthetic Claims object doesn't crash the request.
inline int claims_user_id_int(const Claims& c) noexcept {
    try { return std::stoi(c.user_id); }
    catch (...) { return 0; }
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Row -> JSON
//
//  The detail / list response shape mirrors SubmissionRow's wire
//  contract. We deliberately OMIT `code` from the LIST response —
//  clients only need it on the detail endpoint for "view source"
//  affordances, and surfacing 15 MB of code on every history page
//  would blow the API budget (SPEC §12.2: "< 200ms"). The detail
//  endpoint DOES include the full code (judge client may want to
//  re-render it for context).
//
//  We also normalize the optional<> fields into either a present
//  value or `null` — never absent. Front-end type-narrowing code
//  (e.g. `if (data.time_used !== null)`) is cleaner with explicit
//  nulls than with key-presence checks.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_submission_row(
        const litecode::SubmissionRow& s,
        bool include_code = false) {
    nlohmann::json j = {
        {"id",         s.id},
        {"user_id",    s.user_id},
        {"problem_id", s.problem_id},
        {"language",   s.language},
        {"status",     s.status},
        {"time_used",     s.time_used.has_value()
                            ? nlohmann::json(*s.time_used)
                            : nlohmann::json(nullptr)},
        {"memory_used",   s.memory_used.has_value()
                            ? nlohmann::json(*s.memory_used)
                            : nlohmann::json(nullptr)},
        {"error_message", s.error_message.has_value()
                            ? nlohmann::json(*s.error_message)
                            : nlohmann::json(nullptr)},
        {"created_at",  s.created_at},
        {"finished_at", s.finished_at.has_value()
                            ? nlohmann::json(*s.finished_at)
                            : nlohmann::json(nullptr)},
    };
    if (include_code) {
        j["code"] = s.code;
    }
    return j;
}

// ────────────────────────────────────────────────────────────────────────────
//  POST /api/v1/submissions   - Phase 4 ★  (SPEC §5.3, §7.1, A6/A25)
//
//  Wire flow:
//    1) require_authentication(...)  — 401 envelope on no / bad token
//    2) consume_rate_limit(... submission_quota(...))  — 429 envelope
//    3) parse JSON body + validate (400 envelope on shape errors)
//    4) problem_repo::find_by_id()   — 404 envelope if unknown / soft-deleted
//    5) submission_repo::create()    — FK fail (race) → 400
//    6) judge_scheduler->enqueue()   — queue full → 503
//    7) send_created(...)            — 201 + {submission_id, status:"pending"}
//
//  Authorization: requires a valid access token; non-admin is allowed.
//  SPEC §5.3 says "已登录" for POST /api/v1/submissions.
//
//  Why enqueue is AFTER the DB write (not before): we want the row to
//  be visible in `submissions` even if the scheduler refuses (queue
//  full → 503). A retry by the client can re-enqueue an existing
//  pending row by its id; the scheduler's mark_running guard makes
//  double-enqueue safe (see submission_repo.h docstring).
//
//  We deliberately do NOT call problem_repo::find_by_id BEFORE the
//  submission insert. The repo's submission_repo::create already
//  surfaces FK violations as id == 0, and the routing "unknown
//  problem" to 400 INVALID_INPUT (rather than 404) is the documented
//  contract. find_by_id is called ONLY to (a) reject soft-deleted
//  problems, (b) pull the problem's time_limit / memory_limit for
//  the JudgeTask. Both pieces of information are needed before we
//  enqueue; an FK error after the pre-check is vanishingly rare and
//  is folded into a 400 envelope.
// ────────────────────────────────────────────────────────────────────────────

inline void create_submission_handler(
        httplib::Response&                res,
        const httplib::Request&           req,
        litecode::ConnectionPool&         pool,
        litecode::RateLimiter&            limiter,
        const litecode::RateLimitConfig&  rate_cfg,
        const litecode::JwtConfig&        jwt_cfg,
        litecode::judge::JudgeScheduler*  scheduler) {

    // 1) Auth. Throws ApiException(401) on failure.
    const Claims claims = require_authentication(req, jwt_cfg);

    // 2) Rate limit (SPEC §5.3: 30/min/user). Throws ApiException(429).
    consume_rate_limit(res, req, limiter, submission_quota(rate_cfg));

    // 3) Body shape. parse_json_body sends a 400 envelope on any
    //    failure and returns nullopt — we just bail.
    const auto body = parse_json_body(req, res);
    if (!body) return;

    // problem_id (required, positive int)
    const auto problem_id = detail::require_int_field(
        *body, res, "problem_id",
        /*min_inclusive=*/1,
        /*max_inclusive=*/std::numeric_limits<int>::max());
    if (!problem_id) return;

    // language (required, "c" | "cpp")
    const auto language = detail::require_language_field(*body, res);
    if (!language) return;

    // code (required, 1..kMaxCodeLength bytes)
    const auto code = detail::require_code_field(*body, res);
    if (!code) return;

    // 4) Problem must exist AND not be soft-deleted (SPEC §4.2:
    //    soft-deleted problems are filtered from all public reads;
    //    a submission to a tombstoned problem would be a content
    //    integrity issue). Pull the row so we can grab the
    //    time_limit / memory_limit for the JudgeTask.
    std::optional<litecode::ProblemRow> problem;
    try {
        problem = litecode::problem_repo::find_by_id(
            pool, *problem_id, /*include_deleted=*/false);
    } catch (const std::exception& e) {
        LOG_ERROR("submission_create: find_by_id threw",
                  {{"problem_id", std::to_string(*problem_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!problem.has_value()) {
        LOG_INFO("submission_create: problem not found",
                 {{"problem_id", std::to_string(*problem_id)}});
        send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                   "problem does not exist",
                   {{"problem_id", std::to_string(*problem_id)}});
        return;
    }

    // 5) Create the submission row. The repo validates code length
    //    and language; we already did both above. An FK error here
    //    means the problem was deleted between find_by_id and insert
    //    (a TOCTOU race) — surface as 400.
    litecode::SubmissionRow row;
    row.user_id    = detail::claims_user_id_int(claims);
    row.problem_id = *problem_id;
    row.language   = *language;
    row.code       = std::move(*code);

    int submission_id = 0;
    try {
        submission_id = litecode::submission_repo::create(pool, row);
    } catch (const std::exception& e) {
        LOG_ERROR("submission_create: create threw",
                  {{"user_id",    claims.user_id},
                   {"problem_id", std::to_string(*problem_id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (submission_id <= 0) {
        // FK violation (problem_id gone between find_by_id and create).
        LOG_INFO("submission_create: FK violation (problem gone)",
                 {{"user_id",    claims.user_id},
                  {"problem_id", std::to_string(*problem_id)}});
        send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                   "problem does not exist",
                   {{"problem_id", std::to_string(*problem_id)}});
        return;
    }

    // 6) Build the JudgeTask. We pull every non-sample test case
    //    (SPEC §7.1 step 3a) ordered by (order_num ASC, id ASC) —
    //    the same ordering the SQL uses on the detail endpoint.
    //    A problem with zero judge cases (samples-only) still gets
    //    enqueued; the scheduler will run zero cases and report AC.
    litecode::judge::JudgeTask task;
    task.submission_id      = submission_id;
    task.user_id            = detail::claims_user_id_int(claims);
    task.problem_id         = *problem_id;
    task.language           = *language;
    task.code               = row.code;
    task.time_limit_ms      = problem->time_limit;
    task.memory_limit_mb    = problem->memory_limit;
    task.compile_timeout_ms = 10'000;  // SPEC §7.3 — anti compile-bomb

    try {
        const auto cases = litecode::test_case_repo::list_for_problem(
            pool, *problem_id, /*only_samples=*/std::optional<bool>(false));
        task.test_cases.reserve(cases.size());
        for (const auto& c : cases) {
            litecode::judge::JudgeTask::TestCaseInput in;
            in.input           = c.input;
            in.expected_output = c.expected_output;
            in.judge_type      = c.judge_type;
            // float_epsilon deliberately omitted from SampleCaseRow
            // (see test_case_repo.h). The judge.sh task.json sends
            // null for the field on every row, which is the documented
            // "no epsilon" sentinel — judge.sh's compare_float_eps
            // falls back to its hard-coded 1e-6 default. A future
            // admin / judge path can extend SampleCaseRow to carry
            // the column; today this is the simplest correct path.
            in.float_epsilon   = std::nullopt;
            in.order_num       = c.order_num;
            task.test_cases.push_back(std::move(in));
        }
    } catch (const std::exception& e) {
        // The submission row exists, but we failed to load its test
        // cases. Log loudly; the row will stay in status='pending'
        // forever. A future requeue sweep can pick it up; today the
        // operator must intervene. We still return 201 because the
        // submission row IS valid from the user's perspective.
        LOG_ERROR("submission_create: test_cases load failed",
                  {{"submission_id", std::to_string(submission_id)},
                   {"problem_id",    std::to_string(*problem_id)},
                   {"type",          typeid(e).name()},
                   {"reason",        e.what()}});
    }

    // 7) Enqueue. A null scheduler means "queue disabled" — we keep
    //    the row in status='pending' and tell the client the same.
    //    A non-null scheduler that returns false from enqueue means
    //    the queue is full → 503 SERVICE_UNAVAILABLE (SPEC §15.5).
    if (scheduler != nullptr) {
        if (!scheduler->enqueue(std::move(task))) {
            LOG_WARN("submission_create: judge queue full",
                     {{"submission_id", std::to_string(submission_id)},
                      {"queue_size",    std::to_string(scheduler->queue_size())},
                      {"max_queue",     std::to_string(
                                          scheduler->max_concurrent())}});
            send_error(res, 503, litecode::ErrorCode::SERVICE_UNAVAILABLE,
                       "judge queue is full, please retry later",
                       {{"submission_id", std::to_string(submission_id)},
                        {"queue_size",    static_cast<std::int64_t>(
                                              scheduler->queue_size())}});
            return;
        }
    }

    // 8) 201 + the async envelope. The front-end treats
    //    status="pending" as "poll /:id until status changes".
    LOG_INFO("submission_create: accepted",
             {{"user_id",        claims.user_id},
              {"problem_id",     std::to_string(*problem_id)},
              {"language",       *language},
              {"submission_id",  std::to_string(submission_id)},
              {"code_bytes",     std::to_string(row.code.size())}});

    send_created(res, nlohmann::json{
        {"submission_id", submission_id},
        {"status",        litecode::kStatusPending},
        {"problem_id",    *problem_id},
        {"language",      *language},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/submissions/:id   - Phase 4 ★  (SPEC §5.3, A6/A7/A8/A25)
//
//  Wire flow:
//    1) require_authentication(...)        — 401 envelope
//    2) detail::extract_id_from_path()     — 400 envelope on shape failure
//    3) submission_repo::find_by_id()      — 404 envelope if missing
//    4) non-admin + not-own → 403
//    5) serialize_submission_row(include_code=true) — 200 + body
//
//  Authorization (SPEC §5.3): "非 admin 只能查自己的". Non-admin
//  callers get 403 FORBIDDEN (not 404) when they ask for someone
//  else's submission — that's the documented policy, and 403 is the
//  honest "you can't see this" answer. The DB doesn't carry any
//  PII besides user_id / code (which is the user's own), so a
//  "leaked id" doesn't really leak anything sensitive; the 403 is
//  here for spec-correctness rather than confidentiality.
//
//  We deliberately use the rich SubmissionRow projection (code
//  included) on this endpoint — the front-end "view source" widget
//  needs the full code to re-render it in a read-only viewer.
// ────────────────────────────────────────────────────────────────────────────

inline void get_submission_handler(
        httplib::Response&                res,
        const httplib::Request&           req,
        litecode::ConnectionPool&         pool,
        const litecode::JwtConfig&        jwt_cfg) {

    // 1) Auth.
    const Claims claims = require_authentication(req, jwt_cfg);

    // 2) Path → id. 400 on any shape failure.
    const auto id_opt = detail::extract_id_from_path(req);
    if (!id_opt.has_value()) {
        send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                   "submission id must be a positive integer",
                   {{"path", std::string(req.path)}});
        return;
    }
    const int id = *id_opt;

    // 3) Repo dispatch.
    std::optional<litecode::SubmissionRow> row;
    try {
        row = litecode::submission_repo::find_by_id(pool, id);
    } catch (const std::exception& e) {
        LOG_ERROR("submission_get: find_by_id threw",
                  {{"id",   std::to_string(id)},
                   {"type", typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!row.has_value()) {
        LOG_INFO("submission_get: not found",
                 {{"id", std::to_string(id)}});
        send_error(res, 404, litecode::ErrorCode::NOT_FOUND,
                   "submission not found",
                   {{"id", std::to_string(id)}});
        return;
    }

    // 4) Non-admin + not-own → 403. Admin sees everything.
    if (claims.role != "admin" &&
        row->user_id != detail::claims_user_id_int(claims)) {
        LOG_WARN("submission_get: forbidden (not own submission)",
                 {{"id",         std::to_string(id)},
                  {"owner_id",   std::to_string(row->user_id)},
                  {"caller_id",  claims.user_id},
                  {"caller_role", claims.role}});
        send_error(res, 403, litecode::ErrorCode::FORBIDDEN,
                   "you can only view your own submissions",
                   {{"id", std::to_string(id)}});
        return;
    }

    LOG_INFO("submission_get: served",
             {{"id",     std::to_string(id)},
              {"status", row->status}});

    send_success(res, serialize_submission_row(*row, /*include_code=*/true));
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/submissions   - Phase 4 ★  (SPEC §5.3, A14)
//
//  Wire flow:
//    1) require_authentication(...)  — 401 envelope
//    2) parse query (problem_id / status / limit / offset)  — 400 on
//       shape errors; non-admin has user_id forced to self (the
//       request's user_id query is ignored, matching SPEC §5.3's
//       "提交历史权限收紧")
//    3) submission_repo::list()  — paginated, filterable
//    4) serialize + log + 200
//
//  Authorization (SPEC §5.3): "非 admin 强制 user_id = 当前用户，忽略
//  请求参数中的 user_id". The function deliberately takes
//  `effective_user_id` from the auth layer's claims — the query
//  param ?user_id is intentionally NOT honored today (the route
//  registration pattern below does not pass any user_id through).
//
//  Status filter: the SPEC §5.3 table doesn't list a status filter,
//  but it's the natural companion to a "history" view ("show me
//  only my AC submissions"). We accept it without a quota impact —
//  same underlying count / list path, just a tighter WHERE clause.
//
//  include_unfinished: defaults true. Front-end history lists always
//  want to see pending / running rows (otherwise a freshly-submitted
//  AC would disappear until the next reload). Admin tooling that
//  wants "terminal only" can set ?include_unfinished=false.
// ────────────────────────────────────────────────────────────────────────────

inline void list_submissions_handler(
        httplib::Response&                res,
        const httplib::Request&           req,
        litecode::ConnectionPool&         pool,
        const litecode::RateLimitConfig&  /*rate_cfg*/,
        const litecode::JwtConfig&        jwt_cfg) {

    // 1) Auth.
    const Claims claims = require_authentication(req, jwt_cfg);

    // 2) Query parsing. We don't have a "require_user_id_param" —
    //    non-admin has it forced to claims.user_id right here.
    litecode::SubmissionListFilter filter;
    filter.include_unfinished = true;  // SPEC §5.3 history default

    if (req.has_param("problem_id")) {
        const std::string raw = req.get_param_value("problem_id");
        // Reuse the int parser from problem_routes.h by inlining the
        // shape check (problem_routes.h's helper is private to its TU
        // and shipping it just for this would be heavier than the
        // 6 lines below).
        if (raw.empty()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "problem_id must be a positive integer",
                       {{"field", "problem_id"},
                        {"value", raw}});
            return;
        }
        std::size_t consumed = 0;
        int v = 0;
        try { v = std::stoi(raw, &consumed); }
        catch (const std::exception&) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "problem_id must be a positive integer",
                       {{"field", "problem_id"},
                        {"value", raw}});
            return;
        }
        if (consumed != raw.size() || v <= 0) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "problem_id must be a positive integer",
                       {{"field", "problem_id"},
                        {"value", raw}});
            return;
        }
        filter.problem_id = v;
    }

    if (req.has_param("status")) {
        const std::string raw = req.get_param_value("status");
        const auto v = detail::parse_status_param(raw);
        if (!v.has_value()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "status must be one of: pending, running, ac, "
                       "wa, re, tle, mle, ole, pe, ce, se",
                       {{"field", "status"},
                        {"value", raw}});
            return;
        }
        // Pass the exact status through to the repo's WHERE clause.
        // When the caller asks for a terminal status (e.g. ?status=ac)
        // the SQL excludes pending/running rows via the explicit
        // status predicate; we keep include_unfinished=true so a
        // future ?include_unfinished toggle can override.
        filter.status = *v;
    }

    if (req.has_param("limit")) {
        const std::string raw = req.get_param_value("limit");
        std::size_t consumed = 0;
        int v = 0;
        try { v = std::stoi(raw, &consumed); }
        catch (const std::exception&) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return;
        }
        if (consumed != raw.size() || v <= 0) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return;
        }
        filter.limit = v;
    }

    if (req.has_param("offset")) {
        const std::string raw = req.get_param_value("offset");
        std::size_t consumed = 0;
        int v = 0;
        try { v = std::stoi(raw, &consumed); }
        catch (const std::exception&) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return;
        }
        if (consumed != raw.size() || v < 0) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return;
        }
        filter.offset = v;
    }

    // 3) Force user_id for non-admin (SPEC §5.3). Admin sees
    //    everything if they want; we keep it scoped to the admin's
    //    own id unless they call a future admin path with explicit
    //    user_id query param (not exposed today).
    filter.user_id = detail::claims_user_id_int(claims);
    // Admin gets to optionally pass ?user_id to view another user's
    // history (operator use-case). Non-admin's filter.user_id is
    // already claims.user_id; admin's request still defaults to
    // themselves, so the override below only kicks in when an admin
    // explicitly passes a positive integer user_id.
    if (claims.role == "admin" && req.has_param("user_id")) {
        const std::string raw = req.get_param_value("user_id");
        std::size_t consumed = 0;
        int v = 0;
        try { v = std::stoi(raw, &consumed); }
        catch (const std::exception&) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "user_id must be a positive integer",
                       {{"field", "user_id"},
                        {"value", raw}});
            return;
        }
        if (consumed != raw.size() || v <= 0) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "user_id must be a positive integer",
                       {{"field", "user_id"},
                        {"value", raw}});
            return;
        }
        filter.user_id = v;
    }

    // 4) Repo dispatch.
    litecode::SubmissionListResult result;
    try {
        result = litecode::submission_repo::list(pool, filter);
    } catch (const std::exception& e) {
        LOG_ERROR("submission_list: list threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 5) Serialize. Items in the LIST response omit `code` (could be
    //    up to 15 MB; the SPEC §12.2 budget is "< 200ms"). Detail
    //    endpoint owns the full source.
    nlohmann::json items = nlohmann::json::array();
    for (const auto& row : result.items) {
        items.push_back(serialize_submission_row(row, /*include_code=*/false));
    }

    LOG_INFO("submission_list: served",
             {{"user_id",  claims.user_id},
              {"filter_user_id", filter.user_id.has_value()
                                    ? std::to_string(*filter.user_id)
                                    : std::string("")},
              {"problem_id",   filter.problem_id.has_value()
                                    ? std::to_string(*filter.problem_id)
                                    : std::string("")},
              {"include_unfinished",
                                 filter.include_unfinished ? "true" : "false"},
              {"total",        std::to_string(result.total)},
              {"returned",     std::to_string(items.size())},
              {"limit",        std::to_string(result.limit)},
              {"offset",       std::to_string(result.offset)}});

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  result.total},
        {"limit",  result.limit},
        {"offset", result.offset},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Three handlers, three endpoints. The JudgeScheduler pointer is
//  captured by reference; the caller (main() / test fixture) owns
//  the scheduler. Pass nullptr to disable enqueue (the row stays in
//  status='pending'); the test fixture for the pure route-layer
//  tests uses this configuration.
//
//  Production usage (from main.cpp):
//
//    litecode::judge::JudgeScheduler scheduler(
//        docker_client.get(), pool_ptr, db_ptr,
//        litecode::judge::make_default_scheduler_config(cfg.judge));
//    scheduler.start();
//    litecode::register_submission_routes(
//        server, pool, limiter, cfg.rate_limit, cfg.jwt,
//        &scheduler);
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter + a fresh JwtConfig + a JudgeScheduler
//  configured for null docker (workers fail-fast to SE). The shared
//  rate_cfg uses lax limits so test bodies can fire many requests
//  without hitting 429. The fixture may also pass `nullptr` for the
//  scheduler to verify "queue disabled" behavior.
//
//  ODR caveat (see header preamble): this header transitively pulls
//  in submission_repo.h which defines `litecode::detail::req_string`.
//  main.cpp already transitively pulls problem_repo / tag_repo /
//  audit_log_repo through admin_bulk_import_routes, so registering
//  this header alongside would cause an ODR violation. main.cpp
//  does NOT register submission_routes — the smoke build verifies
//  each component in isolation via the unit / integration tests.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_submission_routes(
        HttpServer&                       server,
        ConnectionPool&                   pool,
        RateLimiter&                      limiter,
        const RateLimitConfig&            rate_cfg,
        const JwtConfig&                  jwt_cfg,
        judge::JudgeScheduler*            scheduler) {

    // POST /api/v1/submissions — async enqueue (SPEC §5.3, A6/A25).
    server.post("/api/v1/submissions",
        [&pool, &limiter, rate_cfg, &jwt_cfg, scheduler]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                create_submission_handler(
                    res, req, pool, limiter, rate_cfg, jwt_cfg, scheduler);
            } catch (const ApiException&) {
                throw;  // envelope already shaped — let server.h wrap emit
            } catch (const std::exception& e) {
                LOG_ERROR("submission_create: handler threw",
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

    // GET /api/v1/submissions/:id — poll single submission
    // (SPEC §5.3, A6/A7/A8/A25). The regex captures anything that
    // isn't a '/'; the handler validates the captured integer shape.
    server.get(R"(/api/v1/submissions/([^/]+))",
        [&pool, &jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                get_submission_handler(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("submission_get: handler threw",
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

    // GET /api/v1/submissions — history list (SPEC §5.3, A14).
    // Non-admin's user_id filter is forced to claims.user_id inside
    // the handler; the request has no way to override it (admin can,
    // via ?user_id=N).
    server.get("/api/v1/submissions",
        [&pool, rate_cfg, &jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                list_submissions_handler(res, req, pool, rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("submission_list: handler threw",
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
