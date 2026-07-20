#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/load_test.sh
# -------------------------------------------------------------
# Phase 8 ☆ 压测报告 —— 5/10/20 人并发判题，验证 SPEC §12.2 性能口径：
#   • 判题端到端 P95 < 5s（简单题）
#   • 提交 API 立即返回 < 200ms
#   • 并发 10 人同时提交不阻塞（超出排队）
#
# 定位：在单测 / 集成测 / e2e_acceptance.sh 之上再压一层「并发压测」。
#       纯黑盒 HTTP 面（curl + jq），提交真实 C++ AC 代码并轮询到终态，
#       统计每个并发级的提交-API 延迟与判题端到端延迟的分位数，
#       并把结果再生进 docs/load-test-report.md。
#
# 依赖：bash + curl + jq + date（毫秒时钟）。
#   注：仓库 JS workflow 脚本禁用 Date.now/Math.random；那是 workflow 约束。
#       bash 压测必须有墙钟来量化延迟，此处用 `date +%s.%N` 合法且必要。
#
# 环境变量（均可覆盖）：
#   BASE_URL             默认 http://localhost:8080
#   ADMIN_USER/ADMIN_PASS 默认 admin / admin123!
#   CONCURRENCY_LEVELS   默认 "5 10 20"（对齐 SPEC 措辞）
#   P95_THRESHOLD_MS     默认 5000（判题 e2e P95 上限，SPEC §12.2）
#   SUBMIT_API_MAX_MS    默认 200（提交 API p95 上限，SPEC §12.2）
#   JUDGE_POLL_TIMEOUT_S 默认 90（单提交轮询到终态最长秒数）
#   WARMUP               默认 1（正式压测前先提交 1 次预热编译缓存 / warm pool）
#   LOAD_STRICT          默认 0；置 1 时「缺栈导致的 SKIP」升级为 FAIL（CI 强约束）
#   REPORT_OUT           默认 ../docs/load-test-report.md（跑完再生结果段）
#
# 用法：
#   bash scripts/load_test.sh                          # 默认宽松
#   CONCURRENCY_LEVELS="5 10 20 50" bash scripts/load_test.sh
#   LOAD_STRICT=1 bash scripts/load_test.sh            # CI 强约束
#
# 退出码：
#   0  —— 无真实断言失败（宽松模式下缺栈的级记 SKIP 不算失败）
#   1  —— 至少一个 FAIL（含 STRICT 模式下因栈缺失被升级的 skip）
# =============================================================
# shellcheck disable=SC2317  # 部分 helper 在某些分支下不被调用，属正常
set -uo pipefail

# ───────────────────────── 路径解析 ─────────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

# ───────────────────────── 配置 ─────────────────────────
BASE_URL="${BASE_URL:-http://localhost:8080}"
BASE_URL="${BASE_URL%/}"
API="${BASE_URL}/api/v1"
ADMIN_USER="${ADMIN_USER:-admin}"
ADMIN_PASS="${ADMIN_PASS:-admin123!}"
CONCURRENCY_LEVELS="${CONCURRENCY_LEVELS:-5 10 20}"
P95_THRESHOLD_MS="${P95_THRESHOLD_MS:-5000}"
SUBMIT_API_MAX_MS="${SUBMIT_API_MAX_MS:-200}"
JUDGE_POLL_TIMEOUT_S="${JUDGE_POLL_TIMEOUT_S:-90}"
WARMUP="${WARMUP:-1}"
LOAD_STRICT="${LOAD_STRICT:-0}"
PROBLEMS_DIR="${PROBLEMS_DIR:-${ROOT}/problems}"
REPORT_OUT="${REPORT_OUT:-${ROOT}/docs/load-test-report.md}"

# ───────────────────────── 计数器 ─────────────────────────
PASS=0; FAIL=0; SKIP=0
ok()   { echo "    ok   - $*"; PASS=$((PASS+1)); }
fail() { echo "    FAIL - $*"; FAIL=$((FAIL+1)); }
skip() { echo "    skip - $*"; SKIP=$((SKIP+1)); }
kase() { echo; echo "── $1  $2"; }

