-- =============================================================
-- V010__add_problem_special_judges.sql
-- Add problem_special_judges table (Phase 4 Special Judge framework)
--
-- Per SPEC §4.3 + §11 Phase 4 ★ "Special Judge 框架":
-- judge_type='special' was added to test_cases by V003 but the
-- matching per-problem SPJ source had no schema, so judge.sh was
-- hard-coded to return SE for every special-type case. This commit
-- provisions that storage.
--
-- One row per problem that opts into special-judge evaluation. The
-- row carries the SPJ source code in MEDIUMTEXT (the SPEC §7.4
-- 16MB MEDIUMTEXT cap; admin paths will clamp at upload time so the
-- wire shape mirrors problem_repo's `description` field). `language`
-- is reserved for a future day where SPJ programs could be written
-- in Python / Java / etc.; today only 'cpp' is supported (the
-- litecode-judge image is C++-only, see judge/Dockerfile).
--
-- ON DELETE CASCADE on the problem_id FK mirrors problem_tags'
-- (problem_id) FK semantics: a hard-deleted problem scrubs its
-- SPJ along with it; a soft-deleted problem keeps the row intact
-- (matches the soft-delete policy for test_cases).
--
-- Idempotent: CREATE TABLE IF NOT EXISTS keeps a re-run a no-op;
-- INSERT IGNORE INTO schema_migrations gates the version-tracking
-- record so scripts/init_db.sh skips on the second pass.
-- =============================================================

CREATE TABLE IF NOT EXISTS problem_special_judges (
    problem_id    INT NOT NULL,                            -- FK problems(id)
    source        MEDIUMTEXT NOT NULL,                     -- SPJ source code (C++)
    language      ENUM('cpp') NOT NULL DEFAULT 'cpp',      -- reserved for future (py/java/...)
    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                                          ON UPDATE CURRENT_TIMESTAMP,

    PRIMARY KEY (problem_id),
    CONSTRAINT fk_problem_special_judges_problem
        FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Track the migration; INIT IGNORE makes a partially-applied prior run
-- not re-fail. init_db.sh dedupes on the version column.
INSERT IGNORE INTO schema_migrations (version) VALUES ('V010');
