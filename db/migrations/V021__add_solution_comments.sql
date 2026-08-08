-- =============================================================
-- V021__add_solution_comments.sql
-- 题解评论系统 + 列表摘要支持
--
-- 新增/修改:
--   - solutions: 新增 comment_count INT NOT NULL DEFAULT 0
--   - solution_comments: 题解评论表（扁平、无嵌套回复、无点赞）
--
-- 设计决策:
--   - comment_count 反范式（仿 like_count / reply_count）— 列表 endpoint
--     序列化时直接带上，避免 N+1。
--   - solution_comments 表独立（不复用 discussions）：题解评论是纯文本 +
--     扁平 + 无 like_count/reply_count，schema 越简单越好。
--   - 索引 (solution_id, is_deleted, created_at ASC)：抽屉按时间正序
--     渲染（最早在前，聊天串体验）。
--   - VARCHAR(2000) 上限：评论应短（讨论是 10000），给前端 maxlength 兜底。
-- =============================================================

START TRANSACTION;

-- ── 1. solutions.comment_count 列 ────────────────────────────
-- IF NOT EXISTS 防御：本地可能手工 ALTER 过；MySQL 8.0+ 支持
SET @col_exists := (
    SELECT COUNT(*) FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name   = 'solutions'
      AND column_name  = 'comment_count'
);
SET @ddl := IF(@col_exists = 0,
    'ALTER TABLE solutions ADD COLUMN comment_count INT NOT NULL DEFAULT 0 AFTER like_count',
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 一次性回填（存量数据：用聚合补真实数；空表场景无副作用）
UPDATE solutions s
SET s.comment_count = (
    SELECT COUNT(*) FROM solution_comments sc
    WHERE sc.solution_id = s.id AND sc.is_deleted = FALSE
)
WHERE s.comment_count = 0
  AND EXISTS (SELECT 1 FROM solution_comments sc WHERE sc.solution_id = s.id);

-- ── 2. solution_comments 表 ────────────────────────────────────
CREATE TABLE IF NOT EXISTS solution_comments (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    solution_id     INT NOT NULL,
    user_id         INT NOT NULL,
    content         VARCHAR(2000) NOT NULL,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_deleted      BOOLEAN NOT NULL DEFAULT FALSE,

    FOREIGN KEY (solution_id) REFERENCES solutions(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id)     REFERENCES users(id)     ON DELETE CASCADE,

    INDEX idx_solution_comments_solution (solution_id, is_deleted, created_at ASC),
    INDEX idx_solution_comments_user     (user_id, is_deleted)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 3. schema_migrations 记录 ──────────────────────────────────
INSERT IGNORE INTO schema_migrations (version) VALUES ('V021');

COMMIT;