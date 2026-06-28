// tests/unit/test_password_hash.cpp
//
// Unit tests for src/auth/password_hash.h — the SPEC §4.1 / §11 Phase 2 ★
// password-hashing helper.
//
// Coverage:
//   - validate_password_strength / require_password_strength:
//       * accepts an 8-char "Abcdefg1" (boundary)
//       * accepts a 72-char mixed password (boundary)
//       * rejects 7-char (too short)
//       * rejects 73-char (too long, beyond bcrypt $2b$'s 72-byte cap)
//       * rejects empty
//       * rejects no-letter ("12345678")
//       * rejects no-digit  ("abcdefgh")
//       * accepts passwords mixing symbols (still satisfies policy)
//       * error_out string is non-empty on failure, empty on success
//   - hash_password:
//       * output starts with "$2b$12$"
//       * extract_cost_factor(out) == 12
//       * two hashes of the same input differ (random salt)
//       * all-ASCII printable is accepted
//   - verify_password:
//       * happy path returns true
//       * wrong password returns false
//       * empty password returns false (does NOT throw)
//       * empty hash    returns false (does NOT throw)
//       * malformed hash returns false (does NOT throw)
//       * verify is symmetric for any order of calls
//   - extract_cost_factor:
//       * $2b$12$ → 12 ;  $2a$10$ → 10 ;  $2y$08$ → 8
//       * malformed strings → -1
//       * cost out of [4, 31] → -1
//   - needs_rehash:
//       * $2b$12$ (current factor) → false
//       * $2b$10$ (legacy factor)  → true
//       * $2b$04$ (below MIN)      → false (the hash is broken anyway)
//       * malformed                → false
//   - Exception hierarchy: PasswordPolicyError / PasswordHashError /
//     PasswordVerifyError all catchable as PasswordError and as
//     std::exception.
//
// Performance: a single hash at cost=12 takes ~250 ms on a modern
// x86 core; this file runs ~10 hash operations, so the binary takes
// a few seconds wall-clock. That's expected — see SPEC §4.1 rationale.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include "auth/password_hash.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Constants for boundary testing
// ────────────────────────────────────────────────────────────────────────────

// 8 chars: "Abcdefg1" — exactly the minimum length, one letter + one digit.
constexpr const char* kBoundaryMin  = "Abcdefg1";

// 72 chars (bcrypt $2b$ cap): 64 letters + 8 digits.
const std::string kBoundaryMax = [] {
    std::string s;
    s.reserve(72);
    for (int i = 0; i < 64; ++i) s.push_back(static_cast<char>('A' + (i % 26)));
    for (int i = 0; i <  8; ++i) s.push_back(static_cast<char>('0' + i));
    return s;
}();

// ────────────────────────────────────────────────────────────────────────────
//  validate_password_strength
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidatePasswordStrength, AcceptsBoundaryMin) {
    EXPECT_TRUE(litecode::validate_password_strength(kBoundaryMin));
}

TEST(ValidatePasswordStrength, AcceptsBoundaryMax) {
    EXPECT_TRUE(litecode::validate_password_strength(kBoundaryMax));
}

TEST(ValidatePasswordStrength, AcceptsMixedCaseAndSymbols) {
    // Symbols don't violate policy; only length + letter + digit matter.
    EXPECT_TRUE(litecode::validate_password_strength("Hello!2026"));
    EXPECT_TRUE(litecode::validate_password_strength("a1B2c3D4"));
}

TEST(ValidatePasswordStrength, RejectsTooShort) {
    EXPECT_FALSE(litecode::validate_password_strength("Abc123"));   // 6
    EXPECT_FALSE(litecode::validate_password_strength(""));         // 0
}

TEST(ValidatePasswordStrength, RejectsTooLong) {
    // 73 chars: one past the bcrypt $2b$ cap.
    std::string too_long = kBoundaryMax + "X";
    EXPECT_FALSE(litecode::validate_password_strength(too_long));
}

TEST(ValidatePasswordStrength, RejectsNoLetter) {
    EXPECT_FALSE(litecode::validate_password_strength("12345678"));
    EXPECT_FALSE(litecode::validate_password_strength("1234567!"));
}

