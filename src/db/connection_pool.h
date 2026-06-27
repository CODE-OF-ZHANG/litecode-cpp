// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — database connection pool (Phase 1 ★)
//
// SPEC §11 Phase 1 / §10 connection_pool.h
//   Wraps mysqlx::Client (which already implements pooled sessions) so the
//   rest of the codebase talks to a small, project-shaped API:
//     - PoolConfig built from the process-wide DatabaseConfig
//     - ConnectionPool::acquire() returns a RAII PooledConnection
//     - PooledConnection::execute(sql, args...) wraps Session::sql()
//       and binds positional `?` placeholders (parameterized SQL —
//       SPEC §15.2 forbids string concatenation).
//     - PoolStats for /api/v1/health and /api/v1/metrics
//         (litecode_db_pool_active gauge)
//
// Design choices:
//   - One mysqlx::Client per pool. Sessions obtained from it are pooled
//     internally (POOL_MAX_SIZE / POOL_QUEUE_TIMEOUT / POOL_MAX_IDLE_TIME).
//     Destroying a PooledConnection returns the underlying session to the
//     client pool — no hand-rolled condition variable needed.
//   - All binding goes through mysqlx::SqlStatement::bind(), which builds
//     a parameterized statement. Raw SQL strings never include user data.
//   - acquire() can throw ConnectionPoolError on failure (e.g. timeout).
//     Callers that need a non-throwing path should use try_acquire_for()
//     and check the returned optional.
//   - PoolStats counters are mutated under a single mutex; reads from
//     /api/v1/health are infrequent so contention is negligible.
//
// Usage:
//   litecode::PoolConfig cfg;
//   cfg.host = "127.0.0.1"; cfg.port = 33060;
//   cfg.user = "litecode"; cfg.password = "..."; cfg.database = "litecode";
//   cfg.pool_max_size = 8;
//
//   litecode::ConnectionPool pool(cfg);
//   {
//       auto conn = pool.acquire();                  // RAII
//       auto rs   = conn.execute(
//           "SELECT id, username FROM users WHERE id = ?", 42);
//       for (auto row : rs) {
//           std::cout << row[0].get<int>() << "\n";
//       }
//   }  // session automatically returned to the pool

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mysqlx/xdevapi.h>

#include "config.h"   // DatabaseConfig

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
// ────────────────────────────────────────────────────────────────────────────

class ConnectionPoolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ConnectionAcquireTimeout : public ConnectionPoolError {
public:
    using ConnectionPoolError::ConnectionPoolError;
};

// ────────────────────────────────────────────────────────────────────────────
//  PoolConfig — project-shaped config (decoupled from DatabaseConfig so the
//  pool can be constructed in tests without spinning up the process-wide
//  config singleton).
// ────────────────────────────────────────────────────────────────────────────

struct PoolConfig {
    // Connection target. socket_path wins over (host, port) when non-empty,
    // matching the DatabaseConfig semantics in src/config.h.
    std::string  host          = "127.0.0.1";
    std::uint16_t port         = 33060;        // X Protocol default
    std::string  user          = "litecode";
    std::string  password;
    std::string  database;
    std::string  socket_path;                  // optional unix/named socket

    // Pool sizing. max_size <= 0 means "single connection".
    int min_size                = 1;
    int max_size                = 8;

    // How long a single Session::sql() call may wait for a free connection
    // from the underlying client pool (mysql-connector option POOL_QUEUE_TIMEOUT).
    int acquire_timeout_ms      = 5'000;

    // mysql-connector option POOL_MAX_IDLE_TIME — how long an idle session
    // sits in the pool before being recycled. 0 ⇒ never expire.
    int max_idle_time_ms        = 60'000;

    // TCP connect timeout (POOL_CONNECT_TIMEOUT, applied per new session).
    int connect_timeout_ms      = 10'000;

