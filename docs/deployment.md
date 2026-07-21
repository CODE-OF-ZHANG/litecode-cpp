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
  profile=logging：     + loki + promtail（Docker json-file 日志聚合）
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

### 2.1.1 一键试运行：导入题 + 跑判题（v1.3.1 final hash `51be5f5` 起）

适合首次启动后想快速体验"提交代码 → 看 AC/WA/TLE"端到端的用户。
脚本会：①启动 docker-compose 11 服务栈 ②应用 V001-V011 migrations
③bulk-import 7 道种子题 ④注册测试用户 ⑤演示 6 种 `judge_type`
（exact / float_eps / ignore_trailing / ignore_case / ignore_all_whitespace /
special）⑥演示 7 种 `status`（AC / WA / TLE / MLE / OLE / CE / RE）⑦SPJ 闭环
演示（admin PUT SPJ → AC → DELETE → 兜底 WA）。

```bash
# 默认宽松模式（失败也不 exit 1，仅打印）
bash scripts/demo_submission.sh

# CI 强约束（任一 case 失败 exit 1）
DEMO_STRICT=1 bash scripts/demo_submission.sh

# 仅探测环境 + 打印计划，不真启动
DEMO_DRY_RUN=1 bash scripts/demo_submission.sh

# 自定义服务地址
BASE_URL=http://host:8080 bash scripts/demo_submission.sh
```

脚本跑完后输出测试账号 + 浏览器入口提示，可直接打开
`http://localhost:8080/problem.html?slug=two-sum` 手动验证。
完整设计文档见 `scripts/demo_submission.sh` 头部注释。

# 日志聚合（Loki + Promtail；默认 Docker logs 仍可用）
docker compose --profile logging up -d

# 监控 + 日志（Grafana Explore 查询 Loki）
docker compose --profile monitoring --profile logging up -d

# 数据库备份（每日 03:00 自动跑）
docker compose --profile backup up -d

# 全部
docker compose --profile proxy --profile monitoring --profile logging --profile backup up -d
```

### 2.3 销毁重建（清空数据）

```bash
docker compose down -v                 # -v 会删所有 named volumes（含 mysql-data / 备份）
```

### 2.4 Caddy 反向代理双模式（v1.2.76 起）

```bash
# 模式 A：本地开发（默认）—— HTTP :80，挂 caddy/Caddyfile.local
docker compose --profile proxy up -d

# 模式 B：生产 —— HTTPS + on_demand TLS，挂 caddy/Caddyfile.prod
# LITECODE_DOMAIN 必须设为公网可达域名（Let's Encrypt HTTP-01 验证）
LITECODE_DOMAIN=oj.example.com \
CADDY_MODE=prod \
docker compose --profile proxy up -d
```

切换机制：`docker-compose.yml` caddy service 的 entrypoint 按 `CADDY_MODE`
env 拷贝对应 Caddyfile 到 `/etc/caddy/Caddyfile`，再 `caddy validate` 校
验语法后 `caddy run`。详见 [`docs/runbooks/caddy.md`](runbooks/caddy.md)。

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
| **Compose**（Phase 7）| `WEB_BIND`、`CADDY_MODE`、`CADDY_HTTP/HTTPS_PORT`、`LITECODE_DOMAIN`、`PROMETHEUS_*`、`GRAFANA_*`、`ALERTMANAGER_*`、`CADVISOR_*`、`NODE_EXPORTER_*`、`LOKI_*`、`BACKUP_*` |

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

`scripts/restore_drill.sh` 端到端恢复演练（v1.2.7X），自动完成 SPEC §16.5 描述的演练剧本，
详细 runbook 见 [`docs/runbooks/monthly-restore-drill.md`](runbooks/monthly-restore-drill.md)。
1. 找到 `${BACKUP_DIR}/litecode_*.sql.gz` 最新一份
2. 拉起隔离 mysql-drill（端口 3307）→ 灌 backup → 校验表行数 + admin 行存在
3. 拉起 drill-proxy + drill-web（端口 8081，连 drill-mysql）
4. smoke：
   - `GET /api/v1/health` 200
   - `POST /auth/login` 200（admin/admin123!）
   - bulk-import `two-sum` 201
   - 注册测试用户 + 提交 AC → 轮询 `ac`
   - `GET /api/v1/metrics` 200（v1.2.68 暴露）
5. 拆 drill 栈（容器 + 临时卷，不污染主栈）
6. 输出 `DRILL_RESULT PASS=N FAIL=N SKIP=N` 末行

调用方式：

```bash
# 手动演练（缺前置时 SKIP 不报错）
bash scripts/restore_drill.sh

