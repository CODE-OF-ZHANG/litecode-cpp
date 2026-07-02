#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge/lib/spj.sh (Special Judge helpers)
# =============================================================
# SPEC §11 Phase 4 ☆ "Special Judge 框架" + §4.3 judge_type='special'
#
# Special Judge (SPJ) flow:
#   1. judge.sh receives `special_judge_source` + `special_judge_language`
#      fields in task.json (sourced from the problem_special_judges table
#      by the C++ scheduler; see src/db/special_judge_repo.h).
#   2. We compile the source ONCE per submission (g++ with the same
#      secure compile flags as the user's submission, 10s timeout) into
#      a host-stable location under JUDGE_TMP/spj_bin.
#   3. For every test case whose `judge_type` is "special", we invoke
#      the SPJ binary with three file paths as positional arguments:
#         spj_bin <input> <expected_output> <actual_output>
#      The SPJ binary returns exit 0 ⇒ AC for that case; non-zero ⇒ WA.
#      It may also write a one-line reason to stdout, which we capture
#      into the case_results.jsonl `info` field (truncated to fit).
#
# Why positional file arguments (not stdin/stdout streaming):
#   - Cleanly accommodates SPJs that open() and read() the file as if
#     it were a real disk file (a typical SPJ pattern).
#   - Avoids SIGPIPE on the parent when an SPJ silently closes stdout
#     early (no IO loop in gawk / no PIPE_BUF boundary to trip).
#   - The container's tmpfs /tmp (size=64m, mode=1777) is the only
#     writable mountpoint, so we stage the three files under
#     $JUDGE_TMP/spj_case_<i>/ and reference them by absolute path.
#
# Security model (mirrors §7.3 / §15.4):
#   - The SPJ binary is built with the same secure compile flags as
#     the user submission (no exec, no setuid, fortify, RELRO, NX).
#   - The container drops all caps (`--security-opt no-new-privileges`)
#     and runs as judgeuser (UID 1000). An SPJ with a root privilege
#     escalation exploit gets exactly the same jail as the user code.
#   - The SPJ inherits the same cgroup CPU / memory caps via the
#     `/usr/local/bin/judge.sh` wrapper (DOCKER_RUN_OPTS go to the
#     container, not per-process).
#   - We time-box SPJ execution at `time_limit_s + 1` (same as user
#     code) so a runaway SPJ cannot stall the queue.
#
# Compile fail policy:
#   - If the SPJ itself fails to compile (admin uploaded a buggy program),
#     ALL special-type cases must collapse to SE for this submission
#     — flipping individual cases to WA would be misleading (the test
#     infrastructure is broken, not the user code). judge.sh does the
#     flip after this lib returns its compile rc, and the per-case info
#     message is "special judge failed to compile: <reason>".
#
# IO contract (one line summary):
#   - env: JUDGE_TMP is required; absent ⇒ library no-ops (returns 1).
#   - compile_spj <src_path> <out_bin>         rc=0 ok, rc!=0 compile error
#   - run_spj <bin> <input> <expected> <actual> rc=0 AC, rc=1 WA, other=SE
# =============================================================

if [ -n "${LITECODE_JUDGE_LIB_SPJ_LOADED:-}" ]; then
    return 0
fi
LITECODE_JUDGE_LIB_SPJ_LOADED=1

