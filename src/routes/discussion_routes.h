// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — discussion routes (Phase 7 ★)
//
// 讨论系统 API

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"
#include "../db/connection_pool.h"
#include "../db/discussion_repo.h"
#include "../middleware/auth_middleware.h"
#include "../routes/notification_routes.h"
#include "../routes/error_handler.h"
#include "../server.h"
#include "../utils/uuid.h"

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Serializers
// ────────────────────────────────────────────────────────────────────────────

inline nlohmann::json serialize_discussion(const DiscussionListRow& r) {
    nlohmann::json j;
    j["id"]           = r.discussion.id;
    j["user_id"]      = r.discussion.user_id;
    j["content"]       = r.discussion.content;
    j["like_count"]   = r.discussion.like_count;
    j["reply_count"]  = r.discussion.reply_count;
    j["created_at"]   = r.discussion.created_at;
    j["updated_at"]   = r.discussion.updated_at;
    if (r.discussion.title)       j["title"]       = *r.discussion.title;
    if (r.discussion.problem_id) j["problem_id"]  = *r.discussion.problem_id;
    if (r.discussion.parent_id)   j["parent_id"]   = *r.discussion.parent_id;
    if (r.discussion.root_id)    j["root_id"]      = *r.discussion.root_id;
    j["user"]["id"]       = r.user.id;
    j["user"]["username"] = r.user.username;
    if (r.user.avatar) j["user"]["avatar"] = *r.user.avatar;
    if (r.problem_slug)  j["problem_slug"]  = *r.problem_slug;
    if (r.problem_title) j["problem_title"] = *r.problem_title;
    return j;
}

// ────────────────────────────────────────────────────────────────────────────
//  Handlers
// ────────────────────────────────────────────────────────────────────────────

// GET /api/v1/problems/:slug/discussions
inline void handle_list_problem_discussions(
    httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {

    std::string slug;
    {
        const auto& path = req.path;
        const std::string prefix = "/api/v1/problems/";
        if (path.size() <= prefix.size()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid path");
            return;
        }
        std::string suffix = path.substr(prefix.size());
        auto slash = suffix.find('/');
        slug = (slash == std::string::npos) ? suffix : suffix.substr(0, slash);
    }

    auto problem_id_opt = discussion_repo::find_problem_id_by_slug(pool, slug);
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

    discussion_repo::DiscussionListFilter filter;
    filter.problem_id = *problem_id_opt;
    filter.limit = limit;
    filter.offset = offset;

    auto result = discussion_repo::list_discussions(pool, filter);

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : result.items) {
        items.push_back(serialize_discussion(item));
    }

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  result.total},
        {"limit",  limit},
        {"offset", offset},
    });
}

// POST /api/v1/problems/:slug/discussions
inline void handle_create_problem_discussion(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    std::string slug;
    {
        const auto& path = req.path;
        const std::string prefix = "/api/v1/problems/";
        if (path.size() <= prefix.size()) {
            send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid path");
            return;
        }
        std::string suffix = path.substr(prefix.size());
        auto slash = suffix.find('/');
        slug = (slash == std::string::npos) ? suffix : suffix.substr(0, slash);
    }

    auto problem_id_opt = discussion_repo::find_problem_id_by_slug(pool, slug);
    if (!problem_id_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "problem not found");
        return;
    }

    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (!body.is_object()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid body");
        return;
    }

    auto content_it = body.find("content");
    if (content_it == body.end()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content required");
        return;
    }

    std::string content = content_it->get<std::string>();
    if (content.empty() || content.size() > 10000) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content must be 1-10000 chars");
        return;
    }

    int user_id = std::stoi(claims.user_id);

    DiscussionRow d;
    d.user_id    = user_id;
    d.problem_id = *problem_id_opt;
    d.content    = content;

    int id = discussion_repo::create(pool, d);

    send_success(res, nlohmann::json{{"id", id}});
}

// GET /api/v1/discussions (全局讨论列表)
inline void handle_list_global_discussions(
    httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {

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

    discussion_repo::DiscussionListFilter filter;
    filter.problem_id = std::nullopt;   // 全局
    filter.limit = limit;
    filter.offset = offset;

    auto result = discussion_repo::list_discussions(pool, filter);

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : result.items) {
        items.push_back(serialize_discussion(item));
    }

    send_success(res, nlohmann::json{
        {"items",  std::move(items)},
        {"total",  result.total},
        {"limit",  limit},
        {"offset", offset},
    });
}

