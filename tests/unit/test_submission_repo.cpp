// tests/unit/test_submission_repo.cpp
//
// Pure-unit tests for src/db/submission_repo.h's helpers + constants.
//
// submission_repo.h is exercised end-to-end by test_submission.cpp
// (route-level: POST/GET /api/v1/submissions) and the SSE handler's
// own binary. But the repo's status / language / code-length
// validators — the "single source of truth" for what counts as a
// valid submission row — historically had no dedicated home; this
// file pins them so a future commit can't quietly widen the contract
// without breaking a test.
//
// All tests are pure C++ — no MySQL, no Docker. They run identically
// on CI lint machines and developer laptops.
//
// Coverage:
//   - is_valid_status: every kStatus* is valid; everything else isn't.
//   - is_terminal_status: "pending" / "running" are transient; all
//     9 terminal kStatus* values are terminal; unknown strings are
//     treated as terminal (defensive default — "I don't know this
//     state, so don't block on it").
//   - is_valid_language: "c" / "cpp" valid; "python" / "go" / "" /
//     uppercase variants rejected.
//   - validate_code_length: 0 rejected, 1 accepted, kMaxCodeLength
//     accepted, kMaxCodeLength + 1 rejected. error_out populated on
//     rejection, untouched on accept.
//   - clamp_list_filter: limit 0 → default, limit 999 → max, offset
//     -5 → 0, well-formed input unchanged.
//   - constants: kMinCodeLength / kMaxCodeLength match SPEC §4.4
//     (15 MB clamp just below MEDIUMTEXT 16 MB).
//   - kStatus* constants: pin the grep-friendly strings used by the
//     scheduler / audit / route layers.
//   - struct defaults: SubmissionRow / SubmissionListFilter /
//     SubmissionListResult fields are zero / empty / sensible defaults.
//   - exception hierarchy: SubmissionNotFoundError derives from
//     SubmissionRepoError derives from std::runtime_error.
//   - kSubmissionSelectColumns pins the 11-column SELECT projection.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "db/submission_repo.h"

