-- =============================================================
-- V003__add_judge_type.sql
-- Add judge_type + float_epsilon to test_cases (v1.2)
--
-- Per SPEC §4.3: judge_type controls how outputs are compared.
-- MVP supports exact / ignore_trailing / float_eps / special.
-- =============================================================

ALTER TABLE test_cases
    ADD COLUMN judge_type    ENUM('exact','ignore_trailing','float_eps','special')
                             NOT NULL DEFAULT 'exact' AFTER is_sample,
    ADD COLUMN float_epsilon DECIMAL(10,8) NULL AFTER judge_type;

INSERT INTO schema_migrations (version) VALUES ('V003');