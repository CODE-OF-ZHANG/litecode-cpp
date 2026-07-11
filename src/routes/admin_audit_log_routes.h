// SPDX-License-Identifier: MIT
//
// LiteCode-CPP -- admin audit-log routes (Phase 6 *)
//
// SPEC Section 5.5 / Section 11 Phase 6 / Section 15.6 / A17, A18, A19, A20, A27:
//   - GET /api/v1/admin/audit-logs         ([admin] admin, 60/min/admin)
//
//       Paginated, filterable view over the `audit_logs` table for the
//       /admin/audit-logs.html page (v1.2.35). The page already renders
//       against the wire shape below; this commit is what makes that
//       page a real data flow rather than the "endpoint pending"
//       soft-state it had been showing since v1.2.35.
//
// Wire flow:
//   1) require_admin(...)                       -- 401 / 403 envelope
//   2) consume_rate_limit(admin_audit_logs_quota) -- 429 envelope
//   3) parse_list_query                         -- 400 on bad shape
//      (admin_id / action / target_type /
//       target_id / since / until / limit / offset)
//   4) audit_log_repo::count + audit_log_repo::list
//      (the repo already does WHERE build + bind chain)
//   5) serialize_audit_row x N (parse `payload` back to JSON;
//      fall back to null on parse failure -- keeps the response
//      well-formed even when a buggy write put garbage in payload)
//   6) send_success(200, {items, total, limit, offset})
//
// Wire shape:
//   GET /api/v1/admin/audit-logs?action=user.role_change
//                          &target_type=user
//                          &since=2026-07-01
//                          &until=2026-07-11
//                          &limit=20
//                          &offset=0
//     200 + {
//       "data": {
//         "items": [
//           { "id": 123,
//             "admin_id": 5 | null,
//             "action": "user.role_change",
//             "target_type": "user" | null,
//             "target_id": "42" | null,
//             "payload": {...} | null,
//             "ip": "1.2.3.4" | null,
//             "created_at": "2026-07-10 12:34:56" },
//           ...
//         ],
//         "total": 1234,
//         "limit": 20,
//         "offset": 0
//       },
//       "request_id": "..."
//     }
//
// Failure modes:
//   - No / bad access token       -> 401 UNAUTHORIZED
//   - Token valid but != admin    -> 403 FORBIDDEN
//   - Rate limit tripped          -> 429 RATE_LIMITED
//   - Bad since/until (non-printable / wrong length) -> 400 INVALID_INPUT
//      details.field="since" | "until"
//   - Bad limit / offset          -> 400 INVALID_INPUT
//   - Bad admin_id (non-integer / <= 0 / > INT_MAX)
//                                -> 400 INVALID_INPUT
//      details.field="admin_id"
//   - Repo throws (driver error / network blip) -> 500 INTERNAL_ERROR
//
// Design notes:
//   - Header-only + inline: matches every other Phase 6 admin route
//     (admin_user_routes / admin_problem_routes / admin_stats_routes).
//   - All writes to `audit_logs` happen elsewhere (audit_log_repo::record
//     in admin_user_routes, admin_problem_routes, admin_bulk_import_routes,
//     and auth_routes). This endpoint is the **read** companion; SPEC Section 15.6
//     notes that read paths shouldn't themselves be audited (pollutes the
//     trail with one row per page refresh).
//   - The audit_log_repo already implements the list / count / WHERE / bind
//     logic (Phase 3 *) -- this route is a thin transport shim that:
//       a) parses query params into an AuditListFilter,
//       b) calls audit_log_repo::list (which goes via count() for `total`),
//       c) materializes each row to JSON, parsing `payload` text back to
//          a structured object (when valid JSON).
//   - `payload` is exposed as `nlohmann::json`. If it isn't valid JSON
//     (which shouldn't happen -- every writer uses nlohmann::json::dump()
//     to produce it -- but a manual SQL UPDATE could break the invariant),
//     we set the field to `null` rather than surfacing a 500. The raw text
//     is preserved in the comment-free shape; JSON consumers can still
//     see "this row has payload but it's malformed" through the null.
//   - ODR caveat: this header pulls in audit_log_repo.h directly. Because
//     audit_log_repo.h uses `audit_log_repo::detail::req_string` (not the
//     naked `litecode::detail` namespace that user_repo / problem_repo
//     collide on), it's safe to include alongside other Phase 6 headers
//     in the test binary. main.cpp does NOT smoke-register this header
//     for the same reason it doesn't register the other Phase 6 admin
//     route modules (admin_user_routes / admin_stats_routes / etc.) --
//     the cross-route ODR risk persists even though it's narrower here.
//     End-to-end coverage is owned by tests/unit/test_admin_audit_logs.cpp.
//
// Usage (production, from main.cpp -- once the cross-route ODR problem
// gets unwound, this becomes the canonical registration pattern):
//
//   litecode::HttpServer     server(cfg.server, cfg.cors);
//   litecode::ConnectionPool pool(PoolConfig::from_database_config(cfg.database));
//   litecode::RateLimiter    limiter;
//   litecode::register_admin_audit_log_routes(server, pool, limiter,
//                                             cfg.rate_limit, cfg.jwt);
//
// Usage (test, from gtest):
//
//   litecode::HttpServer     server(dev_server(), dev_cors());
//   litecode::ConnectionPool pool(test_db_config());
//   litecode::RateLimiter    limiter;
//   litecode::JwtConfig      jwt_cfg = test_jwt_config();
//   litecode::register_admin_audit_log_routes(
//       server, pool, limiter, lax_rate_limit(), jwt_cfg);
//   auto h = start_server(&server);
//   auto r = h.client->Get("/api/v1/admin/audit-logs?action=problem.delete",
//                          admin_token);

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysqlx/xdevapi.h>

