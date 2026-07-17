// SPDX-License-Identifier: MIT
// LiteCode-CPP — server bootstrap (v1.2.48)
//
// This binary used to be a smoke-only executable that returned 0
// after running bcrypt / jwt / json / zlib / openssl / health
// self-tests. The actual HTTP server existed as inline route-header
// definitions but was never wired into a `main()` that calls
// HttpServer::start(). docker-compose spun the container in a
// restart loop because the smoke test exits cleanly with code 0.
//
// v1.2.48 reshapes this file into the real server bootstrap:
//
//   1. run_config_smoke() + init_logger
//   2. Build AppContext via per-component factory functions
//      (build_db_deps / build_auth_deps / build_judge_deps /
//       build_health_deps). Each factory lives in its own TU so
//      it pulls in only the headers it actually needs — avoids
//      the ODR collision documented in memory
//      `reference-odr-collision-msvc`.
//   3. Construct HttpServer + set_mount_point ("/" → /app/web)
//   4. Call every register_X_routes via forward declarations in
//      src/routes/route_registry.h.
//   5. server.start(true)
//   6. Block on a SIGTERM/SIGINT polling loop (PID 1 signal forwarder)
//
// The smoke checks that used to live here moved to
// src/tools/smoke_check.cpp (target `lit_smoke_check`).

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "AppContext.h"
#include "app_context_deps.h"
#include "config.h"
#include "logger.h"
#include "routes/route_registry.h"
#include "server.h"

namespace {

// SIGTERM / SIGINT handler. Sets a process-wide atomic that the
// main loop polls every 250 ms. cpp-httplib installs its own
// SIGINT handler (calls server_->stop()), so SIGINT triggers a
// clean exit; SIGTERM (what `docker stop` sends) needs our handler
// because cpp-httplib doesn't listen for it by default.
std::atomic<bool> g_shutdown_requested{false};

extern "C" void on_signal(int /*sig*/) {
    g_shutdown_requested.store(true, std::memory_order_release);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = &on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
}

} // namespace

