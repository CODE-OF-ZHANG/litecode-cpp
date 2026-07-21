# 性能 Profile Runbook

> **SPEC**：§11 Phase 9 △「性能 Profile（perf / flamegraph 跑一次判题热路径）」 / §12.2 性能验收口径
> **自动化**：
>   - 脚本：`scripts/perf_profile.sh`（三段：HTTP 面分阶段 timing / Prometheus histogram / Linux perf+flamegraph）
>   - 自检：`scripts/lint.sh perf_profile`（bash -n + 22 项关键环节点 + 行数 ≥ 250）
>   - 端到端：`scripts/e2e_acceptance.sh` A45（静态配置 + runbook 引用阈值 + 委托运行反向汇入）
>   - 报告：`docs/performance-profile.md`（运行时再生结果段，沿用 v1.2.65 load-test-report.md 模板）
> **节奏**：每次发版前跑一次（pre-release gate）；CI 默认宽松（缺前置 SKIP 不 fail）；CI 强约束走 `PROFILE_STRICT=1`

---

## 1. 目的

把 SPEC 「perf / flamegraph 跑一次判题热路径」**端到端落地**——不是「在 SPEC
里写一条就算数」，而是「跑一次真能给出热路径瓶颈分布、可复现、可入 CI、可入发版门禁」。

性能调优的两个常见误区：

- **「看脸调优」**：拿生产 latency 数字脑补 root cause，没有 profile 数据。
- **「过度优化」**：在没有真实 hot spot 的地方卷复杂度。

本脚本是 v1.2.65 压测报告的**补充层**——压测回答「吞吐与延迟达标吗？」，
profile 回答「延迟慢在哪？」。两者并列，不互替。

---

## 2. 落地清单（v1.2.74）

| 组件 | 文件 | 作用 |
|------|------|------|
| 主脚本 | `scripts/perf_profile.sh` | 三段测量 + 报告再生 |
| 自检 | `scripts/lint.sh perf_profile` | bash -n + 22 项关键环节点 + 行数 sanity |
| e2e | `scripts/e2e_acceptance.sh` A45 | 静态配置 + 委托运行反向汇入主计数器 |
| 报告 | `docs/performance-profile.md` | 结果段运行时再生（方法论段静态） |
| 火焰图 | `docs/perf-flamegraph.svg` | Phase C 产出（运行时生成；不进 git） |
| 本 runbook | `docs/runbooks/performance-profile.md` | 本文 |

---

## 3. SPEC §12.2 性能验收口径

| 指标 | 标准 | Profile 测量方式 |
|------|------|------------------|
| 健康检查 | < 50ms | Phase A `time_starttransfer` p95 |
| 题目列表 API | < 200ms | Phase A `time_total` p95 |
| 提交 API 响应 | < 200ms | Phase A `time_total` p95 |
| 排行榜 API | < 500ms（SPEC 字面）/ < 200ms（沿用列表阈值） | Phase A `time_total` p95 |
| 判题响应 P95 | < 5s（简单题 < 3s） | Phase B `litecode_judge_duration_seconds` histogram |
| 并发判题 | 支持 10 人 | 见 v1.2.65 `scripts/load_test.sh`（不重做） |
| 内存 / CPU | — | Phase C `perf record -g` 30s 火焰图 |

阈值常量在 `scripts/perf_profile.sh` 头部；改阈值不需要改脚本逻辑。

---

## 4. Phase A — HTTP 面分阶段 timing 拆解

### 4.1 原理

`curl -w` 提供 6 个时间点（秒）：

```
time_namelookup    DNS 解析
time_connect       TCP 三次握手
time_appconnect    TLS 握手（HTTPS）
time_pretransfer   准备发送
time_starttransfer TTFB（首字节）
time_total         总耗时
```

通过对同一 endpoint 采 N 次（默认 20），按 `sort -n` + `ceil(p/100·n)` 序位
法取分位数（与 v1.2.65 load_test.sh 同款，小样本稳健）。

### 4.2 默认覆盖 endpoint

| endpoint | 方法 | SPEC 阈值 | 用途 |
|----------|------|-----------|------|
| `/health` | GET | < 50ms | 健康检查（路由最快路径） |
| `/problems?limit=20` | GET | < 200ms | 列表 API（DB 查询） |
| `/auth/login` | POST | < 200ms | 提交 API 路径（写路径；含 bcrypt 校验） |
| `/api/v1/metrics` | GET | < 200ms | Prometheus scrape 路径（每 15s 触发） |

