// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — solution comment repository (V021 + V022)
//
// V022 扩展:
//   * like_count 反范式列(避免 LIKE 计数 N+1)
//   * parent_id 自引用(扁平 + 引用提示的回复关系)
//   * solution_comment_likes 关联表(V022 新建)

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

// V022: 新增 like_count + parent_id
struct SolutionCommentRow {
    int         id          = 0;
    int         solution_id = 0;
    int         user_id     = 0;
    std::optional<int> parent_id;          // V022: NULL 表示顶层评论
    std::string content;
    int         like_count  = 0;           // V022: 反范式
    std::string created_at;
    bool        is_deleted  = false;
};

struct SolutionCommentUserInfo {
    int                          id       = 0;
    std::string                  username;
    std::optional<std::string>   avatar;
};

struct SolutionCommentListRow {
    SolutionCommentRow      comment;
    SolutionCommentUserInfo user;
};

// ────────────────────────────────────────────────────────────────────────────
//  Error
// ────────────────────────────────────────────────────────────────────────────

class SolutionCommentRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
// ────────────────────────────────────────────────────────────────────────────

namespace solution_comment_detail {

inline std::optional<std::string> opt_string(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return v.get<std::string>(); }
    catch (const std::exception&) { return std::nullopt; }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return row[idx].get<std::string>(); }
    catch (const std::exception& e) {
        throw SolutionCommentRepoError(std::string("solution_comment_repo: required field '") +
                                       std::string(field) + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return static_cast<int>(row[idx].get<std::int64_t>()); }
    catch (const std::exception& e) {
        throw SolutionCommentRepoError(std::string("solution_comment_repo: required field '") +
                                       std::string(field) + "' is not an int: " + e.what());
    }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return static_cast<int>(v.get<std::int64_t>()); }
    catch (const std::exception&) { return std::nullopt; }
}

// V022: 列偏移顺序
//   0 id, 1 solution_id, 2 user_id, 3 parent_id, 4 content, 5 like_count,
//   6 created_at, 7 is_deleted, 8 u.id, 9 u.username, 10 u.avatar
inline SolutionCommentRow row_to_solution_comment(const mysqlx::Row& row) {
    SolutionCommentRow c;
    c.id          = req_int(row, 0, "id");
    c.solution_id = req_int(row, 1, "solution_id");
    c.user_id     = req_int(row, 2, "user_id");
    c.parent_id   = opt_int(row, 3);            // V022
    c.content     = req_string(row, 4, "content");
    c.like_count  = req_int(row, 5, "like_count"); // V022
    c.created_at  = req_string(row, 6, "created_at");
    c.is_deleted  = row[7].get<bool>();
    return c;
}

} // namespace solution_comment_detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace solution_comment_repo {

// create — 插入新评论（V022 支持 parent_id；传入 nullopt 为顶层评论）
inline int create(ConnectionPool& pool, const SolutionCommentRow& c) {
    auto conn = pool.acquire();
    try {
        mysqlx::SqlStatement stmt = conn.session().sql(
            "INSERT INTO solution_comments (solution_id, user_id, parent_id, content) "
            "VALUES (?, ?, ?, ?)");
        stmt.bind(c.solution_id, c.user_id);
        if (c.parent_id.has_value()) {
            stmt.bind(*c.parent_id);
        } else {
            stmt.bind(nullptr);
        }
        stmt.bind(c.content);
        auto rs = stmt.execute();
        return static_cast<int>(rs.getAutoIncrementValue());
    } catch (const mysqlx::Error& e) {
        throw SolutionCommentRepoError(std::string("solution_comment_repo::create: ") + e.what());
    }
}

// find_by_id — 单条查询（含 parent_id / like_count）
inline std::optional<SolutionCommentRow> find_by_id(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT id, solution_id, user_id, parent_id, content, like_count, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
        "       is_deleted "
        "FROM solution_comments WHERE id = ? AND is_deleted = FALSE LIMIT 1",
        id);
    if (!row) return std::nullopt;
    return solution_comment_detail::row_to_solution_comment(*row);
}

