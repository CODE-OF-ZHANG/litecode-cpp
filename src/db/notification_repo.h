// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — notification repository
//
// 通知数据访问层

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"
#include "connection_pool.h"

namespace litecode {

struct NotificationRow {
    int         id          = 0;
    int         user_id     = 0;
    std::string type;
    std::string message;
    std::optional<std::string> link;
    std::optional<int>    reference_id;
    bool        is_read     = false;
    std::string created_at;
};

class NotificationRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace notification_detail {

inline std::optional<std::string> opt_string(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return v.get<std::string>(); }
    catch (const std::exception&) { return std::nullopt; }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return static_cast<int>(v.get<std::int64_t>()); }
    catch (const std::exception&) { return std::nullopt; }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return static_cast<int>(row[idx].get<std::int64_t>()); }
    catch (const std::exception& e) {
        throw NotificationRepoError(std::string("notification_repo: required field '") +
                                   std::string(field) + "' is not an int: " + e.what());
    }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return row[idx].get<std::string>(); }
    catch (const std::exception& e) {
        throw NotificationRepoError(std::string("notification_repo: required field '") +
                                   std::string(field) + "' is not a string: " + e.what());
    }
}

inline NotificationRow row_to_notification(const mysqlx::Row& row) {
    NotificationRow n;
    n.id           = req_int(row, 0, "id");
    n.user_id      = req_int(row, 1, "user_id");
    n.type         = req_string(row, 2, "type");
    n.message      = req_string(row, 3, "message");
    n.link         = opt_string(row, 4);
    n.reference_id = opt_int(row, 5);
    n.is_read      = row[6].get<bool>();
    n.created_at   = req_string(row, 7, "created_at");
    return n;
}

} // namespace notification_detail

namespace notification_repo {

// create — 插入新通知，返回通知 id
inline int create(ConnectionPool& pool, const NotificationRow& n) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "INSERT INTO notifications (user_id, type, message, link, reference_id) "
            "VALUES (?, ?, ?, ?, ?)",
            n.user_id, n.type, n.message,
            n.link.has_value() ? *n.link : "",
            n.reference_id.has_value() ? *n.reference_id : 0);
        return static_cast<int>(rs.getAutoIncrementValue());
    } catch (const mysqlx::Error& e) {
        throw NotificationRepoError(std::string("notification_repo::create: ") + e.what());
    }
}

// list_for_user — 获取用户通知列表
inline std::vector<NotificationRow> list_for_user(ConnectionPool& pool, int user_id, int limit, int offset) {
    std::vector<NotificationRow> result;
    auto conn = pool.acquire();
    auto rs = conn.execute(
        "SELECT id, user_id, type, message, link, reference_id, is_read, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
        "FROM notifications "
        "WHERE user_id = ? "
        "ORDER BY created_at DESC "
        "LIMIT ? OFFSET ?",
        user_id, static_cast<std::int64_t>(limit), static_cast<std::int64_t>(offset));
    for (auto row : rs) {
        try { result.push_back(notification_detail::row_to_notification(row)); }
        catch (...) {}
    }
    return result;
}

// count_unread — 未读数量
inline int count_unread(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    auto row = conn.fetch_scalar<int>(
        "SELECT COUNT(*) FROM notifications WHERE user_id = ? AND is_read = FALSE",
        user_id);
    return row.value_or(0);
}

// mark_read — 标记已读
inline void mark_read(ConnectionPool& pool, int user_id, int notification_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE notifications SET is_read = TRUE WHERE id = ? AND user_id = ?",
        notification_id, user_id);
}

// mark_all_read — 全部已读
inline void mark_all_read(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE notifications SET is_read = TRUE WHERE user_id = ? AND is_read = FALSE",
        user_id);
}

} // namespace notification_repo
} // namespace litecode
