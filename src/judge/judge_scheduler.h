// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — judge scheduler (Phase 4 ★)
//
// SPEC §3.2 / §7.1 / §11 Phase 4 / §12.2 / A6 / A8 / A25 / A29 / A30
//   The asynchronous judge pipeline. The web layer enqueues a
//   submission after POST /api/v1/submissions returns; a worker thread
//   pool pulls tasks off the queue, drives a Docker container through
//   the SPEC §7.1 lifecycle (compile / run / compare), and writes the
//   final status back to the `submissions` table.
//
// Pipeline (one task):
//
//   enqueue(task)
//       │
//       ▼
//   mark_running(id)            ─ submissions.status='running'
//       │
//       ▼
//   pool.acquire()              ─ pop an idle container id (or nullopt
//       │                         ⇒ create one inline)
//       ▼
//   docker.create(per-task)     ─ bind-mount task.json → /tmp/task.json
//       │                         (judge.sh reads via JUDGE_TASK_FILE)
//       ▼
//   docker.start(cid)           ─ boots judge.sh, which compiles +
//       │                         runs + emits final JSON on stdout
//       ▼
//   docker.wait(cid, T+5s)      ─ blocks until judge.sh exits, with
//       │                         an outer watchdog (SPEC §7.1 step 6)
//       ▼
//   docker.logs(cid)            ─ pull stdout (the judge JSON)
//       │
//       ▼
//   parse → JudgeResult
//       │
//       ▼
//   mark_finished(id, ...)      ─ submissions.status='ac|wa|...'/'se'
//       │
//       ▼
//   docker.remove(cid, force)   ─ SPEC §7.1 step 5: NOT returned to pool
//       │
//       ▼
//   pool.release(id)            ─ just nudges the refill thread
//
// Why a per-task container instead of reusing the pool's idle one:
//
//   The pool pre-creates K idle containers with a fixed
//   `--help` command and no bind mounts (see warm_pool.h). Each
//   judge task needs (a) a unique bind mount to a host-side
//   `task.json` file and (b) `JUDGE_TASK_FILE=/tmp/task.json` in the
//   container env. The Docker Engine API does NOT allow adding
//   mounts to an already-created container, and env vars are
//   fixed at create time. So the worker:
//     1) acquires an idle id from the pool (for the image-layer
//        warm-up benefit — pre-pulled layers mean per-task
//        `docker create` is fast), then immediately `docker rm`s it;
//     2) creates a fresh per-task container with the right bind
//        mount + env;
//     3) starts, waits, reads logs, removes the per-task container;
//     4) signals pool.release() so the refill thread tops K back up.
//   The pool's role reduces to "keep the image layers warm" —
//   exactly what SPEC §7.1 step 0 promises. The actual judge
//   containers are still per-task, as SPEC §7.1 step 5 insists.
//
// Threading model:
//   - One task queue (std::deque<JudgeTask>) protected by `queue_mu_`
//     and signaled by `queue_cv_`. push() under lock; pop() under
//     the same lock inside the worker.
//   - N worker threads (cfg.max_concurrent). Each runs worker_loop()
//     in a tight wait → pop → run cycle until shutdown() flips
//     `shutting_down_` and the queue drains.
//   - running_count_ is an atomic so the health probe can read it
//     without taking a lock; the worker adjusts it on entry / exit.
//   - Queue size is bounded by cfg.max_queue_size; enqueue() rejects
//     when full so the route handler can return 503 (SPEC §15.5).
//
// Failure surface:
//   - per-task failures are caught and turned into `status='se'` +
//     a `mark_finished()` call so the submission row reaches a
//     terminal state. Anything thrown out of the worker loop would
//     kill the thread; the loop is wrapped in a catch-all that
//     re-enters the wait. Workers never propagate exceptions to
//     enqueue() callers (the API has already returned 201 by then).
//   - shutdown() is idempotent and joins every worker. A pending
//     task whose row has not yet been marked_finished stays in
//     status='running'; a recovery sweep (requeue_stuck_running)
//     can pick them up after restart. Today's behavior is to
//     let those rows stay running; a Phase 9 cron will sweep.
//
// Observability:
//   - queue_size() / running_count() / max_concurrent() feed
//     /api/v1/health's queue_size + warm_pool fields (SPEC §16.1).
//   - make_probe() wraps the three counters into a HealthService::Probe.
//   - All public methods are noexcept (return std::optional<> / bool
//     for fallible paths) so a docker outage cannot crash the
//     HTTP handler.
//
// Usage (main.cpp boot):
//   auto docker_client = litecode::docker::make_client_from_config(cfg.judge);
//   litecode::judge::WarmPool pool(docker_client.get());
//   pool.start(litecode::judge::make_default_warm_pool_config(cfg.judge));
//   litecode::judge::JudgeSchedulerConfig sc;
//   sc.max_concurrent          = cfg.judge.max_concurrent_judges;
//   sc.max_queue_size          = cfg.judge.max_queue_size;
//   sc.compile_timeout_ms      = cfg.judge.compile_timeout_seconds * 1000;
//   sc.judge_hard_timeout_seconds = cfg.judge.judge_hard_timeout_seconds;
//   sc.output_limit_bytes      = cfg.judge.output_limit_bytes;
//   litecode::judge::JudgeScheduler sched(
//       docker_client.get(), &pool, &db_pool, sc);
//   sched.start();
//   health.register_probe("judge_queue",
//       litecode::judge::JudgeScheduler::make_probe(&sched));
//
//   // On POST /api/v1/submissions:
//   litecode::JudgeTask t;
//   t.submission_id    = new_submission_id;
//   t.user_id          = claims.user_id;
//   t.problem_id       = body.problem_id;
//   t.language         = body.language;
//   t.code             = body.code;
//   t.time_limit_ms    = problem.time_limit;
//   t.memory_limit_mb  = problem.memory_limit;
//   for (auto& tc : test_cases) {
//       litecode::JudgeTask::TestCaseInput in;
//       in.input = tc.input; in.expected_output = tc.expected_output;
//       in.judge_type = tc.judge_type;
//       in.float_epsilon = tc.float_epsilon;
//       in.order_num = tc.order_num;
//       t.test_cases.push_back(std::move(in));
//   }
//   if (!sched.enqueue(std::move(t))) {
//       return 503;  // queue full
//   }

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../config.h"                  // JudgeConfig
#include "../db/connection_pool.h"      // ConnectionPool
#include "../db/submission_repo.h"      // mark_running / mark_finished
#include "../logger.h"                  // LOG_* / RequestIdScope
#include "../routes/system_routes.h"    // HealthService::Probe / ProbeResult
#include "docker_client.h"              // docker::Client / CreateOptions
#include "warm_pool.h"                  // WarmPool / IdleContainer

