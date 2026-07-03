// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend app shell (Phase 5 ★ 前端框架)
//
// Per-page conveniences layered on top of api.js. Owns:
//   - Navigation bar (`[data-nav]` slot; replaces itself on auth changes)
//   - Toast / inline-notification surface wired to `litecode:api-error`
//   - Dark-mode bootstrap (CSS variable theme; respects user override)
//   - `litecode.guard.{requireAuth,requireAdmin,requireGuest}` redirect
//   - Re-exports `litecode.markdown` (owned by markdown.js — see
//     that file for the SRI-pinned DOMPurify + marked pipeline)
//
// All pages should include api.js FIRST and app.js AFTER, then call
// either:
//
//   litecode.boot.shell({ admin: false, title: 'LiteCode' })
//
//   litecode.guard.requireAuth().then(...)
//
//   litecode.guard.requireAdmin().then(...)
//
//   litecode.boot.shell({ guestOnly: true })     // /login + /register
//
// `shell` mounts the nav and hydrates the user (fetching /auth/profile
// in the background). It does NOT block the page render — fetchProfile
// is fire-and-forget so the user sees a nav immediately.
//
// Phase 5 ★ 前端框架 deliverable. See web/index.html for the canonical
// page-boot pattern.

(function (root) {
    'use strict';

    var api = root.litecode && root.litecode.api;
    if (!api) {
        // api.js MUST be loaded first. Failing early beats a half-boot
        // where the nav mounts but logout/login throw mysterious
        // `undefined` errors every call.
        throw new Error('litecode.app.js: api.js must be loaded before app.js');
    }

    // ────────────────────────────────────────────────────────────────────
    //  Constants
    // ────────────────────────────────────────────────────────────────────

    var NAV_LINKS = [
        { label: '题库',         href: '/index.html',     key: 'problems' },
        { label: '排行榜',       href: '/ranking.html',   key: 'ranking'  },
        { label: '个人主页',     href: '#',               key: 'profile', authOnly: true,
          dynamicHref: function (u) { return '/profile.html?u=' + encodeURIComponent(u.username); } },
        { label: '管理后台',     href: '/admin/dashboard.html', key: 'admin', adminOnly: true },
    ];

    var THEME_KEY = 'litecode:theme';

    // ────────────────────────────────────────────────────────────────────
    //  Navigation bar
    //
    //  Renders into ALL elements matching `[data-nav]`. Re-mounts on
    //  auth-changed events so login/logout updates the right-hand
    //  cluster without each page wiring event listeners.
    // ────────────────────────────────────────────────────────────────────

    var navState = { mounted: false, listenersInstalled: false, user: null };

    function el(tag, attrs, children) {
        var node = document.createElement(tag);
        if (attrs) {
            Object.keys(attrs).forEach(function (k) {
                if (k === 'class')      node.className = attrs[k];
                else if (k === 'text')  node.textContent = attrs[k];
                else if (k === 'html')  node.innerHTML = attrs[k];
                else if (k.charAt(0) === 'o' && k.charAt(1) === 'n') {
                    node.addEventListener(k.slice(2).toLowerCase(), attrs[k]);
                } else if (k === 'dataset') {
                    Object.keys(attrs[k]).forEach(function (dk) {
                        node.dataset[dk] = attrs[k][dk];
                    });
                } else {
                    node.setAttribute(k, attrs[k]);
                }
            });
        }
        if (children !== undefined && children !== null) {
            (Array.isArray(children) ? children : [children]).forEach(function (c) {
                if (c === null || c === undefined) return;
                if (typeof c === 'string') node.appendChild(document.createTextNode(c));
                else node.appendChild(c);
            });
        }
        return node;
    }

    function navLinksHtml(user) {
        var frag = document.createDocumentFragment();
        var path = window.location.pathname;
        var onPublicPage = (path === '/' || path === '/index.html' || path.indexOf('/index.html') === 0);

        NAV_LINKS.forEach(function (link) {
            var requiresAuth  = !!link.authOnly;
            var requiresAdmin = !!link.adminOnly;
            if (requiresAuth  && !user) return;
            if (requiresAdmin && (!user || user.role !== 'admin')) return;

            var href = link.href;
            if (link.dynamicHref && user) href = link.dynamicHref(user);

            var a = el('a', {
                href:  href,
                class: 'lc-nav-link' + ((path === href) ? ' lc-nav-link--active' : ''),
                text:  link.label,
                dataset: { key: link.key },
            });

            // Profile link uses a query string; mark-only when matched.
            if (link.key === 'profile' && /\/profile\.html/.test(path)) {
                a.className += ' lc-nav-link--active';
            }
            frag.appendChild(a);
        });

        return frag;
    }

    function renderNav(user) {
        // Late-bind if user lands here before api.js cached.
        user = user || navState.user || api.auth.currentUser || null;

        var slots = document.querySelectorAll('[data-nav]');
        if (!slots.length) return;

        slots.forEach(function (slot) {
            // Wipe prior content so re-render is idempotent.
            while (slot.firstChild) slot.removeChild(slot.firstChild);

            var bar = el('nav', {
                class: 'lc-nav',
                'aria-label': '主导航',
            });

            var brand = el('a', {
                href: '/index.html',
                class: 'lc-nav-brand',
                text: 'LiteCode',
            });
            bar.appendChild(brand);

            var links = el('div', { class: 'lc-nav-links' });
            links.appendChild(navLinksHtml(user));
            bar.appendChild(links);

            var right = el('div', { class: 'lc-nav-right' });

            // Theme toggle is always available.
            var themeBtn = el('button', {
                type: 'button',
                class: 'lc-icon-btn',
                'aria-label': '切换深色模式',
                title: '切换深色模式',
                dataset: { act: 'theme-toggle' },
                text: '🌗',
            });
            themeBtn.addEventListener('click', toggleTheme);
            right.appendChild(themeBtn);

            if (user) {
                var username = String(user.username || '');
                var avatar = el('div', { class: 'lc-nav-user', dataset: { act: 'user-menu' } });
                avatar.appendChild(el('span', {
                    class: 'lc-avatar',
                    text: username ? username.charAt(0).toUpperCase() : '?',
                    title: username,
                }));
                var name = el('span', { class: 'lc-nav-username', text: username });
                avatar.appendChild(name);
                avatar.addEventListener('click', function (e) {
                    e.stopPropagation();
                    openUserMenu(avatar);
                });
                right.appendChild(avatar);
            } else {
                var signIn = el('a', {
                    href: '/login.html',
                    class: 'lc-btn lc-btn--ghost',
                    text: '登录',
                });
                var signUp = el('a', {
                    href: '/register.html',
                    class: 'lc-btn lc-btn--primary',
                    text: '注册',
                });
                right.appendChild(signIn);
                right.appendChild(signUp);
            }

            bar.appendChild(right);
            slot.appendChild(bar);
        });

        navState.mounted = true;
    }

    function openUserMenu(anchor) {
        // Close any prior menu.
        var prior = document.getElementById('lc-user-menu');
        if (prior) { prior.remove(); return; }

        var menu = el('div', {
            class: 'lc-menu',
            id:   'lc-user-menu',
            dataset: { role: 'user-menu' },
        });
        var u = navState.user || api.auth.currentUser;
        if (!u) return;

        menu.appendChild(el('a', { class: 'lc-menu-item', href: '/profile.html?u=' + encodeURIComponent(u.username || ''), text: '个人主页' }));
        if (u.role === 'admin') {
            menu.appendChild(el('a', { class: 'lc-menu-item', href: '/admin/dashboard.html', text: '管理后台' }));
        }
        menu.appendChild(el('div', { class: 'lc-menu-sep' }));
        var logoutBtn = el('button', {
            type: 'button',
            class: 'lc-menu-item lc-menu-item--danger',
            text: '退出登录',
            onClick: function () {
                api.auth.logout().finally(function () {
                    window.location.href = '/login.html';
                });
            },
        });
        menu.appendChild(logoutBtn);

        document.body.appendChild(menu);
        // Position
        var rect = anchor.getBoundingClientRect();
        menu.style.top  = (rect.bottom + 6 + window.scrollY) + 'px';
        menu.style.left = (rect.right + window.scrollX - menu.offsetWidth) + 'px';

        // Click-away to dismiss
        setTimeout(function () {
            document.addEventListener('click', function once(ev) {
                if (menu.contains(ev.target)) return;
                menu.remove();
                document.removeEventListener('click', once);
            });
        }, 0);
    }

    function hydrateUser() {
        // Phase 5 ★ Token 存储:
        //   The access token is in MEMORY ONLY — a page reload wipes it.
        //   To detect "is this user signed in?" we have to ask the
        //   server (via /auth/refresh, which reads the HttpOnly cookie).
        //   If the cookie is present + valid → fresh access token +
        //   user → logged-in nav. If not → guest nav.
        //
        //   We don't gate nav-render on this — the cached user from
        //   sessionStorage gives us an immediate render with the
        //   right avatar/menu, then the refresh either confirms it
        //   (auth-changed listener re-renders) or fires the 401
        //   sign-out path (event listener clears nav).
        //
        //   tryRefresh() is fire-and-forget here — failures are
        //   expected when the cookie is absent (logged-out user) and
        //   must not throw to the caller. We swallow into a guest
        //   nav render.
        navState.user = api.auth.currentUser || null;
        renderNav(navState.user);

        return api.auth.tryRefresh()
            .then(function () { return api.auth.fetchProfile(); })
            .then(function (user) {
                navState.user = user || null;
                renderNav(navState.user);
                return navState.user;
            })
            .catch(function () {
                // Cookie missing / expired / revoked → guest nav.
                // No redirect: page-level guard.requireAuth() will
                // bounce to /login.html if the page actually needs auth.
                navState.user = null;
                renderNav(null);
                return null;
            });
    }

    // ────────────────────────────────────────────────────────────────────
    //  Theme (dark / light) — SPEC §6.3 / A34
    //
    //  - Persisted in localStorage under 'litecode:theme' = 'dark' / 'light'
    //  - If absent, falls back to `prefers-color-scheme: dark`
    //  - Toggled via [data-act="theme-toggle"] or `litecode.theme.toggle()`
    //  - Class lives on <html> so CSS custom-property cascades work
    // ────────────────────────────────────────────────────────────────────

    function applyTheme(theme) {
        var html = document.documentElement;
        // We use a class, not data-theme, so `prefers-color-scheme`
        // media queries can still be overridden by an explicit choice.
        if (theme === 'dark') html.classList.add('dark');
        else                  html.classList.remove('dark');
    }

    function readStoredTheme() {
        try { return localStorage.getItem(THEME_KEY); }
        catch (_) { return null; }
    }
    function writeStoredTheme(theme) {
        try { localStorage.setItem(THEME_KEY, theme); }
        catch (_) { /* ignore */ }
    }
    function detectInitialTheme() {
        var stored = readStoredTheme();
        if (stored === 'dark' || stored === 'light') return stored;
        // Match what the OS / browser is asking for.
        var mq = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)');
        return (mq && mq.matches) ? 'dark' : 'light';
    }
    function toggleTheme() {
        var now = document.documentElement.classList.contains('dark') ? 'dark' : 'light';
        var next = (now === 'dark') ? 'light' : 'dark';
        writeStoredTheme(next);
        applyTheme(next);
    }

    // ────────────────────────────────────────────────────────────────────
    //  Toast — global notification layer listening to `litecode:api-error`
    //
    //  Each toast is dismissable; auto-dismiss after 4s unless hovered.
    //  Pages can also call litecode.toast.success/error/info directly.
    // ────────────────────────────────────────────────────────────────────

    function ensureToastContainer() {
        var c = document.getElementById('lc-toast-container');
        if (c) return c;
        c = el('div', {
            class: 'lc-toasts',
            id:    'lc-toast-container',
            'aria-live': 'polite',
            'aria-atomic': 'false',
        });
        document.body.appendChild(c);
        return c;
    }

    function toast(kind, message, opts) {
        opts = opts || {};
        var container = ensureToastContainer();
        var t = el('div', { class: 'lc-toast lc-toast--' + kind, role: 'status' });
        t.appendChild(el('span', { class: 'lc-toast-msg', text: String(message || '') }));

        var dismiss = el('button', {
            type: 'button',
            class: 'lc-toast-close',
            'aria-label': '关闭通知',
            text: '×',
            onClick: function () { t.remove(); },
        });
        t.appendChild(dismiss);

        container.appendChild(t);

        var ttl = typeof opts.ttl === 'number' ? opts.ttl : 4000;
        var timer = setTimeout(function () {
            if (t.parentNode) t.classList.add('lc-toast--leaving');
            setTimeout(function () { if (t.parentNode) t.remove(); }, 200);
        }, ttl);
        t.addEventListener('mouseenter', function () { clearTimeout(timer); });
        return t;
    }

    // Auto-wire API errors to the toast layer. The api.js module fires
    // a CustomEvent on `litecode:api-error` for every non-2xx throw —
    // we map by HTTP status to message kind.
    function wireGlobalErrors() {
        api.onError(function (e) {
            var err = e && e.err;
            if (!err) return;
            var msg = err.message || ('HTTP ' + (err.status || 0));
            var kind;
            if      (err.status === 401) kind = 'warn';          // 401 will already redirect
            else if (err.status === 403) kind = 'error';
            else if (err.status === 404) kind = 'info';
            else if (err.status === 429) kind = 'warn';
            else if (err.status >= 500)  kind = 'error';
            else                         kind = 'warn';
            toast(kind, msg);
        });
        api.auth.onUnauthorized(function (reason) {
            // Don't toast "session expired" if the cause was a deliberate
            // logout — that's the user, not a problem.
            if (reason && reason.reason === 'logout') return;
            toast('warn', '登录状态已过期，请重新登录');
        });
    }

    // ────────────────────────────────────────────────────────────────────
    //  Page-boot helper — `litecode.boot.shell({...})`
    //
    //  Boots:
    //   - Theme (immediate; before nav so first paint is correct)
    //   - Navigation bar (hydrated from cache + background refresh)
    //   - Global error wiring
    //   - Optional title override (sets <title>)
    //
    //  Returns a Promise that resolves once user hydration finishes
    //  (or fails). Pages that need the user immediately should
    //  `await boot.shell(...)`; pure SSR-style pages can fire-and-forget.
    // ────────────────────────────────────────────────────────────────────

    function bootShell(opts) {
        opts = opts || {};

        // Theme first to avoid a flash of light when the user prefers dark.
        applyTheme(detectInitialTheme());

        wireGlobalErrors();

        // Match `prefers-color-scheme` after first paint so a system
        // change without a manual toggle still moves the UI.
        if (window.matchMedia) {
            var mq = window.matchMedia('(prefers-color-scheme: dark)');
            var listener = function (e) {
                if (readStoredTheme()) return;        // explicit choice wins
                applyTheme(e.matches ? 'dark' : 'light');
            };
            if (mq.addEventListener) mq.addEventListener('change', listener);
            else if (mq.addListener) mq.addListener(listener);
        }

        if (opts.title) document.title = opts.title;

        // Optional: render nav eagerly using the cached user so the page
        // is never naked. Then refresh in the background.
        navState.user = api.auth.currentUser || null;
        renderNav(navState.user);

        // Install the auth-change listener exactly once — every
        // re-render from a state change flows through this single
        // entrypoint.
        if (!navState.listenersInstalled) {
            navState.listenersInstalled = true;
            api.auth.onAuthChanged(function (user) {
                navState.user = user;
                renderNav(user);
            });
        }

        if (opts.guestOnly && api.auth.isLoggedIn()) {
            // Login/register pages bounce authed users to the index.
            window.location.replace('/index.html');
            return new Promise(function () { /* never resolves */ });
        }

        return hydrateUser().then(function (user) {
            if (opts.requireAdmin && (!user || user.role !== 'admin')) {
                toast('error', '没有管理员权限');
                window.location.replace('/index.html');
            }
            return user;
        });
    }

    // ────────────────────────────────────────────────────────────────────
    //  Page guards — `litecode.guard.requireAuth/requireAdmin/requireGuest`
    //
    //  Use at the top of a protected page's IIFE:
    //
    //    litecode.guard.requireAuth().then(function (user) {
    //        // render guarded content
    //    });
    //
    //  Returns Promise<user>. If the auth gate fails, api.js fires the
    //  `litecode:api-unauthorized` event, clears tokens, and redirects to
    //  /login.html — the caller never sees a value.
    // ────────────────────────────────────────────────────────────────────

    var guard = {
        requireAuth: function () {
            if (!api.auth.isLoggedIn()) {
                var next = encodeURIComponent(window.location.pathname + window.location.search);
                window.location.replace('/login.html?next=' + next);
                return new Promise(function () {});
            }
            // Hydrate (or refresh) the user, then return it.
            return api.auth.fetchProfile().catch(function () { return null; });
        },

        requireAdmin: function () {
            return guard.requireAuth().then(function (user) {
                if (!user || user.role !== 'admin') {
                    toast('error', '需要管理员权限');
                    window.location.replace('/index.html');
                    return new Promise(function () {});
                }
                return user;
            });
        },

        requireGuest: function () {
            if (api.auth.isLoggedIn()) {
                window.location.replace('/index.html');
                return new Promise(function () {});
            }
            return Promise.resolve(null);
        },
    };

    // ────────────────────────────────────────────────────────────────────
    //  Markdown sanitizer — DOMPurify + marked, lazy-loaded on first use.
    //
    //  The sanitizer itself lives in `markdown.js` (single source of
    //  truth for the XSS allowlist and the SRI-pinned CDN loader). This
    //  module is a thin re-export so pages that depend on the legacy
    //  `litecode.markdown` API keep working without churn.
    //
    //  Load order: csp.js → markdown.js → app.js. csp.js owns the
    //  canonical page CSP value and the SRI registry; markdown.js owns
    //  the sanitizer + allowlist. app.js doesn't reach into either —
    //  it just hands the caller the same `litecode.markdown` object the
    //  rest of the page already uses.
    //
    //  If a page forgets to load markdown.js, the SRI/allowlist
    //  defaults aren't available and we throw so the misconfiguration
    //  is loud rather than silently shipping a weakened policy.
    // ────────────────────────────────────────────────────────────────────

    var markdown = root.litecode && root.litecode.markdown;
    if (!markdown || typeof markdown.prewarm !== 'function' ||
        typeof markdown.renderSafe !== 'function') {
        throw new Error(
            'litecode.app.js: markdown.js must be loaded before app.js ' +
            '(defines the XSS sanitizer pipeline)'
        );
    }

    // ────────────────────────────────────────────────────────────────────
    //  Export
    // ────────────────────────────────────────────────────────────────────

    root.litecode = root.litecode || {};
    root.litecode.boot    = { shell: bootShell };
    root.litecode.nav     = {
        mount: function () { renderNav(navState.user || api.auth.currentUser); },
        user:  function () { return navState.user || api.auth.currentUser; },
    };
    root.litecode.theme   = {
        get:    function () {
            return document.documentElement.classList.contains('dark') ? 'dark' : 'light';
        },
        set:    function (t) { writeStoredTheme(t); applyTheme(t); },
        toggle: toggleTheme,
    };
    root.litecode.toast   = {
        success: function (m, o) { return toast('success', m, o); },
        error:   function (m, o) { return toast('error',   m, o); },
        info:    function (m, o) { return toast('info',    m, o); },
        warn:    function (m, o) { return toast('warn',    m, o); },
    };
    root.litecode.guard   = guard;
    // Re-export the markdown module so legacy `litecode.markdown.*`
    // call sites still work. The single source of truth is
    // `web/js/markdown.js` — app.js does not own a duplicate
    // implementation.
    root.litecode.markdown = markdown;
})(window);
