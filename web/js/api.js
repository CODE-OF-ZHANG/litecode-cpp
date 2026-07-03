// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend API wrapper (Phase 5 ★ 前端框架 + ★ Token 存储)
//
// One-file module that owns every cross-cutting HTTP concern Phase 5
// promises:
//   1. `litecode.api.{get,post,put,delete}` — opinionated fetch wrapper
//   2. `litecode.api.rawFetch(...)`         — escape hatch for SSE / multipart
//   3. Bearer-token attachment from in-memory only (no storage layer —
//      an XSS that steals the access token still has to wait for the
//      refresh to expire OR the user to log out before it can renew)
//   4. Single-flight refresh-on-401 with one-shot retry. /auth/refresh
//      itself relies on the HttpOnly cookie sent by the browser; the
//      JS layer never sees the refresh value.
//   5. SPEC §5.7 unified error envelope surfacing — `LitecodeApiError`
//      subclass with `{status, code, message, details, request_id}`
//   6. `DispatchEvent` on `document`: `litecode:api-error` /
//      `litecode:api-unauthorized` so the toast layer (app.js) can
//      react without touching every call site
//
// Token storage strategy (Phase 5 ★ Token 存储, SPEC §6.3 / §15.3):
//   - access_token: in-memory only (the `accessToken` module variable).
//     NOT in sessionStorage / localStorage — a refresh of the page
//     wipes it. The /auth/refresh call (cookie path) rehydrates it.
//   - refresh_token: NOT stored in JS at all. /api/v1/auth/login +
//     /api/v1/auth/register + /api/v1/auth/refresh respond with
//     `Set-Cookie: lc_refresh=...; HttpOnly; Secure; SameSite=Strict;
//     Path=/api/v1/auth; Max-Age=...` and the browser stores / sends
//     it transparently. JS reads `document.cookie` and sees nothing
//     because of HttpOnly. /api/v1/auth/logout responds with the
//     same name at Max-Age=0 to delete the cookie server-side.
//
// What this gives us:
//   - XSS that exfiltrates localStorage can no longer grab a refresh
//     token (the value isn't there). Even if it grabs the access
//     token, the 2h TTL is the maximum damage window — there's no
//     way for the attacker to renew without the HttpOnly cookie.
//   - Page reload: in-memory access token is gone. The SPA's
//     `litecode.boot.shell(...)` issues a /auth/refresh on load.
//     The browser automatically attaches the cookie; the server
//     mints a fresh access token (cookie also rotated). No user
//     interaction required unless the cookie has expired.
//
// Public API:
//   litecode.api.baseUrl          — string, defaults to '/api/v1'
//   litecode.api.defaultTimeout   — number, ms, defaults to 20000
//   litecode.api.get/post/put/delete(path, body, opts)
//                                  → Promise<{data, request_id, raw}>
//   litecode.api.rawFetch(path, init)
//                                  → Promise<Response>  (no auto 401 handling)
//   litecode.api.sse(path, onEvent, onError)
//                                  → EventSource wrapper with Bearer query
//
//   litecode.auth.getAccessToken()        — string|null (memory only)
//   litecode.auth.hasRefreshCookie()      — boolean (heuristic — checks
//                                           that the cookie's *name* was
//                                           sent on a recent request;
//                                           cannot read the HttpOnly value)
//   litecode.auth.setAccessToken(token, user?)
//                                           — set on login/register/refresh
//   litecode.auth.clear()                 — drop access token + cached user
//   litecode.auth.fetchProfile()          — GET /auth/profile, cache result
//   litecode.auth.currentUser             — {id, username, role, ...} | null
//   litecode.auth.isLoggedIn              — bool
//   litecode.auth.isAdmin                 — bool
//   litecode.auth.onUnauthorized(cb)      — listener for forced sign-out
//   litecode.auth.onAuthChanged(cb)       — listener for any auth state change
//
//   litecode.auth.tryRefresh()            — Promise<{access_token,user}>
//                                           → POST /auth/refresh (cookie
//                                           path); returns the new access
//                                           token + user; throws on failure
//
//   litecode.api.onError(cb)              — listener for any API error toast
//
// Framework-internal event surface (for app.js / tests):
//   document.addEventListener('litecode:api-unauthorized', e => ...)
//   document.addEventListener('litecode:api-error',        e => ...)
//   document.addEventListener('litecode:auth-changed',     e => ...)
//
// Last review: Phase 5 ★ Token 存储 deliverable (cookie path).

