# 告警规则 Runbook

> **SPEC**：§16.4 / Phase 9 ★ 告警规则（v1.2.73）
> **自动化**：
>   - 配置：`monitoring/alerting/prometheus-alerts.yml`（11 条 Prometheus 规则）
>   - 通知：`monitoring/alerting/alertmanager.yml`（3 个 receiver：default / critical / infra）
>   - 自检：`scripts/lint.sh alerting`（promtool/amtool + jq + 关键环节点）
>   - 端到端：`scripts/e2e_acceptance.sh` A44（配置层 + 接收器层 + 可达性层）
> **节奏**：firing 持续 ≥ 1 小时升级为 P1 incident；critical 立即响应（oncall 5 分钟内 ack）

---

## 1. 目的

把 SPEC §16.4「告警规则（P99 延迟 / 队列积压 / 磁盘 / 证书过期）」**端到端落地**——
不是「写在 SPEC 里就算数」，而是「Prometheus 真在持续 evaluate → 真 firing →
Alertmanager 真发出通知 → oncall 真收到并能查到对应处置手册」。

只有运维人手被 IM 群真实炸过几次、跑过几次 drill，告警规则才算经过验证。

---

## 2. 落地清单（v1.2.73）

### 2.1 SPEC §16.4 字面 → 实际阈值对照

| SPEC §16.4 字面 | v1.2.73 实际 alertname | 阈值 / `for:` | 偏差说明 |
|----------------|----------------------|----------------|----------|
| 判题 P95 > 5s 持续 5 分钟 → 告警 | `JudgeDurationP99TooHigh` | P99 > 5s / 2m | **更严**：用 P99 提前预警 |
| 队列积压 > 50 持续 1 分钟 → 告警 | `JudgeQueueBacklog` | > 50 / 1m | **一致** |
| Web 容器内存 > 80% 持续 5 分钟 → 告警 | `WebContainerMemoryHigh` (v1.2.73 新增) | > 80% / 5m | **一致** |
| 失败登录单 IP > 100/小时 → 告警 | `LoginFailuresByIPHigh` (占位) | `vector(0) > 100` | **未落地**：metrics.cpp 缺 per-IP counter，v1.2.74+ 扩展 |
| 磁盘剩余 < 20% → 告警 | `HostDiskSpaceLow` | < 10% / 5m | **更严**：提前预警 |
| 证书过期前 30 天 → 告警 | `TlsCertificateExpiringSoon` | < 14 天 / 1h | **更紧**：避免 Caddy 续期失败的连锁反应 |

整体策略：现有阈值比 SPEC §16.4 建议**更严**或**更紧**，作为「提前预警」层。
SPEC 字面阈值版本见 git history；如需放宽只需改 `expr` / `for:` 字段，dashboard panel
不动。

### 2.2 完整 11 条 alertname 索引

| Alertname | Service | Severity | 含义 | 速查 |
|-----------|---------|----------|------|------|
| `JudgeDurationP99TooHigh` | judge | critical | 判题 P99 延迟 > 5s | §4.1 |
| `JudgeQueueBacklog` | judge | warning | 队列积压 > 50 | §4.2 |
| `JudgeSubmissionsHighFailureRate` | judge | warning | 失败率 > 50% | §4.3 |
| `JudgeWarmPoolDepleted` | judge | warning | 预热池空 | §4.4 |
| `Web5xxErrorRate` | web | critical | 5xx 比例 > 5% | §4.5 |
| `WebContainerDown` | web | critical | web 容器不可达 | §4.6 |
| `WebContainerMemoryHigh` | web | warning | web 内存 > 80% (v1.2.73 新增) | §4.7 |
| `DbPoolSaturated` | web | warning | DB 连接池 > 90% | §4.8 |
| `HostDiskSpaceLow` | host | warning | 磁盘剩余 < 10% | §4.9 |
| `HostDiskWillFillIn24h` | host | critical | 24h 内写满 | §4.10 |
| `TlsCertificateExpiringSoon` | proxy | warning | TLS 证书 14 天内过期 | §4.11 |
| `LoginFailuresByIPHigh` | auth | warning | 失败登录单 IP > 100/小时 (占位) | §3 已知未落地 |

---

## 3. 已知未落地 / follow-up

### 3.1 `LoginFailuresByIPHigh` 占位 alert

SPEC §16.4 写明「失败登录单 IP > 100/小时 → 告警（可能 CC 攻击）」。
当前 **未真正触发**，原因：

- `src/routes/metrics.h` `MetricsService::inc_counter()` 第二个参数硬编码为
  单 label `status` 字段（其它 label name 会被 lib 拒掉）。
- 需要扩 lib 支持多 label（label name + value 对），或新增专用
  `inc_counter(name, key, value)` API。
- 改 lib 影响范围大（v1.2.68 落地的 14 个 unit tests + 5 个 metric family 注册
  都会受影响），故 v1.2.73 只放占位。

**临时替代**（手工 SQL）：

```sql
-- 跑在过去 1 小时失败的登录，按 IP 聚合
SELECT ip, COUNT(*) AS n
FROM audit_logs
WHERE action = 'login_failure'
  AND created_at >= NOW() - INTERVAL 1 HOUR
GROUP BY ip
HAVING COUNT(*) > 100;
```

