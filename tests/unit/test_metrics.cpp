// tests/unit/test_metrics.cpp
//
// Unit tests for src/routes/metrics.h (Phase 9 ★ v1.2.68).
//
// All tests are pure C++ — no MySQL / Docker / httplib needed.
// We exercise MetricsService's text-format exposition + thread
// safety against the SPEC §16.4 metric families. Two macro layers:
//
//   1) Pure unit tests (no MySQL / Docker):
//        - register_* family APIs register correctly
//        - inc_counter() increments the labelled counter
//        - observe_histogram() partitions samples across buckets + sum/count
//        - gauge provider is sampled at render() time, not eagerly
//        - gauge provider exception renders NaN (does not crash scrape)
//        - render() output is Prometheus text-format compliant
//        - render() output begins with # HELP / # TYPE for each family
//        - histogram emits +Inf bucket + _sum + _count exactly once each
//        - alphabetically stable iteration (regression for Promtool)
//        - concurrent inc_counter / observe_histogram from N threads
//          produces the right totals (no torn updates)
//        - text/plain version=0.0.4 Content-Type on /api/v1/metrics
//        - /api/v1/metrics returns 200 with valid Prometheus body
//        - /api/v1/metrics renders an empty body (no exception) when
//          MetricsService is fresh — operators can scrape a broken
//          config without 5xx storms
//
//   2) Phase 9 ★ registry integration (route smoke):
//        - register_metric_routes attaches to /api/v1/metrics and
//          renders a real Prometheus response via httplib in-process

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "config.h"
#include "logger.h"
#include "routes/metrics.h"
#include "server.h"

namespace {

// Helper: count non-overlapping substring occurrences (independent of
// std::string::find's implementation for the test's edge cases).
std::size_t count_substring(const std::string& haystack,
                            const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Look for an exact-match line like
//   "litecode_judge_duration_seconds_bucket{le=\"1\"} N\n"
// inside the rendered text. Returns the right-hand N or -1 if no
// match. Used to pin bucket counts without depending on text
// ordering between sibling lines.
//
// Why plain string-search rather than std::regex_search: MSVC's
// std::regex implementation is notoriously buggy (slow + incorrect
// for several patterns), and the test would otherwise need a PCH
// dance on Windows. Bracket-balanced find-and-parse is portable
// across all the toolchains we target.
std::int64_t bucket_count(const std::string& text,
                          const std::string& metric,
                          const std::string& le) {
    // Anchor: "<metric>_bucket{le=\"<le>\"} "
    const std::string anchor = metric + "_bucket{le=\"" + le + "\"} ";
    std::size_t pos = text.find(anchor);
    if (pos == std::string::npos) return -1;
    std::size_t value_start = pos + anchor.size();
    // Skip past leading whitespace just in case (the format spec
    // doesn't include any, but be lenient).
    while (value_start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[value_start]))) {
        ++value_start;
    }
    // Walk digits (and an optional leading '-') until a non-digit /
    // newline / EOF.
    if (value_start >= text.size()) return -1;
    std::size_t i = value_start;
    if (text[i] == '-') ++i;
    const std::size_t digits_start = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i == digits_start) return -1;
    try {
        return std::stoll(text.substr(digits_start, i - digits_start));
    } catch (...) {
        return -1;
    }
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests
// ────────────────────────────────────────────────────────────────────────────

TEST(MetricsService, EmptyServiceRendersEmptyText) {
    litecode::MetricsService m;
    EXPECT_EQ(m.render(), "");
    EXPECT_EQ(m.counter_count(), 0u);
    EXPECT_EQ(m.gauge_count(), 0u);
    EXPECT_EQ(m.histogram_count(), 0u);
}

