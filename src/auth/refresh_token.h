// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — Refresh token mechanism (Phase 2 ★)
//
// SPEC §5.1 / §11 Phase 2 / §15.1 / A2 acceptance:
//   - Login / register hand out a {access, refresh} token pair.
//   - /api/v1/auth/refresh swaps a valid refresh token for a fresh pair
//     (rotation — the old refresh is added to the blacklist so it
//     can never be used again, even by the same client).
//   - /api/v1/auth/logout adds the presented refresh token to the
//     blacklist with a TTL equal to its remaining validity, so a
//     stolen token from a logged-out session is rejected on the
//     wire without DB writes (SPEC §15.1).
//   - The blacklist key is `jwt:blacklist:<jti>` in production Redis
//     (per SPEC §5.1) — the abstract `RefreshTokenStore` interface
//     here lets main.cpp inject a Redis-backed implementation when
//     `cfg.redis.enabled` is true, and falls back to the in-memory
//     map when running single-instance (MVP default — SPEC §9).
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (config.h / logger.h / jwt_utils.h / auth_middleware.h /
//     rate_limit.h). The store itself is a small in-memory map; the
//     convenience helpers (`issue_token_pair`, `rotate_token_pair`,
//     `revoke_refresh_token`) wrap jwt_utils.h to keep route handlers
//     one-liner-clean.
//   - Two-tier exception hierarchy mirrors the rest of Phase 2:
//       RefreshTokenError (base) — generic failure
//         ├─ RefreshTokenRevokedError   — presented jti is on the
//         │                              blacklist (logout, rotation,
//         │                              or admin revocation). 401.
//         └─ RefreshTokenInvalidError   — bad signature, expired,
//                                        wrong kind, etc. 401.
//     The base class catches both with one arm in route handlers.
//   - The store uses `steady_clock` internally for TTL bookkeeping
//     so it stays correct under NTP adjustments. The TTL we RECEIVE
//     from the caller is a `std::chrono::seconds duration` — the
//     caller (route handler) computes it from the JWT's `exp` claim
//     using whatever clock the verify() call was given, so test
//     fixtures can drive expiry without sleeping.
//   - In-memory store caps its size to keep the Web container's RSS
//     bounded under attack. When the cap is hit, the next purge
//     cycle evicts the entries that expire soonest. Bounded LRU
//     isn't needed — refresh tokens all have TTL ≤ 7d, so the
//     natural churn is enough.
//   - The default-store accessor (`default_refresh_token_store()`)
//     is process-wide, so a single store is shared across all
//     HTTP worker threads. Tests inject a local instance instead
//     to avoid cross-test pollution (and call
//     `reset_refresh_token_store_for_testing()` between cases).
//
// Usage from /api/v1/auth/refresh (Phase 2 ★):
//
//   void refresh_handler(const Request& req, Response& res) {
//       const auto& cfg = litecode::config();
//       const auto j = litecode::parse_json_body(req, res);
//       if (!j) return;
//       const std::string presented = j->value("refresh_token", "");
//       if (presented.empty()) {
//           litecode::send_error(res, 400, litecode::ErrorCode::INVALID_INPUT,
//                                "missing refresh_token");
//           return;
//       }
//       try {
//           const auto pair = litecode::rotate_token_pair(
//               *litecode::default_refresh_token_store(),
//               cfg.jwt.secret, cfg.jwt.issuer, presented,
//               cfg.jwt.access_ttl_seconds, cfg.jwt.refresh_ttl_seconds);
//           litecode::send_success(res, {
//               {"access_token",  pair.access_token},
//               {"refresh_token", pair.refresh_token},
//               {"token_type",    "Bearer"},
//               {"expires_in",    cfg.jwt.access_ttl_seconds},
//           });
//       } catch (const litecode::RefreshTokenError& e) {
//           litecode::send_error(res, 401, litecode::ErrorCode::UNAUTHORIZED,
//                                "invalid or expired refresh token");
//       }
//   }
//
// Usage from /api/v1/auth/logout:
//
//   litecode::revoke_refresh_token(
//       *litecode::default_refresh_token_store(),
//       presented_refresh, cfg.jwt.secret, cfg.jwt.issuer,
//       cfg.jwt.refresh_ttl_seconds);
//   // (best-effort — logout shouldn't 500 on a malformed token;
//   //  auth_routes decides whether to fold that into the response.)

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "jwt_utils.h"   // verify / Claims / TokenKind / sign_access / sign_refresh / SignedToken / SystemClock

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
//
//  Two-tier hierarchy (mirrors jwt_utils.h's JwtError / JwtVerifyError):
//    - RefreshTokenError        — base; route handlers catch this
//    - RefreshTokenRevokedError — the jti is on the blacklist. 401.
//    - RefreshTokenInvalidError — signature/issuer/kind/expiry bad. 401.
//
//  We deliberately do NOT inherit from JwtError — even though a
//  refresh-token failure is logically a JWT failure, a route handler
//  that wants to map every "your session is gone" condition to 401
//  should be able to catch the broader RefreshTokenError without
//  also pulling in JwtError (and the doxygen-y file graph that
//  comes with it).
// ────────────────────────────────────────────────────────────────────────────

class RefreshTokenError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RefreshTokenRevokedError : public RefreshTokenError {
public:
    using RefreshTokenError::RefreshTokenError;
};

class RefreshTokenInvalidError : public RefreshTokenError {
public:
    using RefreshTokenError::RefreshTokenError;
};

// ────────────────────────────────────────────────────────────────────────────
//  RefreshTokenStore — abstract interface
//
//  Two methods callers actually use:
//    - revoke(jti, ttl)        — register a revocation, valid for `ttl`.
//                                TTL is the JWT's remaining validity so the
//                                blacklist self-cleans when the token
//                                would have expired anyway (SPEC §15.1).
//    - is_revoked(jti)         — check before honoring a refresh.
//
//  `purge_expired()` and `clear()` are bookkeeping — purge is called
//  opportunistically by `is_revoked` and `revoke` to keep the map
//  size bounded; tests / admin tools call `clear()` to reset state.
//
//  `max_entries()` is informational (for /api/v1/admin/stats) — the
//  store itself enforces its cap internally.
// ────────────────────────────────────────────────────────────────────────────

class RefreshTokenStore {
public:
    virtual ~RefreshTokenStore() = default;

    // Revoke a token by its `jti` claim. `ttl` is how long the
    // revocation should remain effective — pass the JWT's remaining
    // lifetime so the entry self-expires. Negative or zero `ttl`
    // counts as a no-op (the token is already expired; no need to
    // remember it).
    virtual void revoke(std::string_view jti,
                        std::chrono::seconds ttl) = 0;

    // Returns true iff `jti` is currently on the blacklist. An empty
    // jti is treated as "not revoked" — it can't have been signed
    // by us (we always generate UUIDs) so there's nothing to match.
    virtual bool is_revoked(std::string_view jti) const = 0;

    // Drop entries whose TTL has passed. Returns the number of
    // entries removed. Called opportunistically on the write path
    // and explicitly by admin tools.
    virtual std::size_t purge_expired() = 0;

    // Number of currently tracked revocations.
    virtual std::size_t size() const = 0;

    // Maximum number of revocations the store will keep before
    // forcing an eviction cycle. 0 ⇒ no cap.
    virtual std::size_t max_entries() const = 0;

    // Drop every entry. Tests only — production code should never
    // call this on the default store (it would log out every
    // user mid-session).
    virtual void clear() = 0;
};

// ────────────────────────────────────────────────────────────────────────────
//  InMemoryRefreshTokenStore — single-process implementation
//
//  Storage: std::unordered_map<jti, expiry_time_point>. Expiry uses
//  steady_clock so a wall-clock NTP jump can't accidentally revive
//  (or kill) a revocation.
//
//  Locking: a single std::mutex guards the map. The hot path
//  (is_revoked + revoke) takes the lock twice — once for an
//  opportunistic purge sweep and once for the actual read/write —
//  and both calls are O(1) amortized. The cap is enforced on the
//  write path; the entry with the earliest expiry is evicted.
//
//  Thread safety: safe for concurrent revoke / is_revoked / clear.
// ────────────────────────────────────────────────────────────────────────────

class InMemoryRefreshTokenStore final : public RefreshTokenStore {
public:
    // Default cap is 100k entries — well above what a busy OJ sees
    // (10 active users × 1 logout/min = 14400/day) and small enough
    // that the map's memory stays in single-digit MB. Tests override
    // to a smaller cap to exercise the eviction path.
    explicit InMemoryRefreshTokenStore(std::size_t max_entries = 100000)
        : max_entries_(max_entries) {}

    void revoke(std::string_view jti,
                std::chrono::seconds ttl) override {
        if (jti.empty()) return;
        if (ttl.count() <= 0) return;        // already expired — no-op

        const auto expiry = std::chrono::steady_clock::now() + ttl;
        std::lock_guard<std::mutex> g(mutex_);

        opportunistic_purge_locked();

        // Evict the entry that expires soonest if we're at the cap.
        // We don't try to be clever about WHICH entry — picking
        // the soonest-to-expire both reclaims the most memory per
        // eviction and ensures we keep the high-value (long-lived)
        // revocations (e.g. logout-of-an-active-session).
        if (max_entries_ > 0 && blacklist_.size() >= max_entries_) {
            auto soonest = blacklist_.begin();
            for (auto it = blacklist_.begin(); it != blacklist_.end(); ++it) {
                if (it->second < soonest->second) soonest = it;
            }
            blacklist_.erase(soonest);
        }

        blacklist_.emplace(std::string(jti), expiry);
    }

    bool is_revoked(std::string_view jti) const override {
        if (jti.empty()) return false;
        std::lock_guard<std::mutex> g(mutex_);

        const auto it = blacklist_.find(std::string(jti));
        if (it == blacklist_.end()) return false;

        if (std::chrono::steady_clock::now() >= it->second) {
            // Expired entry — lazily remove it. We don't update the
            // caller's reference because we're about to return.
            blacklist_.erase(it);
            return false;
        }
        return true;
    }

    std::size_t purge_expired() override {
        std::lock_guard<std::mutex> g(mutex_);
        return purge_expired_locked();
    }

    std::size_t size() const override {
        std::lock_guard<std::mutex> g(mutex_);
        return blacklist_.size();
    }

    std::size_t max_entries() const override {
        return max_entries_;
    }

    void clear() override {
        std::lock_guard<std::mutex> g(mutex_);
        blacklist_.clear();
    }

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void opportunistic_purge_locked() {
        // Same logic as purge_expired_locked() — kept private so the
        // public purge_expired() can hold the lock and still call it.
        purge_expired_locked();
    }

    std::size_t purge_expired_locked() {
        const auto now = Clock::now();
        std::size_t removed = 0;
        for (auto it = blacklist_.begin(); it != blacklist_.end(); ) {
            if (now >= it->second) {
                it = blacklist_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    mutable std::mutex                       mutex_;
    // `mutable` so const methods (is_revoked) can lazily evict
    // expired entries without giving up const-correctness at the
    // call site. All mutations are guarded by `mutex_` above, so
    // the logical state of the store is still externally immutable.
    mutable std::unordered_map<std::string, TimePoint> blacklist_;
    std::size_t                                       max_entries_;
};

// ────────────────────────────────────────────────────────────────────────────
//  Process-wide default store
//
//  main.cpp owns a single InMemoryRefreshTokenStore; tests inject
//  a local instance instead. We expose accessors rather than a true
//  global so the lifetime is well-defined — the store is destroyed
//  in LIFO order at program exit via the static unique_ptr destructor.
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::unique_ptr<RefreshTokenStore>& default_store_slot() {
    static std::unique_ptr<RefreshTokenStore> slot;
    return slot;
}

inline std::mutex& default_store_mutex() {
    static std::mutex m;
    return m;
}

} // namespace detail

// Returns the process-wide store, lazily creating an in-memory one
// on first use. Never returns nullptr. Safe to call from any thread.
inline RefreshTokenStore* default_refresh_token_store() {
    std::lock_guard<std::mutex> g(detail::default_store_mutex());
    auto& slot = detail::default_store_slot();
    if (!slot) {
        slot = std::make_unique<InMemoryRefreshTokenStore>();
    }
    return slot.get();
}

// Install a caller-owned store as the process-wide default. Pass
// nullptr to clear the slot (next call to default_refresh_token_store()
// will lazily recreate a fresh in-memory store). Used by main.cpp
// when wiring a Redis-backed store later.
inline void set_default_refresh_token_store(
        std::unique_ptr<RefreshTokenStore> store) {
    std::lock_guard<std::mutex> g(detail::default_store_mutex());
    detail::default_store_slot() = std::move(store);
}

// Drop the default store. Tests only.
inline void reset_refresh_token_store_for_testing() {
    std::lock_guard<std::mutex> g(detail::default_store_mutex());
    detail::default_store_slot().reset();
}

// ────────────────────────────────────────────────────────────────────────────
//  TokenPair — the wire-format return value of issue/rotate
//
//  Carries both tokens PLUS the jti of the refresh side so logout
//  handlers can revoke it directly without re-parsing the JWT.
//  expires_at lets the route handler emit `expires_in` in the
//  response without re-reading config.
// ────────────────────────────────────────────────────────────────────────────

struct TokenPair {
    std::string                              access_token;
    std::string                              access_jti;
    std::chrono::system_clock::time_point    access_expires_at;

    std::string                              refresh_token;
    std::string                              refresh_jti;
    std::chrono::system_clock::time_point    refresh_expires_at;

    // Number of seconds until the access token expires, computed
    // against `now`. Floor at 0 — never returns negative.
    int64_t access_expires_in_seconds(
            std::chrono::system_clock::time_point now) const {
        const auto remaining = access_expires_at - now;
        if (remaining.count() <= 0) return 0;
        return std::chrono::duration_cast<std::chrono::seconds>(remaining).count();
    }
};

// ────────────────────────────────────────────────────────────────────────────
//  issue_token_pair — login / register helper
//
//  Signs a fresh access + refresh pair. Does NOT consult the
//  blacklist (there is nothing to consult — both tokens are new).
//
//  Throws JwtSignError if either sign fails (propagated unchanged so
//  the route handler can map to 500).
// ────────────────────────────────────────────────────────────────────────────

template <typename Clock = SystemClock>
inline TokenPair issue_token_pair(std::string_view secret,
                                  std::string_view issuer,
                                  const std::string& user_id,
                                  const std::string& username,
                                  const std::string& role,
                                  int access_ttl_seconds,
                                  int refresh_ttl_seconds,
                                  Clock clock = Clock{}) {
    if (access_ttl_seconds < 1) {
        throw RefreshTokenError("issue_token_pair: access_ttl_seconds must be >= 1");
    }
    if (refresh_ttl_seconds < access_ttl_seconds) {
        throw RefreshTokenError(
            "issue_token_pair: refresh_ttl_seconds must be >= access_ttl_seconds");
    }

    const SignedToken access  = sign_access (secret, issuer, user_id,
                                             username, role, access_ttl_seconds);
    const SignedToken refresh = sign_refresh(secret, issuer, user_id,
                                             refresh_ttl_seconds);

    TokenPair out;
    out.access_token       = access.token;
    out.access_jti         = access.jti;
    out.access_expires_at  = clock.now() + std::chrono::seconds(access_ttl_seconds);

    out.refresh_token      = refresh.token;
    out.refresh_jti        = refresh.jti;
    out.refresh_expires_at = clock.now() + std::chrono::seconds(refresh_ttl_seconds);
    return out;
}

// ────────────────────────────────────────────────────────────────────────────
//  rotate_token_pair — refresh helper
//
//  The full /api/v1/auth/refresh flow:
//    1. Verify the presented refresh token (signature, expiry, kind).
//       JwtVerifyError / JwtClaimError are caught and rethrown as
//       RefreshTokenInvalidError so the route handler's single
//       RefreshTokenError catch handles both surfaces.
//    2. Reject if its jti is on the blacklist. → RefreshTokenRevokedError.
//    3. Revoke the old jti (TTL = remaining validity) so it can
//       never be presented again, even by the same client that just
//       exchanged it — a true rotation, not a copy.
//    4. Sign a fresh access + refresh pair with a new jti.
//
//  `username` and `role` MUST be supplied by the caller — the
//  refresh token deliberately does not carry them (least privilege;
//  SPEC §5.1) so we have no way to recover them from the JWT alone.
//  The route handler is expected to look up the user row by
//  `claims.user_id` (or by sub) and pass the live values in. Tests
//  pass the same values the initial login used.
//
//  Throws:
//    - RefreshTokenInvalidError (wraps JwtVerifyError / JwtClaimError)
//    - RefreshTokenRevokedError
//    - RefreshTokenError  — internal misuse (bad TTLs / empty fields)
//    - JwtSignError       — sign failure (propagated)
//
//  The `Clock` template parameter lets tests drive expiry with a
//  frozen clock; production defaults to SystemClock.
// ────────────────────────────────────────────────────────────────────────────

template <typename Clock = SystemClock>
inline TokenPair rotate_token_pair(RefreshTokenStore& store,
                                   std::string_view secret,
                                   std::string_view issuer,
                                   std::string_view presented_refresh_token,
                                   const std::string& username,
                                   const std::string& role,
                                   int access_ttl_seconds,
                                   int refresh_ttl_seconds,
                                   Clock clock = Clock{}) {
    if (access_ttl_seconds < 1) {
        throw RefreshTokenError("rotate_token_pair: access_ttl_seconds must be >= 1");
    }
    if (refresh_ttl_seconds < access_ttl_seconds) {
        throw RefreshTokenError(
            "rotate_token_pair: refresh_ttl_seconds must be >= access_ttl_seconds");
    }
    if (presented_refresh_token.empty()) {
        throw RefreshTokenInvalidError("refresh token is empty");
    }
    if (username.empty()) {
        throw RefreshTokenError(
            "rotate_token_pair: username must not be empty "
            "(the refresh token doesn't carry it; look up the user row)");
    }
    if (role.empty()) {
        throw RefreshTokenError(
            "rotate_token_pair: role must not be empty "
            "(the refresh token doesn't carry it; look up the user row)");
    }

    // 1) Verify the signature / kind / expiry. Any failure becomes
    //    RefreshTokenInvalidError so the route handler's catch is
    //    a single arm.
    Claims claims;
    try {
        claims = verify(std::string(presented_refresh_token),
                        secret, issuer,
                        TokenKind::Refresh, clock);
    } catch (const JwtError& e) {
        // Detail is logged at the call site; we collapse to a single
        // "invalid or expired refresh token" message in the envelope
        // (same anti-enumeration rule as auth_middleware.h).
        throw RefreshTokenInvalidError(
            std::string("refresh token verification failed: ") + e.what());
    }

    // 2) Blacklist check. A revoked jti is an authentication failure
    //    AND a possible theft signal — surfacing it as a distinct
    //    error code lets future audit logs flag the attempt.
    if (store.is_revoked(claims.jti)) {
        throw RefreshTokenRevokedError(
            "refresh token has been revoked (jti=" + claims.jti + ")");
    }

    // 3) Revoke the old jti. TTL is the JWT's remaining lifetime
    //    so the entry self-cleans when the token would have expired
    //    anyway — no need to track it for longer.
    const auto now          = clock.now();
    const auto remaining    = claims.expires_at - now;
    auto remaining_seconds  = std::chrono::duration_cast<std::chrono::seconds>(remaining);
    if (remaining_seconds.count() < 0) remaining_seconds = std::chrono::seconds(0);
    store.revoke(claims.jti, remaining_seconds);

    // 4) Sign the new pair. The new refresh's jti is fresh, so
    //    is_revoked() on the NEXT call to rotate_token_pair() will
    //    not falsely match it.
    return issue_token_pair<Clock>(secret, issuer,
                                   claims.user_id,
                                   username, role,
                                   access_ttl_seconds, refresh_ttl_seconds,
                                   clock);
}

// ────────────────────────────────────────────────────────────────────────────
//  revoke_refresh_token — logout helper
//
//  Best-effort revocation: a malformed / expired refresh token is
//  NOT an error (the goal of logout is to forget the session, and
//  the token is already useless if it's malformed or expired).
//  Returns true iff the token parsed cleanly and was added to the
//  blacklist; false if the token was malformed / expired / empty
//  (logout still returns 200 to the client in that case).
//
//  The caller may also pass an `expected_user_id` to prevent a
//  stolen token from being used to log out a victim's session —
//  the helper verifies `claims.user_id == expected_user_id` when
//  non-empty. Mismatch is reported as a return of false (logged at
//  WARN by the route handler, not as a hard 4xx — the legitimate
//  user's session still gets the requested logout).
// ────────────────────────────────────────────────────────────────────────────

struct RevokeOutcome {
    bool        revoked        = false;   // true ⇒ token was added to the blacklist
    bool        parsed         = false;   // true ⇒ token parsed cleanly (signature+claims)
    bool        user_matched   = false;   // true ⇒ expected_user_id matched (or no constraint)
    std::string jti;                      // populated when parsed=true
    std::string reason;                   // human-readable for logging
};

template <typename Clock = SystemClock>
inline RevokeOutcome revoke_refresh_token(RefreshTokenStore& store,
                                          std::string_view presented_refresh_token,
                                          std::string_view secret,
                                          std::string_view issuer,
                                          int max_ttl_seconds,
                                          std::string_view expected_user_id = {},
                                          Clock clock = Clock{}) {
    RevokeOutcome out;
    if (presented_refresh_token.empty()) {
        out.reason = "refresh token is empty";
        return out;
    }

    Claims claims;
    try {
        claims = verify(std::string(presented_refresh_token),
                        secret, issuer,
                        TokenKind::Refresh, clock);
    } catch (const JwtError& e) {
        // Best-effort: malformed / expired / wrong-kind tokens
        // don't add to the blacklist (they're already useless).
        out.parsed = false;
        out.reason = std::string("token did not parse: ") + e.what();
        return out;
    }
    out.parsed = true;
    out.jti    = claims.jti;

    if (!expected_user_id.empty() && claims.user_id != expected_user_id) {
        out.user_matched = false;
        out.reason       = "user_id mismatch (jti=" + claims.jti +
                           " expected=" + std::string(expected_user_id) +
                           " got="    + claims.user_id + ")";
        return out;
    }
    out.user_matched = true;

    const auto now         = clock.now();
    const auto remaining   = claims.expires_at - now;
    auto remaining_seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
    if (remaining_seconds.count() < 0) remaining_seconds = std::chrono::seconds(0);

    // Cap the TTL at max_ttl_seconds so a malicious token with a
    // far-future `exp` can't keep itself on the blacklist forever.
    // The verify() step already enforced the issuer-signed `exp`,
    // so this cap is purely a defense-in-depth ceiling.
    if (max_ttl_seconds > 0 &&
        remaining_seconds.count() > max_ttl_seconds) {
        remaining_seconds = std::chrono::seconds(max_ttl_seconds);
    }

    store.revoke(claims.jti, remaining_seconds);
    out.revoked = true;
    out.reason  = "revoked (jti=" + claims.jti + ")";
    return out;
}

} // namespace litecode
