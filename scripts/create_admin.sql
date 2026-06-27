-- =============================================================
-- LiteCode-CPP — 初始管理员种子
-- -------------------------------------------------------------
-- 创建第一个 admin 角色用户，用于管理后台登录
-- 默认凭据：
--   Username: admin
--   Password: admin123!
--
-- ⚠️  生产环境务必修改密码！首次登录后请立即更换。
--
-- bcrypt hash 是用项目自带的 src/auth/password_hash.h 生成（cost=12）：
--   ./build/bin/Release/litecode.exe admin123!
--   → $2b$12$pCLRON1sZnHvDPtpVWvSOe7VTeABJpEp76U8mI7AnsVut50oh/Wue
--
-- 若要重置密码（例如部署到生产前），可在 MySQL 命令行：
--   ALTER TABLE users
--     CHANGE password_hash password_hash VARCHAR(255) NOT NULL;
--   -- 然后用 litecode.exe '新密码' 生成 hash，UPDATE users SET password_hash='<hash>' WHERE username='admin';
-- =============================================================

-- 幂等：重复执行不会插入第二行
INSERT INTO users (username, password_hash, role, email, avatar)
VALUES (
    'admin',
    '$2b$12$pCLRON1sZnHvDPtpVWvSOe7VTeABJpEp76U8mI7AnsVut50oh/Wue',
    'admin',
    NULL,
    '/assets/img/default-avatar.svg'
)
ON DUPLICATE KEY UPDATE
    -- 已有 admin 时仅更新密码哈希（便于开发期快速重置）
    password_hash = VALUES(password_hash),
    avatar        = VALUES(avatar);

-- 验证（取消注释可手动检查）
-- SELECT id, username, role, email, created_at FROM users WHERE role = 'admin';