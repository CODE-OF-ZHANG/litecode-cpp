// SPDX-License-Identifier: MIT
//
// web/js/mouse-particles.js — 鼠标引力场粒子拖尾 (v1.3.4 PR 11 ★)
//
// v1.3.4 PR 13 ★ 多 canvas 模式(Bug 1 收口)
//   之前只有 #lc-particles-canvas(viewport 全屏 fixed,z-index:0),
//   .lc-v4-hero / .lc-problem-filters / .lc-rank-table / #profile-content
//   这 4 个**带 opaque 背景**的容器直接把粒子挡掉了(浅色下 multiply
//   还能透一点,深色下完全不可见)。
//
//   现在的实现:
//     · 一个全局 canvas (#lc-particles-canvas) — 全屏,密度高,
//       给页面"未分区"区域铺氛围。
//     · 任意数量的 scoped canvas (.lc-particles-canvas-scoped) —
//       绝对定位 inset:0 + z-index:0 + pointer-events:none,嵌在
//       目标容器内,**渲染在容器背景之上、容器内容之下**。粒子更
//       稀、更小、alpha 更低(0.18 / 0.30,无 glow),绝不挡文字。
//
//   每个 canvas 有独立 measure / resize / 粒子系统;resize 时各自
//   重置 base 位置。鼠标位置全局共享,所有 canvas 的粒子都被同一个
//   鼠标吸引(像同一片水域的多块画布)。
//
// 与 web/js/mouse-deco.js 协作:
//
//   mouse-deco.js 在 :root 上写 --lc-mouse-x / --lc-mouse-y (归一化到 [-1, 1])
//   + --lc-spotlight-x / --lc-spotlight-y (视口像素坐标),rAF 节流到 60fps,
//   负责 spotlight / 4 角装饰 / hero tilt。
//
//   本模块 **不再监听 pointermove**(避免与 mouse-deco 抢主线程 +
//   双 rAF 链),而是每帧从 :root 的 CSS 变量读鼠标位置,把粒子
//   的位置 / 颜色 / 连线画到每个 canvas 上。
//
// 粒子动力学 (v1.3.4 PR12 ★ 双套 config):
//   - 深色 / 浅色两套独立参数,各自粒子数 + 引力 + 漂移 + 连线 + 尺寸 + glow
//     全部不同,深色多一点大一点(亮对暗扩散强)。
//   - scoped canvas 在全局配置基础上 N/2,sizeMax 收窄,glowMul 0(不发光)。
//   - 距鼠标 < mouseInfluence 时,受引力 force 拉向鼠标
//   - 两粒子距离 < lineDist 时画半透明连线 (alpha = 1 - dist/lineDist)
//   - 鼠标附近的粒子 alpha 提升 + size 翻倍
//   - globalCompositeOperation = 'source-over'(alpha 上限 1.0)
//
// Feature gate (同 mouse-deco 风格,保持一致):
//   - (pointer: coarse): 触摸设备没有"鼠标在哪儿"概念,直接 return
//   - prefers-reduced-motion: reduce: 用户要克制动效,直接 return
//   - canvas 不存在 (老 HTML 没引入): 直接 return(整模块 no-op)
//
// CSP: 无外部依赖,无 inline script,无样式注入。SRI 不需要。
//
// Deps: window.litecode 命名空间(与 mouse-deco.js / app.js / api.js 同款)。
// Page consumers: HTML 末尾 <script src="/js/mouse-particles.js" defer>
//   引入。
//
(function (ns) {
    'use strict';

    // ── Feature gating ─────────────────────────────────────────
    if (window.matchMedia) {
        var coarseMq = window.matchMedia('(pointer: coarse)');
        if (coarseMq && coarseMq.matches) return;
        var reducedMq = window.matchMedia('(prefers-reduced-motion: reduce)');
        if (reducedMq && reducedMq.matches) return;
    }

    // ── Canvas discovery ───────────────────────────────────────
    // 全局 fixed canvas(viewport 全屏)
    var globalCanvas = document.getElementById('lc-particles-canvas');
    // scoped canvas(嵌在 .lc-particles-host 容器内的)
    var scopedCanvases = Array.prototype.slice.call(
        document.querySelectorAll('.lc-particles-canvas-scoped')
    );
    // 两个都没有就直接 no-op,节省一次模块加载。
    if (!globalCanvas && !scopedCanvases.length) return;

    // ── State ──────────────────────────────────────────────────
    var dpr = window.devicePixelRatio || 1;
    var rafId = null;
    var running = false;
    var mouse = { px: 0, py: 0 };   // 视口像素坐标(共享给所有 canvas)

    // canvases: 统一的运行时描述数组,每个 canvas 一个对象
    //   { el, ctx, kind, w, h, viewportRect, cfg, particles, appliedCfg }
    //   - kind: 'global' (fixed 全屏) | 'scoped' (绝对定位 inset:0)
    //   - viewportRect: scoped 时记录容器相对 viewport 的 left/top,
    //     这样全局 mouse 坐标能换算到容器坐标系
    var canvases = [];

    // 颜色调色板:双主题统一提亮,粒子作为"光斑点缀"而非"重点装饰",
    // 避免在题目列表 / 标题区等密集文字区域造成视觉遮挡。
    var PALETTE = {
        dark: [
            'hsl(190, 70%, 78%)',   // 浅青
            'hsl(260, 65%, 80%)',   // 浅紫
            'hsl(160, 55%, 72%)',   // 浅绿
            'hsl(45,  70%, 78%)',   // 浅琥珀
        ],
        light: [
            'hsl(195, 65%, 62%)',   // 中青
            'hsl(255, 60%, 65%)',   // 中紫
            'hsl(155, 55%, 55%)',   // 中绿
            'hsl(40,  65%, 60%)',   // 中琥珀
        ],
    };

    // 全局 canvas 配置 — 与 v1.3.4 PR12 follow-up-2 一致。
    var CONFIG = {
        dark: {
            N: 55, mouseInfluence: 220, lineDist: 130,
            force: 0.020, drift: 0.20, spring: 0.0016,
            sizeMin: 2.0, sizeMax: 4.0,
            nearSizeMul: 1.7, glowMul: 6, nearR: 70,
            alphaBase: 0.32, alphaNear: 0.55,
        },
        light: {
            N: 60, mouseInfluence: 210, lineDist: 140,
            force: 0.022, drift: 0.22, spring: 0.0016,
            sizeMin: 2.1, sizeMax: 4.2,
            nearSizeMul: 1.8, glowMul: 7, nearR: 75,
            alphaBase: 0.32, alphaNear: 0.55,
        },
    };

    // scoped canvas 配置 — 在全局基础上"再弱一档",确保不挡文字。
    // 这些容器本身有 opaque 卡片背景 + 大量文字,粒子纯点缀。
    var SCOPED_CONFIG = {
        dark: {
            N: 22, mouseInfluence: 160, lineDist: 90,
            force: 0.020, drift: 0.18, spring: 0.0016,
            sizeMin: 1.6, sizeMax: 3.0,
            nearSizeMul: 1.4, glowMul: 0, nearR: 55,
            alphaBase: 0.18, alphaNear: 0.30,
        },
        light: {
            N: 26, mouseInfluence: 170, lineDist: 100,
            force: 0.022, drift: 0.20, spring: 0.0016,
            sizeMin: 1.7, sizeMax: 3.2,
            nearSizeMul: 1.5, glowMul: 0, nearR: 60,
            alphaBase: 0.18, alphaNear: 0.30,
        },
    };

    // ── Helpers ────────────────────────────────────────────────
    function isDark() {
        return document.documentElement.classList.contains('dark');
    }
    function cfgFor(kind) {
        var d = isDark();
        return (kind === 'scoped')
            ? (d ? SCOPED_CONFIG.dark : SCOPED_CONFIG.light)
            : (d ? CONFIG.dark : CONFIG.light);
    }
    function paletteColor(i) {
        var p = isDark() ? PALETTE.dark : PALETTE.light;
        return p[i % p.length];
    }

    // ── Canvas attach ──────────────────────────────────────────
    function attachCanvas(el, kind) {
        if (!el) return null;
        var ctx = el.getContext('2d');
        if (!ctx) return null;
        var entry = {
            el:        el,
            ctx:       ctx,
            kind:      kind,
            w:         0,
            h:         0,
            // scoped canvas: 容器相对 viewport 的偏移。tick 时用
            // (mouse.px - rect.left, mouse.py - rect.top) 换算到容器
            // 局部坐标。null 表示全局 canvas(已经是 viewport 坐标)。
            hostLeft:  0,
            hostTop:   0,
            cfg:       null,
            appliedCfg: null,
            particles: [],
        };
        if (kind === 'scoped') {
            // scoped canvas 父元素(.lc-particles-host)决定可视区。
            // 父元素负责 position:relative + overflow:hidden,
            // 我们只关心父级的 getBoundingClientRect。
            entry.host = el.parentElement || document.body;
        } else {
            entry.host = document.body;
        }
        return entry;
    }
    function discoverCanvases() {
        canvases = [];
        if (globalCanvas) {
            var g = attachCanvas(globalCanvas, 'global');
            if (g) canvases.push(g);
        }
        scopedCanvases.forEach(function (el) {
            var s = attachCanvas(el, 'scoped');
            if (s) canvases.push(s);
        });
    }

    // ── Measure / resize ───────────────────────────────────────
    function measureEntry(c) {
        if (c.kind === 'global') {
            c.w = window.innerWidth  || document.documentElement.clientWidth  || 0;
            c.h = window.innerHeight || document.documentElement.clientHeight || 0;
            c.hostLeft = 0;
            c.hostTop  = 0;
        } else {
            // scoped: 读父级 rect,容器可能尚未 layout 完(刚插入)
            var rect = c.host.getBoundingClientRect();
            // 容器可能折叠(0×0) — 跳过,等下一次 resize 再试
            if (rect.width <= 0 || rect.height <= 0) return false;
            c.w = rect.width;
            c.h = rect.height;
            c.hostLeft = rect.left;
            c.hostTop  = rect.top;
        }
        c.el.width  = Math.floor(c.w * dpr);
        c.el.height = Math.floor(c.h * dpr);
        c.el.style.width  = c.w + 'px';
        c.el.style.height = c.h + 'px';
        // 修 DPR 累积 bug — setTransform 一次性覆盖
        c.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        return true;
    }
    function measureAll() {
        for (var i = 0; i < canvases.length; i++) measureEntry(canvases[i]);
    }
    var resizeTimer = null;
    function onResize() {
        if (resizeTimer) return;
        resizeTimer = setTimeout(function () {
            resizeTimer = null;
            measureAll();
            // 重置粒子 base position(防止初始 base 跑出视口外)
            for (var i = 0; i < canvases.length; i++) {
                var ps = canvases[i].particles;
                for (var j = 0; j < ps.length; j++) {
                    ps[j].baseX = Math.random() * canvases[i].w;
                    ps[j].baseY = Math.random() * canvases[i].h;
                    ps[j].x = ps[j].baseX;
                    ps[j].y = ps[j].baseY;
                }
            }
        }, 200);
    }
    // 监听 scroll 让 scoped canvas 的 hostLeft/hostTop 同步
    var scrollTimer = null;
    function onScroll() {
        if (scrollTimer) return;
        scrollTimer = setTimeout(function () {
            scrollTimer = null;
            // 只更新 scoped 的 hostLeft/hostTop(像素矩阵不变)
            for (var i = 0; i < canvases.length; i++) {
                var c = canvases[i];
                if (c.kind !== 'scoped') continue;
                var rect = c.host.getBoundingClientRect();
                c.hostLeft = rect.left;
                c.hostTop  = rect.top;
            }
        }, 80);
    }

    // ── Init particles ─────────────────────────────────────────
    function initEntryParticles(c) {
        c.cfg = cfgFor(c.kind);
        c.particles = [];
        for (var i = 0; i < c.cfg.N; i++) {
            var baseX = Math.random() * c.w;
            var baseY = Math.random() * c.h;
            c.particles.push({
                baseX: baseX,
                baseY: baseY,
                x: baseX,
                y: baseY,
                vx: (Math.random() - 0.5) * c.cfg.drift,
                vy: (Math.random() - 0.5) * c.cfg.drift,
                hue: i % 4,
                size: c.cfg.sizeMin + Math.random() * (c.cfg.sizeMax - c.cfg.sizeMin),
                phase: Math.random() * Math.PI * 2,
            });
        }
        c.appliedCfg = c.cfg;
    }
    function ensureEntryCfg(c) {
        var want = cfgFor(c.kind);
        if (c.appliedCfg !== want || c.particles.length !== want.N) {
            initEntryParticles(c);
        }
    }

    // ── Read mouse from CSS vars ───────────────────────────────
    // mouse-deco.js 已经写 --lc-spotlight-x/y(像素坐标),所有 canvas
    // 共用这一份 mouse 位置。
    function readMouse() {
        var sx = document.documentElement.style.getPropertyValue('--lc-spotlight-x');
        var sy = document.documentElement.style.getPropertyValue('--lc-spotlight-y');
        if (sx && sy) {
            var px = parseFloat(sx);
            var py = parseFloat(sy);
            if (isFinite(px) && isFinite(py)) {
                mouse.px = px;
                mouse.py = py;
            }
        }
    }

    // ── Render one canvas ──────────────────────────────────────
    function renderCanvas(c) {
        if (c.w <= 0 || c.h <= 0) return;
        var ctx = c.ctx;
        var cfg = c.cfg;
        ctx.clearRect(0, 0, c.w, c.h);
        ctx.globalCompositeOperation = 'source-over';

        // 把 viewport mouse 坐标换算到 canvas 局部坐标
        var localMx = mouse.px - c.hostLeft;
        var localMy = mouse.py - c.hostTop;

        var N = c.particles.length;

        // ── 1) 更新粒子位置(漂移 + 鼠标引力 + 弹回 base) ──
        for (var i = 0; i < N; i++) {
            var p = c.particles[i];
            // (a) 漂移
            p.x += p.vx;
            p.y += p.vy;
            // (b) 鼠标引力
            var dx = localMx - p.x;
            var dy = localMy - p.y;
            var d2 = dx * dx + dy * dy;
            if (d2 < cfg.mouseInfluence * cfg.mouseInfluence && d2 > 0.01) {
                var dist = Math.sqrt(d2);
                var force = (1 - dist / cfg.mouseInfluence) * cfg.force;
                p.x += (dx / dist) * force * 60;
                p.y += (dy / dist) * force * 60;
            }
            // (c) 弹性拉回 base
            p.x += (p.baseX - p.x) * cfg.spring;
            p.y += (p.baseY - p.y) * cfg.spring;
            // (d) 越界反弹(局部坐标)
            if (p.x < 0)    { p.x = 0;    p.vx = Math.abs(p.vx); }
            if (p.x > c.w)  { p.x = c.w;  p.vx = -Math.abs(p.vx); }
            if (p.y < 0)    { p.y = 0;    p.vy = Math.abs(p.vy); }
            if (p.y > c.h)  { p.y = c.h;  p.vy = -Math.abs(p.vy); }
        }

        // ── 2) 画连线(粒子 ↔ 粒子,鼠标附近才画) ──
        ctx.lineWidth = 1;
        var nearBoostR = cfg.mouseInfluence;
        for (var a = 0; a < N; a++) {
            for (var b = a + 1; b < N; b++) {
                var p1 = c.particles[a];
                var p2 = c.particles[b];
                var lx = p1.x - p2.x;
                var ly = p1.y - p2.y;
                var ld = Math.sqrt(lx * lx + ly * ly);
                if (ld < cfg.lineDist) {
                    var midX = (p1.x + p2.x) / 2;
                    var midY = (p1.y + p2.y) / 2;
                    var md = Math.sqrt((midX - localMx) * (midX - localMx)
                                     + (midY - localMy) * (midY - localMy));
                    var nearBoost = md < nearBoostR ? (1 - md / nearBoostR) * 0.25 : 0;
                    var alpha = (1 - ld / cfg.lineDist) * 0.18 + nearBoost;
                    if (alpha > 1) alpha = 1;
                    ctx.strokeStyle = paletteColor(p1.hue);
                    ctx.globalAlpha = alpha;
                    ctx.beginPath();
                    ctx.moveTo(p1.x, p1.y);
                    ctx.lineTo(p2.x, p2.y);
                    ctx.stroke();
                }
            }
        }

        // ── 3) 画粒子本体(光点) ──
        for (var k = 0; k < N; k++) {
            var q = c.particles[k];
            var qdx = q.x - localMx;
            var qdy = q.y - localMy;
            var qd = Math.sqrt(qdx * qdx + qdy * qdy);
            var isNear = qd < cfg.nearR;
            var alpha = isNear ? cfg.alphaNear : cfg.alphaBase;
            var size  = q.size * (isNear ? cfg.nearSizeMul : 1.0);
            ctx.fillStyle = paletteColor(q.hue);
            ctx.globalAlpha = alpha;
            ctx.beginPath();
            ctx.arc(q.x, q.y, size, 0, Math.PI * 2);
            ctx.fill();
            // glow(仅全局 canvas 有,scoped glowMul=0 跳过)
            if (isNear && cfg.glowMul > 0) {
                var grad = ctx.createRadialGradient(q.x, q.y, 0, q.x, q.y, size * cfg.glowMul);
                grad.addColorStop(0, paletteColor(q.hue));
                grad.addColorStop(1, 'transparent');
                ctx.fillStyle = grad;
                ctx.globalAlpha = 0.22;
                ctx.beginPath();
                ctx.arc(q.x, q.y, size * cfg.glowMul, 0, Math.PI * 2);
                ctx.fill();
            }
        }
        ctx.globalAlpha = 1;
    }

    // ── Per-frame tick ─────────────────────────────────────────
    var lastT = 0;
    function tick(t) {
        rafId = requestAnimationFrame(tick);
        if (!running) return;
        readMouse();
        // 每个 canvas 各自检查 cfg(主题切换 / 首次初始化)
        for (var i = 0; i < canvases.length; i++) {
            ensureEntryCfg(canvases[i]);
            renderCanvas(canvases[i]);
        }
    }

    // ── Public surface ─────────────────────────────────────────
    function start() {
        if (running) return;
        // 重新扫描 canvases — 处理 DOM 动态插入(如 SPA 后续渲染)
        discoverCanvases();
        measureAll();
        for (var i = 0; i < canvases.length; i++) initEntryParticles(canvases[i]);
        // ResizeObserver 监听 scoped host — 让 scoped canvas 在
        // 父容器从 hidden→visible (display:none→block,profile.html
        // showState('content')) 或内容增长导致 box 变化时自动重测
        // + 重置粒子。window resize 不覆盖 scoped host 自身的
        // 尺寸变化。
        if (typeof ResizeObserver !== 'undefined') {
            var ro = new ResizeObserver(function () {
                for (var i = 0; i < canvases.length; i++) {
                    if (canvases[i].kind !== 'scoped') continue;
                    if (measureEntry(canvases[i])) {
                        var ps = canvases[i].particles;
                        for (var j = 0; j < ps.length; j++) {
                            ps[j].baseX = Math.random() * canvases[i].w;
                            ps[j].baseY = Math.random() * canvases[i].h;
                            ps[j].x = ps[j].baseX;
                            ps[j].y = ps[j].baseY;
                        }
                    }
                }
            });
            for (var k = 0; k < canvases.length; k++) {
                if (canvases[k].kind === 'scoped') ro.observe(canvases[k].host);
            }
        }
        running = true;
        lastT = 0;
        rafId = requestAnimationFrame(tick);
    }
    function stop() {
        running = false;
        if (rafId) { cancelAnimationFrame(rafId); rafId = null; }
        for (var i = 0; i < canvases.length; i++) {
            canvases[i].ctx.clearRect(0, 0, canvases[i].w, canvases[i].h);
        }
    }

    // ── Bootstrap ──────────────────────────────────────────────
    window.addEventListener('resize', onResize, { passive: true });
    window.addEventListener('scroll', onScroll, { passive: true });
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', start, { once: true });
    } else {
        start();
    }

    ns.particles = { start: start, stop: stop };
}(window.litecode = window.litecode || {}));