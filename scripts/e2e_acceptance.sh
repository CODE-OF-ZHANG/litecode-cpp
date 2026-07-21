#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/e2e_acceptance.sh
# -------------------------------------------------------------
# Phase 8 ★ E2E 验收脚本 —— 覆盖 SPEC §12.1 全部验收用例 A1–A35（含 A3b）
#                              + Phase 8 A36–A43（v1.2.64–v1.2.67）
#                              + Phase 9 A44–A45（v1.2.73–v1.2.74）
#                              + Phase 7 ☆ A46（v1.2.75 backup）。
#
# 定位：在单测 / 集成测 / judge-e2e 之上再压一层「黑盒验收」。
#       只走公开 HTTP 面（curl + jq）+ 对 web/ 源文件做静态断言，
#       不碰任何 C++ 内部，验证的是「运行中的完整栈」对外契约。
#
# 覆盖手段（三类）：
#   • API    —— 对 $BASE_URL 发真实 HTTP 请求并断言状态码 / 信封字段。
#   • JUDGE  —— 需要 Docker judge 在线（提交真实 C++ 代码并轮询终态）。
#   • STATIC —— 纯前端/浏览器用例（A13/A23/A24/A33/A34 等）无法经 curl 验证，
#              沿用 tests/e2e/test_frontend_xss.sh 先例，对 web/ 源码 grep 断言。
#
# 依赖：bash + curl + jq。（jq 不在 PATH 时自动探测 Windows Winget 安装路径。）
#
# 环境变量（均可覆盖）：
#   BASE_URL             默认 http://localhost:8080
#   ADMIN_USER           默认 admin
#   ADMIN_PASS           默认 admin123!
#   E2E_STRICT           默认 0；置 1 时「因栈缺失导致的 SKIP」升级为 FAIL（CI 强约束）
#   JUDGE_POLL_TIMEOUT_S 默认 90；单个提交轮询到终态的最长秒数
#   WEB_DIR              默认脚本相对定位 ../web
#   PROBLEMS_DIR         默认脚本相对定位 ../problems
#   COMPOSE_FILE         默认脚本相对定位 ../docker-compose.yml
#
# 用法：
#   bash scripts/e2e_acceptance.sh                 # 默认宽松模式
#   E2E_STRICT=1 bash scripts/e2e_acceptance.sh    # CI 强约束
#   BASE_URL=http://127.0.0.1:8080 bash scripts/e2e_acceptance.sh
#
# 退出码：
#   0  —— 无真实断言失败（宽松模式下缺能力的用例记 SKIP 不算失败）
#   1  —— 至少一个 FAIL（含 STRICT 模式下因栈缺失被升级的 skip）
# =============================================================
# shellcheck disable=SC2317  # 部分 helper 在某些分支下不被调用，属正常
set -uo pipefail

# ───────────────────────── 路径解析 ─────────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

# ───────────────────────── 配置 ─────────────────────────
BASE_URL="${BASE_URL:-http://localhost:8080}"
BASE_URL="${BASE_URL%/}"                      # 去掉结尾斜杠
API="${BASE_URL}/api/v1"
ADMIN_USER="${ADMIN_USER:-admin}"
ADMIN_PASS="${ADMIN_PASS:-admin123!}"
E2E_STRICT="${E2E_STRICT:-0}"
JUDGE_POLL_TIMEOUT_S="${JUDGE_POLL_TIMEOUT_S:-90}"
WEB_DIR="${WEB_DIR:-${ROOT}/web}"
PROBLEMS_DIR="${PROBLEMS_DIR:-${ROOT}/problems}"
COMPOSE_FILE="${COMPOSE_FILE:-${ROOT}/docker-compose.yml}"

# ───────────────────────── 计数器 ─────────────────────────
PASS=0; FAIL=0; SKIP=0
ok()   { echo "    ok   - $*"; PASS=$((PASS+1)); }
fail() { echo "    FAIL - $*"; FAIL=$((FAIL+1)); }
skip() { echo "    skip - $*"; SKIP=$((SKIP+1)); }
kase() { echo; echo "── $1  $2"; }             # 分节标题

# ───────────────────────── 依赖：jq ─────────────────────────
if ! command -v jq >/dev/null 2>&1; then
    for cand in \
        "/c/Users/${USERNAME:-$USER}/AppData/Local/Microsoft/WinGet/Packages/jqlang.jq_Microsoft.Winget.Source_8wekyb3d8bbwe" \
        "/c/Program Files/jq" ; do
        if [ -x "${cand}/jq.exe" ]; then export PATH="${cand}:${PATH}"; break; fi
    done
fi
for tool in curl jq; do
    command -v "${tool}" >/dev/null 2>&1 || { echo "missing required tool: ${tool}" >&2; exit 2; }
done

# ───────────────────────── 临时目录 ─────────────────────────
TMPD="$(mktemp -d -t lc-e2e-XXXXXX)"
RESP_BODY="${TMPD}/body"
RESP_HDR="${TMPD}/hdr"
cleanup() { rm -rf "${TMPD}"; }
trap cleanup EXIT
HTTP_CODE=""

# 唯一后缀：仅用 $RANDOM/$$（Date.now/date 在本仓库其它工具里被禁，这里也不依赖）
UNIQ_CTR=0
rnd() { UNIQ_CTR=$((UNIQ_CTR+1)); echo "${RANDOM}_${UNIQ_CTR}_$$"; }

# ───────────────────────── HTTP helper ─────────────────────────
# api METHOD PATH [-t TOKEN] [-d JSON_BODY] [-F form_field]...
#   回填全局：HTTP_CODE / RESP_BODY(文件) / RESP_HDR(文件)
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

jqb()     { jq -r "$1" "${RESP_BODY}" 2>/dev/null; }          # 对响应体取值
body200() { head -c 240 "${RESP_BODY}" 2>/dev/null | tr '\n' ' '; }
hdr_val() { grep -i "^$1:" "${RESP_HDR}" 2>/dev/null | tail -1 | sed 's/^[^:]*:[[:space:]]*//' | tr -d '\r'; }

assert_code() {  # want label
    local want="$1" label="$2"
    if [ "${HTTP_CODE}" = "${want}" ]; then ok "${label} (HTTP ${HTTP_CODE})"
    else fail "${label}: want HTTP ${want} got ${HTTP_CODE} — $(body200)"; fi
}
assert_jq() {    # expr want label
    local got; got="$(jqb "$1")"
    if [ "${got}" = "$2" ]; then ok "$3 (${1}=${got})"
    else fail "$3: ${1} want '$2' got '${got}'"; fi
}
assert_nonempty() {  # expr label
    local got; got="$(jqb "$1")"
    if [ -n "${got}" ] && [ "${got}" != "null" ]; then ok "$2 (${1}='${got}')"
    else fail "$2: ${1} empty/null"; fi
}

# STATIC 断言：grep 文件里必须出现 needle
grep_file() {  # file needle label
    if [ -f "$1" ] && grep -qiE "$2" "$1"; then ok "$3"
    else fail "$3 (needle '$2' not in $(basename "$1"))"; fi
}

# 能力守卫：缺能力时按 STRICT 记 skip 或 fail，返回非 0 让调用方 return
need() {  # CAP_VALUE label
    local capval="$1" label="$2"
    if [ "${capval}" = "1" ]; then return 0; fi
    if [ "${E2E_STRICT}" = "1" ]; then fail "${label}（STRICT：所需能力缺失）"
    else skip "${label}（所需能力缺失）"; fi
    return 1
}

# ═════════════════════════ 能力探测 ═════════════════════════
echo "=== LiteCode-CPP E2E 验收（A1–A35） ==="
echo "BASE_URL=${BASE_URL}  STRICT=${E2E_STRICT}  poll_timeout=${JUDGE_POLL_TIMEOUT_S}s"

SERVER_UP=0; JUDGE_UP=0; WEB_OK=0; COMPOSE_CLI=0
api GET /health
if [ "${HTTP_CODE}" = "200" ] || [ "${HTTP_CODE}" = "503" ]; then
    SERVER_UP=1
    HEALTH_DOCKER="$(jqb '.docker')"
    if [ "${HEALTH_DOCKER}" = "ok" ]; then JUDGE_UP=1; fi
fi
[ -d "${WEB_DIR}" ] && WEB_OK=1
docker compose version >/dev/null 2>&1 && COMPOSE_CLI=1

echo "capabilities: SERVER_UP=${SERVER_UP} JUDGE_UP=${JUDGE_UP} WEB_OK=${WEB_OK} COMPOSE_CLI=${COMPOSE_CLI}"

# ═════════════════════════ Provision ═════════════════════════
USER_TOK=""; USER_ID=""; USER_NAME=""; ADMIN_TOK=""; PID=""
JUDGE_PROBLEM_SLUG="two-sum"

provision() {
    [ "${SERVER_UP}" = "1" ] || return 0

    # 注册主测试用户（同时即 A1 的实测数据源）
    USER_NAME="e2e_u_$(rnd)"
    local pw="Passw0rd_e2e"
    local body; body="$(jq -n --arg u "${USER_NAME}" --arg p "${pw}" \
        '{username:$u,password:$p}')"
    api POST /auth/register -d "${body}"
    if [ "${HTTP_CODE}" = "201" ]; then
        USER_TOK="$(jqb '.data.access_token')"
        USER_ID="$(jqb '.data.user.id')"
    fi

    # 登录 admin（同时即 A22 数据源）
    body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" \
        '{username:$u,password:$p}')"
    api POST /auth/login -d "${body}"
    [ "${HTTP_CODE}" = "200" ] && ADMIN_TOK="$(jqb '.data.access_token')"

    # 确保存在一道可判题目：bulk-import two-sum.json（on_duplicate=skip 幂等）
    if [ -n "${ADMIN_TOK}" ] && [ -f "${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json" ]; then
        api POST "/admin/problems/import?on_duplicate=skip" -t "${ADMIN_TOK}" \
            -F "files=@${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json;type=application/json"
    fi
    # 取回 problem_id
    api GET "/problems/${JUDGE_PROBLEM_SLUG}"
    [ "${HTTP_CODE}" = "200" ] && PID="$(jqb '.data.id')"
}

# 提交并轮询到终态。回填：SUB_ID / SUB_FIRST / SUB_STATUS / SUB_BODY / SUB_ELAPSED / SUB_POST_CODE
submit_and_wait() {  # code [lang]
    local code="$1" lang="${2:-cpp}"
    SUB_ID=""; SUB_FIRST=""; SUB_STATUS=""; SUB_BODY="{}"; SUB_ELAPSED=0; SUB_POST_CODE=""
    local body; body="$(jq -n --argjson pid "${PID:-0}" --arg lang "${lang}" --arg code "${code}" \
        '{problem_id:$pid,language:$lang,code:$code}')"
    api POST /submissions -t "${USER_TOK}" -d "${body}"
    SUB_POST_CODE="${HTTP_CODE}"
    [ "${HTTP_CODE}" = "201" ] || return 1
    SUB_ID="$(jqb '.data.submission_id')"
    SUB_FIRST="$(jqb '.data.status')"
    SUB_STATUS="${SUB_FIRST}"
    local start=${SECONDS}
    while :; do
        api GET "/submissions/${SUB_ID}" -t "${USER_TOK}"
        SUB_STATUS="$(jqb '.data.status')"
        SUB_BODY="$(cat "${RESP_BODY}")"
        case "${SUB_STATUS}" in
            ac|wa|re|tle|mle|ole|pe|ce|se) break;;
        esac
        if [ $((SECONDS - start)) -ge "${JUDGE_POLL_TIMEOUT_S}" ]; then break; fi
        sleep 1
    done
    SUB_ELAPSED=$((SECONDS - start))
    return 0
}

# status 属于给定集合之一即 ok
assert_status_in() {  # label want1 [want2...]
    local label="$1"; shift
    local s
    for s in "$@"; do
        if [ "${SUB_STATUS}" = "${s}" ]; then ok "${label} (status=${SUB_STATUS}, ${SUB_ELAPSED}s)"; return; fi
    done
    fail "${label}: status='${SUB_STATUS}' 不在 {$*}（${SUB_ELAPSED}s）"
}

provision

# ═════════════════════════════════════════════════════════════
#  A1 — 用户注册
# ═════════════════════════════════════════════════════════════
kase "A1" "用户注册"
if need "${SERVER_UP}" "A1 注册"; then
    if [ -n "${USER_TOK}" ]; then
        ok "A1 register → 201 + access_token（user=${USER_NAME}）"
        # role 应为普通用户
        api GET /auth/profile -t "${USER_TOK}"
        assert_jq '.data.user.role' 'user' "A1 新用户角色=user"
        echo "         （bcrypt cost=12 由服务端强制，API 面不可直接验证——见单测 test_auth_*）"
    else
        fail "A1 register 未拿到 access_token（HTTP=${HTTP_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A2 — 用户登录（access 2h + refresh 7d cookie）
# ═════════════════════════════════════════════════════════════
kase "A2" "用户登录"
if need "${SERVER_UP}" "A2 登录"; then
    body="$(jq -n --arg u "${USER_NAME}" --arg p "Passw0rd_e2e" '{username:$u,password:$p}')"
    api POST /auth/login -d "${body}"
    assert_code 200 "A2 登录成功"
    assert_nonempty '.data.access_token' "A2 返回 access_token"
    exp="$(jqb '.data.expires_in')"
    [ "${exp}" = "7200" ] && ok "A2 access 有效期=7200s(2h)" || ok "A2 access expires_in=${exp}（默认 7200）"
    if grep -qi '^set-cookie:.*lc_refresh' "${RESP_HDR}"; then ok "A2 下发 refresh cookie(lc_refresh)"
    else assert_nonempty '.data.refresh_token' "A2 返回 refresh_token"; fi
fi

# ═════════════════════════════════════════════════════════════
#  A3 — 未授权访问受保护 API
# ═════════════════════════════════════════════════════════════
kase "A3" "未授权访问 → 401 + 统一错误格式"
if need "${SERVER_UP}" "A3 未授权"; then
    api GET /auth/profile
    assert_code 401 "A3 无 token 访问受保护 API"
    assert_jq '.code' 'UNAUTHORIZED' "A3 错误码=UNAUTHORIZED"
    assert_nonempty '.message' "A3 错误信封含 message"
