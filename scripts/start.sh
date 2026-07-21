#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/start.sh
# -------------------------------------------------------------
# 一键启动 server（v1.3.1 final hash `8e9d28a` 起）：
#   1) 探测环境（docker / curl / jq）
#   2) docker compose up -d 启动 11 服务栈
#   3) 等 web 服务 ready（轮询 /api/v1/health 最多 180s）
#   4) 应用 V001-V011 DB migrations
#   5) admin 登录 + bulk-import 7 道种子题（idempotent）
#   6) 打印浏览器入口 + 测试账号
#
# 与 demo_submission.sh 区别：
#   - start.sh: 启动 + 导入题 + 打印入口（不带演示 case）
#   - demo_submission.sh: 在 start.sh 基础上再演示 6 judge_type + 7 status + SPJ 闭环
#
# 用法：
#   bash scripts/start.sh                            # 默认启动
#   BASE_URL=http://host:8080 bash scripts/start.sh  # 自定义服务地址
#
# 退出码：
#   0  —— 启动 + 导入完成
#   2  —— 启动前置失败（docker daemon 不通 / compose 校验失败）
# =============================================================
# shellcheck disable=SC2317
set -uo pipefail

# ─────────────── 路径解析 ───────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"
COMPOSE_FILE="${ROOT}/docker-compose.yml"
PROBLEMS_DIR="${ROOT}/problems"

# ─────────────── 配置 ───────────────
BASE_URL="${BASE_URL:-http://localhost:8080}"
BASE_URL="${BASE_URL%/}"
API="${BASE_URL}/api/v1"
ADMIN_USER="${ADMIN_USER:-admin}"
ADMIN_PASS="${ADMIN_PASS:-admin123!}"
COMPOSE_TIMEOUT_S="${COMPOSE_TIMEOUT_S:-180}"

# ─────────────── 颜色 ───────────────
if [ -t 1 ]; then
    C_BOLD="\033[1m"; C_GREEN="\033[32m"; C_YELLOW="\033[33m"
    C_RED="\033[31m"; C_CYAN="\033[36m"; C_RESET="\033[0m"
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_CYAN=""; C_RESET=""
fi
info() { echo -e "${C_BOLD}[*]${C_RESET} $*"; }
ok()   { echo -e "${C_GREEN}[✓]${C_RESET} $*"; }
warn() { echo -e "${C_YELLOW}[!]${C_RESET} $*"; }
err()  { echo -e "${C_RED}[✗]${C_RESET} $*" >&2; }
head() { echo -e "\n${C_CYAN}══ $* ══${C_RESET}"; }

# ─────────────── 依赖 ───────────────
need_cmd() { command -v "$1" >/dev/null 2>&1 || { err "缺少依赖 $1"; exit 2; }; }
need_cmd curl
need_cmd docker
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
TMPD="$(mktemp -d -t lc-start-XXXXXX)"
RESP_BODY="${TMPD}/body"
trap 'rm -rf "${TMPD}"' EXIT

api() {
    local method="$1" path="$2"; shift 2
    local token=""
    while [ $# -gt 0 ]; do
        case "$1" in
            -t) token="$2"; shift 2;;
            *) shift;;
        esac
    done
    local url="${API}${path}"
    local -a args=(-sS -X "${method}" -o "${RESP_BODY}" \
                   -w '%{http_code}' --max-time 30)
    [ -n "${token}" ] && args+=(-H "Authorization: Bearer ${token}")
    HTTP_CODE="$(curl "${args[@]}" "${url}" 2>/dev/null)" || HTTP_CODE="000"
}
jqb() { jq -r "$1" "${RESP_BODY}" 2>/dev/null; }

# ════════════════════════════════════════════════════════════
# §1 启动 docker-compose
# ════════════════════════════════════════════════════════════
head "§1 启动 docker-compose 11 服务栈"

if ! docker info >/dev/null 2>&1; then
    err "docker daemon 不通——请先启动 Docker Desktop（Windows: 右下托盘 → Start）"
    err "Linux/macOS: sudo systemctl start docker 或启动 Docker Desktop.app"
    exit 2
fi
ok "docker daemon OK"

if ! docker compose -f "${COMPOSE_FILE}" config --quiet 2>/dev/null; then
    err "docker-compose.yml 校验失败"
    exit 2
fi
ok "docker compose config OK"

info "docker compose up -d..."
docker compose -f "${COMPOSE_FILE}" up -d 2>&1 | tail -20 || {
    err "docker compose up -d 失败"
    exit 2
}
ok "compose up -d 完成"

