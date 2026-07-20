// tests/unit/test_problem_repo_helpers.cpp
//
// Pure-unit tests for the validators + filter helpers in:
//
//   src/db/problem_repo.h         — Phase 3 ★ slug/difficulty/title/
//                                   time_limit/memory_limit validators
//                                   + clamp_list_filter
//   src/db/problem_revisions_repo.h — Phase 3 ★ follow-up (v1.2.12)
//                                   revision snapshot validators
//                                   (revision_no / summary / action /
//                                   editor_username / slug / title /
//                                   difficulty / time_limit /
//                                   memory_limit / description / ip)
//                                   + looks_like_dup_key_error heuristic
//
// The route handlers under src/routes/admin_problem_routes.h /
// admin_bulk_import_routes.h exercise these helpers transitively, but
// a dedicated home keeps every branch pinned: an off-by-one in a
// validator would silently corrupt the admin write / bulk-import
// contract.
//
// All tests are pure C++ — no MySQL, no Docker. They run identically
// on CI lint machines and developer laptops.

#include <gtest/gtest.h>

#include <string>

#include "db/problem_repo.h"
#include "db/problem_revisions_repo.h"

// ============================================================================
//  problem_repo.h — slug / difficulty / title / time / memory / clamp
// ============================================================================

namespace {

TEST(IsValidSlugChar, AcceptsLowercaseAndDigitsAndHyphen) {
    for (char c : std::string("abcdefghijklmnopqrstuvwxyz")) {
        EXPECT_TRUE(litecode::is_valid_slug_char(c));
    }
    for (char c : std::string("0123456789")) {
        EXPECT_TRUE(litecode::is_valid_slug_char(c));
    }
    EXPECT_TRUE(litecode::is_valid_slug_char('-'));
}

TEST(IsValidSlugChar, RejectsUppercaseAndOther) {
    // Slugs are lowercase-only (SPEC §4.2); uppercase must reject.
    EXPECT_FALSE(litecode::is_valid_slug_char('A'));
    EXPECT_FALSE(litecode::is_valid_slug_char('Z'));
    EXPECT_FALSE(litecode::is_valid_slug_char('_'));
    EXPECT_FALSE(litecode::is_valid_slug_char('.'));
    EXPECT_FALSE(litecode::is_valid_slug_char(' '));
    EXPECT_FALSE(litecode::is_valid_slug_char('!'));
    EXPECT_FALSE(litecode::is_valid_slug_char('\0'));
}

TEST(ValidateSlug, AcceptsMinAndMaxBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::validate_slug(
        std::string(litecode::kMinSlugLength, 'a'), &err));
    EXPECT_TRUE(litecode::validate_slug(
        std::string(litecode::kMaxSlugLength, 'a'), &err));
}

TEST(ValidateSlug, RejectsTooShortAndTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::validate_slug(
        std::string(litecode::kMinSlugLength - 1, 'a'), &err));
    EXPECT_FALSE(litecode::validate_slug(
        std::string(litecode::kMaxSlugLength + 1, 'a'), &err));
}

TEST(ValidateSlug, RejectsLeadingOrTrailingHyphen) {
    std::string err;
    EXPECT_FALSE(litecode::validate_slug("-foo", &err));
    EXPECT_FALSE(litecode::validate_slug("foo-", &err));
    EXPECT_FALSE(litecode::validate_slug("-", &err));
}

TEST(ValidateSlug, RejectsInvalidCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::validate_slug("Foo", &err));      // uppercase
    EXPECT_FALSE(litecode::validate_slug("foo bar", &err));  // space
    EXPECT_FALSE(litecode::validate_slug("foo!", &err));     // punctuation
    EXPECT_FALSE(litecode::validate_slug("foo_bar", &err));  // underscore
}

TEST(IsValidDifficulty, AcceptsAllowedValues) {
    EXPECT_TRUE(litecode::is_valid_difficulty("easy"));
    EXPECT_TRUE(litecode::is_valid_difficulty("medium"));
    EXPECT_TRUE(litecode::is_valid_difficulty("hard"));
}

TEST(IsValidDifficulty, RejectsUnknownValues) {
    EXPECT_FALSE(litecode::is_valid_difficulty(""));
    EXPECT_FALSE(litecode::is_valid_difficulty("Easy"));     // case
    EXPECT_FALSE(litecode::is_valid_difficulty("EASY"));
    EXPECT_FALSE(litecode::is_valid_difficulty("extreme"));
    EXPECT_FALSE(litecode::is_valid_difficulty("trivial"));
}

TEST(ValidateDifficulty, ErrorOutOnUnknown) {
    std::string err;
    EXPECT_FALSE(litecode::validate_difficulty("epic", &err));
    EXPECT_NE(err.find("easy, medium, hard"), std::string::npos);
}

TEST(ValidateTitle, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::validate_title(
        std::string(litecode::kMinTitleLength, 'a'), &err));
    EXPECT_TRUE(litecode::validate_title(
        std::string(litecode::kMaxTitleLength, 'a'), &err));
}

