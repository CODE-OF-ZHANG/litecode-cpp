# Caddy 反向代理 Runbook（v1.2.76 双模式）

> **SPEC**：§11 Phase 1 / Phase 7 / v1.2.76 ★ Caddyfile 双模式
> **自动化**：
>   - 脚本：`scripts/lint.sh caddy`（v1.2.76 关键环节点 + 行数 sanity）
>   - 端到端：`scripts/e2e_acceptance.sh` A47（静态配置 + runbook 引用 + `caddy validate` 反向汇入）
> **运行模式**：
>   - 默认 `CADDY_MODE=local` —— HTTP :80，挂 `caddy/Caddyfile.local`
>   - `CADDY_MODE=prod`  —— HTTPS + on_demand TLS，挂 `caddy/Caddyfile.prod`，必须设 `LITECODE_DOMAIN`

---

## 1. 目的

把 SPEC「Caddyfile 双模式」**端到端落地**——不是「在 SPEC 里写一条就算数」，
而是「真能 HTTP :80 跑起来 + 真能 HTTPS on_demand TLS 接 ACME + 真能
CI 自动校验 + 真能反向汇入 e2e 主计数器」。

---

## 2. 落地清单（v1.2.76）

| 组件 | 文件 | 作用 |
|------|------|------|
| 本地模式 | `caddy/Caddyfile.local` | HTTP :80,转发 web:8080,无 TLS,local dev 默认 |
| 生产模式 | `caddy/Caddyfile.prod`  | HTTPS + on_demand TLS,HTTP→HTTPS 301,HSTS |
| 切换入口 | `docker-compose.yml` caddy service entrypoint | 按 `CADDY_MODE` env 选 Caddyfile + `caddy validate` 后 `caddy run` |
| Deprecated 兼容 | 根目录 `Caddyfile` | 老引用保留为 deprecated 入口，等价 local 模式（v1.2.76 之前的历史） |
| 自检 | `scripts/lint.sh caddy` | 关键环节点 + 模式互斥 + 行数 sanity |
| e2e | `scripts/e2e_acceptance.sh` A47 | 静态配置 + runbook 引用 + `caddy validate` 反向汇入 |
| env 文档 | `.env.example` `CADDY_MODE` / `LITECODE_DOMAIN` 段 | 文档化双模式开关 |
| 部署文档 | `docs/deployment.md §2.4 + §5.5` | 快速启动 + 模式对照表 |
| 本 runbook | `docs/runbooks/caddy.md` | 本文 |

---

## 3. 双模式对照表

| 项 | `local`（默认） | `prod` |
|----|----------------|--------|
| **触发** | `CADDY_MODE=local` 或不设 | `CADDY_MODE=prod` + `LITECODE_DOMAIN=oj.example.com` |
| **挂载文件** | `caddy/Caddyfile.local` | `caddy/Caddyfile.prod` |
| **监听端口** | :80（HTTP only） | :80（重定向到 :443）+ :443（HTTPS） |
| **TLS** | 关（`auto_https off`） | on_demand（Caddy 按 SNI 自动向 LE 申请证书） |
| **HSTS** | 不发（避免本地 HTTP 下浏览器拒绝） | `max-age=31536000; includeSubDomains` |
| **CSP** | `default-src 'self'; ... object-src 'none'; base-uri 'self'; ...` | 与 local 同（避免策略漂移） |
| **健康检查** | wget :80 探活 | wget :80 / :443 任一 200 即健康 |
| **资源上限** | 0.25 cpu / 128 MB | 同 |
| **ACME challenge** | 不需要（HTTP only） | 需要 :80 公网可达（HTTP-01 验证） |

---

## 4. 启用步骤

### 4.1 本地开发（默认）

```bash
# 1. 准备 .env（已有则跳过）
cp .env.example .env

# 2. 启 web + mysql + judge 构建 + docker-proxy + caddy
docker compose --profile proxy up -d

# 3. 验证
curl -s http://127.0.0.1/api/v1/health   # → {"status":"ok","docker":"ok",...}

# 4. 看 caddy 日志
docker compose logs -f caddy
# 预期：[caddy] CADDY_MODE=local → /etc/caddy/conf/Caddyfile.local
```

### 4.2 生产部署

