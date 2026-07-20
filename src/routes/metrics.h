// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Prometheus /metrics exposition (Phase 9 ★ v1.2.68)
//
// SPEC §11 Phase 9 / §10 metrics.h
//   A small, header-only Prometheus text-format exposition library that
//   exposes GET /api/v1/metrics for Prometheus to scrape. Provides three
//   metric types out of the box:
//
//     - counter   litecode_submissions_total{status="ac|wa|..."}
//                 Every judge.sh decision increments exactly one label
//                 value; Prometheus `rate()` / `increase()` over this is
//                 the canonical submission count (per status, per
//                 environment, etc.).
//
//     - histogram litecode_judge_duration_seconds
//                 `_bucket{le="..."}`, `_sum`, `_count`. Prometheus
//                 `histogram_quantile(0.99, rate(_bucket[5m]))` is the
//                 SPEC §16.4 P99 alert source. Bucket boundaries cover
//                 5ms → 10s with extra resolution around the 1s and 5s
//                 SPEC thresholds.
//
//     - gauge     litecode_judge_queue_size
//                 litecode_judge_running_count
//                 litecode_judge_warm_pool_size
//                 litecode_db_pool_active
//                 Sampled at scrape time via a std::function<int64_t()>
//                 provider — pulled live from the underlying subsystem
//                 (JudgeScheduler::queue_size(), WarmPool::size(),
//                 ConnectionPool::stats().active), so the gauge cannot
//                 drift away from reality and no extra periodic thread
//                 is needed.
//
// Why a self-contained library, not the cpp-httplib-internal
// health endpoint? Because:
//   - SPEC §16.4 P99 alert queries use the `_bucket` series (histogram),
//     which is a strict superset of what a counter / gauge alone can
//     express. We pay the histogram cost only for one timer
//     (`judge_duration_seconds`) and keep the rest cheap.
//   - Promscrape needs the exposition format exactly
//     (line order, `# HELP` / `# TYPE`, label ordering, +Inf bucket
//     name, no trailing newline mangling). Re-using a 3rd-party
//     `prometheus-cpp` would add a heavy dependency for ~150 LOC.
//
// Threading model:
//   - inc_counter() + observe_histogram() take a brief internal mutex
//     per family. Contention is negligible (judge workers update at most
//     once per task; db pool counters update per acquire / release —
//     but the mutex is held only for an atomic increment + map[label]
//     insert, on the microsecond order).
//   - Gauge providers run inside render()'s mutex, so a slow provider
//     (e.g. a stuck Docker ping) can stall a /metrics scrape. We
//     deliberately do NOT timebox providers here — Prometheus' scrape
//     timeout is the right backstop (config: `scrape_timeout: 10s` in
//     monitoring/prometheus.yml).
//   - register_*() must NOT be called concurrently with render();
//     registration is single-threaded (boot path), and render() is
//     called by the HTTP handler thread (multi-threaded across
//     scrapes, but Prometheus-side scrape interval is 15s).
//
// Why header-only:
//   - Mirrors HealthService / RateLimiter / ConnectionPool — every
//     other Phase 1/2/4+ subsystem is header-only + inline. Keeps
//     the per-route .cpp file pattern in main.cpp the same.
//   - The compiler can inline render() into the lambda the route
//     handler captures, shaving a function call / 50 KiB out of the
//     hot scrape path on cold caches.
//
// Usage (boot in main.cpp):
//   litecode::MetricsService metrics;
//   metrics.register_counter("litecode_submissions_total",
//       "Total judged submissions by final status (ac|wa|tle|mle|ole|pe|ce|re|se).");
//   metrics.register_histogram("litecode_judge_duration_seconds",
//       "End-to-end judge task duration (seconds, judge.sh wall-clock).",
//       {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10});
//   metrics.register_gauge("litecode_judge_queue_size",
//       "Pending submissions waiting for a judge worker.",
//       [sched]{ return sched ? (int64_t)sched->queue_size() : 0; });
//   ...
//   metrics.register_gauge("litecode_judge_warm_pool_size",
//       "Idle judge containers sitting in the warm pool.",
//       [pool]{ return pool ? (int64_t)pool->size() : 0; });
//   metrics.register_gauge("litecode_db_pool_active",
//       "MySQL sessions currently checked out of the pool.",
//       [db]{ return db ? (int64_t)db->stats().active : 0; });
//   ...
//   // Hand the MetricsService into JudgeScheduler so run_one_task can
//   // call observe + inc at finish():
//   scheduler->set_metrics(&metrics);
//   ...
//   litecode::register_metric_routes(server, metrics);
//
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../server.h"          // HttpServer
#include "../logger.h"          // LOG_WARN

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  MetricsService — Prometheus text-format exposition library.
//
//  All public methods are thread-safe to call concurrently with each
//  other. Registration methods (register_*) are intended to run during
//  boot only; render() is the only method exposed to the HTTP handler
//  thread.
// ────────────────────────────────────────────────────────────────────────────

