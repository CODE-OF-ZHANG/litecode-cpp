-- =============================================================
-- V016__clean_e2e_and_perf_users.sql
-- v1.3.4 PR 12 ★ 数据清洗(补 V015 的漏)
--
-- V015 只删 ^test_/^demo_/^bot_,但 scripts/e2e_acceptance.sh /
-- scripts/load_test.sh / scripts/fuzz_judge.sh / scripts/demo_*.py
-- 通过 /api/v1/auth/register 写入的账号是 e2e_u_* / e2e_rl_reg_* /
-- e2e_lock_* / e2e_audit_* / lc_perf_* / normaluser_test 形态,
-- 不在 V015 过滤范围内,污染了 /ranking.html 视图。
--
-- 保留名单(硬编码,与 scripts/purge_users.sh --keep 默认一致):
--   - admin      (V099 seed 的超级管理员)
--   - zhangxu    (项目 owner,业务真实账号)
-- 其它 user 一律视为测试残留删除(白名单与前缀过滤双保险)。
--
-- 级联影响(由现有 FK 决定):
--   - submissions.user_id        ON DELETE CASCADE → 一起删
--   - refresh_tokens.user_id     ON DELETE CASCADE → 一起删
--   - audit_logs.user_id / admin_id → 多数 SET NULL
--   - problem_revisions.editor_id   → 多数 SET NULL(保留历史归属)
-- 排行榜聚合 CTE 在 user 删后自然不返回,无需额外清理。
--
-- Idempotent: 同 V015 风格,DELETE 多次执行安全(第二次 0 行)。
-- 手动执行(参考 memory v1.3.3 启动踩坑):
--   docker exec -i -e MYSQL_PWD="$MYSQL_ROOT_PASSWORD" litecode-mysql \
--     mysql -uroot litecode < db/migrations/V016__clean_e2e_and_perf_users.sql
-- 验收:
--   docker exec -i -e MYSQL_PWD="$MYSQL_ROOT_PASSWORD" litecode-mysql \
--     mysql -uroot litecode -N -B -e "SELECT COUNT(*) FROM users"
--   期望返回 2(admin + zhangxu)。
-- =============================================================

-- ── 1. 备份保护:删除前先看一眼命中数 ────────────────────────────
SELECT
    SUM(username REGEXP '^e2e_')        AS e2e_users,
    SUM(username REGEXP '^lc_perf_')    AS lc_perf_users,
    SUM(username REGEXP '^normaluser_') AS normaluser_users,
    SUM(username REGEXP '^stress_')     AS stress_users,
    SUM(username REGEXP '^load_')       AS load_users,
    SUM(username REGEXP '^fuzz_')       AS fuzz_users
  FROM users;

-- ── 2. 正式删除:白名单保险 + 前缀过滤双保险 ─────────────────────
START TRANSACTION;
DELETE FROM users
 WHERE username NOT IN ('admin', 'zhangxu')
   AND (  username REGEXP '^e2e_'
       OR username REGEXP '^lc_perf_'
       OR username REGEXP '^normaluser_'
       OR username REGEXP '^stress_'
       OR username REGEXP '^load_'
       OR username REGEXP '^fuzz_'
       );
COMMIT;

-- ── 3. 校验:users 表只剩 admin + zhangxu ────────────────────────
SELECT id, username, role, created_at, last_login
  FROM users
 ORDER BY id;

-- ── 4. schema_migrations 记录(同 V015 风格)─────────────────────
INSERT IGNORE INTO schema_migrations (version) VALUES ('V016');
