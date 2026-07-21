# LiteCode-CPP — Special Judge 运维手册

> **SPEC §11 Phase 4 ☆ + §4.3 judge_type='special' — v1.3.1 闭环落地**
>
> 本手册覆盖 admin 上传 C++ Special Judge（SPJ）源程序的全链路：协议、启
> 用、安全模型、故障排查。与 `judge/README.md` 的协议细节（参数顺序、退出码
> 映射、SPJ 模板）配合使用 — 本文件聚焦"运维"视角。

---

## §1 目的

为什么需要 Special Judge：

| 场景 | exact 比对 | special 比对 |
|------|------------|--------------|
| 两数之和（确定输出） | ✅ 直接用 | 不需要 |
| 浮点求和（1e-4 容差） | ❌ 字节级失败 | ✅ SPJ 容差 |
| 拓扑排序（任意合法解） | ❌ 只接受一种 | ✅ SPJ 验证"是合法拓扑序" |
| 多项式求导（手写 vs 库） | ❌ 符号格式差异 | ✅ SPJ 数值代入验证 |
| 计算几何（浮点误差） | ❌ 浮点字节不稳定 | ✅ SPJ ε 容差 |

简单说：标准比对无法表达的"题目语义的合法输出集合"，用 SPJ 把验证逻辑写进
判题环节。管理员负责把"合法 / 不合法"的判定代码上传，本系统负责编译、
隔离、调度、超时。

---

## §2 落地清单（v1.3.1 P0 闭环）

本节列出 v1.3.1 commit 改动的所有文件，方便审计 / rollback。

### 2.1 后端（src/*）

| 文件 | 改动 |
|------|------|
| `src/db/audit_log_repo.h` | +2 action enum：`kActionProblemSpjUpsert = "problem.spj_upsert"` / `kActionProblemSpjRemove = "problem.spj_remove"`（line 472-473） |
| `src/db/special_judge_repo.h` | **已存在** V010 schema + repo（v1.2.18）：`upsert` / `remove_by_problem_id` / `find_by_problem_id` / `exists_for_problem` / `validate_source` / `validate_language` |
| `src/routes/admin_problem_routes.h` | +3 admin SPJ 端点：`PUT/GET/DELETE /api/v1/admin/problems/:slug/special-judge`；admin 端 256KB clamp（`kMaxSpjSourceLenAdmin`） |
| `src/routes/problem_routes.h` | `serialize_sample` 透 `judge_type`（line ~464）；`serialize_problem_detail` 加 `has_special_judge` 布尔 |
| `src/main.cpp` | 无新代码 — `register_admin_problem_routes` 已在 line 184 注册 |
| `src/judge/judge.sh` | 已有完整 SPJ 接线（line 233-260 SPJ compile + line 475-554 special case 三分支） |
| `judge/lib/spj.sh` | **已存在**（v1.2.18 落地）：`compile_spj` / `run_spj` / `compare_special_with` / `spj_stdout_for_info` / `spj_err_for_info` |

### 2.2 测试（tests/* + judge/tests/*）

| 文件 | 改动 |
|------|------|
| `tests/unit/test_admin_special_judge.cpp` | **新建**（22 用例）：admin 端 clamp / audit action 常量 / validator 边界 / repo 集成（idempotent + FK cascade） |
| `tests/CMakeLists.txt` | +1 `add_executable` / +1 `target_link_libraries` / +1 `add_test`（与 test_special_judge 同款 link 集） |
| `judge/tests/test_judge_e2e.sh` | 升级 `[special]` 占位（v1.2.52 "无 SPJ → WA"）→ 三层断言：(a) 无 SPJ → WA；(b) 有 SPJ + 正解 → AC；(c) SPJ 编译失败 → SE |

### 2.3 自检 / e2e / runbook

