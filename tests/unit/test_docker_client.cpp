// tests/unit/test_docker_client.cpp
//
// Unit + light-integration tests for src/judge/docker_client.h
//   (Phase 4 ★ — Docker client via socket proxy).
//
// Two layers (mirrors the project pattern from test_health.cpp,
// test_connection_pool.cpp):
//
//   1) Pure unit tests (no docker daemon, no socket):
//        - Endpoint URL parser: scheme/host/port/path extraction
//        - Endpoint URL parser: rejects bad schemes / missing scheme
//        - Endpoint URL parser: rejects bogus ports / empty host
//        - Endpoint URL parser: rejects @/query/fragment
//        - Endpoint::to_url + join (slash dedup)
//        - build_create_body schema: image/cmd/env/working_dir/user
//        - build_create_body HostConfig: NetworkMode/ReadonlyRootfs/PidsLimit
//        - build_create_body HostConfig: Memory in bytes / NanoCpus
//        - build_create_body HostConfig: Tmpfs + BindMounts schema
//        - build_create_body negative: empty command block is omitted
//        - build_create_body negative: cpus<=0 / memory_mb<=0 omitted
//        - build_query URL-encoding for spaces + ampersands
//        - CreateOptions validation: image required
//        - start/wait/kill/remove validation: empty container id
//        - wait validation: timeout_ms >= 1
//        - create_exec validation: cmd non-empty / container id non-empty
//        - Exception hierarchy: HttpError carries status/body/url
//        - Client constructor throws on malformed URL
//        - ping() never throws
//
//   2) Light integration tests (in-process httplib::Server simulating
//      the docker socket proxy — no docker daemon required):
//        - GET /_ping → 200 returns true, 500 returns false, connection
//          refused returns false (caught by probe path)
//        - GET /info /version round-trip JSON
//        - POST /containers/create returns id + warnings; request body
//          carries HostConfig schema fields
//        - POST /containers/{id}/start → 204
//        - DELETE /containers/{id}?force=true&v=false → 204
//        - POST /containers/{id}/wait?condition=not-running → exit code
//        - POST /containers/{id}/kill → 204
//        - POST /containers/{id}/exec → exec id
//        - GET /containers/{id}/logs → raw body
//        - GET /containers/{id}/json inspect → JSON
//        - 4xx from daemon → DockerHttpError with status preserved
//        - Connection refused (port 1) → DockerTimeoutError
//        - Slow daemon → wait() times out per-call timeout
//        - make_docker_probe wires ping into HealthService
//
// We deliberately do NOT depend on a real Docker daemon. The httplib
// mock surface area is small (~12 routes) and we own both sides, so
// the test is fully reproducible on dev machines without docker.
//
// All integration cases run in-process; none require a network port
// other than the ephemeral one httplib::Server picks.

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

