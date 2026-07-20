# LiteCode-CPP 压测报告（并发判题）

> Phase 8 ☆「压测报告」。本文件的 **结果段由 `scripts/load_test.sh` 运行时再生**，
> 手改结果段无意义；方法论 / 口径 / 运行方式为静态说明。

## 1. 目的与验收口径（SPEC §12.2）

| 指标 | 标准 |
|------|------|
| 提交 API 响应 | < 200ms（立即返回 submission_id） |
| 判题响应（P95） | < 5s（简单题 < 3s） |
| 并发判题 | 支持 10 人同时提交不阻塞，超出排队 |

对应 SPEC §11 Phase 8：`☆ 压测报告（5/10/20 人并发判题，验证 P95 < 5s）`。

## 2. 方法论

- **被测题目**：`two-sum`（easy，time_limit 1000ms），provision 阶段幂等 bulk-import。
- **提交代码**：two-sum 的 AC 解（`unordered_map` O(n)，见 `scripts/demo_judge_three_states.py` CODE_AC）。
- **并发级**：`5 / 10 / 20`（`CONCURRENCY_LEVELS` 可覆盖）。每级起 N 个后台 worker 同时提交。
- **预热**：正式压测前先单发 1 次（`WARMUP=1`），抹平编译缓存 / warm pool 冷启对首批的影响。
- **单次采样**：worker 提交后轮询 `GET /submissions/:id` 到终态（ac/wa/…/se）或超时。
  - `submit_api_ms`：curl `%{time_total}`（提交请求本身的 RTT，对应「立即返回」口径）。
  - `e2e_ms`：提交 → 终态的墙钟（`date +%s.%N` 差），对应「判题响应 P95」口径。
- **分位数**：对样本 `sort -n` 后取 `ceil(p/100 · n)` 序位（小样本稳健，无浮点插值）。
- **判定**：每级三条断言 —— 判题 e2e P95 < 5000ms、提交 API p95 < 200ms、
  错误数（se/timeout/submit_fail）= 0。任一不满足该级判 FAIL。

## 3. 如何运行

```bash
# 需要 live stack（web + judge docker）在线
bash scripts/load_test.sh

# 自定义并发级 / 阈值
CONCURRENCY_LEVELS="5 10 20 50" P95_THRESHOLD_MS=5000 bash scripts/load_test.sh

# CI 强约束：缺栈直接失败（不再宽松 skip）
LOAD_STRICT=1 bash scripts/load_test.sh
```

跑完本文件的「运行环境」与「结果」两段会被自动覆盖。

## 4. 运行环境（最近一次运行再生）

| 项 | 值 |
|----|----|
| 运行时间 | 待实测（pending） |
| git 版本 | 待实测（pending） |
| BASE_URL | 待实测（pending） |
| warm_pool（/health） | 待实测（pending） |
| 并发级 | 5 10 20 |
| 判题 P95 阈值 | 5000 ms |
| 提交 API 阈值 | 200 ms |

## 5. 结果（最近一次运行再生）

单位：延迟 ms，吞吐 req/s。判题 e2e 分位数只统计到达终态的样本。

| 并发 N | 提交成功(AC) | 错误 | 提交API p95 | e2e min | e2e p50 | e2e p95 | e2e p99 | e2e max | 吞吐 | 判定 |
|--------|------------|------|------------|---------|---------|---------|---------|---------|------|------|
| 5 | 待实测 | — | — | — | — | — | — | — | — | pending |
| 10 | 待实测 | — | — | — | — | — | — | — | — | pending |
| 20 | 待实测 | — | — | — | — | — | — | — | — | pending |

> 上表为 **占位（pending）**：本机 Docker down、无 live stack，尚未产出真实实测数字。
> 在有 web + judge docker 的环境执行 `bash scripts/load_test.sh` 后，本段会被真实分位数覆盖。

## 6. 解读与限制

- 判题 e2e 含**编译 + 容器启动 + 执行 + 回写**全链路；warm pool 命中率直接影响 P95。
- 首批提交若遇冷编译缓存 / 空 warm pool，尾部延迟会抬高——`WARMUP=1` 用于缓解。
- `date +%s.%N` 在 MSYS/Git-Bash 上纳秒精度依赖底层实现，毫秒级足够本口径。
- 并发超过 judge 并发容量时任务排队，e2e 尾延迟随之上升，属预期行为（验证「超出排队不阻塞」）。
