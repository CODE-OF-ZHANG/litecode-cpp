// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Rate-limit middleware (Phase 2 ★)
//
// SPEC §5.1 / §5.2 / §5.3 / §5.5 / §11 Phase 2 / §15.2 / A26 acceptance:
//   - Per-endpoint quotas:
//       POST /api/v1/auth/register            5 / min / IP
//       POST /api/v1/auth/login               10 / min / IP
//       POST /api/v1/submissions              30 / min / user
//       POST/PUT/DELETE /api/v1/admin/...     30 / min / admin
//       POST /api/v1/admin/problems/import     5 / hour / admin
//   - Algorithm: token bucket (smooth refill, configurable burst == capacity).
//   - Storage: in-memory std::unordered_map keyed by (quota_name, client key).
//     MVP single-instance; SPEC §9 calls for Redis on multi-instance deploy.
//   - Wire protocol on deny:
//       HTTP 429 Too Many Requests
//       ErrorCode::RATE_LIMITED
//       Retry-After: <seconds>           (RFC 7231 §7.1.3)
//       X-RateLimit-Limit / -Remaining    (on every gated response)
//   - Wire protocol on allow:
//       X-RateLimit-Limit / -Remaining
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (config.h / logger.h / server.h / jwt_utils.h / auth_middleware.h).
//   - The middleware has two layers:
//       (1) RateLimiter — the storage + algorithm. A single instance
//           can serve many quotas because buckets are keyed by
//           (quota_name, client_key). Tests instantiate it directly
//           with a fake clock to exercise refill without sleeping.
//       (2) consume_rate_limit() — the route-handler guard. Reads
//           the request's client IP and (if a Bearer token is present)
//           its best-effort `sub` claim, then takes a token from the
//           appropriate bucket. Throws ApiException(429, RATE_LIMITED)
//           on deny so server.h's per-request wrap turns it into the
//           SPEC §5.7 envelope without a single line of error plumbing
//           at the call site.
//   - Best-effort user extraction: we base64url-decode the JWT payload
//     WITHOUT verifying the signature. This is fine for keying — the
//     actual auth check (auth_middleware::require_authentication) still
//     runs in the handler and rejects forged tokens. The worst case
//     (an attacker spoofs `sub` to dodge the per-user bucket) still
//     leaves them hitting the per-IP bucket on /auth/* and the
//     auth-required barrier on /submissions.
//   - We do NOT consult the refresh-token blacklist here. That's a
//     revocation concern, not a rate-limit one.
//   - We deliberately do not pull this into server.h's pre-routing
//     hook because not every route is rate-limited (e.g. /api/v1/health
//     must always answer; OPTIONS preflights must be cheap). Routes
//     that need it call consume_rate_limit() at the top of the handler.
//
// Usage (from a Phase 2 / Phase 6 route handler):
//
//   // One limiter for the process; main() owns it.
//   litecode::RateLimiter limiter;
//
//   s.post("/api/v1/auth/register", [&](const Request& req, Response& res) {
//       litecode::consume_rate_limit(
//           res, req, limiter,
//           litecode::auth_register_quota(litecode::config().rate_limit));
//       auto j = litecode::parse_json_body(req, res);
//       if (!j) return;
//       // ...real registration logic...
//   });
//
//   s.post("/api/v1/submissions", [&](const Request& req, Response& res) {
//       const auto claims = litecode::require_authentication(req,
//                                  litecode::config().jwt);
//       litecode::consume_rate_limit(
//           res, req, limiter,
//           litecode::submission_quota(litecode::config().rate_limit),
//           &claims);
//       // ...real submission logic...
//   });

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../config.h"               // RateLimitConfig
#include "../logger.h"               // LOG_WARN
#include "../routes/error_handler.h" // ApiException / ErrorCode::RATE_LIMITED / send_error

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Section: tunables
// ────────────────────────────────────────────────────────────────────────────

