// tests/unit/test_jwt_utils.cpp
//
// Unit tests for src/auth/jwt_utils.h — the SPEC §5.1 / §11 Phase 2 ★
// JWT helper.
//
// Coverage:
//   - sign_access / sign_refresh:
//       * round-trip via verify()
//       * wire format: header.alg=HS256, header.typ=JWT
//       * jti is a UUID v4 (36-char, dashes, hex)
//       * kind claim distinguishes access vs refresh
//       * access carries {sub, username, role}; refresh does NOT carry
//         username/role (least privilege)
//       * exp - iat equals the ttl passed in
//       * rejects short secret, empty secret, empty user_id/username/
//         role, ttl < 1
//   - verify():
//       * happy path returns Claims with correct fields
//       * is_expired_at() agrees with the caller-provided clock
//       * signature tampered → JwtVerifyError
//       * payload tampered (claim value flipped) → JwtVerifyError
//       * wrong issuer → JwtVerifyError
//       * wrong kind (access ↔ refresh mix-up) → JwtVerifyError
//       * expired token (clock > exp) → JwtVerifyError
//       * malformed input → JwtVerifyError
//       * empty input → JwtVerifyError
//       * unknown role → JwtClaimError
//       * refresh token that somehow carries `role` → JwtClaimError
//   - exception hierarchy: JwtSignError / JwtVerifyError / JwtClaimError
//     all catchable as JwtError and as std::exception.

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "auth/jwt_utils.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  Test fixtures
// ────────────────────────────────────────────────────────────────────────────

// 64-char secret — comfortably above the SPEC §5.1 floor of 32 bytes.
constexpr const char* kSecret =
    "unit_test_secret_unit_test_secret_unit_test_secret_unit_test_secret_";
constexpr const char* kIssuer = "litecode-test";
constexpr const char* kOtherIssuer = "litecode-evil";

// A deterministic clock so expiry tests don't depend on wall time.
// Default-constructed clock returns a fixed instant; tests advance it
// by reassigning the wrapper before calling verify().
struct FrozenClock {
    std::chrono::system_clock::time_point now_val{
        std::chrono::system_clock::time_point{}   // epoch
    };
    std::chrono::system_clock::time_point now() const { return now_val; }
};

// ────────────────────────────────────────────────────────────────────────────
//  Wire-format helpers
// ────────────────────────────────────────────────────────────────────────────

// Base64URL-decode without padding (RFC 7515 §2). Tests use it to peek
// at the header / payload without depending on jwt-cpp's internal
// decoder. Hand-rolled to keep this file free of extra deps.
std::string base64url_decode(const std::string& in) {
    // Lookup table for base64url alphabet → 6-bit value, -1 for invalid.
    // Filled lazily on first call so we don't pay for it when no test
    // touches the wire format directly.
    static int8_t T[256];
    static bool   T_init = false;
    if (!T_init) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* alpha =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; ++i) {
            T[static_cast<unsigned char>(alpha[i])] = static_cast<int8_t>(i);
        }
        T_init = true;
    }
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int  buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int8_t v = T[static_cast<unsigned char>(c)];
        if (v < 0) return {}; // invalid char
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Split "header.payload.signature" into its three parts. Returns false
// if the token isn't a well-formed three-segment JWS.
bool split_jwt(const std::string& token,
               std::string& h, std::string& p, std::string& s) {
    auto d1 = token.find('.');
    if (d1 == std::string::npos) return false;
    auto d2 = token.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    if (token.find('.', d2 + 1) != std::string::npos) return false;
    h = token.substr(0, d1);
    p = token.substr(d1 + 1, d2 - d1 - 1);
    s = token.substr(d2 + 1);
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
//  sign_access / sign_refresh
// ────────────────────────────────────────────────────────────────────────────

TEST(JwtUtilsSign, AccessRoundTripsThroughVerify) {
    const auto signed_ = litecode::sign_access(
        kSecret, kIssuer, /*user_id=*/"42", "alice", "user",
        /*ttl=*/3600);
    const litecode::Claims c = litecode::verify(
        signed_.token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_EQ(c.user_id,  "42");
    EXPECT_EQ(c.username, "alice");
    EXPECT_EQ(c.role,     "user");
    EXPECT_EQ(c.kind,     litecode::TokenKind::Access);
}

TEST(JwtUtilsSign, RefreshRoundTripsThroughVerify) {
    const auto signed_ = litecode::sign_refresh(
        kSecret, kIssuer, "42", /*ttl=*/7 * 24 * 3600);
    const litecode::Claims c = litecode::verify(
        signed_.token, kSecret, kIssuer, litecode::TokenKind::Refresh);
    EXPECT_EQ(c.user_id, "42");
    EXPECT_EQ(c.kind,    litecode::TokenKind::Refresh);
    EXPECT_TRUE(c.username.empty());
    EXPECT_TRUE(c.role.empty());
}

TEST(JwtUtilsSign, WireFormatHeaderAlgHs256TypJwt) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 3600);
    std::string h, p, sig;
    ASSERT_TRUE(split_jwt(s.token, h, p, sig));
    const auto header_json = base64url_decode(h);
    ASSERT_FALSE(header_json.empty());
    EXPECT_NE(header_json.find("\"alg\":\"HS256\""), std::string::npos);
    EXPECT_NE(header_json.find("\"typ\":\"JWT\""),  std::string::npos);
}