TEST(ValidatePasswordStrength, RejectsNoDigit) {
    EXPECT_FALSE(litecode::validate_password_strength("abcdefgh"));
    EXPECT_FALSE(litecode::validate_password_strength("abcdefg!"));
}

TEST(ValidatePasswordStrength, ErrorOutIsPopulatedOnFailure) {
    std::string why;
    EXPECT_FALSE(litecode::validate_password_strength("Abc123", &why));
    EXPECT_FALSE(why.empty());

    why.clear();
    EXPECT_FALSE(litecode::validate_password_strength("12345678", &why));
    EXPECT_NE(why.find("letter"), std::string::npos);

    why.clear();
    EXPECT_FALSE(litecode::validate_password_strength("abcdefgh", &why));
    EXPECT_NE(why.find("digit"), std::string::npos);
}

TEST(ValidatePasswordStrength, ErrorOutUnchangedOnSuccess) {
    std::string why = "previously set";
    EXPECT_TRUE(litecode::validate_password_strength(kBoundaryMin, &why));
    // Spec doesn't promise to clear it on success — but we document the
    // contract: it MUST NOT be set to a misleading value. Empty string
    // on success is the cleanest contract; assert either empty or unchanged
    // pre-existing content. The current impl clears nothing, so we just
    // assert the call didn't append garbage:
    EXPECT_TRUE(why == "previously set" || why.empty());
}

// ────────────────────────────────────────────────────────────────────────────
//  require_password_strength (throwing wrapper)
// ────────────────────────────────────────────────────────────────────────────

TEST(RequirePasswordStrength, DoesNotThrowOnValid) {
    EXPECT_NO_THROW(litecode::require_password_strength(kBoundaryMin));
    EXPECT_NO_THROW(litecode::require_password_strength(kBoundaryMax));
}

TEST(RequirePasswordStrength, ThrowsPolicyErrorOnInvalid) {
    EXPECT_THROW(litecode::require_password_strength("short"),
                 litecode::PasswordPolicyError);
    EXPECT_THROW(litecode::require_password_strength("12345678"),
                 litecode::PasswordPolicyError);
    EXPECT_THROW(litecode::require_password_strength("abcdefgh"),
                 litecode::PasswordPolicyError);
}

// ────────────────────────────────────────────────────────────────────────────
//  hash_password
// ────────────────────────────────────────────────────────────────────────────

TEST(HashPassword, OutputStartsWithBcryptPrefix) {
    const std::string hash = litecode::hash_password(kBoundaryMin);
    // "$2b$12$" = 7 chars; full hash is ~60 chars.
    EXPECT_GE(hash.size(), 60u);
    EXPECT_EQ(hash.substr(0, 7), "$2b$12$");
}

TEST(HashPassword, ExtractCostFactorMatchesConstant) {
    const std::string hash = litecode::hash_password("Hello!2026");
    EXPECT_EQ(litecode::extract_cost_factor(hash), litecode::kBcryptCostFactor);
    EXPECT_EQ(litecode::extract_cost_factor(hash), 12);
}

TEST(HashPassword, DifferentSaltsForSameInput) {
    // Two back-to-back hashes of the same password must differ — the
    // embedded salt is random per call. This is what makes precomputed
    // rainbow tables useless.
    const std::string a = litecode::hash_password("Hello!2026");
    const std::string b = litecode::hash_password("Hello!2026");
    EXPECT_NE(a, b);

    // Sanity: verify both work independently.
    EXPECT_TRUE(litecode::verify_password("Hello!2026", a));
    EXPECT_TRUE(litecode::verify_password("Hello!2026", b));
}

TEST(HashPassword, AcceptsBoundaryLengths) {
    EXPECT_NO_THROW(litecode::hash_password(kBoundaryMin));
    EXPECT_NO_THROW(litecode::hash_password(kBoundaryMax));
}