// Soft cap on simultaneously tracked (quota, key) pairs. Once exceeded we
// drop fully-refilled buckets (no state worth keeping) and, if still over
// the cap, drop the next-to-expire buckets. Defends against a flood of
// unique IPs (or user_ids) blowing up the Web container's RSS.
inline constexpr std::size_t kRateLimitMaxTrackedKeys = 100000;

// X-Forwarded-For is honored by default. In production the service runs
// behind Caddy (SPEC §3.1) which already injects X-Forwarded-For with the
// real client IP; without trusting it, every gated request would appear
// to come from 127.0.0.1 and the per-IP bucket would be useless. Set to
// false when terminating TLS directly on the Web container with no proxy.
inline constexpr bool kRateLimitTrustXff = true;

// ────────────────────────────────────────────────────────────────────────────
//  Section: keying
//
//  Three keying modes cover every rate-limited endpoint in SPEC §5:
//   - ByIp           — anonymous endpoints (auth/register, auth/login).
//                      Keyed on the client IP.
//   - ByUser         — authenticated endpoints (submissions). Keyed on
//                      the JWT subject. A handler MUST have already
//                      verified the token (and pass the Claims pointer);
//                      we still extract best-effort from the header to
//                      keep the API one-liner-friendly.
//   - ByUserOrIp     — endpoints that can be hit by either anonymous
//                      or authenticated callers. We key on user_id when
//                      a Bearer token is present, else fall back to IP.
//                      Reserved for future endpoints (e.g. /api/v1/admin/*
//                      in mixed-mode).
// ────────────────────────────────────────────────────────────────────────────

enum class RateLimitKeyType {
    ByIp,
    ByUser,
    ByUserOrIp,
};