Phase A 只对 `time_total p95` 做硬断言；其它阶段只观察不阻断。

### 4.3 跑法

```bash
# 默认 20 次采样
bash scripts/perf_profile.sh

# CI 强约束
PROFILE_STRICT=1 bash scripts/perf_profile.sh

# 自定义采样 / 阈值
PROFILE_SAMPLES=50 HEALTH_MAX_MS=50 bash scripts/perf_profile.sh
```

跑完 `docs/performance-profile.md` 的「§5.1 Phase A」段会被自动覆盖。

---

## 5. Phase B — Prometheus histogram 判题热路径拆解

### 5.1 原理

v1.2.68 落地的 `litecode_judge_duration_seconds` 是 histogram：

```
litecode_judge_duration_seconds_bucket{le="0.005"} 12
litecode_judge_duration_seconds_bucket{le="0.01"}  15
...
litecode_judge_duration_seconds_bucket{le="5"}     234
litecode_judge_duration_seconds_bucket{le="10"}    235
litecode_judge_duration_seconds_bucket{le="+Inf"}  235
litecode_judge_duration_seconds_sum   187.432
litecode_judge_duration_seconds_count 235
```

`histogram_quantile(0.95, rate(_bucket[5m]))` 是 Prometheus 服务端的标准做法。
本脚本在客户端**模拟**这个公式：

1. 前后两次 scrape `/api/v1/metrics`（间隔一段 5 路并发 AC 压测）；
2. `_count` / `_sum` 差值得「Δ样本数 / Δ总耗时 / avg」；
3. 第二个 snapshot 的 `_bucket` 序列用**线性插值**反推 p50/p95/p99
   （典型 histogram_quantile 客户端实现）。

### 5.2 增量为 0 的情况

`Δ_count=0` 意味着本脚本期间没有新判题任务落进 worker。这种情况
profile 失真（baseline 还是历史累计分布）。**判定 FAIL**——把
`PROFILE_SAMPLES` 调大，或者挂一段 v1.2.65 load_test 跑一段时间再 profile。

### 5.3 跑法（已包含在 `scripts/perf_profile.sh` Phase B）

```bash
# 单跑（与 Phase A / C 一起）
bash scripts/perf_profile.sh

# 想加大样本量（先跑 load_test 一段时间）
PROFILE_SAMPLES=50 bash scripts/perf_profile.sh
```

---

## 6. Phase C — Linux perf + flamegraph（可选）

### 6.1 原理