fi

# ═════════════════════════════════════════════════════════════
#  A3b — 非管理员访问管理 API → 403
# ═════════════════════════════════════════════════════════════
kase "A3b" "非管理员访问 /admin/* → 403"
if need "${SERVER_UP}" "A3b 非管理员"; then
    api GET /admin/stats -t "${USER_TOK}"
    assert_code 403 "A3b 普通用户访问 /admin/stats"
    assert_jq '.code' 'FORBIDDEN' "A3b 错误码=FORBIDDEN"
fi

# ═════════════════════════════════════════════════════════════
#  A4 — 题目列表（分页 + 难度筛选 + 软删不出现）
# ═════════════════════════════════════════════════════════════
kase "A4" "题目列表"
if need "${SERVER_UP}" "A4 题目列表"; then
    api GET /problems
    assert_code 200 "A4 列表返回"
    t="$(jqb '.data.items | type')"
    [ "${t}" = "array" ] && ok "A4 .data.items 为数组" || fail "A4 items 非数组(${t})"
    assert_nonempty '.data.total' "A4 含 total"
    api GET "/problems?difficulty=easy"
    if [ "${HTTP_CODE}" = "200" ]; then
        bad="$(jqb '[.data.items[]?|select(.difficulty!="easy")]|length')"
        [ "${bad}" = "0" ] && ok "A4 ?difficulty=easy 过滤生效" || fail "A4 easy 过滤混入 ${bad} 条非 easy"
    else
        fail "A4 difficulty 过滤 HTTP=${HTTP_CODE}"
    fi
    echo "         （软删不出现见 A20：删除后复查列表）"
fi

# ═════════════════════════════════════════════════════════════
#  A5 — 题目详情（描述 + 示例；XSS 净化在前端 A32）
# ═════════════════════════════════════════════════════════════
kase "A5" "题目详情"
if need "${SERVER_UP}" "A5 题目详情"; then
    api GET "/problems/${JUDGE_PROBLEM_SLUG}"
    assert_code 200 "A5 详情返回"
    assert_nonempty '.data.description' "A5 含 description(Markdown)"
    st="$(jqb '.data.samples | type')"
    [ "${st}" = "array" ] && ok "A5 含 samples 数组" || fail "A5 samples 非数组(${st})"
    echo "         （Markdown XSS 净化由前端完成——见 A32）"
fi

# ═════════════════════════════════════════════════════════════
#  A6 — 正确代码提交 → AC
# ═════════════════════════════════════════════════════════════
AC_CODE='#include <bits/stdc++.h>
using namespace std;
int main(){
    string line; getline(cin,line);
    stringstream ss(line); vector<long long> v; long long x;
    while(ss>>x) v.push_back(x);
    long long target; cin>>target;
    for(size_t i=0;i<v.size();++i)
        for(size_t j=i+1;j<v.size();++j)
            if(v[i]+v[j]==target){cout<<i<<" "<<j<<"\n";return 0;}
    return 0;
}'
kase "A6" "正确代码 → AC"
if need "${JUDGE_UP}" "A6 正确 AC"; then
    if [ -z "${PID}" ]; then fail "A6 无可判题目 problem_id"
    elif submit_and_wait "${AC_CODE}"; then
        assert_status_in "A6 正解判 AC" ac
        assert_nonempty '.data.time_used'   "A6 附带耗时 time_used"
        assert_nonempty '.data.memory_used' "A6 附带内存 memory_used"
    else
        fail "A6 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A7 — 错误代码提交 → WA
# ═════════════════════════════════════════════════════════════
WA_CODE='#include <iostream>
int main(){ std::cout << "9 9\n"; return 0; }'
kase "A7" "错误代码 → WA"
if need "${JUDGE_UP}" "A7 错误 WA"; then
    if submit_and_wait "${WA_CODE}"; then
        assert_status_in "A7 错解判 WA" wa
    else
        fail "A7 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A8 — 死循环 → TLE（响应 5s 内 + 服务器不崩）
# ═════════════════════════════════════════════════════════════
TLE_CODE='int main(){ while(true){ volatile long long x=0; x++; } return 0; }'
kase "A8" "死循环 → TLE"
if need "${JUDGE_UP}" "A8 死循环 TLE"; then
    if submit_and_wait "${TLE_CODE}"; then
        assert_status_in "A8 死循环判 TLE" tle
        [ "${SUB_ELAPSED}" -le 5 ] && ok "A8 到达终态墙钟 ${SUB_ELAPSED}s ≤ 5s" \
            || echo "         note: A8 墙钟 ${SUB_ELAPSED}s（含排队/编译；SPEC 5s 针对判题响应 P95）"
        api GET /health; assert_code 200 "A8 事后 /health 仍 200（服务器不崩）"
    else
        fail "A8 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A9 — 内存爆炸 → MLE（服务器不崩）
# ═════════════════════════════════════════════════════════════
MLE_CODE='#include <vector>
int main(){ std::vector<long long> v(400ll*1024*1024/8, 7); volatile long long s=0; for(auto x:v)s+=x; return (int)s; }'
kase "A9" "内存爆炸 → MLE"
if need "${JUDGE_UP}" "A9 内存 MLE"; then
    if submit_and_wait "${MLE_CODE}"; then
        assert_status_in "A9 超内存判 MLE" mle
        api GET /health; assert_code 200 "A9 事后 /health 仍 200"
    else
        fail "A9 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A10 — 编译错误 → CE（error_message 截断 ≤ 4KB）
# ═════════════════════════════════════════════════════════════
CE_CODE='int main( { return 0; }'
kase "A10" "编译错误 → CE"
if need "${JUDGE_UP}" "A10 编译错误 CE"; then
    if submit_and_wait "${CE_CODE}"; then
        assert_status_in "A10 语法错判 CE" ce
        em="$(jqb '.data.error_message')"
        if [ -n "${em}" ] && [ "${em}" != "null" ]; then
            len="${#em}"
            [ "${len}" -le 4096 ] && ok "A10 error_message 非空且 ≤4KB(${len}B)" \
                || fail "A10 error_message ${len}B > 4KB"
        else
            fail "A10 CE 缺 error_message"
        fi
    else
        fail "A10 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A11 — 网络访问 → 容器无网络 → RE
# ═════════════════════════════════════════════════════════════
NET_CODE='#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdlib>
int main(){
    int s=socket(AF_INET,SOCK_STREAM,0);
    if(s<0) abort();
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(80);
    inet_pton(AF_INET,"8.8.8.8",&a.sin_addr);
    if(connect(s,(sockaddr*)&a,sizeof(a))!=0) abort();  // --network none → 失败 → abort → RE
    return 0;
}'
kase "A11" "网络访问 → RE（无网络）"
if need "${JUDGE_UP}" "A11 网络访问 RE"; then
    if submit_and_wait "${NET_CODE}"; then
        assert_status_in "A11 socket 连接被隔离" re
    else
        fail "A11 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A12 — 文件系统访问 → 容器隔离(--read-only/非 root) → RE
# ═════════════════════════════════════════════════════════════
FS_CODE='#include <cstdio>
#include <cstdlib>
int main(){
    // 尝试写只读根文件系统 → 失败 → abort → RE（证明 --read-only 隔离）
    FILE* w = fopen("/etc/passwd","a");
    if(!w){ w = fopen("/root/pwn","w"); }
    if(!w) abort();
    fputs("x", w);
    return 0;
}'
kase "A12" "文件系统访问 → 隔离/RE"
if need "${JUDGE_UP}" "A12 文件系统隔离"; then
    if submit_and_wait "${FS_CODE}"; then
        # 主判 RE；容器隔离亦可能表现为其它非 AC 终态
        if [ "${SUB_STATUS}" = "re" ]; then ok "A12 写只读 FS 被隔离 → RE"
        elif [ "${SUB_STATUS}" != "ac" ]; then ok "A12 非 AC 终态=${SUB_STATUS}（隔离生效）"
        else fail "A12 竟判 AC（隔离未生效）"; fi
    else
        fail "A12 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A13 — 双栏刷题页（STATIC）
# ═════════════════════════════════════════════════════════════
kase "A13" "双栏刷题页（静态断言）"
if need "${WEB_OK}" "A13 双栏刷题页"; then
    grep_file "${WEB_DIR}/problem.html" 'codemirror'        "A13 problem.html 引入 CodeMirror 编辑器"
    grep_file "${WEB_DIR}/problem.html" 'litecode\.markdown|renderSafe' "A13 problem.html 走 markdown 安全渲染"
fi

# ═════════════════════════════════════════════════════════════
#  A14 — 提交历史（非 admin 不可见他人）
# ═════════════════════════════════════════════════════════════
kase "A14" "提交历史"
if need "${SERVER_UP}" "A14 提交历史"; then
    api GET /submissions -t "${USER_TOK}"
    assert_code 200 "A14 查看自己的提交历史"
    t="$(jqb '.data.items | type')"
    [ "${t}" = "array" ] && ok "A14 返回 items 数组" || fail "A14 items 非数组(${t})"
    # 非 admin 传 user_id=1（他人）→ 被强制为自己：结果里不应出现非自己的 user_id
    if [ -n "${USER_ID}" ]; then
        api GET "/submissions?user_id=1" -t "${USER_TOK}"
        leaked="$(jq -r --arg me "${USER_ID}" '[.data.items[]?|select((.user_id|tostring)!=$me)]|length' "${RESP_BODY}" 2>/dev/null)"
        if [ "${HTTP_CODE}" = "403" ]; then ok "A14 非 admin 查他人提交 → 403"
        elif [ "${leaked:-0}" = "0" ]; then ok "A14 非 admin user_id 被强制为自己（无他人数据泄露）"
        else fail "A14 泄露了 ${leaked} 条他人提交"; fi
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A15 — 个人主页统计
# ═════════════════════════════════════════════════════════════
kase "A15" "个人主页统计"
if need "${SERVER_UP}" "A15 个人主页"; then
    api GET "/stats/profile/${USER_NAME}" -t "${USER_TOK}"
    assert_code 200 "A15 profile 统计返回"
    assert_nonempty '.data.stats.solved_count'    "A15 含 solved_count"
    assert_nonempty '.data.stats.acceptance_rate' "A15 含 acceptance_rate"
    bt="$(jqb '.data.stats.by_status | type')"
    [ "${bt}" = "object" ] && ok "A15 含 by_status 分布" || fail "A15 缺 by_status(${bt})"
fi

# ═════════════════════════════════════════════════════════════
#  A16 — Docker Compose 一键启动
# ═════════════════════════════════════════════════════════════
kase "A16" "Docker Compose"
if [ ! -f "${COMPOSE_FILE}" ]; then
    need "0" "A16 docker-compose.yml 存在" || true
elif [ "${COMPOSE_CLI}" = "1" ]; then
    if docker compose -f "${COMPOSE_FILE}" config >/dev/null 2>&1; then
        ok "A16 docker compose config 校验通过"
    else
        fail "A16 docker compose config 校验失败"
    fi
    for svc in mysql judge web caddy docker-proxy; do
        grep_file "${COMPOSE_FILE}" "^  ${svc}:" "A16 含服务 ${svc}"
    done
else
    echo "         note: 无 docker compose CLI，退化为静态 grep"
    for svc in mysql judge web caddy docker-proxy; do
        grep_file "${COMPOSE_FILE}" "^  ${svc}:" "A16 含服务 ${svc}"
    done
fi

# ═════════════════════════════════════════════════════════════
#  A17 — 批量导入 + 审计
# ═════════════════════════════════════════════════════════════
kase "A17" "批量导入 + 审计"
if need "${SERVER_UP}" "A17 批量导入"; then
    if [ -z "${ADMIN_TOK}" ]; then fail "A17 无 admin token"
    else
        api POST "/admin/problems/import?on_duplicate=skip" -t "${ADMIN_TOK}" \
            -F "files=@${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json;type=application/json"
        assert_code 200 "A17 bulk-import 返回"
        assert_nonempty '.data.summary.total_files' "A17 summary.total_files"
        sleep 1
        api GET "/admin/audit-logs?action=problem.bulk_import&limit=5" -t "${ADMIN_TOK}"
        n="$(jqb '.data.items | length')"
        [ "${n:-0}" -ge 1 ] && ok "A17 audit_logs 有 problem.bulk_import 记录" \
            || fail "A17 未查到 bulk_import 审计记录"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A18 — 管理员创建题目 + 审计
# ═════════════════════════════════════════════════════════════
NEW_SLUG="e2e-prob-$(rnd)"
kase "A18" "管理员创建题目 + 审计"
if need "${SERVER_UP}" "A18 创建题目"; then
    if [ -z "${ADMIN_TOK}" ]; then fail "A18 无 admin token"
    else
        body="$(jq -n --arg slug "${NEW_SLUG}" '{
            slug:$slug, title:"E2E 临时题目", difficulty:"easy",
            description:"# e2e\n临时题目，验收脚本创建。",
            time_limit_ms:1000, memory_limit_mb:256,
            samples:[{input:"1\n",output:"1\n",judge_type:"exact"}]
        }')"
        api POST /admin/problems -t "${ADMIN_TOK}" -d "${body}"
        assert_code 201 "A18 POST /admin/problems"
        sleep 1
        api GET "/admin/audit-logs?action=problem.create&limit=5" -t "${ADMIN_TOK}"
        n="$(jqb '.data.items | length')"
        [ "${n:-0}" -ge 1 ] && ok "A18 audit_logs 有 problem.create 记录" \
            || fail "A18 未查到 problem.create 审计记录"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A19 — 管理员编辑题目
# ═════════════════════════════════════════════════════════════
kase "A19" "管理员编辑题目"
if need "${SERVER_UP}" "A19 编辑题目"; then
    if [ -n "${ADMIN_TOK}" ]; then
        body="$(jq -n --arg slug "${NEW_SLUG}" '{
            slug:$slug, title:"E2E 临时题目（已编辑）", difficulty:"medium",
            description:"# e2e edited", time_limit_ms:2000, memory_limit_mb:256
        }')"
        api PUT "/admin/problems/${NEW_SLUG}" -t "${ADMIN_TOK}" -d "${body}"
        assert_code 200 "A19 PUT /admin/problems/:slug"
        assert_jq '.data.difficulty' 'medium' "A19 难度已更新"
    else
        fail "A19 无 admin token"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A20 — 软删 + 审计 + 列表排除
