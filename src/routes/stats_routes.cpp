// SPDX-License-Identifier: MIT
// LiteCode-CPP — stats_routes.cpp (v1.2.48)
//
// Per-route TU: includes routes/stats_routes.h exactly once so
// the inline register_stats_routes definition is parsed here.
// Note: register_stats_routes is defined in `namespace litecode`
// directly (the route header closes its inner `stats_routes` and
// `detail` namespaces before declaring the function), so we put
// our force-emit lambda at the litecode:: level too.

#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/stats_routes.h"
#include "server.h"

#include <new>

namespace litecode {

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

} // namespace litecode