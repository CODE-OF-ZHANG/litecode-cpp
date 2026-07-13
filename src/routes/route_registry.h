// SPDX-License-Identifier: MIT
// LiteCode-CPP — route registry forward declarations (v1.2.48)
//
// Each route header (auth_routes.h, problem_routes.h, ...) defines
// its register_X_routes as an inline function in a SUB-namespace
// (litecode::detail, litecode::admin_audit_log_routes, etc.) to
// dodge the cross-route ODR collision documented in memory
// `reference-odr-collision-msvc`.
//
// main.cpp includes this header (no heavy route/db includes) and
// calls the register_X_routes via the sub-namespace forward
// declarations below. The actual inline definitions live in the
// per-route .cpp files in src/routes/ — each .cpp pulls in
// exactly ONE route header so the inline req_string / req_int /
// truncate_for_envelope helpers (defined in db/*_repo.h) are
// emitted in exactly one TU.

#pragma once

#include "config.h"        // JwtConfig, RateLimitConfig
#include "judge/judge_notifier.h"
#include "judge/judge_scheduler.h"
#include "judge/warm_pool.h"
#include "middleware/rate_limit.h"
#include "routes/auth_routes.h"   // LoginFailureTracker + RefreshTokenStore

namespace litecode {

class HttpServer;
class HealthService;

// auth_routes.h: register_auth_routes is in namespace litecode (not
// detail — both detail blocks close before line 1715).
HttpServer& register_auth_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, LoginFailureTracker&,
    RefreshTokenStore&, const JwtConfig&, const RateLimitConfig&);

// problem_routes.h: register_problem_routes is in namespace litecode.
HttpServer& register_problem_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&);

// tag_routes.h: register_tag_routes is in namespace litecode.
HttpServer& register_tag_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&);

// stats_routes.h: register_stats_routes is in namespace
// litecode::stats_routes.
namespace stats_routes {
HttpServer& register_stats_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&,
    const JwtConfig&);
} // namespace stats_routes

// submission_routes.h: register_submission_routes is in namespace
// litecode::detail (second detail block reopens at 1061).
namespace detail {
HttpServer& register_submission_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&,
    const JwtConfig&, judge::JudgeScheduler*, judge::JudgeNotifier*);
} // namespace detail

// admin_audit_log_routes.h: register_admin_audit_log_routes is in
// namespace litecode::admin_audit_log_routes.
namespace admin_audit_log_routes {
HttpServer& register_admin_audit_log_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&,
    const JwtConfig&);
} // namespace admin_audit_log_routes

// admin_bulk_import_routes.h: register_admin_bulk_import_routes is
// in namespace litecode::bulk_import.
namespace bulk_import {
HttpServer& register_admin_bulk_import_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&,
    const JwtConfig&);
} // namespace bulk_import

// admin_problem_routes.h: register_admin_problem_routes is in
// namespace litecode::detail.
namespace detail {
HttpServer& register_admin_problem_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&,
    const JwtConfig&);
} // namespace detail

// admin_queue_routes.h: register_admin_queue_routes is in
// namespace litecode::admin_queue_routes (the `namespace judge {`
// block at the top of the header is just a forward-decl scope).
namespace admin_queue_routes {
HttpServer& register_admin_queue_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const JwtConfig&,
    const RateLimitConfig&,
    const judge::JudgeScheduler*, const judge::WarmPool*,
    const std::function<ProbeResult()>&);
} // namespace admin_queue_routes

// admin_stats_routes.h: register_admin_stats_routes is in
// namespace litecode::admin_stats_routes.
namespace admin_stats_routes {
HttpServer& register_admin_stats_routes(
    HttpServer&, ConnectionPool&, const JwtConfig&,
    const judge::JudgeScheduler*, const judge::WarmPool*,
    const std::function<ProbeResult()>&);
} // namespace admin_stats_routes

// admin_user_routes.h: register_admin_user_routes is in namespace
// litecode::admin_user_routes.
namespace admin_user_routes {
HttpServer& register_admin_user_routes(
    HttpServer&, ConnectionPool&, RateLimiter&, const RateLimitConfig&,
    const JwtConfig&);
} // namespace admin_user_routes

// system_routes.h: register_health_routes is in namespace litecode.
HttpServer& register_health_routes(HttpServer&, HealthService&);

} // namespace litecode