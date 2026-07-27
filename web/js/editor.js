// SPDX-License-Identifier: MIT
//
// web/js/editor.js — Ace editor lazy-load + IIFE abstraction layer
//
// v1.3.3.9 ★ editor migration: CodeMirror 5 → Ace 1.32.7. Why:
//   (1) Ace's `ace/mode/c_cpp` tokenizer is C/C++ specific (not the
//       clike "C-like-family" fallback that mis-coloured some C++17
//       tokens on dark theme).
//   (2) Ace's editor.setTheme() / session.setMode() API hooks into
//       our theme-boot.js dark-mode flip directly — no manual
//       re-paint, the gutter and selection recolor in real time.
//   (3) Ace 1.32.x is a single ~436 KB UMD bundle (no ES module
//       shenanigans), same SRI-friendly model as CodeMirror 5.x.
//
// Phase 4 ★ frontend polish / v1.2.50 — per-problem code template
// (DB column `problems.template_`, JSON key `template`) is consumed
// here. The public detail endpoint includes the field; this module
// picks it up if present and falls back to the built-in C++/C
// skeleton below — same fallback for an absent or empty value.
//
// SPEC §6.3 + A34 — single source of truth for the editor behavior.
// Mirrors `web/js/{api,markdown,csp,app}.js` style: IIFE wrapper +
// `litecode.editor.*` namespace + `defer`-loaded script tag.
//
// What this module owns:
//   - DEFAULT_TEMPLATES    — built-in C++/C skeletons (fallback only;
//                            per-problem template from API wins)
//   - LANG_MODE / LANG_LABEL — Ace mode id + UI label per language
//   - loadEditor()         — lazy-load the pinned SRI ace bundle +
//                            C/C++ mode via litecode.csp
//   - mountEditor(code, lang) — instance lifecycle (Ace or
//                            plain-textarea fallback)
//   - getEditorValue / setEditorValue / onEditorChange / setMode
//                            — the public surface used by problem.html
//   - templateForLang(lang) — returns the fallback skeleton (or ''
//                            for unknown langs) so problem.html can
//                            decide between problem.template vs
//                            skeleton without a try/catch.
//
// Deps (loaded first via defer tags in problem.html):
//   - litecode.csp.makeScript — SRI-pinned loaders
//   - window.ace            — once loadEditor's promise resolves
//
// Page consumers (problem.html IIFE):
//   - litecode.editor.loadEditor() on boot
//   - litecode.editor.templateForLang(lang) to pick the initial value
//   - litecode.editor.mountEditor(value, lang) once the bundle is up
//   - litecode.editor.getEditorValue() on submit
//
// CSP: no extra origins. SRI pinning lives entirely in csp.js.

