// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — solution routes (Phase 7 ★)
//
// 题解系统 API

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"
#include "../db/connection_pool.h"
#include "../db/solution_repo.h"
#include "../middleware/auth_middleware.h"
#include "../routes/error_handler.h"
#include "../routes/notification_routes.h"
#include "../server.h"

namespace litecode {

// Types are defined in solution_repo.h (included above)

// ────────────────────────────────────────────────────────────────────────────
//  Repo helpers (inline SQL)
// ────────────────────────────────────────────────────────────────────────────

// find_problem_id_by_slug
inline std::optional<int> find_problem_id_by_slug(ConnectionPool& pool, const std::string& slug) {
    auto conn = pool.acquire();
    auto row = conn.fetch_one(
        "SELECT id FROM problems WHERE slug = ? AND is_deleted = FALSE LIMIT 1",
        slug);
    if (!row) return std::nullopt;
    return solution_detail::req_int(*row, 0, "id");
}

inline int create_solution(ConnectionPool& pool, int user_id, int problem_id,
                           const std::string& title, const std::string& content) {
    auto conn = pool.acquire();
    auto rs = conn.execute(
        "INSERT INTO solutions (user_id, problem_id, title, content) VALUES (?, ?, ?, ?)",
        user_id, problem_id, title, content);
    return static_cast<int>(rs.getAutoIncrementValue());
}

struct SolutionListResult {
    std::vector<SolutionListRow> items;
    int total = 0;
};

inline SolutionListResult list_solutions_for_problem(ConnectionPool& pool, int problem_id, int limit, int offset) {
    SolutionListResult result;
    auto conn = pool.acquire();

    auto count_row = conn.fetch_scalar<int>(
        "SELECT COUNT(*) FROM solutions WHERE problem_id = ? AND is_deleted = FALSE",
        problem_id);
    result.total = count_row.value_or(0);

    auto rs = conn.execute(
        "SELECT s.id, s.user_id, s.problem_id, s.title, s.content, s.like_count, "
        "       DATE_FORMAT(s.created_at, '%Y-%m-%d %H:%i:%s'), "
        "       DATE_FORMAT(s.updated_at, '%Y-%m-%d %H:%i:%s'), "
        "       s.is_deleted, "
        "       u.id, u.username, u.avatar "
        "FROM solutions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.problem_id = ? AND s.is_deleted = FALSE "
        "ORDER BY s.like_count DESC, s.created_at DESC "
        "LIMIT ? OFFSET ?",
        problem_id, static_cast<std::int64_t>(limit), static_cast<std::int64_t>(offset));

    for (auto row : rs) {
        try {
            SolutionListRow item;
            item.solution = solution_detail::row_to_solution(row);
            item.user.id       = static_cast<int>((row[9]).get<std::int64_t>());
            item.user.username  = (row[10]).get<std::string>();
            item.user.avatar   = solution_detail::opt_string(row, 11);
            result.items.push_back(std::move(item));
        } catch (...) {}
    }
    return result;
}

inline bool toggle_solution_like(ConnectionPool& pool, int solution_id, int user_id) {
    auto conn = pool.acquire();
    auto existing = conn.fetch_scalar<int>(
        "SELECT 1 FROM solution_likes WHERE user_id = ? AND solution_id = ? LIMIT 1",
        user_id, solution_id);

    if (existing.has_value()) {
        conn.execute("DELETE FROM solution_likes WHERE user_id = ? AND solution_id = ?", user_id, solution_id);
        conn.execute("UPDATE solutions SET like_count = GREATEST(0, like_count - 1) WHERE id = ?", solution_id);
        return false;
    } else {
        conn.execute("INSERT INTO solution_likes (user_id, solution_id) VALUES (?, ?)", user_id, solution_id);
        conn.execute("UPDATE solutions SET like_count = like_count + 1 WHERE id = ?", solution_id);
        return true;
    }
}

inline std::optional<std::string> find_username_by_id(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    auto row = conn.fetch_scalar<std::string>(
        "SELECT username FROM users WHERE id = ? LIMIT 1", user_id);
    return row;
}

// ────────────────────────────────────────────────────────────────────────────
//  Serializers
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_solution(const SolutionListRow& r) {
    nlohmann::json j;
    j["id"]          = r.solution.id;
    j["title"]        = r.solution.title;
    j["content"]      = r.solution.content;
    j["like_count"]   = r.solution.like_count;
    j["created_at"]   = r.solution.created_at;
    j["user"]["id"]       = r.user.id;
    j["user"]["username"] = r.user.username;
    if (r.user.avatar) j["user"]["avatar"] = *r.user.avatar;
    return j;
}

// ────────────────────────────────────────────────────────────────────────────
//  Handlers
// ────────────────────────────────────────────────────────────────────────────

// GET /api/v1/problems/:slug/solutions
inline void handle_list_solutions(httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {
    // 从路径提取 slug
    const auto& path = req.path;
    const std::string prefix = "/api/v1/problems/";
    if (path.size() <= prefix.size()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid path");
        return;
    }
    std::string slug_with_suffix = path.substr(prefix.size());
    auto slash_pos = slug_with_suffix.find('/');
    std::string slug = (slash_pos == std::string::npos) ? slug_with_suffix : slug_with_suffix.substr(0, slash_pos);
    if (slug.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid slug");
        return;
    }

    auto problem_id_opt = find_problem_id_by_slug(pool, slug);
    if (!problem_id_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found");
        return;
    }

    int limit = 20, offset = 0;
    if (req.has_param("limit")) {
        try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
    }
    if (req.has_param("offset")) {
        try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {}
    }
    if (limit <= 0) limit = 20;
    if (limit > 100) limit = 100;
    if (offset < 0) offset = 0;

    auto result = list_solutions_for_problem(pool, *problem_id_opt, limit, offset);

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : result.items) {
        items.push_back(serialize_solution(item));
    }

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  result.total},
        {"limit",  limit},
        {"offset", offset},
    });
}