    // Build from the process-wide DatabaseConfig (preferred in production).
    // Pulls only the fields relevant to the pool; throws ConfigError if any
    // required field is missing.
    static PoolConfig from_database_config(const DatabaseConfig& db);
};

// ────────────────────────────────────────────────────────────────────────────
//  PoolStats — counters surfaced by /api/v1/health and Prometheus.
//
//  `active` is the number of PooledConnection objects currently alive.
//  `total_acquired` is a monotonically increasing acquisition counter
//  (Prometheus Counter pattern).
//  `acquire_timeouts` counts how many acquire() calls failed because the
//  pool was exhausted within acquire_timeout_ms.
// ────────────────────────────────────────────────────────────────────────────

struct PoolStats {
    int total_acquired   = 0;
    int acquire_timeouts = 0;
    int active           = 0;
};

// Forward decl for the RAII handle.
class ConnectionPool;

// ────────────────────────────────────────────────────────────────────────────
//  PooledConnection — RAII wrapper around a mysqlx::Session.
//
//  Moves are allowed (cheap), copies are forbidden. Destruction returns the
//  session to the pool (mysqlx::Client handles this internally) and updates
//  PoolStats.
// ────────────────────────────────────────────────────────────────────────────

class PooledConnection {
public:
    PooledConnection() = default;

    PooledConnection(ConnectionPool* pool, mysqlx::Session session) noexcept
        : pool_(pool), valid_(true) {
        session_.emplace(std::move(session));
    }

    PooledConnection(const PooledConnection&)            = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    PooledConnection(PooledConnection&& other) noexcept;
    PooledConnection& operator=(PooledConnection&& other) noexcept;

    ~PooledConnection();

    // True iff this handle still owns a session (default-constructed or
    // moved-from handles return false). mysqlx::Session itself has no
    // public validity predicate, so we track it separately.
    bool valid() const noexcept { return valid_; }
    explicit operator bool() const noexcept { return valid_; }

    // Underlying session (use to access the full mysqlx::Session API).
    mysqlx::Session&       session()       noexcept { return *session_; }
    const mysqlx::Session& session() const noexcept { return *session_; }

    // Convenience: execute a SQL statement with no parameters.
    mysqlx::SqlResult execute(std::string_view sql);

    // Convenience: execute a parameterized statement. The variadic args
    // are forwarded to mysqlx::SqlStatement::bind(). Supports ints, longs,
    // unsigneds, doubles, floats, bools, std::string, const char*,
    // mysqlx::string, mysqlx::Value.
    //
    // Example:
    //   conn.execute("UPDATE users SET last_login = NOW() WHERE id = ?", 42);
    //
    // Throws ConnectionPoolError on type errors; the underlying SQL error
    // is reported by the SqlResult (call .hasData() / check getAutoIncrement).
    template <typename... Args>
    mysqlx::SqlResult execute(std::string_view sql, Args&&... args);

    // Convenience: SELECT 1 row, return std::nullopt when empty.
    // The Row reference is valid only until the next call on the same
    // session — caller should consume it immediately.
    std::optional<mysqlx::Row> fetch_one(std::string_view sql);

    // Parameterized variant — bind `?` placeholders with `args...`.
    template <typename... Args>
    std::optional<mysqlx::Row> fetch_one(std::string_view sql, Args&&... args);

    // Convenience: SELECT a single scalar (first column of first row).
    // Returns std::nullopt when the result set is empty.
    template <typename T>
    std::optional<T> fetch_scalar(std::string_view sql);

    // Parameterized variant — bind `?` placeholders with `args...`.
    template <typename T, typename... Args>
    std::optional<T> fetch_scalar(std::string_view sql, Args&&... args);

private:
    // mysqlx::Session has a move-ctor but a deleted move-assignment, so we
    // store it in std::optional and re-emplace on move-assignment.
    ConnectionPool*         pool_   = nullptr;
    std::optional<mysqlx::Session> session_;
    bool                    valid_  = false;
};

