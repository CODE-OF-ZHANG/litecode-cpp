#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/tests/test_judge_e2e.sh (端到端集成测试)
# =============================================================
# 调用 judge.sh 跑真实 C++ 提交，覆盖 SPEC §7 全部状态分支：
#   AC / WA / TLE / MLE / RE / OLE / PE / CE / SE
# 运行环境：
#   - 宿主机：GNU bash + g++ + jq + timeout
#   - JUDGE_HOME/JUDGE_TMP 通过环境变量覆盖到测试临时目录
# 设计：
#   每个测试直接构造 task.json，再 run_judge + 验证顶层 status。
#   host g++ 不支持 GNU ld 标志（MinGW 等）时自动跳过整组测试。
# =============================================================
# shellcheck disable=SC2317
# shellcheck disable=SC1091

set -uo pipefail

# 路径：把 /judge /tmp/judge 都指到测试临时目录
TEST_ROOT="$(mktemp -d -t judge-e2e-XXXXXX)"
export TEST_ROOT
export JUDGE_HOME="${TEST_ROOT}/judge_home"
export JUDGE_TMP="${TEST_ROOT}/tmp"
export JUDGE_LIB_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../lib" >/dev/null 2>&1 && pwd)"
mkdir -p "${JUDGE_HOME}" "${JUDGE_TMP}"

JUDGE_SH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)/judge.sh"
chmod +x "${JUDGE_SH}"

# 探测 host g++ 是否支持 GNU ld 链接标志（-z,now / -z,relro）。
HOST_GCC_OK=0
if g++ -Wl,-z,now -Wl,-z,relro -x c++ -o /tmp/__gcc_probe - </dev/null 2>/dev/null; then
    HOST_GCC_OK=1
    rm -f /tmp/__gcc_probe
fi
DOCKER_OK=0
JUDGE_IMAGE="${JUDGE_IMAGE:-litecode-judge:test}"
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    if docker image inspect "${JUDGE_IMAGE}" >/dev/null 2>&1; then
        DOCKER_OK=1
    fi
fi
SKIP_REASON=""
if [ "${HOST_GCC_OK}" -eq 0 ]; then
    if [ "${DOCKER_OK}" -eq 1 ]; then
        echo "[e2e] host g++ 非 GNU ld；改用 docker 镜像 (${JUDGE_IMAGE}) 跑"
    else
        SKIP_REASON="host g++ (MinGW/clang) 不支持 -Wl,-z,now；需 ${JUDGE_IMAGE} 镜像回退"
    fi
fi

PASS=0
FAIL=0
SKIP=0
ok()   { echo "  ok   - $*"; PASS=$((PASS+1)); }
fail() { echo "  FAIL - $*"; FAIL=$((FAIL+1)); }
skip() { echo "  skip - $*"; SKIP=$((SKIP+1)); }

# 工具
for tool in g++ timeout jq; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        if [ "${tool}" = "jq" ]; then
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
        if ! command -v "${tool}" >/dev/null 2>&1; then
            echo "missing required tool: ${tool}" >&2
            exit 1
        fi
    fi
done
echo "[e2e] using jq: $(jq --version 2>&1 | head -1)"

# ───── 通用 task.json 构造 ─────
make_task() {
    local code="$1"
    local cases_json="$2"
    local tlm="${3:-1000}" mlm="${4:-128}" ctm="${5:-10000}" rht="${6:-30000}" olb="${7:-16777216}"
    jq -n \
        --arg code "${code}" \
        --argjson cases "${cases_json}" \
        --argjson tlm "${tlm}" \
        --argjson mlm "${mlm}" \
        --argjson ctm "${ctm}" \
        --argjson rht "${rht}" \
        --argjson olb "${olb}" \
        '{
            submission_id: 42, language: "cpp", code: $code,
            time_limit_ms: $tlm, memory_limit_mb: $mlm,
            compile_timeout_ms: $ctm, run_hard_timeout_ms: $rht,
            output_limit_bytes: $olb,
            test_cases: $cases
        }' > "${TEST_ROOT}/task.json"
}

