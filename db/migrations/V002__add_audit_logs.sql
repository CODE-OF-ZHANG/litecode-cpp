-- =============================================================
-- V002__add_audit_logs.sql
-- Add audit_logs table (v1.2 new)
--
-- Records admin write operations for security auditing.
-- Required by SPEC §15.6 操作审计.
-- =============================================================

CREATE TABLE audit_logs (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    admin_id    INT NULL,                          -- 操作管理员（NULL = 系统/匿名失败登录）
    action      VARCHAR(50)  NOT NULL,             -- e.g. problem.create / problem.delete / user.role_change
    target_type VARCHAR(50)  NULL,                 -- e.g. problem / user / tag
    target_id   VARCHAR(100) NULL,                 -- object ID (slug, user id, ...)
    payload     JSON         NULL,                 -- 变更详情（删除前快照、改角色前后值等）
    ip          VARCHAR(45)  NULL,                 -- IPv4 / IPv6
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,

    INDEX idx_audit_admin_time (admin_id, created_at DESC),
    INDEX idx_audit_action_time (action, created_at),

    FOREIGN KEY (admin_id) REFERENCES users(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO schema_migrations (version) VALUES ('V002');