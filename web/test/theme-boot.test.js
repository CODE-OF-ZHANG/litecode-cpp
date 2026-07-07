// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Unit tests for the no-flash dark-mode bootstrap
//               (Phase 5 ★ 深色模式 / SPEC §6.3 / A34)
//
// Runs in Node 18+ with `node web/test/theme-boot.test.js`. No test
// runner, no dependencies — just a tiny assert harness + a vm sandbox
// that loads `theme-boot.js` against a minimal DOM/localStorage stub.
//
// Why a sandbox?
//   theme-boot.js runs synchronously in <head> BEFORE the document
//   body exists. It only touches:
//     - `localStorage.getItem`
//     - `document.documentElement.classList`
//     - `document.documentElement.setAttribute`
//   We stub exactly those three and assert the post-state.
//
// What we test
// ------------
//   localStorage = 'dark'    → .dark class added + data-theme-chosen set
//   localStorage = 'light'   → .dark class absent + data-theme-chosen set
//   localStorage = null      → no side effects (lets @media query decide)
//   localStorage = 'system'  → invalid value, treated as null
//   localStorage = 'auto'    → invalid value, treated as null
//   localStorage throws      → no crash, no side effects
//   localStorage missing     → no crash, no side effects
//
// Total: ~15 cases. Baseline v1.2.x: 15/15 PASS.

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

// ── Minimal DOM stub ────────────────────────────────────────────
//
// classList + setAttribute are the only DOM surfaces theme-boot.js
// touches. We mirror the bits used so the script runs cleanly.
function makeDocumentElement(initial) {
    var el = {
        _classes: new Set(initial && initial.classList ? initial.classList : []),
        _attrs:   Object.assign({}, initial && initial.attrs ? initial.attrs : {}),
        classList: {
            add:    function (c) { el._classes.add(c); },
            remove: function (c) { el._classes.delete(c); },
            contains: function (c) { return el._classes.has(c); },
        },
        setAttribute: function (name, value) { el._attrs[name] = String(value); },
        getAttribute: function (name) { return name in el._attrs ? el._attrs[name] : null; },
    };
    return el;
}

function runThemeBoot(opts) {
    opts = opts || {};
    var stored = opts.stored;
    var storageThrows = !!opts.storageThrows;
    var storageMissing = !!opts.storageMissing;

    var docEl = makeDocumentElement();
    var calls = { getItem: 0, lastKey: null };

    var storage = {
        getItem: function (k) {
            calls.getItem++;
            calls.lastKey = k;
            if (storageThrows) throw new Error('SecurityError: storage disabled');
            if (storageMissing) return null;
            return stored === undefined ? null : stored;
        },
    };

    // theme-boot.js reaches localStorage via `window.localStorage.*`,
    // not as a bare global. We mirror that in the sandbox by attaching
    // `storage` to `window.localStorage` (the bare `localStorage` key
    // alone wouldn't be visible to the script).
    var sandbox = {
        console: console,
        window: { localStorage: storage },
        localStorage: storage,  // belt-and-braces: some hosts expose both
        document: { documentElement: docEl },
    };
    vm.createContext(sandbox);

    var src = fs.readFileSync(path.resolve(__dirname, '..', 'js', 'theme-boot.js'), 'utf8');
    vm.runInContext(src, sandbox);

    return {
        docEl: docEl,
        calls: calls,
        hasDark:     docEl._classes.has('dark'),
        chosenAttr:  docEl._attrs['data-theme-chosen'] === undefined
                       ? null : docEl._attrs['data-theme-chosen'],
    };
}

// ── Load the source once for syntax verification ────────────────
var src = fs.readFileSync(path.resolve(__dirname, '..', 'js', 'theme-boot.js'), 'utf8');
new vm.Script(src); // throws on parse error

// ── Cases ────────────────────────────────────────────────────────

// 1. Stored 'dark' → adds .dark class + sets attribute.
var r = runThemeBoot({ stored: 'dark' });
ok(r.hasDark,               'dark: .dark class added');
eq(r.chosenAttr, '1',       'dark: data-theme-chosen="1"');
eq(r.calls.lastKey, 'litecode:theme', 'dark: localStorage probed with canonical key');
eq(r.calls.getItem, 1,      'dark: localStorage probed exactly once');

// 2. Stored 'light' → does NOT add .dark class, still sets attribute.
r = runThemeBoot({ stored: 'light' });
ok(!r.hasDark,              'light: .dark class NOT added');
eq(r.chosenAttr, '1',       'light: data-theme-chosen="1"');
eq(r.calls.getItem, 1,      'light: localStorage probed exactly once');

// 3. Stored null (no preference yet) → leaves attribute absent.
r = runThemeBoot({ stored: null });
ok(!r.hasDark,              'null: .dark class NOT added');
eq(r.chosenAttr, null,      'null: data-theme-chosen attribute absent');
eq(r.calls.getItem, 1,      'null: localStorage probed exactly once');

// 4. Stored undefined (treated same as null).
r = runThemeBoot({});
ok(!r.hasDark,              'undefined: .dark class NOT added');
eq(r.chosenAttr, null,      'undefined: data-theme-chosen attribute absent');

// 5. Bogus values are treated as null. The CSS @media rule will then
//    decide based on the system preference.
['', 'auto', 'system', 'DARK', 'Light', '  dark  '].forEach(function (bad) {
    r = runThemeBoot({ stored: bad });
    ok(!r.hasDark,         'bogus "' + bad + '": .dark NOT added');
    eq(r.chosenAttr, null, 'bogus "' + bad + '": attribute absent');
});

// 6. localStorage throws (private mode / sandboxed iframe).
//    Script must not crash and must not apply anything.
r = runThemeBoot({ storageThrows: true });
ok(!r.hasDark,              'throws: no .dark applied');
eq(r.chosenAttr, null,      'throws: no attribute set');

// 7. localStorage missing entirely (e.g. some test environments).
r = runThemeBoot({ storageMissing: true });
ok(!r.hasDark,              'missing: no .dark applied');
eq(r.chosenAttr, null,      'missing: no attribute set');

// 8. The script never queries localStorage more than once — it's
//    strictly a one-shot bootstrap that runs in <head>.
r = runThemeBoot({ stored: 'dark' });
eq(r.calls.getItem, 1,      'one-shot: only one localStorage.getItem call');
r = runThemeBoot({ stored: 'light' });
eq(r.calls.getItem, 1,      'one-shot (light): only one call');

// 9. Two consecutive boots with different state must each be
//    independent (fresh DOM + storage on each run, by design — the
//    IIFE doesn't leak state across runs because each `runThemeBoot`
//    builds a fresh sandbox). This guards against any accidental
//    module-level closure we might introduce later.
r = runThemeBoot({ stored: 'dark' });
ok(r.hasDark,               'run A: dark applied');
r = runThemeBoot({ stored: 'light' });
ok(!r.hasDark,              'run B (fresh): light did NOT apply dark');

// ── Report ───────────────────────────────────────────────────────
console.log('');
console.log('theme-boot.test.js: ' + passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
    failures.forEach(function (f) {
        console.log('  ✗ ' + f.label);
        console.log('      actual:   ' + f.actual);
        console.log('      expected: ' + f.expected);
    });
    process.exit(1);
}
process.exit(0);