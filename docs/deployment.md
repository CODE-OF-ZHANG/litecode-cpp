# LiteCode-CPP — 部署文档

> SPEC §15 / Phase 7 - 部署。本文档覆盖 `docker-compose.yml` 编排的
> 一键部署、环境变量、管理员创建、灾备恢复、升级流程与常见坑。

---

## 1. 服务拓扑

```
                ┌──────────────────┐
   80/443 ────▶ │ Caddy (proxy)    │  ── HTTPS 自动续期 / gzip+zstd
                └────────┬─────────┘
                         │
                         ▼
                ┌──────────────────┐         ┌──────────────────┐
                │ web (litecode)   │ ──────▶ │ docker-proxy     │ ─▶ Docker Host
                │ :8080, uid=1000  │         │ :2375 (白名单)    │     (判题容器)
                └────┬─────────┬───┘         └──────────────────┘
                     │         │
                     ▼         ▼
              ┌─────────┐  ┌──────────────────┐
              │ MySQL   │  │ judge-tmp volume │
              │ :3306   │  │ (named, 共享)    │
              └─────────┘  └──────────────────┘

  profile=monitoring：  + prometheus + grafana + alertmanager + cadvisor + node-exporter
  profile=backup：      + backup (每日 03:00 mysqldump + 异地 rclone)
  profile=proxy：       + caddy
```

---

## 2. 快速启动

### 2.1 第一次启动（dev）

```bash
# 1. 准备配置
cp .env.example .env
$EDITOR .env                           # 改 JWT_SECRET / 密码

# 2. 一键启动（默认 profile：web + mysql + judge 构建 + docker-proxy）
docker compose up -d --build

# 3. 观察启动日志
docker compose logs -f web

# 4. 验证健康检查
curl http://127.0.0.1:8080/api/v1/health
```

### 2.2 启用可选组件

```bash
# 反向代理（Caddy，绑定 80/443）
docker compose --profile proxy up -d

# 监控（Prometheus + Grafana + Alertmanager + cAdvisor + node-exporter）
docker compose --profile monitoring up -d

# 数据库备份（每日 03:00 自动跑）
docker compose --profile backup up -d

# 全部
docker compose --profile proxy --profile monitoring --profile backup up -d
```

### 2.3 销毁重建（清空数据）

```bash
docker compose down -v                 # -v 会删所有 named volumes（含 mysql-data / 备份）
```

---

## 3. 环境变量

`.env` 文件被 `docker-compose.yml` 和 web 容器同时读取。所有变量都有
默认值，但生产部署必须修改以下三项：

| 变量 | 说明 | 生成方式 |
|---|---|---|
| `JWT_SECRET` | 签名 access/refresh token | `openssl rand -base64 48` |
| `MYSQL_ROOT_PASSWORD` | mysql 容器 root 密码 | 任意强密码 |
| `DB_PASSWORD` | 业务库 litecode 用户密码 | 任意强密码 |
| `ADMIN_PASSWORD` | 首次启动自动创建的 admin 密码 | 任意强密码 |

完整 env 清单见 [.env.example](../.env.example)，分组如下：

| 分组 | 主要变量 |
|---|---|
| Server | `SERVER_HOST/PORT/THREAD_POOL_SIZE` |
| Database | `DB_HOST/PORT/USER/PASSWORD/NAME/POOL_*` |
| JWT | `JWT_SECRET/ISSUER/ACCESS_TTL/REFRESH_TTL` |
| Judge | `JUDGE_DEFAULT_*_LIMIT_MS/MB`、`JUDGE_COMPILE_TIMEOUT_SECONDS` 等 |
| Docker | `DOCKER_SOCKET_URL`、`JUDGE_IMAGE`、`JUDGE_PIDS_LIMIT` |
| Redis | `REDIS_ENABLED=0`（MVP 用 in-memory） |
| Logging | `LOG_LEVEL=INFO`、`LOG_FORMAT=json` |
| CORS | `CORS_ALLOWED_ORIGINS` |
| Rate limit | `RATE_LIMIT_REGISTER_PER_MIN=5` 等 |
| Admin bootstrap | `ADMIN_USERNAME/PASSWORD/EMAIL` |
| **Compose**（Phase 7）| `WEB_BIND`、`CADDY_*`、`PROMETHEUS_*`、`GRAFANA_*`、`ALERTMANAGER_*`、`CADVISOR_*`、`NODE_EXPORTER_*`、`BACKUP_*` |

