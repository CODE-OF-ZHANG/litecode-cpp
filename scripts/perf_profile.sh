#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/perf_profile.sh
# -------------------------------------------------------------
# Phase 9 △ 性能 Profile（v1.2.74）—— 把 SPEC「perf / flamegraph
# 跑一次判题热路径」落地为可重复跑的脚本。
#
# 定位：在单测 / 集成测 / e2e_acceptance.sh / load_test.sh 之上
#       再加一层「定位瓶颈」工具。三段黑盒测量：
#
#   A. HTTP 面分阶段 timing 拆解（curl -w 模板的 6 个阶段：
#      dns/connect/ssl/ttfb/total/size_download），对 SPEC §12.2
#      的 5 个关键 endpoint 做 N 次采样取分位数，断言 SPEC 阈值
#   B. Prometheus histogram 采样（前后两次 scrape
#      litecode_judge_duration_seconds 算 _count/_sum 差值 + _bucket
#      反推 p50/p95/p99）—— 给「判题热路径」的总耗时分布
#   C. Linux perf + flamegraph（可选，需要 Linux docker web 容器
#      在线 + perf/FlameGraph 工具链）—— 抓 30s CPU 火焰图
#
# 三段以报告形式落地到 docs/performance-profile.md（运行时再生结果段，
# 方法论 / 口径 / 运行方式为静态说明，沿用 v1.2.65 load-test-report.md
# 模板）。能力缺一律走 SKIP 而非 FAIL（不同 load_test：profile 是
# 探索性工具，缺工具不应该 fail CI；但 STRICT 模式下缺前置升级）。
#
# 依赖（按需）：
#   - bash + curl + jq + date + awk + sort + bc（基础测量必备）
#   - docker（Linux 容器 profile 时必备）
#   - linux-tools-common / linux-tools-$(uname -r)（perf 命令）
#   - FlameGraph 仓库（stackcollapse-perf.pl + flamegraph.pl）
#
# 环境变量（均可覆盖）：
#   BASE_URL              默认 http://localhost:8080
#   ADMIN_USER/ADMIN_PASS 默认 admin / admin123!
#   PROFILE_SAMPLES       默认 20（HTTP 面每 endpoint 采样次数）
#   HEALTH_MAX_MS         默认 50（SPEC §12.2 健康检查阈值）
#   PROBLEMS_LIST_MAX_MS  默认 200（SPEC §12.2 题目列表阈值）
#   SUBMIT_MAX_MS         默认 200（SPEC §12.2 提交 API 阈值）
#   JUDGE_HISTORY_MAX_MS  默认 200（提交历史接口阈值，沿用列表阈值）
#   METRICS_MAX_MS        默认 200（/metrics 内部接口阈值，沿用列表）
#   PERF_CONTAINER        默认 litecode-web（Linux docker 容器名）
#   PERF_DURATION_S       默认 30（perf record 抓多少秒）
#   PERF_FREQ_HZ          默认 99（perf record -F 频率）
#   FLAMEGRAPH_DIR        默认 /opt/FlameGraph（系统级 FlG 路径）
#   PROFILE_STRICT        默认 0；置 1 时「缺前置导致的 SKIP」升级 FAIL
#   REPORT_OUT            默认 ../docs/performance-profile.md
#
# 用法：
#   bash scripts/perf_profile.sh                              # 默认
#   PROFILE_SAMPLES=50 bash scripts/perf_profile.sh
#   PROFILE_STRICT=1 bash scripts/perf_profile.sh             # CI 强约束
#
# 退出码：
#   0  —— 无真实断言失败（缺前置在宽松模式下仅 SKIP）
#   1  —— 至少一个 FAIL（含 STRICT 模式下缺前置被升级的 skip）
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

PROFILE_SAMPLES="${PROFILE_SAMPLES:-20}"
HEALTH_MAX_MS="${HEALTH_MAX_MS:-50}"
PROBLEMS_LIST_MAX_MS="${PROBLEMS_LIST_MAX_MS:-200}"
SUBMIT_MAX_MS="${SUBMIT_MAX_MS:-200}"
JUDGE_HISTORY_MAX_MS="${JUDGE_HISTORY_MAX_MS:-200}"
METRICS_MAX_MS="${METRICS_MAX_MS:-200}"