TEST(MetricsService, CounterIncByLabelValue) {
    litecode::MetricsService m;
    m.register_counter("litecode_submissions_total",
                       "Total judged submissions by final status.");
    EXPECT_EQ(m.counter_count(), 1u);

    m.inc_counter("litecode_submissions_total", "ac");
    m.inc_counter("litecode_submissions_total", "wa");
    m.inc_counter("litecode_submissions_total", "wa");
    m.inc_counter("litecode_submissions_total", "se");

    const std::string out = m.render();
    EXPECT_NE(out.find("# HELP litecode_submissions_total"), std::string::npos);
    EXPECT_NE(out.find("# TYPE litecode_submissions_total counter"),
              std::string::npos);
    // Labels sorted alphabetically: ac, se, wa (Promtool regression).
    const std::size_t pos_ac = out.find("status=\"ac\"");
    const std::size_t pos_se = out.find("status=\"se\"");
    const std::size_t pos_wa = out.find("status=\"wa\"");
    EXPECT_LT(pos_ac, pos_se);
    EXPECT_LT(pos_se, pos_wa);
    EXPECT_NE(out.find("litecode_submissions_total{status=\"ac\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("litecode_submissions_total{status=\"wa\"} 2"),
              std::string::npos);
    EXPECT_NE(out.find("litecode_submissions_total{status=\"se\"} 1"),
              std::string::npos);
}

TEST(MetricsService, IncOnUnregisteredCounterIsNoop) {
    // Operationally important: a worker thread that races past the
    // boot-time register_* calls (e.g. a startup with the metrics
    // registry partially built) must not crash. The MetricsService
    // logs a WARN and swallows the inc.
    litecode::MetricsService m;
    m.inc_counter("not_registered", "ac");
    // Render still works.
    EXPECT_EQ(m.render(), "");
}

TEST(MetricsService, HistogramObserveBucketsCorrectly) {
    // The 11-bucket set used in src/app_context_metrics.cpp.
    litecode::MetricsService m;
    m.register_histogram("litecode_judge_duration_seconds",
                         "end-to-end judge task duration (s)",
                         {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5,
                          1.0, 2.5, 5.0, 10.0});

    // Observe six samples — verify each bucket count is exactly the
    // number of samples <= its upper bound.
    m.observe_histogram("litecode_judge_duration_seconds", 0.003);  // ≤ 0.005, 0.01, ...
    m.observe_histogram("litecode_judge_duration_seconds", 0.020);  // ≤ 0.025, 0.05, ...
    m.observe_histogram("litecode_judge_duration_seconds", 0.200);  // ≤ 0.25, 0.5, ...
    m.observe_histogram("litecode_judge_duration_seconds", 1.500);  // ≤ 2.5, 5, 10
    m.observe_histogram("litecode_judge_duration_seconds", 6.000);  // ≤ 10 only
    m.observe_histogram("litecode_judge_duration_seconds", 30.000); // > 10 → only +Inf

    const std::string out = m.render();
    EXPECT_NE(out.find("# TYPE litecode_judge_duration_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("litecode_judge_duration_seconds_count 6"),
              std::string::npos);
    // +Inf bucket == total_count.
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "+Inf"),
              6);
    // Bucket 0.005 catches the first sample only.
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "0.005"),
              1);
    // Bucket 0.025 catches samples 0.003 + 0.020.
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "0.025"),
              2);
    // Bucket 0.5 catches 0.003 + 0.020 + 0.200.
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "0.5"),
              3);
    // Bucket 2.5 catches 4 samples (skipping 6.0).
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "2.5"),
              4);
    // Bucket 10 catches 5 samples (skipping 30.0).
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "10"),
              5);
    // 30.0 is in the +Inf bucket only — bucket 10 should NOT count it.
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "10"),
              5);
    // _sum is rendered with at most 6 fractional digits.
    // 0.003 + 0.020 + 0.200 + 1.500 + 6.000 + 30.000 = 37.723.
    EXPECT_NE(out.find("litecode_judge_duration_seconds_sum 37.723"),
              std::string::npos);
}

TEST(MetricsService, HistogramObserveOnUnregisteredIsNoop) {
    litecode::MetricsService m;
    m.observe_histogram("not_registered", 1.0);
    EXPECT_EQ(m.render(), "");
}

