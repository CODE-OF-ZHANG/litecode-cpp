#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/lib/common.sh (公共工具)
# =============================================================
# SPEC §7.4 / 通用工具
# 提供：
#   - log_err              错误输出
#   - now_ms               毫秒时间戳
#   - strip_bom / strip_crlf   文本归一化
#   - truncate_text        截断到 N 字节
#   - json_escape          字符串→安全 JSON 字符串
#   - ceil_div             向上取整除法
#   - update_max           取较大值（按变量名）
#   - find_pid_cgroup      根据 PID 反查 cgroup v2 路径
#   - die_se_json          输出 SE JSON 并退出
# 注意：本文件必须用 `source` 加载，不直接执行。
# =============================================================

if [ -n "${LITECODE_JUDGE_LIB_COMMON_LOADED:-}" ]; then
    return 0
fi
LITECODE_JUDGE_LIB_COMMON_LOADED=1

# 严格模式应在外层 set好；这里保持轻量
# shellcheck disable=SC2155

log_err() { echo "[judge.sh] $*" >&2; }

# ISO8601 纳秒级时间戳 → 毫秒（整数）
now_ms() {
    local ns
    ns="$(date +%s%N 2>/dev/null || true)"
    if [ "${ns:-0}" -gt 0 ] 2>/dev/null; then
        echo $(( ns / 1000000 ))
        return
    fi
    local sec
    sec="$(date +%s)"
    echo $(( sec * 1000 ))
}

# 去 UTF-8 BOM（SPEC §7.4）
strip_bom() {
    sed -E '1s/^\xEF\xBB\xBF//'
}

# 去 CRLF（SPEC §7.4）
strip_crlf() { tr -d '\r'; }

# 截断字符串到 N 字节
truncate_text() {
    local text="$1"
    local limit="$2"
    local bytes
    bytes=$(printf '%s' "${text}" | wc -c)
    if [ "${bytes}" -le "${limit}" ]; then
        printf '%s' "${text}"
        return
    fi
    printf '%s\n[...truncated, total %s bytes]' "${text:0:${limit}}" "${bytes}"
}

# 字符串→安全 JSON 字符串（最小转义）
json_escape() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\n'/\\n}"
    s="${s//$'\t'/\\t}"
    s="${s//$'\r'/}"
    printf '%s' "${s}"
}

# 向上取整除法（b > 0）
ceil_div() {
    local a="$1" b="$2"
    if [ "${b:-0}" -le 0 ] 2>/dev/null; then echo 0; return; fi
    echo $(( (a + b - 1) / b ))
}

# 取较大值（通过变量名）
update_max() {
    local var_name="$1" new_val="$2"
    if [ "${new_val:-0}" -gt "${!var_name:-0}" ] 2>/dev/null; then
        eval "${var_name}=${new_val}"
    fi
}

# 输出 SE JSON 到 stdout 并退出（status=2）
# Args: <submission_id> <error_message>
die_se_json() {
    local sid="${1:-null}" msg="${2:-system error}"
    local msg_esc
    msg_esc="$(json_escape "${msg}")"
    if [ -n "${sid}" ] && [ "${sid}" != "null" ]; then
        printf '{"submission_id":%s,"status":"se","time_used_ms":0,"memory_used_kb":0,"error_message":"%s","failed_case_index":null,"case_results":[]}\n' \
            "${sid}" "${msg_esc}"
    else
        printf '{"submission_id":null,"status":"se","time_used_ms":0,"memory_used_kb":0,"error_message":"%s","failed_case_index":null,"case_results":[]}\n' \
            "${msg_esc}"
    fi
    exit 2
}

# 把 case_results.jsonl 合并成 JSON 数组字符串
# 依赖 $JUDGE_TMP 在外层被设置；缺失则返回 []
emit_case_results_from_jsonl() {
    local tmpl="${JUDGE_TMP:-/tmp/judge}"
    local f="${tmpl}/case_results.jsonl"
    if [ ! -f "${f}" ]; then
        echo "[]"
        return
    fi
    awk 'NR>1{printf ","} {printf "%s", $0}' "${f}" > "${f}.joined" 2>/dev/null || true
    {
        printf '['
        awk 'NR>1{printf ","} {printf "%s", $0}' "${f}" 2>/dev/null
        printf ']'
    }
}

# 输出最终结果 JSON（聚合 case_results.jsonl）
# Args: submission_id status error_message total_time_ms total_mem_kb failed_case_index
emit_final_json() {
    local sid="$1" status="$2" err="$3" total_time_ms="$4" total_mem_kb="$5" failed_idx="${6:-}"
    local err_json="null"
    if [ -n "${err}" ]; then
        err_json="\"$(json_escape "${err}")\""
    fi
    local fid_json="null"
    if [ -n "${failed_idx}" ]; then
        fid_json="${failed_idx}"
    fi
    local cases
    cases="$(emit_case_results_from_jsonl)"
    printf '{"submission_id":%s,"status":"%s","time_used_ms":%s,"memory_used_kb":%s,"error_message":%s,"failed_case_index":%s,"case_results":%s}\n' \
        "${sid}" "${status}" "${total_time_ms}" "${total_mem_kb}" "${err_json}" "${fid_json}" "${cases}"
}