// list_for_solution — 某题解的全部评论（时间正序，最早在前；扁平）
inline std::vector<SolutionCommentListRow> list_for_solution(
        ConnectionPool& pool, int solution_id, int limit, int offset) {
    std::vector<SolutionCommentListRow> result;
    if (limit <= 0) limit = 50;
    if (limit > 200) limit = 200;
    if (offset < 0) offset = 0;

    auto conn = pool.acquire();
    auto rs = conn.execute(
        "SELECT c.id, c.solution_id, c.user_id, c.parent_id, c.content, c.like_count, "
        "       DATE_FORMAT(c.created_at, '%Y-%m-%d %H:%i:%s'), "
        "       c.is_deleted, "
        "       u.id, u.username, u.avatar "
        "FROM solution_comments c "
        "JOIN users u ON u.id = c.user_id "
        "WHERE c.solution_id = ? AND c.is_deleted = FALSE "
        "ORDER BY c.created_at ASC, c.id ASC "
        "LIMIT ? OFFSET ?",
        solution_id,
        static_cast<std::int64_t>(limit),
        static_cast<std::int64_t>(offset));

    for (auto row : rs) {
        try {
            SolutionCommentListRow item;
            item.comment = solution_comment_detail::row_to_solution_comment(row);
            // 偏移 8 起为 user 字段
            item.user.id       = static_cast<int>(row[8].get<std::int64_t>());
            item.user.username = row[9].get<std::string>();
            item.user.avatar   = solution_comment_detail::opt_string(row, 10);
            result.push_back(std::move(item));
        } catch (const std::exception&) {
            // skip malformed
        }
    }
    return result;
}

// count_for_solution — 题解评论数（用于分页 / 校验）
inline int count_for_solution(ConnectionPool& pool, int solution_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT COUNT(*) FROM solution_comments "
        "WHERE solution_id = ? AND is_deleted = FALSE",
        solution_id);
    return row.value_or(0);
}

// increment_comment_count — solutions.comment_count += 1
inline void increment_comment_count(ConnectionPool& pool, int solution_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE solutions SET comment_count = comment_count + 1 WHERE id = ?",
        solution_id);
}

// decrement_comment_count — 用 GREATEST 防御减到负
inline void decrement_comment_count(ConnectionPool& pool, int solution_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE solutions SET comment_count = GREATEST(0, comment_count - 1) WHERE id = ?",
        solution_id);
}

// soft_delete — is_deleted = TRUE
inline void soft_delete(ConnectionPool& pool, int comment_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE solution_comments SET is_deleted = TRUE WHERE id = ?",
        comment_id);
}

// ────────────────────────────────────────────────────────────────────────────
//  V022 — 点赞相关
// ────────────────────────────────────────────────────────────────────────────

// has_user_liked — 单条评论是否被指定用户点赞
inline bool has_user_liked(ConnectionPool& pool, int comment_id, int user_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM solution_comment_likes WHERE comment_id = ? AND user_id = ? LIMIT 1",
        comment_id, user_id);
    return row.has_value();
}

// toggle_like — 点赞/取消点赞评论，返回 true = 新增
inline bool toggle_like(ConnectionPool& pool, int comment_id, int user_id) {
    auto conn = pool.acquire();

    const auto existing = conn.fetch_scalar<int>(
        "SELECT 1 FROM solution_comment_likes WHERE comment_id = ? AND user_id = ? LIMIT 1",
        comment_id, user_id);

    if (existing.has_value()) {
        conn.execute(
            "DELETE FROM solution_comment_likes WHERE comment_id = ? AND user_id = ?",
            comment_id, user_id);
        conn.execute(
            "UPDATE solution_comments SET like_count = GREATEST(0, like_count - 1) WHERE id = ?",
            comment_id);
        return false;
    } else {
        conn.execute(
            "INSERT INTO solution_comment_likes (comment_id, user_id) VALUES (?, ?)",
            comment_id, user_id);
        conn.execute(
            "UPDATE solution_comments SET like_count = like_count + 1 WHERE id = ?",
            comment_id);
        return true;
    }
}

} // namespace solution_comment_repo
} // namespace litecode
