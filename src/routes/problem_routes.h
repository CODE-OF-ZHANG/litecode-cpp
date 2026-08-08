// SPDX-License-Identifier: MIT
//
// LiteCode-CPP - problem routes (Phase 3 *)
//
// SPEC §5.2 / §11 Phase 3 / §15.2 / A4, A5 acceptance:
//   - GET /api/v1/problems - public, 60/min/IP
//       paginated, difficulty / tag-id filtering, soft-deleted rows
//       hidden by default (SPEC §4.2 "前台列表自动过滤 is_deleted=FALSE")
//   - GET /api/v1/problems/:slug - public, 60/min/IP
//       problem detail: description (full Markdown body) + tags
//       (from tag_repo) + sample test cases (from test_case_repo).
//       Soft-deleted rows are NEVER returned (SPEC §4.2). The front
//       end runs the description through DOMPurify (SPEC §6.3 + A32);
//       the API delivers the raw Markdown so the front-end can
//       own the sanitization policy.
//   - GET /api/v1/tags - public, no rate limit (owned by tag_routes.h)
//   - POST /api/v1/admin/problems       (🔒 admin, 30/min/user) — owned by
//   - PUT  /api/v1/admin/problems/:slug (🔒 admin, 30/min/user) — admin_problem_routes.h
//   - DELETE /api/v1/admin/problems/:slug (🔒 admin, 30/min/user)
//   - POST /api/v1/admin/problems/import  (🔒 admin, future)
//
// Phase 3 * ships the LIST + DETAIL endpoints. The remaining routes
// (tag list + admin CRUD + bulk import) lay down the same
// `parse_*_request()` / handler / `register_*_routes()` shape that
// Phase 2 auth_routes.h established so the follow-up Phase 3 work
// can land as well-scoped additive commits.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3 module
//     (server.h / error_handler.h / problem_repo.h / auth_routes.h).
//     Tests link this header directly and instantiate the route set
//     with a real ConnectionPool + a (lax) rate limiter.
//   - The list endpoint is PUBLIC: no JWT gate, no admin gate. Soft
//     delete is therefore ALWAYS applied - the public list can never
//     surface tombstones, even if a future caller passes
//     ?include_deleted=true. That field is silently coerced to false
//     on this endpoint. The admin path under /api/v1/admin/problems
//     (future) will own the include_deleted toggle.
//   - The handler is the single funnel for GET /api/v1/problems. It:
//       1) consume_rate_limit() - 60/min/IP; 429 envelope on deny
//       2) parse_list_query()   - extract & validate query params,
//                                returning a ProblemListFilter ready
//                                for problem_repo::list()
//       3) problem_repo::list() - paginated, filterable; throws on
//                                driver failure -> 500 envelope
//       4) send_success()       - 200 + {items, total, limit, offset}
//   - The list response deliberately OMITS `description` and `tags`:
//       * `description` is the full Markdown body (up to MEDIUMTEXT,
//         i.e. ~16MB) and would blow the page size + 200ms budget.
//         The detail endpoint owns description.
//       * `tags` would be a per-row JOIN against problem_tags for
//         the whole page. Phase 3 keeps the list endpoint on a flat
//         projection (SPEC §5.2 "题目列表页，支持按难度/标签筛选" -
//         filtering yes, surfacing no). The detail endpoint surfaces
//         tags via tag_repo::list_tags_for_problem() and samples
//         via test_case_repo::list_samples_for_problem().
//   - All validation messages funnel into the SPEC §5.7 unified
//     envelope via send_error(). The handler itself never writes a
//     raw body.
//
// Usage (production, from main.cpp):
//
//   litecode::HttpServer     server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter    limiter;
//   litecode::register_problem_routes(server, pool, limiter, cfg.rate_limit);
//   server.listen_blocking();
//
// Usage (test, from gtest):
//
//   litecode::HttpServer     server(dev_server(), dev_cors());
//   litecode::ConnectionPool pool(test_db_config());
//   litecode::RateLimiter    limiter;
//   litecode::register_problem_routes(
//       server, pool, limiter, lax_rate_limit());
//   auto h = start_server(&server);
//   auto r = h.client->Get("/api/v1/problems?difficulty=easy&limit=10");

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                          // RateLimitConfig
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/problem_repo.h"                 // problem_repo::list / find_by_slug / ProblemListFilter / ProblemListResult
#include "../db/special_judge_repo.h"           // special_judge_repo::exists_for_problem (v1.3.1 has_special_judge 透出)
#include "../db/tag_repo.h"                     // tag_repo::list_tags_for_problem / TagRow
#include "../db/test_case_repo.h"               // test_case_repo::list_samples_for_problem / SampleCaseRow
#include "../logger.h"                          // LOG_INFO / LOG_WARN
#include "../middleware/rate_limit.h"           // consume_rate_limit / problems_public_quota
#include "../server.h"                          // HttpServer / send_error / send_success / ErrorCode
#include "../utils/security.h"                  // security::validate_path_component_len / has_* (Phase 6 ★ v1.2.45)

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Query-string parsing helpers
//
//  cpp-httplib exposes req.get_param_value("difficulty") which returns
//  the URL-decoded value (or "" when absent). The list endpoint has
//  four query params:
//
//    difficulty  : optional, one of "easy" / "medium" / "hard"
//    tag_id      : optional, positive integer (1..INT_MAX)
//    limit       : optional, 1..100  (out-of-range -> clamp, not 400)
//    offset      : optional, >= 0    (out-of-range -> clamp, not 400)
//
//  Validation policy:
//    - difficulty: an UNKNOWN value is a 400 (the caller almost
//      certainly fat-fingered; silent coercion would mask the bug).
//      An empty / absent value is fine (no filter).
//    - tag_id: a NON-NUMERIC value is a 400. A zero or negative value
//      is a 400 (the FK target must be > 0). An empty / absent value
//      is fine.
//    - limit / offset: out-of-range values are CLAMPED, not 400. The
//      repo does the same clamp internally (clamp_list_filter); doing
//      it here too means an explicit problem-level clamp message can
//      land in the 400 envelope later without changing the contract.
//
//  The four helpers below are intentionally free functions in the
//  `detail` namespace so they don't pollute `litecode::*`. None of
//  them touch the response - they only validate-and-extract; the
//  caller is responsible for writing the 400 envelope when std::nullopt
//  is returned.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// parse_int_param - pull a decimal integer out of a query string value.
// Returns std::nullopt on any parse failure (empty, non-numeric, or
// out of the requested inclusive range). We deliberately don't accept
// negative numbers even for the offset field - `offset < 0` is a 400
// here, clamped to 0 at the call site if the caller wants lenient
// behavior. The route handler maps std::nullopt -> 400 INVALID_INPUT.
inline std::optional<int> parse_int_param(std::string_view raw,
                                          int min_inclusive,
                                          int max_inclusive) {
    if (raw.empty()) return std::nullopt;
    // Reject leading/trailing whitespace - keeps the parser strict.
    if (std::isspace(static_cast<unsigned char>(raw.front())) ||
        std::isspace(static_cast<unsigned char>(raw.back()))) {
        return std::nullopt;
    }
    // strtol-like scan without the locale-sensitive parts. We don't
    // accept '+' signs or leading zeros-with-digits because they make
    // for surprisingly common DoS vectors on public endpoints ("?id=0"
    // vs "?id=+0" vs "?id=00" all equal zero anyway; rejecting them
    // keeps the wire contract unambiguous).
    std::size_t consumed = 0;
    int value = 0;
    try {
        value = std::stoi(std::string(raw), &consumed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (consumed != raw.size()) return std::nullopt;   // trailing junk
    if (value < min_inclusive || value > max_inclusive) return std::nullopt;
    return value;
}

// parse_difficulty_param - validate the difficulty query param against
// the SPEC §4.2 ENUM. Empty => nullopt (no filter, the caller can keep
// it std::nullopt in the filter). Any non-empty value not in the
// allowed set => nullopt with a parse error so the handler can emit a
// 400 envelope with a clear message.
inline std::optional<std::string> parse_difficulty_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    if (raw == "easy" || raw == "medium" || raw == "hard") {
        return std::string(raw);
    }
    return std::nullopt;
}

// parse_bool_param - "true"/"1"/"yes" => true, "false"/"0"/"no" => false,
// anything else => nullopt (handler maps to 400). Case-insensitive.
// Public endpoints ignore this for now (include_deleted is hard-coerced
// to false on the public list), but the helper is here so the future
// admin path can reuse it without copy-paste.
inline std::optional<bool> parse_bool_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    // Lowercase the comparison string without allocating.
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
    if (ci_eq(raw, "true") || ci_eq(raw, "1") || ci_eq(raw, "yes")) return true;
    if (ci_eq(raw, "false") || ci_eq(raw, "0") || ci_eq(raw, "no")) return false;
    return std::nullopt;
}