| 文件 | 改动 |
|------|------|
| `scripts/lint.sh` | +`do_special_judge` 子任务（沿用 `do_caddy` / `do_backup` 模板），9 段 26 项关键环节点 + 行数 sanity + 接入 `all)` case + 独立 `special_judge)` 子命令 |
| `scripts/e2e_acceptance.sh` | +A48（沿用 A47 三层）：A48a 静态 26 项 / A48b runbook 9 项术语 / A48c 真栈（admin PUT SPJ → 公共 detail has_special_judge → 提交 AC_CODE → status=ac → DELETE → has_special_judge=false） |
| `judge/README.md` | 加 `lib/spj.sh` 进目录布局；JSON schema 加 `special_judge_source` / `special_judge_language`；状态判定矩阵 special 行展开三种分支；加完整 Special Judge 协议章节；"已知边界" Special Judge 行从 v1.3 占位改为 ✅ v1.3.1 闭环 |
| `problems/README.md` | `judge_type` 表 `special` 行措辞改写（v1.3+ 占位 → v1.3.1 闭环）；新增"### Special Judge（v1.3.1 闭环）"章节 + 字段表 + 256KB clamp 注释 |
| `docs/runbooks/special-judge.md` | **新建**（本文件） |

### 2.4 SPEC.md 同步

| Line | 改动 |
|------|------|
| 463 | `special → v1.3+ 实现，MVP 阶段留字段` → `special → 挂 problem_special_judges 表 SPJ，judge.sh 调 spj.sh 三件套，无源码兜底判 WA（v1.3.1）` |
| 720 | `special → MVP 返回 SE（v1.3+ 接入 SPJ）` → `special → 有 SPJ 调 spj_bin <input> <expected> <actual>（rc 0/1/其它→AC/WA/SE）；无 SPJ 全 case 判 WA（v1.3.1）` |
| 1067 | `[x] ☆ Special Judge 框架（v1.3）` 追加 commit hash 标注 |
| 1222 | v1.3 路线图 4 子标（Special Judge / Markdown 预净化 / problem_revisions / Contest） |

---

## §3 协议对照表

### 3.1 SPJ 退出码 → submission status 映射

| SPJ 行为 | 退出码 | 该 case 状态 | submission 整体状态 | 备注 |
|----------|--------|--------------|---------------------|------|
| 接受输出 | `0` | `ac` | `ac`（除非有 case 失败） | case_results[i].info = SPJ stdout 第一行（截 2KB） |
| 拒绝输出 | `1` | `wa` | `wa`（首个失败 case） | case_results[i].info = SPJ stdout 第一行（截 2KB） |
| 非法退出（崩溃 / signal） | `!=0` 且 `!=1` | `se` | `se` | case_results[i].info = "spj exit=<rc>: <stdout>" |
| SPJ 编译失败 | (N/A — judge.sh compile_spj 返非 0) | `se` | `se` | error_message = SPJ compile stderr（截 4KB） |
| 题未挂 SPJ（兜底） | (N/A — compare_special 返 1) | `wa` | `wa` | case_results[i].info = "no special judge configured" |

### 3.2 judge.sh 判定分支时序

```
task.json 解析
   ↓
逐 case 解析时探测 SPECIAL_JUDGE_ENABLED=true（如任一 case 是 special）
   ↓
compile_spj <src> <bin>            ← 一次性，10s 超时
   ├─ rc=0 + bin 可执行 →  export LITECODE_SPJ_BIN=<bin>
   └─ rc!=0            →  export LITECODE_SPJ_BIN="" + 后续每个 special case 翻 SE
   ↓
逐 case 运行（time_limit_s+1 内层 timeout）
   ↓
case 走 judge.sh step 3 "case ${JT}" 分支
   ├─ exact → compare_exact
   ├─ ignore_trailing → compare_ignore_trailing
   ├─ float_eps → compare_float_eps
   └─ special →
        ├─ SPJ_BIN 空 + compile_rc!=0 → case=se, FINAL=se, ERROR_MESSAGE=<stderr>
        ├─ SPJ_BIN 空 + 题未挂 SPJ    → case=wa, FINAL=wa
        └─ SPJ_BIN 非空 → compare_special_with → 按 rc 翻 ac/wa/se
```

---

## §4 启用步骤

### 4.1 admin 通过 UI 上传（v1.3.2 前端 UI 已规划；当前 CLI 直跑）