TEST(MetricsService, HistogramRejectsUnsortedBuckets) {
    // The internal sort + unique step normalizes bounds. Even
    // out-of-order input should produce the same canonical output.
    litecode::MetricsService m;
    m.register_histogram("latency_seconds", "test",
                         {5.0, 0.1, 1.0, 0.5});   // unsorted + duplicates
    m.observe_histogram("latency_seconds", 0.3);
    const std::string out = m.render();
    // bounds should be sorted ascending after normalization.
    EXPECT_NE(out.find("latency_seconds_bucket{le=\"0.1\"} 0"),
              std::string::npos);
    EXPECT_NE(out.find("latency_seconds_bucket{le=\"0.5\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("latency_seconds_bucket{le=\"1\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("latency_seconds_bucket{le=\"5\"} 1"),
              std::string::npos);
}

TEST(MetricsService, GaugeProviderSampledAtRenderTime) {
    // The provider is invoked at render() time, not at register time
    // (the whole point of the provider model — the system is queried
    // live, so we don't accumulate stale gauge values).
    std::atomic<int> reads{0};
    std::atomic<int> value{0};
    {
        litecode::MetricsService m;
        m.register_gauge(
            "litecode_db_pool_active",
            "MySQL sessions checked out of the pool.",
            [&]() -> std::int64_t {
                reads.fetch_add(1, std::memory_order_acq_rel);
                return value.load(std::memory_order_acquire);
            });
        EXPECT_EQ(m.gauge_count(), 1u);

        // First render: provider invoked once.
        value.store(7);
        const std::string out1 = m.render();
        EXPECT_EQ(reads.load(), 1);
        EXPECT_NE(out1.find("litecode_db_pool_active 7"),
                  std::string::npos);

        // Update the simulated underlying value, render again —
        // provider picked up the new value (live sample).
        value.store(13);
        const std::string out2 = m.render();
        EXPECT_EQ(reads.load(), 2);
        EXPECT_NE(out2.find("litecode_db_pool_active 13"),
                  std::string::npos);
    }
}

TEST(MetricsService, GaugeProviderExceptionRendersNaN) {
    litecode::MetricsService m;
    m.register_gauge("litecode_judge_warm_pool_size", "warm pool size",
                     []() -> std::int64_t {
                         throw std::runtime_error("docker socket closed");
                     });
    const std::string out = m.render();
    // The metric line is still emitted, with NaN — Promtool accepts
    // NaN in gauge lines (renders as the literal `NaN`).
    EXPECT_NE(out.find("# TYPE litecode_judge_warm_pool_size gauge"),
              std::string::npos);
    EXPECT_NE(out.find("litecode_judge_warm_pool_size NaN"),
              std::string::npos);
}

TEST(MetricsService, CounterLabelsArePrometheusTextCompliant) {
    // Quick conformance pass against the well-formed Prom text spec:
    //   - one # HELP / # TYPE pair per metric family
    //   - one labelled metric line per observed (label, value) pair
    litecode::MetricsService m;
    m.register_counter("litecode_submissions_total", "help text");
    m.inc_counter("litecode_submissions_total", "ac");
    m.inc_counter("litecode_submissions_total", "ac");
    m.inc_counter("litecode_submissions_total", "ce");

    const std::string out = m.render();
    EXPECT_EQ(count_substring(out, "# HELP litecode_submissions_total"), 1u);
    EXPECT_EQ(count_substring(out, "# TYPE litecode_submissions_total counter"),
              1u);
    // No double-namespace prefix slips in.
    EXPECT_EQ(out.find("litecode_litecode_"), std::string::npos);
}

TEST(MetricsService, HistogramEmitsExactlyOneInfBucket) {
    // Promtool / the spec REQUIRES a single +Inf bucket at the end.
    // Multiple +Inf buckets would be a bug; we explicitly assert one.
    litecode::MetricsService m;
    m.register_histogram("litecode_judge_duration_seconds", "test",
                         {0.1, 1.0, 10.0});
    m.observe_histogram("litecode_judge_duration_seconds", 0.05);
    const std::string out = m.render();
    EXPECT_EQ(count_substring(out, "_bucket{le=\"+Inf\"}"), 1u);
}

TEST(MetricsService, ConcurrentIncCounterIsThreadSafe) {
    // 8 worker threads × 1000 incs each → metric should reach exactly
    // 8000 increments per label value. Catches missing / wrong
    // mutex scope in inc_counter().
    litecode::MetricsService m;
    m.register_counter("litecode_submissions_total", "test");

    constexpr int kThreads = 8;
    constexpr int kIncsPerThread = 1000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&]{
            for (int i = 0; i < kIncsPerThread; ++i) {
                m.inc_counter("litecode_submissions_total", "ac");
            }
        });
    }
    for (auto& t : ts) t.join();

    const std::string out = m.render();
    const std::size_t pos = out.find("litecode_submissions_total{status=\"ac\"} ");
    ASSERT_NE(pos, std::string::npos);
    const std::size_t val_pos = pos + std::string(
        "litecode_submissions_total{status=\"ac\"} ").size();
    const std::int64_t total =
        std::stoll(out.substr(val_pos,
                              out.find('\n', val_pos) - val_pos));
    EXPECT_EQ(total, kThreads * kIncsPerThread);
}

