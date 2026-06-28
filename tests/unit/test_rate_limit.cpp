// tests/unit/test_rate_limit.cpp
//
// Unit tests for src/middleware/rate_limit.h — the SPEC §5.1 / §5.2 / §5.3
// / §5.5 / §11 Phase 2 ★ in-memory token-bucket rate limiter.
//
// Coverage:
//   - extract_client_ip:
//       * no forwarded headers         → req.remote_addr
//       * X-Forwarded-For single value → that value
//       * X-Forwarded-For chain        → first (originating) IP
//       * X-Real-IP fallback           → X-Real-IP
//       * OWS trimming around comma split
//   - try_extract_jwt_subject:
//       * no header / wrong scheme      → ""
//       * non-JWT body                 → ""
//       * valid JWT (no verify)        → sub
//       * JWT without `sub` claim      → ""
//       * garbage base64 payload       → ""
//   - detail::base64url_decode:
//       * round-trips a known value
//       * rejects characters outside the URL-safe alphabet
//       * tolerates missing padding
//   - RateLimiter core (token bucket):
//       * allows up to `capacity` requests
//       * denies the (capacity+1)th within the window
//       * refills over time (clock injection — no real sleep)
//       * different (quota, key) pairs have independent buckets
//       * same key under two different quotas doesn't share a bucket
//       * clock going backward does not grant free tokens
//       * peek() does not consume
//       * size() / clear() accounting
//   - consume_rate_limit:
//       * sets X-RateLimit-Limit / -Remaining on allow
//       * on deny: sets Retry-After, throws ApiException(429, RATE_LIMITED)
//       * ByUser quota falls back to IP when no Bearer header present
//   - Quota factories: read RateLimitConfig fields verbatim.
//   - End-to-end via in-process HttpServer (A26):
//       * 5 registrations OK, 6th gets 429 + Retry-After
//       * X-RateLimit-Limit / Remaining populated on every response

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "config.h"
#include "logger.h"
#include "middleware/rate_limit.h"
#include "routes/error_handler.h"
#include "server.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test environment — quiet logger
//
//  consume_rate_limit() emits LOG_WARN on the deny path. We pre-seed
//  a silent logger slot so config() never has to bootstrap from env
//  vars during tests, and so the "blocked" lines don't pollute the
//  test runner's stdout. Same pattern as test_auth_middleware.cpp.
// ────────────────────────────────────────────────────────────────────────────

class RateLimitTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        litecode::LoggingConfig log_cfg;
        log_cfg.level              = "ERROR";
        log_cfg.format             = "text";
        log_cfg.file_path          = "";
        log_cfg.include_request_id = true;
        litecode::init_logger(log_cfg);
    }
    void TearDown() override {
        litecode::reset_logger_for_testing();
    }
};

[[maybe_unused]] auto* g_env_registered = ::testing::AddGlobalTestEnvironment(
    new RateLimitTestEnvironment());

// ────────────────────────────────────────────────────────────────────────────
//  Test clock — single-source-of-truth time, mutated by tests.
//
//  The clock advances on demand (advance()) and reads in steady_clock
//  units. RateLimiter::set_clock() takes any callable returning a
//  time_point; we hand it a closure that captures the atomic.
// ────────────────────────────────────────────────────────────────────────────

class FakeClock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    FakeClock()
        : now_(std::chrono::steady_clock::now()) {}

    time_point now() const { return now_.load(std::memory_order_relaxed); }

    void advance(std::chrono::milliseconds delta) {
        now_.store(now_.load(std::memory_order_relaxed) + delta,
                   std::memory_order_relaxed);
    }

    void set(time_point t) { now_.store(t, std::memory_order_relaxed); }

    // Build a RateLimiter::Clock closure capturing this FakeClock.
    litecode::RateLimiter::Clock as_clock() {
        return [this]{ return this->now(); };
    }

private:
    std::atomic<time_point> now_;
};

// Helper: a quota with the same shape as `auth.register` (5/min/IP).
inline litecode::RateLimitQuota make_test_quota(
        const std::string& name = "test.quota",
        int                capacity = 5,
        std::chrono::seconds window = std::chrono::seconds(60)) {
    return litecode::RateLimitQuota{
        name, capacity, window, litecode::RateLimitKeyType::ByIp,
    };
}

// Build a minimal httplib::Request with optional IP / Authorization.
httplib::Request make_req(std::string_view remote_addr = "10.0.0.1",
                          std::string_view auth_header  = "",
                          std::string_view xff          = "",
                          std::string_view x_real_ip    = "") {
    httplib::Request req;
    req.method = "POST";
    req.path   = "/api/v1/auth/register";
    req.remote_addr = std::string(remote_addr);
    if (!auth_header.empty()) req.set_header("Authorization", std::string(auth_header));
    if (!xff.empty())         req.set_header("X-Forwarded-For", std::string(xff));
    if (!x_real_ip.empty())   req.set_header("X-Real-IP", std::string(x_real_ip));
    return req;
}

// Minimal app config for HttpServer boots.
litecode::AppConfig minimal_app_config() {
    litecode::AppConfig cfg;
    cfg.server.host = "127.0.0.1";
    cfg.server.port = 0;
    cfg.server.thread_pool_size = 2;
    cfg.jwt.secret              = "rate_limit_test_secret_at_least_32_bytes_long_xx";
    cfg.jwt.issuer              = "litecode-rl-test";
    cfg.jwt.access_ttl_seconds  = 600;
    cfg.jwt.refresh_ttl_seconds = 3600;
    // Keep the SPEC §5.1 default quotas so a handler that reads
    // config().rate_limit sees the right numbers.
    cfg.rate_limit.auth_register_per_minute_per_ip = 5;
    cfg.rate_limit.auth_login_per_minute_per_ip    = 10;
    cfg.rate_limit.submission_per_minute_per_user  = 30;
    cfg.rate_limit.admin_write_per_minute          = 30;
    cfg.rate_limit.bulk_import_per_hour            = 5;
    return cfg;
}

