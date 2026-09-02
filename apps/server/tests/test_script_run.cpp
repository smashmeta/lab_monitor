#include <gtest/gtest.h>

#include "script_run.hpp"

using namespace lm::core;

namespace {

ScriptRun run_over(std::vector<std::string> hosts) {
    ScriptRun run;
    run.run_id = "run-1";
    run.script_name = "(custom script)";
    for (auto& host : hosts) {
        run.targets.push_back(RunTarget{std::move(host), TargetState::Pending, {}, {}});
    }
    return run;
}

}  // namespace

TEST(ScriptRunModel, StartsEveryTargetPending) {
    const ScriptRun run = run_over({"PC-001", "PC-002"});
    EXPECT_EQ(run.tally().pending, 2u);
    EXPECT_FALSE(run.is_finished());
}

TEST(ScriptRunModel, ARefusalAtDispatchNeedsNoCommandSent) {
    // A host that is not Online cannot be promised delivery: the topic is
    // Volatile, so there is no queue to hold the command until it returns.
    ScriptRun run = run_over({"PC-001"});
    run.refuse_at_dispatch("PC-001", "host is not online");

    EXPECT_EQ(run.targets.front().state, TargetState::Refused);
    EXPECT_EQ(run.targets.front().detail, "host is not online");
    EXPECT_TRUE(run.is_finished());
}

TEST(ScriptRunModel, RecordsAResultAgainstTheRightHost) {
    ScriptRun run = run_over({"PC-001", "PC-002"});
    run.mark_dispatched("PC-001");
    run.mark_dispatched("PC-002");

    lm::transport::ScriptResultMessage result;
    result.host_id = "PC-002";
    result.run_id = "run-1";
    result.status = ScriptStatus::Completed;
    run.apply_result(result);

    EXPECT_EQ(run.targets[0].state, TargetState::Dispatched);
    EXPECT_EQ(run.targets[1].state, TargetState::Completed);
}

TEST(ScriptRunModel, IgnoresAResultForAHostItNeverTargeted) {
    ScriptRun run = run_over({"PC-001"});
    run.mark_dispatched("PC-001");

    lm::transport::ScriptResultMessage stray;
    stray.host_id = "PC-999";
    stray.run_id = "run-1";
    stray.status = ScriptStatus::Completed;
    run.apply_result(stray);

    EXPECT_EQ(run.targets.front().state, TargetState::Dispatched);
    EXPECT_EQ(run.tally().completed, 0u);
}

TEST(ScriptRunModel, TurnsADispatchedTargetIntoNoResponseAtTheDeadline) {
    ScriptRun run = run_over({"PC-001", "PC-002"});
    run.mark_dispatched("PC-001");
    run.mark_dispatched("PC-002");

    lm::transport::ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = "run-1";
    result.status = ScriptStatus::Completed;
    run.apply_result(result);

    run.apply_deadline();

    EXPECT_EQ(run.targets[0].state, TargetState::Completed) << "an answered host is not touched";
    EXPECT_EQ(run.targets[1].state, TargetState::NoResponse);
    EXPECT_TRUE(run.is_finished());
}

TEST(ScriptRunModel, KeepsRefusedAndNoResponseDistinct) {
    // They call for different actions -- enrol the machine, versus go and see
    // whether it is alive -- so the tally must not merge them into "failed".
    ScriptRun run = run_over({"PC-001", "PC-002"});
    run.refuse_at_dispatch("PC-001", "not enrolled");
    run.mark_dispatched("PC-002");
    run.apply_deadline();

    const RunTally tally = run.tally();
    EXPECT_EQ(tally.refused, 1u);
    EXPECT_EQ(tally.no_response, 1u);
    EXPECT_EQ(tally.failed, 0u);
}

TEST(ScriptRunModel, CountsAReportedFailureAsFailedNotCompleted) {
    ScriptRun run = run_over({"PC-001"});
    run.mark_dispatched("PC-001");

    lm::transport::ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = "run-1";
    result.status = ScriptStatus::Failed;
    result.exit_code = 1;
    run.apply_result(result);

    EXPECT_EQ(run.tally().failed, 1u);
    EXPECT_EQ(run.tally().completed, 0u);
}
