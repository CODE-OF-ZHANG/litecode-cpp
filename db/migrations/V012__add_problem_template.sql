-- =============================================================
-- V012__add_problem_template.sql
-- Add problems.template column (v1.3.2 — frontend redesign + per-problem code template)
--
-- Per SPEC §4.2 extension + Phase 4 ★ frontend polish (v1.3.2 round):
--   - problems.template : per-problem code template injected into the CodeMirror
--                         editor on problem.html load. Nullable MEDIUMTEXT;
--                         NULL or empty string means "fall back to the built-in
--                         C++/C skeleton in web/js/editor.js".
--   - 16 MB ceiling      — same as problems.description.
--   - Position           : AFTER description, keeping the "content" pair
--                         (description + template) clustered in the row.
--
-- Idempotent : INFORMATION_SCHEMA prepare-stmt pattern (matches V011's
-- "INSERT IGNORE INTO schema_migrations" double-guard). init_db.sh can
-- re-run on a partially-applied prior pass without throwing.
--
-- Down-migration is intentionally NOT shipped — re-creating rows from
-- scratch would have to walk problems.template = '' rows anyway, so a
-- `V012_down__drop_problem_template.sql` is left as a future ledger row.
-- =============================================================

SET @col_exists := (
    SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'problems'
      AND COLUMN_NAME  = 'template'
);
SET @sql := IF(@col_exists = 0,
    'ALTER TABLE problems ADD COLUMN template MEDIUMTEXT NULL AFTER description',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

INSERT IGNORE INTO schema_migrations (version) VALUES ('V012');