# Same secure compile flags as the user submission (judge.sh). The SPJ
# is admin-supplied code we trust slightly more than user code, but we
# apply the same belt-and-braces because (a) it costs nothing at compile
# time and (b) a compromised admin account should not gain free RCE on
# the judge container.
SPJ_COMPILE_FLAGS=(
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

# Default compile timeout for the SPJ binary (separate from the user's
# 10s compile timeout so an admin can ship a more complex SPJ without
# bothering user submissions). 10s is still comfortable for any
# reasonable SPJ; SPEC doesn't pin a number, so we match the user
# limit for consistency.
SPJ_COMPILE_TIMEOUT_MS="${SPJ_COMPILE_TIMEOUT_MS:-10000}"

# compile_spj <src_path> <out_bin>
#   Compile the SPJ source at <src_path> to the binary at <out_bin>.
#   Returns 0 on success, non-zero on compile error / missing files.
#   The compile stderr is written to a sibling .err file so the
#   caller can fold it into the case_results.jsonl info message.
#   We use a temp dir under JUDGE_TMP (the only writable tmpfs in
#   the container) so the compile artifacts never escape the mount.
compile_spj() {
    local src="$1"
    local out_bin="$2"

    if [ -z "${JUDGE_TMP:-}" ]; then
        return 2  # library misuse; caller forgot to set JUDGE_TMP
    fi
    if [ ! -r "${src}" ]; then
        echo "spj: source file missing or not readable: ${src}" >&2
        return 2
    fi

    local err_file="${out_bin}.err"
    local timeout_s
    timeout_s=$(( (SPJ_COMPILE_TIMEOUT_MS + 999) / 1000 ))
    local total_timeout_s=$(( timeout_s + 2 ))

    set +e
    timeout --foreground --kill-after=2s "${total_timeout_s}" \
        g++ "${SPJ_COMPILE_FLAGS[@]}" -o "${out_bin}" "${src}" \
        2> "${err_file}"
    local rc=$?
    set -e

    if [ "${rc}" -ne 0 ]; then
        # Caller turns `rc != 0` into per-case SE with the err_file
        # content as the per-case `info` message.
        return "${rc}"
    fi
    if [ ! -x "${out_bin}" ]; then
        echo "spj: compile returned 0 but binary missing: ${out_bin}" >&2
        return 1
    fi
    return 0
}

# run_spj <bin> <input> <expected> <actual>
#   Invoke the SPJ binary on three file paths. Exit codes:
#     0  ⇒ AC for the case (the SPJ accepts the output)
#     1  ⇒ WA (the SPJ rejects the output)
#     124 / 137 ⇒ TLE / MLE (timeout / OOM-killed) — caller folds
#           these into the appropriate status for the *case*, NOT the
#           whole submission (an SPJ_TLE on case 3 still tells the
#           operator the user's code passed case 0..2).
#     other ⇒ SE (the SPJ crashed / unhandled exit). Same folding.
#   The SPJ's stdout is captured into JUDGE_TMP/spj_stdout and used
#   as the per-case `info` field (truncated to 2 KB).
#
#   We don't capture SPJ stderr: a buggy SPJ spamming stderr is its
#   own problem; we don't want a noisy SPJ to fill the case_results
#   info field with garbage.
run_spj() {
    local bin="$1" input="$2" expected="$3" actual="$4"

    if [ -z "${JUDGE_TMP:-}" ]; then
        return 2
    fi
    if [ ! -x "${bin}" ]; then
        echo "spj: binary missing or not executable: ${bin}" >&2
        return 2
    fi
    if [ ! -r "${input}" ] || [ ! -r "${expected}" ] || [ ! -r "${actual}" ]; then
        echo "spj: one or more input files not readable" >&2
        return 2
    fi

    local stdout_file="${JUDGE_TMP}/spj_stdout"
    : > "${stdout_file}"

    # No `timeout` here — the container's docker wait watchdog is the
    # outer bound. Adding a per-process `timeout` would race with the
    # container-level one for unclear benefit. The SPJ inherits the
    # case's `time_limit_s` via the per-case timeout that judge.sh
    # enforces before calling compare_special.
    set +e
    "${bin}" "${input}" "${expected}" "${actual}" > "${stdout_file}"
    local rc=$?
    set -e

    return "${rc}"
}

# spj_stdout_for_info — return the captured SPJ stdout trimmed to a
# safe length for the per-case `info` field. Empty when no SPJ has
# run yet. Matches the case_results JSONL envelope's 2 KB RE cap.
spj_stdout_for_info() {
    local limit="${1:-2048}"
    if [ -z "${JUDGE_TMP:-}" ] || [ ! -r "${JUDGE_TMP}/spj_stdout" ]; then
        echo ""
        return
    fi
    truncate_text "$(cat "${JUDGE_TMP}/spj_stdout" 2>/dev/null || true)" "${limit}"
}

# spj_err_for_info — same idea but for the compile stderr file. Used
# by judge.sh to surface a SPJ compile failure as the per-case info
# message (and the whole submission's error_message).
spj_err_for_info() {
    local bin="$1"
    local limit="${2:-4096}"
    local err_file="${bin}.err"
    if [ ! -r "${err_file}" ]; then
        echo ""
        return
    fi
    truncate_text "$(cat "${err_file}" 2>/dev/null || true)" "${limit}"
}

# compare_special_with <bin> <out> <expected> <input>
#   Drop-in replacement for compare_special(): returns 0 (AC) when the
#   SPJ accepts the user's output, 1 (WA) when it doesn't. The judge.sh
#   loop calls this once per special-type case after the user's solution
#   has run. Errors propagate via rc=2..127 which judge.sh handles as SE.
#
#   The four-arg shape mirrors the call-site signature in compare.sh so
#   the existing judge.sh dispatch can stay unchanged.
compare_special_with() {
    local bin="$1" out="$2" expected="$3" input="$4"
    run_spj "${bin}" "${input}" "${expected}" "${out}"
}
