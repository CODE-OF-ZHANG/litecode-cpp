// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — checkin repository
//
// 每日打卡系统数据访问层
//
// 打卡规则:
//   - 用户每天 AC 至少 1 道题即打卡成功
//   - 中断则 current_streak 归零，历史记录保留
//
// 设计说明:
//   - Header-only + inline: 遵循项目现有模式
//   - try_checkin 由 submission_repo::mark_finished 在 AC 成功后调用
//   - 返回 bool 表示是否真正打卡（当日重复 AC 不重复打卡）

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "../logger.h"
#include "connection_pool.h"

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  CheckinRecordRow
//
//  Plain-data projection of a row from the `checkin_records` table.
// ────────────────────────────────────────────────────────────────────────────

struct CheckinRecordRow {
    int         id            = 0;
    int         user_id       = 0;
    std::string checkin_date;  // "YYYY-MM-DD"
    std::optional<int> problem_id;
    std::optional<int> submission_id;
    std::string created_at;    // "YYYY-MM-DD HH:MM:SS"
};

// ────────────────────────────────────────────────────────────────────────────
//  CheckinStats
//
//  用户打卡统计数据
// ────────────────────────────────────────────────────────────────────────────

struct CheckinStats {
    int current_streak  = 0;   // 当前连续打卡天数
    int longest_streak  = 0;   // 历史最长连续天数
    int total_checkins  = 0;   // 累计打卡天数
    bool checked_in_today = false;  // 今日是否已打卡
};

// ────────────────────────────────────────────────────────────────────────────
//  CheckinRepoError
// ────────────────────────────────────────────────────────────────────────────

class CheckinRepoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ────────────────────────────────────────────────────────────────────────────
//  Row materialization helpers
// ────────────────────────────────────────────────────────────────────────────