// POST /api/v1/problems/:slug/solutions
inline void handle_create_solution(httplib::Response& res, const httplib::Request& req,
                                  ConnectionPool& pool, const JwtConfig& jwt_cfg) {
    auto claims = require_authentication(req, jwt_cfg);

    const auto& path = req.path;
    const std::string prefix = "/api/v1/problems/";
    if (path.size() <= prefix.size()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid path");
        return;
    }
    std::string slug_with_suffix = path.substr(prefix.size());
    auto slash_pos = slug_with_suffix.find('/');
    std::string slug = (slash_pos == std::string::npos) ? slug_with_suffix : slug_with_suffix.substr(0, slash_pos);

    auto problem_id_opt = find_problem_id_by_slug(pool, slug);
    if (!problem_id_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found");
        return;
    }

    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (!body.is_object()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid body");
        return;
    }

    auto title_it = body.find("title");
    auto content_it = body.find("content");
    if (title_it == body.end() || content_it == body.end()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "title and content required");
        return;
    }

    std::string title = title_it->get<std::string>();
    std::string content = content_it->get<std::string>();
    if (title.empty() || title.size() > 200) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "title must be 1-200 chars");
        return;
    }
    if (content.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content cannot be empty");
        return;
    }

    int user_id = std::stoi(claims.user_id);
    int id = create_solution(pool, user_id, *problem_id_opt, title, content);

    send_success(res, nlohmann::json{{"id", id}});
}

// GET /api/v1/solutions/:id
inline void handle_get_solution(httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {
    int solution_id = 0;
    {
        const auto& path = req.path;
        auto last_slash = path.rfind('/');
        if (last_slash != std::string::npos) {
            try { solution_id = std::stoi(path.substr(last_slash + 1)); } catch (...) {}
        }
    }
    if (solution_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid solution id");
        return;
    }

    auto solution_opt = solution_repo::find_by_id(pool, solution_id);
    if (!solution_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "solution not found");
        return;
    }

    SolutionUserInfo ui;
    ui.id = solution_opt->user_id;
    ui.username = find_username_by_id(pool, solution_opt->user_id).value_or("?");

    SolutionListRow row;
    row.solution = *solution_opt;
    row.user = ui;

    send_success(res, serialize_solution(row));
}

// POST /api/v1/solutions/:id/like
inline void handle_like_solution(httplib::Response& res, const httplib::Request& req,
                                 ConnectionPool& pool, const JwtConfig& jwt_cfg) {
    auto claims = require_authentication(req, jwt_cfg);

    int solution_id = 0;
    // 使用正则捕获组获取 ID
    if (req.matches.size() > 1) {
        try { solution_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (solution_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid solution id");
        return;
    }

    auto solution_opt = solution_repo::find_by_id(pool, solution_id);
    if (!solution_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "solution not found");
        return;
    }

    int user_id = std::stoi(claims.user_id);
    bool liked = toggle_solution_like(pool, solution_id, user_id);

    auto updated = solution_repo::find_by_id(pool, solution_id);

    // Phase 7 ★ 通知题解作者（仅在新点赞时）
    if (liked && solution_opt->user_id != user_id) {
        auto conn = pool.acquire();
        auto user_row = conn.fetch_one(
            "SELECT username FROM users WHERE id = ? LIMIT 1", user_id);
        std::string liker_username = user_row
            ? (*user_row)[0].get<std::string>()
            : "某用户";

        notify_solution_like(pool, solution_opt->user_id, user_id,
            solution_id, liker_username, solution_opt->title);
    }

    send_success(res, nlohmann::json{
        {"liked",      liked},
        {"like_count", updated ? updated->like_count : 0},
    });
}

// DELETE /api/v1/solutions/:id
inline void handle_delete_solution(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    int solution_id = 0;
    {
        const auto& path = req.path;
        auto last_slash = path.rfind('/');
        if (last_slash != std::string::npos) {
            try { solution_id = std::stoi(path.substr(last_slash + 1)); } catch (...) {}
        }
    }
    if (solution_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid solution id");
        return;
    }

    auto sol_opt = solution_repo::find_by_id(pool, solution_id);
    if (!sol_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "solution not found");
        return;
    }

    int user_id = std::stoi(claims.user_id);
    int author_id = sol_opt->user_id;
    bool is_admin = (claims.role == "admin");

    // 只有作者或管理员可以删除
    if (user_id != author_id && !is_admin) {
        send_error(res, 403, ErrorCode::FORBIDDEN, "not authorized to delete this solution");
        return;
    }

    solution_repo::soft_delete(pool, solution_id);
    send_success(res, nlohmann::json{{"deleted", true}});
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_solution_routes(
    HttpServer&                server,
    ConnectionPool&            pool,
    RateLimiter&              /*limiter*/,
    const RateLimitConfig&    /*rate_cfg*/,
    const JwtConfig&           jwt_cfg) {

    server.get(R"(/api/v1/problems/([^/]+)/solutions)",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_list_solutions(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("list_solutions: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    server.post(R"(/api/v1/problems/([^/]+)/solutions)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_create_solution(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("create_solution: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    server.get(R"(/api/v1/solutions/(\d+))",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_get_solution(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("get_solution: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    server.post(R"(/api/v1/solutions/(\d+)/like)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_like_solution(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("like_solution: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 删除题解
    server.del(R"(/api/v1/solutions/(\d+))",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_delete_solution(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("delete_solution: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    return server;
}

} // namespace litecode
