// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — logging wrapper
//
// Phase 1 基础设施 / 日志封装 (SPEC §11 Phase 1 ★ / §15.6 / §16.6)
//
// Goals:
//   - Single header, no new third-party deps (we deliberately avoid spdlog
//     so the project's dependency surface stays small; JSON is hand-escaped).
//   - 5 levels: TRACE / DEBUG / INFO / WARN / ERROR (config-driven floor).
//   - Two output formats: JSON (default, machine-friendly, Docker-friendly)
//     and TEXT (human-friendly, useful for `tail -f` in dev).
//   - Sinks: stdout always; optional file with simple size-based rotation
//     (LOG_ROTATION_MAX_SIZE like "10M" + LOG_ROTATION_MAX_FILES = N).
//   - Every line carries the current thread's `request_id` if one is set,
//     so logs from concurrent HTTP handlers and judge workers can be
//     correlated. Per-thread storage uses `thread_local`; HTTP handlers
//     bracket request handling with `RequestIdScope`, judge workers stamp
//     a per-submission id.
//   - Thread-safe: every write is serialized under a single mutex so that
//     rotated lines from concurrent threads never tear.
//   - Configured from LoggingConfig (see src/config.h). init_logger() takes
//     a LoggingConfig; logger() returns the process-wide singleton, lazily
//     bootstrapped from litecode::config() so callers never have to check
//     "did main() init yet?".
//
// Usage:
//   int main() {
//       const auto& cfg = litecode::init_config(".env");
//       litecode::init_logger(cfg.logging);          // explicit boot
//       LOG_INFO("boot complete");
//   }
//
//   // Inside a request handler:
//   void handle(Req& req, Res& res) {
//       litecode::RequestIdScope rid(req.get_header_value("X-Request-Id"));
//       LOG_INFO("request received", {{"method", req.method},
//                                     {"path",   req.path}});
//   }
//
//   // A judge worker, no request_id context:
//   LOG_INFO("judge finished", {{"submission_id", "42"},
//                               {"status",        "ac"}});

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "config.h"   // LoggingConfig

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Section: LogLevel
// ────────────────────────────────────────────────────────────────────────────

enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
};

// Parse the env string ("TRACE"/"DEBUG"/.../ lowercase OK). Returns
// std::nullopt on invalid input — the caller is responsible for defaulting.
// Defined inline so callers don't need to depend on config.h internals.
inline std::optional<LogLevel> parse_log_level(std::string_view s) {
    std::string up;
    up.reserve(s.size());
    for (char c : s) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "TRACE") return LogLevel::TRACE;
    if (up == "DEBUG") return LogLevel::DEBUG;
    if (up == "INFO" ) return LogLevel::INFO;
    if (up == "WARN" ) return LogLevel::WARN;
    if (up == "ERROR") return LogLevel::ERROR;
    return std::nullopt;
}

inline const char* log_level_name(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "INFO";
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: time
// ────────────────────────────────────────────────────────────────────────────

// ISO-8601 UTC with millisecond precision, e.g. "2026-06-28T12:34:56.789Z".
// We avoid <chrono> heavy machinery so the path stays allocation-light.
inline std::string format_iso8601_utc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto t   = system_clock::to_time_t(tp);
    const auto ms  = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif

    char buf[32];
    // 2026-06-28T12:34:56.789Z → 24 chars + NUL
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: request_id thread-local
//
// We keep request_id per-thread (NOT per-logger-instance) because the same
// logger is shared across every HTTP handler thread and every judge worker
// thread — each must be able to stamp "its own" correlation id without
// racing on a shared field.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {
inline std::string& tls_request_id() {
    thread_local std::string id;
    return id;
}
inline bool& tls_request_id_present() {
    thread_local bool present = false;
    return present;
}
} // namespace detail

// Set / clear / read the request_id for the current thread. The accessor
// returns an empty string when none has been set; the writer checks the
// `present` flag separately so an explicit empty-string id (rare but legal)
// is preserved instead of being treated as "no id".
inline void set_thread_request_id(std::string id) {
    detail::tls_request_id() = std::move(id);
    detail::tls_request_id_present() = true;
}
inline void clear_thread_request_id() {
    detail::tls_request_id().clear();
    detail::tls_request_id_present() = false;
}
inline const std::string& current_request_id() {
    return detail::tls_request_id();
}
inline bool current_request_id_present() {
    return detail::tls_request_id_present();
}

