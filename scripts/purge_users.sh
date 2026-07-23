#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/purge_users.sh (v1.3.4.1)
# -------------------------------------------------------------
# 一次性清理测试账号(/api/v1/auth/register 自动化测试期间堆积的
# demo_user_* 等),保留 admin(可选保留 zhangxu)。
#
# 与 scripts/cleanup_demo_data.sh 的区别:
#   - cleanup_demo_data.sh: 清理 demo_submission.sh 留下的 submissions /
#     problem_revisions / rate_limit_quotas 等业务数据。
#   - purge_users.sh (本脚本): 清理 USERS 账号本身,联动 cascade 删
#     submissions / refresh_tokens 等与 user_id 强绑定的数据;只 SET
#     NULL 不动 audit_logs / problem_revisions(留审计 + 题目归属)。
#
# 用法:
#   bash scripts/purge_users.sh              # dry-run,列出将被删用户
#   bash scripts/purge_users.sh --yes        # 真删(默认保留 admin + zhangxu)
#   bash scripts/purge_users.sh --keep zhangxu testuser1 --yes
#                                            # 只保留 zhangxu + testuser1
#   bash scripts/purge_users.sh --include-admin --yes
#                                            # 也删 admin(危险)
#   KEEP=zhangxu,foo bash scripts/purge_users.sh --yes   # 环境变量等价
#
# 默认保留 admin + zhangxu;admin 包含 (admin/root/sysop/operator) 才识别
#
# 删除策略:
#   users.user_id 级联删除(submissions / refresh_tokens)
#   audit_logs.user_id / problem_revisions.user_id SET NULL(留历史)
#   不动 admin_bypass / roles / 其他表
#
# 退出码:
#   0  —— 完成(含 dry-run)
#   1  —— 删除失败 / 用户确认取消
#   2  —— mysql 容器不通 / 参数错
# =============================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

# ─────────────── .env 加载(与 start.sh 保持一致)────────────────
if [ -f "${ROOT}/.env" ]; then
    set -a
    # shellcheck disable=SC1090
    . "${ROOT}/.env"
    set +a
fi

MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-rootpass_change_me}"

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

# ─────────────── 参数解析 ───────────────
DO_YES=0
INCLUDE_ADMIN=0
KEEP_RAW="${KEEP:-admin,zhangxu}"   # 默认保留名单
while [ $# -gt 0 ]; do
    case "$1" in
        --yes|-y) DO_YES=1; shift;;
        --include-admin) INCLUDE_ADMIN=1; shift;;
        --keep) KEEP_RAW="$2"; shift 2;;
        --dry-run) DO_YES=0; shift;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# *//'
            exit 0;;
        *) err "未知参数: $1"; exit 2;;
    esac
done

# 把 "a,b,c" 切成数组;空字符串跳过
IFS=',' read -r -a KEEP_ARR <<< "$KEEP_RAW"
KEEP_LIST=()
for n in "${KEEP_ARR[@]}"; do
    n="${n// /}"
    [ -z "$n" ] && continue
    KEEP_LIST+=("$n")
done

# 守护:admin 默认在保留名单;若用户显式 --include-admin 才剔除
HAS_ADMIN=0
for n in "${KEEP_LIST[@]}"; do
    if [ "$n" = "admin" ]; then HAS_ADMIN=1; break; fi
done
if [ "$HAS_ADMIN" = "0" ] && [ "$INCLUDE_ADMIN" = "0" ]; then
    KEEP_LIST+=("admin")
fi

# 防止 purge 列表只剩保留名单(等于不删)
if [ "${#KEEP_LIST[@]}" -lt 1 ]; then
    err "保留名单为空 — 拒绝执行(会删光全部用户)"
    exit 2
fi

# ─────────────── MySQL 连通 ───────────────
COMPOSE_FILE="${ROOT}/docker-compose.yml"
if ! docker compose -f "${COMPOSE_FILE}" exec -T \
        -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
        mysql -uroot --protocol=socket litecode \
        -e "SELECT 1" >/dev/null 2>&1; then
    err "无法通过 docker compose exec 连入 mysql 容器"
    exit 2
fi

# 把 KEEP 数组转成 SQL IN 列表("'a','b','c'")。
# usernames 都是 [A-Za-z0-9_.\-]+ 干净的,不需要 SQL 转义;直接 grep
# 检查特殊字符后拼接 — 避免双层 "..." + ${...//...} 的引号嵌套。
mysql_in() {
    local out="" n
    for n in "$@"; do
        case "$n" in
            *[\'\"\\\;]*)
                err "保留名单包含非法 SQL 字符: $n"; return 1 ;;
        esac
        out="${out:+${out},}'${n}'"
    done
    echo "$out"
}

KEEP_SQL="$(mysql_in "${KEEP_LIST[@]}")"

# ─────────────── 列出所有用户 + 计划 ───────────────
head "users 表当前状态(按 id 排序)"
ALL_USERS="$(docker compose -f "${COMPOSE_FILE}" exec -T \
    -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
    mysql -uroot --protocol=socket --batch --skip-column-names litecode -N \
    -e "SELECT id,username,role FROM users ORDER BY id;" 2>/dev/null)"

if [ -z "$ALL_USERS" ]; then
    info "(空)"
    exit 0
fi

