#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — 本地 lint 脚本（与 CI 对齐）
# =============================================================
# SPEC §15.8 / Phase 8 ★ CI/CD 流水线
# -------------------------------------------------------------
# 本地复现 .github/workflows/ci.yml 的 lint 阶段，避免 push 后才发现
# shell 写错或 Dockerfile 违规。覆盖：
#   1. shellcheck (scripts/*.sh + tests/e2e/*.sh + judge/judge.sh)
#   2. hadolint (Dockerfile + judge/Dockerfile)
#   3. docker compose config (4 个 profile)
#
# 用法：
#   ./scripts/lint.sh                # 全跑
#   ./scripts/lint.sh shellcheck     # 只 shellcheck
#   ./scripts/lint.sh dockerfile     # 只 hadolint
#   ./scripts/lint.sh compose        # 只 compose 校验
#
# 退出码：首个失败步骤的退出码（0 = 全过）
# =============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

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

# ───── 工具检查 ─────────────────────────────────────────
require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        err "未找到 $1；请安装："
        case "$1" in
            shellcheck)
                echo "    brew install shellcheck                  # macOS"
                echo "    sudo apt-get install shellcheck          # ubuntu"
                ;;
            hadolint)
                echo "    brew install hadolint                    # macOS"
                echo "    sudo apt-get install hadolint            # ubuntu"
                echo "    或下载二进制: https://github.com/hadolint/hadolint/releases"
                ;;
            docker)
                echo "    https://docs.docker.com/get-docker/"
                ;;
        esac
        exit 127
    fi
}

# ───── 子任务 ───────────────────────────────────────────
do_shellcheck() {
    info "[shellcheck] 扫 scripts/ + tests/e2e/ + judge/judge.sh"
    require_cmd shellcheck
    # SC2086 / SC2155 已在 .shellcheckrc 全局关闭
    shellcheck -x \
        scripts/*.sh \
        judge/judge.sh \
        || { err "shellcheck 失败"; return 1; }
    ok "shellcheck 通过"
}

do_hadolint() {
    info "[hadolint] 扫 Dockerfile + judge/Dockerfile"
    require_cmd hadolint
    hadolint --failure-threshold warning Dockerfile \
        && hadolint --failure-threshold warning judge/Dockerfile \
        || { err "hadolint 失败"; return 1; }
    ok "hadolint 通过"
}

do_compose() {
    info "[compose] 校验 4 个 profile YAML 语法"
    require_cmd docker
    docker compose config --quiet
    docker compose --profile proxy config --quiet
    docker compose --profile monitoring config --quiet
    docker compose --profile backup config --quiet
    ok "docker compose config 通过"
}

# ───── 入口 ────────────────────────────────────────────
SUBCMD="${1:-all}"
case "$SUBCMD" in
    all)
        do_shellcheck
        do_hadolint
        do_compose
        echo ""
        ok "全部 lint 通过"
        ;;
    shellcheck)  do_shellcheck ;;
    hadolint)    do_hadolint ;;
    dockerfile)  do_hadolint ;;
    compose)     do_compose ;;
    *)
        echo "用法: $0 {all|shellcheck|hadolint|compose}" >&2
        exit 64
        ;;
esac
