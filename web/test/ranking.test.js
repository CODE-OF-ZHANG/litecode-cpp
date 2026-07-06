// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Unit tests for the ranking page helpers
//               (Phase 5 ★ 排行榜页面, /ranking.html)
//
// Runs in Node 18+ with `node web/test/ranking.test.js`. No test
// runner, no dependencies — a tiny assert + a vm sandbox that loads
// ranking.html's inline IIFE and pulls the pure helpers out of it
// via the `__lcTest` surface (no-op in production).
//
// What we test
// ------------
//   normalizeRankItem  — tolerant row normalization (field-name
//                        spellings, acceptance-rate 0..1 vs 0..100,
//                        derived rate, fallback rank)
//   formatPercent      — 0..1 ratio → "NN.N%"
//   rankMedal          — 1/2/3 → 🥇🥈🥉, else ''
//   rankFromOffset     — page/index → 1-based global rank
//   buildRankQuery     — ?limit&offset construction
//   isEndpointMissing  — 404/501/NOT_FOUND → true (Phase 6 degrade)
//
// Exit 0 on success, 1 on any failure.

'use strict';

const fs   = require('fs');
const path = require('path');
const vm   = require('vm');

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
function close(actual, expected, label) {
    if (Math.abs(actual - expected) < 1e-9) { passed++; return; }
    failed++;
    failures.push({ label: label, actual: String(actual), expected: String(expected) });
}

// ── Load ranking.html and pull out the inline IIFE ─────────────
var htmlPath = path.resolve(__dirname, '..', 'ranking.html');
var html = fs.readFileSync(htmlPath, 'utf8');
var scripts = html.match(/<script>([\s\S]*?)<\/script>/g) || [];
if (!scripts.length) { console.error('FATAL: ranking.html has no inline <script>'); process.exit(2); }
var iife = scripts[scripts.length - 1].replace(/^<script>/, '').replace(/<\/script>$/, '');

var sandbox = {
    console: console,
    setTimeout: setTimeout, clearTimeout: clearTimeout,
    Promise: Promise,
    URLSearchParams: URLSearchParams,
    addEventListener: function () {},
    removeEventListener: function () {},
    location: { search: '', pathname: '/ranking.html', href: 'http://localhost/ranking.html' },
    history: { replaceState: function () {} },
    navigator: { userAgent: 'node' },
    window: null,
    document: {
        addEventListener: function () {},
        getElementById: function () { return null; },
        createElement: function () { return { style: {}, dataset: {}, setAttribute: function () {}, addEventListener: function () {}, appendChild: function () {} }; },
        createTextNode: function () { return {}; },
    },
    litecode: {
        api: { get: function () { return Promise.reject(new Error('stub')); }, auth: { currentUser: null } },
        boot: { shell: function () { return Promise.resolve(); } },
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
ok(t && typeof t.normalizeRankItem === 'function', 'test surface populated');
eq(t.RANK_PAGE_SIZE, 50, 'page size = 50');

// ── normalizeRankItem ───────────────────────────────────────────
var full = t.normalizeRankItem(
    { rank: 3, username: 'alice', solved: 12, submissions: 40, accepted: 20, acceptance_rate: 0.5 }, 99);
eq(full.rank, 3, 'explicit rank kept');
eq(full.username, 'alice', 'username kept');
eq(full.solved, 12, 'solved kept');
eq(full.submissions, 40, 'submissions kept');
eq(full.accepted, 20, 'accepted kept');
close(full.acceptanceRate, 0.5, 'acceptance rate 0..1 kept');

// Alternate field spellings + rate as 0..100.
var alt = t.normalizeRankItem(
    { username: 'bob', solved_count: 5, submission_count: 10, accepted_count: 8, acceptance_rate: 80 }, 7);
eq(alt.rank, 7, 'missing rank → fallback');
eq(alt.solved, 5, 'solved_count spelling');
eq(alt.submissions, 10, 'submission_count spelling');
close(alt.acceptanceRate, 0.8, 'rate 0..100 normalized to 0..1');

// Derived rate when acceptance_rate absent.
var derived = t.normalizeRankItem({ username: 'c', accepted: 3, submissions: 4 }, 1);
close(derived.acceptanceRate, 0.75, 'derived rate = accepted/submissions');

// Zero submissions → no divide-by-zero.
var zero = t.normalizeRankItem({ username: 'd' }, 1);
eq(zero.acceptanceRate, 0, 'zero submissions → 0 rate');
eq(zero.solved, 0, 'missing solved → 0');

// Non-numeric junk coerces to 0, null username → empty string.
var junk = t.normalizeRankItem({ username: null, solved: 'abc', submissions: null }, 2);
eq(junk.solved, 0, 'non-numeric solved → 0');
eq(junk.username, '', 'null username → empty string');

// ── formatPercent ───────────────────────────────────────────────
eq(t.formatPercent(0), '0.0%', 'percent 0');
eq(t.formatPercent(1), '100.0%', 'percent 1');
eq(t.formatPercent(0.3333), '33.3%', 'percent rounds to 1 dp');

// ── rankMedal ───────────────────────────────────────────────────
eq(t.rankMedal(1), '🥇', 'rank 1 → gold');
eq(t.rankMedal(2), '🥈', 'rank 2 → silver');
eq(t.rankMedal(3), '🥉', 'rank 3 → bronze');
eq(t.rankMedal(4), '', 'rank 4 → no medal');
eq(t.rankMedal(0), '', 'rank 0 → no medal');

// ── rankFromOffset ──────────────────────────────────────────────
eq(t.rankFromOffset(1, 0), 1, 'page1 index0 → rank 1');
eq(t.rankFromOffset(1, 4), 5, 'page1 index4 → rank 5');
eq(t.rankFromOffset(2, 0), 51, 'page2 index0 → rank 51');
eq(t.rankFromOffset(3, 9), 110, 'page3 index9 → rank 110');

// ── buildRankQuery ──────────────────────────────────────────────
eq(t.buildRankQuery(1), '?limit=50&offset=0', 'page 1 query');
eq(t.buildRankQuery(2), '?limit=50&offset=50', 'page 2 query');
eq(t.buildRankQuery(4), '?limit=50&offset=150', 'page 4 query');

// ── isEndpointMissing ───────────────────────────────────────────
ok(t.isEndpointMissing({ status: 404 }), '404 → missing');
ok(t.isEndpointMissing({ status: 501 }), '501 → missing');
ok(t.isEndpointMissing({ code: 'NOT_FOUND' }), 'NOT_FOUND code → missing');
ok(!t.isEndpointMissing({ status: 500 }), '500 → not missing (real error)');
ok(!t.isEndpointMissing({ status: 0 }), 'network error → not missing');
ok(!t.isEndpointMissing(null), 'null err → not missing');

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
