// tests/unit/test_refresh_token.cpp
//
// Unit tests for src/auth/refresh_token.h — the SPEC §5.1 / §11
// Phase 2 ★ refresh-token mechanism (issue + rotate + revoke + blacklist).
//
// Coverage:
//   - InMemoryRefreshTokenStore:
//       * revoke + is_revoked happy path
//       * revoke on empty jti is a no-op
//       * revoke with non-positive TTL is a no-op
//       * is_revoked on empty / unknown jti returns false
//       * entries auto-expire on the read path (lazy eviction)
//       * purge_expired() removes only expired entries
//       * size() / max_entries() / clear() accounting
//       * the max_entries cap evicts the soonest-to-expire entry
//         (regression guard for OOM defense — SPEC §15.1)
//       * thread safety: parallel revoke / is_revoked / size()
//         must not crash or produce data races
//   - issue_token_pair:
//       * signs both sides; both verify with the expected kind
//       * access carries username/role, refresh does NOT (least priv)
//       * jti is a fresh UUID v4 (not reused across calls)
//       * access_expires_in_seconds() is correct vs a frozen clock
//       * rejects bad TTLs (access<1, refresh<access)
//   - rotate_token_pair:
//       * happy path: presents valid refresh, gets a NEW pair whose
//         jti differs from the old one
//       * the old refresh token is now revoked (rotation, not copy)
//       * the new refresh can itself be rotated (chain of rotations)
//       * presenting a revoked token → RefreshTokenRevokedError
//       * presenting a malformed / expired / wrong-kind token →
//         RefreshTokenInvalidError
//       * presenting an access token (not refresh) →
//         RefreshTokenInvalidError (token confusion defense)
//   - revoke_refresh_token:
//       * happy path returns RevokeOutcome{revoked=true, parsed=true,
//         user_matched=true} and the jti IS on the blacklist
//       * malformed refresh token → parsed=false, revoked=false
//         (best-effort; the server still returns 200 to the client)
//       * wrong user_id → parsed=true, user_matched=false,
//         revoked=false, token NOT added to blacklist
//       * TTL is capped at max_ttl_seconds (defense-in-depth ceiling)
//   - exception hierarchy: RefreshTokenRevokedError /
//     RefreshTokenInvalidError both catchable as RefreshTokenError
//     and as std::exception
//   - default_refresh_token_store() / set_default / reset round-trip

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "auth/jwt_utils.h"
#include "auth/refresh_token.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures
// ────────────────────────────────────────────────────────────────────────────

// 64-char secret — comfortably above the SPEC §5.1 floor of 32 bytes.
// Same as test_jwt_utils.cpp / test_auth_middleware.cpp; safe to repeat
// because the tests don't share state.
constexpr const char* kSecret = "refresh_token_test_secret_at_least_32_bytes_long_xx";
constexpr const char* kIssuer = "litecode-refresh-test";

// Reasonable TTLs for tests. Real SPEC values: access 2h, refresh 7d.
// Tests use shorter values so reasoning about expiry is intuitive.
constexpr int kAccessTtl  = 600;        // 10 min
constexpr int kRefreshTtl = 7 * 24 * 3600; // 7d — matches SPEC §5.1

// A frozen clock for deterministic TTL / expires_in math. The clock
// value is mutated by tests via `set()` / `advance()`.
struct FrozenClock {
    std::chrono::system_clock::time_point now_val{
        std::chrono::system_clock::time_point{}   // epoch
    };
    std::chrono::system_clock::time_point now() const { return now_val; }
    void set(std::chrono::system_clock::time_point t) { now_val = t; }
    void advance(std::chrono::seconds delta) { now_val += delta; }
};

// ────────────────────────────────────────────────────────────────────────────
//  InMemoryRefreshTokenStore
// ────────────────────────────────────────────────────────────────────────────

TEST(InMemoryStore, RevokeAndCheck) {
    litecode::InMemoryRefreshTokenStore store;
    EXPECT_FALSE(store.is_revoked("jti-1"));
    store.revoke("jti-1", std::chrono::seconds(60));
    EXPECT_TRUE (store.is_revoked("jti-1"));
    EXPECT_FALSE(store.is_revoked("jti-2"));
}