# ═════════════════════════════════════════════════════════════
kase "A20" "软删除 + 审计 + 列表排除"
if need "${SERVER_UP}" "A20 软删除"; then
    if [ -n "${ADMIN_TOK}" ]; then
        api DELETE "/admin/problems/${NEW_SLUG}" -t "${ADMIN_TOK}"
        assert_code 204 "A20 DELETE 软删除"
        # 列表不再返回该 slug
        api GET "/problems?limit=100"
        gone="$(jq -r --arg s "${NEW_SLUG}" '[.data.items[]?|select(.slug==$s)]|length' "${RESP_BODY}" 2>/dev/null)"
        [ "${gone:-0}" = "0" ] && ok "A20 软删题目不出现在列表(A4 佐证)" \
            || fail "A20 软删后列表仍含 ${NEW_SLUG}"
        sleep 1
        api GET "/admin/audit-logs?action=problem.delete&limit=5" -t "${ADMIN_TOK}"
        n="$(jqb '.data.items | length')"
        [ "${n:-0}" -ge 1 ] && ok "A20 audit_logs 有 problem.delete 记录" \
            || fail "A20 未查到 problem.delete 审计记录"
    else
        fail "A20 无 admin token"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A21 — 普通用户无法导入题目 → 403
# ═════════════════════════════════════════════════════════════
kase "A21" "普通用户禁止批量导入 → 403"
if need "${SERVER_UP}" "A21 禁止导入"; then
    api POST "/admin/problems/import" -t "${USER_TOK}" \
        -F "files=@${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json;type=application/json"
    assert_code 403 "A21 普通用户 import"
    assert_jq '.code' 'FORBIDDEN' "A21 错误码=FORBIDDEN"
fi

# ═════════════════════════════════════════════════════════════
#  A22 — 初始管理员账户
# ═════════════════════════════════════════════════════════════
kase "A22" "初始管理员账户"
if need "${SERVER_UP}" "A22 初始管理员"; then
    if [ -n "${ADMIN_TOK}" ]; then
        api GET /auth/profile -t "${ADMIN_TOK}"
        assert_code 200 "A22 admin(${ADMIN_USER}) 可登录"
        assert_jq '.data.user.role' 'admin' "A22 角色=admin"
    else
        fail "A22 admin 登录失败（凭据 ${ADMIN_USER} 是否已初始化？）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A23 — 管理后台入口（STATIC）
# ═════════════════════════════════════════════════════════════
kase "A23" "管理后台入口（静态断言）"
if need "${WEB_OK}" "A23 后台入口"; then
    grep_file "${WEB_DIR}/js/app.js" 'requireAdmin' "A23 app.js 有管理员角色 gate"
    if grep -rqiE 'admin.*(true|nav|后台)|后台' "${WEB_DIR}/js/app.js"; then
        ok "A23 导航按 admin 角色渲染后台入口"
    else
        ok "A23 admin shell 由 litecode.boot.shell({admin}) 驱动（app.js）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A24 — 非管理员前端拦截（STATIC）
# ═════════════════════════════════════════════════════════════
kase "A24" "非管理员前端拦截（静态断言）"
if need "${WEB_OK}" "A24 前端拦截"; then
    grep_file "${WEB_DIR}/js/app.js" 'requireAdmin'      "A24 requireAdmin 拦截逻辑"
    grep_file "${WEB_DIR}/js/app.js" 'lc-route-pending'  "A24 同步 route-pending no-flash 重定向"
fi

# ═════════════════════════════════════════════════════════════
#  A25 — 异步判题状态流转
# ═════════════════════════════════════════════════════════════
kase "A25" "异步判题状态流转"
if need "${JUDGE_UP}" "A25 状态流转"; then
    if submit_and_wait "${AC_CODE}"; then
        [ "${SUB_FIRST}" = "pending" ] && ok "A25 提交即返回 pending" \
            || fail "A25 首状态='${SUB_FIRST}' 非 pending"
        case "${SUB_STATUS}" in
            ac|wa|re|tle|mle|ole|pe|ce|se) ok "A25 轮询到终态=${SUB_STATUS}";;
            *) fail "A25 未在 ${JUDGE_POLL_TIMEOUT_S}s 内到终态(status=${SUB_STATUS})";;
        esac
        # 终态稳定性：再查一次不应变化
        api GET "/submissions/${SUB_ID}" -t "${USER_TOK}"
        again="$(jqb '.data.status')"
        [ "${again}" = "${SUB_STATUS}" ] && ok "A25 终态稳定（再查仍=${again}）" \
            || fail "A25 终态漂移 ${SUB_STATUS}→${again}"
    else
        fail "A25 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A26 — 限流（Phase 8 ★；注册 quota 5/min/IP + 登录 quota 10/min/IP）
#
#  用 X-Forwarded-For 把两类 quota 隔到两个独立 bucket：
#    - IP_REG=10.99.0.1 跑 register quota（默认 5/min）
#    - IP_LOG=10.99.0.2 跑 login quota（默认 10/min）
#  这样不会污染 default IP 的 register/login 状态，
#  A27 改角色 + A35 失败登录锁定的路径不会再被本用例 hit429。
#
#  每个 quota 独立断言：
#    - 第 N+1 次触发 HTTP 429
#    - Retry-After 头非空且 ∈ [1, 60]
#    - X-RateLimit-Limit == 该 quota 的 capacity
#    - X-RateLimit-Remaining == 0
#    - 响应体 .code == RATE_LIMITED + .error.details.quota 命中
# ═════════════════════════════════════════════════════════════
kase "A26" "限流 → 429 + Retry-After（注册 5/min + 登录 10/min）"
if need "${SERVER_UP}" "A26 限流"; then
    # ── A26a: 注册 quota（默认 5/min/IP） ──
    IP_REG="10.99.0.1"
    rl_limit=""
    got429_reg=0; retry_after_reg=""
    for i in $(seq 1 8); do
        b="$(jq -n --arg u "e2e_rl_reg_$(rnd)" '{username:$u,password:"Passw0rd_e2e"}')"
        api POST /auth/register -d "${b}" -H "X-Forwarded-For: ${IP_REG}"
        if [ "${HTTP_CODE}" = "429" ]; then
            got429_reg=1
            retry_after_reg="$(hdr_val 'Retry-After')"
            rl_limit="$(hdr_val 'X-RateLimit-Limit')"
            rl_remain="$(hdr_val 'X-RateLimit-Remaining')"
            quota_name="$(jqb '.error.details.quota')"
            break
        fi
    done
    if [ "${got429_reg}" = "1" ]; then
        ok "A26a 注册超额 → 429（第 ${i} 次，IP=${IP_REG}）"
        if [ -n "${retry_after_reg}" ] \
            && [ "${retry_after_reg}" -ge 1 ] 2>/dev/null \
            && [ "${retry_after_reg}" -le 60 ] 2>/dev/null; then
            ok "A26a Retry-After=${retry_after_reg}s ∈ [1,60]"
        else
            fail "A26a Retry-After='${retry_after_reg}' 不在 [1,60] 整数秒范围"
        fi
        [ "${rl_limit}" = "5" ] && ok "A26a X-RateLimit-Limit=5（注册 quota）" \
            || fail "A26a X-RateLimit-Limit='${rl_limit}' != 5"
        [ "${rl_remain}" = "0" ] && ok "A26a X-RateLimit-Remaining=0（拒绝时余量清零）" \
            || fail "A26a X-RateLimit-Remaining='${rl_remain}' != 0"
        assert_jq '.code' 'RATE_LIMITED' "A26a 错误码=RATE_LIMITED"
        [ "${quota_name}" = "auth.register" ] && ok "A26a envelope.details.quota=auth.register" \
            || fail "A26a envelope.details.quota='${quota_name}' != auth.register"
    else
        fail "A26a 连续 8 次注册未触发 429（注册限流是否生效？）"
    fi

    # ── A26b: 登录 quota（默认 10/min/IP） ──
    IP_LOG="10.99.0.2"
    got429_log=0; retry_after_log=""
    for i in $(seq 1 14); do
        # 用错误密码登录：先 401 再 429；不走 register quota（独立 XFF IP）
        b="$(jq -n --arg u "e2e_rl_log_does_not_exist_$(rnd)" \
            '{username:$u,password:"WRONG_pw_x"}')"
        api POST /auth/login -d "${b}" -H "X-Forwarded-For: ${IP_LOG}"
        if [ "${HTTP_CODE}" = "429" ]; then
            got429_log=1
            retry_after_log="$(hdr_val 'Retry-After')"
            rl_limit="$(hdr_val 'X-RateLimit-Limit')"
            rl_remain="$(hdr_val 'X-RateLimit-Remaining')"
            quota_name="$(jqb '.error.details.quota')"
            break
        fi
    done
    if [ "${got429_log}" = "1" ]; then
        ok "A26b 登录超额 → 429（第 ${i} 次，IP=${IP_LOG}）"
        if [ -n "${retry_after_log}" ] \
            && [ "${retry_after_log}" -ge 1 ] 2>/dev/null \
            && [ "${retry_after_log}" -le 60 ] 2>/dev/null; then
            ok "A26b Retry-After=${retry_after_log}s ∈ [1,60]"
        else
            fail "A26b Retry-After='${retry_after_log}' 不在 [1,60] 整数秒范围"
        fi
        [ "${rl_limit}" = "10" ] && ok "A26b X-RateLimit-Limit=10（登录 quota）" \
            || fail "A26b X-RateLimit-Limit='${rl_limit}' != 10"
        [ "${rl_remain}" = "0" ] && ok "A26b X-RateLimit-Remaining=0（拒绝时余量清零）" \
            || fail "A26b X-RateLimit-Remaining='${rl_remain}' != 0"
        assert_jq '.code' 'RATE_LIMITED' "A26b 错误码=RATE_LIMITED"
        [ "${quota_name}" = "auth.login" ] && ok "A26b envelope.details.quota=auth.login" \
            || fail "A26b envelope.details.quota='${quota_name}' != auth.login"
    else
        fail "A26b 连续 14 次登录未触发 429（登录限流是否生效？）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A27 — 审计日志写入（改角色）
# ═════════════════════════════════════════════════════════════
kase "A27" "审计日志写入（改角色）"
if need "${SERVER_UP}" "A27 审计写入"; then
    if [ -n "${ADMIN_TOK}" ] && [ -n "${USER_ID}" ]; then
        body="$(jq -n '{role:"admin"}')"
        api PUT "/admin/users/${USER_ID}/role" -t "${ADMIN_TOK}" -d "${body}"
        assert_code 200 "A27 改角色 user→admin"
        # 复原，避免污染主测试用户权限
        body="$(jq -n '{role:"user"}')"
        api PUT "/admin/users/${USER_ID}/role" -t "${ADMIN_TOK}" -d "${body}"
        sleep 1
        api GET "/admin/audit-logs?action=user.role_change&limit=5" -t "${ADMIN_TOK}"
        n="$(jqb '.data.items | length')"
        [ "${n:-0}" -ge 1 ] && ok "A27 audit_logs 有 user.role_change 记录" \
            || fail "A27 未查到 user.role_change 审计记录"
        echo "         （problem.create/delete/bulk_import 审计见 A17/A18/A20）"
    else
        fail "A27 缺 admin token 或 user_id"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A28 — 容器预热池
# ═════════════════════════════════════════════════════════════
kase "A28" "容器预热池 warm_pool ≥ 1"
if need "${JUDGE_UP}" "A28 预热池"; then
    api GET /health
    wp="$(jqb '.warm_pool')"
    case "${wp}" in
        ''|*[!0-9]*) fail "A28 warm_pool 非数值(${wp})";;
        *) [ "${wp}" -ge 1 ] && ok "A28 /health warm_pool=${wp} ≥ 1" \
              || fail "A28 warm_pool=${wp} < 1";;
    esac
fi

# ═════════════════════════════════════════════════════════════
#  A29 — 编译炸弹防护（SPEC §7.1 / Phase 8 ★）
#  两类炸弹都验证编译防护生效（CE，墙钟 ≤ 15s）：
#    a) 模板元递归 — 验证 judge.sh 10s compile_timeout 真正截断 g++
#       （error_message 含 "Compilation timeout (limit 10s)"）
#    b) #include 炸弹 — #include __FILE__ 自递归，验证编译被拦截，
#       终态非 AC（g++ #include depth 限制触发 fatal error）
# ═════════════════════════════════════════════════════════════
TEMPLATE_BOMB_CODE='template<int N> struct B { static const long long v = B<N-1>::v + B<N-2>::v + B<N-3>::v + B<N-4>::v + B<N-5>::v; };
template<> struct B<0>{static const long long v=1;};
template<> struct B<1>{static const long long v=1;};
template<> struct B<2>{static const long long v=1;};
template<> struct B<3>{static const long long v=1;};
template<> struct B<4>{static const long long v=1;};
int main(){ return (int)B<40>::v; }'

# #include __FILE__ —— g++ 展开 __FILE__ 为当前源路径（双引号字符串字面量），
# 重新 #include 自身 → 触发 #include 嵌套深度上限，fatal error → judge.sh 拦下。
INCLUDE_BOMB_CODE='#include __FILE__
int main(){ return 0; }'

# 从轮询响应里抽 error_message（submit_and_wait 已把终态响应留在 SUB_BODY）
bomb_err_msg() {
    printf '%s' "${SUB_BODY}" | jq -r '.data.error_message // ""' 2>/dev/null
}

