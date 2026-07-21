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
│   ├── compare.sh     # exact / ignore_trailing / float_eps / special（无 SPJ 兜底）比对器
│   └── spj.sh         # v1.3.1 Special Judge 三件套：compile_spj / run_spj / compare_special_with
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
  "special_judge_source":   "// (optional) C++ SPJ source code ...", // v1.3.1 — 挂 SPJ 时由 web 端从 problem_special_judges 填
  "special_judge_language": "cpp",                        // v1.3.1 — 镜像只支持 cpp
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
| 比对 | judge_type=ignore_case（v1.3.1+） | ASCII 大小写不敏感 cmp（`LC_ALL=C tr '[:upper:][:lower:]' '[:lower:][:upper:]'`）；`Hello` / `HELLO` / `hello` 都判 **AC**；非字母差异（如数字）按原样 cmp |
| 比对 | judge_type=ignore_all_whitespace（v1.3.1+） | 行内 `[ \t]+ → 单空格` 折叠 + 空行忽略后 cmp；`a  b\tc\n\nd e\n` 与期望 `a b c\nd e\n` 判 **AC**；末尾多空格也归一化（与 `ignore_trailing` 在末尾空白上重叠） |
| 比对 | judge_type=special（有 SPJ 编译成功） | 调 `spj_bin <input> <expected> <actual>`：`rc=0` ⇒ **AC**；`rc=1` ⇒ **WA**；其它 ⇒ **SE**（case 级） |
| 比对 | judge_type=special（SPJ 编译失败） | 整 submission **SE** + error_message 含 SPJ 编译 stderr 指纹 |
| 比对 | judge_type=special（题未挂 SPJ） | **WA**（"no special judge configured" 兜底，让 operator 看到题没挂 SPJ） |

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

## Special Judge 协议（v1.3.1）

判题镜像支持**管理员上传的 C++ SPJ 程序**对单题做自定义比对。三参数顺序固定为
`<input> <expected_output> <actual_output>`（不是 Codeforces testlib 的 `<input> <output> <answer>`），
判 `rc=0 ⇒ AC` / `rc=1 ⇒ WA` / 其它 ⇒ SE。整段 SPJ 流程在 `judge/lib/spj.sh`：

| 函数 | 作用 | 调用方 |
|------|------|--------|
| `compile_spj <src> <out_bin>` | g++ 编译 SPJ（10s 超时），写 stderr 到 `<out_bin>.err` | judge.sh 主循环 |
| `run_spj <bin> <input> <expected> <actual>` | 调 SPJ 三件套，rc 直传 | compare_special_with |
| `compare_special_with <bin> <out> <expected> <input>` | drop-in 替代 `compare_special`（参数顺序匹配 judge.sh 调用点） | judge.sh step 3 special case |
| `spj_stdout_for_info [limit]` | 读 `JUDGE_TMP/spj_stdout` 截到 limit 字节，返 SPJ 拒绝原因进 case_results.info | judge.sh case WA / SE 分支 |
| `spj_err_for_info <bin> [limit]` | 读 `<bin>.err` 截到 limit 字节，返编译错误进 error_message | judge.sh SPJ compile failed 分支 |

**SPJ 调用契约**（管理员写 SPJ 时参考）：

```cpp
// int main(int argc, char* argv[])
//   argv[1] = input   file path      (用户提供给 solution 的 stdin 副本)
//   argv[2] = expected output file path  (DB 里 test_cases.expected_output)
//   argv[3] = actual   output file path  (solution 跑出来的 stdout)
//   退出码: 0 = AC, 1 = WA, 其它 = SE
//   stdout 第一行  →  case_results[i].info (截到 2KB)
//   stderr         →  不捕获（噪音隔离）
//   timebox        →  同 solution 的 time_limit_s + 1
//   membox         →  同 solution 的 memory_limit_mb（继承容器 cgroup）
```

**最小可工作 SPJ 模板**（永远 AC：expected 与 actual 字节比较）：