namespace {
// Bring config types into the anonymous namespace's lookup so the
// TEST bodies stay terse (mirrors test_health.cpp / test_config.cpp).
using litecode::JudgeConfig;

// ────────────────────────────────────────────────────────────────────────────
//  StdoutSilencer — `LOG_INFO` from happy-path tests floods the test
//  binary's stdout. Same trick used in test_health.cpp.
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

// ────────────────────────────────────────────────────────────────────────────
//  Pure unit tests — URL parsing, JSON building, exception hierarchy,
//  validators. No HTTP traffic.
// ────────────────────────────────────────────────────────────────────────────

using namespace litecode::docker;

TEST(EndpointParse, BasicTcpUrl) {
    StdoutSilencer silencer;
    Endpoint ep = detail::parse_endpoint_url("tcp://docker-proxy:2375");
    EXPECT_EQ(ep.scheme, "tcp");
    EXPECT_EQ(ep.host,   "docker-proxy");
    EXPECT_EQ(ep.port,   2375);
    EXPECT_EQ(ep.base,   "/");
}

TEST(EndpointParse, HttpUrlWithPath) {
    StdoutSilencer silencer;
    Endpoint ep = detail::parse_endpoint_url("http://localhost:9999/v1.40");
    EXPECT_EQ(ep.scheme, "http");
    EXPECT_EQ(ep.host,   "localhost");
    EXPECT_EQ(ep.port,   9999);
    EXPECT_EQ(ep.base,   "/v1.40");
}

TEST(EndpointParse, NoPathDefaultsToRoot) {
    StdoutSilencer silencer;
    Endpoint ep = detail::parse_endpoint_url("http://h:1");
    EXPECT_EQ(ep.base, "/");
}

TEST(EndpointParse, PortMissingDefaultsTo2375) {
    StdoutSilencer silencer;
    Endpoint ep = detail::parse_endpoint_url("tcp://h");
    EXPECT_EQ(ep.port, 2375);
}

TEST(EndpointParse, RejectsMissingScheme) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("docker-proxy:2375"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsEmptyScheme) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("://host:2375"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsHttpsScheme) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("https://host:2376"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsUnixScheme) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("unix:///var/run/docker.sock"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsBogusPort) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("tcp://h:not-a-port"),
                 DockerConfigError);
    EXPECT_THROW(detail::parse_endpoint_url("tcp://h:0"),
                 DockerConfigError);
    EXPECT_THROW(detail::parse_endpoint_url("tcp://h:99999"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsAtCredential) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("tcp://user@host:2375"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsQueryOrFragment) {
    StdoutSilencer silencer;
    EXPECT_THROW(detail::parse_endpoint_url("tcp://host:2375?foo=bar"),
                 DockerConfigError);
    EXPECT_THROW(detail::parse_endpoint_url("tcp://host:2375#frag"),
                 DockerConfigError);
}

TEST(EndpointParse, RejectsEmptyHost) {
    StdoutSilencer silencer;
    // ":2375" → authority starts with ':' → host empty.
    EXPECT_THROW(detail::parse_endpoint_url("tcp://:2375"),
                 DockerConfigError);
}

TEST(EndpointToUrl, NoTrailingSlash) {
    StdoutSilencer silencer;
    Endpoint ep;
    ep.scheme = "http"; ep.host = "h"; ep.port = 1234;
    EXPECT_EQ(ep.to_url(), "http://h:1234");
}

TEST(EndpointJoin, DedupesSlash) {
    StdoutSilencer silencer;
    Endpoint ep;
    ep.scheme = "http"; ep.host = "h"; ep.port = 1; ep.base = "/";
    EXPECT_EQ(ep.join("/containers/x"),  "/containers/x");
    EXPECT_EQ(ep.join("containers/x"),   "/containers/x");
    ep.base = "/v1.40";
    EXPECT_EQ(ep.join("/containers/x"),  "/v1.40/containers/x");
    EXPECT_EQ(ep.join("containers/x"),   "/v1.40/containers/x");
    ep.base = "/v1.40/";
    EXPECT_EQ(ep.join("/containers/x"),  "/v1.40/containers/x");
}

TEST(EndpointJoin, EmptyBaseDefaultsToSlash) {
    StdoutSilencer silencer;
    Endpoint ep;
    ep.base = "";
    EXPECT_EQ(ep.join("/foo"), "/foo");
}

// ── CreateOptions / build_create_body ──────────────────────────────────────

TEST(BuildCreateBody, ImageAndCmdAndEnv) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image   = "litecode-judge:latest";
    o.command = {"--help"};
    o.env     = {"FOO=bar", "BAZ=qux"};
    o.user    = "judgeuser";
    nlohmann::json b = detail::build_create_body(o);
    EXPECT_EQ(b["Image"],         "litecode-judge:latest");
    EXPECT_EQ(b["Cmd"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"--help"}));
    EXPECT_EQ(b["Env"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"FOO=bar", "BAZ=qux"}));
    EXPECT_EQ(b["User"], "judgeuser");
    EXPECT_EQ(b["AttachStdout"], false);
    EXPECT_EQ(b["Tty"],          false);
}

TEST(BuildCreateBody, EmptyCommandFieldOmitted) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image = "img";
    nlohmann::json b = detail::build_create_body(o);
    EXPECT_FALSE(b.contains("Cmd"));
    EXPECT_FALSE(b.contains("Env"));
    EXPECT_FALSE(b.contains("User"));
    EXPECT_FALSE(b.contains("WorkingDir"));
}