# ───────────────────────── 依赖：jq ─────────────────────────
if ! command -v jq >/dev/null 2>&1; then
    for cand in \
        "/c/Users/${USERNAME:-$USER}/AppData/Local/Microsoft/WinGet/Packages/jqlang.jq_Microsoft.Winget.Source_8wekyb3d8bbwe" \
        "/c/Program Files/jq" ; do
        if [ -x "${cand}/jq.exe" ]; then export PATH="${cand}:${PATH}"; break; fi
    done
fi
for tool in curl jq date sort awk; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "missing required tool: ${tool}" >&2; exit 2; }
done

# ───────────────────────── 临时目录 ─────────────────────────
TMPD="$(mktemp -d -t lc-load-XXXXXX)"
RESP_BODY="${TMPD}/body"
RESP_HDR="${TMPD}/hdr"
cleanup() { rm -rf "${TMPD}"; }
trap cleanup EXIT
HTTP_CODE=""

UNIQ_CTR=0
rnd() { UNIQ_CTR=$((UNIQ_CTR+1)); echo "${RANDOM}_${UNIQ_CTR}_$$"; }

# ───────────────────────── HTTP helper（主线程用，非并发） ─────────────────────────
# 注：并发 worker 不用此 helper（它写共享的 RESP_BODY，会互相踩），worker 自带局部临时文件。
api() {
    local method="$1" path="$2"; shift 2
    local token="" data="" form=0
    local -a extra=()
    while [ $# -gt 0 ]; do
        case "$1" in
            -t) token="$2"; shift 2;;
            -d) data="$2"; shift 2;;
            -F) extra+=(-F "$2"); form=1; shift 2;;
            *)  extra+=("$1"); shift;;
        esac
    done
    local url="${API}${path}"
    local -a args=(-sS -X "${method}" -o "${RESP_BODY}" -D "${RESP_HDR}" \
                   -w '%{http_code}' --max-time 30)
    [ -n "${token}" ] && args+=(-H "Authorization: Bearer ${token}")
    if [ "${form}" -eq 0 ] && [ -n "${data}" ]; then
        args+=(-H "Content-Type: application/json" --data "${data}")
    fi
    [ "${#extra[@]}" -gt 0 ] && args+=("${extra[@]}")
    HTTP_CODE="$(curl "${args[@]}" "${url}" 2>/dev/null)" || HTTP_CODE="000"
    [ -n "${HTTP_CODE}" ] || HTTP_CODE="000"
}
jqb() { jq -r "$1" "${RESP_BODY}" 2>/dev/null; }

# ───────────────────────── 分位数（取序位法，小样本稳健） ─────────────────────────
# percentile FILE P(1-100)  —— FILE 每行一个整数；空文件返回 0
percentile() {
    local f="$1" p="$2"
    [ -s "${f}" ] || { echo 0; return; }
    sort -n "${f}" | awk -v p="${p}" '
        { a[NR]=$1 }
        END {
            n=NR; if (n==0){ print 0; exit }
            idx=int((p/100.0)*n + 0.999999);   # ceil
            if (idx<1) idx=1; if (idx>n) idx=n;
            print a[idx];
        }'
}
minv() { [ -s "$1" ] && sort -n "$1" | head -1 || echo 0; }
maxv() { [ -s "$1" ] && sort -n "$1" | tail -1 || echo 0; }

# ═════════════════════════ 能力探测 ═════════════════════════
echo "=== LiteCode-CPP 并发判题压测（5/10/20） ==="
echo "BASE_URL=${BASE_URL}  levels=[${CONCURRENCY_LEVELS}]  P95<${P95_THRESHOLD_MS}ms  submitAPI<${SUBMIT_API_MAX_MS}ms  STRICT=${LOAD_STRICT}"