# CI / 月度告警（缺前置升级为 FAIL，便于监控）
RESTORE_STRICT=1 bash scripts/restore_drill.sh
```

默认 schedule：`0 9 1-7 * 1`（每月第一个周一上午 09:00，与 CI 周一周期对齐）。

### 5.5 反向代理（Caddy 双模式，v1.2.76）

Caddy 配置已拆分为两个独立文件，docker-compose entrypoint 按 `CADDY_MODE`
env 选择挂载：

| 模式 | 触发 | 配置文件 | 端口 | TLS |
|------|------|---------|------|-----|
| **local**（默认）| `CADDY_MODE=local` 或不设 | `caddy/Caddyfile.local` | :80 | 关 |
| **prod** | `CADDY_MODE=prod` + `LITECODE_DOMAIN` | `caddy/Caddyfile.prod` | :80 (:80→:443 重定向) + :443 | on_demand（自动 LE 申请） |

启用方式：

```bash
# 本地开发：HTTP :80，直接反代 web:8080
docker compose --profile proxy up -d

# 生产：HTTPS + on_demand TLS
LITECODE_DOMAIN=oj.example.com \
CADDY_MODE=prod \
docker compose --profile proxy up -d
```

切换细节 / 故障排查 / on_demand TLS 限制 / DNS-01 自定义镜像 详见
[`docs/runbooks/caddy.md`](runbooks/caddy.md)。

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
| Loki（`logging` profile） | http://127.0.0.1:3100 | 无 |
| Promtail（`logging` profile） | 仅 Docker network :9080 | 无 |

Grafana 启动后自动加载 **LiteCode Phase 9 Overview** 仪表盘（v1.2.69，5 个
panel 分组 14 个 chart：系统概览 / 判题 P95·P99 / 错误率 / 队列·预热池 /
容器·宿主资源），全部 panel 引用 v1.2.68 MetricsService 真实暴露的 7
个 metric family + cAdvisor / node-exporter / Prometheus up：

| 分组 | panel | 数据源 |
|---|---|---|
| 1. 系统概览 | Web 容器在线 / 判题队列 / Worker / 预热池 idle / 预热池目标 K / DB active / 总提交数 / P95 | `up{job="litecode-web"}` + 6 × `litecode_*` gauge + `histogram_quantile(0.95, …)` |
| 2. 判题延迟 | P50 / P95 / P99 时序 + P99 大字 | `litecode_judge_duration_seconds_bucket` (histogram) |
| 3. 错误率 | AC 占比 / 系统错误 (se) / 提交终态分布 | `litecode_submissions_total{status="..."}` |
| 4. 队列 / 预热池 | queue / warm_pool / warm_target / running 时序 | 4 × `litecode_*` gauge |
| 5. 资源 | Web 容器 CPU / 内存 + 宿主磁盘 / CPU | cAdvisor `container_*` + node-exporter `node_*` |

**v1.2.69 Prometheus 数据源契约**：datasources.yml 把 Prometheus DS
的 UID 固定为 `prometheus`，跟 panel `datasource.uid: prometheus` 显式
绑定。Grafana 默认会随机生成 UID，若两者不钉死则整个面板会显示
"No data"（UID mismatch）。

**新增 / 修改 dashboard 后必须跑**：

```bash
scripts/validate_grafana_dashboards.sh
```

会做三件事：(1) `jq empty` 校验所有 `monitoring/grafana/dashboards/*.json`
的 JSON 格式；(2) 交叉对照 v1.2.68 MetricsService 暴露的 7 个
`litecode_*` metric family + histogram 派生 `_bucket/_sum/_count` 后缀，
**阻断任何「幽灵指标」引用漂回仓里**（v1.2.57 placeholder dashboard
里 `litecode_http_requests_total` / `litecode_db_pool_size` /
`litecode_active_sessions` / `litecode_process_start_time_seconds` /
`litecode_judge_active` 这 5 个从未实现的指标就是栽在没这道 lint 上）；
(3) 校验 datasources.yml 的 Prometheus DS 必须有 `uid:` 字段。

### 7.1 日志聚合（Loki + Promtail）

应用日志默认已经满足 SPEC §16.6：`LOG_FORMAT=json` 将一行一个 JSON
对象写到 stdout，Docker `json-file` driver 负责 `docker compose logs` 接管，
并以 `max-size=10m` / `max-file=3` 做宿主机轮转。这个基础路径不依赖 Loki，
因此 Loki 暂时不可用不会阻断 web 启动。

需要集中搜索时启用可选 profile：

```bash
docker compose --profile logging up -d
# 与 Grafana 一起使用：
docker compose --profile monitoring --profile logging up -d
curl http://127.0.0.1:3100/ready
```

Promtail 通过只读 Docker socket 和只读 `/var/lib/docker/containers` 发现并读取
`litecode-*` 容器的 json-file 日志，再发送到 Loki。Linux Docker Engine 上这两个
挂载点必须可读；Docker Desktop/Windows 若 Docker VM 不提供宿主机日志目录，
请继续使用 `docker compose logs`，或把 Promtail 运行在能读取 Docker daemon
日志目录的 Linux 节点上。socket 与日志目录均为 `:ro`，Promtail 不暴露宿主端口，
Loki 默认只绑定 loopback。

Grafana provisioning 会创建固定 UID 为 `loki` 的数据源。打开 Grafana Explore
后可用以下 LogQL 查询：

```logql
{container="litecode-web"}
{container="litecode-web", level="ERROR"} | json
{compose_service="web"} | json | request_id != ""
```

`request_id`、`msg` 和应用字段作为日志内容解析，不作为 Loki label，以避免高
基数标签导致索引膨胀。配置变更后可运行：

```bash
scripts/validate_log_aggregation.sh
```

**新增 metric 时**两处同步：先在 `src/app_context_metrics.cpp` 加
`register_*()` 调用，再在 `scripts/validate_grafana_dashboards.sh` 的
`ALLOWLIST_LITECODE` 块补上对应名字——少任何一处，dashboard 引用会变
"No data" 或 lint 直接 fail。

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

# 5. 日志轮转（json-file + 10MB/3 文件）
scripts/lint.sh logrotate

# 对已启动的全部 profile 容器做 runtime probe
for cid in $(docker compose \
    --profile proxy --profile monitoring --profile backup --profile logging \
    ps -q); do
    docker inspect "$cid" \
        --format '{{.Name}} driver={{.HostConfig.LogConfig.Type}} options={{.HostConfig.LogConfig.Config}}'
done
# 期望：每个容器均为 driver=json-file、max-size=10m、max-file=3

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

### 9.8 Grafana 面板整面板显示 "No data"

**原因**：v1.2.69 之前 dashboard panel 引用了从未实现的幽灵指标
（`litecode_http_requests_total` / `db_pool_size` / `active_sessions` /
`process_start_time_seconds` / `judge_active`），或者 datasources.yml
的 Prometheus DS 没钉 `uid:` 字段，跟 panel `datasource.uid: prometheus`
失配。

**解决**：

```bash
# 1. 跑 linter 看具体哪个 panel 引用了幽灵指标
scripts/validate_grafana_dashboards.sh

# 2. 修 dashboard JSON / datasources.yml，按 linter 报错改
#    - 删掉幽灵 metric 引用
#    - 或在 src/app_context_metrics.cpp register_*() 补上对应真指标
#      （同时改 scripts/validate_grafana_dashboards.sh 的
#       ALLOWLIST_LITECODE，两处必须同步）
#    - datasources.yml 的 Prometheus DS 块必须有 `uid: prometheus`

# 3. Grafana 自动 30s 热重载（dashboards.yml updateIntervalSeconds），
#    等一会刷新面板即可
```

### 9.9 Loki 没有日志

**原因**：`logging` profile 未启动、Promtail 无法读取 Docker json-file 目录，
或 Loki 与 Promtail 不在同一个 `litecode-net` 网络。

**解决**：

```bash
# 查看两个服务状态和 Promtail 采集错误
docker compose --profile logging ps
docker compose --profile logging logs --tail=100 promtail

# 确认 Loki readiness
docker compose --profile logging exec loki wget -qO- http://127.0.0.1:3100/ready

# 验证配置契约（不启动容器）
scripts/validate_log_aggregation.sh
```

Docker Desktop/Windows 不能将 `/var/lib/docker/containers` 映射给 Promtail
时，Loki profile 会保持不可用；这不影响 `docker compose logs web`，也不影响
stdout JSON 或 Docker `json-file` 轮转。把聚合栈部署到 Linux Docker Engine，
或仅使用 Docker logs，是安全的降级路径。

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
| Loki 无日志 | `docker compose --profile logging logs --tail=100 promtail`；再跑 `scripts/validate_log_aggregation.sh` |
| 日志轮转策略异常 | `scripts/lint.sh logrotate`；再用上方 `docker inspect` 检查运行时 LogConfig |

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
| loki (logging) | 0.5 | 256 MB | `loki-data` |
| promtail (logging) | 0.25 | 128 MB | `promtail-positions` |
| backup | 0.5 | 256 MB | `backup-data` |
| **合计（默认 profile）** | **2.0** | **1600 MB** | — |
| **+ monitoring** | **4.5** | **3.2 GB** | — |
| **+ logging** | **5.25** | **3.6 GB** | — |
| **+ monitoring + logging** | **5.25** | **3.6 GB** | — |
| **+ backup** | **5.0** | **3.5 GB** | — |
