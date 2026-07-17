#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge.sh (判题执行脚本 — 总入口)
# =============================================================
# SPEC §7.1 / §7.3 / §7.4 / §15.4
#
# 任务来源（按优先级探测）：
#   1) 环境变量 JUDGE_TASK_FILE=/path/to/task.json
#   2) 命令行参数 $1=/path/to/task.json
#   3) 从 stdin 读 task.json（docker run -i ... | judge.sh）
#
# 输出（stdout 一行 JSON 描述结果）：
#   {
#     "submission_id":    42,
#     "status":           "ac|wa|tle|mle|re|ole|pe|ce|se",
#     "time_used_ms":     12,
#     "memory_used_kb":   2048,
#     "error_message":    null|"...",
#     "failed_case_index":null|0,
#     "case_results": [
#       { "index": 0, "status": "ac", "time_ms": 12, "mem_kb": 2048, "info": null },
#       ...
#     ]
#   }
#
# 任务 JSON schema（web 侧按此结构下发）：
#   {
#     "submission_id":          42,
#     "language":               "cpp",
#     "code":                   "...source...",
#     "time_limit_ms":          1000,
#     "memory_limit_mb":        256,
#     "compile_timeout_ms":     10000,
#     "run_hard_timeout_ms":    30000,
#     "output_limit_bytes":     16777216,
#     "special_judge_source":   "// (optional) C++ SPJ source code ...",
#     "special_judge_language": "cpp",
#     "test_cases": [
#       {
#         "input":           "...",
#         "expected_output": "...",
#         "judge_type":      "exact|ignore_trailing|float_eps|special",
#         "float_epsilon":   0.000001,
#         "order_num":       0
#       }
#     ]
#   }
#
# 进程退出码：
#   0 = 正常完成（含 CE/RE/TLE 等业务结果）
#   2 = 任务描述错误 / 解析失败（web 应当作 SE 处理）
# =============================================================

set -euo pipefail

# ───── 默认 / 常量 ───────────────────────────────────────────
# JUDGE_HOME / JUDGE_TMP 允许环境变量覆盖，便于宿主机集成测试。
JUDGE_HOME="${JUDGE_HOME:-/judge}"
JUDGE_TMP="${JUDGE_TMP:-/tmp/judge}"
JUDGE_BIN="${JUDGE_HOME}/solution"
JUDGE_SRC_BASE="${JUDGE_HOME}/solution"
JUDGE_LIB_DIR="${JUDGE_LIB_DIR:-/usr/local/lib/judge}"

DEFAULT_COMPILE_TIMEOUT_MS=10000        # SPEC §7.3 / §15.4 — compile-bomb
DEFAULT_RUN_HARD_TIMEOUT_MS=30000       # SPEC §7.4 — overall judge timeout
DEFAULT_OUTPUT_LIMIT_BYTES=$((16 * 1024 * 1024))   # SPEC §7.4 — OLE threshold
COMPILE_ERROR_TRUNCATE_BYTES=4096       # SPEC §7.4
RUNTIME_ERROR_TRUNCATE_BYTES=2048       # SPEC §7.4

# g++ 编译安全标志（SPEC §7.1 / §7.3 / §15.4）
COMPILE_FLAGS=(
    "-O2"
    "-std=c++17"
    "-pipe"
    "-fstack-protector-strong"
    "-D_FORTIFY_SOURCE=2"
    "-Wformat"
    "-Wformat-security"
    "-Wl,-z,now"
    "-Wl,-z,relro"
)

# ───── lib 加载 ─────────────────────────────────────────────
# shellcheck source=/dev/null
for f in "${JUDGE_LIB_DIR}/common.sh" "${JUDGE_LIB_DIR}/cgroup.sh" "${JUDGE_LIB_DIR}/compare.sh" "${JUDGE_LIB_DIR}/spj.sh"; do
    if [ ! -r "${f}" ]; then
        echo "[judge.sh] missing lib file: ${f}" >&2
        exit 2
    fi
