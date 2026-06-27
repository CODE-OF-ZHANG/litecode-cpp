// tests/unit/test_logger.cpp
//
// Unit tests for src/logger.h. logger.h is header-only and pulls in only
// the standard library (no spdlog / no nlohmann/json), so the test binary
// links nothing beyond gtest_main.
//
// Coverage:
//   - parse_log_level: valid + lowercase + invalid
//   - parse_size_string: "10M"/"10MiB"/"1024K"/"1G"/plain int/garbage
//   - JSON format: ts / level / logger / msg / request_id + custom fields
//   - TEXT format: ts / level / [req=…] / msg / k=v
//   - JSON escaping: quotes, backslashes, control chars
//   - Level filtering: TRACE/DEBUG dropped when floor is WARN
//   - File rotation: max_size / max_files honored, oldest dropped
//   - File open failure: fall back to stdout-only, no throw
//   - set_level() round-trip + level() accessor
//   - RequestIdScope RAII restoration
//   - Thread-safe concurrent writes (no torn lines, all messages present)
//   - Singleton: init_logger + logger() + reset_logger_for_testing
//
// Each test creates a unique file under std::filesystem::temp_directory_path()
// and removes it (plus any rotated siblings) in its destructor.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>     // GetCurrentProcessId
#endif

#include "config.h"   // LoggingConfig
#include "logger.h"