// RAII helper. Sets the request_id on construction, restores the previous
// value on destruction. Use one per request handler / per judge task.
class RequestIdScope {
public:
    explicit RequestIdScope(std::string id) {
        prev_id_     = detail::tls_request_id();
        prev_present_= detail::tls_request_id_present();
        detail::tls_request_id() = std::move(id);
        detail::tls_request_id_present() = true;
    }
    ~RequestIdScope() {
        detail::tls_request_id() = std::move(prev_id_);
        detail::tls_request_id_present() = prev_present_;
    }
    RequestIdScope(const RequestIdScope&)            = delete;
    RequestIdScope& operator=(const RequestIdScope&) = delete;

private:
    std::string prev_id_;
    bool        prev_present_ = false;
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: size parser ("10M" / "10MB" / "10MiB" / "1024K" / plain int)
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::uintmax_t parse_size_string(std::string_view s, std::uintmax_t fallback_bytes) {
    // Trim whitespace.
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    if (s.empty()) return fallback_bytes;

    // Find where the numeric part ends.
    std::size_t i = 0;
    while (i < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
        ++i;
    }
    if (i == 0) return fallback_bytes;

    std::uintmax_t base = 0;
    try {
        // std::stoull on the numeric prefix is safe — anything past i is
        // unit suffix and gets ignored via substr(0, i).
        base = std::stoull(std::string(s.substr(0, i)));
    } catch (...) {
        return fallback_bytes;
    }

    std::string suffix;
    suffix.reserve(s.size() - i);
    for (std::size_t k = i; k < s.size(); ++k) {
        suffix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[k]))));
    }
    if (suffix.empty() || suffix == "b")         return base;
    if (suffix == "k" || suffix == "kb")         return base * 1024ULL;
    if (suffix == "m" || suffix == "mb")         return base * 1024ULL * 1024ULL;
    if (suffix == "g" || suffix == "gb")         return base * 1024ULL * 1024ULL * 1024ULL;
    if (suffix == "kib")                          return base * 1024ULL;
    if (suffix == "mib")                          return base * 1024ULL * 1024ULL;
    if (suffix == "gib")                          return base * 1024ULL * 1024ULL * 1024ULL;
    return fallback_bytes; // unknown suffix → don't break boot
}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
//  Section: Logger (the actual sink)
// ────────────────────────────────────────────────────────────────────────────

class Logger {
public:
    // Field key/value pairs attached to a single log line.
    using Field = std::pair<std::string_view, std::string_view>;

    explicit Logger(const LoggingConfig& cfg)
        : level_(static_cast<int>(parse_log_level(cfg.level).value_or(LogLevel::INFO))),
          json_format_(detail::upper(cfg.format) == "JSON"),
          include_request_id_(cfg.include_request_id),
          file_path_(cfg.file_path),
          max_file_size_(detail::parse_size_string(cfg.rotation_max_size,
                                                   10ULL * 1024 * 1024)),
          max_files_(cfg.rotation_max_files > 0 ? cfg.rotation_max_files : 1) {
        if (!file_path_.empty()) {
            file_.open(file_path_, std::ios::binary | std::ios::app);
            if (!file_.is_open()) {
                // Don't kill boot — degrade to stdout-only and tell the user.
                std::fprintf(stderr,
                             "[logger] WARN: cannot open log file '%s' (%s); falling back to stdout-only\n",
                             file_path_.c_str(), std::strerror(errno));
                file_path_.clear();
                current_file_size_ = 0;
                return;
            }
            // Seed the running size so rotation doesn't trigger on the very
            // first line after a restart that left a large file behind.
            std::error_code ec;
            current_file_size_ = std::filesystem::file_size(file_path_, ec);
            if (ec) current_file_size_ = 0;
        }
    }

    ~Logger() {
        std::lock_guard<std::mutex> g(io_mu_);
        if (file_.is_open()) {
            try { file_.flush(); } catch (...) {}
            file_.close();
        }
    }

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    // Level is stored atomically so set_level() (which is racy with
    // concurrent log() calls) is well-defined without taking the IO mutex.
    void set_level(LogLevel lvl) {
        level_.store(static_cast<int>(lvl), std::memory_order_relaxed);
    }
    LogLevel level() const {
        return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
    }