TEST(ValidateTitle, RejectsBoundaries) {
    std::string err;
    EXPECT_FALSE(litecode::validate_title(
        std::string(litecode::kMinTitleLength - 1, 'a'), &err));
    EXPECT_FALSE(litecode::validate_title(
        std::string(litecode::kMaxTitleLength + 1, 'a'), &err));
}

TEST(ValidateTimeLimit, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::validate_time_limit(litecode::kMinTimeLimitMs, &err));
    EXPECT_TRUE(litecode::validate_time_limit(litecode::kMaxTimeLimitMs, &err));
    EXPECT_TRUE(litecode::validate_time_limit(1000, &err));
}

TEST(ValidateTimeLimit, RejectsOutOfRange) {
    std::string err;
    EXPECT_FALSE(litecode::validate_time_limit(0, &err));
    EXPECT_FALSE(litecode::validate_time_limit(-1, &err));
    EXPECT_FALSE(litecode::validate_time_limit(
        litecode::kMaxTimeLimitMs + 1, &err));
    EXPECT_NE(err.find("ms"), std::string::npos);
}

TEST(ValidateMemoryLimit, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::validate_memory_limit(litecode::kMinMemoryLimitMb, &err));
    EXPECT_TRUE(litecode::validate_memory_limit(litecode::kMaxMemoryLimitMb, &err));
    EXPECT_TRUE(litecode::validate_memory_limit(128, &err));
}

TEST(ValidateMemoryLimit, RejectsOutOfRange) {
    std::string err;
    EXPECT_FALSE(litecode::validate_memory_limit(0, &err));
    EXPECT_FALSE(litecode::validate_memory_limit(
        litecode::kMaxMemoryLimitMb + 1, &err));
    EXPECT_NE(err.find("MB"), std::string::npos);
}

TEST(ClampProblemListFilter, ZeroLimitBecomesDefault) {
    litecode::ProblemListFilter f;
    f.limit  = 0;
    f.offset = 5;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kDefaultListLimit);
    EXPECT_EQ(f.offset, 5);
}

TEST(ClampProblemListFilter, OverMaxLimitClamps) {
    litecode::ProblemListFilter f;
    f.limit = 9999;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kMaxListLimit);
}

TEST(ClampProblemListFilter, NegativeOffsetClampsToZero) {
    litecode::ProblemListFilter f;
    f.offset = -1;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.offset, 0);
}

// ============================================================================
//  problem_revisions_repo.h — Phase 3 ★ follow-up (v1.2.12)
// ============================================================================

TEST(ProblemRevisionsValidateRevisionNo, AcceptsSentinelZero) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_revision_no(0, &err));
}

TEST(ProblemRevisionsValidateRevisionNo, AcceptsInRange) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_revision_no(1, &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_revision_no(
        litecode::problem_revisions_repo::kMaxRevisionNo, &err));
}

TEST(ProblemRevisionsValidateRevisionNo, RejectsNegative) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_revision_no(-1, &err));
    EXPECT_NE(err.find("negative"), std::string::npos);
}

TEST(ProblemRevisionsValidateRevisionNo, RejectsAboveMax) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_revision_no(
        litecode::problem_revisions_repo::kMaxRevisionNo + 1, &err));
}

TEST(ProblemRevisionsValidateSummary, AcceptsWithinLength) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_summary("", &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_summary(
        std::string(litecode::problem_revisions_repo::kMaxSummaryLength, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateSummary, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_summary(
        "ok\nsummary", &err));           // newline
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_summary(
        std::string("a\x01b"), &err));   // 0x01 control
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_summary(
        std::string("a\x7fb"), &err));   // DEL
}

