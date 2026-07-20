// tests/unit/test_audit_log_helpers.cpp
//
// Pure-unit tests for src/db/audit_log_repo.h's validators +
// build_where_clause helper.
//
// The audit_logs list / count paths in admin_audit_log_routes.cpp
// run build_where_clause() under the hood; a typo in the predicate
// / bind order would silently shift every admin filter view. This
// file pins every branch.
//
// All tests are pure C++ — no MySQL, no Docker. They run identically
// on CI lint machines and developer laptops.
//
// Coverage:
//   - validate_action: every reject branch (too short, too long,
//     control char, NUL).
//   - validate_target_type: too long, control char.
//   - validate_target_id: too long, control char.
//   - validate_ip: empty, too long, control char, common IPv4/IPv6.
//   - validate_datetime: too short, too long, control char, valid
//     "YYYY-MM-DD" / "YYYY-MM-DD HH:MM:SS".
//   - clamp_list_filter: limit 0 → default, over-max, negative offset.
//   - action constants: kActionProblemCreate / kActionProblemUpdate /
//     kActionProblemDelete / kActionProblemRestore /
//     kActionProblemBulkImport / kActionUserRoleChange /
//     kActionUserPasswordChange / kActionLoginFailure /
//     kActionLoginLockout pin SPEC §4.2d + §15.6 strings.
//   - length / limit constants pin SPEC §4.2d contract.
//   - exception hierarchy (NotFoundError derives from RepoError).
//   - detail::build_where_clause:
//       * Empty filter → empty text, no binds
//       * Single optional each (admin_id / action / target_type /
//         target_id / since / until)
//       * All optionals together → 6 predicates, 1 int + 5 str binds
//       * Bad since datetime → error_out populated, partial result
//       * Bad until datetime → error_out populated, partial result

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "db/audit_log_repo.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  validate_action
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateAction, AcceptsCommonActions) {
    std::string err;
    EXPECT_TRUE(litecode::validate_action("problem.create", &err));
    EXPECT_TRUE(litecode::validate_action("user.role_change", &err));
    EXPECT_TRUE(litecode::validate_action("auth.login_failure", &err));
}

TEST(ValidateAction, RejectsEmptyAndTooShort) {
    std::string err;
    EXPECT_FALSE(litecode::validate_action("", &err));
    EXPECT_FALSE(litecode::validate_action("a", &err));   // < kMinActionLength
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateAction, RejectsTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_action(
        std::string(litecode::kMaxActionLength + 1, 'a'), &err));
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateAction, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_action("ok\naction", &err));
    EXPECT_FALSE(litecode::validate_action(std::string("a\x01b"), &err));
    EXPECT_FALSE(litecode::validate_action(std::string("a\x7fb"), &err));
    EXPECT_NE(err.find("control characters"), std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  validate_target_type / validate_target_id
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateTargetType, AcceptsEmptyAndCommon) {
    std::string err;
    EXPECT_TRUE(litecode::validate_target_type("", &err));      // empty OK
    EXPECT_TRUE(litecode::validate_target_type("problem", &err));
    EXPECT_TRUE(litecode::validate_target_type("user", &err));
}

TEST(ValidateTargetType, RejectsTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_target_type(
        std::string(litecode::kMaxTargetTypeLength + 1, 'a'), &err));
}

TEST(ValidateTargetType, RejectsControlChars) {
    std::string err;
    EXPECT_FALSE(litecode::validate_target_type("ok\ntype", &err));
}

TEST(ValidateTargetId, AcceptsEmptyAndCommon) {
    std::string err;
    EXPECT_TRUE(litecode::validate_target_id("", &err));
    EXPECT_TRUE(litecode::validate_target_id("two-sum", &err));
    EXPECT_TRUE(litecode::validate_target_id("42", &err));
}

TEST(ValidateTargetId, RejectsTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_target_id(
        std::string(litecode::kMaxTargetIdLength + 1, 'a'), &err));
}

TEST(ValidateTargetId, RejectsControlChars) {
    std::string err;
    EXPECT_FALSE(litecode::validate_target_id("id\n", &err));
}

// ────────────────────────────────────────────────────────────────────────────
//  validate_ip
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateIp, AcceptsCommonAddresses) {
    std::string err;
    EXPECT_TRUE(litecode::validate_ip("127.0.0.1", &err));
    EXPECT_TRUE(litecode::validate_ip("203.0.113.42", &err));
    EXPECT_TRUE(litecode::validate_ip("2001:db8::1", &err));
}

TEST(ValidateIp, RejectsEmpty) {
    std::string err;
    EXPECT_FALSE(litecode::validate_ip("", &err));
    EXPECT_NE(err.find("not be empty"), std::string::npos);
}

TEST(ValidateIp, RejectsTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_ip(
        std::string(litecode::kMaxIpLength + 1, 'a'), &err));
    EXPECT_NE(err.find("<="), std::string::npos);
}

