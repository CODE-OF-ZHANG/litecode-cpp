#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/demo_submission.sh
# -------------------------------------------------------------
# 一键试运行脚本（v1.3.1 final hash 51be5f5）：
#   1) 探测环境（docker / curl / jq）
#   2) 启动 docker-compose 11 服务栈（idempotent，已跑则跳过）
#   3) 等 web 服务 healthy（轮询 /api/v1/health）
#   4) 应用 V001-V011 DB migrations
#   5) admin 登录 → bulk-import 7 道种子题（two-sum / reverse-integer /
#      palindrome-number / fizzbuzz / valid-parentheses / sqrt-x /
#      trim-trailing-whitespace）
#   6) 注册测试用户 demo_user_<rnd> + 登录拿 user token
#   7) 演示 6 种 judge_type 端到端（exact / float_eps / ignore_trailing /
#      ignore_case / ignore_all_whitespace / special，每种一道题
#      一个 AC 提交 + 轮询终态 + 打印 status / case_results / error_message）
#   8) 演示 7 种 status 端到端（AC / WA / TLE / MLE / OLE / CE / RE，
#      全部在 two-sum 上提交不同代码 → 轮询终态 → 打印）
#   9) 演示 SPJ 闭环（admin PUT SPJ → 公共 detail has_special_judge=true →
#      提交正解 → status=ac → DELETE → has_special_judge=false 兜底）
#  10) 打印汇总表 + 提示用户可在浏览器打开 http://localhost:8080 验证
#
# 设计原则：
#   - idempotent：重复跑不会报错（题已导入 on_duplicate=skip；用户已注册复用）
#   - STRICT 模式：DEMO_STRICT=1 时所有断言失败 exit 1（CI 强约束）
#   - DRY_RUN：DEMO_DRY_RUN=1 只探测环境 + 打印计划，不真启动（CI 能力探测）
#   - 不需要 GPU；普通 4C8G 笔记本 5 分钟内可跑完
#
# 用法：
#   bash scripts/demo_submission.sh                     # 默认宽松模式
#   DEMO_STRICT=1 bash scripts/demo_submission.sh       # CI 强约束
#   DEMO_DRY_RUN=1 bash scripts/demo_submission.sh      # 仅探测
#   BASE_URL=http://host:8080 bash scripts/demo_submission.sh  # 自定义
#
# 退出码：
#   0  —— 所有演示 case 终态符合预期
#   1  —— 至少一个 case 不符合预期（含 STRICT 模式下能力缺失被升级）
#   2  —— 启动前置失败（docker daemon 不通 / compose 缺服务 / jq 缺失）
# =============================================================
# shellcheck disable=SC2317
set -uo pipefail

# ─────────────── 路径解析 ───────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"
COMPOSE_FILE="${ROOT}/docker-compose.yml"
PROBLEMS_DIR="${ROOT}/problems"
WEB_DIR="${ROOT}/web"

# ─────────────── 配置 ───────────────
BASE_URL="${BASE_URL:-http://localhost:8080}"
BASE_URL="${BASE_URL%/}"
API="${BASE_URL}/api/v1"
ADMIN_USER="${ADMIN_USER:-admin}"
ADMIN_PASS="${ADMIN_PASS:-admin123!}"
DEMO_USER="${DEMO_USER:-demo_user}"
DEMO_PASS="${DEMO_PASS:-Demo1234!}"
JUDGE_POLL_TIMEOUT_S="${JUDGE_POLL_TIMEOUT_S:-120}"
COMPOSE_TIMEOUT_S="${COMPOSE_TIMEOUT_S:-180}"
DEMO_STRICT="${DEMO_STRICT:-0}"
DEMO_DRY_RUN="${DEMO_DRY_RUN:-0}"

# ─────────────── 颜色 ───────────────
if [ -t 1 ]; then
    C_BOLD="\033[1m"; C_GREEN="\033[32m"; C_YELLOW="\033[33m"
    C_RED="\033[31m"; C_CYAN="\033[36m"; C_RESET="\033[0m"
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_CYAN=""; C_RESET=""
fi
info()  { echo -e "${C_BOLD}[*]${C_RESET} $*"; }
ok()    { echo -e "${C_GREEN}[✓]${C_RESET} $*"; }
warn()  { echo -e "${C_YELLOW}[!]${C_RESET} $*"; }
err()   { echo -e "${C_RED}[✗]${C_RESET} $*" >&2; }
head()  { echo -e "\n${C_CYAN}══════ $* ══════${C_RESET}"; }

