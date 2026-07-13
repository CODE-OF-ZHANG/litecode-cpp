// SPDX-License-Identifier: MIT
// LiteCode-CPP — admin_audit_log_routes.cpp (v1.2.48)

#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/admin_audit_log_routes.h"
#include "server.h"

#include <new>

namespace litecode {
namespace admin_audit_log_routes {

static int _force_emit_aal = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    RateLimitConfig rc;
    JwtConfig jc;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    // Take address through `volatile` to defeat the compiler's
    // inlining optimization. Without this, GCC inlines
    // register_admin_audit_log_routes into the lambda body
    // and emits no external symbol, which then fails to link
    // when main.cpp's TU tries to call it.
    using _fn_t = decltype(&register_admin_audit_log_routes);
    static volatile _fn_t _fn = &register_admin_audit_log_routes;
    _fn(s, pool, limiter, rc, jc);
    return 0;
}();

} // namespace admin_audit_log_routes
} // namespace litecode