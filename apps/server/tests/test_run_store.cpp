#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

#include "run_store.hpp"

namespace {

ScriptRun sample_run(std::string run_id) {
    ScriptRun run;
    run.run_id = std::move(run_id);
    run.script_name = "Software/install.ps1";
    run.script_body = "exit 0\n";
    run.issued_at = std::chrono::system_clock::now();
    run.timeout_seconds = 120;

    RunTarget completed;
    completed.host_id = "PC-001";
    completed.state = TargetState::Completed;
    lm::transport::ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = run.run_id;
    result.status = lm::core::ScriptStatus::Completed;
    // Every field gets its own non-default, mutually distinct value: a shared
    // value between two fields (e.g. both bools true, or two fields reading
    // "done") would let the writer or reader cross or drop one silently and
    // still pass an equality check.
    result.refusal_reason = "refusal reason text";
    result.exit_code = 7;
    result.has_reported = true;
    // true, not the struct default: a field left at its default cannot catch
    // the serialiser silently dropping it, since default-in equals default-out.
    result.reported_ok = true;
    result.reported_message = "reported message text";
    result.stdout_text = "stdout text";
    result.stderr_text = "stderr text";
    result.duration_ms = 240;
    completed.result = result;
    run.targets.push_back(completed);

    RunTarget refused;
    refused.host_id = "PC-002";
    refused.state = TargetState::Refused;
    refused.detail = "not enrolled for script execution";
    run.targets.push_back(refused);

    return run;
}

}  // namespace

TEST(RunStore, RoundTripsEveryFieldARunViewReadsBack) {
    // A field the writer forgets is invisible on screen -- the history just
    // looks empty -- and a run is written once and never rewritten, so a loss
    // here is permanent.
    const ScriptRun original = sample_run("run-1");

    const auto reloaded = run_from_json(run_to_json(original));
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error();

    EXPECT_EQ(reloaded->run_id, original.run_id);
    EXPECT_EQ(reloaded->script_name, original.script_name);
    EXPECT_EQ(reloaded->script_body, original.script_body);
    EXPECT_EQ(reloaded->timeout_seconds, original.timeout_seconds);
    ASSERT_EQ(reloaded->targets.size(), 2u);

    EXPECT_EQ(reloaded->targets[0].host_id, "PC-001");
    EXPECT_EQ(reloaded->targets[0].state, TargetState::Completed);
    ASSERT_TRUE(reloaded->targets[0].result.has_value());
    EXPECT_EQ(reloaded->targets[0].result->status, lm::core::ScriptStatus::Completed);
    EXPECT_EQ(reloaded->targets[0].result->refusal_reason, "refusal reason text");
    EXPECT_EQ(reloaded->targets[0].result->exit_code, 7);
    EXPECT_EQ(reloaded->targets[0].result->has_reported, true);
    EXPECT_EQ(reloaded->targets[0].result->reported_ok, true);
    EXPECT_EQ(reloaded->targets[0].result->reported_message, "reported message text");
    EXPECT_EQ(reloaded->targets[0].result->stdout_text, "stdout text");
    EXPECT_EQ(reloaded->targets[0].result->stderr_text, "stderr text");
    EXPECT_EQ(reloaded->targets[0].result->duration_ms, 240u);

    EXPECT_EQ(reloaded->targets[1].state, TargetState::Refused);
    EXPECT_EQ(reloaded->targets[1].detail, "not enrolled for script execution");
    EXPECT_FALSE(reloaded->targets[1].result.has_value())
        << "a refused target never had a result and must not gain an empty one";
}

TEST(RunStore, KeepsIssuedAtToTheSecond) {
    // "Delete runs older than" compares against this, so a lost timestamp is a
    // run that cleanup can never reach.
    const ScriptRun original = sample_run("run-2");
    const auto reloaded = run_from_json(run_to_json(original));
    ASSERT_TRUE(reloaded.has_value());

    const auto original_s =
        std::chrono::duration_cast<std::chrono::seconds>(original.issued_at.time_since_epoch());
    const auto reloaded_s =
        std::chrono::duration_cast<std::chrono::seconds>(reloaded->issued_at.time_since_epoch());
    EXPECT_EQ(original_s.count(), reloaded_s.count());
}

TEST(RunStore, RejectsADocumentThatIsNotARunRatherThanInventingOne) {
    EXPECT_FALSE(run_from_json(nlohmann::json::parse("[]")).has_value());
    EXPECT_FALSE(run_from_json(nlohmann::json::parse(R"({"script_name":"a"})")).has_value())
        << "no run_id means nothing can be looked up or deleted";
}

TEST(RunStore, SavesOneFilePerRunAndLoadsThemAllBack) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;

    ASSERT_TRUE(save_run(dir.path(), sample_run("run-a"), &error)) << error.toStdString();
    ASSERT_TRUE(save_run(dir.path(), sample_run("run-b"), &error)) << error.toStdString();

    std::vector<QString> errors;
    std::vector<ScriptRun> runs = load_runs(dir.path(), &errors);

    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(runs.size(), 2u);
    std::ranges::sort(runs, {}, &ScriptRun::run_id);
    EXPECT_EQ(runs[0].run_id, "run-a");
    EXPECT_EQ(runs[1].run_id, "run-b");
}