TEST(BuildCreateBody, NetworkDefaultsToNone) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image = "img";
    auto b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["NetworkMode"], "none");
}

TEST(BuildCreateBody, MemoryInBytes) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image     = "img";
    o.memory_mb = 256;
    auto b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["Memory"].get<std::int64_t>(),
              256LL * 1024 * 1024);
}

TEST(BuildCreateBody, MemoryMbZeroOmitsMemory) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image     = "img";
    o.memory_mb = 0;
    auto b = detail::build_create_body(o);
    EXPECT_FALSE(b["HostConfig"].contains("Memory"));
}

TEST(BuildCreateBody, CpusInNanoCpus) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image = "img";
    o.cpus  = 0.5;
    auto b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["NanoCpus"].get<std::int64_t>(), 500000000LL);

    o.cpus = 1.0;
    b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["NanoCpus"].get<std::int64_t>(), 1000000000LL);
}

TEST(BuildCreateBody, CpusZeroOrNegativeOmitsNanoCpus) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image = "img";
    o.cpus  = 0.0;
    auto b = detail::build_create_body(o);
    EXPECT_FALSE(b["HostConfig"].contains("NanoCpus"));

    o.cpus = -1.5;
    b = detail::build_create_body(o);
    EXPECT_FALSE(b["HostConfig"].contains("NanoCpus"));
}

TEST(BuildCreateBody, ReadOnlyFlag) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image     = "img";
    o.read_only = true;
    auto b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["ReadonlyRootfs"], true);

    o.read_only = false;
    b = detail::build_create_body(o);
    EXPECT_FALSE(b["HostConfig"].contains("ReadonlyRootfs"));
}

TEST(BuildCreateBody, PidsLimitZeroOmits) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image      = "img";
    o.pids_limit = 0;
    auto b = detail::build_create_body(o);
    EXPECT_FALSE(b["HostConfig"].contains("PidsLimit"));

    o.pids_limit = 50;
    b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["PidsLimit"].get<std::int64_t>(), 50);
}

TEST(BuildCreateBody, TmpfsMap) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image = "img";
    o.tmpfs = { {"/tmp", "size=64m,mode=1777"} };
    auto b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["Tmpfs"]["/tmp"],
              "size=64m,mode=1777");
}

TEST(BuildCreateBody, BindMountsSchema) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image = "img";
    o.mounts.push_back({"/host/task.json",
                        "/judge/task.json",
                        /*read_only=*/true});
    auto b = detail::build_create_body(o);
    ASSERT_TRUE(b["HostConfig"]["Mounts"].is_array());
    ASSERT_EQ(b["HostConfig"]["Mounts"].size(), 1u);
    EXPECT_EQ(b["HostConfig"]["Mounts"][0]["Type"],     "bind");
    EXPECT_EQ(b["HostConfig"]["Mounts"][0]["Source"],   "/host/task.json");
    EXPECT_EQ(b["HostConfig"]["Mounts"][0]["Target"],   "/judge/task.json");
    EXPECT_EQ(b["HostConfig"]["Mounts"][0]["ReadOnly"], true);
}

TEST(BuildCreateBody, SecurityOpt) {
    StdoutSilencer silencer;
    CreateOptions o;
    o.image         = "img";
    o.security_opt  = {"no-new-privileges:true"};
    auto b = detail::build_create_body(o);
    EXPECT_EQ(b["HostConfig"]["SecurityOpt"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"no-new-privileges:true"}));
}

// ── build_query URL encoding ──────────────────────────────────────────────

TEST(BuildQuery, EmptyListYieldsEmpty) {
    StdoutSilencer silencer;
    EXPECT_EQ(detail::build_query({}), "");
}

TEST(BuildQuery, RendersSimplePairs) {
    StdoutSilencer silencer;
    auto q = detail::build_query({{"force", "true"}, {"v", "false"}});
    EXPECT_EQ(q, "force=true&v=false");
}

TEST(BuildQuery, DropsEmptyValues) {
    StdoutSilencer silencer;
    auto q = detail::build_query({{"a", ""}, {"b", "x"}});
    EXPECT_EQ(q, "b=x");
}

