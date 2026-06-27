-- ============================================================
-- LiteCode-CPP 数据库初始化脚本
-- 版本: v1.1 MVP
-- 说明: 创建所有数据表及索引
-- 用法: mysql -u root -p < init_db.sql
-- ============================================================

-- 创建数据库（如果不存在）
CREATE DATABASE IF NOT EXISTS litecode
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE litecode;

-- ============================================================
-- 1. users 表 — 用户账户
-- ============================================================
CREATE TABLE IF NOT EXISTS users (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(50)    NOT NULL UNIQUE,
    password_hash VARCHAR(255)   NOT NULL COMMENT 'bcrypt 哈希',
    role          ENUM('user', 'admin') NOT NULL DEFAULT 'user',
    email         VARCHAR(100)   UNIQUE COMMENT '可选',
    avatar        VARCHAR(255)   DEFAULT '/avatars/default.png',
    created_at    DATETIME       NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login    DATETIME       NULL ON UPDATE CURRENT_TIMESTAMP,

    INDEX idx_users_username (username),
    INDEX idx_users_role (role)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 2. problems 表 — 题目
-- ============================================================
CREATE TABLE IF NOT EXISTS problems (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    slug            VARCHAR(100)   NOT NULL UNIQUE COMMENT 'URL 标识，如 two-sum',
    title           VARCHAR(200)   NOT NULL,
    difficulty      ENUM('easy', 'medium', 'hard') NOT NULL DEFAULT 'easy',
    description     TEXT           NOT NULL COMMENT 'Markdown 格式题目描述',
    time_limit      INT            NOT NULL DEFAULT 1000 COMMENT '时间限制(ms)',
    memory_limit    INT            NOT NULL DEFAULT 256 COMMENT '内存限制(MB)',
    accepted_count  INT            NOT NULL DEFAULT 0,
    submission_count INT           NOT NULL DEFAULT 0,
    created_at      DATETIME       NOT NULL DEFAULT CURRENT_TIMESTAMP,

    INDEX idx_problems_difficulty (difficulty),
    INDEX idx_problems_slug (slug)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 3. tags 表 — 标签
-- ============================================================
CREATE TABLE IF NOT EXISTS tags (
    id    INT AUTO_INCREMENT PRIMARY KEY,
    name  VARCHAR(50) NOT NULL UNIQUE COMMENT '标签名称，如"数组""哈希表"',

    INDEX idx_tags_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 4. problem_tags 表 — 题目-标签关联（多对多）
-- ============================================================
CREATE TABLE IF NOT EXISTS problem_tags (
    problem_id  INT NOT NULL,
    tag_id      INT NOT NULL,

    PRIMARY KEY (problem_id, tag_id),
    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id)     REFERENCES tags(id)      ON DELETE CASCADE,

    INDEX idx_problem_tags_tag_id (tag_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 5. test_cases 表 — 测试用例
-- ============================================================
CREATE TABLE IF NOT EXISTS test_cases (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    problem_id      INT            NOT NULL,
    input           LONGTEXT       NOT NULL COMMENT '测试输入',
    expected_output LONGTEXT       NOT NULL COMMENT '期望输出',
    is_sample       TINYINT(1)    NOT NULL DEFAULT 0 COMMENT '是否为示例用例（展示给用户）',
    order_num       INT            NOT NULL DEFAULT 0 COMMENT '用例顺序',

    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,

    INDEX idx_test_cases_problem_id (problem_id),
    INDEX idx_test_cases_sample (problem_id, is_sample)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 6. submissions 表 — 提交记录
-- ============================================================
CREATE TABLE IF NOT EXISTS submissions (
    id            INT AUTO_INCREMENT PRIMARY KEY,
    user_id       INT            NOT NULL,
    problem_id    INT            NOT NULL,
    language      ENUM('c', 'cpp') NOT NULL DEFAULT 'cpp',
    code          LONGTEXT       NOT NULL COMMENT '提交的源代码',
    status        ENUM('pending', 'running', 'ac', 'wa', 're', 'tle', 'mle', 'pe', 'ce')
                                 NOT NULL DEFAULT 'pending',
    time_used     INT            NULL COMMENT '实际耗时(ms)',
    memory_used   INT            NULL COMMENT '实际内存(KB)',
    error_message TEXT            NULL COMMENT '编译错误/运行时错误信息',
    created_at    DATETIME       NOT NULL DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
    FOREIGN KEY (problem_id) REFERENCES problems(id) ON DELETE CASCADE,

    INDEX idx_submissions_user_problem (user_id, problem_id),
    INDEX idx_submissions_status (status),
    INDEX idx_submissions_created_at (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ============================================================
-- 完成
-- ============================================================
-- 所有 6 张表已创建:
--   users, problems, tags, problem_tags, test_cases, submissions
--
-- 下一步: 运行 create_admin.sql 创建初始管理员账户
-- ============================================================