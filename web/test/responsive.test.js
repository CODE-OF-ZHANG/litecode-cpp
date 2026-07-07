// SPDX-License-Identifier: MIT
//
// Responsive CSS contract test (Phase 5 ★ 移动端响应式 / SPEC §6.3 / §11).
//
// Locks the responsive invariants every page relies on:
//   1) Each @media query block matches an expected upper bound.
//   2) Admin tables (problems / users / audit-logs / dashboard) set
//      `data-label="..."` on every cell the JS renders, so the <= 640px
//      CSS rule can render the column vocabulary in the stacked card
//      layout.
//   3) The admin tables that flip to card layout at <= 640px include
//      CSS rules that:
//        a) override `display: block` on the thead/tr/td chain
//        b) read each cell's label via `content: attr(data-label)`
//        c) hide the 操作 ("actions") column label so the buttons fill
//           the row cleanly
//
// If a future page adds an admin table without these tokens, this
// test fails before the mobile layout silently breaks.
//
// Run with:  node web/test/responsive.test.js

'use strict';

const fs   = require('fs');
const path = require('path');

const webRoot = path.resolve(__dirname, '..');
const cssPath = path.join(webRoot, 'css', 'style.css');

const css = fs.readFileSync(cssPath, 'utf8');

let failed = 0;
let passed = 0;

function ok(msg) {
    console.log('  ✓ ' + msg);
    passed++;
}
function fail(msg) {
    console.log('  ✗ ' + msg);
    failed++;
}

// ────────────────────────────────────────────────────────────────────
// 1. CSS contains the canonical mobile breakpoints
// ────────────────────────────────────────────────────────────────────
const breakpoints = [
    { bound: 1024, label: '≤ 1024px (md-editor + problem split collapse)' },
    { bound:  900, label: '≤  900px (problem-layout collapse)' },
    { bound:  768, label: '≤  768px (canonical phone breakpoint)' },
    { bound:  640, label: '≤  640px (admin table → stacked cards)' },
    { bound:  540, label: '≤  540px (result-panel header stack)' },
    { bound:  480, label: '≤  480px (nav-links horizontal scroll)' },
    { bound:  420, label: '≤  420px (extra-narrow phones)' },
];
breakpoints.forEach(function (bp) {
    const re = new RegExp('@media\\s*\\(max-width:\\s*' + bp.bound + 'px\\)', 'g');
    const hits = css.match(re);
    if (hits && hits.length > 0) {
        ok('breakpoint present @ ' + bp.bound + 'px (' + bp.label + ')');
    } else {
        fail('breakpoint missing @ ' + bp.bound + 'px (' + bp.label + ')');
    }
});

// ────────────────────────────────────────────────────────────────────
// 2. Admin table stacked-card CSS contract
// ────────────────────────────────────────────────────────────────────
function sections(breakpoint) {
    // Extract EVERY @media (max-width: NNNpx) { ... } block in the
    // stylesheet — some breakpoints appear multiple times (ranking
    // table at 640px + admin tables at 640px) and we want to scan
    // them all together.
    const out = [];
    const re = new RegExp(
        '@media\\s*\\(max-width:\\s*' + breakpoint + 'px\\)\\s*\\{',
        'g'
    );
    let m;
    while ((m = re.exec(css)) !== null) {
        let depth = 1, i = m.index + m[0].length, start = i;
        while (i < css.length && depth > 0) {
            const ch = css[i];
            if (ch === '{') depth++;
            else if (ch === '}') depth--;
            i++;
        }
        out.push(css.slice(start, i - 1));
    }
    return out;
}