SERVER_UP=0; JUDGE_UP=0; WARM_POOL="n/a"
api GET /health
if [ "${HTTP_CODE}" = "200" ] || [ "${HTTP_CODE}" = "503" ]; then
    SERVER_UP=1
    [ "$(jqb '.docker')" = "ok" ] && JUDGE_UP=1
    WARM_POOL="$(jqb '.warm_pool')"
    [ -z "${WARM_POOL}" ] && WARM_POOL="n/a"
fi
GIT_REV="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
RUN_TS="$(date '+%Y-%m-%d %H:%M:%S %z' 2>/dev/null || echo unknown)"
echo "capabilities: SERVER_UP=${SERVER_UP} JUDGE_UP=${JUDGE_UP} warm_pool=${WARM_POOL} rev=${GIT_REV}"

# 能力守卫：缺能力时按 STRICT 记 skip 或 fail，返回非 0
need() {
    local capval="$1" label="$2"
    if [ "${capval}" = "1" ]; then return 0; fi
    if [ "${LOAD_STRICT}" = "1" ]; then fail "${label}（STRICT：所需能力缺失）"
    else skip "${label}（所需能力缺失）"; fi
    return 1
}

# ═════════════════════════ Provision ═════════════════════════
USER_TOK=""; USER_NAME=""; ADMIN_TOK=""; PID=""
JUDGE_PROBLEM_SLUG="two-sum"

provision() {
    [ "${SERVER_UP}" = "1" ] || return 0
    USER_NAME="lc_load_$(rnd)"
    local pw="Passw0rd_load"
    local body; body="$(jq -n --arg u "${USER_NAME}" --arg p "${pw}" '{username:$u,password:$p}')"
    api POST /auth/register -d "${body}"
    [ "${HTTP_CODE}" = "201" ] && USER_TOK="$(jqb '.data.access_token')"

    body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" '{username:$u,password:$p}')"
    api POST /auth/login -d "${body}"
    [ "${HTTP_CODE}" = "200" ] && ADMIN_TOK="$(jqb '.data.access_token')"

    if [ -n "${ADMIN_TOK}" ] && [ -f "${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json" ]; then
        api POST "/admin/problems/import?on_duplicate=skip" -t "${ADMIN_TOK}" \
            -F "files=@${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json;type=application/json"
    fi
    api GET "/problems/${JUDGE_PROBLEM_SLUG}"
    [ "${HTTP_CODE}" = "200" ] && PID="$(jqb '.data.id')"
}

# ───────────────────────── 判题 payload：two-sum AC 解 ─────────────────────────
# 照搬 scripts/demo_judge_three_states.py 的 CODE_AC（unordered_map O(n)）。
read -r -d '' CODE_AC <<'CPP' || true
#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    vector<int> a;
    int x; int target = 0;
    while (cin >> x) { a.push_back(x); }
    if (a.empty()) return 1;
    target = a.back();
    a.pop_back();
    int n = (int)a.size();
    unordered_map<int,int> seen;
    for (int i = 0; i < n; i++) {
        int need = target - a[i];
        auto it = seen.find(need);
        if (it != seen.end()) { cout << it->second << " " << i << "\n"; return 0; }
        seen[a[i]] = i;
    }
    return 0;
}
CPP

