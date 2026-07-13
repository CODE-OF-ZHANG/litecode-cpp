// SPDX-License-Identifier: MIT
// LiteCode-CPP — auth_routes.cpp (v1.2.48)
//
// Per-route TU: includes routes/auth_routes.h exactly once so
// the inline register_auth_routes definition is parsed here.
// The static initializer at the bottom calls the function
// once, forcing the compiler to ODR-use it and emit a real
// symbol in this TU's object file. The actual HTTP server
// call in main.cpp will resolve to this symbol at link time.

#include "auth/refresh_token.h"
#include "config.h"
#include "middleware/rate_limit.h"
#include "routes/auth_routes.h"
#include "server.h"

#include <new>

namespace litecode {

// Static initializer — runs once at program start. The server
// is bound to 127.0.0.1:0 (port 0 = ephemeral, never bound;
// the OS picks an unused port for the brief moment listen() is
// called). server.start() never runs because we don't call it
// here. The route handlers capture deps by reference; they
// only fire on real HTTP requests, which never happen.
static int _force_emit_auth = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    RateLimiter limiter;
    LoginFailureTracker tracker;
    InMemoryRefreshTokenStore store(1000);
    RateLimitConfig rc;
    JwtConfig jc;

    alignas(ConnectionPool) unsigned char pool_buf[sizeof(ConnectionPool)] = {0};
    ConnectionPool& pool =
        *std::launder(reinterpret_cast<ConnectionPool*>(pool_buf));

    using _fn_t = decltype(&register_auth_routes);
    static volatile _fn_t _fn = &register_auth_routes;
    _fn(s, pool, limiter, tracker, store, jc, rc);
    return 0;
}();

} // namespace litecode