namespace litecode {
namespace judge {

// ────────────────────────────────────────────────────────────────────────────
//  JudgeTask
//
//  Input shape the route handler hands to enqueue(). One task == one
//  row in `submissions`. We deliberately bundle the test cases into
//  the task (rather than letting the worker re-read them from the DB)
//  because (a) the test_case list is small and immutable for the
//  duration of the judge, and (b) caching avoids a second DB round
//  trip on the hot path. If the schema grows (huge judge-only
//  test_case sets, hot-reload support, etc.) this is the spot to
//  revisit.
//
//  The order_num field on each test case preserves the iteration
//  order judge.sh expects — judge.sh reads test_cases in JSON-array
//  order, and our SQL ORDER BY matches.
// ────────────────────────────────────────────────────────────────────────────

struct JudgeTask {
    struct TestCaseInput {
        std::string             input;
        std::string             expected_output;
        std::string             judge_type;        // "exact" | "ignore_trailing" | "float_eps" | "special"
        std::optional<double>   float_epsilon;
        int                     order_num = 0;
    };

    int                        submission_id = 0;
    int                        user_id       = 0;
    int                        problem_id    = 0;
    std::string                language;            // "c" | "cpp"
    std::string                code;
    int                        time_limit_ms   = 1000;   // from problems.time_limit
    int                        memory_limit_mb = 256;    // from problems.memory_limit
    int                        compile_timeout_ms = 10'000;
    std::vector<TestCaseInput> test_cases;
};

// ────────────────────────────────────────────────────────────────────────────
//  JudgeResult
//
//  Parsed back from judge.sh's stdout JSON. The shape mirrors the
//  contract documented in judge/judge.sh:
//
//     {
//       "submission_id":     42,
//       "status":            "ac|wa|tle|mle|re|ole|pe|ce|se",
//       "time_used_ms":      12,
//       "memory_used_kb":    2048,
//       "error_message":     null|"...",
//       "failed_case_index": null|0,
//       "case_results":      [{ "index": 0, "status": "ac", ... }]
//     }
//
//  We tolerate empty / malformed JSON by flipping status to 'se' so
//  the worker can hand it to mark_finished() without a try/catch
//  ladder at every call site.
// ────────────────────────────────────────────────────────────────────────────

struct JudgeResult {
    std::string status            = "se";
    int         time_used_ms      = 0;
    int         memory_used_kb    = 0;
    std::string error_message;
    int         failed_case_index = -1;     // -1 ⇒ no case failed
    bool        parsed            = false;  // true ⇒ came from a clean JSON parse
};

// ────────────────────────────────────────────────────────────────────────────
//  JudgeSchedulerConfig
//
//  Tunables pulled out so production main() can override defaults
//  from JudgeConfig and tests can dial down to single-worker mode.
//  All fields have safe defaults matching SPEC §7.3 / §15.5 / §3.2.
// ────────────────────────────────────────────────────────────────────────────

struct JudgeSchedulerConfig {
    // Worker thread count (SPEC §3.2: default 4; SPEC §15.5 max-concurrent).
    int max_concurrent          = 4;
    // Queue capacity (SPEC §15.5). 0 means "unbounded" (used in tests;
    // production always sets a finite limit so 503 surfaces).
    int max_queue_size          = 50;
    // Compile timeout (SPEC §7.3 / §15.4 compile-bomb guard). Mirrored
    // to the judge.sh task JSON's compile_timeout_ms field.
    int compile_timeout_ms      = 10'000;
    // Outer watchdog for the whole judge.sh execution (SPEC §7.1 step 6
    // / §7.4). The `docker wait` call gets timeout_ms =
    // judge_hard_timeout_seconds * 1000 + 5000.
    int judge_hard_timeout_seconds = 30;
    // Output limit (SPEC §7.4) — surfaced to judge.sh's task JSON so
    // OLE judgment lines up with the route's truncation cap.
    int output_limit_bytes      = 16 * 1024 * 1024;

