// SPDX-License-Identifier: MIT
// LiteCode-CPP — submission_routes.cpp (v1.2.48)
//
// register_submission_routes is defined in `namespace litecode`
// directly (both inner `detail` blocks close before the function
// declaration). Force-emit by calling through a volatile function
// pointer.

#include "config.h"
#include "judge/judge_notifier.h"
#include "judge/judge_scheduler.h"
#include "middleware/rate_limit.h"
#include "routes/submission_routes.h"
#include "server.h"

#include <new>

namespace litecode {

static int _force_emit_submission = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    RateLimitConfig rc;
    JwtConfig jc;
    judge::JudgeScheduler* sched = nullptr;
    judge::JudgeNotifier*  notif = nullptr;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_submission_routes);
    [[gnu::used]] static volatile _fn_t _fn = &register_submission_routes;
    _fn(s, pool, limiter, rc, jc, sched, notif);
    return 0;
}();

} // namespace litecode