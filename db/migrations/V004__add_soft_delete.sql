-- =============================================================
-- V004__add_soft_delete.sql
-- Add is_deleted + updated_at to problems, last_login_ip to users (v1.2)
--
-- Soft delete avoids breaking historical submissions.problem_id FK.
-- updated_at records last modification time for audit trail.
-- last_login_ip captures login IP for security audit (SPEC §15.6).
-- =============================================================

-- problems: soft delete + updated_at
ALTER TABLE problems
    ADD COLUMN is_deleted BOOLEAN     NOT NULL DEFAULT FALSE AFTER submission_count,
    ADD COLUMN updated_at DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP
                                ON UPDATE CURRENT_TIMESTAMP AFTER created_at;

-- users: track login IP (IPv4 / IPv6)
ALTER TABLE users
    ADD COLUMN last_login_ip VARCHAR(45) NULL AFTER last_login;

INSERT INTO schema_migrations (version) VALUES ('V004');