-- V022 — solution_comments 功能扩展
--   1. 加 like_count 反范式列（与 solutions.like_count / comment_count 平行）
--   2. 加 parent_id 自引用（支持扁平 + 引用提示的回复关系）
--   3. 新建 solution_comment_likes 表（点赞关系 + 唯一索引防重复）
--   4. 索引：(solution_id, parent_id, is_deleted, created_at) 提升列表 + 树查询
--
-- 设计取舍:
--   * parent_id 用 INT NULL + 自引用 FK(parent_id → solution_comments.id),
--     not RESTRICT 也不 CASCADE 而是默认 RESTRICT —— 删评论时手动
--     校验后续是否还有子评论(本 PR 留给 API 层做)。
--   * 扁平 + 引用提示:list_for_solution 仍按 created_at ASC 一条线出,
--     parent_id 只影响前端提示文案,不需要树查询。
--   * like_count 反范式:与 solutions.like_count 套路一致,避免点赞数 N+1。

ALTER TABLE solution_comments
    ADD COLUMN like_count INT NOT NULL DEFAULT 0 AFTER content,
    ADD COLUMN parent_id INT NULL AFTER user_id,
    ADD CONSTRAINT fk_solution_comments_parent
        FOREIGN KEY (parent_id) REFERENCES solution_comments(id)
        ON DELETE RESTRICT ON UPDATE CASCADE;

CREATE INDEX idx_solution_comments_solution_parent
    ON solution_comments (solution_id, parent_id, is_deleted, created_at);

CREATE TABLE solution_comment_likes (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    comment_id INT NOT NULL,
    user_id    INT NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uniq_solution_comment_like (comment_id, user_id),
    INDEX idx_solution_comment_likes_comment (comment_id),
    INDEX idx_solution_comment_likes_user (user_id),
    CONSTRAINT fk_solution_comment_likes_comment
        FOREIGN KEY (comment_id) REFERENCES solution_comments(id) ON DELETE CASCADE,
    CONSTRAINT fk_solution_comment_likes_user
        FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO schema_migrations (version) VALUES ('V022');
