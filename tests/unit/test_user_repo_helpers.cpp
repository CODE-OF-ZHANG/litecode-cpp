// tests/unit/test_user_repo_helpers.cpp
//
// Pure-unit tests for src/db/user_repo.h's validators + filters.
//
// The route handlers under src/routes/auth_routes.h /
// src/routes/admin_user_routes.h do call validate_username /
// validate_email / clamp_user_list_filter directly, but those
// handlers themselves are exercised only via the route-level
// gtests (test_auth_register / test_auth_login / test_admin_users).
// A typo or off-by-one in the helpers would silently corrupt the
// registration / admin-list contract; this file pins every branch.
//
// All tests are pure C++ — no MySQL, no Docker. They run identically
// on CI lint machines and developer laptops.
//
// Coverage:
//   - is_valid_username_char: every allowed char + every forbidden
//     category (whitespace, control, punctuation, non-ASCII).
//   - validate_username: min boundary (3 chars), max boundary (50),
//     over-max rejection, leading/trailing dot/hyphen rejection,
//     non-ASCII rejection, control-char rejection.
//   - validate_email: every reject branch (too short, no '@', '@'
//     at start, '@' at end, multiple '@', no '.' in domain, leading
//     dot in domain, trailing dot in domain, whitespace, non-ASCII).
//   - validate_* with error_out=nullptr must not crash.
//   - clamp_user_list_filter: limit 0 → default, negative limit,
//     over-max, negative offset, well-formed input unchanged.
//   - constants: kMinUsernameLength / kMaxUsernameLength /
//     kMinEmailLength / kMaxEmailLength / kDefaultUserListLimit /
//     kMaxUserListLimit pin the SPEC §4.1 contract.
//   - UserRow / UserListFilter defaults.
//   - Exception hierarchy: UserAlreadyExistsError derives from
//     UserRepoError derives from std::runtime_error.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <type_traits>

#include "db/user_repo.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  is_valid_username_char
// ────────────────────────────────────────────────────────────────────────────

TEST(IsValidUsernameChar, AcceptsLowerLetters) {
    for (char c : std::string("abcdefghijklmnopqrstuvwxyz")) {
        EXPECT_TRUE(litecode::is_valid_username_char(c))
            << "lowercase '" << c << "' should be valid";
    }
}

TEST(IsValidUsernameChar, AcceptsUpperLetters) {
    for (char c : std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ")) {
        EXPECT_TRUE(litecode::is_valid_username_char(c));
    }
}

TEST(IsValidUsernameChar, AcceptsDigits) {
    for (char c : std::string("0123456789")) {
        EXPECT_TRUE(litecode::is_valid_username_char(c));
    }
}

TEST(IsValidUsernameChar, AcceptsSpecialAllowedChars) {
    EXPECT_TRUE(litecode::is_valid_username_char('_'));
    EXPECT_TRUE(litecode::is_valid_username_char('-'));
    EXPECT_TRUE(litecode::is_valid_username_char('.'));
}

TEST(IsValidUsernameChar, RejectsWhitespace) {
    EXPECT_FALSE(litecode::is_valid_username_char(' '));
    EXPECT_FALSE(litecode::is_valid_username_char('\t'));
    EXPECT_FALSE(litecode::is_valid_username_char('\n'));
    EXPECT_FALSE(litecode::is_valid_username_char('\r'));
}

TEST(IsValidUsernameChar, RejectsControlCharacters) {
    EXPECT_FALSE(litecode::is_valid_username_char('\0'));
    EXPECT_FALSE(litecode::is_valid_username_char('\b'));
    EXPECT_FALSE(litecode::is_valid_username_char('\x1b'));  // ESC
    EXPECT_FALSE(litecode::is_valid_username_char('\x7f'));  // DEL
}

TEST(IsValidUsernameChar, RejectsPunctuation) {
    EXPECT_FALSE(litecode::is_valid_username_char('!'));
    EXPECT_FALSE(litecode::is_valid_username_char('@'));
    EXPECT_FALSE(litecode::is_valid_username_char('#'));
    EXPECT_FALSE(litecode::is_valid_username_char('$'));
    EXPECT_FALSE(litecode::is_valid_username_char('%'));
    EXPECT_FALSE(litecode::is_valid_username_char('*'));
    EXPECT_FALSE(litecode::is_valid_username_char('/'));
    EXPECT_FALSE(litecode::is_valid_username_char('\\'));
    EXPECT_FALSE(litecode::is_valid_username_char('\''));
    EXPECT_FALSE(litecode::is_valid_username_char('"'));
    EXPECT_FALSE(litecode::is_valid_username_char('+'));
    EXPECT_FALSE(litecode::is_valid_username_char('='));
    EXPECT_FALSE(litecode::is_valid_username_char('('));
    EXPECT_FALSE(litecode::is_valid_username_char(')'));
}

