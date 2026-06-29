-- =============================================================
-- V007__add_unique_email.sql
-- Enforce UNIQUE on users.email (v1.2 SPEC §4.1)
--
-- The original V001 schema had only an INDEX (not UNIQUE) on
-- users.email. SPEC §4.1 mandates:
--     `users.email` UNIQUE NULLS NOT DISTINCT (仅 MySQL 8.0.19+)
-- Without UNIQUE, two accounts can register with the same email —
-- the route handler's pre-check (user_repo::email_exists) catches
-- the same-process case but loses the race against a concurrent
-- INSERT.
--
-- MySQL 8.0.19+ supports NULLS NOT DISTINCT: multiple rows with
-- NULL email are allowed (matches "email is optional"). Pre-8.0.19
-- databases would store each NULL as a distinct value, breaking
-- that.
--
-- Idempotent (per file header comment):
--   - DROP INDEX only if `idx_users_email` exists
--   - ADD CONSTRAINT only if `uq_users_email` is missing
-- 反复执行安全；docker-entrypoint-initdb.d 与 init_db.sh 两套入口均可重入。
-- =============================================================

-- Step 1: drop non-unique index if present
DROP PROCEDURE IF EXISTS migrate_v007_drop_idx;
DELIMITER //
CREATE PROCEDURE migrate_v007_drop_idx()
BEGIN
    IF EXISTS (
        SELECT 1
          FROM information_schema.STATISTICS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'users'
           AND INDEX_NAME   = 'idx_users_email'
    ) THEN
        ALTER TABLE users DROP INDEX idx_users_email;
    END IF;
END //
DELIMITER ;
CALL migrate_v007_drop_idx();
DROP PROCEDURE migrate_v007_drop_idx;

-- Step 2: add UNIQUE constraint if missing
DROP PROCEDURE IF EXISTS migrate_v007_add_uq;
DELIMITER //
CREATE PROCEDURE migrate_v007_add_uq()
BEGIN
    IF NOT EXISTS (
        SELECT 1
          FROM information_schema.TABLE_CONSTRAINTS
         WHERE TABLE_SCHEMA   = DATABASE()
           AND TABLE_NAME     = 'users'
           AND CONSTRAINT_NAME = 'uq_users_email'
    ) THEN
        ALTER TABLE users ADD CONSTRAINT uq_users_email UNIQUE (email);
    END IF;
END //
DELIMITER ;
CALL migrate_v007_add_uq();
DROP PROCEDURE migrate_v007_add_uq;

-- 兼容 init_db.sh 之外的手动重跑场景
INSERT IGNORE INTO schema_migrations (version) VALUES ('V007');