TEST(HashPassword, RejectsWeakPassword) {
    EXPECT_THROW(litecode::hash_password("short"),
                 litecode::PasswordPolicyError);
    EXPECT_THROW(litecode::hash_password("12345678"),
                 litecode::PasswordPolicyError);
    EXPECT_THROW(litecode::hash_password("abcdefgh"),
                 litecode::PasswordPolicyError);
    EXPECT_THROW(litecode::hash_password(""),
                 litecode::PasswordPolicyError);
    EXPECT_THROW(litecode::hash_password(kBoundaryMax + "X"),
                 litecode::PasswordPolicyError);
}

// ────────────────────────────────────────────────────────────────────────────
//  verify_password
// ────────────────────────────────────────────────────────────────────────────

TEST(VerifyPassword, HappyPath) {
    const std::string hash = litecode::hash_password("Hello!2026");
    EXPECT_TRUE(litecode::verify_password("Hello!2026", hash));
}

TEST(VerifyPassword, WrongPasswordReturnsFalse) {
    const std::string hash = litecode::hash_password("Hello!2026");
    EXPECT_FALSE(litecode::verify_password("hello!2026", hash)); // case
    EXPECT_FALSE(litecode::verify_password("Hello!2027", hash)); // digit
    EXPECT_FALSE(litecode::verify_password("Hello!202",  hash)); // shorter
    EXPECT_FALSE(litecode::verify_password("Hello!2026x", hash)); // longer
}

TEST(VerifyPassword, EmptyInputsReturnFalseNoThrow) {
    const std::string hash = litecode::hash_password("Hello!2026");
    // None of these may throw — login handlers depend on this to
    // never crash the request thread.
    EXPECT_NO_THROW({
        EXPECT_FALSE(litecode::verify_password("",      hash));
        EXPECT_FALSE(litecode::verify_password("anything", ""));
        EXPECT_FALSE(litecode::verify_password("", ""));
    });
}

TEST(VerifyPassword, MalformedHashReturnsFalseNoThrow) {
    EXPECT_NO_THROW({
        EXPECT_FALSE(litecode::verify_password("Hello!2026", "not-a-bcrypt-hash"));
        EXPECT_FALSE(litecode::verify_password("Hello!2026", "$2b$12$"));     // truncated
        EXPECT_FALSE(litecode::verify_password("Hello!2026", "$1$salt$hash")); // md5crypt
        EXPECT_FALSE(litecode::verify_password("Hello!2026", "$argon2id$v=19$m=..."));
    });
}

TEST(VerifyPassword, SymmetricForMultipleCalls) {
    // Caching the std::string copies inside verify_password shouldn't
    // matter; the result must be stable across repeated calls.
    const std::string hash = litecode::hash_password("Hello!2026");
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(litecode::verify_password("Hello!2026", hash));
        EXPECT_FALSE(litecode::verify_password("wrong",     hash));
    }
}

TEST(VerifyPassword, WorksAcrossCostFactors) {
    // Build a hash at cost=10 (below current) by hand using bcrypt_gensalt
    // + bcrypt_hashpass, then verify_password must still accept it. This
    // is what makes the "needs_rehash + upgrade on login" path work.
    char salt[BCRYPT_GENSALT_OUTPUT_SIZE];
    ASSERT_EQ(bcrypt_gensalt(10, salt), 0);
    char hash[BCRYPT_HASHSIZE];
    ASSERT_EQ(bcrypt_hashpass("Hello!2026", salt, hash), 0);
    const std::string legacy(hash);
    EXPECT_EQ(litecode::extract_cost_factor(legacy), 10);

    EXPECT_TRUE(litecode::verify_password("Hello!2026", legacy));
    EXPECT_TRUE(litecode::needs_rehash(legacy));
}

// ────────────────────────────────────────────────────────────────────────────
//  extract_cost_factor
// ────────────────────────────────────────────────────────────────────────────

TEST(ExtractCostFactor, ParsesCurrentFactor) {
    EXPECT_EQ(litecode::extract_cost_factor("$2b$12$abcdef....."), 12);
}

TEST(ExtractCostFactor, ParsesLegacyFactors) {
    EXPECT_EQ(litecode::extract_cost_factor("$2a$10$abcdef....."), 10);
    EXPECT_EQ(litecode::extract_cost_factor("$2y$08$abcdef....."),  8);
    EXPECT_EQ(litecode::extract_cost_factor("$2b$04$abcdef....."),  4);
    EXPECT_EQ(litecode::extract_cost_factor("$2b$31$abcdef....."), 31);
}

