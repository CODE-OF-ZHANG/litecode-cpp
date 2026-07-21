# 月度备份恢复演练 Runbook

> **SPEC**：§16.5 / Phase 9 ☆ 备份验证
> **自动化**：`scripts/restore_drill.sh`（v1.2.7X 起；CI/Cron 入口见下方）
> **节奏**：每月第 1 个周一上午 09:00（与 GitHub Actions cron `0 9 * * 1` 对齐）

---

## 1. 目的

在 **不影响主栈** 的前提下，端到端验证「**最近一份数据库 backup 能够灌回 MySQL，
且灌后的栈能正常服务 health / admin 鉴权 / 判题 AC**」。

只有定期演练，灾备恢复流程才是经过验证、可信的；而不只是剧本里写了一段。

---

## 2. 前置条件（演练前自检）

| 项 | 默认值 | 来源 | 备注 |
|----|--------|------|------|
| Docker daemon | `docker` 命令可用 | host | 必须能被脚本调起容器 |
| 镜像 `mysql:8.0.40` | 已 pull / 已构建 | `docker image inspect` | 与生产同版本 |
| 镜像 `tecnativa/docker-socket-proxy:0.1` | 已 pull | `docker image inspect` | drill 隔离栈专用 |
| 镜像 `litecode-web:latest` | 最近构建（dev/prod） | `docker image inspect` | 由 `docker compose build web` 产出 |
| 镜像 `litecode-judge:latest` | 最近构建 | `docker image inspect` | 判题 smoke 依赖 |
| 网络 `litecode-net` | 已存在 | `docker compose up -d` 拉起 default 栈创建 | drill 复用，不重建 |
| **最近 backup** | `${BACKUP_DIR}/litecode_litecode_*.sql.gz` ≥ 1 个 | `backup` 容器 cron 03:00 | **核心**：必须有一份能被读到的真实 backup |
| `MYSQL_ROOT_PASSWORD` | 与生产一致 | `.env` / env 覆盖 | drill 灌库 + 创建 admin 都用它 |

如果上面任何一项缺失：

- **宽松模式**（默认）：`bash scripts/restore_drill.sh` 会记 SKIP，不报错。
- **严格模式**（`RESTORE_STRICT=1`）：升级为 FAIL 并 exit 1，适合 CI/Cron 月度报警检查。

---

## 3. 一键执行

```bash
# 推荐路径：手动演练
cd /path/to/litecode-cpp
bash scripts/restore_drill.sh

# 强约束：CI / 月度告警
RESTORE_STRICT=1 bash scripts/restore_drill.sh
```

脚本会顺次打印 8 个阶段，**末行** 一定输出形如下面的汇总行（与 v1.2.67 `FUZZ_RESULT` 风格一致）：

```
DRILL_RESULT PASS=N FAIL=N SKIP=N
```

### 预期：无栈环境下

```
=== LiteCode-CPP Restore Drill ===
ROOT=/path/litecode-cpp  BACKUP_DIR=/path/litecode-cpp/backup-data/backup  STRICT=0

capabilities:  docker=[NO]  mysql_img=[NO]  proxy_img=[NO]  web_img=[NO]  backup_present=[NO]  compose=[NO]

    skip - 恢复演练前置缺失: docker mysql_img(...) ...
DRILL_RESULT PASS=0 FAIL=0 SKIP=1
```

**含义**：前置缺失时整脚本 SKIP 不启动容器，不会污染主机。

### 预期：完整栈 + 真实 backup

```
=== LiteCode-CPP Restore Drill ===
...
── [1/8] 挑选最新 backup
    ok   - 选中最新备份: litecode_litecode_2026-07-21_030001.sql.gz (4194304 bytes)
    ok   - gzip 完整性校验通过
── [2/8] 启动隔离 mysql-drill 容器
    ok   - mysql-drill 容器已起 (bind=127.0.0.1:3307)
    ok   - mysql-drill healthy (用时 12s)
── [3/8] 灌 backup
    ok   - drill DB litecode 已就绪
    ok   - backup 已灌入 litecode
── [4/8] 灌后校验
    ok   - restore 后表清单: audit_logs=3 problems=42 submissions=128 ...
    ok   - admin 行已存在 (count=1)，可走 smoke 登录
── [5/8] 启动 drill-proxy + drill-web
    ok   - drill-proxy 已起 (CONTAINERS=1 POST=1 EXEC=1；IMAGES/NETWORKS/VOLUMES/EVENTS=0)
    ok   - drill-web 已起 (bind=127.0.0.1:8081, DB_HOST=litecode-drill-mysql)
── [6/8] smoke：等 drill-web /api/v1/health 200
    ok   - /api/v1/health → 200 (用时 5s)
    ok   - /api/v1/health 探针 .docker=ok
...
── [6/8] smoke：admin 登录
    ok   - admin /auth/login → 200 (token 前 12=eyJhbGciOi…)
── [6/8] smoke:bulk-import two-sum
    ok   - bulk-import two-sum → 201（on_duplicate=skip 幂等）
── [6/8] smoke:提交 two-sum AC 并轮询
    ok   - POST /submissions → 201 (id=42 first=pending)
    ok   - two-sum → ac（3s）；restore 后判题链路通畅
── [6/8] smoke:/api/v1/metrics（v1.2.68 Prometheus 接入）
    ok   - /api/v1/metrics → 200, 暴露 18 行 litecode_* 指标
── [7/8] 拆 drill 栈
    ok   - drill 容器 + 临时卷已拆

DRILL_RESULT PASS=14 FAIL=0 SKIP=0
```

