// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — discussion repository
//
// 讨论数据访问层

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"
#include "connection_pool.h"

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Types
// ────────────────────────────────────────────────────────────────────────────

struct DiscussionRow {
    int         id          = 0;
    int         user_id     = 0;
    std::optional<int> problem_id;   // NULL = 全局讨论
    std::optional<int> parent_id;    // NULL = 顶层
    std::optional<int> root_id;     // 指向顶层讨论 id
    std::optional<std::string> title;  // 全局讨论需要标题
    std::string content;
    int         like_count  = 0;
    int         reply_count = 0;
    std::string created_at;
    std::string updated_at;
    bool        is_deleted  = false;
};

struct DiscussionUserInfo {
    int         id       = 0;
    std::string username;
    std::optional<std::string> avatar;
};

struct DiscussionListRow {
    DiscussionRow      discussion;
    DiscussionUserInfo user;
    std::optional<std::string> problem_slug;   // 题目 slug（关联题目时）
    std::optional<std::string> problem_title;
};

// ────────────────────────────────────────────────────────────────────────────
//  Error
// ────────────────────────────────────────────────────────────────────────────

class DiscussionRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
// ────────────────────────────────────────────────────────────────────────────

namespace discussion_detail {

inline std::optional<std::string> opt_string(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return v.get<std::string>(); }
    catch (const std::exception&) { return std::nullopt;
    }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return static_cast<int>(v.get<std::int64_t>()); }
    catch (const std::exception&) { return std::nullopt; }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return row[idx].get<std::string>(); }
    catch (const std::exception& e) {
        throw DiscussionRepoError(std::string("discussion_repo: required field '") +
                                  std::string(field) + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return static_cast<int>(row[idx].get<std::int64_t>()); }
    catch (const std::exception& e) {
        throw DiscussionRepoError(std::string("discussion_repo: required field '") +
                                  std::string(field) + "' is not an int: " + e.what());
    }
}

inline DiscussionRow row_to_discussion(const mysqlx::Row& row) {
    DiscussionRow d;
    d.id          = req_int(row, 0, "id");
    d.user_id     = req_int(row, 1, "user_id");
    d.problem_id  = opt_int(row, 2);
    d.parent_id   = opt_int(row, 3);
    d.root_id     = opt_int(row, 4);
    d.title       = opt_string(row, 5);
    d.content     = req_string(row, 6, "content");
    d.like_count  = req_int(row, 7, "like_count");
    d.reply_count = req_int(row, 8, "reply_count");
    d.created_at  = req_string(row, 9, "created_at");
    d.updated_at  = req_string(row, 10, "updated_at");
    d.is_deleted  = row[11].get<bool>();
    return d;
}

} // namespace discussion_detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace discussion_repo {

// create — 插入新讨论，返回新讨论 id
inline int create(ConnectionPool& pool, const DiscussionRow& d) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "INSERT INTO discussions (user_id, problem_id, parent_id, root_id, title, content, like_count, reply_count) "
            "VALUES (?, ?, ?, ?, ?, ?, 0, 0)",
            d.user_id,
            d.problem_id.has_value() ? mysqlx::Value(*d.problem_id) : mysqlx::Value(nullptr),
            d.parent_id.has_value() ? mysqlx::Value(*d.parent_id) : mysqlx::Value(nullptr),
            d.root_id.has_value() ? mysqlx::Value(*d.root_id) : mysqlx::Value(nullptr),
            d.title.has_value() ? mysqlx::Value(*d.title) : mysqlx::Value(nullptr),
            d.content);
        return static_cast<int>(rs.getAutoIncrementValue());
    } catch (const mysqlx::Error& e) {
        throw DiscussionRepoError(std::string("discussion_repo::create: ") + e.what());
    }
}

// find_by_id — 通过 id 查找讨论
inline std::optional<DiscussionRow> find_by_id(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT id, user_id, problem_id, parent_id, root_id, title, content, "
        "       like_count, reply_count, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
        "       DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s'), "
        "       is_deleted "
        "FROM discussions WHERE id = ? AND is_deleted = FALSE LIMIT 1",
        id);
    if (!row) return std::nullopt;
    return discussion_detail::row_to_discussion(*row);
}