TEST(BuildQuery, EncodesSpacesAmpersandsEtc) {
    StdoutSilencer silencer;
    auto q = detail::build_query({{"k", "a b&c=d?e#f"}});
    EXPECT_EQ(q, "k=a%20b%26c%3Dd%3Fe%23f");
}

// ── Validators / exception hierarchy ───────────────────────────────────────

TEST(HttpError, CarriesStatusUrlBody) {
    StdoutSilencer silencer;
    DockerHttpError e(404, "http://h:1/v1.40/containers/x",
                      "No such container: x");
    EXPECT_EQ(e.status(), 404);
    EXPECT_EQ(e.url(),    "http://h:1/v1.40/containers/x");
    EXPECT_EQ(e.body(),   "No such container: x");
    // Catching via base class should also work for the unified handler
    // in judge_scheduler.h.
    try {
        throw DockerHttpError(500, "u", "b");
    } catch (const DockerClientError& base) {
        EXPECT_NE(std::string(base.what()).find("500"), std::string::npos);
    }
}

TEST(ExceptionHierarchy, InheritsRuntime) {
    StdoutSilencer silencer;
    EXPECT_THROW(throw DockerConfigError("x"), std::runtime_error);
    EXPECT_THROW(throw DockerTimeoutError("x"), DockerClientError);
    EXPECT_THROW(throw DockerHttpError(503, "u", "b"),
                 DockerClientError);
}

TEST(ClientCtor, RejectsMalformedUrl) {
    StdoutSilencer silencer;
    EXPECT_THROW(Client("not-a-url"),       DockerConfigError);
    EXPECT_THROW(Client("https://h:1"),     DockerConfigError);
    EXPECT_THROW(Client("tcp://h:bogus"),   DockerConfigError);
}

TEST(ClientCtor, RejectsNegativeTimeout) {
    StdoutSilencer silencer;
    EXPECT_THROW(Client("tcp://h:1", 0),    DockerConfigError);
    EXPECT_THROW(Client("tcp://h:1", -100), DockerConfigError);
}

TEST(ClientCtor, AcceptsValidUrl) {
    StdoutSilencer silencer;
    Client c("tcp://h:1234");
    EXPECT_EQ(c.endpoint().host, "h");
    EXPECT_EQ(c.endpoint().port, 1234u);
}

// ── Public method validators (zero-network) ───────────────────────────────

TEST(ClientValidators, StartRejectsEmptyId) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.start(""), DockerConfigError);
}

TEST(ClientValidators, WaitRejectsEmptyId) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.wait("", 1000), DockerConfigError);
}

TEST(ClientValidators, WaitRejectsBadTimeout) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.wait("abc", 0),  DockerConfigError);
    EXPECT_THROW(c.wait("abc", -1), DockerConfigError);
}

TEST(ClientValidators, KillRejectsEmptyId) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.kill(""), DockerConfigError);
}

TEST(ClientValidators, RemoveRejectsEmptyId) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.remove(""), DockerConfigError);
}

TEST(ClientValidators, InspectRejectsEmptyId) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.inspect(""), DockerConfigError);
}

TEST(ClientValidators, CreateRejectsEmptyImage) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    CreateOptions o;  // image empty
    EXPECT_THROW(c.create(o), DockerConfigError);
}

TEST(ClientValidators, CreateExecRejectsEmptyCmd) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    EXPECT_THROW(c.create_exec("", {"ls"}), DockerConfigError);
    EXPECT_THROW(c.create_exec("abc", {}),   DockerConfigError);
}

TEST(ClientLogs, LogsRejectsEmptyIdSilently) {
    StdoutSilencer silencer;
    Client c("tcp://h:1");
    // `logs` is best-effort; no throw, just return empty.
    EXPECT_EQ(c.logs(""), "");
}

// ────────────────────────────────────────────────────────────────────────────
//  HTTP integration tests — in-process httplib::Server
//
//  The mock server responds deterministically, captures every request
//  the Client makes (method, path, body, headers), and exposes a
//  couple of knobs so individual tests can simulate slow / failing
//  daemon behaviour.
// ────────────────────────────────────────────────────────────────────────────