run_judge() {
    if [ "${HOST_GCC_OK}" -eq 0 ] && [ "${DOCKER_OK}" -eq 1 ]; then
        # 容器镜像 ENTRYPOINT = /usr/local/bin/judge.sh，CMD = --help
        # 通过 stdin 喂 task.json（经 probe 验证：160 字节原样到达容器内 jq）。
        # 两个 Windows-host-only patch：
        # (1) MSYS_NO_PATHCONV=1 — 阻止 Git Bash 把 /usr/local/lib/judge
        #     翻成 C:/Program Files/Git/usr/local/lib/judge（容器内找不到 common.sh）。
        # (2) "sentinel" 第一个位置参数 /bin/sh：若不加，docker 会把 judge.sh 路径
        #     作为 $1 传给 ENTRYPOINT（judge.sh 本身），judge.sh 把可读的自身当作
        #     task 文件 → jq 解析失败 → SE "submission_id missing"。
        #     /bin/sh 在容器内不可读 → judge.sh 走 stdin 分支读 task.json。
        # v1.2.54: 把 task.json 的 memory_limit_mb 直接透传给 docker run --memory。
        # 之前固定 --memory 256m，MLE case 想在 256m 内分配 200MB → 永远不 OOM →
        # exit 0 → status=ac，掩盖了 mle 分支。
        local task_mem_mb
        task_mem_mb="$(jq -r '.memory_limit_mb // 256' "$1" 2>/dev/null || echo 256)"
        case "${task_mem_mb}" in
            ''|*[!0-9]*) task_mem_mb=256 ;;
        esac
        cat "$1" | MSYS_NO_PATHCONV=1 docker run --rm -i \
            -e "JUDGE_LIB_DIR=/usr/local/lib/judge" \
            --network none --memory "${task_mem_mb}m" --pids-limit 50 \
            --security-opt no-new-privileges \
            "${JUDGE_IMAGE}" /nonexistent_sentinel_for_stdin_branch 2>/dev/null
    else
        JUDGE_TASK_FILE="$1" "${JUDGE_SH}" 2>/dev/null
    fi
}

get_status() { jq -r '.status // ""' <<< "$1"; }
get_failed() { jq -r '.failed_case_index // ""' <<< "$1"; }
get_err()    { jq -r '.error_message // ""' <<< "$1"; }

expect_status() {
    local got="$1" want="$2" label="$3"
    if [ "${got}" = "${want}" ]; then
        ok "${label} → status=${want}"
    else
        fail "${label} → got '${got}', want '${want}'"
    fi
}

# 通用守卫：若 host g++ 不兼容且无 docker，则跳过单个测试
guard_or_skip() {
    local label="$1"
    if [ -n "${SKIP_REASON}" ]; then
        skip "[${label}] ${SKIP_REASON}"
        return 1
    fi
    return 0
}

# 把 task.json 跑在 judge 镜像内（host g++ 不兼容时）
# 注意：此处的 docker run 为 e2e 测试驱动，不需要全严格隔离（生产用 web scheduler 配
# 全套 --cpus/--memory/--read-only/--network=none 等参数）；这里只验 judge.sh 是否正确。
docker_run_judge() {
    docker run --rm \
        -v "${TEST_ROOT}:${TEST_ROOT}" \
        -e "JUDGE_TASK_FILE=$1" \
        -e "JUDGE_HOME=${TEST_ROOT}/judge_home" \
        -e "JUDGE_TMP=${TEST_ROOT}/tmp" \
        -e "JUDGE_LIB_DIR=/usr/local/lib/judge" \
        -e "HOST_TESTING_UID=$(id -u 2>/dev/null || echo 1000)" \
        "${JUDGE_IMAGE}" /usr/local/bin/judge.sh 2>&1
}

# ─────────────────────────────────────────────────────────────
# [AC] 简单 echo
# ─────────────────────────────────────────────────────────────
if guard_or_skip "AC"; then
    code='
#include <iostream>
int main() { std::cout << "hello\n"; return 0; }
'
    cases='[{"input":"","expected_output":"hello\n","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "AC simple echo"
fi

# ─────────────────────────────────────────────────────────────
# [AC] empty expected_output regression
# 锁定 judge.sh:212-217 的 `sed -e '$ {/^$/d}'` 行为：
# 当 expected_output 是空字符串时，jq -r 会输出 "\n"（行终止符），
# buggy 代码会让 expected.txt = "\n" (1 byte) vs 沉默 solution = ""
# → cmp 不等 → WA；
# fixed 代码经 sed 删空行 → expected.txt = "" (0 byte) → AC。
# (实测: 在 alpine + jq 1.6 下，jq -r '""' 输出 1 byte "\n"，
#  sed '$ {/^$/d}' 把这唯一一行空行删掉 → 0 bytes。)
# ─────────────────────────────────────────────────────────────
if guard_or_skip "AC-empty-expected"; then
    code='#include <iostream>
int main() { return 0; }'
    cases='[{"input":"","expected_output":"","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "AC with empty expected_output (jq-empty-line regression)"