> 12-factor 约定：真实环境变量优先于 `.env` 文件。所以生产环境用
> `docker compose --env-file prod.env up` 或 k8s ConfigMap 注入。

---

## 4. 管理员账户

### 4.1 首次启动自动创建

`ADMIN_USERNAME/PASSWORD/EMAIL` 在 web 容器首次启动时由 `ADMIN_USERNAME`+`ADMIN_PASSWORD`
校验：若数据库中不存在该 username，则插入一条 role=admin 的账号（bcrypt cost=12 哈希）。
所以首次启动后立即可用 `admin / admin_change_me_now`（或你 `.env` 里设的值）登录。

### 4.2 手动植入（升级场景）

```bash
# 用 mysql client 跑 create_admin.sql
docker compose exec -T mysql \
    mysql -uroot -p"$MYSQL_ROOT_PASSWORD" litecode \
    < scripts/create_admin.sql

# 或手动：
docker compose exec -T mysql mysql -uroot -p"$MYSQL_ROOT_PASSWORD" litecode -e "
INSERT INTO users (username, password_hash, email, role, created_at)
VALUES (
    'admin2',
    '\$2b\$12\$...',                      -- 用 bcrypt-hash 工具生成
    'admin2@example.com',
    'admin',
    NOW()
);"
```

### 4.3 重置密码

登录后用 admin 端点 `PUT /api/v1/admin/users/:id/role` 不直接重置密码；密码重置走
`POST /api/v1/auth/change-password`（已登录用户自己改）或在 admin 后台改。

---

## 5. 灾备恢复

### 5.1 备份（每日自动 + 手动）

`scripts/backup.sh` 在 backup 容器内以 `cron` 形式每天 03:00 跑：
- `mysqldump --single-transaction --quick` 全量备份到 `/backup/litecode_*.sql.gz`
- 保留最近 14 天
- 若 `RCLONE_REMOTE` 已设置且装了 rclone，自动异地同步

手动备份：
```bash
docker compose --profile backup up -d
docker compose exec backup /usr/local/bin/backup.sh
docker compose exec backup ls -la /backup/
```

### 5.2 恢复

```bash
# 1. 找出最近的备份
ls -t backup-data/backup/litecode_*.sql.gz | head -1

# 2. 灌回数据库
gunzip -c backup-data/backup/litecode_YYYY-MM-DD_HHMMSS.sql.gz | \
    docker compose exec -T mysql \
        mysql -uroot -p"$MYSQL_ROOT_PASSWORD" litecode

# 3. 验证表行数
docker compose exec -T mysql mysql -uroot -p"$MYSQL_ROOT_PASSWORD" litecode -e "
    SELECT TABLE_NAME, TABLE_ROWS
      FROM information_schema.TABLES
     WHERE TABLE_SCHEMA='litecode'
     ORDER BY TABLE_NAME;"
```

### 5.3 异地备份（rclone）

```bash
# 1. 在 backup 容器内配 rclone remote
docker compose exec backup rclone config
# 交互式选 s3 / b2 / webdav / drive 等；按提示填 token

# 2. 在 .env 加
echo 'RCLONE_REMOTE=myremote:bucket/litecode-backups' >> .env

# 3. 重启 backup 服务
docker compose --profile backup up -d backup
```

### 5.4 备份演练（每月 1 次）

`docs/runbooks/monthly-restore-drill.md`（TODO v1.2.58+）会自动化：
1. 启全新 mysql 容器（不同端口）
2. 灌最新备份
3. 跑 smoke tests（健康检查 + admin 登录 + 提交一条 AC）
4. 报告 P95 延迟

---

## 6. 升级流程

```bash
# 1. 拉新代码
git pull

# 2. 看 .env.example 是否有新增必填项
diff <(grep -oE '^[A-Z_]+=' .env | sort -u) \
     <(grep -oE '^[A-Z_]+=' .env.example | sort -u)

# 3. 重新构建镜像（web/judge）
docker compose build web judge

# 4. 滚动重启（mysql 不动）
docker compose up -d --no-deps web

# 5. 观察日志
docker compose logs -f web

# 6. 健康检查
curl http://127.0.0.1:8080/api/v1/health

# 7. 失败时回滚
docker compose down web
git checkout <last-good-commit>
docker compose build web
docker compose up -d --no-deps web
```

