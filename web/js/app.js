// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend app shell (Phase 5 ★ 前端框架)
//
// Per-page conveniences layered on top of api.js. Owns:
//   - Navigation bar (`[data-nav]` slot; replaces itself on auth changes)
//   - Toast / inline-notification surface wired to `litecode:api-error`
//   - Dark-mode toggle (`litecode.theme.{get,set,toggle,reset}` +
//     [data-act="theme-toggle"] button in the nav bar)
//   - `litecode.guard.{requireAuth,requireAdmin,requireGuest}` redirect
//   - Re-exports `litecode.markdown` (owned by markdown.js — see
//     that file for the SRI-pinned DOMPurify + marked pipeline)
//
// Dark-mode load order (SPEC §6.3 / A34 — no flash on refresh):
//
//   <head>
//     <script src="/js/theme-boot.js"></script>     ← sync, in <head>
//     <link rel="stylesheet" href="/css/style.css"> ← after boot
//     <script src="/js/csp.js" defer></script>
//     <script src="/js/api.js" defer></script>
//     <script src="/js/app.js" defer></script>
//   </head>
//
// theme-boot.js reads `litecode:theme` from localStorage and applies
// the right `.dark` class + `data-theme-chosen` attribute before the
// stylesheet paints, so dark-mode users never see a flash of light.
// app.js then exposes the toggle button + public API on top.
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
// Phase 5 ★ 前端权限拦截 (SPEC §6.3 / §15.3 / A24):
//   app.js also installs a SYNCHRONOUS admin-route gate at IIFE load
//   time — before DOMContentLoaded, before any paint. If the URL is
//   /admin/* and sessionStorage already has a cached non-admin user,
//   we `location.replace('/index.html')` immediately so the admin UI
//   never flashes. For the no-cache case (first visit / fresh tab),
//   the async `boot.shell({ requireAdmin: true })` redirects to
//   /login.html (unauthenticated) or /index.html (non-admin) after
//   hydration. While the async gate runs, the page is hidden via
//   `html.lc-route-pending { visibility: hidden }` in style.css so
//   the admin UI stays invisible until the gate resolves.
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
    //  Synchronous admin-route gate (Phase 5 ★ 前端权限拦截 / SPEC A24)
    //
    //  Runs the moment app.js is parsed — before DOMContentLoaded, before
    //  any paint. The script load order is csp.js → api.js → app.js
    //  (defer), so by the time we run the <body> is already parsed but
    //  the browser hasn't composited a frame yet. That window is the
    //  right place to install a no-flash permission check.
    //
    //  Two paths get protected:
    //   1) `cached_user.role !== 'admin'` and URL is /admin/*:
    //      Synchronous `location.replace('/index.html')`. The browser
    //      cancels the in-flight render and starts over on /index.html
    //      — admin UI never paints.
    //
    //   2) Cached user absent OR cached user IS admin: fall through
    //      to the async `boot.shell({ requireAdmin: true })` below
    //      to do the full hydrate-from-cookie check. While that async
    //      gate runs, the body is hidden via `html.lc-route-pending`
    //      (set synchronously here, removed by boot.shell). Without
    //      this a logged-out user could still see the admin shell
    //      render for ~50ms before the cookie probe rejects.
    //
    //  The /admin/* allowlist is intentionally explicit (rather than
    //  `pathname.indexOf('/admin/') === 0`) so a future typo'd admin
    //  path doesn't sneak through. We DO keep the prefix check as a
    //  forward-compat fallback so any future admin page gets the
    //  gate automatically — admins only ever add pages under /admin/.
    // ────────────────────────────────────────────────────────────────────

    var ADMIN_ROUTES = [
        '/admin/dashboard.html',
        '/admin/users.html',
        '/admin/problems.html',
        '/admin/problem-edit.html',
        '/admin/audit-logs.html',
    ];

    function isAdminRoute(path) {
        var p = path || (root.location && root.location.pathname) || '';
        if (ADMIN_ROUTES.indexOf(p) !== -1) return true;
        // Forward-compat: any future /admin/<file>.html also gates.
        return p.indexOf('/admin/') === 0 && /\.html$/.test(p);
    }

    var onAdminRoute = isAdminRoute();

    if (onAdminRoute) {
        // Hide the body while the async gate resolves. App.js removes
        // this class once boot.shell confirms the user is admin (or
        // navigates away). The CSS rule lives in web/css/style.css §21.
        document.documentElement.classList.add('lc-route-pending');

        // If we have a cached user from a prior page and they're not
        // an admin, redirect NOW. No flash, no API round-trip — the
        // cached role is the single source of truth for the common
        // "navigate within the SPA while logged in as non-admin" case.
        try {
            var cachedRaw = root.sessionStorage &&
                root.sessionStorage.getItem('litecode:user');
            if (cachedRaw) {
                var cached = JSON.parse(cachedRaw);
                if (cached && cached.role && cached.role !== 'admin') {
                    root.location.replace('/index.html');
                    // Bail out — the IIFE finishes but the page is
                    // being torn down. Subsequent boot.shell on the
                    // next page (index.html) will mount normally.
                }
            }
        } catch (_) {
            // Malformed JSON in sessionStorage — fall through, let
            // the async gate figure it out from a fresh /auth/profile.
        }
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

            // Theme toggle is always available. Icon + label are
            // synced to the current mode in syncThemeButton() right
            // after renderNav() inserts the button.
            var themeBtn = el('button', {
                type: 'button',
                class: 'lc-icon-btn',
                'aria-label': LABEL_TOGGLE,
                title:       LABEL_TOGGLE,
                dataset: { act: 'theme-toggle' },
                text: '🌗',  // placeholder; syncThemeButton() overwrites
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

        // Sync the theme toggle's icon / aria-pressed to whatever
        // theme-boot.js painted before this nav was rendered. Without
        // this the button would keep the placeholder 🌗 emoji and
        // a11y state until the user clicks it.
        syncThemeButton();

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
    //  - `data-theme-chosen` attribute on <html> signals "user has made
    //    an explicit choice"; style.css uses it to gate the @media
    //    system-preference fallback so an explicit choice wins.
    //
    //  No-flash ordering:
    //    1. web/js/theme-boot.js (sync, in <head> BEFORE stylesheet)
    //       reads localStorage and applies the right class +
    //       data-theme-chosen attribute before the first paint.
    //    2. bootShell() runs after app.js parses; it does NOT need
    //       to re-apply the theme (theme-boot.js already did it),
    //       but it DOES need to keep the toggle button in sync
    //       with the actual state, since the button is rendered
    //       later by renderNav().
    // ────────────────────────────────────────────────────────────────────

    var ICON_LIGHT = '🌞';  // shown when current mode is dark (click to go light)
    var ICON_DARK  = '🌙';  // shown when current mode is light (click to go dark)
    var LABEL_TOGGLE = '切换深色模式';

    function currentTheme() {
        return document.documentElement.classList.contains('dark') ? 'dark' : 'light';
    }

    function applyTheme(theme) {
        // `applyTheme` is called from bootShell on first run for the
        // system-preference fallback path (when localStorage is empty).
        // In that case theme-boot.js left `data-theme-chosen` absent
        // and CSS @media (prefers-color-scheme: dark) is doing the work.
        // If we add `.dark` here we MUST also set the attribute, otherwise
        // a future system-preference change would try to flip back over
        // our explicit class. So applyTheme always commits the choice.
        var html = document.documentElement;
        if (theme === 'dark') html.classList.add('dark');
        else                  html.classList.remove('dark');
        // Always lock the choice — even the system-preference path
        // becomes an explicit choice once we've painted it. The user
        // can always clear localStorage to follow the OS again.
        html.setAttribute('data-theme-chosen', '1');
    }

    function readStoredTheme() {
        try { return localStorage.getItem(THEME_KEY); }
        catch (_) { return null; }
    }
    function writeStoredTheme(theme) {
        try { localStorage.setItem(THEME_KEY, theme); }
        catch (_) { /* ignore */ }
    }
    function clearStoredTheme() {
        try { localStorage.removeItem(THEME_KEY); }
        catch (_) { /* ignore */ }
    }
    function detectInitialTheme() {
        // Returns what the page IS currently showing. theme-boot.js
        // applied either the stored choice or the system preference
        // before paint, so this just reports reality.
        return currentTheme();
    }
    function toggleTheme() {
        var next = currentTheme() === 'dark' ? 'light' : 'dark';
        writeStoredTheme(next);
        applyTheme(next);
        // The toggle button's icon/label need to flip to match the
        // new state. We dispatch a `litecode:theme-changed` event so
        // any nav-bar re-render (or other listener) can sync without
        // each consumer polling currentTheme().
        root.dispatchEvent(new CustomEvent('litecode:theme-changed', {
            detail: { theme: next }
        }));
    }

    // Sync the toggle button's icon + aria-pressed to the current state.
    // Called by renderNav() after the button is built, and by the
    // `litecode:theme-changed` event listener installed at boot.
    function syncThemeButton() {
        var btns = document.querySelectorAll('[data-act="theme-toggle"]');
        if (!btns.length) return;
        var theme = currentTheme();
        var isDark = theme === 'dark';
        btns.forEach(function (btn) {
            // Icon hints the action: 🌙 in light mode (click → dark),
            // 🌞 in dark mode (click → light). Same logical pattern
            // as GitHub / Notion / Vercel.
            btn.textContent = isDark ? ICON_DARK : ICON_LIGHT;
            // aria-pressed is the canonical state for toggle buttons —
            // screen readers announce "pressed" / "not pressed" so the
            // user knows the current mode without seeing the icon.
            btn.setAttribute('aria-pressed', isDark ? 'true' : 'false');
            // Tooltip + accessible name reflect the next action.
            var nextLabel = isDark ? '切换到浅色模式' : '切换到深色模式';
            btn.setAttribute('aria-label', nextLabel);
            btn.setAttribute('title', nextLabel);
        });
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

        // Theme was already applied by theme-boot.js (sync, in <head>,
        // before the stylesheet loads) so first paint is correct.
        // bootShell only needs to:
        //   1. install the prefers-color-scheme listener so a system
        //      change without an explicit choice still moves the UI
        //      AND keeps the toggle button in sync via syncThemeButton.
        //   2. listen for the litecode:theme-changed event so a
        //      re-rendered nav (or other surface) refreshes its
        //      button state when the user toggles.
        if (window.matchMedia) {
            var mq = window.matchMedia('(prefers-color-scheme: dark)');
            var listener = function (e) {
                if (readStoredTheme()) return;        // explicit choice wins
                applyTheme(e.matches ? 'dark' : 'light');
                syncThemeButton();
            };
            if (mq.addEventListener) mq.addEventListener('change', listener);
            else if (mq.addListener) mq.addListener(listener);
        }

        // Wire once: when the user toggles, every page's toggle button
        // (and any other theme-aware surface) re-syncs. Idempotent
        // because listenersInstalled is checked at boot, not per call.
        root.addEventListener('litecode:theme-changed', syncThemeButton);

        wireGlobalErrors();

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

        // Resolve the user (or null), then apply the requireAdmin gate
        // by delegating to litecode.guard.requireAdmin — single source
        // of truth for the redirect policy so callers that use either
        // boot.shell({requireAdmin}) or guard.requireAdmin() see the
        // same behavior (login.html on no-user, index.html on non-admin).
        return hydrateUser().then(function (user) {
            if (opts.requireAdmin) {
                return gate.requireAdmin(user, {
                    silent: false,
                    onAllowed: function (u) {
                        // Allow the admin UI to paint now that the
                        // async gate has cleared. This must run AFTER
                        // hydrateUser so the cached-role race is gone.
                        document.documentElement.classList.remove('lc-route-pending');
                        return u;
                    },
                });
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
    //
    //  Phase 5 ★ 前端权限拦截 / SPEC §6.3 / A24:
    //    requireAdmin differentiates the two failure modes — no user
    //    at all → /login.html (so they can sign in and retry this
    //    exact URL via the `?next=` round-trip); authenticated but
    //    role !== 'admin' → /index.html (they're signed in fine,
    //    just not allowed here). v1.2.21+ cookie path means a fresh
    //    tab is "no user" until /auth/refresh succeeds, so this
    //    path is more common than it looks.
    // ────────────────────────────────────────────────────────────────────

    function nextUrl() {
        return encodeURIComponent(
            (root.location.pathname || '') +
            (root.location.search  || '')
        );
    }

    var gate = {
        // Synchronous check on a hydrated user object. Used by both
        // bootShell({requireAdmin}) and guard.requireAdmin(). The
        // caller decides what to do with the verdict:
        //   silent:true  → return verdict + redirect logic without
        //                 any toast (used when the caller is going
        //                 to show its own UI feedback).
        //   onAllowed    → called when the user IS an admin; lets the
        //                 caller un-hide the page or kick off extra
        //                 side effects. The default is identity.
        //
        // Returns:
        //   - on allowed: the user (after onAllowed has run)
        //   - on denied:  null (the function has already redirected)
        //   - always:     a Promise that resolves once the redirect
        //                 has been issued; awaiting it lets the caller
        //                 know not to do any further work.
        requireAdmin: function (user, opts) {
            opts = opts || {};
            var onAllowed = opts.onAllowed || function (u) { return u; };
            var silent   = !!opts.silent;

            // Case 1: no user at all → login page with ?next= so they
            // come back here after signing in.
            if (!user) {
                if (!silent) toast('warn', '请先登录');
                root.location.replace('/login.html?next=' + nextUrl());
                return new Promise(function () { /* never */ });
            }
            // Case 2: signed in but wrong role → home page. We still
            // toast so the user understands why they were bounced
            // (otherwise it looks like a navigation glitch).
            if (user.role !== 'admin') {
                if (!silent) toast('error', '没有管理员权限');
                root.location.replace('/index.html');
                return new Promise(function () { /* never */ });
            }
            return Promise.resolve(onAllowed(user));
        },
    };

    var guard = {
        // Public alias so callers that prefer the imperative
        // `litecode.guard.requireAdmin()` style get the same
        // policy. Async-only — pulls a fresh profile from the
        // server, so it costs one /auth/profile round-trip. For
        // boot-time gating use boot.shell({requireAdmin:true})
        // which re-uses the cached user (no extra request).
        requireAuth: function () {
            if (!api.auth.isLoggedIn()) {
                var next = nextUrl();
                root.location.replace('/login.html?next=' + next);
                return new Promise(function () {});
            }
            // Hydrate (or refresh) the user, then return it.
            return api.auth.fetchProfile().catch(function () { return null; });
        },

        requireAdmin: function () {
            return guard.requireAuth().then(function (user) {
                return gate.requireAdmin(user, { silent: false });
            });
        },

        requireGuest: function () {
            if (api.auth.isLoggedIn()) {
                root.location.replace('/index.html');
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
        // Returns the theme currently being shown (dark / light).
        // Mirrors `document.documentElement.classList.contains('dark')`
        // but reads through the same helper the rest of the module uses.
        get:    function () { return currentTheme(); },
        // Sets + persists + applies. Fires the `litecode:theme-changed`
        // event so other listeners (the toggle button) re-sync.
        set:    function (t) {
            if (t !== 'dark' && t !== 'light') return;
            writeStoredTheme(t);
            applyTheme(t);
            syncThemeButton();
        },
        // Clears the persisted choice and re-follows the OS via the
        // `prefers-color-scheme` media query. Useful for a future
        // "reset to system" affordance or for tests.
        reset:  function () {
            clearStoredTheme();
            var mq = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)');
            applyTheme((mq && mq.matches) ? 'dark' : 'light');
            syncThemeButton();
        },
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