TEST(MetricsService, ConcurrentObserveHistogramIsThreadSafe) {
    // 4 worker threads × 250 obs each of value=0.05 → the bucket for
    // 0.1 must reach 1000. The _count and _sum must each reach the
    // correct totals.
    litecode::MetricsService m;
    m.register_histogram("litecode_judge_duration_seconds", "test",
                         {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5,
                          1.0, 2.5, 5.0, 10.0});

    constexpr int kThreads = 4;
    constexpr int kObsPerThread = 250;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&]{
            for (int i = 0; i < kObsPerThread; ++i) {
                m.observe_histogram("litecode_judge_duration_seconds", 0.05);
            }
        });
    }
    for (auto& t : ts) t.join();

    const std::string out = m.render();
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "0.05"),
              kThreads * kObsPerThread);   // exact-equal goes here
    EXPECT_EQ(bucket_count(out, "litecode_judge_duration_seconds", "0.1"),
              kThreads * kObsPerThread);   // and into the wider bucket
    EXPECT_NE(out.find("litecode_judge_duration_seconds_count 1000"),
              std::string::npos);
    // 1000 * 0.05 = 50 (no fractional digits in output).
    EXPECT_NE(out.find("litecode_judge_duration_seconds_sum 50"),
              std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  Route integration (in-process server)
// ────────────────────────────────────────────────────────────────────────────

TEST(MetricsRoute, EndpointReturnsTextPlainVersion) {
    litecode::ServerConfig sc{};
    sc.host = "127.0.0.1";
    sc.port = 0;
    litecode::CorsConfig cc{};
    litecode::HttpServer server(sc, cc);

    litecode::MetricsService m;
    m.register_counter("litecode_submissions_total", "test")
     .register_histogram("litecode_judge_duration_seconds", "test",
                         {0.1, 1.0, 10.0})
     .register_gauge("litecode_judge_queue_size", "test",
                     []() -> std::int64_t { return 3; });
    m.inc_counter("litecode_submissions_total", "ac");

    litecode::register_metric_routes(server, m);
    const int port = server.bind_any_port();
    ASSERT_GT(port, 0);
    server.start(/*background=*/true);

    httplib::Client c("127.0.0.1", port);
    auto r = c.Get("/api/v1/metrics");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);
    // Prometheus exposition content-type — version token is important
    // for modern scrapers.
    EXPECT_NE(std::string(r->get_header_value("Content-Type"))
                  .find("text/plain; version=0.0.4"),
              std::string::npos);
    // The body is non-empty and contains the SPEC §16.4 metric
    // families we registered.
    EXPECT_NE(r->body.find("litecode_submissions_total"), std::string::npos);
    EXPECT_NE(r->body.find("litecode_judge_duration_seconds"),
              std::string::npos);
    EXPECT_NE(r->body.find("litecode_judge_queue_size"), std::string::npos);

    server.stop();
}

TEST(MetricsRoute, EmptyMetricsServiceStillReturns200) {
    // A fresh MetricsService has zero families — render() returns an
    // empty string. Operators should still see HTTP 200 (Promtool's
    // `up` stays 1) rather than a 404/500 storm when /api/v1/metrics
    // is wired before any register_*() call completes.
    litecode::ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    litecode::CorsConfig   cc{};
    litecode::HttpServer server(sc, cc);

    litecode::MetricsService m;
    litecode::register_metric_routes(server, m);
    const int port = server.bind_any_port();
    ASSERT_GT(port, 0);
    server.start(/*background=*/true);

    httplib::Client c("127.0.0.1", port);
    auto r = c.Get("/api/v1/metrics");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);
    EXPECT_TRUE(r->body.empty());

    server.stop();
}