    // Docker image to launch (SPEC §7.2). Tests can override to a
    // stand-in name; production pulls `cfg.judge.judge_image` (default
    // "litecode-judge:latest").
    std::string judge_image     = "litecode-judge:latest";
    // Docker network mode (SPEC §7.3 — "none" for full isolation).
    std::string network_mode    = "none";

    // Host-side temp dir parent. Each task creates a unique subdir
    // inside it for its task.json. Defaults to std::filesystem::temp_directory_path().
    // Tests override this to keep tmp directories out of /tmp.
    std::filesystem::path task_dir_parent;
};

// ────────────────────────────────────────────────────────────────────────────
//  JudgeSchedulerError — typed exception surface
//
//  Three tiers, mirroring the rest of Phase 4:
//    - JudgeSchedulerError         — generic failure (driver error,
//                                    config validation)
//    - JudgeSchedulerConfigError   — start() refused to come up
//                                    (returned via std::optional<bool>
//                                    semantics — see start())
//    - JudgeQueueFullError         — enqueue() rejected because the
//                                    queue is at capacity; the route
//                                    handler maps this to 503
//
//  We deliberately do NOT throw out of enqueue() — the route has
//  already returned 201 by the time it's called, and an exception
//  there would crash the handler. enqueue() returns false on
//  queue-full instead and the route maps false → 503.
// ────────────────────────────────────────────────────────────────────────────

class JudgeSchedulerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class JudgeSchedulerConfigError : public JudgeSchedulerError {
public:
    using JudgeSchedulerError::JudgeSchedulerError;
};

class JudgeQueueFullError : public JudgeSchedulerError {
public:
    using JudgeSchedulerError::JudgeSchedulerError;
};

// ────────────────────────────────────────────────────────────────────────────
//  JudgeScheduler
// ────────────────────────────────────────────────────────────────────────────

class JudgeScheduler {
public:
    // Pointers must outlive the scheduler. The scheduler is non-owning;
    // main() owns the docker client, the warm pool, and the DB pool.
    JudgeScheduler(docker::Client*   client,
                   WarmPool*         pool,
                   ConnectionPool*   db,
                   JudgeSchedulerConfig cfg) noexcept
        : client_(client), pool_(pool), db_(db), cfg_(std::move(cfg)) {}

    JudgeScheduler(const JudgeScheduler&)            = delete;
    JudgeScheduler& operator=(const JudgeScheduler&) = delete;
    JudgeScheduler(JudgeScheduler&&)                 = delete;
    JudgeScheduler& operator=(JudgeScheduler&&)      = delete;

    ~JudgeScheduler() { shutdown(); }