### 6.1 数据库迁移

新版本如含 `db/migrations/V0XX__*.sql`，按 `scripts/init_db.sh` 的 idempotent
逻辑自动检测并应用（看 `schema_migrations` 表）。启动时如果 web 容器
发现迁移未应用，会拒绝服务并打印日志；此时手动跑：

```bash
docker compose exec -T web /app/litecode_server --migrate-only    # TODO v1.2.58+
# 或：
MYSQL_HOST=mysql MYSQL_PASSWORD="$MYSQL_ROOT_PASSWORD" \
    ./scripts/init_db.sh root "$MYSQL_ROOT_PASSWORD" mysql 3306 litecode
```

---

## 7. 监控与告警

启 `docker compose --profile monitoring up -d` 后访问：

| 服务 | 地址 | 默认凭据 |
|---|---|---|
| Prometheus | http://127.0.0.1:9090 | 无 |
| Grafana | http://127.0.0.1:3000 | admin / `$GRAFANA_PASSWORD` |
| Alertmanager | http://127.0.0.1:9093 | 无 |
| cAdvisor | http://127.0.0.1:8088 | 无 |
| node-exporter | http://127.0.0.1:9100/metrics | 无 |

Grafana 启动后自动加载 **LiteCode Phase 9 Overview** 仪表盘（5 个 panel 分组：
系统概览 / 判题 P95 / 错误率 / 队列 / 资源）。

告警规则见 [monitoring/alerting/prometheus-alerts.yml](../monitoring/alerting/prometheus-alerts.yml)：
- **JudgeDurationP99TooHigh**（critical）：P99 > 5s 持续 2m
- **JudgeQueueBacklog**（warning）：queue > 50 持续 1m
- **JudgeSubmissionsHighFailureRate**（warning）：非 AC 占比 > 50% 持续 5m
- **JudgeWarmPoolDepleted**（warning）：warm_pool=0 持续 3m
- **Web5xxErrorRate**（critical）：5xx > 5% 持续 5m
- **WebContainerDown**（critical）：up==0 持续 1m
- **DbPoolSaturated**（warning）：pool > 90% 持续 2m
- **HostDiskSpaceLow**（warning）：根盘 < 10% 持续 5m
- **HostDiskWillFillIn24h**（critical）：24h 内写满
- **TlsCertificateExpiringSoon**（warning）：证书 14 天内过期

告警默认走 webhook（`LITECODE_WEBHOOK_URL`，未设则只 log）。生产环境建议接
Slack / 飞书 / 钉钉 / 企业微信机器人（替换 [monitoring/alerting/alertmanager.yml](../monitoring/alerting/alertmanager.yml)
的 receivers 段）。

---

## 8. 安全合规清单（SPEC §15.5）

启动后逐项验证：

```bash
# 1. Web 容器非 root
docker compose exec web id
# 期望：uid=1000(litecode) gid=1000(litecode)

# 2. Web 容器禁止提权
docker inspect litecode-web --format '{{.HostConfig.SecurityOpt}}'
# 期望：[no-new-privileges:true]

# 3. docker-proxy 仅白名单 5 子命令
docker compose exec docker-proxy sh -c 'env | grep -E "^(CONTAINERS|IMAGES|NETWORKS|VOLUMES|EVENTS|EXEC)="'
# 期望：CONTAINERS=1 IMAGES=0 NETWORKS=0 VOLUMES=0 EVENTS=0 EXEC=1

# 4. 判题容器隔离
docker run --rm litecode-judge:latest id
# 期望：uid=1000(judgeuser)

# 5. 日志轮转
docker inspect litecode-web --format '{{.HostConfig.LogConfig.Config}}'
# 期望：max-size=10m max-file=3

# 6. CSP 头
curl -I http://127.0.0.1:8080/ | grep -i content-security-policy
# 期望：default-src 'self'; ...
```

---

## 9. 常见坑

### 9.1 web 容器启动失败：`Permission denied` 写 `/app/logs`

**原因**：宿主机 `./logs` 目录属主是 root，但容器内 uid=1000 写不进去。

**解决**：
```bash
sudo chown -R 1000:1000 logs/
# 或首次启动前：
mkdir -p logs && chmod 777 logs
```

### 9.2 Caddy 启动报 `bind: address already in use`

**原因**：80/443 被宿主机其它服务（nginx/apache）占用。

