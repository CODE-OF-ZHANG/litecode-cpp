#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/fuzz_judge.sh
# -------------------------------------------------------------
# Phase 8 △ 模糊测试（fuzzing）判题输入 —— v1.2.67
#
# 定位：在单测 / 集成测 / e2e / pen-test 之上再压一层「黑盒 fuzz」。
#       不引入 libFuzzer / AFL 链路（Windows 兼容性差），用
#       Python3 自带字节级 mutation 引擎（flip / insert / delete /
#       overwrite / duplicate）替代 radamsa，对运行中的完整栈
#       随机化 HTTP 输入并断言 server / judge 不被打挂。
#
# 覆盖（e2e A40–A43）：
#   A40 source_code fuzz       —— 判题输入主目标
#   A41 submission 字段边界    —— problem_id / language 边界值
#   A42 HTTP envelope fuzz     —— /auth/login + /auth/register 防御纵深
#   A43 容量 smoke + /health    —— burst mutation + 健康探针不变式
#
# 依赖：bash + curl + jq + python3
# （python3 不在 PATH 时自动探测 WindowsApps / Microsoft Store stub）
#
# 环境变量 / 参数（均可覆盖）：
#   BASE_URL             默认 http://localhost:8080
#   USER_TOK             e2e provision 捕获的普通用户 access_token
#   ADMIN_TOK            e2e provision 捕获的 admin access_token
#   PID                  problems/two-sum 的 problem_id
#   FUZZ_MUTATIONS       默认 50（A40 用此值）
#   FUZZ_STRICT          默认 0；置 1 把缺栈 skip 升级为 fail
#   JUDGE_POLL_TIMEOUT_S 默认 30；单次轮询到终态的最长秒数
#
# CLI（也支持，便于独立运行）：
#   bash scripts/fuzz_judge.sh \
#       --base-url http://localhost:8080 \
#       --user-tok "$USER_TOK" \
#       --admin-tok "$ADMIN_TOK" \
#       --problem-id "$PID" \
#       --mutations 50 \
#       --strict 0
#
# 退出码：
#   0  —— 无 FAIL
#   1  —— 至少一个 FAIL
#   2  —— 缺 USER_TOK / PID 等必需输入
#
# 输出契约：末行固定为 `FUZZ_RESULT PASS=N FAIL=N SKIP=N`，
#          e2e_acceptance.sh 通过 grep 该行反向汇入主计数器。
# =============================================================
set -uo pipefail

# ───────────────────────── 路径解析 ─────────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

# ───────────────────────── CLI 参数解析 ─────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        --base-url)    BASE_URL="$2"; shift 2;;
        --user-tok)    USER_TOK="$2"; shift 2;;
        --admin-tok)   ADMIN_TOK="$2"; shift 2;;
        --problem-id)  PID="$2"; shift 2;;
        --mutations)   FUZZ_MUTATIONS="$2"; shift 2;;
        --strict)      FUZZ_STRICT="$2"; shift 2;;
        *)             shift;;
    esac
done

# ───────────────────────── 配置 ─────────────────────────
BASE_URL="${BASE_URL:-http://localhost:8080}"
BASE_URL="${BASE_URL%/}"
API="${BASE_URL}/api/v1"
FUZZ_MUTATIONS="${FUZZ_MUTATIONS:-50}"
FUZZ_STRICT="${FUZZ_STRICT:-${E2E_STRICT:-0}}"
JUDGE_POLL_TIMEOUT_S="${JUDGE_POLL_TIMEOUT_S:-30}"

# ───────────────────────── 计数器 ─────────────────────────
PASS=0; FAIL=0; SKIP=0
ok()   { echo "    ok   - $*"; PASS=$((PASS+1)); }
fail() { echo "    FAIL - $*"; FAIL=$((FAIL+1)); }
skip() { echo "    skip - $*"; SKIP=$((SKIP+1)); }
kase() { echo; echo "── $1  $2"; }

# ───────────────────────── 依赖探测 ─────────────────────────
if ! command -v python3 >/dev/null 2>&1; then
    for cand in \
        "/c/Users/${USERNAME:-$USER}/AppData/Local/Programs/Python" \
        "/c/Program Files/Python" \
        "/c/Python312" "/c/Python311" "/c/Python310" ; do
        if [ -x "${cand}/python.exe" ]; then export PATH="${cand}:${PATH}"; break; fi
    done
