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
-- This migration is idempotent — IF EXISTS guards the case where
-- V001 was patched to add UNIQUE before V007 existed.
-- =============================================================

-- Drop the non-unique index that V001 created; replace with a UNIQUE one.
ALTER TABLE users DROP INDEX idx_users_email;
ALTER TABLE users ADD CONSTRAINT uq_users_email UNIQUE (email);

INSERT INTO schema_migrations (version) VALUES ('V007');