```bash
# 1. 登录 admin 拿 token
TOKEN=$(curl -s -X POST http://localhost:8080/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin123!"}' | jq -r .data.access_token)

# 2. 先把题创建好（judge_type=special 必须出现在 samples / test_cases）
curl -X POST http://localhost:8080/api/v1/admin/problems \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d @/path/to/problem.json

# 3. 上传 SPJ 源码（≤ 256KB；超 256KB → 400 INVALID_INPUT）
SPJ_SOURCE='// your SPJ here ...'
curl -X PUT "http://localhost:8080/api/v1/admin/problems/${SLUG}/special-judge" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d "$(jq -n --arg s "${SPJ_SOURCE}" '{source:$s,language:"cpp"}')"

# 4. 验证：GET 应返 exists=true
curl -X GET "http://localhost:8080/api/v1/admin/problems/${SLUG}/special-judge" \
  -H "Authorization: Bearer $TOKEN" | jq .

# 5. 公共 detail 应有 has_special_judge=true
curl -X GET "http://localhost:8080/api/v1/problems/${SLUG}" | jq .data.has_special_judge
# 期望: true
```

### 4.2 批量导入路径（JSON 含 `special_judge_source`）

```jsonc
{
  "slug": "floating-point-sum",
  "title": "浮点求和（容差 1e-4）",
  "samples": [...],
  "test_cases": [{ "judge_type": "special", "expected_output": "..." }],
  "special_judge_source": "// SPJ 源码字符串...",
  "special_judge_language": "cpp"
}
```

`admin_bulk_import_routes.h::detail::process_one_file` 已识别这两个字段并写入 V010 表
（`tests/unit/test_admin_special_judge.cpp::UpsertIdempotentSameContent` 验证）。

### 4.3 端到端真栈验证（手工）

```bash
# 提交一段应被判 AC 的代码，看 case_results[0].info 是否含 SPJ stdout
curl -X POST http://localhost:8080/api/v1/submissions \
  -H "Authorization: Bearer $USER_TOKEN" \
  -H 'Content-Type: application/json' \
  -d "$(jq -n --argjson pid ${PID} --arg code '#include <iostream>
int main(){std::cout<<"hello\n";return 0;}' \
    '{problem_id:$pid,language:"cpp",code:$code}')"

# 轮询 /api/v1/submissions/:id，期望 status=ac + case_results[0].status=ac
# + case_results[0].info 为 SPJ stdout 第一行（截 2KB）
```

---

## §5 安全模型

### 5.1 SPJ 信任级别

| 层级 | 信任级别 | 防御手段 |
|------|---------|----------|
| 用户提交的 solution | 不信任（恶意代码） | `--read-only` FS / `--network=none` / `--cpus=1` / `--memory=256m` / `--pids-limit=50` / `--security-opt=no-new-privileges` / `USER judgeuser` / compile-time FORTIFY/RELRO |
| admin 上传的 SPJ | 半信任（管理员失误） | 与 solution **完全相同**的 secure compile flags（见 `judge/lib/spj.sh:66-76`）+ 同一容器 / 同一 cgroup / 同一 timebox（`time_limit_s + 1`） |
| SPJ 调用方（judge.sh） | 完全信任（系统代码） | — |

> **Why same jail for SPJ？** admin 账号可能被盗（用户密码爆破 / 内部人员
> 离职 / 密码重用）。SPJ 虽被半信任，但成本上把 SPJ 当成"用户级恶意代码"防御
> 几乎是零（复用同一 Dockerfile / 同一 secure flags），收益却很大 —— 一次
> SPJ 提权就能跑 docker exec / mount host / 改 judge 镜像。

### 5.2 隔离细节

| 维度 | 实现 |
|------|------|
| CPU time | Docker `--cpus=1` + 容器内 `timeout ${time_limit_s+1}` |
| Compile timeout | 容器内 `timeout 10s g++ ...`（SPJ 与 user submission 同） |
| Memory | Docker `--memory`（继承 task.json 的 memory_limit_mb）+ cgroup v2 memory.peak 读取 |
| Network | Docker `--network=none`（SPJ 想访问外网 / DNS = RE） |
| Filesystem | Docker `--read-only` + tmpfs `/tmp:size=64m,mode=1777` |
| Process count | Docker `--pids-limit=50` |
| Privilege | Docker `--security-opt=no-new-privileges` + `USER judgeuser (UID 1000)` |
| Compile hardening | `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,-z,now -Wl,-z,relro` |