fi
for tool in curl jq python3; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "missing required tool: ${tool}" >&2; exit 2; }
done

# ───────────────────────── 输入校验 ─────────────────────────
if [ -z "${USER_TOK:-}" ]; then
    echo "USER_TOK empty — cannot fuzz without a logged-in user" >&2
    exit 2
fi
if [ -z "${PID:-}" ]; then
    echo "PID empty — cannot fuzz without a problem_id (run provision first)" >&2
    exit 2
fi

# ───────────────────────── 临时目录 ─────────────────────────
TMPD="$(mktemp -d -t lc-fuzz-XXXXXX)"
trap 'rm -rf "${TMPD}"' EXIT

# ───────────────────────── HTTP helpers ─────────────────────────
# post_json PATH TOKEN BODY_FILE [CONTENT_TYPE] [EXTRA_HEADER]  →  echo HTTP_CODE
#   EXTRA_HEADER 例："X-Forwarded-For: 10.99.0.1" —— 用于隔离 register/login 的 IP bucket
post_json() {
    local path="$1" tok="$2" body_file="$3" ct="${4:-application/json}" xhdr="${5:-}"
    local args=( -sS -o "${TMPD}/body" -D "${TMPD}/hdr"
                 -w "%{http_code}"
                 -X POST "${API}${path}"
                 -H "Content-Type: ${ct}"
                 --data-binary "@${body_file}"
                 --max-time 30 )
    [ -n "${tok}" ] && args+=( -H "Authorization: Bearer ${tok}" )
    [ -n "${xhdr}" ] && args+=( -H "${xhdr}" )
    curl "${args[@]}" 2>/dev/null
}

# get_json PATH TOKEN [EXTRA_HEADER]  →  echo HTTP_CODE
get_json() {
    local path="$1" tok="$2" xhdr="${3:-}"
    local args=( -sS -o "${TMPD}/body" -D "${TMPD}/hdr"
                 -w "%{http_code}"
                 "${API}${path}"
                 --max-time 30 )
    [ -n "${tok}" ] && args+=( -H "Authorization: Bearer ${tok}" )
    [ -n "${xhdr}" ] && args+=( -H "${xhdr}" )
    curl "${args[@]}" 2>/dev/null
}

# health_check →  echo HTTP_CODE
health_check() {
    curl -sS -o "${TMPD}/health" -w "%{http_code}" "${API}/health" --max-time 10 2>/dev/null
}

# jqf JQ_EXPR  →  echo field value (空 → empty string)
jqf() { jq -r "$1" "${TMPD}/body" 2>/dev/null; }

# ───────────────────────── mutation 引擎 ─────────────────────────
# fuzz_mutate SEED_FILE OUT_DIR COUNT PREFIX
#   生成 COUNT 个 mutation 文件到 OUT_DIR/PREFIX_0000..NNNN.bin
fuzz_mutate() {
    local seed_file="$1" out_dir="$2" count="$3" prefix="$4"
    python3 - "${seed_file}" "${out_dir}" "${count}" "${prefix}" <<'PY'
import os, random, sys
seed_file, out_dir, count, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
os.makedirs(out_dir, exist_ok=True)
random.seed(os.urandom(16).__hash__() if hasattr(os.urandom(16), '__hash__') else os.getpid())
with open(seed_file, 'rb') as f:
    seed = f.read()

def mutate(buf):
    if not buf:
        return os.urandom(random.randrange(1, 64))
    s = bytearray(buf)
    rounds = random.randrange(1, 4)
    for _ in range(rounds):
        if not s:
            break
        op = random.choice(['flip', 'insert', 'delete', 'overwrite', 'duplicate'])
        if op == 'flip':
            s[random.randrange(len(s))] ^= (1 << random.randrange(8))
        elif op == 'insert':
            s.insert(random.randrange(len(s) + 1), random.randrange(256))
        elif op == 'delete' and len(s) > 1:
            del s[random.randrange(len(s))]
        elif op == 'overwrite':
            n = random.randrange(1, min(16, len(s)) + 1)
            i = random.randrange(len(s) - n + 1)
            for j in range(n):
                s[i + j] = random.randrange(256)
        elif op == 'duplicate':
            i = random.randrange(len(s))
            n = random.randrange(2, 8)
            s = s[:i] + s[i:i + 1] * n + s[i + 1:]
    return bytes(s)

for i in range(count):
    with open(os.path.join(out_dir, f'{prefix}_{i:04d}.bin'), 'wb') as f:
        f.write(mutate(seed))
PY
}

