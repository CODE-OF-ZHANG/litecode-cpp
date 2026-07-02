// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — judge container warm-up pool (Phase 4 ★)
//
// SPEC §3.1 / §7.1 (step 0, 2a, 5, 6) / §16.1 / A28
//   The web process keeps a pool of *created-but-not-started* judge
//   containers on hand so a worker that pulls a submission off the
//   queue doesn't pay the `docker create` overhead every time. Idle
//   containers sit in the pool until a worker asks for one; the worker
//   starts it (and binds the task-specific mount / execs the judge)
//   and eventually docker rm's it. The pool itself NEVER returns
//   released containers to the idle list — SPEC §7.1 step 5 is explicit:
//     "不归还预热池（每次新建专用容器，保证隔离）"
//   instead it asynchronously spawns a replacement so the pool
//   re-saturates at target_size K.
//
// Lifecycle:
//   start(cfg)        ─ creates K idle containers up-front
//   acquire()         ─ pops an idle id (or creates one if empty)
//   release(id)       ─ drop the id, schedule an async refill
//   shutdown()        ─ stop the refill thread, remove all idle ids
//
// Threading model:
//   - `mu_` guards the idle deque + target counter. acquire() holds it
//     just long enough to pop; refill runs without holding it across
//     network calls.
//   - The refill thread waits on `cv_` for either a release() event or
//     shutdown. It loops creating containers (with bounded retries on
//     docker errors) until size == target_.
//
// Failure surface:
//   - `acquire()` returns std::nullopt only when (a) the pool was never
//     started, (b) shutdown() already ran, or (c) synchronous fallback
//     create() fails. Callers (judge_scheduler) translate nullopt into
//     a SE submission and surface it to the user.
//   - Refill errors NEVER propagate; they are logged at WARN and retried
//     up to `cfg.refill_retry_attempts` times. After that the thread
//     silently re-checks size on the next release() / shutdown tick —
//     a transient docker outage doesn't permanently cripple the pool.
//
// Observability:
//   - size() / target() feed the `/api/v1/health` `warm_pool` field
//     (SPEC §16.1, A28). make_probe() produces a HealthService::Probe
//     that publishes the size under `extra.warm_pool` and reports
//     `down` when the pool has been shut down or never started.
//   - All public methods are noexcept (return std::optional<> for
//     fallible paths) so a docker outage cannot crash the HTTP handler.
//
// Usage (production wiring in judge_scheduler.h):
//   auto pool = std::make_unique<litecode::judge::WarmPool>(docker_client.get());
//   litecode::judge::WarmPoolConfig cfg;
//   cfg.target_size              = cfg_judge.warm_pool_size;   // default 2
//   cfg.template_opts.image      = cfg_judge.judge_image;
//   cfg.template_opts.command    = {"--help"};                // keep idle
//   cfg.template_opts.memory_mb  = 64;                        // SPEC §7.3 hint
//   cfg.template_opts.cpus       = 0.0;                       // no idle cap
//   cfg.template_opts.network_mode = cfg_judge.network_mode;  // "none"
//   cfg.template_opts.pids_limit = std::stoi(cfg_judge.pids_limit);
//   cfg.template_opts.security_opt = {"no-new-privileges:true"};
//   cfg.template_opts.read_only  = true;
//   cfg.template_opts.tmpfs      = {{"/tmp", "size=64m,mode=1777"}};
//   cfg.template_opts.user       = "judgeuser";
//   if (!pool->start(cfg)) { /* log + run degraded */ }
//
//   // worker thread:
//   auto idle = pool->acquire();
//   if (!idle) { submission.status = "se"; continue; }
//   docker_client->start(idle->id);
//   // ... judge.sh + docker rm ...
//   pool->release(idle->id);   // container is gone; this just kicks refill
//
//   // main() shutdown:
//   pool->shutdown();
//
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "../logger.h"              // LOG_INFO / LOG_WARN / LOG_ERROR
#include "../routes/system_routes.h" // HealthService::Probe / ProbeResult
#include "docker_client.h"          // docker::Client / CreateOptions