fi

# ─────────────────────────────────────────────────────────────
# [WA] 输出与预期不匹配
# ─────────────────────────────────────────────────────────────
if guard_or_skip "WA"; then
    code='
#include <iostream>
int main() { std::cout << "wrong\n"; return 0; }
'
    cases='[
        {"input":"","expected_output":"hello\n","judge_type":"exact","float_epsilon":1e-6},
        {"input":"","expected_output":"wrong\n","judge_type":"exact","float_epsilon":1e-6}
    ]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "wa" "WA wrong answer"
    fid="$(get_failed "${out}")"
    [ "${fid}" = "0" ] && ok "WA captures failed index 0" || fail "WA index want=0 got=${fid}"
fi

# ─────────────────────────────────────────────────────────────
# [CE] 语法错误
# ─────────────────────────────────────────────────────────────
if guard_or_skip "CE-syntax"; then
    code='int main( { return 0; }'
    cases='[{"input":"","expected_output":"x","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ce" "CE syntax error"
    em="$(get_err "${out}")"
    [ -n "${em}" ] && ok "CE carries error_message" || fail "CE no error_message"
fi

# ─────────────────────────────────────────────────────────────
# [CE] 编译炸弹防护（VM 递归）
# ─────────────────────────────────────────────────────────────
if guard_or_skip "CE-bomb"; then
    code='
#include <iostream>
template<int N> struct BOMB {
    static const int value = BOMB<N-1>::v + BOMB<N-2>::v + BOMB<N-3>::v +
                             BOMB<N-4>::v + BOMB<N-5>::v;
};
template<> struct BOMB<0> { static const int v = 1; };
template<> struct BOMB<1> { static const int v = 1; };
template<> struct BOMB<2> { static const int v = 1; };
template<> struct BOMB<3> { static const int v = 1; };
template<> struct BOMB<4> { static const int v = 1; };
int main() { return BOMB<35>::value ? 0 : 1; }
'
    cases='[{"input":"","expected_output":"x","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}" 1000 128 1500 30000 16777216
    out="$(run_judge "${TEST_ROOT}/task.json")"
    got="$(get_status "${out}")"
    if [ "${got}" = "ce" ]; then
        ok "CE compile-bomb (status=ce)"
    else
        [ "${got}" != "ac" ] && ok "CE compile-bomb non-AC (status=${got})" || fail "CE compile-bomb unexpectedly ac"
    fi
fi

# ─────────────────────────────────────────────────────────────
# [TLE] while(true) 死循环
# ─────────────────────────────────────────────────────────────
if guard_or_skip "TLE"; then
    code='#include <iostream>
int main() { while (true) { volatile long long x = 0; x++; } return 0; }'
    cases='[{"input":"","expected_output":"x","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}" 200 128 10000 30000 16777216
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "tle" "TLE infinite loop"
fi

# ─────────────────────────────────────────────────────────────
# [RE] 段错误
# ─────────────────────────────────────────────────────────────
if guard_or_skip "RE"; then
    code='#include <iostream>
int main() { int* p = nullptr; std::cout << *p; return 0; }'
    cases='[{"input":"","expected_output":"1","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "re" "RE segfault"
fi

# ─────────────────────────────────────────────────────────────
# [OLE] 1MB+ 输出
# ─────────────────────────────────────────────────────────────
if guard_or_skip "OLE"; then
    code='#include <cstdio>
#include <vector>
int main() {
    std::vector<char> buf(4096, "a"[0]);
    while (true) {
        std::fwrite(buf.data(), 1, buf.size(), stdout);
        std::fflush(stdout);
    }
    return 0;
}'
    cases='[{"input":"","expected_output":"a","judge_type":"exact","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}" 1000 128 10000 30000 1048576
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ole" "OLE > 1MB"
fi

# ─────────────────────────────────────────────────────────────
# [MLE] 申请超过 memory_limit_mb 的大数组 → kill by OOM
# v1.2.54: run_judge() 现在按 task.json 的 memory_limit_mb 透传给 docker run
# 的 --memory 参数；镜像自带 cgroup v2，OOM-killer 会 SIGKILL 进程 → exit 137
# → judge.sh 进入 mle 分支（line 408）。
# 之前固定 --memory 256m 时，MLE case 在 200MB 分配下从不死；status 永远 ac。
# ─────────────────────────────────────────────────────────────
if guard_or_skip "MLE"; then
    code='#include <vector>