可以挂到 cron（每小时跑一次 + 命中阈值时发 IM 通知），作为旁路告警。

**v1.2.74+ 落地计划**：

1. 扩 `MetricsService::inc_counter` 接受额外 `label_name` / `label_value` 参数（或新增
   `inc_counter_multi` API）。
2. 在 `src/routes/auth_routes.cpp` 失败登录路径（`auth: login failed` 之前）调用
   `metrics.inc_counter("litecode_auth_login_failures_total", "ip", client_ip)`。
3. 把 `LoginFailuresByIPHigh` 的 `expr` 切到
   `sum by (ip) (rate(litecode_auth_login_failures_total[1h])) > 100`。
4. 删掉 alert 上的 `spec_status: planned_v1_2_74` label。

---

## 4. 单条告警处置手册

> 每条都假设你已经收到 IM 通知，按 oncall 视角写「先看哪、后做哪」。

### 4.1 `JudgeDurationP99TooHigh` (critical)

最近 5 分钟 P99 延迟 > 5s。

```bash
# 1. 看队列 + 预热池（是不是堆积）
curl -s http://localhost:8080/api/v1/metrics | grep -E 'litecode_judge_(queue|warm_pool|running)'

# 2. 抽样慢的提交
docker logs litecode-judge --tail 200 | grep -E 'finished.*duration' | sort -k 7 -n | tail -10

# 3. 看 web 进程是否 OOM 边缘
docker stats litecode-web --no-stream
```

可能 root cause：编译炸弹提交（搜 source_code 超长）/ judge 镜像版本漂移 /
web 进程内存紧张导致 worker 卡。

### 4.2 `JudgeQueueBacklog` (warning)

队列 > 50。

```bash
# 看 admin/queue（v1.2.44 路由）
curl -s -H "Authorization: Bearer $ADMIN_TOKEN" http://localhost:8080/api/v1/admin/queue
```

临时扩容 `JUDGE_MAX_CONCURRENT`（compose 重启 web）；长期加 judge 节点。

### 4.3 `JudgeSubmissionsHighFailureRate` (warning)

5m 内非 AC 占比 > 50%。

注意：正常刷题失败率 30-60% 不算异常，**持续 1 小时**再升级 P1。

```bash
curl -s http://localhost:8080/api/v1/metrics | grep litecode_submissions_total
```

### 4.4 `JudgeWarmPoolDepleted` (warning)

warm_pool_size == 0 持续 3m。

```bash
docker logs litecode-web --tail 100 | grep -E 'WarmPool|warm_pool'
docker stats litecode-web --no-stream
```

若 web 已死：直接走 4.6 处置。

### 4.5 `Web5xxErrorRate` (critical)

5xx 比例 > 5%。

```bash
docker logs litecode-web --tail 200
```

看是不是 `api_exception` 大量 throw（路由 bug），或 upstream 超时（mysql /
judge 链路）。

### 4.6 `WebContainerDown` (critical)

Prometheus 抓不到 /api/v1/metrics 持续 1m。

```bash
docker ps | grep litecode-web
docker logs litecode-web --tail 100
docker inspect litecode-web | jq '.[0].State'
```

进程死了就 `docker compose up -d web`（先看 exit code / OOM killed）。

### 4.7 `WebContainerMemoryHigh` (warning) — v1.2.73 新增

web 容器 RSS > 512M × 80% = 410M 持续 5m。

```bash
docker stats litecode-web --no-stream
docker exec litecode-web ps -o pid,rss,comm --sort -rss | head -20
```

看 RSS 是不是被 metrics cache / ConnectionPool 占满；如果是泄漏，最近
commit 是否动过 `routes/metrics.h` 或 `db/connection_pool.h`。

### 4.8 `DbPoolSaturated` (warning)

DB 连接池 active/total > 90% 持续 2m。

```bash
# 看慢查询
docker exec litecode-mysql mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" litecode \
    -e "SHOW PROCESSLIST;" | head -30
docker exec litecode-mysql mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" litecode \
    -e "SHOW ENGINE INNODB STATUS\G" | grep -A 20 "LATEST DETECTED DEADLOCK"
```

临时调高 `DB_POOL_MAX`（compose restart web）。

### 4.9 `HostDiskSpaceLow` (warning)

`/` 剩余 < 10%。

```bash
docker system df
du -sh /var/lib/docker/volumes/litecode-* | sort -h | tail -5
```

清 docker 缓存（`docker image prune` / `docker builder prune`）；归档
backup；扩盘。

### 4.10 `HostDiskWillFillIn24h` (critical)

线性外推 24h 内写满。

**P1 响应**——按 4.9 操作但必须**先停 backup 服务**（避免继续写盘撑爆），
清完后再 `docker compose --profile backup up -d backup`。

### 4.11 `TlsCertificateExpiringSoon` (warning)

TLS 证书 14 天内过期。

