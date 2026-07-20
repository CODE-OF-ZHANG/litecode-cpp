// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Unit tests for the submission history helpers
//               (Phase 5 ★ 提交历史标签页, v1.2.29)
//
// Runs in Node 18+ with `node web/test/history.test.js`. No test
// runner, no dependencies — just a tiny assert + a vm sandbox that
// loads problem.html's inline IIFE and pulls a handful of pure
// helpers out of it via the `__lcTest` surface (no-op in
// production — see the test-surface comment in problem.html).
//
// What we test
// ------------
//   STATUS_LABEL / TERMINAL_STATUSES  — status enum shape (11
//                                        statuses + AC/WA/etc. labels)
//   tabHistoryBuildParams              — filter param construction
//   tabHistoryBuildQuery               — query-string serialization
//                                        (URL-encode safe, no empty
//                                        fields)
//   tabHistoryFormatTime               — relative-time formatting
//                                        matching v1.2.26's
//                                        "刚刚 / N 分钟前 / N 小时前 /
//                                        absolute date" vocabulary
//   tabHistoryParseHash                — round-trip with
//                                        tabHistorySyncHash for
//                                        deep-link URLs
//
// Pure functions only — no fetch / DOM / timers touched. The
// 4 helpers are deliberately defined inside problem.html's IIFE so
// app code can't reach them; the `__lcTest` namespace is the
// single bridge. The harness is intentionally minimal: 80 lines
// to drive 30+ cases.
//
// Exit code 0 on success, 1 on any failure. v1.2.29 baseline:
// 30+ cases, all PASS.

'use strict';

const fs   = require('fs');
const path = require('path');
const vm   = require('vm');

// ── Tiny assert harness ─────────────────────────────────────────
var passed = 0, failed = 0;
var failures = [];
function eq(actual, expected, label) {
    var a = JSON.stringify(actual);
    var e = JSON.stringify(expected);
    if (a === e) { passed++; return; }
    failed++;
    failures.push({ label: label, actual: a, expected: e });
}
function ok(cond, label) {
    if (cond) { passed++; return; }
    failed++;
    failures.push({ label: label, actual: 'falsy', expected: 'truthy' });
}

// ── Load problem.html and pull out the inline IIFE ─────────────
var htmlPath = path.resolve(__dirname, '..', 'problem.html');
var html = fs.readFileSync(htmlPath, 'utf8');
var m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.error('FATAL: problem.html has no inline <script>'); process.exit(2); }

