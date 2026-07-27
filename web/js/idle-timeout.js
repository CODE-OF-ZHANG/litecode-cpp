// SPDX-License-Identifier: MIT
//
// web/js/idle-timeout.js — 1 小时无操作自动登出
//
// v1.3.4 PR 10 ★ 严格登录态 / SPEC §5.1
// 配合后端 JWT access_ttl = 1h (config.h),前端主动检测 idle:
//   - 监听 mousedown / keydown / touchstart / scroll (passive) +
//     visibilitychange(切回标签页重置)
//   - 距 lastActivity > (timeoutMs - warnBeforeMs) → 弹模态
//     "还剩 X 分钟,是否继续?"
//   - 模态有 [继续] (重置 lastActivity) / [立即登出] (清 token + 跳登录页)
//   - 距 lastActivity > timeoutMs → 自动 logout: clearTokens +
//     BroadcastChannel 通知其他 tab + 跳 /login.html?reason=timeout
//   - 收到 BroadcastChannel('litecode-auth', 'logout') 也跳登录页
//
// 跨标签页用 BroadcastChannel,降级到 storage event (同源限制)。
// 移动端 ((pointer: coarse)) 仍然启用,但提示文案更克制。
// reduced-motion 不影响功能,只影响 transition。
//
// 调用方式(defer 加载即可,DOMContentLoaded 自动启动):
//   <script src="/js/idle-timeout.js?v=1.3.4" defer></script>
// 自动启:有 currentUser 时启动,没有则不启动(公开页面无副作用)。
//
// Public surface:
//   litecode.idleTimeout.start({ timeoutMs, warnBeforeMs })
//   litecode.idleTimeout.stop()
//   litecode.idleTimeout.reset()
(function (ns) {
    'use strict';

    // ── 默认值 / 可配置 ─────────────────────────────────────────
    var DEFAULTS = {
        timeoutMs:    60 * 60 * 1000,  // 1h(跟后端 access_ttl 一致)
        warnBeforeMs: 5 * 60 * 1000,   // 最后 5min 弹窗
        checkEveryMs: 30 * 1000,        // 每 30s 检查一次
    };
    var state = {
        running:      false,
        timeoutMs:    DEFAULTS.timeoutMs,
        warnBeforeMs: DEFAULTS.warnBeforeMs,
        checkTimer:   null,
        lastActivity: Date.now(),
        warnEl:       null,
        bc:           null,
        bcFallback:   null,
    };

    // ── DOM 注入:模态(只创建一次) ─────────────────────────────
    function ensureModal() {
        if (state.warnEl) return state.warnEl;
        var div = document.createElement('div');
        div.className = 'lc-modal';
        div.id = 'idle-timeout-modal';
        div.setAttribute('role', 'dialog');
        div.setAttribute('aria-modal', 'true');
        div.setAttribute('aria-labelledby', 'idle-timeout-title');
        div.hidden = true;
        div.innerHTML = [
            '<div class="lc-modal__backdrop" data-act="idle-logout"></div>',
            '<div class="lc-modal__panel" role="document" style="max-width: 440px;">',
            '  <h2 id="idle-timeout-title" style="margin: 0 0 8px; font-size: 18px;">',
            '    即将登出',
            '  </h2>',
            '  <p class="lc-form-help" id="idle-timeout-body" style="margin-bottom: 16px;">',
            '    你已经 1 小时没有操作了,即将自动登出。',
            '  </p>',
            '  <div class="lc-row lc-row--end" style="gap: 8px;">',
            '    <button type="button" class="lc-btn lc-btn--ghost" data-act="idle-logout">立即登出</button>',
            '    <button type="button" class="lc-btn lc-btn--primary" data-act="idle-stay">继续会话</button>',
            '  </div>',
            '</div>',
        ].join('\n');
        document.body.appendChild(div);
        div.addEventListener('click', function (e) {
            var t = e.target;
            while (t && t !== document.body) {
                if (!t.dataset) { t = t.parentNode; continue; }
                if (t.dataset.act === 'idle-stay')  { hideModal(); reset(); return; }
                if (t.dataset.act === 'idle-logout'){ forceLogout(); return; }
                t = t.parentNode;
            }
        });
        state.warnEl = div;
        return div;
    }
    function showModal(remainingMin) {
        var m = ensureModal();
        var body = document.getElementById('idle-timeout-body');
        if (body) {
            body.textContent = '你已 1 小时未操作,还有 ' + remainingMin +
                ' 分钟即将自动登出,是否继续?';
        }
        m.hidden = false;
    }
    function hideModal() {
        if (state.warnEl) state.warnEl.hidden = true;
    }

    // ── 真正的 logout:清 token + 通知多 tab + 跳登录 ───────────
    // 不调 auth.logout() 因为它会发 POST /auth/logout,网络不通/服务器
    // hang 时会卡住。idle 超时是"用户已离开很久"的场景,best-effort
    // 清理即可:清 token / 跳登录页。cookie 由后端在 1h access TTL 后
    // 自动随 refresh 校验失败清除(Phase 5 ★ cookie 路径已对齐)。
    function forceLogout() {
        try {
            if (window.litecode && litecode.api && litecode.api.auth) {
                // 清 access token(直接覆盖 sessionStorage 占位 key)
                try { sessionStorage.removeItem('litecode.access_token'); } catch (_) {}
                // 清当前 user 缓存 + 触发 auth-changed 事件
                if (typeof litecode.api.auth.currentUser !== 'undefined') {
                    litecode.api.auth.currentUser = null;
                }
                if (typeof litecode.api.auth.clearAccessToken === 'function') {
                    litecode.api.auth.clearAccessToken();
                }
                if (typeof litecode.api.auth.invalidateCache === 'function') {
                    try { litecode.api.auth.invalidateCache(); } catch (_) {}
                }
            }
        } catch (_) { /* best effort */ }

        // 通知其他 tab
        try {
            if (state.bc) {
                state.bc.postMessage({ type: 'logout', reason: 'idle' });
            }
        } catch (_) { /* best effort */ }

        // 跳登录页
        var next = window.location.pathname + window.location.search;
        window.location.replace('/login.html?reason=timeout&next=' +
            encodeURIComponent(next));
    }

    // ── BroadcastChannel + storage fallback ──────────────────────
    function setupChannel() {
        if (typeof BroadcastChannel === 'function') {
            try {
                state.bc = new BroadcastChannel('litecode-auth');
                state.bc.addEventListener('message', function (e) {
                    var msg = e && e.data;
                    if (msg && msg.type === 'logout') {
                        // 别的 tab 已登出,本 tab 也跳登录
                        try {
                            if (window.litecode && litecode.api && litecode.api.auth) {
                                try { sessionStorage.removeItem('litecode.access_token'); } catch (_) {}
                                if (typeof litecode.api.auth.currentUser !== 'undefined') {
                                    litecode.api.auth.currentUser = null;
                                }
                            }
                        } catch (_) {}
                        var next = window.location.pathname + window.location.search;
                        window.location.replace('/login.html?reason=timeout&next=' +
                            encodeURIComponent(next));
                    }
                });
                return;
            } catch (_) { /* fall through */ }
        }
        // Fallback:storage 事件。同源 tab 写 sessionStorage 触发
        // 'storage' 事件;但 storage 事件在写自己的 tab 不触发,所以
        // 触发"通知其他 tab"靠 BroadcastChannel;这里只做"被通知"。
        state.bcFallback = true;
    }

    // ── Activity tracking ───────────────────────────────────────
    function onActivity() {
        state.lastActivity = Date.now();
    }
    function attachActivityListeners() {
        // passive 监听避免阻塞 scroll。capture: false 即可。
        var events = ['mousedown', 'keydown', 'touchstart', 'scroll', 'wheel', 'pointerdown'];
        events.forEach(function (ev) {
            window.addEventListener(ev, onActivity, { passive: true });
        });
        // visibilitychange:切回标签页重置(用户可见时不该立即踢)
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) onActivity();
        });
    }

    // ── Check loop ──────────────────────────────────────────────
    function tick() {
        if (!state.running) return;
        var now = Date.now();
        var idle = now - state.lastActivity;
        if (idle >= state.timeoutMs) {
            // 强制登出
            hideModal();
            forceLogout();
            return;
        }
        if (idle >= state.timeoutMs - state.warnBeforeMs) {
            // 弹模态(只在 idle 跨过阈值那一次弹,后续 tick 不再覆盖文案)
            if (state.warnEl && state.warnEl.hidden !== false) {
                var remainMs = state.timeoutMs - idle;
                var remainMin = Math.max(1, Math.ceil(remainMs / 60000));
                showModal(remainMin);
            }
        }
    }

    // ── Public API ──────────────────────────────────────────────
    function start(opts) {
        if (state.running) return;
        opts = opts || {};
        state.timeoutMs    = opts.timeoutMs    || DEFAULTS.timeoutMs;
        state.warnBeforeMs = opts.warnBeforeMs || DEFAULTS.warnBeforeMs;
        state.lastActivity = Date.now();

        // 仅在登录状态下启用(公开页面无意义)
        var logged = !!(window.litecode && litecode.api &&
                        litecode.api.auth && litecode.api.auth.isLoggedIn &&
                        litecode.api.auth.isLoggedIn());
        if (!logged) return;

        attachActivityListeners();
        setupChannel();
        // 先不显示 modal,只是 setInterval
        state.checkTimer = setInterval(tick, DEFAULTS.checkEveryMs);
        state.running = true;

        // ESC 关模态(同时 = 继续会话)
        document.addEventListener('keydown', function (e) {
            if (state.warnEl && !state.warnEl.hidden && e.key === 'Escape') {
                hideModal();
                reset();
            }
        });
    }
    function stop() {
        if (!state.running) return;
        clearInterval(state.checkTimer);
        state.checkTimer = null;
        state.running = false;
        hideModal();
    }
    function reset() {
        state.lastActivity = Date.now();
        hideModal();
    }

    ns.idleTimeout = {
        start: start,
        stop:  stop,
        reset: reset,
    };

    // ── Auto-start on DOMContentLoaded(如果有 currentUser) ───────
    function autoStart() {
        try {
            var logged = !!(window.litecode && litecode.api &&
                            litecode.api.auth && litecode.api.auth.isLoggedIn &&
                            litecode.api.auth.isLoggedIn());
            if (logged) start();
        } catch (_) { /* best effort */ }
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', autoStart, { once: true });
    } else {
        // defer 加载,可能 readyState 已经是 interactive
        setTimeout(autoStart, 0);
    }
}(window.litecode = window.litecode || {}));