    // Bring the scheduler up: validate config, spawn `cfg.max_concurrent`
    // worker threads, return true. The workers immediately enter their
    // wait loops; nothing is judged until enqueue() is called.
    //
    // Returns false when:
    //   - cfg.max_concurrent < 1
    //   - cfg.judge_hard_timeout_seconds < 1
    //   - cfg.compile_timeout_ms < 1
    //   - client_ / db_ are null (we can't talk to docker / can't write
    //     submission status updates)
    //   - a worker thread fails to spawn (rare; OS resource exhaustion)
    //
    // On false-return the scheduler is left in the un-started state —
    // subsequent enqueue() / queue_size() calls are no-ops. This
    // matches WarmPool's contract.
    bool start() {
        if (running_.load(std::memory_order_acquire)) return true;

        if (cfg_.max_concurrent < 1) {
            try { LOG_WARN("judge_scheduler: max_concurrent must be >= 1"); } catch (...) {}
            return false;
        }
        if (cfg_.judge_hard_timeout_seconds < 1) {
            try { LOG_WARN("judge_scheduler: judge_hard_timeout_seconds must be >= 1"); } catch (...) {}
            return false;
        }
        if (cfg_.compile_timeout_ms < 1) {
            try { LOG_WARN("judge_scheduler: compile_timeout_ms must be >= 1"); } catch (...) {}
            return false;
        }
        if (db_ == nullptr) {
            try { LOG_WARN("judge_scheduler: db pool is null; not starting"); } catch (...) {}
            return false;
        }
        // client_ is allowed to be null — workers will treat null as
        // "every docker call fails" and the submission gets status='se'.
        // This matches WarmPool's tolerance for a missing docker stack.

        // Resolve task_dir_parent.
        if (cfg_.task_dir_parent.empty()) {
            try {
                cfg_.task_dir_parent = std::filesystem::temp_directory_path();
            } catch (...) {
                cfg_.task_dir_parent = std::filesystem::path("/tmp");
            }
        }
        try {
            std::filesystem::create_directories(cfg_.task_dir_parent);
        } catch (const std::exception& e) {
            try { LOG_WARN("judge_scheduler: cannot create task_dir_parent",
                          {{"path",   cfg_.task_dir_parent.string()},
                           {"error",  e.what()}}); } catch (...) {}
            return false;
        }

        running_.store(true, std::memory_order_release);
        workers_.clear();
        workers_.reserve(static_cast<std::size_t>(cfg_.max_concurrent));
        for (int i = 0; i < cfg_.max_concurrent; ++i) {
            try {
                workers_.emplace_back([this]{ worker_loop(); });
            } catch (const std::exception& e) {
                try { LOG_ERROR("judge_scheduler: failed to spawn worker",
                                {{"index",  std::to_string(i)},
                                 {"error",  e.what()}}); } catch (...) {}
                shutdown();
                return false;
            }
        }

        try {
            LOG_INFO("judge_scheduler: started",
                     {{"max_concurrent",          std::to_string(cfg_.max_concurrent)},
                      {"max_queue_size",          std::to_string(cfg_.max_queue_size)},
                      {"compile_timeout_ms",      std::to_string(cfg_.compile_timeout_ms)},
                      {"judge_hard_timeout_s",    std::to_string(cfg_.judge_hard_timeout_seconds)},
                      {"output_limit_bytes",      std::to_string(cfg_.output_limit_bytes)},
                      {"task_dir_parent",         cfg_.task_dir_parent.string()}});
        } catch (...) {}
        return true;
    }

    // Idempotent shutdown. Flips `shutting_down_`, signals the queue
    // cv, joins every worker. Workers drain the queue before exiting
    // (so a task enqueued right before shutdown() still gets judged).
    void shutdown() noexcept {
        if (shutting_down_.load(std::memory_order_acquire) &&
            !running_.load(std::memory_order_acquire)) {
            return;
        }
        shutting_down_.store(true, std::memory_order_release);
        queue_cv_.notify_all();

        for (auto& w : workers_) {
            try {
                if (w.joinable()) w.join();
            } catch (const std::exception& e) {
                try { LOG_WARN("judge_scheduler: worker join failed",
                              {{"error", e.what()}}); } catch (...) {}
            } catch (...) {}
        }
        workers_.clear();
        running_.store(false, std::memory_order_release);
        try { LOG_INFO("judge_scheduler: shutdown"); } catch (...) {}
    }

    // Submit a task to the queue. Returns false when:
    //   - the scheduler was never started (or was shut down)
    //   - the queue is at max_queue_size (route → 503)
    //
    // The submission row should already exist (status='pending'); the
    // worker will transition it to running → final. The worker
    // validates the row again with mark_running() (which is a no-op
    // if the row was already running or terminal), so a duplicate
    // enqueue is safe but pointless.
    bool enqueue(JudgeTask task) {
        if (!running_.load(std::memory_order_acquire)) return false;

        {
            std::lock_guard<std::mutex> g(queue_mu_);
            if (cfg_.max_queue_size > 0 &&
                static_cast<int>(queue_.size()) >= cfg_.max_queue_size) {
                return false;  // queue full → 503
            }
            queue_.push_back(std::move(task));
        }
        queue_cv_.notify_one();
        return true;
    }

    // Current pending count (not yet picked up by a worker).
    std::size_t queue_size() const noexcept {
        std::lock_guard<std::mutex> g(queue_mu_);
        return queue_.size();
    }

    // Workers currently inside the per-task body.
    std::size_t running_count() const noexcept {
        return running_count_.load(std::memory_order_acquire);
    }

