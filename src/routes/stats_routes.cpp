// SPDX-License-Identifier: MIT
// LiteCode-CPP — stats_routes.cpp (v1.2.48)

#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/stats_routes.h"
#include "server.h"

#include <new>

namespace litecode {
namespace stats_routes {

static int _force_emit_stats = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    RateLimitConfig rc;
    JwtConfig jc;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_stats_routes);
    [[gnu::used]] static volatile _fn_t _fn = &register_stats_routes;
    _fn(s, pool, limiter, rc, jc);
    return 0;
}();

} // namespace stats_routes
} // namespace litecode