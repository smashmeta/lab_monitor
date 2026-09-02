#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "lm/core/script.hpp"

using namespace lm::core;

TEST(ReportedResult, ReadsTheMarkerLine) {
    const auto parsed = parse_reported_result(
        "starting\nLM-RESULT: {\"ok\": true, \"message\": \"3 files cleaned\"}\n");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->ok);
    EXPECT_EQ(parsed->message, "3 files cleaned");
}

TEST(ReportedResult, IsAbsentWhenNoMarkerWasWritten) {
    // The common case: an existing script that knows nothing about this
    // convention. It is judged on its exit code alone, and "said nothing" must
    // stay distinguishable from "said it succeeded".
    EXPECT_FALSE(parse_reported_result("just some output\n").has_value());
}

TEST(ReportedResult, TakesTheLastMarkerWhenSeveralWereWritten) {
    // A script that reports progress and then a verdict. The verdict is last.
    const auto parsed = parse_reported_result(
        "LM-RESULT: {\"ok\": true, \"message\": \"step one\"}\n"
        "LM-RESULT: {\"ok\": false, \"message\": \"step two failed\"}\n");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->ok);
    EXPECT_EQ(parsed->message, "step two failed");
}

TEST(ReportedResult, IgnoresAMalformedMarkerRatherThanFailingTheRun) {
    // A broken marker must not turn a script that exited 0 into a failure.
    // The exit code still decides; this only ever adds detail.
    EXPECT_FALSE(parse_reported_result("LM-RESULT: not json at all\n").has_value());
    EXPECT_FALSE(parse_reported_result("LM-RESULT: {\"ok\": \"yes\"}\n").has_value())
        << "ok must be a boolean";
}

TEST(ReportedResult, IgnoresALineThatMerelyLooksLikeAMarker) {
    EXPECT_FALSE(parse_reported_result("see LM-RESULT: for details\n").has_value())
        << "the marker has to start the line";
}

TEST(ReportedResult, ToleratesAMissingMessage) {
    const auto parsed = parse_reported_result("LM-RESULT: {\"ok\": true}\n");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->ok);
    EXPECT_TRUE(parsed->message.empty());
}

TEST(ScriptOutcome, IsFailedWhenTheExitCodeIsNonZero) {
    ScriptOutcome outcome;
    outcome.exit_code = 1;
    EXPECT_EQ(outcome.status(), ScriptStatus::Failed);
}

TEST(ScriptOutcome, IsCompletedWhenTheExitCodeIsZeroAndNothingWasReported) {
    ScriptOutcome outcome;
    outcome.exit_code = 0;
    EXPECT_EQ(outcome.status(), ScriptStatus::Completed);
}

TEST(ScriptOutcome, LetsAReportedFailureOverrideASuccessfulExitCode) {
    // A script that catches its own error, reports it, and still exits 0 --
    // which PowerShell does readily. The script's own verdict is the more
    // specific statement, so it wins.
    ScriptOutcome outcome;
    outcome.exit_code = 0;
    outcome.reported = ReportedResult{false, "3 of 5 packages failed"};
    EXPECT_EQ(outcome.status(), ScriptStatus::Failed);
}

TEST(ScriptOutcome, DoesNotLetAReportedSuccessOverrideAFailingExitCode) {
    // The reverse does not hold. A non-zero exit is the process itself saying
    // it failed, and a script cannot talk its way out of that -- it is exactly
    // how a crash after the marker would look.
    ScriptOutcome outcome;
    outcome.exit_code = 1;
    outcome.reported = ReportedResult{true, "all good"};
    EXPECT_EQ(outcome.status(), ScriptStatus::Failed);
}