namespace {

// ────────────────────────────────────────────────────────────────────────────
//  is_valid_status
// ────────────────────────────────────────────────────────────────────────────

TEST(IsValidStatus, AcceptsAllKStatusConstants) {
    EXPECT_TRUE(litecode::is_valid_status("pending"));
    EXPECT_TRUE(litecode::is_valid_status("running"));
    EXPECT_TRUE(litecode::is_valid_status("ac"));
    EXPECT_TRUE(litecode::is_valid_status("wa"));
    EXPECT_TRUE(litecode::is_valid_status("re"));
    EXPECT_TRUE(litecode::is_valid_status("tle"));
    EXPECT_TRUE(litecode::is_valid_status("mle"));
    EXPECT_TRUE(litecode::is_valid_status("ole"));
    EXPECT_TRUE(litecode::is_valid_status("pe"));
    EXPECT_TRUE(litecode::is_valid_status("ce"));
    EXPECT_TRUE(litecode::is_valid_status("se"));
}

TEST(IsValidStatus, RejectsUnknownStatuses) {
    EXPECT_FALSE(litecode::is_valid_status(""));
    EXPECT_FALSE(litecode::is_valid_status("AC"));         // uppercase
    EXPECT_FALSE(litecode::is_valid_status("Accepted"));
    EXPECT_FALSE(litecode::is_valid_status("complete"));
    EXPECT_FALSE(litecode::is_valid_status("judging"));
    EXPECT_FALSE(litecode::is_valid_status(" pending"));   // leading space
    EXPECT_FALSE(litecode::is_valid_status("pending\n"));  // trailing newline
}

// ────────────────────────────────────────────────────────────────────────────
//  is_terminal_status
// ────────────────────────────────────────────────────────────────────────────

TEST(IsTerminalStatus, PendingAndRunningAreTransient) {
    EXPECT_FALSE(litecode::is_terminal_status("pending"));
    EXPECT_FALSE(litecode::is_terminal_status("running"));
}

TEST(IsTerminalStatus, AllKStatusTerminalsAreTerminal) {
    EXPECT_TRUE(litecode::is_terminal_status("ac"));
    EXPECT_TRUE(litecode::is_terminal_status("wa"));
    EXPECT_TRUE(litecode::is_terminal_status("re"));
    EXPECT_TRUE(litecode::is_terminal_status("tle"));
    EXPECT_TRUE(litecode::is_terminal_status("mle"));
    EXPECT_TRUE(litecode::is_terminal_status("ole"));
    EXPECT_TRUE(litecode::is_terminal_status("pe"));
    EXPECT_TRUE(litecode::is_terminal_status("ce"));
    EXPECT_TRUE(litecode::is_terminal_status("se"));
}

TEST(IsTerminalStatus, UnknownDefaultsToTerminal) {
    // "I don't know this state, so don't block on it" — defensive
    // contract used by SSE / scheduler wait_for() loops.
    EXPECT_TRUE(litecode::is_terminal_status(""));
    EXPECT_TRUE(litecode::is_terminal_status("unknown"));
    EXPECT_TRUE(litecode::is_terminal_status("garbage"));
}

// ────────────────────────────────────────────────────────────────────────────
//  is_valid_language
// ────────────────────────────────────────────────────────────────────────────

TEST(IsValidLanguage, AcceptsCAndCpp) {
    EXPECT_TRUE(litecode::is_valid_language("c"));
    EXPECT_TRUE(litecode::is_valid_language("cpp"));
}

TEST(IsValidLanguage, RejectsOtherLanguagesAndShapeVariants) {
    EXPECT_FALSE(litecode::is_valid_language(""));
    EXPECT_FALSE(litecode::is_valid_language("python"));
    EXPECT_FALSE(litecode::is_valid_language("java"));
    EXPECT_FALSE(litecode::is_valid_language("go"));
    EXPECT_FALSE(litecode::is_valid_language("rust"));
    EXPECT_FALSE(litecode::is_valid_language("C"));        // uppercase
    EXPECT_FALSE(litecode::is_valid_language("CPP"));
    EXPECT_FALSE(litecode::is_valid_language("c++"));      // we use "cpp"
    EXPECT_FALSE(litecode::is_valid_language(" c"));       // leading space
}

// ────────────────────────────────────────────────────────────────────────────
//  validate_code_length
// ────────────────────────────────────────────────────────────────────────────

TEST(ValidateCodeLength, RejectsEmpty) {
    std::string err;
    EXPECT_FALSE(litecode::validate_code_length(0, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("between"), std::string::npos);
}

TEST(ValidateCodeLength, AcceptsMinBoundary) {
    std::string err = "should be cleared";
    EXPECT_TRUE(litecode::validate_code_length(1, &err));
    EXPECT_EQ(err, "should be cleared")
        << "error_out is untouched on success";
}

TEST(ValidateCodeLength, AcceptsMaxBoundary) {
    std::string err = "untouched";
    EXPECT_TRUE(litecode::validate_code_length(
        litecode::kMaxCodeLength, &err));
    EXPECT_EQ(err, "untouched");
}

TEST(ValidateCodeLength, RejectsMaxPlusOne) {
    std::string err;
    EXPECT_FALSE(litecode::validate_code_length(
        litecode::kMaxCodeLength + 1, &err));
    EXPECT_FALSE(err.empty());
}

TEST(ValidateCodeLength, NullErrorOutIsAllowed) {
    // error_out is optional; passing nullptr must not crash.
    EXPECT_FALSE(litecode::validate_code_length(0, nullptr));
    EXPECT_TRUE (litecode::validate_code_length(100, nullptr));
    EXPECT_FALSE(litecode::validate_code_length(
        litecode::kMaxCodeLength + 1, nullptr));
}

// ────────────────────────────────────────────────────────────────────────────
//  clamp_list_filter
// ────────────────────────────────────────────────────────────────────────────

TEST(ClampListFilter, NegativeOrZeroLimitBecomesDefault) {
    litecode::SubmissionListFilter f;
    f.limit  = 0;
    f.offset = 5;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kSubmissionDefaultListLimit);
    EXPECT_EQ(f.offset, 5);
}

TEST(ClampListFilter, NegativeLimitBecomesDefault) {
    litecode::SubmissionListFilter f;
    f.limit = -7;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kSubmissionDefaultListLimit);
}

TEST(ClampListFilter, OverMaxLimitClampsToMax) {
    litecode::SubmissionListFilter f;
    f.limit = 999;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit, litecode::kSubmissionMaxListLimit);
}

TEST(ClampListFilter, NegativeOffsetClampsToZero) {
    litecode::SubmissionListFilter f;
    f.offset = -5;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.offset, 0);
}

TEST(ClampListFilter, WellFormedInputUnchanged) {
    litecode::SubmissionListFilter f;
    f.limit  = 25;
    f.offset = 10;
    litecode::clamp_list_filter(f);
    EXPECT_EQ(f.limit,  25);
    EXPECT_EQ(f.offset, 10);
}

// ────────────────────────────────────────────────────────────────────────────
//  Constants — pin the grep-friendly contract
// ────────────────────────────────────────────────────────────────────────────

TEST(SubmissionRepoConstants, CodeLengthCaps) {
    EXPECT_EQ(litecode::kMinCodeLength, 1u);
    // 15 MB — just below MEDIUMTEXT's 16 MB ceiling so a body at the
    // boundary doesn't trip "Data too long" in the wire layer.
    EXPECT_EQ(litecode::kMaxCodeLength, 15u * 1024u * 1024u);
}

TEST(SubmissionRepoConstants, ListLimits) {
    EXPECT_EQ(litecode::kSubmissionDefaultListLimit, 20);
    EXPECT_EQ(litecode::kSubmissionMaxListLimit,     100);
}