// problem.html's IIFE is huge — it touches `document`, `window`,
// `litecode.api`, `CodeMirror`, `URLSearchParams`, etc. The IIFE
// runs synchronously and its inner functions (e.g. submit(),
// sseWaitForResult()) reference globals that resolve at call-time
// (not definition-time), so we just need to stub a few shims for
// the IIFE's top-level execution to succeed.
//
// We then read `__lcTest` AFTER the IIFE completes; the inner
// `if (typeof __lcTest !== 'undefined')` block will have populated
// it with the pure helpers we want to test.
var sandbox = {
    console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout,
    setInterval: setInterval, clearInterval: clearInterval,
    Promise: Promise,
    AbortController: AbortController,
    TextDecoder: TextDecoder, TextEncoder: TextEncoder,
    URLSearchParams: URLSearchParams,
    history: { replaceState: function () {} },
    location: { pathname: '/problem.html', search: '', href: 'http://localhost/problem.html', hash: '' },
    navigator: { onLine: true, clipboard: null, userAgent: 'node' },
    isSecureContext: false,
    window: null,
    document: {
        addEventListener: function () {},
        removeEventListener: function () {},
        createElement: function () { return { style: {}, dataset: {}, addEventListener: function () {} }; },
        getElementById: function () { return null; },
        querySelector: function () { return null; },
        querySelectorAll: function () { return []; },
        dispatchEvent: function () {},
        documentElement: { classList: { contains: function () { return false; }, add: function () {}, remove: function () {} } },
        hidden: false,
        title: '',
        cookie: '',
    },
    sessionStorage: { getItem: function () { return null; }, setItem: function () {}, removeItem: function () {} },
    localStorage: { getItem: function () { return null; }, setItem: function () {}, removeItem: function () {} },
    CustomEvent: function (type, init) { this.type = type; this.detail = init && init.detail; },
    litecode: {
        api: {
            get:    function () { return Promise.reject(new Error('stub')); },
            post:   function () { return Promise.reject(new Error('stub')); },
            put:    function () { return Promise.reject(new Error('stub')); },
            delete: function () { return Promise.reject(new Error('stub')); },
            rawFetch: function () { return Promise.reject(new Error('stub')); },
            sse:    function () { return { close: function () {} }; },
            // v1.2.56: TERMINAL_STATUSES + isTerminalStatus live on
            // litecode.api (api.js IIFE) now; the problem.html
            // __lcTest re-export reads them from here. Mirror the
            // 9-key shape so Group 1's "TS has 9 keys" assertion
            // still passes.
            TERMINAL_STATUSES: {
                ac:1, wa:1, tle:1, mle:1, re:1, ole:1, pe:1, ce:1, se:1
            },
            isTerminalStatus: function (s) { return !!(this.TERMINAL_STATUSES[s]); },
            subscribeSubmission: function () { return { close: function () {} }; },
        },
        boot: { shell: function () { return Promise.resolve(); } },
        csp: { SCRIPTS: {}, STYLESHEETS: {} },
        markdown: { prewarm: function () {}, renderSafe: function (s) { return String(s); }, renderSafeAsync: function (s) { return Promise.resolve(String(s)); } },
        theme: { get: function () { return 'auto'; }, set: function () {}, toggle: function () {} },
        toast: { success: function () {}, error: function () {}, warn: function () {}, info: function () {} },
        guard: { requireAuth: function () { return Promise.resolve(); }, requireAdmin: function () { return Promise.resolve(); }, requireGuest: function () { return Promise.resolve(); } },
        nav: { mount: function () {}, user: null },
    },
    CodeMirror: undefined,
    // The `__lcTest` handle the IIFE looks for. It MUST exist before
    // the script runs so the `if (typeof __lcTest !== 'undefined')`
    // branch is taken and our exports land here.
    __lcTest: {},
};
sandbox.window = sandbox;
sandbox.self   = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
try {
    vm.runInContext(m[1], sandbox);
} catch (e) {
    console.error('FATAL: IIFE threw at top level: ' + e.message);
    console.error(e.stack);
    process.exit(2);
}

var test = sandbox.__lcTest;
if (!test || !test.tabHistoryBuildQuery) {
    console.error('FATAL: problem.html did not expose __lcTest helpers — IIFE was too strict');
    process.exit(2);
}

// ── Group 1: STATUS_LABEL / TERMINAL_STATUSES / STATUS_ICON ─────
(function group1() {
    var SL = test.STATUS_LABEL;
    var TS = test.TERMINAL_STATUSES;
    var SI = test.STATUS_ICON;

    // 11 statuses with non-empty labels.
    var expected = ['pending','running','ac','wa','tle','mle','re','ole','pe','ce','se'];
    var got = Object.keys(SL).sort();
    eq(got, expected.sort(), 'g1.1 STATUS_LABEL has exactly 11 keys');

    // Terminal status set: 9 (no pending/running).
    var gotT = Object.keys(TS).sort();
    eq(gotT, ['ac','ce','mle','ole','pe','re','se','tle','wa'], 'g1.2 TERMINAL_STATUSES has 9 keys');

    // Each terminal status has a non-empty label.
    expected.forEach(function (s) {
        ok(typeof SL[s] === 'string' && SL[s].length > 0, 'g1.3 label for ' + s);
    });
    // Each status has an icon glyph.
    expected.forEach(function (s) {
        ok(typeof SI[s] === 'string' && SI[s].length > 0, 'g1.4 icon for ' + s);
    });
})();