namespace checkin_detail {

inline std::optional<std::string> opt_string(const mysqlx::Row& row,
                                             std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try {
        return v.get<std::string>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::string req_string(const mysqlx::Row& row, std::size_t idx,
                              const char* field) {
    try {
        return row[idx].get<std::string>();
    } catch (const std::exception& e) {
        throw CheckinRepoError(std::string("checkin_repo: required field '") +
                               field + "' is not a string: " + e.what());
    }
}

inline int req_int(const mysqlx::Row& row, std::size_t idx,
                   const char* field) {
    try {
        return static_cast<int>(row[idx].get<std::int64_t>());
    } catch (const std::exception& e) {
        throw CheckinRepoError(std::string("checkin_repo: required field '") +
                               field + "' is not an int: " + e.what());
    }
}

inline std::optional<int> opt_int(const mysqlx::Row& row, std::size_t idx) {
    const auto& v = row[idx];
    if (v.isNull()) return std::nullopt;
    try {
        return static_cast<int>(v.get<std::int64_t>());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline CheckinRecordRow row_to_checkin(const mysqlx::Row& row) {
    CheckinRecordRow r;
    r.id             = req_int   (row, 0, "id");
    r.user_id         = req_int   (row, 1, "user_id");
    r.checkin_date    = req_string(row, 2, "checkin_date");
    r.problem_id      = opt_int  (row, 3);
    r.submission_id   = opt_int  (row, 4);
    r.created_at      = req_string(row, 5, "created_at");
    return r;
}

} // namespace checkin_detail

// ────────────────────────────────────────────────────────────────────────────
//  Public API
// ────────────────────────────────────────────────────────────────────────────

namespace checkin_repo {

// has_checkin_today — 检查用户今日是否已打卡
inline bool has_checkin_today(ConnectionPool& pool, int user_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_scalar<int>(
        "SELECT 1 FROM checkin_records "
        "WHERE user_id = ? AND checkin_date = CURDATE() LIMIT 1",
        user_id);
    return row.has_value();
}

// create — 插入一条打卡记录（每日唯一）
// 返回新记录 id，0 表示今日已打卡（并发情况下的防御）
inline int create(ConnectionPool& pool, int user_id,
                  std::optional<int> problem_id = std::nullopt,
                  std::optional<int> submission_id = std::nullopt) {
    auto conn = pool.acquire();
    try {
        mysqlx::Value pid_val = problem_id
            ? mysqlx::Value(static_cast<std::int64_t>(*problem_id))
            : mysqlx::Value(nullptr);
        mysqlx::Value sid_val = submission_id
            ? mysqlx::Value(static_cast<std::int64_t>(*submission_id))
            : mysqlx::Value(nullptr);

        auto rs = conn.execute(
            "INSERT INTO checkin_records (user_id, checkin_date, problem_id, submission_id) "
            "VALUES (?, CURDATE(), ?, ?)",
            user_id, pid_val, sid_val);
        return static_cast<int>(rs.getAutoIncrementValue());
    } catch (const mysqlx::Error& e) {
        const std::string what = e.what();
        // Duplicate entry (errno 1062) = 今日已打卡
        if (what.find("Duplicate entry") != std::string::npos ||
            what.find("duplicate")        != std::string::npos ||
            what.find("1062")             != std::string::npos) {
            return 0;
        }
        throw CheckinRepoError(std::string("checkin_repo::create: ") + what);
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  try_checkin — AC 成功后调用此函数尝试打卡
//
//  逻辑:
//    1. 检查今日是否已有打卡记录（有则跳过，返回 false）
//    2. 插入 checkin_records
//    3. 计算新 current_streak：若昨日已打卡则 +1，否则重置为 1
//    4. 更新 longest_streak（若超过历史）
//    5. 更新 users.last_checkin
//
//  返回: true = 真正打卡了，false = 今日已打卡（跳过）
// ────────────────────────────────────────────────────────────────────────────

inline bool try_checkin(ConnectionPool& pool, int user_id,
                        std::optional<int> problem_id = std::nullopt,
                        std::optional<int> submission_id = std::nullopt) {
    auto conn = pool.acquire();

    // 1. 检查今日是否已打卡
    const auto today_row = conn.fetch_scalar<int>(
        "SELECT 1 FROM checkin_records "
        "WHERE user_id = ? AND checkin_date = CURDATE() LIMIT 1",
        user_id);
    if (today_row.has_value()) {
        return false;  // 今日已打卡，跳过
    }

    try {
        // 2. 插入打卡记录
        mysqlx::Value pid_val = problem_id
            ? mysqlx::Value(static_cast<std::int64_t>(*problem_id))
            : mysqlx::Value(nullptr);
        mysqlx::Value sid_val = submission_id
            ? mysqlx::Value(static_cast<std::int64_t>(*submission_id))
            : mysqlx::Value(nullptr);

        conn.execute(
            "INSERT INTO checkin_records (user_id, checkin_date, problem_id, submission_id) "
            "VALUES (?, CURDATE(), ?, ?)",
            user_id, pid_val, sid_val);

        // 3. 读取上次的 last_checkin 和 current_streak
        const auto user_row = conn.fetch_one(
            "SELECT last_checkin, current_streak, longest_streak "
            "FROM users WHERE id = ? LIMIT 1",
            user_id);
        if (!user_row) {
            LOG_WARN("checkin_repo::try_checkin: user not found",
                     {{"user_id", std::to_string(user_id)}});
            return true;  // 记录已插入，但用户字段更新失败
        }

        const auto last_checkin_str = checkin_detail::opt_string(*user_row, 0);
        const int  current_streak   = checkin_detail::opt_int(*user_row, 1).value_or(0);
        const int  longest_streak   = checkin_detail::opt_int(*user_row, 2).value_or(0);

        // 4. 计算新连续天数
        int new_streak = 1;  // 新打卡，从1开始
        if (last_checkin_str.has_value()) {
            // 检查是否是连续的（昨天已打卡）
            auto yesterday_row = conn.fetch_scalar<int>(
                "SELECT 1 FROM checkin_records "
                "WHERE user_id = ? AND checkin_date = DATE_SUB(CURDATE(), INTERVAL 1 DAY) "
                "LIMIT 1",
                user_id);
            if (yesterday_row.has_value()) {
                new_streak = current_streak + 1;  // 连续，+1
            }
            // 否则重置为 1（断了）
        }

        int new_longest = std::max(longest_streak, new_streak);

        // 5. 更新 users 表
        conn.execute(
            "UPDATE users "
            "SET last_checkin = CURDATE(), "
            "    current_streak = ?, "
            "    longest_streak = ? "
            "WHERE id = ?",
            new_streak, new_longest, user_id);

        return true;
    } catch (const mysqlx::Error& e) {
        LOG_WARN("checkin_repo::try_checkin failed",
                 {{"user_id", std::to_string(user_id)},
                  {"reason",  e.what()}});
        return false;
    }
}

// get_stats — 获取用户打卡统计
inline CheckinStats get_stats(ConnectionPool& pool, int user_id) {
    CheckinStats stats;
    auto conn = pool.acquire();

    // 从 users 表读取
    const auto user_row = conn.fetch_one(
        "SELECT current_streak, longest_streak, last_checkin "
        "FROM users WHERE id = ? LIMIT 1",
        user_id);
    if (user_row) {
        stats.current_streak  = checkin_detail::opt_int(*user_row, 0).value_or(0);
        stats.longest_streak  = checkin_detail::opt_int(*user_row, 1).value_or(0);
        // last_checkin 不直接用，只用于判断今日
    }

    // 检查今日是否已打卡
    const auto today_row = conn.fetch_scalar<int>(
        "SELECT 1 FROM checkin_records "
        "WHERE user_id = ? AND checkin_date = CURDATE() LIMIT 1",
        user_id);
    stats.checked_in_today = today_row.has_value();

    // 总打卡天数
    const auto total_row = conn.fetch_scalar<int>(
        "SELECT COUNT(*) FROM checkin_records WHERE user_id = ?",
        user_id);
    stats.total_checkins = total_row.value_or(0);

    return stats;
}

// get_calendar — 获取用户近一年的打卡日历
// 返回按日期降序的打卡记录列表
inline std::vector<CheckinRecordRow> get_calendar(ConnectionPool& pool,
                                                    int user_id,
                                                    int days = 365) {
    std::vector<CheckinRecordRow> out;
    auto conn = pool.acquire();

    auto rs = conn.execute(
        "SELECT id, user_id, DATE_FORMAT(checkin_date, '%Y-%m-%d') AS checkin_date, "
        "       problem_id, submission_id, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at "
        "FROM checkin_records "
        "WHERE user_id = ? AND checkin_date >= DATE_SUB(CURDATE(), INTERVAL ? DAY) "
        "ORDER BY checkin_date DESC",
        user_id, days);

    for (auto row : rs) {
        try {
            out.push_back(checkin_detail::row_to_checkin(row));
        } catch (const std::exception&) {
            // Skip malformed rows
        }
    }
    return out;
}

// get_today_checkin — 获取今日打卡记录（如有）
inline std::optional<CheckinRecordRow> get_today_checkin(ConnectionPool& pool,
                                                          int user_id) {
    auto conn = pool.acquire();
    const auto row = conn.fetch_one(
        "SELECT id, user_id, DATE_FORMAT(checkin_date, '%Y-%m-%d') AS checkin_date, "
        "       problem_id, submission_id, "
        "       DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') AS created_at "
        "FROM checkin_records "
        "WHERE user_id = ? AND checkin_date = CURDATE() LIMIT 1",
        user_id);
    if (!row) return std::nullopt;
    return checkin_detail::row_to_checkin(*row);
}

} // namespace checkin_repo
} // namespace litecode
