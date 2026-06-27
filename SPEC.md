# LiteCode-CPP — 产品规格说明书 (SPEC)

> **版本**: v1.1 MVP  
> **日期**: 2026-06-25  
> **定位**: 个人学习型 OJ（Online Judge），以 C++ Web 开发为学习目标  
> **项目名**: LiteCode-CPP

---

## 1. 项目愿景

构建一个轻量级的在线判题系统（OJ），核心复刻 LeetCode 的刷题体验。项目以 **学习 C++ Web 开发** 为首要目标，MVP 阶段聚焦于核心刷题流程跑通，后续迭代完善体验。

**一句话描述**: 用户注册登录 → 浏览题库 → 在双栏界面中阅读题目并编写 C/C++ 代码 → 提交后实时获得判题结果（AC/WA/TLE 等）→ 查看个人提交历史和做题统计。

---

## 2. 需求总览

### 2.1 功能需求

| # | 功能 | 优先级 | 说明 |
|---|------|--------|------|
| F1 | 用户注册/登录 | P0 | 用户名 + 密码，JWT 鉴权 |
| F2 | 题库浏览 | P0 | 题目列表页，支持按难度/标签筛选 |
| F3 | 题目详情 + 代码编辑 | P0 | LeetCode 风格双栏布局：左题目右编辑器 |
| F4 | 代码提交与实时判题 | P0 | 提交 C/C++ 代码 → Docker 沙箱执行 → 返回结果 |
| F5 | 判题结果展示 | P0 | AC / WA / RE / TLE / MLE / PE / CE |
| F6 | 提交历史 | P1 | 查看自己的历史提交及结果 |
| F7 | 个人主页 | P1 | 做题统计（已解决/总数、通过率、提交次数） |
| F8 | 管理员题目导入 | P0 | 仅管理员可批量导入题目（JSON/YAML），普通用户无权操作 |
| F9 | 管理员题目管理 | P0 | 仅管理员可增删改题目，普通用户只读 |
| F10 | 管理员后台页面 | P1 | 管理员专属页面：题目 CRUD、批量导入、用户管理 |
| F11 | 排行榜 | P2 | 按解题数/通过率排名 |

### 2.2 非功能需求

| 维度 | 要求 |
|------|------|
| **性能** | 单次判题响应 < 5s（含容器启动）；支持 5-10 人同时使用 |
| **安全** | Docker 容器隔离 + CPU 时间限制 + 内存限制 + 网络隔离 + 文件系统隔离 + 输出大小限制 |
| **可扩展性** | 判题模块架构预留多语言扩展（C/C++ 优先，后续可加 Python/Java） |
| **部署** | 本地单机运行，Docker Compose 一键启动 |
| **开发周期** | MVP 2-4 周 |

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
│             │ HTTP/REST + JWT (role 權限)                     │
└─────────────┼──────────────────────────────────────────────┘
              │
┌─────────────┼──────────────────────────────────────────────┐
│             ▼        Web 服务器                              │
│              (C++ + cpp-httplib)                             │
│                                                             │
│  ┌─────────┐  ┌─────────┐  ┌──────────┐  ┌───────────┐   │
│  │ 用户模块 │  │ 题目模块 │  │ 提交模块  │  │ 判题调度器 │   │
│  │  Auth   │  │ Problem │  │ Submit   │  │  Judge    │   │
│  └────┬────┘  └────┬────┘  └────┬─────┘  └─────┬─────┘   │
│       │            │            │              │           │
│       │    ┌───────┴───────┐    │              │           │
│       │    │ 管理员中间件   │    │              │           │
│       │    │ (role=admin   │    │              │           │
│       │    │  权限校验)     │    │              │           │
│       │    └───────┬───────┘    │              │           │
│       └────────────┼────────────┘              │           │
│                          │                     │           │
│                     MySQL 连接池               │           │
│                    (ORM/原生SQL)               │           │
└──────────────────────────┼─────────────────────┼──────────┘
                           │                     │
                    ┌──────┴──────┐        ┌──────┴──────┐
                    │   MySQL     │        │ Docker API   │
                    │  数据库     │        │  判题容器    │
                    └─────────────┘        └─────────────┘
```

### 3.2 判题流程图

```
用户提交代码
     │
     ▼
Web 服务器接收请求
     │
     ▼