**解决**：先 `sudo lsof -i :80` 查谁在用，停掉；或改 `.env` 里 `CADDY_HTTP_PORT=8088`。

### 9.3 Prometheus 抓不到 web 容器指标

**原因**：web 容器没启 `/api/v1/metrics` 端点（Phase 9 ★ v1.2.68 之前）。

**解决**：升级 web 到 v1.2.68+（`src/routes/metrics.h` MetricsService 实装 counter / histogram / gauge 全部 §16.4 metric family）；端点返回 `Content-Type: text/plain; version=0.0.4; charset=utf-8`，Prometheus 直接吃。Probed metrics：`litecode_submissions_total{status="..."}` (counter, 9 个 ac/wa/tle/.../se label) + `litecode_judge_duration_seconds_*` (histogram, 11 档 bucket 5ms→10s) + `litecode_judge_queue_size` / `litecode_judge_running_count` / `litecode_judge_warm_pool_size` / `litecode_judge_warm_pool_target` / `litecode_db_pool_active` (5 个 scrape 时 live 采样 gauge)。或 v1.2.68 之前临时把 `monitoring/prometheus.yml` 里 `litecode-web` job 注释掉。

### 9.4 backup 容器退出 `crond: can't open crontab`

**原因**：alpine 镜像的 `/etc/crontabs/root` 不存在或权限不足。

**解决**：检查 `command` 段是否执行成功；`docker compose logs backup` 看 startup 输出。

### 9.5 docker-proxy 健康检查一直 unhealthy

**原因**：tecnativa 镜像没装 `nc`，或 docker daemon socket 权限问题。

**解决**：
```bash
# 看 healthcheck 输出
docker inspect --format '{{range .State.Health.Log}}{{.Output}}{{end}}' litecode-docker-proxy
# 若 "nc: not found"：编辑 docker-compose.yml healthcheck 改用其它探测
# 若 "permission denied"：sudo usermod -aG docker <user>
```

### 9.6 判题容器报 `docker: Error response from daemon: network none not found`

**原因**：`JUDGE_NETWORK_MODE=none` 在 compose 模式下不创建网络；这是正常的，`none` 是
docker 内置关键字不是网络名。

**解决**：无需操作，确认 `JUDGE_NETWORK_MODE=none` 即可。

### 9.7 升级后 admin 登录 401

**原因**：JWT_SECRET 改了，旧 token 全部失效。

**解决**：用户重新登录；或回滚 `.env` 里的 JWT_SECRET。

---

## 10. 故障排查清单

| 症状 | 排查命令 |
|---|---|
| web 502 | `docker compose ps web` 看是否 healthy；`docker compose logs --tail=200 web` |
| mysql 连接拒绝 | `docker compose exec mysql mysqladmin ping -uroot -p"$ROOTPASS"` |
| 判题超时 | `curl http://127.0.0.1:8080/api/v1/admin/queue` 看 queue / warm_pool |
| 静态资源 404 | `docker compose exec web ls /app/web/` 看是否挂载 |
| Caddy 502 | `docker compose logs caddy` 看后端 health_uri 是否通过 |
| Prometheus scrape 失败 | `curl http://127.0.0.1:9090/api/v1/targets` 看 job 状态 |
| Grafana dashboard 空 | `datasource` 选 Prometheus（默认）；F12 console 看 query error |

---

## 11. 附：Compose 各 profile 资源总表

| 服务 | CPU 上限 | 内存上限 | 数据持久化 |
|---|---|---|---|
| mysql | — | 1024 MB | `mysql-data` |
| docker-proxy | — | 64 MB | 无 |
| judge (构建) | — | — | 无（构建后退出） |
| **web** | **2.0** | **512 MB** | `./logs` `./problems` `./web` `judge-tmp` |
| caddy (proxy) | — | — | `caddy-data` `caddy-config` |
| prometheus | 1.0 | 512 MB | `prometheus-data` |
| alertmanager | 0.25 | 128 MB | `alertmanager-data` |
| cadvisor | 0.5 | 256 MB | 无 |
| node-exporter | 0.25 | 64 MB | 无 |
| grafana | 0.5 | 256 MB | `grafana-data` |
| backup | 0.5 | 256 MB | `backup-data` |
| **合计（默认 profile）** | **2.0** | **1600 MB** | — |
| **+ monitoring** | **4.5** | **3.2 GB** | — |
| **+ backup** | **5.0** | **3.5 GB** | — |