---

## 4. drill 栈细节（出了 FAIL 时排查用）

```
┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
│ litecode-drill-mysql│   │litecode-drill-proxy │   │  litecode-drill-web │
│  (3307 → 3306)      │◀──│  (CONTAINERS=1      │◀──│  (8081 → 8080)      │
│  卷：drill-mysql-data│   │   POST=1 EXEC=1     │   │  DB_HOST=mysql-drill│
└─────────────────────┘   │   IMAGES=0 ...)     │   │  DOCKER_SOCKET=     │
                          └─────────────────────┘   │   tcp://drill-proxy │
                                                    └─────────────────────┘
```

- **完全隔离**：drill 容器名 + 卷名带 `drill` 前缀，与 `litecode-mysql` / `litecode-web` 等同干。
- **不复用主栈容器**：避免端口冲突 / 数据污染（即使崩了也只影响 drill）。
- **共享网络 `litecode-net`**：让 drill 之间 + drill 与已构建的 judge image 直接互联。

### 端口选择

| 服务 | 默认 | override 环境变量 |
|------|------|---------------------|
| `litecode-drill-mysql` | `127.0.0.1:3307 → 3306` | `MYSQL_DRILL_PORT_BIND` |
| `litecode-drill-web`   | `127.0.0.1:8081 → 8080` | `WEB_DRILL_PORT_BIND`     |

如果 host 端口被占，可在调用脚本前 override：

```bash
MYSQL_DRILL_PORT_BIND=127.0.0.1:33077 \
WEB_DRILL_PORT_BIND=127.0.0.1:8088  \
bash scripts/restore_drill.sh
```

---

## 5. 故障排查（Drill FAIL 时）

### 5.1 `mysql-drill 60s 未 healthy`

```bash
docker logs litecode-drill-mysql | tail -50
```

检查：root 密码是否与生产一致、磁盘是否够（首次启动 `mysql-data` 卷初始化大约 100MB）。

### 5.2 `灌 backup 失败`

```bash
gunzip -t backup-data/backup/litecode_litecode_*.sql.gz
```

校验 gzip 完整性。`scripts/backup.sh` 结尾自带 `gunzip -t`，但演练中再校一次是给 cron 上"互验"。

### 5.3 `bulk-import two-sum → 403`

意味着 admin 鉴权失败 → backup 里 admin 行的 `password_hash` 与生产期望的 `admin123!`
hash 不一致。**这是「V099__create_admin.sql 没进 mysqldump 输出」的典型症状**。

验证：

```bash
docker exec litecode-drill-mysql mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" litecode \
    -e "SELECT username, role, LEFT(password_hash,7) FROM users WHERE role='admin';"
```

期望：`admin | admin | $2b$12$...`。若 role 不是 admin，或 password_hash 是别的，说明 backup
工具链没把 V099 创的初始 admin 一并 dump（理论上 `--all-databases` 或 `--routines` 应当包进去）
—— 升级 prompt 到 `db/migrations/V099__create_admin.sql` 的 `INSERT` 改用 `SELECT … FROM dual`
确保 idempotent。

### 5.4 `two-sum 终态=se / ole ... ` 未到 ac

drill-web 的 judge 链路异常：

```bash
docker logs litecode-drill-web | grep -E "judge|JudgeScheduler" | tail -30
docker logs litecode-drill-proxy | tail -10
```

通常：drill-proxy 没正确转发 `EXEC` 子命令 → drill-web 的 DOCKER_SOCKET_URL 找不到。

### 5.5 `litecode-drill-web 起不来`（"端口被占"等）

```bash
netstat -ano | grep -E ":3307|:8081"
```

调 `MYSQL_DRILL_PORT_BIND` / `WEB_DRILL_PORT_BIND` 避开冲突。

---

## 6. 升级与监控

### 6.1 Cron 接入

```bash
# /etc/cron.d/litecode-restore-drill
0 9 1-7 * 1   root   /opt/litecode-cpp/scripts/restore_drill.sh \
                              >> /var/log/litecode-drill.log 2>&1
```

`1-7 * 1` = 每月前 7 天内的第一个周一（与项目 CI 周一上午周期对齐）。

### 6.2 解析 `DRILL_RESULT` 末行做告警

```bash
bash scripts/restore_drill.sh 2>&1 | tee drill.log
grep -E '^DRILL_RESULT ' drill.log | \
    awk -v strict="${RESTORE_STRICT:-0}" '
        $3 > 0 || ($4 > 0 && strict == "1") {
            print "[ALERT] restore drill failed:", $0; exit 1
        }'
```

- `FAIL > 0`：drill 中真的有断言失败——立刻处理。
- `SKIP > 0 && STRICT`：CI 强约束下"缺前置"也算异常（可能 backup 服务长期未跑）。
- `SKIP > 0 && loose`：仅作周报提示，留意 backup 服务是否停摆。

### 6.3 与 metrics 联动（v1.2.68+）

drill 自身不暴露指标（隔离栈）。如需把 drill 结果时序化，可在 cron 脚本里
`grep litecode_drill_pass_total|litecode_drill_fail_total` Pushgateway push，
再由 Prometheus 触发对应告警。