```cpp
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 4) return 2;  // 参数错 → SE
    FILE* e = std::fopen(argv[2], "rb");
    FILE* a = std::fopen(argv[3], "rb");
    if (!e || !a) return 2;
    int c1, c2;
    do { c1 = std::fgetc(e); c2 = std::fgetc(a);
         if (c1 != c2) { std::fclose(e); std::fclose(a); return 1; }  // WA
    } while (c1 != EOF && c2 != EOF);
    std::fclose(e); std::fclose(a);
    return 0;  // AC
}
```

**典型进阶 SPJ**（浮点容差、单向差、特殊数据结构匹配）：管理员在 `problems/README.md`
的 `special_judge_source` schema 注释 + `docs/runbooks/special-judge.md §6` 找模板。

**安全模型**（与 solution 同 jail）：
- SPJ 用与 solution 完全相同的 secure compile flags（`-fstack-protector-strong` /
  `-D_FORTIFY_SOURCE=2` / `-Wl,-z,now` / `-Wl,-z,relro`），见 `judge/lib/spj.sh:66-76`
- SPJ 进程继承 solution 的 cgroup CPU / memory cap
- SPJ timebox = `time_limit_s + 1`（与 solution 同超时）
- 容器 `--security-opt=no-new-privileges` + `USER judgeuser (UID 1000)` 兜底
- SPJ 编译失败 → 整 submission 翻 SE + error_message 含 stderr 指纹（不让 case 误判 WA）

**`compare_special` 在 `compare.sh` 的兜底路径**仍保留：题未挂 SPJ 时（即 `task.json` 的
`special_judge_source` 为空字符串），`compare_special` 返 `rc=1` 让 judge.sh 走 WA 分支，
operator 看到「no special judge configured」就知道题还没挂 SPJ — 这是 v1.2.52 起的行为，
v1.3.1 闭环后兜底逻辑完整保留。

**task.json 字段**（web 端从 `problem_special_judges` 填，由 `submission_routes.h:614-647` 装载）：
- `special_judge_source`: 可选，C++ 源码字符串；空 = 无 SPJ（走 compare.sh 兜底 WA）
- `special_judge_language`: 可选，默认 `"cpp"`（镜像只支持 C++）

**完整运行示例**：

```bash
cat > /tmp/spj-task.json <<'EOF'
{
  "submission_id": 42, "language": "cpp",
  "code": "#include <iostream>\nint main(){std::cout<<\"hello\\n\";return 0;}",
  "time_limit_ms": 1000, "memory_limit_mb": 128,
  "compile_timeout_ms": 10000, "run_hard_timeout_ms": 30000,
  "output_limit_bytes": 16777216,
  "special_judge_source": "#include <cstdio>\nint main(int argc,char**argv){if(argc!=4)return 2;FILE*e=fopen(argv[2],\"rb\");FILE*a=fopen(argv[3],\"rb\");if(!e||!a)return 2;int c1,c2;do{c1=fgetc(e);c2=fgetc(a);if(c1!=c2){fclose(e);fclose(a);return 1;}}while(c1!=EOF&&c2!=EOF);fclose(e);fclose(a);return 0;}",
  "special_judge_language": "cpp",
  "test_cases": [
    {"input":"","expected_output":"hello\n","judge_type":"special","float_epsilon":1e-6}
  ]
}
EOF
docker run --rm -i litecode-judge:test /usr/local/bin/judge.sh < /tmp/spj-task.json
# 期望：{"submission_id":42,"status":"ac","time_used_ms":...,"memory_used_kb":...,"error_message":null,"failed_case_index":null,"case_results":[{"index":0,"status":"ac",...,"info":null}]}
```

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
| Special Judge | ✅ v1.3.1 闭环 | admin 上传 C++ SPJ → judge.sh 编译 → 按 rc 判 AC/WA/SE；judge/lib/spj.sh 三件套完整；无 SPJ 兜底 WA |
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
