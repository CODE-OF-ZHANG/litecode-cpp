// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — solution repository
//
// 题解数据访问层

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

struct SolutionRow {
    int         id         = 0;
    int         user_id     = 0;
    int         problem_id  = 0;
    std::string title;
    std::string content;
    int         like_count = 0;
    int         comment_count = 0;     // V021 — 题解评论数（反范式，避免 N+1）
    std::string created_at;
    std::string updated_at;
    bool        is_deleted = false;
};

struct SolutionUserInfo {
    int                          id       = 0;
    std::string                  username;
    std::optional<std::string>   avatar;
};

struct SolutionListRow {
    SolutionRow      solution;
    SolutionUserInfo user;
};

// ────────────────────────────────────────────────────────────────────────────
//  Error
// ────────────────────────────────────────────────────────────────────────────

class SolutionRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization
// ────────────────────────────────────────────────────────────────────────────

namespace solution_detail {

inline std::optional<std::string> opt_string(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return v.get<std::string>(); }
    catch (const std::exception&) { return std::nullopt; }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return row[idx].get<std::string>(); }
    catch (const std::exception& e) {
        throw SolutionRepoError(std::string("solution_repo: required field '") +
                               std::string(field) + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx, const char* field) {
    try { return static_cast<int>(row[idx].get<std::int64_t>()); }
    catch (const std::exception& e) {
        throw SolutionRepoError(std::string("solution_repo: required field '") +
                               std::string(field) + "' is not an int: " + e.what());
    }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try { return static_cast<int>(v.get<std::int64_t>()); }
    catch (const std::exception&) { return std::nullopt; }
}

inline SolutionRow row_to_solution(const mysqlx::Row& row) {
    SolutionRow s;
    s.id            = req_int(row, 0, "id");
    s.user_id        = req_int(row, 1, "user_id");
    s.problem_id     = req_int(row, 2, "problem_id");
    s.title          = req_string(row, 3, "title");
    s.content        = req_string(row, 4, "content");
    s.like_count     = req_int(row, 5, "like_count");
    s.comment_count  = req_int(row, 6, "comment_count");     // V021
    s.created_at     = req_string(row, 7, "created_at");
    s.updated_at     = req_string(row, 8, "updated_at");
    s.is_deleted     = row[9].get<bool>();
    return s;
}

} // namespace solution_detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace solution_repo {

// create — 插入新题解，返回新题解 id
inline int create(ConnectionPool& pool, const SolutionRow& s) {
    auto conn = pool.acquire();
    try {
        auto rs = conn.execute(
            "INSERT INTO solutions (user_id, problem_id, title, content, like_count) "
            "VALUES (?, ?, ?, ?, 0)",
            s.user_id, s.problem_id, s.title, s.content);
        return static_cast<int>(rs.getAutoIncrementValue());
    } catch (const mysqlx::Error& e) {
        throw SolutionRepoError(std::string("solution_repo::create: ") + e.what());
    }
}

// find_by_id — 通过 id 查找题解
inline std::optional<SolutionRow> find_by_id(ConnectionPool& pool, int id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT id, user_id, problem_id, title, content, like_count, comment_count, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
        "       DATE_FORMAT(updated_at, '%Y-%m-%d %H:%i:%s'), "
        "       is_deleted "
        "FROM solutions WHERE id = ? AND is_deleted = FALSE LIMIT 1",
        id);
    if (!row) return std::nullopt;
    return solution_detail::row_to_solution(*row);
}

// list_for_problem — 获取某题目的题解列表
struct SolutionListFilter {
    int         problem_id = 0;
    int         limit     = 20;
    int         offset    = 0;
    std::optional<int> user_id;
};

struct SolutionListResult {
    std::vector<SolutionListRow> items;
    int total = 0;
};

