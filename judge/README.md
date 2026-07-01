# LiteCode-CPP — Judge Module (`judge/`)

> **SPEC §7 / §11 Phase 4 / §15.4 / §15.5**
>
> 判题镜像 + 判题执行脚本，由 web 服务通过 docker run 临时启动。

---

## 目录布局

```
judge/
├── Dockerfile         # 判题镜像构建（g++/gcc/gdb + coreutils/dos2unix/jq + judge.sh）
├── judge.sh           # 入口：编译 + 逐点运行 + cgroup 测量 + 状态聚合 + 输出 JSON
├── lib/               # 模块化子脚本（被 judge.sh source）
│   ├── common.sh      # 时间戳 / 字节截断 / JSON 转义 / 工具函数
│   ├── cgroup.sh      # cgroup v2 cpu.stat / memory.peak 读取
│   └── compare.sh     # exact / ignore_trailing / float_eps / special 比对器
├── tests/
│   ├── test_common_unit.sh   # 单元测试：lib/*.sh（24 用例，~0.1s）
│   └── test_judge_e2e.sh     # 端到端测试：覆盖 SPEC §7 全部状态分支（AC/WA/CE/TLE/RE/OLE/PE/special/SE）
└── README.md          # 本文件
```

---

## 镜像职责

判题容器**只**做一件事：拿到 task.json、编译提交、逐测试点运行、把结果以 JSON 输出到 stdout。

资源硬限制（--cpus / --memory / --pids-limit / --network=none / --read-only / --security-opt no-new-privileges）由 web 的 docker scheduler 在 `docker run` 时注入，**不在镜像里固化**。

编译安全标志（SPEC §7.1 / §7.3 / §15.4）固化在 `judge.sh`：
```
-O2 -std=c++17 -pipe
-fstack-protector-strong
-D_FORTIFY_SOURCE=2
-Wformat -Wformat-security
-Wl,-z,now -Wl,-z,relro
```
编译独立超时（10s）由 `timeout --foreground` 实现（防编译炸弹）；
运行独立超时（30s）作为整个判题脚本的总硬超时，防止卡死容器。

---

## 调用契约

### 任务 JSON schema（web 侧下发）

```jsonc
{
  "submission_id":       42,                             // required
  "language":            "cpp",                          // "cpp" | "c"
  "code":                "...source...\n...",            // 多行源码
  "time_limit_ms":       1000,                           // 默认 1000
  "memory_limit_mb":     256,                            // 默认 256
  "compile_timeout_ms":  10000,                          // 默认 10000
  "run_hard_timeout_ms": 30000,                          // 默认 30000
  "output_limit_bytes":  16777216,                       // 默认 16M，OLE 阈值
  "test_cases": [
    {
      "input":           "STDIN 内容",
      "expected_output": "期望 stdout（已归一化对比）",
      "judge_type":      "exact|ignore_trailing|float_eps|special",
      "float_epsilon":   0.000001,                       // 仅 float_eps
      "order_num":       0                              // 默认数组下标
    }
  ]
}
```

### 任务传输方式（按优先级）

1. 环境变量 `JUDGE_TASK_FILE=/path/to/task.json`
2. 命令行参数 `$1=/path/to/task.json`
3. stdin 喂 JSON（`echo '{...}' | judge.sh`）

web 侧典型调用：
```bash
# 方式 A：写到临时文件，mount 进容器
docker run --rm \
    -v /webdata/task-123.json:/judge/task.json:ro \
    -e JUDGE_TASK_FILE=/judge/task.json \
    litecode-judge:latest

# 方式 B：env 直接传（不适合大 payload）
docker run --rm -i \
    -e JUDGE_TASK="$(base64 -w0 /webdata/task.json)" \
    litecode-judge:latest bash -c '
        echo "$JUDGE_TASK" | base64 -d > /tmp/task.json
        JUDGE_TASK_FILE=/tmp/task.json /usr/local/bin/judge.sh
    '
```

### 结果 JSON（stdout，单行）

