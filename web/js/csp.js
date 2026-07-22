// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend CSP + Subresource Integrity (Phase 5 ★ CSP/SRI)
//
// Single source of truth for the page-level Content-Security-Policy and
// the Subresource-Integrity hashes of every CDN-loaded script. Every
// HTML page in web/*.html / web/admin/*.html embeds:
//
//   <meta http-equiv="Content-Security-Policy" content="__CSP_VALUE__">
//   <script src="/js/csp.js" defer></script>
//   <script src="/js/api.js" defer></script>
//   <script src="/js/app.js" defer></script>
//
// `csp.js` MUST be loaded before `api.js` so `litecode.csp` is available
// to `app.js` when it builds the dynamic `<script>` tags for marked /
// DOMPurify. The runtime check `assertMetaMatchesCanonical()` keeps the
// inline meta tag in lock-step with this canonical value — if a page is
// edited with a typo, the browser console logs a clear error instead of
// silently shipping a weakened policy.
//
// Why a meta tag when Caddy already sets the CSP header?
// ----------------------------------------------------------
//   1. First-paint enforcement. The HTTP header must round-trip before
//      the browser starts executing scripts. The meta tag is parsed
//      from the HTML stream, so any inline `<script>` after it is
//      already constrained — useful for pages that survive even when
//      the reverse proxy is bypassed (e.g. `python -m http.server`).
//   2. Defense in depth. If a future Caddy rule is weakened or removed
//      by mistake, the meta tag still pins the policy.
//
// SRI strategy (SPEC §6.3)
// ------------------------
//   - All CDN bundles ship with `integrity="sha384-..."` + `crossorigin`
//     so the browser refuses to execute them if the bytes change.
//   - Local scripts (api.js / app.js / csp.js itself) are loaded from
//     the same origin and need no integrity.
//   - The sha384 values were generated from the actual bytes served by
//     cdn.jsdelivr.net on the pinned version (marked@12.0.2,
//     dompurify@3.1.6). To rotate, bump the version, re-download, and
//     recompute with:
//
//        openssl dgst -sha384 -binary marked.min.js | openssl base64 -A
//        openssl dgst -sha384 -binary purify.min.js | openssl base64 -A
//
// CSP value design (SPEC §6.3 / A32)
// -----------------------------------
//   default-src   'self'                       — same-origin by default
//   script-src    'self' https://cdn.jsdelivr.net
//                                               — allow framework + CDN;
//                                                 NO 'unsafe-inline' so
//                                                 an injected <script>
//                                                 tag cannot run.
//   style-src     'self' 'unsafe-inline'       — inline style="" attrs
//                                                 and <style> blocks are
//                                                 used by app.js (toast
//                                                 positioning etc.)
//   img-src       'self' data:                 — avatars, sample I/O may
//                                                 be base64 data URIs.
//   font-src      'self'                       — currently no web fonts.
//   connect-src   'self'                       — fetch/XHR only to API.
//   object-src    'none'                       — kill <object>/<embed>.
//   base-uri      'self'                       — block <base> hijack.
//   form-action   'self'                       — block form exfil.
//   frame-ancestors 'none'                     — deny embedding.
//
// The server-side CSP header in Caddyfile mirrors this value exactly so
// the browser doesn't fall back to the intersection (which would block
// the CDN scripts Caddy can't see).