class MetricsService {
public:
    // Gauge provider — called inside render() under the global mutex.
    // MUST be non-throwing; a throwing provider is treated as 0 and
    // logged at warn level once.
    using GaugeProvider = std::function<std::int64_t()>;

    MetricsService()                              = default;
    MetricsService(const MetricsService&)         = delete;
    MetricsService& operator=(const MetricsService&) = delete;
    MetricsService(MetricsService&&)              = delete;
    MetricsService& operator=(MetricsService&&)   = delete;

    // ── counter (single-label family) ────────────────────────────────────
    //
    // increment(key) — bump the labelled counter for `key`. The first
    // call with a fresh `key` inserts a label-value pair with value 1.
    //
    // The label name is fixed at registration time so we don't have to
    // thread a label_set through inc() — every call site in this
    // codebase only ever labels `submissions_total` with `status=...`.
    // If we ever need multi-label counters, we'd swap the registry map
    // for a `std::vector<std::pair<label,value>>` + a render-side join.
    //
    // `key` MUST be a defined submission status (kStatus* from
    // submission_repo.h) — there's no validation here on purpose
    // (avoids a string_view→string copy on the hot path); an unknown
    // key still renders correctly as a new label set, Prometheus
    // accepts it, and the dashboard team sees the typo immediately.
    //
    // MUST be called after register_counter() with the same name.
    // Calling it on an unregistered name is a no-op + a single WARN
    // so a missed wiring doesn't crash the judge worker at 3am.
    void inc_counter(std::string_view name, std::string_view key) noexcept {
        std::lock_guard<std::mutex> g(reg_mu_);
        auto it = counters_.find(std::string(name));
        if (it == counters_.end()) {
            // Caller bug: registered no counter with this name. Don't
            // crash a judge worker; warn once and drop the inc.
            warn_unknown_metric_once(std::string(name));
            return;
        }
        std::lock_guard<std::mutex> cg(it->second->mu);
        ++it->second->values[std::string(key)];
    }

    // ── histogram ────────────────────────────────────────────────────────
    //
    // observe(name, value) — record one sample. Buckets are cumulative
    // (Prometheus contract): bucket[i] counts how many samples had
    // value <= bucket_bounds[i], INCLUDING the +Inf tail bucket we add
    // implicitly (a sample ALWAYS fits in +Inf).
    //
    // Calling observe() on an unregistered histogram is a no-op + WARN
    // (same defensive shape as inc_counter).
    void observe_histogram(std::string_view name, double value) noexcept {
        std::lock_guard<std::mutex> g(reg_mu_);
        auto it = histograms_.find(std::string(name));
        if (it == histograms_.end()) {
            warn_unknown_metric_once(std::string(name));
            return;
        }
        auto& h = *it->second;
        std::lock_guard<std::mutex> hg(h.mu);
        h.total_count += 1;
        h.total_sum   += value;
        // The +Inf bucket (last) always catches. The remaining
        // buckets accumulate ascending — if value <= bound, ++count.
        // Because buckets are inclusive on `le`, a sample equal to a
        // bucket boundary lands in that bucket AND every bucket above
        // it. That matches the Prometheus `le` semantics.
        for (std::size_t i = 0; i < h.bucket_bounds.size(); ++i) {
            if (value <= h.bucket_bounds[i]) ++h.bucket_counts[i];
        }
    }

