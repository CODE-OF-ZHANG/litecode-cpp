// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — password hashing (Phase 2 ★)
//
// SPEC §4.1 / §11 Phase 2 / §15.1 / A1 acceptance:
//   - bcrypt cost factor 12 (~250 ms per hash on modern x86).
//   - Strength policy (mirror the JS front-end so the two never disagree):
//       * 8 ≤ len(password) ≤ 72
//       * contains at least one ASCII letter (a–z / A–Z)
//       * contains at least one ASCII digit  (0–9)
//   - Login flow: the handler hashes the candidate and constant-time
//     compares against `users.password_hash`. We return a bool (no
//     exception) on the verify path so the route handler can fold
//     "no such user" and "wrong password" into the same UNAUTHORIZED
//     envelope (SPEC §15.1 — no user-enumeration leak).
//   - Storage format: a self-contained `$2b$12$<22c salt><31c hash>`
//     string (~60 bytes) — salt is embedded so we only need ONE
//     column in `users`.
//
// Design notes:
//   - Header-only + inline: matches every other Phase 1 / 2 module
//     (config.h / logger.h / jwt_utils.h / error_handler.h). The
//     bcrypt C glue lives in src/auth/bcrypt/litecode_bcrypt.h which
//     the build wires in via CMake (`bcrypt.c` + `blf.c`).
//   - Three-tier exception hierarchy (PasswordPolicyError /
//     PasswordHashError / PasswordVerifyError) mirrors the
//     jwt_utils.h JwtError / JwtSignError / JwtVerifyError pattern so
//     callers can write a single `catch (const litecode::PasswordError&)`
//     when they don't care which surface failed.
//   - `verify_password()` deliberately does NOT throw. Login handlers
//     must always answer "no" to a malformed/empty hash, never crash
//     the request thread. `hash_password()` throws because every
//     failure mode there is genuinely a server-side bug (RNG down,
//     out of memory, etc.) that the operator must see in logs.
//   - `needs_rehash()` lets login opportunistically upgrade legacy
//     `$2b$10$...` rows to `$2b$12$...` so compute drift doesn't
//     accumulate stale hashes. SPEC §15.1 doesn't require it, but the
//     5-minute add is worth not having to write a one-shot migration.
//
// Usage (registration, in user_repo or auth_routes):
//
//   const std::string hash = litecode::hash_password(req.password);
//   // -> $2b$12$....   (or throws litecode::PasswordPolicyError on a
//   //    weak password, which the handler maps to INVALID_INPUT 400)
//
// Usage (login, in auth_routes):
//
//   if (!litecode::verify_password(req.password, row.password_hash)) {
//       throw litecode::ApiException(401, litecode::ErrorCode::UNAUTHORIZED,
//           "invalid username or password");
//   }
//   if (litecode::needs_rehash(row.password_hash)) {
//       row.password_hash = litecode::hash_password(req.password);
//       user_repo.update(row);  // silent upgrade
//   }

#pragma once

#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "litecode_bcrypt.h"   // bcrypt_gensalt / bcrypt_hashpass / bcrypt_checkpass + sizes