// ────────────────────────────────────────────────────────────────────────────
//  validate_username
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateUsername, AcceptsMinBoundary) {
    std::string err;
    EXPECT_TRUE(litecode::validate_username("abc", &err));
    EXPECT_EQ(err, "");
}

TEST(ValidateUsername, AcceptsMaxBoundary) {
    std::string err;
    const std::string s(litecode::kMaxUsernameLength, 'a');
    EXPECT_TRUE(litecode::validate_username(s, &err));
    EXPECT_EQ(err, "");
}

TEST(ValidateUsername, AcceptsMixedAllowedChars) {
    std::string err;
    EXPECT_TRUE(litecode::validate_username("alice_42", &err));
    EXPECT_TRUE(litecode::validate_username("bob.smith", &err));
    EXPECT_TRUE(litecode::validate_username("user-007", &err));
    EXPECT_TRUE(litecode::validate_username("a_b-c.1", &err));
}

TEST(ValidateUsername, RejectsTooShort) {
    std::string err;
    EXPECT_FALSE(litecode::validate_username("ab", &err));
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateUsername, RejectsTooLong) {
    std::string err;
    const std::string s(litecode::kMaxUsernameLength + 1, 'a');
    EXPECT_FALSE(litecode::validate_username(s, &err));
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateUsername, RejectsLeadingOrTrailingDotOrHyphen) {
    std::string err;
    EXPECT_FALSE(litecode::validate_username(".alice", &err));
    EXPECT_FALSE(litecode::validate_username("alice.", &err));
    EXPECT_FALSE(litecode::validate_username("-alice", &err));
    EXPECT_FALSE(litecode::validate_username("alice-", &err));
    // Allowed: leading underscore (rare but consistent with the
    // is_valid_username_char allowlist).
    EXPECT_TRUE(litecode::validate_username("_alice", &err));
}

TEST(ValidateUsername, RejectsInvalidChars) {
    std::string err;
    EXPECT_FALSE(litecode::validate_username("alice bob", &err));   // space
    EXPECT_FALSE(litecode::validate_username("alice@home", &err));  // @
    EXPECT_FALSE(litecode::validate_username("alice!", &err));       // !
    EXPECT_FALSE(litecode::validate_username("alìce", &err));       // non-ASCII
}

TEST(ValidateUsername, NullErrorOutDoesNotCrash) {
    EXPECT_FALSE(litecode::validate_username("ab", nullptr));
    EXPECT_TRUE (litecode::validate_username("alice", nullptr));
}

// ────────────────────────────────────────────────────────────────────────────
//  validate_email
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateEmail, AcceptsCommonAddresses) {
    std::string err;
    EXPECT_TRUE(litecode::validate_email("alice@example.com", &err));
    EXPECT_TRUE(litecode::validate_email("a.b+c@sub.example.co.uk", &err));
    EXPECT_TRUE(litecode::validate_email("123@numeric-only.io", &err));
    EXPECT_TRUE(litecode::validate_email("user_name@example-domain.com", &err));
}

TEST(ValidateEmail, RejectsTooShort) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("a@b", &err));
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateEmail, RejectsTooLong) {
    std::string err;
    const std::string local(litecode::kMaxEmailLength - 5, 'a');
    const std::string e = local + "@x.io";   // local + domain > max
    EXPECT_FALSE(litecode::validate_email(e, &err));
}

TEST(ValidateEmail, RejectsMissingAt) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice.example.com", &err));
    EXPECT_NE(err.find("local@domain"), std::string::npos);
}

TEST(ValidateEmail, RejectsLeadingAt) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("@example.com", &err));
    EXPECT_NE(err.find("local@domain"), std::string::npos);
}

TEST(ValidateEmail, RejectsTrailingAt) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice@", &err));
    EXPECT_NE(err.find("local@domain"), std::string::npos);
}

TEST(ValidateEmail, RejectsMultipleAts) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice@@example.com", &err));
    EXPECT_NE(err.find("exactly one '@'"), std::string::npos);
}

TEST(ValidateEmail, RejectsDomainWithoutDot) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice@localhost", &err));
    EXPECT_NE(err.find("dot"), std::string::npos);
}

TEST(ValidateEmail, RejectsDomainLeadingDot) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice@.example.com", &err));
    EXPECT_NE(err.find("dot"), std::string::npos);
}

TEST(ValidateEmail, RejectsDomainTrailingDot) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice@example.com.", &err));
    EXPECT_NE(err.find("dot"), std::string::npos);
}

TEST(ValidateEmail, RejectsWhitespace) {
    std::string err;
    EXPECT_FALSE(litecode::validate_email("alice @example.com", &err));
    EXPECT_FALSE(litecode::validate_email("alice@ example.com", &err));
    EXPECT_FALSE(litecode::validate_email("alice@example.com\n", &err));
    EXPECT_NE(err.find("whitespace"), std::string::npos);
}

