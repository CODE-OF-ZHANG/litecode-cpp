// tests/unit/test_uuid.cpp
//
// Unit tests for src/utils/uuid.h — the UUID v4 generator used by the
// request-id middleware (and any future batch-import / judge-worker that
// needs random opaque ids).
//
// Coverage:
//   - Shape: 36 chars, dashes at fixed positions
//   - Version nibble is '4' (RFC 4122 v4)
//   - Variant nibble is in {8,9,a,b}
//   - Hex-only otherwise (no surprising chars)
//   - Distinctness over many draws (no RNG birthday collisions in 10k)
//   - Thread-safety: 8 threads × 1000 draws → all 8000 ids distinct,
//     all of them valid v4 shape
//
// The test binary links nothing beyond gtest_main.

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "utils/uuid.h"

namespace {

// Dashes at canonical positions; the version nibble must be '4';
// the variant nibble must be one of the RFC 4122 reserved values.
void expect_valid_v4_shape(const std::string& id) {
    ASSERT_EQ(id.size(), 36u) << "wrong length: " << id;
    EXPECT_EQ(id[8],  '-') << "missing dash at 8: "  << id;
    EXPECT_EQ(id[13], '-') << "missing dash at 13: " << id;
    EXPECT_EQ(id[18], '-') << "missing dash at 18: " << id;
    EXPECT_EQ(id[23], '-') << "missing dash at 23: " << id;
    EXPECT_EQ(id[14], '4') << "version nibble not 4: " << id;

    const char v = id[19];
    EXPECT_TRUE(v == '8' || v == '9' || v == 'a' || v == 'b')
        << "variant nibble not in {8,9,a,b}: " << id;

    // Hex-only elsewhere.
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const char c = id[i];
        const bool hex = (c >= '0' && c <= '9')
                      || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(hex) << "non-hex char at " << i << ": " << id;
    }
}

TEST(UuidV4, SingleIdHasValidShape) {
    expect_valid_v4_shape(litecode::generate_uuid_v4());
}

TEST(UuidV4, ManyIdsAllValidShape) {
    for (int i = 0; i < 5000; ++i) {
        expect_valid_v4_shape(litecode::generate_uuid_v4());
    }
}

TEST(UuidV4, DistinctAcrossDraws) {
    // 122 bits of effective randomness → birthday collision needs ~2^61.
    // 10k draws is trivially safe; this pins the RNG isn't accidentally
    // constant (e.g. a forgotten `static` on a captured-by-value lambda).
    std::set<std::string> seen;
    for (int i = 0; i < 10000; ++i) {
        seen.insert(litecode::generate_uuid_v4());
    }
    EXPECT_EQ(seen.size(), 10000u);
}

TEST(UuidV4, ConcurrentDrawsAreDistinct) {
    // Each thread uses its own thread_local mt19937_64; verify the
    // thread_local isn't accidentally a shared resource, and that the
    // per-thread seeding produces non-overlapping sequences.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;

    std::vector<std::thread> ts;
    std::vector<std::vector<std::string>> buckets(kThreads);
    std::atomic<int> ready{0};

    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t, &buckets, &ready, kThreads, kPerThread]{
            // Stagger starting so seeding differs across threads.
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) < kThreads) {
                std::this_thread::yield();
            }
            buckets[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                buckets[t].push_back(litecode::generate_uuid_v4());
            }
        });
    }
    for (auto& th : ts) th.join();

    // All ids from all threads must be valid shape + globally unique.
    std::set<std::string> global;
    for (int t = 0; t < kThreads; ++t) {
        EXPECT_EQ(buckets[t].size(), static_cast<std::size_t>(kPerThread));
        for (const auto& id : buckets[t]) {
            expect_valid_v4_shape(id);
            auto [_, inserted] = global.insert(id);
            EXPECT_TRUE(inserted) << "duplicate across threads: " << id;
        }
    }
    EXPECT_EQ(global.size(),
              static_cast<std::size_t>(kThreads) * kPerThread);
}

} // anonymous namespace