namespace {

// ── Temp file helper ────────────────────────────────────────────────────────

class TempFile {
public:
    TempFile() {
        static std::atomic<int> counter{0};
        const int n = counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
        const int pid = ::_getpid();
#else
        const int pid = ::getpid();
#endif
        path_ = std::filesystem::temp_directory_path() /
                ("litecode_logger_" + std::to_string(pid) +
                 "_" + std::to_string(n) + ".log");
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        // Pre-remove rotated siblings from previous failed runs.
        for (int i = 1; i <= 10; ++i) {
            std::filesystem::remove(path_.string() + "." + std::to_string(i), ec);
        }
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        for (int i = 1; i <= 10; ++i) {
            std::filesystem::remove(path_.string() + "." + std::to_string(i), ec);
        }
    }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// ── Read whole file as string ───────────────────────────────────────────────

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

// ── Config builders ─────────────────────────────────────────────────────────

litecode::LoggingConfig make_cfg(const std::string& path,
                                 const std::string& fmt,
                                 const std::string& lvl = "TRACE",
                                 const std::string& max_size = "1K",
                                 int max_files = 3) {
    litecode::LoggingConfig c;
    c.level               = lvl;
    c.format              = fmt;
    c.file_path           = path;
    c.include_request_id  = true;
    c.rotation_max_size   = max_size;
    c.rotation_max_files  = max_files;
    return c;
}

// ── Per-test request_id reset ───────────────────────────────────────────────

struct RidReset {
    RidReset()  { litecode::clear_thread_request_id(); }
    ~RidReset() { litecode::clear_thread_request_id(); }
    RidReset(const RidReset&)            = delete;
    RidReset& operator=(const RidReset&) = delete;
};

// ── Tests: small helpers exposed in detail namespace ───────────────────────

TEST(LoggerParse, LogLevelRecognizedAndCaseInsensitive) {
    EXPECT_EQ(litecode::parse_log_level("INFO").value(),  litecode::LogLevel::INFO);
    EXPECT_EQ(litecode::parse_log_level("info").value(),  litecode::LogLevel::INFO);
    EXPECT_EQ(litecode::parse_log_level("Error").value(), litecode::LogLevel::ERROR);
    EXPECT_EQ(litecode::parse_log_level("trace").value(), litecode::LogLevel::TRACE);
    EXPECT_FALSE(litecode::parse_log_level("LOUD").has_value());
    EXPECT_FALSE(litecode::parse_log_level("").has_value());
}

TEST(LoggerParse, SizeString) {
    EXPECT_EQ(litecode::detail::parse_size_string("10M",  0), 10ULL * 1024 * 1024);
    EXPECT_EQ(litecode::detail::parse_size_string("10MB", 0), 10ULL * 1024 * 1024);
    EXPECT_EQ(litecode::detail::parse_size_string("10MiB",0), 10ULL * 1024 * 1024);
    EXPECT_EQ(litecode::detail::parse_size_string("1024K",0), 1024ULL * 1024);
    EXPECT_EQ(litecode::detail::parse_size_string("1G",   0), 1ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(litecode::detail::parse_size_string("512",  0), 512ULL);
    EXPECT_EQ(litecode::detail::parse_size_string("  256B  ", 0), 256ULL);
    // Garbage → fallback
    EXPECT_EQ(litecode::detail::parse_size_string("",     999), 999ULL);
    EXPECT_EQ(litecode::detail::parse_size_string("XYZ",  999), 999ULL);
    EXPECT_EQ(litecode::detail::parse_size_string("10XB", 999), 999ULL);  // unknown suffix
}

// ── Tests: format ───────────────────────────────────────────────────────────

TEST(LoggerFormat, JsonContainsExpectedFields) {
    RidReset _;
    TempFile tf;
    litecode::Logger lg(make_cfg(tf.path(), "JSON"));

    lg.info("user logged in", {{"user_id", "42"}});

    const std::string content = read_file(tf.path());
    EXPECT_NE(content.find("\"level\":\"INFO\""),        std::string::npos);
    EXPECT_NE(content.find("\"logger\":\"litecode\""),   std::string::npos);
    EXPECT_NE(content.find("\"msg\":\"user logged in\""),std::string::npos);
    EXPECT_NE(content.find("\"user_id\":\"42\""),        std::string::npos);
    EXPECT_NE(content.find("\"ts\":\""),                 std::string::npos);
    // No request_id set → must be absent
    EXPECT_EQ(content.find("\"request_id\""),            std::string::npos);
}

TEST(LoggerFormat, JsonAttachesRequestIdWhenSet) {
    RidReset _;
    TempFile tf;
    litecode::Logger lg(make_cfg(tf.path(), "JSON"));

    litecode::set_thread_request_id("abc-123");
    lg.info("with rid");
    litecode::clear_thread_request_id();
    lg.info("without rid");

    const std::string content = read_file(tf.path());
    EXPECT_NE(content.find("\"request_id\":\"abc-123\""), std::string::npos);

    // The second line must NOT carry request_id.
    const auto first_nl = content.find('\n');
    ASSERT_NE(first_nl, std::string::npos);
    const std::string second_line = content.substr(first_nl + 1);
    EXPECT_EQ(second_line.find("\"request_id\""), std::string::npos);
}

TEST(LoggerFormat, JsonEscapesControlChars) {
    RidReset _;
    TempFile tf;
    litecode::Logger lg(make_cfg(tf.path(), "JSON"));

    lg.info(R"(with "quote" and \ backslash and
newline and tab	here)");

    const std::string content = read_file(tf.path());
    EXPECT_NE(content.find("\\\"quote\\\""),    std::string::npos);
    EXPECT_NE(content.find("\\\\"),            std::string::npos);
    EXPECT_NE(content.find("\\n"),             std::string::npos);
    EXPECT_NE(content.find("\\t"),             std::string::npos);

    // Parse the single line as a JSON object: balanced braces + no raw
    // control chars between quotes.
    auto end = content.find_last_not_of("\r\n");
    ASSERT_NE(end, std::string::npos);
    const std::string line = content.substr(0, end + 1);

    int depth = 0;
    bool in_str = false;
    bool escape = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) { escape = false; continue; }
        if (in_str) {
            if (c == '\\') { escape = true; continue; }
            if (c == '"')  { in_str = false; continue; }
            // No raw \n / \t / control bytes may appear inside a string.
            EXPECT_GE(static_cast<unsigned char>(c), 0x20);
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            EXPECT_GE(depth, 0);
        }
    }
    EXPECT_EQ(depth, 0);
    EXPECT_FALSE(in_str);
}

TEST(LoggerFormat, TextStructure) {
    RidReset _;
    TempFile tf;
    litecode::Logger lg(make_cfg(tf.path(), "TEXT"));

    litecode::set_thread_request_id("rid-1");
    lg.info("hello", {{"k", "v"}, {"k2", "value with space"}});

    const std::string content = read_file(tf.path());
    EXPECT_NE(content.find("INFO"),       std::string::npos);
    EXPECT_NE(content.find("[req=rid-1]"),std::string::npos);
    EXPECT_NE(content.find("hello"),      std::string::npos);
    EXPECT_NE(content.find("k=v"),        std::string::npos);
    EXPECT_NE(content.find("k2=\"value with space\""), std::string::npos);
    EXPECT_NE(content.find('T'),          std::string::npos);  // ISO 8601 marker
    // TEXT must NOT use JSON braces
    EXPECT_EQ(content.find('{'),          std::string::npos);
}

TEST(LoggerFormat, IncludeRequestIdDisabled) {
    RidReset _;
    TempFile tf;
    litecode::LoggingConfig c = make_cfg(tf.path(), "JSON");
    c.include_request_id = false;
    litecode::Logger lg(c);

    litecode::set_thread_request_id("hidden-rid");
    lg.info("no rid wanted");

    const std::string content = read_file(tf.path());
    EXPECT_EQ(content.find("hidden-rid"), std::string::npos);
    EXPECT_EQ(content.find("request_id"), std::string::npos);
}

// ── Tests: level filtering ──────────────────────────────────────────────────

TEST(LoggerLevel, FiltersBelowFloor) {
    RidReset _;
    TempFile tf;
    litecode::Logger lg(make_cfg(tf.path(), "JSON", /*lvl=*/"WARN"));

    lg.trace("trace-msg");
    lg.debug("debug-msg");
    lg.info("info-msg");
    lg.warn("warn-msg");
    lg.error("error-msg");

    const std::string content = read_file(tf.path());
    EXPECT_EQ(content.find("trace-msg"), std::string::npos);
    EXPECT_EQ(content.find("debug-msg"), std::string::npos);
    EXPECT_EQ(content.find("info-msg"),  std::string::npos);
    EXPECT_NE(content.find("warn-msg"),  std::string::npos);
    EXPECT_NE(content.find("error-msg"), std::string::npos);
}

TEST(LoggerLevel, SetLevelRoundTrip) {
    litecode::LoggingConfig c;
    c.level    = "INFO";
    c.format   = "JSON";
    c.file_path = "";
    litecode::Logger lg(c);
    EXPECT_EQ(lg.level(), litecode::LogLevel::INFO);

    lg.set_level(litecode::LogLevel::ERROR);
    EXPECT_EQ(lg.level(), litecode::LogLevel::ERROR);

    lg.set_level(litecode::LogLevel::TRACE);
    EXPECT_EQ(lg.level(), litecode::LogLevel::TRACE);
}

// ── Tests: file rotation ────────────────────────────────────────────────────

TEST(LoggerFile, RotatesWhenSizeExceeded) {
    RidReset _;
    TempFile tf;
    litecode::LoggingConfig c = make_cfg(tf.path(), "JSON", "TRACE",
                                        /*max_size=*/"256", /*max_files=*/3);
    litecode::Logger lg(c);

    for (int i = 0; i < 50; ++i) {
        lg.info("payload payload payload payload payload payload payload payload",
                {{"i", std::to_string(i)}});
    }

    // After ≥3 rotations with max_files=3: current + .1 + .2 exist, .3 was dropped.
    EXPECT_TRUE (std::filesystem::exists(tf.path()));
    EXPECT_TRUE (std::filesystem::exists(tf.path() + ".1"));
    EXPECT_TRUE (std::filesystem::exists(tf.path() + ".2"));
    EXPECT_FALSE(std::filesystem::exists(tf.path() + ".3"));
}

TEST(LoggerFile, SingleFileModeTruncates) {
    RidReset _;
    TempFile tf;
    litecode::LoggingConfig c = make_cfg(tf.path(), "JSON", "TRACE",
                                        /*max_size=*/"256", /*max_files=*/1);
    litecode::Logger lg(c);

    for (int i = 0; i < 50; ++i) {
        lg.info("payload payload payload payload payload payload payload payload",
                {{"i", std::to_string(i)}});
    }

    // max_files=1 → no rotated siblings must ever appear.
    EXPECT_TRUE (std::filesystem::exists(tf.path()));
    EXPECT_FALSE(std::filesystem::exists(tf.path() + ".1"));
    EXPECT_FALSE(std::filesystem::exists(tf.path() + ".2"));
}

TEST(LoggerFile, UnwritablePathFallsBackToStdoutOnly) {
    litecode::LoggingConfig c;
    c.level              = "INFO";
    c.format             = "JSON";
    c.file_path          = "/no/such/dir/litecode_cannot_create.log";
    c.include_request_id = true;

    EXPECT_NO_THROW({
        litecode::Logger lg(c);
        EXPECT_TRUE(lg.file_path().empty());
        // Must not crash when emitting.
        lg.info("after fallback");
    });
}

// ── Tests: request_id RAII ──────────────────────────────────────────────────

TEST(LoggerRequestId, ScopeRestoresOuterValue) {
    RidReset _;
    litecode::set_thread_request_id("outer");

    {
        litecode::RequestIdScope inner("inner");
        EXPECT_EQ(litecode::current_request_id(), "inner");
        EXPECT_TRUE(litecode::current_request_id_present());
    }
    EXPECT_EQ(litecode::current_request_id(), "outer");
    EXPECT_TRUE(litecode::current_request_id_present());

    {
        // Empty-string id is still "present" — distinct from "absent".
        litecode::RequestIdScope empty("");
        EXPECT_TRUE(litecode::current_request_id_present());
        EXPECT_EQ(litecode::current_request_id(), "");
    }
    EXPECT_TRUE(litecode::current_request_id_present());
    EXPECT_EQ(litecode::current_request_id(), "outer");

    litecode::clear_thread_request_id();
}

TEST(LoggerRequestId, ScopeDoesNotLeakBetweenThreads) {
    litecode::set_thread_request_id("main-thread");
    std::atomic<bool> observed_other{false};
    std::string       other_value;

    std::thread t([&]() {
        // The worker thread must NOT see "main-thread" — request_id is
        // thread_local. The thread-local slot starts absent.
        if (litecode::current_request_id_present()) {
            observed_other = true;
            other_value    = litecode::current_request_id();
        }
        litecode::RequestIdScope rid("worker-thread");
        // ... and from here onward only "worker-thread" should be visible.
        EXPECT_EQ(litecode::current_request_id(), "worker-thread");
    });
    t.join();

    EXPECT_FALSE(observed_other);
    EXPECT_TRUE (other_value.empty());
    EXPECT_EQ   (litecode::current_request_id(), "main-thread");
    litecode::clear_thread_request_id();
}

// ── Tests: thread-safe concurrent writes ────────────────────────────────────

TEST(LoggerConcurrency, ParallelWritesDoNotTear) {
    RidReset _;
    TempFile tf;
    // Use a rotation size large enough that no rotation happens during the
    // test — otherwise we'd lose most of the 1600 lines to the rotate
    // window (max_files=3 caps total retained capacity at 3 * max_size).
    litecode::Logger lg(make_cfg(tf.path(), "JSON", "INFO",
                                 /*max_size=*/"10M", /*max_files=*/3));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            litecode::RequestIdScope rid("thread-" + std::to_string(t));
            for (int i = 0; i < kPerThread; ++i) {
                lg.info("concurrent line", {{"i", std::to_string(i)}});
            }
        });
    }
    for (auto& th : threads) th.join();

    const std::string content = read_file(tf.path());
    int lines = 0;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const auto end = content.find('\n', pos);
        std::string line = (end == std::string::npos)
            ? content.substr(pos)
            : content.substr(pos, end - pos);
        pos = (end == std::string::npos) ? content.size() : end + 1;
        if (line.empty()) continue;
        ++lines;

        ASSERT_FALSE(line.empty());
        EXPECT_EQ(line.front(), '{') << "line #" << lines << " not JSON";
        EXPECT_EQ(line.back(),  '}') << "line #" << lines << " not JSON";

        // Balanced braces, no unterminated strings inside.
        int depth = 0;
        bool in_str = false, escape = false;
        for (char c : line) {
            if (escape) { escape = false; continue; }
            if (in_str) {
                if (c == '\\') { escape = true; continue; }
                if (c == '"')  { in_str = false; continue; }
                continue;
            }
            if (c == '"')  { in_str = true; continue; }
            if (c == '{')  ++depth;
            else if (c == '}') { --depth; EXPECT_GE(depth, 0); }
        }
        EXPECT_EQ(depth, 0);
        EXPECT_FALSE(in_str);
    }
    EXPECT_EQ(lines, kThreads * kPerThread);
}

