// SPDX-License-Identifier: MIT
// LiteCode-CPP — admin_problem_routes.cpp (v1.2.48)
//
// register_admin_problem_routes is in `namespace litecode`
// directly (the inner `detail` block closes before the function).

#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/admin_problem_routes.h"
#include "server.h"

#include <new>

namespace litecode {

static int _force_emit_ap = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    RateLimitConfig rc;
    JwtConfig jc;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_admin_problem_routes);
    [[gnu::used]] static volatile _fn_t _fn = &register_admin_problem_routes;
    _fn(s, pool, limiter, rc, jc);
    return 0;
}();

} // namespace litecode