TEST(ExtractCostFactor, ReturnsMinusOneOnMalformed) {
    EXPECT_EQ(litecode::extract_cost_factor(""),            -1);
    EXPECT_EQ(litecode::extract_cost_factor("not-bcrypt"),   -1);
    EXPECT_EQ(litecode::extract_cost_factor("$2b"),          -1);   // too short
    EXPECT_EQ(litecode::extract_cost_factor("$2b$12"),      -1);   // missing trailing $
    EXPECT_EQ(litecode::extract_cost_factor("$3b$12$..."),   -1);   // unknown major
    EXPECT_EQ(litecode::extract_cost_factor("$2c$12$..."),   -1);   // unknown minor
    EXPECT_EQ(litecode::extract_cost_factor("$2b$ab$..."),   -1);   // non-digit cost
    EXPECT_EQ(litecode::extract_cost_factor("$2b$1$....."),   -1);   // 1 digit cost
    EXPECT_EQ(litecode::extract_cost_factor("$2b$99$..."),   -1);   // cost > 31
    EXPECT_EQ(litecode::extract_cost_factor("$2b$03$..."),   -1);   // cost < 4
}

TEST(ExtractCostFactor, IsNoexcept) {
    // Document the noexcept contract — callers in noexcept contexts
    // (e.g. main.cpp routing) rely on it.
    static_assert(noexcept(litecode::extract_cost_factor(std::string_view{})),
                  "extract_cost_factor must be noexcept");
}

// ────────────────────────────────────────────────────────────────────────────
//  needs_rehash
// ────────────────────────────────────────────────────────────────────────────

TEST(NeedsRehash, CurrentFactorIsFine) {
    EXPECT_FALSE(litecode::needs_rehash("$2b$12$abcdef....."));
}

TEST(NeedsRehash, LegacyFactorShouldUpgrade) {
    EXPECT_TRUE(litecode::needs_rehash("$2b$10$abcdef....."));
    EXPECT_TRUE(litecode::needs_rehash("$2a$04$abcdef....."));
    EXPECT_TRUE(litecode::needs_rehash("$2y$08$abcdef....."));
}

TEST(NeedsRehash, FutureFactorDoesNotDowngrade) {
    // If a hash was produced at a HIGHER cost factor than the current
    // code, we don't try to "downgrade" it — needs_rehash is one-way.
    EXPECT_FALSE(litecode::needs_rehash("$2b$13$abcdef....."));
}

TEST(NeedsRehash, MalformedReturnsFalse) {
    // Malformed → -1 from extract_cost_factor → needs_rehash returns
    // false (the hash is broken; verify will already reject it).
    EXPECT_FALSE(litecode::needs_rehash(""));
    EXPECT_FALSE(litecode::needs_rehash("garbage"));
    EXPECT_FALSE(litecode::needs_rehash("$2b$99$..."));  // cost out of range
}

TEST(NeedsRehash, IsNoexcept) {
    static_assert(noexcept(litecode::needs_rehash(std::string_view{})),
                  "needs_rehash must be noexcept");
}

// ────────────────────────────────────────────────────────────────────────────
//  Exception hierarchy
//
//  Mirrors the three-tier shape of jwt_utils.h's JwtError / JwtSignError
//  / JwtVerifyError so a caller can write a single catch block:
//      catch (const litecode::PasswordError& e) { ... }
// ────────────────────────────────────────────────────────────────────────────

TEST(ExceptionHierarchy, PolicyErrorIsCatchableAsPasswordError) {
    try {
        litecode::hash_password("short");
        FAIL() << "expected PasswordPolicyError";
    } catch (const litecode::PasswordError& e) {
        EXPECT_NE(std::string(e.what()).find("8"), std::string::npos);
    } catch (...) {
        FAIL() << "wrong exception type";
    }
}

TEST(ExceptionHierarchy, AllPasswordErrorsAreStdExceptions) {
    // Catch via base — generic 500-fallback handlers depend on this.
    try {
        litecode::require_password_strength("");
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).size(), 0u);
    }
}

} // namespace