TEST(SubmissionRepoConstants, KStatusStrings) {
    // Pin the literal strings — the audit log / SSE handler /
    // scheduler all read these by name. A typo here would silently
    // break the wire contract.
    EXPECT_STREQ(litecode::kStatusPending, "pending");
    EXPECT_STREQ(litecode::kStatusRunning, "running");
    EXPECT_STREQ(litecode::kStatusAC,      "ac");
    EXPECT_STREQ(litecode::kStatusWA,      "wa");
    EXPECT_STREQ(litecode::kStatusRE,      "re");
    EXPECT_STREQ(litecode::kStatusTLE,     "tle");
    EXPECT_STREQ(litecode::kStatusMLE,     "mle");
    EXPECT_STREQ(litecode::kStatusOLE,     "ole");
    EXPECT_STREQ(litecode::kStatusPE,      "pe");
    EXPECT_STREQ(litecode::kStatusCE,      "ce");
    EXPECT_STREQ(litecode::kStatusSE,      "se");
}

TEST(SubmissionRepoConstants, SelectColumnsContract) {
    using litecode::submission_repo::detail::kSubmissionSelectColumns;
    const std::string cols(kSubmissionSelectColumns);
    EXPECT_NE(cols.find("id"),             std::string::npos);
    EXPECT_NE(cols.find("user_id"),        std::string::npos);
    EXPECT_NE(cols.find("problem_id"),     std::string::npos);
    EXPECT_NE(cols.find("language"),       std::string::npos);
    EXPECT_NE(cols.find("code"),           std::string::npos);
    EXPECT_NE(cols.find("status"),         std::string::npos);
    EXPECT_NE(cols.find("time_used"),      std::string::npos);
    EXPECT_NE(cols.find("memory_used"),    std::string::npos);
    EXPECT_NE(cols.find("error_message"),  std::string::npos);
    EXPECT_NE(cols.find("created_at"),     std::string::npos);
    EXPECT_NE(cols.find("finished_at"),    std::string::npos);
    // DATE_FORMAT'd → no raw datetime columns on the wire
    EXPECT_NE(cols.find("DATE_FORMAT"),    std::string::npos);
}

// ────────────────────────────────────────────────────────────────────────────
//  Struct defaults
// ────────────────────────────────────────────────────────────────────────────

TEST(SubmissionRowDefaults, ZeroedAndEmpty) {
    litecode::SubmissionRow r;
    EXPECT_EQ(r.id,         0);
    EXPECT_EQ(r.user_id,    0);
    EXPECT_EQ(r.problem_id, 0);
    EXPECT_EQ(r.language,   "");
    EXPECT_EQ(r.code,       "");
    EXPECT_EQ(r.status,     "");
    EXPECT_FALSE(r.time_used.has_value());
    EXPECT_FALSE(r.memory_used.has_value());
    EXPECT_FALSE(r.error_message.has_value());
    EXPECT_EQ(r.created_at, "");
    EXPECT_FALSE(r.finished_at.has_value());
}

TEST(SubmissionListFilterDefaults, IncludeUnfinishedIsTrue) {
    litecode::SubmissionListFilter f;
    EXPECT_TRUE(f.include_unfinished);
    EXPECT_EQ(f.limit,  20);   // kSubmissionDefaultListLimit
    EXPECT_EQ(f.offset, 0);
    EXPECT_FALSE(f.user_id.has_value());
    EXPECT_FALSE(f.problem_id.has_value());
    EXPECT_FALSE(f.status.has_value());
}

TEST(SubmissionListResultDefaults, ZeroedAndEmpty) {
    litecode::SubmissionListResult r;
    EXPECT_TRUE(r.items.empty());
    EXPECT_EQ(r.total,  0);
    EXPECT_EQ(r.limit,  0);
    EXPECT_EQ(r.offset, 0);
}

// ────────────────────────────────────────────────────────────────────────────
//  Exception hierarchy
// ────────────────────────────────────────────────────────────────────────────

TEST(SubmissionRepoExceptions, NotFoundDerivesFromRepoError) {
    static_assert(std::is_base_of<litecode::SubmissionRepoError,
                                  litecode::SubmissionNotFoundError>::value,
                  "SubmissionNotFoundError must derive from SubmissionRepoError");
    litecode::SubmissionNotFoundError nf("missing");
    litecode::SubmissionRepoError    base("base");
    EXPECT_NE(std::string(nf.what()).find("missing"), std::string::npos);
    // catchable via the repo base
    try {
        throw litecode::SubmissionNotFoundError("nf");
    } catch (const litecode::SubmissionRepoError& re) {
        EXPECT_NE(std::string(re.what()).find("nf"), std::string::npos);
    }
    (void)base;
}

TEST(SubmissionRepoExceptions, BothDeriveFromStdRuntimeError) {
    static_assert(std::is_base_of<std::runtime_error,
                                  litecode::SubmissionRepoError>::value, "");
    static_assert(std::is_base_of<std::runtime_error,
                                  litecode::SubmissionNotFoundError>::value, "");
}

}  // namespace