class MockDockerProxy {
public:
    void start() {
        // GET /_ping
        srv_.Get("/_ping",
            [this](const httplib::Request&, httplib::Response& res) {
                record("GET", "/_ping", "");
                if (ping_should_fail_) {
                    res.status = 500;
                    return;
                }
                res.status = 200;
                res.set_content("OK", "text/plain");
            });
        // GET /info
        srv_.Get("/info",
            [this](const httplib::Request&, httplib::Response& res) {
                record("GET", "/info", "");
                res.status = 200;
                res.set_content(R"({"ID":"abc","Containers":3})",
                                "application/json");
            });
        // GET /version
        srv_.Get("/version",
            [this](const httplib::Request&, httplib::Response& res) {
                record("GET", "/version", "");
                res.status = 200;
                res.set_content(R"({"Version":"20.10.21"})",
                                "application/json");
            });

        // POST /containers/create
        srv_.Post("/containers/create",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", "/containers/create", req.body);
                if (next_create_response_.status != 0) {
                    res.status = next_create_response_.status;
                    res.set_content(next_create_response_.body,
                                    "application/json");
                    return;
                }
                res.status = 201;
                res.set_content(
                    R"({"Id":"abc123","Warnings":["could not bind"]})",
                    "application/json");
            });

        // POST /containers/{id}/start
        srv_.Post(R"(/containers/([^/]+)/start)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                if (start_should_fail_) {
                    res.status = 404;
                    res.set_content("No such container", "text/plain");
                    return;
                }
                res.status = 204;
            });

        // DELETE /containers/{id}
        srv_.Delete(R"(/containers/([^/]+))",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("DELETE", req.path, req.body);
                last_query_ = req.params;
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                if (delete_should_fail_) {
                    res.status = 409;
                    res.set_content("conflict", "text/plain");
                    return;
                }
                res.status = 204;
            });

        // POST /containers/{id}/kill
        srv_.Post(R"(/containers/([^/]+)/kill)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                if (kill_should_fail_) {
                    res.status = 500;
                    res.set_content("kill failed", "text/plain");
                    return;
                }
                res.status = 204;
            });

        // POST /containers/{id}/wait
        srv_.Post(R"(/containers/([^/]+)/wait)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                last_query_ = req.params;

                if (wait_delay_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(wait_delay_ms_));
                }
                if (wait_should_fail_) {
                    res.status = 500;
                    res.set_content("wait failed", "text/plain");
                    return;
                }
                res.status = 200;
                res.set_content(
                    nlohmann::json{
                        {"StatusCode", wait_exit_code_},
                        {"Error",      wait_error_str_}
                    }.dump(),
                    "application/json");
            });

        // POST /containers/{id}/exec
        srv_.Post(R"(/containers/([^/]+)/exec)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("POST", req.path, req.body);
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                if (exec_should_fail_) {
                    res.status = 404;
                    res.set_content("no container", "text/plain");
                    return;
                }
                res.status = 201;
                res.set_content(R"({"Id":"exec-id-42"})",
                                "application/json");
            });

        // GET /containers/{id}/json (inspect)
        srv_.Get(R"(/containers/([^/]+)/json)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("GET", req.path, "");
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                res.status = 200;
                res.set_content(
                    R"({"Id":"abc","State":{"Running":false}})",
                    "application/json");
            });

        // GET /containers/{id}/logs
        srv_.Get(R"(/containers/([^/]+)/logs)",
            [this](const httplib::Request& req, httplib::Response& res) {
                record("GET", req.path, "");
                last_query_ = req.params;
                if (req.matches.size() > 1) last_id_ = req.matches[1];
                res.status = 200;
                res.set_content("hello-judge-stdout", "text/plain");
            });

        // Anything else → 404 (so accidental requests surface clearly).
        srv_.set_default_headers({{"Server", "mock-docker-proxy"}});

        const int port = srv_.bind_to_any_port("127.0.0.1");
        ASSERT_TRUE(port > 0) << "failed to bind ephemeral port";
        port_ = port;
        thread_ = std::thread([this]{ srv_.listen_after_bind(); });
    }

    void stop() {
        if (port_ > 0) srv_.stop();
        if (thread_.joinable()) thread_.join();
        port_ = 0;
    }

    ~MockDockerProxy() { stop(); }

    int port() const noexcept { return port_; }
    std::string url() const {
        std::ostringstream os;
        os << "http://127.0.0.1:" << port_;
        return os.str();
    }

    // State hooks used by individual tests.
    bool  ping_should_fail_      = false;
    bool  start_should_fail_     = false;
    bool  delete_should_fail_    = false;
    bool  kill_should_fail_      = false;
    bool  wait_should_fail_      = false;
    int   wait_delay_ms_         = 0;
    int   wait_exit_code_        = 0;
    std::string wait_error_str_  = "";
    bool  exec_should_fail_      = false;

    struct PrebuiltResponse { int status = 0; std::string body; };
    PrebuiltResponse next_create_response_{};

    // Recorded request state — best-effort; not strictly thread-safe
    // because the test driver serializes them anyway.
    struct RecordedRequest {
        std::string method;
        std::string path;
        std::string body;
    };
    std::vector<RecordedRequest>& requests() noexcept { return requests_; }
    const std::vector<RecordedRequest>& requests() const noexcept {
        return requests_; }
    const std::string& last_id()    const noexcept { return last_id_; }
    httplib::Params    last_query() const noexcept { return last_query_; }

