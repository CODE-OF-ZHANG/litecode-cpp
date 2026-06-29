# LiteCode-CPP — 产品规格说明书 (SPEC)

> **版本**: v1.2 (MVP+)  
> **日期**: 2026-06-27  
> **定位**: 个人学习型 OJ（Online Judge），以 C++ Web 开发为学习目标  
> **项目名**: LiteCode-CPP  
> **变更记录**: 详见 [§0 变更日志](#0-变更日志)

---

## 0. 变更日志

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v1.0 | 2026-06-20 | 初稿，核心功能定义 |
| v1.1 | 2026-06-25 | MVP 规格：tags 拆表、管理员模块、批量导入、20+ 验收用例 |
| **v1.2** | **2026-06-27** | **本次大版本**（基于代码审查的安全/性能/可维护性整改，详见下方） |

### v1.2 主要变更摘要

1. **安全加固** — Docker Socket 代理、g++ 安全编译标志、编译炸弹防护、CSP/SRI、Markdown XSS 防护、`audit_logs` 表
2. **判题优化** — 异步判题 + 任务队列、容器预热池、OLE 判定、CRLF/BOM 归一化、`test_cases.judge_type` 字段
3. **可观测性** — `/api/v1/health`、`/api/v1/metrics`、统一错误格式、`X-Request-Id` 链路追踪
4. **运维** — 数据库迁移工具、Prometheus + Grafana、日志策略、备份策略、HTTPS 反向代理
5. **API 演进** — `/api/v1/` 版本前缀、Refresh Token、限流中间件、健康检查
6. **数据模型** — `problems.is_deleted` 软删除、`audit_logs` 表、复合索引建议、`users.last_login_ip`
7. **前端体验** — 编辑器草稿持久化（localStorage）、深色模式、CSP/SRI、移动端响应式
8. **TODO 增补** — 新增 Phase 8（质量保障）、Phase 9（运维监控），各 Phase 增补条目
9. **验收增补** — 新增 A25–A34 验收用例（异步判题、限流、审计、容器池、编译炸弹、OLE、健康检查、XSS、草稿、深色模式）

---

## 1. 项目愿景

构建一个轻量级的在线判题系统（OJ），核心复刻 LeetCode 的刷题体验。项目以 **学习 C++ Web 开发** 为首要目标，MVP 阶段聚焦于核心刷题流程跑通，后续迭代完善体验。

**一句话描述**: 用户注册登录 → 浏览题库 → 在双栏界面中阅读题目并编写 C/C++ 代码 → 提交后实时获得判题结果（AC/WA/TLE 等）→ 查看个人提交历史和做题统计。

---

## 2. 需求总览

### 2.1 功能需求

| # | 功能 | 优先级 | 说明 |
|---|------|--------|------|
| F1 | 用户注册/登录 | P0 | 用户名 + 密码，JWT (access + refresh) 鉴权 |
| F2 | 题库浏览 | P0 | 题目列表页，支持按难度/标签筛选 |
| F3 | 题目详情 + 代码编辑 | P0 | LeetCode 风格双栏布局：左题目右编辑器，代码草稿持久化 |
| F4 | 代码提交与实时判题 | P0 | 异步提交 C/C++ 代码 → Docker 沙箱执行 → 轮询/SSE 拿结果 |
| F5 | 判题结果展示 | P0 | AC / WA / RE / TLE / MLE / OLE / PE / CE |
| F6 | 提交历史 | P1 | 查看自己的历史提交及结果（非管理员只能查自己） |
| F7 | 个人主页 | P1 | 做题统计（已解决/总数、通过率、提交次数） |
| F8 | 管理员题目导入 | P0 | 仅管理员可批量导入题目（JSON/YAML），普通用户无权操作 |
| F9 | 管理员题目管理 | P0 | 仅管理员可增删改题目（软删除），普通用户只读 |
| F10 | 管理员后台页面 | P1 | 管理员专属页面：题目 CRUD、批量导入、用户管理、系统概览 |
| F11 | 排行榜 | P2 | 按解题数/通过率排名 |
| F12 | 健康检查 | P1 | `/api/v1/health` 暴露 DB / Docker 可达性，docker-compose healthcheck 用 |
| F13 | 审计日志 | P1 | 管理员关键操作（删题、改角色、批量导入）写入 `audit_logs` |
| F14 | 限流 | P1 | 注册/登录/提交按 IP+用户限流，防刷 |
| F15 | 可观测性 | P2 | `/api/v1/metrics` Prometheus 指标、日志 JSON 化、请求 ID 链路追踪 |

### 2.2 非功能需求

| 维度 | 要求 |
|------|------|
| **性能** | 单次判题响应 < 5s（提交立即返回 `submission_id`；含容器启动，简单题目 < 3s）；支持 5-10 人同时使用 |
| **安全** | Docker 容器隔离 + Socket 代理 + CPU 时间限制 + 内存限制 + 网络隔离 + 文件系统隔离 + 输出大小限制 + 编译超时 + 编译安全标志 + 密码 bcrypt + JWT 签名 + CSP/SRI + Markdown XSS 净化 + SQL 参数化 |
| **可扩展性** | 判题模块架构预留多语言扩展（C/C++ 优先，后续可加 Python/Java） |
| **部署** | 本地单机运行，Docker Compose 一键启动；Caddy 反向代理 + 自动 HTTPS |
| **开发周期** | MVP 2-4 周 |
| **可观测** | 结构化日志（JSON） + Prometheus 指标 + 请求 ID 串联 |
| **数据保留** | 失败提交（WA/TLE/RE/CE）90 天后清理；AC 提交永久保留；mysqldump 每日异地备份 |

---

## 3. 系统架构

### 3.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      浏览器 (前端)                           │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────────┐    │
│  │  题目列表页  │  │ 刷题页(双栏)  │  │  个人主页/排行  │    │
│  └──────┬──────┘  └──────┬───────┘  └────────┬────────┘    │
│         │                │                    │              │
│  ┌──────┴────────────────┼────────────────────┘              │
│  │  管理后台 (🔒 admin)  │                                   │
│  │  题目CRUD | 批量导入 | 用户管理                            │
│  └──────────┬─────────────┘                                   │
│             │ HTTPS + JWT (含 role 字段)                     │
└─────────────┼──────────────────────────────────────────────┘
              │
┌─────────────┼──────────────────────────────────────────────┐
│             ▼        Web 服务器 (C++)                       │
│         [多线程 HTTP server]                                │
│                                                             │
│  ┌─────────┐  ┌─────────┐  ┌──────────┐  ┌───────────┐   │
│  │ 用户模块 │  │ 题目模块 │  │ 提交模块  │  │ 判题调度器 │   │
│  │  Auth   │  │ Problem │  │ Submit   │  │  Judge    │   │
│  └────┬────┘  └────┬────┘  └────┬─────┘  └─────┬─────┘   │
│       │            │            │              │           │
│       │    ┌───────┴───────┐    │              │           │
│       │    │ 管理员中间件   │    │              │           │
│       │    │ 限流中间件     │    │              │           │
│       │    │ 请求 ID 中间件 │    │              │           │
│       │    └───────┬───────┘    │              │           │
│       └────────────┼────────────┘              │           │
│                          │                     │           │
│                     MySQL 连接池               │  异步任务队列  │
│                    (ORM/原生SQL)               │  + 容器预热池 │
└──────────────────────────┼─────────────────────┼──────────┘
                           │                     │
                    ┌──────┴──────┐        ┌──────┴──────┐
                    │   MySQL     │        │ Docker API   │
                    │  数据库     │        │ (via socket  │
                    └─────────────┘        │  proxy)      │
                                           └──────┬──────┘
                                                  │
                                ┌─────────────────┴────────────┐
                                │  容器预热池 (N 个 idle 容器)  │
                                │  + 临时运行容器 (执行判题)     │
                                └──────────────────────────────┘
```

> **关键设计变化（v1.2）**:
> - 判题调度器改为**异步任务队列**（线程池 + condition_variable），提交 API 立即返回 `submission_id`
> - 启动时维护**容器预热池**（2-3 个 idle 容器），减少容器冷启动开销
> - Web → Docker 通过 **Docker Socket 代理**（仅暴露 5 个白名单子命令），不直接挂 socket

### 3.2 判题流程图（异步版）

```
用户提交代码
     │
     ▼
POST /api/v1/submissions
     │
     ▼
┌─────────────────┐
│ 写入 submissions │ status=pending
│ 返回 submission_id
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 判题任务队列     │ ← 线程池拉取任务
│  JudgeScheduler │   限制最大并发数 (如 4)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 从预热池取容器   │ ← 优先复用 idle 容器
│ 容器执行判题     │   不足时新建
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 容器内执行流程   │
│ 1. 编译 (g++ +  │ ← 独立超时 (10s)
│    安全标志)     │   失败 → CE
│ 2. 逐点运行      │ ← 文本归一化 (CRLF/BOM)
│ 3. 内存/时间测量 │ ← cgroup v2 memory.current
│ 4. 比对输出      │ ← judge_type 分支
│ 5. 输出截断      │ ← 16MB → OLE
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 状态回写 DB     │ status=ac/wa/tle/mle/re/ole/pe/ce
│ 释放容器到预热池 │
└────────┬────────┘
         │
         ▼
前端轮询 /api/v1/submissions/:id
（或 SSE 推送）
```

### 3.3 数据流概览

```
前端 (HTML/CSS/JS + CodeMirror/Monaco)
  │
  │  REST API (JSON) + JWT (含 role 字段) + X-Request-Id
  ▼
路由层 (多线程 HTTP server)
  │
  ├─ /api/v1/auth/*           → 用户模块 (注册/登录/刷新/登出/JWT)
  ├─ /api/v1/problems/*       → 题目模块 (列表/详情/筛选)         [公开]
  ├─ /api/v1/tags             → 标签模块                          [公开]
  ├─ /api/v1/submissions/*    → 提交模块 (异步提交/查询/历史)     [需登录]
  ├─ /api/v1/stats/*          → 统计模块 (个人统计/排行榜)        [公开/登录]
  ├─ /api/v1/admin/problems/* → 题目管理 (CRUD/批量导入)          [🔒 admin]
  ├─ /api/v1/admin/users/*    → 用户管理 (列表/角色变更)          [🔒 admin]
  ├─ /api/v1/admin/stats      → 系统统计                          [🔒 admin]
  ├─ /api/v1/admin/audit-logs → 审计日志查询                      [🔒 admin]
  ├─ /api/v1/health           → 健康检查 (DB + Docker)            [公开]
  └─ /api/v1/metrics          → Prometheus 指标                   [内网]
  │
  ▼
数据层 (MySQL)
  │
  ├─ users           用户表 (含 role 字段)
  ├─ problems        题目表 (含 is_deleted 软删除)
  ├─ tags            标签表
  ├─ problem_tags    题目-标签关联表
  ├─ test_cases      测试用例表 (含 judge_type)
  ├─ submissions     提交记录表
  └─ audit_logs      管理员操作审计表
```

---

## 4. 数据模型

> **v1.2 增补**:
> - `users.email` UNIQUE 策略修订（兼容可选）
> - `users.last_login_ip` 新增
> - `problems.is_deleted` 软删除字段新增
> - `test_cases.judge_type` 判定类型字段新增
> - `audit_logs` 表新增
> - §4.6 索引建议新增

### 4.1 users 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| username | VARCHAR(50) UNIQUE NOT NULL | 用户名 |
| password_hash | VARCHAR(255) NOT NULL | bcrypt 哈希（cost=12） |
| role | ENUM('user','admin') DEFAULT 'user' | 角色：普通用户 / 管理员 |
| email | VARCHAR(100) | 邮箱（可选，UNIQUE NULLS NOT DISTINCT，仅 MySQL 8.0.19+） |
| avatar | VARCHAR(255) | 头像 URL（默认） |
| created_at | DATETIME NOT NULL | 注册时间 |
| last_login | DATETIME | 最后登录 |
| last_login_ip | VARCHAR(45) | 最后登录 IP（支持 IPv6） |

> **权限模型**:
> - `user`（普通用户）：浏览题目、提交代码、查看自己的提交历史和个人统计
> - `admin`（管理员）：拥有普通用户所有权限 + 题目 CRUD（软删） + 批量导入 + 用户角色管理 + 查看审计日志
> - 系统初始化时通过脚本或配置创建第一个管理员账户
>
> **密码策略**:
> - bcrypt cost factor = 12（≈ 250ms 哈希，平衡安全与性能）
> - 前端校验：长度 ≥ 8，必须含字母+数字
> - 后端二次校验

### 4.2 problems 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| slug | VARCHAR(100) UNIQUE NOT NULL | 题目 URL 标识（如 two-sum） |
| title | VARCHAR(200) NOT NULL | 题目标题 |
| difficulty | ENUM('easy','medium','hard') NOT NULL | 难度 |
| description | MEDIUMTEXT NOT NULL | 题目描述（Markdown，存储前在管理端预净化） |
| time_limit | INT NOT NULL DEFAULT 1000 | 时间限制（ms） |
| memory_limit | INT NOT NULL DEFAULT 256 | 内存限制（MB） |
| accepted_count | INT NOT NULL DEFAULT 0 | 通过人数（**仅参考，不用于排行榜**，排行榜另算） |
| submission_count | INT NOT NULL DEFAULT 0 | 总提交数（仅参考） |
| is_deleted | BOOLEAN NOT NULL DEFAULT FALSE | 软删除标记（v1.2 新增） |
| created_at | DATETIME NOT NULL | 创建时间 |
| updated_at | DATETIME NOT NULL | 更新时间 |

> **原子性说明**: `tags` 字段原设计为逗号分隔字符串，违反 1NF 原子性要求，已拆分为独立的 `tags` 表和 `problem_tags` 关联表（见 4.2b、4.2c）。
>
> **软删除**（v1.2）: 删除题目时不真正 DELETE，而是 `UPDATE is_deleted = TRUE` + `updated_at = NOW()`。这样：
> - 历史 `submissions.problem_id` 仍可外键引用
> - 管理员可恢复误删
> - 前台列表自动过滤 `is_deleted = FALSE`

### 4.2b tags 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| name | VARCHAR(50) UNIQUE NOT NULL | 标签名称（如"数组""哈希表"） |

### 4.2c problem_tags 表（题目-标签关联）

| 字段 | 类型 | 说明 |
|------|------|------|
| problem_id | INT FOREIGN KEY → problems(id) | 关联题目 |
| tag_id | INT FOREIGN KEY → tags(id) | 关联标签 |
| PRIMARY KEY | (problem_id, tag_id) | 联合主键 |

### 4.2d audit_logs 表（v1.2 新增）

| 字段 | 类型 | 说明 |
|------|------|------|
| id | BIGINT AUTO_INCREMENT | 主键 |
| admin_id | INT FOREIGN KEY → users(id) | 操作管理员 |
| action | VARCHAR(50) NOT NULL | 操作类型（`problem.create` / `problem.delete` / `user.role_change` / `problem.bulk_import` 等） |
| target_type | VARCHAR(50) | 对象类型（`problem` / `user` / `tag`） |
| target_id | VARCHAR(100) | 对象 ID |
| payload | JSON | 操作详情（如删除前快照、变更前后值） |
| ip | VARCHAR(45) | 操作 IP |
| created_at | DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP | 操作时间 |

> **写入场景**（v1.2 必须实现）:
> - 创建/修改/删除（软删）题目
> - 批量导入题目
> - 变更用户角色
> - 失败登录尝试（>= 5 次）
> - 修改管理员密码

### 4.3 test_cases 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| problem_id | INT FOREIGN KEY → problems(id) NOT NULL | 关联题目 |
| input | LONGTEXT NOT NULL | 测试输入（统一 UTF-8 + LF 换行） |
| expected_output | LONGTEXT NOT NULL | 期望输出（同上） |
| is_sample | BOOLEAN NOT NULL DEFAULT FALSE | 是否为示例用例（展示给用户） |
| judge_type | ENUM('exact','ignore_trailing','float_eps','special') NOT NULL DEFAULT 'exact' | 判定类型（v1.2 新增） |
| float_epsilon | DECIMAL(10,8) NULL | 浮点误差容忍（仅 `judge_type=float_eps` 时使用） |
| order_num | INT NOT NULL DEFAULT 0 | 用例顺序 |

> **judge_type 说明**（v1.2）:
> | 取值 | 行为 |
> |------|------|
> | `exact` | 完全字符串匹配（默认） |
> | `ignore_trailing` | 忽略每行尾部空白后逐行比较（AC/PE 判定沿用 v1.1 规则） |
> | `float_eps` | 按浮点比较，绝对/相对误差 < `float_epsilon` 视为相等 |
> | `special` | Special Judge（v1.3+ 实现，MVP 阶段留字段） |

### 4.4 submissions 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| user_id | INT FOREIGN KEY → users(id) NOT NULL | 提交用户 |
| problem_id | INT FOREIGN KEY → problems(id) NOT NULL | 提交题目 |
| language | ENUM('c','cpp') NOT NULL | 编程语言 |
| code | MEDIUMTEXT NOT NULL | 提交的源代码（MEDIUMTEXT 16MB 上限足够） |
| status | ENUM('pending','running','ac','wa','re','tle','mle','ole','pe','ce','se') NOT NULL DEFAULT 'pending' | 判题结果（v1.2 新增 `ole`、`se`） |
| time_used | INT | 实际耗时（ms） |
| memory_used | INT | 实际内存（KB，从 cgroup v2 读取） |
| error_message | TEXT | 编译错误/运行时错误信息（截断至 4KB） |
| created_at | DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP | 提交时间 |
| finished_at | DATETIME | 判题完成时间（用于计算队列等待时长） |

> **新状态码**（v1.2）:
> - `ole` (Output Limit Exceeded) — 输出超过 16MB 截断
> - `se` (System Error) — 判题基础设施异常（容器启动失败、DB 写失败等），与 CE 区分

### 4.5 索引建议（v1.2 新增）

> 单条 SQL 加索引 DDL 写在 migrations 脚本里，这里只列**逻辑**索引。

| 表 | 索引列 | 用途 |
|----|--------|------|
| users | UNIQUE(username) | 登录查询 |
| users | UNIQUE(email) | 邮箱查重 |
| problems | UNIQUE(slug) | 详情查询 |
| problems | (is_deleted, difficulty, created_at) | 列表分页 + 难度筛选 + 软删过滤 |
| problems | (is_deleted, slug) | slug 查询时直接走软删过滤的覆盖索引 |
| test_cases | (problem_id, order_num) | 判题按序拉取 |
| test_cases | (problem_id, is_sample, order_num) | 题目详情页示例展示 |
| submissions | (user_id, problem_id, created_at DESC) | 个人提交历史 |
| submissions | (problem_id, status, created_at) | 管理员查通过率 |
| submissions | (status, created_at) | 异步队列拉取待判题（status IN ('pending','running')） |
| submissions | (created_at) | 数据保留策略（90 天前清理） |
| audit_logs | (admin_id, created_at DESC) | 管理员操作历史 |
| audit_logs | (action, created_at) | 按操作类型筛选 |
| problem_tags | (tag_id, problem_id) | 反向查"某标签下所有题" |

---

## 5. API 设计

> **v1.2 增补**:
> - 全部路径加 `/api/v1/` 前缀
> - `POST /api/v1/auth/refresh`、`POST /api/v1/auth/logout` 新增
> - `POST /api/v1/submissions` 改为异步：返回 `{submission_id, status:"pending"}`，前端轮询 `/api/v1/submissions/:id`
> - `/api/v1/health`、`/api/v1/metrics` 新增
> - 提交历史查询权限收紧：非 admin 强制 `user_id = 当前用户`
> - 限流注解：注册/登录/提交按 IP+用户限流（条目详见每条 API 说明）

### 5.1 用户模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| POST | `/api/v1/auth/register` | 公开 | 5 次/分/IP | 用户注册（默认 role=user），返回 access + refresh |
| POST | `/api/v1/auth/login` | 公开 | 10 次/分/IP | 用户登录，返回 access (2h) + refresh (7d) |
| POST | `/api/v1/auth/refresh` | 已登录 | - | 用 refresh token 换新 access |
| POST | `/api/v1/auth/logout` | 已登录 | - | 注销（refresh token 加入黑名单到过期） |
| GET  | `/api/v1/auth/profile` | 已登录 | - | 获取当前用户信息 |

> **JWT 细节**（v1.2 明确）:
> - 算法：HS256（secret 从环境变量 `JWT_SECRET` 读取，启动时校验非空且 ≥ 32 字节）
> - access token TTL：2 小时；refresh token TTL：7 天
> - Payload：`{sub: user_id, username, role, iat, exp}`
> - 黑名单：refresh token 注销时写入 Redis（key=`jwt:blacklist:<jti>`，TTL=剩余有效期）

### 5.2 题目模块（公开只读 + 管理员写入）

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| GET | `/api/v1/problems` | 公开 | 60 次/分/IP | 题目列表（分页、难度/标签筛选，自动过滤 `is_deleted=FALSE`） |
| GET | `/api/v1/problems/:slug` | 公开 | 60 次/分/IP | 题目详情（含示例用例 + 标签） |
| GET | `/api/v1/tags` | 公开 | - | 所有标签列表 |
| POST | `/api/v1/admin/problems` | 🔒 admin | 30 次/分 | 创建单道题目（含标签关联，自动创建不存在的标签） |
| PUT | `/api/v1/admin/problems/:slug` | 🔒 admin | 30 次/分 | 修改题目（含标签关联更新） |
| DELETE | `/api/v1/admin/problems/:slug` | 🔒 admin | 10 次/分 | 软删除题目（设置 `is_deleted=TRUE`，不真正删除） |
| POST | `/api/v1/admin/problems/import` | 🔒 admin | 5 次/小时 | 批量导入（multipart/form-data，JSON/YAML 文件） |

> 全部路径以 `/api/v1/admin/*` 开头需要 JWT 中 `role=admin`，非管理员返回 403 Forbidden。

### 5.3 提交模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| POST | `/api/v1/submissions` | 已登录 | 30 次/分/用户 | **异步**：写入 `submissions(status=pending)`，入队后立即返回 `{submission_id, status:"pending"}` |
| GET | `/api/v1/submissions/:id` | 已登录 | - | 查询单次结果；非 admin 只能查自己的 |
| GET | `/api/v1/submissions` | 已登录 | - | 提交历史列表；**非 admin 强制 `user_id = 当前用户`**，忽略请求参数中的 `user_id` |
| GET | `/api/v1/submissions/sse/:id` | 已登录 | - | (可选) SSE 推送判题结果，完成时主动推一条 |

> **异步设计**（v1.2）: 客户端典型流程：
> ```
> POST /api/v1/submissions → {id: 42, status: "pending"}
> ↓ 客户端 1-2s 轮询
> GET /api/v1/submissions/42 → {status: "running"}
> ↓ 继续轮询
> GET /api/v1/submissions/42 → {status: "ac", time_used: 12, memory_used: 2048}
> ```

### 5.4 统计模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| GET | `/api/v1/stats/profile/:username` | 已登录 | - | 用户做题统计 |
| GET | `/api/v1/stats/ranking` | 公开 | 30 次/分/IP | 排行榜（默认 100 名，可选分页） |

### 5.5 管理员模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| GET | `/api/v1/admin/users` | 🔒 admin | 60 次/分 | 用户列表（分页 + 搜索） |
| PUT | `/api/v1/admin/users/:id/role` | 🔒 admin | 10 次/分 | 修改用户角色（写入 `audit_logs`） |
| GET | `/api/v1/admin/stats` | 🔒 admin | - | 系统统计（题目数、用户数、提交数、队列长度） |
| GET | `/api/v1/admin/audit-logs` | 🔒 admin | - | 审计日志查询（分页 + 筛选） |
| GET | `/api/v1/admin/queue` | 🔒 admin | - | 判题队列状态（等待中/运行中数量） |

### 5.6 系统模块（v1.2 新增）

| 方法 | 路径 | 权限 | 说明 |
|------|------|------|------|
| GET | `/api/v1/health` | 公开 | 健康检查：`{db: "ok", docker: "ok", queue_size: 3, warm_pool: 2}` |
| GET | `/api/v1/metrics` | 内网 | Prometheus 指标（`litecode_submissions_total{status="ac"}`、`litecode_judge_duration_seconds` 等） |

### 5.7 全局约定（v1.2 新增）

- **请求 ID**: 所有响应携带 `X-Request-Id` 头（服务端生成 UUID v4），用于日志串联
- **统一错误格式**:
  ```json
  {
    "code": "INVALID_INPUT",
    "message": "用户名长度必须在 3-50 之间",
    "details": { "field": "username" },
    "request_id": "550e8400-e29b-41d4-a716-446655440000"
  }
  ```
- **错误码枚举**: `INVALID_INPUT` / `UNAUTHORIZED` / `FORBIDDEN` / `NOT_FOUND` / `RATE_LIMITED` / `CONFLICT` / `INTERNAL_ERROR` / `SERVICE_UNAVAILABLE`
- **CORS**: 默认仅允许本地域名（`http://localhost:8080`），生产环境从配置读取白名单

---

## 6. 前端页面设计

### 6.1 页面清单

| 页面 | 路径 | 权限 | 说明 |
|------|------|------|------|
| 首页/题库列表 | `/` | 公开 | 题目列表，筛选、搜索 |
| 刷题页 | `/problems/:slug` | 公开 | 双栏：左题目右编辑器，**代码草稿自动保存到 localStorage** |
| 登录页 | `/login` | 公开 | 用户登录 |
| 注册页 | `/register` | 公开 | 用户注册 |
| 个人主页 | `/profile/:username` | 已登录 | 做题统计、提交历史 |
| 排行榜 | `/ranking` | 公开 | 全站排名 |
| 🔒 管理后台-题目管理 | `/admin/problems` | admin | 题目列表、增删改、批量导入 |
| 🔒 管理后台-题目编辑 | `/admin/problems/edit/:slug` | admin | 新建/编辑题目表单 |
| 🔒 管理后台-用户管理 | `/admin/users` | admin | 用户列表、角色管理 |
| 🔒 管理后台-系统概览 | `/admin/dashboard` | admin | 系统统计（题目数、用户数、提交数、队列、判题机时延） |
| 🔒 管理后台-审计日志 | `/admin/audit-logs` | admin | 审计日志查询 |

> **导航逻辑**: 普通用户导航栏只显示"题库/排行榜/个人主页"；管理员用户额外显示"管理后台"入口。非管理员直接访问 `/admin/*` 路径时前端拦截跳转至首页。

### 6.2 刷题页布局（核心页面）

```
┌──────────────────────────────────────────────────┐
│  Logo    题库  排行榜  讨论          [用户头像 ▾] │
├─────────────────────┬────────────────────────────┤
│                     │  ┌─────────────────────┐   │
│   题目描述区域       │  │   代码编辑器          │   │
│   (Markdown 已净化)  │  │  (CodeMirror/Monaco)  │   │
│                     │  │  [草稿自动保存]        │   │
│   - 描述            │  │                       │   │
│   - 示例输入/输出    │  │  C++  ▼              │   │
│   - 约束条件        │  │                       │   │
│                     │  └─────────────────────┘   │
│                     │  ┌─────────────────────┐   │
│                     │  │   运行结果 / 提交结果  │   │
│                     │  │   AC ✅ / WA ❌        │   │
│                     │  │   时间: 12ms 内存: 2MB │   │
│                     │  │   (失败时显示测试点)   │   │
│                     │  └─────────────────────┘   │
│                     │  [运行] [提交]               │
├─────────────────────┴────────────────────────────┤
│  提交历史标签页                                     │
└──────────────────────────────────────────────────┘
```

### 6.3 前端安全与体验（v1.2 新增）

| 类别 | 要求 | 实现 |
|------|------|------|
| **CSP** | 严格 CSP 头 | `<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; style-src 'self' 'unsafe-inline'; img-src 'self' data:;">` |
| **SRI** | CDN 资源 integrity 校验 | CodeMirror/Monaco 走 CDN 时加 `integrity` + `crossorigin="anonymous"` |
| **XSS 防护** | 题目描述 Markdown 渲染必须净化 | 用 [DOMPurify](https://github.com/cure53/DOMPurify) 配合 [marked](https://marked.js/) |
| **代码草稿** | 写一半代码不丢失 | `localStorage.setItem('code:<slug>:<lang>', code)`，提交成功后清除，刷新时弹"恢复草稿"提示 |
| **Token 存储** | 防 XSS 盗 token | access token 存内存，refresh token 走 `HttpOnly; Secure; SameSite=Strict` cookie |
| **深色模式** | 用户体验 | CSS 变量 + `prefers-color-scheme`，支持手动切换并持久化 |
| **移动端** | 响应式 | 刷题页在 < 768px 切换为上下布局；管理后台在 < 1024px 折叠侧栏 |
| **a11y** | 可访问性 | 主要按钮/链接加 `aria-label`，编辑器支持键盘 Tab 缩进 |

---

## 7. 判题模块详细设计

> **v1.2 重大改造**:
> - 异步任务队列（线程池 + condition_variable）
> - 容器预热池
> - g++ 安全编译标志 + 编译独立超时
> - OLE 判定分支
> - 文本归一化（CRLF/BOM）
> - 内存/时间精确测量（cgroup v2）
> - `judge_type` 判题分支
> - `SE` 系统错误状态

### 7.1 Docker 判题流程（v1.2 完整版）

```
0. 系统启动
   - 判题调度器初始化：启动 1 个 dispatcher 线程 + N 个 worker 线程（默认 N=4）
   - 创建容器预热池：docker create × K 个 idle 容器（默认 K=2）

1. 接收提交 → 写入 submissions(status=pending) → 入队
   - 返回 {submission_id, status: "pending"}
   - 同时启动 SSE/轮询由前端负责

2. Worker 从队列取任务
   a. 从预热池 docker start 一个容器；若池空则 docker create + start 新容器
   b. 更新 status=running

3. 容器内执行（受 cgroup + Docker 资源限制）
   a. **编译** (独立超时 10s，防编译炸弹)
      g++ -O2 -std=c++17 -pipe \
          -fstack-protector-strong \
          -D_FORTIFY_SOURCE=2 \
          -Wformat -Wformat-security \
          -Wl,-z,now -Wl,-z,relro \
          -o solution solution.cpp
      · 编译失败 → CE（error_message 截断至 4KB）
      · 编译超时 → CE（标记为 "Compilation timeout"）
      · 编译 OOM → MLE
   b. **运行** (每个测试点独立)
      · 文本归一化：输入/期望输出先 `tr -d '\r'` + 去 BOM，保证跨平台一致
      · 运行: `timeout {time_limit/1000+1} ./solution < input.txt > output.txt`
      · 输出截断：若 output.txt > 16MB → OLE（判当前测试点失败并跳过剩余比对）
      · 时间测量：从 cgroup v2 `cpu.stat` 读取 `usage_usec`（精确到 ms）
      · 内存测量：从 cgroup v2 `memory.current` 读取（KB）
      · 结果判定:
        · 超时 → TLE
        · 内存超限 → MLE
        · 非零退出码 → RE
        · 正常退出 → 根据 test_cases.judge_type 比对:
          - `exact`         → 完全相同 AC，否则 WA
          - `ignore_trailing` → 去尾部空白后相同 AC，否则 PE
          - `float_eps`     → 浮点按 epsilon 比较，相同 AC，否则 WA
          - `special`       → MVP 返回 SE（v1.3+ 接入 SPJ）
   c. 汇总：所有测试点通过 → AC；首个失败测试点为该 submission 的最终状态
   d. 任何基础设施异常（容器 OOM、DB 写失败）→ SE

4. 状态回写 DB
   - status, time_used(取最大值), memory_used(取最大值), error_message, finished_at

5. 容器归还
   - docker stop + docker rm 当前容器
   - 不归还预热池（每次新建专用容器，保证隔离）
   - 维护预热池大小：异步补齐至 K 个

6. 异常兜底
   - Web 进程对每个 docker run 设 30s 硬超时（防 judge.sh 自身卡死）
```

### 7.2 判题 Docker 镜像 (Dockerfile)

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ gcc gdb coreutils dos2unix \
    && rm -rf /var/lib/apt/lists/*

# 判题运行脚本
COPY judge.sh /usr/local/bin/judge.sh
RUN chmod +x /usr/local/bin/judge.sh

# 安全：非 root 运行（即使 --read-only 也以 nobody 启动）
RUN useradd -m -u 1000 judgeuser
USER judgeuser
WORKDIR /judge

ENTRYPOINT ["/usr/local/bin/judge.sh"]
```

### 7.3 判题安全策略汇总

| 策略 | 实现方式 | 参数 |
|------|----------|------|
| CPU 时间限制 | Docker `--cpus` + 容器内 `timeout` | 默认 1s |
| **编译超时**（v1.2） | 容器内 `timeout 10s g++ ...` | 10s（防编译炸弹） |
| 内存限制 | Docker `--memory` | 默认 256MB |
| 内存测量（v1.2） | 读 cgroup v2 `memory.current` | KB 精度 |
| 网络隔离 | Docker `--network=none` | 完全禁止网络 |
| 文件系统隔离 | Docker `--read-only` + 临时写入目录 | 只读根 + /tmp 可写 |
| 进程数限制 | Docker `--pids-limit` | 最多 50 个进程 |
| **输出大小限制**（v1.2） | judge.sh 中 `head -c 16M` 截断 | > 16MB 判 OLE |
| 权限提升防护 | Docker `--security-opt=no-new-privileges` | 禁止提权 |
| **编译标志**（v1.2） | `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Wl,-z,now -Wl,-z,relro` | 防溢出利用 |
| **Docker Socket 隔离**（v1.2） | Web 容器通过 [docker-socket-proxy](https://github.com/Tecnativa/docker-socket-proxy) 访问 Docker | 仅白名单 5 个子命令 |
| **Web 容器自身**（v1.2） | `--cpus=2 --memory=512m` + 非 root | 防止 Web 进程失控影响判题 |
| 进程身份（v1.2） | judge 容器内 `USER judgeuser` | 即使逃逸也无 root |

### 7.4 判题规则细则（v1.2 新增）

| 项 | 规则 |
|----|------|
| 换行符 | 输入/期望输出统一 LF（`\n`），容器内 `tr -d '\r'` 归一化 |
| BOM | 容器内 `sed '1s/^\xEF\xBB\xBF//'` 去 UTF-8 BOM |
| 浮点输出 | 推荐题目用 `printf("%.6f")` 或固定格式；`judge_type=float_eps` 兜底 |
| 时间测量 | 取所有测试点中最长 `cpu.stat usage_usec`，向上取整为 ms |
| 内存测量 | 取所有测试点中 `memory.peak`（cgroup v2）或 `memory.max_usage`（v1），单位 KB |
| OLE 行为 | 输出 > 16MB → 立即判 OLE，**不再**继续比对 |
| CE 信息 | 截断至前 4KB（避免恶意超长编译错误撑爆 DB） |
| RE 信息 | 截断至前 2KB |
| 系统错误 | 容器启动失败、DB 写失败、Docker daemon 不可达 → `status=se` |

---

## 8. 题目导入格式

### 8.1 单道题目 JSON 结构

```json
{
  "slug": "two-sum",
  "title": "两数之和",
  "difficulty": "easy",
  "tags": ["数组", "哈希表"],
  "time_limit_ms": 1000,
  "memory_limit_mb": 256,
  "description_md": "# 题目描述\n\n给定一个整数数组...",
  "samples": [
    {
      "input": "[2,7,11,15]\n9",
      "output": "[0,1]"
    }
  ],
  "test_cases": [
    {
      "input": "2,7,11,15\n9\n",
      "expected_output": "0,1\n",
      "is_sample": true,
      "judge_type": "exact",
      "float_epsilon": null
    }
  ]
}
```

> **v1.2 增补**:
> - `test_cases[].judge_type` 新增字段（默认 `exact`，可省略）
> - `test_cases[].float_epsilon` 新增字段（仅 `float_eps` 时使用）
> - `description_md` 建议在管理端**导入前**预净化（v1.2 软要求，v1.3 强制）

### 8.2 批量导入（🔒 管理员专属）

仅管理员可通过 API 端点批量上传题目。普通用户调用此接口返回 403。

```
POST /api/v1/admin/problems/import
Authorization: Bearer <admin-jwt-token>
Content-Type: multipart/form-data

files: problems/*.json
```

> **设计意图**: 题目来源完全由管理员控制，普通用户只能浏览和做题，不能导入或创建题目。
>
> **v1.2 行为**:
> - 单次最多 50 个文件 / 10MB
> - slug 冲突时默认**跳过**该条并返回详情；可选 query 参数 `?on_duplicate=overwrite` 覆盖
> - 导入完成后写入一条 `audit_logs` 记录（payload 含成功/失败计数）

---

## 9. 技术栈汇总

| 层 | 技术选型 | 说明 |
|----|---------|------|
| **前端** | HTML + CSS + JavaScript | 原生，无框架；DOMPurify + marked 做 Markdown 净化 |
| **代码编辑器** | CodeMirror 6 或 Monaco Editor | CDN 引入（带 SRI） |
| **后端** | C++17 + cpp-httplib (多线程) | header-only HTTP 库，**必须**配置 `ThreadPool` 参数 |
| **Web 框架备选** | Crow / userver | 如果 cpp-httplib 并发不够，可平滑切换 |
| **数据库** | MySQL 8.0.19+ | 关系型存储（`UNIQUE NULLS NOT DISTINCT` 需要 ≥ 8.0.19） |
| **数据库连接** | mysql-connector/cpp 或 sqlpp11 | C++ MySQL 驱动 |
| **判题沙箱** | Docker Engine API（经 Socket 代理） | 容器隔离执行 |
| **认证** | JWT (jwt-cpp 库) | HS256，access 2h + refresh 7d |
| **密码哈希** | bcrypt | cost=12 |
| **限流** | 内存令牌桶（单实例）/ Redis（多实例） | MVP 用内存 |
| **Token 黑名单** | Redis | refresh token 注销用 |
| **构建** | CMake | 跨平台构建 |
| **部署** | Docker Compose | 一键启动 Web + MySQL + Judge + Socket Proxy + Caddy |
| **反向代理** | Caddy | 自动 HTTPS（生产）/ HTTP-only（本地） |
| **数据库迁移** | Flyway 社区版 或纯 SQL 脚本 (`db/migrations/V001__init.sql`, `V002__audit_log.sql`) | v1.2 新增 |
| **监控** | Prometheus + Grafana | `/api/v1/metrics` 暴露指标（v1.2 新增） |
| **日志** | spdlog + JSON 输出 | stdout + 文件，Docker logs 接管（v1.2 新增） |
| **备份** | mysqldump 每日 + 异地 | cron job（v1.2 新增） |

---

## 10. 项目目录结构

```
litecode-cpp/
├── CMakeLists.txt
├── docker-compose.yml
├── Caddyfile                  # 反向代理配置（v1.2 新增）
├── Dockerfile                 # Web 服务镜像
├── .env.example               # 环境变量模板（v1.2 增补）
├── .gitignore                 # （项目初始化已存在）
├── README.md                  # （项目初始化已存在）
├── LICENSE                    # （项目初始化已存在）
├── dependence.md              # 依赖说明（项目初始化已存在）
├── docs/                      # 补充文档目录（项目初始化已存在）
├── third_party/               # 第三方库（nlohmann/json, cpp-httplib, jwt-cpp, zlib）
├── build/                     # CMake 构建产物（运行时生成，已 .gitignore）
├── monitoring/                # 监控配置（v1.2 增补）
│   ├── prometheus.yml         # Prometheus 抓取配置
│   └── grafana/
│       └── datasources.yml    # Grafana 数据源
├── judge/                     # 判题模块
│   ├── Dockerfile             # 判题镜像
│   ├── judge.sh               # 判题执行脚本
│   └── judge_server.h         # 判题调度器（线程池 + 任务队列 + 预热池）
├── src/                       # 后端源码
│   ├── main.cpp
│   ├── server.h               # HTTP 服务器入口（多线程）
│   ├── config.h               # 配置管理
│   ├── logger.h               # 日志封装（JSON 格式，v1.2）
│   ├── middleware/
│   │   ├── request_id.h       # 请求 ID 注入（v1.2）
│   │   ├── rate_limit.h       # 限流中间件（v1.2）
│   │   ├── auth_middleware.h  # JWT 认证
│   │   └── admin_middleware.h # 管理员权限校验（v1.2 唯一保留位置，原 src/auth/ 同名文件已废弃）
│   ├── utils/                 # 通用工具（v1.2 增补）
│   │   ├── uuid.h             # UUID v4 生成（X-Request-Id 用）
│   │   ├── string_utils.h
│   │   └── time_utils.h
│   ├── cache/                 # Redis 客户端（v1.2 增补）
│   │   └── redis_client.h     # token 黑名单 / 限流计数用
│   ├── db/
│   │   ├── connection_pool.h
│   │   ├── migration.h        # 迁移工具封装（v1.2）
│   │   ├── user_repo.h
│   │   ├── problem_repo.h
│   │   ├── tag_repo.h
│   │   ├── submission_repo.h
│   │   └── audit_log_repo.h   # 审计日志（v1.2）
│   ├── auth/
│   │   ├── jwt_utils.h
│   │   ├── password_hash.h
│   │   ├── refresh_token.h    # refresh token + 黑名单（v1.2）
│   │   └── bcrypt/            # bcrypt 源码（项目初始化已存在）
│   ├── routes/
│   │   ├── auth_routes.h
│   │   ├── problem_routes.h
│   │   ├── submission_routes.h
│   │   ├── stats_routes.h
│   │   ├── admin_routes.h
│   │   ├── system_routes.h    # /health /metrics（v1.2）
│   │   └── error_handler.h    # 统一错误格式（v1.2）
│   └── judge/
│       ├── judge_scheduler.h
│       ├── docker_client.h
│       └── warm_pool.h        # 容器预热池（v1.2）
├── web/                       # 前端静态文件
│   ├── index.html
│   ├── problem.html
│   ├── login.html
│   ├── register.html
│   ├── profile.html
│   ├── ranking.html
│   ├── admin/
│   │   ├── dashboard.html
│   │   ├── problems.html
│   │   ├── problem-edit.html
│   │   ├── users.html
│   │   └── audit-logs.html    # 审计日志页（v1.2）
│   ├── css/
│   │   └── style.css          # 支持深色模式（v1.2）
│   ├── assets/                # 静态资源（v1.2 增补）
│   │   └── img/
│   │       └── default-avatar.svg
│   └── js/
│       ├── app.js
│       ├── api.js
│       ├── editor.js          # 草稿持久化（v1.2）
│       ├── markdown.js        # DOMPurify + marked（v1.2）
│       └── admin.js
├── problems/                  # 题目数据
│   ├── two-sum.json
│   └── ...
├── db/
│   └── migrations/            # 数据库迁移脚本（v1.2）
│       ├── V001__init.sql
│       ├── V002__add_audit_logs.sql
│       ├── V003__add_judge_type.sql
│       ├── V004__add_soft_delete.sql
│       ├── V005__add_indexes.sql
│       └── V006__add_ole_se_status.sql  # 补 V005 遗漏：status ENUM 增 ole/se
├── scripts/
│   ├── init_db.sh             # 数据库初始化入口
│   ├── create_admin.sql
│   ├── seed_problems.py
│   └── backup.sh              # 备份脚本（v1.2）
└── tests/
    ├── unit/
    │   ├── test_auth.cpp
    │   ├── test_problem.cpp
    │   ├── test_judge.cpp
    │   ├── test_rate_limit.cpp # v1.2
    │   ├── test_audit_log.cpp  # v1.2
    │   └── test_stats.cpp
    ├── integration/
    │   ├── test_api.cpp
    │   ├── test_judge_flow.cpp
    │   └── test_security.cpp   # 编译炸弹、OLE、SSE 等（v1.2）
    └── e2e/                   # 端到端验收（v1.2 增补，§12.1 自动化）
        └── e2e_acceptance.sh
├── logs/                      # 日志文件输出目录
```

---

## 11. TODO 清单（MVP 开发计划）

> **v1.2 重要变更**:
> - 各 Phase **增补**了安全/性能/可观测相关条目
> - 新增 **Phase 8（质量保障）** 和 **Phase 9（运维与监控）**
> - 标注每条优先级：`★ 必做` / `☆ 应做` / `△ 可选`

### Phase 1 - 基础设施

- [x] ★ 项目目录结构 + CMake 构建（引入 cpp-httplib、mysql-connector、jwt-cpp、spdlog）
- [x] ★ 数据库初始化脚本（建表 SQL + 初始管理员种子数据）
- [x] ★ 配置管理（config.h：DB / 端口 / JWT_SECRET / 判题参数等；env 优先 + 默认值）
- [x] ★ 日志封装（logger.h：JSON 格式，INFO/WARN/ERROR，stdout + 文件，**带 request_id 字段**）
- [x] ★ 数据库连接池（connection_pool.h：连接池 + 基础查询封装）
- [x] ★ HTTP 服务框架（server.h：路由注册 + CORS + 统一 JSON 响应 + **多线程 ThreadPool**）
- [x] ★ Docker Compose 开发环境（Web + MySQL + Socket Proxy + Judge 容器一键启动）
- [x] ★ 请求 ID 中间件（生成 UUID v4 注入响应头 + 贯穿日志）
- [x] ★ 健康检查端点 `/api/v1/health`（DB + Docker 探测）
- [x] ★ 统一错误处理（error_handler.h：§5.7 错误码枚举 + 统一响应格式）
- [x] ☆ Caddyfile 反向代理配置
- [x] ☆ Prometheus + Grafana docker-compose 接入
- [x] △ JSON 日志输出（spdlog + JSON 格式化）

### Phase 2 - 登录注册模块

- [x] ★ JWT 工具（jwt_utils.h：HS256 签发 + 验证 + 提取 user_id/role；secret 从 env 读且 ≥ 32 字节）
- [x] ★ 密码哈希（password_hash.h：bcrypt cost=12，header-only + 内联；`hash_password` / `verify_password` / `extract_cost_factor` / `needs_rehash`；失败抛 `PasswordError` 三级异常，verify 路径 `noexcept`；tests/unit/test_password_hash.cpp 33 用例全通过 ~6.3s）
- [x] ★ 密码强度校验（后端 `validate_password_strength` / `require_password_strength`：8 ≤ len ≤ 72（含 bcrypt `$2b$` 72 字节硬上限）+ 字母 + 数字；前端实现见 web/js/app.js 时复用同策略，避免前后端规则漂移）
- [x] ★ JWT 认证中间件（auth_middleware.h + admin_middleware.h：header-only 内联；`extract_bearer_token`（OWS/大小写/CRLF 注入防护）+ `require_authentication`（401 统一信封，验签失败/过期/错 issuer/错 kind 全归一为 "invalid or expired token" 防探测）+ `require_role`（403）+ `require_admin`（401→403 链式校验）；tests/unit/test_auth_middleware.cpp 32 用例覆盖 token 解析、过期/篡改/换 issuer/换 kind、role 校验、E2E HttpServer 往返，全通过 ~0.27s）
- [x] ★ 管理员权限中间件（admin_middleware.h：`require_admin` 链式 `require_authentication` + `require_role("admin")`，未登录 401、登录但非 admin 403）
- [x] ★ 限流中间件（按 IP+用户，令牌桶，§5.1 各端点配额）
- [x] ★ Refresh Token 机制（签发 + 刷新 + 黑名单）
- [x] ★ 用户注册 API（POST /api/v1/auth/register，限流 5/分/IP）
- [x] ★ 用户登录 API（POST /api/v1/auth/login，限流 10/分/IP；header-only + inline；`login_handler` + `LoginFailureTracker`（per-username count, kAuditLogEvery=5）+ `detail::parse_login_request`；bcrypt verify `noexcept`；anti-enumeration：用户不存在 vs 密码错误 → 同 401 envelope；5 次连续失败 → `audit_log_repo::record_login_failure` 写 audit_logs；成功 → 重置计数 + `user_repo::update_last_login`；tests/unit/test_auth_login.cpp 29 用例全通过 ~14s）
- [x] ★ Refresh API（POST /api/v1/auth/refresh；header-only + inline；`refresh_handler` + `RefreshRequest` / `detail::parse_refresh_request`；无 rate limit（SPEC §5.1）；`verify` → `user_repo::find_by_id` → `rotate_token_pair`（自动 blacklist check + revoke old + sign new）；anti-enumeration：bad sig/expired/revoked/wrong kind/deleted user → 同 401 envelope；新 access token 携带最新 username/role；tests/unit/test_auth_refresh.cpp 25 用例全通过 ~6.9s）
- [ ] ★ Logout API（POST /api/v1/auth/logout，refresh 入黑名单）
- [ ] ★ 用户信息 API（GET /api/v1/auth/profile）
- [x] ☆ 失败登录审计（连续 5 次失败写 audit_logs，详见 login_handler + LoginFailureTracker）

### Phase 3 - 题目模块

- [ ] ★ 题目数据模型（problem_repo.h：CRUD + 软删除 + 软删过滤查询）
- [ ] ★ 标签数据模型（tag_repo.h：标签 + 题目-标签关联）
- [ ] ★ 审计日志数据模型（audit_log_repo.h）
- [ ] ★ 数据库迁移脚本（V001-V005，按 §10 目录落地）
- [ ] ★ 题目列表 API（GET /api/v1/problems，分页 + 难度/标签筛选 + 软删过滤）
- [ ] ★ 题目详情 API（GET /api/v1/problems/:slug，含示例 + 标签）
- [ ] ★ 标签列表 API（GET /api/v1/tags）
- [ ] ★ 管理员题目 CRUD API（POST/PUT/DELETE /api/v1/admin/problems/*，限流到位）
- [ ] ★ 管理员批量导入 API（POST /api/v1/admin/problems/import，import 行为见 §8.2）
- [ ] ★ 示例题目数据（5-10 道 JSON 题目文件，含 judge_type 字段）
- [ ] ☆ 题目版本/编辑历史表（`problem_revisions`）— v1.3 考虑

### Phase 4 - 代码执行与判题模块

- [ ] ★ 判题 Docker 镜像（judge/Dockerfile + judge.sh，§7.2 用户、§7.3 安全标志）
- [ ] ★ Docker 客户端（docker_client.h：经 socket 代理的 create/start/exec/kill/rm）
- [ ] ★ 容器预热池（warm_pool.h：启动时预创建 K 个 idle，异步补齐）
- [ ] ★ 判题调度器（judge_scheduler.h：**线程池 + 任务队列** + 最大并发数 + 30s 硬超时）
- [ ] ★ 异步判题流程（POST 立即返回 submission_id，worker 异步执行）
- [ ] ★ 提交数据模型（submission_repo.h：pending/running/终态全生命周期）
- [ ] ★ 提交代码 API（POST /api/v1/submissions 异步）
- [ ] ★ 查询提交结果 API（GET /api/v1/submissions/:id）
- [ ] ★ 提交历史列表 API（GET /api/v1/submissions，**非 admin 强制 user_id = 自己**）
- [ ] ★ g++ 安全编译标志 + 独立编译超时（防编译炸弹）
- [ ] ★ OLE 判定分支（> 16MB 截断 → 判 OLE 立即终止）
- [ ] ★ 文本归一化（CRLF/BOM 归一化，§7.4）
- [ ] ★ 内存/时间精确测量（cgroup v2 `cpu.stat` / `memory.current`）
- [ ] ★ `judge_type` 判题分支（exact / ignore_trailing / float_eps / special）
- [ ] ★ SE 系统错误状态（容器/DB 异常时使用）
- [ ] ☆ SSE 推送（GET /api/v1/submissions/sse/:id）
- [ ] ☆ Special Judge 框架（v1.3）

### Phase 5 - 前端页面

- [ ] ★ 前端框架（公共导航栏 + api.js 封装 + 统一错误处理 + 401 自动跳转登录）
- [ ] ★ CSP 头 + CDN 资源 SRI 配置
- [ ] ★ Markdown XSS 净化（DOMPurify + marked）
- [ ] ★ Token 存储（access 内存 / refresh HttpOnly cookie）
- [ ] ★ 登录页面（/login.html）
- [ ] ★ 注册页面（/register.html，密码强度提示）
- [ ] ★ 题目列表页面（/，筛选 + 分页）
- [ ] ★ 题目详情 + 代码编辑器页面（双栏布局，集成 CodeMirror/Monaco）
- [ ] ★ **编辑器草稿持久化**（localStorage，提交成功后清除，刷新提示恢复）
- [ ] ★ 提交结果展示（AC/WA/TLE 状态 + 耗时/内存 + **失败时显示测试点**）
- [ ] ★ 异步判题轮询/SSE 客户端
- [ ] ★ 提交历史标签页（刷题页下方）
- [ ] ★ 个人主页（/profile/:username，做题统计）
- [ ] ★ 排行榜页面（/ranking.html）
- [ ] ★ 管理后台 - 题目管理页面（/admin/problems.html）
- [ ] ★ 管理后台 - 批量导入页面（/admin/problems.html 导入区）
- [ ] ★ 管理后台 - 用户管理页面（/admin/users.html）
- [ ] ★ 管理后台 - 系统概览页面（/admin/dashboard.html，含队列/预热池/指标）
- [ ] ★ 管理后台 - 审计日志页面（/admin/audit-logs.html）
- [ ] ★ 前端权限拦截（非管理员 → 跳转首页，未登录 → 跳转登录）
- [ ] ★ 深色模式（CSS 变量 + `prefers-color-scheme` + 手动切换持久化）
- [ ] ☆ 移动端响应式（< 768px 切换上下布局）

### Phase 6 - 统计与安全

- [ ] ★ 用户做题统计 API（GET /api/v1/stats/profile/:username）
- [ ] ★ 排行榜 API（GET /api/v1/stats/ranking）
- [ ] ★ 管理员用户管理 API（GET /api/v1/admin/users, PUT /api/v1/admin/users/:id/role，**写 audit_logs**）
- [ ] ★ 管理员系统统计 API（GET /api/v1/admin/stats，**含队列/预热池状态**）
- [ ] ★ 审计日志 API（GET /api/v1/admin/audit-logs）
- [ ] ★ 判题队列状态 API（GET /api/v1/admin/queue）
- [ ] ★ 安全加固（输入校验 + SQL 参数化 + XSS 防护 + CSP + SRI）
- [ ] ★ 错误处理统一（§5.7 错误码 + 响应格式）
- [ ] ☆ 失败登录锁定（连续 N 次失败 15 分钟内禁止该用户名登录）

### Phase 7 - 部署

- [ ] ★ 完善 Docker Compose（Web + MySQL + Judge + Socket Proxy + Caddy + Prometheus + Grafana）
- [ ] ★ Docker Socket 代理（[tecnativa/docker-socket-proxy](https://github.com/Tecnativa/docker-socket-proxy)，白名单 5 子命令）
- [ ] ★ Web 容器资源限制（--cpus=2 --memory=512m，非 root 运行）
- [ ] ★ Caddy 反向代理（生产 HTTPS / 本地 HTTP）
- [ ] ★ README + 部署文档（环境变量 + 管理员创建 + 灾备恢复）
- [ ] ☆ 备份脚本（backup.sh：mysqldump 每日 + 异地）
- [ ] ☆ 监控告警（Grafana 面板：判题 P99 延迟 > 5s 告警 / 队列积压 > 50 告警）

### Phase 8 - 质量保障（v1.2 新增）

- [ ] ★ CI/CD 流水线（GitHub Actions：编译 + 单测 + 集成测试 + lint）
- [ ] ★ 单元测试覆盖率 ≥ 60%（核心模块：auth / judge / repo / rate_limit / audit）
- [ ] ★ E2E 验收脚本（scripts/e2e_acceptance.sh：覆盖 §12.1 所有 A1–A34 用例）
- [ ] ★ 编译炸弹防护测试（提交模板元递归 / `#include` 炸弹，验证 10s 超时）
- [ ] ★ OLE 判定测试（提交死循环输出 100MB，验证 OLE + 容器不被撑爆）
- [ ] ★ 限流测试（注册/登录 1 分钟内 100 次请求，验证 429 + Retry-After）
- [ ] ★ 审计日志测试（删题/改角色后查 audit-logs 验证写入）
- [ ] ☆ 压测报告（5/10/20 人并发判题，验证 P95 < 5s）
- [ ] ☆ 渗透测试（XSS / SQL 注入 / CSRF 扫描）
- [ ] △ 模糊测试（fuzzing）判题输入

### Phase 9 - 运维与监控（v1.2 新增）

- [ ] ★ Prometheus 指标接入（`/api/v1/metrics`：submissions_total{status}、judge_duration_seconds、queue_size、warm_pool_size、db_pool_active）
- [ ] ★ Grafana 面板（系统概览 / 判题 P95 / 错误率 / 队列 / 资源）
- [ ] ★ 日志聚合（stdout JSON 格式，Docker logs 接管，可选 Loki/ELK）
- [ ] ★ 日志轮转（logrotate 或 Docker log driver `json-file` + `max-size`）
- [ ] ☆ 备份验证（每月 1 次 restore drill 到测试环境）
- [ ] ☆ 告警规则（P99 延迟、队列积压、磁盘、证书过期）
- [ ] △ 性能 Profile（`perf` / `flamegraph` 跑一次判题热路径）

---

## 12. 验收标准

### 12.1 MVP 必须通过的验收用例

| # | 验收用例 | 通过标准 |
|---|---------|---------|
| A1 | 用户注册 | 注册成功返回 201，密码以 bcrypt cost=12 哈希存储 |
| A2 | 用户登录 | 登录成功返回 access (2h) + refresh (7d) |
| A3 | 未授权访问 | 未携带 token 访问受保护 API 返回 401 + 统一错误格式 |
| A3b | 非管理员访问管理 API | 普通用户访问 /api/v1/admin/* 返回 403 |
| A4 | 题目列表 | 返回分页题目列表，支持按难度/标签筛选，**软删题目不出现** |
| A5 | 题目详情 | 返回题目描述 + 示例用例，**Markdown 已 XSS 净化** |
| A6 | 正确代码提交 | 异步返回 submission_id；轮询后状态=AC，附耗时 + 内存 |
| A7 | 错误代码提交 | 异步返回 submission_id；轮询后状态=WA + 失败测试点信息 |
| A8 | 死循环代码 | 提交 `while(true)` → TLE，**响应在 5s 内**，服务器不崩溃 |
| A9 | 内存爆炸代码 | 提交 `malloc(INT_MAX)` → MLE，服务器不崩溃 |
| A10 | 编译错误代码 | 提交语法错误 → CE + 编译错误信息（截断至 4KB） |
| A11 | 网络访问代码 | 提交含 socket 代码 → 容器无网络，返回 RE |
| A12 | 文件系统访问 | 提交读 `/etc/passwd` → 容器隔离（--read-only + 非 root），返回 RE |
| A13 | 双栏刷题页 | 左侧题目正确渲染（无 XSS），右侧编辑器语法高亮 |
| A14 | 提交历史 | 可查看自己的历史提交；**非 admin 查 user_id=X (非自己) 返回空或 403** |
| A15 | 个人主页 | 显示已解决数、通过率、提交统计 |
| A16 | Docker Compose | `docker-compose up` 一键启动全部服务（含 socket proxy / caddy） |
| A17 | 题目批量导入 | 管理员上传 JSON 后可通过 API 查询到，**audit_logs 有一条记录** |
| A18 | 管理员创建题目 | POST /api/v1/admin/problems 成功，**audit_logs 有记录** |
| A19 | 管理员编辑题目 | PUT /api/v1/admin/problems/:slug 成功 |
| A20 | 管理员删除题目 | DELETE 软删除成功，列表 API 不再返回该题，**audit_logs 有记录** |
| A21 | 普通用户无法导入题目 | 调用 /api/v1/admin/problems/import 返回 403 |
| A22 | 初始管理员账户 | 系统初始化后存在至少一个 admin 账户可登录管理后台 |
| A23 | 管理后台页面 | 管理员登录后导航栏显示"管理后台"入口 |
| A24 | 非管理员前端拦截 | 普通用户访问 /admin/* 被前端拦截跳转至首页 |
| **A25** | **异步判题状态流转**（v1.2） | 提交后立即返回 pending；轮询可见 pending→running→终态；终态后 status 不再变化 |
| **A26** | **限流生效**（v1.2） | 1 分钟内注册 6 次 → 第 6 次返回 429 + Retry-After |
| **A27** | **审计日志写入**（v1.2） | 删题/改角色/批量导入后，audit_logs 表有对应记录 |
| **A28** | **容器预热池生效**（v1.2） | `/api/v1/health` 返回 `warm_pool ≥ 1`；判题任务 worker 优先从池取容器 |
| **A29** | **编译炸弹防护**（v1.2） | 提交模板元递归 / 巨型宏 → CE (Compilation timeout)，判题在 15s 内返回 |
| **A30** | **OLE 判定**（v1.2） | 提交死循环 `while(true) printf("a");` → 判 OLE，**容器内存不被撑爆**（限制 16MB 输出） |
| **A31** | **健康检查**（v1.2） | `GET /api/v1/health` 返回 DB / Docker 状态，docker-compose healthcheck 通过 |
| **A32** | **Markdown XSS 防护**（v1.2） | 题目描述含 `<script>alert(1)</script>` → 前端不执行，HTML 实体或过滤后展示 |
| **A33** | **编辑器草稿持久化**（v1.2） | 写一半代码刷新页面 → 弹"恢复草稿"提示，确认后代码回来 |
| **A34** | **深色模式**（v1.2） | 切换深色模式后页面正确变色，刷新后保持 |

### 12.2 性能验收

| 指标 | 标准 |
|------|------|
| 提交 API 响应 | < 200ms（立即返回 submission_id） |
| 判题响应（P95） | < 5s（简单题 < 3s） |
| 题目列表 API | < 200ms |
| 排行榜 API | < 500ms |
| 并发判题 | 支持 10 人同时提交不阻塞，超出排队 |
| 健康检查 | < 50ms |

---

## 13. 风险与权衡

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| cpp-httplib 单线程/低并发 | 高并发时性能瓶颈 | **v1.2 缓解**：启用 ThreadPool；预留切换 Crow/userver 的路径 |
| Docker 容器启动延迟 | 判题响应慢 | **v1.2 缓解**：容器预热池 + 复用 idle 容器 |
| Docker Socket 暴露 Web 容器 | Web 被攻破后控制宿主机 | **v1.2 缓解**：Docker Socket 代理（白名单 5 子命令）+ Web 容器非 root |
| 编译炸弹 | 单提交占用判题资源过久 | **v1.2 缓解**：g++ 独立 10s 超时 + Docker pids-limit |
| 恶意代码读敏感文件 | 信息泄露 | **v1.2 缓解**：--read-only + 非 root 运行 + 网络隔离 |
| C++ ORM 生态不如其他语言 | 数据库操作开发效率低 | 封装基础 Repo 层，手写 SQL |
| 前端原生 JS 开发效率低 | UI 开发慢 | **v1.2 缓解**：DOMPurify / marked 轻量库 |
| 浮点数判题 PE 边界模糊 | 判题结果争议 | **v1.2 缓解**：`judge_type=float_eps` 显式字段，默认走 exact |
| Docker 判题资源消耗 | 单机内存压力 | **v1.2 缓解**：判题线程池最大并发限制 + 队列机制 |
| 管理员权限提升风险 | 误操作或恶意操作 | **v1.2 缓解**：audit_logs 全量记录 + Grafana 异常告警 |
| 初始管理员创建 | 首次部署时无管理账户 | 提供创建管理员脚本，文档明确说明 |
| 提交表无限增长 | 长期运行后 DB 膨胀 | **v1.2 缓解**：90 天前失败提交清理 + AC 永久保留 + 备份策略 |
| 同步判题阻塞 HTTP | 5+ 人并发时接口卡死 | **v1.2 缓解**：异步判题 + 任务队列 + 轮询/SSE |
| CSRF 攻击 | 跨站请求伪造 | **v1.2 缓解**：refresh token HttpOnly + SameSite=Strict cookie |

---

## 14. 后续迭代规划（Post-MVP）

| 版本 | 功能 |
|------|------|
| v1.3 | Special Judge、Markdown 预净化强制、problem_revisions 编辑历史、比赛/Contest 模块 |
| v1.4 | 讨论区、题解、收藏、错题本 |
| v1.5 | 多语言支持（Python/Java/Go）、管理员操作审计日志增强、系统监控面板（已 v1.2 部分落地） |
| v2.0 | 多实例 + Redis session 共享、判题微服务拆分 |

---

## 15. 安全设计（v1.2 新增）

> 将散落在各章节的安全策略**集中归档**，方便安全审计和新人上手。

### 15.1 认证与会话

- bcrypt cost=12 哈希密码
- JWT HS256，access 2h + refresh 7d
- refresh token 注销入 Redis 黑名单（TTL = 剩余有效期）
- refresh token 走 `HttpOnly; Secure; SameSite=Strict` cookie
- 失败登录 5 次 → 写 audit_logs，可选锁定 15 分钟

### 15.2 API 安全

- 全部 SQL 参数化（`?` 占位符），禁止字符串拼接
- 限流：注册 5/分/IP、登录 10/分/IP、提交 30/分/用户、管理员写操作 30/分/分/IP
- 非 admin 查他人提交历史 → 403 或返回空
- 管理后台路径（/api/v1/admin/*）严格 role=admin 校验
- CORS 白名单（生产从 env 读取）

### 15.3 前端安全

- CSP `default-src 'self'`，CDN 加 SRI
- 题目描述 Markdown 经 DOMPurify 净化
- 用户输入（昵称、评论）双向 XSS 防护
- access token 存内存（防 XSS 盗取），refresh 走 cookie
- 提交前前端基础校验（长度、字符集）

### 15.4 判题沙箱

- 容器 `--read-only` + 非 root 运行 + `--network=none` + `--pids-limit=50`
- 编译 g++ 启用 `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Wl,-z,now -Wl,-z,relro`
- 编译独立 10s 超时（防编译炸弹）
- 运行 30s 硬超时（防 judge.sh 自身卡死）
- 输出 16MB 截断 → OLE
- CRLF/BOM 归一化，避免 Windows 编码差异
- 内存/时间从 cgroup v2 精确读取

### 15.5 容器编排

- Web → Docker 经 **socket-proxy**（白名单 5 子命令）
- Web 容器 `--cpus=2 --memory=512m`，非 root 运行
- Judge 容器每判题任务**独立** docker run，不复用容器实例（仅预热池）
- 资源硬限制：max-concurrent=4 + 队列上限=50，超出返回 503

### 15.6 操作审计

- 管理员**所有**写操作（CRUD / 批量导入 / 改角色）写入 `audit_logs`
- 失败登录 ≥ 5 次写入
- 关键操作（删题、改角色）需前端二次确认
- 审计日志可查询接口：`GET /api/v1/admin/audit-logs`

### 15.7 数据安全

- 密码 bcrypt 哈希存储
- 数据库连接密码从 env 读，不入代码
- mysqldump 每日异地备份，保留 30 天
- 失败提交 90 天后清理，AC 提交永久保留

---

## 16. 运维与监控（v1.2 新增）

### 16.1 健康检查

- `GET /api/v1/health`（公开）：
  ```json
  {
    "status": "ok",
    "db": "ok",
    "docker": "ok",
    "queue_size": 3,
    "warm_pool": 2,
    "uptime_seconds": 86400
  }
  ```
- docker-compose 用作容器 `healthcheck`：`curl -f http://localhost:8080/api/v1/health || exit 1`

### 16.2 Prometheus 指标

`GET /api/v1/metrics`（内网）暴露：

| 指标名 | 类型 | 标签 | 说明 |
|--------|------|------|------|
| `litecode_http_requests_total` | Counter | method, path, status | HTTP 请求计数 |
| `litecode_http_request_duration_seconds` | Histogram | method, path | HTTP 请求耗时 |
| `litecode_submissions_total` | Counter | status | 提交结果分布 |
| `litecode_judge_duration_seconds` | Histogram | status | 判题耗时（队列等待 + 编译 + 运行） |
| `litecode_judge_queue_size` | Gauge | - | 当前排队任务数 |
| `litecode_judge_warm_pool_size` | Gauge | - | 当前预热池容器数 |
| `litecode_db_pool_active` | Gauge | - | DB 连接池活跃连接数 |
| `litecode_auth_failures_total` | Counter | ip | 登录失败计数 |
| `litecode_docker_operations_total` | Counter | operation, status | Docker 操作计数 |

### 16.3 Grafana 面板（建议）

- **系统总览**: 在线人数、提交数 / 分钟、判题 P50/P95/P99
- **判题健康**: 队列长度、预热池大小、判题机 CPU/内存
- **错误率**: 4xx/5xx 比例、CE/TLE/MLE 分布
- **安全**: 失败登录 Top IP、限流触发次数

### 16.4 告警规则（建议）

- 判题 P95 > 5s 持续 5 分钟 → 告警
- 队列积压 > 50 持续 1 分钟 → 告警
- Web 容器内存 > 80% 持续 5 分钟 → 告警
- 失败登录单 IP > 100/小时 → 告警（可能 CC 攻击）
- 磁盘剩余 < 20% → 告警
- 证书过期前 30 天 → 告警

### 16.5 备份与恢复

- `scripts/backup.sh` 每日凌晨 3 点 mysqldump + 压缩 + 异地（OSS/S3）
- 保留策略：日备 7 份 / 周备 4 份 / 月备 6 份
- 每月 1 次 restore drill 到测试环境，验证备份可用

### 16.6 日志策略

- 应用日志：JSON 格式，stdout + 文件，**含 request_id**
- Docker 接管：使用 `json-file` log driver + `max-size=10m` + `max-file=3`
- 关键日志：登录成功/失败、判题异常、管理员写操作、容器启动失败
- 可选：Loki/ELK 聚合

---

*本文档基于 v1.1 经深度审查后升级为 v1.2，重点解决**安全、可观测性、判题异步化、可维护性**四类问题。确认规格完整后进入开发阶段。*