TEST(InMemoryStore, EmptyJtiIsNoOp) {
    litecode::InMemoryRefreshTokenStore store;
    store.revoke("",      std::chrono::seconds(60));   // empty jti
    store.revoke("ok",    std::chrono::seconds(0));    // zero TTL
    store.revoke("bad",   std::chrono::seconds(-1));   // negative TTL
    EXPECT_EQ(store.size(), 0u);
    EXPECT_FALSE(store.is_revoked(""));
    EXPECT_FALSE(store.is_revoked("ok"));
    EXPECT_FALSE(store.is_revoked("bad"));
}

TEST(InMemoryStore, LazyEvictionOnIsRevoked) {
    // The store uses steady_clock internally; we can't inject a
    // fake clock, so we revoke with a short TTL and sleep past it.
    // 1.2s is well below gtest's per-test timeout but well above
    // the wall-clock floor we'd need to make this flaky.
    litecode::InMemoryRefreshTokenStore store;
    store.revoke("short", std::chrono::seconds(1));
    EXPECT_TRUE(store.is_revoked("short"));
    EXPECT_EQ(store.size(), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(store.is_revoked("short"));     // lazy-evicted
    EXPECT_EQ(store.size(), 0u);
}

TEST(InMemoryStore, PurgeExpiredRemovesOnlyExpired) {
    litecode::InMemoryRefreshTokenStore store;
    store.revoke("short",  std::chrono::seconds(1));
    store.revoke("medium", std::chrono::seconds(60));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const std::size_t removed = store.purge_expired();
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_FALSE(store.is_revoked("short"));
    EXPECT_TRUE (store.is_revoked("medium"));
}

TEST(InMemoryStore, ClearAndAccounting) {
    litecode::InMemoryRefreshTokenStore store;
    EXPECT_EQ(store.size(), 0u);
    store.revoke("a", std::chrono::seconds(60));
    store.revoke("b", std::chrono::seconds(60));
    store.revoke("c", std::chrono::seconds(60));
    EXPECT_EQ(store.size(), 3u);
    store.clear();
    EXPECT_EQ(store.size(), 0u);
    EXPECT_FALSE(store.is_revoked("a"));
}

TEST(InMemoryStore, MaxEntriesEvictsSoonestToExpire) {
    // Cap = 3. We revoke four entries with strictly increasing TTLs
    // and expect the one with the SHORTEST TTL to be evicted (it
    // contributes the least to ongoing session safety).
    litecode::InMemoryRefreshTokenStore store(/*max_entries=*/3);
    store.revoke("short",  std::chrono::seconds(60));
    store.revoke("medium", std::chrono::seconds(3600));
    store.revoke("long",   std::chrono::seconds(24 * 3600));
    store.revoke("newer",  std::chrono::seconds(2 * 24 * 3600));
    EXPECT_EQ(store.size(), 3u);
    EXPECT_FALSE(store.is_revoked("short"));   // evicted
    EXPECT_TRUE (store.is_revoked("medium"));
    EXPECT_TRUE (store.is_revoked("long"));
    EXPECT_TRUE (store.is_revoked("newer"));
}

TEST(InMemoryStore, ZeroCapMeansUnlimited) {
    litecode::InMemoryRefreshTokenStore store(/*max_entries=*/0);
    for (int i = 0; i < 1000; ++i) {
        store.revoke("jti-" + std::to_string(i), std::chrono::seconds(60));
    }
    EXPECT_EQ(store.size(), 1000u);
    EXPECT_EQ(store.max_entries(), 0u);
}

TEST(InMemoryStore, ConcurrentAccessIsSafe) {
    // Spawn 4 writer threads + 4 reader threads, each doing 500
    // operations. With TSan enabled (or a multi-threaded stress
    // build) this catches data races; in a regular build we just
    // verify the store doesn't crash and ends with a sensible size.
    litecode::InMemoryRefreshTokenStore store;

    constexpr int kOpsPerThread = 500;
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;

    std::atomic<bool> start_gate{false};
    std::vector<std::thread> threads;
    threads.reserve(kWriters + kReaders);

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w]() {
            while (!start_gate.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kOpsPerThread; ++i) {
                store.revoke("w" + std::to_string(w) + "-" + std::to_string(i),
                             std::chrono::seconds(60));
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&, r]() {
            while (!start_gate.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kOpsPerThread; ++i) {
                (void)store.is_revoked("w0-" + std::to_string(i % kOpsPerThread));
                (void)store.size();
            }
        });
    }
    start_gate.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    // All writers used distinct (writer, i) keys, so size is
    // bounded by the cap (100k by default — never hit here).
    EXPECT_GT(store.size(), 0u);
    EXPECT_LE(store.size(), static_cast<std::size_t>(kWriters * kOpsPerThread));
}

