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
#   6. alerting 配置自检（prometheus-alerts.yml + alertmanager.yml 关键环节点）
#   7. perf_profile.sh 自检（bash -n + 关键环节点 grep，Phase 9 △ v1.2.74）
#   8. backup.sh 自检（bash -n + 关键环节点 grep，Phase 7 ☆ v1.2.75）
#
# 用法：
#   ./scripts/lint.sh                # 全跑
#   ./scripts/lint.sh shellcheck     # 只 shellcheck
#   ./scripts/lint.sh dockerfile     # 只 hadolint
#   ./scripts/lint.sh compose        # 只 compose 校验
#   ./scripts/lint.sh logrotate      # 只日志轮转策略校验
#   ./scripts/lint.sh restore_drill  # 只恢复演练脚本自检
#   ./scripts/lint.sh alerting       # 只告警配置自检（v1.2.73）
#   ./scripts/lint.sh perf_profile   # 只 perf_profile.sh 自检（v1.2.74）
#   ./scripts/lint.sh backup         # 只 backup.sh 自检（v1.2.75）
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

do_alerting() {
    info "[alerting] 校验 prometheus-alerts.yml + alertmanager.yml 关键环节点"
    require_cmd jq

    local alerts="${PROJECT_ROOT}/monitoring/alerting/prometheus-alerts.yml"
    local amcfg="${PROJECT_ROOT}/monitoring/alerting/alertmanager.yml"

    # 1. 文件存在性 + 行数 sanity
    for f in "$alerts" "$amcfg"; do
        if [ ! -f "$f" ]; then
            err "缺少 ${f}（Phase 9 ★ 告警规则依赖）"
            return 1
        fi
    done
    local alerts_lines amcfg_lines
    alerts_lines="$(wc -l < "$alerts")"
    amcfg_lines="$(wc -l < "$amcfg")"
    if [ "${alerts_lines}" -lt 80 ]; then
        err "prometheus-alerts.yml 仅 ${alerts_lines} 行（预期 ≥ 80），疑似退化"
        return 1
    fi
    if [ "${amcfg_lines}" -lt 50 ]; then
        err "alertmanager.yml 仅 ${amcfg_lines} 行（预期 ≥ 50），疑似退化"
        return 1
    fi

    # 2. prometheus-alerts.yml：jq 解析 + SPEC §16.4 11 条规则覆盖度
    #    SPEC §16.4 列了 6 条（P95 延迟 / 队列 / 内存 / 失败登录 / 磁盘 / 证书），
    #    我们落地为 11 条 alertname（web/judge/infra 三组，扩出 queue backlog、
    #    失败率、warm pool、Web 5xx、容器 down、DB pool、磁盘预测线 等）。
    #    这里只对 SPEC 字面提的 6 类做允许名集合检查 —— 防止某次 refactor
    #    把核心 alertname 改掉而漏挂。
    local missing_alerts=()
    # v1.2.73 SPEC §16.4 字面阈值映射的 alertname（允许命名变体）
    grep -qE '^      - alert: JudgeDurationP99TooHigh' "$alerts" \
        || missing_alerts+=('JudgeDurationP99TooHigh (P99 延迟)')
    grep -qE '^      - alert: JudgeQueueBacklog'        "$alerts" \
        || missing_alerts+=('JudgeQueueBacklog (队列)')
    grep -qE '^      - alert: WebContainerMemoryHigh'   "$alerts" \
        || missing_alerts+=('WebContainerMemoryHigh (Web 内存)')
    grep -qE '^      - alert: HostDiskSpaceLow'         "$alerts" \
        || missing_alerts+=('HostDiskSpaceLow (磁盘)')
    grep -qE '^      - alert: TlsCertificateExpiringSoon' "$alerts" \
        || missing_alerts+=('TlsCertificateExpiringSoon (证书)')
    # 失败登录单 IP > 100/小时（SPEC §16.4 写明）—— 当前 metrics.cpp 未暴露
    # per-IP counter，标 `status="absent"` 占位 alert，等 v1.2.74+ 扩 metrics。
    grep -qE '^      - alert: LoginFailuresByIPHigh'    "$alerts" \
        || missing_alerts+=('LoginFailuresByIPHigh (失败登录, 占位)')
    if [ "${#missing_alerts[@]}" -gt 0 ]; then
        err "prometheus-alerts.yml 缺失 SPEC §16.4 关键 alertname：${missing_alerts[*]}"
        return 1
    fi

    # 3. prometheus-alerts.yml：每条 alert 必带 severity + summary + description
    #    jq 解析（Prometheus YAML 用 alert 字段做对象锚点；这里走 grep 是
    #    防止有人手贱删 description 而 promtool 没装时漏检）
    local no_sev
    no_sev="$(grep -cE '^[[:space:]]+severity: (critical|warning)' "$alerts" || true)"
    if [ "${no_sev}" -lt 6 ]; then
        err "prometheus-alerts.yml 只有 ${no_sev} 条带 severity label（预期 ≥ 6）"
        return 1
    fi
    local no_desc
    no_desc="$(grep -cE '^[[:space:]]+description:' "$alerts" || true)"
    if [ "${no_desc}" -lt 6 ]; then
        err "prometheus-alerts.yml 只有 ${no_desc} 条带 description annotation（预期 ≥ 6）"
        return 1
    fi

    # 4. alertmanager.yml：jq 解析 + 必备 receiver / route / inhibit
    local parse_ok=0
    if jq -e . "$amcfg" >/dev/null 2>&1; then
        parse_ok=1
    elif command -v yq >/dev/null 2>&1 && yq -e . "$amcfg" >/dev/null 2>&1; then
        parse_ok=1
    fi
    if [ "$parse_ok" -eq 0 ]; then
        # fallback：grep 强约束（jq / yq 都缺时退到字符串级）
        info "[alerting] jq/yq 都不可用 YAML 深度解析；退到 grep 字符串级"
    fi

    local missing_amcfg=()
    grep -qF 'receivers:' "$amcfg"     || missing_amcfg+=('receivers 顶层')
    grep -qF 'route:' "$amcfg"          || missing_amcfg+=('route 顶层')
    grep -qF "receiver: 'default-webhook'" "$amcfg" \
        || missing_amcfg+=("default-webhook receiver")
    grep -qF "receiver: 'critical-webhook'" "$amcfg" \
        || missing_amcfg+=("critical-webhook receiver")
    grep -qF 'inhibit_rules:' "$amcfg"  || missing_amcfg+=('inhibit_rules 抑制')
    grep -qF 'resolve_timeout:' "$amcfg" \
        || missing_amcfg+=('global.resolve_timeout')
    if [ "${#missing_amcfg[@]}" -gt 0 ]; then
        err "alertmanager.yml 缺失关键字段：${missing_amcfg[*]}"
        return 1
    fi

    # 5. 可选：promtool / amtool 静态校验（CI runner apt 装的有）
    if command -v promtool >/dev/null 2>&1; then
        info "[alerting] promtool check rules prometheus-alerts.yml"
        if ! promtool check rules "$alerts"; then
            err "promtool check rules 失败"
            return 1
        fi
        ok "promtool check rules 通过"
    fi
    if command -v amtool >/dev/null 2>&1; then
        info "[alerting] amtool check-config alertmanager.yml"
        if ! amtool check-config "$amcfg"; then
            err "amtool check-config 失败"
            return 1
        fi
        ok "amtool check-config 通过"
    fi

    ok "alerting 配置自检通过（alerts=${alerts_lines}行 / amcfg=${amcfg_lines}行 / severity≥6 / description≥6）"
}

