# LiteCode-CPP — CI/CD 文档

> SPEC §15.8 / Phase 8 ★ CI/CD 流水线。本文档覆盖 `.github/workflows/`
> 的 4 个 jobs 运行机制、本地复现、依赖关系、故障排查与扩展指引。

---

## 1. 总览

| Workflow | 触发器 | Jobs | 跑测平台 |
|---|---|---|---|
| `ci.yml` | push to main / PR / workflow_dispatch | lint + build + integration-test + docker-build | GitHub-hosted ubuntu-22.04 |
| `release.yml` | tag v*.*.* / manual | ci + 多架构 docker build + GHCR push + GitHub Release | GitHub-hosted ubuntu-22.04 |
| `dependabot.yml` | 每周一 09:00 | 自动 PR 升级 docker / docker-compose / GitHub Actions 版本 | — |

**总时长**（典型 PR, 50+ test binaries, 4-way ctest 并行）：
- lint: ~1 min
- build (ccache 热): ~3-5 min；冷启 ~10 min
- integration-test (含 MySQL 启动 + ctest): ~8-12 min
- docker-build: ~5-8 min（amd64 only），多架构 ~15-20 min
- **wall-clock**（4 jobs 并行）：~12 min（PR）/ ~20 min（release tag）

**并发控制**：相同 ref 的旧 run 自动 cancel，节省 CI 分钟。

---

## 2. Jobs 详解

### 2.1 lint

跑：
- **shellcheck** —— 扫 `scripts/*.sh` + `tests/e2e/*.sh` + `judge/judge.sh`
- **hadolint** —— Dockerfile + judge/Dockerfile（severity ≥ warning 失败）
- **docker compose config** —— 验证 4 个 profile（default / proxy / monitoring / backup）YAML 语法

不扫 `src/` C++ 代码（v1.2.58 暂不强制 clang-format，避免一波 PR 全是格式 fix）。

### 2.2 build

跑：
1. 装依赖：build-essential + cmake + libmysqlcppconn-dev (Oracle APT) + libssl-dev + libcurl4-openssl-dev + uuid-dev + libprotobuf-dev + ccache
2. ccache 1GB 容量，key = CMakeLists.txt + src/** + tests/** hash（src 改了就失效）
3. cmake configure (`-DUSE_LOCAL_DEPS=ON -DLITECODE_BUILD_TESTS=ON`)
4. cmake build `-j$(nproc)`
5. 跑 `./build/bin/lit_smoke_check`（不需 MySQL 的冒烟）
6. upload artifacts → `build/bin/` + `build/tests/` (7 天保留)
7. 打印 ccache stats（命中率）

### 2.3 integration-test

依赖 build job。关键点：

- **MySQL service container** —— `mysql:8.0.40` 容器化在 job 内启（端口 33060 → 33060）
- **`mysqladmin ping` healthcheck** —— 通过后才跑测试
- **env 注入** —— DB_HOST/PORT/USER/PASSWORD/JWT_SECRET 全用 ephemeral 占位
- **`scripts/init_db.sh`** —— 跑 migrations + 植入 admin
- **ctest** —— `-j4 --output-on-failure -E 'auth_refresh|auth_cookie_storage|warm_pool' --no-tests=error --timeout 120`
  - `-E '...'` 跳过 3 个 pre-existing flake（见 [[pre-existing test flakes]]）
  - `--no-tests=error` 让"环境缺失自动 skip"的 case 不算 fail
  - `--timeout 120` 单 test 上限 2 分钟（防 hang）
- **失败时上传 ctest log** —— `Testing/` + `**/test_detail.xml` 14 天保留

### 2.4 docker-build

依赖 build job。关键点：

- **buildx + type=gha cache** —— 跨 jobs 共享 image layer cache
- **only on main 推 GHCR** —— PR 不推，仅 build 验证可构建性
- **web + judge 两个镜像** —— judge 不带 docker socket mount（CI runner 无 docker）
- **provenance: false** —— web 镜像省 SBOM（judge 仍生成）
- **GHCR 路径** —— `ghcr.io/<owner>/litecode-web:<sha>` + `<branch>` + `latest` (main)

### 2.5 release（仅 release.yml）

- **触发**：git tag `vX.Y.Z` 推送（不匹配 `v1.2.57-rc1` 等 prerelease）
- **多架构** —— `linux/amd64,linux/arm64`（QEMU 模拟 arm64 build）
- **镜像打 semver** —— `v1.2.58` / `v1.2` / `v1` / `latest` + sha
- **GitHub Release** —— 自动 changelog（从上一个 tag 到现在）
- **prerelease 标记** —— tag 含 `-rc` / `-beta` 自动标 prerelease

---

## 3. 本地复现 CI

### 3.1 完整跑一遍（≈ 15 分钟）

```bash
# 1. 装依赖（ubuntu 22.04 / 24.04）
sudo apt-get update && sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config ccache \
    libssl-dev zlib1g-dev libcurl4-openssl-dev uuid-dev libprotobuf-dev protobuf-compiler \
    default-mysql-client

