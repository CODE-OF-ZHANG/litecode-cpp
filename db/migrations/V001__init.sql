-- =============================================================
-- V001__init.sql
-- Initial schema (v1.1 baseline)
-- Creates core tables: schema_migrations, users, problems, tags,
--                      problem_tags, test_cases, submissions
--
-- Note: This migration recreates the v1.1 schema. Later migrations
--       (V002-V005) add v1.2 fields (audit_logs, judge_type,
--       is_deleted, last_login_ip, composite indexes).
-- =============================================================

-- Migration version tracking (Flyway-style)
CREATE TABLE IF NOT EXISTS schema_migrations (
    version    VARCHAR(20) PRIMARY KEY,
    applied_at DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------
-- users
-- ----------------------------
CREATE TABLE users (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(50)  NOT NULL UNIQUE,
    password_hash VARCHAR(255) NOT NULL,
    role          ENUM('user','admin') NOT NULL DEFAULT 'user',
    email         VARCHAR(100) NULL,
    avatar        VARCHAR(255) NULL,
    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login    DATETIME NULL,
    INDEX idx_users_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------
-- problems
-- ----------------------------
CREATE TABLE problems (
    id               INT AUTO_INCREMENT PRIMARY KEY,
    slug             VARCHAR(100)  NOT NULL UNIQUE,
    title            VARCHAR(200)  NOT NULL,
    difficulty       ENUM('easy','medium','hard') NOT NULL,
    description      MEDIUMTEXT    NOT NULL,
    time_limit       INT NOT NULL DEFAULT 1000,
    memory_limit     INT NOT NULL DEFAULT 256,
    accepted_count   INT NOT NULL DEFAULT 0,
    submission_count INT NOT NULL DEFAULT 0,
    created_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------
-- tags
-- ----------------------------
CREATE TABLE tags (
    id   INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(50) NOT NULL UNIQUE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------
-- problem_tags (M:N)
-- ----------------------------
CREATE TABLE problem_tags (
    problem_id INT NOT NULL,
    tag_id     INT NOT NULL,
    PRIMARY KEY (problem_id, tag_id),
    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id)     REFERENCES tags(id)     ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------
-- test_cases
-- ----------------------------
CREATE TABLE test_cases (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    problem_id      INT NOT NULL,
    input           LONGTEXT NOT NULL,
    expected_output LONGTEXT NOT NULL,
    is_sample       BOOLEAN NOT NULL DEFAULT FALSE,
    order_num       INT NOT NULL DEFAULT 0,
    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ----------------------------
-- submissions
-- ----------------------------
CREATE TABLE submissions (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    user_id       INT NOT NULL,
    problem_id    INT NOT NULL,
    language      ENUM('c','cpp') NOT NULL,
    code          LONGTEXT NOT NULL,
    status        ENUM('pending','running','ac','wa','re','tle','mle','pe','ce')
                  NOT NULL DEFAULT 'pending',
    time_used     INT NULL,    -- ms
    memory_used   INT NULL,    -- KB
    error_message TEXT NULL,
    created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Record migration as applied
INSERT INTO schema_migrations (version) VALUES ('V001');