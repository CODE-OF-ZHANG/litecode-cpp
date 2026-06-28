// tests/unit/test_health.cpp
//
// Unit + light-integration tests for src/routes/system_routes.h.
//
// Two layers, matching the project's test pattern (see test_connection_pool.cpp):
//
//   1) Pure unit tests (no MySQL / Docker / scheduler required):
//        - HealthService aggregation: ok probe → 200 + status:"ok"
//        - Any probe down → 503 + status:"degraded"
//        - Probe throw is captured (endpoint never crashes)
//        - Top-level mirror fields (db, docker) track probe state
//        - extra.{queue_size, warm_pool} merged into top-level body
//        - register_probe last-write-wins for duplicate names
//        - unregister_probe removes by name; no-op when absent
//        - mark_process_start_time + process_uptime monotonic
//        - X-Request-Id middleware stamp reaches the handler
//        - Content-Type charset utf-8, body not wrapped in `data:`
//        - /api/v1/metrics stub returns 501
//
//   2) Integration tests (require a reachable MySQL — skipped otherwise):
//        - make_db_probe(real_pool) returns ok=true when MySQL is up
//        - End-to-end GET /api/v1/health returns 200 when wired to a
//          real, healthy pool
//
// The integration tests reuse the same DbFixture pattern from
// test_connection_pool.cpp so a CI box without MySQL still passes.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"
#include "logger.h"
#include "server.h"
#include "routes/system_routes.h"
#include "db/connection_pool.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test helpers (mirror test_server.cpp)
// ────────────────────────────────────────────────────────────────────────────

litecode::ServerConfig dev_server() {
    litecode::ServerConfig s;
    s.host = "127.0.0.1";
    s.port = 0;
    s.thread_pool_size = 2;
    return s;
}

litecode::CorsConfig dev_cors() {
    litecode::CorsConfig c;
    c.allowed_origins = "http://localhost:8080";
    c.allow_credentials = false;
    return c;
}

struct ServerHandle {
    litecode::HttpServer*             server = nullptr;
    std::unique_ptr<httplib::Client>  client;
    int                               port = 0;
    ~ServerHandle() { if (server) server->stop(); }
};

ServerHandle start_server(litecode::HttpServer* s) {
    const int port = s->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0);
    EXPECT_TRUE(s->start(/*background=*/true));
    auto c = std::make_unique<httplib::Client>("127.0.0.1", port);
    c->set_connection_timeout(2, 0);
    c->set_read_timeout(5, 0);
    c->set_write_timeout(5, 0);
    c->set_keep_alive(false);
    return ServerHandle{s, std::move(c), port};
}

class StdoutSilencer {
public:
    StdoutSilencer()    { original_ = std::cout.rdbuf(sink_.rdbuf()); }
    ~StdoutSilencer()   { std::cout.rdbuf(original_); }
    std::string captured() const { return sink_.str(); }
private:
    std::stringstream sink_;
    std::streambuf*   original_ = nullptr;
};

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — no MySQL needed
// ────────────────────────────────────────────────────────────────────────────

TEST(HealthService, AllHealthyReturns200AndStatusOk) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    h.register_probe("db",         [] { return litecode::ProbeResult{true,  "ok", nullptr}; });
    h.register_probe("uptime",     litecode::make_uptime_probe());
    h.register_probe("queue_size", litecode::make_queue_size_probe());
    h.register_probe("warm_pool",  litecode::make_warm_pool_probe());
    h.register_probe("docker",     [] { return litecode::ProbeResult{true,  "ok", nullptr}; });

    int status = 0;
    const auto body = h.build_response(&status);

    EXPECT_EQ(status, 200);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_EQ(body["db"],     "ok");
    EXPECT_EQ(body["docker"], "ok");
}

