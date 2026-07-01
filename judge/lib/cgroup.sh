#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/lib/cgroup.sh (cgroup v2 测量)
# =============================================================
# SPEC §7.1.b / §7.4 / §15.4
# 在容器内基于 cgroup v2 (unified hierarchy) 读取时间/内存：
#   - cpu  : $CGROUP_BASE/cpu.stat → 累加 usage_usec
#   - memory: $CGROUP_BASE/memory.peak → 容器自创建以来峰值（字节）
# 提供：
#   - cgroup_v2_base                  探测 cgroup v2 挂载根；空 = 不可用
#   - read_cgroup_cpu_usec            当前 usage_usec
#   - read_cgroup_mem_peak_bytes      memory.peak 字节
#   - elapsed_since_ms <from_usec>    从 from_usec 到现在的消耗毫秒
#   - mem_kb_peak                     memory.peak 的 KB 上限
# =============================================================

if [ -n "${LITECODE_JUDGE_LIB_CGROUP_LOADED:-}" ]; then
    return 0
fi
LITECODE_JUDGE_LIB_CGROUP_LOADED=1

LITECODE_CGROUP_FS_ROOT="${LITECODE_CGROUP_FS_ROOT:-/sys/fs/cgroup}"

# 探测 cgroup v2 挂载根
cgroup_v2_base() {
    local relpath
    relpath="$(awk -F: '$1=="0"{print $3; exit}' /proc/self/cgroup 2>/dev/null || true)"
    if [ -z "${relpath}" ]; then
        relpath="/"
    fi
    if [ -d "${LITECODE_CGROUP_FS_ROOT}${relpath}" ] && [ -r "${LITECODE_CGROUP_FS_ROOT}${relpath}/cpu.stat" ]; then
        echo "${LITECODE_CGROUP_FS_ROOT}${relpath}"
        return
    fi
    if [ -r "${LITECODE_CGROUP_FS_ROOT}/cpu.stat" ]; then
        echo "${LITECODE_CGROUP_FS_ROOT}"
        return
    fi
    echo ""
}

# cgroup v2 cpu 累计 usage_usec（不可读则返回 0）
read_cgroup_cpu_usec() {
    local base="${1:-}"
    if [ -z "${base}" ] || [ ! -r "${base}/cpu.stat" ]; then
        echo 0
        return
    fi
    awk '/^usage_usec / { print $2; exit }' "${base}/cpu.stat"
}

# cgroup memory.peak（容器创建以来峰值，SPEC §7.4 优先项）
read_cgroup_mem_peak_bytes() {
    local base="${1:-}"
    if [ -z "${base}" ] || [ ! -r "${base}/memory.peak" ]; then
        echo 0
        return
    fi
    cat "${base}/memory.peak" 2>/dev/null || echo 0
}

# 经过 from_usec 之后的毫秒数（向上取整；SPEC §7.4：向上取整为 ms）
elapsed_since_ms() {
    local from_usec="$1"
    local base="${2:-}"
    local to_usec
    to_usec="$(read_cgroup_cpu_usec "${base}")"
    if [ "${to_usec:-0}" -le "${from_usec:-0}" ] 2>/dev/null; then
        echo 0
        return
    fi
    local diff_us=$(( to_usec - from_usec ))
    local b=1000
    echo $(( (diff_us + b - 1) / b ))
}

# 当前 memory.peak（KB，向上取整）
mem_kb_peak() {
    local base="${1:-}"
    local bytes
    bytes="$(read_cgroup_mem_peak_bytes "${base}")"
    bytes="${bytes:-0}"
    if [ "${bytes:-0}" -le 0 ] 2>/dev/null; then echo 0; return; fi
    echo $(( (bytes + 1023) / 1024 ))
}