```bash
# 1. 在 .env 加：
#   CADDY_MODE=prod
#   LITECODE_DOMAIN=oj.example.com
#   CADDY_HTTP_PORT=80
#   CADDY_HTTPS_PORT=443
echo 'CADDY_MODE=prod' >> .env
echo 'LITECODE_DOMAIN=oj.example.com' >> .env

# 2. 确保 :80 端口在公网可达（LE 的 HTTP-01 challenge 会打过来）
#    云厂商安全组 / 防火墙需要放行 80 + 443 入站
#    DNS A/AAAA 记录把 oj.example.com 指到本机公网 IP

# 3. 启栈
docker compose --profile proxy up -d

# 4. 看 caddy 日志（首访会触发 ACME 申请）
docker compose logs -f caddy
# 预期：
#   [caddy] CADDY_MODE=prod → /etc/caddy/conf/Caddyfile.prod
#   ... tls.on_demand ... obtaining certificate ...

# 5. 验证 HTTPS
curl -sI https://oj.example.com/api/v1/health   # → 200 + Strict-Transport-Security 头
```

---

## 5. on_demand TLS 限制

`on_demand` 模式（v1.2.76 默认 prod 配置）有以下必须知道的边界：

### 5.1 :80 端口必须公网可达

Let's Encrypt 用 HTTP-01 challenge 验证域名所有权——会从公网对你的 :80
发起 `GET http://oj.example.com/.well-known/acme-challenge/<token>`。
如果 :80 被防火墙挡 / 没做端口转发 / 在 NAT 后面 → 证书永远签不下来。

**解决**：
- 云厂商安全组放行 :80 + :443 入站
- 路由器 / NAT 把 80/443 DNAT 到部署机
- 或改用 DNS-01 challenge（见 §7）

### 5.2 首次访问才触发申请

`on_demand` 字面就是「按需」——首次任意客户端访问 `https://oj.example.com`
时，Caddy 才后台向 LE 申请证书。申请期间（一般 5-30 秒）浏览器会卡住或
显示 `net::ERR_CERT_AUTHORITY_INVALID`。等几十秒重试即好。

### 5.3 滥用风险（DoS → 大量子域证书签发）

`on_demand_tls.ask []` 是「不限制 SNI」——攻击者扫你 IP 的 443 端口，
对每个随机 `<x>.oj.example.com` 子域发请求，Caddy 都会真的去向 LE 申请
证书。LE 对单个账户有 rate limit（5 个 duplicate cert per week / 50
cert per week per domain），被攻击后会触发「证书签发配额用尽」。

**生产建议**（v1.2.76+ follow-up）：
- 部署 introspection endpoint（POST /tls/introspect 鉴权后返 200）
- 在 `on_demand_tls` block 填：
  ```
  on_demand_tls {
      ask https://your-introspect.example.com
  }
  ```
- Caddy 申请证书前先回调问「这个 SNI 该不该签」，把决定权交回业务层

### 5.4 证书续期

LE 证书 90 天有效。Caddy 后台 60 天起自动续期（不需要重启）。续期失败
会记 ERROR 日志，触发 `TlsCertificateExpiringSoon` 告警（v1.2.73 alerting
已配）。

---

## 6. 故障排查

### 6.1 Caddy 容器反复重启：`bind: address already in use`

宿主机 :80 / :443 已被其他进程占用（常见：Apache / Nginx / 另一个 Caddy 实例）。

```bash
# 看谁占了 80/443
sudo ss -tlnp | grep -E ':80|:443'

# 停掉冲突服务
sudo systemctl stop nginx
sudo systemctl disable nginx

# 或改 Caddy 端口（.env）：
#   CADDY_HTTP_PORT=8080
#   CADDY_HTTPS_PORT=8443
# 注意：自定义端口后 LE HTTP-01 验证不再适用（LE 不接非 80/443），
# 必须改用 DNS-01（见 §7）或换回 80/443。
```

### 6.2 caddy validate 失败

```bash
# 看完整报错
docker compose logs caddy | grep -A 20 'caddy validate'

# 常见原因：
#   - 占位符 {$LITECODE_DOMAIN:example.com} 没设 env → sed 替换失败
#     docker compose.yml caddy service entrypoint 那段 sed 是不是被改坏
#   - Caddyfile 语法错（括号不匹配 / 引号不对）
#     本地单独跑：docker run --rm -v $(pwd)/caddy/Caddyfile.local:/etc/caddy/Caddyfile:ro caddy:2.8-alpine caddy validate --config /etc/caddy/Caddyfile
```

### 6.3 prod 模式 `obtaining certificate` 永远卡住

Caddy 在向 LE 申请证书，但收不到 HTTP-01 challenge 响应。