# ─────────────── 计数器 ───────────────
PASS=0; FAIL=0; SKIP=0
pass() { ok "$*"; PASS=$((PASS+1)); }
fail() { err "$*"; FAIL=$((FAIL+1)); }
skip() { echo -e "${C_YELLOW}[skip]${C_RESET} $*"; SKIP=$((SKIP+1)); }

# ─────────────── 依赖探测 ───────────────
need_cmd() {
    command -v "$1" >/dev/null 2>&1 || { err "缺少依赖 $1"; exit 2; }
}
need_cmd curl
need_cmd docker
# jq 兜底（Windows 上 msys 默认不带 jq）
if ! command -v jq >/dev/null 2>&1; then
    for cand in \
        "/c/Users/${USERNAME:-$USER}/AppData/Local/Microsoft/WinGet/Packages/jqlang.jq_Microsoft.Winget.Source_8wekyb3d8bbwe" \
        "/c/Program Files/jq"; do
        if [ -x "${cand}/jq.exe" ]; then
            export PATH="${cand}:${PATH}"
            break
        fi
    done
fi
need_cmd jq

# ─────────────── HTTP helper ───────────────
TMPD="$(mktemp -d -t lc-demo-XXXXXX)"
RESP_BODY="${TMPD}/body"
RESP_HDR="${TMPD}/hdr"
cleanup() { rm -rf "${TMPD}"; }
trap cleanup EXIT
HTTP_CODE=""

api() {
    local method="$1" path="$2"; shift 2
    local token="" data=""
    while [ $# -gt 0 ]; do
        case "$1" in
            -t) token="$2"; shift 2;;
            -d) data="$2"; shift 2;;
            *) shift;;
        esac
    done
    local url="${API}${path}"
    local -a args=(-sS -X "${method}" -o "${RESP_BODY}" -D "${RESP_HDR}" \
                   -w '%{http_code}' --max-time 30)
    [ -n "${token}" ] && args+=(-H "Authorization: Bearer ${token}")
    if [ -n "${data}" ]; then
        args+=(-H "Content-Type: application/json" --data "${data}")
    fi
    HTTP_CODE="$(curl "${args[@]}" "${url}" 2>/dev/null)" || HTTP_CODE="000"
    [ -n "${HTTP_CODE}" ] || HTTP_CODE="000"
}
jqb() { jq -r "$1" "${RESP_BODY}" 2>/dev/null; }

# ─────────────── 轮询等终态 ───────────────
# 回填：SUB_ID / SUB_STATUS / SUB_BODY
submit_and_wait() {
    local code="$1" lang="${2:-cpp}"
    local pid="$3"
    SUB_ID=""; SUB_STATUS=""; SUB_BODY="{}"
    local body; body="$(jq -n --argjson pid "${pid}" --arg lang "${lang}" \
        --arg code "${code}" '{problem_id:$pid,language:$lang,code:$code}')"
    api POST /submissions -t "${USER_TOK}" -d "${body}"
    [ "${HTTP_CODE}" = "201" ] || return 1
    SUB_ID="$(jqb '.data.submission_id')"
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
    return 0
}

assert_status() {
    local label="$1" want="$2"
    if [ "${SUB_STATUS}" = "${want}" ]; then
        pass "${label} → ${want}"
    else
        fail "${label} → ${SUB_STATUS}（预期 ${want}）"
    fi
}

# ─────────────── §1 启动 ───────────────
head "§1 启动 docker-compose 11 服务栈"
if [ "${DEMO_DRY_RUN}" = "1" ]; then
    info "[DRY_RUN] 跳过实际启动，仅打印计划"
    cat <<EOF
  计划执行（DEMO_DRY_RUN=1 关闭）：
    1. docker compose -f ${COMPOSE_FILE} config --quiet           # 校验
    2. docker compose -f ${COMPOSE_FILE} up -d                     # 后台启
    3. 轮询 /api/v1/health 直到 docker=ok                          # 等 ready
    4. bash ${ROOT}/scripts/init_db.sh root \${MYSQL_ROOT_PASSWORD} mysql 3306 litecode
    5. admin login → bulk-import 7 道种子题
    6. 注册测试用户 → 演示 6 judge_type + 7 status + SPJ 闭环