TEST(HealthService, AnyProbeDownReturns503AndStatusDegraded) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    h.register_probe("db",     [] { return litecode::ProbeResult{true,  "ok",     nullptr}; });
    h.register_probe("docker", [] { return litecode::ProbeResult{false, "daemon unreachable", nullptr}; });

    int status = 0;
    const auto body = h.build_response(&status);

    EXPECT_EQ(status, 503);
    EXPECT_EQ(body["status"], "degraded");
    EXPECT_EQ(body["db"],     "ok");
    EXPECT_EQ(body["docker"], "down");
    EXPECT_EQ(body["checks"]["docker"]["ok"],     false);
    EXPECT_EQ(body["checks"]["docker"]["detail"], "daemon unreachable");
}

TEST(HealthService, ThrowingProbeDoesNotCrashEndpoint) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    h.register_probe("db", [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    h.register_probe("explode", []() -> litecode::ProbeResult {
        throw std::runtime_error("boom");
    });

    int status = 0;
    const auto body = h.build_response(&status);

    EXPECT_EQ(status, 503);
    EXPECT_EQ(body["status"], "degraded");
    EXPECT_EQ(body["checks"]["explode"]["ok"], false);
    EXPECT_NE(body["checks"]["explode"]["detail"].get<std::string>().find("boom"),
              std::string::npos);
}

TEST(HealthService, ExtraFieldsMergedIntoTopLevel) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    h.register_probe("db",         [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    h.register_probe("queue_size", [] {
        return litecode::ProbeResult{true, "q", nlohmann::json{{"queue_size", 7}}};
    });
    h.register_probe("warm_pool", [] {
        return litecode::ProbeResult{true, "w", nlohmann::json{{"warm_pool", 3}}};
    });

    int status = 0;
    const auto body = h.build_response(&status);

    EXPECT_EQ(status, 200);
    EXPECT_EQ(body["queue_size"], 7);
    EXPECT_EQ(body["warm_pool"],  3);
}

TEST(HealthService, RegisterProbeDuplicateReplaces) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    h.register_probe("db", [] { return litecode::ProbeResult{true, "first", nullptr}; });
    h.register_probe("db", [] { return litecode::ProbeResult{false, "second", nullptr}; });
    EXPECT_EQ(h.probe_count(), 1u);

    int status = 0;
    const auto body = h.build_response(&status);
    EXPECT_EQ(status, 503);
    EXPECT_EQ(body["checks"]["db"]["detail"], "second");
}

TEST(HealthService, UnregisterProbeRemoves) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    h.register_probe("db",     [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    h.register_probe("docker", [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    EXPECT_EQ(h.probe_count(), 2u);
    h.unregister_probe("db");
    EXPECT_EQ(h.probe_count(), 1u);
    h.unregister_probe("does-not-exist");  // no-op
    EXPECT_EQ(h.probe_count(), 1u);
}

TEST(HealthService, NoProbesReturnsOkWithDefaults) {
    StdoutSilencer silencer;
    litecode::HealthService h;
    int status = 0;
    const auto body = h.build_response(&status);
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body["status"],  "ok");
    EXPECT_EQ(body["db"],      "ok");
    EXPECT_EQ(body["docker"],  "n/a");
    EXPECT_EQ(body["queue_size"], 0);
    EXPECT_EQ(body["warm_pool"],  0);
    EXPECT_EQ(body["checks"].size(), 0u);
}

TEST(HealthService, MakeDbProbeNullPoolReportsDown) {
    StdoutSilencer silencer;
    auto probe = litecode::make_db_probe(nullptr);
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("no connection pool"), std::string::npos);
}

TEST(HealthService, MakeUptimeProbeReportsLiveAndMonotonic) {
    StdoutSilencer silencer;
    litecode::mark_process_start_time();
    auto p1 = litecode::make_uptime_probe()();
    EXPECT_TRUE(p1.ok);
    EXPECT_GE(p1.extra["uptime_seconds"].get<std::int64_t>(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto p2 = litecode::make_uptime_probe()();
    EXPECT_GE(p2.extra["uptime_seconds"].get<std::int64_t>(),
              p1.extra["uptime_seconds"].get<std::int64_t>() + 1);
}

TEST(HealthService, MakeDbProbeFromBadConfigReportsDown) {
    // PoolConfig pointing at a port nothing is listening on. ping()
    // catches everything and returns false instead of throwing — so
    // the probe is "down" rather than crashing the endpoint.
    StdoutSilencer silencer;
    litecode::PoolConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1;             // nothing listens here
    cfg.user = "nobody";
    cfg.password = "nopass";
    cfg.database = "litecode";
    cfg.connect_timeout_ms = 500;
    cfg.acquire_timeout_ms = 500;
    cfg.min_size = 1;
    cfg.max_size = 1;

    try {
        litecode::ConnectionPool pool(cfg);
        auto probe = litecode::make_db_probe(&pool);
        auto r = probe();
        EXPECT_FALSE(r.ok);
        EXPECT_NE(r.detail.find("select 1 failed"), std::string::npos);
    } catch (const std::exception& e) {
        // Some MySQL client builds throw on initial session acquisition;
        // that's fine for the contract — the probe is unavailable.
        GTEST_SKIP() << "pool construction threw: " << e.what();
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  HTTP route tests — exercise the handler through HttpServer
// ────────────────────────────────────────────────────────────────────────────

TEST(HealthRoute, ReturnsExpectedShapeAndStatus) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db",         [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    h.register_probe("uptime",     litecode::make_uptime_probe());
    h.register_probe("queue_size", litecode::make_queue_size_probe());
    h.register_probe("warm_pool",  litecode::make_warm_pool_probe());
    h.register_probe("docker",     [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    auto r = srv.client->Get("/api/v1/health");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_NE(r->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    EXPECT_NE(r->get_header_value("Content-Type").find("charset=utf-8"),
              std::string::npos);
    // Body must NOT be wrapped in `data: {…}` (the spec shows fields at
    // the top level, not under .data).
    EXPECT_EQ(r->body.find("\"data\":"), std::string::npos);

    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["status"],     "ok");
    EXPECT_EQ(body["db"],         "ok");
    EXPECT_EQ(body["docker"],     "ok");
    EXPECT_TRUE(body.contains("queue_size"));
    EXPECT_TRUE(body.contains("warm_pool"));
    EXPECT_TRUE(body.contains("uptime_seconds"));
    EXPECT_TRUE(body.contains("checks"));
    EXPECT_TRUE(body["checks"].is_object());
    EXPECT_EQ(body["checks"]["db"]["ok"], true);
}

TEST(HealthRoute, DownProbeYields503) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db",     [] { return litecode::ProbeResult{true,  "ok", nullptr}; });
    h.register_probe("docker", [] { return litecode::ProbeResult{false, "down", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    auto r = srv.client->Get("/api/v1/health");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 503) << "body=" << r->body;
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["status"], "degraded");
    EXPECT_EQ(body["db"],     "ok");
    EXPECT_EQ(body["docker"], "down");
}

TEST(HealthRoute, RequestIdHeaderIsPresent) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db", [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    auto r = srv.client->Get("/api/v1/health");
    ASSERT_TRUE(r);
    const auto rid = r->get_header_value("X-Request-Id");
    EXPECT_EQ(rid.size(), 36u) << "missing or malformed X-Request-Id";
    EXPECT_EQ(rid[14], '4');
}

TEST(HealthRoute, ClientSuppliedValidRequestIdIsEchoed) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db", [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    httplib::Headers hdrs = { {"X-Request-Id", "health-test-001"} };
    auto r = srv.client->Get("/api/v1/health", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->get_header_value("X-Request-Id"), "health-test-001");
}

TEST(HealthRoute, CorsPreflightShortCircuits) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db", [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    httplib::Headers hdrs = {
        {"Origin", "http://localhost:8080"},
        {"Access-Control-Request-Method", "GET"},
    };
    auto r = srv.client->Options("/api/v1/health", hdrs);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
}

TEST(HealthRoute, MetricsStubReturns501) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    auto r = srv.client->Get("/api/v1/metrics");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 501);
    EXPECT_NE(r->body.find("\"code\":\"SERVICE_UNAVAILABLE\""), std::string::npos);
}

TEST(HealthRoute, ConcurrentRequestsAllReturnValid) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db", [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);

    constexpr int N = 20;
    std::vector<std::future<int>> futs;
    for (int i = 0; i < N; ++i) {
        futs.push_back(std::async(std::launch::async, [&]{
            auto r = srv.client->Get("/api/v1/health");
            if (!r) return 0;
            return r->status;
        }));
    }
    for (auto& f : futs) {
        ASSERT_EQ(f.get(), 200) << "concurrent request failed";
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — real MySQL pool (skipped if unreachable)
// ────────────────────────────────────────────────────────────────────────────

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    if (const char* v = std::getenv(key); v && *v) return v;
    return fallback;
}
std::uint16_t env_u16_or(const char* key, std::uint16_t fallback) {
    if (const char* v = std::getenv(key); v && *v) {
        try { return static_cast<std::uint16_t>(std::stoi(v)); }
        catch (...) { return fallback; }
    }
    return fallback;
}

class DbHealthFixture : public ::testing::Test {
protected:
    void SetUp() override {
        litecode::PoolConfig cfg;
        cfg.host     = env_or("LITECODE_TEST_DB_HOST",     "127.0.0.1");
        cfg.port     = env_u16_or("LITECODE_TEST_DB_PORT", 33060);
        cfg.user     = env_or("LITECODE_TEST_DB_USER",     "root");
        cfg.password = env_or("LITECODE_TEST_DB_PASSWORD", "123456");
        cfg.database = env_or("LITECODE_TEST_DB_NAME",     "litecode");
        cfg.min_size = 1;
        cfg.max_size = 2;
        cfg.connect_timeout_ms = 2000;
        cfg.acquire_timeout_ms = 2000;
        try {
            pool_ = std::make_unique<litecode::ConnectionPool>(cfg);
        } catch (const std::exception& e) {
            pool_.reset();
            GTEST_SKIP() << "MySQL not reachable, skipping: " << e.what();
        }
        if (!pool_->ping()) {
            pool_.reset();
            GTEST_SKIP() << "MySQL ping failed, skipping";
        }
    }
    std::unique_ptr<litecode::ConnectionPool> pool_;
};

} // namespace

TEST_F(DbHealthFixture, MakeDbProbeReportsOkAgainstRealPool) {
    StdoutSilencer silencer;
    auto probe = litecode::make_db_probe(pool_.get());
    auto r = probe();
    EXPECT_TRUE(r.ok) << r.detail;
    EXPECT_NE(r.detail.find("select 1 ok"), std::string::npos);
}

TEST_F(DbHealthFixture, EndToEndHealthReturns200) {
    StdoutSilencer silencer;
    litecode::HttpServer s(dev_server(), dev_cors());
    litecode::HealthService h;
    h.register_probe("db",         litecode::make_db_probe(pool_.get()));
    h.register_probe("uptime",     litecode::make_uptime_probe());
    h.register_probe("queue_size", litecode::make_queue_size_probe());
    h.register_probe("warm_pool",  litecode::make_warm_pool_probe());
    h.register_probe("docker",     [] { return litecode::ProbeResult{true, "ok", nullptr}; });
    litecode::register_health_routes(s, h);

    auto srv = start_server(&s);
    auto r = srv.client->Get("/api/v1/health");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["status"], "ok");
    EXPECT_EQ(body["db"],     "ok");
    EXPECT_EQ(body["checks"]["db"]["ok"], true);
}

} // anonymous namespace