# ════════════════════════════════════════════════════════════
# §2 等 web 服务 ready
# ════════════════════════════════════════════════════════════
head "§2 等 web 服务 ready（轮询 /api/v1/health 最多 ${COMPOSE_TIMEOUT_S}s）"
info "等 mysql + web healthcheck 通过 + judge 镜像构建..."
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
    err "服务未在 ${COMPOSE_TIMEOUT_S}s 内 ready"
    err "排错: docker compose logs web | docker compose logs judge"
    exit 2
fi
ok "web 服务 ready（HTTP=$(jqb '.status'), docker=$(jqb '.docker'), warm_pool=$(jqb '.warm_pool')）"

# ════════════════════════════════════════════════════════════
# §3 应用 DB migrations
# ════════════════════════════════════════════════════════════
head "§3 应用 V001-V011 DB migrations"
MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-rootpass}"
if command -v mysql >/dev/null 2>&1; then
    if MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" mysql -h 127.0.0.1 -P 3306 -uroot \
            --protocol=TCP -e "SELECT 1" >/dev/null 2>&1; then
        info "应用 migrations..."
        if bash "${ROOT}/scripts/init_db.sh" root "${MYSQL_ROOT_PASSWORD}" \
                127.0.0.1 3306 litecode 2>&1 | tail -8; then
            ok "V001-V011 migrations 应用完成"
        else
            warn "init_db.sh 返回非 0（可能 V001-V011 已全部应用）"
        fi
    else
        warn "无法直连 MySQL 3306（容器初始化可能已自动跑），跳过显式 migrate"
    fi
else
    warn "无 mysql 客户端，跳过显式 migrate（容器 web 启动时会自动应用 V001-V011）"
fi

# ════════════════════════════════════════════════════════════
# §4 admin 登录 + bulk-import 7 道种子题
# ════════════════════════════════════════════════════════════
head "§4 admin 登录 + bulk-import 7 道种子题"

# admin 登录
body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" \
    '{username:$u,password:$p}')"
api POST /auth/login -d "${body}"
if [ "${HTTP_CODE}" != "200" ]; then
    err "admin 登录失败（凭据 ${ADMIN_USER}/${ADMIN_PASS}）"
    err "如已修改密码或首次未自动植入：mysql> SELECT * FROM users WHERE username='admin';"
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
IMPORTED=0; SKIPPED=0
for slug in "${SEEDS[@]}"; do
    if [ ! -f "${PROBLEMS_DIR}/${slug}.json" ]; then
        warn "${slug}.json 不存在，跳过"
        continue
    fi
    api POST "/admin/problems/import?on_duplicate=skip" -t "${ADMIN_TOK}" \
        -F "files=@${PROBLEMS_DIR}/${slug}.json;type=application/json"
    if [ "${HTTP_CODE}" = "200" ]; then
        IMPORTED=$((IMPORTED + 1))
    else
        SKIPPED=$((SKIPPED + 1))
    fi
done
ok "bulk-import 完成（imported=${IMPORTED}, skipped=${SKIPPED}）"

# ════════════════════════════════════════════════════════════
# §5 浏览器入口
# ════════════════════════════════════════════════════════════
head "§5 完成 — 浏览器入口"
echo ""
echo -e "${C_BOLD}🌐 Web 入口:${C_RESET}"
echo "   首页           http://localhost:8080/"
echo "   注册           http://localhost:8080/register.html"
echo "   登录           http://localhost:8080/login.html"
echo "   题目列表       http://localhost:8080/index.html"
echo "   刷题页(two-sum) http://localhost:8080/problem.html?slug=two-sum"
echo "   排行榜         http://localhost:8080/ranking.html"
echo "   个人主页       http://localhost:8080/profile.html"
echo "   管理后台       http://localhost:8080/admin/dashboard.html"
echo ""
echo -e "${C_BOLD}👤 账户:${C_RESET}"
echo "   管理员:  ${ADMIN_USER} / ${ADMIN_PASS}    （请尽快改密码）"
echo ""
echo -e "${C_BOLD}📚 已导入题（${#SEEDS[@]} 道）:${C_RESET}"
for slug in "${SEEDS[@]}"; do echo "   - ${slug}"; done
echo ""
echo -e "${C_BOLD}🔬 进阶:${C_RESET}"
echo "   跑端到端判题演示（6 judge_type + 7 status + SPJ 闭环）："
echo "   bash scripts/demo_submission.sh"
echo ""
echo -e "${C_BOLD}📋 健康检查:${C_RESET}"
echo "   curl http://localhost:8080/api/v1/health"
echo ""
echo -e "${C_BOLD}🛑 停止服务:${C_RESET}"
echo "   docker compose -f ${COMPOSE_FILE} down    # 停 + 保留数据卷"
echo "   docker compose -f ${COMPOSE_FILE} down -v # 停 + 清数据卷"