// list_for_problem — 获取题目的讨论列表（仅顶层）
struct DiscussionListFilter {
    std::optional<int> problem_id;   // 有值 = 题目讨论，null = 全局讨论
    int                 limit        = 20;
    int                 offset       = 0;
    std::optional<int>  user_id;     // 筛选特定用户
};

struct DiscussionListResult {
    std::vector<DiscussionListRow> items;
    int total = 0;
};

inline DiscussionListResult list_discussions(ConnectionPool& pool,
                                            const DiscussionListFilter& filter) {
    DiscussionListResult result;
    auto conn = pool.acquire();

    // COUNT — 只统计顶层讨论（parent_id IS NULL）
    std::string count_sql =
        "SELECT COUNT(*) FROM discussions "
        "WHERE is_deleted = FALSE AND parent_id IS NULL";
    if (filter.problem_id.has_value()) {
        count_sql += " AND problem_id = ?";
    } else {
        count_sql += " AND problem_id IS NULL";
    }
    if (filter.user_id.has_value()) count_sql += " AND user_id = ?";

    mysqlx::SqlResult count_rs;
    if (filter.problem_id.has_value() && filter.user_id.has_value()) {
        count_rs = conn.execute(count_sql, *filter.problem_id, *filter.user_id);
    } else if (filter.problem_id.has_value()) {
        count_rs = conn.execute(count_sql, *filter.problem_id);
    } else if (filter.user_id.has_value()) {
        count_rs = conn.execute(count_sql, *filter.user_id);
    } else {
        count_rs = conn.execute(count_sql);
    }
    for (auto row : count_rs) {
        result.total = static_cast<int>(row[0].get<std::int64_t>());
    }

    // SELECT — 顶层讨论 + 作者信息
    std::string sql =
        "SELECT d.id, d.user_id, d.problem_id, d.parent_id, d.root_id, d.title, d.content, "
        "       d.like_count, d.reply_count, "
        "       DATE_FORMAT(d.created_at, '%Y-%m-%d %H:%i:%s'), "
        "       DATE_FORMAT(d.updated_at, '%Y-%m-%d %H:%i:%s'), "
        "       d.is_deleted, "
        "       u.id, u.username, u.avatar, "
        "       p.slug, p.title "
        "FROM discussions d "
        "JOIN users u ON u.id = d.user_id "
        "LEFT JOIN problems p ON p.id = d.problem_id AND p.is_deleted = FALSE "
        "WHERE d.is_deleted = FALSE AND d.parent_id IS NULL";
    if (filter.problem_id.has_value()) {
        sql += " AND d.problem_id = ?";
    } else {
        sql += " AND d.problem_id IS NULL";
    }
    if (filter.user_id.has_value()) sql += " AND d.user_id = ?";
    sql += " ORDER BY d.created_at DESC LIMIT ? OFFSET ?";

    mysqlx::SqlResult rs;
    if (filter.problem_id.has_value() && filter.user_id.has_value()) {
        rs = conn.execute(sql, *filter.problem_id, *filter.user_id,
            static_cast<std::int64_t>(filter.limit),
            static_cast<std::int64_t>(filter.offset));
    } else if (filter.problem_id.has_value()) {
        rs = conn.execute(sql, *filter.problem_id,
            static_cast<std::int64_t>(filter.limit),
            static_cast<std::int64_t>(filter.offset));
    } else if (filter.user_id.has_value()) {
        rs = conn.execute(sql, *filter.user_id,
            static_cast<std::int64_t>(filter.limit),
            static_cast<std::int64_t>(filter.offset));
    } else {
        rs = conn.execute(sql,
            static_cast<std::int64_t>(filter.limit),
            static_cast<std::int64_t>(filter.offset));
    }
    for (auto row : rs) {
        try {
            DiscussionListRow item;
            item.discussion = discussion_detail::row_to_discussion(row);
            // user 字段从偏移 12 开始
            item.user.id       = static_cast<int>(row[12].get<std::int64_t>());
            item.user.username  = row[13].get<std::string>();
            item.user.avatar    = discussion_detail::opt_string(row, 14);
            item.problem_slug   = discussion_detail::opt_string(row, 15);
            item.problem_title  = discussion_detail::opt_string(row, 16);
            result.items.push_back(std::move(item));
        } catch (const std::exception&) {
            // skip malformed
        }
    }
    return result;
}