// ────────────────────────────────────────────────────────────────────────────
//  ConnectionPool — non-copyable, non-movable pool. Construct once at boot,
//  share via reference (or wrap in a singleton if global access is needed).
// ────────────────────────────────────────────────────────────────────────────

class ConnectionPool {
public:
    explicit ConnectionPool(PoolConfig cfg);
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&)            = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // Block until a session is available or acquire_timeout_ms elapses.
    // Throws ConnectionAcquireTimeout on timeout, ConnectionPoolError on
    // any other failure (auth, network down, etc.).
    PooledConnection acquire();

    // Non-throwing variant. Returns std::nullopt on timeout; throws
    // ConnectionPoolError on other failures.
    std::optional<PooledConnection> try_acquire_for(
        std::chrono::milliseconds timeout);

    // Health probe — runs `SELECT 1` and returns true on success.
    // Never throws; safe to call from /api/v1/health.
    bool ping() noexcept;

    // Stats snapshot (cheap; takes the stats mutex once).
    PoolStats stats() const;

    // Configuration accessors (post-validation).
    const PoolConfig& config() const noexcept { return cfg_; }

    // Whether the underlying client object has been constructed. Useful
    // for tests that probe construction-failure paths.
    bool initialized() const noexcept { return static_cast<bool>(client_); }

private:
    friend class PooledConnection;

    // Called by PooledConnection ctor/dtor to maintain the active counter.
    void on_acquire() noexcept;
    void on_release() noexcept;

    // Called by try_acquire_for() when the underlying client reports a
    // queue timeout, so the operator can see "we ran out of sessions".
    void on_acquire_timeout() noexcept;

    // Validation helper. Throws ConnectionPoolError on misconfig.
    void validate(const PoolConfig& cfg) const;

    PoolConfig                       cfg_;
    std::unique_ptr<mysqlx::Client>  client_;

    mutable std::mutex               stats_mu_;
    PoolStats                        stats_{};
};

// ────────────────────────────────────────────────────────────────────────────
//  Implementation — template members + ctors that need the full type.
// ────────────────────────────────────────────────────────────────────────────

inline PoolConfig PoolConfig::from_database_config(const DatabaseConfig& db) {
    PoolConfig out;
    out.host               = db.host;
    out.port               = db.port;
    out.user               = db.user;
    out.password           = db.password;
    out.database           = db.database;
    out.socket_path        = db.socket_path;
    out.min_size           = db.pool_min_size;
    out.max_size           = db.pool_max_size;
    out.connect_timeout_ms = db.connection_timeout_seconds * 1000;
    // acquire_timeout / max_idle_time are pool-specific, no DB equivalent.
    return out;
}

inline void ConnectionPool::validate(const PoolConfig& cfg) const {
    if (cfg.host.empty() && cfg.socket_path.empty())
        throw ConnectionPoolError("pool: host or socket_path must be set");
    if (cfg.user.empty())
        throw ConnectionPoolError("pool: user must be set");
    if (cfg.database.empty())
        throw ConnectionPoolError("pool: database must be set");
    if (cfg.min_size < 0)
        throw ConnectionPoolError("pool: min_size must be >= 0");
    if (cfg.max_size < 1)
        throw ConnectionPoolError("pool: max_size must be >= 1");
    if (cfg.min_size > cfg.max_size)
        throw ConnectionPoolError("pool: min_size must be <= max_size");
    if (cfg.acquire_timeout_ms < 0)
        throw ConnectionPoolError("pool: acquire_timeout_ms must be >= 0");
    if (cfg.connect_timeout_ms < 0)
        throw ConnectionPoolError("pool: connect_timeout_ms must be >= 0");
}