private:
    void record(const std::string& m,
                const std::string& p,
                const std::string& b) {
        requests_.push_back({m, p, b});
    }

    httplib::Server                srv_;
    std::thread                    thread_;
    int                            port_ = 0;
    std::vector<RecordedRequest>   requests_;
    std::string                    last_id_;
    httplib::Params                last_query_;
};

// ── Light integration tests ────────────────────────────────────────────────

TEST(DockerClientIntegration, PingReachable) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    EXPECT_TRUE(c.ping());
    EXPECT_EQ(proxy.requests().back().method, "GET");
    EXPECT_EQ(proxy.requests().back().path,   "/_ping");
}

TEST(DockerClientIntegration, PingUnreachable) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.ping_should_fail_ = true;
    proxy.start();
    Client c(proxy.url());
    EXPECT_FALSE(c.ping());
}

TEST(DockerClientIntegration, PingConnectionRefusedReturnsFalse) {
    StdoutSilencer silencer;
    // 127.0.0.1:1 — nothing listens; the probe path must NOT throw.
    Client c("http://127.0.0.1:1");
    EXPECT_FALSE(c.ping());
}

TEST(DockerClientIntegration, InfoAndVersion) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    auto i = c.info();
    EXPECT_EQ(i["ID"], "abc");
    EXPECT_EQ(i["Containers"].get<int>(), 3);

    auto v = c.version();
    EXPECT_EQ(v["Version"], "20.10.21");
}

TEST(DockerClientIntegration, CreateRoundTripsIdAndWarnings) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());

    CreateOptions o;
    o.image      = "litecode-judge:latest";
    o.command    = {"--help"};
    o.memory_mb  = 64;
    o.cpus       = 0.5;
    o.env        = {"FOO=bar"};
    o.tmpfs      = { {"/tmp", "size=32m"} };
    o.mounts.push_back({"/host/task.json",
                        "/judge/task.json",
                        /*read_only=*/true});
    o.network_mode = "none";
    o.read_only    = true;
    o.pids_limit   = 32;
    o.user         = "judgeuser";
    o.security_opt = {"no-new-privileges:true"};

    auto r = c.create(o);
    EXPECT_EQ(r.id, "abc123");
    EXPECT_EQ(r.warnings, "could not bind");

    // Last recorded request should be POST /containers/create with
    // the schema we expect.
    const auto& req = proxy.requests().back();
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.path,   "/containers/create");

    auto body = nlohmann::json::parse(req.body);
    EXPECT_EQ(body["Image"], "litecode-judge:latest");
    EXPECT_EQ(body["Cmd"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"--help"}));
    EXPECT_EQ(body["Env"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"FOO=bar"}));
    EXPECT_EQ(body["User"], "judgeuser");
    EXPECT_EQ(body["HostConfig"]["NetworkMode"],     "none");
    EXPECT_EQ(body["HostConfig"]["ReadonlyRootfs"],  true);
    EXPECT_EQ(body["HostConfig"]["PidsLimit"].get<std::int64_t>(), 32);
    EXPECT_EQ(body["HostConfig"]["Memory"].get<std::int64_t>(),
              64LL * 1024 * 1024);
    EXPECT_EQ(body["HostConfig"]["NanoCpus"].get<std::int64_t>(),
              500000000LL);
    EXPECT_EQ(body["HostConfig"]["Tmpfs"]["/tmp"], "size=32m");
    EXPECT_EQ(body["HostConfig"]["SecurityOpt"].get<std::vector<std::string>>(),
              (std::vector<std::string>{"no-new-privileges:true"}));
    EXPECT_EQ(body["HostConfig"]["Mounts"][0]["Target"], "/judge/task.json");
}