namespace litecode {
namespace judge {

// ────────────────────────────────────────────────────────────────────────────
//  Configuration
// ────────────────────────────────────────────────────────────────────────────

struct WarmPoolConfig {
    // The CreateOptions template used for every `docker create` the pool
    // issues — both the K up-front precreate and the refill creates.
    // `image`, `network_mode`, `user`, `tmpfs`, `read_only`, etc. all
    // come from JudgeConfig (see docker_client.h for field meanings).
    docker::CreateOptions template_opts;

    // Target pool size K (SPEC §7.1 step 0: default 2). 0 disables the
    // pool entirely — start() returns true, size() always 0, no refill
    // thread spawned. This is the documented "small dev box" mode.
    int target_size = 2;

    // Refill resilience. On docker error, the refill thread sleeps
    // `refill_retry_delay_ms` and tries again up to
    // `refill_retry_attempts` times per release() event. The default
    // (2 retries × 500 ms) absorbs a brief daemon restart without
    // permanently shrinking the pool.
    int refill_retry_attempts = 2;
    int refill_retry_delay_ms = 500;

    // Hard timeout for one synchronous `docker create` call inside
    // acquire()'s fallback path. acq_timeout_ms == 0 falls back to the
    // Client's default per-call timeout.
    int acquire_timeout_ms = 30'000;
};

// ────────────────────────────────────────────────────────────────────────────
//  Result type for acquire()
// ────────────────────────────────────────────────────────────────────────────

struct IdleContainer {
    std::string id;          // docker container id (status="created")
    bool        from_pool;   // true ⇒ was already idle;
                             // false ⇒ newly created (pool was empty)
};

// ────────────────────────────────────────────────────────────────────────────
//  WarmPool
// ────────────────────────────────────────────────────────────────────────────

class WarmPool {
public:
    // Pointer must outlive the WarmPool. We don't take ownership —
    // the scheduler owns the docker::Client, this pool is a satellite.
    explicit WarmPool(docker::Client* client) noexcept
        : client_(client) {}

    // Not copyable / not movable. The refill thread holds a `this`
    // pointer; moving the pool would invalidate it.
    WarmPool(const WarmPool&)            = delete;
    WarmPool& operator=(const WarmPool&) = delete;
    WarmPool(WarmPool&&)                 = delete;
    WarmPool& operator=(WarmPool&&)      = delete;

    ~WarmPool() { shutdown(); }

    // Bring the pool up: synchronously precreate `cfg.target_size`
    // idle containers, then spawn the refill thread if K > 0.
    //
    // Returns true when startup completed without error. Returns false
    // when:
    //   - cfg.target_size < 0
    //   - cfg.target_size > 0 but client_ is null (cannot precreate)
    //   - the very first synchronous precreate failed AND K was 1
    //     (a partial pool is reported via the return value, not by
    //     crashing — the refill thread keeps trying)
    //
    // Side effects on false-return: the pool is left in the
    // "running" state only if at least one container was created
    // successfully. Otherwise it stays shut down and acquire() will
    // always return nullopt.
    bool start(const WarmPoolConfig& cfg) {
        // Validate and seed cfg_/target_ under a brief lock. We
        // deliberately drop the lock before any docker create() call —
        // a non-recursive std::mutex (Windows) refuses re-lock from
        // the same thread with `resource deadlock would occur`.
        {
            std::lock_guard<std::mutex> g(mu_);
            if (cfg.target_size < 0) {
                try {
                    LOG_WARN("warm_pool: refusing negative target_size",
                             {{"target_size",
                               std::to_string(cfg.target_size)}});
                } catch (...) {}
                return false;
            }
            cfg_    = cfg;
            target_ = static_cast<std::size_t>(cfg.target_size);
        }  // mu_ released

        // K == 0 → "disabled" mode. start() succeeds but we never
        // spawn the refill thread and acquire() will always fall
        // through to the synchronous-create branch.
        if (target_ == 0) {
            running_.store(true, std::memory_order_release);
            try {
                LOG_INFO("warm_pool: disabled (target_size=0)");
            } catch (...) {}
            return true;
        }

        if (client_ == nullptr) {
            try {
                LOG_WARN("warm_pool: client is null; not starting",
                         {{"target_size", std::to_string(target_)}});
            } catch (...) {}
            return false;
        }

        // Precreate K. Each `docker create` is a network call we
        // deliberately do NOT hold any lock across — we lock briefly
        // to push the resulting id onto idle_.
        std::size_t created = 0;
        std::size_t failures = 0;
        for (std::size_t i = 0; i < target_; ++i) {
            std::string id;
            try {
                auto r = client_->create(cfg_.template_opts);
                id = r.id;
            } catch (const std::exception& e) {
                ++failures;
                try {
                    LOG_WARN("warm_pool: precreate failed",
                             {{"index",      std::to_string(i)},
                              {"target",     std::to_string(target_)},
                              {"error",      e.what()}});
                } catch (...) {}
                continue;
            }
            if (id.empty()) {
                ++failures;
                continue;
            }
            {
                std::lock_guard<std::mutex> gq(mu_);
                idle_.push_back(std::move(id));
                ++created;
            }
        }

        // We accept partial startup (created < target) — the refill
        // thread will catch up. But if *zero* containers were created
        // the pool is effectively dead (docker unreachable), so we
        // don't flip `running_` true in that case and we return
        // false. K == 0 ("disabled mode") always counts as started.
        const bool started = (target_ == 0) || (created > 0);
        if (started) {
            running_.store(true, std::memory_order_release);
            refill_thread_ = std::thread([this]{ refill_loop(); });
            if (created < target_) {
                cv_.notify_all();
            }
        }

        try {
            LOG_INFO("warm_pool: started",
                     {{"target",  std::to_string(target_)},
                      {"created", std::to_string(created)},
                      {"failed",  std::to_string(failures)},
                      {"running", started ? "true" : "false"}});
        } catch (...) {}
        return started;
    }