int main() { std::vector<int> v(200 * 1024 * 1024 / 4, 0); return (int)v.size(); }'
    cases='[{"input":"","expected_output":"0","judge_type":"exact","float_epsilon":1e-6}]'
    # args: tlm mlm ctm rht olb — 1000ms / 64MB / 10000ms / 30000ms / 16MB
    make_task "${code}" "${cases}" 1000 64 10000 30000 16777216
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "mle" "MLE 200MB > 64MB limit"
fi

# ─────────────────────────────────────────────────────────────
# [PE] ignore_trailing 行尾空白容忍（这里走 AC 分支）
# ─────────────────────────────────────────────────────────────
if guard_or_skip "ignore_trailing"; then
    code='#include <iostream>
int main() { std::cout << "42  \n"; return 0; }'
    cases='[{"input":"","expected_output":"42","judge_type":"ignore_trailing","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "ignore_trailing matching"
fi

# ─────────────────────────────────────────────────────────────
# [float_eps] 容差内
# 锁定 compare.sh::compare_float_eps 用 gawk 多维数组 a_tok[lnA][i] 的能力。
# 镜像已自带 gawk（v1.2.53），ubuntu:22.04 显式 update-alternatives --set。
# ─────────────────────────────────────────────────────────────
if guard_or_skip "float_eps"; then
    code='#include <cstdio>
int main(){ printf("3.14159 3.141593\n"); return 0; }'
    cases='[{"input":"","expected_output":"3.14159 3.141593","judge_type":"float_eps","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "float_eps within tolerance"
fi

# ─────────────────────────────────────────────────────────────
# [ignore_case] v1.3.1+ — SPEC §4.3 judge_type 扩展
# 用户写 "Hello World" 与期望 "HELLO world" → AC；
# 写 "abc123" 与期望 "abc124" → WA（数字差异）。
# ─────────────────────────────────────────────────────────────
if guard_or_skip "ignore_case"; then
    code='#include <iostream>
int main(){ std::cout << "Hello World\n"; return 0; }'
    cases='[{"input":"","expected_output":"HELLO world","judge_type":"ignore_case","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "ignore_case 大小写归一化 → AC"

    code='#include <iostream>
int main(){ std::cout << "abc123\n"; return 0; }'
    cases='[{"input":"","expected_output":"abc124","judge_type":"ignore_case","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "wa" "ignore_case 非字母差异 → WA"
fi

# ─────────────────────────────────────────────────────────────
# [ignore_all_whitespace] v1.3.1+ — SPEC §4.3 judge_type 扩展
# 用户写 "a  b\tc\n\nd e\n" 与期望 "a b c\nd e\n" → AC；
# 内容不同（"alpha" vs "gamma"）→ WA。
# ─────────────────────────────────────────────────────────────
if guard_or_skip "ignore_all_whitespace"; then
    code='#include <iostream>
int main(){ std::cout << "a  b\tc\n\nd e\n"; return 0; }'
    cases='[{"input":"","expected_output":"a b c\nd e\n","judge_type":"ignore_all_whitespace","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "ignore_all_whitespace 行内多空格折叠 + 空行忽略 → AC"

    code='#include <iostream>
int main(){ std::cout << "alpha\nbeta\n"; return 0; }'
    cases='[{"input":"","expected_output":"alpha\ngamma\n","judge_type":"ignore_all_whitespace","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "wa" "ignore_all_whitespace 内容不同 → WA"
fi

# ─────────────────────────────────────────────────────────────
# [special] v1.3.1 — 闭合 SPJ 端到端 (SPEC §11 Phase 4 ☆ / §4.3)
#
# 三层断言（v1.2.52 占位只验"无 SPJ → WA"）：
#   a) 无 SPJ → WA（保留 v1.2.52 行为作为回归保险）
#   b) 有 SPJ + 正解 → AC（验证 compile_spj + compare_special_with
#      三参数顺序 <bin> <input> <expected> <actual> + rc=0 走通）
#   c) SPJ 编译失败 → 整 submission SE（验证 SPJ_COMPILE_RC != 0 时
#      每个 special case 都 emit SE + FINAL_STATUS='se'）
# judge.sh section "判 Special Judge 编译" 已有完整接线（line 233-260 +
# step 3 case special 分支 line 475-554）。
# ─────────────────────────────────────────────────────────────
if guard_or_skip "special"; then
    # (a) 无 SPJ — 保留 v1.2.52 行为
    code='#include <iostream>
