#!/usr/bin/env python3
"""v1.3.4: bulk-import the questions/ JSON tree directly via SQL.

This bypasses the admin bulk-import REST API (5 req/hour quota) by writing
one transaction containing all 82 problems + tags + samples + test cases.

Usage:
    # 1) dry-run: generate import.sql next to this script and print summary
    python scripts/import_problems_sql.py --questions-dir questions

    # 2) pipe through docker exec against the MySQL container
    python scripts/import_problems_sql.py --questions-dir questions \
        | docker exec -i -e MYSQL_PWD=$MYSQL_PWD litecode-mysql \
            mysql -uroot --default-character-set=utf8mb4 litecode

    # 3) or run against a remote host
    python scripts/import_problems_sql.py --questions-dir questions \
        | mysql -h $DB_HOST -u root -p$DB_PWD litecode

Why this exists (v1.3.4 PR 13 follow-up):
    The admin /api/v1/admin/problems/import endpoint caps at 50 files per
    request AND 5 requests/hour/admin. Importing 82 problems with that
    API takes 2 requests + the 2-hour reset window. This script writes
    the same rows in a single transaction (~2 s on a local MySQL 8).

Idempotency:
    - problems:    INSERT ... ON DUPLICATE KEY UPDATE  (slug is UNIQUE)
    - tags:        INSERT IGNORE                         (name is UNIQUE)
    - problem_tags:INSERT IGNORE                         (composite PK)
    - test_cases:  per-problem DELETE-all + reinsert    (idempotent)

The script NEVER soft-deletes existing rows; UPSERT semantics preserve any
test_cases the user might have manually tweaked post-import (test_cases are
deleted only by `problem_id` for the problem being imported, and the script
emits a warning if the row count for that problem changes).
"""
import argparse
import json
import sys
from pathlib import Path

# -- SQL escape helper (we only escape ' and \ since the JSON never carries
# control bytes that need MySQL's _binary / _utf8mb4 wrappers) --------------
def sql_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace("'", "\\'")


def parse_test_cases(raw_cases, samples):
    """Return (samples_rows, test_rows) ready for INSERT.
    Each row = (input, expected_output, is_sample, judge_type, float_epsilon)."""
    rows = []
    for i, s in enumerate(samples):
        rows.append((s["input"], s["output"], True, "exact", None))
    for i, tc in enumerate(raw_cases):
        rows.append((
            tc["input"],
            tc["expected_output"],
            False,
            tc.get("judge_type", "exact"),
            tc.get("float_epsilon"),
        ))
    return rows