// clamp_pagination - clamp the (limit, offset) pair to the SPEC +
// repo's safe ranges. Used by the handler so a future caller-facing
// clamp message can land in the 400 envelope without changing the
// repo contract.
inline void clamp_pagination(int& limit, int& offset) {
    if (limit <= 0) limit = litecode::kDefaultListLimit;
    if (limit > litecode::kMaxListLimit) limit = litecode::kMaxListLimit;
    if (offset < 0) offset = 0;
}

// parse_list_query - translate req.{get_param_value, has_param} into
// a ProblemListFilter ready to hand to problem_repo::list(). Returns
// false and writes a 400 envelope on any validation failure. On
// success, the out-parameter `out` is populated and the function
// returns true.
//
// `pool` is unused today (reserved for a future "expand tag by slug"
// pre-check) - keeping the parameter so the signature mirrors the
// other parse_* helpers in Phase 2 and a future commit that needs
// to validate tag_id by looking up tag_repo::find_by_id() doesn't
// have to retune every test call site.
inline bool parse_list_query(const httplib::Request&      req,
                             httplib::Response&          res,
                             litecode::ConnectionPool&   /*pool*/,
                             litecode::ProblemListFilter& out) {
    out = litecode::ProblemListFilter{};
    // Public list: always hide soft-deleted rows (SPEC §5.2 + §4.2).
    // Even if the caller passes ?include_deleted=true, we silently
    // coerce to false here - the admin path under
    // /api/v1/admin/problems owns the toggle.
    out.include_deleted = false;

    // difficulty (optional). Empty / absent => no filter. Unknown
    // value => 400 INVALID_INPUT.
    if (req.has_param("difficulty")) {
        const std::string raw = req.get_param_value("difficulty");
        const auto v = parse_difficulty_param(raw);
        if (!v.has_value()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "difficulty must be one of: easy, medium, hard",
                       {{"field", "difficulty"},
                        {"value", raw}});
            return false;
        }
        out.difficulty = *v;
    }

    // tag_id (optional). Absent => no filter. Non-numeric / out-of-range
    // => 400 INVALID_INPUT. We accept any positive int that fits in
    // 32 bits - the actual existence check (if any) is the repo's
    // job; this layer just guards shape.
    if (req.has_param("tag_id")) {
        const std::string raw = req.get_param_value("tag_id");
        const auto v = parse_int_param(raw, /*min_inclusive=*/1,
                                          /*max_inclusive=*/
                                          std::numeric_limits<int>::max());
        if (!v.has_value()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "tag_id must be a positive integer",
                       {{"field", "tag_id"},
                        {"value", raw}});
            return false;
        }
        out.tag_id = *v;
    }

    // limit (optional). Absent => repo default. Non-numeric /
    // out-of-INT_MAX / <= 0 => 400. Values > kMaxListLimit are
    // accepted here and clamped to kMaxListLimit by
    // clamp_pagination() below (matches problem_repo::clamp_list_filter's
    // lenient policy). Rejecting 500 with a 400 would force every
    // client to know the cap; clamping is friendlier and the cap
    // is a server-side safety, not a contract.
    if (req.has_param("limit")) {
        const std::string raw = req.get_param_value("limit");
        const auto v = parse_int_param(raw, /*min_inclusive=*/1,
                                          /*max_inclusive=*/
                                          std::numeric_limits<int>::max());
        if (!v.has_value()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
        out.limit = *v;
    }

    // offset (optional). Absent => 0. Invalid => 400.
    if (req.has_param("offset")) {
        const std::string raw = req.get_param_value("offset");
        const auto v = parse_int_param(raw, /*min_inclusive=*/0,
                                          /*max_inclusive=*/
                                          std::numeric_limits<int>::max());
        if (!v.has_value()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
        out.offset = *v;
    }

    // include_deleted (optional boolean). Public list ALWAYS forces
    // false regardless of input (see comment above). We still
    // validate shape so a typo (?include_deleted=truthy) surfaces
    // a clean 400 instead of being silently swallowed.
    if (req.has_param("include_deleted")) {
        const std::string raw = req.get_param_value("include_deleted");
        const auto v = parse_bool_param(raw);
        if (!v.has_value()) {
            send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                       "include_deleted must be one of: true, false, 1, 0, yes, no",
                       {{"field", "include_deleted"},
                        {"value", raw}});
            return false;
        }
        // Coerce to false on this public endpoint. Admin paths
        // (future) will read the field through.
        out.include_deleted = false;
    }

    // Defense-in-depth clamp on (limit, offset) - the repo's
    // clamp_list_filter does this too, but doing it here keeps the
    // SQL bind values sane even if a future caller skips the repo.
    clamp_pagination(out.limit, out.offset);

    return true;
}

