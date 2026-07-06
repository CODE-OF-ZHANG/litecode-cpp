// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend unit tests for the SSE parser (Phase 5 ★ 异步判题轮询/SSE 客户端)
//
// Runs in Node 18+ with `node web/test/sse-parser.test.js`. No test
// runner, no dependencies — just a tiny assert + a vm sandbox that
// loads `web/js/api.js` and reaches into its private closure to
// extract the two pure functions we want to test:
//
//   parseSseFrames(buffer)  → { frames: string[], rest: string }
//   parseSseFrame(frame)    → { event, data, retry }
//
// The two functions are *pure*: they don't touch the DOM, fetch, or
// timers, so a 60-line harness is enough to lock down the parser
// contract without spinning up a real browser.
//
// Why this matters: the SSE stream is the user's only feedback while
// the judge is running. A regression in the frame splitter (e.g.
// greedily eating the trailing \n\n, dropping the final frame) would
// silently turn "AC" into "judge never finished". A 30+ case test
// file means the next refactor can't break that.
//
// Test groups:
//   1) parseSseFrames — boundary / split / incomplete-tail behaviour
//   2) parseSseFrame  — event / data / retry / multi-line / unknown
//   3) end-to-end     — server-emitted text → expected dispatch
//
// Exit code 0 on success, 1 on any failure. v1.2.28 baseline: 30+
// cases, all PASS.

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
function throws(fn, label) {
    try { fn(); failed++; failures.push({ label: label, actual: 'did not throw', expected: 'throw' }); }
    catch (_) { passed++; }
}

// ── Load api.js in a sandbox and expose the private helpers ────
//
// api.js wraps everything in an IIFE but re-exports the two pure
// SSE parsers on `litecode.api._sseParseFrames` / `_sseParseFrame`.
// The vm sandbox stubs out `window` so the IIFE's `(this.litecode
// = ...)` line lands on our sandbox object; from there we can pull
// the helpers out.
var apiPath = path.resolve(__dirname, '..', 'js', 'api.js');
var apiSrc  = fs.readFileSync(apiPath, 'utf8');
var harness = '';   // no extra trailing code needed

// Minimal browser shim. api.js touches the DOM only inside the
// `litecode.boot.shell` IIFE; none of that runs until app.js calls
// it, so we can leave document/window mostly empty.
var sandbox = {
    console: console,
    setTimeout: setTimeout,
    clearTimeout: clearTimeout,
    setInterval: setInterval,
    clearInterval: clearInterval,
    Promise: Promise,
    AbortController: AbortController,
    TextDecoder: TextDecoder,
    TextEncoder: TextEncoder,
    URLSearchParams: URLSearchParams,
    fetch: function () { return Promise.reject(new Error('fetch not stubbed')); },
    navigator: { onLine: true },
    isSecureContext: false,
    window: null,
    document: {
        addEventListener: function () {},
        removeEventListener: function () {},
        createElement: function () { return { style: {} }; },
        getElementById: function () { return null; },
        querySelector: function () { return null; },
        querySelectorAll: function () { return []; },
        dispatchEvent: function () {},
        cookie: '',
    },
    location: { pathname: '/test.html', search: '', href: 'http://localhost/test.html' },
    sessionStorage: { getItem: function () { return null; }, setItem: function () {}, removeItem: function () {} },
    localStorage: { getItem: function () { return null; }, setItem: function () {}, removeItem: function () {} },
    CustomEvent: function (type, init) { this.type = type; this.detail = init && init.detail; },
};
sandbox.window   = sandbox;
sandbox.self     = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(apiSrc + '\n' + harness, sandbox);

var litecode = sandbox.litecode;
if (!litecode || !litecode.api || !litecode.api._sseParseFrames) {
    console.error('FATAL: api.js did not expose _sseParseFrames on litecode.api');
    process.exit(2);
}
var parseSseFrames = litecode.api._sseParseFrames;
var parseSseFrame  = litecode.api._sseParseFrame;
var LitecodeApiError = litecode.api.LitecodeApiError;

// ── Group 1: parseSseFrames ─────────────────────────────────────
//
// Spec: split on \n\n (two consecutive LF), return completed
// frames + the trailing incomplete chunk.

