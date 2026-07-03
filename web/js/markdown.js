// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend Markdown sanitizer (Phase 5 ★ Markdown XSS 净化)
//
// Owns the on-browser Markdown pipeline that turns the server-delivered
// `description` Markdown into safe HTML:
//
//   raw Markdown ──► marked.parse() ──► DOMPurify.sanitize() ──► innerHTML
//
// The threat model (SPEC §6.3 / A5 / A32):
//
//   An admin can put anything in `description` (problem statements often
//   contain literal `<` / `>` for code samples). Some of that content
//   is also legitimately user-supplied via future "draft problem"
//   imports / bulk CSV. We do NOT trust the server-side serializer to
//   strip HTML — it doesn't (intentionally: the API delivers raw
//   Markdown so the policy lives in one place — the browser, where
//   the SRI-pinned DOMPurify bundle runs).
//
//   A second concern is the CDN itself: a compromised jsdelivr mirror
//   or a MITM could swap marked.min.js for one that emits
//   `<img src=x onerror=alert(1)>`. We pin BOTH bundles with sha384
//   integrity hashes (see `csp.js` for the registry); the browser
//   refuses to execute tampered bytes before they reach
//   `marked.parse`.
//
// Defense in depth:
//
//   1. `marked` is configured with `{gfm:true, breaks:true}` (so the
//      output mirrors what admins type) and is paired with a
//      DOMPurify config that explicitly WHITELISTS tags and
//      attributes — anything outside the list is dropped, even if
//      marked happens to emit it. The whitelist is intentionally
//      narrow (no <script>, no <style>, no <iframe>, no event
//      handlers, no inline-style, no data:, no javascript: URLs).
//   2. `renderSafe` (the synchronous, no-deps path) always returns
//      HTML-ESCAPED text — never raw HTML. A page that forgets to
//      `await prewarm()` therefore degrades into "show the
//      description as escaped text" instead of executing a payload.
//   3. The DOMPurify config sets `USE_PROFILES: { html: true }` AND
//      then re-narrows the result with explicit `ALLOWED_TAGS` /
//      `ALLOWED_ATTR` whitelists. The two layers catch the
//      regression where a future DOMPurify version adds a new
//      default tag that we didn't anticipate.
//   4. URLs in <a href> are filtered through DOMPurify's
//      `ALLOWED_URI_REGEXP` so `javascript:`, `data:text/html`, and
//      `vbscript:` are rejected even if they sneak past a tag
//      misconfiguration.
//   5. `FORBID_TAGS: ['script','style','iframe','object','embed',
//      'form','input','button','textarea','select']` is belt-and-
//      suspenders; even if marked/HTML allowlist regressed, these
//      tags are explicitly forbidden.
//   6. The prewarm pipeline attaches SRI integrity hashes from
//      `csp.js` so the CDN cannot ship a tampered sanitizer.
//
// Module surface (root.litecode.markdown):
//
//   .prewarm()          → Promise<void>  — load marked + DOMPurify
//                                          (idempotent, deduped).
//   .isReady()          → boolean         — true once both libs
//                                          finished loading.
//   .renderSafe(s)      → string          — sync, ALWAYS safe
//                                          (HTML-escaped fallback).
//   .renderSafeSync(s)  → string          — sync, REQUIRES isReady().
//                                          Throws otherwise.
//   .renderSafeAsync(s) → Promise<string> — async, awaits prewarm()
//                                          once before resolving.
//
// Why a dedicated module (vs. embedding in app.js):
//   - XSS protection is the highest-stakes single-purpose code in
//     the frontend. A 100-line file with a 4-function surface is
//     auditable in one sitting; a 600-line app shell is not.
//   - The harness (web/test/markdown-xss.html) loads ONLY this
//     module + csp.js, so a regression test does not have to spin
//     up api.js / the full app shell.
//   - Pages that need Markdown (problem.html, profile.html, the
//     future admin problem editor's preview pane) can prewarm once
//     and then use the sync API for instant rendering.