    // ── registration ─────────────────────────────────────────────────────
    //
    // Each register_*_x method installs one metric family under `name`.
    // Re-registering an existing name REPLACES the previous family —
    // useful for tests but generally a bug at boot (the existing
    // counter / histogram state is reset).
    //
    // Returns *this so calls chain.
    MetricsService& register_counter(std::string name,
                                     std::string help) {
        auto fam = std::make_shared<CounterFamily>();
        fam->help = std::move(help);
        std::lock_guard<std::mutex> g(reg_mu_);
        counters_[std::move(name)] = std::move(fam);
        return *this;
    }

    MetricsService& register_gauge(std::string name,
                                   std::string help,
                                   GaugeProvider provider) {
        auto fam = std::make_shared<GaugeFamily>();
        fam->help       = std::move(help);
        fam->provider   = std::move(provider);
        std::lock_guard<std::mutex> g(reg_mu_);
        gauges_[std::move(name)] = std::move(fam);
        return *this;
    }

    MetricsService& register_histogram(std::string name,
                                      std::string help,
                                      std::vector<double> bucket_bounds) {
        // Validate / normalize bucket bounds. Prometheus requires
        // strictly ascending bounds; we sort + uniquify defensively.
        std::sort(bucket_bounds.begin(), bucket_bounds.end());
        bucket_bounds.erase(
            std::unique(bucket_bounds.begin(), bucket_bounds.end()),
            bucket_bounds.end());

        auto fam = std::make_shared<HistogramFamily>();
        fam->help           = std::move(help);
        fam->bucket_bounds  = std::move(bucket_bounds);
        fam->bucket_counts.assign(
            fam->bucket_bounds.size(), std::int64_t{0});
        std::lock_guard<std::mutex> g(reg_mu_);
        histograms_[std::move(name)] = std::move(fam);
        return *this;
    }