TEST(ValidateIp, RejectsControlChars) {
    std::string err;
    EXPECT_FALSE(litecode::validate_ip(std::string("1.2.3\x01"), &err));
    EXPECT_FALSE(litecode::validate_ip(std::string("a\nb"), &err));
}

// ────────────────────────────────────────────────────────────────────────────
//  validate_datetime
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateDatetime, AcceptsMinAndMaxLength) {
    std::string err;
    // min: "YYYY-MM-DD" (10 chars)
    EXPECT_TRUE(litecode::validate_datetime("2025-01-15", &err));
    // max: "YYYY-MM-DD HH:MM:SS" (19 chars)
    EXPECT_TRUE(litecode::validate_datetime("2025-01-15 09:30:00", &err));
}

TEST(ValidateDatetime, RejectsTooShortAndTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_datetime("2025", &err));
    EXPECT_FALSE(litecode::validate_datetime(
        std::string(litecode::kMaxDatetimeLength + 1, 'a'), &err));
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateDatetime, RejectsControlChars) {
    std::string err;
    EXPECT_FALSE(litecode::validate_datetime("2025-01-15\n", &err));
    EXPECT_FALSE(litecode::validate_datetime(std::string("2025\x01-01-01"), &err));
}

// ────────────────────────────────────────────────────────────────────────────
//  clamp_list_filter
// ────────────────────────────────────────────────────────────────────────────

TEST(ClampAuditListFilter, ZeroLimitBecomesDefault) {
    litecode::AuditListFilter f;
    f.limit  = 0;
    f.offset = 5;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultAuditListLimit);
    EXPECT_EQ(f.offset, 5);
}

TEST(ClampAuditListFilter, OverMaxLimitClampsToMax) {
    litecode::AuditListFilter f;
    f.limit = 9999;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kMaxAuditListLimit);
}

TEST(ClampAuditListFilter, NegativeOffsetClampsToZero) {
    litecode::AuditListFilter f;
    f.offset = -5;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.offset, 0);
}

// ────────────────────────────────────────────────────────────────────────────
//  Action constants — pin SPEC §4.2d + §15.6
// ────────────────────────────────────────────────────────────────────────────

TEST(AuditLogActionConstants, PinSpec) {
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemCreate,     "problem.create");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemUpdate,     "problem.update");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemDelete,     "problem.delete");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemRestore,    "problem.restore");
    EXPECT_STREQ(litecode::audit_log_repo::kActionProblemBulkImport, "problem.bulk_import");
    EXPECT_STREQ(litecode::audit_log_repo::kActionUserRoleChange,    "user.role_change");
    EXPECT_STREQ(litecode::audit_log_repo::kActionUserPasswordChange,"user.password_change");
    EXPECT_STREQ(litecode::audit_log_repo::kActionLoginFailure,      "auth.login_failure");
    EXPECT_STREQ(litecode::audit_log_repo::kActionLoginLockout,      "auth.login_locked");
}

// ────────────────────────────────────────────────────────────────────────────
//  Constants — length / limit caps
// ────────────────────────────────────────────────────────────────────────────

TEST(AuditLogConstants, LengthCaps) {
    EXPECT_EQ(litecode::kMinActionLength, 3u);
    EXPECT_GE(litecode::kMaxActionLength, 32u);
    EXPECT_GE(litecode::kMaxTargetTypeLength, 32u);
    EXPECT_GE(litecode::kMaxTargetIdLength,   64u);
    EXPECT_EQ(litecode::kMaxIpLength, 45u);     // IPv6 max
    EXPECT_EQ(litecode::kMinDatetimeLength, 10u);
    EXPECT_EQ(litecode::kMaxDatetimeLength, 19u);
}

TEST(AuditLogConstants, ListLimits) {
    EXPECT_EQ(litecode::kDefaultAuditListLimit, 20);
    EXPECT_EQ(litecode::kMaxAuditListLimit,     100);
}

// ────────────────────────────────────────────────────────────────────────────
//  Exception hierarchy
// ────────────────────────────────────────────────────────────────────────────

TEST(AuditLogExceptions, NotFoundDerivesFromRepoError) {
    static_assert(std::is_base_of<litecode::AuditLogRepoError,
                                  litecode::AuditLogNotFoundError>::value,
                  "AuditLogNotFoundError must derive from AuditLogRepoError");
    litecode::AuditLogNotFoundError nf("missing");
    EXPECT_NE(std::string(nf.what()).find("missing"), std::string::npos);
}

TEST(AuditLogExceptions, BothDeriveFromStdRuntimeError) {
    static_assert(std::is_base_of<std::runtime_error,
                                  litecode::AuditLogRepoError>::value, "");
    static_assert(std::is_base_of<std::runtime_error,
                                  litecode::AuditLogNotFoundError>::value, "");
}

// ────────────────────────────────────────────────────────────────────────────
//  detail::build_where_clause — the critical SQL builder
// ────────────────────────────────────────────────────────────────────────────