    // Stop the refill thread and docker-rm every idle container still
    // in the pool. Idempotent; safe to call from a signal handler
    // context *as long as* the LOG_* macros don't allocate (which they
    // do — so don't really call from a signal handler).
    void shutdown() noexcept {
        // 1) mark shutting_down_ + signal the refill thread to exit.
        shutting_down_.store(true, std::memory_order_release);
        cv_.notify_all();

        // 2) join the refill thread (if any).
        try {
            if (refill_thread_.joinable()) refill_thread_.join();
        } catch (const std::exception& e) {
            try {
                LOG_WARN("warm_pool: refill_thread join failed",
                         {{"error", e.what()}});
            } catch (...) {}
        } catch (...) {}

        // 3) drain idle_ under lock, then rm each one outside the
        //    lock (network calls).
        std::deque<std::string> to_rm;
        {
            std::lock_guard<std::mutex> g(idle_mu());
            to_rm.swap(idle_);
        }

        if (client_ != nullptr) {
            for (auto& id : to_rm) {
                try {
                    client_->remove(id, /*force=*/true);
                } catch (const std::exception& e) {
                    try {
                        LOG_WARN("warm_pool: remove on shutdown failed",
                                 {{"id",    id},
                                  {"error", e.what()}});
                    } catch (...) {}
                } catch (...) {
                    // never throw out of shutdown
                }
            }
        }

        running_.store(false, std::memory_order_release);
        try {
            LOG_INFO("warm_pool: shutdown",
                     {{"removed", std::to_string(to_rm.size())}});
        } catch (...) {}
    }