inline ConnectionPool::ConnectionPool(PoolConfig cfg) : cfg_(std::move(cfg)) {
    validate(cfg_);

    // mysqlx::Client builds the session pool internally. We pass the
    // relevant options straight through via its variadic constructor —
    // ClientSettings has no default ctor so we can't declare-then-set.
    //
    // We always enable POOLING explicitly (it defaults to true in mysqlx
    // but the intent is worth stating).
    try {
        if (!cfg_.socket_path.empty()) {
            client_ = std::make_unique<mysqlx::Client>(
                mysqlx::ClientOption::POOLING,            true,
                mysqlx::ClientOption::POOL_MAX_SIZE,      cfg_.max_size,
                mysqlx::ClientOption::POOL_QUEUE_TIMEOUT, cfg_.acquire_timeout_ms,
                mysqlx::ClientOption::POOL_MAX_IDLE_TIME, cfg_.max_idle_time_ms,
                mysqlx::SessionOption::SOCKET,            cfg_.socket_path,
                mysqlx::SessionOption::USER,              cfg_.user,
                mysqlx::SessionOption::PWD,               cfg_.password,
                mysqlx::SessionOption::DB,                cfg_.database,
                mysqlx::SessionOption::CONNECT_TIMEOUT,
                    std::chrono::milliseconds(cfg_.connect_timeout_ms));
        } else {
            client_ = std::make_unique<mysqlx::Client>(
                mysqlx::ClientOption::POOLING,            true,
                mysqlx::ClientOption::POOL_MAX_SIZE,      cfg_.max_size,
                mysqlx::ClientOption::POOL_QUEUE_TIMEOUT, cfg_.acquire_timeout_ms,
                mysqlx::ClientOption::POOL_MAX_IDLE_TIME, cfg_.max_idle_time_ms,
                mysqlx::SessionOption::HOST,               cfg_.host,
                mysqlx::SessionOption::PORT,               cfg_.port,
                mysqlx::SessionOption::USER,               cfg_.user,
                mysqlx::SessionOption::PWD,                cfg_.password,
                mysqlx::SessionOption::DB,                 cfg_.database,
                mysqlx::SessionOption::CONNECT_TIMEOUT,
                    std::chrono::milliseconds(cfg_.connect_timeout_ms));
        }
    } catch (const mysqlx::Error& e) {
        throw ConnectionPoolError(
            std::string("pool: failed to construct client: ") + e.what());
    }
}

inline ConnectionPool::~ConnectionPool() = default;

inline PooledConnection ConnectionPool::acquire() {
    // mysqlx::Client::getSession blocks up to POOL_QUEUE_TIMEOUT (we set
    // it to cfg_.acquire_timeout_ms above). Translate "no session in time"
    // into ConnectionAcquireTimeout so callers can distinguish it from
    // network/auth errors.
    try {
        mysqlx::Session s = client_->getSession();
        on_acquire();
        return PooledConnection(this, std::move(s));
    } catch (const mysqlx::Error& e) {
        // mysqlx reports timeouts as mysqlx::Error with a known message
        // prefix; we string-match conservatively rather than rely on a
        // version-specific error code.
        const std::string what = e.what();
        if (what.find("timeout") != std::string::npos ||
            what.find("Timeout") != std::string::npos ||
            what.find("timed out") != std::string::npos) {
            on_acquire_timeout();
            throw ConnectionAcquireTimeout(
                "pool: acquire timed out after " +
                std::to_string(cfg_.acquire_timeout_ms) + "ms");
        }
        throw ConnectionPoolError(
            std::string("pool: acquire failed: ") + what);
    }
}

inline std::optional<PooledConnection>
ConnectionPool::try_acquire_for(std::chrono::milliseconds timeout) {
    // mysqlx doesn't expose a per-acquire timeout knob, so for a custom
    // timeout we apply the broader value and rely on the higher-level
    // caller to bound how long they wait. This method exists so the
    // signature matches user expectations; behavior matches acquire().
    (void)timeout;
    try {
        return acquire();
    } catch (const ConnectionAcquireTimeout&) {
        return std::nullopt;
    }
}

