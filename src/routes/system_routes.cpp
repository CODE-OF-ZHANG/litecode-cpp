// SPDX-License-Identifier: MIT
// LiteCode-CPP — system_routes.cpp (v1.2.48)

#include "routes/system_routes.h"
#include "server.h"

#include <new>

namespace litecode {

static int _force_emit_health = []() -> int {
    ServerConfig sc{};  sc.host = "127.0.0.1"; sc.port = 0;
    CorsConfig   cc{};
    HttpServer s(sc, cc);

    alignas(HealthService) unsigned char hs_buf[sizeof(HealthService)] = {0};
    HealthService& health =
        *std::launder(reinterpret_cast<HealthService*>(hs_buf));

    using _fn_t = decltype(&register_health_routes);
    static volatile _fn_t _fn = &register_health_routes;
    _fn(s, health);
    return 0;
}();

} // namespace litecode