    // ── render() — Prometheus text format, version 0.0.4 ────────────────
    //
    // Output shape (one metric family per block, alphabetical by family
    // name for stable diffs in Promtool's regression mode):
    //
    //   # HELP <name> <help text>
    //   # TYPE <name> counter|gauge|histogram
    //   <name>{label="value"} 123
    //   ...
    //
    // Histogram exposition adds:
    //   <name>_bucket{le="<bound>"} <cumulative_count>
    //   <name>_sum <total_sum>
    //   <name>_count <total_count>
    //
    // We always emit a `<name>_bucket{le="+Inf"}` line, with the
    // monotonic total count (`bucket_counts.back()` is always equal
    // to `total_count`, but we follow the convention of writing the
    // explicit +Inf bucket so Promtool's `check metrics` doesn't
    // complain).
    //
    // NO trailing newline is appended; cpp-httplib's set_content()
    // writes its own Content-Length so the body can be framed
    // either way. A trailing `\n` is tolerated by all Prometheus
    // versions we test against.
    std::string render() const {
        std::ostringstream os;

        // Snapshot the family registry (cheap — std::map of shared_ptrs).
        // Render outside the snapshot lock so providers can't re-enter.
        std::vector<std::pair<std::string, std::shared_ptr<CounterFamily>>>
            cs;
        std::vector<std::pair<std::string, std::shared_ptr<GaugeFamily>>>
            gs;
        std::vector<std::pair<std::string, std::shared_ptr<HistogramFamily>>>
            hs;
        {
            std::lock_guard<std::mutex> g(reg_mu_);
            cs.reserve(counters_.size());
            for (const auto& kv : counters_) cs.emplace_back(kv);
            gs.reserve(gauges_.size());
            for (const auto& kv : gauges_)    gs.emplace_back(kv);
            hs.reserve(histograms_.size());
            for (const auto& kv : histograms_) hs.emplace_back(kv);
        }

        for (const auto& [name, fam] : cs) {
            os << "# HELP " << name << ' ' << fam->help << '\n';
            os << "# TYPE " << name << " counter\n";
            std::map<std::string, std::int64_t> snap;
            {
                std::lock_guard<std::mutex> cg(fam->mu);
                snap = fam->values;
            }
            // Alphabetical label iteration for stable output.
            for (const auto& [k, v] : snap) {
                os << name << "{status=\"" << k << "\"} " << v << '\n';
            }
        }

        for (const auto& [name, fam] : gs) {
            std::int64_t value = 0;
            bool provider_ok = true;
            try {
                if (fam->provider) value = fam->provider();
            } catch (const std::exception& e) {
                try {
                    LOG_WARN("metrics: gauge provider threw",
                             {{"gauge", name},
                              {"error", e.what()}});
                } catch (...) {}
                provider_ok = false;
            } catch (...) {
                provider_ok = false;
            }
            os << "# HELP " << name << ' ' << fam->help << '\n';
            os << "# TYPE " << name << " gauge\n";
            if (provider_ok) {
                os << name << ' ' << value << '\n';
            } else {
                // NaN surfaces the broken provider to operators without
                // dropping the metric from the scrape entirely.
                os << name << " NaN\n";
            }
        }

        for (const auto& [name, fam] : hs) {
            // Snapshot the histogram atomically.
            std::vector<double> bounds;
            std::vector<std::int64_t> counts;
            std::int64_t total_count = 0;
            double total_sum = 0.0;
            {
                std::lock_guard<std::mutex> hg(fam->mu);
                bounds      = fam->bucket_bounds;
                counts      = fam->bucket_counts;
                total_count = fam->total_count;
                total_sum   = fam->total_sum;
            }
            os << "# HELP " << name << ' ' << fam->help << '\n';
            os << "# TYPE " << name << " histogram\n";
            // _bucket{le="..."} lines, ascending; explicit +Inf last.
            for (std::size_t i = 0; i < bounds.size(); ++i) {
                os << name << "_bucket{le=\"" << format_double(bounds[i])
                   << "\"} " << counts[i] << '\n';
            }
            os << name << "_bucket{le=\"+Inf\"} " << total_count << '\n';
            // _sum — emit with 6 decimal places (Prometheus default
            // for histogram sums). Avoid scientific notation for
            // normal judge durations; we use `std::fixed` +
            // setprecision for portable output.
            os << name << "_sum " << format_double(total_sum) << '\n';
            os << name << "_count " << total_count << '\n';
        }
        return os.str();
    }

    // ── introspection (test-only) ────────────────────────────────────────
    std::size_t counter_count() const noexcept {
        std::lock_guard<std::mutex> g(reg_mu_);
        return counters_.size();
    }
    std::size_t gauge_count() const noexcept {
        std::lock_guard<std::mutex> g(reg_mu_);
        return gauges_.size();
    }
    std::size_t histogram_count() const noexcept {
        std::lock_guard<std::mutex> g(reg_mu_);
        return histograms_.size();
    }

private:
    // ── family types ─────────────────────────────────────────────────────

    struct CounterFamily {
        std::string                            help;
        std::map<std::string, std::int64_t>    values;     // label -> count
        mutable std::mutex                     mu;
    };

    struct GaugeFamily {
        std::string                            help;
        GaugeProvider                          provider;
        mutable std::mutex                     mu;
    };

    struct HistogramFamily {
        std::string                            help;
        std::vector<double>                    bucket_bounds; // ascending
        std::vector<std::int64_t>              bucket_counts; // same size
        std::int64_t                           total_count = 0;
        double                                 total_sum   = 0.0;
        mutable std::mutex                     mu;
    };

    // ── utilities ────────────────────────────────────────────────────────