// ────────────────────────────────────────────────────────────────────────────
//  issue_token_pair
// ────────────────────────────────────────────────────────────────────────────

TEST(IssueTokenPair, SignsBothSidesAndJtisDiffer) {
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, /*user_id=*/"42", "alice", "user",
        kAccessTtl, kRefreshTtl);

    EXPECT_FALSE(pair.access_token.empty());
    EXPECT_FALSE(pair.refresh_token.empty());
    EXPECT_NE(pair.access_jti, pair.refresh_jti);
    EXPECT_NE(pair.access_token, pair.refresh_token);
}

TEST(IssueTokenPair, AccessCarriesUsernameAndRoleRefreshDoesNot) {
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);

    // Access side: full claims.
    const auto access_claims = litecode::verify(
        pair.access_token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_EQ(access_claims.user_id,  "42");
    EXPECT_EQ(access_claims.username, "alice");
    EXPECT_EQ(access_claims.role,     "user");

    // Refresh side: sub only — no username / role (least privilege).
    const auto refresh_claims = litecode::verify(
        pair.refresh_token, kSecret, kIssuer, litecode::TokenKind::Refresh);
    EXPECT_EQ(refresh_claims.user_id, "42");
    EXPECT_EQ(refresh_claims.kind,    litecode::TokenKind::Refresh);
    EXPECT_TRUE(refresh_claims.username.empty());
    EXPECT_TRUE(refresh_claims.role.empty());
}

TEST(IssueTokenPair, JtiIsUuidV4) {
    // 36 chars, dashes at fixed positions, index 14 == '4', index
    // 19 in {8,9,a,b}. Same shape as uuid.h's contract.
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "1", "u", "user", kAccessTtl, kRefreshTtl);
    for (const std::string* s : {&pair.access_jti, &pair.refresh_jti}) {
        ASSERT_EQ(s->size(), 36u);
        EXPECT_EQ((*s)[8],  '-');
        EXPECT_EQ((*s)[13], '-');
        EXPECT_EQ((*s)[18], '-');
        EXPECT_EQ((*s)[23], '-');
        EXPECT_EQ((*s)[14], '4');                // version 4
        EXPECT_NE(std::string("89ab").find((*s)[19]), std::string::npos);
    }
}

TEST(IssueTokenPair, EachCallProducesFreshJti) {
    const auto a = litecode::issue_token_pair(
        kSecret, kIssuer, "1", "u", "user", kAccessTtl, kRefreshTtl);
    const auto b = litecode::issue_token_pair(
        kSecret, kIssuer, "1", "u", "user", kAccessTtl, kRefreshTtl);
    EXPECT_NE(a.access_jti,  b.access_jti);
    EXPECT_NE(a.refresh_jti, b.refresh_jti);
}

TEST(IssueTokenPair, AccessExpiresInSecondsIsCorrect) {
    FrozenClock clock;
    clock.set(std::chrono::system_clock::time_point{});   // epoch

    const auto pair = litecode::issue_token_pair<FrozenClock>(
        kSecret, kIssuer, "1", "u", "user", kAccessTtl, kRefreshTtl, clock);

    // At t=0 the access has full TTL remaining.
    EXPECT_EQ(pair.access_expires_in_seconds(clock.now()), kAccessTtl);

    // Halfway through, half the TTL remains.
    clock.advance(std::chrono::seconds(kAccessTtl / 2));
    EXPECT_EQ(pair.access_expires_in_seconds(clock.now()), kAccessTtl / 2);

    // After the TTL elapses, the helper floors at 0 (never negative).
    clock.advance(std::chrono::seconds(kAccessTtl));
    EXPECT_EQ(pair.access_expires_in_seconds(clock.now()), 0);
}

