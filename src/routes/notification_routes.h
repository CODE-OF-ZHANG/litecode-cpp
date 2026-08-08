// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — notification routes (Phase 7 ★)
//
// 通知系统 API + SSE 实时推送

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"
#include "../db/connection_pool.h"
#include "../db/notification_repo.h"
#include "../middleware/auth_middleware.h"
#include "../routes/error_handler.h"
#include "../server.h"

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  SSE notification broadcaster
//
//  Maintains a map of user_id → queue of pending notifications.
//  When a notification is created, it's pushed to the user's queue
//  and the SSE connection wakes up and sends it.
// ────────────────────────────────────────────────────────────────────────────

class NotificationBroadcaster {
public:
    // Per-user event queue
    struct UserChannel {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<std::string> events;   // serialized SSE frames
        bool disconnected = false;
    };

    void join(int user_id) {
        std::lock_guard<std::mutex> lock(全局_mutex_);
        auto& ch = channels_[user_id];
        std::lock_guard<std::mutex> lock2(ch.mtx);
        ch.disconnected = false;
    }

    void leave(int user_id) {
        std::lock_guard<std::mutex> lock(全局_mutex_);
        auto it = channels_.find(user_id);
        if (it != channels_.end()) {
            std::lock_guard<std::mutex> lock2(it->second.mtx);
            it->second.disconnected = true;
            it->second.cv.notify_all();
        }
    }

    // Push a notification to a user's queue (called from event handlers)
    void push(int user_id, const std::string& event_type, const nlohmann::json& data) {
        std::string frame = "event: " + event_type + "\ndata: " + data.dump() + "\n\n";
        std::lock_guard<std::mutex> lock(全局_mutex_);
        auto it = channels_.find(user_id);
        if (it != channels_.end()) {
            std::lock_guard<std::mutex> lock2(it->second.mtx);
            if (!it->second.disconnected) {
                it->second.events.push(frame);
                it->second.cv.notify_all();
            }
        }
    }

    // Pop the next frame for a user (blocks until available or disconnected)
    // Returns "" on disconnect.
    std::string pop(int user_id, std::chrono::seconds timeout) {
        std::lock_guard<std::mutex> lock(全局_mutex_);
        auto it = channels_.find(user_id);
        if (it == channels_.end()) return "";

        {
            std::lock_guard<std::mutex> lock2(it->second.mtx);
            if (!it->second.events.empty()) {
                auto frame = it->second.events.front();
                it->second.events.pop();
                return frame;
            }
            if (it->second.disconnected) return "";
        }

        // Wait with timeout
        std::unique_lock<std::mutex> lock2(it->second.mtx);
        it->second.cv.wait_for(lock2, timeout, [&]() {
            return it->second.disconnected || !it->second.events.empty();
        });
        if (it->second.disconnected) return "";
        if (!it->second.events.empty()) {
            auto frame = it->second.events.front();
            it->second.events.pop();
            return frame;
        }
        return "";   // timeout
    }

    static NotificationBroadcaster& instance() {
        static NotificationBroadcaster inst;
        return inst;
    }

private:
    std::mutex 全局_mutex_;
    std::map<int, UserChannel> channels_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Helpers
// ────────────────────────────────────────────────────────────────────────────

inline std::string make_sse_frame(const std::string& event, const nlohmann::json& data) {
    return "event: " + event + "\ndata: " + data.dump() + "\n\n";
}

// ────────────────────────────────────────────────────────────────────────────
//  Handlers
// ────────────────────────────────────────────────────────────────────────────

// GET /api/v1/notifications — list notifications
inline void handle_list_notifications(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);
    int user_id = std::stoi(claims.user_id);

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

    auto items = notification_repo::list_for_user(pool, user_id, limit, offset);
    auto unread_count = notification_repo::count_unread(pool, user_id);

    nlohmann::json items_json = nlohmann::json::array();
    for (const auto& n : items) {
        nlohmann::json j;
        j["id"] = n.id;
        j["type"] = n.type;
        j["message"] = n.message;
        if (n.link) j["link"] = *n.link;
        if (n.reference_id) j["reference_id"] = *n.reference_id;
        j["is_read"] = n.is_read;
        j["created_at"] = n.created_at;
        items_json.push_back(std::move(j));
    }

    send_success(res, nlohmann::json{
        {"items", std::move(items_json)},
        {"unread_count", unread_count},
        {"limit", limit},
        {"offset", offset},
    });
}

