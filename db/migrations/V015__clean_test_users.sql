-- =============================================================
-- V015__clean_test_users.sql
-- v1.3.4 PR 11 ★ 数据清洗 + SPEC §5.1
--
-- 清掉历史遗留的测试用户 test_xxx / demo_xxx / bot_xxx。
-- 这些是开发期反复注册用来跑 e2e / stress test 的账号,
-- 不应该出现在生产 ranking / profile 数据里污染真实用户。
--
-- 不动的用户:
--   - V099 seed 的 admin 账户(已硬编码为唯一超级管理员,本 PR 加 server-side 锁定)
--   - V014 已存的真实用户(普通 user + 其他 admin,只要 username 不匹配 ^test_/^demo_/^bot_)
--   - submissions / audit_logs 等其他表的 FK 关联记录:
--       * 删除 user 时 FK 会级联清理 dependent rows
--       * audit_logs.action='login' / 'submit' 等引用的 user_id 也一并消失
--       * 排行榜聚合走的 CTE/SUM 在 user 删后自然不返回,无需额外清理
--
-- Idempotent:
--   - DELETE 多次执行安全(第二次 0 行)
--   - INSERT IGNORE INTO schema_migrations 重复执行也安全
--   - init_db.sh 看到 schema_migrations 里有 V015 就自动跳过
--
-- 手动执行(参考 memory v1.3.3 启动踩坑):
--   docker exec -i -e MYSQL_PWD="$MYSQL_ROOT_PASSWORD" litecode-mysql \
--     mysql -uroot litecode < db/migrations/V015__clean_test_users.sql
--
-- 验收(执行后必须返回 0):
--   docker exec -i -e MYSQL_PWD="$MYSQL_ROOT_PASSWORD" litecode-mysql \
--     mysql -uroot litecode -N -B -e \
--       "SELECT COUNT(*) FROM users
--        WHERE username REGEXP '^test_'
--           OR username REGEXP '^demo_'
--           OR username REGEXP '^bot_'"
-- =============================================================

-- 备份保护:删除前先看一眼命中数
SELECT
    SUM(username REGEXP '^test_') AS test_users,
    SUM(username REGEXP '^demo_') AS demo_users,
    SUM(username REGEXP '^bot_')  AS bot_users
  FROM users;

-- 正式删除
DELETE FROM users
 WHERE username REGEXP '^test_'
    OR username REGEXP '^demo_'
    OR username REGEXP '^bot_';

-- 校验:必须返回 0
SELECT COUNT(*) AS remaining_test_like
  FROM users
 WHERE username REGEXP '^test_'
    OR username REGEXP '^demo_'
    OR username REGEXP '^bot_';

-- 兼容 init_db.sh 的版本记录(同 V014 风格)
INSERT IGNORE INTO schema_migrations (version) VALUES ('V015');