// Seed the env vars load_config() requires (JWT_SECRET) and bootstrap
// the singleton. Returns the populated AppConfig so a handler can
// close over it without re-reading litecode::config().
//
// Why this helper exists: route handlers that call
// `litecode::config().rate_limit` from inside the per-request
// critical section will trigger the lazy config bootstrap on the
// very first request. If JWT_SECRET isn't set in the process env,
// load_config() throws ConfigError, which propagates as a 500 (the
// wrap catches ApiException but not ConfigError). Pre-seeding the
// env once per test avoids that footgun.
//
// We use _putenv_s on Windows and setenv on POSIX — config.h's
// load_env_file() does the same dance, so this stays consistent.
void prime_config_for_test(const litecode::AppConfig& cfg) {
#if defined(_WIN32)
    _putenv_s("JWT_SECRET",            cfg.jwt.secret.c_str());
    _putenv_s("JWT_ISSUER",            cfg.jwt.issuer.c_str());
    _putenv_s("JWT_ACCESS_TTL_SECONDS",std::to_string(cfg.jwt.access_ttl_seconds).c_str());
    _putenv_s("JWT_REFRESH_TTL_SECONDS",
              std::to_string(cfg.jwt.refresh_ttl_seconds).c_str());
    _putenv_s("RATE_LIMIT_REGISTER_PER_MIN",
              std::to_string(cfg.rate_limit.auth_register_per_minute_per_ip).c_str());
    _putenv_s("RATE_LIMIT_LOGIN_PER_MIN",
              std::to_string(cfg.rate_limit.auth_login_per_minute_per_ip).c_str());
    _putenv_s("RATE_LIMIT_SUBMIT_PER_MIN",
              std::to_string(cfg.rate_limit.submission_per_minute_per_user).c_str());
    _putenv_s("RATE_LIMIT_ADMIN_WRITE_PER_MIN",
              std::to_string(cfg.rate_limit.admin_write_per_minute).c_str());
    _putenv_s("RATE_LIMIT_BULK_IMPORT_PER_HOUR",
              std::to_string(cfg.rate_limit.bulk_import_per_hour).c_str());
#else
    setenv("JWT_SECRET",            cfg.jwt.secret.c_str(), 1);
    setenv("JWT_ISSUER",            cfg.jwt.issuer.c_str(), 1);
    setenv("JWT_ACCESS_TTL_SECONDS",std::to_string(cfg.jwt.access_ttl_seconds).c_str(), 1);
    setenv("JWT_REFRESH_TTL_SECONDS",
           std::to_string(cfg.jwt.refresh_ttl_seconds).c_str(), 1);
    setenv("RATE_LIMIT_REGISTER_PER_MIN",
           std::to_string(cfg.rate_limit.auth_register_per_minute_per_ip).c_str(), 1);
    setenv("RATE_LIMIT_LOGIN_PER_MIN",
           std::to_string(cfg.rate_limit.auth_login_per_minute_per_ip).c_str(), 1);
    setenv("RATE_LIMIT_SUBMIT_PER_MIN",
           std::to_string(cfg.rate_limit.submission_per_minute_per_user).c_str(), 1);
    setenv("RATE_LIMIT_ADMIN_WRITE_PER_MIN",
           std::to_string(cfg.rate_limit.admin_write_per_minute).c_str(), 1);
    setenv("RATE_LIMIT_BULK_IMPORT_PER_HOUR",
           std::to_string(cfg.rate_limit.bulk_import_per_hour).c_str(), 1);
#endif
    // Bootstrap the singleton so litecode::config() never has to do
    // the load-from-env lazy path inside a request.
    litecode::reset_config_for_testing();
    litecode::init_config();
}

// RAII HttpServer handle. Mirrors test_auth_middleware.cpp.
struct ServerHandle {
    litecode::HttpServer*            server = nullptr;
    std::unique_ptr<httplib::Client> client;
    int                              port   = 0;

    ServerHandle(litecode::HttpServer* s, httplib::Client* c, int p)
        : server(s), client(c), port(p) {}
    ServerHandle(ServerHandle&& o) noexcept
        : server(o.server), client(std::move(o.client)), port(o.port) {
        o.server = nullptr; o.port = 0;
    }
    ServerHandle& operator=(ServerHandle&&) = delete;
    ServerHandle(const ServerHandle&)            = delete;
    ServerHandle& operator=(const ServerHandle&) = delete;
    // IMPORTANT: do NOT call server->stop() here. The HttpServer
    // destructor (in server.h) already calls stop() and joins the
    // listen thread, and cpp-httplib 0.18.3's Server::stop() does
    // NOT reset `is_running_` to false after closing the socket —
    // a second call then trips the `svr_sock_ != INVALID_SOCKET`
    // assertion inside stop(). Leaving the stop to the HttpServer
    // destructor (the only call site) avoids that race entirely.
    ~ServerHandle() = default;
};