TEST(DockerClientIntegration, CreatePrebuiltErrorResponseSurfacesHttpError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.next_create_response_ = {409, R"({"message":"conflict"})"};
    proxy.start();
    Client c(proxy.url());

    CreateOptions o;
    o.image = "img";
    try {
        c.create(o);
        FAIL() << "expected DockerHttpError";
    } catch (const DockerHttpError& e) {
        EXPECT_EQ(e.status(), 409);
    }
}

TEST(DockerClientIntegration, StartReturnsNoErrorOn204) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    EXPECT_NO_THROW(c.start("abc"));
    EXPECT_EQ(proxy.last_id(), "abc");
}

TEST(DockerClientIntegration, Start404RaisesHttpError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start_should_fail_ = true;
    proxy.start();
    Client c(proxy.url());
    try {
        c.start("missing");
        FAIL() << "expected DockerHttpError";
    } catch (const DockerHttpError& e) {
        EXPECT_EQ(e.status(), 404);
    }
}

TEST(DockerClientIntegration, KillNoErrorOn204) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    EXPECT_NO_THROW(c.kill("abc"));
}

TEST(DockerClientIntegration, Kill404RaisesHttpError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.kill_should_fail_ = true;
    proxy.start();
    Client c(proxy.url());
    EXPECT_THROW(c.kill("abc"), DockerHttpError);
}

TEST(DockerClientIntegration, RemoveEncodesQueryString) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    EXPECT_NO_THROW(c.remove("abc"));
    // Verify our default `force=true&v=false` is what we send.
    const auto& q = proxy.last_query();
    EXPECT_TRUE(q.find("force") != q.end());
    EXPECT_EQ(q.find("force")->second, "true");
    EXPECT_TRUE(q.find("v") != q.end());
    EXPECT_EQ(q.find("v")->second, "false");
}

TEST(DockerClientIntegration, RemoveWithFalseFlags) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    EXPECT_NO_THROW(c.remove("abc", /*force=*/false,
                                  /*remove_volumes=*/true));
    const auto& q = proxy.last_query();
    EXPECT_EQ(q.find("force")->second, "false");
    EXPECT_EQ(q.find("v")->second,     "true");
}

TEST(DockerClientIntegration, Remove409RaisesHttpError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.delete_should_fail_ = true;
    proxy.start();
    Client c(proxy.url());
    EXPECT_THROW(c.remove("abc"), DockerHttpError);
}

TEST(DockerClientIntegration, WaitReturnsExitCode) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.wait_exit_code_  = 42;
    proxy.wait_error_str_  = "";
    proxy.start();
    Client c(proxy.url());
    auto w = c.wait("abc", 5000);
    EXPECT_EQ(w.exit_code, 42);
    EXPECT_EQ(w.error, "");

    // Verify the query string carries condition=not-running (the only
    // thing /wait accepts).
    const auto& q = proxy.last_query();
    ASSERT_TRUE(q.find("condition") != q.end());
    EXPECT_EQ(q.find("condition")->second, "not-running");
}