done
# shellcheck source=/dev/null
. "${JUDGE_LIB_DIR}/common.sh"
# shellcheck source=/dev/null
. "${JUDGE_LIB_DIR}/cgroup.sh"
# shellcheck source=/dev/null
. "${JUDGE_LIB_DIR}/compare.sh"
# shellcheck source=/dev/null
. "${JUDGE_LIB_DIR}/spj.sh"

# =============================================================
# Section A — 帮助 / 探活（docker-compose build-only 用法）
# =============================================================
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ] || [ "${1:-}" = "true" ]; then
    cat <<'EOF'
litecode-judge (SPEC §7.2 / §7.3)
  usage:
    judge.sh <task.json>
    JUDGE_TASK_FILE=/path/to/task.json judge.sh
    cat task.json | judge.sh
EOF
    exit 0
fi

# =============================================================
# Section B — 解析任务 JSON
# =============================================================
TASK_FILE=""
if [ -n "${JUDGE_TASK_FILE:-}" ] && [ -r "${JUDGE_TASK_FILE}" ]; then
    TASK_FILE="${JUDGE_TASK_FILE}"
elif [ "${1:-}" != "" ] && [ -r "${1}" ]; then
    TASK_FILE="$1"
else
    tmpdir="$(mktemp -d -p /tmp judge-stdin-XXXXXX)"
    TASK_FILE="${tmpdir}/task.json"
    cat > "${TASK_FILE}"
fi

if [ ! -s "${TASK_FILE}" ]; then
    die_se_json "" "task json empty"
fi

set +e
SUBMISSION_ID="$(jq -er '.submission_id // empty' "${TASK_FILE}" 2>/dev/null)"
JQ_RC=$?
set -e
if [ "${JQ_RC}" -ne 0 ] || [ -z "${SUBMISSION_ID:-}" ]; then
    die_se_json "" "submission_id missing"
fi

LANGUAGE="$(jq -r '.language // "cpp"' "${TASK_FILE}" 2>/dev/null)"
LANGUAGE="${LANGUAGE:-cpp}"

CODE="$(jq -r '.code // ""' "${TASK_FILE}" 2>/dev/null)"
if [ -z "${CODE}" ] || [ "${CODE}" = "null" ]; then
    die_se_json "${SUBMISSION_ID}" "code missing"
fi

if [ "${LANGUAGE}" != "cpp" ] && [ "${LANGUAGE}" != "c" ]; then
    die_se_json "${SUBMISSION_ID}" "unsupported language: ${LANGUAGE}"
fi

TIME_LIMIT_MS="$(jq -r '.time_limit_ms // 1000' "${TASK_FILE}" 2>/dev/null)"
TIME_LIMIT_MS="${TIME_LIMIT_MS:-1000}"
MEMORY_LIMIT_MB="$(jq -r '.memory_limit_mb // 256' "${TASK_FILE}" 2>/dev/null)"
MEMORY_LIMIT_MB="${MEMORY_LIMIT_MB:-256}"
COMPILE_TIMEOUT_MS="$(jq -r '.compile_timeout_ms // '"${DEFAULT_COMPILE_TIMEOUT_MS}" "${TASK_FILE}" 2>/dev/null)"
COMPILE_TIMEOUT_MS="${COMPILE_TIMEOUT_MS:-${DEFAULT_COMPILE_TIMEOUT_MS}}"
RUN_HARD_TIMEOUT_MS="$(jq -r '.run_hard_timeout_ms // '"${DEFAULT_RUN_HARD_TIMEOUT_MS}" "${TASK_FILE}" 2>/dev/null)"
RUN_HARD_TIMEOUT_MS="${RUN_HARD_TIMEOUT_MS:-${DEFAULT_RUN_HARD_TIMEOUT_MS}}"
OUTPUT_LIMIT_BYTES="$(jq -r '.output_limit_bytes // '"${DEFAULT_OUTPUT_LIMIT_BYTES}" "${TASK_FILE}" 2>/dev/null)"
OUTPUT_LIMIT_BYTES="${OUTPUT_LIMIT_BYTES:-${DEFAULT_OUTPUT_LIMIT_BYTES}}"