EOF
    exit 0
fi

if ! docker info >/dev/null 2>&1; then
    err "docker daemon 不通——请先启动 Docker Desktop（Windows: 右下托盘 → Start）"
    exit 2
fi
ok "docker daemon OK"

# docker compose 校验
if ! docker compose -f "${COMPOSE_FILE}" config --quiet 2>/dev/null; then
    err "docker-compose.yml 校验失败"
    exit 2
fi
ok "docker compose config OK"

# 启动栈（idempotent）
info "docker compose up -d（11 服务：mysql / judge / web / caddy / docker-proxy + monitoring profile）..."
# 关键: web 容器是 C++ binary,代码改动后必须 rebuild -- 否则新加的
# admin 端 validator (ignore_case / ignore_all_whitespace) / judge.sh 接线 / V011
# 等都还是老版本。这是 v1.2.5x 起的"compose up 不重建"陷阱。
# FORCE_REBUILD=0 默认：自动检测（git HEAD vs web 镜像创建时间）
# FORCE_REBUILD=1 强制重建（首次 5-8 分钟）
# FORCE_REBUILD=0 + image 较新 → 跳过 build
GIT_HEAD="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
WEB_IMAGE_CREATED="$(docker inspect litecode-web:latest --format '{{.Created}}' 2>/dev/null | head -1 || echo '')"
NEED_REBUILD="0"
if [ "${FORCE_REBUILD:-0}" = "1" ]; then
    NEED_REBUILD="1"
    warn "FORCE_REBUILD=1 强制重建 web + judge 镜像"
elif [ -z "${WEB_IMAGE_CREATED}" ]; then
    NEED_REBUILD="1"
    warn "litecode-web:latest 镜像不存在，需先 build"
else
    # 镜像创建时间晚于 src/ 任意 .h 文件 → 不用重建
    NEED_REBUILD="$(find "${ROOT}/src" -name '*.h' -o -name '*.cpp' -newer "${WEB_IMAGE_CREATED}" 2>/dev/null | head -1 | grep -c . || echo 0)"
    if [ "${NEED_REBUILD}" = "1" ]; then
        warn "检测到 src/* 比 web 镜像新 → rebuild"
    fi
fi
if [ "${NEED_REBUILD}" = "1" ]; then
    info "docker compose build web judge（首次 5-8 分钟，增量几秒）..."
    docker compose -f "${COMPOSE_FILE}" build web judge 2>&1 | tail -10 || {
        err "docker compose build 失败"
        exit 2
    }
    ok "web + judge 镜像已 rebuild（git HEAD=${GIT_HEAD}）"
else
    ok "web + judge 镜像已是最新（git HEAD=${GIT_HEAD}, 无需 rebuild）"
fi
docker compose -f "${COMPOSE_FILE}" up -d >/dev/null 2>&1 || {
    err "docker compose up -d 失败"
    exit 2
}
ok "compose up -d 完成"

# 等 web 服务 ready（最多 COMPOSE_TIMEOUT_S 秒）
info "等 web 服务 ready（轮询 /api/v1/health 最多 ${COMPOSE_TIMEOUT_S}s）..."
wait_start=${SECONDS}
SERVER_READY=0
while [ $((SECONDS - wait_start)) -lt "${COMPOSE_TIMEOUT_S}" ]; do
    api GET /health
    if [ "${HTTP_CODE}" = "200" ] || [ "${HTTP_CODE}" = "503" ]; then
        DOCKER_PROBE="$(jqb '.docker')"
        if [ "${DOCKER_PROBE}" = "ok" ]; then
            SERVER_READY=1
            break
        fi
    fi
    sleep 3
done
if [ "${SERVER_READY}" != "1" ]; then
    err "服务未在 ${COMPOSE_TIMEOUT_S}s 内 ready——docker compose logs web 看详情"
    exit 2
fi
ok "web 服务 ready（docker=ok）"