    int max_concurrent() const noexcept {
        return cfg_.max_concurrent;
    }

    // True iff start() succeeded AND shutdown() has not been called.
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Health probe — wire into HealthService via:
    //     health.register_probe("judge_queue",
    //         litecode::judge::JudgeScheduler::make_probe(&sched));
    // Always reports ok=true (the queue is a soft signal, not a hard
    // outage), and publishes queue_size + running + max_concurrent
    // under extra so /api/v1/health renders them verbatim.
    static HealthService::Probe make_probe(JudgeScheduler* s) {
        return [s]() -> ProbeResult {
            ProbeResult r;
            if (s == nullptr) {
                r.ok     = false;
                r.detail = "no judge scheduler configured";
                return r;
            }
            r.ok     = true;
            r.detail = s->running() ? "judge queue live" : "judge queue not running";
            r.extra  = {
                {"queue_size",     static_cast<std::int64_t>(s->queue_size())},
                {"running",        static_cast<std::int64_t>(s->running_count())},
                {"max_concurrent", static_cast<std::int64_t>(s->max_concurrent())},
            };
            return r;
        };
    }

private:
    // ── worker loop ────────────────────────────────────────────────────────
    //
    // One pass:
    //   1. wait for a task or shutdown
    //   2. mark_running the submission
    //   3. write task.json to a unique host tempdir
    //   4. acquire a pool container (then immediately docker rm it —
    //      we need to create a per-task container with a bind mount
    //      and the API doesn't let us add mounts post-creation)
    //   5. docker create + start + wait + logs + remove
    //   6. parse logs → JudgeResult
    //   7. mark_finished the submission
    //   8. delete the task tempdir + pool.release
    void worker_loop() {
        while (true) {
            JudgeTask task;
            {
                std::unique_lock<std::mutex> lk(queue_mu_);
                queue_cv_.wait(lk, [this]{
                    return shutting_down_.load(std::memory_order_acquire) ||
                           !queue_.empty();
                });
                if (shutting_down_.load(std::memory_order_acquire) &&
                    queue_.empty()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }

            running_count_.fetch_add(1, std::memory_order_acq_rel);
            try {
                run_one_task(task);
            } catch (const std::exception& e) {
                // Should never happen — run_one_task catches everything.
                // This is the absolute last line of defense: log + flip
                // the submission to SE so it doesn't hang in 'running'.
                try {
                    LOG_ERROR("judge_scheduler: worker escaped run_one_task",
                              {{"submission_id", std::to_string(task.submission_id)},
                               {"error",         e.what()}});
                    submission_repo::mark_finished(
                        *db_, task.submission_id,
                        "se", std::nullopt, std::nullopt,
                        std::string("scheduler internal error: ") + e.what());
                } catch (...) {
                    // DB write failed too — there's nothing more we can do.
                }
            } catch (...) {
                try {
                    LOG_ERROR("judge_scheduler: worker escaped run_one_task (unknown)",
                              {{"submission_id", std::to_string(task.submission_id)}});
                } catch (...) {}
            }
            running_count_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Run one task end-to-end. Never throws — every failure path
    // converges on mark_finished(status='se', ...).
    void run_one_task(JudgeTask& task) {
        // Null docker client ⇒ every docker call would crash. Bail
        // out with SE before doing anything; this is the documented
        // "dev box without docker" path.
        if (client_ == nullptr) {
            try {
                LOG_ERROR("judge_scheduler: no docker client; task rejected",
                          {{"submission_id", std::to_string(task.submission_id)}});
            } catch (...) {}
            finish_se(task.submission_id, "no docker client configured");
            return;
        }

        // Stamp the request_id thread-local so log lines from this
        // worker are correlated with the submission (mirrors the
        // HTTP-handler pattern in server.h / auth_routes.h).
        RequestIdScope rid_scope("judge-" + std::to_string(task.submission_id));

        // 1. Flip status=running. If the row was already terminal or
        //    somebody else already started it, drop the task silently
        //    — redoing a finished judge would just race with the row
        //    the original worker already wrote.
        bool claimed = false;
        try {
            claimed = submission_repo::mark_running(*db_, task.submission_id);
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: mark_running failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            return;  // row stays 'pending'; a future requeue sweep can retry
        }
        if (!claimed) {
            try { LOG_WARN("judge_scheduler: submission already claimed",
                          {{"submission_id", std::to_string(task.submission_id)}}); } catch (...) {}
            return;
        }

        // 2. Write task.json to a unique host tempdir.
        std::filesystem::path task_dir;
        std::filesystem::path task_file;
        try {
            task_dir = make_task_dir(task.submission_id);
            task_file = task_dir / "task.json";
            write_task_json(task_file, task);
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: task.json write failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            finish_se(task.submission_id, std::string("failed to write task.json: ") + e.what());
            return;
        }

        // 3. Acquire a pool container (best-effort) for image-layer
        //    warm-up. The pool can return nullopt (no docker /
        //    pool not running) — that's fine; we just skip the
        //    prewarm step.
        std::string pool_id;
        bool pool_gave_one = false;
        if (pool_ != nullptr) {
            try {
                auto idle = pool_->acquire();
                if (idle.has_value()) {
                    pool_id = idle->id;
                    pool_gave_one = true;
                    // Immediately discard — we can't add a bind mount
                    // to an existing container.
                    try { client_->remove(pool_id, /*force=*/true); }
                    catch (const std::exception& e) {
                        try { LOG_WARN("judge_scheduler: discard pool container failed",
                                      {{"id",    pool_id},
                                       {"error", e.what()}}); } catch (...) {}
                        pool_gave_one = false;
                    }
                }
            } catch (const std::exception& e) {
                try { LOG_WARN("judge_scheduler: pool.acquire failed",
                              {{"error", e.what()}}); } catch (...) {}
            }
        }

        // 4. Create per-task container with bind mount + JUDGE_TASK_FILE env.
        std::string cid;
        try {
            docker::CreateOptions opts;
            opts.image        = cfg_.judge_image;
            opts.command      = {"/usr/local/bin/judge.sh"};
            opts.env          = {"JUDGE_TASK_FILE=/tmp/task.json",
                                  "JUDGE_HOME=/judge",
                                  "JUDGE_TMP=/tmp/judge"};
            opts.network_mode = cfg_.network_mode;
            opts.read_only    = true;
            opts.memory_mb    = task.memory_limit_mb;
            opts.cpus         = 0.0;             // rely on docker wait timeout for the wall clock
            opts.pids_limit   = 50;
            opts.security_opt = {"no-new-privileges:true"};
            opts.tmpfs        = { {"/tmp", "size=64m,mode=1777"} };
            opts.user         = "judgeuser";
            opts.working_dir  = "/judge";
            opts.mounts = { docker::BindMount{
                task_file.string(), "/tmp/task.json",
                /*read_only=*/true } };

            auto cr = client_->create(opts);
            if (cr.id.empty()) {
                finish_se(task.submission_id, "docker create returned empty id");
                cleanup_and_release(task_dir, pool_id, pool_gave_one);
                return;
            }
            cid = cr.id;
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: docker create failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            finish_se(task.submission_id, std::string("docker create failed: ") + e.what());
            cleanup_and_release(task_dir, pool_id, pool_gave_one);
            return;
        }

        // 5. Start the container.
        try {
            client_->start(cid);
        } catch (const std::exception& e) {
            try_remove(cid, "after start failure");
            finish_se(task.submission_id, std::string("docker start failed: ") + e.what());
            cleanup_and_release(task_dir, pool_id, pool_gave_one);
            return;
        }

        // 6. Wait with the outer watchdog.
        const int timeout_ms = cfg_.judge_hard_timeout_seconds * 1000 + 5000;
        docker::WaitResult wres;
        try {
            wres = client_->wait(cid, timeout_ms);
        } catch (const docker::DockerTimeoutError& e) {
            try { LOG_WARN("judge_scheduler: hard timeout exceeded",
                          {{"submission_id", std::to_string(task.submission_id)},
                           {"timeout_ms",    std::to_string(timeout_ms)}}); } catch (...) {}
            try_kill_and_remove(cid);
            finish_se(task.submission_id, "judge exceeded hard timeout (" +
                std::to_string(cfg_.judge_hard_timeout_seconds) + "s)");
            cleanup_and_release(task_dir, pool_id, pool_gave_one);
            return;
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: docker wait failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            try_kill_and_remove(cid);
            finish_se(task.submission_id, std::string("docker wait failed: ") + e.what());
            cleanup_and_release(task_dir, pool_id, pool_gave_one);
            return;
        }

        // 7. Pull stdout. judge.sh emits the final JSON on stdout
        //    (see judge/judge.sh Section E). We ignore stderr —
        //    diagnostic logs from g++ / runtime stderr go to docker
        //    logs too but we only parse the JSON line at the end.
        std::string logs;
        try {
            logs = client_->logs(cid, /*stdout=*/true, /*stderr=*/false);
        } catch (const std::exception& e) {
            try { LOG_WARN("judge_scheduler: docker logs failed",
                          {{"submission_id", std::to_string(task.submission_id)},
                           {"error",         e.what()}}); } catch (...) {}
            logs.clear();
        }
        try_remove(cid, "after wait");

        // 8. Parse the JSON. judge.sh writes one line of JSON; if
        //    multiple lines are present (some early log lines or a
        //    crash trace before the final emit), we take the last
        //    parseable one.
        JudgeResult result = parse_judge_result_json(logs, wres.exit_code);

        // 9. Persist the result.
        try {
            submission_repo::mark_finished(
                *db_, task.submission_id, result.status,
                std::optional<int>(result.time_used_ms),
                std::optional<int>(result.memory_used_kb),
                result.error_message);
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: mark_finished failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"status",        result.status},
                             {"error",         e.what()}}); } catch (...) {}
            // Don't return — fall through to cleanup. The submission
            // will stay in 'running'; the operator can requeue manually.
        }