# ───── Special Judge 字段（SPEC §11 Phase 4 ☆ / §4.3）───────────
# 这两个字段为可选：当任意 test_case.judge_type = "special" 时，C++ 侧
# （submission_routes.h / judge_scheduler.h）会从 problem_special_judges 表
# 把对应 problem 的 SPJ 源码塞进来。空串 = 该题没有挂 SPJ，judge.sh
# 退化为 "special 类型点统一判 WA"（见 compare.sh::compare_special）。
#
# 之所以用 `//` 作 sentinel：`jq -r` 对 missing key 返 "null"；我们用
# `// null` 兜底再用 shell 参数展开把 "null" / 空 都归一为空。避免
# jq 不可用（极少见）时 `//` jq 短路给到 default 的同时又把坏字符串
# 当 SPJ 源码喂给 g++。
SPECIAL_JUDGE_SOURCE_RAW="$(jq -r '.special_judge_source // ""' "${TASK_FILE}" 2>/dev/null || echo "")"
if [ "${SPECIAL_JUDGE_SOURCE_RAW}" = "null" ]; then
    SPECIAL_JUDGE_SOURCE_RAW=""
fi
SPECIAL_JUDGE_LANGUAGE_RAW="$(jq -r '.special_judge_language // ""' "${TASK_FILE}" 2>/dev/null || echo "")"
if [ "${SPECIAL_JUDGE_LANGUAGE_RAW}" = "null" ]; then
    SPECIAL_JUDGE_LANGUAGE_RAW=""
fi
SPECIAL_JUDGE_ENABLED="false"

TC_COUNT="$(jq -r '(.test_cases // []) | length' "${TASK_FILE}" 2>/dev/null)"
TC_COUNT="${TC_COUNT:-0}"

# 没有测试用例 → 直接 AC
if [ "${TC_COUNT}" = "0" ]; then
    printf '{"submission_id":%s,"status":"ac","time_used_ms":0,"memory_used_kb":0,"error_message":null,"failed_case_index":null,"case_results":[]}\n' \
        "${SUBMISSION_ID}"
    exit 0
fi

# 把每点的 input/expected 归一化后落盘
mkdir -p "${JUDGE_TMP}"
TEST_CASES_DIR="$(mktemp -d -p /tmp judge-cases-XXXXXX)"
trap 'rm -rf "${TEST_CASES_DIR:-}" "${JUDGE_TMP:?}/case_results.jsonl" "${JUDGE_BIN:-}" "${COMPILE_STDERR:-}" 2>/dev/null || true' EXIT

for ((i = 0; i < TC_COUNT; i++)); do
    cdir="${TEST_CASES_DIR}/${i}"
    mkdir -p "${cdir}"
    set +e
    # `jq -r` always appends a trailing '\n' to scalar string output.
    # Without dropping it, expected.txt / input.txt would always carry
    # one extra newline byte compared to what the user's code wrote to
    # stdout, making compare_exact return MISMATCH for every test case
    # (v1.2.50 hit this — every AC submission landed as WA). We use
    # `printf '%s'` so jq's trailing newline is preserved as part of
    # the value, then `sed '$ {/^$/d}'` trims exactly one trailing
    # newline IF (and only if) the file ends with one. Empty strings
    # (e.g. expected_output for problems with no expected) end up as
    # truly empty files, which is the desired behavior.
    jq -r ".test_cases[${i}].input // \"\""            "${TASK_FILE}" \
        | strip_bom | strip_crlf \
        | sed -e '$ {/^$/d}' > "${cdir}/input.txt"
    jq -r ".test_cases[${i}].expected_output // \"\""   "${TASK_FILE}" \
        | strip_bom | strip_crlf \
        | sed -e '$ {/^$/d}' > "${cdir}/expected.txt"
    jq -r ".test_cases[${i}].judge_type // \"exact\""  "${TASK_FILE}" > "${cdir}/judge_type"
    jq -r ".test_cases[${i}].float_epsilon // 0.000001" "${TASK_FILE}" > "${cdir}/float_epsilon"
    set -e
    # 探测任一 case 是否需特殊判（SPEC §11 Phase 4 ☆ SPJ 框架）；
    # 一旦启用则后续在编译用户代码完成后立刻编译 SPJ。
    if [ -z "${SPECIAL_JUDGE_SOURCE_RAW}" ]; then
        continue
    fi
    case "$(cat "${cdir}/judge_type" 2>/dev/null || true)" in
        special)
            SPECIAL_JUDGE_ENABLED="true"
            ;;
    esac
