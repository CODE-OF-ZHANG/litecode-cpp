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
    var CSP_VALUE =
        "default-src 'self'; " +
        "script-src 'self' https://cdn.jsdelivr.net; " +
        "style-src 'self' 'unsafe-inline'; " +
        "img-src 'self' data:; " +
        "font-src 'self'; " +
        "connect-src 'self'; " +
        "object-src 'none'; " +
        "base-uri 'self'; " +
        "form-action 'self'; " +
        "frame-ancestors 'none'";

    // CDN scripts — the only scripts the SPA loads off-site. Pin
    // version + integrity so a compromised CDN or transparent proxy
    // can't swap the bytes. Both sha384 values were generated from
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

    // ────────────────────────────────────────────────────────────────────
    //  Module surface
    // ────────────────────────────────────────────────────────────────────

    var ns = {
        CSP_VALUE:    CSP_VALUE,
        SCRIPTS:      SCRIPTS,
        findMetaCsp:  findMetaCsp,
        assertMetaMatchesCanonical: assertMetaMatchesCanonical,
        makeScript:   makeScript,
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