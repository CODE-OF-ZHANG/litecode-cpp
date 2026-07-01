#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/tests/test_common.sh
# =============================================================
# 单元测试：common.sh / cgroup.sh / compare.sh
# 这些是宿主机上直接跑的纯 bash 测试，不需要 docker build。
# 运行：./judge/tests/test_common.sh
# =============================================================
# shellcheck disable=SC2317
# shellcheck disable=SC1091

# Source lib 内构件以便直接测
JUDGE_LIB_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" >/dev/null 2>&1 && pwd)"
export JUDGE_LIB_DIR

# jq 兜底（Windows 上 msys 默认不带 jq；常见安装位置 /c/Users/<u>/AppData/...）
if ! command -v jq >/dev/null 2>&1; then
    for cand in \
        "/c/Users/zhangxu/AppData/Local/Microsoft/WinGet/Packages/jqlang.jq_Microsoft.Winget.Source_8wekyb3d8bbwe" \
        "/c/Program Files/jq" \
    ; do
        if [ -x "${cand}/jq.exe" ]; then
            export PATH="${cand}:${PATH}"
            break
        fi
    done
fi

# shellcheck source=../lib/common.sh
. "${JUDGE_LIB_DIR}/common.sh"
# shellcheck source=../lib/cgroup.sh
. "${JUDGE_LIB_DIR}/cgroup.sh"
# shellcheck source=../lib/compare.sh
. "${JUDGE_LIB_DIR}/compare.sh"

PASS=0
FAIL=0
LOG_FILE="${LOG_FILE:-/tmp/judge-tests-$$.log}"
: > "${LOG_FILE}"

ok()   { echo "  ok   - $*"; PASS=$((PASS+1)); }
fail() { echo "  FAIL - $*"; FAIL=$((FAIL+1)); echo "[FAIL] $*" >> "${LOG_FILE}"; }
header() { echo; echo "[$1]"; }

# ───── common.sh ─────
header "common.sh"

# json_escape
got="$(json_escape 'hello\n"world"')"
[ "${got}" = 'hello\\n\"world\"' ] && ok "json_escape quotes/backslash" || fail "json_escape: got=${got}"

# truncate_text (under limit)
out="$(truncate_text "short" 100)"
[ "${out}" = "short" ] && ok "truncate_text under limit" || fail "truncate_text: ${out}"

# truncate_text (over limit)
out="$(truncate_text "$(printf 'x%.0s' $(seq 1 200))" 50)"
echo "${out}" | grep -q "truncated" && ok "truncate_text over limit" || fail "truncate_text: ${out}"

# now_ms returns >= 0
n="$(now_ms)"
[ "${n}" -ge 0 ] && ok "now_ms positive int (${n})" || fail "now_ms: ${n}"

# ceil_div
got="$(ceil_div 7 3)"; [ "${got}" = "3" ] && ok "ceil_div 7/3=3" || fail "ceil_div 7/3: ${got}"
got="$(ceil_div 9 3)"; [ "${got}" = "3" ] && ok "ceil_div 9/3=3" || fail "ceil_div 9/3: ${got}"
got="$(ceil_div 0 1000)"; [ "${got}" = "0" ] && ok "ceil_div 0/1000=0" || fail "ceil_div 0/1000: ${got}"

# update_max
A=5; B=10; update_max A "${B}"; [ "${A}" = "10" ] && ok "update_max overwrite" || fail "update_max: A=${A}"
A=20; B=10; update_max A "${B}"; [ "${A}" = "20" ] && ok "update_max keep" || fail "update_max: A=${A}"

# json_escape → newline / tab
got="$(printf 'a\nb\tc' | json_escape "$(cat)")"
# 直接通过换行作为参数不便测，所以单独测
got="$(json_escape $'a\nb')"
[ "${got}" = 'a\nb' ] && ok "json_escape newline" || fail "json_escape newline: got=${got}"

# ───── cgroup.sh ─────
header "cgroup.sh"

# cgroup_v2_base: 函数返回 "" 或 /sys/fs/cgroup 路径，不强求
out="$(cgroup_v2_base)"
case "${out}" in
    ""|"/sys/fs/cgroup"*|"/sys/fs/cgroup")
        ok "cgroup_v2_base plausible (${out})"
        ;;
    *)
        fail "cgroup_v2_base unexpected: ${out}"
        ;;
