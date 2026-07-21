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
#   3. docker compose config (5 个 profile)
#   4. log rotation policy (all services: json-file + max-size/max-file)
#   5. restore_drill.sh 自检（bash -n + 关键环节点 grep）
#
# 用法：
#   ./scripts/lint.sh                # 全跑
#   ./scripts/lint.sh shellcheck     # 只 shellcheck
#   ./scripts/lint.sh dockerfile     # 只 hadolint
#   ./scripts/lint.sh compose        # 只 compose 校验
#   ./scripts/lint.sh logrotate      # 只日志轮转策略校验
#   ./scripts/lint.sh restore_drill  # 只恢复演练脚本自检
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
    info "[compose] 校验 5 个 profile YAML 语法"
    require_cmd docker
    docker compose config --quiet
    docker compose --profile proxy config --quiet
    docker compose --profile monitoring config --quiet
    docker compose --profile backup config --quiet
    docker compose --profile logging config --quiet
    ok "docker compose config 通过"
}

do_logrotate() {
    info "[logrotate] 校验所有 profile 服务的 Docker 日志轮转策略"
    require_cmd docker
    require_cmd jq

    local rendered bad_services total
    if ! rendered="$(docker compose \
        --profile proxy \
        --profile monitoring \
        --profile backup \
        --profile logging \
        config --format json)"; then
        err "docker compose 完整 profile 渲染失败"
        return 1
    fi

    total="$(jq '.services | length' <<<"$rendered")"
    if [ "$total" -eq 0 ]; then
        err "未发现任何 Compose service"
        return 1
    fi

    bad_services="$(jq -r '
        .services
        | to_entries[]
        | select(
            (.value.logging.driver // "") != "json-file" or
            (.value.logging.options["max-size"] // "") != "10m" or
            (.value.logging.options["max-file"] // "") != "3"
          )
        | .key
    ' <<<"$rendered")"

    if [ -n "$bad_services" ]; then
        err "发现以下 service 的日志轮转策略不符合 json-file/10m/3："
        while IFS= read -r service; do
            [ -z "$service" ] || err "  service: $service"
        done <<<"$bad_services"
        return 1
    fi

    ok "${total} 个 service 均使用 json-file + max-size=10m + max-file=3"
}

do_restore_drill() {
    info "[restore_drill] scripts/restore_drill.sh bash -n + 关键环节点"
    local f="${PROJECT_ROOT}/scripts/restore_drill.sh"
    if [ ! -f "${f}" ]; then
        err "缺少 ${f}（Phase 9 ☆ 备份验证依赖）"
        return 1
    fi

    # 1. 语法
    if ! bash -n "${f}"; then
        err "bash -n restore_drill.sh 失败"
        return 1
    fi

    # 2. 关键环节点 grep（避免某次重构把核心骨架改掉）
    # 期望出现 all，缺一即 fail。每条都可独立 grep -F 匹配。
    local missing=()
    # DRILL_RESULT 汇总行（在 emit_result / 末尾）
    grep -qF 'DRILL_RESULT PASS=' "${f}" || missing+=('DRILL_RESULT 汇总行')
    # RESTORE_STRICT 开关
    grep -qF 'RESTORE_STRICT' "${f}"     || missing+=('RESTORE_STRICT 开关')
    # 能力探测输出（capabilities: ...）
    grep -qF 'capabilities:' "${f}"      || missing+=('能力探测行')
    # 收尾 trap + cleanup
    grep -qF "trap 'cleanup; emit_result' EXIT" "${f}" || missing+=('cleanup EXIT trap')
    grep -qF 'cleanup()' "${f}"          || missing+=('cleanup 收尾函数')
    # drill 容器命名约定（用变量名 + 实际引用都覆盖）
    grep -qF 'MYSQL_DRILL_CONTAINER' "${f}" || missing+=('MYSQL_DRILL_CONTAINER var')
    grep -qF 'litecode-drill-mysql' "${f}"  || missing+=('litecode-drill-mysql 默认名')
    grep -qF 'WEB_DRILL_CONTAINER' "${f}"   || missing+=('WEB_DRILL_CONTAINER var')
    grep -qF 'PROXY_DRILL_CONTAINER' "${f}" || missing+=('PROXY_DRILL_CONTAINER var')
    # backup 灌库 / gzip 校验
    grep -qF 'gunzip -c' "${f}"    || missing+=('gunzip -c 灌 backup')
    grep -qF 'gunzip -t' "${f}"    || missing+=('gunzip -t 完整性校验')
    # smoke 探针
    grep -qF '/auth/login' "${f}"  || missing+=('admin 登录 smoke')
    grep -qF 'JUDGE_PROBLEM_SLUG' "${f}" || missing+=('JUDGE_PROBLEM_SLUG 默认值')
    grep -qF '/problems/${JUDGE_PROBLEM_SLUG}' "${f}" || missing+=('取 PID smoke')
    grep -qF '/admin/problems/import' "${f}" || missing+=('bulk-import smoke')
    grep -qF '/api/v1/metrics' "${f}"      || missing+=('metrics smoke（v1.2.68 配套）')
    if [ "${#missing[@]}" -gt 0 ]; then
        err "scripts/restore_drill.sh 缺失关键环节点：${missing[*]}"
        return 1
    fi

    # 3. 行数 sanity（合规模的细节，本身不是真理；仅防止「不小心整段删空」类事故）
    local lines
    lines="$(wc -l < "${f}")"
    if [ "${lines}" -lt 150 ]; then
        err "scripts/restore_drill.sh 仅 ${lines} 行（预期 ≥ 150 行），疑似退化"
        return 1
    fi

    ok "restore_drill.sh bash -n + 关键环节点 + 行数=${lines} 通过"
}

# ───── 入口 ────────────────────────────────────────────
SUBCMD="${1:-all}"
case "$SUBCMD" in
    all)
        do_shellcheck
        do_hadolint
        do_compose
        do_logrotate
        do_restore_drill
        echo ""
        ok "全部 lint 通过"
        ;;
    shellcheck)    do_shellcheck ;;
    hadolint)      do_hadolint ;;
    dockerfile)    do_hadolint ;;
    compose)       do_compose ;;
    logrotate)     do_logrotate ;;
    restore_drill) do_restore_drill ;;
    *)
        echo "用法: $0 {all|shellcheck|hadolint|compose|logrotate|restore_drill}" >&2
        exit 64
        ;;
esac