```jsonc
{
  "submission_id":    42,
  "status":           "ac|wa|tle|mle|re|ole|pe|ce|se",
  "time_used_ms":     12,                         // 所有测试点最长用时
  "memory_used_kb":   2048,                       // 所有测试点最大内存
  "error_message":    null|"...",
  "failed_case_index": null|0,                    // 首个失败的测试点下标
  "case_results": [
    { "index": 0, "status": "ac", "time_ms": 12, "mem_kb": 2048, "info": null },
    ...
  ]
}
```

### 退出码

| 退出码 | 含义 |
|--------|------|
| 0      | 正常完成（包括 CE/RE/TLE 等业务异常） |
| 2      | 任务描述错误（JSON 解析失败）/ 缺少 submission_id（web 应当作 SE 写回 DB） |

---

## 状态判定矩阵（SPEC §7.1）

| 阶段 | 检测 | → 状态 |
|------|------|--------|
| 编译 | `g++ ... → 0` | 继续 |
| 编译 | `g++ ... → 124/137`（timeout） | **CE** "Compilation timeout" |
| 编译 | `g++ ... → 1`（错误码） | **CE** + stderr（截断 4KB） |
| 运行 | 内层 timeout (time_limit_s + 1) → 124 | **TLE** |
| 运行 | 内层 timeout 触发 kill-after → 137 | **MLE**（典型：cgroup memory 杀进程） |
| 运行 | exit code ≠ 0 | **RE** + stderr（截断 2KB） |
| 运行 | stdout 字节 > output_limit_bytes | **OLE**（不再比对） |
| 比对 | judge_type=exact cmp -s 通过 | **AC** |
| 比对 | judge_type=exact cmp -s 失败 | **WA** |
| 比对 | judge_type=ignore_trailing rstrip 后相等 | **AC** |
| 比对 | judge_type=ignore_trailing rstrip 后不等 | **PE** |
| 比对 | judge_type=float_eps | **AC** / **WA** |
| 比对 | judge_type=special | **SE**（v1.3 占位） |

---

## 文本归一化（SPEC §7.4）

| 项 | 实现 |
|----|------|
| UTF-8 BOM | `sed '1s/^\xEF\xBB\xBF//'`（仅首行首三字节） |
| CRLF | `tr -d '\r'`（输入 + 期望输出都过） |
| 输出截断 | `head -c ${output_limit_bytes}` 写入 output.txt，超出立即判 OLE |

归一化在每个测试点的 `input.txt` / `expected.txt` 落盘前完成；运行后读 `output.txt` / `expected.txt` 一起喂给比较器。

---

## cgroup v2 资源测量（SPEC §7.4）

| 指标 | 来源 | 单位 | 实现 |
|------|------|------|------|
| 时间 | `$CGROUP_BASE/cpu.stat → usage_usec` | ms | 每测试点前后差，向上取整 |
| 内存 | `$CGROUP_BASE/memory.peak` | KB | 测试点结束的容器峰值，向上取整 |

`$CGROUP_BASE` 来自 `/proc/self/cgroup` 中 `0::/xxx` 的相对路径；unified hierarchy 下通常即 `/sys/fs/cgroup`。

---

## 安全（SPEC §7.3 / §15.4 / §15.5）

| 维度 | 实现位置 |
|------|----------|
| CPU 时间 | Docker `--cpus` + 容器内 `timeout ${time_limit_s+1}` |
| 编译超时 | 容器内 `timeout 10s g++ ...` |
| 内存 | Docker `--memory` + cgroup v2 memory.peak 读取 |
| 网络 | Docker `--network=none` |
| 文件系统 | Docker `--read-only` + tmpfs `/tmp` 临时写 |
| 进程数 | Docker `--pids-limit=50` |
| 提权防护 | Docker `--security-opt=no-new-privileges` + `USER judgeuser` (UID 1000) |
| 编译强化 | `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,-z,now -Wl,-z,relro` |
| Web 隔离 | Docker Socket 白名单代理 + Web 容器非 root + 自身 cpus/memory 限制 |
| 输出限制 | head -c 16M 截断 → OLE（> 16MB 即停，不再比对） |
| 错误截断 | CE 4KB / RE 2KB（防撑爆 DB） |