    // ── Core emit. Other helpers are thin wrappers. ────────────────────────
    void log(LogLevel lvl,
             std::string_view msg,
             std::initializer_list<Field> fields = {}) {
        // Fast pre-check on the level floor; cheap atomic load.
        if (static_cast<int>(lvl) < level_.load(std::memory_order_relaxed)) return;

        // Serialize the line outside the lock so heavy formatting
        // doesn't block other threads. The format step touches only
        // its own stack buffers.
        const bool have_rid = detail::tls_request_id_present();
        const std::string& rid = detail::tls_request_id();
        const bool want_rid = include_request_id_ && have_rid;
        const std::string ts = format_iso8601_utc(std::chrono::system_clock::now());
        const std::string line = format_line(lvl, ts, msg, fields, rid, want_rid);
        const std::size_t bytes = line.size() + 1; // +1 for '\n'

        std::lock_guard<std::mutex> g(io_mu_);
        // stdout first — always. docker logs / kubectl logs pick this up.
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);

        if (file_path_.empty() || !file_.is_open()) return;

        rotate_if_needed(bytes);
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.put('\n');
        file_.flush();   // forensics: don't lose lines on crash
        current_file_size_ += bytes;
    }

    // ── Level shortcuts (inlinable — cheap, no allocation) ──────────────────
    void trace(std::string_view m, std::initializer_list<Field> f = {}) { log(LogLevel::TRACE, m, f); }
    void debug(std::string_view m, std::initializer_list<Field> f = {}) { log(LogLevel::DEBUG, m, f); }
    void info (std::string_view m, std::initializer_list<Field> f = {}) { log(LogLevel::INFO,  m, f); }
    void warn (std::string_view m, std::initializer_list<Field> f = {}) { log(LogLevel::WARN,  m, f); }
    void error(std::string_view m, std::initializer_list<Field> f = {}) { log(LogLevel::ERROR, m, f); }

    // Test/diagnostic helpers.
    const std::string& file_path() const { return file_path_; }
    bool json_format() const { return json_format_; }