# ─────────────── §2 应用 DB migrations ───────────────
head "§2 应用 V001-V011 DB migrations"
# 直接用 docker exec + mysql 客户端逐文件应用 SQL（最稳，跨平台一致）：
#   - 不依赖 init_db.sh（它在 host 上跑，要求 host 装 mysql client）
#   - 不依赖 host:3306 暴露（compose 默认不暴露 MySQL 端口到 host）
#   - 直接用 stdin 把 SQL 内容灌进容器内的 mysql 客户端
MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-123456}"
APPLIED_VIA=""
APPLIED=0
SKIPPED=0

if command -v docker >/dev/null 2>&1 && docker ps --format '{{.Names}}' 2>/dev/null \
        | grep -q '^litecode-mysql$'; then
    info "用 docker exec 在容器内应用 V*.sql（绕开 Windows msys 路径转换）..."
    # 先确保 schema_migrations 表存在（V001__init.sql 已应用过的话）
    docker exec -e MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" litecode-mysql \
        mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" litecode \
        -e "CREATE TABLE IF NOT EXISTS schema_migrations (version VARCHAR(32) PRIMARY KEY, applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;" 2>&1 | tail -5
    # 按字母序遍历 SQL，逐个应用（已应用的跳过）
    for sql in "${ROOT}"/db/migrations/V*.sql; do
        [ -f "${sql}" ] || continue
        version="$(basename "${sql}" .sql | cut -d'_' -f1)"
        # 检查是否已应用
        applied_now="$(docker exec -e MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" litecode-mysql \
            mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" litecode -N -B \
            -e "SELECT COUNT(*) FROM schema_migrations WHERE version='${version}'" 2>/dev/null | tail -1)"
        if [ "${applied_now:-0}" -gt 0 ]; then
            SKIPPED=$((SKIPPED + 1))
            continue
        fi
        # 用 stdin 重定向把 SQL 灌进容器内的 mysql 客户端
        if docker exec -i -e MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" litecode-mysql \
                mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" litecode \
                < "${sql}" 2>&1 | tail -3; then
            APPLIED=$((APPLIED + 1))
        fi
    done
    APPLIED_VIA="(via docker exec litecode-mysql)"
    ok "migrations 应用完成 ${APPLIED_VIA}（applied=${APPLIED}, skipped=${SKIPPED}）"
elif command -v mysql >/dev/null 2>&1 && \
     MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" mysql -h 127.0.0.1 -P 3306 -uroot \
        --protocol=TCP -e "SELECT 1" >/dev/null 2>&1; then
    info "host 直连 MySQL 跑 init_db.sh..."
    if bash "${ROOT}/scripts/init_db.sh" root "${MYSQL_ROOT_PASSWORD}" \
            127.0.0.1 3306 litecode 2>&1 | tail -10; then
        ok "V001-V011 migrations 应用完成（host mysql）"
    else
        warn "init_db.sh 返回非 0（可能 V001-V011 已全部应用）"
    fi
else
    warn "无法定位 MySQL（容器 litecode-mysql 不在 + host 3306 不通），跳过显式 migrate"
    warn "如 judge_type 扩列 / admin 端 500，先确认 V011 已应用："
    warn "  docker exec litecode-mysql mysql -uroot -p123456 litecode -e \"SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_NAME='test_cases' AND COLUMN_NAME='judge_type'\""
fi

# ─────────────── §3 admin 登录 + bulk-import ───────────────
head "§3 admin 登录 + bulk-import 7 道种子题"
body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" \
    '{username:$u,password:$p}')"
api POST /auth/login -d "${body}"
if [ "${HTTP_CODE}" != "200" ]; then
    err "admin 登录失败（凭据 ${ADMIN_USER}/${ADMIN_PASS}）"
    exit 2
fi
ADMIN_TOK="$(jqb '.data.access_token')"
ok "admin 登录成功（role=$(jqb '.data.user.role')）"

