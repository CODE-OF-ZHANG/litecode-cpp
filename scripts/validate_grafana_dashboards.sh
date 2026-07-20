#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — Grafana dashboard JSON linter (Phase 9 ★ v1.2.69)
# =============================================================
# 黑盒静态校验 monitoring/grafana/dashboards/*.json：
#   1. JSON.parse（jq 空往返）—— 阻断格式错误入仓
#   2. Prometheus expr 引用 metric name 交叉对照「LiteCode 自身 7 个
#      metric family allowlist」（SPEC §16.4 + v1.2.68 MetricsService
#      实装）—— 阻断 v1.2.57 placeholder dashboard 里残留的
#      litecode_http_requests_total / db_pool_size / active_sessions /
#      process_start_time_seconds / judge_active 这 5 个从未实现的
#      「幽灵指标」漂回仓里（Phase 9 ★ 第二项的契约层覆盖）
#   3. Datasource UID lint —— 校验 monitoring/grafana/datasources.yml
#      里的 Prometheus DS 必须有 uid 字段（v1.2.69 落钉为
#      `prometheus`，否则 panel `datasource.uid: prometheus`
#      找不到 DS → 整面板 "No data"）
#
# 退出码：
#   0 — 全过
#   1 — 至少一个 dashboard JSON 解析失败 / 引用幽灵指标 / DS 没钉 uid
#
# 用法：
#   scripts/validate_grafana_dashboards.sh
# =============================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DASH_DIR="${REPO_ROOT}/monitoring/grafana/dashboards"
DS_FILE="${REPO_ROOT}/monitoring/grafana/datasources.yml"

if [ -t 1 ]; then
  RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; NC=$'\033[0m'
else
  RED=''; GRN=''; YLW=''; NC=''
fi
errs=0
ok=0
warns=0
log_err()  { echo "${RED}ERR${NC}  $*" >&2; errs=$((errs+1)); }
log_warn() { echo "${YLW}WARN${NC} $*";                   warns=$((warns+1)); }
log_ok()   { echo "${GRN}OK${NC}   $*";                   ok=$((ok+1)); }

if ! command -v jq >/dev/null 2>&1; then
  echo "${RED}ERR${NC}  jq not found — install jq (apt/brew/winget) first" >&2
  exit 1
fi

# 自身实现 metric allowlist（v1.2.68 MetricsService 暴露的 7 family）
# ⚠ 此列表必须与 src/app_context_metrics.cpp register_*(...) 完全一致
#    改 metrics.cpp 时记得同步
ALLOWLIST_LITECODE='litecode_submissions_total
litecode_judge_duration_seconds
litecode_judge_queue_size
litecode_judge_running_count
litecode_judge_warm_pool_size
litecode_judge_warm_pool_target
litecode_db_pool_active'

# 把 allowlist 转成 jq 能吃的 JSON 数组
ALLOWLIST_JSON="$(printf '%s\n' "${ALLOWLIST_LITECODE}" \
  | jq -R . | jq -s 'map({(.): 1}) | add // {}')"

# ───── 1+2. JSON.parse + metric allowlist cross-check ─────────
shopt -s nullglob
dash_files=( "${DASH_DIR}"/*.json )
if [ ${#dash_files[@]} -eq 0 ]; then
  log_warn "no dashboards found in ${DASH_DIR}"
fi

# 展开 allowlist：histogram 自动衍生 `<name>_bucket{le=...}` /
# `<name>_sum` / `<name>_count` series（Prometheus text format 标准），
# 一并放行。其他 base name（counter / gauge）原样保留。
EXPANDED_ALLOW_JSON="$(jq -n \
  --argjson base "${ALLOWLIST_JSON}" '
    $base + (
      $base | to_entries | map(
        .key as $k |
        { ($k + "_bucket"): 1, ($k + "_sum"): 1, ($k + "_count"): 1 }
      ) | add // {}
    )
  ')"

# jq 程序：collect expr.litecode_* → 过滤 allowlist → 输出幽灵
JQ_LINT='
  [.. | objects | .expr? | select(type == "string") |
     scan("litecode_[A-Za-z0-9_]+")] | unique |
   map(select(. as $m | $allow[$m] | not)) | .[]
'
export ALLOWLIST_JSON

for f in "${dash_files[@]}"; do
  parsed="$(jq empty "${f}" 2>&1)" || {
    log_err "JSON parse failed: ${f}"
    echo "${parsed}" | head -5 >&2
    continue
  }

  phantom="$(jq -r --argjson allow "${EXPANDED_ALLOW_JSON}" "${JQ_LINT}" "${f}" 2>/dev/null || true)"
  # jq 在没匹配时会输出 "null"；过滤掉
  phantom="$(printf '%s\n' "${phantom}" | grep -v '^null$' || true)"

  if [ -n "${phantom}" ]; then
    while IFS= read -r metric; do
      [ -z "${metric}" ] && continue
      log_err "$(basename "${f}") 引用幽灵指标: ${metric} (v1.2.68 MetricsService 未暴露)"
    done <<< "${phantom}"
    log_ok "JSON parse OK: $(basename "${f}")"
  else
    log_ok "JSON parse OK + litecode_* 全部命中 allowlist: $(basename "${f}")"
  fi
done

# ───── 3. datasource UID lint ────────────────────────────────
if [ ! -f "${DS_FILE}" ]; then
  log_err "datasources.yml 不存在: ${DS_FILE}"
else
  # 简易 awk 找 Prometheus DS block 里的 uid 字段
  if awk '
    /^[[:space:]]*-[[:space:]]*name:[[:space:]]*Prometheus[[:space:]]*$/ { in_prom=1; next }
    in_prom && /^[[:space:]]*-/ && !/^[[:space:]]*-[[:space:]]*name:/ { in_prom=0 }
    in_prom && /^[[:space:]]+uid:[[:space:]]*.+/ { found=1; exit }
    END { exit !found }
  ' "${DS_FILE}"; then
    log_ok "datasources.yml: Prometheus DS 有 uid 字段"
  else
    log_err "datasources.yml: Prometheus DS 缺 uid 字段 —— panel datasource.uid: prometheus 会找不到 DS"
  fi
fi

# ───── 总结 ─────────────────────────────────────────────────
echo
echo "─── 总结 ────────────────"
echo "  OK   = ${ok}"
echo "  WARN = ${warns}"
echo "  ERR  = ${errs}"
echo

if [ "${errs}" -gt 0 ]; then
  echo "${RED}FAIL${NC}: ${errs} 个错误需修复"
  exit 1
fi
echo "${GRN}PASS${NC}: Grafana dashboard 校验通过"
exit 0