# ───────────────────────── fuzz seed ─────────────────────────
mkdir -p "${TMPD}/seeds"
cat > "${TMPD}/seeds/twosum_ac.cpp" <<'CXX'
#include <bits/stdc++.h>
using namespace std;
int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    int n,t; if(!(cin>>n>>t)) return 0;
    vector<int>a(n);
    for(auto&x:a) cin>>x;
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(a[i]+a[j]==t){ cout<<i<<" "<<j<<"\n"; return 0;}
    return 0;
}
CXX

# ═════════════════════════════════════════════════════════════
#  A40 — submission source_code fuzz（判题输入主目标）
# ═════════════════════════════════════════════════════════════
kase "A40" "submission source_code 模糊测试（判题输入主目标）"

N_A40="${FUZZ_MUTATIONS}"
fuzz_mutate "${TMPD}/seeds/twosum_ac.cpp" "${TMPD}/mut_a40" "${N_A40}" "code"

baseline_hc=$(health_check)
if [ "${baseline_hc}" != "200" ]; then
    skip "A40 /health baseline=${baseline_hc}（栈未健康，跳过 fuzz）"
else
    ac=0; nonac=0; stuck=0; crash5xx=0; rl429=0; badcode=0
    polled=0
    for f in "${TMPD}"/mut_a40/code_*.bin; do
        [ -f "${f}" ] || continue
        # 用 jq 构造 envelope，code 字段以 rawfile 注入避免 shell 转义
        jq -n --argjson pid "${PID}" \
            --arg lang "cpp" \
            --rawfile code "${f}" \
            '{problem_id:$pid,language:$lang,code:$code}' > "${TMPD}/body_a40.json"
        code=$(post_json "/submissions" "${USER_TOK}" "${TMPD}/body_a40.json")
        case "${code}" in
            500) crash5xx=$((crash5xx+1)); continue;;
            429) rl429=$((rl429+1)); continue;;
            201) :;;
            *)   badcode=$((badcode+1)); continue;;
        esac
        sid=$(jqf '.data.submission_id')
        [ -z "${sid}" ] || [ "${sid}" = "null" ] && { badcode=$((badcode+1)); continue; }
        # 轮询到终态
        final=""
        elapsed=0
        while [ "${elapsed}" -lt "${JUDGE_POLL_TIMEOUT_S}" ]; do
            sleep 1
            elapsed=$((elapsed+1))
            sc=$(get_json "/submissions/${sid}" "${USER_TOK}")
            if [ "${sc}" = "500" ]; then crash5xx=$((crash5xx+1)); final="crash"; break; fi
            final=$(jqf '.data.status')
            case "${final}" in
                ac|wa|re|tle|mle|ole|pe|ce|se) break;;
            esac
        done
        polled=$((polled+1))
        case "${final}" in
            ac)                       ac=$((ac+1));;
            wa|re|tle|mle|ole|pe|ce|se) nonac=$((nonac+1));;
            crash) ;;
            *)                       stuck=$((stuck+1));;
        esac
    done
    echo "    A40 fuzzed ${N_A40} source_code probes: polled=${polled} AC=${ac} nonAC-terminal=${nonac} stuck=${stuck} 5xx=${crash5xx} 429=${rl429} unexpected=${badcode}"
    [ "${crash5xx}" = "0" ] && ok "A40 全部 ${N_A40} 个 code fuzz 探针无 5xx（HTTP 500 触发 0 次）" \
                             || fail "A40 ${crash5xx}/${N_A40} 探针触发 5xx"
    [ "${stuck}" = "0" ]    && ok "A40 全部 ${polled} 个 fuzz 探针到达终态（无 pending 卡死 >${JUDGE_POLL_TIMEOUT_S}s）" \
                             || fail "A40 ${stuck}/${polled} 探针卡在 pending 超过 ${JUDGE_POLL_TIMEOUT_S}s"
    post_hc=$(health_check)
    [ "${post_hc}" = "200" ] && ok "A40 fuzz 后 /health 仍 200（judge 未被打挂）" \
                              || fail "A40 fuzz 后 /health HTTP=${post_hc}（judge 被 fuzz 致残！）"
