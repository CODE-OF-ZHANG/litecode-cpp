// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Frontend API wrapper (Phase 5 ★ 前端框架 / A32)
//
// One-file module that owns every cross-cutting HTTP concern Phase 5
// promises:
//   1. `litecode.api.{get,post,put,delete}` — opinionated fetch wrapper
//   2. `litecode.api.rawFetch(...)`         — escape hatch for SSE / multipart
//   3. Bearer-token attachment from sessionStorage
//   4. Single-flight refresh-on-401 with one-shot retry, hard-redirect
//      to /login.html when refresh also fails
//   5. SPEC §5.7 unified error envelope surfacing — `LitecodeApiError`
//      subclass with `{status, code, message, details, request_id}`
//   6. `DispatchEvent` on `document`: `litecode:api-error` /
//      `litecode:api-unauthorized` so the toast layer (app.js) can
//      react without touching every call site
//
// Storage strategy (Phase 5 ★ vs. SPEC §6.3):
//   SPEC §6.3 wants access in memory + refresh in HttpOnly Secure cookie.
//   /api/v1/auth/login returns JSON only today; no Set-Cookie. Until the
//   backend starts issuing a HttpOnly cookie and /refresh learns to read
//   from it, the frontend stores BOTH tokens in sessionStorage
//   (tab-scope). Both are wiped on tab close, which is a downgrade from
//   the SPEC §6.3 ideal but acceptable for the MVP SPA. Migration is a
//   future Phase 5/6 follow-up — see TODO at the bottom of this file.
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
//   litecode.auth.getAccessToken()        — string|null
//   litecode.auth.getRefreshToken()       — string|null
//   litecode.auth.setTokens(access, refresh, user?)
//   litecode.auth.clear()                 — drop both tokens + cached user
//   litecode.auth.fetchProfile()          — GET /auth/profile, cache result
//   litecode.auth.currentUser             — {id, username, role, ...} | null
//   litecode.auth.isLoggedIn              — bool
//   litecode.auth.isAdmin                 — bool
//   litecode.auth.onUnauthorized(cb)      — listener for forced sign-out
//
//   litecode.api.onError(cb)              — listener for any API error toast
//
// Framework-internal event surface (for app.js / tests):
//   document.addEventListener('litecode:api-unauthorized', e => ...)
//   document.addEventListener('litecode:api-error',        e => ...)
//   document.addEventListener('litecode:auth-changed',     e => ...)
//
// Last review: Phase 5 ★ 前端框架 deliverable.