# ───────────────────────── 并发 worker（自带局部临时文件，不踩共享态） ─────────────────────────
# run_worker IDX OUTDIR  → 写 OUTDIR/w_IDX.csv:  submit_http,submit_ms,e2e_ms,final_status
run_worker() {
    local idx="$1" out="$2"
    local body_f="${out}/w_${idx}.body"
    local sub_json; sub_json="$(jq -n --argjson pid "${PID:-0}" --arg code "${CODE_AC}" \
        '{problem_id:$pid,language:"cpp",code:$code}')"
    local resp scode stime
    resp="$(curl -sS -X POST "${API}/submissions" \
        -H "Authorization: Bearer ${USER_TOK}" -H "Content-Type: application/json" \
        --data "${sub_json}" -o "${body_f}" -w '%{http_code} %{time_total}' \
        --max-time 30 2>/dev/null)" || resp="000 0"
    scode="${resp%% *}"; stime="${resp##* }"
    local submit_ms; submit_ms="$(awk -v t="${stime}" 'BEGIN{printf "%d", t*1000}')"
    if [ "${scode}" != "201" ]; then
        echo "${scode},${submit_ms},0,submit_fail" > "${out}/w_${idx}.csv"; return
    fi
    local sid; sid="$(jq -r '.data.submission_id' "${body_f}" 2>/dev/null)"
    local start end e2e_ms status="pending" t0
    start="$(date +%s.%N)"; t0=${SECONDS}
    while :; do
        curl -sS "${API}/submissions/${sid}" -H "Authorization: Bearer ${USER_TOK}" \
            -o "${body_f}" --max-time 30 >/dev/null 2>&1
        status="$(jq -r '.data.status' "${body_f}" 2>/dev/null)"
        case "${status}" in ac|wa|re|tle|mle|ole|pe|ce|se) break;; esac
        if [ $((SECONDS - t0)) -ge "${JUDGE_POLL_TIMEOUT_S}" ]; then status="timeout"; break; fi
        sleep 1
    done
    end="$(date +%s.%N)"
    e2e_ms="$(awk -v s="${start}" -v e="${end}" 'BEGIN{printf "%d",(e-s)*1000}')"
    echo "${scode},${submit_ms},${e2e_ms},${status}" > "${out}/w_${idx}.csv"
}

# ───────────────────────── 每并发级：跑 + 聚合 + 断言 ─────────────────────────
REPORT_ROWS="${TMPD}/report_rows.md"   # 每级一行 markdown
: > "${REPORT_ROWS}"
RAN_ANY=0

run_level() {  # N
    local n="$1"
    local ldir="${TMPD}/lvl_${n}"; mkdir -p "${ldir}"
    kase "L${n}" "并发判题 ${n} 路"
    local i bstart bend
    bstart="$(date +%s.%N)"
    for i in $(seq 1 "${n}"); do run_worker "${i}" "${ldir}" & done
    wait
    bend="$(date +%s.%N)"

    cat "${ldir}"/w_*.csv > "${ldir}/all.csv" 2>/dev/null
    awk -F, '$1==201{print $2}' "${ldir}/all.csv" > "${ldir}/submit_ms.txt"
    awk -F, '($3+0)>0{print $3}' "${ldir}/all.csv" > "${ldir}/e2e_ms.txt"
    local success errs others
    success="$(awk -F, '$4=="ac"{c++}END{print c+0}' "${ldir}/all.csv")"
    errs="$(awk -F, '$4=="se"||$4=="timeout"||$4=="submit_fail"{c++}END{print c+0}' "${ldir}/all.csv")"
    others="$(awk -F, '$4!="ac"&&$4!="se"&&$4!="timeout"&&$4!="submit_fail"{c++}END{print c+0}' "${ldir}/all.csv")"

    local sub_p95 e2e_p50 e2e_p95 e2e_p99 e2e_min e2e_max
    sub_p95="$(percentile "${ldir}/submit_ms.txt" 95)"
    e2e_p50="$(percentile "${ldir}/e2e_ms.txt" 50)"
    e2e_p95="$(percentile "${ldir}/e2e_ms.txt" 95)"
    e2e_p99="$(percentile "${ldir}/e2e_ms.txt" 99)"
    e2e_min="$(minv "${ldir}/e2e_ms.txt")"
    e2e_max="$(maxv "${ldir}/e2e_ms.txt")"
    local wall tput
    wall="$(awk -v s="${bstart}" -v e="${bend}" 'BEGIN{printf "%.2f",(e-s)}')"
    tput="$(awk -v n="${n}" -v w="${wall}" 'BEGIN{ if(w+0>0) printf "%.2f", n/w; else print "0.00" }')"

    echo "    并发=${n}  提交成功(AC)=${success}  非AC=${others}  错误(se/timeout/submit_fail)=${errs}"
    echo "    提交API p95=${sub_p95}ms   判题e2e: min=${e2e_min} p50=${e2e_p50} p95=${e2e_p95} p99=${e2e_p99} max=${e2e_max} (ms)"
    echo "    批墙钟=${wall}s   吞吐=${tput} req/s"

    # 断言
    local verdict="PASS"
    if [ "${e2e_p95}" -lt "${P95_THRESHOLD_MS}" ] && [ "${e2e_p95}" -gt 0 ]; then
        ok "L${n} 判题 e2e P95=${e2e_p95}ms < ${P95_THRESHOLD_MS}ms"
    else
        fail "L${n} 判题 e2e P95=${e2e_p95}ms 未达标（阈值 ${P95_THRESHOLD_MS}ms）"; verdict="FAIL"
    fi
    if [ "${sub_p95}" -lt "${SUBMIT_API_MAX_MS}" ] && [ "${sub_p95}" -ge 0 ]; then
        ok "L${n} 提交 API p95=${sub_p95}ms < ${SUBMIT_API_MAX_MS}ms（立即返回）"
    else
        fail "L${n} 提交 API p95=${sub_p95}ms 超过 ${SUBMIT_API_MAX_MS}ms"; verdict="FAIL"
    fi
    if [ "${errs}" -eq 0 ]; then
        ok "L${n} 无系统错误（se/timeout/submit_fail=0）"
    else
        fail "L${n} 出现 ${errs} 个错误（se/timeout/submit_fail）"; verdict="FAIL"
    fi

    # markdown 行
    printf '| %d | %d | %d | %d | %s | %s | %s | %s | %s | %s | %s |\n' \
        "${n}" "${success}" "${errs}" "${sub_p95}" "${e2e_min}" "${e2e_p50}" \
        "${e2e_p95}" "${e2e_p99}" "${e2e_max}" "${tput}" "${verdict}" >> "${REPORT_ROWS}"
    RAN_ANY=1
}

