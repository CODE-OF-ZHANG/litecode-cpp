#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — 数据库初始化入口脚本
# -------------------------------------------------------------
# 功能：
#   1. 创建数据库（若不存在）
#   2. 按顺序应用 db/migrations/V*.sql 迁移
#   3. 调用 scripts/create_admin.sql 植入初始管理员
#
# 用法：
#   ./init_db.sh                              # 用默认参数
#   ./init_db.sh root mypass                  # 指定 user/password
#   ./init_db.sh root mypass localhost 3306 litecode
#
# 也支持环境变量（优先级低于命令行参数）：
#   MYSQL_USER, MYSQL_PASSWORD, MYSQL_HOST, MYSQL_PORT, DB_NAME
#
# Docker 环境推荐：
#   docker compose exec -T mysql mysql -uroot -p"$MYSQL_ROOT_PASSWORD" < /dev/stdin
#   或：./init_db.sh root "$MYSQL_ROOT_PASSWORD" mysql 3306 litecode
# =============================================================
set -euo pipefail

# ---------- 默认值 ----------
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-}"
MYSQL_HOST="${MYSQL_HOST:-localhost}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
DB_NAME="${DB_NAME:-litecode}"
SKIP_SEED="${SKIP_SEED:-0}"  # 1 = 不植入 admin（CI 场景）

# ---------- 命令行覆盖 ----------
if [ $# -ge 1 ]; then MYSQL_USER="$1"; fi
if [ $# -ge 2 ]; then MYSQL_PASSWORD="$2"; fi
if [ $# -ge 3 ]; then MYSQL_HOST="$3"; fi
if [ $# -ge 4 ]; then MYSQL_PORT="$4"; fi
if [ $# -ge 5 ]; then DB_NAME="$5"; fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MIGRATIONS_DIR="$PROJECT_ROOT/db/migrations"

# ---------- 颜色 / 输出 ----------
if [ -t 1 ]; then
    C_BOLD="\033[1m"; C_GREEN="\033[32m"; C_YELLOW="\033[33m"
    C_RED="\033[31m"; C_RESET="\033[0m"
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_RESET=""
fi
info()  { echo -e "${C_BOLD}[*]${C_RESET} $*"; }
ok()    { echo -e "${C_GREEN}[✓]${C_RESET} $*"; }
warn()  { echo -e "${C_YELLOW}[!]${C_RESET} $*"; }
err()   { echo -e "${C_RED}[✗]${C_RESET} $*" >&2; }

# ---------- 前置检查 ----------
command -v mysql >/dev/null 2>&1 || { err "mysql client 未在 PATH 中"; exit 1; }
[ -d "$MIGRATIONS_DIR" ] || { err "迁移目录不存在: $MIGRATIONS_DIR"; exit 1; }

echo -e "${C_BOLD}════════════════════════════════════════${C_RESET}"
echo -e "${C_BOLD} LiteCode-CPP 数据库初始化${C_RESET}"
echo -e "${C_BOLD}════════════════════════════════════════${C_RESET}"
info "Host:     ${MYSQL_HOST}:${MYSQL_PORT}"
info "Database: ${DB_NAME}"
info "Migrations: ${MIGRATIONS_DIR}"
echo -e "${C_BOLD}════════════════════════════════════════${C_RESET}"

MYSQL_CMD=(mysql
    -h "$MYSQL_HOST"
    -P "$MYSQL_PORT"
    -u "$MYSQL_USER"
    --protocol=TCP
    --default-character-set=utf8mb4
)

# Password argument: -pXXX (or skip when empty to avoid tty prompt)
if [ -n "$MYSQL_PASSWORD" ]; then
    PWD_ARG=(-p"$MYSQL_PASSWORD")
else
    PWD_ARG=()
fi

# ---------- Step 1: 创建数据库 ----------
info "[1/3] 创建数据库 '${DB_NAME}'（若不存在）"
"${MYSQL_CMD[@]}" "${PWD_ARG[@]}" -e \
    "CREATE DATABASE IF NOT EXISTS \`${DB_NAME}\`
       DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
ok "数据库就绪"

# ---------- Step 2: 应用迁移 ----------
info "[2/3] 应用迁移脚本"
shopt -s nullglob
applied_count=0
skipped_count=0

# 排序：V001, V002, ..., V010, V011 ...
for f in $(ls "$MIGRATIONS_DIR"/V*.sql 2>/dev/null | sort); do
    # version 仅取 V001 这种短前缀（去掉 __init 等描述后缀），
    # 与 schema_migrations 表里 INSERT 的 version 一致
    version="$(basename "$f" .sql | cut -d'_' -f1)"

    # 检查是否已应用
    exists=$("${MYSQL_CMD[@]}" "${PWD_ARG[@]}" -N -B "$DB_NAME" \
        -e "SELECT COUNT(*) FROM schema_migrations WHERE version='${version}';" 2>/dev/null || echo "0")

    if [ "${exists:-0}" -gt 0 ]; then
        warn "  ⊙ ${version}（已应用，跳过）"
        skipped_count=$((skipped_count + 1))
        continue
    fi

    info "  → 应用 ${version} ..."
    if "${MYSQL_CMD[@]}" "${PWD_ARG[@]}" "$DB_NAME" < "$f"; then
        ok "  ✓ ${version}"
        applied_count=$((applied_count + 1))
    else
        err "  ✗ ${version} 失败，请检查上方错误"
        exit 1
    fi
done

if [ $applied_count -eq 0 ] && [ $skipped_count -gt 0 ]; then
    warn "所有迁移均已应用，跳过 (共 $skipped_count 个)"
fi

# ---------- Step 3: 初始管理员 ----------
if [ "$SKIP_SEED" = "1" ]; then
    warn "[3/3] SKIP_SEED=1，跳过初始管理员"
else
    info "[3/3] 植入初始管理员账户"
    if "${MYSQL_CMD[@]}" "${PWD_ARG[@]}" "$DB_NAME" \
        < "$SCRIPT_DIR/create_admin.sql"; then
        ok "  ✓ 初始管理员就绪"
    else
        warn "  ! 管理员植入失败（可能已存在，可忽略）"
    fi
fi

# ---------- 验证 ----------
echo ""
echo -e "${C_BOLD}════════════════════════════════════════${C_RESET}"
info "验证：表结构"
"${MYSQL_CMD[@]}" "${PWD_ARG[@]}" "$DB_NAME" -e \
    "SELECT TABLE_NAME, TABLE_ROWS
       FROM information_schema.TABLES
      WHERE TABLE_SCHEMA = '${DB_NAME}'
      ORDER BY TABLE_NAME;" 2>/dev/null || true

echo ""
echo -e "${C_BOLD}════════════════════════════════════════${C_RESET}"
ok "数据库初始化完成"
echo -e "${C_BOLD}════════════════════════════════════════${C_RESET}"
echo ""
echo "默认管理员账户："
echo "  Username: admin"
echo "  Password: admin123!"
echo ""
echo -e "${C_YELLOW}⚠️  首次登录后请立即修改密码！${C_RESET}"