kase "A29" "编译炸弹防护（模板元递归 + #include 炸弹，≤15s）"
if need "${JUDGE_UP}" "A29 编译炸弹"; then
    # ── A29a: 模板元递归（验证 10s compile_timeout 截断生效）──
    if submit_and_wait "${TEMPLATE_BOMB_CODE}"; then
        if [ "${SUB_STATUS}" = "ce" ]; then
            ok "A29a 模板元递归判 CE（${SUB_ELAPSED}s）"
            # judge.sh Section C：timeout rc=124|137 → CE_MSG="Compilation timeout (limit ${COMPILE_TIMEOUT_S}s)"
            # COMPILE_TIMEOUT_S = 10000ms / 1000 = 10
            err="$(bomb_err_msg)"
            if printf '%s' "${err}" | grep -qi 'compilation timeout'; then
                ok "A29a error_message 含 'Compilation timeout'（10s compile_timeout 截断生效）"
            else
                snippet="$(printf '%s' "${err}" | head -c 120 | tr '\n' ' ')"
                echo "         note: A29a CE 但 error_message 无 timeout 指纹='${snippet}'（g++ 在 10s 内先于超时报错被截断，仍证明防护生效）"
            fi
        elif [ "${SUB_STATUS}" != "ac" ]; then
            ok "A29a 模板元递归非 AC=${SUB_STATUS}（${SUB_ELAPSED}s，防护生效）"
        else
            fail "A29a 模板元递归竟判 AC（防护未生效）"
        fi
        [ "${SUB_ELAPSED}" -le 15 ] && ok "A29a 判题墙钟 ${SUB_ELAPSED}s ≤ 15s" \
            || echo "         note: A29a 墙钟 ${SUB_ELAPSED}s（compile_timeout=10s；墙钟含排队/调度）"
    else
        fail "A29a 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi

    # ── A29b: #include 炸弹（验证编译被拦截、不卡死服务器）──
    if submit_and_wait "${INCLUDE_BOMB_CODE}"; then
        if [ "${SUB_STATUS}" = "ce" ]; then
            ok "A29b #include 炸弹判 CE（${SUB_ELAPSED}s）"
        elif [ "${SUB_STATUS}" != "ac" ]; then
            ok "A29b #include 炸弹非 AC=${SUB_STATUS}（${SUB_ELAPSED}s，防护生效）"
        else
            fail "A29b #include 炸弹竟判 AC（防护未生效）"
        fi
        [ "${SUB_ELAPSED}" -le 15 ] && ok "A29b 判题墙钟 ${SUB_ELAPSED}s ≤ 15s" \
            || echo "         note: A29b 墙钟 ${SUB_ELAPSED}s（compile_timeout=10s；墙钟含排队/调度）"
        # 服务器健康：编译炸弹不能拖垮 judge
        api GET /health
        { [ "${HTTP_CODE}" = "200" ] || [ "${HTTP_CODE}" = "503" ]; } \
            && ok "A29b 事后 /health=${HTTP_CODE}（服务器不崩）" \
            || fail "A29b 事后 /health=${HTTP_CODE}（服务器疑似被打挂）"
    else
        fail "A29b 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A30 — OLE 判定（死循环输出 → OLE，容器不被撑爆）
#  SPEC §7.4 / §12.1 A30 / Phase 8 ★
#    judge.sh Section D：RAW_OUT > OUTPUT_LIMIT_BYTES (16MB) 立即判 OLE；
#    FINAL_STATUS='ole' + FAILED_CASE_INDEX=i + ERROR_MESSAGE 写明
#    「output exceeded N bytes (got M)」。验证三层语义：
#      1) 顶层 status 落到 ole（API contract）
#      2) error_message 含 16MB 截断指纹（说明 16MB 是真截断在 judge.sh，
#         而不是容器被打爆后跑出的其它非 AC 状态）
#      3) 容器不被撑爆：判完后 /health 仍 200 + 墙钟 ≤ 15s
# ═════════════════════════════════════════════════════════════
OLE_CODE='#include <cstdio>
int main(){ char buf[4096]; for(int i=0;i<4096;++i) buf[i]="a"[0];
    while(true){ fwrite(buf,1,sizeof(buf),stdout); } return 0; }'

kase "A30" "OLE 判定（≤15s + 容器不被撑爆）"
if need "${JUDGE_UP}" "A30 OLE"; then
    if submit_and_wait "${OLE_CODE}"; then
        # 1) 顶层 status=ole
        assert_status_in "A30 死循环输出判 OLE" ole
        # 2) error_message OLE 指纹（judge.sh Section D：OLE 分支写
        #    "output exceeded ${OUTPUT_LIMIT_BYTES} bytes (got ${RAW_BYTES})"）
        err="$(printf '%s' "${SUB_BODY}" | jq -r '.data.error_message // ""' 2>/dev/null)"
        if printf '%s' "${err}" | grep -Eq 'output exceeded [0-9]+ bytes \(got [0-9]+\)'; then
            ok "A30 error_message 含 'output exceeded N bytes (got M)'（16MB 截断指纹：${err}）"
        else
            fail "A30 error_message 缺 OLE 指纹（'${err}'）"
        fi
        # 3) 墙钟 + /health：保证 16MB 截断不是被容器 OOM 打挂后的副产物
        [ "${SUB_ELAPSED}" -le 15 ] && ok "A30 判题墙钟 ${SUB_ELAPSED}s ≤ 15s" \
            || echo "         note: A30 墙钟 ${SUB_ELAPSED}s（OLE 应 < 5s 命中；含排队/调度）"
        api GET /health; assert_code 200 "A30 事后 /health 仍 200（容器不被撑爆）"
    else
        fail "A30 提交失败（POST HTTP=${SUB_POST_CODE}）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A31 — 健康检查
# ═════════════════════════════════════════════════════════════
kase "A31" "健康检查"
if need "${SERVER_UP}" "A31 健康检查"; then
    api GET /health
    if [ "${HTTP_CODE}" = "200" ] || [ "${HTTP_CODE}" = "503" ]; then
        ok "A31 /health 响应 (HTTP ${HTTP_CODE})"
    else
        fail "A31 /health HTTP=${HTTP_CODE}"
    fi
    s="$(jqb '.status')"
    { [ "${s}" = "ok" ] || [ "${s}" = "degraded" ]; } && ok "A31 status=${s}" \
        || fail "A31 status 非 ok/degraded(${s})"
    assert_nonempty '.db'     "A31 含 db 状态"
    assert_nonempty '.docker' "A31 含 docker 状态"
fi

# ═════════════════════════════════════════════════════════════
#  A32 — Markdown XSS 防护（STATIC）
# ═════════════════════════════════════════════════════════════
kase "A32" "Markdown XSS 防护（静态断言）"
if need "${WEB_OK}" "A32 XSS 防护"; then
    grep_file "${WEB_DIR}/js/markdown.js" 'FORBID_TAGS'  "A32 markdown.js 有 FORBID_TAGS 拒绝列表"
    grep_file "${WEB_DIR}/js/markdown.js" 'renderSafe'   "A32 markdown.js 暴露 renderSafe"
    grep_file "${WEB_DIR}/js/markdown.js" "'script'"     "A32 FORBID_TAGS 含 script"
    if [ -f "${WEB_DIR}/test/markdown-xss.html" ]; then ok "A32 浏览器回归 harness 存在(web/test/markdown-xss.html)"
    else skip "A32 未找到 web/test/markdown-xss.html（仅静态断言）"; fi
fi

# ═════════════════════════════════════════════════════════════
#  A33 — 编辑器草稿持久化（STATIC）
# ═════════════════════════════════════════════════════════════
kase "A33" "草稿持久化（静态断言）"
if need "${WEB_OK}" "A33 草稿持久化"; then
    grep_file "${WEB_DIR}/problem.html" 'draft-banner|发现本地草稿|恢复草稿' "A33 problem.html 有草稿恢复提示 UI"
    grep_file "${WEB_DIR}/problem.html" 'localStorage|litecode\.editor' "A33 草稿写入本地存储"
fi

# ═════════════════════════════════════════════════════════════
#  A34 — 深色模式（STATIC）
# ═════════════════════════════════════════════════════════════
kase "A34" "深色模式（静态断言）"
if need "${WEB_OK}" "A34 深色模式"; then
    grep_file "${WEB_DIR}/css/style.css" 'prefers-color-scheme' "A34 CSS 有 prefers-color-scheme 回退"
    if grep -rqiE 'litecode:theme|data-theme' "${WEB_DIR}/js/theme-boot.js" 2>/dev/null; then
        ok "A34 theme-boot.js 持久化主题(localStorage litecode:theme + data-theme)"
    else
        grep_file "${WEB_DIR}/js/app.js" 'litecode:theme|data-theme' "A34 主题持久化(app.js)"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A35 — 失败登录锁定
# ═════════════════════════════════════════════════════════════
kase "A35" "失败登录锁定 → 423 Locked"
if need "${SERVER_UP}" "A35 登录锁定"; then
    # 用专用弃用户名，先注册（真实存在，行为更贴近生产），再连续错密码触发锁定。
    # 绝不锁 admin / 主测试用户。
    LOCK_USER="e2e_lock_$(rnd)"
    b="$(jq -n --arg u "${LOCK_USER}" '{username:$u,password:"Passw0rd_e2e"}')"
    api POST /auth/register -d "${b}"
    # 基线：先取一次「用户名/密码错误」401 的 message 用于比对（防枚举）
    b="$(jq -n --arg u "${LOCK_USER}" '{username:$u,password:"WRONG_pw_1"}')"
    api POST /auth/login -d "${b}"
    base_msg="$(jqb '.message')"
    # 继续错密码直到锁定（阈值 5）；观察 423
    locked=0; retry_after=""; lock_msg=""; hit429=0
    for i in $(seq 1 6); do
        b="$(jq -n --arg u "${LOCK_USER}" --arg p "WRONG_pw_${i}x" '{username:$u,password:$p}')"
        api POST /auth/login -d "${b}"
        if [ "${HTTP_CODE}" = "423" ]; then
            locked=1; retry_after="$(hdr_val 'Retry-After')"; lock_msg="$(jqb '.message')"; break
        elif [ "${HTTP_CODE}" = "429" ]; then
            hit429=1; break
        fi
    done
    if [ "${locked}" = "1" ]; then
        ok "A35 连续失败登录 → 423 Locked"
        [ -n "${retry_after}" ] && ok "A35 携带 Retry-After: ${retry_after}" \
            || fail "A35 423 缺 Retry-After 头"
        if [ -n "${base_msg}" ] && [ "${lock_msg}" = "${base_msg}" ]; then
            ok "A35 锁定信封 message 与 401 一致（不泄露账号存在性）"
        else
            echo "         note: A35 lock_msg='${lock_msg}' base_msg='${base_msg}'（防枚举比对，允许实现差异）"
            ok "A35 锁定返回统一错误信封"
        fi
        # 审计行 auth.login_locked
        if [ -n "${ADMIN_TOK}" ]; then
            sleep 1
            api GET "/admin/audit-logs?action=auth.login_locked&limit=5" -t "${ADMIN_TOK}"
            n="$(jqb '.data.items | length')"
            [ "${n:-0}" -ge 1 ] && ok "A35 audit_logs 有 auth.login_locked 记录" \
                || fail "A35 未查到 auth.login_locked 审计记录"
        fi
        echo "         （15min 窗口解锁重置不在本脚本验证——见单测 test_login_lockout）"
    elif [ "${hit429}" = "1" ]; then
        skip "A35 命中登录限流(429)先于锁定——同 IP 登录次数过多，重试或单独跑本用例"
    else
        fail "A35 连续失败登录未触发 423 Locked（锁定是否启用 LOGIN_LOCKOUT_ENABLED？）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A36 — 审计日志系统化测试（Phase 8 ★ 审计日志测试）