// ── Tests: singleton ────────────────────────────────────────────────────────

TEST(LoggerSingleton, InitReturnsSameInstance) {
    litecode::reset_logger_for_testing();

    litecode::LoggingConfig c;
    c.level    = "WARN";
    c.format   = "JSON";
    c.file_path = "";

    litecode::init_logger(c);
    litecode::Logger& lg = litecode::logger();
    EXPECT_EQ(lg.level(), litecode::LogLevel::WARN);

    // Re-init replaces the underlying Logger instance, so the previous
    // reference is dangling. Re-fetch via logger() to observe the new one.
    c.level = "ERROR";
    litecode::init_logger(c);
    litecode::Logger& lg2 = litecode::logger();
    EXPECT_EQ(lg2.level(), litecode::LogLevel::ERROR);

    litecode::reset_logger_for_testing();
}

TEST(LoggerSingleton, LazyBootstrapFromConfig) {
    litecode::reset_logger_for_testing();

    // The lazy path through logger() → config() → load_config() requires
    // JWT_SECRET to be set (or LITECODE_ALLOW_INSECURE_DEFAULTS=1). Stage
    // a valid 32-byte secret just for this test, then tear it down.
    const char* prev_secret = std::getenv("JWT_SECRET");
    const bool had_prev = (prev_secret != nullptr);
    const std::string prev_value = had_prev ? prev_secret : std::string{};
#if defined(_WIN32)
    _putenv_s("JWT_SECRET", "lazy_bootstrap_test_secret_32_bytes!");
#else
    setenv("JWT_SECRET", "lazy_bootstrap_test_secret_32_bytes!", 1);
#endif

    litecode::Logger& lg = litecode::logger();
    EXPECT_NO_THROW(lg.info("lazy bootstrap",
                            {{"source", "config().logging"}}));

    // Restore env so other tests don't pick up our staged secret.
#if defined(_WIN32)
    if (had_prev) _putenv_s("JWT_SECRET", prev_value.c_str());
    else          _putenv_s("JWT_SECRET", "");
#else
    if (had_prev) setenv("JWT_SECRET", prev_value.c_str(), 1);
    else          unsetenv("JWT_SECRET");
#endif
    litecode::reset_config_for_testing();
    litecode::reset_logger_for_testing();
}

} // namespace