TEST(IssueTokenPair, RejectsBadTtls) {
    EXPECT_THROW(
        litecode::issue_token_pair(kSecret, kIssuer, "1", "u", "user",
                                   /*access_ttl=*/0, kRefreshTtl),
        litecode::RefreshTokenError);
    EXPECT_THROW(
        litecode::issue_token_pair(kSecret, kIssuer, "1", "u", "user",
                                   /*access_ttl=*/-1, kRefreshTtl),
        litecode::RefreshTokenError);
    EXPECT_THROW(
        litecode::issue_token_pair(kSecret, kIssuer, "1", "u", "user",
                                   kAccessTtl, /*refresh_ttl=*/60),
        litecode::RefreshTokenError);
}

TEST(IssueTokenPair, ExpiresAtMatchesClockPlusTtl) {
    FrozenClock clock;
    const auto t0 = std::chrono::system_clock::time_point{} + std::chrono::hours(1);
    clock.set(t0);

    const auto pair = litecode::issue_token_pair<FrozenClock>(
        kSecret, kIssuer, "1", "u", "user", kAccessTtl, kRefreshTtl, clock);
    EXPECT_EQ(pair.access_expires_at,  t0 + std::chrono::seconds(kAccessTtl));
    EXPECT_EQ(pair.refresh_expires_at, t0 + std::chrono::seconds(kRefreshTtl));
}

// ────────────────────────────────────────────────────────────────────────────
//  rotate_token_pair
// ────────────────────────────────────────────────────────────────────────────

TEST(RotateTokenPair, HappyPathProducesNewPairAndRevokesOld) {
    litecode::InMemoryRefreshTokenStore store;
    const auto initial = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);

    // Before rotation, the original refresh is NOT revoked.
    EXPECT_FALSE(store.is_revoked(initial.refresh_jti));

    const auto rotated = litecode::rotate_token_pair(
        store, kSecret, kIssuer, initial.refresh_token,
        /*username=*/"alice", /*role=*/"user",
        kAccessTtl, kRefreshTtl);

    // The new pair is fresh — different jti on both sides.
    EXPECT_NE(rotated.refresh_jti, initial.refresh_jti);
    EXPECT_NE(rotated.access_jti,  initial.access_jti);
    EXPECT_NE(rotated.refresh_token, initial.refresh_token);

    // The old refresh is now on the blacklist (rotation, not copy).
    EXPECT_TRUE(store.is_revoked(initial.refresh_jti));
    // The new refresh is NOT.
    EXPECT_FALSE(store.is_revoked(rotated.refresh_jti));
}

TEST(RotateTokenPair, ChainOfRotationsWorks) {
    // /api/v1/auth/refresh can be called repeatedly with the latest
    // refresh; each call rotates forward and blacklists the prior one.
    litecode::InMemoryRefreshTokenStore store;
    auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);

    constexpr int kSteps = 5;
    for (int i = 0; i < kSteps; ++i) {
        const auto next = litecode::rotate_token_pair(
            store, kSecret, kIssuer, pair.refresh_token,
            /*username=*/"alice", /*role=*/"user",
            kAccessTtl, kRefreshTtl);
        EXPECT_TRUE (store.is_revoked(pair.refresh_jti));
        EXPECT_FALSE(store.is_revoked(next.refresh_jti));
        pair = next;
    }
}

TEST(RotateTokenPair, PresentingRevokedTokenIsRevokedError) {
    // The whole point of rotation: a refresh that's been used once
    // MUST not be usable again. The wire-level error code is the
    // 401 UNAUTHORIZED envelope, but we surface the reason so the
    // route handler can log it (and so a future audit can flag
    // theft attempts).
    litecode::InMemoryRefreshTokenStore store;
    const auto initial = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);
    litecode::rotate_token_pair(
        store, kSecret, kIssuer, initial.refresh_token,
        /*username=*/"alice", /*role=*/"user",
        kAccessTtl, kRefreshTtl);

    EXPECT_THROW(
        litecode::rotate_token_pair(
            store, kSecret, kIssuer, initial.refresh_token,
            /*username=*/"alice", /*role=*/"user",
            kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenRevokedError);
}

