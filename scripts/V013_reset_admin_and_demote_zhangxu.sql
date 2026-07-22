-- =============================================================
-- LiteCode-CPP — admin 账号密码重置 + zhangxu 降权 (v1.3.3.7)
-- -------------------------------------------------------------
-- 由 scripts/admin_tool.py 引导执行,也可手动通过 docker exec:
--
--   docker exec -i litecode-mysql mysql -uroot -p"$MYSQL_ROOT_PASSWORD" litecode \
--     < scripts/V013_reset_admin_and_demote_zhangxu.sql
--
-- ⚠️ 注意:这不是一个 Flyway 迁移,而是一次性运维脚本。
--   - 没有 INSERT INTO schema_migrations 那行 — Flyway 不该知道它跑过
--   - 写入 db/migrations/ 目录会污染新建部署的初始化路径(已经在 V099
--     里 seed admin 了),所以单独放在 scripts/ 下
--
-- 行为:
--   1. 把 username='admin' 的 password_hash 重置成 admin123 的 bcrypt 哈希
--      (cost=12,与 src/auth/password_hash.h 的 hash_password() 同源)
--      ON DUPLICATE KEY UPDATE — admin 不存在时插入,已存在时刷新密码
--   2. 把 username='zhangxu' 的 role 强制降为 'user'
--      (即便后续通过 admin 后台又把他提到 admin,这里也只能执行一次;
--       admin_tool.py 会再跑一次就再降一次)
--   3. 保留其余 admin — 只精确撤 zhangxu 一人,避免误伤
--   4. 末尾 SELECT 打印当前 admin 列表 + zhangxu 的最新 role,方便人工复核
--
-- 密码策略说明:
--   admin123 长度 8 + 字母 + 数字,刚好满足 SPEC §4.1 / password_hash.h
--   的最低门槛。生产部署请跑 admin_tool.py reset-admin 后立即在
--   管理后台改成强密码。
-- =============================================================

-- ── 1. admin 账号:密码重置为 admin123 ───────────────────────────────
-- bcrypt $2b$12$5jgfIpZUxAmv4qAP1sCIlO0ysrl9QprxoxxaqSjBoLq.DEvk0Snd2
-- 由 bcrypt.hashpw(b'admin123', bcrypt.gensalt(rounds=12, prefix=b'2b')) 生成
-- 与 src/auth/password_hash.h::hash_password 输出格式兼容
INSERT INTO users (username, password_hash, role, email, avatar)
VALUES (
    'admin',
    '$2b$12$5jgfIpZUxAmv4qAP1sCIlO0ysrl9QprxoxxaqSjBoLq.DEvk0Snd2',
    'admin',
    NULL,
    '/assets/img/default-avatar.svg'
)
ON DUPLICATE KEY UPDATE
    -- 已有 admin 时只刷密码哈希,保留其它字段(email/avatar/last_login)
    password_hash = VALUES(password_hash);

-- ── 2. zhangxu:精确降权到 user,不动其它 admin ───────────────────────
UPDATE users
   SET role = 'user'
 WHERE username = 'zhangxu'
   AND role     = 'admin';

-- ── 3. 复核:打印当前所有 admin 与 zhangxu 的最新 role ─────────────────
SELECT id, username, role, email, created_at, last_login
  FROM users
 WHERE role = 'admin' OR username = 'zhangxu'
 ORDER BY role DESC, username ASC;