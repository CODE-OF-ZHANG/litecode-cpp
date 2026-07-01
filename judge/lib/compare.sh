#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/lib/compare.sh (结果比对器)
# =============================================================
# SPEC §7.1.b / §7.4
# 提供：
#   - compare_exact <out> <expected>           完全相同（已归一化的字节相等）
#   - compare_ignore_trailing <out> <expected> 行级 rstrip 后相等
#   - compare_float_eps <out> <expected> <eps> 浮点 epsilon
#   - compare_special <out> <expected>          special judge（v1.3 占位）
# 全部返回：0 相等，1 不等
# =============================================================

if [ -n "${LITECODE_JUDGE_LIB_COMPARE_LOADED:-}" ]; then
    return 0
fi
LITECODE_JUDGE_LIB_COMPARE_LOADED=1

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

# special judge — v1.3 占位（v1.2/Phase 4 MVP 永远返回不相等，驱动 SE）
compare_special() {
    return 1
}