// RateLimitQuota — bundles a SPEC §5 endpoint's quota and keying policy.
//
// `name` is a short stable identifier used in logs / as the bucket-name
// prefix in the storage map (so two different quotas with the same
// numerical limit never share a bucket).
struct RateLimitQuota {
    std::string               name;
    int                       capacity;        // bucket size == max burst
    std::chrono::seconds      window;          // full refill period
    RateLimitKeyType          key_type;
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: SPEC §5 quota factories
//
//  These read from RateLimitConfig and return a fully-populated Quota.
//  The "1 minute" / "1 hour" window is hard-coded; the rate comes from
//  config so operators can dial the limit without recompiling (SPEC §9
//  says "MVP 用内存"; future work can add a Redis-backed adapter).
// ────────────────────────────────────────────────────────────────────────────

inline RateLimitQuota auth_register_quota(const RateLimitConfig& cfg) {
    return RateLimitQuota{
        "auth.register",
        cfg.auth_register_per_minute_per_ip,
        std::chrono::minutes(1),
        RateLimitKeyType::ByIp,
    };
}

inline RateLimitQuota auth_login_quota(const RateLimitConfig& cfg) {
    return RateLimitQuota{
        "auth.login",
        cfg.auth_login_per_minute_per_ip,
        std::chrono::minutes(1),
        RateLimitKeyType::ByIp,
    };
}

inline RateLimitQuota submission_quota(const RateLimitConfig& cfg) {
    return RateLimitQuota{
        "submission",
        cfg.submission_per_minute_per_user,
        std::chrono::minutes(1),
        RateLimitKeyType::ByUser,
    };
}

inline RateLimitQuota admin_write_quota(const RateLimitConfig& cfg) {
    // Per SPEC §5.2/§5.5: 30/min for admin write operations. The
    // per-admin keying is intentional — admins share a single 30/min
    // budget (not per-route), so a single operator can't fire all
    // their admin endpoints at full rate in parallel.
    return RateLimitQuota{
        "admin.write",
        cfg.admin_write_per_minute,
        std::chrono::minutes(1),
        RateLimitKeyType::ByUser,
    };
}

inline RateLimitQuota bulk_import_quota(const RateLimitConfig& cfg) {
    return RateLimitQuota{
        "admin.bulk_import",
        cfg.bulk_import_per_hour,
        std::chrono::hours(1),
        RateLimitKeyType::ByUser,
    };
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: RateLimitDecision
//
//  What the limiter returns to its caller. `allowed` is the only
//  field the route handler truly cares about; the rest is filled in
//  so we can stamp the X-RateLimit-* headers and compute Retry-After
//  on the wire without re-doing the math.
// ────────────────────────────────────────────────────────────────────────────

struct RateLimitDecision {
    bool                                 allowed = true;
    int                                  limit            = 0;
    int                                  remaining        = 0;
    std::chrono::seconds                 retry_after{0}; // 0 when allowed
    std::chrono::steady_clock::time_point reset_at{};
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: RateLimiter
//
//  Thread-safe in-memory token-bucket store. Buckets are keyed by
//  (quota_name, client_key); capacity and window come from the
//  per-call RateLimitQuota so the same instance can serve many
//  quotas with different rates.
//
//  Token bucket semantics:
//    - Each bucket holds `capacity` tokens (== max burst).
//    - Tokens refill smoothly: after `window` seconds of idleness the
//      bucket is back to full. Partial refills are accumulated as
//      fractional tokens (double precision).
//    - A consume() call takes 1 token if the bucket has >= 1,
//      else rejects.
//    - A second consume() call within the same millisecond can
//      succeed (up to capacity bursts) then start rejecting —
//      that's the intended "burst then steady-state" behavior
//      of token bucket vs. fixed-window.
//
//  Thread safety: every public method takes a single internal mutex.
//  The map is small (one entry per active client) and the critical
//  section is bounded (no allocations beyond the lookup), so contention
//  is fine for the SPEC §2.2 "5-10 人同时使用" load.
// ────────────────────────────────────────────────────────────────────────────

class RateLimiter {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // Default constructor wires the system steady_clock. Tests pass
    // their own closure to fast-forward time deterministically.
    RateLimiter()
        : clock_([]{
              return std::chrono::steady_clock::now();
          }) {}

    // Inject a fake clock. The closure must be thread-safe (RateLimiter
    // invokes it under the mutex). Pass nullptr to revert to system time.
    void set_clock(Clock fn) {
        std::lock_guard<std::mutex> g(mu_);
        clock_ = fn ? std::move(fn) : Clock{[]{
            return std::chrono::steady_clock::now();
        }};
    }

    // consume — take one token from `quota.name|key` if available.
    //
    // The bucket is created lazily on first sight; capacity / window
    // are taken from the per-call Quota. If a previous call used a
    // different capacity / window for the same (quota.name, key) —
    // i.e. the operator changed RateLimitConfig at runtime — we
    // re-initialize the bucket rather than mix math. In normal
    // operation the config doesn't change after boot, so this branch
    // is cold; we still handle it correctly for tests.
    RateLimitDecision consume(const RateLimitQuota& quota,
                              const std::string&   key) {
        const std::string composite = quota.name + "|" + key;
        const int        capacity   = quota.capacity;
        const auto       window     = quota.window;
        const auto       window_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(window).count();

        std::lock_guard<std::mutex> g(mu_);
        prune_if_needed_locked();

        const auto now = clock_();
        auto& bucket   = buckets_[composite];

        // (Re-)init when first seen or when capacity/window changed.
        if (bucket.capacity != capacity || bucket.window != window) {
            bucket.capacity    = capacity;
            bucket.window      = window;
            bucket.tokens      = static_cast<double>(capacity);
            bucket.last_refill = now;
        }

        // Refill. Time delta is converted to milliseconds; we add
        // `elapsed / window * capacity` tokens. Negative time deltas
        // (clock went backward) clamp to zero — refusing to refill
        // forward of "now" prevents a maliciously-set system clock
        // from granting free tokens.
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - bucket.last_refill).count();
        if (elapsed_ms > 0 && window_ms > 0 && capacity > 0) {
            const double add = static_cast<double>(elapsed_ms)
                             / static_cast<double>(window_ms)
                             * static_cast<double>(capacity);
            bucket.tokens = std::min<double>(
                static_cast<double>(capacity),
                bucket.tokens + add);
            bucket.last_refill = now;
        }

        RateLimitDecision d;
        d.limit = capacity;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            d.allowed   = true;
            d.remaining = static_cast<int>(bucket.tokens);   // floor
            // Time for the bucket to be fully refilled. We round up
            // so clients never retry a hair too early.
            const double missing   = static_cast<double>(capacity) - bucket.tokens;
            const double ms_per_t = static_cast<double>(window_ms)
                                  / static_cast<double>(capacity);
            const auto reset_ms = static_cast<std::int64_t>(
                missing * ms_per_t + 0.5);
            d.reset_at    = now + std::chrono::milliseconds(reset_ms);
            d.retry_after = std::chrono::seconds(0);
        } else {
            d.allowed   = false;
            d.remaining = 0;
            // Time to reach 1 full token from the current (negative-
            // effective) level. ms_per_t is per-token refill.
            const double ms_per_t = static_cast<double>(window_ms)
                                  / static_cast<double>(capacity);
            const double need_ms  = (1.0 - bucket.tokens) * ms_per_t;
            // Round up + ensure at least 1 second on the wire so
            // clients that obey Retry-After=0 (immediate) don't busy-
            // loop themselves into a 429 storm.
            const auto retry_s = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(need_ms / 1000.0 + 0.999));
            d.retry_after = std::chrono::seconds(retry_s);
            d.reset_at    = now + std::chrono::milliseconds(
                                static_cast<std::int64_t>(need_ms + 0.5));
        }
        return d;
    }