### 5.3 SPJ timebox 与 memory 复用

SPJ 进程**继承** task.json 的 time_limit_ms 与 memory_limit_mb，不另设超时 /
内存。这是设计上的一致选择：管理员写 SPJ 时应该把 SPJ 的开销视为"题目的
额外成本"——例如某题判 100 个点，每点用户 solution 跑 1s，SPJ 跑 0.5s，
总时长应小于 `time_limit_ms × 100 + 50s`。如果 SPJ 实际耗时远超 time_limit，
请管理员用 v1.3.3 dry-run 端点（在保存前模拟）调优，或直接放宽题目 time_limit。

---

## §6 故障排查 5 类

### 6.1 SPJ 编译失败 → submission 整体 SE

**症状**：`/submissions/:id` 返 `status=se` + `error_message` 含
`"special judge source failed to compile"` 或 g++ stderr 文本。

**原因**：admin 上传的 C++ 源码本身有语法错 / 头文件缺失 / API 用错。

**诊断**：
```bash
# 1. 直接 GET 看 SPJ 源码是否就是你上传的版本（可能 admin 误粘错）
curl -X GET "http://localhost:8080/api/v1/admin/problems/${SLUG}/special-judge" \
  -H "Authorization: Bearer $ADMIN_TOKEN" | jq .data.source

# 2. 手动跑一遍 compile_spj 看具体 stderr
docker run --rm -i \
  --network=none --read-only --cpus=1 --memory=256m --pids-limit=50 \
  --security-opt=no-new-privileges \
  --tmpfs /tmp:size=64m,mode=1777 \
  litecode-judge:test /usr/local/bin/judge.sh < /tmp/spj-debug-task.json
# /tmp/spj-debug-task.json 里塞 special_judge_source + 一个 simple test case
```

**修复**：admin 修复 SPJ 源码 → 重新 PUT 一次（幂等 upsert，覆盖旧版本）。

### 6.2 SPJ 死循环 → submission 整体 SE（SPJ 超时 / kill-after）

**症状**：`status=se` + `error_message` 含 `"special judge crashed (exit=124)"`
或 `"exit=137"`。

**原因**：SPJ 内部写了 `while(true)` / 死锁 / 系统调用阻塞（如 open 没
返回值的情况下 read 阻塞）。

**诊断**：
- exit=124 → 内层 timeout 触发 SIGTERM，SPJ 没响应
- exit=137 → SIGKILL（kill-after 兜底，124 后 2s 内没退）
- 其他 exit → SPJ 主动 crash / 抛异常

**修复**：在 SPJ 内做"软超时"（每次循环 `clock()`，超过阈值主动 return 2），
或拆解为多步迭代。

### 6.3 SPJ 误判 AC（错解被判 AC）

**症状**：status=ac，但提交的 solution 实际上是错的（肉眼 / 手工验证）。

**原因**：SPJ 自身 bug —— 例如浮点比较写错符号 / 漏检一种边界情况 / 期望
输出解析时只读了第一行却忽略多行 / 误把 argv[3] 当 expected。

**诊断**：
```bash
# 手工调一遍 SPJ，把 bad 提交的实际输出塞到 argv[3]
docker run --rm -i \
  --network=none --read-only \
  litecode-judge:test bash -c '
    g++ -O2 -std=c++17 -o /tmp/spj /tmp/spj_source.cpp
    echo "expected:" > /tmp/expected.txt
    echo "<正确输出>" >> /tmp/expected.txt
    echo "actual:" > /tmp/actual.txt
    echo "<错解输出>" >> /tmp/actual.txt
    /tmp/spj /dev/null /tmp/expected.txt /tmp/actual.txt
    echo "exit=$?"
  '
```

**修复**：admin 修复 SPJ，回归再跑 e2e A48c 端到端。

### 6.4 submission 整体 SE 但 SPJ 正常

**症状**：`status=se` + case_results[i].status=se + info 含 SPJ rc，但
SPJ 单独跑没问题。