TEST(RotateTokenPair, MalformedTokenIsInvalidError) {
    litecode::InMemoryRefreshTokenStore store;
    EXPECT_THROW(
        litecode::rotate_token_pair(
            store, kSecret, kIssuer, "not-a-jwt",
            "alice", "user", kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenInvalidError);
    EXPECT_THROW(
        litecode::rotate_token_pair(
            store, kSecret, kIssuer, "",
            "alice", "user", kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenInvalidError);
}

TEST(RotateTokenPair, ExpiredRefreshIsInvalidError) {
    // Sign a refresh with a TTL of 1s, then present it after the
    // clock has moved past its exp. The FrozenClock is seeded to
    // "now" (real time) so the iat check passes; sign_refresh uses
    // SystemClock internally, so iat = real_now and the FrozenClock
    // must agree that real_now <= now. Then we advance FrozenClock
    // by 2s to push it past exp.
    FrozenClock clock;
    clock.set(std::chrono::system_clock::now());

    litecode::InMemoryRefreshTokenStore store;
    const auto signed_ = litecode::sign_refresh(
        kSecret, kIssuer, "42", /*ttl=*/1);
    clock.advance(std::chrono::seconds(2));

    EXPECT_THROW(
        litecode::rotate_token_pair<FrozenClock>(
            store, kSecret, kIssuer, signed_.token,
            "alice", "user",
            kAccessTtl, kRefreshTtl, clock),
        litecode::RefreshTokenInvalidError);
}

TEST(RotateTokenPair, AccessTokenIsNotAcceptedAsRefresh) {
    // Token-confusion defense: a stolen access token must NOT be
    // redeemable at /api/v1/auth/refresh. The verifier's
    // expected_kind=Refresh catches this — the failure surfaces
    // as RefreshTokenInvalidError (signature OK, kind wrong).
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);
    EXPECT_THROW(
        litecode::rotate_token_pair(
            store, kSecret, kIssuer, pair.access_token,
            "alice", "user",
            kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenInvalidError);
}

TEST(RotateTokenPair, WrongSecretIsInvalidError) {
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);
    EXPECT_THROW(
        litecode::rotate_token_pair(
            store, /*secret=*/"some_other_secret_at_least_32_bytes_long_padding_xx",
            kIssuer, pair.refresh_token,
            "alice", "user",
            kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenInvalidError);
}

TEST(RotateTokenPair, RejectsBadTtls) {
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "1", "u", "user", kAccessTtl, kRefreshTtl);
    EXPECT_THROW(
        litecode::rotate_token_pair(store, kSecret, kIssuer, pair.refresh_token,
                                    "u", "user",
                                    /*access_ttl=*/0, kRefreshTtl),
        litecode::RefreshTokenError);
    EXPECT_THROW(
        litecode::rotate_token_pair(store, kSecret, kIssuer, pair.refresh_token,
                                    "u", "user",
                                    kAccessTtl, /*refresh_ttl=*/60),
        litecode::RefreshTokenError);
}

TEST(RotateTokenPair, RejectsEmptyUsernameOrRole) {
    // Defence-in-depth: the refresh token doesn't carry username/role
    // (least privilege), so the route handler MUST look them up from
    // the user row and pass them in. We refuse to silently sign an
    // access token with empty claims.
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);
    EXPECT_THROW(
        litecode::rotate_token_pair(store, kSecret, kIssuer, pair.refresh_token,
                                    /*username=*/"", /*role=*/"user",
                                    kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenError);
    EXPECT_THROW(
        litecode::rotate_token_pair(store, kSecret, kIssuer, pair.refresh_token,
                                    "alice", /*role=*/"",
                                    kAccessTtl, kRefreshTtl),
        litecode::RefreshTokenError);
}

TEST(RotateTokenPair, NewAccessTokenHasCorrectClaims) {
    litecode::InMemoryRefreshTokenStore store;
    const auto initial = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);
    const auto rotated = litecode::rotate_token_pair(
        store, kSecret, kIssuer, initial.refresh_token,
        "alice", "user",
        kAccessTtl, kRefreshTtl);

    // The new access token must be verifiable as an access token
    // (kind=access, has username/role). The route handler is
    // responsible for looking the user row up and passing username/role
    // in — verify that the rotated pair carries them.
    const auto rotated_access = litecode::verify(
        rotated.access_token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_EQ(rotated_access.kind,     litecode::TokenKind::Access);
    EXPECT_EQ(rotated_access.user_id,  "42");
    EXPECT_EQ(rotated_access.username, "alice");
    EXPECT_EQ(rotated_access.role,     "user");

    const auto rotated_refresh = litecode::verify(
        rotated.refresh_token, kSecret, kIssuer, litecode::TokenKind::Refresh);
    EXPECT_EQ(rotated_refresh.kind,    litecode::TokenKind::Refresh);
    EXPECT_EQ(rotated_refresh.user_id, "42");
    EXPECT_TRUE(rotated_refresh.username.empty());
    EXPECT_TRUE(rotated_refresh.role.empty());
}

// ────────────────────────────────────────────────────────────────────────────
//  revoke_refresh_token
// ────────────────────────────────────────────────────────────────────────────

TEST(RevokeRefreshToken, HappyPathAddsToBlacklist) {
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);

    const auto outcome = litecode::revoke_refresh_token(
        store, pair.refresh_token, kSecret, kIssuer, kRefreshTtl);

    EXPECT_TRUE(outcome.parsed);
    EXPECT_TRUE(outcome.revoked);
    EXPECT_TRUE(outcome.user_matched);
    EXPECT_EQ(outcome.jti, pair.refresh_jti);
    EXPECT_TRUE(store.is_revoked(pair.refresh_jti));
}

