-- =============================================================
-- V014__user_profile_and_username_history.sql
-- v1.3.4 PR 9 ★ 个人资料编辑 + 用户名可改 + 旧名永久 alias
-- SPEC §5.1 / §5.2 + 新功能
--
-- 三件事:
--   1) users 加 display_name / school / bio — 个人资料编辑新增字段
--   2) users 加 token_version + username_changed_at — 改名频率限制
--      基础设施 + 未来 JWT 缓存强制失效钩子(本 PR 不强制启用)
--   3) 新表 user_username_history — 旧 username 永久 alias,像 GitHub
--      改名后旧名依然解析到当前 user(/profile?username=oldname 跳新名)
--
-- Idempotent (同 V007 风格,DROP IF EXISTS + information_schema 检查);
-- docker-entrypoint-initdb.d 与 init_db.sh 反复执行均安全。
-- =============================================================

-- ── Step 1: 给 users 加 5 列 ─────────────────────────────────
DROP PROCEDURE IF EXISTS migrate_v014_add_user_columns;
DELIMITER //
CREATE PROCEDURE migrate_v014_add_user_columns()
BEGIN
    -- display_name: 昵称,1..50 字符,可空(空时回退到 username)
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'users'
           AND COLUMN_NAME  = 'display_name'
    ) THEN
        ALTER TABLE users ADD COLUMN display_name VARCHAR(50) NULL AFTER username;
    END IF;

    -- school: 学校,0..100 字符,可空
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'users'
           AND COLUMN_NAME  = 'school'
    ) THEN
        ALTER TABLE users ADD COLUMN school VARCHAR(100) NULL AFTER email;
    END IF;

    -- bio: 个人简介,0..500 字符,可空
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'users'
           AND COLUMN_NAME  = 'bio'
    ) THEN
        ALTER TABLE users ADD COLUMN bio VARCHAR(500) NULL AFTER school;
    END IF;

    -- token_version: 预留 JWT 缓存失效钩子(本 PR 不强制启用,见
    -- memory v1.3.3.7 admin 改 role 后 JWT 缓存 stale 2h-7d 的问题)。
    -- 后续 PR 加 require_role 时对照 claims.tv 即可。本字段先建好。
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'users'
           AND COLUMN_NAME  = 'token_version'
    ) THEN
        ALTER TABLE users ADD COLUMN token_version INT NOT NULL DEFAULT 0 AFTER role;
    END IF;

    -- username_changed_at: 改名频率限制用(本 PR 限定 1 天 1 次)
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'users'
           AND COLUMN_NAME  = 'username_changed_at'
    ) THEN
        ALTER TABLE users ADD COLUMN username_changed_at DATETIME NULL AFTER last_login;
    END IF;
END //
DELIMITER ;
CALL migrate_v014_add_user_columns();
DROP PROCEDURE migrate_v014_add_user_columns;

-- ── Step 2: 新表 user_username_history ────────────────────────
-- 旧 username 永久 alias。改名时把旧名写进来,UNIQUE 保证不被
-- 其他人注册。`/api/v1/users/lookup?username=old` 路由先查 users
-- 表,miss 再查 history 表 → 跳到当前 user。
DROP PROCEDURE IF EXISTS migrate_v014_create_history;
DELIMITER //
CREATE PROCEDURE migrate_v014_create_history()
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.TABLES
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'user_username_history'
    ) THEN
        CREATE TABLE user_username_history (
            id            INT AUTO_INCREMENT PRIMARY KEY,
            user_id       INT  NOT NULL,
            old_username  VARCHAR(50) NOT NULL,
            changed_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            UNIQUE KEY uq_history_old_username (old_username),
            INDEX idx_history_user (user_id),
            FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
    END IF;
END //
DELIMITER ;
CALL migrate_v014_create_history();
DROP PROCEDURE migrate_v014_create_history;

-- 兼容 init_db.sh 之外的手动重跑场景
INSERT IGNORE INTO schema_migrations (version) VALUES ('V014');
