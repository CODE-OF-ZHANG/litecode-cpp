// SPDX-License-Identifier: MIT
//
// web/js/fade-in.js — 装饰元素入场动效(IntersectionObserver)
//
// v1.3.4 PR 8 ★ 视觉提升 / SPEC §6.3
// 给任何带 .lc-fade-in 的元素加"进入视口时淡入 + 上移 8px"。
// 支持 data-lc-stagger="0,1,2..." 设置延迟(50ms / 步),让
// 卡片网格逐个出现有节奏感。仅用于装饰元素(KPI / chip / podium),
// 不用在功能按钮 / 表单上(避免误触)。
//
// 约束:
//   - 一次性触发(进入视口后 unobserve,不重复)
//   - prefers-reduced-motion: reduce 跳过监听,直接 visible
//   - 浏览器不支持 IntersectionObserver 时回退到直接 visible
//
// Deps: 无。Page consumers 在 HTML 末尾 <script src="/js/fade-in.js"
// defer> 引入。
(function (ns) {
    'use strict';

    // reduced-motion 用户不想看入场动效 — 立即把所有 .lc-fade-in
    // 标 visible,IntersectionObserver 都不需要建。
    var reduced = false;
    if (window.matchMedia) {
        var mq = window.matchMedia('(prefers-reduced-motion: reduce)');
        reduced = !!(mq && mq.matches);
    }

    function reveal(el) {
        if (!el || el.classList.contains('lc-fade-in--visible')) return;
        var stagger = parseInt(el.getAttribute('data-lc-stagger') || '0', 10);
        if (stagger > 0) {
            // setTimeout 在 IO 回调内用 — 一帧 16ms 远小于 50ms*stagger,
            // 不影响 IO 主循环。stagger 限制 ≤ 12(0..11),超过 clamp。
            var delay = Math.min(stagger, 12) * 50;
            setTimeout(function () {
                el.classList.add('lc-fade-in--visible');
            }, delay);
        } else {
            el.classList.add('lc-fade-in--visible');
        }
    }

    function init() {
        var nodes = document.querySelectorAll('.lc-fade-in');
        if (!nodes.length) return;

        if (reduced || typeof window.IntersectionObserver !== 'function') {
            // 回退路径:直接全部 visible
            for (var i = 0; i < nodes.length; i++) {
                nodes[i].classList.add('lc-fade-in--visible');
            }
            return;
        }

        var io = new IntersectionObserver(function (entries) {
            for (var i = 0; i < entries.length; i++) {
                var entry = entries[i];
                if (entry.isIntersecting) {
                    reveal(entry.target);
                    io.unobserve(entry.target);
                }
            }
        }, {
            // 进入视口 10% 即触发(给 stagger 留点视觉缓冲)
            threshold: 0.10,
            // 底部多预留 40px 提前加载
            rootMargin: '0px 0px -40px 0px',
        });

        for (var j = 0; j < nodes.length; j++) {
            io.observe(nodes[j]);
        }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init, { once: true });
    } else {
        init();
    }

    // Public surface (debug only)
    ns.fadeIn = {
        init: init,
        reveal: reveal,
    };
}(window.litecode = window.litecode || {}));
