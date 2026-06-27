-- =============================================================
-- V006__add_ole_se_status.sql
-- Extend submissions.status ENUM with ole/se (v1.2)
--
-- Per SPEC §4.4 / §3.2: ole = Output Limit Exceeded (>16MB),
--                       se  = System Error (基础设施异常)
--
-- 补 V005 的遗漏：V001 创建 submissions 时用的是 v1.1 状态集，
-- 没有把 v1.2 新增的 ole/se 加进去。
-- =============================================================

ALTER TABLE submissions
    MODIFY COLUMN status
        ENUM('pending','running','ac','wa','re','tle','mle','ole','pe','ce','se')
        NOT NULL DEFAULT 'pending';

INSERT INTO schema_migrations (version) VALUES ('V006');