TEST(RevokeRefreshToken, MalformedTokenIsBestEffort) {
    // Logout should never 500 on a malformed token. parsed=false,
    // revoked=false, the store stays clean.
    litecode::InMemoryRefreshTokenStore store;
    const auto outcome = litecode::revoke_refresh_token(
        store, "not-a-jwt", kSecret, kIssuer, kRefreshTtl);
    EXPECT_FALSE(outcome.parsed);
    EXPECT_FALSE(outcome.revoked);
    EXPECT_EQ(store.size(), 0u);
}

TEST(RevokeRefreshToken, ExpiredTokenIsBestEffort) {
    FrozenClock clock;
    clock.set(std::chrono::system_clock::time_point{});
    litecode::InMemoryRefreshTokenStore store;
    const auto signed_ = litecode::sign_refresh(
        kSecret, kIssuer, "42", /*ttl=*/1);
    clock.advance(std::chrono::seconds(2));

    const auto outcome = litecode::revoke_refresh_token<FrozenClock>(
        store, signed_.token, kSecret, kIssuer, kRefreshTtl, {}, clock);
    EXPECT_FALSE(outcome.parsed);
    EXPECT_FALSE(outcome.revoked);
}

TEST(RevokeRefreshToken, WrongUserIsRefused) {
    // The user_id constraint stops a stolen token from being used
    // to log out a victim's session: the token parses, but its sub
    // doesn't match the caller, so we DON'T add it to the blacklist.
    // The legitimate user's session is untouched.
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);

    const auto outcome = litecode::revoke_refresh_token(
        store, pair.refresh_token, kSecret, kIssuer, kRefreshTtl,
        /*expected_user_id=*/"99");

    EXPECT_TRUE (outcome.parsed);
    EXPECT_FALSE(outcome.user_matched);
    EXPECT_FALSE(outcome.revoked);
    EXPECT_FALSE(store.is_revoked(pair.refresh_jti));
}

TEST(RevokeRefreshToken, MatchingUserIdIsRevoked) {
    litecode::InMemoryRefreshTokenStore store;
    const auto pair = litecode::issue_token_pair(
        kSecret, kIssuer, "42", "alice", "user",
        kAccessTtl, kRefreshTtl);
    const auto outcome = litecode::revoke_refresh_token(
        store, pair.refresh_token, kSecret, kIssuer, kRefreshTtl,
        /*expected_user_id=*/"42");
    EXPECT_TRUE(outcome.parsed);
    EXPECT_TRUE(outcome.user_matched);
    EXPECT_TRUE(outcome.revoked);
    EXPECT_TRUE(store.is_revoked(pair.refresh_jti));
}

