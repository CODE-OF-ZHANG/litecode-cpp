// SPDX-License-Identifier: MIT
// LiteCode-CPP — admin_user_routes.cpp (v1.2.48)

#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/admin_user_routes.h"
#include "server.h"

#include <new>

namespace litecode {
namespace admin_user_routes {

static int _force_emit_au = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    RateLimitConfig rc;
    JwtConfig jc;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_admin_user_routes);
    static volatile _fn_t _fn = &register_admin_user_routes;
    _fn(s, pool, limiter, rc, jc);
    return 0;
}();

} // namespace admin_user_routes
} // namespace litecode