int main() {
    std::cout << "LiteCode-CPP starting..." << std::endl;

    // ── 1. Config + logger ────────────────────────────────────────────────
    if (int rc = litecode::run_config_smoke(); rc != 0) return rc;

    litecode::mark_process_start_time();

    const auto& cfg = litecode::config();
    litecode::init_logger(cfg.logging);

    // ── 2. AppContext — per-component factories (ODR-safe) ───────────────
    // Order matters:
    //   1. db       — independent.
    //   2. auth     — independent (only depends on cfg.login_lockout).
    //   3. judge    — independent (only depends on cfg.judge).
    //   4. health   — needs the *non-null* db.pool + judge.docker_client
    //                  refs so it can register the corresponding probes.
    // Then assemble ctx by moving each dep's fields in.
    auto db    = litecode::build_db_deps(cfg.database);
    auto auth  = litecode::build_auth_deps(cfg.login_lockout);
    auto judge = litecode::build_judge_deps(cfg.judge, db.pool.get());

    // The docker probe is captured both by the HealthService (for
    // /api/v1/health) and by /api/v1/admin/{queue,stats} — build it
    // once here so the lambda lives in main()'s stack and is captured
    // by value into ctx.docker_probe.
    std::function<litecode::ProbeResult()> docker_probe;
    if (judge.docker_client) {
        docker_probe = litecode::docker::make_docker_probe(
            judge.docker_client.get());
    }

    auto health = litecode::build_health_deps(db, judge, docker_probe);

    litecode::AppContext ctx;
    ctx.config         = const_cast<litecode::AppConfig*>(&cfg);
    ctx.db_pool        = std::move(db.pool);
    ctx.limiter        = std::move(auth.limiter);
    ctx.login_tracker  = std::move(auth.login_tracker);
    ctx.refresh_store  = std::move(auth.refresh_store);
    ctx.docker_client  = std::move(judge.docker_client);
    ctx.warm_pool      = std::move(judge.warm_pool);
    ctx.scheduler      = std::move(judge.scheduler);
    ctx.notifier       = std::move(judge.notifier);
    ctx.health         = std::move(health.health);
    ctx.docker_probe   = docker_probe;

    // ── 3. Construct HttpServer + mount static assets ─────────────────────
    litecode::HttpServer server(cfg.server, cfg.cors);

    // cpp-httplib's mount_point: any request that doesn't match a
    // registered route is served from /app/web. Route handlers
    // always win, so /api/v1/* hits the registered handlers and
    // everything else (CSS / JS / *.html) hits the filesystem.
    server.mount("/", "/app/web");

    // ── 4. Register every route set ───────────────────────────────────────
    // Order doesn't matter to the server (matches are prefix-based),
    // but we register user-facing first, then admin — easier to
    // read in `docker logs` boot trace.

    // Health route is independent of DB — register first so it's
    // available even when MySQL is down.
    if (ctx.health) {
        litecode::register_health_routes(server, *ctx.health);
    }

    if (ctx.db_pool) {
        litecode::register_auth_routes(server, *ctx.db_pool,
            *ctx.limiter, *ctx.login_tracker, *ctx.refresh_store,
            cfg.jwt, cfg.rate_limit);
        litecode::register_problem_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit);
        litecode::register_tag_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit);
        // stats_routes.h: register_stats_routes is defined in
        // `namespace litecode` directly (the inner `stats_routes`
        // and `detail` blocks both close before the function).
        litecode::register_stats_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit, cfg.jwt);
        // submission_routes.h: same — defined in `namespace
        // litecode` (both `detail` blocks close first).
        litecode::register_submission_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit, cfg.jwt,
            ctx.scheduler.get(), ctx.notifier.get());

        litecode::admin_user_routes::register_admin_user_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit, cfg.jwt);
        // admin_problem_routes.h: `detail` closes before the
        // function declaration, so it's in `namespace litecode`.
        litecode::register_admin_problem_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit, cfg.jwt);
        // admin_bulk_import_routes.h: same — `bulk_import` and
        // `detail` both close first.
        litecode::register_admin_bulk_import_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit, cfg.jwt);
        litecode::admin_audit_log_routes::register_admin_audit_log_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.rate_limit, cfg.jwt);
        litecode::admin_stats_routes::register_admin_stats_routes(server, *ctx.db_pool,
            cfg.jwt, ctx.scheduler.get(), ctx.warm_pool.get(),
            ctx.docker_probe);
        litecode::admin_queue_routes::register_admin_queue_routes(server, *ctx.db_pool,
            *ctx.limiter, cfg.jwt, cfg.rate_limit,
            ctx.scheduler.get(), ctx.warm_pool.get(), ctx.docker_probe);
    } else {
        std::cerr << "[boot] WARN: skipping register_*_routes because"
                     " db_pool is null — every API will 503" << std::endl;
    }

    // ── 5. Start listen thread ────────────────────────────────────────────
    // v1.2.48: probe bind first with cpp-httplib's bind_to_port()
    // so we get a clear error message if 0.0.0.0:8080 is already
    // taken (TIME_WAIT from previous container, etc.) — then fall
    // through to the background listen thread.
    if (!server.bind_to_port(cfg.server.host, cfg.server.port)) {
        std::cerr << "[boot] FATAL: HttpServer::bind_to_port("
                  << cfg.server.host << ":" << cfg.server.port
                  << ") failed (port already in use?)" << std::endl;
        return 1;
    }
    if (!server.start(/*background=*/true)) {
        std::cerr << "[boot] FATAL: HttpServer::start() returned false"
                  << std::endl;
        return 1;
    }

    LOG_INFO("boot complete");

    // ── 6. Block on shutdown signal ───────────────────────────────────────
    install_signal_handlers();
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    LOG_INFO("shutdown requested — stopping server");
    server.stop();

    if (ctx.scheduler) ctx.scheduler->shutdown();
    if (ctx.warm_pool) ctx.warm_pool->shutdown();
    return 0;
}