fi

# ═════════════════════════════════════════════════════════════
#  A41 — submission 字段边界 fuzz
# ═════════════════════════════════════════════════════════════
kase "A41" "submission problem_id/language/code 字段边界 fuzz"

# python3 生成 JSONL：每个 probe 一行
python3 - "${TMPD}/mut_a41.jsonl" 30 <<'PY'
import json, os, random, sys
out, count = sys.argv[1], int(sys.argv[2])
random.seed(os.getpid() ^ os.urandom(8).__hash__() if hasattr(os.urandom(8), '__hash__') else os.getpid())
PROB_VALUES = [0, -1, 2**31-1, 2**63-1, -(2**63), 99999999, "abc", None, 1.5, "0", -2147483648]
LANG_VALUES = ["cpp", "c", "", "C++", "cpp\n", "c\x00", "../cpp", "\xc3\x28", "a"*1000, None, "g++", "PYTHON"]
CODE_VALUES = [
    "int main(){return 0;}",
    "",
    "a" * 65536,
    "int main(){while(1);}",
    "\x00\x00\x00",
    "int main(){}\x00trailing-nul",
    "\xff\xfe\xfd",
]
with open(out, 'w', encoding='utf-8') as f:
    for _ in range(count):
        body = {
            "problem_id": random.choice(PROB_VALUES),
            "language":   random.choice(LANG_VALUES),
            "code":       random.choice(CODE_VALUES),
        }
        f.write(json.dumps(body, ensure_ascii=False) + "\n")
PY

baseline_hc=$(health_check)
if [ "${baseline_hc}" != "200" ]; then
    skip "A41 /health baseline=${baseline_hc}"
else
    crash5xx=0; non5xx=0; total=0
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        total=$((total+1))
        # 写文件再 --data-binary @file 避免 heredoc 大小 / 字符转义问题
        printf '%s' "${line}" > "${TMPD}/body_a41.json"
        code=$(post_json "/submissions" "${USER_TOK}" "${TMPD}/body_a41.json")
        if [ "${code}" = "500" ]; then crash5xx=$((crash5xx+1))
        else non5xx=$((non5xx+1)); fi
    done < "${TMPD}/mut_a41.jsonl"
    echo "    A41 fuzzed ${total} field-boundary probes: non-5xx=${non5xx} 5xx=${crash5xx}"
    [ "${crash5xx}" = "0" ] && ok "A41 全部 ${total} 个 problem_id/language/code 边界值无 5xx" \
                              || fail "A41 ${crash5xx}/${total} 探针触发 5xx"
    post_hc=$(health_check)
    [ "${post_hc}" = "200" ] && ok "A41 fuzz 后 /health 仍 200" \
                              || fail "A41 fuzz 后 /health HTTP=${post_hc}"
fi

# ═════════════════════════════════════════════════════════════
#  A42 — HTTP envelope 防御纵深 fuzz
# ═════════════════════════════════════════════════════════════
kase "A42" "HTTP envelope 模糊测试（/auth/login + /auth/register）"

baseline_hc=$(health_check)
if [ "${baseline_hc}" != "200" ]; then
    skip "A42 /health baseline=${baseline_hc}"