TEST(RevokeRefreshToken, TtlIsCappedByMaxTtlSeconds) {
    // Sign a refresh with a 1-day TTL, but cap the revocation TTL
    // at 1 second. The store's internal steady_clock should expire
    // the entry ~1s later, even though the JWT itself is valid for
    // 24h. We use a real sleep because the store tracks expiry with
    // steady_clock (not a caller-provided clock — see design notes).
    litecode::InMemoryRefreshTokenStore store;
    const auto signed_ = litecode::sign_refresh(
        kSecret, kIssuer, "42", /*ttl=*/24 * 3600);

    const auto outcome = litecode::revoke_refresh_token(
        store, signed_.token, kSecret, kIssuer,
        /*max_ttl_seconds=*/1, /*expected_user_id=*/{});
    EXPECT_TRUE(outcome.parsed);
    EXPECT_TRUE(outcome.revoked);
    EXPECT_TRUE (store.is_revoked(signed_.jti));

    // 1.2s real sleep → past the cap; the entry should self-evict.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(store.is_revoked(signed_.jti));
}

TEST(RevokeRefreshToken, EmptyTokenIsBestEffort) {
    litecode::InMemoryRefreshTokenStore store;
    const auto outcome = litecode::revoke_refresh_token(
        store, "", kSecret, kIssuer, kRefreshTtl);
    EXPECT_FALSE(outcome.parsed);
    EXPECT_FALSE(outcome.revoked);
    EXPECT_EQ(store.size(), 0u);
}

// ────────────────────────────────────────────────────────────────────────────
//  Exception hierarchy
// ────────────────────────────────────────────────────────────────────────────

TEST(ExceptionHierarchy, BothSubclassesCatchableAsBase) {
    litecode::RefreshTokenRevokedError  r("revoked");
    litecode::RefreshTokenInvalidError  i("invalid");
    litecode::RefreshTokenError*        pr = &r;
    litecode::RefreshTokenError*        pi = &i;
    EXPECT_STREQ(pr->what(), "revoked");
    EXPECT_STREQ(pi->what(), "invalid");
    // And as std::exception.
    try { throw r; } catch (const std::exception& e) { EXPECT_STREQ(e.what(), "revoked"); }
    try { throw i; } catch (const std::exception& e) { EXPECT_STREQ(e.what(), "invalid"); }
}

// ────────────────────────────────────────────────────────────────────────────
//  Process-wide default store
// ────────────────────────────────────────────────────────────────────────────

TEST(DefaultStore, LazyCreatesInMemoryInstance) {
    // reset at start so we don't share state with earlier tests.
    litecode::reset_refresh_token_store_for_testing();
    litecode::RefreshTokenStore* a = litecode::default_refresh_token_store();
    litecode::RefreshTokenStore* b = litecode::default_refresh_token_store();
    EXPECT_EQ(a, b);   // same instance
    EXPECT_NE(a, nullptr);

    // Sanity: the default is functional.
    a->revoke("default-store-jti", std::chrono::seconds(60));
    EXPECT_TRUE(a->is_revoked("default-store-jti"));
    a->clear();

    litecode::reset_refresh_token_store_for_testing();
}

TEST(DefaultStore, SetDefaultSwapsInstance) {
    litecode::reset_refresh_token_store_for_testing();
    auto custom = std::make_unique<litecode::InMemoryRefreshTokenStore>();
    auto* raw = custom.get();
    litecode::set_default_refresh_token_store(std::move(custom));
    EXPECT_EQ(litecode::default_refresh_token_store(), raw);
    litecode::reset_refresh_token_store_for_testing();
}

TEST(DefaultStore, ResetClearsSlot) {
    // Reset then grab a fresh instance. We DON'T compare pointers
    // (a malloc reuse can return the same address after free); we
    // compare STATE — the recreated instance must not carry over
    // the previous instance's blacklist.
    litecode::reset_refresh_token_store_for_testing();
    auto* a = litecode::default_refresh_token_store();
    a->revoke("persists-across-reset", std::chrono::seconds(60));
    EXPECT_TRUE(a->is_revoked("persists-across-reset"));

    litecode::reset_refresh_token_store_for_testing();
    auto* b = litecode::default_refresh_token_store();
    EXPECT_EQ(b->size(), 0u);
    EXPECT_FALSE(b->is_revoked("persists-across-reset"));

    litecode::reset_refresh_token_store_for_testing();
}

} // namespace