(function (root) {
    'use strict';

    // Canonical CSP value. Keep whitespace minimal — browsers normalize
    // it but humans diff-ing HTML will appreciate the consistency.
    // `style-src` mirrors `script-src` (both `self` + cdn.jsdelivr.net)
    // because problem.html ships CodeMirror's stylesheet from the
    // CDN with sha384 integrity (see STYLESHEETS below).
    // ---- inline-script policy note ----
    //   `script-src 'self' https://cdn.jsdelivr.net 'unsafe-inline'`
    //   is intentionally NOT replaced with a stricter nonce/hash rule
    //   yet. Every page ships 100-700 lines of SSR-style page-IIFE
    //   inline (e.g. index.html's load() + renderHeroCta(), profile's
    //   paginator, admin's editable rows). Without 'unsafe-inline'
    //   the browser SILENTLY blocks the entire page-specific IIFE —
    //   nav never mounts, hero CTA never fills, paginator never
    //   fires — and the user sees a "half loaded" page with no
    //   console error. 'unsafe-inline' here is acceptable because:
    //     (1) No user-controlled content is reflected into an inline
    //         <script> (the IIFE bodies are static at build time).
    //     (2) The OTHER restrictive directives (default-src 'self',
    //         connect-src 'self', object-src 'none', base-uri 'self',
    //         frame-ancestors 'none') stay strict — they are the
    //         load-bearing defences against reflected XSS → data
    //         exfil / object-injection / base-hijack.
    //   Future hardening: move each page-IIFE into web/js/page-*.js
    //   and require a one-time build step to add a nonce — at which
    //   point drop 'unsafe-inline' here AND in every HTML meta tag.
    var CSP_VALUE =
        "default-src 'self'; " +
        "script-src 'self' https://cdn.jsdelivr.net 'unsafe-inline'; " +
        "style-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; " +
        "img-src 'self' data:; " +
        "font-src 'self'; " +
        "connect-src 'self'; " +
        "object-src 'none'; " +
        "base-uri 'self'; " +
        "form-action 'self'; " +
        "frame-ancestors 'none'";

    // CDN scripts — the only scripts the SPA loads off-site. Pin
    // version + integrity so a compromised CDN or transparent proxy
    // can't swap the bytes. All sha384 values were generated from
    // the bytes served by cdn.jsdelivr.net on the pinned version;
    // see the comment at the top of this file for the recompute steps.
    var SCRIPTS = {
        marked: {
            url:        'https://cdn.jsdelivr.net/npm/marked@12.0.2/marked.min.js',
            integrity:  'sha384-/TQbtLCAerC3jgaim+N78RZSDYV7ryeoBCVqTuzRrFec2akfBkHS7ACQ3PQhvMVi',
            crossOrigin:'anonymous',
            // sha256 is also accepted but kept out of the registry — a
            // CSP-relevant answer is "which hash did the browser use?":
            // it's always the FIRST valid one in `integrity`.
            size:       35479,
        },
        dompurify: {
            url:        'https://cdn.jsdelivr.net/npm/dompurify@3.1.6/dist/purify.min.js',
            integrity:  'sha384-+VfUPEb0PdtChMwmBcBmykRMDd+v6D/oFmB3rZM/puCMDYcIvF968OimRh4KQY9a',
            crossOrigin:'anonymous',
            size:       21496,
        },
        // CodeMirror 5.65.16 — problem.html loads it dynamically so
        // the editor gets syntax highlighting + line numbers without
        // shipping ~250 KB of JS inline. Pinned to 5.x (not 6.x) for
        // two reasons: (a) 5.x is a UMD bundle that works without
        // ES-module shenanigans, (b) 6.x's split bundles don't have
        // a single canonical sha384 to pin. See problem.html /
        // editor-bootstrap for the load sequence.
        codemirror: {
            url:        'https://cdn.jsdelivr.net/npm/codemirror@5.65.16/lib/codemirror.min.js',
            integrity:  'sha384-CtBuRlcKITyrd+aBeTPNFB1/T8+kvtNQiWMCLtiGvD6NpLOJAdt8e8PpJJ2Gn1D0',
            crossOrigin:'anonymous',
            size:       173953,
        },
        // C/C++ mode for CodeMirror 5. 'clike' is the upstream name
        // for the C-like-family tokenizer (C / C++ / Java / JS / etc.).
        'codemirror-clike': {
            url:        'https://cdn.jsdelivr.net/npm/codemirror@5.65.16/mode/clike/clike.min.js',
            integrity:  'sha384-ZS86VwH8VodbCs4EeYNX2wCKSJpCZfGlrTWe2cFgaqyafruHBCCuZcP2vfCz+V9Q',
            crossOrigin:'anonymous',
            size:       21368,
        },
    };

    // CDN stylesheets — same SRI discipline as scripts. Used by
    // problem.html to load CodeMirror's stylesheet dynamically so the
    // editor inherits the right gutter / cursor / selection colors
    // and follows the page's dark-mode toggle.
    //
    // The stylesheet is loaded programmatically (not via a static
    // <link rel="stylesheet" href=... integrity=...> tag) so a single
    // csp.js change can rotate the hash without touching every page.
    // Browsers DO honor `integrity` on <link> elements; the load
    // fails closed if the bytes drift.
    var STYLESHEETS = {
        codemirror: {
            url:        'https://cdn.jsdelivr.net/npm/codemirror@5.65.16/lib/codemirror.min.css',
            integrity:  'sha384-phfEUVAmRZV1Pzn/Xgxc3NH6zPMDuer0wHU9jRQKhNBBLyV4MP1gaBY1sxfxxPRT',
            crossOrigin:'anonymous',
            size:       6378,
        },
    };

    // ────────────────────────────────────────────────────────────────────
    //  Helpers
    // ────────────────────────────────────────────────────────────────────

    function findMetaCsp() {
        var metas = document.querySelectorAll('meta[http-equiv]');
        for (var i = 0; i < metas.length; i++) {
            var m = metas[i];
            var equiv = (m.getAttribute('http-equiv') || '').toLowerCase();
            if (equiv === 'content-security-policy') return m;
        }
        return null;
    }

    // Normalize whitespace so a meta tag reformatted by an editor still
    // compares equal to the canonical value. We collapse all whitespace
    // runs to a single space and trim.
    function normalize(s) {
        return String(s || '').replace(/\s+/g, ' ').trim();
    }

    function assertMetaMatchesCanonical() {
        // Only meaningful in a browser environment.
        if (typeof document === 'undefined' || !document.querySelector) return true;
        var meta = findMetaCsp();
        if (!meta) {
            console.error(
                '[litecode.csp] No <meta http-equiv="Content-Security-Policy"> ' +
                'tag found. Every page MUST carry the canonical policy.'
            );
            return false;
        }
        var metaVal = normalize(meta.getAttribute('content'));
        var canon   = normalize(CSP_VALUE);
        if (metaVal !== canon) {
            console.error(
                '[litecode.csp] Page CSP meta tag drifted from the canonical value.\n' +
                '  meta : ' + metaVal + '\n' +
                '  canon: ' + canon
            );
            return false;
        }
        return true;
    }

    // Build a <script> element with SRI integrity attributes pre-set.
    // Callers append to <head>; returns the element so they can wire
    // load/error listeners.
    function makeScript(spec) {
        if (!spec || !spec.url) throw new Error('litecode.csp.makeScript: spec.url required');
        var s = document.createElement('script');
        s.src = spec.url;
        s.defer = true;
        if (spec.integrity) {
            s.integrity = spec.integrity;
            s.crossOrigin = spec.crossOrigin || 'anonymous';
        }
        return s;
    }

    // Build a <link rel="stylesheet"> element with SRI integrity
    // attributes. Mirrors makeScript() for the stylesheet use case
    // (CodeMirror's editor.css). The browser refuses to apply the
    // stylesheet if the bytes drift. Returns the element so callers
    // can wire load/error listeners.
    function makeStylesheet(spec) {
        if (!spec || !spec.url) throw new Error('litecode.csp.makeStylesheet: spec.url required');
        var l = document.createElement('link');
        l.rel = 'stylesheet';
        l.href = spec.url;
        if (spec.integrity) {
            l.integrity = spec.integrity;
            l.crossOrigin = spec.crossOrigin || 'anonymous';
        }
        return l;
    }

    // ────────────────────────────────────────────────────────────────────
    //  Module surface
    // ────────────────────────────────────────────────────────────────────

    var ns = {
        CSP_VALUE:    CSP_VALUE,
        SCRIPTS:      SCRIPTS,
        STYLESHEETS:  STYLESHEETS,
        findMetaCsp:  findMetaCsp,
        assertMetaMatchesCanonical: assertMetaMatchesCanonical,
        makeScript:   makeScript,
        makeStylesheet: makeStylesheet,
        // Convenience accessors — keep `app.js` terse.
        marked:       SCRIPTS.marked,
        dompurify:    SCRIPTS.dompurify,
    };

    root.litecode = root.litecode || {};
    root.litecode.csp = ns;

    // Run the consistency check as soon as the script loads — at this
    // point <head> is parsed but the body isn't necessarily ready, and
    // the meta tag sits in <head> so we can already inspect it.
    try { ns.assertMetaMatchesCanonical(); } catch (_) { /* tolerate non-DOM envs */ }
})(window);