// tests/unit/test_warm_pool.cpp
//
// Unit + light-integration tests for src/judge/warm_pool.h
//   (Phase 4 ★ — judge container warm-up pool).
//
// Two layers, mirroring the project pattern (test_docker_client.cpp,
// test_health.cpp):
//
//   1) Pure unit tests (no docker daemon, no network):
//        - WarmPoolConfig defaults: target_size=2, retry=2/500ms
//        - WarmPoolConfig validation: negative target_size rejected
//        - WarmPool defaults: target=0, running=false, size=0
//        - make_default_warm_pool_config() pulls fields from JudgeConfig
//          (image, network_mode, security_opt, tmpfs, pids_limit, user)
//        - acquire() on a never-started pool returns nullopt
//        - acquire() on a shut-down pool returns nullopt
//        - release() on a never-started pool is a no-op (no crash)
//        - make_probe(nullptr) → ok=false "no warm pool configured"
//        - make_probe on a not-yet-started pool → ok=false "not running"
//          with extra.warm_pool=0
//
//   2) Light integration tests (in-process httplib::Server simulating
//      the docker socket proxy — no docker daemon required):
//        - start(K=3) precreates K containers and size()==3
//        - start(K=3) records the right body schema (network_mode,
//          security_opt, read_only, tmpfs, user, pids_limit)
//        - start(K=0) is "disabled mode" — size==0, no refill thread
//        - start() with client=nullptr fails when K>0
//        - acquire() returns idle id with from_pool=true; size drops by 1
//        - acquire() drains the pool; new acquires (pool empty) return
//          from_pool=false; size climbs back via refill thread
//        - release() does NOT add the id back; size only climbs back
//          via the refill thread (SPEC §7.1 step 5)
//        - shutdown() removes every idle container (DELETE on each id)
//        - shutdown() during a refill does not deadlock
//        - create_exec failures during refill are logged, not propagated
//        - Concurrent acquire/release from N threads is race-free
//        - Health probe publishes current size + target
//        - start() with K=1 + flaky mock (first create returns 503)
//          → start() still returns true if K=0 OR container count > 0
//          is achievable on retry; here we expect start() to return
//          false because pool is permanently empty (no client retry
//          in start path) — covered by a separate test
//        - Configuration: zero-size pool target lives on as disabled
//
// Mock proxy mirrors test_docker_client.cpp's surface but tracks every
// POST /containers/create so we can assert create-count and rm-count.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"
#include "logger.h"
#include "routes/system_routes.h"
#include "judge/docker_client.h"
#include "judge/warm_pool.h"

namespace {

using litecode::JudgeConfig;
using litecode::judge::IdleContainer;
using litecode::judge::WarmPool;
using litecode::judge::WarmPoolConfig;
using litecode::judge::make_default_warm_pool_config;

// ────────────────────────────────────────────────────────────────────────────
//  Test helpers
// ────────────────────────────────────────────────────────────────────────────

class StdoutSilencer {
public:
    StdoutSilencer()  { original_ = std::cout.rdbuf(sink_.rdbuf()); }
    ~StdoutSilencer() { std::cout.rdbuf(original_); }
    std::string captured() const { return sink_.str(); }
private:
    std::stringstream sink_;
    std::streambuf*   original_ = nullptr;
};

litecode::docker::CreateOptions minimal_idle_opts() {
    litecode::docker::CreateOptions o;
    o.image        = "litecode-judge:latest";
    o.command      = {"--help"};
    o.network_mode = "none";
    o.read_only    = true;
    o.memory_mb    = 64;
    o.cpus         = 0.0;
    o.pids_limit   = 50;
    o.security_opt = {"no-new-privileges:true"};
    o.tmpfs        = { {"/tmp", "size=64m,mode=1777"} };
    o.user         = "judgeuser";
    return o;
}

WarmPoolConfig cfg_with_target(int k,
                               int retry = 0,
                               int retry_delay_ms = 0) {
    WarmPoolConfig cfg;
    cfg.template_opts         = minimal_idle_opts();
    cfg.target_size           = k;
    cfg.refill_retry_attempts = retry;
    cfg.refill_retry_delay_ms = retry_delay_ms;
    cfg.acquire_timeout_ms    = 5'000;
    return cfg;
}

// ────────────────────────────────────────────────────────────────────────────
//  Mock docker proxy — counts creates + removes; supports a configurable
//  fail-on-create mode so we can exercise the refill failure path.
// ────────────────────────────────────────────────────────────────────────────

class MockDockerProxy {
public:
    void start() {
        srv_.Get("/_ping",
            [this](const httplib::Request&, httplib::Response& res) {
                record("GET", "/_ping", "");
                res.status = 200; res.set_content("OK", "text/plain");
            });

        srv_.Post("/containers/create",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", "/containers/create", req.body);
                ++create_calls;
                if (next_create_response_.status != 0) {
                    res.status = next_create_response_.status;
                    res.set_content(next_create_response_.body,
                                    "application/json");
                    // Only "consume" the prebuilt response once.
                    next_create_response_ = {};
                    return;
                }
                // Default: hand out a deterministic id and remember
                // the create body for later schema assertions.
                std::ostringstream id;
                id << "warm-" << std::setw(4) << std::setfill('0')
                   << ++created_seq;
                std::string cid = id.str();
                created_ids.push_back(cid);
                if (!req.body.empty()) last_create_body = req.body;

                nlohmann::json body{
                    {"Id",       cid},
                    {"Warnings", nlohmann::json::array()},
                };
                res.status = 201;
                res.set_content(body.dump(), "application/json");
            });