TEST(BuildWhereClause, EmptyFilterProducesEmptyTextAndNoBinds) {
    litecode::AuditListFilter f;
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(w.text.empty());
    EXPECT_TRUE(w.bound_int.empty());
    EXPECT_TRUE(w.bound_str.empty());
}

TEST(BuildWhereClause, AdminIdOnly) {
    litecode::AuditListFilter f;
    f.admin_id = 7;
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(w.text, " WHERE admin_id = ?");
    ASSERT_EQ(w.bound_int.size(), 1u);
    EXPECT_EQ(w.bound_int[0], 7);
    EXPECT_TRUE(w.bound_str.empty());
}

TEST(BuildWhereClause, ActionOnly) {
    litecode::AuditListFilter f;
    f.action = std::string("problem.create");
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(w.text, " WHERE action = ?");
    EXPECT_TRUE(w.bound_int.empty());
    ASSERT_EQ(w.bound_str.size(), 1u);
    EXPECT_EQ(w.bound_str[0], "problem.create");
}

TEST(BuildWhereClause, TargetTypeOnly) {
    litecode::AuditListFilter f;
    f.target_type = std::string("problem");
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_EQ(w.text, " WHERE target_type = ?");
    ASSERT_EQ(w.bound_str.size(), 1u);
    EXPECT_EQ(w.bound_str[0], "problem");
}

TEST(BuildWhereClause, TargetIdOnly) {
    litecode::AuditListFilter f;
    f.target_id = std::string("two-sum");
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_EQ(w.text, " WHERE target_id = ?");
    ASSERT_EQ(w.bound_str.size(), 1u);
    EXPECT_EQ(w.bound_str[0], "two-sum");
}

TEST(BuildWhereClause, SinceOnly) {
    litecode::AuditListFilter f;
    f.since = std::string("2025-01-15");
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(w.text, " WHERE created_at >= ?");
    ASSERT_EQ(w.bound_str.size(), 1u);
    EXPECT_EQ(w.bound_str[0], "2025-01-15");
}

TEST(BuildWhereClause, UntilOnly) {
    litecode::AuditListFilter f;
    f.until = std::string("2025-12-31 23:59:59");
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(w.text, " WHERE created_at <  ?");   // note the spacing
    ASSERT_EQ(w.bound_str.size(), 1u);
    EXPECT_EQ(w.bound_str[0], "2025-12-31 23:59:59");
}

TEST(BuildWhereClause, AllFiltersTogetherChainsWithAnd) {
    litecode::AuditListFilter f;
    f.admin_id    = 42;
    f.action      = std::string("user.role_change");
    f.target_type = std::string("user");
    f.target_id   = std::string("alice");
    f.since       = std::string("2025-01-01");
    f.until       = std::string("2025-12-31 23:59:59");
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());

    // Predicate order matches the implementation (admin_id → action
    // → target_type → target_id → since → until). The leading
    // "WHERE " belongs to the first predicate; subsequent predicates
    // prepend " AND ".
    EXPECT_EQ(w.text,
              " WHERE admin_id = ?"
              " AND action = ?"
              " AND target_type = ?"
              " AND target_id = ?"
              " AND created_at >= ?"
              " AND created_at <  ?");

    ASSERT_EQ(w.bound_int.size(), 1u);
    EXPECT_EQ(w.bound_int[0], 42);
    ASSERT_EQ(w.bound_str.size(), 5u);
    EXPECT_EQ(w.bound_str[0], "user.role_change");
    EXPECT_EQ(w.bound_str[1], "user");
    EXPECT_EQ(w.bound_str[2], "alice");
    EXPECT_EQ(w.bound_str[3], "2025-01-01");
    EXPECT_EQ(w.bound_str[4], "2025-12-31 23:59:59");
}

TEST(BuildWhereClause, BadSinceDatetimePopulatesErrorOut) {
    litecode::AuditListFilter f;
    f.since = std::string("yesterday");  // too short, also non-numeric
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("since:"), std::string::npos);
    // The validator rejected the value; the partial WhereClause is
    // returned with no binds (caller treats this as a hard fail).
    EXPECT_TRUE(w.bound_str.empty());
}

TEST(BuildWhereClause, BadUntilDatetimePopulatesErrorOut) {
    litecode::AuditListFilter f;
    f.until = std::string("2025-13-40 99:99:99");  // 19 chars but control-free
    // (size passes; only length + control-char are checked here — the
    // real datetime parse is MySQL's job).
    std::string err;
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(w.text, " WHERE created_at <  ?");

    // Now exercise the real reject branch — a control char.
    f.until = std::string("2025-01-01 00:00:0\x01");
    auto w2 = litecode::audit_log_repo::detail::build_where_clause(f, &err);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("until:"), std::string::npos);
}

TEST(BuildWhereClause, NullErrorOutIsAllowed) {
    litecode::AuditListFilter f;
    f.since = std::string("yesterday");
    // No error_out → no crash, just returns partial WhereClause.
    auto w = litecode::audit_log_repo::detail::build_where_clause(f, nullptr);
    EXPECT_TRUE(w.bound_str.empty());
}

}  // namespace