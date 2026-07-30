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
#include "../db/checkin_repo.h"         // checkin_repo::try_checkin (Phase 7 ★ 打卡系统)
#include "../logger.h"                  // LOG_* / RequestIdScope
#include "../routes/metrics.h"          // MetricsService (Phase 9 ★ v1.2.68)
#include "../routes/system_routes.h"    // HealthService::Probe / ProbeResult
#include "docker_client.h"              // docker::Client / CreateOptions
#include "judge_notifier.h"             // JudgeNotifier (Phase 4 ★ SSE)
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

    // ─── Special Judge 框架 (Phase 4 ☆ v1.2.18) ───
    //
    // Loaded from the problem_special_judges table by the submission
    // route when it builds the JudgeTask (see special_judge_repo.h).
    // judge.sh treats an empty `spj_source` as "no SPJ attached",
    // in which case every judge_type=special case folds to WA (the
    // operator can iterate without SE flooding the dashboard).
    //
    // `spj_language` is reserved for future py/java/... — only
    // "cpp" is honored by judge.sh today (the litecode-judge image
    // is C++-only, see judge/Dockerfile).
    std::string                spj_source;
    std::string                spj_language;       // "cpp" | "" (empty == unset)
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

// v1.3.4 PR 3 — per-case payload populated by judge.sh's `case_results`
// JSONL and surfaced through the synchronous run-samples endpoint. The
// base JudgeScheduler (async path) does NOT serialize these fields — it
// keeps the small envelope — because the existing `submissions` row
// only persists aggregate verdict + timings. The SampleRunner fills
// the full CaseResult vector and lets the route handler stream it as
// the wire response.
struct CaseResult {
    int         index           = 0;
    std::string status;                     // "ac" | "wa" | "tle" | "mle" | "re" | "ole" | "pe" | "se" | "skipped"
    int         time_ms         = 0;
    int         mem_kb          = 0;
    std::string info;                       // short reason string; non-null only on failure
    // Populated ONLY by synchronous run-samples; judge.sh writes these
    // into case_results.jsonl on disk. Truncated to ~4 KB / 1 KB by
    // judge.sh to keep the JSONL line well under any sensible buffer.
    std::string input;                      // raw stdin fed to user program
    std::string expected_output;
    std::string actual_output;
    // NOTE: field is `case_stderr`, NOT `stderr`. The latter is a
    // POSIX macro defined by `<cstdio>` to a FILE* and breaks MSVC
    // parsing ("error C2059: 语法错误:'常数'"). Wire JSON key stays
    // "stderr" — only the C++ identifier is renamed.
    std::string case_stderr;
};

struct JudgeResult {
    std::string status            = "se";
    int         time_used_ms      = 0;
    int         memory_used_kb    = 0;
    std::string error_message;
    int         failed_case_index = -1;     // -1 ⇒ no case failed
    bool        parsed            = false;  // true ⇒ came from a clean JSON parse
    // v1.3.4 PR 3 — populated by parse_judge_result_json() whenever
    // judge.sh emits a `case_results` array (which is now always). Empty
    // for backward compatibility (legacy judges without the array).
    std::vector<CaseResult> case_results;
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

    // v1.2.50: docker named-volume name that BOTH the web
    // container's task_dir_parent mount AND the judge container's
    // task.json mount resolve to. When non-empty, the judge
    // container is created with a `"Type": "volume"` mount instead
    // of a bind mount — the named volume's backing dir lives in the
    // docker daemon's storage area so it's reachable on Docker
    // Desktop too (where bind-mount source paths break across the
    // host-VM boundary). When empty, legacy bind-mount behavior.
    std::string task_volume_name;
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
    // main() owns the docker client, the warm pool, the DB pool, and
    // (Phase 4 ★ SSE) the JudgeNotifier.
    JudgeScheduler(docker::Client*   client,
                   WarmPool*         pool,
                   ConnectionPool*   db,
                   JudgeSchedulerConfig cfg) noexcept
        : client_(client), pool_(pool), db_(db), cfg_(std::move(cfg)) {}