    // Prometheus text format uses bare / quoted double strings only on
    // label values. Bucket boundaries serialize as "0.005" / "1" / "+Inf"
    // (the trailing +Inf bucket is a separate explicit line). We
    // serialize all doubles with up to 6 fractional digits (matches
    // Prometheus client_golang default) and strip trailing zeros so
    // "0.005000" collapses to "0.005" — the canonical Prometheus form.
    // A leading sign only appears when v < 0.
    static std::string format_double(double v) {
        std::ostringstream os;
        // For exact integer values, drop the fractional part so
        // gauge `42` doesn't render as `42.000000`.
        if (v == std::floor(v) && std::abs(v) < 1e15) {
            os << static_cast<std::int64_t>(v);
            return os.str();
        }
        os << std::fixed << std::setprecision(6) << v;
        std::string s = os.str();
        // Strip trailing zeros from the fractional part (e.g.
        // "0.005000" → "0.005"); keep at least one fractional digit
        // so "0.0" never collapses to a bare "0", which would be
        // ambiguous against the integer bucket 0.
        if (s.find('.') != std::string::npos) {
            std::size_t last_nonzero = s.find_last_not_of('0');
            if (last_nonzero != std::string::npos
                && s[last_nonzero] != '.') {
                s.erase(last_nonzero + 1);
            }
        }
        return s;
    }

    // Warn once per unknown metric to avoid log spam in tight loops.
    // Not strictly O(1) — we keep a small set of names we've already
    // warned about. The default cap is 32 names, more than enough for
    // boot mis-wirings in practice.
    void warn_unknown_metric_once(const std::string& name) noexcept {
        // Unsynchronized read of warned_ — the worst case is two
        // log lines for the same metric at boot, which is harmless.
        for (const auto& w : warned_) {
            if (w == name) return;
        }
        warned_.push_back(name);
        try {
            LOG_WARN("metrics: inc/observe on unregistered metric",
                     {{"metric", name}});
        } catch (...) {}
    }

    // ── registry state ───────────────────────────────────────────────────

    mutable std::mutex                                  reg_mu_;
    std::map<std::string, std::shared_ptr<CounterFamily>>     counters_;
    std::map<std::string, std::shared_ptr<GaugeFamily>>       gauges_;
    std::map<std::string, std::shared_ptr<HistogramFamily>>   histograms_;
    std::vector<std::string>                            warned_;     // bounded; see warn_unknown_metric_once
};

// ────────────────────────────────────────────────────────────────────────────
//  Route registration.
//
//  GET /api/v1/metrics — public; returns the Prometheus text format
//  (Content-Type: text/plain; version=0.0.4; charset=utf-8 — the canonical
//  exposition content-type). On a MetricsService that hasn't been wired
//  up at all, render() returns an empty body (Promtool accepts an empty
//  scrape with `up==1`).
//
//  Replaces the Phase 1 placeholder (501 SERVICE_UNAVAILABLE) that
//  system_routes.h registered. main.cpp calls register_metric_routes
//  AFTER register_health_routes so the new handler wins.
// ────────────────────────────────────────────────────────────────────────────

inline HttpServer& register_metric_routes(HttpServer& server,
                                          MetricsService& metrics) {
    server.get("/api/v1/metrics",
        [&metrics](const httplib::Request&, httplib::Response& res) {
            try {
                std::string body = metrics.render();
                // The canonical content-type per
                // https://prometheus.io/docs/instrumenting/exposition_formats/#text-based-format
                // is "text/plain; version=0.0.4; charset=utf-8".
                // Older scrapers (pre-2.0) only care about the
                // "text/plain" prefix; modern ones check the version
                // token. Logging bytes-written is skipped to avoid
                // spamming the access log on a 15s scrape cadence.
                res.set_content(body,
                    "text/plain; version=0.0.4; charset=utf-8");
            } catch (const std::exception& e) {
                // render() is best-effort — a misconfigured gauge
                // provider throwing would have been swallowed
                // already, but defense in depth here keeps a /metrics
                // scrape from crashing the HTTP server. We return
                // an empty body (Prometheus' `up` still flips, and
                // Promtool records the scrape as "successful but
                // empty").
                try {
                    LOG_WARN("metrics: render() escaped exception",
                             {{"error", e.what()}});
                } catch (...) {}
                res.status = 200;
                res.set_content("", "text/plain; version=0.0.4; charset=utf-8");
            } catch (...) {
                res.status = 200;
                res.set_content("", "text/plain; version=0.0.4; charset=utf-8");
            }
        });
    return server;
}

} // namespace litecode