(function (ns) {
    'use strict';

    // ── Constants ──────────────────────────────────────────────

    // Ace mode id for the C-like-family tokenizer. Ace's c_cpp mode
    // handles both `c` and `cpp` internally — we pass the same id
    // for both langs; the syntax state machine switches based on
    // keyword recognition.
    var ACE_MODE_C_CPP = 'ace/mode/c_cpp';

    var LANG_MODE = {
        c:   ACE_MODE_C_CPP,
        cpp: ACE_MODE_C_CPP,
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
    // verbatim in the editor.
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
            '    return 1;',
            '}',
            '',
        ].join('\n'),
    };

    // Closured state — no `var editor = {}` leak. The exported
    // get / set / on* functions are the only public touch-points.
    var state = {
        editor:   null,    // Ace Editor instance, or null
        session:  null,    // Ace EditSession, cached for setMode
        textarea: null,    // <textarea> fallback
        ready:    false,   // mountEditor has run
    };

    // ── Ace bootstrap ──────────────────────────────────────────
    // Lazy-load the SRI-pinned Ace bundle + C/C++ mode; returns a
    // promise that resolves to true on success, false on fallback.
    //
    // Why no CSS load? Ace injects its own stylesheet when
    // `ace.edit()` is called the first time; we override the
    // theme colours via inline CSS variables in style.css
    // (`.ace_editor` rules) so the page's dark-mode toggle flows
    // through without reloading the Ace stylesheet.
    function loadEditor() {
        if (!window.litecode || !litecode.csp) {
            console.warn('[editor] csp.js not loaded; falling back to <textarea>');
            return Promise.resolve(false);
        }
        var aceSpec  = litecode.csp.SCRIPTS && litecode.csp.SCRIPTS.ace;
        var modeSpec = litecode.csp.SCRIPTS && litecode.csp.SCRIPTS['ace-mode-c_cpp'];

        function loadJs(spec) {
            return new Promise(function (resolve, reject) {
                if (!spec) { reject(new Error('no script spec')); return; }
                var existing = document.querySelector(
                    'script[data-lc-ace-src="' + spec.url + '"]'
                );
                if (existing && existing.dataset.loaded === '1') { resolve(); return; }
                if (existing) {
                    existing.addEventListener('load',  function () { resolve(); });
                    existing.addEventListener('error', function () { reject(new Error('js load failed')); });
                    return;
                }
                var s = litecode.csp.makeScript(spec);
                s.dataset.lcAceSrc = spec.url;
                s.addEventListener('load',  function () { s.dataset.loaded = '1'; resolve(); });
                s.addEventListener('error', function () { reject(new Error('js load failed')); });
                document.head.appendChild(s);
            });
        }

        return loadJs(aceSpec)
            .then(function () { return loadJs(modeSpec); })
            .then(function () { return true; })
            .catch(function (err) {
                console.warn('[editor] Ace load failed, using <textarea> fallback', err);
                return false;
            });
    }

    // ── Theme resolution ───────────────────────────────────────
    // Ace ships its own theme files (e.g. textmate / monokai /
    // tomorrow_night). We don't pull any extra theme bundle — both
    // light and dark modes use Ace's built-in `github` /
    // `tomorrow_night` themes (kept intentionally neutral so the
    // page-level `style.css` palette overrides dominate the visual).
    //
    // v1.3.4 PR 4 — host class `lc-ace--dark` /
    // `lc-ace--leetcode-light` is added next to `.lc-editor-shell`
    // so `web/css/style.css` can override individual token colours
    // without re-loading an Ace stylesheet (one HTTP hop, same
    // crispness, theme-boot.js dark flip becomes a class swap).
    //
    // The theme id is computed off the document's `.dark` class so
    // theme-boot.js flips take effect immediately.
    //
    // v1.3.4 PR 5 follow-up — body.lc-cyber is a DECORATION marker
    // (the grid strip / terminal / podium chrome), not a dark-mode
    // signal. The original code treated it as one, which meant pages
    // like problem.html (always <body class="lc-cyber">) locked the
    // editor to tomorrow_night regardless of the actual theme toggle.
    // User-visible symptom: in light mode the editor rendered Ace's
    // dark syntax tokens on top of the page's light editor-shell bg,
    // making keywords / strings / numbers essentially invisible.
    // We now check ONLY the html.dark class — that's the single
    // source of truth for "is dark mode on" set by theme-boot.js.
    function isDarkMode() {
        return document.documentElement.classList.contains('dark');
    }
    function currentAceTheme() {
        return isDarkMode() ? 'ace/theme/tomorrow_night'
                            : 'ace/theme/github';
    }
    function applyHostThemeClass(host) {
        if (!host) return;
        host.classList.remove('lc-ace--dark', 'lc-ace--leetcode-light');
        host.classList.add(isDarkMode() ? 'lc-ace--dark'
                                        : 'lc-ace--leetcode-light');
    }

    // ── Editor instance lifecycle ──────────────────────────────
    function mountEditor(initialCode, lang) {
        state.textarea = document.getElementById('code-textarea');
        if (window.ace && state.textarea) {
            // The host div (`#editor-shell`) wraps the textarea. Ace
            // wants a div to attach to, so we either reuse the
            // textarea's parent or fall through. We pick the parent
            // because the CSS for `.lc-editor-shell` already
            // constrains height + min-height there.
            var host = state.textarea.parentNode;
            // Hide the underlying textarea — Ace paints over the
            // same coordinates. We keep it in the DOM (not remove)
            // so screen readers without JS still see the field.
            state.textarea.style.display = 'none';

            state.editor = window.ace.edit(host, {
                value:           initialCode || '',
                mode:            LANG_MODE[lang] || ACE_MODE_C_CPP,
                theme:           currentAceTheme(),
                wrap:            true,
                tabSize:         4,
                useSoftTabs:     true,
                showPrintMargin: false,
                fontSize:        '14px',
                // Ace ships a small built-in C/C++ worker for
                // auto-completion; opt-in for nicer UX without
                // paying for the full language server.
                enableBasicAutocompletion: true,
                enableSnippets:           false,
                enableLiveAutocompletion:  false,
            });
            state.session = state.editor.getSession();
            // v1.3.4 PR 4 — paint the host class so the page-level
            // CSS palette overrides the neutral Ace theme colours.
            // Toggled in lockstep with theme-boot.js flips.
            applyHostThemeClass(host);

            // Re-sync theme on dark-mode toggle. We monkey-patch
            // the exposed toggle so the saved reference is held
            // by our own closure variable, not the litecode
            // namespace — same pattern the old CodeMirror code
            // used.
            var origToggle = window.litecode && litecode.theme && litecode.theme.toggle;
            if (origToggle) {
                litecode.theme.toggle = function () {
                    origToggle();
                    if (state.editor) {
                        state.editor.setTheme(currentAceTheme());
                        // v1.3.4 PR 4 — also swap host class so the
                        // CSS palette stays in lockstep with Ace.
                        applyHostThemeClass(host);
                    }
                };
            }
            // Set explicit min-height on the host so Ace can
            // compute its layout on the first paint; the CSS
            // rule already caps it but Ace needs an explicit
            // pixel hint to size its gutter.
            state.editor.renderer.setOption('showLineNumbers', true);

            state.ready = true;
        } else {
            // Plain-textarea fallback: tab/shift-tab indentation +
            // no other niceties. Keeps the editor usable when the
            // bundle fails (offline, CSP block, etc.).
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
        if (state.editor)   return state.editor.getValue();
        if (state.textarea) return state.textarea.value;
        return '';
    }
    function setEditorValue(v) {
        if (state.editor) {
            // Setting value through the model keeps undo history
            // — same as CodeMirror's `setValue` from the user's
            // perspective (the page-side expectation is "replace
            // the buffer", which Ace does with `setValue`).
            state.editor.session.setValue(v || '');
        } else if (state.textarea) {
            state.textarea.value = v || '';
        }
    }
    function setMode(lang) {
        // Ace's C/C++ mode covers both `c` and `cpp`; the mode id
        // is the same and Ace's syntax state machine branches
        // internally. No-op on the textarea fallback.
        if (state.editor) {
            state.editor.session.setMode(LANG_MODE[lang] || ACE_MODE_C_CPP);
        }
    }
    function onEditorChange(cb) {
        if (state.editor) {
            state.editor.session.on('change', cb);
        } else if (state.textarea) {
            state.textarea.addEventListener('input', cb);
        }
    }

    // ── Per-problem template picker ───────────────────────────
    // Returns the fallback skeleton for `lang` (or '' for unknown
    // lang). The caller (problem.html) is expected to prefer the
    // API-supplied per-problem template over this fallback.
    function templateForLang(lang) {
        return DEFAULT_TEMPLATES[lang] || '';
    }

    ns.editor = {
        loadEditor:        loadEditor,
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