**原因**：通常是用户 solution 的运行失败被 judge.sh 错算成 SPJ 错。
看 error_message 是不是 `"judge script exceeded hard timeout"` /
`"container died"`。

**诊断**：
- 多次提交同样的 SPJ 但不同 solution：若都 SE → SPJ 自身（回到 §6.2）
- 单次提交 SE、换 submission 又 AC → 那次 submission 的 solution 异常

**修复**：换一份简单 solution 重跑（如本题正解）。

### 6.5 删除 SPJ 后兜底 → 提交全 WA

**症状**：DELETE 后再提交正解，status=wa + case_results[i].info
= `"no special judge configured"`。

**原因**：admin 误删 SPJ（admin UI / 批量导入 / SQL 直接 `DELETE`）。

**修复**：admin 重新 PUT SPJ；如果 SPJ 源码丢了，GET 仍能返最后一次保存
的内容（除非 row 真删了）；彻底丢了需要从 git history / 内部 wiki 恢复
或重写。

---

## §7 dry-run 端点（v1.3.3 计划）

v1.3.3 计划增加 `POST /api/v1/admin/problems/:slug/special-judge/test`：
admin 在 PUT SPJ 之前用一组 mock input/expected/actual 跑一遍当前 SPJ，
看返回 AC/WA/SE + case_results.info，避免"上线后才发现 SPJ 写错"。

v1.3.1 阶段该端点未实现；admin UI 留 placeholder 灰按钮（commit 2 v1.3.2
会处理 UI 占位）。

---

## §8 与其他组件关系

| 组件 | 关系 | 接口 |
|------|------|------|
| `src/judge/judge.sh` | 调 `compile_spj` + `compare_special_with` | judge.sh step 3 case special |
| `judge/lib/spj.sh` | SPJ 三件套 lib | 被 judge.sh source |
| `judge/lib/compare.sh` | `compare_special`（无 SPJ 兜底 WA） | 被 judge.sh 间接走（v1.2.52 行为保留） |
| `src/db/special_judge_repo.h` | V010 表 CRUD + exists_for_problem | submission_routes 装载 + admin_problem_routes 三端点 |
| `src/routes/submission_routes.h` | 装载 task.json `special_judge_source` / `special_judge_language` 字段 | line 614-647 |
| `src/routes/admin_problem_routes.h` | 3 admin SPJ 端点（PUT/GET/DELETE） | line 1131-1534（v1.3.1 新加） |
| `src/routes/problem_routes.h` | 公共 detail 透 `has_special_judge` + samples[].judge_type | line 631-736（v1.3.1 改） |
| `src/db/audit_log_repo.h` | 写 `problem.spj_upsert` / `problem.spj_remove` 两条 | line 472-473 |
| `judge/README.md` | 协议细节（参数顺序 / 退出码 / 模板） | 本文件 §3 / §5.1 配合 |
| `problems/README.md` | 题目 JSON schema + bulk-import 路径 | 本文件 §4.2 配合 |

---

## §9 版本演进

| 版本 | 状态 | 内容 |
|------|------|------|
| v1.2.18 | 已落地 | V010 schema + special_judge_repo（**未挂 admin / judge 端**） |
| v1.2.52 | 已落地 | compare.sh 改成"无 SPJ → WA"（operator 可见性） |
| v1.3.1 | ✅ 本 commit | admin 3 端点 + judge.sh 接线 + 测试 + 自检 + e2e + docs + SPEC 同步 |
| v1.3.2 | 计划 | 前端 admin SPJ 编辑 UI（problem-edit.html 动态块）+ problem.html 详情页 SPJ 徽章 + api.js 3 shim + CSS 样式；scripts/lint.sh + scripts/e2e_acceptance.sh A49 静态断言 |
| v1.3.3 | 计划 | `POST /api/v1/admin/problems/:slug/special-judge/test` dry-run 端点 + admin UI"dry-run"按钮接上；admin 256KB cap 抬到 1MB（与 repo ceiling 拉近） |
| v1.4 | 远期 | Interactive Judge（problem.interactive + judge_server 双进程 + mkfifo 编排）；SPJ 多版本（problem_special_judges.version_no）；SPJ LRU 缓存（高频读优化） |