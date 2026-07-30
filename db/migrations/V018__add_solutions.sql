-- =============================================================
-- V018__add_solutions.sql
-- 题解系统
--
-- 新增表:
--   - solutions: 题解主表
--   - solution_likes: 题解点赞表
-- =============================================================

-- ── 1. solutions 表 ─────────────────────────────────────────
CREATE TABLE IF NOT EXISTS solutions (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    user_id         INT NOT NULL,
    problem_id      INT NOT NULL,
    title           VARCHAR(200) NOT NULL,
    content         MEDIUMTEXT NOT NULL,               -- Markdown 正文（包含代码）
    like_count      INT NOT NULL DEFAULT 0,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    is_deleted      BOOLEAN NOT NULL DEFAULT FALSE,
    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,
    INDEX idx_solutions_problem_id (problem_id, is_deleted, like_count DESC, created_at DESC),
    INDEX idx_solutions_user_id    (user_id, is_deleted)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 2. solution_likes 表 ────────────────────────────────────
CREATE TABLE IF NOT EXISTS solution_likes (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    user_id         INT NOT NULL,
    solution_id     INT NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id)     REFERENCES users(id)     ON DELETE CASCADE,
    FOREIGN KEY (solution_id) REFERENCES solutions(id) ON DELETE CASCADE,
    UNIQUE KEY uk_user_solution (user_id, solution_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 3. schema_migrations 记录 ──────────────────────────────────
INSERT IGNORE INTO schema_migrations (version) VALUES ('V018');