// parse_slug_param - validate the slug that comes out of the URL
// path for GET /api/v1/problems/:slug. Returns the slug (unchanged
// — the route layer passes it straight to problem_repo::find_by_slug)
// on success, or std::nullopt on any shape failure.
//
// The validation rules mirror problem_repo::validate_slug():
//   - 1..100 chars (kMinSlugLength..kMaxSlugLength)
//   - lowercase ASCII letters / digits / '-' only
//   - must not start or end with '-'
//
// We deliberately don't accept +'d URL-decoded bytes that the
// slug would never legitimately have come from, even though
// cpp-httplib's `get_param_value` would happily decode them. The
// route layer strips the path prefix and calls this helper with
// the remaining bytes; an out-of-shape value is a 400 INVALID_INPUT
// envelope, never a 500.
//
// Phase 6 ★ v1.2.45 hardening (SPEC §15.2):
//   - Reject the slug early when it contains path-traversal sequences
//     ("..", "%2e%2e", "/"). The cpp-httplib route registration uses a
//     regex pattern that should keep these out, but a defense-in-depth
//     check here costs ~20 ns and stops a future regex regression from
//     becoming a security hole.
//   - Reject control chars + HTML special chars + U+2028/2029 before
//     passing through. The slug surfaces in URLs and is rendered on
//     profile pages; better to 400 up-front than let the front-end
//     decide whether to render or escape.
inline std::optional<std::string> parse_slug_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    if (!litecode::security::validate_path_component_len(raw)) return std::nullopt;
    // Path-traversal trip-wires. Even though the route pattern is
    // "/api/v1/problems/([^/]+)" (no '/'), we still guard against a
    // URL-decoded "%2F" slip (cpp-httplib decodes before matching).
    if (raw.find("..") != std::string_view::npos) return std::nullopt;
    if (litecode::security::has_control_chars(raw)) return std::nullopt;
    if (litecode::security::has_html_special_chars(raw)) return std::nullopt;
    if (litecode::security::has_json_special_chars(raw)) return std::nullopt;
    std::string err;
    const std::string s(raw);  // problem_repo::validate_slug takes string_view
    if (!litecode::validate_slug(s, &err)) {
        return std::nullopt;
    }
    return s;
}

