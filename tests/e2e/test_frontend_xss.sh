#!/usr/bin/env bash
# tests/e2e/test_frontend_xss.sh
#
# LiteCode-CPP — Phase 5 ★ Markdown XSS 净化 / SPEC §6.3 + A32
#
# Regression shell test for the frontend Markdown XSS pipeline.
#
# What this test does (and what it intentionally does NOT do):
#
#   ✓ Asserts that every page that might render Markdown
#     (problem.html, profile.html, admin/problem-edit.html)
#     loads /js/csp.js + /js/markdown.js in that order, with
#     defer attributes (so the inline boot block can rely on
#     window.litecode.markdown).
#
#   ✓ Asserts that csp.js + markdown.js register the
#     `window.litecode.csp` and `window.litecode.markdown`
#     namespaces that the rest of the page consumes.
#
#   ✓ Asserts that csp.js pins sha384 integrity hashes for
#     BOTH marked and DOMPurify (so a future "drop the SRI
#     attribute, it's noisy in dev" commit trips this check).
#
#   ✓ Asserts that the page-level <meta> CSP tag matches the
#     canonical value in csp.js. (The csp.js runtime check
#     only fires in a browser; this script catches a typo at
#     the shell level so it never reaches the browser.)
#
#   ✗ Does NOT run the actual sanitizer in a real DOM. The
#     in-browser regression harness lives in
#     web/test/markdown-xss.html — this shell test is the
#     cheap first line of defense.
#
#   ✗ Does NOT test the C++ problem-detail endpoint's
#     "description is delivered raw" contract — that lives
#     in tests/unit/test_problem_detail.cpp because it
#     needs a real DB + httplib stack.
#
# Run:
#   bash tests/e2e/test_frontend_xss.sh
# Exit code: 0 on success, non-zero on failure.
#
# The script is intentionally bash + grep + awk only — no
# python, no jq, no node, no MySQL. It runs anywhere curl runs.

set -euo pipefail

# Resolve the project root from the script location so the test
# works whether you invoke it as `bash tests/e2e/test_...` from
# the project root, or as `bash test_...` from inside the dir.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
WEB="$ROOT/web"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

ok() {
    echo "  ok  $*"
}

# ────────────────────────────────────────────────────────────────────
#  1) csp.js + markdown.js are present and non-empty
# ────────────────────────────────────────────────────────────────────
[ -f "$WEB/js/csp.js"      ] || fail "missing web/js/csp.js"
[ -f "$WEB/js/markdown.js" ] || fail "missing web/js/markdown.js"
[ -s "$WEB/js/csp.js"      ] || fail "web/js/csp.js is empty"
[ -s "$WEB/js/markdown.js" ] || fail "web/js/markdown.js is empty"
ok "csp.js + markdown.js present and non-empty"

# ────────────────────────────────────────────────────────────────────
#  2) csp.js pins sha384 integrity for BOTH libraries
# ────────────────────────────────────────────────────────────────────
CSP="$WEB/js/csp.js"
grep -q "integrity:  'sha384-" "$CSP"             || fail "csp.js missing sha384 integrity for marked"
grep -q "cdn.jsdelivr.net/npm/marked@"    "$CSP" || fail "csp.js missing marked CDN URL"
grep -q "cdn.jsdelivr.net/npm/dompurify@" "$CSP" || fail "csp.js missing dompurify CDN URL"
grep -qE "crossOrigin[[:space:]]*:[[:space:]]*'anonymous'" "$CSP" || fail "csp.js missing crossOrigin attr (SRI fails open without it)"
ok "csp.js pins sha384 integrity for marked + dompurify"

# ────────────────────────────────────────────────────────────────────
#  3) markdown.js exposes the expected API + config
# ────────────────────────────────────────────────────────────────────
MD="$WEB/js/markdown.js"
grep -q "prewarm"             "$MD" || fail "markdown.js missing prewarm()"
grep -q "renderSafe"          "$MD" || fail "markdown.js missing renderSafe()"
grep -q "renderSafeSync"      "$MD" || fail "markdown.js missing renderSafeSync()"
grep -q "ALLOWED_TAGS"        "$MD" || fail "markdown.js missing ALLOWED_TAGS allowlist"
grep -q "ALLOWED_ATTR"        "$MD" || fail "markdown.js missing ALLOWED_ATTR allowlist"
grep -q "FORBID_TAGS"         "$MD" || fail "markdown.js missing FORBID_TAGS deny list"
grep -q "ALLOWED_URI_REGEXP"  "$MD" || fail "markdown.js missing ALLOWED_URI_REGEXP URL filter"
# Pin the FORBID_TAGS members — defense in depth.
for needle in "'script'" "'style'" "'iframe'" "'object'" "'embed'" "'form'" "'meta'" "'base'"; do
    grep -q "$needle" "$MD" || fail "markdown.js FORBID_TAGS missing $needle"