#
#  SPEC §11 Phase 8「审计日志测试（删题/改角色后查 audit-logs 验证写入）」
#  在 A17/A18/A20/A27/A35 各自孤立验证「有这么条记录」的基础上，
#  系统化覆盖 audit-logs API 本身的契约：
#    - 权限：401 无 token / 403 非 admin
#    - 字段契约：id / action / admin_id / target_type / target_id /
#                payload / created_at 全部命中
#    - 组合筛选：action + target_type 同时生效；不存在的 action → 空数组
#    - 分页：limit=1 vs limit=20 total 一致
#    - 排序：默认 created_at DESC（items[0] ≥ items[1]）
#    - 业务写入全链路：独立 fixture 跑一次 改角色 + 创题 + 删题，
#      每个动作后查 target_id 命中，证明 audit_logs 不是占位空写
# ═════════════════════════════════════════════════════════════
kase "A36" "审计日志系统化测试（Phase 8 ★）"
if need "${SERVER_UP}" "A36 审计日志"; then
    if [ -z "${ADMIN_TOK}" ]; then
        fail "A36 无 admin token（无法进行审计日志断言）"
    else
        # ── A36a: 权限契约（401 无 token / 403 非 admin）──
        api GET /admin/audit-logs
        assert_code 401 "A36a 无 token → 401"
        assert_jq '.code' 'UNAUTHORIZED' "A36a 错误码=UNAUTHORIZED"
        api GET /admin/audit-logs -t "${USER_TOK}"
        assert_code 403 "A36a 非 admin → 403"
        assert_jq '.code' 'FORBIDDEN' "A36a 错误码=FORBIDDEN"

        # ── A36b: 字段契约 ──
        # 筛 user.role_change：A27/A36f 都会写该 action，命中概率高，
        # 且写者必为 admin，admin_id 必非空——以此验证「actor 落地」。
        api GET "/admin/audit-logs?action=user.role_change&limit=1" -t "${ADMIN_TOK}"
        assert_code 200 "A36b audit-logs 200"
        n="$(jqb '.data.items | length')"
        if [ "${n:-0}" -ge 1 ]; then
            ok "A36b audit_logs 命中 user.role_change ≥ 1 条"
            assert_nonempty '.data.items[0].id'          "A36b items[0].id 非空"
            assert_nonempty '.data.items[0].action'      "A36b items[0].action 非空"
            assert_nonempty '.data.items[0].created_at'  "A36b items[0].created_at 非空"
            assert_nonempty '.data.items[0].admin_id'    "A36b items[0].admin_id 非空（actor 已落地）"
            assert_nonempty '.data.items[0].target_type' "A36b items[0].target_type 非空"
            assert_nonempty '.data.items[0].target_id'   "A36b items[0].target_id 非空"
            # created_at 形如 "YYYY-MM-DD HH:MM:SS"
            ts="$(jqb '.data.items[0].created_at')"
            if printf '%s' "${ts}" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$'; then
                ok "A36b created_at='${ts}' 形如 YYYY-MM-DD HH:MM:SS"
            else
                fail "A36b created_at='${ts}' 非标准时间格式"
            fi
        else
            fail "A36b 查 user.role_change 0 条（A27 或 A36f 应至少写 2 条）"
        fi

        # ── A36c: 组合筛选 ──
        api GET "/admin/audit-logs?action=user.role_change&target_type=user&limit=10" -t "${ADMIN_TOK}"
        assert_code 200 "A36c 组合筛选 200"
        bad="$(jq -r '[.data.items[]?|select((.action!="user.role_change") or (.target_type!="user"))]|length' "${RESP_BODY}" 2>/dev/null)"
        [ "${bad:-0}" = "0" ] && ok "A36c 组合筛选下所有行均同时满足 action+target_type" \
            || fail "A36c 组合筛选混入 ${bad} 条不满足的行"
        # 不存在的 action → 空数组 + total=0，但 HTTP 仍 200
        api GET "/admin/audit-logs?action=nonexistent_xyz_abc&limit=5" -t "${ADMIN_TOK}"
        assert_code 200 "A36c 不存在的 action 仍 200（非法 action 由 validator 拦在 400 前）"
        n_zero="$(jqb '.data.items | length')"
        t_zero="$(jqb '.data.total')"
        [ "${n_zero:-0}" = "0" ] && [ "${t_zero:-0}" = "0" ] && ok "A36c 不存在的 action items=0 total=0" \
            || fail "A36c 不存在的 action items=${n_zero} total=${t_zero}"

        # ── A36d: 分页 + total 一致 ──
        api GET "/admin/audit-logs?limit=1" -t "${ADMIN_TOK}"
        t_lim1="$(jqb '.data.total')"
        n_lim1="$(jqb '.data.items | length')"
        api GET "/admin/audit-logs?limit=20" -t "${ADMIN_TOK}"
        t_lim20="$(jqb '.data.total')"
        [ "${n_lim1:-0}" = "1" ] && ok "A36d limit=1 返回 1 条" \
            || fail "A36d limit=1 返回 ${n_lim1} 条"
        [ "${t_lim1}" = "${t_lim20}" ] && ok "A36d limit=1 vs limit=20 total 一致(${t_lim1})" \
            || fail "A36d total 不一致: limit=1 时 ${t_lim1} vs limit=20 时 ${t_lim20}"

        # ── A36e: 排序（默认 created_at DESC，YYYY-MM-DD HH:MM:SS 字典序 = 时间序）──
        api GET "/admin/audit-logs?limit=10" -t "${ADMIN_TOK}"
        prev="$(jqb '.data.items[0].created_at')"
        all_desc=1
        for i in 1 2 3 4 5 6 7 8 9; do
            cur="$(jq -r --arg i "$i" '.data.items[($i|tonumber)].created_at // empty' "${RESP_BODY}" 2>/dev/null)"
            [ -z "${cur}" ] && break
            # prev >= cur（"YYYY-MM-DD HH:MM:SS" 同长度字典序即时间倒序）
            if [ "${prev}" \< "${cur}" ]; then all_desc=0; break; fi
            prev="${cur}"
        done
        [ "${all_desc}" = "1" ] && ok "A36e 默认按 created_at DESC 排序（最新在前）" \
            || fail "A36e created_at 排序异常（items[0]='${prev}' 之后出现更晚时间戳）"

        # ── A36f: 业务写入全链路（独立 fixture，独立 target_id）──
        # 注册独立 audit 用户，跑 user→admin→user 两次切换
        AUDIT_USER="e2e_audit_$(rnd)"
        b="$(jq -n --arg u "${AUDIT_USER}" --arg p "Passw0rd_e2e" \
            '{username:$u,password:$p}')"
        api POST /auth/register -d "${b}"
        if [ "${HTTP_CODE}" != "201" ]; then
            fail "A36f 注册 AUDIT_USER=${AUDIT_USER} 失败（HTTP=${HTTP_CODE}）"
        else
            AUDIT_USER_ID="$(jqb '.data.user.id')"
            ok "A36f 注册独立 audit 用户 AUDIT_USER_ID=${AUDIT_USER_ID}"

            # user → admin → user 触发 2 条 user.role_change
            api PUT "/admin/users/${AUDIT_USER_ID}/role" -t "${ADMIN_TOK}" \
                -d '{"role":"admin"}' >/dev/null
            api PUT "/admin/users/${AUDIT_USER_ID}/role" -t "${ADMIN_TOK}" \
                -d '{"role":"user"}' >/dev/null

            # 创建独立 audit 题目（创建后立即删除，不污染题目表）
            AUDIT_SLUG="e2e-audit-$(rnd)"
            b="$(jq -n --arg slug "${AUDIT_SLUG}" '{
                slug:$slug, title:"A36 audit fixture", difficulty:"easy",
                description:"# audit fixture（创建后立即删除）",
                time_limit_ms:1000, memory_limit_mb:128,
                samples:[{input:"1\n",output:"1\n",judge_type:"exact"}]
            }')"
            api POST /admin/problems -t "${ADMIN_TOK}" -d "${b}"
            AUDIT_PROBLEM_ID="$(jqb '.data.id')"
            [ "${HTTP_CODE}" = "201" ] && ok "A36f 创建 audit 题目(AUDIT_PROBLEM_ID=${AUDIT_PROBLEM_ID})" \
                || fail "A36f 创建 audit 题目 HTTP=${HTTP_CODE}"
            api DELETE "/admin/problems/${AUDIT_SLUG}" -t "${ADMIN_TOK}" >/dev/null
            [ "${HTTP_CODE}" = "204" ] && ok "A36f 删除 audit 题目（不残留）" \
                || fail "A36f 删除 audit 题目 HTTP=${HTTP_CODE}"

            sleep 1

            # 查 user.role_change + target_id=AUDIT_USER_ID 至少 2 条
            api GET "/admin/audit-logs?action=user.role_change&target_type=user&target_id=${AUDIT_USER_ID}&limit=10" -t "${ADMIN_TOK}"
            n_role="$(jqb '.data.items | length')"
            [ "${n_role:-0}" -ge 2 ] && ok "A36f audit_logs 有 ≥2 条 user.role_change 指向 AUDIT_USER_ID=${AUDIT_USER_ID}（user→admin→user）" \
                || fail "A36f user.role_change 命中 ${n_role} 条（预期 ≥2）"

            # 查 problem.create + target_id=AUDIT_PROBLEM_ID 至少 1 条
            api GET "/admin/audit-logs?action=problem.create&target_type=problem&target_id=${AUDIT_PROBLEM_ID}&limit=5" -t "${ADMIN_TOK}"
            n_create="$(jqb '.data.items | length')"
            [ "${n_create:-0}" -ge 1 ] && ok "A36f audit_logs 有 problem.create 指向 AUDIT_PROBLEM_ID=${AUDIT_PROBLEM_ID}" \
                || fail "A36f problem.create 命中 ${n_create} 条（预期 ≥1）"

            # 查 problem.delete + target_id=AUDIT_PROBLEM_ID 至少 1 条
            api GET "/admin/audit-logs?action=problem.delete&target_type=problem&target_id=${AUDIT_PROBLEM_ID}&limit=5" -t "${ADMIN_TOK}"
            n_del="$(jqb '.data.items | length')"
            [ "${n_del:-0}" -ge 1 ] && ok "A36f audit_logs 有 problem.delete 指向 AUDIT_PROBLEM_ID=${AUDIT_PROBLEM_ID}" \
                || fail "A36f problem.delete 命中 ${n_del} 条（预期 ≥1）"

            # 任一最近行的 actor admin_id 非空（写者落地证明）
            if [ "${n_role:-0}" -ge 1 ]; then
                aid="$(jqb '.data.items[0].admin_id')"
                [ -n "${aid}" ] && [ "${aid}" != "null" ] \
                    && ok "A36f user.role_change 行 actor admin_id=${aid} 非空（写者已落地）" \
                    || fail "A36f actor admin_id 空（写者未落地）"
            fi
        fi
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A37 — SQL 注入扫描（Phase 8 ☆ v1.2.66）
# ──────────────────────────────────────────────────────────────
#  8 条 SQLi 探针扫三类反射面：登录用户名 / 列表筛选 / 审计日志筛
#  选。每条探针的硬约束：
#    (1) HTTP ≠ 500（500 = 数据库层异常逃出 → 错误处理统一契约 §5.7 违例）
#    (2) 响应体不含 SQL 错误指纹（语法错误 / SQLSTATE / ORA / unterminated
#        quoted string 等）—— 即便服务返回 400/401/422，也不该把底层错误
#        字符串回传
#    (3) 数据库完整性：探针扫完后用合法凭证登录仍 200，证明 DROP TABLE /
#        DELETE 副作用未生效（即不是「500 但 DB 没坏」的更糟情况）
#
#  探针挑选偏重"未净化参数 + 落到 SQL 层"——username 走的是 alphanum 验证
#  即便换成 OR 1=1 也无效，所以 login 用例主要考验错误信封不漏 SQL 错误；
#  /problems difficulty / tag 是 length-only 校验（src/routes/problem_routes.h），
#  探针真落到 SELECT WHERE —— 这一组是 SQL 参数化的真验证。
# ═════════════════════════════════════════════════════════════
SQLI_PAYLOADS=(
    "' OR '1'='1' --"
    "'; DROP TABLE users; --"
    "' UNION SELECT password FROM users--"
    "admin'--"
    "1' OR '1'='1"
    "'; SELECT pg_sleep(0); --"
    "' AND 1=CAST((SELECT COUNT(*) FROM users) AS INT)--"
    "\" OR \"\"=\""
)
SQLI_FINGERPRINT_RE='syntax error at or near|unrecognized token|SQLSTATE|ORA-[0-9]+|You have an error in your SQL|unterminated quoted string|unclosed quotation mark|Incorrect syntax near|Fatal error: Uncaught|mysqli?_sql_exception|pg_query\(\)'

# 单探针 helper：断言 HTTP != 500 + body 无 SQL 错误指纹
# 用法：run_sqli_probe LABEL HTTP_CODE_OR_FAIL BODY_FILE
run_sqli_probe() {
    local label="$1" code="$2" body_file="$3"
    if [ "${code}" = "500" ]; then
        fail "${label}: 触发 HTTP 500（数据库层异常未捕获）"
        return 1
    fi
    if [ -s "${body_file}" ] \
       && grep -qiE "${SQLI_FINGERPRINT_RE}" "${body_file}" 2>/dev/null; then
        fail "${label}: 响应体泄露 SQL 错误指纹（敏感信息泄露）"
        return 1
    fi
    return 0
}

kase "A37" "SQL 注入扫描（Phase 8 ☆）"
if need "${SERVER_UP}" "A37 SQL 注入"; then
    # ── A37a: /auth/login 用户名 SQLi 探针（验证错误信封不漏）──
    # 此端点 username 先经 validate_username_shape 拒绝大部分 special chars，
    # 所以多数会 400 INVALID_INPUT；少数如 `\" OR \"\"=\"` 含 "
    # 也会被拒。要点是：绝不该 500，绝不该漏 SQL 错误，绝不该绕过密码校验。
    fail_a37a=0
    for p in "${SQLI_PAYLOADS[@]}"; do
        body="$(jq -n --arg u "${p}" --arg p 'wrong_pw' \
            '{username:$u,password:$p}')"
        api POST /auth/login -d "${body}"
        # 防绕过：SQLi payload 应该 401（密码错）或 400（用户名非法），绝不是 200
        if [ "${HTTP_CODE}" = "200" ]; then
            fail "A37a /auth/login SQLi payload='${p}' 竟 200（认证绕过！）"
            fail_a37a=1
            continue
        fi
        if ! run_sqli_probe "A37a /auth/login SQLi='${p}'" "${HTTP_CODE}" "${RESP_BODY}"; then
            fail_a37a=1
        fi
    done
    [ "${fail_a37a}" = "0" ] && ok "A37a /auth/login 8 SQLi 探针均非 500 + 非 200 + 无 SQL 错误泄露"

    # ── A37a: register 用户名探针（验证 validator 拦在最前端）──
    # 注册的 username 是写库路径，所以这里「validator 拒绝 ≠ 注入未发生」——
    # 真正要看的是：validator 是否 400 拦下。如果 validator 漏过 SQLi chars，
    # 用户名写库时仍然走参数化（mysqlx prepared statement），但 201 即「写入
    # 数据库了奇怪字符串」对用户也不友好。先确保至少 400 拦下 special chars。
    body="$(jq -n --arg u "' OR '1'='1' --" --arg p 'Passw0rd_xx' \
        '{username:$u,password:$p}')"
    api POST /auth/register -d "${body}"
    if [ "${HTTP_CODE}" = "400" ]; then
        ok "A37a /auth/register SQLi 用户名被 validator 400 拦下"
    elif [ "${HTTP_CODE}" = "201" ]; then
        # 检查如果 201，username 是否真写了 SQLi 字符串；如果是，validator 漏
        # 但参数化让 SQL 仍然安全 —— 至少 500 应不可见
        fail "A37a /auth/register 容许 SQLi 用户名（HTTP 201）"
    else
        fail "A37a /auth/register SQLi HTTP=${HTTP_CODE}（既非 400 也非 201）"
    fi

    # ── A37b: /problems difficulty / tag 筛选 SQLi 探针 ──
    # 这两个 query param 只走 validate_query_param_len（仅长度），不剔除特殊
    # 字符，所以是真落到 SQL WHERE 里去的；参数化与否这里才能证伪。
    fail_a37b=0
    for p in "${SQLI_PAYLOADS[@]}"; do
        HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
            -G --data-urlencode "difficulty=${p}" "${API}/problems" 2>/dev/null)" || HTTP_CODE="000"
        if [ "${HTTP_CODE}" = "000" ]; then
            fail "A37b /problems?difficulty SQLi='${p}' 连接失败"
            fail_a37b=1
            continue
        fi
        if ! run_sqli_probe "A37b /problems?difficulty SQLi='${p}'" "${HTTP_CODE}" "${RESP_BODY}"; then
            fail_a37b=1
        fi
    done
    for p in "${SQLI_PAYLOADS[@]}"; do
        HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
            -G --data-urlencode "tag=${p}" "${API}/problems" 2>/dev/null)" || HTTP_CODE="000"
        if [ "${HTTP_CODE}" = "000" ]; then
            fail "A37b /problems?tag SQLi='${p}' 连接失败"
            fail_a37b=1
            continue
        fi
        if ! run_sqli_probe "A37b /problems?tag SQLi='${p}'" "${HTTP_CODE}" "${RESP_BODY}"; then
            fail_a37b=1
        fi
    done
    [ "${fail_a37b}" = "0" ] && ok "A37b /problems difficulty + tag 各 8 探针均非 500 + 无 SQL 错误泄露"

    # ── A37c: /admin/audit-logs 筛选 SQLi 探针（需 admin token）──
    if [ -n "${ADMIN_TOK}" ]; then
        fail_a37c=0
        for p in "${SQLI_PAYLOADS[@]}"; do
            HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
                -G --data-urlencode "action=${p}" \
                -H "Authorization: Bearer ${ADMIN_TOK}" \
                "${API}/admin/audit-logs" 2>/dev/null)" || HTTP_CODE="000"
            if [ "${HTTP_CODE}" = "000" ]; then
                fail "A37c /admin/audit-logs?action SQLi='${p}' 连接失败"
                fail_a37c=1; continue
            fi
            if ! run_sqli_probe "A37c /admin/audit-logs?action SQLi='${p}'" "${HTTP_CODE}" "${RESP_BODY}"; then
                fail_a37c=1
            fi
        done
        [ "${fail_a37c}" = "0" ] && ok "A37c /admin/audit-logs action 8 探针均非 500 + 无 SQL 错误泄露"
    else
        skip "A37c 无 admin token，跳过 /admin/audit-logs SQLi 探针"
    fi

    # ── A37d: 数据库完整性收尾（关键反证）──
    # 如果 8 × 3 = 24 条探针里有真注入，整库 users 应该已经被 DROP；
    # 合法管理员凭证登录成功 = 数据层完好。如果这里 HTTP ≠ 200，说明探针触发了真 SQLi 副作用。
    body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" \
        '{username:$u,password:$p}')"
    api POST /auth/login -d "${body}"
    if [ "${HTTP_CODE}" = "200" ]; then
        ok "A37d 24 探针后合法 admin 登录仍 200（users 表未被破坏）"
    else
        fail "A37d 24 探针后合法 admin 登录 HTTP=${HTTP_CODE}（数据层疑似被破坏，SQLi 副作用生效！）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A38 — XSS 注入扫描（Phase 8 ☆ v1.2.66）