    // peek — read-only check (does NOT consume). Useful for tests
    // and /api/v1/admin/stats ("queue size, warm pool, current
    // per-IP request rate"). Returns a decision with allowed=true
    // and remaining=current floor.
    RateLimitDecision peek(const std::string& quota_name,
                           const std::string& key,
                           int                capacity,
                           std::chrono::seconds window) const {
        const std::string composite = quota_name + "|" + key;
        const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(window).count();

        std::lock_guard<std::mutex> g(mu_);
        auto it = buckets_.find(composite);
        if (it == buckets_.end()) {
            return RateLimitDecision{
                /*allowed=*/true,
                /*limit=*/capacity,
                /*remaining=*/capacity,
                /*retry_after=*/std::chrono::seconds(0),
                /*reset_at=*/clock_(),
            };
        }
        const auto now = clock_();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - it->second.last_refill).count();
        double tokens = it->second.tokens;
        if (elapsed_ms > 0 && window_ms > 0 && capacity > 0) {
            const double add = static_cast<double>(elapsed_ms)
                             / static_cast<double>(window_ms)
                             * static_cast<double>(capacity);
            tokens = std::min<double>(static_cast<double>(capacity),
                                      tokens + add);
        }
        return RateLimitDecision{
            /*allowed=*/tokens >= 1.0,
            /*limit=*/capacity,
            /*remaining=*/static_cast<int>(tokens),
            /*retry_after=*/std::chrono::seconds(0),
            /*reset_at=*/now,
        };
    }

    // Number of (quota, key) pairs currently tracked. Memory accounting
    // for /api/v1/admin/stats.
    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return buckets_.size();
    }

    // Wipe all state. Test-only — there's no production code path that
    // should be calling this (operators change RateLimitConfig and let
    // the buckets naturally re-init).
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        buckets_.clear();
    }

private:
    struct Bucket {
        int                                  capacity    = 0;
        std::chrono::seconds                 window{0};
        double                               tokens      = 0.0;
        std::chrono::steady_clock::time_point last_refill{};
    };

    // prune_if_needed_locked — memory bound. Called under the mutex.
    //
    // If we're at the soft cap, drop buckets that are already full
    // (they don't need state to keep ticking — the next consume()
    // call will lazily create a fresh full bucket). If still over
    // the cap after that, drop the oldest 10% by last_refill.
    void prune_if_needed_locked() {
        if (buckets_.size() < kRateLimitMaxTrackedKeys) return;

        for (auto it = buckets_.begin(); it != buckets_.end(); ) {
            if (it->second.tokens >= static_cast<double>(it->second.capacity)) {
                it = buckets_.erase(it);
            } else {
                ++it;
            }
        }
        if (buckets_.size() < kRateLimitMaxTrackedKeys) return;

        // Vector of (last_refill, *iterator) for the remaining
        // (non-full) buckets. Sort, drop the oldest 10%.
        std::vector<std::pair<std::chrono::steady_clock::time_point,
                              std::unordered_map<std::string, Bucket>::iterator>>
            entries;
        entries.reserve(buckets_.size());
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
            entries.emplace_back(it->second.last_refill, it);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        const std::size_t drop = std::max<std::size_t>(1, entries.size() / 10);
        for (std::size_t i = 0; i < drop; ++i) {
            buckets_.erase(entries[i].second);
        }
    }

    mutable std::mutex                                     mu_;
    Clock                                                  clock_;
    std::unordered_map<std::string, Bucket>                buckets_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Section: client IP extraction
