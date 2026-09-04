#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
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

/// Everything this process writes carries its own pid, so a second copy of
/// this binary running at the same time cannot contaminate what is counted or
/// which marker file is read.
std::string pid_suffix() { return std::to_string(GetCurrentProcessId()); }

std::filesystem::path executable_directory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD written =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), written)).parent_path();
}

/// The two places the runner will write its temporary .ps1, counting only the
/// files this process could have left.
std::size_t temporary_scripts_left_behind() {
    const std::string prefix = "lm-script-" + pid_suffix() + "-";
    std::size_t found = 0;
    for (const std::filesystem::path& directory :
         {executable_directory(), std::filesystem::temp_directory_path()}) {
        if (directory.empty() || !std::filesystem::exists(directory)) {
            continue;
        }
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ignored)) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(prefix) && entry.path().extension() == ".ps1") {
                ++found;
            }
        }
    }
    return found;
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
    EXPECT_EQ(outcome.status(), lm::core::ScriptStatus::Error)
        << "a killed run reached no verdict of its own";
}

TEST(ScriptRunnerWindows, DoesNotHangOnAScriptThatAsksForInput) {
    // -NonInteractive matters: without it a prompt blocks until the timeout
    // with nothing in the output to say why.
    const lm::core::ScriptOutcome outcome = run("$x = Read-Host 'name'\nexit 0\n", 10s);

    EXPECT_FALSE(outcome.timed_out)
        << "a prompting script should fail immediately, not hang";
}

TEST(ScriptRunnerWindows, CapsRunawayOutput) {
    // 20,000 lines of 30 characters is ~600 KB against a 64 KB cap, which
    // overruns it nearly ten times over and fills the pipe many times before
    // that -- proving the same thing as ten times the lines did, in a second
    // rather than twenty.
    const lm::core::ScriptOutcome outcome =
        run("1..20000 | ForEach-Object { Write-Output 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' }\nexit 0\n",
            60s);

    EXPECT_LE(outcome.stdout_text.size(), 70u * 1024u)
        << "output must be capped before it reaches the wire";
    EXPECT_NE(outcome.stdout_text.find("truncated"), std::string::npos)
        << "truncation must be visible, not silent";
}

TEST(ScriptRunnerWindows, LeavesNoTemporaryScriptBehind) {
    // The body has to reach disk because powershell.exe -File takes a path,
    // so every route out has to take the file with it -- otherwise a machine
    // accumulates one .ps1 for every script it was ever sent. Checked on the
    // path that returns normally and on the one that kills the process, since
    // the second unwinds through a different part of the function.
    const std::size_t before = temporary_scripts_left_behind();

    run("Write-Output 'done'\nexit 0\n");
    EXPECT_EQ(temporary_scripts_left_behind(), before) << "after a run that finished";

    run("Start-Sleep -Seconds 30\n", 2s);
    EXPECT_EQ(temporary_scripts_left_behind(), before) << "after a run that was killed";
}

TEST(ScriptRunnerWindows, KillsWhatTheScriptStartedToo) {
    // The whole reason for the job object: a script that launched an installer
    // must not leave the installer running once the timeout has "killed" it.
    // Two markers rather than one, so a Start-Process that never ran at all
    // fails this test instead of passing it vacuously.
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    const std::filesystem::path started_marker =
        directory / ("lm-script-grandchild-started-" + pid_suffix() + ".txt");
    const std::filesystem::path survived_marker =
        directory / ("lm-script-grandchild-survived-" + pid_suffix() + ".txt");
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
