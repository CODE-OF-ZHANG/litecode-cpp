// SPDX-License-Identifier: MIT
// LiteCode-CPP — problem_routes.cpp (v1.2.48)

#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/problem_routes.h"
#include "server.h"

#include <new>

namespace litecode {

static int _force_emit_problem = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    RateLimitConfig rc;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_problem_routes);
    static volatile _fn_t _fn = &register_problem_routes;
    _fn(s, pool, limiter, rc);
    return 0;
}();

} // namespace litecode