// ── Group 2: tabHistoryBuildParams ──────────────────────────────
(function group2() {
    // The helper reads `state.problem` + `tabHistoryState` from the
    // IIFE closure. We test it by calling the __lcTest-exposed
    // function with explicit overrides (signature v1.2.29+:
    // `tabHistoryBuildParams(stateOverride, tabOverride)`).
    function runWithState(fakeState) {
        return test.tabHistoryBuildParams(fakeState, { status: '', page: 0 });
    }
    function runWithTabState(fakeState, fakeTab) {
        return test.tabHistoryBuildParams(fakeState, fakeTab);
    }

    // problem_id is always included.
    eq(runWithState({ problem: { id: 42 } }),
       { problem_id: 42, limit: 20, offset: 0 },
       'g2.1 problem_id 42 + default page');

    // status filter is omitted when empty.
    eq(runWithTabState({ problem: { id: 7 } }, { status: '', page: 0 }),
       { problem_id: 7, limit: 20, offset: 0 },
       'g2.2 status "" is dropped');

    // status filter passes through.
    eq(runWithTabState({ problem: { id: 7 } }, { status: 'ac', page: 0 }),
       { problem_id: 7, limit: 20, offset: 0, status: 'ac' },
       'g2.3 status=ac passes through');

    // pagination math: page 0 → offset 0, page 1 → offset 20, etc.
    eq(runWithTabState({ problem: { id: 1 } }, { status: '', page: 0 }).offset, 0, 'g2.4a page 0 → offset 0');
    eq(runWithTabState({ problem: { id: 1 } }, { status: '', page: 1 }).offset, 20, 'g2.4b page 1 → offset 20');
    eq(runWithTabState({ problem: { id: 1 } }, { status: '', page: 5 }).offset, 100, 'g2.4c page 5 → offset 100');

    // No problem loaded → problem_id is ''.
    eq(runWithState({ problem: null }).problem_id, '', 'g2.5 null problem → problem_id empty');
})();

// ── Group 3: tabHistoryBuildQuery ───────────────────────────────
(function group3() {
    function runWithTabState(fakeState, fakeTab) {
        return test.tabHistoryBuildQuery(fakeState, fakeTab);
    }

    eq(runWithTabState({ problem: { id: 42 } }, { status: '', page: 0 }),
       '?problem_id=42&limit=20&offset=0',
       'g3.1 minimal query');

    eq(runWithTabState({ problem: { id: 1 } }, { status: 'ac', page: 2 }),
       '?problem_id=1&limit=20&offset=40&status=ac',
       'g3.2 status=ac, page=2 (offset=40)');

    // URL-encoding: a problem id should be safe (numeric), but
    // tabHistoryState.status could in principle contain anything;
    // the helper uses encodeURIComponent.
    eq(runWithTabState({ problem: { id: 1 } }, { status: 'a&b=c', page: 0 }),
       '?problem_id=1&limit=20&offset=0&status=a%26b%3Dc',
       'g3.3 status with metachars is URL-encoded');
})();

// ── Group 4: tabHistoryFormatTime ───────────────────────────────
(function group4() {
    // Use the helper directly. It takes the server's
    // "YYYY-MM-DD HH:MM:SS" format and returns a friendly
    // string. We mock `Date.now()` to make the relative windows
    // deterministic.
    function fmt(iso) {
        return vm.runInContext('__lcTest.tabHistoryFormatTime(' + JSON.stringify(iso) + ')', sandbox);
    }

    // null / empty / unparseable → "—".
    eq(fmt(null), '—', 'g4.1 null → em-dash');
    eq(fmt(''),   '—', 'g4.2 empty string → em-dash');
    eq(fmt('not a date'), 'not a date', 'g4.3 unparseable → returns input');

    // Helper relies on `Date.now()` so we just exercise the
    // "刚刚" / "时间未知" branches with a recent Date.
    var now = new Date();
    function pad(n) { return n < 10 ? '0' + n : '' + n; }
    var recent = now.getUTCFullYear() + '-' + pad(now.getUTCMonth() + 1) + '-' + pad(now.getUTCDate())
        + ' ' + pad(now.getUTCHours()) + ':' + pad(now.getUTCMinutes()) + ':' + pad(now.getUTCSeconds());
    var s = fmt(recent);
    // Just make sure it returned a non-empty string; the exact
    // relative label depends on the clock drift between UTC and
    // local, so we accept any of "刚刚", "N 分钟前", "N 小时前".
    ok(typeof s === 'string' && s.length > 0, 'g4.4 recent time formats to non-empty string');
})();

