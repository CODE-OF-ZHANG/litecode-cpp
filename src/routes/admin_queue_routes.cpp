// SPDX-License-Identifier: MIT
// LiteCode-CPP — admin_queue_routes.cpp (v1.2.48)

#include "config.h"
#include "judge/judge_scheduler.h"
#include "judge/warm_pool.h"
#include "middleware/rate_limit.h"
#include "routes/admin_queue_routes.h"
#include "server.h"

#include <new>

namespace litecode {
namespace admin_queue_routes {

static int _force_emit_aq = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    JwtConfig jc;
    RateLimitConfig rc;
    const judge::JudgeScheduler* sched = nullptr;
    const judge::WarmPool*       wp    = nullptr;
    std::function<ProbeResult()> dp;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_admin_queue_routes);
    static volatile _fn_t _fn = &register_admin_queue_routes;
    _fn(s, pool, limiter, jc, rc, sched, wp, dp);
    return 0;
}();

} // namespace admin_queue_routes
} // namespace litecode