int main() { std::cout << "anything\n"; return 0; }'
    cases='[{"input":"","expected_output":"anything","judge_type":"special","float_epsilon":1e-6}]'
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "wa" "special judge w/o SPJ → WA (compare.sh:148)"

    # (b) 有 SPJ + 正解 → AC。
    # SPJ 始终 AC（读三个文件，expected == actual 时返 0），
    # 用户代码输出 "hello" 匹配 expected="hello"。
    spj_always_ac='// minimal SPJ: read three file paths, byte-compare
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    FILE* e = std::fopen(argv[2], "rb");
    FILE* a = std::fopen(argv[3], "rb");
    if (!e || !a) return 2;
    int c1, c2;
    do { c1 = std::fgetc(e); c2 = std::fgetc(a);
         if (c1 != c2) { std::fclose(e); std::fclose(a); return 1; }
    } while (c1 != EOF && c2 != EOF);
    std::fclose(e); std::fclose(a);
    return 0;
}'
    code='#include <iostream>
int main() { std::cout << "hello\n"; return 0; }'
    cases='[{"input":"","expected_output":"hello","judge_type":"special","float_epsilon":1e-6}]'
    jq -n \
        --arg code "${code}" \
        --arg spj  "${spj_always_ac}" \
        --argjson cases "${cases}" \
        '{
            submission_id: 42, language: "cpp", code: $code,
            special_judge_source: $spj, special_judge_language: "cpp",
            time_limit_ms: 1000, memory_limit_mb: 128,
            compile_timeout_ms: 10000, run_hard_timeout_ms: 30000,
            output_limit_bytes: 16777216,
            test_cases: $cases
        }' > "${TEST_ROOT}/task.json"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "special judge with SPJ + 正解 → AC (compile_spj + compare_special_with rc=0)"

    # (c) SPJ 编译失败 → 整 submission SE。
    # 故意给一个 g++ 编译失败的 SPJ：主函数签名错（缺分号）。
    spj_compile_fail='this is not valid C++; int main( { return 0; }'
    code='#include <iostream>
int main() { std::cout << "hello\n"; return 0; }'
    cases='[{"input":"","expected_output":"hello","judge_type":"special","float_epsilon":1e-6}]'
    jq -n \
        --arg code "${code}" \
        --arg spj  "${spj_compile_fail}" \
        --argjson cases "${cases}" \
        '{
            submission_id: 42, language: "cpp", code: $code,
            special_judge_source: $spj, special_judge_language: "cpp",
            time_limit_ms: 1000, memory_limit_mb: 128,
            compile_timeout_ms: 10000, run_hard_timeout_ms: 30000,
            output_limit_bytes: 16777216,
            test_cases: $cases
        }' > "${TEST_ROOT}/task.json"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "se" "SPJ 编译失败 → 整 submission SE (compile_spj rc != 0)"
fi

# ─────────────────────────────────────────────────────────────
# [SE] 缺 submission_id（纯 JSON 解析，无 g++ 兼容问题）
# ─────────────────────────────────────────────────────────────
echo '{"language":"cpp","code":"int main(){return 0;}","test_cases":[]}' > "${TEST_ROOT}/task-bad.json"
out="$(run_judge "${TEST_ROOT}/task-bad.json")"
expect_status "$(get_status "${out}")" "se" "missing submission_id → SE"

# ─────────────────────────────────────────────────────────────
# [CRLF/BOM] 归一化
# ─────────────────────────────────────────────────────────────
if guard_or_skip "CRLF-BOM"; then
    code='#include <iostream>
int main() { int x; std::cin >> x; std::cout << (x*2) << "\n"; return 0; }'
    inp="$(printf '\xef\xbb\xbf21\r\n')"
    exp="$(printf '42\n')"
    cases="$(jq -n --arg i "${inp}" --arg e "${exp}" \
        '[{"input":$i,"expected_output":$e,"judge_type":"exact","float_epsilon":1e-6}]')"
    make_task "${code}" "${cases}"
    out="$(run_judge "${TEST_ROOT}/task.json")"
    expect_status "$(get_status "${out}")" "ac" "CRLF + BOM normalization"
fi

# ─────────────────────────────────────────────────────────────
# [help] --help / true 探活
# ─────────────────────────────────────────────────────────────
out="$("${JUDGE_SH}" --help 2>&1)"
echo "${out}" | grep -q "litecode-judge" && ok "--help banner" || fail "--help banner"
out="$("${JUDGE_SH}" true 2>&1)"
echo "${out}" | grep -q "litecode-judge" && ok "true probe" || fail "true probe"

# ─────────────────────────────────────────────────────────────
echo
echo "Passed: ${PASS}, Failed: ${FAIL}, Skipped: ${SKIP}"
rm -rf "${TEST_ROOT}"
exit 0
