-- =============================================================
-- V009__add_problem_revisions.sql
-- Add problem_revisions table (v1.2.12)
--
-- Records an immutable snapshot of every CREATE / UPDATE on a
-- problem so the v1.3 reader / diff / restore surfaces can replay
-- the content history without re-joining `problems` (which may
-- have been renamed or hard-deleted by then).
--
-- SPEC §11 Phase 3 item (originally tagged "v1.3 考虑", pulled
-- forward to v1.2.12 for the storage layer) + §15.6.
--
-- Idempotent: CREATE TABLE IF NOT EXISTS makes a re-run a no-op.
-- The trailing INSERT IGNORE INTO schema_migrations lets
-- `scripts/init_db.sh` skip the script on re-apply even if a
-- previous run partially failed (mirrors V002 audit_logs).
-- =============================================================

CREATE TABLE IF NOT EXISTS problem_revisions (
    id                BIGINT AUTO_INCREMENT PRIMARY KEY,        -- append-only, BIGINT like audit_logs
    problem_id        INT  NOT NULL,                            -- FK problems(id)
    revision_no       INT  NOT NULL,                            -- per-problem, 1..N
    editor_id         INT  NULL,                                -- FK users(id), SET NULL on delete
    editor_username   VARCHAR(50) NOT NULL,                     -- snapshot, survives user deletion
    editor_ip         VARCHAR(45) NULL,                         -- same column shape as audit_logs.ip
    action            VARCHAR(20) NOT NULL,                     -- 'create' | 'update'
    slug              VARCHAR(100) NOT NULL,
    title             VARCHAR(200) NOT NULL,
    difficulty        VARCHAR(10)  NOT NULL,
    time_limit        INT NOT NULL,
    memory_limit      INT NOT NULL,
    description       MEDIUMTEXT NOT NULL,                      -- 16MB ceiling, same as problems.description
    tags_snapshot     JSON NOT NULL,                            -- array of tag name strings
    samples_snapshot  JSON NOT NULL,                            -- array of {input, output} objects
    summary           VARCHAR(200) NULL,                        -- human-readable "what changed"
    created_at        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    UNIQUE KEY uq_problem_revisions_problem_revno (problem_id, revision_no),
    KEY idx_problem_revisions_problem      (problem_id, revision_no DESC),
    KEY idx_problem_revisions_problem_time (problem_id, created_at  DESC),
    KEY idx_problem_revisions_editor       (editor_id,    created_at DESC),

    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,
    FOREIGN KEY (editor_id)  REFERENCES users(id)    ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Use INSERT IGNORE so a partially-applied prior run won't re-fail.
INSERT IGNORE INTO schema_migrations (version) VALUES ('V009');