// POST /api/v1/discussions (创建全局讨论)
inline void handle_create_global_discussion(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

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
    if (content.empty() || content.size() > 10000) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content must be 1-10000 chars");
        return;
    }

    int user_id = std::stoi(claims.user_id);

    DiscussionRow d;
    d.user_id  = user_id;
    d.title    = title;
    d.content  = content;
    // problem_id = nullopt → 全局讨论

    int id = discussion_repo::create(pool, d);

    send_success(res, nlohmann::json{{"id", id}});
}

// GET /api/v1/discussions/:id
inline void handle_get_discussion(
    httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {

    int discussion_id = 0;
    if (req.matches.size() > 1) {
        try { discussion_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (discussion_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid discussion id");
        return;
    }

    auto disc_opt = discussion_repo::find_by_id(pool, discussion_id);
    if (!disc_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "discussion not found");
        return;
    }

    // 获取顶层讨论信息
    DiscussionListRow row;
    row.discussion = *disc_opt;

    // 获取作者信息
    auto conn = pool.acquire();
    auto user_row = conn.fetch_one(
        "SELECT id, username, avatar FROM users WHERE id = ? LIMIT 1",
        disc_opt->user_id);
    if (user_row) {
        row.user.id = static_cast<int>((*user_row)[0].get<std::int64_t>());
        row.user.username = (*user_row)[1].get<std::string>();
        const auto& av = (*user_row)[2];
        if (!av.isNull()) row.user.avatar = av.get<std::string>();
    }

    // 获取 problem 信息
    if (disc_opt->problem_id) {
        auto prob_row = conn.fetch_one(
            "SELECT slug, title FROM problems WHERE id = ? AND is_deleted = FALSE LIMIT 1",
            *disc_opt->problem_id);
        if (prob_row) {
            row.problem_slug  = (*prob_row)[0].get<std::string>();
            row.problem_title = (*prob_row)[1].get<std::string>();
        }
    }

    // 获取回复
    auto replies = discussion_repo::list_replies(pool, discussion_id);
    nlohmann::json replies_json = nlohmann::json::array();
    for (const auto& r : replies) {
        replies_json.push_back(serialize_discussion(r));
    }

    nlohmann::json result = serialize_discussion(row);
    result["replies"] = std::move(replies_json);

    send_success(res, result);
}

// POST /api/v1/discussions/:id/replies
inline void handle_reply_discussion(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    // 取 matches[1] = (\d+) 数字 id;req.path.rfind('/') 会拿到 "/replies"
    // 或 "/like",不是数字,stoi 抛异常 → 兜底为 0 → "invalid id" 400。
    int discussion_id = 0;
    if (req.matches.size() > 1) {
        try { discussion_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (discussion_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid discussion id");
        return;
    }

    auto parent_opt = discussion_repo::find_by_id(pool, discussion_id);
    if (!parent_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "discussion not found");
        return;
    }

    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (!body.is_object()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid body");
        return;
    }

    auto content_it = body.find("content");
    if (content_it == body.end()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content required");
        return;
    }

    std::string content = content_it->get<std::string>();
    if (content.empty() || content.size() > 10000) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content must be 1-10000 chars");
        return;
    }

    int user_id = std::stoi(claims.user_id);

    // 确定 root_id（顶层讨论）
    int root_id = parent_opt->root_id.has_value() ? *parent_opt->root_id : parent_opt->id;

    DiscussionRow d;
    d.user_id    = user_id;
    d.problem_id = parent_opt->problem_id;
    d.parent_id  = discussion_id;
    d.root_id    = root_id;
    d.content    = content;

    int id = discussion_repo::create(pool, d);
    // v1.3.5 PR 13: 计数永远记在顶层讨论;嵌套回复时递增 root_id(不是 parent_id),
    // 这样删除任意位置的回复都能正确回滚同一根讨论的 reply_count。
    discussion_repo::increment_reply_count(pool, root_id);

    // Phase 7 ★ 通知被回复者
    // 注意：简化处理，通知发送给被回复的讨论/回复的作者
    // 不通知自己（已在 notify_discussion_reply 内部过滤）
    // 这里只通知顶层讨论的作者；实际可扩展为逐层通知
    if (parent_opt->user_id != user_id) {
        // 查找回复者用户名
        auto conn = pool.acquire();
        auto user_row = conn.fetch_one(
            "SELECT username FROM users WHERE id = ? LIMIT 1", user_id);
        std::string replier_username = user_row
            ? (*user_row)[0].get<std::string>()
            : "某用户";

        notify_discussion_reply(pool, parent_opt->user_id, user_id,
            discussion_id, replier_username,
            parent_opt->title ? *parent_opt->title : parent_opt->content);
    }

    send_success(res, nlohmann::json{{"id", id}});
}

// POST /api/v1/discussions/:id/like
inline void handle_like_discussion(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    int discussion_id = 0;
    if (req.matches.size() > 1) {
        try { discussion_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (discussion_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid discussion id");
        return;
    }

    auto disc_opt = discussion_repo::find_by_id(pool, discussion_id);
    if (!disc_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "discussion not found");
        return;
    }

    int user_id = std::stoi(claims.user_id);
    bool liked = discussion_repo::toggle_like(pool, discussion_id, user_id);

    auto updated = discussion_repo::find_by_id(pool, discussion_id);

    send_success(res, nlohmann::json{
        {"liked",      liked},
        {"like_count", updated ? updated->like_count : 0},
    });
}

// DELETE /api/v1/discussions/:id
inline void handle_delete_discussion(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    int discussion_id = 0;
    if (req.matches.size() > 1) {
        try { discussion_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (discussion_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid discussion id");
        return;
    }

    auto disc_opt = discussion_repo::find_by_id(pool, discussion_id);
    if (!disc_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "discussion not found");
        return;
    }

    int user_id = std::stoi(claims.user_id);
    int author_id = disc_opt->user_id;
    bool is_admin = (claims.role == "admin");

    // 只有作者或管理员可以删除
    if (user_id != author_id && !is_admin) {
        send_error(res, 403, ErrorCode::FORBIDDEN, "not authorized to delete this discussion");
        return;
    }

    discussion_repo::soft_delete(pool, discussion_id);
    // v1.3.5 PR 13: 当被删除的是回复(parent_id 有值),需要回滚根讨论的 reply_count。
    // 顶层讨论自身没有 root_id,不需要递减自己。
    if (disc_opt->parent_id.has_value()) {
        int root_id = disc_opt->root_id.has_value()
            ? *disc_opt->root_id
            : *disc_opt->parent_id;
        discussion_repo::decrement_reply_count(pool, root_id);
    }
    send_success(res, nlohmann::json{{"deleted", true}});
}

// GET /api/v1/discussions/count/:problem_id — 获取题目讨论数
inline void handle_count_problem_discussions(
    httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {

    int problem_id = 0;
    {
        const auto& path = req.path;
        auto last_slash = path.rfind('/');
        if (last_slash != std::string::npos) {
            try { problem_id = std::stoi(path.substr(last_slash + 1)); } catch (...) {}
        }
    }
    if (problem_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid problem id");
        return;
    }

    int count = discussion_repo::count_for_problem(pool, problem_id);
    send_success(res, nlohmann::json{{"count", count}});
}

// POST /api/v1/discussions/upload-image
// v1.3.5 PR 13: 讨论/题解发布框需要上传图片。头像上传走 /auth/avatar(路径写死
// {user_id},多人冲突),所以为讨论另起一个 endpoint,UUID 命名各图独立文件名。
//
// multipart/form-data,字段名:"image"
// 限制:≤ 2 MB,只允许 JPEG / PNG / GIF(87a/89a) / WEBP(magic bytes)
// 路径:/app/uploads/discussion_images/{uuid}.{ext}
// 静态服务通过 main.cpp::server.mount("/uploads", "/app/uploads") 提供
// 返回:{"url": "/uploads/discussion_images/{uuid}.jpg"}
inline void handle_upload_discussion_image(
    httplib::Response& res, const httplib::Request& req,
    const JwtConfig& jwt_cfg) {

    // 鉴权:JWT bearer / cookie 均可(同其他讨论端点)
    auto claims = require_authentication(req, jwt_cfg);

    int user_id = 0;
    try {
        user_id = std::stoi(std::string(claims.user_id));
    } catch (const std::exception&) {
        send_error(res, 401, ErrorCode::UNAUTHORIZED, "invalid sub claim");
        return;
    }
    if (user_id <= 0) {
        send_error(res, 401, ErrorCode::UNAUTHORIZED, "invalid sub claim");
        return;
    }

    // 校验 multipart field
    const auto it = req.files.find("image");
    if (it == req.files.end()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "missing 'image' field in multipart body",
                   {{"expected_field", "image"}});
        return;
    }

    const auto& file = it->second;
    const std::string& bytes = file.content;

    constexpr std::size_t kMaxImageBytes = 2u * 1024u * 1024u;
    if (bytes.empty()) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "image file is empty");
        return;
    }
    if (bytes.size() > kMaxImageBytes) {
        send_error(res, 413, ErrorCode::INVALID_INPUT,
                   "image too large (max 2 MB)",
                   {{"max_bytes", static_cast<std::uint64_t>(kMaxImageBytes)},
                    {"got_bytes", static_cast<std::uint64_t>(bytes.size())}});
        return;
    }

    // magic bytes 校验:扩展名以 magic bytes 决定,不信任客户端 content_type / filename
    const auto byte_at = [&bytes](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };

    std::string ext;
    if (bytes.size() >= 3 &&
        byte_at(0) == 0xFF && byte_at(1) == 0xD8 && byte_at(2) == 0xFF) {
        ext = ".jpg";
    } else if (bytes.size() >= 8 &&
        byte_at(0) == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G' &&
        byte_at(4) == 0x0D && byte_at(5) == 0x0A &&
        byte_at(6) == 0x1A && byte_at(7) == 0x0A) {
        ext = ".png";
    } else if (bytes.size() >= 6 &&
        bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
        bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9') &&
        bytes[5] == 'a') {
        ext = ".gif";
    } else if (bytes.size() >= 12 &&
        bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' &&
        bytes[3] == 'F' &&
        bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' &&
        bytes[11] == 'P') {
        ext = ".webp";
    } else {
        send_error(res, 400, ErrorCode::INVALID_INPUT,
                   "image must be JPEG, PNG, GIF, or WEBP "
                   "(magic bytes mismatch)",
                   {{"content_type", file.content_type}});
        return;
    }

    const std::string id = generate_uuid_v4();
    const std::string directory = "/app/uploads/discussion_images";
    const std::string filename = id + ext;
    const std::string path = directory + "/" + filename;
    const std::string url  = "/uploads/discussion_images/" + filename;

    try {
        std::filesystem::create_directories(directory);

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                       "failed to open discussion image file");
            return;
        }

        output.write(bytes.data(),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();

        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                       "failed to write discussion image file");
            return;
        }
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        LOG_ERROR("discussion image upload failed",
                  {{"user_id", std::to_string(user_id)},
                   {"reason", e.what()}});
        send_error(res, 500, ErrorCode::INTERNAL_ERROR,
                   "failed to store discussion image");
        return;
    }

    LOG_INFO("discussion image uploaded",
             {{"user_id", std::to_string(user_id)},
              {"size", std::to_string(bytes.size())},
              {"ext", ext},
              {"file_id", id}});

    send_success(res, nlohmann::json{{"url", url}});
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_discussion_routes(
    HttpServer&                server,
    ConnectionPool&            pool,
    RateLimiter&              /*limiter*/,
    const RateLimitConfig&    /*rate_cfg*/,
    const JwtConfig&           jwt_cfg) {

    // 题目下讨论列表
    server.get(R"(/api/v1/problems/([^/]+)/discussions)",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_list_problem_discussions(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("list_problem_discussions: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 发布题目讨论
    server.post(R"(/api/v1/problems/([^/]+)/discussions)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_create_problem_discussion(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("create_problem_discussion: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 全局讨论列表
    server.get(R"(/api/v1/discussions)",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_list_global_discussions(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("list_global_discussions: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 创建全局讨论
    server.post(R"(/api/v1/discussions)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_create_global_discussion(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("create_global_discussion: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // v1.3.5 PR 13: 讨论图片上传端点。
    // 必须注册在 /discussions/(\d+) 之前,虽然 "upload-image" 不会和数字 ID 冲突,
    // 但保持语义上「动作类路由」前置的惯例,更易阅读。
    server.post(R"(/api/v1/discussions/upload-image)",
        [&jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_upload_discussion_image(res, req, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("upload_discussion_image: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 讨论详情（含回复）
    server.get(R"(/api/v1/discussions/(\d+))",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_get_discussion(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("get_discussion: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 回复讨论
    server.post(R"(/api/v1/discussions/(\d+)/replies)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_reply_discussion(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("reply_discussion: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 点赞/取消点赞
    server.post(R"(/api/v1/discussions/(\d+)/like)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_like_discussion(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("like_discussion: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 删除讨论
    server.del(R"(/api/v1/discussions/(\d+))",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_delete_discussion(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("delete_discussion: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 题目讨论数
    server.get(R"(/api/v1/discussions/count/(\d+))",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_count_problem_discussions(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("count_problem_discussions: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    return server;
}

} // namespace litecode