TEST(DockerClientIntegration, WaitCapturesErrorBody) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.wait_exit_code_ = 137;
    proxy.wait_error_str_ = "oom";
    proxy.start();
    Client c(proxy.url());
    auto w = c.wait("abc", 5000);
    EXPECT_EQ(w.exit_code, 137);
    EXPECT_EQ(w.error,     "oom");
}

TEST(DockerClientIntegration, Wait5xxRaisesHttpError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.wait_should_fail_ = true;
    proxy.start();
    Client c(proxy.url());
    try {
        c.wait("abc", 5000);
        FAIL() << "expected DockerHttpError";
    } catch (const DockerHttpError& e) {
        EXPECT_EQ(e.status(), 500);
    }
}

TEST(DockerClientIntegration, WaitTimeoutRaisesDockerTimeoutError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.wait_delay_ms_ = 500;  // hold the connection 500ms
    proxy.start();
    Client c(proxy.url());
    try {
        c.wait("abc", /*timeout_ms=*/100);
        FAIL() << "expected DockerTimeoutError";
    } catch (const DockerTimeoutError&) {
        SUCCEED();
    } catch (const std::exception& e) {
        FAIL() << "wrong exception type: " << e.what();
    }
}

TEST(DockerClientIntegration, InspectReturnsJson) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    auto j = c.inspect("abc");
    EXPECT_EQ(j["Id"], "abc");
    EXPECT_EQ(j["State"]["Running"], false);
}

TEST(DockerClientIntegration, CreateExecReturnsId) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    auto e = c.create_exec("abc", {"ls", "/judge"});
    EXPECT_EQ(e.id, "exec-id-42");
}

TEST(DockerClientIntegration, CreateExec404RaisesHttpError) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.exec_should_fail_ = true;
    proxy.start();
    Client c(proxy.url());
    EXPECT_THROW(c.create_exec("abc", {"ls"}), DockerHttpError);
}

TEST(DockerClientIntegration, LogsReturnsRawBody) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    auto body = c.logs("abc");
    EXPECT_EQ(body, "hello-judge-stdout");
}

TEST(DockerClientIntegration, ConnectionRefusedThrowsTimeoutError) {
    StdoutSilencer silencer;
    // Nothing listens on 127.0.0.1:1.
    Client c("http://127.0.0.1:1");
    try {
        c.info();
        FAIL() << "expected DockerTimeoutError";
    } catch (const DockerTimeoutError&) {
        SUCCEED();
    } catch (const std::exception& e) {
        FAIL() << "wrong exception type: " << e.what();
    }
}

// ── Health probe wiring ────────────────────────────────────────────────────

TEST(DockerProbe, NullClientReportsDown) {
    StdoutSilencer silencer;
    auto probe = make_docker_probe(static_cast<Client*>(nullptr));
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("no docker client"), std::string::npos);
}

TEST(DockerProbe, ReachableClientReportsOk) {
    StdoutSilencer silencer;
    MockDockerProxy proxy;
    proxy.start();
    Client c(proxy.url());
    auto probe = make_docker_probe(&c);
    auto r = probe();
    EXPECT_TRUE(r.ok);
    EXPECT_NE(r.detail.find("reachable"), std::string::npos);
}

TEST(DockerProbe, FailedPingReportsDown) {
    StdoutSilencer silencer;
    Client c("http://127.0.0.1:1");   // nothing listening
    auto probe = make_docker_probe(&c);
    auto r = probe();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("unreachable"), std::string::npos);
}

// ── Config bridge ──────────────────────────────────────────────────────────

TEST(MakeClientFromConfig, EmptyUrlYieldsNull) {
    StdoutSilencer silencer;
    JudgeConfig cfg;
    cfg.docker_socket_url = "";
    auto c = make_client_from_config(cfg);
    EXPECT_EQ(c, nullptr);
}

TEST(MakeClientFromConfig, ValidConfigYieldsClient) {
    StdoutSilencer silencer;
    JudgeConfig cfg;
    cfg.docker_socket_url = "tcp://docker-proxy:2375";
    auto c = make_client_from_config(cfg);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->endpoint().host, "docker-proxy");
    EXPECT_EQ(c->endpoint().port, 2375u);
}

} // anonymous namespace
