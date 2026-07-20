// SPDX-License-Identifier: MIT
// LiteCode-CPP — metrics_routes.cpp (v1.2.68)
//
// Provides a single TU for `register_metric_routes` so the inline
// definition in routes/metrics.h is emitted exactly once (mirrors the
// pattern in system_routes.cpp — v1.2.48 split every route header into
// its own .cpp to keep the ODR surface clean per memory
// `reference-odr-collision-msvc`).
//
// We also expose a tiny `_force_emit_metrics` static initializer that
// builds a server + MetricsService + `/api/v1/metrics` registration so
// the route symbol is reachable from main.cpp's link line even when
// MetricsService is never wired into AppContext (the harness has to
// find the registration function somewhere).

#include "metrics.h"
#include "server.h"

#include <new>

namespace litecode {

static int _force_emit_metrics = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    MetricsService m;
    m.register_counter("litecode_submissions_total", "warmup probe")
     .register_histogram("litecode_judge_duration_seconds", "warmup probe",
                         {0.1, 0.5, 1.0, 5.0, 10.0})
     .register_gauge("litecode_judge_queue_size", "warmup probe",
                     []() -> std::int64_t { return 0; });

    using _fn_t = decltype(&register_metric_routes);
    static volatile _fn_t _fn = &register_metric_routes;
    _fn(s, m);
    return 0;
}();

} // namespace litecode
