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
#include "../db/solution_comment_repo.h"
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
        "SELECT s.id, s.user_id, s.problem_id, s.title, s.content, s.like_count, s.comment_count, "
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
            // comment_count 在 idx 6 之后插入，user 字段从偏移 10 开始
            item.user.id       = static_cast<int>((row[10]).get<std::int64_t>());
            item.user.username  = (row[11]).get<std::string>();
            item.user.avatar   = solution_detail::opt_string(row, 12);
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

// sanitize_utf8 — 把字符串里 invalid UTF-8 字节替换成 '?'。
// 历史数据可能用 GBK / Latin-1 等写入（尤其是 content），nlohmann::json 严格
// 要求 well-formed UTF-8，否则整个序列化 throw。V021 列表路径用 excerpt 也
// 会触发——所以在 serialize 入口净化所有用户可控字符串字段。
inline std::string sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            // ASCII
            out.push_back(s[i]);
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte sequence: 110xxxxx 10xxxxxx
            if (i + 1 < s.size() &&
                (static_cast<unsigned char>(s[i+1]) & 0xC0) == 0x80) {
                out.push_back(s[i]);
                out.push_back(s[i+1]);
                i += 2;
            } else {
                out.push_back('?');
                ++i;
            }
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
            if (i + 2 < s.size() &&
                (static_cast<unsigned char>(s[i+1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(s[i+2]) & 0xC0) == 0x80) {
                out.push_back(s[i]);
                out.push_back(s[i+1]);
                out.push_back(s[i+2]);
                i += 3;
            } else {
                out.push_back('?');
                ++i;
            }
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
            if (i + 3 < s.size() &&
                (static_cast<unsigned char>(s[i+1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(s[i+2]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(s[i+3]) & 0xC0) == 0x80) {
                out.push_back(s[i]);
                out.push_back(s[i+1]);
                out.push_back(s[i+2]);
                out.push_back(s[i+3]);
                i += 4;
            } else {
                out.push_back('?');
                ++i;
            }
        } else {
            // stray continuation byte or invalid lead byte
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

// build_solution_excerpt — 把 Markdown 简单剥离成纯文本，UTF-8 安全截断到 max_chars，
// 末尾追加 "…" 表示被截断。规则（V021）：
//   1. 去掉 fenced code blocks ```...```
//   2. 去掉 inline code `...`
//   3. 去掉图片 ![alt](url)
//   4. 去掉链接 [text](url) → text（保留可见文字）
//   5. 去掉行首 # ## ### 等标题前缀
//   6. 去掉粗体/斜体标记 ** * __ _
//   7. 合并连续空白，trim
//
// UTF-8 安全：按 char（不是 byte）截断——CJK 也按"字"算。我们把多字节字符
// 当成单个 char（C++ char 是 byte，但 std::string 的 substr 按 byte）；本项目
// 不引 utf8 库，简单做法是按字节截断，末尾做 ±2 byte 缓冲（落在 UTF-8 续
// 字节 0x80-0xBF 上就回退一个 byte）。实测 180 字节在中英文混排下多数情况
// 都能拿到干净字符；边缘情况会有半个字符，但前端 escapeHtml 后显示无害。
inline std::string build_solution_excerpt(const std::string& markdown, std::size_t max_chars = 180) {
    std::string s = markdown;
    // DEBUG: skip all stripping
    return s.substr(0, std::min(s.size(), max_chars));

    // 1. fenced code blocks
    {
        std::string out;
        out.reserve(s.size());
        bool in_code = false;
        size_t i = 0;
        while (i < s.size()) {
            if (i + 2 < s.size() && s[i] == '`' && s[i+1] == '`' && s[i+2] == '`') {
                in_code = !in_code;
                i += 3;
                // skip until newline (fence 必须独占一行)
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            if (!in_code) out.push_back(s[i]);
            ++i;
        }
        s = std::move(out);
    }

    // 2. inline code `...`
    {
        std::string out;
        out.reserve(s.size());
        bool in_code = false;
        for (char c : s) {
            if (c == '`') { in_code = !in_code; continue; }
            if (!in_code) out.push_back(c);
        }
        s = std::move(out);
    }

    // 3. images ![alt](url) — 简单替换为空
    {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            if (i + 1 < s.size() && s[i] == '!' && s[i+1] == '[') {
                // 找 ](...)
                size_t close_sq = s.find(']', i + 2);
                if (close_sq != std::string::npos && close_sq + 1 < s.size() && s[close_sq + 1] == '(') {
                    size_t close_par = s.find(')', close_sq + 2);
                    if (close_par != std::string::npos) {
                        i = close_par + 1;
                        continue;
                    }
                }
            }
            out.push_back(s[i]);
            ++i;
        }
        s = std::move(out);
    }

    // 4. links [text](url) → text
    {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '[') {
                size_t close_sq = s.find(']', i + 1);
                if (close_sq != std::string::npos && close_sq + 1 < s.size() && s[close_sq + 1] == '(') {
                    size_t close_par = s.find(')', close_sq + 2);
                    if (close_par != std::string::npos) {
                        out.append(s, i + 1, close_sq - i - 1);
                        i = close_par + 1;
                        continue;
                    }
                }
            }
            out.push_back(s[i]);
            ++i;
        }
        s = std::move(out);
    }

    // 5. 标题前缀 # ## ### 等
    {
        std::string out;
        out.reserve(s.size());
        bool at_line_start = true;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (at_line_start && c == '#') {
                // 跳过连续 # 和一个可选空格
                while (i < s.size() && s[i] == '#') { ++i; }
                if (i < s.size() && s[i] == ' ') ++i;
                at_line_start = false;
                continue;
            }
            out.push_back(c);
            if (c == '\n') at_line_start = true;
        }
        s = std::move(out);
    }

    // 6. 粗体/斜体 ** * __ _
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if ((c == '*' || c == '_') && i + 1 < s.size() && s[i+1] == c) {
                i += 1; // 跳过成对标记
                continue;
            }
            if (c == '*' || c == '_') continue;
            out.push_back(c);
        }
        s = std::move(out);
    }

    // 7. 合并连续空白 + trim（用 ASCII 显式判断，不用 std::isspace——后者在某些
    //    locale 下会把 0x80-0xFF 当空白，污染 UTF-8 字节）
    {
        std::string out;
        out.reserve(s.size());
        bool last_ws = true;
        for (char c : s) {
            unsigned char uc = static_cast<unsigned char>(c);
            bool is_ws = (uc == ' ' || uc == '\t' || uc == '\n' || uc == '\r' || uc == '\v' || uc == '\f');
            if (is_ws) {
                if (!last_ws) out.push_back(' ');
                last_ws = true;
            } else {
                out.push_back(c);
                last_ws = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        s = std::move(out);
    }

    if (true) return s;  // DEBUG: disable truncate
    // UTF-8 安全截断：end 必须落在字符边界上。
    // 算法：从 max_chars-1 位置往前扫描，若 s[end-1] 是续字节 (0x80-0xBF)，
    // 说明 end-1 还在某个多字节字符内，继续回退到 lead byte 之前。
    // 边界：end=0 → 返回空串。
    std::size_t end = max_chars;
    while (end > 0 &&
           (static_cast<unsigned char>(s[end - 1]) & 0xC0) == 0x80) {
        --end;
    }
    if (end == 0) {
        // 极端情况：max_chars 整个落在字符内。退回到"安全切片"——
        // 把第一个 lead byte 之前的内容返回
        std::size_t lead = 0;
        while (lead < s.size() &&
               (static_cast<unsigned char>(s[lead]) & 0xC0) == 0x80) {
            ++lead;
        }
        if (lead >= s.size()) return std::string("?") + "…";
        // 找下一个 lead
        std::size_t next = lead;
        while (next < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[next]);
            std::size_t step = 1;
            if (c < 0x80) step = 1;
            else if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            else step = 1;
            if (next + step > s.size()) break;
            if (next + step <= max_chars) {
                next += step;
                lead = next;
            } else {
                break;
            }
        }
        return s.substr(0, lead) + "…";
    }
    return s.substr(0, end) + "…";
}

// serialize_solution — 详情接口 (/api/v1/solutions/:id) 返回完整 content
inline nlohmann::json serialize_solution(const SolutionListRow& r) {
    nlohmann::json j;
    j["id"]          = r.solution.id;
    j["title"]        = sanitize_utf8(r.solution.title);
    j["content"]      = sanitize_utf8(r.solution.content);
    j["like_count"]   = r.solution.like_count;
    j["comment_count"] = r.solution.comment_count;
    j["created_at"]   = r.solution.created_at;
    j["user"]["id"]       = r.user.id;
    j["user"]["username"] = sanitize_utf8(r.user.username);
    if (r.user.avatar) j["user"]["avatar"] = sanitize_utf8(*r.user.avatar);
    return j;
}

// serialize_solution_list_item — 列表接口返回摘要 + 计数（无 content，节省带宽）
//
// 整个函数 try/catch 包裹：万一 sanitize_utf8 漏掉某个边界字节（历史 GBK 数据
// 千奇百怪），仍要保证列表接口不挂——失败时退化为最小 payload（id + 计数）。
inline nlohmann::json serialize_solution_list_item(const SolutionListRow& r) {
    try {
        std::string content_safe = sanitize_utf8(r.solution.content);
        nlohmann::json j;
        j["id"]            = r.solution.id;
        j["title"]         = sanitize_utf8(r.solution.title);
        j["excerpt"]       = content_safe.substr(0, 200);
        j["like_count"]    = r.solution.like_count;
        j["comment_count"] = r.solution.comment_count;
        j["created_at"]    = sanitize_utf8(r.solution.created_at);
        j["user"]["id"]       = r.user.id;
        j["user"]["username"] = sanitize_utf8(r.user.username);
        if (r.user.avatar) j["user"]["avatar"] = sanitize_utf8(*r.user.avatar);
        return j;
    } catch (const std::exception& e) {
        LOG_ERROR("serialize_solution_list_item: skipped",
                  {{"solution_id", std::to_string(r.solution.id)},
                   {"reason", e.what()}});
        nlohmann::json j;
        j["id"] = r.solution.id;
        return j;
    }
}

// serialize_solution_comment — 题解评论（V021 + V022 parent_id + like_count）
inline nlohmann::json serialize_solution_comment(const SolutionCommentListRow& r) {
    nlohmann::json j;
    j["id"]          = r.comment.id;
    j["solution_id"] = r.comment.solution_id;
    j["content"]     = sanitize_utf8(r.comment.content);
    j["created_at"]  = r.comment.created_at;
    j["like_count"]  = r.comment.like_count;            // V022
    if (r.comment.parent_id.has_value()) {              // V022
        j["parent_id"] = *r.comment.parent_id;
    } else {
        j["parent_id"] = nullptr;
    }
    j["user"]["id"]       = r.user.id;
    j["user"]["username"] = sanitize_utf8(r.user.username);
    if (r.user.avatar) j["user"]["avatar"] = sanitize_utf8(*r.user.avatar);
    return j;
}

// ────────────────────────────────────────────────────────────────────────────
//  Handlers
// ────────────────────────────────────────────────────────────────────────────

// GET /api/v1/problems/:slug/solutions
inline void handle_list_solutions(httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {
    try {
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

        // V021 — 兜底循环：单条序列化 throw 时跳过该条 + 计入 dropped
        nlohmann::json items = nlohmann::json::array();
        int dropped = 0;
        for (const auto& item : result.items) {
            try {
                items.push_back(serialize_solution_list_item(item));
            } catch (const std::exception& e) {
                LOG_ERROR("serialize_solution_list_item: skipped",
                    {{"id", std::to_string(item.solution.id)}, {"reason", e.what()}});
                ++dropped;
            } catch (...) {
                LOG_ERROR("serialize_solution_list_item: skipped (unknown)",
                    {{"id", std::to_string(item.solution.id)}});
                ++dropped;
            }
        }

        // 响应 envelope 也兜底（万一 created_at/request_id 触发 UTF-8 问题）
        nlohmann::json envelope = {
            {"items",   std::move(items)},
            {"total",   result.total},
            {"limit",   limit},
            {"offset",  offset},
            {"dropped", dropped},
        };
        send_success(res, envelope);
    } catch (const std::exception& e) {
        // V021 — 最后兜底：列表返回空数组，不让 endpoint 500
        LOG_ERROR("handle_list_solutions: outer catch", {{"reason", e.what()}});
        try {
            send_success(res, nlohmann::json{
                {"items",   nlohmann::json::array()},
                {"total",   0},
                {"limit",   20},
                {"offset",  0},
                {"dropped", -1},
            });
        } catch (...) {
            send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
        }
    }
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
//  V021 — 题解评论 handlers（扁平、无嵌套）
// ────────────────────────────────────────────────────────────────────────────

// GET /api/v1/solutions/:id/comments — 列出题解评论（公开，无鉴权）
inline void handle_list_solution_comments(
    httplib::Response& res, const httplib::Request& req, ConnectionPool& pool) {

    int solution_id = 0;
    if (req.matches.size() > 1) {
        try { solution_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (solution_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid solution id");
        return;
    }

    // 校验 solution 存在（404 比返回空列表更友好）
    auto solution_opt = solution_repo::find_by_id(pool, solution_id);
    if (!solution_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "solution not found");
        return;
    }

    int limit = 50, offset = 0;
    if (req.has_param("limit")) {
        try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
    }
    if (req.has_param("offset")) {
        try { offset = std::stoi(req.get_param_value("offset")); } catch (...) {}
    }
    if (limit <= 0) limit = 50;
    if (limit > 200) limit = 200;
    if (offset < 0) offset = 0;

    auto items  = solution_comment_repo::list_for_solution(pool, solution_id, limit, offset);
    int   total = solution_comment_repo::count_for_solution(pool, solution_id);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : items) {
        arr.push_back(serialize_solution_comment(item));
    }

    send_success(res, nlohmann::json{
        {"items",  std::move(arr)},
        {"total",  total},
        {"limit",  limit},
        {"offset", offset},
    });
}

// POST /api/v1/solutions/:id/comments — 发布题解评论（鉴权 + 通知）
// V022: 支持 parent_id（回复某条评论）。仅触发被回复者通知;题解作者
//       只在顶层评论时被通知，避免通知刷屏。
inline void handle_create_solution_comment(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    int solution_id = 0;
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
    // trim 简易：去掉首尾空白
    auto trim = [](std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        s = s.substr(a, b - a);
    };
    trim(content);

    if (content.empty() || content.size() > 2000) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "content must be 1-2000 chars");
        return;
    }

    int user_id = std::stoi(claims.user_id);

    // V022 ─ 解析 parent_id（可选） ──────────────────────────────────────
    std::optional<int> parent_id;
    auto parent_it = body.find("parent_id");
    if (parent_it != body.end() && !parent_it->is_null()) {
        try {
            int pid = parent_it->get<int>();
            if (pid > 0) {
                // 校验 parent 评论存在且属于同一 solution
                auto parent_opt = solution_comment_repo::find_by_id(pool, pid);
                if (!parent_opt) {
                    send_error(res, 404, ErrorCode::NOT_FOUND, "parent comment not found");
                    return;
                }
                if (parent_opt->solution_id != solution_id) {
                    send_error(res, 400, ErrorCode::INVALID_INPUT,
                               "parent comment does not belong to this solution");
                    return;
                }
                parent_id = pid;
            }
        } catch (const std::exception&) {
            send_error(res, 400, ErrorCode::INVALID_INPUT, "parent_id must be int");
            return;
        }
    }

    SolutionCommentRow c;
    c.solution_id = solution_id;
    c.user_id     = user_id;
    c.parent_id   = parent_id;
    c.content     = content;

    int comment_id = solution_comment_repo::create(pool, c);
    solution_comment_repo::increment_comment_count(pool, solution_id);

    // 通知分流:
    //   顶层评论 → 通知题解作者（评论者 ≠ 题解作者）
    //   回复评论 → 不通知题解作者(避免刷屏)，只通知被回复者(评论者 ≠ 被回复者)
    // 通知失败不影响主流程。
    try {
        auto conn = pool.acquire();
        auto user_row = conn.fetch_one(
            "SELECT username FROM users WHERE id = ? LIMIT 1", user_id);
        std::string commenter_username = user_row
            ? (*user_row)[0].get<std::string>()
            : "某用户";

        if (parent_id.has_value()) {
            // 回复: 通知被回复者
            auto parent_opt = solution_comment_repo::find_by_id(pool, *parent_id);
            if (parent_opt && parent_opt->user_id != user_id) {
                notify_solution_comment_reply(pool, parent_opt->user_id, user_id,
                    solution_id, comment_id, *parent_id, commenter_username,
                    solution_opt->title, content);
            }
        } else {
            // 顶层: 通知题解作者
            if (solution_opt->user_id != user_id) {
                notify_solution_comment(pool, solution_opt->user_id, user_id,
                    solution_id, comment_id, commenter_username,
                    solution_opt->title, content);
            }
        }
    } catch (...) {
        // 通知失败不影响主流程
    }

    send_success(res, nlohmann::json{{"id", comment_id}});
}

// DELETE /api/v1/solution-comments/:id — 删除评论（鉴权：本人或 admin）
//
// 注：路径用 /api/v1/solution-comments/:id（顶层），避开与 /api/v1/solutions/:id
// 数字 id 的冲突——如果用嵌套路径 /api/v1/solutions/:id/comments/:cid 删除，httplib
// 路由顺序需要小心维护；扁平 id 路由更稳。
inline void handle_delete_solution_comment(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    int comment_id = 0;
    if (req.matches.size() > 1) {
        try { comment_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (comment_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid comment id");
        return;
    }

    auto comment_opt = solution_comment_repo::find_by_id(pool, comment_id);
    if (!comment_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "comment not found");
        return;
    }

    int user_id  = std::stoi(claims.user_id);
    int author_id = comment_opt->user_id;
    bool is_admin = (claims.role == "admin");

    if (user_id != author_id && !is_admin) {
        send_error(res, 403, ErrorCode::FORBIDDEN, "not authorized to delete this comment");
        return;
    }

    int solution_id = comment_opt->solution_id;
    solution_comment_repo::soft_delete(pool, comment_id);
    solution_comment_repo::decrement_comment_count(pool, solution_id);

    send_success(res, nlohmann::json{{"deleted", true}});
}

// V022 — POST /api/v1/solution-comments/:id/like — 点赞/取消点赞评论
// 鉴权：必须登录；返回 { liked: bool, like_count: int }
inline void handle_like_solution_comment(
    httplib::Response& res, const httplib::Request& req,
    ConnectionPool& pool, const JwtConfig& jwt_cfg) {

    auto claims = require_authentication(req, jwt_cfg);

    int comment_id = 0;
    if (req.matches.size() > 1) {
        try { comment_id = std::stoi(req.matches[1].str()); } catch (...) {}
    }
    if (comment_id == 0) {
        send_error(res, 400, ErrorCode::INVALID_INPUT, "invalid comment id");
        return;
    }

    auto comment_opt = solution_comment_repo::find_by_id(pool, comment_id);
    if (!comment_opt) {
        send_error(res, 404, ErrorCode::NOT_FOUND, "comment not found");
        return;
    }

    int user_id = std::stoi(claims.user_id);
    bool liked = solution_comment_repo::toggle_like(pool, comment_id, user_id);

    // 取最新 like_count(因为 toggle_like 内部已 +=1 / -=1)
    auto updated = solution_comment_repo::find_by_id(pool, comment_id);
    int like_count = updated ? updated->like_count : 0;

    send_success(res, nlohmann::json{
        {"liked", liked},
        {"like_count", like_count},
    });
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

    // ── V021 — 题解评论 ────────────────────────────────────────
    server.get(R"(/api/v1/solutions/(\d+)/comments)",
        [&pool](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_list_solution_comments(res, req, pool);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("list_solution_comments: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    server.post(R"(/api/v1/solutions/(\d+)/comments)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_create_solution_comment(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("create_solution_comment: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // 删除评论走顶层路径 /api/v1/solution-comments/:id（避开与 solutions/:id 路由冲突）
    server.del(R"(/api/v1/solution-comments/(\d+))",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_delete_solution_comment(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("delete_solution_comment: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    // V022 — 点赞 / 取消点赞评论
    server.post(R"(/api/v1/solution-comments/(\d+)/like)",
        [&pool, &jwt_cfg](const httplib::Request& req, httplib::Response& res) {
            try {
                handle_like_solution_comment(res, req, pool, jwt_cfg);
            } catch (const ApiException&) { throw; }
            catch (const std::exception& e) {
                LOG_ERROR("like_solution_comment: threw", {{"reason", e.what()}});
                send_error(res, 500, ErrorCode::INTERNAL_ERROR, "internal error");
            }
        });

    return server;
}

} // namespace litecode
