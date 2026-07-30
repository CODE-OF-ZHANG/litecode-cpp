-- SPDX-License-Identifier: MIT
--
-- LiteCode-CPP — V020: add notifications system (Phase 7 ★)
--
-- 通知系统：存储用户通知，支持 SSE 实时推送

START TRANSACTION;

CREATE TABLE IF NOT EXISTS notifications (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    user_id         INT NOT NULL,                -- 接收通知的用户
    type            VARCHAR(50) NOT NULL,         -- ac_result | discussion_reply | solution_like | checkin_streak
    message         VARCHAR(500) NOT NULL,        -- 通知文本
    link            VARCHAR(255) NULL,             -- 点击跳转链接
    reference_id    INT NULL,                     -- 关联ID（讨论ID/题解ID等）
    is_read         BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    INDEX idx_notifications_user_unread (user_id, is_read, created_at DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

COMMIT;