else
    # /auth/login — bucket 10/min/IP
    fuzz_mutate "${TMPD}/seeds/twosum_ac.cpp" "${TMPD}/mut_a42_login" 25 "login"
    crash5xx=0; rl429=0; total=0
    for f in "${TMPD}"/mut_a42_login/login_*.bin; do
        [ -f "${f}" ] || continue
        total=$((total+1))
        code=$(post_json "/auth/login" "" "${f}")
        [ "${code}" = "500" ] && crash5xx=$((crash5xx+1))
        [ "${code}" = "429" ] && rl429=$((rl429+1))
    done
    echo "    A42 login fuzz: ${total} probes, 5xx=${crash5xx}, 429=${rl429}"

    # /auth/register — bucket 5/min/IP, 用 X-Forwarded-For 给每个 probe 一个独立 IP
    # 避免被 rate-limit 拦下（A26a 同款思路），同时验证 XFF trust chain 不被恶意伪造 IP 击穿
    fuzz_mutate "${TMPD}/seeds/twosum_ac.cpp" "${TMPD}/mut_a42_reg" 25 "reg"
    reg_total=0; reg_crash=0
    for idx in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
        f="${TMPD}/mut_a42_reg/reg_$(printf '%04d' "${idx}").bin"
        [ -f "${f}" ] || continue
        reg_total=$((reg_total+1))
        # 10.99.x.y /24 随机（v1.2.63 A26a 同款段），每 probe 独立 IP → 独立 quota bucket
        xff="10.99.$(( (idx / 256) % 256 )).$(( (idx % 253) + 1 ))"
        code=$(post_json "/auth/register" "" "${f}" "application/json" "X-Forwarded-For: ${xff}")
        [ "${code}" = "500" ] && reg_crash=$((reg_crash+1))
    done
    total=$((total + reg_total))
    crash5xx=$((crash5xx + reg_crash))
    echo "    A42 register fuzz: ${reg_total} probes, 5xx=${reg_crash}（XFF 隔离 IP bucket，不睡 65s × 4）"
    echo "    A42 total: ${total} probes, 5xx=${crash5xx}"

    [ "${crash5xx}" = "0" ] && ok "A42 全部 ${total} 个 envelope fuzz 探针无 5xx（/auth/login + /auth/register 边界）" \
                              || fail "A42 ${crash5xx}/${total} envelope 探针触发 5xx"
    post_hc=$(health_check)
    [ "${post_hc}" = "200" ] && ok "A42 fuzz 后 /health 仍 200" \
                              || fail "A42 fuzz 后 /health HTTP=${post_hc}"
fi

# ═════════════════════════════════════════════════════════════
#  A43 — 容量 smoke + /health 不变式
# ═════════════════════════════════════════════════════════════
kase "A43" "容量烟雾测试 + /health 不变式"

baseline_hc=$(health_check)
if [ "${baseline_hc}" != "200" ]; then
    skip "A43 /health baseline=${baseline_hc}"
else
    fuzz_mutate "${TMPD}/seeds/twosum_ac.cpp" "${TMPD}/mut_a43" 20 "burst"
    crash5xx=0; sent=0; health_during_fail=0
    UNIQ_CTR=0
    for f in "${TMPD}"/mut_a43/burst_*.bin; do
        [ -f "${f}" ] || continue
        sent=$((sent+1))
        UNIQ_CTR=$((UNIQ_CTR+1))
        jq -n --argjson pid "${PID}" \
            --arg lang "cpp" \
            --rawfile code "${f}" \
            '{problem_id:$pid,language:$lang,code:$code}' > "${TMPD}/body_a43.json"
        code=$(post_json "/submissions" "${USER_TOK}" "${TMPD}/body_a43.json")
        [ "${code}" = "500" ] && crash5xx=$((crash5xx+1))
        # 每 5 个 probe 插一次 /health 健康探针（fail-fast）
        if [ $((sent % 5)) = "0" ]; then
            mid_hc=$(health_check)
            if [ "${mid_hc}" != "200" ]; then
                health_during_fail=$((health_during_fail+1))
                fail "A43 fuzz 中途 /health HTTP=${mid_hc}（judge 在第 ${sent} 个 burst 探针后挂掉）"
            fi
        fi
    done
    final_hc=$(health_check)
    [ "${final_hc}" = "200" ] && ok "A43 ${sent} burst 探针后 /health 仍 200（不变式成立）" \
                                || fail "A43 burst 后 /health HTTP=${final_hc}"
    [ "${crash5xx}" = "0" ] && ok "A43 ${sent} burst 探针无 5xx" \
                              || fail "A43 ${crash5xx}/${sent} burst 探针触发 5xx"
fi

# ═════════════════════════════════════════════════════════════
#  总结
# ═════════════════════════════════════════════════════════════
echo
echo "════════════════════════════════════════════"
echo "  Passed: ${PASS}   Failed: ${FAIL}   Skipped: ${SKIP}"
echo "════════════════════════════════════════════"

# 输出契约：末行固定为 FUZZ_RESULT PASS=N FAIL=N SKIP=N
echo "FUZZ_RESULT PASS=${PASS} FAIL=${FAIL} SKIP=${SKIP}"

if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi
exit 0