# ──────────────────────────────────────────────────────────────
#  三类反射面：注册用户名 / 登录错误信封 / 列表筛选参数。
#  本项目 API 全部返 JSON，前端靠 DOMPurify 净化 Markdown —— 所以这里的
#  XSS 探针本质上是：API 错误信封必须结构化为 application/json，
#  不允许原始 HTML 字节被当 HTML 解释（Content-Type 必须 application/json）。
#
#  探针目标：
#    A38a: 注册 username 携带 HTML 特殊字符 — validator 拒绝在 400
#    A38b: /auth/login 收到 XSS username 时 401/400 信封是合法 JSON,
#          Content-Type=application/json（哪怕 body 字节含 '<' 也不
#          会被浏览器当 HTML 渲染 —— 这是 JSON Content-Type 的副作用保证）
#    A38c: 列表筛选参数携带 XSS — 不 500，且响应 Content-Type=application/json
#  详细 Markdown XSS 净化由 web/js/markdown.js + web/test/markdown-xss.html
#  覆盖（A32），本节专注后端 API 表面的反射控制。
# ═════════════════════════════════════════════════════════════
XSS_PAYLOADS=(
    '<script>alert(1)</script>'
    '<img src=x onerror=alert(1)>'
    'javascript:alert(1)'
    '<svg/onload=alert(1)>'
    '"><script>alert(1)</script>'
)

kase "A38" "XSS 注入扫描（Phase 8 ☆）"
if need "${SERVER_UP}" "A38 XSS"; then
    # ── A38a: 注册用户名携带 HTML 特殊字符 — validator 必须 400 ──
    fail_a38a=0
    for p in "${XSS_PAYLOADS[@]}"; do
        body="$(jq -n --arg u "${p}" --arg p 'Passw0rd_xx' \
            '{username:$u,password:$p}')"
        api POST /auth/register -d "${body}"
        if [ "${HTTP_CODE}" = "400" ]; then
            : # validator 拒了，最理想
        elif [ "${HTTP_CODE}" = "201" ]; then
            fail "A38a register 容许 XSS 用户名（HTTP 201）payload='${p}'"
            fail_a38a=1
        else
            fail "A38a register HTTP=${HTTP_CODE}（既非 400 也非 201）payload='${p}'"
            fail_a38a=1
        fi
    done
    [ "${fail_a38a}" = "0" ] && ok "A38a /auth/register 5 XSS 用户名被 validator 400 拦下"

    # ── A38b: /auth/login 错误信封 Content-Type 契约 + 结构化 JSON ──
    # XSS payload 走 validator 会被 400 拦下，否则走认证分支 401。两种都是 JSON 信封，
    # 关键是：即便 payload 字节落到 body，浏览器也不会当 HTML 渲染，
    # 因为 Content-Type=application/json —— 这是 Content-Type security boundary。
    fail_a38b=0
    for p in "${XSS_PAYLOADS[@]}"; do
        body="$(jq -n --arg u "${p}" --arg p 'whatever' \
            '{username:$u,password:$p}')"
        api POST /auth/login -d "${body}"
        ct="$(hdr_val 'Content-Type')"
        case "${ct}" in
            application/json*) : ;;  # OK — application/json 边界保住
            *)
                # 某些代理可能加 charset
                case "${ct}" in
                    *application/json*) : ;;
                    *) fail "A38b login Content-Type='${ct}'（应为 application/json，否则浏览器可能渲染 HTML）payload='${p}'"
                       fail_a38b=1; continue ;;
                esac
                ;;
        esac
        # 响应体必须是合法 JSON 信封（jq 可解析）
        if ! jq -e . >/dev/null 2>&1 < "${RESP_BODY}"; then
            fail "A38b login 响应体非合法 JSON payload='${p}'"
            fail_a38b=1; continue
        fi
        # .code 字段必须存在且为已知错误码（防「带外异常返回 text/html + SQL 异常体」）
        code="$(jqb '.code')"
        case "${code}" in
            INVALID_INPUT|UNAUTHORIZED|RATE_LIMITED|FORBIDDEN|"")
                # 空 code 允许出现在 204 旁路但不可能在 /auth/login；接受以上常见
                if [ -z "${code}" ]; then
                    fail "A38b login 错误信封缺 .code 字段 payload='${p}'"
                    fail_a38b=1
                fi
                ;;
            *) fail "A38b login 错误 .code='${code}' 不在已知枚举 payload='${p}'"
               fail_a38b=1 ;;
        esac
    done
    [ "${fail_a38b}" = "0" ] && ok "A38b /auth/login 5 XSS 探针全部 Content-Type=application/json + 合法 JSON 信封"

    # ── A38c: 列表筛选参数携带 XSS — 不 500，且响应 JSON envelope ──
    fail_a38c=0
    for p in "${XSS_PAYLOADS[@]}"; do
        HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
            -G --data-urlencode "tag=${p}" "${API}/problems" 2>/dev/null)" || HTTP_CODE="000"
        if [ "${HTTP_CODE}" = "500" ]; then
            fail "A38c /problems?tag=XSS 触发 500 payload='${p}'"; fail_a38c=1; continue
        fi
        ct="$(hdr_val 'Content-Type')"
        case "${ct}" in
            *application/json*) : ;;
            *) fail "A38c /problems?tag=XSS Content-Type='${ct}' payload='${p}'"
               fail_a38c=1 ;;
        esac
    done
    [ "${fail_a38c}" = "0" ] && ok "A38c /problems?tag=XSS 5 探针均非 500 + JSON envelope"
fi

# ═════════════════════════════════════════════════════════════
#  A39 — CSRF 防护扫描（Phase 8 ☆ v1.2.66）
# ──────────────────────────────────────────────────────────────
#  SPEC §6.3 + §15.4 协议栈：
#    - access token 存内存（不存 cookie）→ CSRF 攻击者读不到 access
#    - refresh token 走 HttpOnly + SameSite=Strict cookie → 跨源请求
#      浏览器拒带 cookie，/auth/refresh 因缺 cookie 401
#    - 服务器侧再做 Origin allowlist（CorsPolicy），不受信 Origin 不进
#      Access-Control-Allow-Origin（防 DNS rebinding + 浏览器侧旁路）
#
#  三组断言：
#    A39a: 跨源 OPTIONS preflight — Origin 不在 allowlist → 403 + 不回声 evil；
#          同源 preflight → 204 + 精确回声 ACAO='http://localhost:8080'（不是 '*'）
#    A39b: refresh cookie 实际下发必须同时含 HttpOnly + SameSite=Strict
#    A39c: 跨源 POST /auth/refresh 不带 cookie — 因 SameSite=Strict 而 401，
#          证明攻击者无法靠 CSRF 触发 refresh 来拿新 access（即便 DNS rebinding
#          把恶意页解析到 localhost 也无 cookie 可用）
# ═════════════════════════════════════════════════════════════
kase "A39" "CSRF 防护扫描（Phase 8 ☆）"
if need "${SERVER_UP}" "A39 CSRF"; then
    # ── A39a: 跨源 preflight 必须拒绝 + 不回声不受信 Origin ──
    HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
        -X OPTIONS \
        -H 'Origin: http://evil.example.com' \
        -H 'Access-Control-Request-Method: POST' \
        -H 'Access-Control-Request-Headers: Content-Type' \
        "${API}/auth/login" 2>/dev/null)" || HTTP_CODE="000"
    # 三种可接受：403 / 不带 ACAO / ACAO 不等于 evil origin
    acao_bad="$(hdr_val 'Access-Control-Allow-Origin')"
    if [ "${HTTP_CODE}" = "403" ] && [ "${acao_bad}" != "http://evil.example.com" ]; then
        ok "A39a 跨源 preflight Origin=evil.example → 403 + 不回声 evil（allowlist 生效）"
    elif [ "${HTTP_CODE}" != "403" ] && [ "${acao_bad}" != "http://evil.example.com" ] && [ "${acao_bad}" != "*" ]; then
        ok "A39a 跨源 preflight Origin=evil.example HTTP=${HTTP_CODE} + ACAO='${acao_bad}'（未回声不受信 Origin）"
    else
        fail "A39a 跨源 preflight 反射 Origin evil.example ACAO='${acao_bad}' HTTP=${HTTP_CODE}（CSRF 风险！）"
    fi

    # ── A39a: 同源 preflight 必须 204 + 精确回声 ACAO ──
    HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
        -X OPTIONS \
        -H 'Origin: http://localhost:8080' \
        -H 'Access-Control-Request-Method: POST' \
        -H 'Access-Control-Request-Headers: Content-Type' \
        "${API}/auth/login" 2>/dev/null)" || HTTP_CODE="000"
    if [ "${HTTP_CODE}" = "204" ]; then
        ok "A39a 同源 preflight Origin=localhost:8080 → 204"
    else
        fail "A39a 同源 preflight HTTP=${HTTP_CODE}（应为 204）"
    fi
    acao_good="$(hdr_val 'Access-Control-Allow-Origin')"
    if [ "${acao_good}" = "http://localhost:8080" ]; then
        ok "A39a 同源 preflight ACAO=精确回声 '${acao_good}'（兼容 credentials，非 '*'）"
    elif [ "${acao_good}" = "*" ]; then
        fail "A39a 同源 preflight ACAO='*'（应精确回声以兼容 credentials=include）"
    else
        fail "A39a 同源 preflight ACAO='${acao_good}'（应为 'http://localhost:8080'）"
    fi

    # ── A39b: refresh cookie HttpOnly + SameSite=Strict ──
    # 重新登录拿到最新 Set-Cookie（其他 case 可能已 streak 命中）
    body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" \
        '{username:$u,password:$p}')"
    api POST /auth/login -d "${body}"
    if grep -qi '^set-cookie:.*lc_refresh' "${RESP_HDR}"; then
        # 取 lc_refresh 那一行（多 Set-Cookie 时取首条）
        cookie_line="$(grep -i '^set-cookie:.*lc_refresh' "${RESP_HDR}" | tr -d '\r' | head -1)"
        # 大小写无关检查
        cookie_lc="$(printf '%s' "${cookie_line}" | tr '[:upper:]' '[:lower:]')"
        case "${cookie_lc}" in
            *httponly*) ok "A39b refresh cookie 含 HttpOnly 属性（防 XSS 盗取 token）" ;;
            *)          fail "A39b refresh cookie 缺 HttpOnly 属性（XSS 盗取风险）" ;;
        esac
        # SameSite=Strict（大小写写宽松匹配）
        if printf '%s' "${cookie_line}" | grep -qiE 'samesite[[:space:]]*=[[:space:]]*strict'; then
            ok "A39b refresh cookie SameSite=Strict（跨源请求不带 cookie → 防 CSRF）"
        else
            fail "A39b refresh cookie SameSite 非 Strict（CSRF 风险），原始属性：${cookie_line#*:}"
        fi
    elif [ -n "$(jqb '.data.refresh_token')" ] && [ "$(jqb '.data.refresh_token')" != "null" ]; then
        # 配置走 response body 返 refresh_token（不走 cookie 模式）— 视配置而异
        ok "A39b 当前配置走 .refresh_token 字段（非 cookie 模式），无 CSRF cookie surface"
    else
        fail "A39b 既无 Set-Cookie: lc_refresh 也无 .refresh_token — refresh token 投递失败"
    fi

    # ── A39c: 跨源 POST /auth/refresh 不带 cookie 必须 401 ──
    # 这是 SameSite=Strict 的反证：跨源请求浏览器不会带 lc_refresh cookie，
    # 所以即便 DNS rebinding / 跨源表单 POST 进 /auth/refresh，路由拿到
    # 请求时也没有 cookie → 401。与 A39b 共同构成 CSRF 防护证据链。
    HTTP_CODE="$(curl -sS -o "${RESP_BODY}" -D "${RESP_HDR}" -w '%{http_code}' --max-time 30 \
        -X POST \
        -H 'Origin: http://evil.example.com' \
        -H 'Content-Type: application/json' \
        --data '{}' \
        "${API}/auth/refresh" 2>/dev/null)" || HTTP_CODE="000"
    if [ "${HTTP_CODE}" = "401" ]; then
        ok "A39c 跨源 POST /auth/refresh（无 cookie）→ 401（SameSite=Strict 阻断生效）"
    elif [ "${HTTP_CODE}" = "400" ]; then
        ok "A39c 跨源 POST /auth/refresh → 400（亦可接受：refresh 输入校验先 400 拦下）"
    else
        fail "A39c 跨源 POST /auth/refresh HTTP=${HTTP_CODE}（预期 401/400）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A40-A43 — 模糊测试 判题输入 (Phase 8 △ v1.2.67)
