// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext judge factory (v1.2.48)
//
// Defines litecode::build_judge_deps(). Pulls in the four judge
// headers (docker_client / warm_pool / judge_scheduler /
// judge_notifier). Each per-route .cpp file ALSO pulls in
// judge_scheduler.h to access make_probe — the inline definitions
// of the small JudgeScheduler methods that live in the header
// are duplicated across TUs without issue (single inline
// definition per TU, all same body).

#include "app_context_deps.h"
#include "config.h"
#include "judge/docker_client.h"
#include "judge/judge_notifier.h"
#include "judge/judge_scheduler.h"

#include <filesystem>
#include "judge/warm_pool.h"

#include <iostream>

namespace litecode {

JudgeDeps build_judge_deps(const JudgeConfig& cfg,
                           ConnectionPool*   db) {
    JudgeDeps out;

    // Docker socket client — nullptr when DOCKER_SOCKET_URL is empty
    // (dev box without docker).
    out.docker_client = docker::make_client_from_config(cfg);
    if (!out.docker_client) {
        std::cerr << "[boot] WARN: no DOCKER_SOCKET_URL — judge disabled,"
                     " docker probe will report 'down'" << std::endl;
    }

    // Warm pool — only meaningful when docker_client exists.
    if (out.docker_client) {
        out.warm_pool = std::make_unique<judge::WarmPool>(
            out.docker_client.get());
        try {
            out.warm_pool->start(
                judge::make_default_warm_pool_config(cfg));
        } catch (const std::exception& e) {
            std::cerr << "[boot] WARN: warm_pool start failed: "
                      << e.what() << std::endl;
            out.warm_pool.reset();
        }
    }

    // Scheduler — always constructed; worker is no-op when
    // docker_client is null. The db_pool is passed in from main
    // (after build_db_deps) so the scheduler's `start()` succeeds —
    // start() returns false when db_ is null (judge_scheduler.h:392)
    // and submissions then fail with "judge queue is full" because
    // running_.load() == false.
    {
        // v1.2.50: pass cfg.task_dir_parent explicitly so the scheduler
        // can stage task.json on a host-visible shared volume
        // (see docker-compose judge-tmp mount). Without this, the
        // bind-mount source path lives inside the web container's
        // /tmp and the host docker daemon can't see it.
        auto sched_cfg = judge::make_default_scheduler_config(
            cfg, std::filesystem::path(cfg.task_dir_parent));
        out.scheduler = std::make_unique<judge::JudgeScheduler>(
            out.docker_client.get(),
            out.warm_pool.get(),
            db,                                    // wired here (was nullptr pre-v1.2.50)
            std::move(sched_cfg));
    }

    // SSE notifier + scheduler wiring.
    out.notifier = std::make_unique<judge::JudgeNotifier>();
    out.scheduler->set_notifier(out.notifier.get());

    // v1.2.50: start the worker pool here so submissions can be
    // enqueued. Pre-v1.2.50 this call was missing and the
    // scheduler stayed in the un-started state — submissions got
    // 503 "judge queue is full" because running_.load() == false.
    if (out.scheduler) {
        if (!out.scheduler->start()) {
            std::cerr << "[boot] WARN: judge_scheduler start failed"
                      << std::endl;
        }
    }

    return out;
}

} // namespace litecode