// SPDX-License-Identifier: MIT
//
// LiteCode-CPP - problem routes (Phase 3 *)
//
// SPEC §5.2 / §11 Phase 3 / §15.2 / A4 acceptance:
//   - GET /api/v1/problems - public, 60/min/IP
//       paginated, difficulty / tag-id filtering, soft-deleted rows
//       hidden by default (SPEC §4.2 "前台列表自动过滤 is_deleted=FALSE")
//   - GET /api/v1/problems/:slug - public, 60/min/IP  (future: problem
//       detail; SPEC §5.2 - wired here as a 501 placeholder so the
//       route table is stable and the front-end can stage its fetch
//       path endpoint-by-endpoint)
//   - GET /api/v1/tags - public, no rate limit (future: tag list)
//   - POST /api/v1/admin/problems       (🔒 admin, future)
//   - PUT  /api/v1/admin/problems/:slug (🔒 admin, future)
//   - DELETE /api/v1/admin/problems/:slug (🔒 admin, future)
//   - POST /api/v1/admin/problems/import  (🔒 admin, future)
//
// Phase 3 * ships the LIST endpoint only. The rest of the file lays
// down the same `parse_*_request()` / handler / `register_*_routes()`
// shape that Phase 2 auth_routes.h established so the follow-up
// Phase 3 work (detail / admin CRUD / bulk import) can land as
// well-scoped additive commits.
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
//         filtering yes, surfacing no). The detail endpoint will
//         surface tags via tag_repo::list_tags_for_problem().
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
#include "../db/problem_repo.h"                 // problem_repo::list / ProblemListFilter / ProblemListResult
#include "../logger.h"                          // LOG_INFO / LOG_WARN
#include "../middleware/rate_limit.h"           // consume_rate_limit / problems_public_quota
#include "../server.h"                          // HttpServer / send_error / send_success / ErrorCode

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
//  Route registration
//
//  Returns HttpServer& so callers can chain. Phase 3 * ships only the
//  list endpoint; the detail / admin routes are wired as 501
//  placeholders so the route table is stable and the front-end can
//  stage its integration endpoint-by-endpoint.
//
//  Production usage (from main.cpp):
//
//    litecode::HttpServer     server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//    litecode::RateLimiter    limiter;
//    litecode::register_problem_routes(
//        server, pool, limiter, cfg.rate_limit);
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

    // GET /api/v1/problems/:slug - Phase 3 follow-up (SPEC §5.2, A5)
// 501 placeholder. The detail endpoint lands as an additive commit
// and reuses parse_list_query's siblings (parse_slug_param) once
// it ships.
    server.get(R"(/api/v1/problems/([^/]+))",
        [](const httplib::Request&, httplib::Response& res) {
            send_error(res, 501, ErrorCode::SERVICE_UNAVAILABLE,
                       "GET /api/v1/problems/:slug not yet implemented "
                       "(see SPEC section 11 Phase 3 - problem detail)");
        });

// GET /api/v1/tags - Phase 3 follow-up (SPEC §5.2)
// 501 placeholder. Will read tag_repo::list_with_count() and ship
// the same envelope shape as /api/v1/problems.
    server.get("/api/v1/tags",
        [](const httplib::Request&, httplib::Response& res) {
            send_error(res, 501, ErrorCode::SERVICE_UNAVAILABLE,
                       "GET /api/v1/tags not yet implemented "
                       "(see SPEC section 11 Phase 3 - tag list)");
        });

    // POST /api/v1/admin/problems       (🔒 admin, SPEC §5.2, A18)
    // PUT  /api/v1/admin/problems/:slug (🔒 admin, SPEC §5.2, A19)
    // DEL  /api/v1/admin/problems/:slug (🔒 admin, SPEC §5.2, A20)
    // POST /api/v1/admin/problems/import  (🔒 admin, SPEC §5.2, A17, A21)
    // All four land as Phase 3 follow-up commits with the admin
    // middleware (require_admin) wired in front of the handler.
    auto not_implemented = [](const httplib::Request&,
                              httplib::Response& res) {
        send_error(res, 501, ErrorCode::SERVICE_UNAVAILABLE,
                   "this admin problem endpoint is not yet implemented "
                   "(see SPEC §11 Phase 3 - admin problem CRUD / bulk import)");
    };
    server.post(R"(/api/v1/admin/problems/import)", not_implemented);
    server.post(R"(/api/v1/admin/problems)",         not_implemented);
    server.put (R"(/api/v1/admin/problems/([^/]+))", not_implemented);
    server.del (R"(/api/v1/admin/problems/([^/]+))", not_implemented);

    return server;
}

} // namespace litecode