(function group1() {
    // Empty input → no frames, no rest.
    eq(parseSseFrames(''), { frames: [], rest: '' }, 'g1.1 empty input');

    // Single complete frame, no trailing junk.
    eq(parseSseFrames('event: result\ndata: {"id":1}\n\n'),
       { frames: ['event: result\ndata: {"id":1}'], rest: '' },
       'g1.2 single complete frame');

    // Two complete frames.
    eq(parseSseFrames('event: a\ndata: 1\n\nevent: b\ndata: 2\n\n'),
       { frames: ['event: a\ndata: 1', 'event: b\ndata: 2'], rest: '' },
       'g1.3 two complete frames');

    // Incomplete trailing frame stays in `rest`.
    var r = parseSseFrames('event: x\ndata: hello\n\nevent: y\ndata:');
    eq(r.frames, ['event: x\ndata: hello'], 'g1.4a incomplete tail: completed frames');
    eq(r.rest,  'event: y\ndata:',          'g1.4b incomplete tail: rest buffer');

    // Continuation: feeding the rest back in completes the frame.
    var r2 = parseSseFrames(r.rest + ' world\n\n');
    eq(r2.frames, ['event: y\ndata: world'], 'g1.5a rest + tail = full frame');
    eq(r2.rest,   '',                          'g1.5b rest drained after completion');

    // Empty frames between \n\n are skipped (this happens at end of
    // the server's "retry: 3000\n\n" preamble when followed by a
    // data frame).
    eq(parseSseFrames('\n\nevent: r\ndata: ok\n\n'),
       { frames: ['event: r\ndata: ok'], rest: '' },
       'g1.6 leading empty frame is dropped');

    // Multi-line `data:` field — the line joiner is in parseSseFrame,
    // not parseSseFrames, but the splitter should still treat the
    // whole block as ONE frame.
    var multi = 'event: result\ndata: line1\ndata: line2\n\n';
    var r3 = parseSseFrames(multi);
    eq(r3.frames.length, 1, 'g1.7a multi-line data: counts as 1 frame');
    ok(r3.frames[0].indexOf('data: line1') !== -1, 'g1.7b multi-line data: first line preserved');
    ok(r3.frames[0].indexOf('data: line2') !== -1, 'g1.7c multi-line data: second line preserved');

    // Server's retry preamble: "retry: 3000\n\n" + a real frame.
    var withRetry = 'retry: 3000\n\nevent: result\ndata: {"x":1}\n\n';
    var r4 = parseSseFrames(withRetry);
    eq(r4.frames, ['retry: 3000', 'event: result\ndata: {"x":1}'], 'g1.8 retry preamble is its own frame');
    eq(r4.rest,   '',                                               'g1.8 retry preamble drained rest');

    // Buffer that's ONLY a partial frame (no terminator at all).
    var r5 = parseSseFrames('event: pending\ndata: {"id":42}');
    eq(r5.frames, [],                        'g1.9a no terminator → no frames');
    eq(r5.rest,   'event: pending\ndata: {"id":42}', 'g1.9b no terminator → all in rest');

    // Buffer ending in \n (one LF, not two). Treated as incomplete.
    var r6 = parseSseFrames('event: a\ndata: 1\n');
    eq(r6.frames, [],                  'g1.10a single \\n → no frames');
    eq(r6.rest,   'event: a\ndata: 1\n', 'g1.10b single \\n → all in rest');

    // Many small frames in one chunk (heartbeat-style).
    var tiny = '';
    for (var i = 0; i < 20; i++) tiny += 'data: ' + i + '\n\n';
    var r7 = parseSseFrames(tiny);
    eq(r7.frames.length, 20, 'g1.11 20 small frames all split');
    eq(r7.rest,          '',  'g1.11 20 small frames drained');
})();

// ── Group 2: parseSseFrame ──────────────────────────────────────

(function group2() {
    // Minimal valid frame.
    eq(parseSseFrame('event: result\ndata: {"x":1}'),
       { event: 'result', data: '{"x":1}', retry: null },
       'g2.1 minimal frame');

    // With leading space stripped (per RFC 8895).
    eq(parseSseFrame('event:   result\ndata:    {"x":1}'),
       { event: 'result', data: '{"x":1}', retry: null },
       'g2.2 leading space after colon is stripped');

    // Multi-line data: lines are joined with \n.
    eq(parseSseFrame('data: a\ndata: b\ndata: c'),
       { event: null, data: 'a\nb\nc', retry: null },
       'g2.3 multi-line data is joined with \\n');

    // retry: parsed as integer.
    eq(parseSseFrame('retry: 3000'),
       { event: null, data: null, retry: 3000 },
       'g2.4 retry: 3000 → 3000');

    // retry: garbage → null.
    eq(parseSseFrame('retry: abc'),
       { event: null, data: null, retry: null },
       'g2.5 retry: garbage → null');

    // Unknown fields are ignored.
    eq(parseSseFrame('id: 42\nevent: result\ndata: ok\n:heartbeat comment'),
       { event: 'result', data: 'ok', retry: null },
       'g2.6 unknown id + :comment are ignored');

    // Lines without a colon: per RFC 8895 the whole line is the
    // field name and the value is the empty string. So 'event' (a
    // bare field name with no colon, no value) is recognised as
    // an event-type field with empty value.
    eq(parseSseFrame('event'),
       { event: '', data: null, retry: null },
       'g2.7 line with no colon: whole line is field name, value empty');

    // Empty input.
    eq(parseSseFrame(''),
       { event: null, data: null, retry: null },
       'g2.8 empty input');

    // Comment-only frame.
    eq(parseSseFrame(': this is a comment'),
       { event: null, data: null, retry: null },
       'g2.9 comment-only frame');

    // Blank line inside frame is ignored.
    eq(parseSseFrame('event: a\n\ndata: 1'),
       { event: 'a', data: '1', retry: null },
       'g2.10 blank line inside frame is ignored');

    // Realistic server payload (the v1.2.17 shape).
    eq(parseSseFrame('event: result\ndata: {"submission_id":42,"status":"ac","time_used":7}'),
       { event: 'result', data: '{"submission_id":42,"status":"ac","time_used":7}', retry: null },
       'g2.11 realistic server payload');
})();

