-- =============================================================
-- V008__add_finished_at.sql
-- Add submissions.finished_at (SPEC §4.4)
--
-- 补 V001 遗漏：submissions 表的 `finished_at DATETIME` 字段用于记录
-- 判题完成时间（区别于 created_at = 入队时间），后续：
--   - 算队列等待时长 = finished_at - created_at
--   - 算实际判题耗时 = finished_at - <真正进入 running 时刻>
--   - 数据保留策略按 finished_at 清理失败提交
--
-- 幂等：V008 只在字段/索引不存在时 ADD COLUMN / CREATE INDEX，
-- 反复执行安全。schema_migrations 那行由 init_db.sh 去重，
-- 但单独执行 V008 时也能容忍 schema_migrations 已有 V008。
-- =============================================================

-- Step 1: ADD COLUMN only if missing
DROP PROCEDURE IF EXISTS migrate_v008_add_finished_at;
DELIMITER //
CREATE PROCEDURE migrate_v008_add_finished_at()
BEGIN
    IF NOT EXISTS (
        SELECT 1
          FROM information_schema.COLUMNS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'submissions'
           AND COLUMN_NAME  = 'finished_at'
    ) THEN
        ALTER TABLE submissions
            ADD COLUMN finished_at DATETIME NULL AFTER created_at;
    END IF;
END //
DELIMITER ;
CALL migrate_v008_add_finished_at();
DROP PROCEDURE migrate_v008_add_finished_at;

-- Step 2: CREATE INDEX only if missing
DROP PROCEDURE IF EXISTS migrate_v008_add_idx;
DELIMITER //
CREATE PROCEDURE migrate_v008_add_idx()
BEGIN
    IF NOT EXISTS (
        SELECT 1
          FROM information_schema.STATISTICS
         WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME   = 'submissions'
           AND INDEX_NAME   = 'idx_submissions_finished'
    ) THEN
        CREATE INDEX idx_submissions_finished ON submissions (finished_at);
    END IF;
END //
DELIMITER ;
CALL migrate_v008_add_idx();
DROP PROCEDURE migrate_v008_add_idx;

-- Step 3: track migration (safe to re-apply; init_db.sh uses
-- ON DUPLICATE semantics via INSERT IGNORE if you switch it,
-- but plain INSERT is fine here since init_db.sh is the canonical
-- path and it skips already-applied versions).
INSERT IGNORE INTO schema_migrations (version) VALUES ('V008');