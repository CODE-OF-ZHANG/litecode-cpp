// SPDX-License-Identifier: MIT
//
// web/js/mouse-deco.js — 鼠标跟随装饰层驱动
//
// v1.3.4 PR 8 ★ 视觉提升 / SPEC §6.3
// 把鼠标位置归一化写到 :root 的 CSS 变量,让 .lc-v4-bg-aurora /
// .lc-v4-bg-oat / .lc-v4-spotlight / .lc-v4-deco--{tl,tr,bl,br} /
// .lc-v4-hero-mock 这 5 类装饰元素 GPU 加速跟随;rAF 节流到 ≤ 60fps。
//
// 约束:
//   - 移动端 ((pointer: coarse)) 直接禁用(无 hover 语义)
//   - prefers-reduced-motion: reduce 禁用(无装饰动画)
//   - 全部 passive 监听 + transform/opacity(不触发 layout)
//   - 鼠标离开视口 200ms 后移除 .lc-mouse-active(让 spotlight 渐隐)
//
// Deps: 无。Page consumers 在 HTML 末尾 <script src="/js/mouse-deco.js"
// defer> 引入(同 app.js / csp.js / api.js 的方式,不走 litecode.csp
// makeScript — 这 4 个是基础设施,不需要 SRI)。
(function (ns) {
    'use strict';

    // ── Feature gating ──────────────────────────────────────────
    // Touch-only 设备没有"鼠标在哪儿"的概念;reduced-motion 用户
    // 不希望装饰元素动。两者都直接 return,所有 CSS 的 fallback
    // (var(--lc-mouse-x) 默认 0) 生效 = 元素停在视口中心不偏移。
    if (window.matchMedia) {
        var coarseMq = window.matchMedia('(pointer: coarse)');
        if (coarseMq && coarseMq.matches) return;
        var reducedMq = window.matchMedia('(prefers-reduced-motion: reduce)');
        if (reducedMq && reducedMq.matches) return;
    }

    var docEl = document.documentElement;
    var docBody = document.body;
    var vw = 0, vh = 0;
    var lastX = 0, lastY = 0;
    var rafPending = false;
    var activeTimer = null;

    // ── Viewport size (cached) ─────────────────────────────────
    // 鼠标坐标要归一化到 [-1, 1],0 = 视口中心。视口尺寸在 resize
    // 时刷新(节流 200ms)。多数时候视口不变,addEventListener
    // 是 debounce 性质的开销。
    function measureViewport() {
        vw = window.innerWidth  || docEl.clientWidth  || 0;
        vh = window.innerHeight || docEl.clientHeight || 0;
    }
    measureViewport();
    var resizeTimer = null;
    window.addEventListener('resize', function () {
        if (resizeTimer) return;
        resizeTimer = setTimeout(function () {
            resizeTimer = null;
            measureViewport();
        }, 200);
    }, { passive: true });

    // ── rAF 节流写变量 ──────────────────────────────────────────
    // pointermove 触发频率跟鼠标 DPI / OS 加速有关,通常是 60-120Hz。
    // 浏览器一帧只需要 1 次变量赋值,所以 rAF 合并即可;不需要额外
    // 的 throttle/debounce 库。
    function flush() {
        rafPending = false;
        if (!vw || !vh) return;
        // 中心归一化:鼠标在视口左上 = (-1, -1),右下 = (+1, +1)
        var nx = (lastX / vw) * 2 - 1;
        var ny = (lastY / vh) * 2 - 1;
        // 写 CSS 变量。设到 :root 让 token cascade 同步生效。
        docEl.style.setProperty('--lc-mouse-x', nx.toFixed(4));
        docEl.style.setProperty('--lc-mouse-y', ny.toFixed(4));
        // spotlight 直接用 px 坐标(避免让 CSS 算 calc)
        docEl.style.setProperty('--lc-spotlight-x', lastX + 'px');
        docEl.style.setProperty('--lc-spotlight-y', lastY + 'px');
    }
    function scheduleFlush() {
        if (rafPending) return;
        rafPending = true;
        requestAnimationFrame(flush);
    }

    // ── pointermove 监听 ───────────────────────────────────────
    // passive: true — 永远不 preventDefault,避免阻塞主线程 scroll。
    // 只在 mousedown/pointerdown/click 主动交互时才需要 non-passive。
    function onMove(x, y) {
        lastX = x;
        lastY = y;
        // spotlight 渐显:首次 move 时加 class,后续维持;离开视口
        // 200ms 后移除 class(让 CSS 400ms opacity transition 渐隐)
        if (docBody && !docBody.classList.contains('lc-mouse-active')) {
            docBody.classList.add('lc-mouse-active');
        }
        if (activeTimer) {
            clearTimeout(activeTimer);
            activeTimer = null;
        }
        scheduleFlush();
    }
    function onLeave() {
        if (activeTimer) clearTimeout(activeTimer);
        activeTimer = setTimeout(function () {
            if (docBody) docBody.classList.remove('lc-mouse-active');
            // 鼠标归零(让 4 角装饰平滑回中)
            lastX = vw / 2;
            lastY = vh / 2;
            docEl.style.setProperty('--lc-mouse-x', '0');
            docEl.style.setProperty('--lc-mouse-y', '0');
        }, 200);
    }
    window.addEventListener('pointermove', function (e) {
        onMove(e.clientX, e.clientY);
    }, { passive: true });
    window.addEventListener('pointerleave', onLeave, { passive: true });
    window.addEventListener('blur', onLeave, { passive: true });

    // ── Hero 3D tilt(可选,仅首页 .lc-v4-hero-mock) ────────────
    // 单元素级监听 — 鼠标在卡片范围内时,根据鼠标相对卡片中心的
    // 偏移算 tilt 角度写到 --lc-tilt-x / --lc-tilt-y,CSS 端
    // perspective(800px) rotateY/X 6deg 跟随。querySelectorAll
    // 一次拿全,以后 DOM 变也无需重新绑(只在初次扫一次)。
    function bindTilt(el) {
        if (!el) return;
        el.addEventListener('pointermove', function (e) {
            var rect = el.getBoundingClientRect();
            var dx = (e.clientX - rect.left) / rect.width  * 2 - 1;
            var dy = (e.clientY - rect.top)  / rect.height * 2 - 1;
            el.style.setProperty('--lc-tilt-x', dy.toFixed(3));
            el.style.setProperty('--lc-tilt-y', dx.toFixed(3));
        }, { passive: true });
        el.addEventListener('pointerleave', function () {
            el.style.setProperty('--lc-tilt-x', '0');
            el.style.setProperty('--lc-tilt-y', '0');
        }, { passive: true });
    }
    function initTilt() {
        var nodes = document.querySelectorAll('.lc-v4-hero-mock');
        for (var i = 0; i < nodes.length; i++) bindTilt(nodes[i]);
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initTilt, { once: true });
    } else {
        initTilt();
    }

    // Public surface (debug only — no real consumers right now)
    ns.mouseDeco = {
        flush: flush,
        bindTilt: bindTilt,
    };
}(window.litecode = window.litecode || {}));