ServerHandle start_server(litecode::HttpServer* server) {
    const int port = server->bind_any_port("127.0.0.1");
    EXPECT_GT(port, 0);
    EXPECT_TRUE(server->start(/*background=*/true));
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2, 0);
    client->set_read_timeout(5, 0);
    client->set_write_timeout(5, 0);
    client->set_keep_alive(false);
    return ServerHandle(server, client.release(), port);
}

class StdoutSilencer {
public:
    StdoutSilencer() { orig_ = std::cout.rdbuf(sink_.rdbuf()); }
    ~StdoutSilencer() { std::cout.rdbuf(orig_); }
private:
    std::stringstream  sink_;
    std::streambuf*     orig_ = nullptr;
};

// ────────────────────────────────────────────────────────────────────────────
//  extract_client_ip
// ────────────────────────────────────────────────────────────────────────────

TEST(ExtractClientIp, FallsBackToRemoteAddr) {
    auto req = make_req("203.0.113.7", "", "", "");
    EXPECT_EQ(litecode::extract_client_ip(req), "203.0.113.7");
}

TEST(ExtractClientIp, HonorsXForwardedForSingle) {
    auto req = make_req("127.0.0.1", "", "198.51.100.42", "");
    EXPECT_EQ(litecode::extract_client_ip(req), "198.51.100.42");
}

TEST(ExtractClientIp, HonorsXForwardedForChainFirst) {
    // RFC 7239: the left-most address is the original client.
    auto req = make_req("127.0.0.1", "",
                        "198.51.100.42, 10.0.0.1, 10.0.0.2", "");
    EXPECT_EQ(litecode::extract_client_ip(req), "198.51.100.42");
}

TEST(ExtractClientIp, TrimsOwsAroundFirstXff) {
    auto req = make_req("127.0.0.1", "", "   198.51.100.42 , 10.0.0.1  ", "");
    EXPECT_EQ(litecode::extract_client_ip(req), "198.51.100.42");
}

TEST(ExtractClientIp, HonorsXRealIpWhenXffAbsent) {
    auto req = make_req("127.0.0.1", "", "", "198.51.100.99");
    EXPECT_EQ(litecode::extract_client_ip(req), "198.51.100.99");
}

TEST(ExtractClientIp, XffWinsOverXRealIp) {
    auto req = make_req("127.0.0.1", "", "198.51.100.1", "198.51.100.2");
    EXPECT_EQ(litecode::extract_client_ip(req), "198.51.100.1");
}

TEST(ExtractClientIp, EmptyForwardedHeadersFallBackToRemote) {
    auto req = make_req("10.0.0.5", "", "   ", "");
    EXPECT_EQ(litecode::extract_client_ip(req), "10.0.0.5");
}

// ────────────────────────────────────────────────────────────────────────────
//  detail::base64url_decode (white-box)
// ────────────────────────────────────────────────────────────────────────────

TEST(Base64UrlDecode, RoundTripsKnown) {
    // "Hello, world!" → base64url → "SGVsbG8sIHdvcmxkIQ"
    EXPECT_EQ(litecode::detail::base64url_decode("SGVsbG8sIHdvcmxkIQ"),
              "Hello, world!");
}