TEST(ProblemRevisionsValidateSummary, RejectsTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_summary(
        std::string(litecode::problem_revisions_repo::kMaxSummaryLength + 1, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateAction, AcceptsCreateAndUpdate) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_action("create", &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_action("update", &err));
}

TEST(ProblemRevisionsValidateAction, RejectsOtherStrings) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("delete", &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("",      &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("insert", &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_action("Create", &err));  // case
    EXPECT_NE(err.find("'create' or 'update'"), std::string::npos);
}

TEST(ProblemRevisionsValidateEditorUsername, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_editor_username(
        std::string(litecode::problem_revisions_repo::kMinEditorUsernameLength, 'a'),
        &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_editor_username(
        std::string(litecode::problem_revisions_repo::kMaxEditorUsernameLength, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateEditorUsername, RejectsTooShortOrLong) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_editor_username(
        std::string(litecode::problem_revisions_repo::kMinEditorUsernameLength - 1, 'a'),
        &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_editor_username(
        std::string(litecode::problem_revisions_repo::kMaxEditorUsernameLength + 1, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateEditorUsername, RejectsControlChars) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_editor_username(
        std::string("a\x01b"), &err));
}

TEST(ProblemRevisionsValidateSlug, AcceptsAndRejectsLikeProblemRepo) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_slug(
        "two-sum", &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_slug(
        "Two-Sum", &err));       // uppercase
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_slug("", &err));
}

TEST(ProblemRevisionsValidateTitle, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_title(
        std::string(litecode::problem_revisions_repo::kMinRevisionTitleLength, 'a'),
        &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_title(
        std::string(litecode::problem_revisions_repo::kMaxRevisionTitleLength, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateDifficulty, AcceptsAllowed) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_difficulty("easy",   &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_difficulty("medium", &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_difficulty("hard",   &err));
}

TEST(ProblemRevisionsValidateDifficulty, RejectsOther) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_difficulty("epic", &err));
}

TEST(ProblemRevisionsValidateTimeLimit, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_time_limit(
        litecode::problem_revisions_repo::kMinRevisionTimeLimitMs, &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_time_limit(
        litecode::problem_revisions_repo::kMaxRevisionTimeLimitMs, &err));
}

TEST(ProblemRevisionsValidateTimeLimit, RejectsOutOfRange) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_time_limit(0, &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_time_limit(
        litecode::problem_revisions_repo::kMaxRevisionTimeLimitMs + 1, &err));
}

TEST(ProblemRevisionsValidateMemoryLimit, AcceptsBoundaries) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_memory_limit(
        litecode::problem_revisions_repo::kMinRevisionMemoryLimitMb, &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_memory_limit(
        litecode::problem_revisions_repo::kMaxRevisionMemoryLimitMb, &err));
}

TEST(ProblemRevisionsValidateMemoryLimit, RejectsOutOfRange) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_memory_limit(0, &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_memory_limit(
        litecode::problem_revisions_repo::kMaxRevisionMemoryLimitMb + 1, &err));
}

TEST(ProblemRevisionsValidateDescription, AcceptsMarkdownWithNewlinesAndTabs) {
    // Unlike validate_summary, description accepts \n / \r / \t
    // because Markdown legitimately uses them.
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_description(
        "## Title\n\n- item 1\n- item 2\tindented\n", &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_description(
        std::string(litecode::problem_revisions_repo::kMinRevisionDescriptionLen, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateDescription, RejectsNullAndControlChars) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_description(
        std::string("a\x00b"), &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_description(
        std::string("a\x01b"), &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_description(
        std::string("a\x7fb"), &err));
}

TEST(ProblemRevisionsValidateDescription, RejectsTooShortOrTooLong) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_description(
        std::string(litecode::problem_revisions_repo::kMinRevisionDescriptionLen - 1, 'a'),
        &err));
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_description(
        std::string(litecode::problem_revisions_repo::kMaxRevisionDescriptionLen + 1, 'a'),
        &err));
}

TEST(ProblemRevisionsValidateIp, AcceptsEmpty) {
    // IP is optional in this repo — empty means "no editor IP".
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_ip("", &err));
}

TEST(ProblemRevisionsValidateIp, AcceptsCommonAddresses) {
    std::string err;
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_ip("127.0.0.1", &err));
    EXPECT_TRUE(litecode::problem_revisions_repo::validate_ip(
        "2001:db8::1", &err));
}

TEST(ProblemRevisionsValidateIp, RejectsTooLongOrControl) {
    std::string err;
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_ip(
        std::string(46, 'a'), &err));   // > 45 chars
    EXPECT_FALSE(litecode::problem_revisions_repo::validate_ip(
        std::string("1.2.3\x01"), &err));
}

TEST(ProblemRevisionsLooksLikeDupKeyError, MatchesDuplicateAnd1062) {
    EXPECT_TRUE(litecode::problem_revisions_repo::detail::looks_like_dup_key_error(
        "Duplicate entry 'foo' for key 'PRIMARY'"));
    EXPECT_TRUE(litecode::problem_revisions_repo::detail::looks_like_dup_key_error(
        "duplicate key value"));
    EXPECT_TRUE(litecode::problem_revisions_repo::detail::looks_like_dup_key_error(
        "MySQL error 1062: Duplicate entry"));
}

TEST(ProblemRevisionsLooksLikeDupKeyError, RejectsOtherErrors) {
    EXPECT_FALSE(litecode::problem_revisions_repo::detail::looks_like_dup_key_error(
        "syntax error"));
    EXPECT_FALSE(litecode::problem_revisions_repo::detail::looks_like_dup_key_error(
        ""));
    EXPECT_FALSE(litecode::problem_revisions_repo::detail::looks_like_dup_key_error(
        "foreign key constraint fails"));
}

TEST(ProblemRevisionsClampListFilter, ZeroLimitAndNegativeOffset) {
    litecode::problem_revisions_repo::RevisionListFilter f;
    f.limit  = 0;
    f.offset = -5;
    litecode::problem_revisions_repo::clamp_list_filter(f);
    EXPECT_EQ(f.limit,
              litecode::problem_revisions_repo::kDefaultRevisionListLimit);
    EXPECT_EQ(f.offset, 0);
}

TEST(ProblemRevisionsClampListFilter, OverMaxLimitClamps) {
    litecode::problem_revisions_repo::RevisionListFilter f;
    f.limit = 9999;
    litecode::problem_revisions_repo::clamp_list_filter(f);
    EXPECT_EQ(f.limit,
              litecode::problem_revisions_repo::kMaxRevisionListLimit);
}

}  // namespace