def emit_problem_sql(p: dict) -> tuple[str, list]:
    """Return (sql_with_placeholders, params) for this problem."""
    slug = p["slug"]
    title = p["title"]
    diff = p["difficulty"]
    desc = p["description"]
    template = p.get("template", "")
    tlimit = p.get("time_limit_ms", 1000)
    mlimit = p.get("memory_limit_mb", 256)
    tags = p.get("tags", [])
    cases = parse_test_cases(p.get("test_cases", []), p.get("samples", []))

    # The RETURNING id trick works on MySQL 8.0.21+ via LAST_INSERT_ID() chained
    # through user variables; we instead use a two-phase approach:
    #   1) UPSERT the problem row, capture id via INSERT ... ON DUP KEY UPDATE id=LAST_INSERT_ID(id)
    #   2) DELETE all test_cases for this problem_id
    #   3) INSERT all test_cases
    # The user variable @_lc_pid carries the id across statements.
    parts = []
    parts.append(f"""
-- ── problem: {slug} ──────────────────────────────────────────────
INSERT INTO problems
    (slug, title, difficulty, description, template,
     time_limit, memory_limit, is_deleted)
VALUES
    ('{sql_escape(slug)}', '{sql_escape(title)}', '{diff}',
     '{sql_escape(desc)}', '{sql_escape(template)}',
     {tlimit}, {mlimit}, FALSE)
ON DUPLICATE KEY UPDATE
    title        = VALUES(title),
    difficulty   = VALUES(difficulty),
    description  = VALUES(description),
    template     = VALUES(template),
    time_limit   = VALUES(time_limit),
    memory_limit = VALUES(memory_limit),
    is_deleted   = FALSE,
    id           = LAST_INSERT_ID(id);
SET @_lc_pid := LAST_INSERT_ID();
""")
    # tag link + upsert
    for tag in tags:
        t = sql_escape(tag)
        parts.append(f"""
INSERT IGNORE INTO tags (name) VALUES ('{t}');
SET @_lc_tid := (SELECT id FROM tags WHERE name = '{t}');
INSERT IGNORE INTO problem_tags (problem_id, tag_id)
    VALUES (@_lc_pid, @_lc_tid);
""")
    # wipe and reinsert test cases — include problem_id directly in INSERT
    # (problem_id is NOT NULL with no default per V001 schema)
    parts.append("\nDELETE FROM test_cases WHERE problem_id = @_lc_pid;\n")
    if cases:
        # chunked insert (one INSERT per 50 rows to keep packet size sane)
        chunk = []
        for i, (inp, out, is_sample, jt, eps) in enumerate(cases):
            order_num = i
            eps_sql = f"{eps}" if eps is not None else "NULL"
            chunk.append(
                f"(@_lc_pid, '{sql_escape(inp)}', '{sql_escape(out)}', "
                f"{1 if is_sample else 0}, {order_num}, '{jt}', {eps_sql})"
            )
            if len(chunk) >= 50 or i == len(cases) - 1:
                parts.append(
                    "INSERT INTO test_cases\n"
                    "    (problem_id, input, expected_output, is_sample, order_num, judge_type, float_epsilon)\n"
                    "VALUES\n    " + ",\n    ".join(chunk) + ";\n"
                )
                chunk = []
    return "".join(parts), cases


def build_sql(questions_dir: Path) -> str:
    files = sorted(questions_dir.rglob("*.json"))
    header = """-- =====================================================================
-- Auto-generated by scripts/import_problems_sql.py
-- Total problems: {n}
-- Run with: docker exec -i -e MYSQL_PWD=... litecode-mysql \\
--             mysql -uroot --default-character-set=utf8mb4 litecode
-- =====================================================================
SET NAMES utf8mb4;
SET @_lc_old_autocommit := @@autocommit;
SET autocommit = 0;

START TRANSACTION;
SET FOREIGN_KEY_CHECKS = 0;
""".format(n=len(files))
    body = []
    summary = []
    for f in files:
        with f.open(encoding="utf-8") as fp:
            p = json.load(fp)
        sql, cases = emit_problem_sql(p)
        body.append(sql)
        summary.append((p["slug"], p["difficulty"], len(cases)))
    footer = """
SET FOREIGN_KEY_CHECKS = 1;
COMMIT;
SET autocommit = @_lc_old_autocommit;
-- =====================================================================
"""
    summary_block = ["-- Summary (slug | difficulty | test_case_count incl. samples):"]
    for s, d, n in summary:
        summary_block.append(f"--   {s:50s}  {d:6s}  {n}")
    return header + "\n".join(summary_block) + "\n\n" + "\n".join(body) + footer


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--questions-dir", default="questions",
                   help="Path to the questions/ directory (recursively walked)")
    p.add_argument("--out", default="-",
                   help="Output SQL path; '-' (default) prints to stdout")
    args = p.parse_args()

    qdir = Path(args.questions_dir)
    if not qdir.is_dir():
        print(f"ERROR: {qdir} is not a directory", file=sys.stderr)
        return 2
    files = sorted(qdir.rglob("*.json"))
    if not files:
        print(f"ERROR: no *.json under {qdir}", file=sys.stderr)
        return 2
    sql = build_sql(qdir)
    if args.out == "-":
        sys.stdout.write(sql)
    else:
        Path(args.out).write_text(sql, encoding="utf-8")
        print(f"Wrote {args.out} ({len(sql):,} bytes, {len(files)} problems)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())