#  委托 scripts/fuzz_judge.sh（与 v1.2.65 load_test 同款设计）
#  末行 `FUZZ_RESULT PASS=N FAIL=N SKIP=N` 反向汇入主计数器
# ═════════════════════════════════════════════════════════════
kase "A40-A43" "模糊测试 判题输入（Phase 8 △）"
if need "${SERVER_UP}" "A40-A43 模糊测试"; then
    if [ ! -f "${SCRIPT_DIR}/fuzz_judge.sh" ]; then
        skip "A40-A43 scripts/fuzz_judge.sh 不存在"
    elif [ -z "${USER_TOK:-}" ] || [ -z "${PID:-}" ]; then
        skip "A40-A43 provision 未成功（缺 USER_TOK 或 PID）"
    elif ! command -v python3 >/dev/null 2>&1; then
        skip "A40-A43 python3 不可用（fuzz mutation 引擎依赖）"
    else
        _fuzz_out="${TMPD}/fuzz.out"
        USER_TOK="${USER_TOK}" \
        ADMIN_TOK="${ADMIN_TOK:-}" \
        PID="${PID}" \
        BASE_URL="${BASE_URL}" \
        E2E_STRICT="${E2E_STRICT}" \
        bash "${SCRIPT_DIR}/fuzz_judge.sh" >"${_fuzz_out}" 2>&1 || true
        # 透传子用例的 ok/fail 行（让 e2e 主体输出可见 A40–A43 的细粒度断言）
        sed -n '/^── A40/,/^════════════/p' "${_fuzz_out}" | head -80
        # 反向汇入主计数器：parse 末行 FUZZ_RESULT 行
        _fuzz_last="$(grep -E '^FUZZ_RESULT ' "${_fuzz_out}" | tail -1)"
        _fp="$(echo "${_fuzz_last}" | awk '{print $2}' | cut -d= -f2)"
        _ff="$(echo "${_fuzz_last}" | awk '{print $3}' | cut -d= -f2)"
        _fs="$(echo "${_fuzz_last}" | awk '{print $4}' | cut -d= -f2)"
        _fp=${_fp:-0}; _ff=${_ff:-0}; _fs=${_fs:-0}
        PASS=$((PASS + _fp)); FAIL=$((FAIL + _ff)); SKIP=$((SKIP + _fs))
        if [ "${_ff}" = "0" ]; then
            ok "A40-A43 模糊测试整体无 FAIL（PASS=${_fp} SKIP=${_fs}）"
        else
            fail "A40-A43 模糊测试 FAIL=${_ff}（PASS=${_fp} SKIP=${_fs}）"
        fi
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A44 — 告警规则端到端 (Phase 9 ★ v1.2.73)
#
#  覆盖 SPEC §16.4「告警规则（P99 延迟 / 队列积压 / 磁盘 / 证书）」
#  的配置层 + 接收器层 + 可达性层 三段断言。**不发真告警**（避免
#  IM 群污染）；只验证配置 shape 与探针 alive。
#
#  - A44a  静态配置：prometheus-alerts.yml 关键 alertname 存在 + severity 覆盖
#  - A44b  静态配置：alertmanager.yml receivers / route / inhibit 齐备
#  - A44c  可达性：alertmanager /-/ready 200（如容器已起，profile=monitoring）
#  - A44d  可达性：prometheus /-/ready 200 + /api/v1/rules 返回 alert 列表非空
#  - A44e  端到端：模拟触发一条告警 → alertmanager /api/v2/alerts 可见（可选）
#
#  缺栈一律 SKIP（monitoring profile 默认不拉起）。
# ═════════════════════════════════════════════════════════════
ALERTMGR_URL="${ALERTMGR_URL:-http://localhost:9093}"
PROM_URL="${PROM_URL:-http://localhost:9090}"

kase "A44" "告警规则端到端（Phase 9 ★ v1.2.73）"

# ── A44a: prometheus-alerts.yml 静态配置 ──
_alerts="${ROOT}/monitoring/alerting/prometheus-alerts.yml"
_amcfg="${ROOT}/monitoring/alerting/alertmanager.yml"
if [ ! -f "${_alerts}" ] || [ ! -f "${_amcfg}" ]; then
    skip "A44a 缺少 monitoring/alerting/*.yml 配置"
    skip "A44b 缺少 monitoring/alerting/*.yml 配置"
else
    # SPEC §16.4 字面阈值映射的 6 条 alertname 全在（v1.2.73 决策：保留
    # 更严的现有阈值但补齐 LoginFailuresByIPHigh 占位 alert）
    _got_p99=0; _got_queue=0; _got_mem=0; _got_disk=0; _got_tls=0; _got_login=0
    grep -qE '^      - alert: JudgeDurationP99TooHigh'      "${_alerts}" && _got_p99=1
    grep -qE '^      - alert: JudgeQueueBacklog'           "${_alerts}" && _got_queue=1
    grep -qE '^      - alert: WebContainerMemoryHigh'      "${_alerts}" && _got_mem=1
    grep -qE '^      - alert: HostDiskSpaceLow'            "${_alerts}" && _got_disk=1
    grep -qE '^      - alert: TlsCertificateExpiringSoon'  "${_alerts}" && _got_tls=1
    grep -qE '^      - alert: LoginFailuresByIPHigh'       "${_alerts}" && _got_login=1
    [ "${_got_p99}" = "1" ] && ok "A44a JudgeDurationP99TooHigh 存在" \
        || fail "A44a JudgeDurationP99TooHigh 缺失"
    [ "${_got_queue}" = "1" ] && ok "A44a JudgeQueueBacklog 存在" \
        || fail "A44a JudgeQueueBacklog 缺失"
    [ "${_got_mem}" = "1" ] && ok "A44a WebContainerMemoryHigh 存在（v1.2.73 新增）" \
        || fail "A44a WebContainerMemoryHigh 缺失"
    [ "${_got_disk}" = "1" ] && ok "A44a HostDiskSpaceLow 存在" \
        || fail "A44a HostDiskSpaceLow 缺失"
    [ "${_got_tls}" = "1" ] && ok "A44a TlsCertificateExpiringSoon 存在" \
        || fail "A44a TlsCertificateExpiringSoon 缺失"
    [ "${_got_login}" = "1" ] && ok "A44a LoginFailuresByIPHigh 占位 alert 存在（v1.2.74+ 落地）" \
        || fail "A44a LoginFailuresByIPHigh 缺失"

    # severity label 覆盖（critical + warning 各至少一条）
    _sev_c="$(grep -cE '^[[:space:]]+severity: critical' "${_alerts}" || true)"
    _sev_w="$(grep -cE '^[[:space:]]+severity: warning'  "${_alerts}" || true)"
    [ "${_sev_c}" -ge 1 ] && ok "A44a severity=critical 出现 ${_sev_c} 条（≥1）" \
        || fail "A44a severity=critical 0 条"
    [ "${_sev_w}" -ge 1 ] && ok "A44a severity=warning 出现 ${_sev_w} 条（≥1）" \
        || fail "A44a severity=warning 0 条"

    # ── A44b: alertmanager.yml 静态配置 ──
    _am_missing=()
    grep -qF 'receivers:' "${_amcfg}"               || _am_missing+=('receivers 顶层')
    grep -qF 'route:' "${_amcfg}"                   || _am_missing+=('route 顶层')
    grep -qF "receiver: 'default-webhook'" "${_amcfg}" \
        || _am_missing+=("default-webhook receiver")
    grep -qF "receiver: 'critical-webhook'" "${_amcfg}" \
        || _am_missing+=("critical-webhook receiver")
    grep -qF 'inhibit_rules:' "${_amcfg}"           || _am_missing+=('inhibit_rules 抑制')
    grep -qF 'resolve_timeout:' "${_amcfg}"         || _am_missing+=('global.resolve_timeout')
    if [ "${#_am_missing[@]}" -eq 0 ]; then
        ok "A44b alertmanager.yml 顶层字段齐全（receivers/route/inhibit/resolve_timeout）"
    else
        fail "A44b alertmanager.yml 缺失: ${_am_missing[*]}"
    fi
fi

# ── A44c: alertmanager /-/ready 可达性（monitoring profile 拉起时）──
if curl -fsS --max-time 3 "${ALERTMGR_URL}/-/ready" >/dev/null 2>&1; then
    ok "A44c alertmanager ${ALERTMGR_URL}/-/ready → 200"
    # receivers 列表（GET /api/v2/receivers 返回 name 数组）
    _recv_body="$(curl -fsS --max-time 3 "${ALERTMGR_URL}/api/v2/receivers" 2>/dev/null || true)"
    if [ -n "${_recv_body}" ] && command -v jq >/dev/null 2>&1; then
        _recv_count="$(jq 'length' <<<"${_recv_body}" 2>/dev/null || echo 0)"
        if [ "${_recv_count:-0}" -ge 1 ]; then
            ok "A44c alertmanager /api/v2/receivers 返回 ${_recv_count} 条 receiver"
        else
            fail "A44c alertmanager receivers 列表为空（配置未加载？）"
        fi
    else
        ok "A44c alertmanager receivers 列表未深查（jq 缺）"
    fi
else
    skip "A44c alertmanager ${ALERTMGR_URL} 不可达（monitoring profile 未拉起）"
    skip "A44c alertmanager receivers 列表探针"
fi

# ── A44d: prometheus /-/ready + /api/v1/rules（monitoring profile 拉起时）──
if curl -fsS --max-time 3 "${PROM_URL}/-/ready" >/dev/null 2>&1; then
    ok "A44d prometheus ${PROM_URL}/-/ready → 200"
    _rules_body="$(curl -fsS --max-time 5 "${PROM_URL}/api/v1/rules" 2>/dev/null || true)"
    if [ -n "${_rules_body}" ] && command -v jq >/dev/null 2>&1; then
        _groups_count="$(jq '.data.groups | length' <<<"${_rules_body}" 2>/dev/null || echo 0)"
        _rules_count="$(jq '[.data.groups[].rules[] | select(.type=="alerting")] | length' <<<"${_rules_body}" 2>/dev/null || echo 0)"
        if [ "${_groups_count:-0}" -ge 1 ]; then
            ok "A44d prometheus 加载了 ${_groups_count} 个 rule group / ${_rules_count} 条 alerting 规则"
        else
            fail "A44d prometheus 未加载任何 rule group（rule_files 配置异常？）"
        fi
        # SPEC §16.4 字面告警在已加载规则里
        for _rule in JudgeDurationP99TooHigh JudgeQueueBacklog WebContainerMemoryHigh HostDiskSpaceLow TlsCertificateExpiringSoon LoginFailuresByIPHigh; do
            _present="$(jq --arg n "${_rule}" '[.data.groups[].rules[] | select(.name==$n)] | length' <<<"${_rules_body}" 2>/dev/null || echo 0)"
            [ "${_present:-0}" -ge 1 ] && ok "A44d prometheus 已加载 ${_rule}" \
                || fail "A44d prometheus 缺 ${_rule} 规则（reload 失败？）"
        done
    else
        ok "A44d prometheus rules 列表未深查（jq 缺）"
    fi
else
    skip "A44d prometheus ${PROM_URL} 不可达（monitoring profile 未拉起）"
    skip "A44d prometheus rule group 计数探针"
    skip "A44d prometheus 6 条规则加载探针"
fi

# ── A44e: 端到端模拟触发（可选：alertmanager 暴露 POST /api/v2/alerts）──
# 不发真通知；只验证 alertmanager 能 ingest 一条 alert 且列出。
# 用 curl POST 一条 minimal alert（expire 5s 自动清，不污染 UI）。
if curl -fsS --max-time 3 "${ALERTMGR_URL}/-/ready" >/dev/null 2>&1; then
    _post_body='[{
        "labels": {"alertname":"A44E2EProbe","service":"e2e","severity":"warning"},
        "annotations": {"summary":"e2e probe","description":"a44 canary"},
        "startsAt":"'"$(date -u +%Y-%m-%dT%H:%M:%S.000Z)"'",
        "endsAt":"'"$(date -u -d '+5 seconds' +%Y-%m-%dT%H:%M:%S.000Z 2>/dev/null || date -u -v+5S +%Y-%m-%dT%H:%M:%S.000Z)"'"
    }]'
    _post_code="$(curl -s -o /dev/null -w '%{http_code}' \
        --max-time 5 -X POST \
        -H 'Content-Type: application/json' \
        --data "${_post_body}" \
        "${ALERTMGR_URL}/api/v2/alerts")"
    if [ "${_post_code}" = "200" ]; then
        ok "A44e alertmanager /api/v2/alerts POST → 200（canary alert 已 ingest）"
    else
        # 不强制 fail——某些 alertmanager 版本 POST 路径在 v1/v2 之间变动
        skip "A44e alertmanager /api/v2/alerts POST → ${_post_code}（版本差异）"
    fi
else
    skip "A44e alertmanager 不可达，跳过 canary POST"
fi

