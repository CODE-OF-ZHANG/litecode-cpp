// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Theme no-flash bootstrap (Phase 5 ★ 深色模式 / SPEC §6.3 / A34)
//
// Synchronous script loaded in <head> BEFORE the stylesheet. Its only
// job is to read `litecode:theme` from localStorage and reflect that
// choice on `<html>` (via the `.dark` class + `data-theme-chosen`
// attribute) so the very first paint is already in the user's chosen
// mode. Without this script a dark-mode user would see ~50ms of light
// theme before app.js boots and applies the class.
//
// Why a separate synchronous script (instead of inlining)?
//   1. CSP policy is `script-src 'self' https://cdn.jsdelivr.net` — no
//      `'unsafe-inline'`. Inline <script> blocks would be rejected.
//   2. The script is tiny (~25 lines) so a sync fetch in <head> costs
//      only a few ms and we preserve the strict CSP.
//   3. `defer` would push execution past the first paint — exactly
//      what we're trying to avoid.
//
// Wiring contract (MUST be loaded first script in <head>):
//
//     <head>
//       <meta http-equiv="Content-Security-Policy" ...>
//       <title>...</title>
//       <script src="/js/theme-boot.js"></script>     ← this file, sync
//       <link rel="stylesheet" href="/css/style.css">
//       <script src="/js/csp.js" defer></script>
//       <script src="/js/api.js" defer></script>
//       <script src="/js/app.js" defer></script>
//     </head>
//
// State machine (persisted in localStorage['litecode:theme']):
//
//   'dark'   → add `.dark` class on <html>, set `data-theme-chosen`
//   'light'  → remove `.dark` class,           set `data-theme-chosen`
//   null     → leave attribute absent → @media (prefers-color-scheme)
//                                         can still apply dark colors
//
// `data-theme-chosen` is the signal that the user has made an explicit
// choice. style.css uses `html:not([data-theme-chosen])` to gate the
// system-preference fallback rule so an explicit choice wins.
//
// The HTML pages MUST NOT hard-code `data-theme-chosen` on <html> —
// doing so would block the system-preference path forever (the CSS
// would always see "user has chosen" and never apply @media dark).
// Leave the attribute absent and let this script set it conditionally.
//
// Touching the DOM: only `document.documentElement.classList` and
// `setAttribute` are used, both available the moment <html> starts
// parsing. We don't query or wait for anything else.

(function () {
    'use strict';

    var STORAGE_KEY = 'litecode:theme';

    // localStorage may be unavailable (private mode, sandboxed iframe
    // with quota 0, etc.). Read in a try/catch and bail out — the
    // default light theme is the safe fallback.
    var stored;
    try { stored = window.localStorage.getItem(STORAGE_KEY); }
    catch (_) { return; }

    if (stored !== 'dark' && stored !== 'light') return;

    var html = document.documentElement;
    if (stored === 'dark') html.classList.add('dark');
    // 'light' is the default — no class change needed.
    html.setAttribute('data-theme-chosen', '1');
})();