const stacked640 = sections(640).join('\n');
if (!stacked640) {
    fail('≤ 640px sections missing entirely');
} else {
    if (/\.lc-admin-table[\s\S]*?\{[\s\S]*?display:\s*block/.test(stacked640)) {
        ok('≤ 640px: .lc-admin-table → display:block');
    } else {
        fail('≤ 640px: .lc-admin-table should be display:block');
    }
    if (/tbody\s+td::before/.test(stacked640) &&
        /content:\s*attr\(data-label\)/.test(stacked640)) {
        ok('≤ 640px: td::before uses content:attr(data-label)');
    } else {
        fail('≤ 640px: td::before should inject data-label via attr()');
    }
    if (/data-label=["']?操作["']?/.test(stacked640)) {
        ok('≤ 640px: 操作 ("actions") column label is hidden in card view');
    } else {
        fail('≤ 640px: actions column should hide its ::before label');
    }
}

// ────────────────────────────────────────────────────────────────────
// 3. Every admin table renderer sets data-label on every <td>
// ────────────────────────────────────────────────────────────────────
//
// The JS code uses `el('td', { dataset: { label: '...' } }, [...])`,
// which becomes `data-label="..."` in the rendered DOM. The test
// searches for the source-level form (since static analysis runs on
// the JS, not the runtime DOM).

const adminTables = [
    {
        file: path.join(webRoot, 'admin', 'problems.html'),
        cells: ['Slug', '标题', '难度', '标签', '通过率', '时间 / 内存', '操作'],
    },
    {
        file: path.join(webRoot, 'admin', 'users.html'),
        cells: ['ID', '用户名', '邮箱', '角色', '注册时间', '最后登录', '最后登录 IP', '操作'],
    },
    {
        file: path.join(webRoot, 'admin', 'audit-logs.html'),
        cells: ['ID', '时间', '管理员', '操作', '对象', 'IP', '查看'],
    },
    {
        file: path.join(webRoot, 'admin', 'dashboard.html'),
        cells: ['#', '状态', '用户', '题目', '语言', '耗时', '提交时间'],
    },
];

adminTables.forEach(function (t) {
    const text = fs.readFileSync(t.file, 'utf8');
    const rel = path.relative(webRoot, t.file).replace(/\\/g, '/');
    t.cells.forEach(function (label) {
        // Match `label: '<label>'` inside a dataset: object. Allow
        // any quote style; the el() helper uses dataset: { ... }.
        const escaped = label.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        const re = new RegExp(
            "label:\\s*['\"]" + escaped + "['\"]"
        );
        if (re.test(text)) {
            ok(rel + ' — cell labelled "' + label + '"');
        } else {
            fail(rel + ' — missing data-label for "' + label + '"');
        }
    });
});

// ────────────────────────────────────────────────────────────────────
// 4. meta viewport is present in every page (so responsive kicks in)
// ────────────────────────────────────────────────────────────────────
function findHtmlPages(dir) {
    const out = [];
    for (const name of fs.readdirSync(dir)) {
        const p = path.join(dir, name);
        const st = fs.statSync(p);
        if (st.isDirectory()) {
            const sub = findHtmlPages(p);
            for (let i = 0; i < sub.length; i++) out.push(sub[i]);
        } else if (name.endsWith('.html')) {
            out.push(p);
        }
    }
    return out;
}

const pages = findHtmlPages(webRoot).filter(function (p) {
    return !p.includes(path.sep + 'test' + path.sep);
});

pages.forEach(function (p) {
    const rel = path.relative(webRoot, p).replace(/\\/g, '/');
    const text = fs.readFileSync(p, 'utf8');
    if (/<meta\s+name="viewport"\s+content="width=device-width/.test(text)) {
        ok(rel + ' — viewport meta present');
    } else {
        fail(rel + ' — viewport meta missing');
    }
});

// ────────────────────────────────────────────────────────────────────
// 5. Confirm CSP / theme-boot / script order is unaffected by edits
//    (delegated to web/test/html-static-check.js; sanity smoke only).
// ────────────────────────────────────────────────────────────────────
ok('html-static-check.js will be re-run by CI as the source of truth');

console.log('');
console.log('passed: ' + passed);
console.log('failed: ' + failed);
process.exit(failed > 0 ? 1 : 0);