#include "../config.h"                                // RateLimitConfig / JwtConfig
#include "../db/audit_log_repo.h"                     // AuditListFilter / AuditListResult / AuditRow / validate_datetime / clamp_list_filter
#include "../db/connection_pool.h"                    // ConnectionPool
#include "../logger.h"                                // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../middleware/admin_middleware.h"           // require_admin
#include "../middleware/rate_limit.h"                 // consume_rate_limit / admin_audit_logs_quota
#include "../routes/error_handler.h"                  // parse_json_body / ErrorCode / send_error / send_success
#include "../server.h"                                // HttpServer / send_success / send_error / ApiException

namespace litecode {

namespace admin_audit_log_routes {

// --------------------------------------------------------------------------------------------------------------------------------------------------------
//  Parsing helpers -- local namespace to dodge cross-route ODR collisions.
//  Mirrors the convention used in admin_user_routes.h / admin_problem_routes.h.
// --------------------------------------------------------------------------------------------------------------------------------------------------------

namespace detail {

// truncate_for_envelope -- keep the value field in 400 envelopes bounded
// to 64 chars (a hostile value could otherwise be multi-MB and balloon
// the error body). The "+..." suffix is appended to make the truncation
// obvious to the admin reading the error in the UI.
inline constexpr std::size_t kAdminAuditValueMax = 64;

inline std::string truncate_for_envelope(const std::string& v) {
    if (v.size() <= kAdminAuditValueMax) return v;
    return v.substr(0, kAdminAuditValueMax) + "...";
}

// parse_admin_id_param -- accept only an ASCII-positive integer in
// [1, INT_MAX]. Returns nullopt on any malformed input (the handler
// maps to 400 INVALID_INPUT). We use the same shape as admin_user_routes
// so the two admin read endpoints look symmetric to a tester.
inline std::optional<int> parse_admin_id_param(std::string_view raw) {
    if (raw.empty()) return std::nullopt;
    if (raw.size() > 11) return std::nullopt;   // INT_MAX = 2147483647 (10 digits)
    for (char c : raw) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        const long v = std::stol(std::string(raw));
        if (v < 1 || v > 2147483647L) return std::nullopt;
        return static_cast<int>(v);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// parse_datetime_param -- accept a YYYY-MM-DD or YYYY-MM-DD HH:MM:SS
// string. We delegate the printable-charset + length check to
// litecode::validate_datetime (the validator that lives at the top
// of audit_log_repo.h, just outside the audit_log_repo namespace
// block -- same validator the repo uses before binding the value
// to a `?` placeholder).
inline bool parse_datetime_param(std::string_view raw,
                                std::string_view field_name,
                                std::string& out_value,
                                httplib::Response& res) {
    if (raw.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "datetime value is empty",
                   {{"field", std::string(field_name)}});
        return false;
    }
    std::string err;
    if (!litecode::validate_datetime(raw, &err)) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   std::string("invalid datetime: ") + err,
                   {{"field", std::string(field_name)},
                    {"value", truncate_for_envelope(std::string(raw))}});
        return false;
    }
    out_value = std::string(raw);
    return true;
}

// parse_list_query -- translate req.{has_param, get_param_value}
// into an audit_log_repo::AuditListFilter. Recognized query params:
//   - admin_id     (optional; positive integer <= INT_MAX)
//   - action       (optional; free-form string, <= kMaxActionLength)
//   - target_type  (optional; <= kMaxTargetTypeLength)
//   - target_id    (optional; <= kMaxTargetIdLength)
//   - since        (optional; YYYY-MM-DD or YYYY-MM-DD HH:MM:SS)
//   - until        (optional; YYYY-MM-DD or YYYY-MM-DD HH:MM:SS)
//   - limit        (optional; default 20, range [1, kMaxAuditListLimit])
//   - offset       (optional; default 0, >= 0)
//
// Bad shape writes a 400 envelope and returns false; success returns
// true and populates `out`. The route handler then calls the repo,
// which applies clamp_list_filter() defensively.
//
// Note on free-form strings (action / target_type / target_id):
// the audit_log_repo::validate_* functions reject control chars and
// length-limit the values against the V002 column widths. We replicate
// that here so a hostile query string can't reach the repo's bind()
// step with a 10-MB action value (the route layer's first line of
// defense is rejecting it before constructing the SELECT).
inline bool parse_list_query(const httplib::Request& req,
                             httplib::Response&     res,
                             litecode::AuditListFilter& out) {
    out = litecode::AuditListFilter{};

    if (req.has_param("admin_id")) {
        const std::string raw = req.get_param_value("admin_id");
        const auto v = parse_admin_id_param(raw);
        if (!v.has_value()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "admin_id must be a positive integer",
                       {{"field", "admin_id"},
                        {"value", truncate_for_envelope(raw)}});
            return false;
        }
        out.admin_id = *v;
    }
    if (req.has_param("action")) {
        const std::string raw = req.get_param_value("action");
        std::string err;
        if (!litecode::validate_action(raw, &err)) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       std::string("invalid action: ") + err,
                       {{"field", "action"},
                        {"value", truncate_for_envelope(raw)}});
            return false;
        }
        out.action = raw;
    }
    if (req.has_param("target_type")) {
        const std::string raw = req.get_param_value("target_type");
        std::string err;
        if (!litecode::validate_target_type(raw, &err)) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       std::string("invalid target_type: ") + err,
                       {{"field", "target_type"},
                        {"value", truncate_for_envelope(raw)}});
            return false;
        }
        out.target_type = raw;
    }
    if (req.has_param("target_id")) {
        const std::string raw = req.get_param_value("target_id");
        std::string err;
        if (!litecode::validate_target_id(raw, &err)) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       std::string("invalid target_id: ") + err,
                       {{"field", "target_id"},
                        {"value", truncate_for_envelope(raw)}});
            return false;
        }
        out.target_id = raw;
    }
    if (req.has_param("since")) {
        const std::string raw = req.get_param_value("since");
        std::string v;
        if (!parse_datetime_param(raw, "since", v, res)) return false;
        out.since = v;
    }
    if (req.has_param("until")) {
        const std::string raw = req.get_param_value("until");
        std::string v;
        if (!parse_datetime_param(raw, "until", v, res)) return false;
        out.until = v;
    }
    if (req.has_param("limit")) {
        const std::string raw = req.get_param_value("limit");
        if (raw.empty()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
        try {
            std::size_t consumed = 0;
            const int v = std::stoi(raw, &consumed);
            if (consumed != raw.size() || v < 1) {
                send_error(res, 400, ErrorCode::INVALID_INPUT,
                           "limit must be a positive integer",
                           {{"field", "limit"},
                            {"value", raw}});
                return false;
            }
            out.limit = v;
        } catch (const std::exception&) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "limit must be a positive integer",
                       {{"field", "limit"},
                        {"value", raw}});
            return false;
        }
    }
    if (req.has_param("offset")) {
        const std::string raw = req.get_param_value("offset");
        if (raw.empty()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
        try {
            std::size_t consumed = 0;
            const int v = std::stoi(raw, &consumed);
            if (consumed != raw.size() || v < 0) {
                send_error(res, 400, ErrorCode::INVALID_INPUT,
                           "offset must be a non-negative integer",
                           {{"field", "offset"},
                            {"value", raw}});
                return false;
            }
            out.offset = v;
        } catch (const std::exception&) {
            send_error(res, 400, ErrorCode::INVALID_INPUT,
                       "offset must be a non-negative integer",
                       {{"field", "offset"},
                        {"value", raw}});
            return false;
        }
    }
    // Final defensive clamp so the caller can read the effective limit
    // and offset back out of the filter struct. The repo would clamp
    // again (clamp_list_filter is idempotent), but doing it here keeps
    // the route's `out` aligned with what the SELECT will see.
    litecode::clamp_list_filter(out);
    return true;
}

}  // namespace detail