// POST /api/v1/notifications/:id/read — mark as read
inline void handle_mark_read(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);
    int user_id = std::stoi(claims.user_id);

    int notif_id = 0;
    {
        const auto& path = req.path;
        auto last_slash = path.rfind('/');
        if (last_slash != std::string::npos) {
            try { notif_id = std::stoi(path.substr(last_slash + 1)); } catch (...) {}
        }
    }
    if (notif_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid notification id");
        return;
    }

    notification_repo::mark_read(pool, user_id, notif_id);
    send_success(res, nlohmann::json{{"ok", true}});
}

// POST /api/v1/notifications/read-all — mark all as read
inline void handle_mark_all_read(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);
    int user_id = std::stoi(claims.user_id);

    notification_repo::mark_all_read(pool, user_id);
    send_success(res, nlohmann::json{{"ok", true}});
}

// GET /api/v1/notifications/unread-count
inline void handle_unread_count(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);
    int user_id = std::stoi(claims.user_id);

    int count = notification_repo::count_unread(pool, user_id);
    send_success(res, nlohmann::json{{"count", count}});
}

// GET /api/v1/notifications/stream — SSE real-time stream
inline void handle_notification_stream(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);
    int user_id = std::stoi(claims.user_id);

    auto& bc = NotificationBroadcaster::instance();
    bc.join(user_id);

    res.set_header("Content-Type", "text/event-stream; charset=utf-8");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_chunked_content_provider("text/event-stream",
        [&](size_t /*offset*/, httplib::DataSink& sink) {
            // Send a heartbeat every 30s to keep the connection alive
            auto frame = bc.pop(user_id, std::chrono::seconds(30));
            if (!frame.empty()) {
                sink.write(frame.data(), frame.size());
            } else {
                // heartbeat
                std::string ping = ": ping\n\n";
                sink.write(ping.data(), ping.size());
            }
            return true;   // keep-alive
        });

    bc.leave(user_id);
}