done

# 编译 Special Judge（一次性，编译错误 → 当前 submission 整体 SE）
# 仅当 SPECIAL_JUDGE_ENABLED=true 时执行。
SPJ_BIN_PATH=""
SPJ_COMPILE_RC=0
if [ "${SPECIAL_JUDGE_ENABLED}" = "true" ]; then
    if [ -z "${SPECIAL_JUDGE_LANGUAGE_RAW}" ] || [ "${SPECIAL_JUDGE_LANGUAGE_RAW}" != "cpp" ]; then
        # 防御：admin 没填语言或填了非 cpp（当前镜像只支持 cpp）。
        # 视作 SE，避免把垃圾塞进 g++。
        SPECIAL_JUDGE_ENABLED="false"
        SPJ_COMPILE_RC=1
    else
        SPJ_BIN_PATH="${JUDGE_TMP}/spj_bin"
        SPJ_SRC_PATH="${JUDGE_TMP}/spj_src.cpp"
        mkdir -p "${JUDGE_TMP}"
        printf '%s' "${SPECIAL_JUDGE_SOURCE_RAW}" > "${SPJ_SRC_PATH}"
        set +e
        compile_spj "${SPJ_SRC_PATH}" "${SPJ_BIN_PATH}"
        SPJ_COMPILE_RC=$?
        set -e
        if [ "${SPJ_COMPILE_RC}" -eq 0 ] && [ -x "${SPJ_BIN_PATH}" ]; then
            # 暴露给 compare.sh：所有 judge_type=special 的点都走 SPJ。
            export LITECODE_SPJ_BIN="${SPJ_BIN_PATH}"
        else
            # 编译失败：后续每个特殊点 emit SE，并把 stderr 折进 info。
            export LITECODE_SPJ_BIN=""
        fi
    fi
fi

# 探测 cgroup 基线（空 = host/无 cgroup 容器内）
CGROUP_BASE="$(cgroup_v2_base)"

# =============================================================
# Section C — 编译（独立 10s 超时，SPEC §7.1）
# =============================================================
SRC_EXT="cpp"
GCC_BIN="g++"
JUDGE_SRC="${JUDGE_SRC_BASE}.${SRC_EXT}"
if [ "${LANGUAGE}" = "c" ]; then
    SRC_EXT="c"
    GCC_BIN="gcc"
fi

printf '%s' "${CODE}" > "${JUDGE_SRC}"

COMPILE_TIMEOUT_S=$(( (COMPILE_TIMEOUT_MS + 999) / 1000 ))
COMPILE_TOTAL_TIMEOUT_S=$((COMPILE_TIMEOUT_S + 2))

COMPILE_STDERR="$(mktemp -p /tmp judge-compile-XXXXXX.err)"

set +e
timeout --foreground --kill-after=2s "${COMPILE_TOTAL_TIMEOUT_S}" \
    "${GCC_BIN}" "${COMPILE_FLAGS[@]}" -o "${JUDGE_BIN}" "${JUDGE_SRC}" \
    2> "${COMPILE_STDERR}"
COMPILE_RC=$?
set -e