// --------------------------------------------------------------------------------------------------------------------------------------------------------
//  Row -> JSON
//
//  Field-by-field serialization, mirroring the wire shape the admin UI
//  (v1.2.35) already renders against. `payload` is JSON-typed from the
//  repo's `CAST(payload AS CHAR)` round-trip; we parse it back to a
//  structured object when it parses cleanly, otherwise emit `null`.
//
//  We deliberately do NOT include `admin_username` -- the spec leaves
//  that to the front-end (admin_id is shown as a link to
//  /admin/users.html?q=id:N). This keeps the wire payload small and
//  avoids widening the ODR surface with a user_repo::find_by_username
//  call.
// --------------------------------------------------------------------------------------------------------------------------------------------------------

inline nlohmann::json serialize_audit_row(const litecode::AuditRow& r) {
    nlohmann::json j = {
        {"id",         r.id},
        {"action",     r.action},
        {"created_at", r.created_at},
    };
    if (r.admin_id.has_value()) j["admin_id"] = *r.admin_id;
    else                        j["admin_id"] = nullptr;

    if (r.target_type.has_value()) j["target_type"] = *r.target_type;
    else                          j["target_type"] = nullptr;
    if (r.target_id.has_value())   j["target_id"]   = *r.target_id;
    else                          j["target_id"]   = nullptr;

    if (r.ip.has_value()) j["ip"] = *r.ip;
    else                  j["ip"] = nullptr;

    // payload round-trip. The repo stores JSON text (well, the bound
    // MySQL JSON column). We try to parse; on failure we keep null so
    // the wire shape stays homogeneous -- a never-set field and a
    // never-parsable field are visually identical, with the operator
    // able to drill in via /admin/audit-logs/:id (Phase 7+) if they
    // need the raw bytes.
    if (r.payload.has_value()) {
        try {
            nlohmann::json parsed = nlohmann::json::parse(*r.payload);
            j["payload"] = std::move(parsed);
        } catch (const std::exception&) {
            j["payload"] = nullptr;
        }
    } else {
        j["payload"] = nullptr;
    }

    return j;
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------
//  GET /api/v1/admin/audit-logs   (SPEC Section 5.5, Section 15.6)
//
//  Wire flow:
//    1) require_admin(...)                       -- 401 / 403
//    2) consume_rate_limit(admin_audit_logs_quota) -- 429
//    3) parse_list_query                         -- 400 on bad shape
//       (admin_id / action / target_type /
//        target_id / since / until / limit / offset)
//    4) audit_log_repo::list(pool, f)
//       - returns {items, total, limit, offset}
//       - throws AuditLogRepoError on driver / parse errors
//    5) serialize each row (parse payload back to JSON)
//    6) send_success(200, {items, total, limit, offset})
//
//  Authorization: admin only.
//  Audit log:      NOT written (read path -- see preamble).
//  Rate limit:     admin.audit_logs bucket, 60/min/admin by default.
// --------------------------------------------------------------------------------------------------------------------------------------------------------

inline void list_admin_audit_logs_handler(
        httplib::Response&             res,
        const httplib::Request&        req,
        ConnectionPool&                pool,
        RateLimiter&                   limiter,
        const RateLimitConfig&         rate_cfg,
        const JwtConfig&               jwt_cfg) {

    // 1) Admin gate. require_admin throws ApiException(401, ...) or
    //    ApiException(403, ...); server.h's per-request wrap emits
    //    the envelope. Read paths don't audit-log so we don't need
    //    to capture the Claims.
    require_admin(req, jwt_cfg);

    // 2) Rate limit (admin.audit_logs bucket, keyed by user_id).
    consume_rate_limit(res, req, limiter, admin_audit_logs_quota(rate_cfg));

    // 3) Parse query.
    litecode::AuditListFilter f;
    if (!detail::parse_list_query(req, res, f)) {
        return;  // 400 already on the wire
    }

    // 4) Repo call. audit_log_repo::list does the count() inside
    //    (saving a round-trip); throws AuditLogRepoError on driver /
    //    parse errors. A throw here is 500 -- a partial-success on a
    //    read endpoint would be worse than a hard failure (the admin
    //    can't trust the visible rows if total doesn't match them).
    litecode::AuditListResult page;
    try {
        page = audit_log_repo::list(pool, f);
    } catch (const std::exception& e) {
        LOG_ERROR("admin_audit_logs: repo threw",
                  {{"type",   typeid(e).name()},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   std::string("internal error: ") + e.what());
        return;
    }

    // 5) Serialize. items.start to push_back one at a time so a
    //    single corrupt-row doesn't kill the whole page (defensive --
    //    every well-formed row has the same shape, but a future
    //    schema-change regression could blow up here).
    nlohmann::json items = nlohmann::json::array();
    for (const auto& row : page.items) {
        try {
            items.push_back(serialize_audit_row(row));
        } catch (const std::exception& e) {
            // Should not happen -- serialize_audit_row doesn't throw
            // for valid AuditRow inputs. Log and skip rather than
            // fail the whole request.
            LOG_WARN("admin_audit_logs: serialize_audit_row threw",
                     {{"id",     std::to_string(row.id)},
                      {"type",   typeid(e).name()},
                      {"reason", e.what()}});
        }
    }

    LOG_INFO("admin_audit_logs: served",
             {{"total",     std::to_string(page.total)},
              {"returned",  std::to_string(items.size())},
              {"limit",     std::to_string(page.limit)},
              {"offset",    std::to_string(page.offset)},
              {"has_admin",  f.admin_id.has_value()    ? "true" : "false"},
              {"has_action", f.action.has_value()      ? "true" : "false"},
              {"has_type",   f.target_type.has_value() ? "true" : "false"},
              {"has_id",     f.target_id.has_value()   ? "true" : "false"},
              {"has_since",  f.since.has_value()       ? "true" : "false"},
              {"has_until",  f.until.has_value()       ? "true" : "false"}});

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  page.total},
        {"limit",  page.limit},
        {"offset", page.offset},
    });
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------
//  Route registration
//
//  Returns HttpServer& so callers can chain. jwt_cfg is captured by
//  reference; the caller (main() / test fixture) owns it. rate_cfg
//  is captured by value (defends against a temporary RateLimitConfig
//  going out of scope -- same defensive pattern as problem_routes.h /
//  admin_user_routes.h).
//
//  Production usage (from main.cpp):
//
//    litecode::register_admin_audit_log_routes(
//        server, pool, limiter, cfg.rate_limit, cfg.jwt);
//
//  Tests pass an in-process server + a freshly-constructed pool +
//  a fresh RateLimiter (so bucket state is per-test).
//
//  ODR caveat (see header preamble): this header is not registered
//  from main.cpp for the same reason admin_user_routes.h /
//  admin_stats_routes.h / admin_problem_routes.h /
//  admin_bulk_import_routes.h aren't. End-to-end coverage is owned
//  by tests/unit/test_admin_audit_logs.cpp.
// --------------------------------------------------------------------------------------------------------------------------------------------------------

inline HttpServer& register_admin_audit_log_routes(
        HttpServer&            server,
        ConnectionPool&        pool,
        RateLimiter&           limiter,
        const RateLimitConfig& rate_cfg,
        const JwtConfig&       jwt_cfg) {

    // GET /api/v1/admin/audit-logs -- list (SPEC Section 5.5, Section 15.6).
    server.get("/api/v1/admin/audit-logs",
        [&pool, &limiter, rate_cfg, jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                list_admin_audit_logs_handler(res, req, pool, limiter,
                                              rate_cfg, jwt_cfg);
            } catch (const ApiException&) {
                // Already an envelope -- let server.h wrap() emit it.
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("admin_audit_logs: handler threw",
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

}  // namespace admin_audit_log_routes

}  // namespace litecode