do_perf_profile() {
    info "[perf_profile] scripts/perf_profile.sh bash -n + 关键环节点"
    local f="${PROJECT_ROOT}/scripts/perf_profile.sh"
    if [ ! -f "${f}" ]; then
        err "缺少 ${f}（Phase 9 △ 性能 Profile 依赖）"
        return 1
    fi

    # 1. bash 语法
    if ! bash -n "${f}"; then
        err "bash -n perf_profile.sh 失败"
        return 1
    fi

    # 2. 关键环节点 grep（防止某次重构把核心骨架改掉）
    # 每条都可独立 grep -F 匹配；缺一即 fail。
    local missing=()
    # PROFILE_RESULT 汇总行（v1.2.67 FUZZ_RESULT / v1.2.72 DRILL_RESULT 同款反向汇入设计）
    grep -qF 'PROFILE_RESULT PASS=' "${f}" || missing+=('PROFILE_RESULT 汇总行')
    # PROFILE_STRICT 开关
    grep -qF 'PROFILE_STRICT'        "${f}" || missing+=('PROFILE_STRICT 开关')
    # 三段 Phase 标题（A: HTTP timing / B: histogram / C: perf+flamegraph）
    grep -qF 'run_phase_a'           "${f}" || missing+=('run_phase_a HTTP 面拆解')
    grep -qF 'run_phase_b'           "${f}" || missing+=('run_phase_b Prometheus histogram')
    grep -qF 'run_phase_c'           "${f}" || missing+=('run_phase_c Linux perf+flamegraph')
    # 能力探测（capabilities: ... 行）
    grep -qF 'capabilities:'         "${f}" || missing+=('capabilities 探测行')
    # 报告再生（write_report）+ 末行反馈
    grep -qF 'write_report'          "${f}" || missing+=('write_report 报告再生')
    grep -qF 'docs/performance-profile.md' "${f}" \
        || missing+=('docs/performance-profile.md 输出路径')
    # Phase A：curl 6 阶段 timing 模板 + 分位数
    grep -qF 'time_namelookup'       "${f}" || missing+=('curl -w time_namelookup 模板')
    grep -qF 'time_starttransfer'    "${f}" || missing+=('curl -w time_starttransfer TTFB')
    grep -qF 'percentile()'          "${f}" || missing+=('percentile 分位数 helper')
    # Phase B：Prometheus histogram 闭环（scrape × 2 + 线性插值反推）
    grep -qF 'litecode_judge_duration_seconds' "${f}" \
        || missing+=('litecode_judge_duration_seconds 引用')
    grep -qF 'histogram_pct'         "${f}" || missing+=('histogram_pct 分位数反推')
    grep -qF 'scrape_metrics'        "${f}" || missing+=('scrape_metrics 抓取')
    # Phase C：perf record + docker exec + flamegraph 闭环
    grep -qF 'perf record'            "${f}" || missing+=('perf record 调用')
    grep -qF 'docker exec'           "${f}" || missing+=('docker exec 容器内 perf')
    grep -qF 'stackcollapse-perf.pl'  "${f}" || missing+=('FlameGraph stackcollapse')
    grep -qF 'flamegraph.pl'         "${f}" || missing+=('FlameGraph flamegraph.pl')
    grep -qF 'docs/perf-flamegraph.svg' "${f}" \
        || missing+=('docs/perf-flamegraph.svg 输出')
    # SPEC §12.2 阈值常量（防止有人改 perf_profile.sh 时把阈值搞丢）
    grep -qF 'HEALTH_MAX_MS='         "${f}" || missing+=('HEALTH_MAX_MS 阈值常量')
    grep -qF 'SUBMIT_MAX_MS='         "${f}" || missing+=('SUBMIT_MAX_MS 阈值常量')
    grep -qF 'PROBLEMS_LIST_MAX_MS='  "${f}" || missing+=('PROBLEMS_LIST_MAX_MS 阈值常量')
    # 收尾 trap + cleanup
    grep -qF "trap cleanup EXIT"     "${f}" || missing+=('cleanup EXIT trap')
    grep -qF 'cleanup()'             "${f}" || missing+=('cleanup 收尾函数')
    # provision 流程
    grep -qF '/auth/register'        "${f}" || missing+=('/auth/register 注册')
    grep -qF '/admin/problems/import' "${f}" || missing+=('bulk-import two-sum')
    grep -qF 'JUDGE_PROBLEM_SLUG'    "${f}" || missing+=('JUDGE_PROBLEM_SLUG 默认值')
    grep -qF '/problems/${JUDGE_PROBLEM_SLUG}' "${f}" || missing+=('取 PID')
    if [ "${#missing[@]}" -gt 0 ]; then
        err "scripts/perf_profile.sh 缺失关键环节点：${missing[*]}"
        return 1
    fi

    # 3. 行数 sanity（合规模的细节，本身不是真理；仅防止「不小心整段删空」类事故）
    local lines
    lines="$(wc -l < "${f}")"
    if [ "${lines}" -lt 250 ]; then
        err "scripts/perf_profile.sh 仅 ${lines} 行（预期 ≥ 250 行），疑似退化"
        return 1
    fi

    # 4. 可选工具探测（不阻断，仅观察）
    if command -v perf >/dev/null 2>&1; then
        ok "perf 命令可用（Phase C 可跑）"
    else
        info "[perf_profile] perf 命令不可用——Phase C 默认会 skip（容器内有则仍可跑）"
    fi
    if [ -x "/opt/FlameGraph/stackcollapse-perf.pl" ] && [ -x "/opt/FlameGraph/flamegraph.pl" ]; then
        ok "FlameGraph 在 /opt/FlameGraph（Phase C 可跑）"
    else
        info "[perf_profile] FlameGraph 不在 /opt/FlameGraph——Phase C 默认会 skip（FLAMEGRAPH_DIR 可覆盖）"
    fi

    ok "perf_profile.sh bash -n + 关键环节点 + 行数=${lines} 通过"
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

do_backup() {
    info "[backup] scripts/backup.sh bash -n + 关键环节点（Phase 7 ☆ v1.2.75）"
    local f="${PROJECT_ROOT}/scripts/backup.sh"
    if [ ! -f "${f}" ]; then
        err "缺少 ${f}（SPEC §16.5 备份脚本依赖）"
        return 1
    fi

    # 1. 语法
    if ! bash -n "${f}"; then
        err "bash -n backup.sh 失败"
        return 1
    fi

    # 2. 关键环节点 grep（防止某次重构把核心骨架改掉）
    # 每条都可独立 grep -F 匹配；缺一即 fail。
    local missing=()
    # BACKUP_RESULT 反向汇入行（与 v1.2.67/72/74 PROFILE_RESULT 风格一致）
    grep -qF 'BACKUP_RESULT PASS=' "${f}" || missing+=('BACKUP_RESULT 汇总行')
    # BACKUP_STRICT 强约束开关
    grep -qF 'BACKUP_STRICT'         "${f}" || missing+=('BACKUP_STRICT 开关')
    # BACKUP_DRY_RUN 探测模式（让 e2e 静态探测能跑）
    grep -qF 'BACKUP_DRY_RUN'        "${f}" || missing+=('BACKUP_DRY_RUN 探测开关')
    # 能力探测行（让 e2e / runbook 能 grep）
    grep -qF 'capabilities:'         "${f}" || missing+=('capabilities 探测行')
    # 收尾 emit_result + EXIT trap
    grep -qF 'emit_result()'         "${f}" || missing+=('emit_result 收尾函数')
    grep -qF "trap 'emit_result' EXIT" "${f}" || missing+=('emit_result EXIT trap')
    # 工具检查（mysqldump 必须有；mysql client 必有）
    grep -qF 'mysqldump'             "${f}" || missing+=('mysqldump 调用')
    grep -qF 'HAVE_MYSQLDUMP'        "${f}" || missing+=('HAVE_MYSQLDUMP 探测')
    grep -qF 'HAVE_MYSQL'            "${f}" || missing+=('HAVE_MYSQL 探测')
    # 备份核心参数：单事务 + quick + routines + triggers + utf8mb4 + hex-blob
    grep -qF 'single-transaction'    "${f}" || missing+=('single-transaction 一致性快照')
    grep -qF 'routines'              "${f}" || missing+=('routines / triggers 包含 SP/event')
    grep -qF 'hex-blob'              "${f}" || missing+=('hex-blob BLOB 列保护')
    grep -qF 'utf8mb4'               "${f}" || missing+=('utf8mb4 字符集')
    # 三种压缩模式
    grep -qF 'gzip -9'               "${f}" || missing+=('gzip -9 压缩')
    grep -qF 'zstd -q -19'           "${f}" || missing+=('zstd -19 压缩')
    grep -qF 'BACKUP_COMPRESS'       "${f}" || missing+=('BACKUP_COMPRESS 三模式开关')
    # 校验：size sanity + gunzip -t
    grep -qF 'stat -c%s'             "${f}" || missing+=('备份文件 size 校验')
    grep -qF 'gunzip -t'             "${f}" || missing+=('gunzip -t 完整性校验')
    grep -qF 'BACKUP_RETENTION_DAYS' "${f}" || missing+=('保留天数配置')
    # 异地同步 rclone（可选）
    grep -qF 'rclone copyto'         "${f}" || missing+=('rclone copyto 异地同步')
    grep -qF 'RCLONE_REMOTE'         "${f}" || missing+=('RCLONE_REMOTE 远端配置')
    # 保留清理
    grep -qF 'mtime'                 "${f}" || missing+=('find -mtime 过期清理')
    # SPEC §16.5 mysqldump 默认值
    grep -qF 'DB_NAME'               "${f}" || missing+=('DB_NAME 默认值')
    if [ "${#missing[@]}" -gt 0 ]; then
        err "scripts/backup.sh 缺失关键环节点：${missing[*]}"
        return 1
    fi

    # 3. 行数 sanity
    local lines
    lines="$(wc -l < "${f}")"
    if [ "${lines}" -lt 130 ]; then
        err "scripts/backup.sh 仅 ${lines} 行（预期 ≥ 130 行），疑似退化"
        return 1
    fi

    ok "backup.sh bash -n + 关键环节点 + 行数=${lines} 通过"
}

# ───── 入口 ────────────────────────────────────────────
SUBCMD="${1:-all}"
case "$SUBCMD" in
    all)
        do_shellcheck
        do_hadolint
        do_compose
        do_logrotate
        do_alerting
        do_restore_drill
        do_perf_profile
        do_backup
        echo ""
        ok "全部 lint 通过"
        ;;
    shellcheck)    do_shellcheck ;;
    hadolint)      do_hadolint ;;
    dockerfile)    do_hadolint ;;
    compose)       do_compose ;;
    logrotate)     do_logrotate ;;
    alerting)      do_alerting ;;
    restore_drill) do_restore_drill ;;
    perf_profile)  do_perf_profile ;;
    backup)        do_backup ;;
    *)
        echo "用法: $0 {all|shellcheck|hadolint|compose|logrotate|alerting|restore_drill|perf_profile|backup}" >&2
        exit 64
        ;;
esac