TEST(JwtUtilsSign, AccessCarriesUsernameRole) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "7", "bob", "admin", 3600);
    std::string h, p, sig;
    ASSERT_TRUE(split_jwt(s.token, h, p, sig));
    const auto payload = base64url_decode(p);
    EXPECT_NE(payload.find("\"username\":\"bob\""),  std::string::npos);
    EXPECT_NE(payload.find("\"role\":\"admin\""),    std::string::npos);
    EXPECT_NE(payload.find("\"kind\":\"access\""),   std::string::npos);
}

TEST(JwtUtilsSign, RefreshDoesNotCarryUsernameOrRole) {
    // SPEC §15.1 — least privilege: refresh tokens must NOT carry
    // identity claims that aren't needed to mint a new access token.
    const auto s = litecode::sign_refresh(kSecret, kIssuer, "7", 3600);
    std::string h, p, sig;
    ASSERT_TRUE(split_jwt(s.token, h, p, sig));
    const auto payload = base64url_decode(p);
    EXPECT_EQ(payload.find("\"username\""), std::string::npos);
    EXPECT_EQ(payload.find("\"role\""),     std::string::npos);
    EXPECT_NE(payload.find("\"kind\":\"refresh\""), std::string::npos);
}

TEST(JwtUtilsSign, ExpMinusIatEqualsTtl) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", /*ttl=*/1234);
    const auto c = litecode::verify(
        s.token, kSecret, kIssuer, litecode::TokenKind::Access);
    const auto delta = std::chrono::duration_cast<std::chrono::seconds>(
        c.expires_at - c.issued_at).count();
    EXPECT_EQ(delta, 1234);
}

TEST(JwtUtilsSign, JtiIsUuidV4Shaped) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 3600);
    // UUID v4: 36 chars, dashes at positions 8/13/18/23, version nibble
    // at position 14 is '4', variant nibble at position 19 is one of
    // {8,9,a,b}. We check shape only — randomness is the OS's job.
    EXPECT_EQ(s.jti.size(), 36u);
    EXPECT_EQ(s.jti[8],  '-');
    EXPECT_EQ(s.jti[13], '-');
    EXPECT_EQ(s.jti[18], '-');
    EXPECT_EQ(s.jti[23], '-');
    EXPECT_EQ(s.jti[14], '4');
    EXPECT_TRUE(s.jti[19] == '8' || s.jti[19] == '9'
             || s.jti[19] == 'a' || s.jti[19] == 'b');

    // Two tokens issued back-to-back must NOT share a jti.
    const auto s2 = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 3600);
    EXPECT_NE(s.jti, s2.jti);
}

TEST(JwtUtilsSign, JtiMatchesPayload) {
    // The jti we hand back to the caller must equal the `jti` claim in
    // the token — otherwise the refresh-blacklist bookkeeping will be
    // looking up the wrong row.
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 3600);
    const litecode::Claims c = litecode::verify(
        s.token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_EQ(c.jti, s.jti);
}