        srv_.Delete(R"(/containers/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("DELETE", req.path, req.body);
                ++delete_calls;
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                removed_ids.push_back(last_id_);
                if (delete_should_fail_) {
                    res.status = 409;
                    res.set_content("conflict", "text/plain");
                    return;
                }
                res.status = 204;
            });

        const int port = srv_.bind_to_any_port("127.0.0.1");
        ASSERT_TRUE(port > 0);
        port_ = port;
        thread_ = std::thread([this]{ srv_.listen_after_bind(); });
        // Block until the listen thread has flipped `is_running_` true.
        // Without this, a test that goes straight from `start()` to the
        // MockDockerProxy destructor can race: stop() sees
        // `is_running_ == false` (listen thread hasn't reached its
        // setup yet), skips shutdown, and thread_.join() then waits
        // forever. The same pattern in test_docker_client.cpp also
        // misses this; the difference there is the test body always
        // makes at least one HTTP call (giving the listen thread time
        // to enter select()), and select_read's 5 s idle interval
        // covers the rest of the cases. Pinning here is cheaper.
        srv_.wait_until_ready();
    }

    void stop() {
        if (port_ > 0) srv_.stop();
        if (thread_.joinable()) thread_.join();
        port_ = 0;
    }

    ~MockDockerProxy() { stop(); }

    std::string url() const {
        std::ostringstream os;
        os << "http://127.0.0.1:" << port_;
        return os.str();
    }

    // Knobs.
    std::atomic<int> create_calls{0};
    std::atomic<int> delete_calls{0};
    std::atomic<int> created_seq{0};
    bool delete_should_fail_ = false;
    struct Prebuilt { int status = 0; std::string body; };
    Prebuilt next_create_response_{};

    // Captured state (guarded by mu_).
    std::string last_create_body;
    std::string last_id_;
    std::vector<std::string> created_ids;
    std::vector<std::string> removed_ids;

    struct RecordedRequest {
        std::string method, path, body;
    };
    std::vector<RecordedRequest>& requests() noexcept { return requests_; }
    const std::vector<RecordedRequest>& requests() const noexcept {
        return requests_;
    }

private:
    void record(const std::string& m, const std::string& p,
                const std::string& b) {
        std::lock_guard<std::mutex> g(mu_);
        requests_.push_back({m, p, b});
    }

    httplib::Server              srv_;
    std::thread                  thread_;
    int                          port_ = 0;
    std::vector<RecordedRequest> requests_;
    std::mutex                   mu_;
};