┌─────────────────┐
│  判题调度器      │
│  JudgeScheduler │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 创建 Docker 容器 │  ← 从预构建镜像启动
│ 设置资源限制     │  ← CPU/内存/网络/文件系统
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 容器内执行流程   │
│ 1. 写入源代码    │
│ 2. 编译 (g++)   │
│ 3. 运行 + 比对  │  ← 逐测试点运行
│ 4. 收集结果      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 结果判定         │
│ AC/WA/TLE/MLE/  │
│ RE/PE/CE        │
└────────┬────────┘
         │
         ▼
返回给用户 + 写入数据库
```

### 3.3 数据流概览

```
前端 (HTML/CSS/JS)
  │
  │  REST API (JSON) + JWT (含 role 字段)
  ▼
路由层 (cpp-httplib)
  │
  ├─ /api/auth/*           → 用户模块 (注册/登录/JWT)
  ├─ /api/problems/*       → 题目模块 (列表/详情/筛选)  [公开]
  ├─ /api/submissions/*    → 提交模块 (提交/历史/统计)  [需登录]
  ├─ /api/stats/*          → 统计模块 (个人统计/排行榜)  [公开/登录]
  ├─ /api/admin/problems/* → 题目管理 (CRUD/批量导入)    [🔒 admin]
  ├─ /api/admin/users/*    → 用户管理 (列表/角色变更)    [🔒 admin]
  └─ /api/admin/stats      → 系统统计                    [🔒 admin]
  │
  ▼
数据层 (MySQL)
  │
  ├─ users           用户表 (含 role 字段)
  ├─ problems        题目表
  ├─ tags            标签表
  ├─ problem_tags    题目-标签关联表
  ├─ test_cases      测试用例表
  └─ submissions     提交记录表
```

---

## 4. 数据模型

### 4.1 users 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| username | VARCHAR(50) UNIQUE | 用户名 |
| password_hash | VARCHAR(255) | bcrypt 哈希 |
| role | ENUM('user','admin') DEFAULT 'user' | 角色：普通用户 / 管理员 |
| email | VARCHAR(100) UNIQUE | 邮箱（可选） |
| avatar | VARCHAR(255) | 头像 URL（默认） |
| created_at | DATETIME | 注册时间 |
| last_login | DATETIME | 最后登录 |

> **权限模型**: 
> - `user`（普通用户）：浏览题目、提交代码、查看提交历史和个人统计
> - `admin`（管理员）：拥有普通用户所有权限 + 题目增删改 + 批量导入 + 用户管理
> - 系统初始化时通过脚本或配置创建第一个管理员账户

### 4.2 problems 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| slug | VARCHAR(100) UNIQUE | 题目 URL 标识（如 two-sum） |
| title | VARCHAR(200) | 题目标题 |
| difficulty | ENUM('easy','medium','hard') | 难度 |
| description | TEXT | 题目描述（Markdown） |
| time_limit | INT | 时间限制（ms），默认 1000 |
| memory_limit | INT | 内存限制（MB），默认 256 |
| accepted_count | INT | 通过人数 |
| submission_count | INT | 总提交数 |
| created_at | DATETIME | 创建时间 |

> **原子性说明**: `tags` 字段原设计为逗号分隔字符串，违反 1NF 原子性要求，已拆分为独立的 `tags` 表和 `problem_tags` 关联表（见 4.2b、4.2c）。

### 4.2b tags 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| name | VARCHAR(50) UNIQUE | 标签名称（如"数组""哈希表"） |

### 4.2c problem_tags 表（题目-标签关联）

| 字段 | 类型 | 说明 |
|------|------|------|
| problem_id | INT FOREIGN KEY → problems(id) | 关联题目 |
| tag_id | INT FOREIGN KEY → tags(id) | 关联标签 |
| PRIMARY KEY | (problem_id, tag_id) | 联合主键 |

> **关系说明**: 题目与标签为多对多关系，一道题可有多个标签，一个标签可对应多道题。通过 `problem_tags` 关联表实现。

### 4.3 test_cases 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| problem_id | INT FOREIGN KEY | 关联题目 |
| input | LONGTEXT | 测试输入 |
| expected_output | LONGTEXT | 期望输出 |
| is_sample | BOOLEAN | 是否为示例用例（展示给用户） |
| order_num | INT | 用例顺序 |

### 4.4 submissions 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| user_id | INT FOREIGN KEY | 提交用户 |
| problem_id | INT FOREIGN KEY | 提交题目 |
| language | ENUM('c','cpp') | 编程语言 |
| code | LONGTEXT | 提交的源代码 |
| status | ENUM('pending','running','ac','wa','re','tle','mle','pe','ce') | 判题结果 |
| time_used | INT | 实际耗时（ms） |
| memory_used | INT | 实际内存（KB） |
| error_message | TEXT | 编译错误/运行时错误信息 |
| created_at | DATETIME | 提交时间 |

---

## 5. API 设计

### 5.1 用户模块

| 方法 | 路径 | 权限 | 说明 |
|------|------|------|------|
| POST | `/api/auth/register` | 公开 | 用户注册（默认 role=user） |
| POST | `/api/auth/login` | 公开 | 用户登录，返回 JWT |
| GET | `/api/auth/profile` | 已登录 | 获取当前用户信息 |

### 5.2 题目模块（公开只读 + 管理员写入）

| 方法 | 路径 | 权限 | 说明 |
|------|------|------|------|
| GET | `/api/problems` | 公开 | 题目列表（支持分页、难度/标签筛选，标签通过 `?tag=数组` 查询，后端 JOIN `problem_tags` + `tags` 表） |
| GET | `/api/problems/:slug` | 公开 | 题目详情（含示例用例 + 标签列表） |
| GET | `/api/tags` | 公开 | 所有标签列表（用于前端筛选下拉） |
| POST | `/api/admin/problems` | 🔒 admin | 创建单道题目（含标签关联，自动创建不存在的标签） |
| PUT | `/api/admin/problems/:slug` | 🔒 admin | 修改题目（含标签关联更新） |
| DELETE | `/api/admin/problems/:slug` | 🔒 admin | 删除题目（级联删除关联的 problem_tags 记录） |
| POST | `/api/admin/problems/import` | 🔒 admin | 批量导入题目（JSON/YAML 文件上传，标签自动创建并关联） |

> **权限说明**: `/api/admin/*` 路径需要 JWT 中 role=admin，非管理员访问返回 403 Forbidden。

### 5.3 提交模块

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/submissions` | 提交代码判题 |
| GET | `/api/submissions/:id` | 查询单次提交结果 |
| GET | `/api/submissions?problem_id=&user_id=` | 提交历史列表 |

### 5.4 统计模块

| 方法 | 路径 | 权限 | 说明 |
|------|------|------|------|
| GET | `/api/stats/profile/:username` | 已登录 | 用户做题统计 |
| GET | `/api/stats/ranking` | 公开 | 排行榜 |

### 5.5 管理员模块

| 方法 | 路径 | 权限 | 说明 |
|------|------|------|------|
| GET | `/api/admin/users` | 🔒 admin | 用户列表（含角色） |
| PUT | `/api/admin/users/:id/role` | 🔒 admin | 修改用户角色（提升/降级管理员） |
| GET | `/api/admin/stats` | 🔒 admin | 系统统计（题目数、用户数、提交数等） |

---

## 6. 前端页面设计

### 6.1 页面清单

| 页面 | 路径 | 权限 | 说明 |
|------|------|------|------|
| 首页/题库列表 | `/` | 公开 | 题目列表，筛选、搜索 |
| 刷题页 | `/problems/:slug` | 公开 | 双栏：左题目右编辑器 |
| 登录页 | `/login` | 公开 | 用户登录 |
| 注册页 | `/register` | 公开 | 用户注册 |
| 个人主页 | `/profile/:username` | 已登录 | 做题统计、提交历史 |
| 排行榜 | `/ranking` | 公开 | 全站排名 |
| 🔒 管理后台-题目管理 | `/admin/problems` | admin | 题目列表、增删改、批量导入 |
| 🔒 管理后台-题目编辑 | `/admin/problems/edit/:slug` | admin | 新建/编辑题目表单 |
| 🔒 管理后台-用户管理 | `/admin/users` | admin | 用户列表、角色管理 |
| 🔒 管理后台-系统概览 | `/admin/dashboard` | admin | 系统统计（题目数、用户数、提交数） |

> **导航逻辑**: 普通用户导航栏只显示"题库/排行榜/个人主页"；管理员用户额外显示"管理后台"入口。非管理员直接访问 `/admin/*` 路径时前端拦截跳转至首页。

### 6.2 刷题页布局（核心页面）

```
┌──────────────────────────────────────────────────┐
│  Logo    题库  排行榜  讨论          [用户头像 ▾] │
├─────────────────────┬────────────────────────────┤
│                     │  ┌─────────────────────┐   │
│   题目描述区域       │  │   代码编辑器          │   │
│                     │  │  (CodeMirror/Monaco)  │   │
│   - 描述            │  │                       │   │
│   - 示例输入/输出    │  │  C++  ▼              │   │
│   - 约束条件        │  │                       │   │
│                     │  └─────────────────────┘   │
│                     │  ┌─────────────────────┐   │
│                     │  │   运行结果 / 提交结果  │   │
│                     │  │   AC ✅ / WA ❌        │   │
│                     │  │   时间: 12ms 内存: 2MB │   │
│                     │  └─────────────────────┘   │
│                     │  [运行] [提交]               │
├─────────────────────┴────────────────────────────┤
│  提交历史标签页                                     │
└──────────────────────────────────────────────────┘
```

---

## 7. 判题模块详细设计

### 7.1 Docker 判题流程

```
1. 接收提交 → 写入代码到临时文件
2. 调用 Docker API 创建容器
   - 镜像: litecode-judge:latest
   - 资源限制:
     · --cpus=1 --cpu-period=100000 --cpu-quota=100000
     · --memory=256m --memory-swap=256m
     · --network=none
     · --read-only (除 /tmp 写入目录)
     · --pids-limit=50
     · --security-opt=no-new-privileges
   - 挂载: 代码文件 + 测试数据（只读）
3. 容器内执行:
   a. 编译: g++ -O2 -std=c++17 -o solution solution.cpp
      - 编译失败 → 返回 CE
   b. 逐测试点运行:
      - 运行: timeout {time_limit} ./solution < input.txt > output.txt
      - 判断结果:
        · 超时 → TLE
        · 内存超限 → MLE
        · 非零退出码 → RE
        · 正常退出 → 比对 output.txt 与 expected.txt
          - 完全匹配 → AC (此测试点)
          - 忽略尾部空白后匹配 → PE
          - 不匹配 → WA
   c. 限制输出大小（> 16MB 截断）
4. 收集所有测试点结果 → 汇总判题结果
5. 删除容器 → 返回结果
```

### 7.2 判题 Docker 镜像 (Dockerfile)

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    g++ gcc gdb \
    && rm -rf /var/lib/apt/lists/*
# 判题运行脚本
COPY judge.sh /usr/local/bin/judge.sh
RUN chmod +x /usr/local/bin/judge.sh
WORKDIR /judge
ENTRYPOINT ["/usr/local/bin/judge.sh"]
```

### 7.3 判题安全策略汇总

| 策略 | 实现方式 | 参数 |
|------|----------|------|
| CPU 时间限制 | Docker `--cpus` + 容器内 `timeout` 命令 | 默认 1s |
| 内存限制 | Docker `--memory` | 默认 256MB |
| 网络隔离 | Docker `--network=none` | 完全禁止网络 |
| 文件系统隔离 | Docker `--read-only` + 临时写入目录 | 只读根 + /tmp 可写 |
| 进程数限制 | Docker `--pids-limit` | 最多 50 个进程 |
| 输出大小限制 | 运行脚本中截断 | 最大 16MB |
| 权限提升防护 | Docker `--security-opt=no-new-privileges` | 禁止提权 |

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
      "is_sample": true
    }
  ]
}
```

> **标签存储说明**: JSON 导入格式中 `tags` 为字符串数组，属于传输层的序列化格式，不违反原子性。后端导入时应将每个标签查找或创建到 `tags` 表，再写入 `problem_tags` 关联表，确保数据库中标签存储符合 1NF。

### 8.2 批量导入（🔒 管理员专属）

仅管理员可通过 API 端点批量上传题目。普通用户调用此接口返回 403。

```
POST /api/admin/problems/import
Authorization: Bearer <admin-jwt-token>
Content-Type: multipart/form-data

files: problems/*.json
```

> **设计意图**: 题目来源完全由管理员控制，普通用户只能浏览和做题，不能导入或创建题目。

---

## 9. 技术栈汇总

| 层 | 技术选型 | 说明 |
|----|---------|------|
| **前端** | HTML + CSS + JavaScript | 原生，无框架 |
| **代码编辑器** | CodeMirror 6 或 Monaco Editor | CDN 引入 |
| **后端** | C++17 + cpp-httplib | header-only HTTP 库 |
| **数据库** | MySQL 8.0 | 关系型存储 |
| **数据库连接** | mysql-connector/cpp 或 sqlpp11 | C++ MySQL 驱动 |
| **判题沙箱** | Docker Engine API | 容器隔离执行 |
| **认证** | JWT (jwt-cpp 库) | 无状态鉴权 |
| **密码哈希** | bcrypt (通过 libbcrypt 或自实现) | 安全存储 |
| **构建** | CMake | 跨平台构建 |
| **部署** | Docker Compose | 一键启动 Web + MySQL + Judge |

---

## 10. 项目目录结构

```
litecode-cpp/
├── CMakeLists.txt
├── docker-compose.yml
├── Dockerfile              # Web 服务镜像
├── judge/                  # 判题模块
│   ├── Dockerfile          # 判题镜像
│   ├── judge.sh            # 判题执行脚本
│   └── judge_server.h      # 判题调度器
├── src/                    # 后端源码
│   ├── main.cpp
│   ├── server.h            # HTTP 服务器入口
│   ├── config.h            # 配置管理
│   ├── logger.h            # 日志封装（INFO/WARN/ERROR，输出到 stdout + 文件）
│   ├── db/
│   │   ├── connection_pool.h
│   │   ├── user_repo.h
│   │   ├── problem_repo.h
│   │   ├── tag_repo.h          # 标签 + 题目-标签关联 CRUD
│   │   └── submission_repo.h
│   ├── auth/
│   │   ├── jwt_utils.h
│   │   ├── password_hash.h
│   │   └── admin_middleware.h  # 管理员权限校验中间件
│   ├── routes/
│   │   ├── auth_routes.h
│   │   ├── problem_routes.h
│   │   ├── submission_routes.h
│   │   ├── stats_routes.h
│   │   └── admin_routes.h     # 管理员路由（题目CRUD/导入/用户管理）
│   └── judge/
│       ├── judge_scheduler.h
│       └── docker_client.h
├── web/                    # 前端静态文件
│   ├── index.html           # 题目列表
│   ├── problem.html         # 刷题页（双栏）
│   ├── login.html
│   ├── register.html
│   ├── profile.html
│   ├── ranking.html
│   ├── admin/               # 🔒 管理员页面
│   │   ├── dashboard.html    # 系统概览
│   │   ├── problems.html     # 题目管理（列表+导入）
│   │   ├── problem-edit.html # 新建/编辑题目
│   │   └── users.html        # 用户管理
│   ├── css/
│   │   └── style.css
│   └── js/
│       ├── app.js
│       ├── api.js           # API 调用封装（含 admin API）
│       ├── editor.js         # 代码编辑器封装
│       └── admin.js          # 管理员页面逻辑
├── problems/                # 题目数据
│   ├── two-sum.json
│   ├── add-two-numbers.json
│   └── ...
├── scripts/
│   ├── init_db.sql          # 数据库初始化（含 users/problems/tags/problem_tags/test_cases/submissions 表）
│   ├── create_admin.sql     # 创建初始管理员账户
│   └── seed_problems.py     # 示例题目生成脚本
├── logs/                    # 日志文件输出目录（运行时生成）
└── tests/                   # 测试
    ├── unit/                # 单元测试
    │   ├── test_auth.cpp
    │   ├── test_problem.cpp
    │   ├── test_judge.cpp
    │   └── test_stats.cpp
    └── integration/         # 集成测试
        ├── test_api.cpp
        └── test_judge_flow.cpp
```

---

## 11. TODO 清单（MVP 开发计划）

### Phase 1 - 基础设施

- [ ] 项目目录结构 + CMake 构建（引入 cpp-httplib、mysql-connector、jwt-cpp）
- [ ] 数据库初始化脚本（建表 SQL + 初始管理员种子数据）
- [ ] 配置管理（config.h：数据库/端口/密钥等配置项）
- [ ] 日志封装（logger.h：INFO/WARN/ERROR，stdout + 文件）
- [ ] 数据库连接池（connection_pool.h：连接池 + 基础查询封装）
- [ ] HTTP 服务框架（server.h：路由注册 + CORS + 统一 JSON 响应）
- [ ] Docker Compose 开发环境（Web + MySQL 容器一键启动）

### Phase 2 - 登录注册模块

- [ ] JWT 工具（jwt_utils.h：签发 + 验证 + 提取 user_id/role）
- [ ] 密码哈希（password_hash.h：bcrypt 加密/验证）
- [ ] JWT 认证中间件（拦截请求，提取用户信息）
- [ ] 管理员权限中间件（校验 role=admin，非管理员返回 403）
- [ ] 用户注册 API（POST /api/auth/register）
- [ ] 用户登录 API（POST /api/auth/login，返回 JWT）
- [ ] 用户信息 API（GET /api/auth/profile）

### Phase 3 - 题目模块

- [ ] 题目数据模型（problem_repo.h：题目 CRUD）
- [ ] 标签数据模型（tag_repo.h：标签 + 题目-标签关联）
- [ ] 题目列表 API（GET /api/problems，分页 + 筛选）
- [ ] 题目详情 API（GET /api/problems/:slug，含示例用例 + 标签）
- [ ] 标签列表 API（GET /api/tags）
- [ ] 管理员题目 CRUD API（POST/PUT/DELETE /api/admin/problems/*）
- [ ] 管理员批量导入 API（POST /api/admin/problems/import）
- [ ] 示例题目数据（5-10 道 JSON 题目文件）

### Phase 4 - 代码执行与判题模块

- [ ] 判题 Docker 镜像（judge/Dockerfile + judge.sh）
- [ ] Docker 客户端（docker_client.h：创建/执行/销毁容器）
- [ ] 判题调度器（judge_scheduler.h：编译 → 逐点运行 → 比对 → 汇总结果）
- [ ] 提交数据模型（submission_repo.h：提交记录 CRUD）
- [ ] 提交代码 API（POST /api/submissions → 判题 → 返回结果）
- [ ] 查询提交结果 API（GET /api/submissions/:id）
- [ ] 提交历史列表 API（GET /api/submissions?problem_id=&user_id=）

### Phase 5 - 前端页面

- [ ] 前端框架（公共导航栏 + api.js 封装 + 统一错误处理）
- [ ] 登录页面（/login.html）
- [ ] 注册页面（/register.html）
- [ ] 题目列表页面（/，筛选 + 分页）
- [ ] 题目详情 + 代码编辑器页面（双栏布局，集成 CodeMirror/Monaco）
- [ ] 提交结果展示（AC/WA/TLE 状态 + 耗时/内存）
- [ ] 提交历史标签页（刷题页下方）
- [ ] 个人主页（/profile/:username，做题统计）
- [ ] 排行榜页面（/ranking.html）
- [ ] 管理后台 - 题目管理页面（/admin/problems.html）
- [ ] 管理后台 - 批量导入页面（/admin/problems.html 导入区）
- [ ] 管理后台 - 用户管理页面（/admin/users.html）
- [ ] 管理后台 - 系统概览页面（/admin/dashboard.html）
- [ ] 前端权限拦截（非管理员 → 跳转首页，未登录 → 跳转登录）

### Phase 6 - 统计与安全

- [ ] 用户做题统计 API（GET /api/stats/profile/:username）
- [ ] 排行榜 API（GET /api/stats/ranking）
- [ ] 管理员用户管理 API（GET /api/admin/users, PUT /api/admin/users/:id/role）
- [ ] 管理员系统统计 API（GET /api/admin/stats）
- [ ] 安全加固（输入校验 + SQL 注入防护 + XSS 防护）
- [ ] 错误处理统一（4xx/5xx 统一响应格式）

### Phase 7 - 部署

- [ ] 完善 Docker Compose（生产配置 + 判题容器池）
- [ ] README + 部署文档（环境变量 + 管理员创建说明）

---

## 12. 验收标准

### 12.1 MVP 必须通过的验收用例

| # | 验收用例 | 通过标准 |
|---|---------|---------|
| A1 | 用户注册 | 注册成功返回 201，密码以 bcrypt 哈希存储 |
| A2 | 用户登录 | 登录成功返回 JWT token，后续请求可鉴权 |
| A3 | 未授权访问 | 未携带 token 访问受保护 API 返回 401 |
| A3b | 非管理员访问管理 API | 普通用户访问 /api/admin/* 返回 403 Forbidden |
| A4 | 题目列表 | 返回分页题目列表，支持按难度/标签筛选 |
| A5 | 题目详情 | 返回题目描述 + 示例用例 |
| A6 | 正确代码提交 | 提交正确 C++ 代码 → 返回 AC + 耗时 + 内存 |
| A7 | 错误代码提交 | 提交错误代码 → 返回 WA + 失败测试点信息 |
| A8 | 死循环代码 | 提交 `while(true)` → 返回 TLE，服务器不崩溃 |
| A9 | 内存爆炸代码 | 提交 `malloc(INT_MAX)` → 返回 MLE，服务器不崩溃 |
| A10 | 编译错误代码 | 提交语法错误代码 → 返回 CE + 编译错误信息 |
| A11 | 网络访问代码 | 提交含 socket 代码 → 容器无网络，返回 RE |
| A12 | 文件系统访问 | 提交读 `/etc/passwd` 的代码 → 容器隔离，返回 RE |
| A13 | 双栏刷题页 | 左侧题目正确渲染，右侧编辑器语法高亮 |
| A14 | 提交历史 | 可查看自己的历史提交及结果 |
| A15 | 个人主页 | 显示已解决数、通过率、提交统计 |
| A16 | Docker Compose | `docker-compose up` 一键启动全部服务 |
| A17 | 题目批量导入 | 管理员上传 JSON 题目文件后可通过 API 查询到 |
| A18 | 管理员创建题目 | 管理员通过 POST /api/admin/problems 创建新题目成功 |
| A19 | 管理员编辑题目 | 管理员通过 PUT /api/admin/problems/:slug 修改题目成功 |
| A20 | 管理员删除题目 | 管理员通过 DELETE /api/admin/problems/:slug 删除题目成功 |
| A21 | 普通用户无法导入题目 | 普通用户调用 POST /api/admin/problems/import 返回 403 |
| A22 | 初始管理员账户 | 系统初始化后存在至少一个 admin 账户可登录管理后台 |
| A23 | 管理后台页面 | 管理员登录后导航栏显示"管理后台"入口 |
| A24 | 非管理员前端拦截 | 普通用户访问 /admin/* 被前端拦截跳转至首页 |

### 12.2 性能验收

| 指标 | 标准 |
|------|------|
| 单次判题响应 | < 5s（含容器启动，简单题目 < 3s） |
| 题目列表 API | < 200ms |
| 并发判题 | 支持 5 人同时提交不阻塞 |

---

## 13. 风险与权衡

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| cpp-httplib 单线程/低并发 | 高并发时性能瓶颈 | MVP 阶段 5-10 人够用；后续可换框架或多实例 |
| Docker 容器启动延迟 | 判题响应慢 | 预热容器池或使用 nsjail 替代 |
| C++ ORM 生态不如其他语言 | 数据库操作开发效率低 | 封装基础 Repo 层，手写 SQL |
| 前端原生 JS 开发效率低 | UI 开发慢 | 引入轻量工具库（如 Alpine.js），但不引入框架 |
| 浮点数判题 PE 边界模糊 | 判题结果争议 | MVP 不支持浮点特殊判题，仅精确比对 |
| Docker 判题资源消耗 | 单机内存压力 | 限制并发判题数（队列机制） |
| 管理员权限提升风险 | 误操作或恶意操作 | 关键操作（删除题目/变更角色）需二次确认；操作日志可追溯 |
| 初始管理员创建 | 首次部署时无管理账户可用 | 提供创建管理员脚本，文档明确说明 |

---

## 14. 后续迭代规划（Post-MVP）

| 版本 | 功能 |
|------|------|
| v1.1 | 排行榜、题目收藏、做题进度追踪 |
| v1.2 | 多语言支持（Python/Java）、Special Judge |
| v1.3 | 比赛/Contest 模块 |
| v1.4 | 讨论区、题解 |
| v1.5 | 管理员操作审计日志、系统监控面板 |

---

*本文档由深度访谈生成，确认规格完整后进入开发阶段。*