# 分类:保留 / 删除
TO_KEEP=()
TO_DELETE=()
while IFS=$'\t' read -r uid uname urole; do
    uid="${uid// /}"; uname="${uname// /}"; urole="${urole// /}"
    is_keep=0
    for n in "${KEEP_LIST[@]}"; do
        if [ "$uname" = "$n" ]; then is_keep=1; break; fi
    done
    if [ "$is_keep" = "1" ]; then
        TO_KEEP+=("$(printf '%4s  %-30s  %s' "$uid" "$uname" "$urole")")
    else
        TO_DELETE+=("$(printf '%4s  %-30s  %s' "$uid" "$uname" "$urole")")
    fi
done <<< "$ALL_USERS"

echo "${C_BOLD}保留 (${#TO_KEEP[@]}):${C_RESET}"
for row in "${TO_KEEP[@]}"; do echo "  $row"; done
echo ""
echo "${C_BOLD}将删除 (${#TO_DELETE[@]}):${C_RESET}"
if [ "${#TO_DELETE[@]}" -eq 0 ]; then
    echo "  (无)"
else
    for row in "${TO_DELETE[@]}"; do echo "  $row"; done
fi

# 子计数:列出 submissions / refresh_tokens / audit_logs 三个关联表
# 当前每个将被删 user 的行数 — 给用户看清楚 cascade 影响
if [ "${#TO_DELETE[@]}" -gt 0 ]; then
    head "将被级联清理的关联数据(估算)"
    NONKEEP_SQL="SELECT id FROM users WHERE username NOT IN (${KEEP_SQL})"
    SUBMISSIONS_TOTAL="$(docker compose -f "${COMPOSE_FILE}" exec -T \
        -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
        mysql -uroot --protocol=socket --batch --skip-column-names litecode -N \
        -e "SELECT IFNULL(SUM(c),0) FROM (SELECT COUNT(*) c FROM submissions WHERE user_id IN (${NONKEEP_SQL})) x;" 2>/dev/null)"
    REFRESH_TOTAL="$(docker compose -f "${COMPOSE_FILE}" exec -T \
        -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
        mysql -uroot --protocol=socket --batch --skip-column-names litecode -N \
        -e "SELECT IFNULL(SUM(c),0) FROM (SELECT COUNT(*) c FROM refresh_tokens WHERE user_id IN (${NONKEEP_SQL})) x;" 2>/dev/null)"
    AUDIT_NULLING="$(docker compose -f "${COMPOSE_FILE}" exec -T \
        -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
        mysql -uroot --protocol=socket --batch --skip-column-names litecode -N \
        -e "SELECT COUNT(*) FROM audit_logs WHERE user_id IN (${NONKEEP_SQL});" 2>/dev/null)"
    REVISION_NULLING="$(docker compose -f "${COMPOSE_FILE}" exec -T \
        -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
        mysql -uroot --protocol=socket --batch --skip-column-names litecode -N \
        -e "SELECT COUNT(*) FROM problem_revisions WHERE user_id IN (${NONKEEP_SQL});" 2>/dev/null)"

    printf "  %-22s %s 行 (FK CASCADE,自动删)\n" "submissions"   "${SUBMISSIONS_TOTAL:-0}"
    printf "  %-22s %s 行 (FK CASCADE,自动删)\n" "refresh_tokens" "${REFRESH_TOTAL:-0}"
    printf "  %-22s %s 行 (SET NULL,历史保留)\n" "audit_logs"     "${AUDIT_NULLING:-0}"
    printf "  %-22s %s 行 (SET NULL,历史保留)\n" "problem_revisions" "${REVISION_NULLING:-0}"
fi

# ─────────────── Dry-run / 确认 ───────────────
if [ "${DO_YES}" != "1" ]; then
    head "Dry-run 完成 — 加 --yes 才会执行删除"
    exit 0
fi

if [ "${#TO_DELETE[@]}" -eq 0 ]; then
    info "无用户可删,无需执行"
    exit 0
fi

echo ""
warn "即将永久删除 ${#TO_DELETE[@]} 个账号(及其 submissions / refresh_tokens)!"
if [ -t 0 ] && [ -t 1 ]; then
    read -rp "确认输入 yes 继续: " CONFIRM
    if [ "$CONFIRM" != "yes" ] && [ "$CONFIRM" != "y" ]; then
        info "已取消"
        exit 0
    fi
else
    info "(非交互终端 — 跳过再次确认,直接执行)"
fi

# ─────────────── 执行删除 ───────────────
head "执行删除"
DELETE_SQL="DELETE FROM users WHERE username NOT IN (${KEEP_SQL});"
if docker compose -f "${COMPOSE_FILE}" exec -T \
        -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
        mysql -uroot --protocol=socket litecode \
        -e "${DELETE_SQL}" 2>&1; then
    ok "删除完成"
else
    err "删除失败(回看上方 mysql 错误)"
    exit 1
fi

# 验证
head "完成后状态"
docker compose -f "${COMPOSE_FILE}" exec -T \
    -e "MYSQL_PWD=${MYSQL_ROOT_PASSWORD}" mysql \
    mysql -uroot --protocol=socket litecode \
    -e "SELECT id,username,role,SUBSTRING(created_at,1,19) AS created FROM users ORDER BY id;" 2>&1

ok "排行榜现在只显示保留账号的真实 AC 记录"