//
//  Order:
//    1. X-Forwarded-For — first comma-separated value. Behind a proxy
//       (Caddy) this is the real client; without it we'd bucket
//       127.0.0.1 (the proxy) and the limit would be useless.
//    2. X-Real-IP — nginx's equivalent.
//    3. req.remote_addr — last-resort socket peer.
//
//  We only honor (1) and (2) when kRateLimitTrustXff is true; see
//  the rationale near the constant declaration.
// ────────────────────────────────────────────────────────────────────────────

inline std::string extract_client_ip(const httplib::Request& req) {
    if constexpr (kRateLimitTrustXff) {
        const std::string xff = req.get_header_value("X-Forwarded-For");
        if (!xff.empty()) {
            const auto comma = xff.find(',');
            std::string first = (comma == std::string::npos)
                                  ? xff
                                  : xff.substr(0, comma);
            // Trim surrounding OWS.
            while (!first.empty()
                   && std::isspace(static_cast<unsigned char>(first.front()))) {
                first.erase(first.begin());
            }
            while (!first.empty()
                   && std::isspace(static_cast<unsigned char>(first.back()))) {
                first.pop_back();
            }
            if (!first.empty()) return first;
        }
        const std::string real = req.get_header_value("X-Real-IP");
        if (!real.empty()) return real;
    }
    return req.remote_addr;
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: best-effort JWT subject extraction
//
//  For RateLimitKeyType::ByUser / ByUserOrIp we want to key on the
//  caller's user_id. Doing a full verify() here is overkill (and
//  costly) — the auth_middleware::require_authentication() call in
//  the route handler is the actual gate. A rate-limit attacker who
//  forges a `sub` only earns themselves a fresh per-user bucket
//  they could have gotten from a different IP anyway; they still
//  hit the per-IP bucket on /auth/* and the auth-required barrier
//  on /submissions.
//
//  Algorithm:
//    - Look for `Authorization: Bearer <token>` via auth_middleware's
//      extract_bearer_token (CRLF / whitespace / case already handled).
//    - Split on '.'; we need exactly three segments.
//    - base64url-decode the second segment (the payload).
//    - Parse as JSON, pull `sub` (string).
//    - Return empty string on ANY failure (we never want to crash a
//      request just because the rate-limiter couldn't read the token).
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

// base64url-decode (RFC 4648 §5). '+' → '-', '/' → '_', '=' padding
// stripped. Returns empty string on malformed input — we never throw
// from this path because a malformed token just means "we'll fall
// back to IP keying", not "abort the request".
inline std::string base64url_decode(std::string_view in) {
    if (in.empty()) return {};
    // Translate URL-safe alphabet to standard base64.
    std::string normalized;
    normalized.reserve(in.size() + 4);
    for (char c : in) {
        if      (c == '-') normalized.push_back('+');
        else if (c == '_') normalized.push_back('/');
        else               normalized.push_back(c);
    }
    // Re-pad to a multiple of 4.
    while (normalized.size() % 4 != 0) normalized.push_back('=');

    static constexpr int8_t kInvalid = -1;
    static const auto kTbl = []{
        std::array<int8_t, 256> t{};
        t.fill(kInvalid);
        const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(alpha[i])] = static_cast<int8_t>(i);
        return t;
    }();

    std::string out;
    out.reserve((normalized.size() / 4) * 3);
    // accumulator + bits together hold the unconsumed bits. Every time
    // we shift accumulator left by 6 the previously-accumulated bits
    // move up; when we extract a byte we must mask off the bits we
    // just emitted, otherwise the next iteration's shift pulls in
    // STALE bits and decodes garbage.
    unsigned int bits        = 0;
    unsigned int accumulator = 0;
    for (char c : normalized) {
        if (c == '=') break;             // padding — stop early
        const int8_t v = kTbl[static_cast<unsigned char>(c)];
        if (v < 0) return {};            // malformed character
        accumulator = (accumulator << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
            // Drop the bits we just consumed. (1u << 0) is 1, so the
            // mask is well-defined even when bits == 0 (we clear
            // everything; the next iteration's shift starts fresh).
            accumulator &= (1u << bits) - 1u;
        }
    }
    return out;
}

} // namespace detail