TEST(RunStore, ACorruptFileCostsOneRunRatherThanAllOfThem) {
    // The whole reason for one file per run (spec section 8).
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(save_run(dir.path(), sample_run("good"), &error));

    QFile bad(QDir(dir.path()).filePath(QStringLiteral("corrupt.json")));
    ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
    bad.write("{ this is not json");
    bad.close();

    std::vector<QString> errors;
    const std::vector<ScriptRun> runs = load_runs(dir.path(), &errors);

    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs.front().run_id, "good");
    EXPECT_EQ(errors.size(), 1u) << "the loss is reported, not silent";
}

TEST(RunStore, DeletesOneRunsFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(save_run(dir.path(), sample_run("run-a"), &error));
    ASSERT_TRUE(save_run(dir.path(), sample_run("run-b"), &error));

    ASSERT_TRUE(delete_run(dir.path(), "run-a", &error)) << error.toStdString();

    std::vector<QString> errors;
    const std::vector<ScriptRun> runs = load_runs(dir.path(), &errors);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs.front().run_id, "run-b");
}

TEST(RunStore, DeletingARunThatIsNotThereIsAFailureThatSaysSo) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    EXPECT_FALSE(delete_run(dir.path(), "never-existed", &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(RunStore, RejectsAPathTraversingRunId) {
    // run_id is server-generated and already filesystem-safe, but save_run()
    // and delete_run() must not simply trust that. Covers every character the
    // guard rejects: '/', '\', "..", ':' (an NTFS alternate-data-stream
    // separator on Windows), and empty (would otherwise write plain ".json").
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    for (const std::string& unsafe_id :
         {std::string("../evil"), std::string("..\\evil"), std::string("a/b"), std::string("a\\b"),
          std::string("a:b"), std::string("")}) {
        QString error;
        EXPECT_FALSE(save_run(dir.path(), sample_run(unsafe_id), &error)) << unsafe_id;
        EXPECT_FALSE(error.isEmpty()) << unsafe_id;

        error.clear();
        EXPECT_FALSE(delete_run(dir.path(), unsafe_id, &error)) << unsafe_id;
        EXPECT_FALSE(error.isEmpty()) << unsafe_id;
    }
}

TEST(RunStore, RoundTripsEveryTargetState) {
    // to_string(TargetState) and target_state_from_string() live in different
    // files (script_run.cpp) and are matched by hand rather than by an
    // exhaustive switch on the parse side, so nothing at compile time forces
    // them to agree. This is the safety net: adding a TargetState enumerator
    // without updating the parse fails here instead of silently becoming an
    // unreadable history file.
    constexpr TargetState kStates[] = {TargetState::Pending,   TargetState::Dispatched,
                                        TargetState::Completed, TargetState::Failed,
                                        TargetState::Refused,   TargetState::NoResponse};
    for (const TargetState state : kStates) {
        ScriptRun run = sample_run("run-target-state");
        run.targets.clear();
        RunTarget target;
        target.host_id = "PC-001";
        target.state = state;
        run.targets.push_back(target);

        const auto reloaded = run_from_json(run_to_json(run));
        ASSERT_TRUE(reloaded.has_value()) << to_string(state);
        ASSERT_EQ(reloaded->targets.size(), 1u);
        EXPECT_EQ(reloaded->targets[0].state, state) << to_string(state);
    }
}

TEST(RunStore, RoundTripsEveryScriptStatus) {
    // Same reasoning as RoundTripsEveryTargetState, for
    // lm::core::to_string(ScriptStatus) and the local parse in run_store.cpp.
    constexpr lm::core::ScriptStatus kStatuses[] = {
        lm::core::ScriptStatus::Completed, lm::core::ScriptStatus::Failed,
        lm::core::ScriptStatus::Refused, lm::core::ScriptStatus::Error};
    for (const lm::core::ScriptStatus status : kStatuses) {
        ScriptRun run = sample_run("run-script-status");
        run.targets.clear();

        RunTarget target;
        target.host_id = "PC-001";
        target.state = TargetState::Completed;
        lm::transport::ScriptResultMessage result;
        result.host_id = "PC-001";
        result.run_id = run.run_id;
        result.status = status;
        target.result = result;
        run.targets.push_back(target);

        const auto reloaded = run_from_json(run_to_json(run));
        ASSERT_TRUE(reloaded.has_value()) << lm::core::to_string(status);
        ASSERT_EQ(reloaded->targets.size(), 1u);
        ASSERT_TRUE(reloaded->targets[0].result.has_value());
        EXPECT_EQ(reloaded->targets[0].result->status, status) << lm::core::to_string(status);
    }
}

TEST(TargetStateFromString, RejectsUnknownAndBogusText) {
    // "Unknown" is to_string(TargetState)'s own unreachable fallback after an
    // exhaustive switch -- it must not read back as any real state, and this
    // is correct today only by its absence from an if-chain that a later
    // well-meaning branch could quietly undo.
    EXPECT_FALSE(target_state_from_string("Unknown").has_value());
    EXPECT_FALSE(target_state_from_string("Banana").has_value());
}

TEST(RunStore, RejectsUnknownAndBogusScriptStatus) {
    // script_status_from_string is file-local to run_store.cpp -- the inverse
    // of lm::core::to_string(ScriptStatus) -- so it is reachable only through
    // run_from_json here. Same reasoning as
    // TargetStateFromString.RejectsUnknownAndBogusText: "Unknown" is
    // to_string(ScriptStatus)'s own unreachable fallback and must not parse
    // back as a real status.
    for (const std::string& status_text : {std::string("Unknown"), std::string("Banana")}) {
        nlohmann::json document = run_to_json(sample_run("run-bad-status"));
        document["targets"][0]["result"]["status"] = status_text;
        EXPECT_FALSE(run_from_json(document).has_value()) << status_text;
    }
}