PERF_CONTAINER="${PERF_CONTAINER:-litecode-web}"
PERF_DURATION_S="${PERF_DURATION_S:-30}"
PERF_FREQ_HZ="${PERF_FREQ_HZ:-99}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/opt/FlameGraph}"

PROFILE_STRICT="${PROFILE_STRICT:-0}"
PROBLEMS_DIR="${PROBLEMS_DIR:-${ROOT}/problems}"
REPORT_OUT="${REPORT_OUT:-${ROOT}/docs/performance-profile.md}"

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
# bc 可选（计算 _count × bucket_bound 差值用）；缺则降级到 awk
HAVE_BC=0
command -v bc >/dev/null 2>&1 && HAVE_BC=1

# ───────────────────────── 临时目录 ─────────────────────────
TMPD="$(mktemp -d -t lc-perf-XXXXXX)"
RESP_BODY="${TMPD}/body"
RESP_HDR="${TMPD}/hdr"
cleanup() { rm -rf "${TMPD}"; }
trap cleanup EXIT
HTTP_CODE=""

UNIQ_CTR=0
rnd() { UNIQ_CTR=$((UNIQ_CTR+1)); echo "${RANDOM}_${UNIQ_CTR}_$$"; }

# ───────────────────────── HTTP helper（主线程用，非并发） ─────────────────────────
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

# ───────────────────────── 分位数（与 load_test.sh 同款；小样本稳健） ─────────────────────────
percentile() {
    local f="$1" p="$2"
    [ -s "${f}" ] || { echo 0; return; }
    sort -n "${f}" | awk -v p="${p}" '
        { a[NR]=$1 }
        END {
            n=NR; if (n==0){ print 0; exit }
            idx=int((p/100.0)*n + 0.999999);
            if (idx<1) idx=1; if (idx>n) idx=n;
            print a[idx];
        }'
}
minv() { [ -s "$1" ] && sort -n "$1" | head -1 || echo 0; }
maxv() { [ -s "$1" ] && sort -n "$1" | tail -1 || echo 0; }

# ═════════════════════════ 能力探测 ═════════════════════════
echo "=== LiteCode-CPP 性能 Profile（HTTP + Prometheus histogram + Linux perf） ==="
echo "BASE_URL=${BASE_URL}  samples=${PROFILE_SAMPLES}  STRICT=${PROFILE_STRICT}"

SERVER_UP=0; JUDGE_UP=0; WARM_POOL="n/a"
api GET /health
if [ "${HTTP_CODE}" = "200" ] || [ "${HTTP_CODE}" = "503" ]; then
    SERVER_UP=1
    [ "$(jqb '.docker')" = "ok" ] && JUDGE_UP=1
    WARM_POOL="$(jqb '.warm_pool')"
    [ -z "${WARM_POOL}" ] && WARM_POOL="n/a"
fi

PERF_AVAILABLE=0; FLAMEGRAPH_AVAILABLE=0
if command -v perf >/dev/null 2>&1; then
    PERF_AVAILABLE=1
fi
if [ -x "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" ] && [ -x "${FLAMEGRAPH_DIR}/flamegraph.pl" ]; then
    FLAMEGRAPH_AVAILABLE=1
fi
PERF_IN_CONTAINER=0
if command -v docker >/dev/null 2>&1; then
    if docker inspect "${PERF_CONTAINER}" >/dev/null 2>&1; then
        PERF_IN_CONTAINER=1
    fi
fi

GIT_REV="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
RUN_TS="$(date '+%Y-%m-%d %H:%M:%S %z' 2>/dev/null || echo unknown)"
echo "capabilities: SERVER_UP=${SERVER_UP} JUDGE_UP=${JUDGE_UP} warm_pool=${WARM_POOL} perf=${PERF_AVAILABLE} flamegraph=${FLAMEGRAPH_AVAILABLE} docker_container=${PERF_IN_CONTAINER} rev=${GIT_REV}"

# 能力守卫
need() {
    local capval="$1" label="$2"
    if [ "${capval}" = "1" ]; then return 0; fi
    if [ "${PROFILE_STRICT}" = "1" ]; then fail "${label}（STRICT：所需能力缺失）"
    else skip "${label}（所需能力缺失）"; fi
    return 1
}