case "${COMPILE_RC}" in
    0)
        if [ ! -x "${JUDGE_BIN}" ]; then
            emit_final_json "${SUBMISSION_ID}" "se" "compile returned 0 but binary missing" 0 0 ""
            rm -f "${COMPILE_STDERR}"
            exit 0
        fi
        ;;
    124|137)
        CE_MSG="$(truncate_text "Compilation timeout (limit ${COMPILE_TIMEOUT_S}s)" "${COMPILE_ERROR_TRUNCATE_BYTES}")"
        emit_final_json "${SUBMISSION_ID}" "ce" "${CE_MSG}" 0 0 ""
        rm -f "${COMPILE_STDERR}"
        exit 0
        ;;
    *)
        ERR_RAW="$(cat "${COMPILE_STDERR}" 2>/dev/null || true)"
        [ -z "${ERR_RAW}" ] && ERR_RAW="compilation failed (exit ${COMPILE_RC})"
        CE_MSG="$(truncate_text "${ERR_RAW}" "${COMPILE_ERROR_TRUNCATE_BYTES}")"
        emit_final_json "${SUBMISSION_ID}" "ce" "${CE_MSG}" 0 0 ""
        rm -f "${COMPILE_STDERR}"
        exit 0
        ;;
esac
rm -f "${COMPILE_STDERR}"

# =============================================================
# Section D — 逐点运行（SPEC §7.1.b / §7.4 / §15.4）
# =============================================================
TIME_LIMIT_S=$(( (TIME_LIMIT_MS + 999) / 1000 ))

TOTAL_TIME_MS=0
TOTAL_MEM_KB=0
FINAL_STATUS="ac"
FAILED_CASE_INDEX=""
ERROR_MESSAGE=""
JUDGE_START_MS=$(now_ms)

