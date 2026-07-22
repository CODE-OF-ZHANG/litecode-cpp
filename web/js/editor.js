// SPDX-License-Identifier: MIT
//
// web/js/editor.js — CodeMirror 5 lazy-load + IIFE 抽象层
//
// Phase 4 ★ frontend polish / v1.3.2 (template field): per-problem
// code template (DB column `problems.template_`, JSON key `template`)
// is consumed here. The public detail endpoint includes the field;
// this module picks it up if present and falls back to the built-in
// C++/C skeleton below — same fallback for an absent or empty value.
//
// SPEC §6.3 + A34 — single source of truth for the editor behavior.
// Mirrors `web/js/{api,markdown,csp,app}.js` style: IIFE wrapper +
// `litecode.editor.*` namespace + `defer`-loaded script tag.
//
// What this module owns:
//   - DEFAULT_TEMPLATES    — built-in C++/C skeletons (fallback only;
//                            per-problem template from API wins)
//   - LANG_MODE / LANG_LABEL — CodeMirror mode + UI label per language
//   - loadCodemirror()     — lazy-load the pinned SRI codemirror 5
//                            bundle + clike mode via litecode.csp
//   - mountEditor(code, lang) — instance lifecycle (CodeMirror or
//                            plain-textarea fallback)
//   - getEditorValue / setEditorValue / onEditorChange — the public
//                            surface used by problem.html
//   - templateForLang(lang) — returns the fallback skeleton (or ''
//                            for unknown langs) so problem.html can
//                            decide between problem.template vs
//                            skeleton without a try/catch.
//
// Deps (loaded first via defer tags in problem.html):
//   - litecode.csp.makeStylesheet / makeScript — SRI-pinned loaders
//   - window.CodeMirror — once loadCodemirror's promise resolves
//
// Page consumers (problem.html IIFE):
//   - litecode.editor.loadCodemirror() on boot
//   - litecode.editor.templateForLang(lang) to pick the initial value
//   - litecode.editor.mountEditor(value, lang) once the bundle is up
//   - litecode.editor.getEditorValue() on submit
//
// CSP: no extra origins. SRI pinning lives entirely in csp.js.

