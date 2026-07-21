#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/lib/compare.sh (结果比对器)
# =============================================================
# SPEC §7.1.b / §7.4
# 提供：
#   - compare_exact               <out> <expected>       完全相同（已归一化的字节相等）
#   - compare_ignore_trailing     <out> <expected>       行级 rstrip 后相等
#   - compare_ignore_case         <out> <expected>       ASCII 大小写不敏感（v1.3.1+）
#   - compare_ignore_all_whitespace <out> <expected>     行内多空格折叠 + 空行忽略（v1.3.1+）
#   - compare_float_eps           <out> <expected> <eps> 浮点 epsilon
#   - compare_special             <out> <expected>       special judge（v1.3 闭环 v1.3.1）
# 全部返回：0 相等，1 不等
# =============================================================

if [ -n "${LITECODE_JUDGE_LIB_COMPARE_LOADED:-}" ]; then
    return 0
fi
LITECODE_JUDGE_LIB_COMPARE_LOADED=1

# LITECODE_SPJ_BIN — exported by judge.sh when a problem has a
# problem_special_judges row. compare_special() dispatches to the
# compiled SPJ binary (see judge/lib/spj.sh) when this env var is
# set to an executable. When unset OR the file is missing, we fall
# back to "WA" so a problem with no SPJ attached (yet) still produces
# a deterministic verdict for every judge_type=special case. judge.sh
# sources lib/spj.sh lazily after setting this variable so the
# compare_* family stays composable.
LITECODE_SPJ_BIN="${LITECODE_SPJ_BIN:-}"

# 完全相同（精度交给上游的 CRLF/BOM 归一化）
compare_exact() {
    local a="$1" b="$2"
    if cmp -s "${a}" "${b}"; then
        return 0
    fi
    return 1
}

# ignore_trailing — 行级 rstrip 后逐行 cmp（SPEC §7.1.b）
compare_ignore_trailing() {
    local a="$1" b="$2"
    local tmp_a tmp_b
    tmp_a="$(mktemp -p /tmp judge-ita-XXXXXX)"
    tmp_b="$(mktemp -p /tmp judge-itb-XXXXXX)"
    # rstrip 去掉 [ \t]+ 在每行末尾
    sed -E 's/[ \t]+$//' "${a}" > "${tmp_a}"
    sed -E 's/[ \t]+$//' "${b}" > "${tmp_b}"
    local rc=1
    if cmp -s "${tmp_a}" "${tmp_b}"; then
        rc=0
    fi
    rm -f "${tmp_a}" "${tmp_b}"
    return "${rc}"
}

# ignore_case — ASCII 大小写不敏感（v1.3.1+ / SPEC §4.3 / §11 Phase 4 ☆ 扩展）
#
# 语义：把所有 ASCII a-z / A-Z 归一化为大写后逐字节 cmp（cmp -i 等价），
#       其它字节（含 UTF-8 多字节字符 / 数字 / 符号 / 中文等）按原样 cmp。
#       与主流 OJ（Codeforces / AtCoder / LeetCode）的 "case-insensitive"
#       输出一致：用户写 "Hello" 与期望 "HELLO" / "hello" 均判 AC。
#
# 实现细节：
#   1) 用 LC_ALL=C tr '[:upper:][:lower:]' '[:lower:][:upper:]' 做字母大小写翻转；
#      tr 在 C locale 下严格按 ASCII 表，不会受 locale 影响。
#   2) 仅翻转一次（不是逐字符 tolower），效率与 tr 等价；不用 sed y/ 因为它
#      在非 GNU sed 上行为差异。
#   3) 输出行结构 / 空白 / 行数变化 → 仍判 1（不等）；ignore_case 只对字母
#      做归一化，不处理空白 / 行数差异（那是 ignore_trailing /
#      ignore_all_whitespace 的语义）。
compare_ignore_case() {
    local a="$1" b="$2"
    local tmp_a tmp_b
    tmp_a="$(mktemp -p /tmp judge-ic-XXXXXX)"
    tmp_b="$(mktemp -p /tmp judge-ic-XXXXXX)"
    LC_ALL=C tr '[:upper:][:lower:]' '[:lower:][:upper:]' < "${a}" > "${tmp_a}"
    LC_ALL=C tr '[:upper:][:lower:]' '[:lower:][:upper:]' < "${b}" > "${tmp_b}"
    local rc=1
    if cmp -s "${tmp_a}" "${tmp_b}"; then
        rc=0
    fi
    rm -f "${tmp_a}" "${tmp_b}"
    return "${rc}"
}

# ignore_all_whitespace — 行内多空格折叠 + 空行忽略（v1.3.1+ / SPEC §4.3）
#
# 语义：把每行 [ \t]+ 折叠成单空格（保留行结构），并删除空行（只剩
#       \n 或全空白字符的行），然后 cmp。比 ignore_trailing 严格：
#       ignore_trailing 只 rstrip 末尾空白，"a  b\n" 期望 "ab\n" 仍 PE；
#       ignore_all_whitespace 把行内多空格折叠为单空格后两边一致才算 AC。
#
# 实现细节：
#   1) sed -E 's/[ \t]+/ /g' — 行内多空格折叠为单空格（不动行尾）
#   2) sed '/^[[:space:]]*$/d' — 删除空行 / 全空白行
#   3) cmp -s — 字节级比较
#   4) 不归一化 tab 为 space（保留 \t 与 " " 的区别），但 [ \t]+ 折叠后
#      tab 与 space 都折叠为单 space，行尾的 [\t ]+ 也被折叠 → 与 ignore_trailing
#      在"末尾空白"语义上重叠但更宽松（行内也折叠）。
#   5) 不处理 BOM / CRLF（已由 judge.sh Section C 落盘前归一化）。
compare_ignore_all_whitespace() {
    local a="$1" b="$2"
    local tmp_a tmp_b
    tmp_a="$(mktemp -p /tmp judge-iaw-XXXXXX)"
    tmp_b="$(mktemp -p /tmp judge-iaw-XXXXXX)"
    sed -E 's/[ \t]+/ /g; /^[[:space:]]*$/d' "${a}" > "${tmp_a}"
    sed -E 's/[ \t]+/ /g; /^[[:space:]]*$/d' "${b}" > "${tmp_b}"
    local rc=1
    if cmp -s "${tmp_a}" "${tmp_b}"; then
        rc=0
    fi
    rm -f "${tmp_a}" "${tmp_b}"
    return "${rc}"
}

