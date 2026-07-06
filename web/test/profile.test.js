// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Unit tests for the profile page helpers
//               (Phase 5 ★ 个人主页 / 做题统计)
//
// Runs in Node 18+ with `node web/test/profile.test.js`. No test
// runner, no dependencies — just a tiny assert + a vm sandbox that
// loads profile.html's inline IIFE and pulls the pure helpers out
// of it via the `__lcTest` surface (no-op in production — see the
// test-surface comment in profile.html).
//
// What we test
// ------------
//   resolveUsername    — ?username= → {username,isSelf}, self-detect
//   computeStats       — submission aggregation: solved / attempted /
//                        acceptance rate / per-status / per-difficulty
//   buildProblemMeta   — id→meta map + per-difficulty live totals
//   formatPercent      — 0..1 ratio → "NN.N%"
//   formatProfileTime  — relative/absolute vocabulary
//   pct                — safe divide (0 denom → 0)
//
// Pure functions only — no fetch / DOM / timers touched. Exit 0 on
// success, 1 on any failure.

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

// ── Load profile.html and pull out the inline IIFE ─────────────
var htmlPath = path.resolve(__dirname, '..', 'profile.html');
var html = fs.readFileSync(htmlPath, 'utf8');
// Grab the LAST inline <script> (the IIFE) — there is only one, but
// be explicit in case a header script is added later.
var scripts = html.match(/<script>([\s\S]*?)<\/script>/g) || [];
if (!scripts.length) { console.error('FATAL: profile.html has no inline <script>'); process.exit(2); }
var iife = scripts[scripts.length - 1].replace(/^<script>/, '').replace(/<\/script>$/, '');

var sandbox = {
    console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout,
    Promise: Promise,
    URLSearchParams: URLSearchParams,
    addEventListener: function () {},
    removeEventListener: function () {},
    location: { search: '', pathname: '/profile.html', href: 'http://localhost/profile.html' },
    navigator: { onLine: true, userAgent: 'node' },
    window: null,
    document: {
        addEventListener: function () {},
        removeEventListener: function () {},
        createElement: function () { return { style: {}, dataset: {}, setAttribute: function () {}, appendChild: function () {} }; },
        getElementById: function () { return null; },
        createTextNode: function () { return {}; },
    },
    litecode: {
        api: {
            get: function () { return Promise.reject(new Error('stub')); },
            auth: { fetchProfile: function () { return Promise.reject(new Error('stub')); }, currentUser: null },
        },
        boot: { shell: function () { return Promise.resolve(); } },
        guard: { requireAuth: function () { return Promise.resolve(); } },
    },
    __lcTest: {},
};
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
try {
    vm.runInContext(iife, sandbox);
} catch (e) {
    console.error('FATAL: IIFE threw at top level: ' + e.message);
    console.error(e.stack);
    process.exit(2);
}

var t = sandbox.__lcTest;
ok(t && typeof t.computeStats === 'function', 'test surface populated');

// ── Group 1: resolveUsername ────────────────────────────────────
eq(t.resolveUsername('', 'alice'), { username: 'alice', isSelf: true }, 'empty search → self');
eq(t.resolveUsername('?username=alice', 'alice'), { username: 'alice', isSelf: true }, 'own username → self');
eq(t.resolveUsername('?username=Alice', 'alice'), { username: 'Alice', isSelf: true }, 'case-insensitive self match');
eq(t.resolveUsername('?username=bob', 'alice'), { username: 'bob', isSelf: false }, 'other username → not self');
eq(t.resolveUsername('?username=bob', ''), { username: 'bob', isSelf: false }, 'no current user → not self');
eq(t.resolveUsername('?username=%20', 'alice'), { username: 'alice', isSelf: true }, 'blank-only username trims to self');