dev：Caddy 自动续期失败 → 检查 DNS API token（`CLOUDFLARE_API_TOKEN` 等）。
prod：先用临时证书续命（`caddy reload --force`），再排查续期失败 root cause。

---

## 5. 一键操作

### 5.1 触发 canary alert（验证链路通畅，不污染 IM）

```bash
# 启动 monitoring profile
docker compose --profile monitoring up -d alertmanager prometheus

# POST 一条 5s 后过期的 alert（不会真发通知，因为 default-webhook 没在跑）
curl -s -X POST http://localhost:9093/api/v2/alerts \
  -H 'Content-Type: application/json' \
  -d '[{
    "labels": {"alertname":"CanaryAlert","service":"e2e"},
    "annotations": {"summary":"canary"},
    "startsAt":"'"$(date -u +%Y-%m-%dT%H:%M:%S.000Z)"'",
    "endsAt":"'"$(date -u -d '+30 seconds' +%Y-%m-%dT%H:%M:%S.000Z)"'"
  }]'
# 浏览器 :9093 看到这条 alert，约 30s 自动消失
```

### 5.2 静默（silence）当前告警

```bash
# 命令行创建 silence（用 amtool；apt 装：prometheus-amtool）
amtool silence add \
    --comment="dev 环境，sandbox 中" \
    --duration=2h \
    --alertmanager=http://localhost:9093 \
    alertname="JudgeQueueBacklog" severity="warning"
```

UI：浏览器访问 :9093 → Silences → New Silence。

### 5.3 切换通知 channel

```bash
# 1. 编辑 monitoring/alerting/alertmanager.yml，把 webhook_configs 注释掉，
#    启用下面 slack_configs / email_configs / pagerduty_configs 示例块
# 2. 填上 webhook URL / token / SMTP
# 3. amtool check-config alertmanager.yml          # 本地校验
# 4. docker compose --profile monitoring up -d alertmanager   # 自动 reload
# 5. curl -X POST http://localhost:9093/-/reload    # 或走 compose restart
```

### 5.4 跑自检（CI 强约束）

```bash
# 静/动态都跑
bash scripts/lint.sh alerting

# 真发 canary 探针（需要 monitoring profile 拉起）
bash scripts/e2e_acceptance.sh
```

`scripts/lint.sh alerting` 含 5 段断言：

1. 文件存在 + 行数 sanity（防退化）
2. 6 条 SPEC §16.4 字面 alertname 全在
3. severity label 覆盖（critical + warning）
4. alertmanager.yml 顶层字段齐备（receivers/route/inhibit/resolve_timeout）
5. 可选 `promtool check rules` / `amtool check-config`（CI runner 上有）

---

## 6. 故障排查

### 6.1 Prometheus 没加载规则

```bash
# 看 prometheus 启动日志
docker logs litecode-prometheus | grep -E 'loading|rule|error'

# 强制 reload
curl -X POST http://localhost:9090/-/reload

# 列已加载规则
curl -s http://localhost:9090/api/v1/rules | jq '.data.groups[].rules[] | {name, state, health}'
```

### 6.2 Alertmanager 没收到 alert

```bash
# 1. 看 alertmanager 日志
docker logs litecode-alertmanager --tail 100

# 2. 列 alertmanager 看到的 alert
curl -s http://localhost:9093/api/v2/alerts | jq '.[] | {labels, state}'

# 3. 看 receiver 列表
curl -s http://localhost:9093/api/v2/receivers | jq
```

常见 root cause：

- `alerting:` 段没配对（`prometheus.yml` 缺 alertmanager target）
- `webhook URL` 不可达（容器间 DNS 没解析 `host.docker.internal`）
- `resolve_timeout` 太短（默认 5m，建议不动）

### 6.3 通知发不出去（webhook 5xx）

```bash
docker logs litecode-alertmanager --tail 50 | grep -E 'webhook|send|notify'
```

dev 环境：webhook URL 是 `http://127.0.0.1:5001/alerts`，没跑接收端所以 5xx
是**正常的**。生产替换成真 webhook / Slack / PagerDuty。

---

## 7. 与 metrics / dashboard 的关系

- **metrics**（v1.2.68）：规则 expr 全部引用 `litecode_*` metric family。
- **dashboard**（v1.2.69 `phase9-overview.json`）：Grafana panel 也读同一组
  metric，但 chart 是「持续可视化」，alert 是「过阈值才通知」。两者并列，
  不互替。
- **新增 metric 时**：`src/app_context_metrics.cpp` `register_*()` + 本 runbook
  索引表（§2.2）+ alert 文件（如果新增指标需要新告警）三处同步。

---

## 8. 版本演进

| 版本 | 改动 |
|------|------|
| v1.2.57 | Phase 7 落地 alertmanager + 10 条规则（compose 接入） |
| v1.2.73 | 本文档 + 11 条规则（新增 `WebContainerMemoryHigh` + 占位 `LoginFailuresByIPHigh`）+ alertmanager 3 receiver / 2 inhibit 规则 + `scripts/lint.sh alerting` + e2e A44 + 完整 runbook |
| v1.2.74+ (planned) | `LoginFailuresByIPHigh` 真正触发（扩 metrics.cpp 多 label）|