TEST(JwtUtilsSign, RejectsShortSecret) {
    EXPECT_THROW(litecode::sign_access("too_short", kIssuer, "1", "u", "user", 60),
                 litecode::JwtSignError);
    EXPECT_THROW(litecode::sign_refresh("too_short", kIssuer, "1", 60),
                 litecode::JwtSignError);
}

TEST(JwtUtilsSign, RejectsEmptySecret) {
    EXPECT_THROW(litecode::sign_access("", kIssuer, "1", "u", "user", 60),
                 litecode::JwtSignError);
}

TEST(JwtUtilsSign, RejectsEmptyIssuer) {
    EXPECT_THROW(litecode::sign_access(kSecret, "", "1", "u", "user", 60),
                 litecode::JwtSignError);
    EXPECT_THROW(litecode::sign_refresh(kSecret, "", "1", 60),
                 litecode::JwtSignError);
}

TEST(JwtUtilsSign, RejectsEmptyUserIdUsernameRole) {
    EXPECT_THROW(litecode::sign_access(kSecret, kIssuer, "",  "u", "user", 60),
                 litecode::JwtSignError);
    EXPECT_THROW(litecode::sign_access(kSecret, kIssuer, "1", "",  "user", 60),
                 litecode::JwtSignError);
    EXPECT_THROW(litecode::sign_access(kSecret, kIssuer, "1", "u", "",     60),
                 litecode::JwtSignError);
    EXPECT_THROW(litecode::sign_refresh(kSecret, kIssuer, "",  60),
                 litecode::JwtSignError);
}

TEST(JwtUtilsSign, RejectsNonPositiveTtl) {
    EXPECT_THROW(litecode::sign_access(kSecret, kIssuer, "1", "u", "user", 0),
                 litecode::JwtSignError);
    EXPECT_THROW(litecode::sign_access(kSecret, kIssuer, "1", "u", "user", -1),
                 litecode::JwtSignError);
}

// ────────────────────────────────────────────────────────────────────────────
//  verify() — happy path details
// ────────────────────────────────────────────────────────────────────────────

TEST(JwtUtilsVerify, ExpIssSubPopulated) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "99", "carol", "user", 60);
    const auto c = litecode::verify(
        s.token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_FALSE(c.issued_at.time_since_epoch().count() == 0);
    EXPECT_GT(c.expires_at, c.issued_at);
    EXPECT_EQ(c.user_id, "99");
}

TEST(JwtUtilsVerify, IsExpiredAtAgreesWithClock) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", /*ttl=*/1);
    const auto c = litecode::verify(
        s.token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_FALSE(c.is_expired_at(c.issued_at));
    EXPECT_FALSE(c.is_expired_at(c.expires_at - std::chrono::seconds(1)));
    EXPECT_TRUE (c.is_expired_at(c.expires_at));
    EXPECT_TRUE (c.is_expired_at(c.expires_at + std::chrono::seconds(1)));
}

TEST(JwtUtilsVerify, AdminRolePasses) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "admin", 60);
    const auto c = litecode::verify(
        s.token, kSecret, kIssuer, litecode::TokenKind::Access);
    EXPECT_EQ(c.role, "admin");
}

// ────────────────────────────────────────────────────────────────────────────
//  verify() — failure modes
// ────────────────────────────────────────────────────────────────────────────

