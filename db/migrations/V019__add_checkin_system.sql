-- =============================================================
-- V019__add_checkin_system.sql
-- 每日打卡系统
--
-- 新增表:
--   - checkin_records: 用户每日打卡记录
--
-- 修改表:
--   - users: 新增 current_streak, longest_streak, last_checkin 字段
--
-- 打卡规则:
--   - 用户每天 AC 至少 1 道题即打卡成功
--   - 中断则 current_streak 归零,历史记录保留
-- =============================================================

-- ── 1. checkin_records 表 ─────────────────────────────────────
CREATE TABLE IF NOT EXISTS checkin_records (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    user_id         INT NOT NULL,
    checkin_date    DATE NOT NULL,
    problem_id      INT NULL,                           -- 触发打卡的题目（可空）
    submission_id   INT NULL,                           -- 对应的 submission id
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id)       REFERENCES users(id)       ON DELETE CASCADE,
    FOREIGN KEY (problem_id)   REFERENCES problems(id)      ON DELETE SET NULL,
    FOREIGN KEY (submission_id) REFERENCES submissions(id)   ON DELETE SET NULL,
    UNIQUE KEY uk_user_date (user_id, checkin_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ── 2. users 表新增打卡字段 ────────────────────────────────────
ALTER TABLE users
    ADD COLUMN current_streak  INT NOT NULL DEFAULT 0 COMMENT '当前连续打卡天数',
    ADD COLUMN longest_streak  INT NOT NULL DEFAULT 0 COMMENT '历史最长连续天数',
    ADD COLUMN last_checkin    DATE NULL COMMENT '上次打卡日期';

-- ── 3. schema_migrations 记录 ──────────────────────────────────
INSERT IGNORE INTO schema_migrations (version) VALUES ('V019');