inline SolutionListResult list_for_problem(ConnectionPool& pool,
                                         const SolutionListFilter& filter) {
    SolutionListResult result;
    auto conn = pool.acquire();

    // COUNT
    std::string count_sql = "SELECT COUNT(*) FROM solutions WHERE problem_id = ? AND is_deleted = FALSE";
    if (filter.user_id.has_value()) {
        count_sql += " AND user_id = ?";
    }
    mysqlx::SqlStatement count_stmt = conn.session().sql(count_sql);
    count_stmt.bind(filter.problem_id);
    if (filter.user_id.has_value()) {
        count_stmt.bind(*filter.user_id);
    }
    auto count_rs = count_stmt.execute();
    for (auto row : count_rs) {
        result.total = static_cast<int>(row[0].get<std::int64_t>());
    }

    // SELECT with JOIN
    std::string sql =
        "SELECT s.id, s.user_id, s.problem_id, s.title, s.content, s.like_count, s.comment_count, "
        "       DATE_FORMAT(s.created_at, '%Y-%m-%d %H:%i:%s'), "
        "       DATE_FORMAT(s.updated_at, '%Y-%m-%d %H:%i:%s'), "
        "       s.is_deleted, "
        "       u.id, u.username, u.avatar "
        "FROM solutions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.problem_id = ? AND s.is_deleted = FALSE ";
    if (filter.user_id.has_value()) {
        sql += "AND s.user_id = ? ";
    }
    sql += "ORDER BY s.like_count DESC, s.created_at DESC LIMIT ? OFFSET ?";

    mysqlx::SqlStatement stmt = conn.session().sql(sql);
    stmt.bind(filter.problem_id);
    if (filter.user_id.has_value()) {
        stmt.bind(*filter.user_id);
    }
    stmt.bind(static_cast<std::int64_t>(filter.limit));
    stmt.bind(static_cast<std::int64_t>(filter.offset));

    auto rs = stmt.execute();
    for (auto row : rs) {
        try {
            SolutionListRow item;
            item.solution = solution_detail::row_to_solution(row);
            // user 字段从偏移 10 开始（comment_count 在 idx 6 之后插入）
            item.user.id       = static_cast<int>(row[10].get<std::int64_t>());
            item.user.username  = (row[11]).get<std::string>();
            item.user.avatar    = solution_detail::opt_string(row, 12);
            result.items.push_back(std::move(item));
        } catch (const std::exception&) {
            // skip malformed
        }
    }
    return result;
}

// count_for_problem — 获取题解总数
inline int count_for_problem(ConnectionPool& pool, int problem_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT COUNT(*) FROM solutions WHERE problem_id = ? AND is_deleted = FALSE",
        problem_id);
    return row.value_or(0);
}

// toggle_like — 点赞/取消点赞，返回 true = 新增点赞
inline bool toggle_like(ConnectionPool& pool, int solution_id, int user_id) {
    auto conn = pool.acquire();

    const auto existing = conn.fetch_scalar<int>(
        "SELECT 1 FROM solution_likes WHERE user_id = ? AND solution_id = ? LIMIT 1",
        user_id, solution_id);

    if (existing.has_value()) {
        conn.execute(
            "DELETE FROM solution_likes WHERE user_id = ? AND solution_id = ?",
            user_id, solution_id);
        conn.execute(
            "UPDATE solutions SET like_count = GREATEST(0, like_count - 1) WHERE id = ?",
            solution_id);
        return false;
    } else {
        conn.execute(
            "INSERT INTO solution_likes (user_id, solution_id) VALUES (?, ?)",
            user_id, solution_id);
        conn.execute(
            "UPDATE solutions SET like_count = like_count + 1 WHERE id = ?",
            solution_id);
        return true;
    }
}

// has_user_liked
inline bool has_user_liked(ConnectionPool& pool, int solution_id, int user_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM solution_likes WHERE user_id = ? AND solution_id = ? LIMIT 1",
        user_id, solution_id);
    return row.has_value();
}

// soft_delete — 软删除题解（将 is_deleted 设为 true）
inline void soft_delete(ConnectionPool& pool, int solution_id) {
    auto conn = pool.acquire();
    conn.execute(
        "UPDATE solutions SET is_deleted = TRUE WHERE id = ?",
        solution_id);
}

} // namespace solution_repo
} // namespace litecode