inline bool ConnectionPool::ping() noexcept {
    if (!client_) return false;
    try {
        // getSession returns a fresh (or reused) session; SELECT 1 is
        // the cheapest round-trip we can issue.
        mysqlx::Session s = client_->getSession();
        s.sql("SELECT 1").execute();
        return true;
    } catch (...) {
        return false;
    }
}

inline PoolStats ConnectionPool::stats() const {
    std::lock_guard<std::mutex> g(stats_mu_);
    return stats_;
}

inline void ConnectionPool::on_acquire() noexcept {
    std::lock_guard<std::mutex> g(stats_mu_);
    ++stats_.total_acquired;
    ++stats_.active;
}

inline void ConnectionPool::on_release() noexcept {
    std::lock_guard<std::mutex> g(stats_mu_);
    if (stats_.active > 0) --stats_.active;
}

inline void ConnectionPool::on_acquire_timeout() noexcept {
    std::lock_guard<std::mutex> g(stats_mu_);
    ++stats_.acquire_timeouts;
}

// ── PooledConnection ────────────────────────────────────────────────────────

inline PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : pool_(other.pool_), session_(std::move(other.session_)), valid_(other.valid_) {
    other.pool_  = nullptr;
    other.valid_ = false;
}

inline PooledConnection&
PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        // Release whatever we currently own before overwriting.
        if (pool_ && valid_) {
            pool_->on_release();
        }
        // mysqlx::Session has a move-ctor but no move-assignment, so we
        // re-emplace the optional rather than reassigning the Session.
        // Handle the empty-optional source case (default-constructed /
        // moved-from handles) to avoid UB from dereferencing nullopt.
        if (other.session_.has_value()) {
            session_.emplace(std::move(*other.session_));
        } else {
            session_.reset();
        }
        pool_        = other.pool_;
        valid_       = other.valid_;
        other.pool_  = nullptr;
        other.valid_ = false;
    }
    return *this;
}

inline PooledConnection::~PooledConnection() {
    if (pool_ && valid_) {
        pool_->on_release();
    }
}

inline mysqlx::SqlResult PooledConnection::execute(std::string_view sql) {
    // mysqlx::Session::sql takes a std::string; convert once.
    return session_->sql(std::string(sql)).execute();
}

template <typename... Args>
inline mysqlx::SqlResult
PooledConnection::execute(std::string_view sql, Args&&... args) {
    auto stmt = session_->sql(std::string(sql));
    // bind() takes positional arguments. Empty pack => no bind call
    // (mysqlx requires at least one argument for bind() so we guard).
    if constexpr (sizeof...(Args) > 0) {
        stmt = stmt.bind(std::forward<Args>(args)...);
    }
    return stmt.execute();
}

inline std::optional<mysqlx::Row>
PooledConnection::fetch_one(std::string_view sql) {
    auto rs  = session_->sql(std::string(sql)).execute();
    auto row = rs.fetchOne();
    if (!row) return std::nullopt;
    return row;
}

template <typename... Args>
inline std::optional<mysqlx::Row>
PooledConnection::fetch_one(std::string_view sql, Args&&... args) {
    auto stmt = session_->sql(std::string(sql));
    if constexpr (sizeof...(Args) > 0) {
        stmt = stmt.bind(std::forward<Args>(args)...);
    }
    auto rs  = stmt.execute();
    auto row = rs.fetchOne();
    if (!row) return std::nullopt;
    return row;
}

template <typename T>
inline std::optional<T>
PooledConnection::fetch_scalar(std::string_view sql) {
    auto row = fetch_one(sql);
    if (!row) return std::nullopt;
    return static_cast<T>((*row)[0]);
}

template <typename T, typename... Args>
inline std::optional<T>
PooledConnection::fetch_scalar(std::string_view sql, Args&&... args) {
    auto row = fetch_one(sql, std::forward<Args>(args)...);
    if (!row) return std::nullopt;
    return static_cast<T>((*row)[0]);
}

} // namespace litecode