// ────────────────────────────────────────────────────────────────────────────
//  Route registration
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_notification_routes(
    HttpServer&                server,
    ConnectionPool&            pool,
    RateLimiter&              /*limiter*/,
    const RateLimitConfig&    /*rate_cfg*/,
    const JwtConfig&           jwt_cfg) {

    // List notifications
    server.get(R"(/api/v1/notifications)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_list_notifications(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("list_notifications: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // Unread count
    server.get(R"(/api/v1/notifications/unread-count)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_unread_count(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("unread_count: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // Mark one as read
    server.post(R"(/api/v1/notifications/(\d+)/read)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_mark_read(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("mark_read: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // Mark all as read
    server.post(R"(/api/v1/notifications/read-all)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_mark_all_read(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("mark_all_read: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // SSE real-time notification stream
    server.get(R"(/api/v1/notifications/stream)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_notification_stream(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("notification_stream: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    return server;
}

// ────────────────────────────────────────────────────────────────────────────
//  Trigger helpers — call these from route handlers to push notifications
// ────────────────────────────────────────────────────────────────────────────

// notify_discussion_reply — 通知被回复的讨论作者
inline void notify_discussion_reply(
    ConnectionPool& pool,
    int author_user_id,          // 被通知的人（讨论作者）
    int replier_user_id,         // 回复者（避免通知自己）
    int discussion_id,
    const std::string& replier_username,
    const std::string& discussion_title) {

    if (author_user_id == replier_user_id) return;   // 不通知自己

    NotificationRow n;
    n.user_id = author_user_id;
    n.type = "discussion_reply";
    n.message = replier_username + " 回复了你的讨论：「" +
                (discussion_title.size() > 50
                    ? discussion_title.substr(0, 50) + "..."
                    : discussion_title) + "」";
    n.link = "/discuss.html?id=" + std::to_string(discussion_id);
    n.reference_id = discussion_id;

    try { notification_repo::create(pool, n); } catch (...) {}

    // Push via SSE
    auto& bc = NotificationBroadcaster::instance();
    nlohmann::json payload = nlohmann::json{
        {"type", n.type},
        {"message", n.message},
    };
    if (n.link) payload["link"] = *n.link;
    if (n.reference_id) payload["reference_id"] = *n.reference_id;
    bc.push(author_user_id, "notification", payload);
}

// notify_solution_like — 通知被点赞的题解作者
inline void notify_solution_like(
    ConnectionPool& pool,
    int author_user_id,
    int liker_user_id,
    int solution_id,
    const std::string& liker_username,
    const std::string& solution_title) {

    if (author_user_id == liker_user_id) return;

    NotificationRow n;
    n.user_id = author_user_id;
    n.type = "solution_like";
    n.message = liker_username + " 点赞了你的题解：「" +
                (solution_title.size() > 50
                    ? solution_title.substr(0, 50) + "..."
                    : solution_title) + "」";
    n.link = "/solution.html?id=" + std::to_string(solution_id);
    n.reference_id = solution_id;

    try { notification_repo::create(pool, n); } catch (...) {}

    auto& bc = NotificationBroadcaster::instance();
    nlohmann::json payload = nlohmann::json{
        {"type", n.type},
        {"message", n.message},
    };
    if (n.link) payload["link"] = *n.link;
    if (n.reference_id) payload["reference_id"] = *n.reference_id;
    bc.push(author_user_id, "notification", payload);
}

// notify_solution_comment — 通知题解作者：有人评论了你的题解 (V021)
//
// 与 notify_solution_like 的差异：
//   - 多一个 comment_id 用于锚点 (#comment-N)，跳到 solution.html 直接定位
//   - 消息正文里带评论摘要（30 字），点开看到「为什么收到通知」
inline void notify_solution_comment(
    ConnectionPool& pool,
    int author_user_id,
    int commenter_user_id,
    int solution_id,
    int comment_id,
    const std::string& commenter_username,
    const std::string& solution_title,
    const std::string& comment_excerpt) {

    if (author_user_id == commenter_user_id) return;

    NotificationRow n;
    n.user_id = author_user_id;
    n.type = "solution_comment";

    std::string title_clip = solution_title.size() > 50
        ? solution_title.substr(0, 50) + "..."
        : solution_title;
    std::string preview = comment_excerpt;
    // 去除首尾空白，避免消息里出现孤立的换行/空格
    auto trim = [](std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        s = s.substr(a, b - a);
    };
    trim(preview);
    if (preview.size() > 30) preview = preview.substr(0, 30) + "...";

    n.message = commenter_username + " 评论了你的题解：「" + title_clip + "」: " + preview;
    n.link = "/solution.html?id=" + std::to_string(solution_id) +
             "#comment-" + std::to_string(comment_id);
    n.reference_id = solution_id;

    try { notification_repo::create(pool, n); } catch (...) {}

    auto& bc = NotificationBroadcaster::instance();
    nlohmann::json payload = nlohmann::json{
        {"type", n.type},
        {"message", n.message},
    };
    if (n.link) payload["link"] = *n.link;
    if (n.reference_id) payload["reference_id"] = *n.reference_id;
    bc.push(author_user_id, "notification", payload);
}

// V022 — notify_solution_comment_reply
// 通知被你回复的评论作者:V022 决策 = 嵌套回复不打扰题解作者,只通知被回复者。
// 自己的评论自己回 → 不通知(type=solution_comment_reply)
inline void notify_solution_comment_reply(
    ConnectionPool& pool,
    int target_user_id,
    int replier_user_id,
    int solution_id,
    int reply_comment_id,
    int parent_comment_id,
    const std::string& replier_username,
    const std::string& solution_title,
    const std::string& reply_excerpt) {

    if (target_user_id == replier_user_id) return;

    NotificationRow n;
    n.user_id = target_user_id;
    n.type = "solution_comment_reply";

    std::string title_clip = solution_title.size() > 50
        ? solution_title.substr(0, 50) + "..."
        : solution_title;
    std::string preview = reply_excerpt;
    auto trim = [](std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        s = s.substr(a, b - a);
    };
    trim(preview);
    if (preview.size() > 30) preview = preview.substr(0, 30) + "...";

    n.message = replier_username + " 回复了你的评论：「" + title_clip + "」: " + preview;
    n.link = "/solution.html?id=" + std::to_string(solution_id) +
             "#comment-" + std::to_string(reply_comment_id);
    n.reference_id = solution_id;

    try { notification_repo::create(pool, n); } catch (...) {}

    auto& bc = NotificationBroadcaster::instance();
    nlohmann::json payload = nlohmann::json{
        {"type", n.type},
        {"message", n.message},
    };
    if (n.link) payload["link"] = *n.link;
    if (n.reference_id) payload["reference_id"] = *n.reference_id;
    bc.push(target_user_id, "notification", payload);
}

} // namespace litecode