// extract_slug_from_path - strip the GET /api/v1/problems/ prefix
// from req.path and hand the remainder to parse_slug_param. The
// route registration uses a regex pattern (R"(/api/v1/problems/
// ([^/]+))") so the only thing this function has to do is the
// same prefix-strip the regex would otherwise give us via
// httplib::Server's `Matches` argument. We deliberately don't
// pull Matches out because that would force the route to drop
// down to a 3-arg handler signature; path-string parsing is
// cheap, explicit, and free of regex-capture surprises.
//
// Returns std::nullopt when:
//   - the path doesn't carry the expected prefix (shouldn't
//     happen because the regex pattern guarantees it, but we
//     defend in depth)
//   - the slug portion is empty
//   - the slug portion fails problem_repo::validate_slug
inline std::optional<std::string> extract_slug_from_path(
        const httplib::Request& req) {
    static constexpr std::string_view kPrefix = "/api/v1/problems/";
    const std::string& path = req.path;
    if (path.size() <= kPrefix.size()) return std::nullopt;
    if (path.compare(0, kPrefix.size(), kPrefix) != 0) return std::nullopt;
    return parse_slug_param(std::string_view(path).substr(kPrefix.size()));
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Row -> JSON
//
//  We deliberately omit the heavy fields:
//    - description : the full Markdown body (MEDIUMTEXT, up to 16MB);
//                    too large for a list card. Detail endpoint owns it.
//    - is_deleted  : hidden by definition on the public list. A `true`
//                    value here would mean we leaked a tombstone.
//  We DO include the maintenance counters (accepted_count /
//  submission_count) because the SPEC §4.2 calls them out as list-card
//  signals ("通过人数" / "总提交数") and they're cheap INTs.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_problem_row(const litecode::ProblemRow& p) {
    return nlohmann::json{
        {"id",               p.id},
        {"slug",             p.slug},
        {"title",            p.title},
        {"difficulty",       p.difficulty},
        {"time_limit",       p.time_limit},
        {"memory_limit",     p.memory_limit},
        {"accepted_count",   p.accepted_count},
        {"submission_count", p.submission_count},
        {"created_at",       p.created_at},
        {"updated_at",       p.updated_at},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Detail row -> JSON
//
//  Adds the heavy / multi-row fields the list endpoint deliberately
//  omits:
//    - description : the full Markdown body (SPEC §4.2 MEDIUMTEXT).
//                    Delivered RAW — the front-end runs it through
//                    DOMPurify (SPEC §6.3 + A32). Centralizing
//                    sanitization in the front-end keeps the API
//                    contract stable when the policy evolves.
//    - tags        : list of {id, name} from tag_repo. Ordered by
//                    tag name ASC (matches tag_repo's own ordering,
//                    so the front-end doesn't have to re-sort).
//    - samples     : list of {input, output, judge_type} from
//                    test_case_repo. Ordered by (order_num ASC, id
//                    ASC) — same as the DB. judge_type IS surfaced
//                    (v1.3.1 — was deliberately hidden before); the
//                    admin editor reads it back from /problems/:slug
//                    so the `judge_type` select doesn't fall back to
//                    "exact" on every edit.
//
//  We still OMIT is_deleted (same reason as the list endpoint:
//  a `true` value would mean we leaked a tombstone). We DO include
//  the maintenance counters (accepted_count / submission_count)
//  because the front-end uses them to render "AC率" / "提交数"
//  on the detail page header.
//
//  v1.3.1 also adds `has_special_judge` (bool) — defaults to false.
//  The public GET /problems/:slug handler looks up the SPJ row via
//  special_judge_repo::exists_for_problem and threads the result in.
//  Admin POST/PUT callers (admin_problem_routes.h) accept the default
//  false; admin sees the truth via the dedicated GET endpoint.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_sample(const litecode::SampleCaseRow& s) {
    return nlohmann::json{
        {"input",      s.input},
        {"output",     s.expected_output},
        {"judge_type", s.judge_type},
    };
}

inline nlohmann::json serialize_problem_detail(
        const litecode::ProblemRow& p,
        const std::vector<litecode::TagRow>& tags,
        const std::vector<litecode::SampleCaseRow>& samples,
        bool has_special_judge = false) {
    nlohmann::json tags_j = nlohmann::json::array();
    for (const auto& t : tags) {
        tags_j.push_back(nlohmann::json{
            {"id",   t.id},
            {"name", t.name},
        });
    }
    nlohmann::json samples_j = nlohmann::json::array();
    for (const auto& s : samples) {
        samples_j.push_back(serialize_sample(s));
    }
    return nlohmann::json{
        {"id",                 p.id},
        {"slug",               p.slug},
        {"title",              p.title},
        {"difficulty",         p.difficulty},
        {"description",        p.description},
        {"template",           p.template_},     // v1.3.2: per-problem code template
        {"time_limit",         p.time_limit},
        {"memory_limit",       p.memory_limit},
        {"accepted_count",     p.accepted_count},
        {"submission_count",   p.submission_count},
        {"tags",               std::move(tags_j)},
        {"samples",            std::move(samples_j)},
        {"has_special_judge",  has_special_judge},
        {"created_at",         p.created_at},
        {"updated_at",         p.updated_at},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/problems   - Phase 3 *  (SPEC §5.2, §11 Phase 3, A4)
//
//  Wire flow:
//    1) consume_rate_limit()    - 60/min/IP; 429 envelope on deny
//    2) detail::parse_list_query() - 400 on bad query params
//    3) problem_repo::list()    - paginated, filterable; throws => 500
//    4) send_success()          - 200 + {items, total, limit, offset}
//
//  Authorization: NONE. SPEC §5.2 row says "公开". The route handler
//  does not invoke require_authentication; a missing / bogus
//  Authorization header (if present) is ignored.
//
//  Soft delete: ALWAYS applied. SPEC §4.2: "前台列表自动过滤
//  is_deleted=FALSE". The detail/admin endpoints are where tombstone
//  visibility is gated.
//
//  What we DON'T do here:
//    - We don't include description (too large for a list page;
//      detail endpoint owns it).
//    - We don't include tags (would require a JOIN against
//      problem_tags; the filter path uses EXISTS, the projection
//      path stays flat - see problem_repo.h docstring). Detail
//      endpoint surfaces tags via tag_repo::list_tags_for_problem().
//    - We don't write to audit_logs. Public reads are not auditable
//      actions; the structured log line below is enough for ops.
// ────────────────────────────────────────────────────────────────────────────

inline void list_problems_handler(httplib::Response&         res,
                                  const httplib::Request&    req,
                                  litecode::ConnectionPool&  pool,
                                  litecode::RateLimiter&     limiter,
                                  const litecode::RateLimitConfig& rate_cfg) {
    // 1) Rate limit FIRST so a flood of malformed query strings
    //    doesn't blow past the validation work. consume_rate_limit
    //    throws ApiException(429, RATE_LIMITED) on deny; server.h's
    //    per-request wrap turns it into the unified envelope.
    consume_rate_limit(res, req, limiter, problems_public_quota(rate_cfg));

    // 2) Parse + validate the query string into a ProblemListFilter.
    //    On failure, a 400 envelope is already on the wire and we
    //    just bail.
    litecode::ProblemListFilter filter;
    if (!detail::parse_list_query(req, res, pool, filter)) {
        return;                                     // 400 already sent
    }

    // 3) Repo dispatch. problem_repo::list() throws on driver error;
    //    the lambda's catch (std::exception&) below folds it into a
    //    500 envelope.
    litecode::ProblemListResult result;
    try {
        result = litecode::problem_repo::list(pool, filter);
    } catch (const std::exception& e) {
        LOG_ERROR("problem_list: list threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 4) Serialize. nlohmann::json lets us build the items array
    //    inline; serialize_problem_row keeps the per-row shape in
    //    one place so detail / admin paths can reuse it without
    //    drift.
    nlohmann::json items = nlohmann::json::array();
    for (const auto& row : result.items) {
        items.push_back(serialize_problem_row(row));
    }

    LOG_INFO("problem_list: served",
             {{"total",          std::to_string(result.total)},
              {"returned",       std::to_string(items.size())},
              {"limit",          std::to_string(result.limit)},
              {"offset",         std::to_string(result.offset)},
              {"difficulty",     filter.difficulty
                                  ? std::string(*filter.difficulty)
                                  : std::string("")},
              {"tag_id",         filter.tag_id
                                  ? std::to_string(*filter.tag_id)
                                  : std::string("")}});

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  result.total},
        {"limit",  result.limit},
        {"offset", result.offset},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/problems/:slug   - Phase 3 *  (SPEC §5.2, §11 Phase 3, A5)
//
//  Wire flow:
//    1) consume_rate_limit()           - 60/min/IP; 429 envelope on deny
//    2) detail::extract_slug_from_path - 400 envelope on bad slug shape
//    3) problem_repo::find_by_slug()   - include_deleted=false; throws
//                                        or returns nullopt (→ 404)
//    4) tag_repo::list_tags_for_problem()       - empty vector ⇒ "tags":[]
//    5) test_case_repo::list_samples_for_problem() - same
//    6) serialize_problem_detail()     - 200 + full body
//
//  Authorization: NONE. SPEC §5.2 row says "公开". The route
//  handler does not invoke require_authentication; a missing /
//  bogus Authorization header (if present) is ignored.
//
//  Soft delete: ALWAYS applied. SPEC §4.2: "前台列表自动过滤
//  is_deleted=FALSE". The detail endpoint cannot surface a
//  tombstone; admin paths under /api/v1/admin/problems/:slug
//  own the include_deleted toggle (future).
//
//  What we DON'T do here:
//    - We don't write to audit_logs. Public reads are not
//      auditable actions; the structured log line below is
//      enough for ops.
//    - We don't sanitize `description` here. SPEC §6.3 + A32
//      require the front-end to run Markdown through DOMPurify
//      before injecting into the page; the API delivers raw
//      Markdown so the policy stays in one place (the browser).
//    - We don't include judge_type on the samples. The
//      front-end renders samples as text and never compares;
//      judge_type would be a UI hint at best, and the column
//      is non-trivial to render correctly ("float_eps" needs
//      a number-input pair). Adding it is a pure additive
//      change later.
// ────────────────────────────────────────────────────────────────────────────

inline void get_problem_detail_handler(httplib::Response&         res,
                                       const httplib::Request&    req,
                                       litecode::ConnectionPool&  pool,
                                       litecode::RateLimiter&     limiter,
                                       const litecode::RateLimitConfig& rate_cfg) {
    // 1) Rate limit FIRST so a flood of malformed slugs doesn't
    //    blow past the validation work. consume_rate_limit throws
    //    ApiException(429, RATE_LIMITED) on deny; server.h's
    //    per-request wrap turns it into the unified envelope.
    consume_rate_limit(res, req, limiter, problems_public_quota(rate_cfg));

    // 2) Pull + validate the slug from the path. On any shape
    //    failure the helper returns std::nullopt and we emit a
    //    400 envelope. We DON'T pre-validate via a HEAD or
    //    existence check — problem_repo::find_by_slug does the
    //    authoritative lookup below; this layer's job is just
    //    "is this string even shaped like a slug".
    const auto slug = detail::extract_slug_from_path(req);
    if (!slug.has_value()) {
        send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
                   "slug must be 1-100 chars of [a-z0-9-], not starting "
                   "or ending with '-'",
                   {{"field", "slug"},
                    {"value", std::string(req.path)}});
        return;
    }
    const std::string& slug_str = *slug;

    // 3) Repo dispatch. find_by_slug throws on driver error and
    //    returns std::nullopt when no live row matches. We do NOT
    //    auto-create missing rows (this is a public read path).
    std::optional<litecode::ProblemRow> row;
    try {
        row = litecode::problem_repo::find_by_slug(
            pool, slug_str, /*include_deleted=*/false);
    } catch (const std::exception& e) {
        LOG_ERROR("problem_detail: find_by_slug threw",
                  {{"slug",   slug_str},
                   {"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }
    if (!row.has_value()) {
        // 404 envelope. We use a generic "not found" message — the
        // caller should not be able to distinguish "no such slug"
        // from "slug exists but is soft-deleted" via the response
        // shape (anti-enumeration: a deleted problem's URL is not
        // a probe oracle). The structured log below does record
        // the slug, so ops can still trace abuse.
        LOG_INFO("problem_detail: not found",
                 {{"slug", slug_str}});
        send_error(res, 404, litecode::ErrorCode::NOT_FOUND,
                   "problem not found",
                   {{"slug", slug_str}});
        return;
    }

    // 4) Tags. list_tags_for_problem is bounded (a problem has at
    //    most a handful of tags in practice; even pathological
    //    hand-tagged imports are < 20). The empty-vector case
    //    maps cleanly to "tags": [] in the response.
    std::vector<litecode::TagRow> tags;
    try {
        tags = litecode::tag_repo::list_tags_for_problem(pool, row->id);
    } catch (const std::exception& e) {
        LOG_ERROR("problem_detail: list_tags_for_problem threw",
                  {{"problem_id", std::to_string(row->id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 5) Samples. Same shape as tags — bounded, returns empty
    //    vector when the problem has no sample test cases. The
    //    public detail endpoint NEVER surfaces non-sample rows
    //    (SPEC §4.3: "是否为示例用例（展示给用户）"); the judge
    //    flow loads the full set via test_case_repo::list_for_problem.
    std::vector<litecode::SampleCaseRow> samples;
    try {
        samples = litecode::test_case_repo::list_samples_for_problem(
            pool, row->id);
    } catch (const std::exception& e) {
        LOG_ERROR("problem_detail: list_samples_for_problem threw",
                  {{"problem_id", std::to_string(row->id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 6) Special-judge probe (v1.3.1). One extra SELECT against
    //    problem_special_judges — boolean EXISTS, doesn't pull
    //    the source bytes (that stays admin-only via the dedicated
    //    GET endpoint). Failure surfaces a 500 like any other repo
    //    probe in this handler — the SPJ table is a side table that
    //    shouldn't block the detail page when down.
    bool has_special_judge = false;
    try {
        has_special_judge =
            litecode::special_judge_repo::exists_for_problem(pool, row->id);
    } catch (const std::exception& e) {
        LOG_ERROR("problem_detail: exists_for_problem threw",
                  {{"problem_id", std::to_string(row->id)},
                   {"type",       typeid(e).name()},
                   {"reason",     e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 7) Serialize + log + respond. nlohmann::json lets us build
    //    the response in one place; serialize_problem_detail is
    //    the single owner of the field shape.
    LOG_INFO("problem_detail: served",
             {{"slug",       slug_str},
              {"problem_id", std::to_string(row->id)},
              {"tags",       std::to_string(tags.size())},
              {"samples",    std::to_string(samples.size())},
              {"has_special_judge", has_special_judge ? "1" : "0"}});

    send_success(res, serialize_problem_detail(*row, tags, samples, has_special_judge));
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/problems/:slug/navigation — Phase 7 ★
//  返回当前题目的上一题和下一题（按 created_at DESC, id DESC 排序）
// ────────────────────────────────────────────────────────────────────────────

inline void handle_problem_navigation(
    httplib::Response&        res,
    const httplib::Request&   req,
    ConnectionPool&           pool,
    RateLimiter&              limiter,
    const RateLimitConfig&    rate_cfg) {

    consume_rate_limit(res, req, limiter, problems_public_quota(rate_cfg));

    // 获取 slug（直接从路径提取，因为 navigation 路由的 regex 已经是 /api/v1/problems/:slug/navigation）
    std::string slug;
    {
        const auto& path = req.path;
        const std::string prefix = "/api/v1/problems/";
        const std::string suffix = "/navigation";
        if (path.size() <= prefix.size() + suffix.size()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid path");
            return;
        }
        slug = path.substr(prefix.size());
        if (slug.size() >= suffix.size() && slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0) {
            slug = slug.substr(0, slug.size() - suffix.size());
        }
    }
    if (slug.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid slug");
        return;
    }

    // 查找当前题目
    auto current = litecode::problem_repo::find_by_slug(pool, slug, false);
    if (!current) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found");
        return;
    }

    // 上一题：created_at < current.created_at OR (created_at == current.created_at AND id < current.id)
    // 先获取当前题目的排序位置
    // 用 created_at DESC, id DESC 来找 prev 和 next
    // 注:ProblemListFilter 是 aggregate struct,所有字段都有默认值,
    // 这里只覆写 limit,其余(difficulty/tag_id/include_deleted)留空。
    litecode::ProblemListFilter prev_filter;
    prev_filter.limit  = 1000;
    prev_filter.offset = 0;
    auto allPrev = litecode::problem_repo::list(pool, prev_filter);

    // 找当前题目在列表中的位置
    int currentIdx = -1;
    for (int i = 0; i < (int)allPrev.items.size(); i++) {
        if (allPrev.items[i].id == current->id) {
            currentIdx = i;
            break;
        }
    }

    std::optional<litecode::ProblemRow> prevProblem;
    std::optional<litecode::ProblemRow> nextProblem;

    if (currentIdx > 0) {
        prevProblem = allPrev.items[currentIdx - 1];
    }
    if (currentIdx >= 0 && currentIdx < (int)allPrev.items.size() - 1) {
        nextProblem = allPrev.items[currentIdx + 1];
    }

    nlohmann::json j;
    if (prevProblem) {
        j["prev"] = nlohmann::json{
            {"slug",  prevProblem->slug},
            {"title", prevProblem->title},
        };
    } else {
        j["prev"] = nullptr;
    }
    if (nextProblem) {
        j["next"] = nlohmann::json{
            {"slug",  nextProblem->slug},
            {"title", nextProblem->title},
        };
    } else {
        j["next"] = nullptr;
    }

    send_success(res, std::move(j));
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Returns HttpServer& so callers can chain. Phase 3 * ships the
//  public list + detail endpoints; the admin CRUD routes live in
//  src/routes/admin_problem_routes.h and are registered by a
//  separate call to register_admin_problem_routes(). The bulk
//  import endpoint remains a 501 placeholder pending Phase 3's
//  bulk-import commit.
//
//  Production usage (from main.cpp):
//
//    litecode::HttpServer     server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//    litecode::RateLimiter    limiter;
//    litecode::register_problem_routes(
//        server, pool, limiter, cfg.rate_limit);
//    litecode::register_admin_problem_routes(
//        server, pool, limiter, cfg.rate_limit, cfg.jwt);
//    server.listen_blocking();
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter (so bucket state is per-test). The shared
//  rate_cfg uses lax limits so test bodies can fire many requests
//  without hitting 429.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_problem_routes(HttpServer&              server,
                                          ConnectionPool&          pool,
                                          RateLimiter&             limiter,
                                          const RateLimitConfig&   rate_cfg) {
    // GET /api/v1/problems - public list, 60/min/IP (SPEC §5.2)
    //
    // The lambda captures pool / limiter BY REFERENCE (they outlive
    // the server, owned by main() / the test fixture) and rate_cfg BY
    // VALUE (defends against a temporary RateLimitConfig going out
    // of scope - same defensive pattern as auth_routes.h).
    server.get("/api/v1/problems",
        [&pool, &limiter, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                list_problems_handler(res, req, pool, limiter, rate_cfg);
            } catch (const ApiException&) {
                // Already an envelope - let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("problem_list: handler threw",
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

    // GET /api/v1/problems/:slug/navigation - Phase 7 ★
    // 必须在 /problems/:slug 路由之前注册，否则 /problems/xxx/navigation 会被 slug 路由捕获
    server.get(R"(/api/v1/problems/([^/]+)/navigation)",
        [&pool, &limiter, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                handle_problem_navigation(res, req, pool, limiter, rate_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("problem_navigation: handler threw",
                          {{"type", typeid(e).name()}, {"reason", e.what()}});
                if (res.body.empty()) {
                    send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                               std::string("internal error: ") + e.what());
                } else {
                    throw;
                }
            }
        });

    // GET /api/v1/problems/:slug - public detail, 60/min/IP
    // (SPEC §5.2, A5). The regex pattern captures anything that
    // isn't a '/' so multi-segment slugs (none allowed by
    // problem_repo::validate_slug, but defended in depth) don't
    // match. The handler itself validates the shape via
    // detail::extract_slug_from_path and emits a 400 envelope
    // on any failure.
    server.get(R"(/api/v1/problems/([^/]+))",
        [&pool, &limiter, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                get_problem_detail_handler(res, req, pool, limiter, rate_cfg);
            } catch (const ApiException&) {
                // Already an envelope - let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("problem_detail: handler threw",
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

// GET /api/v1/tags is owned by src/routes/tag_routes.h
// (Phase 3 *) — it lives in its own header so the ODR-clean
// include set (only tag_repo, no problem_repo / audit_log_repo)
// keeps main.cpp smoke registration tractable. problem_routes
// does NOT register /api/v1/tags.

    // POST /api/v1/admin/problems/import  (🔒 admin, SPEC §5.2, A17, A21) —
    //                                       owned by admin_bulk_import_routes.h
    //                                       (Phase 3 v1.2.10). This header
    //                                       intentionally does NOT register
    //                                       that endpoint; a separate
    //                                       register_admin_bulk_import_routes()
    //                                       call wires it.

    return server;
}

} // namespace litecode