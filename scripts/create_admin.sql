-- ============================================================
-- LiteCode-CPP 创建初始管理员账户
-- 说明: 创建第一个 admin 角色用户，用于管理后台登录
-- 用法: mysql -u root -p litecode < create_admin.sql
--
-- ⚠️  重要：请在生产环境中修改以下默认密码！
--     默认密码: admin123
--     默认 bcrypt hash 对应: admin123
--     bcrypt cost=12 的哈希值，可通过 C++ 代码重新生成
-- ============================================================

USE litecode;

-- 插入初始管理员账户
-- 密码: admin123 (bcrypt cost=12)
-- ⚠️  生产环境务必修改密码！首次登录后请立即更换
INSERT INTO users (username, password_hash, role, email)
VALUES (
    'admin',
    '$2b$12$1GNHqZ5bd2FtnwmyU0VK0OhkHIMtkdCb/8lXwFnxlm1WCtAW7GCCW',
    'admin',
    'admin@litecode.local'
)
ON DUPLICATE KEY UPDATE username = username;
-- ON DUPLICATE KEY UPDATE 确保重复运行不会报错

-- ============================================================
-- 验证管理员已创建（可选）
-- SELECT id, username, role, created_at FROM users WHERE role = 'admin';
-- ============================================================