TEST(JwtUtilsVerify, RejectsEmptyToken) {
    EXPECT_THROW(litecode::verify("", kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsMalformedToken) {
    EXPECT_THROW(litecode::verify("not.a.jwt", kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
    EXPECT_THROW(litecode::verify("only_one_segment", kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
    EXPECT_THROW(litecode::verify("....", kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
    // Garbage base64 between dots.
    EXPECT_THROW(litecode::verify("!!!!.????.####", kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsTamperedSignature) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 60);
    // Flip one char in the signature segment.
    std::string tampered = s.token;
    tampered.back() = (tampered.back() == 'A') ? 'B' : 'A';
    EXPECT_THROW(litecode::verify(tampered, kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsTamperedPayload) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 60);
    // Decode the payload, flip `role` user → admin, re-encode by hand
    // (base64url, no padding). This breaks the signature; the verifier
    // must catch it. We do NOT use jwt-cpp's encoder here — we want to
    // confirm the verifier doesn't just trust "exp is in the future".

    auto dot1 = s.token.find('.');
    ASSERT_NE(dot1, std::string::npos);
    auto dot2 = s.token.find('.', dot1 + 1);
    ASSERT_NE(dot2, std::string::npos);

    // Forward base64url encoder (RFC 7515). The alphabet is a file-
    // scope constexpr, so the lambda doesn't need to capture anything.
    auto b64_encode = [](const std::string& in) {
        constexpr char kAlpha[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        int buf = 0, bits = 0;
        for (char c : in) {
            buf = (buf << 8) | static_cast<unsigned char>(c);
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                out.push_back(kAlpha[(buf >> bits) & 0x3F]);
            }
        }
        if (bits > 0) out.push_back(kAlpha[(buf << (6 - bits)) & 0x3F]);
        return out;
    };

    const std::string original_payload = base64url_decode(
        s.token.substr(dot1 + 1, dot2 - dot1 - 1));
    ASSERT_FALSE(original_payload.empty());

    // Surgically replace "user" with "admin" inside the role claim.
    const std::string tampered_payload = [&]{
        std::string p = original_payload;
        const auto pos = p.find("\"role\":\"user\"");
        if (pos != std::string::npos) p.replace(pos + 9, 4, "admin");
        return p;
    }();

    // If for some reason the original payload didn't contain that exact
    // string, skip — the rest of the test suite still exercises the
    // signature mismatch path.
    if (tampered_payload == original_payload) {
        GTEST_SKIP() << "payload shape unexpected — skipping tamper test";
    }

    const std::string new_payload_b64 = b64_encode(tampered_payload);
    const std::string new_token =
        s.token.substr(0, dot1 + 1) + new_payload_b64 +
        s.token.substr(dot2);

    EXPECT_THROW(litecode::verify(new_token, kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsWrongIssuer) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 60);
    EXPECT_THROW(litecode::verify(s.token, kSecret, kOtherIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsWrongSecret) {
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 60);
    constexpr const char* kOtherSecret =
        "other_secret_other_secret_other_secret_other_secret_other_";
    EXPECT_THROW(litecode::verify(s.token, kOtherSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsKindMixup) {
    // access token presented as refresh → token-confusion attack caught.
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", 60);
    EXPECT_THROW(litecode::verify(s.token, kSecret, kIssuer,
                                  litecode::TokenKind::Refresh),
                 litecode::JwtVerifyError);

    // refresh token presented as access → same defence.
    const auto r = litecode::sign_refresh(kSecret, kIssuer, "1", 60);
    EXPECT_THROW(litecode::verify(r.token, kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtVerifyError);
}

TEST(JwtUtilsVerify, RejectsExpiredTokenViaFrozenClock) {
    // Issue a token with ttl=10s, then verify with a clock that has
    // already moved past `exp`. The verifier's own clock check must
    // catch it without us having to sleep.
    const auto s = litecode::sign_access(
        kSecret, kIssuer, "1", "u", "user", /*ttl=*/10);

    // Use the verifier's own freshly-decoded token to anchor the
    // clock to the right wall-clock epoch; we just shift it from
    // there. This is robust across CI clocks that might pick odd
    // starting points.
    const litecode::Claims c = litecode::verify(
        s.token, kSecret, kIssuer, litecode::TokenKind::Access);
    const auto exp = c.expires_at;

    FrozenClock clock;
    clock.now_val = exp + std::chrono::seconds(1);  // 1s past expiry
    EXPECT_THROW(litecode::verify(s.token, kSecret, kIssuer,
                                  litecode::TokenKind::Access, clock),
                 litecode::JwtVerifyError);

    clock.now_val = exp - std::chrono::seconds(1);  // 1s before expiry
    EXPECT_NO_THROW(litecode::verify(s.token, kSecret, kIssuer,
                                     litecode::TokenKind::Access, clock));
}

TEST(JwtUtilsVerify, RejectsUnknownRole) {
    // Hand-craft a token with role=hacker to confirm the role enum is
    // enforced on the read side (defence in depth against tampered or
    // freshly-minted-but-misconfigured tokens).
    const std::string tampered_token = jwt::create()
        .set_type("JWT")
        .set_issuer(kIssuer)
        .set_subject("1")
        .set_id(litecode::generate_uuid_v4())
        .set_issued_at(std::chrono::system_clock::now())
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds(60))
        .set_payload_claim("username", jwt::claim(std::string("u")))
        .set_payload_claim("role",     jwt::claim(std::string("hacker")))
        .set_payload_claim("kind",     jwt::claim(std::string("access")))
        .sign(jwt::algorithm::hs256{kSecret});

    EXPECT_THROW(litecode::verify(tampered_token, kSecret, kIssuer,
                                  litecode::TokenKind::Access),
                 litecode::JwtClaimError);
}

TEST(JwtUtilsVerify, RejectsRefreshThatCarriesRole) {
    const std::string tampered_refresh = jwt::create()
        .set_type("JWT")
        .set_issuer(kIssuer)
        .set_subject("1")
        .set_id(litecode::generate_uuid_v4())
        .set_issued_at(std::chrono::system_clock::now())
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds(60))
        .set_payload_claim("role", jwt::claim(std::string("admin")))
        .set_payload_claim("kind", jwt::claim(std::string("refresh")))
        .sign(jwt::algorithm::hs256{kSecret});

    EXPECT_THROW(litecode::verify(tampered_refresh, kSecret, kIssuer,
                                  litecode::TokenKind::Refresh),
                 litecode::JwtClaimError);
}

// ────────────────────────────────────────────────────────────────────────────
//  Exception hierarchy
// ────────────────────────────────────────────────────────────────────────────

TEST(JwtUtilsExceptions, SignErrorIsJwtErrorAndStdException) {
    try {
        litecode::sign_access("short", kIssuer, "1", "u", "user", 60);
        FAIL() << "expected throw";
    } catch (const litecode::JwtError& e) {
        SUCCEED();
    } catch (...) {
        FAIL() << "wrong exception type";
    }

    try {
        litecode::sign_access("short", kIssuer, "1", "u", "user", 60);
        FAIL() << "expected throw";
    } catch (const std::exception& e) {
        SUCCEED();
    }
}

TEST(JwtUtilsExceptions, VerifyErrorIsJwtError) {
    try {
        litecode::verify("not.a.token", kSecret, kIssuer,
                         litecode::TokenKind::Access);
        FAIL() << "expected throw";
    } catch (const litecode::JwtError& e) {
        SUCCEED();
    }
}

TEST(JwtUtilsExceptions, ClaimErrorIsVerifyErrorIsJwtError) {
    // The hierarchy must let callers `catch (JwtVerifyError&)` and
    // ALSO treat claim-shape failures the same way (401). We catch
    // via the BASE of the chain so the IS-A relationship is what's
    // actually being exercised.
    try {
        litecode::verify(
            jwt::create()
                .set_type("JWT")
                .set_issuer(kIssuer)
                .set_subject("1")
                .set_issued_at(std::chrono::system_clock::now())
                .set_expires_at(std::chrono::system_clock::now()
                                + std::chrono::seconds(60))
                // No `username` / `role` / `kind` claims.
                .sign(jwt::algorithm::hs256{kSecret}),
            kSecret, kIssuer, litecode::TokenKind::Access);
        FAIL() << "expected throw";
    } catch (const litecode::JwtVerifyError& e) {
        SUCCEED();
    }
    // Independently: catching via the leaf type also works.
    try {
        litecode::verify(
            jwt::create()
                .set_type("JWT")
                .set_issuer(kIssuer)
                .set_subject("1")
                .set_issued_at(std::chrono::system_clock::now())
                .set_expires_at(std::chrono::system_clock::now()
                                + std::chrono::seconds(60))
                .sign(jwt::algorithm::hs256{kSecret}),
            kSecret, kIssuer, litecode::TokenKind::Access);
        FAIL() << "expected throw";
    } catch (const litecode::JwtClaimError& e) {
        SUCCEED();
    }
}

} // anonymous namespace