private:

    // ── JSON escaping. We only need to handle the cases that actually
    //    appear in our log lines: control chars, '\\', '"', and we keep
    //    raw UTF-8 bytes intact (RFC 8259 allows them inside strings). ──
    static void append_json_escaped(std::string& out, std::string_view in) {
        out.reserve(out.size() + in.size() + 2);
        for (unsigned char c : in) {
            switch (c) {
                case '"':  out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\b': out.append("\\b");  break;
                case '\f': out.append("\\f");  break;
                case '\n': out.append("\\n");  break;
                case '\r': out.append("\\r");  break;
                case '\t': out.append("\\t");  break;
                default:
                    if (c < 0x20) {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                        out.append(esc);
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
    }

    static void append_kv_json(std::string& out,
                               std::string_view key,
                               std::string_view val,
                               bool& needs_comma) {
        if (needs_comma) out.push_back(',');
        out.push_back('"');
        append_json_escaped(out, key);
        out.append("\":\"", 3);
        append_json_escaped(out, val);
        out.push_back('"');
        needs_comma = true;
    }

    std::string format_line(LogLevel lvl,
                            const std::string& ts,
                            std::string_view msg,
                            std::initializer_list<Field> fields,
                            const std::string& rid,
                            bool include_rid) const {
        if (json_format_) {
            std::string out;
            out.reserve(128 + msg.size());
            out.push_back('{');
            bool nc = false;
            append_kv_json(out, "ts",    ts,    nc);
            append_kv_json(out, "level", log_level_name(lvl), nc);
            append_kv_json(out, "logger","litecode", nc);
            append_kv_json(out, "msg",   msg,   nc);
            if (include_rid) append_kv_json(out, "request_id", rid, nc);
            for (const auto& f : fields) append_kv_json(out, f.first, f.second, nc);
            out.push_back('}');
            return out;
        }
        // TEXT: <ts> <LEVEL> [req=<id>] <msg> k1=v1 k2=v2
        std::string out;
        out.reserve(64 + msg.size());
        out.append(ts);
        out.push_back(' ');
        out.append(log_level_name(lvl));
        if (include_rid) {
            out.append(" [req=");
            out.append(rid);
            out.push_back(']');
        }
        out.push_back(' ');
        out.append(msg);
        for (const auto& f : fields) {
            out.push_back(' ');
            out.append(f.first);
            out.push_back('=');
            // For TEXT we keep it simple: quote if value contains a space,
            // otherwise write raw. Never multi-line values from this path.
            bool needs_quote = false;
            for (char c : f.second) {
                if (std::isspace(static_cast<unsigned char>(c))) { needs_quote = true; break; }
            }
            if (needs_quote) {
                out.push_back('"');
                // Simple double-quote escape (no \\n etc.) — adequate for
                // short structured values like error messages.
                for (char c : f.second) {
                    if (c == '"') out.push_back('\\');
                    out.push_back(c);
                }
                out.push_back('"');
            } else {
                out.append(f.second);
            }
        }
        return out;
    }

    void rotate_if_needed(std::size_t incoming_bytes) {
        if (current_file_size_ + incoming_bytes <= max_file_size_) return;
        if (max_files_ <= 1) {
            // Single-file mode: just truncate.
            file_.close();
            std::ofstream trunc(file_path_, std::ios::binary | std::ios::trunc);
            trunc.close();
            file_.open(file_path_, std::ios::binary | std::ios::app);
            current_file_size_ = 0;
            return;
        }
        file_.close();

        // Drop the oldest, shift the rest down by one.
        const std::string oldest = file_path_ + "." + std::to_string(max_files_ - 1);
        std::error_code ec;
        std::filesystem::remove(oldest, ec);

        for (int i = max_files_ - 2; i >= 1; --i) {
            const std::string from = file_path_ + "." + std::to_string(i);
            const std::string to   = file_path_ + "." + std::to_string(i + 1);
            std::filesystem::rename(from, to, ec);
        }
        const std::string first_rotated = file_path_ + ".1";
        std::filesystem::rename(file_path_, first_rotated, ec);

        file_.open(file_path_, std::ios::binary | std::ios::app);
        current_file_size_ = 0;
    }

    std::atomic<int>    level_;
    bool                json_format_;
    bool                include_request_id_;
    std::string         file_path_;
    std::uintmax_t      max_file_size_;
    int                 max_files_;
    std::ofstream       file_;
    std::uintmax_t      current_file_size_ = 0;

    std::mutex          io_mu_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: process-wide singleton (mirror config.h's pattern)
//
// Why a singleton: every request handler and judge worker thread uses the
// same logger; constructing/destructing one per thread would scatter
// configuration. We deliberately keep this lazy so tests can pre-seed
// env vars via ScopedEnv BEFORE the first log() call.
//
// `init_logger()` makes the configuration explicit and survives across
// reset_logger_for_testing() calls; `logger()` lazily bootstraps from the
// already-initialized LoggingConfig.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {
inline std::unique_ptr<Logger>& logger_slot() {
    static std::unique_ptr<Logger> slot;
    return slot;
}
inline std::mutex& logger_mutex() {
    static std::mutex m;
    return m;
}
} // namespace detail

inline Logger& init_logger(const LoggingConfig& cfg) {
    std::lock_guard<std::mutex> g(detail::logger_mutex());
    auto& slot = detail::logger_slot();
    // Replace if already present so re-init picks up new settings.
    slot = std::make_unique<Logger>(cfg);
    return *slot;
}

inline Logger& logger() {
    auto& slot = detail::logger_slot();
    if (slot) return *slot;

    std::lock_guard<std::mutex> g(detail::logger_mutex());
    if (!slot) {
        // Lazy bootstrap from the already-loaded config. If config()
        // itself wasn't initialized, its own lazy bootstrap fires here,
        // so this is safe to call from main() before init_config().
        slot = std::make_unique<Logger>(config().logging);
    }
    return *slot;
}

inline void reset_logger_for_testing() {
    std::lock_guard<std::mutex> g(detail::logger_mutex());
    detail::logger_slot().reset();
}

} // namespace litecode

// ────────────────────────────────────────────────────────────────────────────
//  Section: convenience macros
//
// We deliberately don't use ##__VA_ARGS__ tricks — keeping the call site
// honest means reviewers can always grep for "LOG_INFO(" to find every
// log statement. The two-arg form `LOG_INFO("msg", {{"k","v"}})` is fine
// in C++17 with brace elision.
// ────────────────────────────────────────────────────────────────────────────

#define LOG_TRACE(msg, ...) ::litecode::logger().trace((msg), ##__VA_ARGS__)
#define LOG_DEBUG(msg, ...) ::litecode::logger().debug((msg), ##__VA_ARGS__)
#define LOG_INFO(msg, ...)  ::litecode::logger().info ((msg), ##__VA_ARGS__)
#define LOG_WARN(msg, ...)  ::litecode::logger().warn ((msg), ##__VA_ARGS__)
#define LOG_ERROR(msg, ...) ::litecode::logger().error((msg), ##__VA_ARGS__)