```bash
# 从外部模拟 LE 的请求
curl -v http://oj.example.com/.well-known/acme-challenge/test

# 看是不是：
#   - :80 公网不可达（防火墙 / 安全组）
#   - DNS A 记录没指对 IP（dig oj.example.com +short）
#   - 已有别的 web server 占着 :80 返了非 404
```

### 6.4 浏览器报 `NET::ERR_CERT_AUTHORITY_INVALID`

Caddy 还没拿到证书 / 证书过期 / 证书 chain 不完整。

```bash
# 看证书状态
echo | openssl s_client -connect oj.example.com:443 -servername oj.example.com 2>/dev/null \
    | openssl x509 -noout -dates -issuer

# 看 caddy 是不是还在 retry 申请
docker compose logs caddy | grep -E 'certificate|acme'
```

### 6.5 HSTS 重定向循环（生产启用后浏览器拒绝访问）

Caddy 启了 HSTS (`max-age=31536000`)，浏览器会强制 HTTPS，但你 :443 还
没起来 / 证书没签好 → 死循环。

**紧急恢复**：在浏览器输入 `chrome://net-internals/#hsts` 把
`oj.example.com` 从 HSTS list 删掉，再排查 :443 / 证书问题。

### 6.6 本地 HTTP 下浏览器报 `HSTS` 错误

local 模式不应该有 HSTS。如果误开了：
- 检查 `caddy/Caddyfile.local` 里 `Strict-Transport-Security` 那行是否被注释
- 浏览器 chrome://net-internals/#hsts 清掉旧记录

### 6.7 镜像 `caddy:2.8-alpine` 拉不到

CI runner 或国内开发机可能拉不动 Docker Hub。

```bash
# 改镜像源（.env 或 docker-compose.yml）：
#   image: caddy:2.8-alpine
# 改为：
#   image: registry.cn-hangzhou.aliyuncs.com/.../caddy:2.8-alpine
# 或本地构建 Dockerfile FROM caddy:2.8-alpine 然后 docker save / load。
```

---

## 7. DNS-01 验证（follow-up）

v1.2.76 默认 on_demand TLS 用 HTTP-01，需要 :80 公网可达。某些部署环境
（NAT 后面 / 云函数 / 国内未备案）拿不到 :80 公网，需要 DNS-01：

```bash
# 1. 用 xcaddy 构建自定义 Caddy 镜像集成 DNS 插件
# 例：cloudflare 插件
docker build -t litecode-caddy:custom - <<'EOF'
FROM caddy:2.8-builder AS builder
RUN xcaddy build \
    --with github.com/caddy-dns/cloudflare

FROM caddy:2.8
COPY --from=builder /usr/bin/caddy /usr/bin/caddy
EOF

# 2. docker-compose.yml caddy service 改 image
#    image: litecode-caddy:custom

# 3. .env 加 CF_API_TOKEN（cloudflare dashboard 创建）
#    CF_API_TOKEN=<your-token>

# 4. caddy/Caddyfile.prod 的 tls block 改成：
#    tls {
#        on_demand
#        dns cloudflare {env.CF_API_TOKEN}
#    }
```

属于 v1.3+ 规划；当前 HTTP-01 已覆盖大多数场景。

---

## 8. 与其他组件的关系

| 工件 | 回答的问题 | 文件 |
|------|-----------|------|
| **`caddy/Caddyfile.{local,prod}`（本文 v1.2.76）** | 「Caddy 怎么反代 web 容器？HTTP 还是 HTTPS？」 | 本文 + 自身 |
| [`docs/deployment.md §2.4 + §5.5`](../../deployment.md) | 「5 分钟怎么起 / 灾备恢复在哪」 | `docs/deployment.md` |
| [`monitoring/alerting/prometheus-alerts.yml`](../../../monitoring/alerting/) | 「证书快过期了谁告警？」（`TlsCertificateExpiringSoon`） | `docs/runbooks/alerting.md` |
| [`docs/runbooks/backup.md`](backup.md) | 「数据库灾备」 | 同目录 |

---

## 9. 版本演进

| 版本 | 改动 |
|------|------|
| v1.2.1 之前 | 单文件 `Caddyfile`（HTTP :80 单一模式，注释里有「生产示例」段） |
| **v1.2.76** | **本文**：拆分 `caddy/Caddyfile.{local,prod}` 双文件 + docker-compose entrypoint 按 `CADDY_MODE` 切换 + `caddy validate` 静态校验 + lint.sh + e2e A47 + runbook + SPEC.md sync |
| v1.3+ (planned) | DNS-01 自定义 Caddy 镜像 / introspection endpoint 防滥用 / 自动续期监控加强 |
