// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — checkin routes (Phase 7 ★)
//
// 每日打卡系统 API
//
// API 端点:
//   - GET  /api/v1/checkin/today    — 获取今日打卡状态（需认证）
//   - GET  /api/v1/checkin/calendar — 获取打卡日历（需认证）
//   - GET  /api/v1/checkin/stats    — 获取打卡统计（需认证）
//
// 打卡触发: 由 judge_scheduler.h 在 AC 成功后自动调用
//
// Wire shapes:
//
//   GET /api/v1/checkin/today (200):
//   {
//     "checked_in": true | false,
//     "checkin": { "id": 1, "problem_id": 42, "created_at": "..." }
//   }
//
//   GET /api/v1/checkin/calendar (200):
//   {
//     "records": [{ "date": "2026-07-30", "problem_id": 42 }, ...]
//   }
//
//   GET /api/v1/checkin/stats (200):
//   {
//     "current_streak": 5,
//     "longest_streak": 10,
//     "total_checkins": 30,
//     "checked_in_today": true
//   }
//
// Failure modes:
//   - No / bad access token → 401 UNAUTHORIZED (ApiException)
//   - Any repo throw        → 500 INTERNAL_ERROR

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"                     // RateLimitConfig / JwtConfig
#include "../db/connection_pool.h"         // ConnectionPool
#include "../db/checkin_repo.h"           // checkin_repo::*
#include "../middleware/auth_middleware.h" // require_authentication / Claims
#include "../routes/error_handler.h"       // ApiException / ErrorCode / send_error
#include "../server.h"                     // HttpServer / send_success

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Serializers
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_checkin_record(const CheckinRecordRow& r) {
    nlohmann::json j;
    j["date"] = r.checkin_date;
    if (r.problem_id) j["problem_id"] = *r.problem_id;
    return j;
}

// ────────────────────────────────────────────────────────────────────────────
//  Handlers
// ────────────────────────────────────────────────────────────────────────────

// GET /api/v1/checkin/today
inline void handle_checkin_today(
    httplib::Response&        res,
    const httplib::Request&   req,
    ConnectionPool&           pool,
    const JwtConfig&          jwt_cfg) {

    const Claims claims = require_authentication(req, jwt_cfg);
    const int user_id = std::stoi(claims.user_id);

    const auto today = checkin_repo::get_today_checkin(pool, user_id);

    nlohmann::json j;
    j["checked_in"] = today.has_value();
    if (today) {
        j["checkin"]["id"] = today->id;
        if (today->problem_id) j["checkin"]["problem_id"] = *today->problem_id;
        j["checkin"]["created_at"] = today->created_at;
    }
    send_success(res, std::move(j));
}

// GET /api/v1/checkin/calendar
inline void handle_checkin_calendar(
    httplib::Response&        res,
    const httplib::Request&   req,
    ConnectionPool&           pool,
    const JwtConfig&          jwt_cfg) {

    const Claims claims = require_authentication(req, jwt_cfg);
    const int user_id = std::stoi(claims.user_id);

    const auto records = checkin_repo::get_calendar(pool, user_id, 365);

    nlohmann::json j;
    j["records"] = nlohmann::json::array();
    for (const auto& r : records) {
        j["records"].push_back(serialize_checkin_record(r));
    }
    send_success(res, std::move(j));
}

// GET /api/v1/checkin/stats
inline void handle_checkin_stats(
    httplib::Response&        res,
    const httplib::Request&   req,
    ConnectionPool&           pool,
    const JwtConfig&          jwt_cfg) {

    const Claims claims = require_authentication(req, jwt_cfg);
    const int user_id = std::stoi(claims.user_id);

    const auto stats = checkin_repo::get_stats(pool, user_id);

    nlohmann::json j;
    j["current_streak"]   = stats.current_streak;
    j["longest_streak"]   = stats.longest_streak;
    j["total_checkins"]   = stats.total_checkins;
    j["checked_in_today"] = stats.checked_in_today;
    send_success(res, std::move(j));
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_checkin_routes(
    HttpServer&                server,
    ConnectionPool&            pool,
    RateLimiter&              /*limiter*/,
    const RateLimitConfig&    /*rate_cfg*/,
    const JwtConfig&          jwt_cfg) {

    // GET /api/v1/checkin/today
    server.get("/api/v1/checkin/today",
        [&pool, &jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                handle_checkin_today(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("checkin_today: handler threw",
                          {{"type", typeid(e).name()}, {"reason", e.what()}});
                if (res.body.empty()) {
                    send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                               std::string("internal error: ") + e.what());
                } else {
                    throw;
                }
            }
        });

    // GET /api/v1/checkin/calendar
    server.get("/api/v1/checkin/calendar",
        [&pool, &jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                handle_checkin_calendar(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("checkin_calendar: handler threw",
                          {{"type", typeid(e).name()}, {"reason", e.what()}});
                if (res.body.empty()) {
                    send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                               std::string("internal error: ") + e.what());
                } else {
                    throw;
                }
            }
        });

    // GET /api/v1/checkin/stats
    server.get("/api/v1/checkin/stats",
        [&pool, &jwt_cfg]
        (const httplib::Request& req, httplib::Response& res) {
            try {
                handle_checkin_stats(res, req, pool, jwt_cfg);
            } catch (const ApiException&) {
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("checkin_stats: handler threw",
                          {{"type", typeid(e).name()}, {"reason", e.what()}});
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