# bulk-import 7 道题（idempotent：on_duplicate=skip）
SEEDS=(
    "two-sum"
    "reverse-integer"
    "palindrome-number"
    "fizzbuzz"
    "valid-parentheses"
    "sqrt-x"
    "trim-trailing-whitespace"
)
for slug in "${SEEDS[@]}"; do
    if [ ! -f "${PROBLEMS_DIR}/${slug}.json" ]; then
        skip "${slug}.json 不存在，跳过"
        continue
    fi
    api POST "/admin/problems/import?on_duplicate=skip" -t "${ADMIN_TOK}" \
        -F "files=@${PROBLEMS_DIR}/${slug}.json;type=application/json"
    if [ "${HTTP_CODE}" = "200" ]; then
        n="$(jqb '.data.summary.total_files')"
        pass "bulk-import ${slug}（total_files=${n}）"
    else
        skip "bulk-import ${slug} HTTP=${HTTP_CODE}（可能已存在）"
    fi
done

# 拿 two-sum problem_id 作 AC/WA/TLE/MLE/OLE/CE/RE 演示用
api GET "/problems/two-sum"
TWO_SUM_PID="$(jqb '.data.id')"
if [ -z "${TWO_SUM_PID}" ] || [ "${TWO_SUM_PID}" = "null" ]; then
    err "two-sum 未导入（problem_id 为空）"
    exit 2
fi
ok "two-sum problem_id=${TWO_SUM_PID}"

# ─────────────── §4 注册 + 登录测试用户 ───────────────
head "§4 注册 + 登录测试用户 ${DEMO_USER}"
UNIQ="$(date +%s)_$$"
DEMO_USER_FINAL="${DEMO_USER}_${UNIQ}"
body="$(jq -n --arg u "${DEMO_USER_FINAL}" --arg p "${DEMO_PASS}" \
    '{username:$u,password:$p}')"
api POST /auth/register -d "${body}"
if [ "${HTTP_CODE}" = "201" ]; then
    USER_TOK="$(jqb '.data.access_token')"
    USER_ID="$(jqb '.data.user.id')"
    pass "注册成功（user=${DEMO_USER_FINAL}, id=${USER_ID}）"
elif [ "${HTTP_CODE}" = "409" ]; then
    warn "用户已存在，跳过注册；走登录拿 token"
    body="$(jq -n --arg u "${DEMO_USER_FINAL}" --arg p "${DEMO_PASS}" \
        '{username:$u,password:$p}')"
    api POST /auth/login -d "${body}"
    if [ "${HTTP_CODE}" = "200" ]; then
        USER_TOK="$(jqb '.data.access_token')"
        USER_ID="$(jqb '.data.user.id')"
        pass "登录成功（id=${USER_ID}）"
    else
        err "登录失败 HTTP=${HTTP_CODE}"
        exit 2
    fi
else
    err "注册失败 HTTP=${HTTP_CODE}"
    exit 2
fi

# ─────────────── §5 6 种 judge_type 演示 ───────────────
head "§5 6 种 judge_type 端到端演示（exact / float_eps / ignore_trailing / ignore_case / ignore_all_whitespace / special）"

# (1) exact: two-sum AC
CODE_TWO_SUM_AC='#include <bits/stdc++.h>
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
info "[exact] two-sum 提交 AC 解 → 期望 status=ac"
if submit_and_wait "${CODE_TWO_SUM_AC}" cpp "${TWO_SUM_PID}"; then
    assert_status "[exact] two-sum AC 解" ac
else
    fail "提交失败 POST HTTP=${HTTP_CODE}"
fi

# (2) float_eps: sqrt-x AC
api GET "/problems/sqrt-x"
SQRT_X_PID="$(jqb '.data.id')"
CODE_SQRT_AC='#include <cstdio>
#include <cmath>
int main(){ double x; scanf("%lf",&x); printf("%.6f\n", sqrt(x)); return 0; }'
info "[float_eps] sqrt-x 提交 AC 解 → 期望 status=ac（容差内）"
if submit_and_wait "${CODE_SQRT_AC}" cpp "${SQRT_X_PID}"; then
    assert_status "[float_eps] sqrt-x AC 解" ac
else
    fail "提交失败 POST HTTP=${HTTP_CODE}"
fi

