-- =============================================================
-- V005__add_indexes.sql
-- Composite indexes for query optimization (v1.2)
--
-- Based on SPEC §4.5 索引建议. These cover the hot read paths:
--  - problem list pagination + difficulty filter + soft-delete filter
--  - test-case sample display
--  - submission history / queue polling / data retention
--  - audit log queries by admin
--  - reverse problem-tag lookup
-- =============================================================

-- problems
CREATE INDEX idx_problems_list         ON problems (is_deleted, difficulty, created_at);
CREATE INDEX idx_problems_slug_active   ON problems (is_deleted, slug);

-- test_cases
CREATE INDEX idx_test_cases_problem_order ON test_cases (problem_id, order_num);
CREATE INDEX idx_test_cases_sample         ON test_cases (problem_id, is_sample, order_num);

-- submissions (hot paths: history / queue / retention)
CREATE INDEX idx_submissions_user_history  ON submissions (user_id, problem_id, created_at DESC);
CREATE INDEX idx_submissions_problem_status ON submissions (problem_id, status, created_at);
CREATE INDEX idx_submissions_queue         ON submissions (status, created_at);
CREATE INDEX idx_submissions_created       ON submissions (created_at);

-- problem_tags: reverse lookup "all problems under tag X"
CREATE INDEX idx_problem_tags_tag ON problem_tags (tag_id, problem_id);

-- audit_logs: index already created inline in V002; keep here for clarity

INSERT INTO schema_migrations (version) VALUES ('V005');