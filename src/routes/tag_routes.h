// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — tag routes (Phase 3 *)
//
// SPEC §4.2b / §4.2c / §5.2 / §11 Phase 3 / A4 + A18 acceptance:
//   - GET /api/v1/tags - public, NO rate limit (SPEC §5.2 row 3)
//
//       Returns the full list of tags ordered by name ASC, each row
//       carrying a `problem_count` (live problems only — soft-deleted
//       rows excluded, mirroring the public problem list behavior
//       in problem_repo::list()). This is the same shape the admin
//       "tag management" view wants, but the public endpoint never
//       needs to expose soft-deleted problems; the admin path (Phase 6)
//       will own the `count_live_problems=false` toggle.
//
//       Pagination is deliberately omitted. SPEC §5.2 says "all
//       tags" and tags are bounded (handful of dozen for a learning
//       OJ). tag_repo::list_with_count() mirrors this contract:
//       no LIMIT, no OFFSET. The response is `{items, total}` only.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1/2/3 route
//     module (auth_routes.h / problem_routes.h / system_routes.h).
//     Tests link this header directly and instantiate the route set
//     with a real ConnectionPool + a (lax) RateLimiter (the route
//     does not call consume_rate_limit() — the spec puts no quota
//     on this endpoint — but the registration signature mirrors
//     problem_routes.h so a future "add 60/min/IP quota" commit
//     doesn't have to retune every test call site).
//   - The handler is the single funnel for GET /api/v1/tags. It:
//       1) tag_repo::list_with_count() - throws on driver error ->
//                                          500 envelope
//       2) send_success()             - 200 + {items, total}
//   - The list response deliberately OMITS a per-tag association
//     row dump. SPEC §5.2 "题目列表页，支持按难度/标签筛选" — the
//     public read path is "give me the chip list", not "give me
//     the chip<->problem pivot". problem_repo::list() owns
//     tag_id filtering via EXISTS; problem_routes::get_problem_detail_handler
//     owns per-problem tag surfacing.
//   - All validation messages funnel into the SPEC §5.7 unified
//     envelope via send_error(). The handler itself never writes a
//     raw body.
//   - The ODR concern that motivated the v1.2.6 / v1.2.7 changelog
//     note ("main.cpp pulls in both auth_routes.h and
//     problem_routes.h, and both define litecode::detail::req_string
//     / req_int") does NOT apply here: tag_routes.h does not pull
//     in problem_repo.h or audit_log_repo.h, and it does not define
//     any `litecode::detail::*` helpers (the only helpers live in
//     `litecode::tag_routes::detail`, mirroring the tag_repo /
//     test_case_repo convention). main.cpp can include this header
//     alongside auth_routes.h without an ODR collision; we still
//     do NOT smoke-register tag_routes from main.cpp for the same
//     reason problem_routes isn't there — end-to-end coverage is
//     owned by the test binary.
//
// Usage (production, from main.cpp):
//
//   litecode::HttpServer     server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter    limiter;
//   litecode::register_tag_routes(server, pool, limiter, cfg.rate_limit);
//   server.listen_blocking();
//
// Usage (test, from gtest):
//
//   litecode::HttpServer     server(dev_server(), dev_cors());
//   litecode::ConnectionPool pool(test_db_config());
//   litecode::RateLimiter    limiter;
//   litecode::register_tag_routes(server, pool, limiter, lax_rate_limit());
//   auto h = start_server(&server);
//   auto r = h.client->Get("/api/v1/tags");

#pragma once

