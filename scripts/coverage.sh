#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — 本地覆盖率脚本（与 .github/workflows/ci.yml coverage job 对齐）
# =============================================================
# SPEC §15.8 / Phase 8 ★ 覆盖率门禁（v1.2.59）
# -------------------------------------------------------------
# 在本地复现 CI 的 coverage 阶段：
#   1. capture  lcov --capture --directory build → coverage.info / coverage-core.info
#   2. report   lcov --list + genhtml HTML report
#   3. gate     阈值门禁（核心 ≥ 80% / 全库 ≥ 40%，可 env 覆盖）
#
# 用法：
#   ./scripts/coverage.sh                # capture + report + gate（默认）
#   ./scripts/coverage.sh capture         # 只 capture
#   ./scripts/coverage.sh report          # 只 report
#   ./scripts/coverage.sh gate            # 只 gate（解析已生成的 coverage.info）
#
# 前置：已经用 -DLITECODE_ENABLE_COVERAGE=ON 编译过，且 ctest 已跑过生成 .gcda：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#       -DUSE_LOCAL_DEPS=ON -DLITECODE_BUILD_TESTS=ON \
#       -DLITECODE_ENABLE_COVERAGE=ON
#   cmake --build build -j"$(nproc)"
#   docker run -d --rm -p 33060:33060 -e MYSQL_ROOT_PASSWORD=rootpass_change_me \
#       -e MYSQL_DATABASE=litecode_test mysql:8.0.40
#   ./scripts/init_db.sh root rootpass_change_me 127.0.0.1 33060 litecode_test
#   (cd build && ctest -j4 -E 'auth_refresh|auth_cookie_storage|warm_pool' --no-tests=error)
#   ./scripts/coverage.sh all
#
# 退出码：首个失败步骤的退出码（0 = 全过）。
# =============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
CORE_MIN="${CORE_MIN:-80}"
REPO_MIN="${REPO_MIN:-40}"

# ───── 颜色 ─────────────────────────────────────────────
if [ -t 1 ]; then
    C_BOLD="\033[1m"; C_GREEN="\033[32m"; C_YELLOW="\033[33m"
    C_RED="\033[31m"; C_RESET="\033[0m"
else
    C_BOLD=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_RESET=""
fi
info() { echo -e "${C_BOLD}[*]${C_RESET} $*"; }
ok()   { echo -e "${C_GREEN}[✓]${C_RESET} $*"; }
err()  { echo -e "${C_RED}[✗]${C_RESET} $*" >&2; }
warn() { echo -e "${C_YELLOW}[!]${C_RESET} $*"; }

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        err "未找到 $1：请："
        echo "    sudo apt-get install -y lcov              # ubuntu"
        echo "    brew install lcov                        # macOS"
        exit 127
    fi
}

# ───── 子任务 ───────────────────────────────────────────
do_capture() {
    require_cmd lcov
    info "lcov capture from ${BUILD_DIR}/ → coverage-raw.info"
    # --ignore-errors 容许 gcov 版本差异（ubuntu-22.04 + 自带版本偶发 format 噪点）
    lcov --capture --directory "$BUILD_DIR" \
         --output-file coverage-raw.info \
         --ignore-errors mismatch \
         --ignore-errors gcov \
         --ignore-errors unused
    info "filter → coverage.info (whole-repo: src/** minus third_party/tests/build)"
    lcov --remove coverage-raw.info \
         '/usr/*' \
         '*/third_party/*' \
         '*/tests/*' \
         '*/build/*' \
         --output-file coverage.info
    info "extract core aggregate → coverage-core.info"
    # SPEC.md:1119 写明的 5 块：auth / judge / repo / rate_limit / audit
    # audit 与 repo 在 src/db/audit_log_repo.h 重叠，lcov dedups，no harm.
    lcov --extract coverage.info \
         '*/src/auth/*' \
         '*/src/judge/*' \
         '*/src/db/*_repo.h' \
         '*/src/middleware/rate_limit.h' \
         '*/src/routes/admin_audit_log_routes.*' \
         --output-file coverage-core.info
    ok "lcov capture + filter done"
    echo "    whole-repo: coverage.info"
    echo "    core:       coverage-core.info"
}

do_report() {
    require_cmd lcov
    info "lcov --list coverage.info (whole-repo)"
    lcov --list coverage.info
    info "lcov --list coverage-core.info (core aggregate)"
    lcov --list coverage-core.info
    if command -v genhtml >/dev/null 2>&1; then
        info "genhtml → ${BUILD_DIR}/coverage-html/"
        mkdir -p "$BUILD_DIR/coverage-html"
        genhtml coverage.info \
            --output-directory "$BUILD_DIR/coverage-html" \
            --legend --show-details \
            || warn "genhtml 失败（不影响 gate）"
        ok "HTML report: ${BUILD_DIR}/coverage-html/index.html"
    else
        warn "genhtml 未装，跳过 HTML；apt: lcov 包自带 genhtml"
    fi
}

# pct <info_file> → 输出 line coverage %（从 lcov --summary 解析）
pct() {
    local info="$1"
    # lcov --summary 输出形如：" lines......: 78.4% (1234/1576)"
    # awk 取第 2 列（去掉 %），仅取第一行匹配
    lcov --summary "$info" 2>&1 \
        | awk '/lines\.+/ {gsub("%","",$2); print $2; exit}'
}

do_gate() {
    require_cmd lcov
    info "Threshold gate (核心 ≥ ${CORE_MIN}% / 全库 ≥ ${REPO_MIN}%)"
    local core repo
    core=$(pct coverage-core.info)
    repo=$(pct coverage.info)
    echo "    core = ${core}%   (min ${CORE_MIN}%)"
    echo "    repo = ${repo}%   (min ${REPO_MIN}%)"
    # awk BEGIN{} 用于浮点比较（shell 不支持），exit !(c>=m) 反转
    if ! awk -v c="$core" -v m="$CORE_MIN" 'BEGIN{exit !(c+0>=m+0)}'; then
        err "core ${core}% < ${CORE_MIN}% — gate FAIL"
        exit 1
    fi
    if ! awk -v r="$repo" -v m="$REPO_MIN" 'BEGIN{exit !(r+0>=m+0)}'; then
        err "repo ${repo}% < ${REPO_MIN}% — gate FAIL"
        exit 1
    fi
    ok "coverage gate PASS"
}

# ───── 入口 ────────────────────────────────────────────
SUBCMD="${1:-all}"
case "$SUBCMD" in
    all)
        do_capture
        do_report
        do_gate
        echo ""
        ok "coverage: capture + report + gate 全过"
        ;;
    capture)  do_capture ;;
    report)   do_report ;;
    gate)     do_gate ;;
    *)
        echo "用法: $0 {all|capture|report|gate}" >&2
        echo "  env: BUILD_DIR (default build) / CORE_MIN (default 80) / REPO_MIN (default 40)" >&2
        exit 64
        ;;
esac