# (3) ignore_trailing: trim-trailing-whitespace AC
api GET "/problems/trim-trailing-whitespace"
TRIM_PID="$(jqb '.data.id')"
CODE_TRIM_AC='#include <iostream>
#include <string>
int main(){
    int n; std::cin >> n;
    std::string line; std::getline(std::cin, line);  // consume trailing newline after n
    for (int i = 0; i < n; ++i) {
        std::getline(std::cin, line);
        // rstrip trailing space (0x20) + tab (0x09)
        while (!line.empty() && (line.back() == 0x20 || line.back() == 0x09)) line.pop_back();
        std::cout << line;
        if (i + 1 < n) std::cout << "\n";
    }
    return 0;
}'
info "[ignore_trailing] trim-trailing-whitespace 提交（输出末尾多空格） → 期望 status=ac"
if submit_and_wait "${CODE_TRIM_AC}" cpp "${TRIM_PID}"; then
    assert_status "[ignore_trailing] trim-trailing-whitespace" ac
else
    fail "提交失败 POST HTTP=${HTTP_CODE}"
fi

# (4) ignore_case: 新建临时题 + AC
# 注意: description 不能含中文 —— admin 路由 nlohmann::json parser 对 0xDD
# 字节拒绝（v1.3.1+ bug, UTF-8 strict mode bug）,需要纯英文 / ASCII。
JT_CASE_SLUG="demo-icase-$(date +%s)"
jt_case_body="$(jq -n --arg slug "${JT_CASE_SLUG}" '{
    slug:$slug, title:"Demo ignore_case", difficulty:"easy",
    description:"# demo ignore_case",
    time_limit_ms:1000, memory_limit_mb:256,
    samples:[{input:"",output:"hello world",judge_type:"ignore_case"}],
    test_cases:[{input:"",expected_output:"HELLO WORLD",judge_type:"ignore_case",float_epsilon:1e-6}]
}')"
api POST /admin/problems -t "${ADMIN_TOK}" -d "${jt_case_body}"
if [ "${HTTP_CODE}" = "201" ]; then
    api GET "/problems/${JT_CASE_SLUG}"
    JT_CASE_PID="$(jqb '.data.id')"
    CODE_ICASE_AC='#include <iostream>
int main(){ std::cout << "Hello World\n"; return 0; }'
    info "[ignore_case] 新建临时题 → 提交 Hello World（vs 期望 HELLO WORLD）→ 期望 status=ac"
    if submit_and_wait "${CODE_ICASE_AC}" cpp "${JT_CASE_PID}"; then
        assert_status "[ignore_case] 大小写归一化 AC" ac
    else
        fail "提交失败 POST HTTP=${HTTP_CODE}"
    fi
else
    skip "[ignore_case] 新建临时题失败 HTTP=${HTTP_CODE}"
fi

# (5) ignore_all_whitespace: 新建临时题 + AC
JT_AW_SLUG="demo-iaw-$(date +%s)"
jt_aw_body="$(jq -n --arg slug "${JT_AW_SLUG}" '{
    slug:$slug, title:"Demo ignore_all_whitespace", difficulty:"easy",
    description:"# demo ignore_all_whitespace",
    time_limit_ms:1000, memory_limit_mb:256,
    samples:[{input:"",output:"a b c\nd e",judge_type:"ignore_all_whitespace"}],
    test_cases:[{input:"",expected_output:"a b c\nd e\n",judge_type:"ignore_all_whitespace",float_epsilon:1e-6}]
}')"
api POST /admin/problems -t "${ADMIN_TOK}" -d "${jt_aw_body}"
if [ "${HTTP_CODE}" = "201" ]; then
    api GET "/problems/${JT_AW_SLUG}"
    JT_AW_PID="$(jqb '.data.id')"
    CODE_IAW_AC='#include <iostream>
int main(){ std::cout << "a  b\tc\n\nd e\n"; return 0; }'
    info "[ignore_all_whitespace] 新建临时题 → 提交 a  b\\tc\\n\\nd e（vs 期望 a b c\\nd e）→ 期望 status=ac"
    if submit_and_wait "${CODE_IAW_AC}" cpp "${JT_AW_PID}"; then
        assert_status "[ignore_all_whitespace] 行内多空格折叠 AC" ac
    else
        fail "提交失败 POST HTTP=${HTTP_CODE}"
    fi
else
    skip "[ignore_all_whitespace] 新建临时题失败 HTTP=${HTTP_CODE}"
fi

