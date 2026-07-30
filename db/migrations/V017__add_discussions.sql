-- SPDX-License-Identifier: MIT
--
-- LiteCode-CPP — V017: add discussions system (Phase 7 ★)
--
-- 讨论系统：
--   - discussions 表：主讨论（顶层）和回复（嵌套通过 parent_id / root_id 实现）
--   - discussion_likes 表：点赞
--
-- problem_id = NULL 表示全局讨论区，非 NULL 表示题目下讨论区

START TRANSACTION;

-- ─── discussions 主表 ───────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS discussions (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    user_id         INT NOT NULL,
    problem_id      INT NULL,                -- NULL = 全局讨论
    parent_id       INT NULL,                -- NULL = 顶层，非 NULL = 回复
    root_id         INT NULL,                -- 指向顶层讨论 id（方便嵌套查询）
    title           VARCHAR(200) NULL,       -- 全局讨论需要标题
    content         MEDIUMTEXT NOT NULL,
    like_count      INT NOT NULL DEFAULT 0,
    reply_count     INT NOT NULL DEFAULT 0,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    is_deleted      BOOLEAN NOT NULL DEFAULT FALSE,

    FOREIGN KEY (user_id)      REFERENCES users(id)       ON DELETE CASCADE,
    FOREIGN KEY (problem_id)   REFERENCES problems(id)   ON DELETE CASCADE,
    FOREIGN KEY (parent_id)     REFERENCES discussions(id) ON DELETE CASCADE,
    FOREIGN KEY (root_id)      REFERENCES discussions(id) ON DELETE CASCADE,

    INDEX idx_discussions_problem_id (problem_id, is_deleted, created_at DESC),
    INDEX idx_discussions_user_id     (user_id, is_deleted, created_at DESC),
    INDEX idx_discussions_root_id     (root_id, is_deleted)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ─── discussion_likes 点赞表 ───────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS discussion_likes (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    user_id         INT NOT NULL,
    discussion_id   INT NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (user_id)       REFERENCES users(id)       ON DELETE CASCADE,
    FOREIGN KEY (discussion_id) REFERENCES discussions(id)  ON DELETE CASCADE,

    UNIQUE KEY uk_user_discussion (user_id, discussion_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

COMMIT;