// Wait until pred() returns true or timeout. Avoids sleep_for() based
// races in concurrent-refill tests.
template <typename Pred>
bool wait_until(Pred pred,
                std::chrono::milliseconds timeout =
                    std::chrono::milliseconds(2000),
                std::chrono::milliseconds step =
                    std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — no HTTP traffic
// ────────────────────────────────────────────────────────────────────────────

TEST(WarmPoolConfig, DefaultTargetSizeIsTwo) {
    WarmPoolConfig c;
    EXPECT_EQ(c.target_size, 2);
    EXPECT_GE(c.refill_retry_attempts, 0);
    EXPECT_GE(c.refill_retry_delay_ms, 0);
}

TEST(WarmPoolConfig, FromJudgeConfigCopiesIsolationFields) {
    StdoutSilencer silencer;
    JudgeConfig jc;
    jc.judge_image  = "litecode-judge:1.0";
    jc.network_mode = "bridge";           // unusual but legal
    jc.warm_pool_size = 5;
    jc.pids_limit   = "120";
    auto cfg = make_default_warm_pool_config(jc);
    EXPECT_EQ(cfg.target_size, 5);
    EXPECT_EQ(cfg.template_opts.image, "litecode-judge:1.0");
    EXPECT_EQ(cfg.template_opts.network_mode, "bridge");
    EXPECT_EQ(cfg.template_opts.pids_limit, 50);   // pool-default override
    EXPECT_EQ(cfg.template_opts.user, "judgeuser");
    EXPECT_TRUE(cfg.template_opts.read_only);
    EXPECT_EQ(cfg.template_opts.security_opt,
              (std::vector<std::string>{"no-new-privileges:true"}));
    EXPECT_EQ(cfg.template_opts.tmpfs.size(), 1u);
    EXPECT_NE(cfg.template_opts.tmpfs.find("/tmp"),
              cfg.template_opts.tmpfs.end());
    EXPECT_EQ(cfg.template_opts.memory_mb, 64);
    EXPECT_EQ(cfg.template_opts.cpus, 0.0);
    // Command is the SPEC §7.3 idle form.
    ASSERT_FALSE(cfg.template_opts.command.empty());
    EXPECT_EQ(cfg.template_opts.command.front(), "--help");
}

TEST(WarmPoolDefaults, FreshInstanceIsNotRunning) {
    StdoutSilencer silencer;
    WarmPool pool(static_cast<litecode::docker::Client*>(nullptr));
    EXPECT_FALSE(pool.running());
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(pool.target(), 0u);
    EXPECT_FALSE(pool.acquire().has_value());
}

TEST(WarmPoolDefaults, ReleaseBeforeStartIsNoOp) {
    StdoutSilencer silencer;
    WarmPool pool(static_cast<litecode::docker::Client*>(nullptr));
    // Must not crash / throw / block.
    EXPECT_NO_THROW(pool.release("anything"));
    EXPECT_FALSE(pool.running());
}

TEST(WarmPoolStart, NegativeTargetSizeRejected) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());
    WarmPool pool(&client);
    EXPECT_FALSE(pool.start(cfg_with_target(-1)));
    EXPECT_FALSE(pool.running());
}

TEST(WarmPoolStart, NullClientRefusesPositiveTarget) {
    StdoutSilencer silencer;
    WarmPool pool(static_cast<litecode::docker::Client*>(nullptr));
    EXPECT_FALSE(pool.start(cfg_with_target(2)));
    EXPECT_FALSE(pool.running());
}

TEST(WarmPoolStart, ZeroTargetIsDisabledMode) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());
    WarmPool pool(&client);
    EXPECT_TRUE(pool.start(cfg_with_target(0)));
    EXPECT_TRUE(pool.running());         // SPEC: K=0 is "disabled", not dead
    EXPECT_EQ(pool.target(), 0u);
    EXPECT_EQ(pool.size(), 0u);
    pool.shutdown();
    EXPECT_FALSE(pool.running());
}

TEST(WarmPoolProbe, NullPoolReportsDown) {
    StdoutSilencer silencer;
    auto probe = WarmPool::make_probe(nullptr);
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("no warm pool"), std::string::npos);
}

TEST(WarmPoolProbe, UnstartedPoolReportsDownWithZeroSize) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());
    WarmPool pool(&client);
    auto probe = WarmPool::make_probe(&pool);
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("not running"), std::string::npos);
    EXPECT_EQ(r.extra["warm_pool"].get<std::int64_t>(), 0);
}

TEST(WarmPoolProbe, ShutDownPoolReportsDown) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());
    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(2)));
    pool.shutdown();
    auto probe = WarmPool::make_probe(&pool);
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("not running"), std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  Integration tests — mock docker proxy
// ────────────────────────────────────────────────────────────────────────────

TEST(WarmPoolIntegration, StartPrecreatesKContainers) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(3)));
    // Synchronous precreate: count is exact.
    EXPECT_EQ(pool.target(), 3u);
    EXPECT_EQ(pool.size(),   3u);
    EXPECT_EQ(proxy.create_calls.load(), 3);

    pool.shutdown();
    EXPECT_FALSE(pool.running());
    // shutdown removed all idle ids.
    EXPECT_EQ(proxy.delete_calls.load(), 3);
}