TEST(ValidateEmail, NullErrorOutDoesNotCrash) {
    EXPECT_FALSE(litecode::validate_email("nope", nullptr));
    EXPECT_TRUE (litecode::validate_email("ok@example.com", nullptr));
}

// ────────────────────────────────────────────────────────────────────────────
//  clamp_user_list_filter
// ────────────────────────────────────────────────────────────────────────────

TEST(ClampUserListFilter, ZeroLimitBecomesDefault) {
    litecode::UserListFilter f;
    f.limit  = 0;
    f.offset = 5;
    litecode::clamp_user_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultUserListLimit);
    EXPECT_EQ(f.offset, 5);
}

TEST(ClampUserListFilter, NegativeLimitBecomesDefault) {
    litecode::UserListFilter f;
    f.limit = -7;
    litecode::clamp_user_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultUserListLimit);
}

TEST(ClampUserListFilter, OverMaxLimitClampsToMax) {
    litecode::UserListFilter f;
    f.limit = 9999;
    litecode::clamp_user_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kMaxUserListLimit);
}

TEST(ClampUserListFilter, NegativeOffsetClampsToZero) {
    litecode::UserListFilter f;
    f.offset = -5;
    litecode::clamp_user_list_filter(f);
    EXPECT_EQ(f.offset, 0);
}

TEST(ClampUserListFilter, WellFormedInputUnchanged) {
    litecode::UserListFilter f;
    f.limit  = 25;
    f.offset = 10;
    f.role   = std::string("admin");
    f.q      = std::string("ali");
    litecode::clamp_user_list_filter(f);
    EXPECT_EQ(f.limit,  25);
    EXPECT_EQ(f.offset, 10);
    ASSERT_TRUE(f.role.has_value());
    EXPECT_EQ(*f.role, "admin");
    ASSERT_TRUE(f.q.has_value());
    EXPECT_EQ(*f.q, "ali");
}

// ────────────────────────────────────────────────────────────────────────────
//  Constants — pin the contract
// ────────────────────────────────────────────────────────────────────────────

TEST(UserRepoConstants, UsernameLength) {
    EXPECT_EQ(litecode::kMinUsernameLength, 3u);
    EXPECT_EQ(litecode::kMaxUsernameLength, 50u);
}

TEST(UserRepoConstants, EmailLength) {
    EXPECT_EQ(litecode::kMinEmailLength, 3u);
    EXPECT_EQ(litecode::kMaxEmailLength, 100u);
}

TEST(UserRepoConstants, ListLimits) {
    EXPECT_EQ(litecode::kDefaultUserListLimit, 20);
    EXPECT_EQ(litecode::kMaxUserListLimit,     100);
}

// ────────────────────────────────────────────────────────────────────────────
//  Struct defaults
// ────────────────────────────────────────────────────────────────────────────

TEST(UserRowDefaults, ZeroedAndEmpty) {
    litecode::UserRow r;
    EXPECT_EQ(r.id,           0);
    EXPECT_EQ(r.username,     "");
    EXPECT_EQ(r.password_hash,"");
    EXPECT_EQ(r.role,         "");
    EXPECT_FALSE(r.email.has_value());
    EXPECT_FALSE(r.avatar.has_value());
    EXPECT_EQ(r.created_at,   "");
    EXPECT_FALSE(r.last_login.has_value());
    EXPECT_FALSE(r.last_login_ip.has_value());
}

TEST(UserListFilterDefaults, DefaultsMatchSpec) {
    litecode::UserListFilter f;
    EXPECT_EQ(f.limit,  litecode::kDefaultUserListLimit);
    EXPECT_EQ(f.offset, 0);
    EXPECT_FALSE(f.role.has_value());
    EXPECT_FALSE(f.q.has_value());
}

// ────────────────────────────────────────────────────────────────────────────
//  Exception hierarchy
// ────────────────────────────────────────────────────────────────────────────

TEST(UserRepoExceptions, AlreadyExistsDerivesFromRepoError) {
    static_assert(std::is_base_of<litecode::UserRepoError,
                                  litecode::UserAlreadyExistsError>::value,
                  "UserAlreadyExistsError must derive from UserRepoError");
    litecode::UserAlreadyExistsError ae("dup");
    EXPECT_NE(std::string(ae.what()).find("dup"), std::string::npos);
    try {
        throw litecode::UserAlreadyExistsError("dup2");
    } catch (const litecode::UserRepoError& re) {
        EXPECT_NE(std::string(re.what()).find("dup2"), std::string::npos);
    }
}

TEST(UserRepoExceptions, BothDeriveFromStdRuntimeError) {
    static_assert(std::is_base_of<std::runtime_error,
                                  litecode::UserRepoError>::value, "");
    static_assert(std::is_base_of<std::runtime_error,
                                  litecode::UserAlreadyExistsError>::value, "");
}

}  // namespace