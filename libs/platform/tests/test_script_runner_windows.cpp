#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "lm/platform/probes.hpp"

using namespace std::chrono_literals;

namespace {

lm::core::ScriptOutcome run(const std::string& body,
                            std::chrono::seconds timeout = 30s) {
    const auto runner = lm::platform::make_script_runner();
    EXPECT_NE(runner, nullptr);
    return runner->run(body, timeout);
}

}  // namespace

TEST(ScriptRunnerWindows, CapturesOutputAndAZeroExitCode) {
    const lm::core::ScriptOutcome outcome = run("Write-Output 'hello from the script'\nexit 0\n");

    EXPECT_EQ(outcome.exit_code, 0);
    EXPECT_EQ(outcome.status(), lm::core::ScriptStatus::Completed);
    EXPECT_NE(outcome.stdout_text.find("hello from the script"), std::string::npos)
        << outcome.stdout_text;
}

TEST(ScriptRunnerWindows, ReportsANonZeroExitCode) {
    const lm::core::ScriptOutcome outcome = run("exit 3\n");

    EXPECT_EQ(outcome.exit_code, 3);
    EXPECT_EQ(outcome.status(), lm::core::ScriptStatus::Failed);
}

TEST(ScriptRunnerWindows, CapturesStandardError) {
    const lm::core::ScriptOutcome outcome =
        run("[Console]::Error.WriteLine('something broke')\nexit 1\n");

    EXPECT_NE(outcome.stderr_text.find("something broke"), std::string::npos)
        << outcome.stderr_text;
}

TEST(ScriptRunnerWindows, ParsesTheScriptsOwnVerdict) {
    const lm::core::ScriptOutcome outcome = run(
        "$r = @{ ok = $false; message = 'two of three failed' } | ConvertTo-Json -Compress\n"
        "Write-Output \"LM-RESULT: $r\"\n"
        "exit 0\n");

    ASSERT_TRUE(outcome.reported.has_value()) << outcome.stdout_text;
    EXPECT_FALSE(outcome.reported->ok);
    EXPECT_EQ(outcome.reported->message, "two of three failed");
    EXPECT_EQ(outcome.status(), lm::core::ScriptStatus::Failed)
        << "a reported failure overrides a zero exit code";
}

TEST(ScriptRunnerWindows, KillsAScriptThatOverrunsItsTimeout) {
    const auto started = std::chrono::steady_clock::now();
    const lm::core::ScriptOutcome outcome = run("Start-Sleep -Seconds 30\n", 2s);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(outcome.timed_out);
    EXPECT_LT(elapsed, 15s) << "the timeout did not actually stop it";
}

TEST(ScriptRunnerWindows, DoesNotHangOnAScriptThatAsksForInput) {
    // -NonInteractive matters: without it a prompt blocks until the timeout
    // with nothing in the output to say why.
    const lm::core::ScriptOutcome outcome = run("$x = Read-Host 'name'\nexit 0\n", 10s);

    EXPECT_FALSE(outcome.timed_out)
        << "a prompting script should fail immediately, not hang";
}

TEST(ScriptRunnerWindows, CapsRunawayOutput) {
    const lm::core::ScriptOutcome outcome =
        run("1..200000 | ForEach-Object { Write-Output 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' }\nexit 0\n",
            60s);

    EXPECT_LE(outcome.stdout_text.size(), 70u * 1024u)
        << "output must be capped before it reaches the wire";
    EXPECT_NE(outcome.stdout_text.find("truncated"), std::string::npos)
        << "truncation must be visible, not silent";
}

TEST(ScriptRunnerWindows, KillsWhatTheScriptStartedToo) {
    // The whole reason for the job object: a script that launched an installer
    // must not leave the installer running once the timeout has "killed" it.
    // Two markers rather than one, so a Start-Process that never ran at all
    // fails this test instead of passing it vacuously.
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path started_marker = directory / "lm-script-grandchild-started.txt";
    const std::filesystem::path survived_marker = directory / "lm-script-grandchild-survived.txt";
    std::filesystem::remove(started_marker);
    std::filesystem::remove(survived_marker);

    const std::string body =
        std::string(R"PS($grandchild = "Set-Content -Path ')PS") + started_marker.string() +
        R"PS(' -Value started; Start-Sleep -Seconds 6; Set-Content -Path ')PS" +
        survived_marker.string() +
        R"PS(' -Value survived"
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-Command',$grandchild
Start-Sleep -Seconds 30
)PS";

    const lm::core::ScriptOutcome outcome = run(body, 3s);
    ASSERT_TRUE(outcome.timed_out) << outcome.stderr_text;

    // Well past the point at which the grandchild would write its second marker.
    std::this_thread::sleep_for(10s);

    EXPECT_TRUE(std::filesystem::exists(started_marker))
        << "the script never started anything, so this would prove nothing";
    EXPECT_FALSE(std::filesystem::exists(survived_marker))
        << "the process the script started outlived the timeout that killed the script";

    std::filesystem::remove(started_marker);
    std::filesystem::remove(survived_marker);
}