(function (ns) {
    'use strict';

    // ── Constants ──────────────────────────────────────────────

    var LANG_MODE = {
        c:   'text/x-c',
        cpp: 'text/x-c++src',
    };
    var LANG_LABEL = {
        c:   'C',
        cpp: 'C++',
    };

    // Fallback skeleton — used only when the API's per-problem
    // `template` is absent OR empty. The PHP-side DB column is
    // MEDIUMTEXT NULL; HTTP transports it as JSON `""` (or omits
    // the key on older clients); both are detected by the
    // consumer as "fall back". Keep these compact — they're shown
    // verbatim in the CodeMirror editor.
    var DEFAULT_TEMPLATES = {
        cpp: [
            '#include <bits/stdc++.h>',
            'using namespace std;',
            '',
            'int main() {',
            '    ios::sync_with_stdio(false);',
            '    cin.tie(nullptr);',
            '    // TODO: 实现题目逻辑',
            '    return 0;',
            '}',
            '',
        ].join('\n'),
        c: [
            '#include <stdio.h>',
            '#include <stdlib.h>',
            '',
            'int main(void) {',
            '    // TODO: 实现题目逻辑',
            '    return 0;',
            '}',
            '',
        ].join('\n'),
    };

    // Closured state — no `var editor = {}` leak. The exported
    // get / set / on* functions are the only public touch-points.
    var state = {
        cm: null,             // CodeMirror instance, or null
        textarea: null,       // <textarea> fallback
        ready: false,         // mountEditor has run
    };

    // ── CodeMirror bootstrap ───────────────────────────────────
    // Lazy-load the SRI-pinned CM bundle + clike mode; returns a
    // promise that resolves to true on success, false on fallback.
    function loadCodemirror() {
        if (!window.litecode || !litecode.csp) {
            console.warn('[editor] csp.js not loaded; falling back to <textarea>');
            return Promise.resolve(false);
        }
        var cssSpec  = litecode.csp.STYLESHEETS && litecode.csp.STYLESHEETS.codemirror;
        var jsSpec   = litecode.csp.SCRIPTS     && litecode.csp.SCRIPTS.codemirror;
        var modeSpec = litecode.csp.SCRIPTS     && litecode.csp.SCRIPTS['codemirror-clike'];

        function loadStyle(spec) {
            return new Promise(function (resolve, reject) {
                if (!spec) { reject(new Error('no stylesheet spec')); return; }
                var existing = document.querySelector(
                    'link[data-lc-cm-css="' + spec.url + '"]'
                );
                if (existing) { resolve(existing); return; }
                var l = litecode.csp.makeStylesheet(spec);
                l.dataset.lcCmCss = spec.url;
                l.addEventListener('load',  function () { resolve(l); });
                l.addEventListener('error', function () { reject(new Error('css load failed')); });
                document.head.appendChild(l);
            });
        }
        function loadJs(spec) {
            return new Promise(function (resolve, reject) {
                if (!spec) { reject(new Error('no script spec')); return; }
                var existing = document.querySelector(
                    'script[data-lc-cm-src="' + spec.url + '"]'
                );
                if (existing && existing.dataset.loaded === '1') { resolve(); return; }
                if (existing) {
                    existing.addEventListener('load',  function () { resolve(); });
                    existing.addEventListener('error', function () { reject(new Error('js load failed')); });
                    return;
                }
                var s = litecode.csp.makeScript(spec);
                s.dataset.lcCmSrc = spec.url;
                s.addEventListener('load',  function () { s.dataset.loaded = '1'; resolve(); });
                s.addEventListener('error', function () { reject(new Error('js load failed')); });
                document.head.appendChild(s);
            });
        }

        return loadStyle(cssSpec)
            .then(function () { return loadJs(jsSpec); })
            .then(function () { return loadJs(modeSpec); })
            .then(function () { return true; })
            .catch(function (err) {
                console.warn('[editor] CodeMirror load failed, using <textarea> fallback', err);
                return false;
            });
    }

    // ── Editor instance lifecycle ──────────────────────────────
    function mountEditor(initialCode, lang) {
        state.textarea = document.getElementById('code-textarea');
        if (window.CodeMirror && state.textarea) {
            state.cm = window.CodeMirror.fromTextArea(state.textarea, {
                mode: LANG_MODE[lang] || LANG_MODE.cpp,
                lineNumbers: true,
                indentUnit: 4,
                tabSize: 4,
                indentWithTabs: false,
                smartIndent: true,
                lineWrapping: true,
                matchBrackets: true,
                autoCloseBrackets: true,
                styleActiveLine: true,
                theme: document.documentElement.classList.contains('dark')
                    ? 'monokai' : 'default',
            });
            // Re-sync theme on dark-mode toggle. We monkey-patch the
            // exposed toggle so the saved reference is held by our
            // own closure variable, not the litecode namespace.
            var origToggle = window.litecode && litecode.theme && litecode.theme.toggle;
            if (origToggle) {
                litecode.theme.toggle = function () {
                    origToggle();
                    if (state.cm) {
                        state.cm.setOption('theme',
                            document.documentElement.classList.contains('dark')
                                ? 'monokai' : 'default');
                    }
                };
            }
            state.cm.setValue(initialCode || '');
            state.ready = true;
        } else {
            // Plain-textarea fallback: tab/shift-tab indentation +
            // no other niceties. Keeps the editor usable when the
            // CDN bundle fails (offline, CSP block, etc.).
            state.textarea.value = initialCode || '';
            state.textarea.addEventListener('keydown', function (e) {
                if (e.key !== 'Tab') return;
                e.preventDefault();
                var ta = e.target;
                var start = ta.selectionStart, end = ta.selectionEnd;
                var v = ta.value;
                if (e.shiftKey) {
                    var lineStart = v.lastIndexOf('\n', start - 1) + 1;
                    var line = v.slice(lineStart, end);
                    var dedented = line.replace(/^( {1,4})/gm, '');
                    ta.value = v.slice(0, lineStart) + dedented + v.slice(end);
                    ta.selectionStart = lineStart;
                    ta.selectionEnd = lineStart + dedented.length;
                } else {
                    ta.value = v.slice(0, start) + '    ' + v.slice(end);
                    ta.selectionStart = ta.selectionEnd = start + 4;
                }
            });
            state.ready = true;
        }
    }

    // ── Public editor API ──────────────────────────────────────
    function getEditorValue() {
        if (state.cm)        return state.cm.getValue();
        if (state.textarea)  return state.textarea.value;
        return '';
    }
    function setEditorValue(v) {
        if (state.cm)        state.cm.setValue(v || '');
        else if (state.textarea) state.textarea.value = v || '';
    }
    // setMode — switch the CodeMirror mode when the language picker
    // changes (no-op on the textarea fallback — syntax highlighting
    // only matters in the CodeMirror path). No-op if mountEditor
    // hasn't run yet (state.cm is still null).
    function setMode(lang) {
        if (state.cm) {
            state.cm.setOption('mode', LANG_MODE[lang] || LANG_MODE.cpp);
        }
    }
    function onEditorChange(cb) {
        if (state.cm)        state.cm.on('change', cb);
        else if (state.textarea) state.textarea.addEventListener('input', cb);
    }

    // ── Per-problem template picker ───────────────────────────
    // Returns the fallback skeleton for `lang` (or '' for unknown
    // lang). The caller (problem.html) is expected to prefer the
    // API-supplied per-problem template over this fallback.
    function templateForLang(lang) {
        return DEFAULT_TEMPLATES[lang] || '';
    }

    ns.editor = {
        loadCodemirror:    loadCodemirror,
        mountEditor:       mountEditor,
        getEditorValue:    getEditorValue,
        setEditorValue:    setEditorValue,
        setMode:           setMode,
        onEditorChange:    onEditorChange,
        templateForLang:   templateForLang,

        // Constants — exported for the consumer (problem.html's
        // status badge / language picker) without re-declaring.
        DEFAULT_TEMPLATES: DEFAULT_TEMPLATES,
        LANG_MODE:         LANG_MODE,
        LANG_LABEL:        LANG_LABEL,
    };
}(window.litecode = window.litecode || {}));