TEST(Base64UrlDecode, RoundTripsTestEncoder) {
    // Round-trip the test helper's encoder against the middleware's
    // decoder so we know the two agree on the alphabet and bit
    // ordering. This is what HappyPathReturnsSub implicitly exercises;
    // a focused test makes a regression easier to localize.
    auto encode = [](const std::string& s) {
        static const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : s) {
            val = (val << 8) | c; valb += 8;
            while (valb >= 0) {
                out.push_back(alpha[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(alpha[((val << 8) >> (valb + 8)) & 0x3F]);
        return out;
    };
    const std::string in = R"({"sub":"42","username":"alice","role":"user"})";
    const std::string enc = encode(in);
    const std::string dec = litecode::detail::base64url_decode(enc);
    EXPECT_EQ(dec, in) << "encoded: " << enc;
}

TEST(Base64UrlDecode, RoundTripsJsonPayload) {
    // Same idea but using a multi-segment string that exercises
    // both partial-byte boundaries (3 byte → 4 char group, 1 byte
    // tail → 2 char group + '=' padding). The test helper's encoder
    // emits no padding, the middleware's decoder must add it before
    // looking up values.
    auto encode = [](const std::string& s) {
        static const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : s) {
            val = (val << 8) | c; valb += 8;
            while (valb >= 0) {
                out.push_back(alpha[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(alpha[((val << 8) >> (valb + 8)) & 0x3F]);
        return out;
    };
    for (const std::string& s : {
             std::string("a"),
             std::string("ab"),
             std::string("abc"),
             std::string("abcd"),
             std::string("hello"),
             std::string("Hello, world!"),
             std::string(40, 'x'),
         }) {
        EXPECT_EQ(litecode::detail::base64url_decode(encode(s)), s)
            << "len=" << s.size();
    }
}

TEST(Base64UrlDecode, RejectsNonAlphabet) {
    EXPECT_EQ(litecode::detail::base64url_decode("not base64!"), "");
    EXPECT_EQ(litecode::detail::base64url_decode("!!!!"),         "");
}

TEST(Base64UrlDecode, EmptyReturnsEmpty) {
    EXPECT_EQ(litecode::detail::base64url_decode(""), "");
}

// ────────────────────────────────────────────────────────────────────────────
//  try_extract_jwt_subject
// ────────────────────────────────────────────────────────────────────────────

namespace {

// Build a hand-rolled JWT (header.payload.signature) with the given
// payload (JSON object). Signature segment is opaque — we never verify.
// base64url-encode without padding.
std::string make_unsigned_jwt(const nlohmann::json& payload) {
    auto b64url = [](const std::string& s) {
        static const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : s) {
            val = (val << 8) | c; valb += 8;
            while (valb >= 0) {
                out.push_back(alpha[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(alpha[((val << 8) >> (valb + 8)) & 0x3F]);
        return out;
    };
    const std::string header  = R"({"alg":"HS256","typ":"JWT"})";
    return b64url(header) + "." + b64url(payload.dump()) + ".sig";
}

} // anonymous namespace

TEST(TryExtractJwtSubject, MissingHeaderReturnsEmpty) {
    auto req = make_req("1.2.3.4", "");
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

TEST(TryExtractJwtSubject, WrongSchemeReturnsEmpty) {
    auto req = make_req("1.2.3.4", "Basic dXNlcjpwYXNz");
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

TEST(TryExtractJwtSubject, MalformedTokenReturnsEmpty) {
    auto req = make_req("1.2.3.4", "Bearer not.a.jwt");
    // dot1 = 3, dot2 = 5 — payload "a" base64-decodes to a single
    // byte, which is not valid JSON, so subject is "".
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

TEST(TryExtractJwtSubject, TooFewSegmentsReturnsEmpty) {
    auto req = make_req("1.2.3.4", "Bearer onlyonepart");
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

TEST(TryExtractJwtSubject, EmptyPayloadReturnsEmpty) {
    auto req = make_req("1.2.3.4", "Bearer header..sig");
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

TEST(TryExtractJwtSubject, HappyPathReturnsSub) {
    const std::string tok = make_unsigned_jwt({
        {"sub",      "42"},
        {"username", "alice"},
        {"role",     "user"},
    });
    auto req = make_req("1.2.3.4", "Bearer " + tok);
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "42");
}

TEST(TryExtractJwtSubject, PayloadWithoutSubReturnsEmpty) {
    const std::string tok = make_unsigned_jwt({{"username", "alice"}});
    auto req = make_req("1.2.3.4", "Bearer " + tok);
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

TEST(TryExtractJwtSubject, NonStringSubReturnsEmpty) {
    const std::string tok = make_unsigned_jwt({{"sub", 42}});
    auto req = make_req("1.2.3.4", "Bearer " + tok);
    EXPECT_EQ(litecode::try_extract_jwt_subject(req), "");
}

// ────────────────────────────────────────────────────────────────────────────
//  RateLimiter — token bucket semantics
// ────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, AllowsUpToCapacity) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("rl.allow", 5, std::chrono::seconds(60));
    for (int i = 0; i < 5; ++i) {
        const auto d = rl.consume(q, "ip:1.1.1.1");
        EXPECT_TRUE(d.allowed) << "request #" << (i + 1) << " should be allowed";
        EXPECT_EQ(d.limit, 5);
        EXPECT_EQ(d.remaining, 4 - i);
    }
}

TEST(RateLimiter, RejectsBeyondCapacity) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("rl.reject", 3, std::chrono::seconds(60));
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rl.consume(q, "ip:2.2.2.2").allowed);
    }
    const auto d = rl.consume(q, "ip:2.2.2.2");
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.limit, 3);
    EXPECT_EQ(d.remaining, 0);
    EXPECT_GE(d.retry_after.count(), 1);
}

TEST(RateLimiter, RetryAfterReflectsTimeToOneToken) {
    litecode::RateLimiter rl;
    FakeClock clock;
    rl.set_clock(clock.as_clock());

    const auto q = make_test_quota("rl.retry", 60, std::chrono::seconds(60));
    // 1 token / second at capacity=60, window=60s.
    for (int i = 0; i < 60; ++i) ASSERT_TRUE(rl.consume(q, "ip:3.3.3.3").allowed);
    const auto d = rl.consume(q, "ip:3.3.3.3");
    EXPECT_FALSE(d.allowed);
    // Need ~1 full second to refill 1 token. Retry-After floors to
    // seconds with a ceiling so it should be exactly 1.
    EXPECT_EQ(d.retry_after.count(), 1);
}

TEST(RateLimiter, RefillsOverTime) {
    litecode::RateLimiter rl;
    FakeClock clock;
    rl.set_clock(clock.as_clock());

    const auto q = make_test_quota("rl.refill", 10, std::chrono::seconds(10));
    // Drain 10 tokens.
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(rl.consume(q, "ip:4.4.4.4").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:4.4.4.4").allowed);

    // 5s later = 5 tokens worth of refill (10 cap / 10s = 1 tok/s).
    clock.advance(std::chrono::seconds(5));
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rl.consume(q, "ip:4.4.4.4").allowed)
            << "refilled request #" << (i + 1);
    }
    // Bucket empty again.
    EXPECT_FALSE(rl.consume(q, "ip:4.4.4.4").allowed);

    // 10s later = full refill.
    clock.advance(std::chrono::seconds(10));
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(rl.consume(q, "ip:4.4.4.4").allowed)
            << "second-cycle request #" << (i + 1);
    }
}

TEST(RateLimiter, PartialRefillIsFractional) {
    litecode::RateLimiter rl;
    FakeClock clock;
    rl.set_clock(clock.as_clock());

    const auto q = make_test_quota("rl.partial", 10, std::chrono::seconds(10));
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(rl.consume(q, "ip:5.5.5.5").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:5.5.5.5").allowed);

    // 1s = 1 token worth. We should get exactly 1 new consume through.
    clock.advance(std::chrono::seconds(1));
    EXPECT_TRUE (rl.consume(q, "ip:5.5.5.5").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:5.5.5.5").allowed);
}

TEST(RateLimiter, DifferentKeysHaveIndependentBuckets) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("rl.keys", 2, std::chrono::seconds(60));
    ASSERT_TRUE(rl.consume(q, "ip:6.6.6.6").allowed);
    ASSERT_TRUE(rl.consume(q, "ip:6.6.6.6").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:6.6.6.6").allowed);
    // Different key → fresh bucket.
    EXPECT_TRUE(rl.consume(q, "ip:7.7.7.7").allowed);
    EXPECT_TRUE(rl.consume(q, "ip:7.7.7.7").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:7.7.7.7").allowed);
    // Original still denied.
    EXPECT_FALSE(rl.consume(q, "ip:6.6.6.6").allowed);
}

TEST(RateLimiter, DifferentQuotasDoNotShareBuckets) {
    litecode::RateLimiter rl;
    const auto q1 = make_test_quota("rl.qa", 1, std::chrono::seconds(60));
    const auto q2 = make_test_quota("rl.qb", 1, std::chrono::seconds(60));
    ASSERT_TRUE(rl.consume(q1, "ip:8.8.8.8").allowed);
    EXPECT_FALSE(rl.consume(q1, "ip:8.8.8.8").allowed);
    // Same key, different quota → independent bucket.
    EXPECT_TRUE(rl.consume(q2, "ip:8.8.8.8").allowed);
    EXPECT_FALSE(rl.consume(q2, "ip:8.8.8.8").allowed);
    // Original quota still drained.
    EXPECT_FALSE(rl.consume(q1, "ip:8.8.8.8").allowed);
}

TEST(RateLimiter, ClockGoingBackwardDoesNotGrantTokens) {
    litecode::RateLimiter rl;
    FakeClock clock;
    rl.set_clock(clock.as_clock());

    const auto q = make_test_quota("rl.back", 5, std::chrono::seconds(60));
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(rl.consume(q, "ip:9.9.9.9").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:9.9.9.9").allowed);

    // "Travel back in time" by 60s. The refill math computes a
    // negative `add` and must NOT grant tokens.
    clock.advance(std::chrono::seconds(-60));
    EXPECT_FALSE(rl.consume(q, "ip:9.9.9.9").allowed);
}

TEST(RateLimiter, PeekDoesNotConsume) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("rl.peek", 3, std::chrono::seconds(60));
    ASSERT_TRUE(rl.consume(q, "ip:10.10.10.10").allowed);
    ASSERT_TRUE(rl.consume(q, "ip:10.10.10.10").allowed);
    // Two consumed, one remaining. peek should see that.
    const auto d = rl.peek(q.name, "ip:10.10.10.10", q.capacity, q.window);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.limit, 3);
    EXPECT_EQ(d.remaining, 1);
    // And another consume still goes through.
    EXPECT_TRUE(rl.consume(q, "ip:10.10.10.10").allowed);
    EXPECT_FALSE(rl.consume(q, "ip:10.10.10.10").allowed);
}

TEST(RateLimiter, PeekOnUnknownKeyReturnsFull) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("rl.peeku", 4, std::chrono::seconds(60));
    const auto d = rl.peek(q.name, "ip:11.11.11.11", q.capacity, q.window);
    EXPECT_TRUE(d.allowed);
    EXPECT_EQ(d.remaining, 4);
}

TEST(RateLimiter, SizeReflectsTrackedKeys) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("rl.size", 5, std::chrono::seconds(60));
    EXPECT_EQ(rl.size(), 0u);
    rl.consume(q, "ip:a");
    rl.consume(q, "ip:b");
    rl.consume(q, "ip:b");  // same key — does NOT add a new entry
    EXPECT_EQ(rl.size(), 2u);
    rl.clear();
    EXPECT_EQ(rl.size(), 0u);
}

TEST(RateLimiter, CapacityChangeAtSameNameReinitializes) {
    // Operator edits RateLimitConfig between two calls. The bucket
    // for (rl.changecap, ip:x) was created at cap=2; next call uses
    // cap=5 with a new RateLimitQuota. We must re-init, not just
    // keep the old "drained" state.
    litecode::RateLimiter rl;
    const auto q2 = make_test_quota("rl.changecap", 2, std::chrono::seconds(60));
    ASSERT_TRUE(rl.consume(q2, "ip:12.12.12.12").allowed);
    ASSERT_TRUE(rl.consume(q2, "ip:12.12.12.12").allowed);
    EXPECT_FALSE(rl.consume(q2, "ip:12.12.12.12").allowed);

    const auto q5 = make_test_quota("rl.changecap", 5, std::chrono::seconds(60));
    // 5 fresh tokens under the new capacity.
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rl.consume(q5, "ip:12.12.12.12").allowed)
            << "post-change request #" << (i + 1);
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Quota factories
// ────────────────────────────────────────────────────────────────────────────

TEST(QuotaFactories, AuthRegisterFromConfig) {
    litecode::RateLimitConfig cfg;
    cfg.auth_register_per_minute_per_ip = 7;
    const auto q = litecode::auth_register_quota(cfg);
    EXPECT_EQ(q.name,      "auth.register");
    EXPECT_EQ(q.capacity,  7);
    EXPECT_EQ(q.window,    std::chrono::minutes(1));
    EXPECT_EQ(q.key_type,  litecode::RateLimitKeyType::ByIp);
}

TEST(QuotaFactories, AuthLoginFromConfig) {
    litecode::RateLimitConfig cfg;
    cfg.auth_login_per_minute_per_ip = 25;
    const auto q = litecode::auth_login_quota(cfg);
    EXPECT_EQ(q.name,     "auth.login");
    EXPECT_EQ(q.capacity, 25);
    EXPECT_EQ(q.window,   std::chrono::minutes(1));
    EXPECT_EQ(q.key_type, litecode::RateLimitKeyType::ByIp);
}

TEST(QuotaFactories, SubmissionFromConfig) {
    litecode::RateLimitConfig cfg;
    cfg.submission_per_minute_per_user = 50;
    const auto q = litecode::submission_quota(cfg);
    EXPECT_EQ(q.name,     "submission");
    EXPECT_EQ(q.capacity, 50);
    EXPECT_EQ(q.window,   std::chrono::minutes(1));
    EXPECT_EQ(q.key_type, litecode::RateLimitKeyType::ByUser);
}

TEST(QuotaFactories, AdminWriteFromConfig) {
    litecode::RateLimitConfig cfg;
    cfg.admin_write_per_minute = 100;
    const auto q = litecode::admin_write_quota(cfg);
    EXPECT_EQ(q.name,     "admin.write");
    EXPECT_EQ(q.capacity, 100);
    EXPECT_EQ(q.window,   std::chrono::minutes(1));
    EXPECT_EQ(q.key_type, litecode::RateLimitKeyType::ByUser);
}

TEST(QuotaFactories, BulkImportFromConfig) {
    litecode::RateLimitConfig cfg;
    cfg.bulk_import_per_hour = 3;
    const auto q = litecode::bulk_import_quota(cfg);
    EXPECT_EQ(q.name,     "admin.bulk_import");
    EXPECT_EQ(q.capacity, 3);
    EXPECT_EQ(q.window,   std::chrono::hours(1));
    EXPECT_EQ(q.key_type, litecode::RateLimitKeyType::ByUser);
}

// ────────────────────────────────────────────────────────────────────────────
//  consume_rate_limit
// ────────────────────────────────────────────────────────────────────────────

TEST(ConsumeRateLimit, SetsHeadersOnAllow) {
    litecode::RateLimiter rl;
    httplib::Request  req = make_req("10.0.0.1");
    httplib::Response res;
    litecode::consume_rate_limit(res, req, rl, make_test_quota());

    EXPECT_EQ(res.get_header_value("X-RateLimit-Limit"),     "5");
    EXPECT_EQ(res.get_header_value("X-RateLimit-Remaining"), "4");
    EXPECT_EQ(res.get_header_value("Retry-After"),           "");
}

TEST(ConsumeRateLimit, Throws429OnDeny) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("throw.q", 1, std::chrono::seconds(60));
    httplib::Request req = make_req("10.0.0.2");

    // First call: allowed.
    {
        httplib::Response res;
        EXPECT_NO_THROW(litecode::consume_rate_limit(res, req, rl, q));
        EXPECT_EQ(res.get_header_value("X-RateLimit-Limit"),     "1");
        EXPECT_EQ(res.get_header_value("X-RateLimit-Remaining"), "0");
    }
    // Second call: denied.
    httplib::Response res2;
    try {
        litecode::consume_rate_limit(res2, req, rl, q);
        FAIL() << "expected ApiException";
    } catch (const litecode::ApiException& e) {
        EXPECT_EQ(e.status(), 429);
        EXPECT_EQ(e.code(),   litecode::ErrorCode::RATE_LIMITED);
        EXPECT_NE(e.message().find("rate limit exceeded"), std::string::npos);
        EXPECT_NE(e.message().find("throw.q"),              std::string::npos);
    }
    EXPECT_EQ(res2.get_header_value("X-RateLimit-Limit"),     "1");
    EXPECT_EQ(res2.get_header_value("X-RateLimit-Remaining"), "0");
    EXPECT_FALSE(res2.get_header_value("Retry-After").empty());
    EXPECT_GE(std::stoi(res2.get_header_value("Retry-After")), 1);
}

TEST(ConsumeRateLimit, DetailsContainQuotaAndRetryAfter) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("det.q", 1, std::chrono::seconds(60));
    httplib::Request req = make_req("10.0.0.3");
    {
        httplib::Response res;
        litecode::consume_rate_limit(res, req, rl, q);
    }
    httplib::Response res;
    try {
        litecode::consume_rate_limit(res, req, rl, q);
    } catch (const litecode::ApiException& e) {
        ASSERT_TRUE(e.details().is_object());
        EXPECT_EQ(e.details().value("quota", std::string{}), "det.q");
        EXPECT_EQ(e.details().value("limit", 0),              1);
        EXPECT_GE(e.details().value("retry_after_s", 0),       1);
    }
}

TEST(ConsumeRateLimit, ByUserFallsBackToIpWhenNoAuthHeader) {
    litecode::RateLimiter rl;
    const auto q = litecode::RateLimitQuota{
        "sub.q", 1, std::chrono::seconds(60),
        litecode::RateLimitKeyType::ByUser,
    };
    httplib::Request req = make_req("10.0.0.4");  // no Authorization
    httplib::Response res;
    EXPECT_NO_THROW(litecode::consume_rate_limit(res, req, rl, q));
    // Second call: same IP — should be denied (fell back to IP keying).
    httplib::Response res2;
    EXPECT_THROW(litecode::consume_rate_limit(res2, req, rl, q),
                 litecode::ApiException);
}

TEST(ConsumeRateLimit, ByUserKeysOnSubject) {
    litecode::RateLimiter rl;
    const auto q = litecode::RateLimitQuota{
        "sub.q2", 1, std::chrono::seconds(60),
        litecode::RateLimitKeyType::ByUser,
    };
    const std::string tok_alice = make_unsigned_jwt({{"sub", "alice"}});
    const std::string tok_bob   = make_unsigned_jwt({{"sub", "bob"}});

    auto req_alice = make_req("10.0.0.5", "Bearer " + tok_alice);
    auto req_bob   = make_req("10.0.0.5", "Bearer " + tok_bob);

    httplib::Response res;
    EXPECT_NO_THROW(litecode::consume_rate_limit(res, req_alice, rl, q));
    // Same IP, different user → fresh bucket.
    EXPECT_NO_THROW(litecode::consume_rate_limit(res, req_bob, rl, q));

    // Now each has used their one allowed request; same user → deny.
    httplib::Response res2;
    EXPECT_THROW(litecode::consume_rate_limit(res2, req_alice, rl, q),
                 litecode::ApiException);
    EXPECT_THROW(litecode::consume_rate_limit(res2, req_bob,   rl, q),
                 litecode::ApiException);
}

TEST(ConsumeRateLimit, ByUserOrIpPrefersUserThenFallsBack) {
    litecode::RateLimiter rl;
    const auto q = litecode::RateLimitQuota{
        "mix.q", 1, std::chrono::seconds(60),
        litecode::RateLimitKeyType::ByUserOrIp,
    };
    // No auth → IP keying.
    auto req_anon = make_req("10.0.0.6");
    httplib::Response res;
    EXPECT_NO_THROW(litecode::consume_rate_limit(res, req_anon, rl, q));
    // Same IP, with a token → user keying, different bucket.
    const std::string tok = make_unsigned_jwt({{"sub", "u1"}});
    auto req_user = make_req("10.0.0.6", "Bearer " + tok);
    EXPECT_NO_THROW(litecode::consume_rate_limit(res, req_user, rl, q));
    // Each only allowed 1, so the second anon from the same IP is denied.
    httplib::Response res2;
    EXPECT_THROW(litecode::consume_rate_limit(res2, req_anon, rl, q),
                 litecode::ApiException);
    // And so is the second same-user request.
    EXPECT_THROW(litecode::consume_rate_limit(res2, req_user, rl, q),
                 litecode::ApiException);
}

TEST(ConsumeRateLimit, DifferentIpsHaveIndependentBuckets) {
    litecode::RateLimiter rl;
    const auto q = make_test_quota("ip.q", 1, std::chrono::seconds(60));
    httplib::Response res;
    EXPECT_NO_THROW(litecode::consume_rate_limit(
        res, make_req("10.0.0.7"), rl, q));
    // Different IP — fresh bucket.
    EXPECT_NO_THROW(litecode::consume_rate_limit(
        res, make_req("10.0.0.8"), rl, q));
    // Both should now be at their second consume — denied.
    httplib::Response res2;
    EXPECT_THROW(litecode::consume_rate_limit(
        res2, make_req("10.0.0.7"), rl, q), litecode::ApiException);
    EXPECT_THROW(litecode::consume_rate_limit(
        res2, make_req("10.0.0.8"), rl, q), litecode::ApiException);
}

// ────────────────────────────────────────────────────────────────────────────
//  End-to-end: in-process HttpServer (A26 acceptance: 5 OK, 6th 429)
//
//  The first E2E test below pins the wire protocol (status code,
//  JSON envelope, request_id, headers) that SPEC A26 actually
//  cares about. The next two are belt-and-braces coverage for the
//  multi-client and per-endpoint quota cases.
//
//  Test infrastructure note: ServerHandle's destructor does NOT
//  call server->stop() — that's the HttpServer's job in its own
//  destructor. Calling stop() twice trips a
//  `svr_sock_ != INVALID_SOCKET` assertion in cpp-httplib 0.18.3
//  because that version's `Server::stop()` does not reset
//  `is_running_` back to false. Same caveat applies to the
//  ServerHandle struct in test_auth_middleware.cpp.
// ────────────────────────────────────────────────────────────────────────────

namespace {

// Minimal register-handler that ONLY does the rate-limit check. We
// don't go through the real auth path because Phase 2's register
// route is in a separate ticket; the rate-limit wiring is what A26
// really exercises.
//
// We capture `rl_cfg` by value (NOT via litecode::config() at call
// time) so concurrent tests that reset_config_for_testing() don't
// race with the server's handler thread on the config singleton.
auto make_register_handler(litecode::RateLimiter&        limiter,
                           const litecode::RateLimitConfig& rl_cfg) {
    const litecode::RateLimitQuota quota =
        litecode::auth_register_quota(rl_cfg);
    return [&limiter, quota](const httplib::Request& req,
                            httplib::Response&       res) {
        litecode::consume_rate_limit(res, req, limiter, quota);
        litecode::send_created(res, {{"ok", true}});
    };
}

} // anonymous namespace

TEST(RateLimitE2E, A26FifthAllowedSixthReturns429) {
    StdoutSilencer silencer;
    litecode::AppConfig cfg = minimal_app_config();
    prime_config_for_test(cfg);
    litecode::HttpServer  s(cfg.server, cfg.cors);
    litecode::RateLimiter limiter;
    s.post("/api/v1/auth/register",
           make_register_handler(limiter, cfg.rate_limit));

    auto h = start_server(&s);

    for (int i = 1; i <= 5; ++i) {
        auto r = h.client->Post("/api/v1/auth/register",
                                R"({"username":"x"})",
                                "application/json");
        ASSERT_TRUE(r) << "POST #" << i << " failed: " << r.error();
        EXPECT_EQ(r->status, 201) << "request #" << i;
        EXPECT_EQ(r->get_header_value("X-RateLimit-Limit"),     "5");
        // First request: 4 remaining. Then 3, 2, 1, 0.
        EXPECT_EQ(r->get_header_value("X-RateLimit-Remaining"),
                  std::to_string(5 - i));
        EXPECT_EQ(r->get_header_value("Content-Type"),
                  "application/json; charset=utf-8");
    }
    auto r6 = h.client->Post("/api/v1/auth/register",
                             R"({"username":"x"})",
                             "application/json");
    ASSERT_TRUE(r6) << "POST #6 failed: " << r6.error();
    EXPECT_EQ(r6->status, 429);

    // Unified error envelope (SPEC §5.7).
    const auto body = nlohmann::json::parse(r6->body);
    EXPECT_EQ(body["code"], "RATE_LIMITED");
    EXPECT_FALSE(body["message"].get<std::string>().empty());
    EXPECT_TRUE (body.contains("request_id"));
    EXPECT_EQ   (body["request_id"], r6->get_header_value("X-Request-Id"));
    EXPECT_EQ   (body["details"].value("quota", std::string{}),
                 "auth.register");
    EXPECT_GE   (body["details"].value("retry_after_s", 0), 1);

    // Retry-After header is set on the wire.
    const auto ra = r6->get_header_value("Retry-After");
    EXPECT_FALSE(ra.empty());
    EXPECT_GE   (std::stoi(ra), 1);

    // X-RateLimit-* headers are also set on the 429 response.
    EXPECT_EQ(r6->get_header_value("X-RateLimit-Limit"),     "5");
    EXPECT_EQ(r6->get_header_value("X-RateLimit-Remaining"), "0");
}

TEST(RateLimitE2E, DifferentClientsAreIndependent) {
    // Two clients (different X-Forwarded-For) hit the same gated
    // endpoint; each gets their own 5/min budget because the
    // middleware honors XFF.
    //
    // Implementation note: we use httplib::Client::set_default_headers
    // to install X-Forwarded-For rather than the 4-arg
    // Post(path, headers, body, content_type) overload, which appears
    // to interact badly with cpp-httplib's internal state in this
    // test fixture (a downstream stop() assertion fires when the
    // client is destructed). set_default_headers is a documented
    // escape hatch that produces the same wire request.
    StdoutSilencer silencer;
    litecode::AppConfig cfg = minimal_app_config();
    prime_config_for_test(cfg);
    litecode::HttpServer  s(cfg.server, cfg.cors);
    litecode::RateLimiter limiter;
    s.post("/api/v1/auth/register",
           make_register_handler(limiter, cfg.rate_limit));
    auto h = start_server(&s);

    // Client A: no XFF (uses req.remote_addr = 127.0.0.1).
    for (int i = 1; i <= 5; ++i) {
        auto r = h.client->Post("/api/v1/auth/register",
                                R"({"u":"x"})", "application/json");
        ASSERT_TRUE(r) << "POST #" << i << " failed: " << r.error();
        EXPECT_EQ(r->status, 201) << "request #" << i;
    }
    auto a6 = h.client->Post("/api/v1/auth/register",
                             R"({"u":"x"})", "application/json");
    ASSERT_TRUE(a6) << "POST #6 failed: " << a6.error();
    EXPECT_EQ(a6->status, 429);

    // Client B: install X-Forwarded-For via the Client's default
    // headers. The middleware should key on the new IP and grant a
    // fresh 5.
    h.client->set_default_headers({{"X-Forwarded-For", "198.51.100.42"}});
    for (int i = 1; i <= 5; ++i) {
        auto r = h.client->Post("/api/v1/auth/register",
                                R"({"u":"x"})", "application/json");
        ASSERT_TRUE(r) << "client B POST #" << i << ": " << r.error();
        EXPECT_EQ(r->status, 201) << "client B request #" << i;
    }
    auto b6 = h.client->Post("/api/v1/auth/register",
                             R"({"u":"x"})", "application/json");
    ASSERT_TRUE(b6);
    EXPECT_EQ(b6->status, 429);
}

TEST(RateLimitE2E, SuccessfulResponseCarriesRateLimitHeaders) {
    // SPEC §5.7-ish: every gated response carries X-RateLimit-* even
    // when allowed, so well-behaved clients can back off proactively.
    StdoutSilencer silencer;
    litecode::AppConfig cfg = minimal_app_config();
    prime_config_for_test(cfg);
    litecode::HttpServer  s(cfg.server, cfg.cors);
    litecode::RateLimiter limiter;
    s.post("/api/v1/auth/register",
           make_register_handler(limiter, cfg.rate_limit));
    auto h = start_server(&s);
    h.client->set_default_headers({{"X-Forwarded-For", "9.9.9.9"}});

    auto r = h.client->Post("/api/v1/auth/register",
                            R"({"u":"x"})", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 201);
    EXPECT_EQ(r->get_header_value("X-RateLimit-Limit"),     "5");
    EXPECT_EQ(r->get_header_value("X-RateLimit-Remaining"), "4");
    EXPECT_EQ(r->get_header_value("Content-Type"),
              "application/json; charset=utf-8");
}

} // anonymous namespace