[perf](https://perf.wiki.kernel.org/) 是 Linux 内核的 CPU profiler；
[FlameGraph](https://github.com/brendangregg/FlameGraph) 是 Brendan Gregg 的
火焰图生成器套件。两者结合是定位「CPU 在哪个函数最热」的事实标准：

```
docker exec litecode-web perf record -F 99 -p $PID -g -- sleep 30
docker cp litecode-web:/tmp/perf.data /tmp/
perf script -i /tmp/perf.data | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

- `-F 99` 采样频率 99Hz（避免与某些 timer 冲突；99 是 perf 文档建议）
- `-g` 抓调用栈（不抓只能给扁平 profile）
- `-- sleep 30` 抓 30s（短了样本不够；长了 flame.svg 太大）

### 6.2 前置条件

| 工具 | 安装 | 缺失时行为 |
|------|------|-----------|
| `perf` 命令 | `apt install linux-tools-$(uname -r)`（容器内或宿主） | Phase C SKIP |
| FlameGraph | `git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph` | Phase C SKIP |
| Linux 容器 | `docker compose up -d web` | Phase C SKIP |
| `perf_event_paranoid` | Ubuntu 默认 4，需 `--privileged` 或 `kernel.perf_event_paranoid=1` | perf record 失败 |

Windows / macOS dev box **永远 SKIP Phase C**（无 perf 内核模块）——这是预期的，
不算 bug。

### 6.3 火焰图阅读

火焰图 SVG 用浏览器打开：

- **y 轴** = 调用栈深度（顶层是 `main` / `litecode_server`）
- **x 轴** = 字母序排序后**并不**是时间；每个矩形的**宽度**代表「采样点命中
  次数」≈ CPU 在该函数花的相对时间
- **红色** = on-CPU（最常见，找宽的红色矩形就是 hot spot）
- **黄色** = 部分 on-CPU
- **绿色** = off-CPU（阻塞在 IO / sleep）

找热点的标准操作：

1. 找最宽的红色顶层矩形 = 「这个函数最耗 CPU」
2. 点一下，火焰图 drill 到该函数为顶，看它的子调用
3. 重复直到看到具体库调用（如 `std::sort` / `mysql_real_query` / `docker_post`）

### 6.4 跑法

```bash
# 自动（容器在线 + 工具齐全时跑；否则 SKIP）
bash scripts/perf_profile.sh

# 手动（自定义容器 / 频率 / 时长）
PERF_CONTAINER=litecode-web-staging \
PERF_DURATION_S=60 \
PERF_FREQ_HZ=999 \
FLAMEGRAPH_DIR=/opt/FlameGraph \
bash scripts/perf_profile.sh
```

火焰图输出：`docs/perf-flamegraph.svg`（运行时产物，不入 git）。

---

## 7. 一键操作

### 7.1 完整跑一次（pre-release）

```bash
# 1. 起栈
docker compose up -d mysql docker-proxy judge web

# 2. 等 /health 全绿
until curl -fsS http://localhost:8080/api/v1/health | jq -e '.docker=="ok"'; do
    sleep 2
done

# 3. 跑 profile（默认 20 采样；Phase C 自动判断）
bash scripts/perf_profile.sh

# 4. 看报告
less docs/performance-profile.md
[ -f docs/perf-flamegraph.svg ] && xdg-open docs/perf-flamegraph.svg
```

### 7.2 只跑某一段

```bash
# 改脚本默认 phase 开关在脚本顶部；目前是 A+B+C 全跑
# 想要只跑 A：临时注释 run_phase_b / run_phase_c
```

### 7.3 跑自检 / e2e

```bash
# 静态（无栈依赖）
bash scripts/lint.sh perf_profile

# 端到端（live 栈）
bash scripts/e2e_acceptance.sh   # 默认宽松
PROFILE_STRICT=1 E2E_STRICT=1 bash scripts/e2e_acceptance.sh   # CI 强约束
```

### 7.4 集成到发版门禁

CI workflow（v1.2.58 已落地）当前未直接跑 profile（避免 30s+ perf 抓取拖慢
PR feedback）。建议：

- **PR**：跑 `lint.sh perf_profile` + `e2e A45a` 静态配置层（秒级）
- **main → release**：人工触发 `perf_profile.sh`（含 Phase C），把
  `docs/performance-profile.md` + `docs/perf-flamegraph.svg` 作为 release
  artifact 上传

---

## 8. 故障排查

### 8.1 Phase A 全 endpoint 超阈值

最常见 root cause：

- **/health 超 50ms**：DB ping 或 Docker ping 阻塞。看 `docker stats litecode-web`
- **/problems 超 200ms**：DB 查询或 page cache miss。`SHOW PROCESSLIST` 看慢查询
- **/auth/login 超 200ms**：bcrypt cost=12 在测试机器上大约 100ms，正常；
  超 200ms 看是不是同 IP 触发 rate limit（v1.2.63）走 429 误算
- **/api/v1/metrics 超 200ms**：histogram bucket 数太多 / gauge provider 阻塞
  （v1.2.68 落地 7 family，默认 < 50ms）

### 8.2 Phase B histogram 无新增样本

```bash
# 手动验证 /api/v1/metrics 本身是否可达
curl -sS http://localhost:8080/api/v1/metrics | grep judge_duration_seconds

# 看 worker 是不是真的在跑
curl -sS http://localhost:8080/api/v1/metrics | grep -E 'litecode_judge_(running|queue)'
```

如果 `running_count=0` 但 `queue_size>0`，是 worker 阻塞了。
否则增大 `PROFILE_SAMPLES` 或先跑 v1.2.65 load_test 一段时间。

### 8.3 Phase C `perf record` 失败

```bash
# 在容器内手动跑
docker exec litecode-web perf record -F 99 -p 1 -g -- sleep 5
# 常见报错：
#   'Permission denied' → kernel.perf_event_paranoid > 1；docker run --privileged
#   'perf not found'     → apt install linux-tools-$(uname -r)
#   'PMU: list might be truncated' → 容器内核与镜像不匹配，重建镜像
```

### 8.4 `stackcollapse-perf.pl` / `flamegraph.pl` 缺失

```bash
# Debian/Ubuntu
git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph

# macOS
brew install FlameGraph
ln -s /opt/homebrew/opt/FlameGraph /opt/FlameGraph 2>/dev/null || true
```

设 `FLAMEGRAPH_DIR` 环境变量指向实际路径。

### 8.5 火焰图全是灰色 / 全是 on-CPU 但函数名都 `<unknown>`

- 容器镜像**去掉了 debug 符号**。重建镜像时带 `-DCMAKE_BUILD_TYPE=RelWithDebInfo`
  或 `cmake -DCMAKE_BUILD_TYPE=Debug` 重新编译（v1.2.68 metrics.cpp 已经是
  Release；flame graph 才有符号）
- 火焰图 SVG 过大（> 10MB）浏览器卡死：在 `flamegraph.pl` 加
  `--countname samples --width 1800`

---

## 9. 与 load_test / metrics / alerting 的关系

| 工件 | 回答的问题 | 文件 |
|------|-----------|------|
| `load_test.sh`（v1.2.65） | 「并发判题达标吗？」 | `docs/load-test-report.md` |
| `perf_profile.sh`（v1.2.74） | 「延迟慢在哪？」 | `docs/performance-profile.md` + `docs/perf-flamegraph.svg` |
| `MetricsService`（v1.2.68） | 「持续指标是什么？」 | `/api/v1/metrics`（Prometheus 抓） |
| `prometheus-alerts.yml`（v1.2.73） | 「过阈值要通知吗？」 | `monitoring/alerting/` |
| `restore_drill.sh`（v1.2.72） | 「备份可恢复吗？」 | `docs/runbooks/monthly-restore-drill.md` |

性能问题的标准诊断流程：

```
1. Prometheus 触发 JudgeDurationP99TooHigh 告警
   ↓
2. 查 docs/load-test-report.md 确认是否一直超标还是偶发
   ↓
3. 跑 scripts/perf_profile.sh 跑一次
   ↓
4. Phase A 看 HTTP 路由耗时是否飙高
   Phase B 看 histogram 增量是否集中某一 bucket（编译 / 启动 / 执行哪段）
   Phase C 看火焰图 hot spot 是哪个函数
   ↓
5. 定位 root cause → 改代码 → 重跑 profile 验证
```

---

## 10. 已知未落地 / follow-up

### 10.1 Phase C 自动化 CI 触发

当前 Phase C 不入 CI（30s perf record 拖慢 feedback）。建议 v1.2.75+
加 `nightly-profile.yml`：每天 02:00 自动跑 Phase A+B+C，把
`docs/performance-profile.md` + `docs/perf-flamegraph.svg` 作为 artifact
上传，与昨日做 diff（火焰图 regression = 热路径函数占比变化 > 5% 即报警）。

### 10.2 HTTP request duration histogram

`src/routes/metrics.h` 当前只暴露 `judge_duration_seconds` histogram。
SPEC §16.2 还提到 `litecode_http_request_duration_seconds`（按 method +
path label）。Phase A 用 `curl -w` 模拟做了客户端分阶段，但服务端的
「全路由 P95」要靠这个 histogram 才能在 Prometheus 里 `histogram_quantile`
算出来。

落地路径：

1. `MetricsService::observe_histogram` 已经是单 label（status）专用；
   `register_histogram(name, help, buckets)` 可以直接复用一个无 label 版
   （`observe_histogram_nolabel(name, value)` 或扩 inc_counter 多 label）。
2. 在 `src/app_context_metrics.cpp` 注册 `litecode_http_request_duration_seconds`，
   bucket 边界对齐 Phase A（5ms→10s）。
3. 在每个 `register_*_routes` 的 handler 外包一层 wrapper（`timer_t` +
   `metrics.observe_histogram(name, dur)`）。

### 10.3 Per-IP per-endpoint counter

`LoginFailuresByIPHigh` 占位 alert（v1.2.73 已落地）的真触发依赖
`litecode_auth_failures_total{ip}` —— `MetricsService::inc_counter`
当前 label 写死为 `status`。扩成多 label 后 v1.2.75+ 把告警 expr
从 `vector(0) > 100` 切到 `sum by (ip) (rate(...[1h])) > 100`。

---

## 11. 版本演进

| 版本 | 改动 |
|------|------|
| v1.2.65 | `scripts/load_test.sh` 黑盒压测 + `docs/load-test-report.md`（Phase 8 ☆） |
| v1.2.68 | `MetricsService` 落地 7 family：`litecode_submissions_total` + `litecode_judge_duration_seconds` + 5 gauge（Phase 9 ★） |
| v1.2.74 | **本文** + `scripts/perf_profile.sh` + `scripts/lint.sh perf_profile` + `scripts/e2e_acceptance.sh` A45 + `docs/performance-profile.md`（Phase 9 △） |
| v1.2.75+ (planned) | `litecode_http_request_duration_seconds` histogram + nightly profile CI + `LoginFailuresByIPHigh` 真触发 |