for ((i = 0; i < TC_COUNT; i++)); do
    # 全局硬超时（防 judge.sh 自身卡死）
    NOW_MS_VAL=$(now_ms)
    ELAPSED=$(( NOW_MS_VAL - JUDGE_START_MS ))
    if [ "${ELAPSED}" -gt "${RUN_HARD_TIMEOUT_MS}" ]; then
        FINAL_STATUS="se"
        FAILED_CASE_INDEX="${i}"
        ERROR_MESSAGE="judge script exceeded hard timeout (${RUN_HARD_TIMEOUT_MS} ms)"
        printf '{"index":%s,"status":"se","time_ms":0,"mem_kb":0,"info":null}\n' "${i}" \
            >> "${JUDGE_TMP}/case_results.jsonl"
        break
    fi

    cdir="${TEST_CASES_DIR}/${i}"
    JT="$(cat "${cdir}/judge_type")"

    if [ "${FINAL_STATUS}" != "ac" ]; then
        # 已失败：后续点记 skipped（保留顺序，便于前端按点展示）
        printf '{"index":%s,"status":"skipped","time_ms":0,"mem_kb":0,"info":null}\n' "${i}" \
            >> "${JUDGE_TMP}/case_results.jsonl"
        continue
    fi

    RAW_OUT="${cdir}/raw_output.txt"
    OUT_FILE="${cdir}/output.txt"
    STDERR_FILE="${cdir}/stderr.txt"

    CPU_BEFORE_USEC="$(read_cgroup_cpu_usec "${CGROUP_BASE}")"
    CPU_BEFORE_USEC="${CPU_BEFORE_USEC:-0}"

    set +e
    timeout --kill-after=2s $((TIME_LIMIT_S + 1)) \
        "${JUDGE_BIN}" \
            < "${cdir}/input.txt" \
            2> "${STDERR_FILE}" \
            > "${RAW_OUT}"
    RUN_RC=$?
    set -e

    # OLE 立即判定（SPEC §7.4：> 16MB → OLE，不再比对）
    RAW_BYTES=0
    if [ -s "${RAW_OUT}" ]; then
        RAW_BYTES=$(wc -c < "${RAW_OUT}" | tr -d ' ')
    fi
    if [ "${RAW_BYTES:-0}" -gt "${OUTPUT_LIMIT_BYTES}" ]; then
        TIME_MS=$(elapsed_since_ms "${CPU_BEFORE_USEC}" "${CGROUP_BASE}")
        MEM_KB=$(mem_kb_peak "${CGROUP_BASE}")
        update_max TOTAL_TIME_MS "${TIME_MS}"
        update_max TOTAL_MEM_KB "${MEM_KB}"
        FINAL_STATUS="ole"
        FAILED_CASE_INDEX="${i}"
        ERROR_MESSAGE="output exceeded ${OUTPUT_LIMIT_BYTES} bytes (got ${RAW_BYTES})"
        printf '{"index":%s,"status":"ole","time_ms":%s,"mem_kb":%s,"info":"output %s > %s bytes"}\n' \
            "${i}" "${TIME_MS}" "${MEM_KB}" "${RAW_BYTES}" "${OUTPUT_LIMIT_BYTES}" \
            >> "${JUDGE_TMP}/case_results.jsonl"
        continue
    fi

    head -c "${OUTPUT_LIMIT_BYTES}" "${RAW_OUT}" > "${OUT_FILE}" 2>/dev/null || true

    case "${RUN_RC}" in
        0)
            # 正常退出 → 比对
            ;;
        124)
            TIME_MS=$(elapsed_since_ms "${CPU_BEFORE_USEC}" "${CGROUP_BASE}")
            MEM_KB=$(mem_kb_peak "${CGROUP_BASE}")
            update_max TOTAL_TIME_MS "${TIME_MS}"
            update_max TOTAL_MEM_KB "${MEM_KB}"
            FINAL_STATUS="tle"
            FAILED_CASE_INDEX="${i}"
            ERROR_MESSAGE="time limit exceeded (${TIME_MS} ms > ${TIME_LIMIT_MS} ms)"
            printf '{"index":%s,"status":"tle","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                "${i}" "${TIME_MS}" "${MEM_KB}" >> "${JUDGE_TMP}/case_results.jsonl"
            continue
            ;;
        137)
            TIME_MS=$(elapsed_since_ms "${CPU_BEFORE_USEC}" "${CGROUP_BASE}")
            MEM_KB=$(mem_kb_peak "${CGROUP_BASE}")
            update_max TOTAL_TIME_MS "${TIME_MS}"
            update_max TOTAL_MEM_KB "${MEM_KB}"
            FINAL_STATUS="mle"
            FAILED_CASE_INDEX="${i}"
            ERROR_MESSAGE="memory limit exceeded"
            printf '{"index":%s,"status":"mle","time_ms":%s,"mem_kb":%s,"info":"OOM killed"}\n' \
                "${i}" "${TIME_MS}" "${MEM_KB}" >> "${JUDGE_TMP}/case_results.jsonl"
            continue
            ;;
        *)
            TIME_MS=$(elapsed_since_ms "${CPU_BEFORE_USEC}" "${CGROUP_BASE}")
            MEM_KB=$(mem_kb_peak "${CGROUP_BASE}")
            update_max TOTAL_TIME_MS "${TIME_MS}"
            update_max TOTAL_MEM_KB "${MEM_KB}"
            STDERR_RAW="$(cat "${STDERR_FILE}" 2>/dev/null || true)"
            RE_ERR="$(truncate_text "${STDERR_RAW:-runtime error: exit code ${RUN_RC}}" "${RUNTIME_ERROR_TRUNCATE_BYTES}")"
            FINAL_STATUS="re"
            FAILED_CASE_INDEX="${i}"
            ERROR_MESSAGE="${RE_ERR}"
            printf '{"index":%s,"status":"re","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                "${i}" "${TIME_MS}" "${MEM_KB}" >> "${JUDGE_TMP}/case_results.jsonl"
            continue
            ;;
    esac

    TIME_MS=$(elapsed_since_ms "${CPU_BEFORE_USEC}" "${CGROUP_BASE}")
    MEM_KB=$(mem_kb_peak "${CGROUP_BASE}")
    update_max TOTAL_TIME_MS "${TIME_MS}"
    update_max TOTAL_MEM_KB "${MEM_KB}"

    EXPECTED_FILE="${cdir}/expected.txt"
    case "${JT}" in
        exact)
            if compare_exact "${OUT_FILE}" "${EXPECTED_FILE}"; then
                CASE_STATUS="ac"
            else
                FINAL_STATUS="wa"
                FAILED_CASE_INDEX="${i}"
                CASE_STATUS="wa"
            fi
            printf '{"index":%s,"status":"%s","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                "${i}" "${CASE_STATUS}" "${TIME_MS}" "${MEM_KB}" \
                >> "${JUDGE_TMP}/case_results.jsonl"
            ;;
        ignore_trailing)
            if compare_ignore_trailing "${OUT_FILE}" "${EXPECTED_FILE}"; then
                CASE_STATUS="ac"
            else
                FINAL_STATUS="pe"
                FAILED_CASE_INDEX="${i}"
                CASE_STATUS="pe"
            fi
            printf '{"index":%s,"status":"%s","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                "${i}" "${CASE_STATUS}" "${TIME_MS}" "${MEM_KB}" \
                >> "${JUDGE_TMP}/case_results.jsonl"
            ;;
        float_eps)
            EPS="$(cat "${cdir}/float_epsilon")"
            if compare_float_eps "${OUT_FILE}" "${EXPECTED_FILE}" "${EPS}"; then
                CASE_STATUS="ac"
            else
                FINAL_STATUS="wa"
                FAILED_CASE_INDEX="${i}"
                CASE_STATUS="wa"
            fi
            printf '{"index":%s,"status":"%s","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                "${i}" "${CASE_STATUS}" "${TIME_MS}" "${MEM_KB}" \
                >> "${JUDGE_TMP}/case_results.jsonl"
            ;;
        special)
            # ─── Special Judge 调度（SPEC §11 Phase 4 ☆ / §4.3）───
            # 三个分支：
            #   1) SPJ 编译失败 → 该 case 折 SE，整 submission 折 SE
            #   2) SPJ 未配置且 test case 是 special → 折 WA（operator 意图：
            #      题还没挂 SPJ；admin 上传后再跑就有结果了）
            #   3) SPJ 已编译 + 调用 → 按 SPJ exit code 判定 AC/WA，SPJ 自己的
            #      崩溃（非法退出码）按 SE 抓
            if [ "${SPECIAL_JUDGE_ENABLED}" != "true" ] || [ -z "${LITECODE_SPJ_BIN:-}" ] || [ ! -x "${LITECODE_SPJ_BIN:-}" ]; then
                if [ "${SPJ_COMPILE_RC:-0}" -ne 0 ]; then
                    # 1) 编译失败 — 把 stderr 摘到 info + 整 submission SE
                    SPJ_ERR="$(spj_err_for_info "${SPJ_BIN_PATH:-}" 2048)"
                    SPJ_ERR="${SPJ_ERR:-special judge source failed to compile}"
                    FINAL_STATUS="se"
                    FAILED_CASE_INDEX="${i}"
                    ERROR_MESSAGE="${SPJ_ERR}"
                    printf '{"index":%s,"status":"se","time_ms":%s,"mem_kb":%s,"info":"spj compile failed: %s"}\n' \
                        "${i}" "${TIME_MS}" "${MEM_KB}" "${SPJ_ERR}" \
                        >> "${JUDGE_TMP}/case_results.jsonl"
                else
                    # 2) 题未挂 SPJ — admin 还没上传；判 WA 让 operator 看见
                    FINAL_STATUS="wa"
                    FAILED_CASE_INDEX="${i}"
                    CASE_STATUS="wa"
                    printf '{"index":%s,"status":"wa","time_ms":%s,"mem_kb":%s,"info":"no special judge configured"}\n' \
                        "${i}" "${TIME_MS}" "${MEM_KB}" \
                        >> "${JUDGE_TMP}/case_results.jsonl"
                fi
            else
                # 3) 调 SPJ。 rc=0 AC, rc=1 WA, 其它 SE。
                set +e
                compare_special_with "${LITECODE_SPJ_BIN}" "${OUT_FILE}" "${EXPECTED_FILE}" "${cdir}/input.txt"
                SPJ_RC=$?
                set -e
                case "${SPJ_RC}" in
                    0)
                        CASE_STATUS="ac"
                        printf '{"index":%s,"status":"ac","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                            "${i}" "${TIME_MS}" "${MEM_KB}" \
                            >> "${JUDGE_TMP}/case_results.jsonl"
                        ;;
                    1)
                        FINAL_STATUS="wa"
                        FAILED_CASE_INDEX="${i}"
                        CASE_STATUS="wa"
                        SPJ_INFO="$(spj_stdout_for_info 256)"
                        if [ -z "${SPJ_INFO}" ]; then
                            printf '{"index":%s,"status":"wa","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                                "${i}" "${TIME_MS}" "${MEM_KB}" \
                                >> "${JUDGE_TMP}/case_results.jsonl"
                        else
                            printf '{"index":%s,"status":"wa","time_ms":%s,"mem_kb":%s,"info":"%s"}\n' \
                                "${i}" "${TIME_MS}" "${MEM_KB}" "${SPJ_INFO}" \
                                >> "${JUDGE_TMP}/case_results.jsonl"
                        fi
                        ;;
                    *)
                        # SPJ 自身崩了（非法退出码）。把 case 判 SE 让 operator 看得见，
                        # 但 FINAL_STATUS 留给后续 case 覆盖（不污染整 submission 的根
                        # 因 — SPJ 崩在中间意味着前面 AC 的点真的对了）。注意：当前实
                        # 现里 FINAL_STATUS 一旦被改成非 ac 就固定（continue 的护栏），
                        # 所以为了一致性这里也把 FINAL_STATUS='se'，FAILED_CASE_INDEX
                        # 设为本 case；下一个 case 因为 FINAL_STATUS!='ac' 直接 skipped。
                        FINAL_STATUS="se"
                        FAILED_CASE_INDEX="${i}"
                        ERROR_MESSAGE="special judge crashed (exit=${SPJ_RC})"
                        SPJ_INFO="$(spj_stdout_for_info 256)"
                        if [ -z "${SPJ_INFO}" ]; then
                            printf '{"index":%s,"status":"se","time_ms":%s,"mem_kb":%s,"info":"spj exit=%s"}\n' \
                                "${i}" "${TIME_MS}" "${MEM_KB}" "${SPJ_RC}" \
                                >> "${JUDGE_TMP}/case_results.jsonl"
                        else
                            printf '{"index":%s,"status":"se","time_ms":%s,"mem_kb":%s,"info":"spj exit=%s: %s"}\n' \
                                "${i}" "${TIME_MS}" "${MEM_KB}" "${SPJ_RC}" "${SPJ_INFO}" \
                                >> "${JUDGE_TMP}/case_results.jsonl"
                        fi
                        ;;
                esac
            fi
            ;;
        *)
            FINAL_STATUS="se"
            FAILED_CASE_INDEX="${i}"
            ERROR_MESSAGE="unknown judge_type: ${JT}"
            printf '{"index":%s,"status":"se","time_ms":%s,"mem_kb":%s,"info":null}\n' \
                "${i}" "${TIME_MS}" "${MEM_KB}" >> "${JUDGE_TMP}/case_results.jsonl"
            ;;
    esac
done

# =============================================================
# Section E — 输出最终 JSON
# =============================================================
emit_final_json "${SUBMISSION_ID}" "${FINAL_STATUS}" "${ERROR_MESSAGE}" \
    "${TOTAL_TIME_MS}" "${TOTAL_MEM_KB}" "${FAILED_CASE_INDEX}"
exit 0