#include <cstddef>
#include <exception>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                          // RateLimitConfig
#include "../db/connection_pool.h"              // ConnectionPool
#include "../db/tag_repo.h"                     // tag_repo::list_with_count / TagWithCount
#include "../logger.h"                          // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../server.h"                          // HttpServer / send_error / send_success / ErrorCode

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Row -> JSON
//
//  We deliberately OMIT timestamps (tags table has no created_at /
//  updated_at column — SPEC §4.2b lists only id + name). The
//  `problem_count` is the live-problem count (soft-deleted problems
//  excluded); the admin path will add a separate flag if it wants
//  to surface tombstones.
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_tag_with_count(
        const litecode::TagWithCount& twc) {
    return nlohmann::json{
        {"id",            twc.tag.id},
        {"name",          twc.tag.name},
        {"problem_count", twc.problem_count},
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  GET /api/v1/tags   - Phase 3 *  (SPEC §5.2, §11 Phase 3, A4)
//
//  Wire flow:
//    1) tag_repo::list_with_count() - throws on driver error -> 500
//    2) send_success()              - 200 + {items, total}
//
//  Authorization: NONE. SPEC §5.2 row says "公开". The route
//  handler does not invoke require_authentication; a missing /
//  bogus Authorization header (if present) is ignored.
//
//  Rate limit: NONE. SPEC §5.2 does not list a quota for this
//  endpoint, and the row in problem_routes.h's header comment
//  reads "GET /api/v1/tags - public, no rate limit (future:
//  tag list)". The handler therefore does NOT call
//  consume_rate_limit() and the response carries no
//  X-RateLimit-* headers. If a future quota lands on this
//  endpoint, the line to add is:
//      consume_rate_limit(res, req, limiter, problems_public_quota(rate_cfg));
//  and the rate_limit policy header in tests/unit/test_tag_list.cpp
//  must change to assert the X-RateLimit-* presence.
//
//  Soft delete: ALWAYS applied to `problem_count`. The handler
//  hard-codes count_live_problems=true — soft-deleted problems
//  don't count. This mirrors the public problem list (which forces
//  include_deleted=false) and prevents the "Hash" tag from showing
//  count=12 just because 12 tombstones used to carry it.
//
//  What we DON'T do here:
//    - We don't paginate. SPEC §5.2 "all tags"; the repo's
//      list_with_count() also doesn't paginate. The admin path
//      (future, with `count_live_problems=false`) can layer
//      pagination on if the table grows past a few hundred rows.
//    - We don't include per-tag problem ids. The frontend renders
//      chips; clicking a chip navigates to /problems?tag_id=N and
//      the existing list endpoint handles the filter.
//    - We don't write to audit_logs. Public reads are not
//      auditable actions; the structured log line below is enough
//      for ops.
// ────────────────────────────────────────────────────────────────────────────

inline void list_tags_handler(httplib::Response&         res,
                              const httplib::Request&    req,
                              litecode::ConnectionPool&  pool,
                              litecode::RateLimiter&     /*limiter*/,
                              const litecode::RateLimitConfig& /*rate_cfg*/) {
    // Repo dispatch. list_with_count() throws TagRepoError on driver
    // failure; the lambda's catch (std::exception&) below folds it
    // into a 500 envelope. count_live_problems=true is hard-coded
    // (see handler docstring).
    std::vector<litecode::TagWithCount> rows;
    try {
        rows = litecode::tag_repo::list_with_count(
            pool, /*count_live_problems=*/true);
    } catch (const std::exception& e) {
        LOG_ERROR("tag_list: list_with_count threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, litecode::ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // Serialize. nlohmann::json lets us build the items array
    // inline; serialize_tag_with_count keeps the per-row shape in
    // one place so the future admin path can reuse it without drift.
    nlohmann::json items = nlohmann::json::array();
    for (const auto& row : rows) {
        items.push_back(serialize_tag_with_count(row));
    }

    LOG_INFO("tag_list: served",
             {{"total",    std::to_string(rows.size())},
              {"returned", std::to_string(items.size())}});

    send_success(res, nlohmann::json{
        {"items", std::move(items)},
        {"total", rows.size()},
    });
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
//
//  Returns HttpServer& so callers can chain. Phase 3 * ships only the
//  list endpoint. Admin write paths under /api/v1/admin/tags (future)
//  are wired separately, in admin_routes.h.
//
//  Production usage (from main.cpp):
//
//    litecode::HttpServer     server(cfg.server, cfg.cors);
//    litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//    litecode::RateLimiter    limiter;
//    litecode::register_tag_routes(server, pool, limiter, cfg.rate_limit);
//    server.listen_blocking();
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter (so bucket state is per-test, even though
//  this endpoint doesn't consume a bucket).
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_tag_routes(HttpServer&              server,
                                       ConnectionPool&          pool,
                                       RateLimiter&             limiter,
                                       const RateLimitConfig&   rate_cfg) {
    // GET /api/v1/tags - public list, no rate limit (SPEC §5.2).
    //
    // The lambda captures pool / limiter BY REFERENCE (they outlive
    // the server, owned by main() / the test fixture) and rate_cfg BY
    // VALUE (defends against a temporary RateLimitConfig going out
    // of scope - same defensive pattern as problem_routes.h).
    server.get("/api/v1/tags",
        [&pool, &limiter, rate_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                list_tags_handler(res, req, pool, limiter, rate_cfg);
            } catch (const ApiException&) {
                // Already an envelope - let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("tag_list: handler threw",
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