# float_eps — 浮点 epsilon 比较（SPEC §7.1.b）
#
# 规则（业内通用 "double epsilon"）：
#   |a - b| <= EPS  ─ 或 ─  |a - b| / max(|a|, |b|) <= EPS  → 相等
#   其余 → 不等
#
# 输入约定：两个文件已经过 CRLF/BOM 归一化（同 judge.sh Section C）。
# 任一文件结构差异（行数 / 每行 token 数 / token 非数字）→ 不等。
#
# 实现：用 getline 把 A 读入内存数组，然后用 <(cat b) 输入校验 B；
# 不支持 a/b 同时含大量行（应贴合 OJ 数据规模，1k 行内即可）。
compare_float_eps() {
    local a="$1" b="$2" eps="$3"
    awk -v EPS_str="${eps}" -v fileA="${a}" '
        function absv(x) { return x < 0 ? -x : x }
        function feq(x, y,    d, big) {
            if (x == y) return 1
            d = absv(x - y)
            if (d <= EPS + 0) return 1
            big = absv(x); if (absv(y) > big) big = absv(y)
            if (big + 0 < 1e-300) return 0
            if (d / big <= EPS + 0) return 1
            return 0
        }
        function isnum(s) {
            if (s == "") return 0
            if (s ~ /^[+-]?[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?$/) return 1
            if (s ~ /^[+-]?[.][0-9]+([eE][+-]?[0-9]+)?$/) return 1
            return 0
        }
        BEGIN {
            EPS = EPS_str + 0
            failed = 0
            # 把 A 一次性读入 a_ntok[ln] / a_tok[ln][i]
            lnA = 0
            while ((getline line < fileA) > 0) {
                lnA++
                if (line == "") {
                    a_ntok[lnA] = 0
                    continue
                }
                n = split(line, t, /[ \t]+/)
                a_ntok[lnA] = n
                for (i = 1; i <= n; i++) a_tok[lnA][i] = t[i]
            }
            close(fileA)
        }
        # 注意：awk 中途 exit 仍会跑 END，END 里的 exit 0 会覆盖。
        # 所以用 failed 标志，最后只在 END 里统一决定。
        {
            lnB = FNR
            if (lnB > lnA) { failed = 1; exit 0 }     # 提前结束，不让 END 跑到
            line = $0
            if (line == "" && a_ntok[lnB] == 0) next
            n = split(line, t, /[ \t]+/)
            if (n != a_ntok[lnB]) { failed = 1; exit 0 }
            for (i = 1; i <= n; i++) {
                if (!isnum(a_tok[lnB][i]) || !isnum(t[i])) { failed = 1; exit 0 }
                if (!feq(a_tok[lnB][i]+0, t[i]+0)) { failed = 1; exit 0 }
            }
            next
        }
        END {
            if (failed) exit 1
            # B 读完后剩余行也算 fail
            if (FNR != lnA) exit 1
            exit 0
        }
    ' "${b}"
}

# special judge — Phase 4 ☆ Special Judge 框架（v1.2.18）
#
# Calls the SPJ binary (compiled from problem_special_judges.source
# upstream by judge.sh) with three file paths: <input> <expected> <out>.
# Returns 0 when the SPJ exits 0 (AC), 1 when non-zero (WA). Exit codes
# 124 / 137 / other collapse to WA here; judge.sh separately decides
# whether to surface SE based on whether the SPJ itself failed to
# compile (compile failure ⇒ SE; SPJ runtime crash on a single case ⇒
# also SE for that one case, but later cases continue — the SPJ's own
# crashes are not "the user's fault").
#
# Contract:
#   compare_special <out> <expected> <input>
#   rc=0 ⇒ AC, rc=1 ⇒ WA, rc=2 ⇒ SE (no SPJ attached)
#
# Fall-back when LITECODE_SPJ_BIN is unset OR the file is not
# executable: we return 1 (WA) so the submission lands in 'wa'
# rather than 'se'. This matches the "judge admin has not yet
# attached an SPJ" intent — the operator can iterate without a
# system error stamp on every run. (Folding 'no SPJ' into 'SE'
# would block the C++ scheduler / admin-dashboard from
# distinguishing "broken problem" from "wrong answer").
compare_special() {
    local out="$1" expected="$2" input="$3"
    if [ -n "${LITECODE_SPJ_BIN}" ] && [ -x "${LITECODE_SPJ_BIN}" ]; then
        # Delegate to spj.sh — needs lib/spj.sh to be sourced by the
        # caller (judge.sh does this in Section D before the test loop).
        compare_special_with "${LITECODE_SPJ_BIN}" "${out}" "${expected}" "${input}"
        return $?
    fi
    # No SPJ ⇒ WA. judge.sh separately writes an info message so the
    # operator can see why every special case is failing.
    return 1
}