容器内只有 `judgeuser`（UID 1000）：即使逃逸也无 root 可提。
用户态进程数受限（pids-limit=50），单提交无法 fork 炸弹。

---

## 测试

```bash
# 单元测试（无需 docker，直接 bash + python/jq）
cd judge/tests
./test_common_unit.sh
# 期望输出：Passed: 24, Failed: 0

# 端到端测试（需已构建 litecode-judge 镜像 + docker daemon）
./test_judge_e2e.sh
# 自动检测 host g++ 兼容性：
#   - host g++ 是 GNU ld（Linux/macOS 自带）：直接 host 跑
#   - host g++ 非 GNU ld（Windows MinGW 等）：改用 docker 镜像（litecode-judge:latest）
#   - 无 docker 环境：跳过 e2e（仅 unit 测试生效）

# 自定义镜像 tag
JUDGE_IMAGE=litecode-judge:v1.2 ./test_judge_e2e.sh
```

### 单独用 docker 跑

```bash
# 1) 编译一个最小测试 task.json
cat > /tmp/task.json <<'EOF'
{
  "submission_id": 42, "language": "cpp",
  "code": "#include <iostream>\nint main() { std::cout << \"hi\\n\"; return 0; }",
  "time_limit_ms": 1000, "memory_limit_mb": 128,
  "compile_timeout_ms": 10000, "run_hard_timeout_ms": 30000,
  "output_limit_bytes": 16777216,
  "test_cases": [{"input":"","expected_output":"hi\n","judge_type":"exact","float_epsilon":1e-6}]
}
EOF

# 2) 喂 stdin → 容器 → 截 stdout JSON
docker build -t litecode-judge:latest ./judge/
cat /tmp/task.json | docker run --rm -i litecode-judge:latest /usr/local/bin/judge.sh
# 期望：{"submission_id":42,"status":"ac",...,"case_results":[{"index":0,"status":"ac",...}]}
```

### 全资源隔离（生产用，web 调度器层配）

```bash
docker run --rm \
    --network=none --read-only \
    --cpus=1 --memory=256m --pids-limit=50 \
    --security-opt=no-new-privileges \
    --tmpfs /tmp:size=64m,mode=1777 \
    -v /webdata/task-123.json:/judge/task.json:ro \
    litecode-judge:latest /usr/local/bin/judge.sh
```

---

## 已知边界 / 后续工作

| 项 | 状态 | 说明 |
|----|------|------|
| Special Judge | **v1.3** 占位 | v1.2 返回 SE；逻辑占位可释放队列槽位但不真判 |
| Markdown 预净化 | **v1.3** 强制 | v1.2 软要求；与 Phase 4 判题模块无关 |
| Compile-time TLE 防御 | ✅ Phase 4 | g++ 独立 10s timeout + 模板递归炸测 |
| OLE 防御 | ✅ Phase 4 | 16MB 截断 → 立即判 OLE，不再比对 |
| 容器预热池 | Phase 4 follow-up | v1.2 SPEC §3.2 + §11 Phase 4，本镜像已可被空闲复用 |
| 异步判题调度器 | Phase 4 follow-up | web 侧 `src/judge/judge_scheduler.h`（空 stub）走本镜像 |

---

## 文件大小一览

| 文件 | 大小 | 角色 |
|------|------|------|
| `Dockerfile` | ~30 行 | 镜像构建；非 root + jq + 编译工具链 |
| `judge.sh` | ~280 行 | 主调度：解析 → 编译 → 逐点运行 → 聚合 JSON |
| `lib/common.sh` | ~100 行 | 工具（log / truncate / json_escape / ceil / now_ms / die_se_json / emit_final_json） |
| `lib/cgroup.sh` | ~50 行 | cgroup v2 cpu.stat / memory.peak 读取 |
| `lib/compare.sh` | ~110 行 | exact / ignore_trailing / float_eps / special |
| `tests/test_common_unit.sh` | ~120 行 | 单测：lib |
| `tests/test_judge_e2e.sh` | ~250 行 | 端到端：AC/WA/CE/TLE/RE/OLE/PE/special/SE 全部分支 |