# ═════════════════════════ Provision ═════════════════════════
USER_TOK=""; USER_NAME=""; ADMIN_TOK=""; PID=""
JUDGE_PROBLEM_SLUG="two-sum"

provision() {
    [ "${SERVER_UP}" = "1" ] || return 0
    USER_NAME="lc_perf_$(rnd)"
    local pw="Passw0rd_perf"
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

# ═════════════════════════ Phase A：HTTP 面分阶段 timing 拆解 ═════════════════════════
# curl -w 模板的 6 个阶段：
#   time_namelookup     DNS 解析
#   time_connect        TCP 三次握手
#   time_appconnect     TLS 握手（HTTPS 时；HTTP 时为 0）
#   time_pretransfer    准备发送（认证 + cookie 解析）
#   time_starttransfer  TTFB（首字节）
#   time_total          总耗时
REPORT_PHASE_A=""
HTTP_RAN=0

# endpoint -> 阈值 ms 的映射（SPEC §12.2）
# label|url|method|token?|max_ms
ENDPOINTS=(
    "health|/health|GET||${HEALTH_MAX_MS}"
    "problems_list|/problems?limit=20|GET||${PROBLEMS_LIST_MAX_MS}"
    "auth_login|/auth/login|POST||${SUBMIT_MAX_MS}"
    "metrics|/api/v1/metrics|GET||${METRICS_MAX_MS}"
)

run_phase_a() {
    kase "Phase A" "HTTP 面分阶段 timing 拆解（curl -w 6 阶段）"
    [ "${SERVER_UP}" = "1" ] || { skip "Phase A 全套（栈缺失）"; return 1; }

    # 表头：endpoint, p50_total, p95_total, max_total, dns_p95, connect_p95, ttfb_p95, samples, threshold, verdict
    local row
    local _path _method _tok _max _label
    for ep in "${ENDPOINTS[@]}"; do
        IFS='|' read -r _label _path _method _tok _max <<<"${ep}"
        local dir="${TMPD}/a_${_label}"
        mkdir -p "${dir}"
        # 对每个 endpoint 采样 N 次：每次只采集 total（+ 各阶段分位数用单独 write-out 模板）
        local i body data_arg=()
        for i in $(seq 1 "${PROFILE_SAMPLES}"); do
            # /auth/login 需要 body；其它 GET 不带
            if [ "${_method}" = "POST" ] && [ "${_label}" = "auth_login" ]; then
                # 用错误密码以走完整路径又不污染 login bucket（v1.2.63 A26b 思路）
                data_arg=(--data "$(jq -n --arg u "lc_perf_probe_$(rnd)" --arg p "wrong_pw_$$" '{username:$u,password:$p}')" \
                          -H 'Content-Type: application/json')
            else
                data_arg=()
            fi
            local -a args=(-sS -X "${_method}" -o /dev/null \
                           -w '%{time_namelookup} %{time_connect} %{time_appconnect} %{time_pretransfer} %{time_starttransfer} %{time_total}\n' \
                           --max-time 10)
            [ -n "${_tok}" ] && args+=(-H "Authorization: Bearer ${_tok}")
            [ "${#data_arg[@]}" -gt 0 ] && args+=("${data_arg[@]}")
            curl "${args[@]}" "${BASE_URL}${_path}" >> "${dir}/all.txt" 2>/dev/null || true
        done

        # 拆分 6 列到独立文件
        awk '{print $1*1000}' "${dir}/all.txt" > "${dir}/dns.txt"     2>/dev/null || true
        awk '{print $2*1000}' "${dir}/all.txt" > "${dir}/connect.txt" 2>/dev/null || true
        awk '{print $3*1000}' "${dir}/all.txt" > "${dir}/tls.txt"     2>/dev/null || true
        awk '{print $4*1000}' "${dir}/all.txt" > "${dir}/prexfr.txt"  2>/dev/null || true
        awk '{print $5*1000}' "${dir}/all.txt" > "${dir}/ttfb.txt"    2>/dev/null || true
        awk '{print $6*1000}' "${dir}/all.txt" > "${dir}/total.txt"   2>/dev/null || true

        local p50 p95 mx dns95 conn95 ttfb95 samples n_lines
        n_lines="$(wc -l < "${dir}/total.txt" 2>/dev/null || echo 0)"
        samples="${n_lines}"
        p50="$(percentile "${dir}/total.txt" 50)"
        p95="$(percentile "${dir}/total.txt" 95)"
        mx="$(maxv   "${dir}/total.txt")"
        dns95="$(percentile "${dir}/dns.txt" 95)"
        conn95="$(percentile "${dir}/connect.txt" 95)"
        ttfb95="$(percentile "${dir}/ttfb.txt" 95)"

        # 阈值断言：只看 total p95（其它阶段只观察，不阻断）
        local verdict="PASS"
        if [ "${p95}" -lt "${_max}" ] && [ "${p95}" -gt 0 ]; then
            ok "${_label} total p95=${p95}ms < ${_max}ms（SPEC §12.2）"
        else
            fail "${_label} total p95=${p95}ms 未达 SPEC §12.2 阈值 ${_max}ms"
            verdict="FAIL"
        fi
        echo "    ${_label}  p50=${p50}ms  p95=${p95}ms  max=${mx}ms  |  dns p95=${dns95}  connect p95=${conn95}  ttfb p95=${ttfb95}  (ms)  n=${samples}"

        printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
            "${_label}" "${p50}" "${p95}" "${mx}" "${dns95}" "${conn95}" "${ttfb95}" \
            "${samples}" "${_max}" "${verdict}" >> "${REPORT_PHASE_A}"
        HTTP_RAN=1
    done
}

# ═════════════════════════ Phase B：Prometheus histogram 采样 ═════════════════════════
# 抓 /api/v1/metrics 两次（间隔一段压测），对比 litecode_judge_duration_seconds
# 的 _count / _sum 增量，按 _bucket 反推 p50/p95/p99 占比。
# 输出落在 markdown 表里（不进 ctest）。
REPORT_PHASE_B=""
PROM_RAN=0

scrape_metrics() {
    local out="$1"
    curl -sS "${API}/metrics" -o "${out}" --max-time 10 2>/dev/null || true
}

parse_histogram() {
    # parse_histogram FILE -> 写 stdout 三行：
    #   _count N
    #   _sum S
    #   _bucket{le="X"} C ...
    awk '
        /^litecode_judge_duration_seconds_count/ { print "_count", $2; next }
        /^litecode_judge_duration_seconds_sum/   { print "_sum", $2;   next }
        /^litecode_judge_duration_seconds_bucket/ { print $0;          next }
    ' "$1"
}

histogram_pct() {
    # histogram_pct FILE P(1-100) —— 给 _bucket 序列，按"≤"累计，线性插值求 P 位置
    # 输入文件每行 `litecode_judge_duration_seconds_bucket{le="..."} count`
    # 返回 P 分位数（秒，字符串）
    awk -v pct="$1" '
        function unquote(s) {
            sub(/^[^"]*"/, "", s);
            sub(/"$/, "", s);
            return s;
        }
        /^litecode_judge_duration_seconds_bucket/ {
            le = unquote($0);
            cnt = $NF + 0;
            if (le == "+Inf") { inf = cnt; next; }
            bounds[++n] = le + 0;
            counts[n] = cnt;
        }
        END {
            if (n == 0) { print "0"; exit }
            total = inf; if (total == 0) total = counts[n];
            target = (pct / 100.0) * total;
            prev_bound = 0; prev_cnt = 0;
            for (i = 1; i <= n; i++) {
                if (counts[i] >= target) {
                    if (i == 1) { print bounds[i]; exit }
                    # 线性插值
                    span = bounds[i] - prev_bound;
                    diff = counts[i] - prev_cnt;
                    if (diff == 0) { print bounds[i]; exit }
                    frac = (target - prev_cnt) / diff;
                    printf "%.6f\n", prev_bound + frac * span;
                    exit
                }
                prev_bound = bounds[i]; prev_cnt = counts[i];
            }
            print bounds[n];
        }
    ' "$2"
}

run_phase_b() {
    kase "Phase B" "Prometheus histogram 采样（判题热路径）"
    [ "${SERVER_UP}" = "1" ] || { skip "Phase B 全套（栈缺失）"; return 1; }
    [ -n "${USER_TOK}" ] || { skip "Phase B（provision 失败：USER_TOK 缺失）"; return 1; }
    [ -n "${PID}" ]    || { skip "Phase B（provision 失败：PID 缺失）";    return 1; }

    local snap1="${TMPD}/b_snap1.txt" snap2="${TMPD}/b_snap2.txt"
    scrape_metrics "${snap1}"

    # 跑一轮 small 压测（5 个并发提交 two-sum AC），让 histogram 有样本
    local workers_dir="${TMPD}/b_workers"; mkdir -p "${workers_dir}"
    local i
    for i in $(seq 1 5); do
        (
            local bf="${workers_dir}/w_${i}.body"
            local body; body="$(jq -n --argjson pid "${PID:-0}" --arg code "${CODE_AC}" \
                '{problem_id:$pid,language:"cpp",code:$code}')"
            local resp scode
            resp="$(curl -sS -X POST "${API}/submissions" \
                -H "Authorization: Bearer ${USER_TOK}" -H "Content-Type: application/json" \
                --data "${body}" -o "${bf}" -w '%{http_code}' --max-time 30 2>/dev/null)" || resp="000"
            scode="${resp}"
            if [ "${scode}" = "201" ]; then
                local sid; sid="$(jq -r '.data.submission_id' "${bf}" 2>/dev/null)"
                local t0=${SECONDS}
                while :; do
                    curl -sS "${API}/submissions/${sid}" -H "Authorization: Bearer ${USER_TOK}" \
                        -o "${bf}" --max-time 30 >/dev/null 2>&1
                    local st; st="$(jq -r '.data.status' "${bf}" 2>/dev/null)"
                    case "${st}" in ac|wa|re|tle|mle|ole|pe|ce|se) break;; esac
                    [ $((SECONDS - t0)) -ge 60 ] && break
                    sleep 1
                done
            fi
        ) &
    done
    wait

    # 压测结束后再 scrape 一次（让 mark_finished 的 observe 都跑完）
    sleep 1
    scrape_metrics "${snap2}"

    local c1 c2 s1 s2
    c1="$(grep '^litecode_judge_duration_seconds_count' "${snap1}" | awk '{print $2}' 2>/dev/null || echo 0)"
    c2="$(grep '^litecode_judge_duration_seconds_count' "${snap2}" | awk '{print $2}' 2>/dev/null || echo 0)"
    s1="$(grep '^litecode_judge_duration_seconds_sum'   "${snap1}" | awk '{print $2}' 2>/dev/null || echo 0)"
    s2="$(grep '^litecode_judge_duration_seconds_sum'   "${snap2}" | awk '{print $2}' 2>/dev/null || echo 0)"
    local dc ds; dc=$((c2 - c1)); ds="$(awk -v a="${s2}" -v b="${s1}" 'BEGIN{printf "%.6f", a-b}')"
    local avg; avg="$(awk -v s="${ds}" -v n="${dc}" 'BEGIN{ if (n+0>0) printf "%.6f", s/n; else print "0" }')"

    local p50 p95 p99
    p50="$(histogram_pct 50 "${snap2}")"
    p95="$(histogram_pct 95 "${snap2}")"
    p99="$(histogram_pct 99 "${snap2}")"

    echo "    histogram 增量：Δ_count=${dc}  Δ_sum=${ds}s  avg=${avg}s/p95_baseline=${p95}s"
    if [ "${dc}" -gt 0 ]; then
        ok "Phase B 抓取到 ${dc} 个新增判题样本（histogram 活跃）"
        ok "Phase B p50=${p50}s / p95=${p95}s / p99=${p99}s（snapshot 累计分布）"
        PROM_RAN=1
        REPORT_PHASE_B="$(printf '| judge_duration_seconds | %s | %s | %s | %s | %s |' \
            "${dc}" "${avg}" "${p50}" "${p95}" "${p99}")"
    else
        fail "Phase B histogram 无新增样本（scrape 间隔内无判题？）—— 改判 P95 不可信"
    fi
}

# ═════════════════════════ Phase C：Linux perf + flamegraph（可选） ═════════════════════════
REPORT_PHASE_C=""
FLAME_OUT="${ROOT}/docs/perf-flamegraph.svg"
PERF_DATA="${TMPD}/perf.data"
PERF_SCRIPT="${TMPD}/perf.script"
PERF_RAN=0

run_phase_c() {
    kase "Phase C" "Linux perf + flamegraph（可选）"
    if [ "${PERF_IN_CONTAINER}" != "1" ]; then
        skip "Phase C perf（容器 ${PERF_CONTAINER} 不可达；本机非 Linux 容器内）"
        return 1
    fi
    if [ "${PERF_AVAILABLE}" != "1" ]; then
        # 容器外 perf 不一定在；尝试 docker exec 容器内 perf
        if ! docker exec "${PERF_CONTAINER}" sh -c 'command -v perf >/dev/null 2>&1' 2>/dev/null; then
            skip "Phase C perf（容器内无 perf 命令；apt install linux-tools-\$(uname -r)）"
            return 1
        fi
    fi
    if [ "${FLAMEGRAPH_AVAILABLE}" != "1" ]; then
        skip "Phase C flamegraph（FLAMEGRAPH_DIR=${FLAMEGRAPH_DIR} 无 stackcollapse-perf.pl / flamegraph.pl）"
        return 1
    fi

    # 1. 找 web 进程 PID（容器内 1 通常是 init 进程；具体进程看 ps）
    local pid
    pid="$(docker exec "${PERF_CONTAINER}" sh -c 'pgrep -n litecode_server || pgrep -n -f litecode_server || echo 1' 2>/dev/null | tr -d '\r\n')"
    [ -z "${pid}" ] && pid="1"
    echo "    perf target pid = ${pid}（容器 ${PERF_CONTAINER}）"

    # 2. perf record（容器内 -g 抓调用栈；持续 ${PERF_DURATION_S}s）
    if ! docker exec "${PERF_CONTAINER}" sh -c \
        "perf record -F ${PERF_FREQ_HZ} -p ${pid} -g -o /tmp/perf.data -- sleep ${PERF_DURATION_S}" \
        >/dev/null 2>&1; then
        fail "Phase C perf record 失败（容器内核不支持？检查 --privileged / kernel.perf_event_paranoid）"
        return 1
    fi

    # 3. 拉 perf.data 回宿主 + perf script 生成可折叠栈
    if ! docker cp "${PERF_CONTAINER}:/tmp/perf.data" "${PERF_DATA}" 2>/dev/null; then
        fail "Phase C docker cp perf.data 失败"
        return 1
    fi
    # perf script 必须在能访问 perf.data 的环境跑；宿主有 perf 就跑宿主
    if command -v perf >/dev/null 2>&1; then
        perf script -i "${PERF_DATA}" > "${PERF_SCRIPT}" 2>/dev/null \
            || docker exec "${PERF_CONTAINER}" perf script -i /tmp/perf.data > "${PERF_SCRIPT}" 2>/dev/null \
            || { fail "Phase C perf script 失败"; return 1; }
    else
        docker exec -i "${PERF_CONTAINER}" perf script -i /tmp/perf.data < /dev/null > "${PERF_SCRIPT}" 2>/dev/null \
            || { fail "Phase C perf script 失败"; return 1; }
    fi

    # 4. 折叠 + 火焰图
    if ! "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" "${PERF_SCRIPT}" 2>/dev/null \
        | "${FLAMEGRAPH_DIR}/flamegraph.pl" --title "LiteCode-CPP judge hotpath" > "${FLAME_OUT}" 2>/dev/null; then
        fail "Phase C flamegraph 生成失败（stackcollapse/flamegraph perl 脚本异常）"
        return 1
    fi
    if [ -s "${FLAME_OUT}" ]; then
        local svg_size
        svg_size="$(wc -c < "${FLAME_OUT}")"
        ok "Phase C flamegraph 生成成功（${FLAME_OUT}, ${svg_size} bytes）"
        PERF_RAN=1
        REPORT_PHASE_C="| ${pid} | ${PERF_DURATION_S}s | ${PERF_FREQ_HZ}Hz | ${FLAME_OUT} |"
    else
        fail "Phase C flamegraph 输出为空"
    fi
}

# ═════════════════════════ 报告再生 ═════════════════════════
write_report() {
    mkdir -p "$(dirname "${REPORT_OUT}")"
    cat > "${REPORT_OUT}" <<EOF
# LiteCode-CPP 性能 Profile 报告

> Phase 9 △「性能 Profile」。本文件的 **结果段由 \`scripts/perf_profile.sh\` 运行时再生**，
> 手改结果段无意义；方法论 / 口径 / 运行方式为静态说明（沿用 v1.2.65 load-test-report.md
> 模板）。

## 1. 目的与验收口径（SPEC §12.2）

| 指标 | 标准 | 工具 |
|------|------|------|
| 健康检查 | < 50ms | curl \`time_starttransfer\` |
| 题目列表 API | < 200ms | curl \`time_total\` |
| 提交 API 响应 | < 200ms | curl \`time_total\` |
| 排行榜 API（沿用列表阈值） | < 200ms | curl \`time_total\` |
| 判题响应 P95 | < 5s | Prometheus \`litecode_judge_duration_seconds\` |
| 并发判题 | 支持 10 人 | 见 v1.2.65 load_test.sh |

本脚本不重做并发压测（v1.2.65 已落地），只补 **单 endpoint timing 拆解** + **histogram 抓取**
+ **perf 火焰图** 三段定位能力。

## 2. 方法论

- **Phase A — HTTP 面分阶段 timing**：用 \`curl -w '%{time_namelookup} %{time_connect} %{time_appconnect} %{time_pretransfer} %{time_starttransfer} %{time_total}\n'\` 模板，对 4 个关键 endpoint（/health, /problems, /auth/login, /api/v1/metrics）各采样 \`PROFILE_SAMPLES\` 次（默认 20），按 \`sort -n\` + \`ceil(p/100·n)\` 取分位数；阈值用 SPEC §12.2 表格的 raw value 注入（脚本顶部常量）。
- **Phase B — Prometheus histogram**：前后两次 scrape \`/api/v1/metrics\`（间隔一段 5-路并发 AC 压测），对比 \`litecode_judge_duration_seconds\` 的 \`_count\` / \`_sum\` 增量；从第二个 snapshot 的 \`_bucket\` 序列用线性插值反推 p50/p95/p99（典型 histogram_quantile 客户端实现）。
- **Phase C — Linux perf + flamegraph**（可选）：\`docker exec litecode-web perf record -F 99 -g -p \$PID -- sleep 30\` → \`docker cp\` → \`perf script\` → \`stackcollapse-perf.pl | flamegraph.pl\` → \`docs/perf-flamegraph.svg\`。**只 Linux 容器 + perf/FlameGraph 工具链齐全时跑**；其他平台 skip。

## 3. 如何运行

\`\`\`bash
# 默认（缺前置走 SKIP）
bash scripts/perf_profile.sh

# CI 强约束
PROFILE_STRICT=1 bash scripts/perf_profile.sh

# 自定义采样次数 / 阈值
PROFILE_SAMPLES=50 HEALTH_MAX_MS=50 bash scripts/perf_profile.sh
\`\`\`

跑完本文件的「运行环境」与「结果」两段会被自动覆盖。

## 4. 运行环境（最近一次运行再生）

| 项 | 值 |
|----|----|
| 运行时间 | ${RUN_TS} |
| git 版本 | ${GIT_REV} |
| BASE_URL | ${BASE_URL} |
| warm_pool（/health） | ${WARM_POOL} |
| HTTP 采样次数 | ${PROFILE_SAMPLES} |
| perf 容器 | ${PERF_CONTAINER} |
| perf 抓取时长 | ${PERF_DURATION_S}s |
| perf 频率 | ${PERF_FREQ_HZ}Hz |

## 5. 结果（最近一次运行再生）

### 5.1 Phase A — HTTP 面分阶段 timing 拆解

单位：ms。p50 / p95 / max 是 \`time_total\`；dns p95 / connect p95 / ttfb p95 是各阶段分位数。
阈值来自 SPEC §12.2；只对 \`total p95\` 做硬断言。

| endpoint | p50 total | p95 total | max total | dns p95 | connect p95 | ttfb p95 | samples | threshold | verdict |
|----------|-----------|-----------|-----------|---------|-------------|----------|---------|-----------|---------|
$(if [ -n "${REPORT_PHASE_A}" ]; then printf '%s\n' "${REPORT_PHASE_A}"; else echo '| (skipped) | — | — | — | — | — | — | — | — | — |'; fi)

### 5.2 Phase B — Prometheus histogram 判题热路径

从 \`litecode_judge_duration_seconds\` snapshot 反推（p95_baseline 是 snapshot 累计分布，
不是本脚本期间增量）。

$(if [ -n "${REPORT_PHASE_B}" ]; then
echo '| histogram | Δ_count | Δ_sum_avg (s) | p50 (s) | p95 (s) | p99 (s) |'
echo '|-----------|---------|---------------|---------|---------|---------|'
echo "${REPORT_PHASE_B}"
else
echo '_(skipped：栈缺失 / provision 失败)_'
fi)

### 5.3 Phase C — Linux perf + flamegraph

$(if [ "${PERF_RAN}" = "1" ]; then
echo '| target pid | duration | freq | output |'
echo '|-----------|----------|------|--------|'
echo "${REPORT_PHASE_C}"
echo ""
echo "火焰图：\`docs/perf-flamegraph.svg\`（浏览器打开；红色 = on-CPU 最热）"
else
echo '_(skipped：本机非 Linux 容器 / perf 或 FlameGraph 工具链缺失)_'
echo ""
echo '**如何补跑**：'
echo '1. \`docker compose up -d web\`（容器内核需 \`--privileged\` 或 \`kernel.perf_event_paranoid<=1\`）'
echo '2. \`docker exec litecode-web sh -c "perf record -F 99 -p \$(pgrep -n litecode_server) -g -- sleep 30"\`'
echo '3. \`docker cp litecode-web:/tmp/perf.data /tmp/\` → \`perf script -i /tmp/perf.data\` → \`stackcollapse-perf.pl | flamegraph.pl > docs/perf-flamegraph.svg\`'
fi)

## 6. 解读与限制

- **Phase A** 在 localhost 跑，DNS / TCP 几乎为 0；TTFB 接近 total；想看真实网络开销需要在
  生产 host 上跑（或加 \`--interface\` 模拟远端）。
- **Phase B** 增量 Δ_count 太小（< 5）时分位数线性插值不稳；可加大并发级或挂一段
  v1.2.65 load_test 的 5/10/20 跑一段时间。
- **Phase C** 需要容器内核开 \`perf_event_paranoid\`（Ubuntu 22.04 默认 4，多数场景
  需要 \`--privileged\` 或 \`kernel.perf_event_paranoid=1\`）。
- \`date +%s.%N\` 在 MSYS/Git-Bash 上纳秒精度依赖底层，毫秒级足够本口径。
EOF
    echo "report → ${REPORT_OUT}"
}

# ═════════════════════════ 主流程 ═════════════════════════
provision

if [ "${SERVER_UP}" != "1" ]; then
    skip "Phase A / B / C 全套（栈缺失）"
else
    run_phase_a || true
    run_phase_b || true
    run_phase_c || true
fi

write_report

# ═════════════════════════ 总结 ═════════════════════════
echo
echo "════════════════════════════════════════════"
echo "  Passed: ${PASS}   Failed: ${FAIL}   Skipped: ${SKIP}"
echo "════════════════════════════════════════════"
# PROFILE_RESULT 末行（v1.2.67 FUZZ_RESULT / v1.2.72 DRILL_RESULT 同款反向汇入设计）
echo "PROFILE_RESULT PASS=${PASS} FAIL=${FAIL} SKIP=${SKIP}"

if [ "${FAIL}" -gt 0 ]; then
    echo "结果：FAIL（存在未达标 endpoint，或 STRICT 模式下能力缺失被升级）"
    exit 1
fi
if [ "${HTTP_RAN}" = "0" ] && [ "${PROM_RAN}" = "0" ] && [ "${PERF_RAN}" = "0" ]; then
    echo "结果：SKIP（未执行任何 phase —— 栈 / 工具缺失；报告未再生）"
else
    echo "结果：PASS（所有 phase 已运行；如全 SKIP 也在末尾 PROFILE_RESULT 体现）"
fi
exit 0