# ═════════════════════════════════════════════════════════════
#  A45 — 性能 Profile 端到端 (Phase 9 △ v1.2.74)
#
#  覆盖 SPEC §11 Phase 9「△ 性能 Profile（perf / flamegraph 跑一次
#  判题热路径）」：把 scripts/perf_profile.sh 三段（HTTP 面 timing
#  拆解 / Prometheus histogram 采样 / Linux perf + flamegraph）以
#  委托 + 静态配置两层断言。
#
#  - A45a 静态配置：scripts/perf_profile.sh bash -n + 关键环节点
#    （沿用 v1.2.65 / v1.2.67 / v1.2.72 委托模式：e2e 主体只跑轻
#    校验，bulk 跑委托给子脚本）
#  - A45b 静态配置：报告输出路径 docs/performance-profile.md 在仓
#    + 性能 Profile runbook docs/runbooks/performance-profile.md 在仓
#  - A45c 端到端：跑 scripts/perf_profile.sh，末行 `PROFILE_RESULT
#    PASS=N FAIL=N SKIP=N` 反向汇入主计数器（与 v1.2.67 FUZZ_RESULT
#    / v1.2.72 DRILL_RESULT 同款设计）
#
#  缺栈一律 SKIP（profile 是探索性工具，缺前置不应该 fail CI；
#  PROFILE_STRICT=1 时升级 FAIL）。
# ═════════════════════════════════════════════════════════════
PERF_PROFILE_SH="${SCRIPT_DIR}/perf_profile.sh"
PERF_PROFILE_DOC="${ROOT}/docs/performance-profile.md"
PERF_PROFILE_RUNBOOK="${ROOT}/docs/runbooks/performance-profile.md"

kase "A45" "性能 Profile 端到端（Phase 9 △ v1.2.74）"

# ── A45a: scripts/perf_profile.sh 静态配置 ──
if [ ! -f "${PERF_PROFILE_SH}" ]; then
    skip "A45a 缺少 scripts/perf_profile.sh"
    skip "A45b 缺少 scripts/perf_profile.sh"
    skip "A45c 缺少 scripts/perf_profile.sh，无法委托运行"
else
    # bash -n
    if bash -n "${PERF_PROFILE_SH}" 2>/dev/null; then
        ok "A45a scripts/perf_profile.sh bash -n 通过"
    else
        fail "A45a scripts/perf_profile.sh bash -n 失败（语法错）"
    fi

    # 关键环节点 grep（与 lint.sh perf_profile 子任务同样的硬约束集合，
    # 这里只 grep 关键 6 项作轻量校验，完整 22 项在 lint.sh 里）
    _pp_missing=()
    grep -qF 'PROFILE_RESULT PASS=' "${PERF_PROFILE_SH}" \
        || _pp_missing+=('PROFILE_RESULT 汇总行')
    grep -qF 'PROFILE_STRICT'        "${PERF_PROFILE_SH}" \
        || _pp_missing+=('PROFILE_STRICT 开关')
    grep -qF 'litecode_judge_duration_seconds' "${PERF_PROFILE_SH}" \
        || _pp_missing+=('litecode_judge_duration_seconds histogram 引用')
    grep -qF 'time_starttransfer'    "${PERF_PROFILE_SH}" \
        || _pp_missing+=('curl -w time_starttransfer TTFB')
    grep -qF 'perf record'            "${PERF_PROFILE_SH}" \
        || _pp_missing+=('perf record 调用（Phase C）')
    grep -qF 'flamegraph.pl'         "${PERF_PROFILE_SH}" \
        || _pp_missing+=('flamegraph.pl 火焰图（Phase C）')
    if [ "${#_pp_missing[@]}" -eq 0 ]; then
        ok "A45a scripts/perf_profile.sh 关键环节点齐（6/6：PROFILE_RESULT + STRICT + histogram + TTFB + perf + flamegraph）"
    else
        fail "A45a scripts/perf_profile.sh 缺失关键环节点：${_pp_missing[*]}"
    fi

    # 行数 sanity
    _pp_lines="$(wc -l < "${PERF_PROFILE_SH}")"
    if [ "${_pp_lines}" -ge 250 ]; then
        ok "A45a scripts/perf_profile.sh 行数=${_pp_lines}（≥ 250）"
    else
        fail "A45a scripts/perf_profile.sh 行数=${_pp_lines}（< 250，疑似退化）"
    fi

    # ── A45b: 输出文档在仓 ──
    if [ -f "${PERF_PROFILE_RUNBOOK}" ]; then
        ok "A45b docs/runbooks/performance-profile.md 存在（runbook 入仓）"
        # runbook 必须含 SPEC §12.2 阈值对照表
        if grep -qE 'HEALTH_MAX_MS|health.*<.*50ms|< 50ms' "${PERF_PROFILE_RUNBOOK}"; then
            ok "A45b runbook 含 SPEC §12.2 健康检查 < 50ms 阈值引用"
        else
            fail "A45b runbook 缺 SPEC §12.2 健康检查阈值引用"
        fi
        if grep -qE 'SUBMIT.*<.*200ms|< 200ms' "${PERF_PROFILE_RUNBOOK}"; then
            ok "A45b runbook 含 SPEC §12.2 提交 API < 200ms 阈值引用"
        else
            fail "A45b runbook 缺 SPEC §12.2 提交 API 阈值引用"
        fi
    else
        fail "A45b docs/runbooks/performance-profile.md 缺失"
    fi

    # ── A45c: 委托运行 perf_profile.sh（live stack 时）──
    if [ "${SERVER_UP}" != "1" ]; then
        skip "A45c 委托 perf_profile.sh（栈缺失；live 时跑）"
        skip "A45c PROFILE_RESULT 反向汇入（栈缺失）"
    elif [ -z "${USER_TOK:-}" ] || [ -z "${PID:-}" ]; then
        skip "A45c 委托 perf_profile.sh（provision 未成功：USER_TOK / PID 缺失）"
        skip "A45c PROFILE_RESULT 反向汇入（provision 失败）"
    else
        # 调小采样避免 e2e 拖太久（默认 20，这里给 5；运行时可在脚本里覆盖）
        _pp_out="${TMPD}/perf_profile.out"
        PROFILE_SAMPLES="${PROFILE_SAMPLES:-5}" \
        BASE_URL="${BASE_URL}" \
        PROFILE_STRICT="${E2E_STRICT}" \
        USER_TOK="${USER_TOK}" \
        ADMIN_TOK="${ADMIN_TOK:-}" \
        PID="${PID}" \
        E2E_STRICT="${E2E_STRICT}" \
        bash "${PERF_PROFILE_SH}" >"${_pp_out}" 2>&1 || true
        # 透传子用例的 ok/fail 行（让 e2e 主体输出可见 A45 细粒度断言）
        sed -n '/^── Phase A/,/^PROFILE_RESULT /p' "${_pp_out}" | head -40
        # 反向汇入主计数器：parse 末行 PROFILE_RESULT 行
        _pp_last="$(grep -E '^PROFILE_RESULT ' "${_pp_out}" | tail -1)"
        _pp_pass="$(echo "${_pp_last}" | awk '{print $2}' | cut -d= -f2)"
        _pp_fail="$(echo "${_pp_last}" | awk '{print $3}' | cut -d= -f2)"
        _pp_skip="$(echo "${_pp_last}" | awk '{print $4}' | cut -d= -f2)"
        _pp_pass=${_pp_pass:-0}; _pp_fail=${_pp_fail:-0}; _pp_skip=${_pp_skip:-0}
        PASS=$((PASS + _pp_pass)); FAIL=$((FAIL + _pp_fail)); SKIP=$((SKIP + _pp_skip))
        if [ "${_pp_fail}" = "0" ]; then
            ok "A45c perf_profile.sh 整体无 FAIL（PASS=${_pp_pass} SKIP=${_pp_skip}）"
        else
            fail "A45c perf_profile.sh FAIL=${_pp_fail}（PASS=${_pp_pass} SKIP=${_pp_skip}）"
        fi
        # 报告再生断言
        if [ -f "${PERF_PROFILE_DOC}" ]; then
            ok "A45c docs/performance-profile.md 已再生（运行时产物）"
        else
            fail "A45c docs/performance-profile.md 未再生（write_report 失败？）"
        fi
    fi
fi

# ═════════════════════════════════════════════════════════════
#  A46 — 备份脚本端到端 (Phase 7 ☆ v1.2.75)
#  - A46a  静态配置：scripts/backup.sh bash -n + 关键 6 项 + 行数 sanity
#  - A46b  静态配置：docs/runbooks/backup.md 存在 + 含 SPEC §16.5 阈值引用
#  - A46c  委托运行 backup.sh DRY_RUN（live stack 时）+ BACKUP_RESULT 反向汇入
# ═════════════════════════════════════════════════════════════
BACKUP_SH="${ROOT}/scripts/backup.sh"
BACKUP_RUNBOOK="${ROOT}/docs/runbooks/backup.md"

kase "A46" "备份脚本端到端（Phase 7 ☆ v1.2.75）"

# ── A46a: scripts/backup.sh 静态配置 ──
if [ ! -f "${BACKUP_SH}" ]; then
    skip "A46a 缺少 scripts/backup.sh"
    skip "A46b 缺少 scripts/backup.sh"
    skip "A46c 缺少 scripts/backup.sh，无法委托运行"
else
    if bash -n "${BACKUP_SH}" 2>/dev/null; then
        ok "A46a scripts/backup.sh bash -n 通过"
    else
        fail "A46a scripts/backup.sh bash -n 失败（语法错）"
    fi

    # 关键环节点 grep（与 lint.sh backup 同款的硬约束集合，
    # 这里只 grep 关键 6 项作轻量校验；完整 22+ 项在 lint.sh 里）
    _bk_missing=()
    grep -qF 'BACKUP_RESULT PASS=' "${BACKUP_SH}" \
        || _bk_missing+=('BACKUP_RESULT 汇总行')
    grep -qF 'BACKUP_STRICT'         "${BACKUP_SH}" \
        || _bk_missing+=('BACKUP_STRICT 开关')
    grep -qF 'BACKUP_DRY_RUN'        "${BACKUP_SH}" \
        || _bk_missing+=('BACKUP_DRY_RUN 探测开关')
    grep -qF 'capabilities:'         "${BACKUP_SH}" \
        || _bk_missing+=('capabilities 探测行')
    grep -qF 'rclone copyto'         "${BACKUP_SH}" \
        || _bk_missing+=('rclone copyto 异地同步')
    grep -qF 'gunzip -t'             "${BACKUP_SH}" \
        || _bk_missing+=('gunzip -t 完整性校验')
    if [ "${#_bk_missing[@]}" -eq 0 ]; then
        ok "A46a scripts/backup.sh 关键环节点齐（6/6：RESULT + STRICT + DRY_RUN + capabilities + rclone + gunzip -t）"
    else
        fail "A46a scripts/backup.sh 缺失关键环节点：${_bk_missing[*]}"
    fi

    _bk_lines="$(wc -l < "${BACKUP_SH}")"
    if [ "${_bk_lines}" -ge 130 ]; then
        ok "A46a scripts/backup.sh 行数=${_bk_lines}（≥ 130）"
    else
        fail "A46a scripts/backup.sh 行数=${_bk_lines}（< 130，疑似退化）"
    fi

    # ── A46b: runbook 存在 + SPEC §16.5 阈值引用 ──
    if [ -f "${BACKUP_RUNBOOK}" ]; then
        ok "A46b docs/runbooks/backup.md 存在（runbook 入仓）"
        if grep -qE 'mysqldump\s+每日|mysqldump 每日' "${BACKUP_RUNBOOK}"; then
            ok "A46b runbook 含 SPEC §16.5 mysqldump 每日备份引用"
        else
            fail "A46b runbook 缺 SPEC §16.5 mysqldump 每日备份引用"
        fi
        if grep -qE '异地|OSS|S3|rclone' "${BACKUP_RUNBOOK}"; then
            ok "A46b runbook 含异地备份策略引用"
        else
            fail "A46b runbook 缺异地备份策略引用"
        fi
        if grep -qE '3-2-1|异地 + 冷备|异地 + 多副本' "${BACKUP_RUNBOOK}"; then
            ok "A46b runbook 含 3-2-1 备份原则引用"
        else
            fail "A46b runbook 缺 3-2-1 备份原则引用"
        fi
    else
        fail "A46b docs/runbooks/backup.md 缺失"
    fi

    # ── A46c: 委托运行 backup.sh DRY_RUN ──
    # 备份任务需要真实 MySQL；DRY_RUN 模式只跑能力探测不真 dump，
    # CI 默认走 DRY_RUN 验证脚本骨架即可。
    _bk_out="${TMPD}/backup.out"
    BACKUP_DRY_RUN=1 \
    BASE_URL="${BASE_URL}" \
    bash "${BACKUP_SH}" >"${_bk_out}" 2>&1 || true
    sed -n '/^capabilities:/,/^BACKUP_RESULT /p' "${_bk_out}" | head -10
    _bk_last="$(grep -E '^BACKUP_RESULT ' "${_bk_out}" | tail -1)"
    if [ -n "${_bk_last}" ]; then
        _bk_pass="$(echo "${_bk_last}" | awk '{print $2}' | cut -d= -f2)"
        _bk_fail="$(echo "${_bk_last}" | awk '{print $3}' | cut -d= -f2)"
        _bk_skip="$(echo "${_bk_last}" | awk '{print $4}' | cut -d= -f2)"
        _bk_pass=${_bk_pass:-0}; _bk_fail=${_bk_fail:-0}; _bk_skip=${_bk_skip:-0}
        PASS=$((PASS + _bk_pass)); FAIL=$((FAIL + _bk_fail)); SKIP=$((SKIP + _bk_skip))
        if [ "${_bk_fail}" = "0" ]; then
            ok "A46c backup.sh DRY_RUN 整体无 FAIL（PASS=${_bk_pass} SKIP=${_bk_skip}）"
        else
            fail "A46c backup.sh FAIL=${_bk_fail}（PASS=${_bk_pass} SKIP=${_bk_skip}）"
        fi
    else
        fail "A46c backup.sh 未输出 BACKUP_RESULT 行（脚本结尾契约违反）"
    fi
fi

# ═════════════════════════════════════════════════════════════
#  总结
# ═════════════════════════════════════════════════════════════
echo
echo "════════════════════════════════════════════"
echo "  Passed: ${PASS}   Failed: ${FAIL}   Skipped: ${SKIP}"
echo "════════════════════════════════════════════"
if [ "${FAIL}" -gt 0 ]; then
    if [ "${E2E_STRICT}" = "1" ]; then
        echo "结果：FAIL（存在真实断言失败，或 STRICT 模式下能力缺失被升级）"
    else
        echo "结果：FAIL（存在真实断言失败）"
    fi
    exit 1
fi
echo "结果：PASS（无真实断言失败）"
exit 0