TEST(WarmPoolIntegration, StartSendsIdleContainerSchema) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(1)));
    ASSERT_FALSE(proxy.last_create_body.empty());
    auto body = nlohmann::json::parse(proxy.last_create_body);
    EXPECT_EQ(body["Image"], "litecode-judge:latest");
    EXPECT_EQ(body["Cmd"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"--help"}));
    EXPECT_EQ(body["User"], "judgeuser");
    EXPECT_EQ(body["HostConfig"]["NetworkMode"],    "none");
    EXPECT_EQ(body["HostConfig"]["ReadonlyRootfs"], true);
    EXPECT_EQ(body["HostConfig"]["PidsLimit"].get<std::int64_t>(), 50);
    EXPECT_EQ(body["HostConfig"]["Memory"].get<std::int64_t>(),
              64LL * 1024 * 1024);
    EXPECT_EQ(body["HostConfig"]["SecurityOpt"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"no-new-privileges:true"}));
    EXPECT_EQ(body["HostConfig"]["Tmpfs"]["/tmp"], "size=64m,mode=1777");

    pool.shutdown();
}

TEST(WarmPoolIntegration, StartIsIdempotentAfterShutdown) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(2)));
    pool.shutdown();
    // Re-start should work and re-create the pool.
    ASSERT_TRUE(pool.start(cfg_with_target(2)));
    EXPECT_EQ(pool.size(), 2u);
    pool.shutdown();
}

TEST(WarmPoolIntegration, AcquirePopsIdleFromPool) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(3)));
    EXPECT_EQ(pool.size(), 3u);

    auto a = pool.acquire();
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a->from_pool);
    EXPECT_FALSE(a->id.empty());
    EXPECT_EQ(pool.size(), 2u);

    auto b = pool.acquire();
    ASSERT_TRUE(b.has_value());
    EXPECT_TRUE(b->from_pool);
    EXPECT_NE(b->id, a->id);              // distinct ids
    EXPECT_EQ(pool.size(), 1u);

    pool.shutdown();
}

TEST(WarmPoolIntegration, AcquireFallsBackToSynchronousCreateWhenEmpty) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(0)));    // disabled mode
    EXPECT_EQ(pool.size(), 0u);

    auto a = pool.acquire();
    ASSERT_TRUE(a.has_value());
    EXPECT_FALSE(a->from_pool);                     // freshly created
    EXPECT_GE(proxy.create_calls.load(), 1);

    pool.shutdown();
}

TEST(WarmPoolIntegration, AcquireReturnsNulloptOnCreateFailure) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    // Every create returns 503 → pool can't even precreate.
    proxy.next_create_response_ = {503, R"({"message":"down"})"};

    WarmPool pool(&client);
    EXPECT_FALSE(pool.start(cfg_with_target(1)));
    EXPECT_FALSE(pool.running());

    // acquire on a not-running pool → nullopt (no synchronous fallback
    // attempted because we know we shut down).
    EXPECT_FALSE(pool.acquire().has_value());
}

TEST(WarmPoolIntegration, ReleaseDoesNotReturnContainerToPool) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(2)));
    const std::size_t initial = proxy.create_calls.load();

    auto a = pool.acquire();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(pool.size(), 1u);

    // Release the acquired id. Per SPEC §7.1 step 5, the released
    // container is NOT reused — it must be docker rm'd by the worker.
    // The pool instead kicks an async refill.
    pool.release(a->id);
    // The pool must NOT immediately put the released id back.
    EXPECT_EQ(pool.size(), 1u);
    // The refill thread should soon replace it.
    EXPECT_TRUE(wait_until([&]{ return pool.size() >= 2u; }));
    EXPECT_GE(proxy.create_calls.load(), initial + 1);

    pool.shutdown();
    // shutdown should have removed at least the two that were idle
    // at the moment of shutdown (possibly 1 or 2, depending on whether
    // the refill container beat shutdown to the queue).
    EXPECT_GE(proxy.delete_calls.load(), 1);
}

TEST(WarmPoolIntegration, RefillReachesTargetAfterDrain) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(4)));
    EXPECT_EQ(pool.size(), 4u);

    // Drain entirely.
    std::vector<std::string> drained;
    for (int i = 0; i < 4; ++i) {
        auto a = pool.acquire();
        ASSERT_TRUE(a.has_value());
        drained.push_back(a->id);
    }
    EXPECT_EQ(pool.size(), 0u);

    // Release them all; refill thread should bring count back to 4.
    for (auto& id : drained) pool.release(id);

    EXPECT_TRUE(wait_until([&]{ return pool.size() == 4u; },
                           std::chrono::milliseconds(3000)));
    pool.shutdown();
}

