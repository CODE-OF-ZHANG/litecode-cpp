# judge/tests/ — 判题套件测试

## 跑什么

| 文件 | 范围 | 依赖 |
|------|------|------|
| `test_common_unit.sh` | `lib/common.sh` / `cgroup.sh` / `compare.sh` 纯 bash 单元测试 | bash + jq |
| `test_judge_e2e.sh` | `judge.sh` 端到端：13 个状态分支（AC / AC-empty-expected / WA / CE-syntax / CE-bomb / TLE / RE / OLE / MLE / ignore_trailing / float_eps / SJ / SE-missing / CRLF-BOM） | bash + jq + docker + host g++ (GNU ld) |

## 跑命令

```bash
bash judge/tests/test_common_unit.sh && bash judge/tests/test_judge_e2e.sh
```

只跑 e2e：

```bash
bash judge/tests/test_judge_e2e.sh
```

## 跳过条件

- 缺 `docker` → e2e 走 `JUDGE_SH` 本地路径，所有 `guard_or_skip` 降级为 skip
- host g++ 不是 GNU ld（macOS 系统 clang）→ e2e 同样降级为 skip
- 缺 `jq` → `test_common_unit.sh` 早退；e2e 也跑不动

## v1.2.51 新增 case

| case | 锁住的不变量 |
|------|-------------|
| `AC-empty-expected` | `judge.sh:212-217` 的 `sed -e '$ {/^$/d}'` —— 空 expected_output 时 sed 把 jq 的行终止符删掉，避免 expected.txt = "\n" vs solution = "" 的 cmp 不等 |
| `MLE` | 200MB > 64MB limit → docker OOM kill → mle 状态 |

## v1.2.51 新增 gtest（`tests/unit/`）

| test | 锁住的不变量 |
|------|-------------|
| `BuildCreateBody.VolumeMountsSchema` | `Mount::volume_mount` 序列化路径（v1.2.50-b 新分支） |
| `BuildCreateBody.MixedBindAndVolume` | Bind + Volume 同数组，按各自 schema 序列化 |
| `StripDockerLogFrames.MultiplexedStdout` | 8-byte frame 解析：stream=1 → 提取 payload |
| `StripDockerLogFrames.MixedStdoutAndStderr` | 两帧（stdout + stderr）分别提取并按位拼接 |
| `StripDockerLogFrames.NonMultiplexedPassthrough` | 首字节 > 2 → 原样返回 |
| `EndpointToUrl.NormalizesTcpToHttp` | `tcp://h:p` → `http://h:p` 归一化 |
| `JudgeSchedulerConfig.TaskVolumeNameEmptyBindFallback` | `cfg.task_volume_name=""` → Bind 路径 |
| `JudgeSchedulerConfig.TaskVolumeNameSetVolumeMount` | `cfg.task_volume_name="judge-tmp"` → Volume 路径 |
| `JudgeSchedulerConfig.TmpfsExecFlagOnBothMounts` | `opts.tmpfs` 含 `/tmp` + `/judge`，都带 `,exec` |

## 跑 gtest

需要 CMake 配置时开测试：

```bash
cmake -B build -DLITECODE_BUILD_TESTS=ON
cmake --build build --target test_docker_client test_judge_scheduler
ctest --test-dir build -R 'docker_client|judge_scheduler' --output-on-failure
```