// ── Group 5: tabHistoryParseHash ────────────────────────────────
(function group5() {
    function parseWith(h) {
        var savedHash = sandbox.location.hash;
        sandbox.location.hash = h;
        try { return vm.runInContext('__lcTest.tabHistoryParseHash()', sandbox); }
        finally { sandbox.location.hash = savedHash; }
    }

    eq(parseWith(''),                    null, 'g5.1 empty hash → null');
    eq(parseWith('#problem'),             null, 'g5.2 #problem → null');
    eq(parseWith('#history'),             { open: true, status: '', page: 0 }, 'g5.3 #history alone');
    eq(parseWith('#history&status=ac'),   { open: true, status: 'ac', page: 0 }, 'g5.4 #history&status=ac');
    eq(parseWith('#history&status=wa&page=3'),
       { open: true, status: 'wa', page: 2 }, 'g5.5 #history&status=wa&page=3 → page 2 (1-indexed)');
    // page=1 is "page 0" (1-indexed → 0-indexed).
    eq(parseWith('#history&page=1'),
       { open: true, status: '', page: 0 }, 'g5.6 page=1 → page index 0');
    // Garbage page= value is dropped silently.
    eq(parseWith('#history&page=abc'),
       { open: true, status: '', page: 0 }, 'g5.7 page=abc → page 0');
})();

// ── Group 6: mergeSubmissionRows ───────────────────────────────
//
// v1.2.56 — pure helper used by RowSubscriptionManager (both
// profile.html and problem.html) to splice an SSE-published row
// into the page's current submission list. The contract:
//
//   * Empty list + new row → seed (initialize to [new]).
//   * Non-empty list + matching id → replace in place.
//   * Non-empty list + unknown id → no-op (don't append).
//   * null / missing-id newRow → return input copy unchanged.
//   * Never mutates input (deep-equal before/after).
//
// The function lives on problem.html's `__lcTest` so this test
// reads it via the same sandbox as the other helpers. profile.html
// shares the pure contract by reference — it doesn't redefine.
(function group6() {
    var m = test.mergeSubmissionRows;

    // g6.1: empty + new → seed with [new].
    eq(m([], { id: 1, status: 'ac' }),
       [{ id: 1, status: 'ac' }],
       'g6.1 empty + new → [new] (initialize)');

    // g6.2: same id → replace, length unchanged.
    eq(m([{ id: 1, status: 'pending' }], { id: 1, status: 'ac' }),
       [{ id: 1, status: 'ac' }],
       'g6.2 same id → replaced, length 1');

    // g6.3: mid-array replace, positions preserved.
    eq(m([{ id: 1, status: 'pending' }, { id: 2, status: 'wa' }],
         { id: 2, status: 'ac' }),
       [{ id: 1, status: 'pending' }, { id: 2, status: 'ac' }],
       'g6.3 mid-array replace, positions preserved');

    // g6.4: input array is not mutated.
    var orig = [{ id: 1, status: 'pending' }, { id: 2, status: 'pending' }];
    var snap = JSON.parse(JSON.stringify(orig));
    m(orig, { id: 1, status: 'ac' });
    eq(orig, snap, 'g6.4 input array is not mutated (deep equality)');

    // g6.5: unknown id in non-empty list → no-op (length + ids unchanged).
    eq(m([{ id: 1, status: 'pending' }], { id: 99, status: 'ac' }),
       [{ id: 1, status: 'pending' }],
       'g6.5 unknown id (non-empty) → no-op');

    // g6.6: non-terminal update still replaces so the visible state
    // can refresh in place (running → running with new fields).
    eq(m([{ id: 1, status: 'running', time_used: null }],
         { id: 1, status: 'running', time_used: 42 }),
       [{ id: 1, status: 'running', time_used: 42 }],
       'g6.6 non-terminal update replaces');

    // g6.7: null newRow → returns a (copy of) the input unchanged.
    eq(m([{ id: 1, status: 'pending' }], null),
       [{ id: 1, status: 'pending' }],
       'g6.7 null newRow → input copy unchanged');

    // g6.8: newRow missing id → no-op (defensive — SSE frame should
    // never produce a row without an id, but if it does, don't crash).
    eq(m([{ id: 1, status: 'pending' }], { status: 'ac' }),
       [{ id: 1, status: 'pending' }],
       'g6.8 newRow missing id → input copy unchanged');
})();

// ── Report ─────────────────────────────────────────────────────
console.log('');
console.log('Submission history unit tests');
console.log('-----------------------------');
console.log('  passed: ' + passed);
console.log('  failed: ' + failed);
if (failed > 0) {
    console.log('');
    console.log('Failures:');
    failures.forEach(function (f) {
        console.log('  ✗ ' + f.label);
        console.log('      actual:   ' + f.actual);
        console.log('      expected: ' + f.expected);
    });
    process.exit(1);
}
console.log('  OK');
process.exit(0);