# (6) special: admin PUT SPJ → 提交正解 → AC → DELETE → 兜底
JT_SPJ_SLUG="demo-spj-$(date +%s)"
jt_spj_body="$(jq -n --arg slug "${JT_SPJ_SLUG}" '{
    slug:$slug, title:"Demo SPJ", difficulty:"easy",
    description:"# demo Special Judge",
    time_limit_ms:1000, memory_limit_mb:256,
    samples:[{input:"",output:"hello",judge_type:"special"}],
    test_cases:[{input:"",expected_output:"hello",judge_type:"special",float_epsilon:1e-6}]
}')"
api POST /admin/problems -t "${ADMIN_TOK}" -d "${jt_spj_body}"
if [ "${HTTP_CODE}" = "201" ]; then
    api GET "/problems/${JT_SPJ_SLUG}"
    JT_SPJ_PID="$(jqb '.data.id')"
    spj_src='// always-AC SPJ: byte-compare expected vs actual
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    FILE* e = std::fopen(argv[2], "rb");
    FILE* a = std::fopen(argv[3], "rb");
    if (!e || !a) return 2;
    int c1, c2;
    do { c1 = std::fgetc(e); c2 = std::fgetc(a);
         if (c1 != c2) { std::fclose(e); std::fclose(a); return 1; }
    } while (c1 != EOF && c2 != EOF);
    std::fclose(e); std::fclose(a);
    return 0;
}'
    spj_body="$(jq -n --arg s "${spj_src}" '{source:$s,language:"cpp"}')"
    api PUT "/admin/problems/${JT_SPJ_SLUG}/special-judge" -t "${ADMIN_TOK}" -d "${spj_body}"
    if [ "${HTTP_CODE}" = "200" ]; then
        pass "admin PUT SPJ（200）"

        # 公共 detail 应有 has_special_judge=true
        api GET "/problems/${JT_SPJ_SLUG}"
        hsp="$(jqb '.data.has_special_judge')"
        [ "${hsp}" = "true" ] && pass "[special] has_special_judge=true" \
            || fail "[special] has_special_judge='${hsp}'"

        # 提交正解 → AC
        CODE_SPJ_AC='#include <iostream>
int main(){ std::cout << "hello\n"; return 0; }'
        info "[special] 提交正解 hello（vs 期望 hello）→ 期望 status=ac（SPJ 接受）"
        if submit_and_wait "${CODE_SPJ_AC}" cpp "${JT_SPJ_PID}"; then
            assert_status "[special] SPJ 接受 → AC" ac
        else
            fail "提交失败 POST HTTP=${HTTP_CODE}"
        fi

        # DELETE → 公共 detail has_special_judge=false → 提交正解 → 兜底 WA
        api DELETE "/admin/problems/${JT_SPJ_SLUG}/special-judge" -t "${ADMIN_TOK}"
        if [ "${HTTP_CODE}" = "204" ]; then
            pass "admin DELETE SPJ（204）"
            api GET "/problems/${JT_SPJ_SLUG}"
            hsp2="$(jqb '.data.has_special_judge')"
            [ "${hsp2}" = "false" ] && pass "[special] DELETE 后 has_special_judge=false" \
                || fail "[special] DELETE 后 has_special_judge='${hsp2}'"
            info "[special] 无 SPJ 兜底 → 提交正解仍判 WA（operator 可见题没挂 SPJ）"
            if submit_and_wait "${CODE_SPJ_AC}" cpp "${JT_SPJ_PID}"; then
                assert_status "[special] 无 SPJ 兜底 → WA" wa
            else
                fail "提交失败 POST HTTP=${HTTP_CODE}"
            fi
        fi
    else
        fail "[special] PUT SPJ HTTP=${HTTP_CODE}"
    fi
else
    skip "[special] 新建临时题失败 HTTP=${HTTP_CODE}"
fi

# ─────────────── §6 7 种 status 演示 ───────────────
head "§6 7 种 status 端到端演示（two-sum 上提交不同代码）"

# AC
info "[status] two-sum 提交 AC 解 → ac"
if submit_and_wait "${CODE_TWO_SUM_AC}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum AC 解" ac
fi

# WA
CODE_WA='#include <iostream>
int main(){ std::cout << "9 9\n"; return 0; }'
info "[status] two-sum 提交错解 → wa"
if submit_and_wait "${CODE_WA}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum 错解" wa
fi