# 2. Oracle MySQL APT 仓库
curl -fsSL https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 \
    | sudo gpg --dearmor -o /usr/share/keyrings/mysql-apt.gpg
echo "deb [signed-by=/usr/share/keyrings/mysql-apt.gpg] \
    https://repo.mysql.com/apt/ubuntu $(lsb_release -cs) mysql-tools" \
    | sudo tee /etc/apt/sources.list.d/mysql.list
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    libmysqlcppconn-dev libmysqlcppconn10 libmysqlcppconnx2

# 3. 软链头文件（Oracle 包装在 mysql-cppconn/ 命名空间下）
sudo ln -sf /usr/include/mysql-cppconn/mysqlx /usr/include/mysqlx
sudo ln -sf /usr/include/mysql-cppconn/mysql  /usr/include/mysql
sudo ln -sf /usr/include/mysql-cppconn/jdbc  /usr/include/jdbc

# 4. 构建（与 ci.yml 一致）
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_LOCAL_DEPS=ON \
    -DLITECODE_BUILD_TESTS=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j$(nproc)

# 5. 跑 MySQL（Docker / 本地 / 公司 dev box 都可）
docker run -d --rm --name litecode-ci-mysql \
    -e MYSQL_ROOT_PASSWORD=rootpass_change_me \
    -e MYSQL_DATABASE=litecode_test \
    -p 33060:33060 \
    mysql:8.0.40

# 6. 等 MySQL ready
until mysql -h127.0.0.1 -P33060 -uroot -prootpass_change_me \
    -e 'SELECT 1' >/dev/null 2>&1; do sleep 2; done

# 7. 初始化 + 跑测试
DB_HOST=127.0.0.1 DB_PORT=33060 DB_USER=root \
DB_PASSWORD=rootpass_change_me DB_NAME=litecode_test \
    ./scripts/init_db.sh root rootpass_change_me 127.0.0.1 33060 litecode_test

cd build && ctest -j4 --output-on-failure \
    -E 'auth_refresh|auth_cookie_storage|warm_pool' \
    --no-tests=error --timeout 120