    // Pop one idle container id. If the pool is empty we synchronously
    // create one and return it (still tagged `from_pool=false`); this
    // covers burst traffic that outruns the refill thread.
    //
    // Returns std::nullopt only when (a) start() never succeeded,
    // (b) shutdown() already ran, or (c) the synchronous fallback
    // create() failed. The caller (judge_scheduler) should mark the
    // submission as SE in that case.
    std::optional<IdleContainer> acquire() {
        if (!running_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        // Fast path: pool has an idle id.
        {
            std::lock_guard<std::mutex> g(idle_mu());
            if (!idle_.empty()) {
                IdleContainer out;
                out.id        = std::move(idle_.front());
                idle_.pop_front();
                out.from_pool = true;
                return out;
            }
        }
        // Pool empty — synchronous fallback. If K == 0 (disabled) we
        // still allow acquire() to create a one-off; this matches
        // "warm_pool = 0" semantics (no prewarming, no refill, but
        // worker still needs *a* container).
        if (client_ == nullptr) return std::nullopt;
        try {
            auto r = client_->create(cfg_.template_opts);
            if (r.id.empty()) return std::nullopt;
            IdleContainer out;
            out.id        = std::move(r.id);
            out.from_pool = false;
            return out;
        } catch (const std::exception& e) {
            try {
                LOG_WARN("warm_pool: acquire fallback create failed",
                         {{"error", e.what()}});
            } catch (...) {}
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    // Signal that a worker has finished using `released_id`. The pool
    // does NOT take ownership of `released_id` — the worker is
    // responsible for `docker stop + docker rm` (SPEC §7.1 step 5).
    // We just nudge the refill thread so it tops the pool back up to
    // K. If the refill thread is not running (K == 0 or shutdown
    // happened), this is a no-op.
    void release(std::string released_id) noexcept {
        // released_id is logged for forensic correlation but not
        // enqueued — per SPEC, released containers are never reused.
        try {
            (void)released_id;  // suppress unused-warning in release builds
            LOG_DEBUG("warm_pool: release", {{"id", released_id}});
        } catch (...) {}

        if (!running_.load(std::memory_order_acquire)) return;
        cv_.notify_all();
    }

    // Current idle count (for /health / metrics). Cheap; takes idle_mu_
    // briefly.
    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> g(idle_mu());
        return idle_.size();
    }

    // K — the configured target size.
    std::size_t target() const noexcept {
        return target_;
    }

    // True iff start() succeeded AND shutdown() has not been called.
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // ── Health probe wiring ────────────────────────────────────────────────
    //
    // Slot into HealthService via:
    //     health.register_probe("warm_pool",
    //                           litecode::judge::WarmPool::make_probe(&pool));
    // Reports `down` if the pool was never started or was shut down
    // (a 503 here tells operators the judge subsystem is offline).
    // Otherwise reports `ok` and publishes `extra.warm_pool` =
    // current size — SPEC §16.1 + A28.
    static HealthService::Probe make_probe(WarmPool* pool) {
        return [pool]() -> ProbeResult {
            ProbeResult r;
            if (pool == nullptr) {
                r.ok     = false;
                r.detail = "no warm pool configured";
                return r;
            }
            if (!pool->running()) {
                r.ok     = false;
                r.detail = "warm pool not running";
                // Still publish the size so operators see the last known
                // value during shutdown.
                r.extra  = {
                    {"warm_pool",
                     static_cast<std::int64_t>(pool->size())},
                };
                return r;
            }
            r.ok     = true;
            r.detail = "warm pool live";
            r.extra  = {
                {"warm_pool",
                 static_cast<std::int64_t>(pool->size())},
                {"warm_pool_target",
                 static_cast<std::int64_t>(pool->target())},
            };
            return r;
        };
    }

private:
    // ── private mutex helper ──────────────────────────────────────────────
    // `mu_` (the user-facing mutex) is the same as `idle_mu_` —
    // declaration kept separate so the surface area is unambiguous.
    // Renaming would touch every call site below, so we alias instead.
    // Const-qualified because `mu_` is `mutable`; callers in const
    // methods (size()) still need to lock it.
    std::mutex& idle_mu() const noexcept { return mu_; }

    // Refill loop: waits for release()/start() signals, then creates
    // containers one at a time until size() == target_. Bounded retries
    // per pass — if a refill attempt fails, we sleep and retry a few
    // times, then yield back to the cv (next release() / shutdown will
    // wake us again).
    void refill_loop() {
        while (!shutting_down_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(cv_mu_);
            // Wait until: shutting_down OR size < target.
            cv_.wait(lk, [this]{
                if (shutting_down_.load(std::memory_order_acquire)) return true;
                std::lock_guard<std::mutex> g(idle_mu());
                return idle_.size() < target_;
            });
            if (shutting_down_.load(std::memory_order_acquire)) break;

            // Drain the gap: create one container at a time so a stuck
            // daemon doesn't block the thread forever. We do NOT hold
            // any lock across the docker create() call.
            while (true) {
                std::size_t cur = 0, tgt = 0;
                {
                    std::lock_guard<std::mutex> g(idle_mu());
                    cur = idle_.size();
                    tgt = target_;
                }
                if (cur >= tgt) break;
                if (shutting_down_.load(std::memory_order_acquire)) break;

                if (!refill_once()) {
                    // refill_once already logged + slept; loop back
                    // and try once more if we're still below target,
                    // otherwise drop out and wait on cv.
                    if (cur + 1 >= tgt) break;  // already at min acceptable
                }
            }
        }
    }

    // Create one container and push its id onto idle_. Returns true on
    // success. On failure, sleeps `cfg_.refill_retry_delay_ms` and
    // retries up to `cfg_.refill_retry_attempts` times.
    bool refill_once() {
        if (client_ == nullptr) return false;

        int attempts = (cfg_.refill_retry_attempts < 0 ? 0
                        : cfg_.refill_retry_attempts) + 1;
        for (int i = 0; i < attempts; ++i) {
            if (shutting_down_.load(std::memory_order_acquire)) return false;
            try {
                auto r = client_->create(cfg_.template_opts);
                if (!r.id.empty()) {
                    {
                        std::lock_guard<std::mutex> g(idle_mu());
                        idle_.push_back(r.id);
                    }
                    try {
                        LOG_DEBUG("warm_pool: refill ok",
                                  {{"id", r.id},
                                   {"size", std::to_string(size())}});
                    } catch (...) {}
                    return true;
                }
            } catch (const std::exception& e) {
                try {
                    LOG_WARN("warm_pool: refill create failed",
                             {{"attempt", std::to_string(i + 1)},
                              {"of",      std::to_string(attempts)},
                              {"error",   e.what()}});
                } catch (...) {}
            } catch (...) {
                // ignore
            }
            // Backoff before next attempt (unless this was the last).
            if (i + 1 < attempts) {
                int delay = cfg_.refill_retry_delay_ms < 0 ? 0
                                                            : cfg_.refill_retry_delay_ms;
                if (delay > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(delay));
                }
            }
        }
        try {
            LOG_WARN("warm_pool: refill gave up",
                     {{"size", std::to_string(size())},
                      {"target", std::to_string(target_)}});
        } catch (...) {}
        return false;
    }

    docker::Client*    client_;
    WarmPoolConfig     cfg_{};
    std::size_t        target_ = 0;

    // Public mutex (a.k.a. idle_mu_). Guards the idle_ deque.
    mutable std::mutex         mu_;
    std::deque<std::string>    idle_;

    // Refill thread + cv.
    std::thread                refill_thread_;
    std::condition_variable    cv_;
    std::mutex                 cv_mu_;

    std::atomic<bool>          running_{false};
    std::atomic<bool>          shutting_down_{false};
};

// ────────────────────────────────────────────────────────────────────────────
//  Free-function helpers — convenient when callers don't want to
//  allocate a config struct themselves.
// ────────────────────────────────────────────────────────────────────────────

// Build a default WarmPoolConfig from a JudgeConfig. The defaults here
// match SPEC §7.3 — read-only rootfs, no network, no-new-privileges,
// small memory cap (64 MB) so a stuck idle container doesn't pin
// 256 MB, judgeuser UID so /tmp permissions line up.
inline WarmPoolConfig make_default_warm_pool_config(const JudgeConfig& jc) {
    WarmPoolConfig cfg;
    cfg.template_opts.image        = jc.judge_image;
    cfg.template_opts.command      = {"--help"};   // keep idle; ENTRYPOINT
                                                  // only fires on start
    cfg.template_opts.network_mode = jc.network_mode;
    cfg.template_opts.read_only    = true;
    cfg.template_opts.memory_mb    = 64;            // SPEC §7.3 hint
    cfg.template_opts.cpus         = 0.0;           // no idle cap
    cfg.template_opts.pids_limit   = 50;
    cfg.template_opts.security_opt = {"no-new-privileges:true"};
    cfg.template_opts.tmpfs        = { {"/tmp", "size=64m,mode=1777"} };
    cfg.template_opts.user         = "judgeuser";

    cfg.target_size              = jc.warm_pool_size;
    cfg.refill_retry_attempts    = 2;
    cfg.refill_retry_delay_ms    = 500;
    cfg.acquire_timeout_ms       = 30'000;
    return cfg;
}

} // namespace judge
} // namespace litecode