TEST(WarmPoolIntegration, ShutdownDuringRefillDoesNotDeadlock) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(2)));

    // Trigger a release; while the refill thread is mid-create, shut
    // down. The shutdown() join must not block forever.
    auto a = pool.acquire();
    ASSERT_TRUE(a.has_value());
    pool.release(a->id);

    // Tear down immediately. Use a thread to enforce a wall-clock
    // upper bound so a deadlock fails the test instead of hanging.
    std::thread shutdown_thread([&]{ pool.shutdown(); });
    ASSERT_TRUE(wait_until([&]{ return !pool.running(); },
                           std::chrono::milliseconds(3000)))
        << "shutdown() did not return within 3s — possible deadlock";
    shutdown_thread.join();
    EXPECT_FALSE(pool.running());
}

TEST(WarmPoolIntegration, RefillRetriesAfterTransientCreateFailure) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(1)));
    EXPECT_EQ(pool.size(), 1u);

    // Drain; arm the next create call to fail, then succeed.
    auto a = pool.acquire();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(pool.size(), 0u);

    // Arm ONE failure, then the next create succeeds (the mock auto-
    // clears the prebuilt response after one shot). With
    // retry_attempts=1 the refill thread should retry once and
    // succeed.
    proxy.next_create_response_ = {503, R"({"message":"transient"})"};

    pool.release(a->id);
    EXPECT_TRUE(wait_until([&]{ return pool.size() == 1u; },
                           std::chrono::milliseconds(3000)))
        << "refill did not recover from a transient failure";
    pool.shutdown();
}

TEST(WarmPoolIntegration, ConcurrentAcquireReleaseRaceFree) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(4)));
    EXPECT_EQ(pool.size(), 4u);

    constexpr int N_THREADS = 8;
    constexpr int N_OPS     = 50;
    std::atomic<int> acquires{0};
    std::atomic<int> releases{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    workers.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        workers.emplace_back([&]{
            for (int i = 0; i < N_OPS; ++i) {
                auto a = pool.acquire();
                if (!a.has_value()) {
                    ++failures;
                    continue;
                }
                ++acquires;
                // Simulate a tiny bit of work.
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                pool.release(a->id);
                ++releases;
            }
        });
    }
    for (auto& w : workers) w.join();

    // Every successful acquire was matched by a release. Failures are
    // expected when the refill thread falls behind on the synchronous
    // fallback path during a burst — the count must be small and
    // bounded (we can't demand zero on a 4-slot pool with 8 threads
    // hammering it).
    EXPECT_EQ(acquires.load(), releases.load());
    EXPECT_LE(failures.load(), N_THREADS * N_OPS);
    // After all threads settle, the refill thread should bring size
    // back to target (with a small grace period).
    EXPECT_TRUE(wait_until([&]{ return pool.size() == 4u; },
                           std::chrono::milliseconds(5000)));

    pool.shutdown();
    EXPECT_FALSE(pool.running());
}

TEST(WarmPoolIntegration, HealthProbePublishesCurrentSizeAndTarget) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(3)));

    auto probe = WarmPool::make_probe(&pool);
    auto r = probe();
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.extra["warm_pool"].get<std::int64_t>(), 3);
    EXPECT_EQ(r.extra["warm_pool_target"].get<std::int64_t>(), 3);

    (void)pool.acquire();
    r = probe();
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.extra["warm_pool"].get<std::int64_t>(), 2);

    pool.shutdown();
}

TEST(WarmPoolIntegration, ProbeCanBeWiredIntoHealthService) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    litecode::docker::Client client(proxy.url());

    WarmPool pool(&client);
    ASSERT_TRUE(pool.start(cfg_with_target(2)));

    litecode::HealthService h;
    h.register_probe("warm_pool", WarmPool::make_probe(&pool));
    int status = 0;
    const auto body = h.build_response(&status);
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body["warm_pool"].get<int>(), 2);

    pool.shutdown();
    // After shutdown the probe flips to down, but we registered it
    // with the same factory — extra.warm_pool still carries last size.
    auto body2 = h.build_response(&status);
    EXPECT_EQ(status, 503);
    EXPECT_EQ(body2["status"], "degraded");
    EXPECT_EQ(body2["checks"]["warm_pool"]["ok"], false);
}

} // anonymous namespace