# TLE
CODE_TLE='int main(){ while(true){ volatile long long x=0; x++; } return 0; }'
info "[status] two-sum 提交死循环 → tle"
if submit_and_wait "${CODE_TLE}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum 死循环" tle
fi

# MLE
CODE_MLE='#include <cstdlib>
#include <cstring>
int main(){
    // 直接分配 500MB char 数组（> 256MB memory_limit_mb）
    // 用 static 数组 / 全局数组可能更稳；这里用堆分配避免栈溢出
    const size_t N = 500ul * 1024 * 1024;
    char* p = (char*)malloc(N);
    if (!p) return 99;  // malloc failed → RE（虽然不是理想路径）
    memset(p, 7, N);
    volatile long long s = 0;
    for (size_t i = 0; i < N; ++i) s += p[i];
    free(p);
    return (int)s;
}'
info "[status] two-sum 提交内存爆炸 → mle"
if submit_and_wait "${CODE_MLE}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum 内存爆炸" mle
fi

# OLE
CODE_OLE='#include <cstdio>
#include <cstdlib>
int main(){
    // 单次输出 17MB（> 16MB 阈值），不循环避免容器被 docker-proxy 杀
    // 用 malloc 而非栈（栈 17MB 会爆 8MB 默认栈）
    const size_t N = 17 * 1024 * 1024;
    char* buf = (char*)malloc(N);
    for (size_t i = 0; i < N; ++i) buf[i] = "a"[0];
    fwrite(buf, 1, N, stdout);
    free(buf);
    return 0;
}'
info "[status] two-sum 提交死循环输出 → ole（> 16MB 立即判）"
if submit_and_wait "${CODE_OLE}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum 死循环输出" ole
fi

# CE
CODE_CE='int main( { return 0; }'
info "[status] two-sum 提交语法错 → ce"
if submit_and_wait "${CODE_CE}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum 语法错" ce
fi

# RE
CODE_RE='#include <cstdlib>
int main(){ abort(); }'
info "[status] two-sum 提交 abort() → re"
if submit_and_wait "${CODE_RE}" cpp "${TWO_SUM_PID}"; then
    assert_status "[status] two-sum abort()" re
fi

# ─────────────── §7 汇总 ───────────────
head "§7 汇总"
echo ""
echo -e "${C_BOLD}══════════════════════════════════════════════════════${C_RESET}"
echo -e "${C_BOLD}  Passed: ${C_GREEN}${PASS}${C_RESET}   Failed: ${C_RED}${FAIL}${C_RESET}   Skipped: ${C_YELLOW}${SKIP}${C_RESET}${C_RESET}"
echo -e "${C_BOLD}══════════════════════════════════════════════════════${C_RESET}"
echo ""
echo -e "${C_BOLD}🌐 在浏览器打开:${C_RESET}"
echo "   http://localhost:8080/                 # 首页"
echo "   http://localhost:8080/login.html        # 登录"
echo "   http://localhost:8080/problem.html?slug=two-sum  # 刷题页"
echo "   http://localhost:8080/admin/problems.html        # 管理后台"
echo ""
echo -e "${C_BOLD}👤 测试账号:${C_RESET}"
echo "   普通用户：${DEMO_USER_FINAL} / ${DEMO_PASS}"
echo "   管理员：  ${ADMIN_USER} / ${ADMIN_PASS}"
echo ""
echo -e "${C_BOLD}📊 进阶面板:${C_RESET}"
echo "   Prometheus:        http://localhost:9090   (默认 profile 不启,需 docker compose --profile monitoring up -d)"
echo "   Grafana:           http://localhost:3000   (同上)"
echo "   caddy 反代:        http://localhost        (默认 80,生产模式 CADDY_MODE=prod)"
echo ""

if [ "${FAIL}" -gt 0 ]; then
    if [ "${DEMO_STRICT}" = "1" ]; then
        err "DEMO 失败（STRICT 模式下失败升级 exit 1）"
        exit 1
    fi
    err "DEMO 有 ${FAIL} 项失败（DEMO_STRICT=0 仍继续；置 1 强约束）"
    exit 0
fi

ok "DEMO 全部通过——请在浏览器体验刷题流程"