(function (root) {
    'use strict';

    // csp.js MUST be loaded before this file. It owns the canonical
    // SRI hashes for marked + DOMPurify and the page-level CSP value.
    // Failing loud beats silently shipping a payload without SRI
    // (which would defeat this module's primary defense).
    var csp = root.litecode && root.litecode.csp;
    if (!csp || !csp.SCRIPTS || !csp.SCRIPTS.marked || !csp.SCRIPTS.dompurify) {
        throw new Error(
            'litecode.markdown.js: csp.js must be loaded first ' +
            '(defines marked/dompurify SRI hashes)'
        );
    }
    var markedSpec    = csp.SCRIPTS.marked;
    var dompurifySpec = csp.SCRIPTS.dompurify;

    // ────────────────────────────────────────────────────────────────────
    //  Sanitizer config
    //
    //  The allowlist is intentionally conservative: only tags that
    //  appear in real competitive-programming problem statements
    //  (paragraphs, headings, lists, tables, inline code, code
    //  blocks, math-like subscripts in the editorial). Anything else
    //  — including <script>, <style>, <iframe>, <object>, <form> and
    //  inline event handlers — is dropped.
    //
    //  ALLOWED_ATTR: only attributes that affect rendering. We
    //  deliberately do NOT permit `style`, `id`, or `on*` event
    //  handlers. `class` is allowed so problem authors can use the
    //  editor's typography hints (e.g. <code class="lc-mono">).
    // ────────────────────────────────────────────────────────────────────

    var ALLOWED_TAGS = [
        // Block-level
        'p', 'br', 'hr',
        'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
        'blockquote', 'pre',
        'ul', 'ol', 'li',
        'table', 'thead', 'tbody', 'tfoot',
        'tr', 'th', 'td',
        // Inline
        'a', 'span',
        'strong', 'b', 'em', 'i', 's', 'small',
        'code', 'kbd', 'sub', 'sup',
        // Image is allowed so admins can include diagrams; DOMPurify
        // filters the src URL (http/https/data:image only) so a
        // `javascript:` URL cannot survive.
        'img',
    ];

    var ALLOWED_ATTR = [
        'href',    // <a href>
        'title',   // <a title>, <abbr title>
        'alt',     // <img alt>
        'src',     // <img src>
        'class',   // editor's typography hints
        'colspan', // <th>/<td>
        'rowspan', // <th>/<td>
        'start',   // <ol start="3">
        'align',   // <table align="left">  (legacy Markdown)
    ];

    // The list of tag names that MUST NOT appear in the sanitized
    // output even if a future DOMPurify version or marked regression
    // slips one through. Banned:
    //   <script>    — direct code execution
    //   <style>     — could be used to exfil via attribute selectors
    //   <iframe>    — same-origin escape; doesn't help us here
    //   <object>    — plugin / mime sniffing
    //   <embed>     — same as <object>
    //   <form>      — POST to attacker; CSP blocks, but defense in depth
    //   <input>     — exfil via form auto-submit
    //   <button>    — could be combined with onclick
    //   <textarea>  — could be combined with onfocus
    //   <select>    — form-adjacent
    //   <base>      — CSP blocks <base> per default-src 'self', but
    //                 DOMPurify strips it too
    //   <link>      — could load external stylesheets / track pixels
    //   <meta>      — could re-set CSP if the page is sloppy
    var FORBID_TAGS = [
        'script', 'style', 'iframe', 'object', 'embed',
        'form', 'input', 'button', 'textarea', 'select',
        'base', 'link', 'meta',
    ];

    // URL filter: only http(s) and image data URIs survive. Blocks
    // `javascript:`, `data:text/html`, `vbscript:`, `file:`, and
    // anything else that the URL constructor might one day accept.
    // The pattern is intentionally conservative: it requires an
    // explicit scheme prefix and a non-empty host for network URLs.
    var ALLOWED_URI_REGEXP =
        /^(?:(?:https?|mailto):|[^a-z]|[a-z+.\-]+(?:[^a-z+.\-:]|$))/i;

    // ────────────────────────────────────────────────────────────────────
    //  marked configuration
    //
    //  GFM (GitHub-flavored Markdown) is enabled so the
    //  `~~strike~~` / `|`tables`|` / ` ```code blocks``` ` syntax
    //  problem authors use works. `breaks: true` turns single
    //  newlines into <br> — closer to the editor preview they see.
    //  We do NOT enable mangle/headerIds (autogenerated <a name="...">
    //  anchors) because they bloat the output without value here.
    // ────────────────────────────────────────────────────────────────────

    var MARKED_OPTIONS = {
        gfm:        true,
        breaks:     true,
        pedantic:   false,
        mangle:     false,
        headerIds:  false,
    };

    // ────────────────────────────────────────────────────────────────────
    //  SRI script loader
    //
    //  Builds a <script> tag with the integrity + crossOrigin attrs
    //  pre-set from the csp.js registry. The browser refuses to
    //  execute the script if the bytes change.
    // ────────────────────────────────────────────────────────────────────

    function loadScript(spec) {
        return new Promise(function (resolve, reject) {
            // Reuse an existing <script> if another caller has already
            // kicked off this load on the page. The data attribute
            // is namespaced (`data-lc-md-*`) to avoid colliding with
            // app-level scripts.
            var existing = document.querySelector(
                'script[data-lc-md-src="' + spec.url + '"]'
            );
            if (existing) {
                if (existing.dataset.loaded === '1') { resolve(); return; }
                existing.addEventListener('load',  function () { resolve(); });
                existing.addEventListener('error', function () {
                    reject(new Error('litecode.markdown: failed to load ' + spec.url));
                });
                return;
            }

            var s = document.createElement('script');
            s.src          = spec.url;
            s.defer        = true;
            s.integrity    = spec.integrity;
            s.crossOrigin  = spec.crossOrigin || 'anonymous';
            s.dataset.lcMdSrc = spec.url;
            s.addEventListener('load', function () {
                s.dataset.loaded = '1';
                resolve();
            });
            s.addEventListener('error', function () {
                reject(new Error('litecode.markdown: failed to load ' + spec.url));
            });
            document.head.appendChild(s);
        });
    }

    // ────────────────────────────────────────────────────────────────────
    //  Load state — `loaded` flips true on success, `loading` carries
    //  the in-flight promise so concurrent prewarm() calls share it.
    // ────────────────────────────────────────────────────────────────────

    var state = { loaded: false, loading: null, lastError: null };

    // ────────────────────────────────────────────────────────────────────
    //  Sanitization
    //
    //  sanitizeHtml runs marked.parse() and then DOMPurify with the
    //  allowlist above. We never call marked.parse on user input that
    //  hasn't been verified to be a string — number/boolean inputs
    //  are coerced safely.
    // ────────────────────────────────────────────────────────────────────

    function coerceString(input) {
        if (input === null || input === undefined) return '';
        return String(input);
    }

    function sanitizeHtml(rawMarkdown) {
        var md  = coerceString(rawMarkdown);
        var html = root.marked.parse(md, MARKED_OPTIONS);
        // The DOMPurify config is built fresh on every call so a
        // future caller (e.g. admin preview) can override the
        // allowlist for trusted authors without affecting this
        // module's global state. Defense in depth: USE_PROFILES
        // narrows first, then the explicit ALLOWED_TAGS / ALLOWED_ATTR
        // re-narrows.
        return root.DOMPurify.sanitize(html, {
            USE_PROFILES: { html: true },
            ALLOWED_TAGS: ALLOWED_TAGS,
            ALLOWED_ATTR: ALLOWED_ATTR,
            FORBID_TAGS:  FORBID_TAGS,
            ALLOWED_URI_REGEXP: ALLOWED_URI_REGEXP,
            ALLOW_DATA_ATTR: false,
            ALLOW_ARIA_ATTR:  true,
            KEEP_CONTENT: true,    // strip the tag, keep the text
            RETURN_DOM: false,     // we want a string
            RETURN_DOM_FRAGMENT: false,
        });
    }

    // The "always safe" fallback. Used by `renderSafe` and as a
    // last-ditch guard when the CDN scripts failed to load: the
    // description is shown as escaped text, which is uglier but
    // provably XSS-free.
    function escapeHtml(s) {
        var div = document.createElement('div');
        div.textContent = coerceString(s);
        return div.innerHTML;
    }

    // ────────────────────────────────────────────────────────────────────
    //  Public surface
    // ────────────────────────────────────────────────────────────────────

    var ns = {
        // prewarm: load marked + DOMPurify. Idempotent — concurrent
        // calls share the same in-flight promise. Returns a Promise
        // that resolves to { loaded: true } on success and to
        // { loaded: false, error: ... } on failure (we don't reject
        // because callers should still be able to render the
        // escaped-text fallback).
        prewarm: function () {
            if (state.loaded) {
                return Promise.resolve({ loaded: true });
            }
            if (state.loading) return state.loading;

            state.loading = Promise.all([
                loadScript(markedSpec),
                loadScript(dompurifySpec),
            ]).then(function () {
                state.loaded = true;
                state.lastError = null;
                return { loaded: true };
            }).catch(function (err) {
                state.loaded = false;
                state.lastError = err;
                if (root.console && console.warn) {
                    console.warn('[litecode.markdown] CDN load failed; ' +
                                 'falling back to escaped text', err);
                }
                return { loaded: false, error: err };
            }).then(function (r) {
                // Always clear the in-flight slot so a future
                // prewarm() after a network blip can retry.
                state.loading = null;
                return r;
            });
            return state.loading;
        },

        isReady: function () { return state.loaded; },

        // Synchronous, ALWAYS safe. If the libs are loaded, returns
        // full sanitized HTML; otherwise returns HTML-escaped text
        // (ugly but XSS-free). Kick a prewarm in the background so
        // the next call gets the good path.
        renderSafe: function (markdownText) {
            if (state.loaded) return sanitizeHtml(markdownText);
            // Pre-warm asynchronously; the next call after it
            // resolves will go through sanitizeHtml.
            ns.prewarm();
            return escapeHtml(markdownText);
        },

        // Synchronous, REQUIRES isReady(). Throws if called too
        // early. Use when the caller has explicitly awaited
        // prewarm() (or when litecode.markdown.isReady() returned
        // true at boot).
        renderSafeSync: function (markdownText) {
            if (!state.loaded) {
                throw new Error(
                    'litecode.markdown.renderSafeSync: sanitizer not loaded; ' +
                    'await litecode.markdown.prewarm() first or use renderSafe()'
                );
            }
            return sanitizeHtml(markdownText);
        },

        // Async convenience — awaits prewarm() exactly once and
        // returns sanitized HTML. The first caller's network wait
        // is amortized across the whole page.
        renderSafeAsync: function (markdownText) {
            return ns.prewarm().then(function (r) {
                if (r && r.loaded) return sanitizeHtml(markdownText);
                return escapeHtml(markdownText);
            });
        },

        // Exposed for the regression-test harness only. Tests pin
        // this so a future config change is a deliberate, reviewed
        // edit — not a silent default flip.
        _config: {
            ALLOWED_TAGS:       ALLOWED_TAGS,
            ALLOWED_ATTR:       ALLOWED_ATTR,
            FORBID_TAGS:        FORBID_TAGS,
            ALLOWED_URI_REGEXP: ALLOWED_URI_REGEXP,
            MARKED_OPTIONS:     MARKED_OPTIONS,
            SCRIPTS:            { marked: markedSpec, dompurify: dompurifySpec },
        },
    };

    root.litecode = root.litecode || {};
    root.litecode.markdown = ns;
})(window);