(function (root) {
    'use strict';

    // ────────────────────────────────────────────────────────────────────
    //  Constants
    // ────────────────────────────────────────────────────────────────────

    var STORAGE_KEY_ACCESS  = 'litecode:access_token';
    var STORAGE_KEY_REFRESH = 'litecode:refresh_token';
    var STORAGE_KEY_USER    = 'litecode:user';
    var LOGIN_PATH          = '/login.html';

    var DEFAULT_BASE_URL    = '/api/v1';
    var DEFAULT_TIMEOUT_MS  = 20000;            // submit/refresh = 30s, see below
    var SUBMIT_TIMEOUT_MS   = 30000;
    var REFRESH_TIMEOUT_MS  = 30000;

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
    //  Storage layer — sessionStorage only. Tokens are wiped on tab close.
    // ────────────────────────────────────────────────────────────────────

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

    function getAccessToken()  { return storageGet(STORAGE_KEY_ACCESS); }
    function getRefreshToken() { return storageGet(STORAGE_KEY_REFRESH); }

    function setTokens(access, refresh, user) {
        if (access)  storageSet(STORAGE_KEY_ACCESS, access);
        if (refresh) storageSet(STORAGE_KEY_REFRESH, refresh);
        if (user !== undefined) {
            if (user === null) storageRemove(STORAGE_KEY_USER);
            else storageSet(STORAGE_KEY_USER, JSON.stringify(user));
        }
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
    // ────────────────────────────────────────────────────────────────────

    var inflightRefresh = null;
    function baseUrl() { return ns.baseUrl(); }

    function refreshTokens() {
        if (inflightRefresh) return inflightRefresh;

        var refresh = getRefreshToken();
        if (!refresh) return Promise.reject(new Error('no refresh token'));

        var ctrl = new AbortController();
        var timer = setTimeout(function () { ctrl.abort(); }, REFRESH_TIMEOUT_MS);

        inflightRefresh = fetch(joinUrl(baseUrl(), '/auth/refresh'), {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Accept':       'application/json',
            },
            body: JSON.stringify({ refresh_token: refresh }),
            credentials: 'same-origin',
            signal: ctrl.signal,
        }).then(function (resp) {
            clearTimeout(timer);
            return resp.text().then(function (body) {
                var parsed = null;
                try { parsed = body ? JSON.parse(body) : null; } catch (_) {}
                if (!resp.ok || !parsed || !parsed.data || !parsed.data.access_token) {
                    throw LitecodeApiError(resp.status, parsed, parsed && parsed.request_id);
                }
                var data = parsed.data;
                setTokens(data.access_token, data.refresh_token || refresh, data.user || undefined);
                return data.access_token;
            });
        }).catch(function (err) {
            clearTimeout(timer);
            // Clear refresh token on any refresh failure — next 401
            // will hard-redirect instead of looping forever.
            storageRemove(STORAGE_KEY_REFRESH);
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

        // Always include credentials so refresh-cookie migration (future)
        // can flip `same-origin` → `include` without touching call sites.
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
    //  If the refresh itself fails (or there is no refresh token), the
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

    function clearAuthLocal() {
        storageRemove(STORAGE_KEY_ACCESS);
        storageRemove(STORAGE_KEY_REFRESH);
        writeCachedUser(null);
        emitAuthChanged();
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
                // already in storage so doFetch picks it up.
                var replayInit = Object.assign({}, init || {});
                return doFetch(path, replayInit, opts);
            }).catch(function (refreshErr) {
                // Refresh failed → force sign-out.
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
        getAccessToken:  getAccessToken,
        getRefreshToken: getRefreshToken,
        setTokens:       setTokens,
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
            }, { noRetryOn401: true }).then(function (resp) {
                var d = resp.data || {};
                setTokens(d.access_token || null, d.refresh_token || null, d.user || null);
                return d;
            });
        },

        login: function (username, password) {
            return fetchWithAutoRefresh('/auth/login', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ username: username, password: password }),
            }, { noRetryOn401: true }).then(function (resp) {
                var d = resp.data || {};
                setTokens(d.access_token || null, d.refresh_token || null, d.user || null);
                return d;
            });
        },

        logout: function () {
            var rt = getRefreshToken();
            // Best-effort: ask the server to revoke. We swallow network
            // errors; the local clear+redirect runs regardless.
            var p = Promise.resolve();
            if (rt) {
                p = fetch(joinUrl(baseUrl(), '/auth/logout'), {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ refresh_token: rt }),
                    credentials: 'same-origin',
                }).catch(function () { /* swallow */ });
            }
            return p.then(function () {
                clearAuthLocal();
                emitUnauthorized({ reason: 'logout' });
                return true;
            });
        },
    };

    // Hydrate currentUser from storage so the nav can render an avatar
    // IMMEDIATELY on the next page render before fetchProfile() returns.
    try { auth.currentUser = readCachedUser(); } catch (_) { auth.currentUser = null; }

    ns.auth = auth;
    root.litecode = root.litecode || {};
    root.litecode.api = ns;

    // ────────────────────────────────────────────────────────────────────
    //  TODO — Phase 5/6 follow-ups not in scope of "前端框架" deliverable:
    //   1. Move refresh to HttpOnly; Secure; SameSite=Strict cookie.
    //      Needs backend:
    //        (a) /auth/login + /auth/register response: Set-Cookie header
    //        (b) /auth/refresh: read refresh_token from cookie OR body
    //        (c) /auth/logout: clear the cookie
    //   2. Wire SSE auth: today EventSource can't set Authorization; the
    //      openSse() helper above uses a query-string fallback which the
    //      backend does NOT currently accept.
    //   3. Multi-tab sync via the `storage` event so a logout in one tab
    //      signs out the others. Handled today by the eventual redirect.
    // ────────────────────────────────────────────────────────────────────
})(window);