// ── Group 2: formatPercent + pct ────────────────────────────────
eq(t.pct(0, 0), 0, 'pct 0/0 → 0 (no NaN)');
eq(t.pct(3, 4), 0.75, 'pct 3/4');
eq(t.formatPercent(0), '0.0%', 'percent 0');
eq(t.formatPercent(1), '100.0%', 'percent 1');
eq(t.formatPercent(0.3333), '33.3%', 'percent rounds to 1 dp');

// ── Group 3: buildProblemMeta ───────────────────────────────────
var meta = t.buildProblemMeta([
    { id: 1, slug: 'two-sum', title: '两数之和', difficulty: 'easy' },
    { id: 2, slug: 'add-two', title: 'Add Two', difficulty: 'medium' },
    { id: 3, slug: 'hard-one', title: 'Hard', difficulty: 'hard' },
    { id: 4, slug: 'easy-two', title: 'E2', difficulty: 'easy' },
]);
eq(meta.diffTotals, { easy: 2, medium: 1, hard: 1 }, 'per-difficulty totals');
eq(meta.map['1'].slug, 'two-sum', 'meta map keyed by string id');
eq(meta.map['2'].title, 'Add Two', 'meta map title');
ok(meta.map['99'] === undefined, 'unknown id absent from map');
eq(t.buildProblemMeta([]).diffTotals, { easy: 0, medium: 0, hard: 0 }, 'empty catalog → zero totals');

// ── Group 4: computeStats ───────────────────────────────────────
var subs = [
    { problem_id: 1, status: 'ac' },
    { problem_id: 1, status: 'wa' },   // same problem, still solved once
    { problem_id: 2, status: 'ac' },
    { problem_id: 3, status: 'tle' },  // attempted, not solved
    { problem_id: 3, status: 'wa' },
];
var stats = t.computeStats(subs, meta.map);
eq(stats.totalSubmissions, 5, 'total submissions');
eq(stats.acSubmissions, 2, 'ac submissions');
eq(stats.solvedCount, 2, 'distinct solved problems (1 & 2)');
eq(stats.attemptedCount, 3, 'distinct attempted problems (1,2,3)');
eq(stats.acceptanceRate, 2 / 5, 'acceptance rate = ac/total');
eq(stats.byStatus, { ac: 2, wa: 2, tle: 1 }, 'per-status counts');
eq(stats.byDifficultySolved, { easy: 1, medium: 1, hard: 0, unknown: 0 }, 'solved by difficulty');

// Empty history.
var empty = t.computeStats([], meta.map);
eq(empty.totalSubmissions, 0, 'empty: 0 submissions');
eq(empty.acceptanceRate, 0, 'empty: 0 acceptance (no NaN)');
eq(empty.solvedCount, 0, 'empty: 0 solved');

// AC for a problem missing from the catalog (soft-deleted) → counted
// in solved total but bucketed as unknown difficulty.
var orphan = t.computeStats([{ problem_id: 999, status: 'ac' }], meta.map);
eq(orphan.solvedCount, 1, 'orphan AC counts as solved');
eq(orphan.byDifficultySolved.unknown, 1, 'orphan AC → unknown difficulty bucket');

// Missing status defaults to pending (defensive).
var noStatus = t.computeStats([{ problem_id: 1 }], meta.map);
eq(noStatus.byStatus, { pending: 1 }, 'missing status → pending');

// ── Group 5: formatProfileTime ──────────────────────────────────
eq(t.formatProfileTime(''), '—', 'empty time → em dash');
eq(t.formatProfileTime('garbage'), 'garbage', 'unparseable → passthrough');
ok(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/.test(t.formatProfileTime('2020-01-02 03:04:05')),
   'old timestamp → absolute date form');

// ── Summary ─────────────────────────────────────────────────────
console.log('');
if (failed) {
    console.log('FAILURES:');
    failures.forEach(function (f) {
        console.log('  ✗ ' + f.label + '\n      actual:   ' + f.actual + '\n      expected: ' + f.expected);
    });
}
console.log('passed: ' + passed);
console.log('failed: ' + failed);
process.exit(failed > 0 ? 1 : 0);