        try {
            LOG_INFO("judge_scheduler: finished",
                     {{"submission_id", std::to_string(task.submission_id)},
                      {"status",        result.status},
                      {"time_ms",       std::to_string(result.time_used_ms)},
                      {"mem_kb",        std::to_string(result.memory_used_kb)},
                      {"parsed",        result.parsed ? "true" : "false"}});
        } catch (...) {}

        cleanup_and_release(task_dir, pool_id, pool_gave_one);
    }

    // Helper: write the final SE state and exit cleanly.
    void finish_se(int submission_id, const std::string& err) {
        try {
            submission_repo::mark_finished(
                *db_, submission_id, "se",
                std::nullopt, std::nullopt, err);
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: SE mark_finished failed",
                            {{"submission_id", std::to_string(submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
        }
    }

    // Helper: remove a host task tempdir + signal pool.release().
    // Logs all errors and never throws.
    void cleanup_and_release(const std::filesystem::path& task_dir,
                             const std::string& pool_id,
                             bool pool_gave_one) {
        try {
            std::error_code ec;
            std::filesystem::remove_all(task_dir, ec);
            if (ec) {
                try { LOG_WARN("judge_scheduler: remove_all failed",
                              {{"path",  task_dir.string()},
                               {"error", ec.message()}}); } catch (...) {}
            }
        } catch (...) {}
        if (pool_ != nullptr && pool_gave_one && !pool_id.empty()) {
            try { pool_->release(pool_id); } catch (...) {}
        }
    }

    void try_remove(const std::string& cid, const char* context) {
        if (cid.empty()) return;
        try {
            client_->remove(cid, /*force=*/true);
        } catch (const std::exception& e) {
            try { LOG_WARN("judge_scheduler: docker remove failed",
                          {{"context", context ? std::string(context) : std::string("?")},
                           {"id",      cid},
                           {"error",   e.what()}}); } catch (...) {}
        } catch (...) {}
    }

    void try_kill_and_remove(const std::string& cid) {
        if (cid.empty()) return;
        try { client_->kill(cid); } catch (...) {}
        try_remove(cid, "after kill");
    }

    // Build a unique temp dir under cfg_.task_dir_parent for this
    // submission's task.json. Names embed the submission id + a
    // timestamp suffix so concurrent judges don't collide.
    std::filesystem::path make_task_dir(int submission_id) {
        using namespace std::chrono;
        const auto now_us = duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count();
        std::ostringstream name;
        name << "litecode-judge-" << submission_id << "-" << now_us;
        std::filesystem::path d = cfg_.task_dir_parent / name.str();
        std::filesystem::create_directories(d);
        return d;
    }

    // Serialize the task to a JSON file on disk. Shape mirrors
    // judge.sh's parser (see judge/judge.sh Section B).
    void write_task_json(const std::filesystem::path& path,
                         const JudgeTask& task) {
        nlohmann::json j;
        j["submission_id"]       = task.submission_id;
        j["language"]            = task.language;
        j["code"]                = task.code;
        j["time_limit_ms"]       = task.time_limit_ms;
        j["memory_limit_mb"]     = task.memory_limit_mb;
        j["compile_timeout_ms"]  = task.compile_timeout_ms;
        j["run_hard_timeout_ms"] = cfg_.judge_hard_timeout_seconds * 1000;
        j["output_limit_bytes"]  = cfg_.output_limit_bytes;
        j["test_cases"]          = nlohmann::json::array();
        for (const auto& tc : task.test_cases) {
            nlohmann::json e;
            e["input"]           = tc.input;
            e["expected_output"] = tc.expected_output;
            e["judge_type"]      = tc.judge_type;
            if (tc.float_epsilon.has_value()) {
                e["float_epsilon"] = *tc.float_epsilon;
            } else {
                e["float_epsilon"] = nullptr;
            }
            e["order_num"] = tc.order_num;
            j["test_cases"].push_back(std::move(e));
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            throw JudgeSchedulerError("cannot open task.json for writing: " +
                                      path.string());
        }
        out << j.dump();
        if (!out.good()) {
            throw JudgeSchedulerError("task.json write failed: " + path.string());
        }
    }

    // Parse the JSON judge.sh emits on stdout. We look for the LAST
    // `{...}` JSON object in the logs (defensive — some early log
    // lines may precede it). If nothing parses, return a SE result
    // with `parsed=false` so the worker can mark_finished without
    // a try/catch ladder at every call site.
    //
    // Public (vs. the more typical private) so tests/unit/test_judge_scheduler.cpp
    // can exercise the JSON shape handling without spinning up a
    // docker mock for every case.
public:
    static JudgeResult parse_judge_result_json(const std::string& logs,
                                               int wait_exit_code) {
        JudgeResult r;
        if (logs.empty()) {
            r.status = "se";
            r.error_message = "judge produced no stdout";
            return r;
        }
        // Walk backwards looking for the last line starting with '{'.
        // judge.sh's emit_final_json emits a single JSON object on one
        // line at the very end of stdout; earlier lines are g++ /
        // runtime stderr (we suppressed stderr in logs()) or incidental
        // prints. The "last '{' line wins" rule is robust against any
        // prefix noise.
        //
        // We split logs into a vector of lines first (cheap; logs are
        // small — judge.sh stdout is bounded by the 4 KB error truncate
        // and the JSON line itself is < 1 KB). Walking backwards over
        // the vector is simpler than the index-juggling alternative
        // and removes a class of off-by-one bugs at line boundaries.
        std::vector<std::string> lines;
        lines.reserve(8);
        std::string cur;
        for (char c : logs) {
            if (c == '\n') {
                lines.push_back(std::move(cur));
                cur.clear();
            } else if (c != '\r') {  // strip CR (CRLF normalization)
                cur.push_back(c);
            }
        }
        if (!cur.empty()) lines.push_back(std::move(cur));

        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            std::string& line = *it;
            // Trim leading whitespace.
            std::size_t a = 0;
            while (a < line.size() &&
                   (line[a] == ' ' || line[a] == '\t')) ++a;
            if (a >= line.size() || line[a] != '{') continue;
            try {
                auto j = nlohmann::json::parse(line);
                if (!j.is_object()) continue;
                if (j.contains("status") && j["status"].is_string()) {
                    r.status = j["status"].get<std::string>();
                } else {
                    r.status = "se";
                }
                if (j.contains("time_used_ms") && j["time_used_ms"].is_number()) {
                    r.time_used_ms = j["time_used_ms"].get<int>();
                }
                if (j.contains("memory_used_kb") && j["memory_used_kb"].is_number()) {
                    r.memory_used_kb = j["memory_used_kb"].get<int>();
                }
                if (j.contains("error_message") && j["error_message"].is_string()) {
                    r.error_message = j["error_message"].get<std::string>();
                }
                if (j.contains("failed_case_index") &&
                    j["failed_case_index"].is_number()) {
                    r.failed_case_index = j["failed_case_index"].get<int>();
                }
                r.parsed = true;
                return r;
            } catch (...) {
                // Not parseable as JSON; keep walking.
                continue;
            }
        }
        // No parseable JSON line.
        r.status = "se";
        r.error_message = "judge produced no parseable result JSON";
        if (wait_exit_code != 0) {
            r.error_message += " (wait exit=" + std::to_string(wait_exit_code) + ")";
        }
        return r;
    }

    docker::Client*       client_;
    WarmPool*             pool_;
    ConnectionPool*       db_;
    JudgeSchedulerConfig  cfg_;

    std::deque<JudgeTask> queue_;
    mutable std::mutex    queue_mu_;
    std::condition_variable queue_cv_;

    std::vector<std::thread>     workers_;
    std::atomic<std::size_t>     running_count_{0};

    std::atomic<bool>      running_{false};
    std::atomic<bool>      shutting_down_{false};
};

// ────────────────────────────────────────────────────────────────────────────
//  Default-config factory — pulls from JudgeConfig.
// ────────────────────────────────────────────────────────────────────────────

inline JudgeSchedulerConfig make_default_scheduler_config(
    const JudgeConfig& jc,
    std::filesystem::path task_dir_parent = {})
{
    JudgeSchedulerConfig c;
    c.max_concurrent              = jc.max_concurrent_judges;
    c.max_queue_size              = jc.max_queue_size;
    c.compile_timeout_ms          = jc.compile_timeout_seconds * 1000;
    c.judge_hard_timeout_seconds  = jc.judge_hard_timeout_seconds;
    c.output_limit_bytes          = jc.output_limit_bytes;
    c.judge_image                 = jc.judge_image;
    c.network_mode                = jc.network_mode;
    c.task_dir_parent             = std::move(task_dir_parent);
    return c;
}

}  // namespace judge
}  // namespace litecode