// try_extract_jwt_subject — return the `sub` claim from the request's
// Bearer token, or "" if any step fails. Never throws.
//
// Implementation note: we deliberately do NOT call
// `auth_middleware::extract_bearer_token(req)`. That helper's
// Request-overload returns a `std::string_view` aliasing a local
// `std::string` that's destroyed at function return — reading the
// returned view is undefined behavior in general (it happens to
// work for callers that consume the value immediately, but not for
// a chain like `std::string(sv)`). To stay correct we parse the
// Authorization header ourselves in this header. The behaviour we
// want is identical to auth_middleware's parser (case-insensitive
// Bearer scheme, OWS tolerated, embedded whitespace rejected as
// malformed).
inline std::string try_extract_jwt_subject(const httplib::Request& req) {
    // Step 1: pull the header. get_header_value() returns a std::string
    // BY VALUE; we hold it in a local to keep the buffer alive.
    const std::string header_value = req.get_header_value("Authorization");
    if (header_value.empty()) return {};

    // Step 2: locate the bearer token. The Authorization header is
    // "Bearer <token>" with optional surrounding OWS (RFC 7235).
    const std::string_view header(header_value);
    std::size_t pos = 0;
    while (pos < header.size()
           && std::isspace(static_cast<unsigned char>(header[pos]))) {
        ++pos;
    }
    static constexpr std::string_view kBearer = "Bearer";
    if (pos + kBearer.size() > header.size()) return {};
    for (std::size_t i = 0; i < kBearer.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(header[pos + i]);
        const unsigned char b = static_cast<unsigned char>(kBearer[i]);
        if (std::tolower(a) != std::tolower(b)) return {};
    }
    pos += kBearer.size();
    while (pos < header.size()
           && std::isspace(static_cast<unsigned char>(header[pos]))) {
        ++pos;
    }
    if (pos >= header.size()) return {};

    // Token goes until the next whitespace (none, in a well-formed
    // header) OR end of string.
    const auto end = header.find_first_of(" \t\r\n", pos);
    const std::size_t token_len = (end == std::string_view::npos)
                                      ? header.size() - pos
                                      : end - pos;
    if (token_len == 0) return {};
    const std::string_view token_view(header.data() + pos, token_len);

    // Step 3: split the JWT and decode the payload. We copy the
    // token_view into a fresh std::string so the segments we hand
    // to base64url_decode are unambiguous and short-lived copies.
    const std::string token(token_view);
    const auto dot1 = token.find('.');
    if (dot1 == std::string::npos) return {};
    const auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return {};
    if (dot2 == dot1 + 1) return {};   // empty payload
    const std::string payload_str = detail::base64url_decode(
        std::string_view(token.data() + dot1 + 1, dot2 - dot1 - 1));
    if (payload_str.empty()) return {};

    try {
        const auto j = nlohmann::json::parse(payload_str);
        if (j.is_object() && j.contains("sub") && j["sub"].is_string()) {
            return j["sub"].get<std::string>();
        }
    } catch (const std::exception&) {
        // Malformed JSON in the payload — fall through to "".
    }
    return {};
}

