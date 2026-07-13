// SPDX-License-Identifier: MIT
// LiteCode-CPP — AppContext DB factory (v1.2.48)
//
// Defines litecode::build_db_deps(). Only depends on
// connection_pool.h + config.h, so this TU stays ODR-clean.

#include "app_context_deps.h"
#include "config.h"
#include "db/connection_pool.h"

#include <iostream>

namespace litecode {

DbDeps build_db_deps(const DatabaseConfig& cfg) {
    DbDeps out;
    try {
        // PoolConfig is in src/db/connection_pool.h, the same header
        // that defines ConnectionPool itself, so no extra includes
        // needed beyond what we already pulled in.
        PoolConfig pc;
        pc.host               = cfg.host;
        pc.port               = cfg.port;
        pc.user               = cfg.user;
        pc.password           = cfg.password;
        pc.database           = cfg.database;
        pc.socket_path        = cfg.socket_path;
        pc.min_size           = cfg.pool_min_size;
        pc.max_size           = cfg.pool_max_size;
        pc.acquire_timeout_ms = 5'000;
        pc.connect_timeout_ms = 10'000;
        pc.max_idle_time_ms   = 60'000;
        out.pool = std::make_unique<ConnectionPool>(pc);
    } catch (const std::exception& e) {
        std::cerr << "[boot] WARN: ConnectionPool init failed: "
                  << e.what() << " — routes will return 503 until MySQL"
                     " comes up" << std::endl;
    }
    return out;
}

} // namespace litecode