```

### 3.2 仅跑 lint（≈ 30 秒）

```bash
# shellcheck
shellcheck scripts/*.sh tests/e2e/*.sh judge/judge.sh

# hadolint
hadolint Dockerfile
hadolint judge/Dockerfile

# compose 校验
docker compose config --quiet
docker compose --profile monitoring config --quiet
```

### 3.3 镜像本地构建（与 CI 一致）

```bash
DOCKER_BUILDKIT=1 docker build \
    --cache-from type=gha \
    --cache-to type=gha,mode=max \
    -t litecode-web:dev -f Dockerfile .

DOCKER_BUILDKIT=1 docker build \
    -t litecode-judge:dev -f judge/Dockerfile ./judge
```

---

## 4. 故障排查

### 4.1 integration-test: MySQL 启动超时

```
Error: timeout reached while waiting for MySQL
```

**原因**：`mysqladmin ping` 健康检查 10 次 × 10s 仍失败。

**排查**：
```bash
# 1. 看 MySQL 容器日志
docker compose logs mysql

# 2. 通常是 mysql-connector-c++ X Protocol 端口（33060）未暴露
#    检查 MySQL 容器是否启 caching_sha2_password 插件（v8.0 默认）
docker exec -it litecode-ci-mysql \
    mysql -uroot -prootpass_change_me \
    -e "SHOW VARIABLES LIKE 'mysqlx_port'"
```

**解决**：CI 用 `ports: - 33060:33060` 暴露 X Protocol；本地也用 `-p 33060:33060`。

### 4.2 ctest: `auth_refresh` / `auth_cookie_storage` / `warm_pool` 偶发失败

**原因**：pre-existing flakes（见 [project-pre-existing-test-flakes](../memory/project-pre-existing-test-flakes.md)）。CI 已用 `-E` 跳过；本地也建议加：

```bash
ctest -j4 -E 'auth_refresh|auth_cookie_storage|warm_pool' --timeout 120
```

### 4.3 build: `mysql-cppconn/jdbc.h: No such file or directory`

**原因**：Oracle APT 包把头装在 `/usr/include/mysql-cppconn/` 而非 `/usr/include/`。

**解决**：建软链：
```bash
sudo ln -sf /usr/include/mysql-cppconn/mysqlx /usr/include/mysqlx
sudo ln -sf /usr/include/mysql-cppconn/mysql  /usr/include/mysql
sudo ln -sf /usr/include/mysql-cppconn/jdbc  /usr/include/jdbc
```

CI 已自动建。

### 4.4 docker-build: `failed to solve: failed to compute cache key`

**原因**：Dockerfile `COPY` 了不存在的文件，或 build context 路径错。

**排查**：
```bash
# 本地复现
docker build -f Dockerfile . --progress=plain --no-cache 2>&1 | tail -50
```

### 4.5 release: tag 推送但 GHCR 推不上去

**原因**：仓库没启用 GHCR，或 token 权限不足。

**解决**：
1. GitHub repo → Settings → Packages → 确认 "Allow GitHub Actions to create and approve packages" 开启
2. workflow `permissions` 段需要 `packages: write`（release.yml 已设）

### 4.6 lint: shellcheck 报 SC2155（声明与赋值合一）

```
In scripts/backup.sh line 89:
    local size=$(stat -c%s "$OUTFILE")
                        ^--------^ SC2155: Declare and assign separately.
```

**原因**：shellcheck 默认建议 `local size; size=$(stat ...)`，避免子 shell 失败时变量丢失。

**缓解**：项目惯用合一写法，已在 `.shellcheckrc` 加 `disable=SC2155`。如要全局修正，可手动 `sed -i` 拆开。

---

## 5. 扩展指引

### 5.1 加新 CI job

```yaml
# .github/workflows/ci.yml
my-new-job:
  name: My Job
  runs-on: ubuntu-22.04
  needs: [lint, build]
  steps:
    - uses: actions/checkout@v4
    - run: echo "..."
```

注意：
- 所有 jobs 必须 `runs-on: ubuntu-22.04`（匹配项目 base + Oracle MySQL APT 支持）
- 需要 MySQL 时复用 `services:` 块（或独立写）
- `timeout-minutes` 必填（防 hang）

### 5.2 启用 clang-format lint（Phase 8 后续）

未来要把 .clang-format 强制化：

```yaml
# 在 lint job 加：
- name: clang-format check (src/)
  run: |
    sudo apt-get install -y clang-format-17
    find src -name '*.cpp' -o -name '*.h' | \
        xargs clang-format-17 --dry-run --Werror
```

注意：clang-format 版本与本地版本差异会导致 false positive。建议容器内 `clang-format-17`，并随 .clang-format 一起 lockstep。

### 5.3 加 E2E job（Phase 8 ★ 第 3 项）

E2E 跑 docker compose + 真实判题管线。当前 ci.yml 不含此 job（依赖 docker-in-docker，CI runner 已支持但要 30+ min）。后续单独 job：

```yaml
e2e:
  runs-on: ubuntu-22.04
  needs: docker-build
  steps:
    - uses: actions/checkout@v4
    - run: docker compose up -d --build
    - run: docker compose exec -T web /wait-for-it.sh mysql:33060
    - run: ./scripts/init_db.sh ...
    - run: ./tests/e2e/e2e_acceptance.sh
    - run: docker compose down -v
```

### 5.4 覆盖率 job（Phase 8 ★ 第 2 项，v1.2.59 已实装）

job **已实装**于 `.github/workflows/ci.yml`（覆盖 draft snippet 的 6 个缺口）：

| 缺口 | draft snippet（旧） | 实装后 |
|---|---|---|
| ① `USE_LOCAL_DEPS=ON` | ❌ | ✅ cmake 显式传 |
| ② 保留 `-Wl,--allow-multiple-definition` | ❌（覆盖 linker flag 风险） | ✅ 走 `LITECODE_ENABLE_COVERAGE` CMake option，内部用 `add_link_options(--coverage)` append，**不覆盖** `:209` / `:270` 的 ODR flag |
| ③ ctest `-E` flake | ❌ | ✅ `-E 'auth_refresh\|auth_cookie_storage\|warm_pool'` |
| ④ MySQL service | ❌ | ✅ 复用 `integration-test` 的 `mysql:8.0.40` service block |
| ⑤ build deps (mysql-cppconn) | ❌ | ✅ apt 装 `libmysqlcppconn-dev/10/x2` + 头文件软链 |
| ⑥ 阈值门禁 | ❌（仅 `lcov --list`） | ✅ `scripts/coverage.sh gate` 硬卡 80/40 |

阈值目标：**核心模块 ≥ 80% + 全代码库 ≥ 40%**（v1.2.58 决策，严格按 SPEC.md:1119 写定的 5 块：`auth / judge / repo / rate_limit / audit`）。

### 5.5 本地覆盖率跑法

完全照 CI 在本地 reproduce：

```bash
# 1. configure（带 coverage instrumentation + Debug build）
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_LOCAL_DEPS=ON \
  -DLITECODE_BUILD_TESTS=ON \
  -DLITECODE_ENABLE_COVERAGE=ON

# 2. build
cmake --build build -j"$(nproc)"

# 3. MySQL（任选 docker / native / dev box）
docker run -d --rm --name litecode-coverage-mysql \
  -e MYSQL_ROOT_PASSWORD=rootpass_change_me \
  -e MYSQL_DATABASE=litecode_test \
  -p 33060:33060 \
  mysql:8.0.40

# 4. 初始化
until mysql -h127.0.0.1 -P33060 -uroot -prootpass_change_me \
  -e 'SELECT 1' >/dev/null 2>&1; do sleep 2; done
./scripts/init_db.sh root rootpass_change_me 127.0.0.1 33060 litecode_test

# 5. 跑 ctest（生成 .gcda 数据文件）
cd build && ctest -j4 --output-on-failure \
  -E 'auth_refresh|auth_cookie_storage|warm_pool' \
  --no-tests=error --timeout 120
cd ..

# 6. lcov + 门禁（一行搞定，可拆 capture / report / gate）
sudo apt-get install -y lcov   # 一次性
./scripts/coverage.sh all      # → coverage-raw.info + coverage.info + coverage-core.info + HTML

# 7. 看 HTML（可选）
xdg-open build/coverage-html/index.html
```

`scripts/coverage.sh` 的 sub-command：

| sub-command | 作用 |
|---|---|
| `capture` | `lcov --capture --directory build` → 过滤 → `coverage.info`（全库）+ `coverage-core.info`（核心 5 块） |
| `report` | `lcov --list` + `genhtml` → HTML 在 `build/coverage-html/index.html` |
| `gate` | 解析 `--summary` → awk 浮点比较 → 核心 ≥ 80% & 全库 ≥ 40% 才 exit 0 |
| `all` | capture + report + gate（默认） |

env 覆盖：`BUILD_DIR=foo CORE_MIN=85 REPO_MIN=45 ./scripts/coverage.sh all`

### 5.6 门禁 + Codecov 解释

#### 5.6.1 路径映射（5 核心模块）

| SPEC 模块 | lcov `--extract` pattern | 代码位置 |
|---|---|---|
| `auth` | `*/src/auth/*` | jwt_utils.h, refresh_token.h, password_hash.h, bcrypt/{blf,litecode_bcrypt}.h |
| `judge` | `*/src/judge/*` | warm_pool.h, docker_client.h, judge_scheduler.h, judge_notifier.h |
| `repo` | `*/src/db/*_repo.h` | user/problem/submission/tag/test_case/special_judge/problem_revisions_repo.h |
| `rate_limit` | `*/src/middleware/rate_limit.h` | rate_limit.h（单文件） |
| `audit` | `*/src/db/audit_log_repo.h` + `*/src/routes/admin_audit_log_routes.*` | audit_log_repo.h + admin_audit_log_routes.{h,cpp} |

`repo` ∩ `audit` 在 `audit_log_repo.h` 重叠 → lcov 自动 dedup，无害。

#### 5.6.2 3 个 flake 的处理

`integration-test` job 的 `-E 'auth_refresh|auth_cookie_storage|warm_pool'` 在 `coverage` job **同样应用**（见 [[pre-existing test flakes]]）。这些 case 排除后：

- 它们对应的代码行算**未覆盖**（保守计数 → gate 更严）
- 修掉这 3 个 flake 后可放开 `-E` 兜底，得分可能涨 1-3%

#### 5.6.3 权威 gate vs Codecov

| 项 | scripts/coverage.sh gate | Codecov status check |
|---|---|---|
| 是否 fail PR | ✅ 必须过 | ❌ 仅 informational |
| 跑测位置 | 本地 / CI 都跑 | CI 跑 |
| 离线和 token 依赖 | 否 | 是（要 `CODECOV_TOKEN`） |
| 阈值源 | `CORE_MIN` / `REPO_MIN` env（默认 80 / 40） | `codecov.yml` 同步写 |

**PR 不能合并的唯一依据 = `coverage` job 的 `gate` step 退出码**。Codecov 是 secondary dashboard。

#### 5.6.4 CODECOV_TOKEN 前置

`coverage` job 用 `codecov/codecov-action@v4`，需要：

1. 在 https://codecov.io 注册 GitHub 账号 → Add repository → 拿到 `CODECOV_TOKEN`
2. 到本仓库 Settings → Secrets and variables → Actions → **New repository secret** → Name: `CODECOV_TOKEN` / Value: 上一步的 token
3. 下次 PR 即可看到 Codecov bot comment

**token 缺失时**：`fail_ci_if_error: false` 让上传 step 跳过（PR 不因此 fail），但 **`gate` step 仍硬卡**（PR 因 gate fail 不能合）。

---

## 6. 相关文件速查

| 文件 | 用途 |
|---|---|
| `.github/workflows/ci.yml` | 主 CI（lint + build + integration-test + docker-build） |
| `.github/workflows/release.yml` | tag 触发的多架构镜像 + GitHub Release |
| `.github/dependabot.yml` | 依赖自动升级（docker / docker-compose / GA） |
| `.github/ISSUE_TEMPLATE/bug_report.yml` | Bug 报告模板 |
| `.github/ISSUE_TEMPLATE/feature_request.yml` | 功能请求模板 |
| `.clang-format` | C++ 格式基线（v1.2.58 暂不强制） |
| `.shellcheckrc` | shellcheck 配置（severity=warning + 关闭 SC2086/SC2155） |
| `scripts/init_db.sh` | migrations + admin 植入（CI 与本地复用） |
| `tests/CMakeLists.txt` | 50+ 测试 binary + add_test 注册 |
| `docs/deployment.md` | 部署文档（与 CI 测试覆盖互补） |