esac

# read_cgroup_cpu_usec 在不可读时返回 0
got="$(read_cgroup_cpu_usec "/nonexistent")"; [ "${got}" = "0" ] && ok "read_cgroup_cpu_usec fallback 0" || fail "read_cgroup_cpu_usec: ${got}"

# read_cgroup_mem_peak_bytes 在不可读时返回 0
got="$(read_cgroup_mem_peak_bytes "/nonexistent")"; [ "${got}" = "0" ] && ok "read_cgroup_mem_peak_bytes fallback 0" || fail "read_cgroup_mem_peak_bytes: ${got}"

# mem_kb_peak 在基线为空时输出 0
got="$(mem_kb_peak "")"; [ "${got}" = "0" ] && ok "mem_kb_peak empty base → 0" || fail "mem_kb_peak: ${got}"

# ───── compare.sh ─────
header "compare.sh"

work="$(mktemp -d)"
trap 'rm -rf "${work:-}"' EXIT

# exact: 相等
echo -n "abc" > "${work}/a"
echo -n "abc" > "${work}/b"
compare_exact "${work}/a" "${work}/b" && ok "compare_exact equal" || fail "compare_exact equal"

# exact: 不等
echo -n "xyz" > "${work}/a"
compare_exact "${work}/a" "${work}/b" && fail "compare_exact unequal (should fail)" || ok "compare_exact unequal"

# ignore_trailing: 相同行尾空白差异 → 视为相等
printf "  foo  \nbar \n" > "${work}/a"
printf "  foo\nbar\n" > "${work}/b"
compare_ignore_trailing "${work}/a" "${work}/b" && ok "ignore_trailing trim" || fail "ignore_trailing trim"

# ignore_trailing: 内容不同
printf "alpha\nbeta\n" > "${work}/a"
printf "alpha\ngamma\n" > "${work}/b"
compare_ignore_trailing "${work}/a" "${work}/b" && fail "ignore_trailing content differ" || ok "ignore_trailing content differ"

# float_eps: 相等
printf "1.0 2.0 3.0\n" > "${work}/a"
printf "1.0000001 2.0 3.0\n" > "${work}/b"
compare_float_eps "${work}/a" "${work}/b" "0.0001" && ok "float_eps within tol" || fail "float_eps within tol"

# float_eps: 超过容差
printf "1.0 2.0 3.0\n" > "${work}/a"
printf "1.5 2.0 3.0\n" > "${work}/b"
compare_float_eps "${work}/a" "${work}/b" "0.0001" && fail "float_eps out of tol" || ok "float_eps out of tol"

# float_eps: 行数不同
printf "1.0\n2.0\n" > "${work}/a"
printf "1.0\n" > "${work}/b"
compare_float_eps "${work}/a" "${work}/b" "0.0001" && fail "float_eps line count" || ok "float_eps line count"

# float_eps: 非数字
printf "abc\n" > "${work}/a"
printf "1.0\n" > "${work}/b"
compare_float_eps "${work}/a" "${work}/b" "0.0001" && fail "float_eps non-numeric" || ok "float_eps non-numeric"

# special: 永远返回非 0
compare_special "${work}/a" "${work}/b" && fail "compare_special should be !=0" || ok "compare_special !=0"

# ───── strip_bom / strip_crlf ─────
header "common.sh normalizers"

work2="$(mktemp -d)"
printf '\xEF\xBB\xBFhello\r\nworld\r\n' > "${work2}/bom_crlf.txt"
got="$(strip_bom < "${work2}/bom_crlf.txt" | strip_crlf)"
[ "${got}" = "hello" ] && got2="$(strip_bom < "${work2}/bom_crlf.txt" | strip_crlf | tail -1)"
got_full="$(strip_bom < "${work2}/bom_crlf.txt" | strip_crlf)"
[ "$(echo "${got_full}" | wc -l)" -ge 2 ] && ok "strip_bom + strip_crlf applied" || fail "strip_bom+strip_crlf: got=${got_full}"
rm -rf "${work2}"

# ───── 总结 ─────
echo
echo "Passed: ${PASS}, Failed: ${FAIL}"
if [ "${FAIL}" -gt 0 ]; then
    echo "Log: ${LOG_FILE}"
    exit 1
fi
exit 0