// resolve_client_key — pick the bucket key for a given request + quota.
//
// Returns "<kind>:<value>" so two different key types never collide
// (e.g. a user_id of "127.0.0.1" is harmless because we prefix it
// with "user:").
inline std::string resolve_client_key(const httplib::Request& req,
                                      const RateLimitQuota&   quota) {
    if (quota.key_type == RateLimitKeyType::ByIp) {
        return std::string("ip:") + extract_client_ip(req);
    }
    // ByUser / ByUserOrIp — try to get the user_id from the Bearer
    // token, fall back to IP when not available. The fallback is
    // important: a /submissions caller without a valid token will
    // 401 in the auth_middleware before our limit matters, but if
    // for some reason a request slips through, we still want to
    // bucket them by IP rather than crashing or giving them an
    // empty key.
    const std::string sub = try_extract_jwt_subject(req);
    if (!sub.empty()) {
        return std::string("user:") + sub;
    }
    if (quota.key_type == RateLimitKeyType::ByUser) {
        // Strict ByUser with no user_id → key by IP. Keeps the
        // bucket limit working even for un-authed probes; the
        // route handler will still 401 them.
        return std::string("ip:") + extract_client_ip(req);
    }
    // ByUserOrIp.
    return std::string("ip:") + extract_client_ip(req);
}

// ────────────────────────────────────────────────────────────────────────────
//  Section: consume_rate_limit — the route-handler guard
//
//  This is the only function route handlers need to call. It:
//    1. Resolves the client key (IP or user_id) per the quota.
//    2. Takes one token from the matching bucket.
//    3. On allow: stamps X-RateLimit-Limit / -Remaining and returns.
//    4. On deny:  stamps the same headers, sets Retry-After, logs a
//       WARN line (with the per-thread X-Request-Id, courtesy of
//       server.h's RequestIdScope), and throws
//       ApiException(429, RATE_LIMITED, ...). server.h's per-request
//       wrap catches the exception and emits the SPEC §5.7 envelope.
//
//  Log shape (single line, JSON-friendly via existing logger):
//      WARN rate_limit: blocked
//           quota=auth.register  key_kind=ip  retry_after_s=12
//      (key value is intentionally NOT logged — IP / user_id are PII
//      per SPEC §15.7 and the "kind" is enough to spot attacks.)
// ────────────────────────────────────────────────────────────────────────────

inline void consume_rate_limit(httplib::Response&     res,
                               const httplib::Request& req,
                               RateLimiter&           limiter,
                               const RateLimitQuota&   quota) {
    const std::string key = resolve_client_key(req, quota);
    const std::size_t colon = key.find(':');
    const std::string key_kind = (colon == std::string::npos)
                                    ? std::string("?")
                                    : key.substr(0, colon);

    const auto decision = limiter.consume(quota, key);

    // Always set the "you're being tracked" headers, even on success.
    // Clients use these to back off proactively (well-behaved bots
    // read them) and they're invaluable when debugging limit disputes.
    res.set_header("X-RateLimit-Limit",     std::to_string(decision.limit));
    res.set_header("X-RateLimit-Remaining", std::to_string(std::max(0, decision.remaining)));

    if (decision.allowed) return;

    // Deny path. The Retry-After value is always at least 1 second —
    // see the comment in RateLimiter::consume() for why. RFC 7231
    // allows either a delta-seconds (integer) or HTTP-date; we use
    // the integer form, which is universally supported.
    const auto retry_s = decision.retry_after.count();
    res.set_header("Retry-After", std::to_string(retry_s));

    LOG_WARN("rate_limit: blocked",
             {{"quota",         quota.name},
              {"key_kind",      key_kind},
              {"limit",         std::to_string(decision.limit)},
              {"retry_after_s", std::to_string(retry_s)}});

    throw ApiException(
        /*status=*/429,
        /*code=*/ErrorCode::RATE_LIMITED,
        /*message=*/"rate limit exceeded for " + quota.name,
        /*details=*/nlohmann::json{
            {"quota",          quota.name},
            {"limit",          decision.limit},
            {"retry_after_s",  retry_s},
        });
}

} // namespace litecode
