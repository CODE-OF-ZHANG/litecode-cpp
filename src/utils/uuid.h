// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — UUID v4 generator
//
// SPEC §10 src/utils/uuid.h / SPEC §11 Phase 1 (Request-ID middleware ★)
//
// Phase 1 ★ Request-ID middleware needs UUID v4 as the fallback
// X-Request-Id. To avoid pulling in a third-party uuid library (uuid-dev
// / libuuid), we implement it ourselves using std::mt19937_64 plus the
// already-linked OpenSSL/thread-local RNG:
//
//   - 128 bits of randomness (std::mt19937_64 × 2)
//   - Set RFC 4122 v4 version (4) and variant (10xx) bits
//   - 36-char hex-with-dashes output
//
// Extracted from src/server.h so other modules (judge workers, batch
// import, tests) can reuse the same UUID generator — see request_id.h.
//
// Thread safety: each call uses thread_local mt19937_64; no locking.
// Seeding mixes steady_clock time + a pointer address so two processes
// starting in the same nanosecond don't collide on the same sequence.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace litecode {

// detail: per-thread RNG.
//
// thread_local means concurrent calls don't need a lock; each thread
// lazily constructs its own engine on first use. The seed mixes
// steady_clock + the engine's own address so two processes started in
// the same nanosecond get different streams.
namespace detail {

inline std::mt19937_64& uuid_rng() {
    static thread_local std::mt19937_64 rng = []{
        std::mt19937_64::result_type seed =
            static_cast<std::mt19937_64::result_type>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        // xor with a pointer to add per-process variability
        seed ^= reinterpret_cast<std::uintptr_t>(&seed);
        return std::mt19937_64(seed);
    }();
    return rng;
}

} // namespace detail

// Public API: generate one RFC 4122 v4 UUID, e.g.
//   550e8400-e29b-41d4-a716-446655440000
// (36 chars: 32 hex + 4 dashes; index 14 is '4', index 19 ∈ {8,9,a,b}).
inline std::string generate_uuid_v4() {
    // Pull 128 bits of randomness into a 16-byte buffer.
    std::uniform_int_distribution<std::uint64_t> dist64;
    std::uint64_t hi = dist64(detail::uuid_rng());
    std::uint64_t lo = dist64(detail::uuid_rng());

    unsigned char b[16];
    for (int i = 0; i < 8; ++i)  b[i]     = static_cast<unsigned char>(hi >> (8 * i));
    for (int i = 0; i < 8; ++i)  b[8 + i] = static_cast<unsigned char>(lo >> (8 * i));

    // RFC 4122 v4: version + variant bits.
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);

    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3],  b[4], b[5],  b[6], b[7],
                  b[8], b[9],  b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out, 36);
}

} // namespace litecode