    // Wire a JudgeNotifier (Phase 4 ★ SSE). After this call, the
    // worker publishes a result row to the notifier on every
    // mark_finished() so SSE clients watching the submission can
    // unblock. Passing nullptr disables the publish side (the
    // "notifier not configured" path — same as a missing docker
    // client, dev boxes can still POST submissions but the SSE
    // endpoint will time out). The pointer must outlive the
    // scheduler; the scheduler does not take ownership.
    void set_notifier(JudgeNotifier* notifier) noexcept {
        notifier_ = notifier;
    }

    JudgeNotifier* notifier() const noexcept { return notifier_; }

    // Wire a MetricsService (Phase 9 ★ v1.2.68). After this call the
    // worker observes a `judge_duration_seconds` sample + bumps
    // `submissions_total{status=...}` on every finished task. Passing
    // nullptr disables the metrics side (the "metrics not configured"
    // path — common in unit tests; production main.cpp always wires a
    // live service so /api/v1/metrics has signal). The pointer must
    // outlive the scheduler; the scheduler does not take ownership.
    void set_metrics(MetricsService* metrics) noexcept {
        metrics_ = metrics;
    }

    MetricsService* metrics() const noexcept { return metrics_; }

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

    // Configured queue capacity (SPEC §15.5). 0 means "unbounded"
    // (test-only; production always sets a finite limit so 503 surfaces
    // on overflow — see enqueue()). Surfaced for the /api/v1/admin/queue
    // route so the operator-facing gauge can show "size / max_queue_size".
    int max_queue_size() const noexcept {
        return cfg_.max_queue_size;
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
                {"max_queue_size", static_cast<std::int64_t>(s->max_queue_size())},
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
        // Phase 9 ★ v1.2.68 — capture wall-clock start as early as
        // possible so the observed `judge_duration_seconds` sample
        // includes everything the worker did for this submission
        // (parse → docker run → docker wait → json parse). The
        // steady_clock is monotonic so clock skew across workers
        // can't make a sample negative.
        const auto task_started_at = std::chrono::steady_clock::now();

        // Null docker client ⇒ every docker call would crash. Bail
        // out with SE before doing anything; this is the documented
        // "dev box without docker" path.
        if (client_ == nullptr) {
            try {
                LOG_ERROR("judge_scheduler: no docker client; task rejected",
                          {{"submission_id", std::to_string(task.submission_id)}});
            } catch (...) {}
            finish_se(task.submission_id, task_started_at,
                      "no docker client configured");
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
            try { LOG_INFO("judge_scheduler: task_dir created",
                           {{"submission_id", std::to_string(task.submission_id)},
                            {"task_dir",      task_dir.string()}}); } catch (...) {}
            task_file = task_dir / "task.json";
            write_task_json(task_file, task);
            try { LOG_INFO("judge_scheduler: task.json written",
                           {{"submission_id", std::to_string(task.submission_id)},
                            {"task_file",     task_file.string()},
                            {"size",          std::to_string(std::filesystem::file_size(task_file))}}); } catch (...) {}
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: task.json write failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            finish_se(task.submission_id, task_started_at,
                      std::string("failed to write task.json: ") + e.what());
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

        // 4. Create per-task container with task.json mount + JUDGE_TASK_FILE env.
        //    v1.2.50: when cfg_.task_volume_name is set, mount a NAMED
        //    VOLUME rather than bind-mounting task_file. Both the web
        //    container and the judge container see the same backing
        //    storage of the named volume, regardless of host OS
        //    filesystem quirks (Docker Desktop host-VM boundary
        //    included). Empty task_volume_name → fall back to legacy
        //    bind mount (keeps Linux-host dev / lab setups working).
        std::string cid;
        try {
            docker::CreateOptions opts;
            opts.image        = cfg_.judge_image;
            opts.command      = {"/usr/local/bin/judge.sh"};
            opts.env          = {"JUDGE_TASK_FILE=" + task_file.string(),
                                  "JUDGE_HOME=/judge",
                                  "JUDGE_TMP=/tmp/judge"};
            opts.network_mode = cfg_.network_mode;
            opts.read_only    = true;
            opts.memory_mb    = task.memory_limit_mb;
            opts.cpus         = 0.0;             // rely on docker wait timeout for the wall clock
            opts.pids_limit   = 50;
            opts.security_opt = {"no-new-privileges:true"};
            // tmpfs mounts for the per-task container:
            //   /tmp   — judge.sh mktemp dir, compile stderr file,
            //            per-case stdin files, JSONL of case_results
            //   /judge — judge.sh stages solution.cpp here, then
            //            g++ writes the compiled `solution` binary
            //            here. /judge is WORKDIR. The per-task
            //            container's --read-only rootfs blocks the
            //            printf at judge.sh:262 otherwise (returns
            //            shell exit 1 with no JSON stdout → SE).
            //
            // Both mounts MUST carry `exec` because Docker's tmpfs
            // default is noexec. With noexec, `[ -x /judge/solution ]`
            // returns false even when the binary is built fine —
            // judge.sh then exits "compile returned 0 but binary
            // missing" (also SE). (Hit + diagnosed v1.2.50.)
            opts.tmpfs        = { {"/tmp",  "size=64m,mode=1777,exec"},
                                   {"/judge","size=64m,mode=1777,exec"} };
            opts.user         = "judgeuser";
            opts.working_dir  = "/judge";
            if (!cfg_.task_volume_name.empty()) {
                // Named-volume path — mount the same volume web writes
                // to at the SAME base path the worker stages task.json
                // at. Both the web container and the per-task judge
                // container mount "task-volume" at /tmp/litecode-judge,
                // then web writes /tmp/litecode-judge/<subdir>/task.json
                // and the judge reads /tmp/litecode-judge/<subdir>/task.json.
                // The named volume has no separate "host source" — its
                // backing dir lives in /var/lib/docker/volumes so the
                // same path resolves on both sides of the docker-proxy
                // boundary, including Docker Desktop Windows/macOS where
                // a bind mount would be invisible to the host VM.
                opts.mounts = { docker::Mount::volume_mount(
                    cfg_.task_volume_name,
                    "/tmp/litecode-judge",
                    /*read_only=*/true) };
            } else {
                opts.mounts = { docker::Mount::bind_mount(
                    task_file.string(),
                    "/tmp/task.json",
                    /*read_only=*/true) };
            }

            auto cr = client_->create(opts);
            if (cr.id.empty()) {
                finish_se(task.submission_id, task_started_at,
                          "docker create returned empty id");
                cleanup_and_release(task_dir, pool_id, pool_gave_one);
                return;
            }
            cid = cr.id;
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: docker create failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            finish_se(task.submission_id, task_started_at,
                      std::string("docker create failed: ") + e.what());
            cleanup_and_release(task_dir, pool_id, pool_gave_one);
            return;
        }

        // 5. Start the container.
        try {
            client_->start(cid);
        } catch (const std::exception& e) {
            try_remove(cid, "after start failure");
            finish_se(task.submission_id, task_started_at,
                      std::string("docker start failed: ") + e.what());
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
            finish_se(task.submission_id, task_started_at, "judge exceeded hard timeout (" +
                std::to_string(cfg_.judge_hard_timeout_seconds) + "s)");
            cleanup_and_release(task_dir, pool_id, pool_gave_one);
            return;
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: docker wait failed",
                            {{"submission_id", std::to_string(task.submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
            try_kill_and_remove(cid);
            finish_se(task.submission_id, task_started_at,
                      std::string("docker wait failed: ") + e.what());
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
            try { LOG_WARN("judge_scheduler: docker logs (stdout) failed",
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
        bool persisted = false;
        try {
            persisted = submission_repo::mark_finished(
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

        // 9a. v1.2.68 (Phase 9 ★) — record the histogram sample +
        // counter increment for the terminal status we just
        // persisted. Only counted when mark_finished actually
        // flipped the row (mirrors the notifier_ publish guard below
        // — we never want to double-count an "already terminal" row
        // because a sweep or recovery re-ran run_one_task). The
        // duration histogram is observed at the wall-clock level
        // (task_started_at → now); status comes from the JudgeResult
        // we just parsed (ac / wa / tle / mle / ole / pe / ce / re /
        // se — every kStatus* constant except the transient pending /
        // running pair).
        if (persisted) {
            record_task_metrics(task_started_at, result.status);

            // Phase 7 ★ 打卡系统: AC 成功后尝试打卡
            if (result.status == "ac") {
                try {
                    checkin_repo::try_checkin(
                        *db_, task.user_id, task.problem_id, task.submission_id);
                } catch (const std::exception& e) {
                    // 打卡失败不影响判题结果，只记录日志
                    try {
                        LOG_WARN("checkin failed",
                                 {{"user_id", std::to_string(task.user_id)},
                                  {"submission_id", std::to_string(task.submission_id)},
                                  {"error", e.what()}});
                    } catch (...) {}
                }
            }
        }

        // 9b. Phase 4 ★ SSE — publish the result to the notifier.
        //     The notifier wakes every SSE handler that subscribed
        //     via GET /api/v1/submissions/sse/:id. We only publish
        //     when mark_finished() actually flipped the row (a
        //     defensive guard against the "row was already terminal"
        //     case — there are no SSE subscribers to wake in that
        //     path anyway, and re-publishing a stale row would
        //     surprise clients that connected after the row was
        //     already done).
        //
        //     We assemble the SubmissionRow from the task +
        //     mark_finished result rather than re-reading the DB
        //     (one round-trip saved on every judge). The row's
        //     status / time / mem / error_message come from
        //     `result`; the rest is identity (id / user_id /
        //     problem_id / language) and code (which the SSE
        //     handler does not include anyway — code is a detail
        //     field). finished_at is left nullopt because we did
        //     not SELECT it; the SSE client doesn't strictly need
        //     it (the publish event signals "result is ready, GET
        //     /:id for the full row if you want finished_at").
        if (persisted && notifier_ != nullptr) {
            try {
                SubmissionRow published;
                published.id          = task.submission_id;
                published.user_id     = task.user_id;
                published.problem_id  = task.problem_id;
                published.language    = task.language;
                published.code        = task.code;
                published.status      = result.status;
                published.time_used   = result.time_used_ms > 0
                                            ? std::optional<int>(result.time_used_ms)
                                            : std::nullopt;
                published.memory_used = result.memory_used_kb > 0
                                            ? std::optional<int>(result.memory_used_kb)
                                            : std::nullopt;
                published.error_message = result.error_message.empty()
                                            ? std::optional<std::string>()
                                            : std::optional<std::string>(result.error_message);
                // created_at / finished_at: leave as defaults
                // (empty / nullopt). The SSE handler's
                // serialize_submission_row treats empty
                // created_at as "not loaded" and the client
                // interprets that as "poll /:id for the full
                // record". Today's SSE clients always re-fetch
                // the row via GET /:id on the publish event, so
                // the truncation is invisible.
                (void)notifier_->publish(published);
            } catch (...) {
                // The publish path itself doesn't throw (we wrap
                // every callback invocation in try/catch), but the
                // SubmissionRow construction might. Swallow.
            }
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
    //
    // v1.2.68 (Phase 9 ★): also records the judge-duration histogram
    // sample + increments `submissions_total{status="se"}` exactly
    // once per SE exit, via record_task_metrics(). Centralizing the
    // metric emit here means every failure branch counts toward the
    // counter (and contributes a sample to judge_duration_seconds)
    // without each call site duplicating the bookkeeping.
    void finish_se(int submission_id,
                   std::chrono::steady_clock::time_point started_at,
                   const std::string& err) {
        try {
            submission_repo::mark_finished(
                *db_, submission_id, "se",
                std::nullopt, std::nullopt, err);
        } catch (const std::exception& e) {
            try { LOG_ERROR("judge_scheduler: SE mark_finished failed",
                            {{"submission_id", std::to_string(submission_id)},
                             {"error",         e.what()}}); } catch (...) {}
        }
        record_task_metrics(started_at, kStatusSE);
    }

    // Helper: emit judge_duration_seconds + submissions_total labels
    // for the just-finished task. Safe to call when metrics_ is null
    // (no-op). Always uses status=kStatus* constants; an unexpected
    // status string lands in the "se" bucket so we never silently
    // drop a counter increment.
    //
    // Why a wall-clock sample (not judge.sh's time_used_ms):
    //   - judge.sh reports the user-code runtime; the histogram we
    //     publish is the END-TO-END worker duration — includes docker
    //     create + start + wait + log pull + parse. P99 alerts over
    //     this metric track wall-clock latency the operator actually
    //     cares about.
    void record_task_metrics(std::chrono::steady_clock::time_point started_at,
                             std::string_view status) {
        if (metrics_ == nullptr) return;
        const auto elapsed_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - started_at).count();
        try {
            metrics_->observe_histogram("litecode_judge_duration_seconds",
                                        elapsed_seconds);
        } catch (...) {
            // metric sink must never crash a worker
        }
        try {
            metrics_->inc_counter("litecode_submissions_total", status);
        } catch (...) {
            // ditto — the noop-on-unknown path inside the service
            // is sufficient; this catch is defensive against future
            // MetricsService API changes that might throw.
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
        // Special Judge (Phase 4 ☆). Empty spj_source ⇒ judge.sh's
        // special-type cases fall back to "no SPJ ⇒ WA" (the operator
        // can iterate without flooding 'se'). Empty spj_language is
        // tolerated and treated identically to "no SPJ attached".
        if (task.spj_source.empty()) {
            j["special_judge_source"] = nullptr;
        } else {
            j["special_judge_source"] = task.spj_source;
        }
        if (task.spj_language.empty()) {
            j["special_judge_language"] = nullptr;
        } else {
            j["special_judge_language"] = task.spj_language;
        }
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
            if (wait_exit_code != 0) {
                r.error_message += " (wait exit=" + std::to_string(wait_exit_code) + ")";
            }
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
                // v1.3.4 PR 3 — parse `case_results[]` if present. Each
                // element is read leniently: missing or wrong-typed
                // fields fall back to defaults rather than dropping the
                // whole row, so a partial upgrade of judge.sh on one
                // node can't sink the wire response.
                if (j.contains("case_results") && j["case_results"].is_array()) {
                    for (const auto& cj : j["case_results"]) {
                        if (!cj.is_object()) continue;
                        CaseResult c;
                        if (cj.contains("index") && cj["index"].is_number()) {
                            c.index = cj["index"].get<int>();
                        }
                        if (cj.contains("status") && cj["status"].is_string()) {
                            c.status = cj["status"].get<std::string>();
                        }
                        if (cj.contains("time_ms") && cj["time_ms"].is_number()) {
                            c.time_ms = cj["time_ms"].get<int>();
                        }
                        if (cj.contains("mem_kb") && cj["mem_kb"].is_number()) {
                            c.mem_kb = cj["mem_kb"].get<int>();
                        }
                        if (cj.contains("info") && cj["info"].is_string()) {
                            c.info = cj["info"].get<std::string>();
                        }
                        if (cj.contains("input") && cj["input"].is_string()) {
                            c.input = cj["input"].get<std::string>();
                        }
                        if (cj.contains("expected_output") && cj["expected_output"].is_string()) {
                            c.expected_output = cj["expected_output"].get<std::string>();
                        }
                        if (cj.contains("actual_output") && cj["actual_output"].is_string()) {
                            c.actual_output = cj["actual_output"].get<std::string>();
                        }
                        if (cj.contains("stderr") && cj["stderr"].is_string()) {
                            c.case_stderr = cj["stderr"].get<std::string>();
                        }
                        r.case_results.push_back(std::move(c));
                    }
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
    JudgeNotifier*        notifier_ = nullptr;   // Phase 4 ★ SSE — non-owning
    MetricsService*       metrics_ = nullptr;    // Phase 9 ★ v1.2.68 Prometheus — non-owning
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
    c.task_volume_name            = jc.task_volume_name;
    return c;
}

}  // namespace judge
}  // namespace litecode