// list_replies — 获取某讨论的回复列表
inline std::vector<DiscussionListRow> list_replies(ConnectionPool& pool, int root_id) {
    std::vector<DiscussionListRow> result;
    auto conn = pool.acquire();

    auto rs = conn.execute(
        "SELECT d.id, d.user_id, d.problem_id, d.parent_id, d.root_id, d.title, d.content, "
        "       d.like_count, d.reply_count, "
        "       DATE_FORMAT(d.created_at, '%Y-%m-%d %H:%i:%s'), "
        "       DATE_FORMAT(d.updated_at, '%Y-%m-%d %H:%i:%s'), "
        "       d.is_deleted, "
        "       u.id, u.username, u.avatar, "
        "       p.slug, p.title "
        "FROM discussions d "
        "JOIN users u ON u.id = d.user_id "
        "LEFT JOIN problems p ON p.id = d.problem_id AND p.is_deleted = FALSE "
        "WHERE d.is_deleted = FALSE AND d.root_id = ? "
        "ORDER BY d.created_at ASC",
        root_id);

    for (auto row : rs) {
        try {
            DiscussionListRow item;
            item.discussion = discussion_detail::row_to_discussion(row);
            item.user.id       = static_cast<int>(row[12].get<std::int64_t>());
            item.user.username  = row[13].get<std::string>();
            item.user.avatar    = discussion_detail::opt_string(row, 14);
            item.problem_slug   = discussion_detail::opt_string(row, 15);
            item.problem_title  = discussion_detail::opt_string(row, 16);
            result.push_back(std::move(item));
        } catch (const std::exception&) {}
    }
    return result;
}

// count_for_problem — 获取题目讨论总数（顶层）
inline int count_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT COUNT(*) FROM discussions WHERE problem_id = ? AND is_deleted = FALSE AND parent_id IS NULL",
        problem_id);
    return row.value_or(0);
}

// toggle_like — 点赞/取消点赞，返回 true = 新增点赞
inline bool toggle_like(ConnectionPool& pool, int discussion_id, int user_id) {
    auto conn = pool.acquire();

    const auto existing = conn.fetch_scalar<int>(
        "SELECT 1 FROM discussion_likes WHERE user_id = ? AND discussion_id = ? LIMIT 1",
        user_id, discussion_id);

    if (existing.has_value()) {
        conn.execute(
            "DELETE FROM discussion_likes WHERE user_id = ? AND discussion_id = ?",
            user_id, discussion_id);
        conn.execute(
            "UPDATE discussions SET like_count = GREATEST(0, like_count - 1) WHERE id = ?",
            discussion_id);
        return false;
    } else {
        conn.execute(
            "INSERT INTO discussion_likes (user_id, discussion_id) VALUES (?, ?)",
            user_id, discussion_id);
        conn.execute(
            "UPDATE discussions SET like_count = like_count + 1 WHERE id = ?",
            discussion_id);
        return true;
    }
}

// has_user_liked
inline bool has_user_liked(ConnectionPool& pool, int discussion_id, int user_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM discussion_likes WHERE user_id = ? AND discussion_id = ? LIMIT 1",
        user_id, discussion_id);
    return row.has_value();
}

// increment_reply_count — 回复数 +1
inline void increment_reply_count(ConnectionPool& pool, int discussion_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE discussions SET reply_count = reply_count + 1 WHERE id = ?",
        discussion_id);
}

// find_problem_id_by_slug
inline std::optional<int> find_problem_id_by_slug(ConnectionPool& pool, const std::string& slug) {
    auto conn = pool.acquire();
    auto row = conn.fetch_scalar<int>(
        "SELECT id FROM problems WHERE slug = ? AND is_deleted = FALSE LIMIT 1",
        slug);
    return row;
}

// soft_delete — 软删除讨论（将 is_deleted 设为 true）
inline void soft_delete(ConnectionPool& pool, int discussion_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE discussions SET is_deleted = TRUE WHERE id = ?",
        discussion_id);
}

} // namespace discussion_repo
} // namespace litecode