# ───────────────────────── 报告再生 ─────────────────────────
write_report() {
    local rows; rows="$(cat "${REPORT_ROWS}")"
    mkdir -p "$(dirname "${REPORT_OUT}")"
    cat > "${REPORT_OUT}" <<EOF
# LiteCode-CPP 压测报告（并发判题）

> Phase 8 ☆「压测报告」。本文件的 **结果段由 \`scripts/load_test.sh\` 运行时再生**，
> 手改结果段无意义；方法论 / 口径 / 运行方式为静态说明。

## 1. 目的与验收口径（SPEC §12.2）

| 指标 | 标准 |
|------|------|
| 提交 API 响应 | < 200ms（立即返回 submission_id） |
| 判题响应（P95） | < 5s（简单题 < 3s） |
| 并发判题 | 支持 10 人同时提交不阻塞，超出排队 |

对应 SPEC §11 Phase 8：\`☆ 压测报告（5/10/20 人并发判题，验证 P95 < 5s）\`。

## 2. 方法论

- **被测题目**：\`two-sum\`（easy，time_limit 1000ms），provision 阶段幂等 bulk-import。
- **提交代码**：two-sum 的 AC 解（\`unordered_map\` O(n)，见 \`scripts/demo_judge_three_states.py\` CODE_AC）。
- **并发级**：\`5 / 10 / 20\`（\`CONCURRENCY_LEVELS\` 可覆盖）。每级起 N 个后台 worker 同时提交。
- **预热**：正式压测前先单发 1 次（\`WARMUP=1\`），抹平编译缓存 / warm pool 冷启对首批的影响。
- **单次采样**：worker 提交后轮询 \`GET /submissions/:id\` 到终态（ac/wa/…/se）或超时。
  - \`submit_api_ms\`：curl \`%{time_total}\`（提交请求本身的 RTT，对应「立即返回」口径）。
  - \`e2e_ms\`：提交 → 终态的墙钟（\`date +%s.%N\` 差），对应「判题响应 P95」口径。
- **分位数**：对样本 \`sort -n\` 后取 \`ceil(p/100 · n)\` 序位（小样本稳健，无浮点插值）。
- **判定**：每级三条断言 —— 判题 e2e P95 < ${P95_THRESHOLD_MS}ms、提交 API p95 < ${SUBMIT_API_MAX_MS}ms、
  错误数（se/timeout/submit_fail）= 0。任一不满足该级判 FAIL。

## 3. 如何运行

\`\`\`bash
# 需要 live stack（web + judge docker）在线
bash scripts/load_test.sh

# 自定义并发级 / 阈值
CONCURRENCY_LEVELS="5 10 20 50" P95_THRESHOLD_MS=5000 bash scripts/load_test.sh

# CI 强约束：缺栈直接失败（不再宽松 skip）
LOAD_STRICT=1 bash scripts/load_test.sh
\`\`\`

跑完本文件的「运行环境」与「结果」两段会被自动覆盖。

## 4. 运行环境（最近一次运行再生）

| 项 | 值 |
|----|----|
| 运行时间 | ${RUN_TS} |
| git 版本 | ${GIT_REV} |
| BASE_URL | ${BASE_URL} |
| warm_pool（/health） | ${WARM_POOL} |
| 并发级 | ${CONCURRENCY_LEVELS} |
| 判题 P95 阈值 | ${P95_THRESHOLD_MS} ms |
| 提交 API 阈值 | ${SUBMIT_API_MAX_MS} ms |

## 5. 结果（最近一次运行再生）

单位：延迟 ms，吞吐 req/s。判题 e2e 分位数只统计到达终态的样本。

| 并发 N | 提交成功(AC) | 错误 | 提交API p95 | e2e min | e2e p50 | e2e p95 | e2e p99 | e2e max | 吞吐 | 判定 |
|--------|------------|------|------------|---------|---------|---------|---------|---------|------|------|
${rows}

## 6. 解读与限制

- 判题 e2e 含**编译 + 容器启动 + 执行 + 回写**全链路；warm pool 命中率直接影响 P95。
- 首批提交若遇冷编译缓存 / 空 warm pool，尾部延迟会抬高——\`WARMUP=1\` 用于缓解。
- \`date +%s.%N\` 在 MSYS/Git-Bash 上纳秒精度依赖底层实现，毫秒级足够本口径。
- 并发超过 judge 并发容量时任务排队，e2e 尾延迟随之上升，属预期行为（验证「超出排队不阻塞」）。
EOF
    echo "report → ${REPORT_OUT}"
}

# ═════════════════════════ 主流程 ═════════════════════════
provision

if need "${JUDGE_UP}" "并发判题压测（需 web + judge docker 在线）"; then
    if [ -z "${USER_TOK}" ] || [ -z "${PID}" ]; then
        fail "provision 失败（USER_TOK / PID 缺失，HTTP=${HTTP_CODE}）——无法压测"
    else
        # 预热
        if [ "${WARMUP}" = "1" ]; then
            echo "── warmup  预热单次提交（不计入统计）"
            mkdir -p "${TMPD}/warm"
            run_worker 0 "${TMPD}/warm"
            echo "    warmup → $(cat "${TMPD}/warm/w_0.csv" 2>/dev/null)"
        fi
        for lvl in ${CONCURRENCY_LEVELS}; do
            run_level "${lvl}"
        done
        write_report
    fi
fi

# ═════════════════════════ 总结 ═════════════════════════
echo
echo "════════════════════════════════════════════"
echo "  Passed: ${PASS}   Failed: ${FAIL}   Skipped: ${SKIP}"
echo "════════════════════════════════════════════"
if [ "${FAIL}" -gt 0 ]; then
    echo "结果：FAIL（存在未达标并发级，或 STRICT 模式下能力缺失被升级）"
    exit 1
fi
if [ "${RAN_ANY}" = "0" ]; then
    echo "结果：SKIP（未执行任何并发级——栈缺失；报告未再生）"
else
    echo "结果：PASS（所有并发级达标）"
fi
exit 0