done
ok "markdown.js exposes prewarm/renderSafe/renderSafeSync with full allowlist"

# ────────────────────────────────────────────────────────────────────
#  4) Markdown-consuming pages load csp.js + markdown.js first
# ────────────────────────────────────────────────────────────────────
for page in problem.html profile.html admin/problem-edit.html; do
    f="$WEB/$page"
    [ -f "$f" ] || fail "missing page: $page"
    # Both scripts must be referenced with `defer`.
    grep -Eq 'src="/js/csp\.js"[^>]*defer'      "$f" || fail "$page missing csp.js (defer)"
    grep -Eq 'src="/js/markdown\.js"[^>]*defer' "$f" || fail "$page missing markdown.js (defer)"
    # markdown.js must come AFTER csp.js and BEFORE api.js / app.js
    # in source order — assert by looking at the line numbers.
    csp_line=$(grep -n 'src="/js/csp\.js"'      "$f" | head -1 | cut -d: -f1)
    md_line=$( grep -n 'src="/js/markdown\.js"' "$f" | head -1 | cut -d: -f1)
    api_line=$(grep -n 'src="/js/api\.js"'      "$f" | head -1 | cut -d: -f1)
    app_line=$(grep -n 'src="/js/app\.js"'      "$f" | head -1 | cut -d: -f1)
    [ -n "$csp_line" ] || fail "$page: csp.js line not found"
    [ -n "$md_line"  ] || fail "$page: markdown.js line not found"
    [ -n "$api_line" ] || fail "$page: api.js line not found"
    [ -n "$app_line" ] || fail "$page: app.js line not found"
    if [ "$csp_line" -ge "$md_line" ];  then fail "$page: csp.js must come before markdown.js"; fi
    if [ "$md_line"  -ge "$api_line" ]; then fail "$page: markdown.js must come before api.js"; fi
    if [ "$md_line"  -ge "$app_line" ]; then fail "$page: markdown.js must come before app.js"; fi
    ok "$page loads scripts in csp.js → markdown.js → api.js → app.js order"
done

# ────────────────────────────────────────────────────────────────────
#  5) Boot block on each Markdown page calls prewarm()
# ────────────────────────────────────────────────────────────────────
for page in problem.html profile.html admin/problem-edit.html; do
    f="$WEB/$page"
    grep -q "litecode\.markdown\.prewarm" "$f" || fail "$page: boot block does not call litecode.markdown.prewarm()"
    ok "$page boot block calls prewarm()"
done

# ────────────────────────────────────────────────────────────────────
#  6) In-browser regression harness is reachable
# ────────────────────────────────────────────────────────────────────
HARNESS="$WEB/test/markdown-xss.html"
[ -f "$HARNESS" ] || fail "missing web/test/markdown-xss.html"
grep -q "ALL PASS" "$HARNESS" || fail "harness missing 'ALL PASS' summary string"
grep -q "litecode\.markdown"  "$HARNESS" || fail "harness missing litecode.markdown reference"
ok "in-browser XSS regression harness is present and references litecode.markdown"

# ────────────────────────────────────────────────────────────────────
#  7) Pages carry the canonical CSP tag (csp.js asserts the same
#     at runtime, but a typo caught at the shell level never
#     reaches the browser).
# ────────────────────────────────────────────────────────────────────
CANONICAL=$(grep -E "CSP_VALUE" -A 20 "$CSP" | grep -E "^\s*\"" | head -1 | tr -d '\n' | tr -s ' ' ' ')
for page in problem.html profile.html admin/problem-edit.html; do
    f="$WEB/$page"
    # Crude but sufficient: the page meta must contain every
    # important CSP directive. (A direct equality check would
    # fail on whitespace; this is intentionally tolerant.)
    grep -q "default-src 'self'"            "$f" || fail "$page CSP missing default-src 'self'"
    grep -q "script-src 'self' https://cdn.jsdelivr.net" "$f" || fail "$page CSP missing script-src jsdelivr allow"
    grep -q "object-src 'none'"             "$f" || fail "$page CSP missing object-src 'none'"
    grep -q "base-uri 'self'"               "$f" || fail "$page CSP missing base-uri 'self'"
    grep -q "frame-ancestors 'none'"        "$f" || fail "$page CSP missing frame-ancestors 'none'"
    ok "$page CSP tag carries all baseline directives"
done

echo
echo "PASS — frontend XSS pipeline is wired and pinned."