namespace litecode {

// ────────────────────────────────────────────────────────────────────────────
//  Constants
//
//  kBcryptCostFactor — SPEC §4.1 floor. 12 → 2^12 = 4096 key-stretching
//  rounds; expect ~250 ms per hash on a 2026-era x86 core. Going higher
//  would noticeably degrade the 5-10-concurrent-user target from §2.2.
//
//  kMinPasswordLength / kMaxPasswordLength — strength-policy bounds.
//  The 72-byte upper bound is NOT arbitrary: bcrypt $2b$ ignores any
//  key material past 72 bytes (see bcrypt.c bcrypt_hashpass_internal()
//  — key_len is capped at 72). Allowing longer passwords would silently
//  truncate, so we reject them up-front to avoid the surprise.
// ────────────────────────────────────────────────────────────────────────────

inline constexpr int kBcryptCostFactor   = 12;
inline constexpr std::size_t kMinPasswordLength = 8;
inline constexpr std::size_t kMaxPasswordLength = 72;

// ────────────────────────────────────────────────────────────────────────────
//  Exceptions
//
//  Three surfaces, mirroring jwt_utils.h:
//    - PasswordPolicyError — caller's input violated the strength
//                            rules. → INVALID_INPUT 400.
//    - PasswordHashError   — bcrypt primitive failed (RNG down, OOM).
//                            Genuine server-side bug. → INTERNAL_ERROR 500.
//    - PasswordVerifyError — currently never thrown (verify_password
//                            is noexcept + returns false), but the
//                            type is reserved so future implementations
//                            (e.g. argon2 with a verify-throws variant)
//                            can swap in without touching callers.
// ────────────────────────────────────────────────────────────────────────────

class PasswordError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PasswordPolicyError : public PasswordError {
public:
    using PasswordError::PasswordError;
};

class PasswordHashError : public PasswordError {
public:
    using PasswordError::PasswordError;
};

class PasswordVerifyError : public PasswordError {
public:
    using PasswordError::PasswordError;
};

// ────────────────────────────────────────────────────────────────────────────
//  Strength validation
//
//  `validate_password_strength` is the SOURCE OF TRUTH for the policy.
//  The JS front-end mirrors this same check (see web/js/app.js when
//  written) so users get the same error message before the request
//  ever leaves the browser. Both checks MUST agree — divergence here
//  would let users register passwords the server then rejects, which
//  is a classic UX bug.
//
//  The `error_out` parameter is set to a human-readable string on
//  failure so route handlers can surface it directly as the
//  `message` field of the §5.7 INVALID_INPUT envelope.
// ────────────────────────────────────────────────────────────────────────────

inline bool validate_password_strength(std::string_view password,
                                       std::string* error_out = nullptr) {
    if (password.size() < kMinPasswordLength) {
        if (error_out) {
            *error_out = "password must be at least " +
                         std::to_string(kMinPasswordLength) + " characters";
        }
        return false;
    }
    if (password.size() > kMaxPasswordLength) {
        if (error_out) {
            *error_out = "password must be at most " +
                         std::to_string(kMaxPasswordLength) + " characters";
        }
        return false;
    }

    bool has_letter = false;
    bool has_digit  = false;
    for (unsigned char c : password) {
        if (!has_letter && std::isalpha(c) != 0) has_letter = true;
        if (!has_digit  && std::isdigit(c)  != 0) has_digit  = true;
        if (has_letter && has_digit) break;
    }
    if (!has_letter || !has_digit) {
        if (error_out) {
            *error_out = "password must contain both letters and digits";
        }
        return false;
    }
    return true;
}

// Throwing convenience for callers that want a single line of policy
// enforcement. Matches the `require_*` helpers in jwt_utils.h.
inline void require_password_strength(std::string_view password) {
    std::string why;
    if (!validate_password_strength(password, &why)) {
        throw PasswordPolicyError(why);
    }
}

// ────────────────────────────────────────────────────────────────────────────
//  Hashing
//
//  hash_password — produce a self-contained bcrypt hash.
//
//  Output format: `$2b$<NN>$<22c salt><31c hash>` (~60 bytes including
//  the NUL — BCRYPT_HASHSIZE = 64). Salt is embedded so the verifier
//  needs only the single string.
//
//  Cost factor is fixed at kBcryptCostFactor (SPEC §4.1). We do NOT
//  accept it as a parameter because letting routes vary it is a
//  recipe for accidentally downgrading some users' hashes; if a
//  future migration needs a higher factor, the right move is to
//  raise the constant, not pass it in.
//
//  Throws:
//    - PasswordPolicyError if `password` violates the strength policy.
//    - PasswordHashError   if bcrypt_gensalt / bcrypt_hashpass fail.
//                          On any modern box these only fail if the
//                          kernel RNG (/dev/urandom, CryptGenRandom)
//                          is broken — i.e. the box is in serious
//                          trouble and we'd rather log + 500 than
//                          silently fall back to a weaker primitive.
// ────────────────────────────────────────────────────────────────────────────

inline std::string hash_password(std::string_view password) {
    require_password_strength(password);

    // The bcrypt C API takes `const char*` (NUL-terminated). string_view
    // isn't guaranteed NUL-terminated; copy into a std::string for the
    // hop across the C boundary.
    const std::string pw_copy(password);

    char salt[BCRYPT_GENSALT_OUTPUT_SIZE];
    if (bcrypt_gensalt(kBcryptCostFactor, salt) != 0) {
        throw PasswordHashError(
            "bcrypt_gensalt failed (secure RNG unavailable)");
    }

    char hash[BCRYPT_HASHSIZE];
    if (bcrypt_hashpass(pw_copy.c_str(), salt, hash) != 0) {
        throw PasswordHashError(
            "bcrypt_hashpass failed for cost=" +
            std::to_string(kBcryptCostFactor));
    }
    return std::string(hash);
}

// ────────────────────────────────────────────────────────────────────────────
//  Verification
//
//  verify_password — constant-time-ish check of a candidate against a
//  stored bcrypt hash. Returns true iff the password matches.
//
//  noexcept: we never throw. A malformed/empty hash returns false
//  (i.e. "wrong password" from the route's perspective) instead of
//  500-ing the request. This is deliberate — login handlers must
//  not leak the difference between "user not found" and "bad
//  password" (SPEC §15.1 anti-enumeration), and crashing on a
//  garbage hash would be worse than silently failing.
//
//  The bcrypt library itself uses timingsafe_bcmp for the final
//  comparison; the salt-parsing path is NOT constant-time but the
//  only thing it reveals is "is this a valid bcrypt string at all",
//  which is the same answer for every wrong password.
// ────────────────────────────────────────────────────────────────────────────

inline bool verify_password(std::string_view password,
                            std::string_view hash) noexcept {
    if (password.empty() || hash.empty()) return false;

    // Same NUL-termination reason as hash_password().
    const std::string pw_copy(password);
    const std::string hash_copy(hash);

    return bcrypt_checkpass(pw_copy.c_str(), hash_copy.c_str()) == 0;
}

// ────────────────────────────────────────────────────────────────────────────
//  Rehash detection
//
//  extract_cost_factor — parse the leading `$2b$NN$` (or `$2a$NN$` /
//  `$2y$NN$`) prefix of a stored hash and return NN. Returns -1 on
//  any malformed input. Used by `needs_rehash` and by future audit
//  logging that wants to record the work factor for each hash.
//
//  accepts $2a/$2b/$2y so legacy PHP / Python hashes verify cleanly
//  (their cost factors still parse, and bcrypt_checkpass handles all
//  three minor versions).
// ────────────────────────────────────────────────────────────────────────────

inline int extract_cost_factor(std::string_view hash) noexcept {
    // Minimum well-formed bcrypt prefix: "$2b$NN$" = 7 chars.
    if (hash.size() < 7) return -1;
    if (hash[0] != '$')  return -1;
    if (hash[1] != '2')  return -1;
    // Accept $2a (legacy PHP), $2b (current OpenBSD), $2y (PHP 7+).
    const char minor = hash[2];
    if (minor != 'a' && minor != 'b' && minor != 'y') return -1;
    if (hash[3] != '$') return -1;
    if (!std::isdigit(static_cast<unsigned char>(hash[4]))) return -1;
    if (!std::isdigit(static_cast<unsigned char>(hash[5]))) return -1;
    if (hash[6] != '$') return -1;
    const int tens = hash[4] - '0';
    const int ones = hash[5] - '0';
    const int cost = tens * 10 + ones;
    // bcrypt $2x caps workfactor at 31 (2^31 rounds would take forever).
    if (cost < 4 || cost > 31) return -1;
    return cost;
}

// needs_rehash — true iff `hash` is a bcrypt string produced at a cost
// factor LOWER than kBcryptCostFactor. Login handlers should call this
// after a successful verify_password and, if true, re-hash with the
// current cost factor and UPDATE the row.
//
// Returns false on malformed input — the hash is already broken and
// a successful verify against it is impossible, so the rehash question
// is moot (the next failed login will be answered with UNAUTHORIZED
// and nothing more we can do at the bcrypt layer).
inline bool needs_rehash(std::string_view hash) noexcept {
    const int cost = extract_cost_factor(hash);
    return cost > 0 && cost < kBcryptCostFactor;
}

} // namespace litecode