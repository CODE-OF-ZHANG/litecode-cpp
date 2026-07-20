// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext metrics factory (Phase 9 ★ v1.2.68)
//
// Defines litecode::build_metrics_deps(). One TU that pulls in
// routes/metrics.h + the judge / db subsystems the gauge providers
// capture. Split out of main.cpp so main.cpp stays ODR-safe (per
// memory `reference-odr-collision-msvc`).
//
// Gauge provider lambdas:
//   - The providers are std::function<int64_t()> capturing raw
//     pointers (judge::JudgeScheduler*, judge::WarmPool*,
//     ConnectionPool*). This is intentional — the AppContext owns
//     the unique_ptrs; the MetricsService borrows. main.cpp's
//     shutdown order drains the scheduler + warm_pool BEFORE the
//     MetricsService unique_ptr in AppContext goes out of scope,
//     so the lambdas never dereference a freed pointer.
//   - Each provider is null-safe (`ptr ? value : 0`). The
//     "subsystem not configured" case (test harness with no docker
//     client, db_pool never built, etc.) renders the gauge as a
//     literal `0` rather than crashing the scrape. Promtool logs a
//     warning when a gauge holds zero forever — that's useful
//     signal on a misconfigured boot, not a bug.

#include "app_context_deps.h"
#include "db/connection_pool.h"
#include "judge/judge_scheduler.h"
#include "judge/warm_pool.h"
#include "routes/metrics.h"

namespace litecode {

namespace {

// Prometheus bucket boundaries for judge_duration_seconds (SPEC §16.4).
// Picked for resolution around the 1s "AC ordinary compile" tier and
// the 5s P99 alert threshold (monitoring/alerting/prometheus-alerts.yml).
// We deliberately do NOT extend past 10s because the worker has a
// 30s hard watchdog; samples beyond 10s are pathological (still
// captured in the +Inf bucket for analytics, but not surfaced in
// P95/P99 dashboards as their own bucket).
const std::vector<double> kJudgeDurationBuckets = {
    0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
};

} // namespace

MetricsDeps build_metrics_deps(const DbDeps&    db,
                                const JudgeDeps& judge) {
    MetricsDeps out;
    out.metrics = std::make_unique<MetricsService>();

    // ── counters ─────────────────────────────────────────────────────
    // The label_key "status" is the standard Prometheus key for a
    // submissions-status counter. The label_value is one of the
    // kStatus* constants (ac / wa / tle / mle / ole / pe / ce / re /
    // se). We don't enumerate the label set up-front — JudgeScheduler
    // calls inc_counter() with whatever string judge.sh decided on,
    // and Prometheus happily renders whichever label values arrive.
    out.metrics->register_counter(
        "litecode_submissions_total",
        "Total judged submissions, labeled by final status.");

    // ── histograms ───────────────────────────────────────────────────
    out.metrics->register_histogram(
        "litecode_judge_duration_seconds",
        "End-to-end judge task duration in seconds, from "
        "worker pickup to mark_finished.",
        kJudgeDurationBuckets);

    // ── gauges — sampled at scrape time ──────────────────────────────

    // Queue size: JudgeScheduler::queue_size() returns the pending
    // count (not yet picked up). Together with running_count below,
    // these give operators the "load" picture (Phase 9 dashboard).
    out.metrics->register_gauge(
        "litecode_judge_queue_size",
        "Pending submissions waiting for a judge worker.",
        [sched = judge.scheduler.get()]() -> std::int64_t {
            return sched ? static_cast<std::int64_t>(sched->queue_size())
                         : static_cast<std::int64_t>(0);
        });

    // Running count: JudgeScheduler::running_count() returns the
    // in-flight worker count. Always <= max_concurrent; surfaces
    // whether all workers are hot.
    out.metrics->register_gauge(
        "litecode_judge_running_count",
        "Judge workers currently inside run_one_task().",
        [sched = judge.scheduler.get()]() -> std::int64_t {
            return sched ? static_cast<std::int64_t>(sched->running_count())
                         : static_cast<std::int64_t>(0);
        });

    // Warm pool: WarmPool::size() returns the current idle count
    // (containers not yet handed out). Provider is null-safe so the
    // "no docker socket" case still scrapes.
    out.metrics->register_gauge(
        "litecode_judge_warm_pool_size",
        "Idle judge containers sitting in the warm pool.",
        [pool = judge.warm_pool.get()]() -> std::int64_t {
            return pool ? static_cast<std::int64_t>(pool->size())
                        : static_cast<std::int64_t>(0);
        });

    // Warm pool target: shows operators the K vs current size
    // relationship — useful because the SPEC §16.4
    // JudgeWarmPoolDepleted alert fires on size==0 for 3m regardless
    // of target.
    out.metrics->register_gauge(
        "litecode_judge_warm_pool_target",
        "Configured target size of the warm pool (K).",
        [pool = judge.warm_pool.get()]() -> std::int64_t {
            return pool ? static_cast<std::int64_t>(pool->target())
                        : static_cast<std::int64_t>(0);
        });

    // DB pool active: ConnectionPool::stats().active counts in-flight
    // RAII handles. 0 = idle pool; pool_max_size = saturated.
    // Exposing via a provider (vs. copying the counter into metrics
    // on every acquire/release) means a bug or ODR mismatch on
    // PoolStats can't desync the metrics from reality — the gauge
    // always reports the canonical .stats() snapshot.
    out.metrics->register_gauge(
        "litecode_db_pool_active",
        "MySQL sessions currently checked out of the connection pool.",
        [db_pool = db.pool.get()]() -> std::int64_t {
            if (!db_pool) return static_cast<std::int64_t>(0);
            try {
                return static_cast<std::int64_t>(db_pool->stats().active);
            } catch (...) {
                // stats() takes its own mutex and doesn't throw,
                // but defensive: a thrown provider renders NaN, not
                // 0. We swallow to keep the gauge at 0.
                return static_cast<std::int64_t>(0);
            }
        });

    return out;
}

} // namespace litecode