// ── Group 3: end-to-end ────────────────────────────────────────
//
// Run a multi-chunk "stream" through parseSseFrames incrementally,
// just like api.js's reader loop would. This is the integration
// shape the application actually exercises.

(function group3() {
    function pump(chunks) {
        var out = [];
        var buf = '';
        for (var i = 0; i < chunks.length; i++) {
            buf += chunks[i];
            var p = parseSseFrames(buf);
            buf = p.rest;
            for (var j = 0; j < p.frames.length; j++) {
                out.push(parseSseFrame(p.frames[j]));
            }
        }
        // Flush whatever the server shipped as the very last frame
        // without a trailing \n\n (the cpp-httplib quirk — see
        // api.js's openSse reader loop's tail handler).
        if (buf.length > 0) {
            out.push(parseSseFrame(buf));
        }
        return out;
    }

    // Happy path: one-shot result frame.
    var frames1 = pump(['event: result\ndata: {"status":"ac"}\n\n']);
    eq(frames1.length,         1,             'g3.1a one-shot: 1 frame');
    eq(frames1[0].event,       'result',      'g3.1b one-shot: event=result');
    eq(JSON.parse(frames1[0].data).status, 'ac', 'g3.1c one-shot: data parsed');

    // Server splits the response across two TCP segments.
    var frames2 = pump([
        'event: result\ndata: {"id":1',
        ',"status":"wa","time_used":3}\n\n',
    ]);
    eq(frames2.length, 1, 'g3.2 split: 1 frame after both chunks');
    eq(JSON.parse(frames2[0].data).status, 'wa', 'g3.2 split: data merged');

    // Server's retry: 3000 preamble + a result frame.
    var frames3 = pump(['retry: 3000\n\nevent: result\ndata: {"status":"ac"}\n\n']);
    eq(frames3.length, 2,                  'g3.3 retry + result: 2 frames');
    eq(frames3[0].retry, 3000,             'g3.3 retry + result: retry parsed');
    eq(frames3[1].event, 'result',         'g3.3 retry + result: second event=result');

    // Error frame (404 on the SSE endpoint).
    var frames4 = pump(['event: error\ndata: {"status":404,"code":"NOT_FOUND","message":"submission not found"}\n\n']);
    eq(frames4.length, 1,           'g3.4 error frame: 1 frame');
    eq(frames4[0].event, 'error',   'g3.4 error frame: event=error');
    var errData = JSON.parse(frames4[0].data);
    eq(errData.status,  404,        'g3.4 error frame: status=404');
    eq(errData.code,    'NOT_FOUND','g3.4 error frame: code=NOT_FOUND');

    // Pending event (server's 25s timeout).
    var frames5 = pump(['event: pending\ndata: {"submission_id":7}\n\n']);
    eq(frames5[0].event, 'pending',  'g3.5 pending frame: event=pending');
    eq(JSON.parse(frames5[0].data).submission_id, 7, 'g3.5 pending frame: id=7');

    // Frame split mid-\n\n (worst case for the reader).
    var frames6 = pump([
        'event: result\ndata: {"x":1}\n',
        '\n',
    ]);
    eq(frames6.length, 1,                    'g3.6 split \\n\\n: 1 frame');
    eq(frames6[0].event, 'result',            'g3.6 split \\n\\n: event parsed');

    // Garbage (no events, no data) — parser yields nulls.
    var frames7 = pump(['just some random text\n\n']);
    eq(frames7[0].event, null, 'g3.7 garbage: event=null');
    eq(frames7[0].data,  null, 'g3.7 garbage: data=null');

    // Chinese / emoji UTF-8 in data — must round-trip through
    // TextDecoder + JSON.parse on the caller side. The parser
    // itself is byte-agnostic; we just check it preserves bytes.
    var frames8 = pump(['event: result\ndata: {"msg":"判题完成 🎉"}\n\n']);
    var d8 = JSON.parse(frames8[0].data);
    eq(d8.msg, '判题完成 🎉', 'g3.8 UTF-8 round-trip through parser');
})();

// ── Report ─────────────────────────────────────────────────────
console.log('');
console.log('SSE parser unit tests');
console.log('---------------------');
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