(function (root) {
    'use strict';

    // ────────────────────────────────────────────────────────────────────
    //  Constants
    // ────────────────────────────────────────────────────────────────────

    // Refresh cookie NAME — kept in lock-step with the backend's
    // CookieConfig default (`lc_refresh`). The JS layer does NOT read
    // the value (it's HttpOnly) but it CAN detect that the cookie was
    // sent at all via a sentinel fetch with the path /api/v1/auth/*
    // — see hasRefreshCookie() below.
    var REFRESH_COOKIE_NAME    = 'lc_refresh';
    var STORAGE_KEY_USER       = 'litecode:user';
    var LOGIN_PATH             = '/login.html';

    var DEFAULT_BASE_URL       = '/api/v1';
    var DEFAULT_TIMEOUT_MS     = 20000;            // submit/refresh = 30s, see below
    var SUBMIT_TIMEOUT_MS      = 30000;
    var REFRESH_TIMEOUT_MS     = 30000;

    // ────────────────────────────────────────────────────────────────────
    //  LitecodeApiError — surfaced to callers; matches SPEC §5.7 envelope.
    //
    //  The backend sends:
    //    { "code": "INVALID_INPUT", "message": "...", "details": {...},
    //      "request_id": "uuid-v4" }
    //  Success sends:
    //    { "data": { ... }, "request_id": "uuid-v4" }
    //  Everything else is wrapped here so per-page code can branch on
    //  `instanceof LitecodeApiError && err.code === 'RATE_LIMITED'` etc.
    // ────────────────────────────────────────────────────────────────────

    function LitecodeApiError(status, payload, requestId) {
        var name = 'LitecodeApiError';
        var msg  = (payload && payload.message)
            ? String(payload.message)
            : ('HTTP ' + status);
        var code = (payload && payload.code) ? String(payload.code) : 'INTERNAL_ERROR';
        var details = (payload && payload.details !== undefined) ? payload.details : null;

        var err = new Error(msg);
        err.name        = name;
        err.status      = status;
        err.code        = code;
        err.details     = details;
        err.request_id  = requestId || (payload && payload.request_id) || null;
        err.isLitecodeError = true;
        return err;
    }

    // ────────────────────────────────────────────────────────────────────
    //  Access token storage — IN-MEMORY ONLY (Phase 5 ★ Token 存储)
    //
    //  We deliberately do NOT use sessionStorage / localStorage for the
    //  access token. A page reload wipes `accessToken` (variable below),
    //  forcing the SPA to call /auth/refresh (cookie path) on its next
    //  user-facing action — and that rehydrates a fresh access token
    //  without any user interaction.
    //
    //  Cached user (id/username/role) DOES survive reload via
    //  sessionStorage, because:
    //   - It contains no secret material
    //   - The nav bar needs to render BEFORE /auth/refresh completes
    //   - Stale data here only causes a wrong avatar/menu, never a
    //     privilege escalation — the access token's role claim is
    //     authoritative on every request
    // ────────────────────────────────────────────────────────────────────

    var accessToken = null;                  // Phase 5 ★: in-memory ONLY.

    function getAccessToken()  { return accessToken; }

    // Note: there is no `setRefreshToken` — the refresh is owned by
    // the browser's cookie jar. Calling code that thinks it has a
    // refresh token is now wrong; we delete the API surface so the
    // migration is grep-able.

    function storageGet(key) {
        try { return window.sessionStorage.getItem(key); }
        catch (_) { return null; }
    }
    function storageSet(key, value) {
        try { window.sessionStorage.setItem(key, value); }
        catch (_) { /* sessionStorage may be unavailable (private mode, quota); drop silently */ }
    }
    function storageRemove(key) {
        try { window.sessionStorage.removeItem(key); }
        catch (_) { /* same */ }
    }

    function setAccessToken(access, user) {
        accessToken = access || null;
        if (user !== undefined) {
            writeCachedUser(user);
        }
        emitAuthChanged();
    }

    function clearAuthLocal() {
        accessToken = null;
        storageRemove(STORAGE_KEY_USER);
        cachedUser = null;
        emitAuthChanged();
    }

    var cachedUser = null;
    function readCachedUser() {
        if (cachedUser) return cachedUser;
        var raw = storageGet(STORAGE_KEY_USER);
        if (!raw) return null;
        try { cachedUser = JSON.parse(raw); }
        catch (_) { cachedUser = null; }
        return cachedUser;
    }
    function writeCachedUser(user) {
        cachedUser = user || null;
        if (user) storageSet(STORAGE_KEY_USER, JSON.stringify(user));
        else      storageRemove(STORAGE_KEY_USER);
    }

    // Heuristic: did the browser actually send the refresh cookie on
    // the last /auth/refresh we made? We can't read the HttpOnly value,
    // but the backend's failure modes all return 401 — if the cookie
    // was sent AND valid, we got 200. Track the most recent outcome
    // and expose it via this getter. Used by `litecode.guard.requireAuth`
    // (in app.js) to decide whether to bounce the user to /login.html
    // BEFORE making a profile call.
    var lastRefreshSucceeded = false;
    function markRefreshSucceeded(ok) { lastRefreshSucceeded = !!ok; }
    function hasRefreshCookie()      { return lastRefreshSucceeded; }

    // ────────────────────────────────────────────────────────────────────
    //  Event helper — CustomEvent with a `detail` payload. Wrappers below
    //  plus a tiny pub/sub for the auth module keep cross-module glue
    //  free of hard imports.
    // ────────────────────────────────────────────────────────────────────

    function emit(name, detail) {
        try {
            window.dispatchEvent(new CustomEvent(name, { detail: detail || null }));
        } catch (_) { /* CustomEvent ctor may throw in old engines; drop */ }
    }
    function emitAuthChanged()    { emit('litecode:auth-changed', readCachedUser()); }
    function emitUnauthorized(reason) { emit('litecode:api-unauthorized', reason || null); }
    function emitApiError(err, meta) { emit('litecode:api-error', { err: err, meta: meta || null }); }

    // ────────────────────────────────────────────────────────────────────
    //  Refresh-once mutex — when 401 hits, only one in-flight refresh
    //  retries at a time; every other 401 attaches to the same promise.
    //  This prevents a stale token + a race from triggering N refreshes
    //  in a 100ms window (which the backend happily rate-limits).
    //
    //  Phase 5 ★: /auth/refresh itself takes NO body — the refresh
    //  token arrives via the HttpOnly cookie automatically. We still
    //  send `credentials: 'same-origin'` so the browser knows to
    //  include cookies on the cross-origin-friendly fetch.
    // ────────────────────────────────────────────────────────────────────

    var inflightRefresh = null;
    function baseUrl() { return ns.baseUrl(); }

    function refreshTokens() {
        if (inflightRefresh) return inflightRefresh;

        var ctrl = new AbortController();
        var timer = setTimeout(function () { ctrl.abort(); }, REFRESH_TIMEOUT_MS);

        // Phase 5 ★: body is empty. The HttpOnly cookie is the only
        // refresh source. We send credentials: 'same-origin' so the
        // browser attaches it; CORS preflight on /api/v1 already
        // permits credentials via the server's CORS policy.
        inflightRefresh = fetch(joinUrl(baseUrl(), '/auth/refresh'), {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept':       'application/json',
            },
            // Body intentionally omitted — the cookie carries the
            // refresh. We used to send `{refresh_token: refresh}` here
            // (Phase 2 storage in sessionStorage); Phase 5 deletes that.
            credentials: 'same-origin',
            signal: ctrl.signal,
        }).then(function (resp) {
            clearTimeout(timer);
            return resp.text().then(function (body) {
                var parsed = null;
                try { parsed = body ? JSON.parse(body) : null; } catch (_) {}
                if (!resp.ok || !parsed || !parsed.data || !parsed.data.access_token) {
                    markRefreshSucceeded(false);
                    throw LitecodeApiError(resp.status, parsed, parsed && parsed.request_id);
                }
                var data = parsed.data;
                // Set-Cookie was already written by the browser (we
                // can't see it from JS, but it's there). The body
                // ALSO carries a refresh_token — the server sends it
                // for back-compat with COOKIE_ALLOW_BODY_FALLBACK=true;
                // we deliberately ignore it here because the cookie
                // is the canonical source.
                setAccessToken(data.access_token, data.user || undefined);
                markRefreshSucceeded(true);
                return data.access_token;
            });
        }).catch(function (err) {
            clearTimeout(timer);
            markRefreshSucceeded(false);
            inflightRefresh = null;
            throw err;
        }).then(function (token) {
            inflightRefresh = null;
            return token;
        });

        return inflightRefresh;
    }

    // ────────────────────────────────────────────────────────────────────
    //  URL + header helpers
    // ────────────────────────────────────────────────────────────────────

    function joinUrl(base, path) {
        if (!path) return base;
        if (path.charAt(0) !== '/') path = '/' + path;
        return base + path;
    }

    // ────────────────────────────────────────────────────────────────────
    //  Network core — one fetch with timeout + envelope parsing
    //
    //  Returns: { ok, status, data, request_id, raw }
    //  Throws:  LitecodeApiError on non-2xx (so callers can `instanceof`
    //           branch without re-parsing the body)
    // ────────────────────────────────────────────────────────────────────

    function doFetch(path, init, opts) {
        var url = joinUrl(baseUrl(), path);

        var ctrl = (init && init.signal) ? null : new AbortController();
        var timeoutMs = (opts && opts.timeoutMs) || DEFAULT_TIMEOUT_MS;
        var timer = null;
        if (ctrl) {
            timer = setTimeout(function () { try { ctrl.abort(); } catch (_) {} }, timeoutMs);
        }

        var fetchInit = Object.assign({}, init || {});
        fetchInit.signal = (init && init.signal) || (ctrl && ctrl.signal) || null;

        // Always include credentials so the HttpOnly refresh cookie is
        // attached on every same-origin fetch. This is mandatory for
        // the cookie path to work — without it, /auth/refresh won't
        // see the cookie and we'll always 401.
        if (!fetchInit.credentials) fetchInit.credentials = 'same-origin';

        // Honour caller-supplied headers, but layer Authorization LAST so
        // a stale "Authorization: Bearer null" can't override ours.
        if (!fetchInit.headers) fetchInit.headers = {};
        var token = getAccessToken();
        if (token) {
            fetchInit.headers['Authorization'] = 'Bearer ' + token;
        }

        return fetch(url, fetchInit).then(function (resp) {
            if (timer) clearTimeout(timer);

            var requestId = resp.headers.get('X-Request-Id') || null;
            var ct        = resp.headers.get('Content-Type') || '';
            var looksJson = ct.indexOf('application/json') !== -1;

            return resp.text().then(function (body) {
                var parsed = null;
                if (body && looksJson) {
                    try { parsed = JSON.parse(body); } catch (_) {}
                }
                if (!resp.ok) {
                    throw LitecodeApiError(resp.status, parsed, requestId);
                }
                // Success envelope: { data, request_id? }
                if (parsed && typeof parsed === 'object' && 'data' in parsed) {
                    return {
                        ok: true,
                        status: resp.status,
                        data: parsed.data,
                        request_id: requestId || (parsed.request_id || null),
                        raw: resp,
                    };
                }
                // Non-envelope success — return the parsed body as `data`
                // anyway so call sites stay uniform.
                return {
                    ok: true,
                    status: resp.status,
                    data: parsed,
                    request_id: requestId,
                    raw: resp,
                };
            });
        }, function (err) {
            if (timer) clearTimeout(timer);
            if (err && err.name === 'AbortError') {
                throw LitecodeApiError(0, {
                    code: 'INTERNAL_ERROR',
                    message: 'request aborted (timeout or navigation)',
                }, null);
            }
            throw LitecodeApiError(0, {
                code: 'INTERNAL_ERROR',
                message: 'network failure: ' + (err && err.message ? err.message : 'unknown'),
            }, null);
        });
    }

    // ────────────────────────────────────────────────────────────────────
    //  Auth-aware fetch — wraps doFetch() with single-flight 401 retry.
    //  If the refresh itself fails (or there is no refresh cookie), the
    //  module clears storage, fires `litecode:api-unauthorized`, and
    //  hard-redirects to /login.html. The original caller's promise
    //  rejects with a LitecodeApiError either way.
    // ────────────────────────────────────────────────────────────────────

    var redirectInFlight = false;
    function forceSignOut(reason) {
        if (redirectInFlight) return;
        redirectInFlight = true;
        // Fire event BEFORE clearing so app.js can show a toast pointing
        // at the reason; listeners should also persist through the clear.
        emitUnauthorized(reason);
        clearAuthLocal();
        // Tiny defer so listeners can run before navigation tears down.
        var next = encodeURIComponent(window.location.pathname + window.location.search);
        setTimeout(function () {
            window.location.href = LOGIN_PATH + '?next=' + next;
        }, 50);
    }

    function fetchWithAutoRefresh(path, init, opts) {
        return doFetch(path, init, opts).catch(function (err) {
            var is401 = err && err.status === 401;
            var isNoRetry = opts && opts.noRetryOn401;
            if (!is401 || isNoRetry) throw err;

            // Avoid refresh loops on the refresh endpoint itself.
            if (path === '/auth/refresh' || path.indexOf('/auth/refresh') === 0) throw err;

            // Try the refresh exactly once.
            return refreshTokens().then(function () {
                // Replay the original request; the new access token is
                // already in memory so doFetch picks it up.
                var replayInit = Object.assign({}, init || {});
                return doFetch(path, replayInit, opts);
            }).catch(function (refreshErr) {
                // Refresh failed → force sign-out. This is the path
                // where the refresh cookie is missing / expired / revoked
                // — there is literally no way to recover without the
                // user logging in again.
                forceSignOut({
                    reason: 'refresh-failed',
                    original_status: err.status,
                    refresh_error: refreshErr && refreshErr.code,
                });
                throw err; // surface to caller too
            });
        });
    }

    // ────────────────────────────────────────────────────────────────────
    //  Public method helpers
    // ────────────────────────────────────────────────────────────────────

    function jsonBody(body) {
        if (body === undefined || body === null) return undefined;
        return JSON.stringify(body);
    }

    function doMethod(method, path, body, opts) {
        var init = { method: method, headers: {} };
        if (body !== undefined) {
            init.headers['Content-Type'] = 'application/json';
            init.body = jsonBody(body);
        }
        return fetchWithAutoRefresh(path, init, opts || {});
    }

    // ────────────────────────────────────────────────────────────────────
    //  rawFetch — escape hatch for SSE / streaming / binary. No 401 retry
    //  (the caller owns the response stream).
    // ────────────────────────────────────────────────────────────────────

    function rawFetch(path, init) {
        init = init || {};
        var token = getAccessToken();
        init.headers = Object.assign({}, init.headers || {});
        if (token && !init.headers['Authorization']) {
            init.headers['Authorization'] = 'Bearer ' + token;
        }
        if (!init.credentials) init.credentials = 'same-origin';
        return fetch(joinUrl(baseUrl(), path), init);
    }

    // ────────────────────────────────────────────────────────────────────
    //  SSE helper — EventSource can't send Authorization headers, so we
    //  fall back to `?access_token=...` query. The backend doesn't accept
    //  query tokens today, so this is a soft-fail; Phase 5+ would add a
    //  dedicated SSE handshake. For now this is here so the front-end
    //  has a typed anchor when submissions polling is upgraded to push.
    // ────────────────────────────────────────────────────────────────────

    function openSse(path, onEvent, onError) {
        var token = getAccessToken();
        var sep = path.indexOf('?') === -1 ? '?' : '&';
        var url = joinUrl(baseUrl(), path) + (token ? sep + 'access_token=' + encodeURIComponent(token) : '');
        var es = new EventSource(url, { withCredentials: true });
        if (onEvent) es.addEventListener('result', function (ev) {
            try { onEvent(JSON.parse(ev.data)); } catch (e) { onEvent(ev.data); }
        });
        if (onError) es.addEventListener('error', onError);
        return es;
    }

    // ────────────────────────────────────────────────────────────────────
    //  Module surface
    // ────────────────────────────────────────────────────────────────────

    var ns = {
        LitecodeApiError: LitecodeApiError,

        baseUrl:        function () { return DEFAULT_BASE_URL; },
        defaultTimeout: DEFAULT_TIMEOUT_MS,
        submitTimeout:  SUBMIT_TIMEOUT_MS,

        // CRUD
        get:    function (path, opts) { return doMethod('GET',    path, undefined, opts); },
        delete: function (path, opts) { return doMethod('DELETE', path, undefined, opts); },
        post:   function (path, body, opts) { return doMethod('POST',   path, body, opts); },
        put:    function (path, body, opts) { return doMethod('PUT',    path, body, opts); },
        patch:  function (path, body, opts) { return doMethod('PATCH',  path, body, opts); },

        // Escapes
        rawFetch: rawFetch,
        sse:      openSse,

        // Auth namespace — see api.auth below for the canonical set.
        // Filled in after the auth object is defined so we can hoist.
        auth: null,

        // Error wiring — register a global notifier (toast) without
        // forcing a hard dependency on app.js.
        onError: function (cb) {
            function listener(e) { cb(e.detail); }
            window.addEventListener('litecode:api-error', listener);
            return function () { window.removeEventListener('litecode:api-error', listener); };
        },
    };

    // ────────────────────────────────────────────────────────────────────
    //  Auth namespace
    // ────────────────────────────────────────────────────────────────────

    var auth = {
        getAccessToken: getAccessToken,
        hasRefreshCookie: hasRefreshCookie,
        setAccessToken: setAccessToken,
        clear:           clearAuthLocal,

        currentUser:     null,                 // populated lazily
        isLoggedIn: function () { return !!getAccessToken(); },
        isAdmin:    function () {
            var u = this.currentUser;
            return !!(u && u.role === 'admin');
        },

        // GET /auth/profile — populates currentUser + emits auth-changed.
        // Throws LitecodeApiError on auth failure; the auto-refresh in
        // fetchWithAutoRefresh already handles 401.
        fetchProfile: function () {
            var self = this;
            return fetchWithAutoRefresh('/auth/profile', undefined, { noRetryOn401: false })
                .then(function (resp) {
                    var user = (resp && resp.data && resp.data.user) || null;
                    writeCachedUser(user);
                    self.currentUser = user;
                    emitAuthChanged();
                    return user;
                });
        },

        // Phase 5 ★ tryRefresh() — issue a single /auth/refresh call
        // (cookie path). Returns the new access token + user; throws
        // LitecodeApiError on failure (no cookie, cookie expired,
        // cookie revoked, etc). Useful for boot-time rehydration:
        //
        //   litecode.boot.shell = function () {
        //     return litecode.auth.tryRefresh()
        //       .then(function () { return litecode.auth.fetchProfile(); })
        //       .catch(function () { /* not signed in; show guest nav */ });
        //   };
        //
        tryRefresh: function () {
            return refreshTokens().then(function (token) {
                return { access_token: token };
            });
        },

        // Listener for forced sign-out (token-stolen, refresh-broken, etc.).
        onUnauthorized: function (cb) {
            function listener(e) { cb(e.detail); }
            window.addEventListener('litecode:api-unauthorized', listener);
            return function () { window.removeEventListener('litecode:api-unauthorized', listener); };
        },

        // Subscribe to ANY auth state change (login, logout, role swap).
        onAuthChanged: function (cb) {
            function listener() { cb(readCachedUser()); }
            window.addEventListener('litecode:auth-changed', listener);
            return function () { window.removeEventListener('litecode:auth-changed', listener); };
        },

        // High-level helpers — every page should call these, never build
        // the body themselves. Keeps request shapes in lock-step with
        // auth_routes.h's parse_register_request / parse_login_request.
        register: function (username, password, email) {
            var body = { username: username, password: password };
            if (email) body.email = email;
            return fetchWithAutoRefresh('/auth/register', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body),
                credentials: 'same-origin',
            }, { noRetryOn401: true }).then(function (resp) {
                var d = resp.data || {};
                // The server stamped Set-Cookie lc_refresh=<refresh>; we
                // can't read it from JS, but it's now in the cookie jar.
                // We only need to stash the access token + user.
                setAccessToken(d.access_token || null, d.user || null);
                markRefreshSucceeded(true);
                return d;
            });
        },

        login: function (username, password) {
            return fetchWithAutoRefresh('/auth/login', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ username: username, password: password }),
                credentials: 'same-origin',
            }, { noRetryOn401: true }).then(function (resp) {
                var d = resp.data || {};
                setAccessToken(d.access_token || null, d.user || null);
                markRefreshSucceeded(true);
                return d;
            });
        },

        logout: function () {
            // Best-effort: ask the server to revoke + clear the cookie.
            // We swallow network errors; the local clear+redirect runs
            // regardless. credentials: 'same-origin' so the cookie jar
            // is sent (though /auth/logout is Bearer-gated, not cookie-
            // gated, the cookie's Max-Age=0 is emitted by the server in
            // response to ANY authenticated request and we want the
            // browser to honor it).
            var p = Promise.resolve();
            var token = getAccessToken();
            if (token) {
                p = fetch(joinUrl(baseUrl(), '/auth/logout'), {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                        'Authorization': 'Bearer ' + token,
                    },
                    body: JSON.stringify({}),   // body unused — refresh in cookie
                    credentials: 'same-origin',
                }).catch(function () { /* swallow */ });
            }
            return p.then(function () {
                clearAuthLocal();
                markRefreshSucceeded(false);
                emitUnauthorized({ reason: 'logout' });
                return true;
            });
        },
    };

    // Hydrate currentUser from storage so the nav can render an avatar
    // IMMEDIATELY on the next page render before fetchProfile() returns.
    // Note: accessToken is NOT hydrated — that's the whole point of
    // Phase 5 ★; the SPA issues tryRefresh() on boot to mint a fresh one.
    try { auth.currentUser = readCachedUser(); } catch (_) { auth.currentUser = null; }

    ns.auth = auth;
    root.litecode = root.litecode || {};
    root.litecode.api = ns;

    // ────────────────────────────────────────────────────────────────────
    //  Removed APIs (Phase 5 ★ migration notes):
    //
    //  - litecode.auth.getRefreshToken / setTokens with refresh arg
    //    were deleted. The refresh token is no longer in JS — it's
    //    only in the HttpOnly cookie. Any pre-Phase-5 caller that did
    //    `api.auth.getRefreshToken()` will now get `null`; replace
    //    with `api.auth.tryRefresh()` (returns the access token +
    //    user, throws on failure).
    //
    //  - `STORAGE_KEY_REFRESH` (`litecode:refresh_token` in
    //    sessionStorage) is no longer written. Existing entries from
    //    pre-Phase-5 sessions are inert — the server ignores the
    //    body.refresh_token when the cookie is present (Phase 5 ★
    //    cookie has priority), so the stale sessionStorage value is
    //    harmless. We don't delete them to avoid touching unrelated
    //    browser state; they'll be GC'd when the user clears site
    //    data.
    // ────────────────────────────────────────────────────────────────────
})(window);