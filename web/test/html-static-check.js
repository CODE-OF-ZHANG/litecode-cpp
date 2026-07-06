// Static check for every production HTML page (Phase 5 ★):
//   1. CSP meta tag byte-for-byte matches the csp.js canonical
//      CSP_VALUE (defense-in-depth — if a page is edited with a
//      typo, the browser will accept a weaker policy silently).
//   2. <script src=...> load order is csp.js → api.js → app.js
//      (with markdown.js, if present, between csp.js and app.js).
//
// The XSS test harness (web/test/markdown-xss.html) is excluded:
// it is a standalone page not served in production and has no
// boot.shell, so it does not need the meta + script order contract.

'use strict';

const fs   = require('fs');
const path = require('path');

const cspJs = fs.readFileSync(path.resolve(__dirname, '..', 'js', 'csp.js'), 'utf8');
const start = cspJs.indexOf('var CSP_VALUE');
const next  = cspJs.indexOf('var SCRIPTS', start);
const block = cspJs.slice(start, next);
const strings = [];
const re = /"([^"]*)"|'([^']*)'/g;
let m;
while ((m = re.exec(block)) !== null) strings.push(m[1] != null ? m[1] : m[2]);
const canonical = strings.join('').replace(/\s+/g, ' ').trim();

function findHtml(dir) {
    var out = [];
    for (var name of fs.readdirSync(dir)) {
        var p = path.join(dir, name);
        var st = fs.statSync(p);
        if (st.isDirectory()) out = out.concat(findHtml(p));
        else if (name.endsWith('.html')) out.push(p);
    }
    return out;
}

var webRoot = path.resolve(__dirname, '..');
var pages = findHtml(webRoot).sort();

var failed = 0;
var passed = 0;
pages.forEach(function (p) {
    var rel = p.replace(webRoot, '').replace(/\\/g, '/');
    if (rel.indexOf('/test/') === 0) {
        console.log('  - ' + rel + '  (test harness, skipped)');
        return;
    }
    var html = fs.readFileSync(p, 'utf8');

    var metaMatch = html.match(/<meta\s+http-equiv="Content-Security-Policy"\s+content="([^"]+)"/);
    if (!metaMatch) {
        console.log('  ✗ ' + rel + '  no CSP meta tag');
        failed++;
        return;
    }
    var metaNormalized = metaMatch[1].replace(/\s+/g, ' ').trim();
    if (metaNormalized !== canonical) {
        console.log('  ✗ ' + rel + '  CSP meta != canonical');
        console.log('    meta:      ' + metaNormalized);
        console.log('    canonical: ' + canonical);
        failed++;
        return;
    }

    var scripts = [];
    var re2 = /<script\s+src="([^"]+)"/g;
    var m2;
    while ((m2 = re2.exec(html)) !== null) scripts.push(m2[1]);
    var order = scripts.map(function (s) { return s.replace(/^.*\//, ''); });
    if (!order[0] || order[0] !== 'csp.js') {
        console.log('  ✗ ' + rel + '  first script is not csp.js: ' + JSON.stringify(order));
        failed++;
        return;
    }
    var apiIdx = order.indexOf('api.js');
    var appIdx = order.indexOf('app.js');
    if (apiIdx === -1) { console.log('  ✗ ' + rel + '  no api.js'); failed++; return; }
    if (appIdx === -1) { console.log('  ✗ ' + rel + '  no app.js'); failed++; return; }
    if (apiIdx <= 0 || appIdx <= 0 || apiIdx >= appIdx) {
        console.log('  ✗ ' + rel + '  bad order: ' + JSON.stringify(order));
        failed++;
        return;
    }
    var mdIdx = order.indexOf('markdown.js');
    if (mdIdx !== -1 && (mdIdx <= 0 || mdIdx >= appIdx)) {
        console.log('  ✗ ' + rel + '  markdown.js out of place: ' + JSON.stringify(order));
        failed++;
        return;
    }
    console.log('  ✓ ' + rel + '  order=[' + order.join(',') + ']');
    passed++;
});

console.log('');
console.log('passed: ' + passed);
console.log('failed: ' + failed);
process.exit(failed > 0 ? 1 : 0);
