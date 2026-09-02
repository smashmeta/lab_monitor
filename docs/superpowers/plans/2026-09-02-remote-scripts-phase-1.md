# Remote Script Execution — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A server operator can type a PowerShell script, pick hosts, press Run, and
watch each host's outcome arrive live.

**Architecture:** Two new `VOLATILE` DDS topics carry a command out and a result back,
keyed by host id. The client executes on a thread of its own so the 10 s announce is
never blocked, refuses anything it is not enrolled for, and ignores a `run_id` it has
already seen. The server owns a pure run state machine and a Scripts tab over it.

**Tech Stack:** C++23, Qt 6 Widgets, Fast DDS 3.6 + FastCDR, gtest, spdlog.

**Spec:** `docs/superpowers/specs/2026-09-02-remote-scripts-design.md`

## Global Constraints

- **Build with the VS 2026 CMake**, never the one on PATH. Configure:
  `"C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows`
  Build: `... --build --preset windows-debug`. Test: `...\ctest.exe --preset windows-debug`.
- **Adding a source file or a subdirectory requires re-running configure**, not just build.
- **`lm_core` depends on `nlohmann-json` and nothing else.** No Qt, no DDS, no syscalls.
  This is load-bearing; adding any other dependency to `libs/core` is a design violation.
- **C++23.** Types `PascalCase`, functions `snake_case`, private members `trailing_`,
  include prefix `lm/<lib>/…`, `/W4` warnings-as-errors.
- **A new `Q_OBJECT` class in `lm_ui` needs `LM_UI_EXPORT`** or it links today and fails
  the first time someone casts to it. `Q_OBJECT` headers under `include/` must be listed
  explicitly in `add_library` sources — AUTOMOC will not find them otherwise.
- **Keep `HostState` and `Capability` switches exhaustive** — no `default:`.
- **gtest cannot stream `QString` or `std::variant`** into a failure message. Use
  `.toStdString()` and `EXPECT_TRUE(a == b)`.
- **Nothing periodic is logged.** Script dispatch and results are events, so they *are*
  logged, at `info`.
- Commit after every task. Do not push.

---

### Task 1: Elevation detection and two new capabilities

**Files:**
- Modify: `libs/core/include/lm/core/types.hpp`
- Modify: `libs/core/src/types.cpp`
- Create: `libs/platform/src/windows/elevation_windows.cpp`
- Create: `libs/platform/src/linux/elevation_linux.cpp`
- Modify: `libs/platform/include/lm/platform/probes.hpp`
- Modify: `libs/platform/CMakeLists.txt`
- Test: `libs/core/tests/test_capabilities.cpp` (create)
- Test: `libs/platform/tests/test_elevation.cpp` (create)

**Interfaces:**
- Consumes: `lm::core::Capability`, `lm::core::Capabilities`
- Produces: `lm::core::Capability::Scripts`, `lm::core::Capability::Elevated`,
  `lm::platform::is_elevated() -> bool`

- [ ] **Step 1: Write the failing capability test**

Create `libs/core/tests/test_capabilities.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lm/core/types.hpp"

using namespace lm::core;

TEST(Capabilities, CarriesScriptsAndElevatedIndependently) {
    Capabilities caps;
    caps.add(Capability::Scripts);

    EXPECT_TRUE(caps.has(Capability::Scripts));
    EXPECT_FALSE(caps.has(Capability::Elevated))
        << "enrolled for scripts is not the same as able to run them elevated";

    caps.add(Capability::Elevated);
    EXPECT_TRUE(caps.has(Capability::Elevated));
}

TEST(Capabilities, NamesTheNewOnes) {
    // Shown wherever a rule or a run says why it could not proceed.
    EXPECT_FALSE(to_string(Capability::Scripts).empty());
    EXPECT_FALSE(to_string(Capability::Elevated).empty());
}

TEST(Capabilities, TheNewBitsDoNotCollideWithTheExisting) {
    // The raw value rides the announce as a bitmask, so a collision would make
    // one capability silently imply another.
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Dds);
    const std::uint32_t before = caps.raw();

    caps.add(Capability::Scripts).add(Capability::Elevated);
    EXPECT_EQ(before & caps.raw(), before);
    EXPECT_NE(caps.raw(), before);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lm_core_tests.exe --gtest_filter="Capabilities.*"`
Expected: compile error — `Capability::Scripts` is not a member.

- [ ] **Step 3: Add the enumerators**

In `libs/core/include/lm/core/types.hpp`, after `Dds`:

```cpp
    /// The machine is enrolled for remote script execution. Off unless the
    /// client was started with --allow-scripts: an agent upgrade must never
    /// silently turn a monitoring box into one that runs remote code.
    Scripts   = 1u << 6,
    /// The agent holds an elevated token, so a script it runs can install or
    /// uninstall. Reported separately from Scripts because they fail
    /// differently: not enrolled is a policy decision, not elevated is a
    /// machine that will try and be denied.
    Elevated  = 1u << 7,
```

In `libs/core/src/types.cpp`, add the two arms to `to_string(Capability)`:

```cpp
        case Capability::Scripts:  return "Scripts";
        case Capability::Elevated: return "Elevated";
```

Register the new test file in `libs/core/CMakeLists.txt` by adding
`tests/test_capabilities.cpp` to the `lm_add_test(lm_core_tests ... SOURCES ...)` list.

- [ ] **Step 4: Reconfigure, build, and run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_core_tests
build\windows\bin\Debug\lm_core_tests.exe --gtest_filter="Capabilities.*"
```
Expected: PASS, 3 tests.

- [ ] **Step 5: Write the failing elevation test**

Create `libs/platform/tests/test_elevation.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lm/platform/probes.hpp"

TEST(Elevation, AnswersWithoutThrowing) {
    // The value depends on how the test runner was launched, so the assertion
    // is that the question is answerable at all -- a throwing or hanging
    // implementation would take the agent's startup with it.
    EXPECT_NO_THROW({
        const bool elevated = lm::platform::is_elevated();
        (void)elevated;
    });
}

TEST(Elevation, IsStableWithinAProcess) {
    // A process's token does not change under it, so two calls must agree.
    // If they ever disagree the capability advertised on the announce would
    // flap, and the server would show a host gaining and losing elevation.
    EXPECT_EQ(lm::platform::is_elevated(), lm::platform::is_elevated());
}
```

- [ ] **Step 6: Run it and watch it fail**

Run: `build\windows\bin\Debug\lm_platform_tests.exe --gtest_filter="Elevation.*"`
Expected: compile error — `is_elevated` is not a member of `lm::platform`.

- [ ] **Step 7: Declare and implement it**

In `libs/platform/include/lm/platform/probes.hpp`, before the closing namespace:

```cpp
/// Whether this process holds an elevated token.
///
/// Constant for the life of the process: a token does not change underneath a
/// running program, which is why the announce can carry it as a capability
/// rather than re-checking per script.
[[nodiscard]] bool is_elevated();
```

Create `libs/platform/src/windows/elevation_windows.cpp`:

```cpp
#include "lm/platform/probes.hpp"

#include <windows.h>

namespace lm::platform {

bool is_elevated() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        // No token to ask about. Reporting "not elevated" is the safe answer:
        // it under-promises, so the server warns rather than dispatching a
        // script that will be denied.
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

}  // namespace lm::platform
```

Create `libs/platform/src/linux/elevation_linux.cpp`:

```cpp
#include "lm/platform/probes.hpp"

#include <unistd.h>

namespace lm::platform {

bool is_elevated() { return geteuid() == 0; }

}  // namespace lm::platform
```

In `libs/platform/CMakeLists.txt`, add `src/windows/elevation_windows.cpp` to the
`if(WIN32)` source list and `src/linux/elevation_linux.cpp` to the `else()` list, and add
`tests/test_elevation.cpp` to `lm_platform_test_sources` (the platform-independent list,
not the `if(WIN32)` one — both platforms have an implementation).

- [ ] **Step 8: Reconfigure, build, run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug
build\windows\bin\Debug\lm_platform_tests.exe --gtest_filter="Elevation.*"
```
Expected: PASS, 2 tests.

- [ ] **Step 9: Commit**

```bash
git add libs/core libs/platform
git commit -m "feat: capabilities for script execution and elevation

Scripts says the machine is enrolled; Elevated says a script it runs can
install or uninstall. Two bits rather than one because they fail
differently -- not enrolled is a policy decision somebody made, not
elevated is a machine that will try and be denied -- and the server needs
to warn about the second before dispatching rather than after.

is_elevated() answers from TokenElevation and reports false when it cannot
ask, which under-promises on purpose: the server then warns instead of
dispatching a script that would be denied."
```

---

### Task 2: The script result type and its reported-verdict parsing

**Files:**
- Create: `libs/core/include/lm/core/script.hpp`
- Create: `libs/core/src/script.cpp`
- Modify: `libs/core/CMakeLists.txt`
- Test: `libs/core/tests/test_script_result.cpp` (create)

**Interfaces:**
- Produces: `lm::core::ScriptStatus`, `lm::core::ReportedResult`,
  `lm::core::ScriptOutcome`, `lm::core::parse_reported_result(std::string_view) ->
  std::optional<ReportedResult>`

- [ ] **Step 1: Write the failing test**

Create `libs/core/tests/test_script_result.cpp`:

```cpp
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
```

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lm_core_tests.exe --gtest_filter="ReportedResult.*:ScriptOutcome.*"`
Expected: compile error — `lm/core/script.hpp` does not exist.

- [ ] **Step 3: Write the header**

Create `libs/core/include/lm/core/script.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lm::core {

/// What became of one script on one host.
enum class ScriptStatus {
    Completed,  ///< ran, and reported success
    Failed,     ///< ran, and reported failure
    Refused,    ///< the host would not run it — see refusal_reason
    Error       ///< could not be run: timed out, or the shell would not start
};

[[nodiscard]] std::string to_string(ScriptStatus status);

/// A script's own verdict, from its optional `LM-RESULT:` line.
struct ReportedResult {
    bool ok = false;
    std::string message;
    friend bool operator==(const ReportedResult&, const ReportedResult&) = default;
};

/// The last `LM-RESULT:` line in the output, if there is a well-formed one.
///
/// Optional by design. Nothing is required of a script, so an existing one
/// works unchanged and is judged on its exit code alone -- and the empty
/// optional keeps "said nothing" distinguishable from "said it succeeded",
/// which a plain bool could not.
///
/// A malformed marker yields nullopt rather than an error: a broken line must
/// only ever fail to add detail, never turn a script that exited 0 into a
/// failure.
[[nodiscard]] std::optional<ReportedResult> parse_reported_result(std::string_view output);

/// One execution, as the client observed it.
struct ScriptOutcome {
    std::int32_t exit_code = 0;
    std::optional<ReportedResult> reported;
    std::string stdout_text;
    std::string stderr_text;
    std::uint64_t duration_ms = 0;
    /// Set when the process had to be killed rather than exiting on its own.
    bool timed_out = false;

    /// Completed only when the process exited 0 *and* did not report a failure.
    ///
    /// The asymmetry is deliberate. A reported failure overrides a zero exit
    /// code, because PowerShell readily exits 0 after catching its own error.
    /// A reported success does not override a non-zero exit code, because that
    /// is the process itself saying it failed -- which is also exactly what a
    /// crash occurring after the marker was written would look like.
    [[nodiscard]] ScriptStatus status() const;
};

}  // namespace lm::core
```

- [ ] **Step 4: Write the implementation**

Create `libs/core/src/script.cpp`:

```cpp
#include "lm/core/script.hpp"

#include <string>

#include <nlohmann/json.hpp>

namespace lm::core {
namespace {

constexpr std::string_view kMarker = "LM-RESULT:";

}  // namespace

std::string to_string(ScriptStatus status) {
    switch (status) {
        case ScriptStatus::Completed: return "Completed";
        case ScriptStatus::Failed:    return "Failed";
        case ScriptStatus::Refused:   return "Refused";
        case ScriptStatus::Error:     return "Error";
    }
    return "Unknown";
}

std::optional<ReportedResult> parse_reported_result(std::string_view output) {
    std::optional<ReportedResult> found;

    std::size_t line_start = 0;
    while (line_start <= output.size()) {
        const std::size_t line_end = output.find('\n', line_start);
        std::string_view line = output.substr(
            line_start, line_end == std::string_view::npos ? line_end : line_end - line_start);
        // Trailing \r, because the output came from a Windows process.
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // starts_with, not find: "see LM-RESULT: for details" is prose, not a
        // verdict, and a script that mentions the convention must not trip it.
        if (line.starts_with(kMarker)) {
            std::string_view payload = line.substr(kMarker.size());
            while (!payload.empty() && payload.front() == ' ') {
                payload.remove_prefix(1);
            }
            // Deliberately non-throwing: a malformed marker adds no detail
            // rather than failing the run.
            const nlohmann::json parsed =
                nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_object() && parsed.contains("ok") && parsed["ok"].is_boolean()) {
                ReportedResult result;
                result.ok = parsed["ok"].get<bool>();
                if (parsed.contains("message") && parsed["message"].is_string()) {
                    result.message = parsed["message"].get<std::string>();
                }
                // Last one wins: a script may report progress and then a verdict.
                found = result;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    return found;
}

ScriptStatus ScriptOutcome::status() const {
    if (exit_code != 0) {
        return ScriptStatus::Failed;
    }
    if (reported.has_value() && !reported->ok) {
        return ScriptStatus::Failed;
    }
    return ScriptStatus::Completed;
}

}  // namespace lm::core
```

Add `src/script.cpp` to `add_library(lm_core ...)` and `tests/test_script_result.cpp` to
the `lm_core_tests` sources in `libs/core/CMakeLists.txt`.

- [ ] **Step 5: Reconfigure, build, run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_core_tests
build\windows\bin\Debug\lm_core_tests.exe --gtest_filter="ReportedResult.*:ScriptOutcome.*"
```
Expected: PASS, 10 tests.

- [ ] **Step 6: Commit**

```bash
git add libs/core
git commit -m "feat: a script's own verdict, parsed from its optional LM-RESULT line

The exit code is the baseline, so every existing script works unchanged.
A script that wants to say why writes a final LM-RESULT line, and the
optional keeps 'said nothing' distinguishable from 'said it succeeded'.

Two asymmetries are deliberate. A reported failure overrides a zero exit
code, because PowerShell exits 0 readily after catching its own error. A
reported success does not override a non-zero exit code, because that is
the process saying it failed -- which is also what a crash after the
marker would look like.

A malformed marker yields nothing rather than an error: a broken line must
only fail to add detail, never turn a script that exited 0 into a failure.
starts_with rather than find, so prose mentioning the convention does not
trip it."
```

---

### Task 3: Wire messages and codecs

**Files:**
- Modify: `libs/transport/include/lm/transport/messages.hpp`
- Modify: `libs/transport/include/lm/transport/codec.hpp`
- Modify: `libs/transport/src/codec.cpp`
- Test: `libs/transport/tests/test_codec.cpp`

**Interfaces:**
- Consumes: `lm::core::ScriptStatus`, `lm::core::ReportedResult`
- Produces: `lm::transport::ScriptCommand`, `lm::transport::ScriptResultMessage`, their
  `encode`/`decode`/`key_of` overloads

- [ ] **Step 1: Write the failing test**

Append to `libs/transport/tests/test_codec.cpp`:

```cpp
TEST(Codec, ScriptCommandRoundTrips) {
    ScriptCommand original;
    original.host_id = "PC-001";
    original.run_id = "run-7f3a";
    original.script_name = "Maintenance/Clear-TempFiles.ps1";
    original.script_body = "Write-Output \"hello\"\nexit 0\n";
    original.timeout_seconds = 120;

    ScriptCommand decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded, original);
}

TEST(Codec, ScriptCommandSurvivesABodyWithEveryAwkwardCharacter) {
    // A PowerShell script is not a tidy identifier: quotes, backslashes,
    // newlines and UTF-8 all appear routinely, and a codec that mangles any of
    // them corrupts the script silently on its way to a hundred machines.
    ScriptCommand original;
    original.host_id = "PC-001";
    original.run_id = "run-1";
    original.script_name = "(custom script)";
    original.script_body = "$p = \"C:\\Program Files\\Acme\"\r\n# \xc3\xa5\xc3\xa4\xc3\xb6\nexit 0";

    ScriptCommand decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded.script_body, original.script_body);
}

TEST(Codec, ScriptResultRoundTrips) {
    ScriptResultMessage original;
    original.host_id = "PC-001";
    original.run_id = "run-7f3a";
    original.status = lm::core::ScriptStatus::Failed;
    original.exit_code = 3;
    original.has_reported = true;
    original.reported_ok = false;
    original.reported_message = "3 of 5 packages failed";
    original.stdout_text = "line one\nline two";
    original.stderr_text = "something went wrong";
    original.duration_ms = 4200;

    ScriptResultMessage decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded, original);
}

TEST(Codec, ScriptResultCarriesARefusalWithItsReason) {
    ScriptResultMessage original;
    original.host_id = "PC-001";
    original.run_id = "run-7f3a";
    original.status = lm::core::ScriptStatus::Refused;
    original.refusal_reason = "not enrolled for scripts";

    ScriptResultMessage decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded.status, lm::core::ScriptStatus::Refused);
    EXPECT_EQ(decoded.refusal_reason, "not enrolled for scripts");
}

TEST(Codec, ATruncatedScriptCommandIsRejected) {
    // Unlike ClientAnnounce, there is no tolerance here. A half-read command is
    // a half-read script body, and running that would be worse than running
    // nothing at all.
    ScriptCommand original;
    original.host_id = "PC-001";
    original.run_id = "run-1";
    original.script_body = "exit 0";

    std::vector<std::uint8_t> bytes = encode(original);
    bytes.resize(bytes.size() / 2);

    ScriptCommand decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, ScriptMessagesAreKeyedByHost) {
    ScriptCommand command;
    command.host_id = "PC-001";
    EXPECT_EQ(key_of(command), "PC-001");

    ScriptResultMessage result;
    result.host_id = "PC-002";
    EXPECT_EQ(key_of(result), "PC-002");
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lm_transport_tests.exe --gtest_filter="Codec.Script*"`
Expected: compile error — `ScriptCommand` is not a member of `lm::transport`.

- [ ] **Step 3: Add the message structs**

In `libs/transport/include/lm/transport/messages.hpp`, add `#include "lm/core/script.hpp"`
at the top and these structs before the closing namespace:

```cpp
/// Topic: ScriptCommand. Reliable, **Volatile**, keyed by host_id.
///
/// Volatile is not an oversight and must not be "fixed" to TransientLocal. Every
/// other topic here carries state, where a late joiner should receive the last
/// value. A command is an event: with transient-local durability a client that
/// restarts would receive and execute whatever it missed, so rebooting a machine
/// could silently re-run last week's uninstall.
struct ScriptCommand {
    core::HostId host_id;
    /// Correlates the result back to its run. The client also remembers the
    /// ones it has executed, so a redelivered sample runs once.
    std::string run_id;
    /// For the logs and the audit trail; "(custom script)" when typed in.
    std::string script_name;
    /// The full text. The server always sends the body, never a name -- the
    /// client never reads the share and needs no access to it.
    std::string script_body;
    std::uint32_t timeout_seconds = 120;
    friend bool operator==(const ScriptCommand&, const ScriptCommand&) = default;
};

/// Topic: ScriptResult. Reliable, Volatile, keyed by host_id.
struct ScriptResultMessage {
    core::HostId host_id;
    std::string run_id;
    core::ScriptStatus status = core::ScriptStatus::Error;
    /// Set when status is Refused: not enrolled, already running, no shell.
    std::string refusal_reason;
    std::int32_t exit_code = 0;
    /// Whether the script wrote an LM-RESULT line at all. Kept as a separate
    /// flag rather than a sentinel in reported_ok, so "said nothing" stays
    /// distinguishable from "said it failed" on the wire as well as in core.
    bool has_reported = false;
    bool reported_ok = false;
    std::string reported_message;
    std::string stdout_text;
    std::string stderr_text;
    std::uint64_t duration_ms = 0;
    friend bool operator==(const ScriptResultMessage&, const ScriptResultMessage&) = default;
};
```

- [ ] **Step 4: Declare the codec overloads**

In `libs/transport/include/lm/transport/codec.hpp`, add:

```cpp
[[nodiscard]] std::vector<std::uint8_t> encode(const ScriptCommand& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ScriptResultMessage& message);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ScriptCommand& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ScriptResultMessage& out);
[[nodiscard]] std::string key_of(const ScriptCommand& message);
[[nodiscard]] std::string key_of(const ScriptResultMessage& message);
```

- [ ] **Step 5: Implement them**

In `libs/transport/src/codec.cpp`, before `key_of`:

```cpp
// --- ScriptCommand ----------------------------------------------------------

std::vector<std::uint8_t> encode(const ScriptCommand& message) {
    return serialise([&](Cdr& writer) {
        writer << message.host_id << message.run_id << message.script_name
               << message.script_body << message.timeout_seconds;
    });
}

bool decode(std::span<const std::uint8_t> bytes, ScriptCommand& out) {
    ScriptCommand parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.host_id >> parsed.run_id >> parsed.script_name >>
            parsed.script_body >> parsed.timeout_seconds;
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
}

// --- ScriptResultMessage ----------------------------------------------------

std::vector<std::uint8_t> encode(const ScriptResultMessage& message) {
    return serialise([&](Cdr& writer) {
        writer << message.host_id << message.run_id
               << static_cast<std::uint32_t>(message.status) << message.refusal_reason
               << message.exit_code << message.has_reported << message.reported_ok
               << message.reported_message << message.stdout_text << message.stderr_text
               << message.duration_ms;
    });
}

bool decode(std::span<const std::uint8_t> bytes, ScriptResultMessage& out) {
    ScriptResultMessage parsed;
    std::uint32_t status = 0;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.host_id >> parsed.run_id >> status >> parsed.refusal_reason >>
            parsed.exit_code >> parsed.has_reported >> parsed.reported_ok >>
            parsed.reported_message >> parsed.stdout_text >> parsed.stderr_text >>
            parsed.duration_ms;
    });
    if (!ok) {
        return false;
    }
    // Anything outside the enum becomes Error rather than a value no switch
    // handles: the wire is not trusted to stay in range.
    switch (status) {
        case 0: parsed.status = core::ScriptStatus::Completed; break;
        case 1: parsed.status = core::ScriptStatus::Failed; break;
        case 2: parsed.status = core::ScriptStatus::Refused; break;
        default: parsed.status = core::ScriptStatus::Error; break;
    }
    out = std::move(parsed);
    return true;
}
```

And with the other `key_of` definitions:

```cpp
std::string key_of(const ScriptCommand& message) { return message.host_id; }
std::string key_of(const ScriptResultMessage& message) { return message.host_id; }
```

- [ ] **Step 6: Build and run**

```
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_transport_tests
build\windows\bin\Debug\lm_transport_tests.exe --gtest_filter="Codec.Script*"
```
Expected: PASS, 6 tests.

- [ ] **Step 7: Commit**

```bash
git add libs/transport
git commit -m "feat: wire format for script commands and results

Two messages, both keyed by host. ScriptCommand carries the body rather
than a name, so the client never reads the share and needs no access to
it -- and the two ends cannot disagree about what a filename contains.

The header records that the topic is Volatile and why it must stay that
way: every other topic here carries state, where a late joiner should get
the last value, but a command is an event, and transient-local durability
would mean a rebooted machine silently re-running last week's uninstall.

A truncated command is rejected outright, unlike ClientAnnounce which
tolerates a short buffer. A half-read announce costs a stale capability
flag; a half-read command is a half-read script body, and running that is
worse than running nothing. A status outside the enum decodes as Error
rather than a value no switch handles."
```

---

### Task 4: Carry the two topics over the in-memory bus

**Files:**
- Modify: `libs/transport/include/lm/transport/in_memory_transport.hpp`
- Modify: `libs/transport/src/in_memory_transport.cpp`
- Modify: `libs/transport/include/lm/transport/transport.hpp`
- Test: `libs/transport/tests/test_in_memory_transport.cpp`

**Interfaces:**
- Consumes: `ScriptCommand`, `ScriptResultMessage`
- Produces: `IClientTransport::publish_script_result`, `IClientTransport::on_script_command`,
  `IServerTransport::publish_script_command`, `IServerTransport::on_script_result`,
  `MessageBus::publish_script_command`, `MessageBus::subscribe_script_command`,
  `MessageBus::publish_script_result`, `MessageBus::subscribe_script_result`

- [ ] **Step 1: Write the failing test**

Append to `libs/transport/tests/test_in_memory_transport.cpp`:

```cpp
TEST(InMemoryTransport, DeliversAScriptCommandFromServerToClient) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<ScriptCommand> seen;
    client->on_script_command([&](const ScriptCommand& command) { seen.push_back(command); });

    ScriptCommand command;
    command.host_id = "PC-001";
    command.run_id = "run-1";
    command.script_body = "exit 0";
    server->publish_script_command(command);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front().run_id, "run-1");
}

TEST(InMemoryTransport, DeliversAScriptResultFromClientToServer) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<ScriptResultMessage> seen;
    server->on_script_result([&](const ScriptResultMessage& result) { seen.push_back(result); });

    ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = "run-1";
    result.status = lm::core::ScriptStatus::Completed;
    client->publish_script_result(result);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front().status, lm::core::ScriptStatus::Completed);
}

TEST(InMemoryTransport, DoesNotReplayACommandToALateSubscriber) {
    // The in-memory bus must model the topic's Volatile durability, or every
    // test built on it would prove the opposite of what production does.
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    ScriptCommand command;
    command.host_id = "PC-001";
    command.run_id = "run-1";
    server->publish_script_command(command);

    std::vector<ScriptCommand> seen;
    client->on_script_command([&](const ScriptCommand& c) { seen.push_back(c); });

    EXPECT_TRUE(seen.empty()) << "a command must not be replayed to a client that joined later";
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lm_transport_tests.exe --gtest_filter="InMemoryTransport.*Script*"`
Expected: compile error — no member `on_script_command`.

- [ ] **Step 3: Extend the transport interfaces**

In `libs/transport/include/lm/transport/transport.hpp`, add to `IClientTransport`:

```cpp
    virtual void publish_script_result(const ScriptResultMessage& message) = 0;

    /// Commands addressed to this host. Never replayed on reconnect: the topic
    /// is Volatile, so a client that restarts does not re-run what it missed.
    virtual void on_script_command(std::function<void(const ScriptCommand&)> handler) = 0;
```

and to `IServerTransport`:

```cpp
    virtual void publish_script_command(const ScriptCommand& message) = 0;

    virtual void on_script_result(std::function<void(const ScriptResultMessage&)> handler) = 0;
```

- [ ] **Step 4: Extend the bus and both in-memory transports**

In `libs/transport/include/lm/transport/in_memory_transport.hpp`, add to `MessageBus`:

```cpp
    void publish_script_command(const ScriptCommand& message);
    void publish_script_result(const ScriptResultMessage& message);

    /// Deliberately no replay, matching the topic's Volatile durability.
    void subscribe_script_command(std::function<void(const ScriptCommand&)> handler);
    void subscribe_script_result(std::function<void(const ScriptResultMessage&)> handler);
```

with the two matching handler vectors as private members. Implement them in
`libs/transport/src/in_memory_transport.cpp` exactly as the announce handlers are
implemented — a loop over the handler vector, and **no retained value**. Then implement
the four new virtuals on the in-memory client and server classes by forwarding to the bus.

- [ ] **Step 5: Build and run**

```
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_transport_tests
build\windows\bin\Debug\lm_transport_tests.exe --gtest_filter="InMemoryTransport.*"
```
Expected: PASS, including the three new tests.

- [ ] **Step 6: Commit**

```bash
git add libs/transport
git commit -m "feat: carry script commands and results over the in-memory bus

The in-memory bus deliberately does not retain a command, mirroring the
topic's Volatile durability. Without that, every test built on this bus
would prove the opposite of what production does -- and the one behaviour
most worth not getting wrong here is that a client which joins late does
not receive, and run, a command it missed."
```

---

### Task 5: Fast DDS implementation of the two topics

**Files:**
- Modify: `libs/transport/src/fastdds/fast_dds_transport.cpp`
- Test: `libs/transport/tests/test_dds_loopback.cpp` (integration-gated)

**Interfaces:**
- Consumes: the four virtuals from Task 4
- Produces: nothing new; this makes them work over a real domain

- [ ] **Step 1: Write the failing integration test**

Append to the integration-gated loopback test file:

```cpp
TEST(FastDdsLoopback, AScriptCommandReachesTheClientAndItsResultComesBack) {
    MessageBus unused;
    DdsConfig config;
    config.domain_id = 71;  // a domain of its own, away from the other tests
    const auto server = make_dds_server(config);
    const auto client = make_dds_client(config);

    std::vector<ScriptCommand> commands;
    client->on_script_command([&](const ScriptCommand& c) { commands.push_back(c); });
    std::vector<ScriptResultMessage> results;
    server->on_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    ScriptCommand command;
    command.host_id = "PC-001";
    command.run_id = "run-1";
    command.script_body = "exit 0";

    // Published in a loop: these topics are Reliable but discovery still races
    // a single publish, which is how FastDdsLoopback.ResourceSamplesReachThe
    // Server was de-flaked.
    for (int i = 0; i < 20 && commands.empty(); ++i) {
        server->publish_script_command(command);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(commands.empty()) << "no command arrived";

    ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = "run-1";
    result.status = lm::core::ScriptStatus::Completed;
    for (int i = 0; i < 20 && results.empty(); ++i) {
        client->publish_script_result(result);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(results.empty()) << "no result came back";
    EXPECT_EQ(results.front().run_id, "run-1");
}
```

- [ ] **Step 2: Run it and watch it fail**

```
"C:\Program Files\...\cmake.exe" --preset windows -DLM_BUILD_INTEGRATION_TESTS=ON
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_transport_dds_tests
build\windows\bin\Debug\lm_transport_dds_tests.exe --gtest_filter="*Script*"
```
Expected: compile error — the DDS transport does not implement the new virtuals.

- [ ] **Step 3: Add the topics**

In `libs/transport/src/fastdds/fast_dds_transport.cpp`, beside the existing constants:

```cpp
constexpr const char* kScriptCommandTopicName = "lm.transport.ScriptCommand";
constexpr const char* kScriptResultTopicName = "lm.transport.ScriptResult";
constexpr const char* kScriptCommandTypeName = "lm::transport::ScriptCommand";
constexpr const char* kScriptResultTypeName = "lm::transport::ScriptResultMessage";
```

Create the writer and reader for each following the existing announce pattern, with QoS:

```cpp
// Reliable, so a command is not dropped in transit -- but VOLATILE, so a
// client that joins later never receives it. See ScriptCommand in messages.hpp:
// changing this to TRANSIENT_LOCAL would make a rebooted machine re-run
// whatever it missed.
qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
qos.durability().kind = VOLATILE_DURABILITY_QOS;
qos.history().kind = KEEP_LAST_HISTORY_QOS;
qos.history().depth = 1;
```

The server writes `ScriptCommand` and reads `ScriptResult`; the client reads
`ScriptCommand` and writes `ScriptResult`. Read with the polling pattern the DDS probe
uses, **not** an `on_data_available` listener — a Fast DDS reader listener can match and
still never fire, which cost a day once already.

- [ ] **Step 4: Build and run**

```
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_transport_dds_tests
build\windows\bin\Debug\lm_transport_dds_tests.exe --gtest_filter="*Script*"
```
Expected: PASS.

- [ ] **Step 5: Restore the default build configuration**

```
"C:\Program Files\...\cmake.exe" --preset windows -DLM_BUILD_INTEGRATION_TESTS=OFF
"C:\Program Files\...\cmake.exe" --build --preset windows-debug
"C:\Program Files\...\ctest.exe" --preset windows-debug
```
Expected: 8/8 binaries pass.

- [ ] **Step 6: Commit**

```bash
git add libs/transport
git commit -m "feat: script topics over Fast DDS

Reliable so a command is not dropped in transit, Volatile so a client that
joins later never receives one. Read by polling rather than through an
on_data_available listener: a Fast DDS reader listener can match and still
never fire, which this project has already lost a day to once."
```

---

### Task 6: IScriptRunner, its fake, and the Windows PowerShell implementation

**Files:**
- Modify: `libs/platform/include/lm/platform/probes.hpp`
- Modify: `libs/platform/include/lm/platform/fakes.hpp`
- Create: `libs/platform/src/windows/script_runner_windows.cpp`
- Create: `libs/platform/src/linux/script_runner_linux.cpp`
- Modify: `libs/platform/CMakeLists.txt`
- Test: `libs/platform/tests/test_script_runner_windows.cpp` (create)

**Interfaces:**
- Consumes: `lm::core::ScriptOutcome`
- Produces: `lm::platform::IScriptRunner` with
  `run(const std::string& body, std::chrono::seconds timeout) -> core::ScriptOutcome`,
  `lm::platform::make_script_runner() -> std::unique_ptr<IScriptRunner>`,
  `lm::platform::FakeScriptRunner`

- [ ] **Step 1: Write the failing test**

Create `libs/platform/tests/test_script_runner_windows.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <memory>

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
```

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lm_platform_tests.exe --gtest_filter="ScriptRunnerWindows.*"`
Expected: compile error — `make_script_runner` is not a member.

- [ ] **Step 3: Declare the interface and the fake**

In `libs/platform/include/lm/platform/probes.hpp`:

```cpp
/// Runs a script body and returns what happened.
///
/// Blocking: the caller is responsible for not calling this on a thread that
/// must stay responsive. The client runs it on a thread of its own, because the
/// monitoring worker carries the 10 s announce and a 60 s script blocking it
/// would push the host past its liveliness lease -- the fleet would watch it go
/// Offline mid-run and then come back.
class IScriptRunner {
public:
    virtual ~IScriptRunner() = default;
    virtual core::ScriptOutcome run(const std::string& body,
                                    std::chrono::seconds timeout) = 0;
};

/// The runner for this platform, or nullptr where none is implemented.
[[nodiscard]] std::unique_ptr<IScriptRunner> make_script_runner();
```

with `#include <chrono>` and `#include "lm/core/script.hpp"` at the top.

In `libs/platform/include/lm/platform/fakes.hpp`:

```cpp
class FakeScriptRunner : public IScriptRunner {
public:
    core::ScriptOutcome next;
    std::vector<std::string> bodies;
    /// Blocks until released, for testing that execution does not sit on the
    /// monitoring thread.
    std::function<void()> before_returning;

    core::ScriptOutcome run(const std::string& body, std::chrono::seconds) override {
        bodies.push_back(body);
        if (before_returning) {
            before_returning();
        }
        return next;
    }
};
```

- [ ] **Step 4: Implement the Windows runner**

Create `libs/platform/src/windows/script_runner_windows.cpp`. Use `CreateProcessW` with
redirected pipes and a **job object** so the timeout kills the whole tree:

```cpp
#include "lm/platform/probes.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <string>

#include "lm/core/script.hpp"

namespace lm::platform {
namespace {

/// 64 KB per stream. A script that prints a megabyte is one whose author will
/// be glad the fleet did not try to carry it -- and the cap is enforced here
/// rather than at the wire so the memory is never allocated either.
constexpr std::size_t kMaxStreamBytes = 64u * 1024u;

void append_capped(std::string& target, const char* data, std::size_t length) {
    if (target.size() >= kMaxStreamBytes) {
        return;
    }
    const std::size_t room = kMaxStreamBytes - target.size();
    target.append(data, std::min(room, length));
    if (target.size() >= kMaxStreamBytes) {
        target += "\n[output truncated at 64 KB]";
    }
}

class WindowsScriptRunner : public IScriptRunner {
public:
    core::ScriptOutcome run(const std::string& body, std::chrono::seconds timeout) override;
};

}  // namespace

std::unique_ptr<IScriptRunner> make_script_runner() {
    return std::make_unique<WindowsScriptRunner>();
}

}  // namespace lm::platform
```

The `run` body must:
1. Write `body` to a temporary `.ps1` beside the executable and delete it at the end
   (use a scope guard so an early return still deletes it).
2. Create a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so closing the handle
   kills the process **and everything it started** — a script that launched an installer
   leaves the installer running otherwise.
3. `CreateProcessW` on
   `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "<temp>"`
   with `stdout`/`stderr` redirected to pipes and `CREATE_NO_WINDOW | CREATE_SUSPENDED`,
   assign it to the job, then resume.
4. Drain both pipes on separate threads into `stdout_text` / `stderr_text` via
   `append_capped` — reading them serially deadlocks when one fills.
5. `WaitForSingleObject` with the timeout in milliseconds. On `WAIT_TIMEOUT`, set
   `timed_out = true`, close the job handle to kill the tree, and set `exit_code` to 1.
6. Otherwise `GetExitCodeProcess` into `exit_code`.
7. Set `reported = core::parse_reported_result(stdout_text)` and `duration_ms`.

Create `libs/platform/src/linux/script_runner_linux.cpp` returning `nullptr` from
`make_script_runner()`, with a comment that Linux script execution is not implemented and
a client there advertises no `Capability::Scripts`.

Add both files to `libs/platform/CMakeLists.txt` in their platform branches, and
`tests/test_script_runner_windows.cpp` to the `if(WIN32)` test list.

- [ ] **Step 5: Reconfigure, build, run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lm_platform_tests
build\windows\bin\Debug\lm_platform_tests.exe --gtest_filter="ScriptRunnerWindows.*"
```
Expected: PASS, 7 tests.

- [ ] **Step 6: Commit**

```bash
git add libs/platform
git commit -m "feat: a PowerShell script runner behind IScriptRunner

CreateProcessW into a job object with KILL_ON_JOB_CLOSE, so a timeout kills
the process tree rather than the shell alone -- a script that launched an
installer would otherwise leave the installer running.

-NonInteractive is load-bearing: without it a script that prompts blocks
until the timeout with nothing in the output to say why, which is the
least diagnosable failure this feature can produce.

Both pipes are drained on their own threads, because reading them serially
deadlocks the moment one fills. Output is capped at 64 KB per stream at
the point of capture, so a runaway script never allocates the memory, and
the truncation says so rather than being silent."
```

---

### Task 7: The client executes commands, off the worker thread

**Files:**
- Modify: `apps/client/monitor_worker.hpp`
- Modify: `apps/client/monitor_worker.cpp`
- Modify: `apps/client/main.cpp`
- Test: `apps/client/tests/test_script_execution.cpp` (create)
- Modify: `apps/client/CMakeLists.txt`

**Interfaces:**
- Consumes: `IScriptRunner`, `FakeScriptRunner`, `ScriptCommand`, `ScriptResultMessage`,
  `Capability::Scripts`, `Capability::Elevated`
- Produces: `MonitorWorker` handling `on_script_command`; `--allow-scripts` on the client

- [ ] **Step 1: Write the failing test**

Create `apps/client/tests/test_script_execution.cpp`:

```cpp
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>

#include <chrono>
#include <memory>

#include "lm/platform/fakes.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "monitor_worker.hpp"

using namespace lm::core;
using namespace lm::transport;
using namespace std::chrono_literals;

namespace {

ScriptCommand command_for(const std::string& host, const std::string& run_id) {
    ScriptCommand command;
    command.host_id = host;
    command.run_id = run_id;
    command.script_name = "(custom script)";
    command.script_body = "exit 0";
    command.timeout_seconds = 5;
    return command;
}

/// Pumps the event loop until `done` or the deadline, so a queued result has a
/// chance to arrive without the test sleeping blindly.
void pump_until(const std::function<bool()>& done, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(10ms);
    }
}

}  // namespace

TEST(ClientScripts, RunsACommandAddressedToThisHostAndReportsTheOutcome) {
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    runner->next.exit_code = 0;
    runner->next.stdout_text = "did the thing";
    auto* runner_ptr = runner.get();

    // Harness: a worker with scripts enabled, on a host called PC-001.
    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, ScriptStatus::Completed);
    EXPECT_EQ(results.front().run_id, "run-1");
    EXPECT_EQ(results.front().stdout_text, "did the thing");
    ASSERT_EQ(runner_ptr->bodies.size(), 1u);
    EXPECT_EQ(runner_ptr->bodies.front(), "exit 0");
}

TEST(ClientScripts, IgnoresACommandAddressedToAnotherHost) {
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-002", "run-1"));
    pump_until([&] { return !results.empty(); }, 500ms);

    EXPECT_TRUE(results.empty()) << "a host must not answer for another";
    EXPECT_TRUE(runner_ptr->bodies.empty());
}

TEST(ClientScripts, RefusesWhenNotEnrolled) {
    // The opt-in is what stops an agent upgrade silently turning a monitoring
    // box into one that runs remote code, so it must refuse *visibly* rather
    // than staying quiet -- an operator needs to know why nothing happened.
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/false, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, ScriptStatus::Refused);
    EXPECT_FALSE(results.front().refusal_reason.empty());
    EXPECT_TRUE(runner_ptr->bodies.empty()) << "nothing may run when not enrolled";
}

TEST(ClientScripts, RunsARepeatedRunIdOnlyOnce) {
    // Volatile durability stops replay after a restart; it does not stop
    // redelivery within a session. Running an uninstall twice is the failure
    // this prevents.
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });
    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return results.size() > 1; }, 500ms);

    EXPECT_EQ(runner_ptr->bodies.size(), 1u) << "the same run must execute once";
}

TEST(ClientScripts, KeepsAnnouncingWhileAScriptIsRunning) {
    // The worker thread carries the 10 s announce. If execution sat on it, a
    // long script would push this host past its liveliness lease and the fleet
    // would watch it go Offline mid-run and then come back.
    MessageBus bus;
    std::atomic<bool> release{false};
    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    runner->before_returning = [&] {
        while (!release) {
            std::this_thread::sleep_for(10ms);
        }
    };
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));

    // While the script is stuck, the worker must still service its own timers.
    std::vector<ClientAnnounce> announces;
    bus.subscribe_announce([&](const ClientAnnounce& a) { announces.push_back(a); });
    harness.force_announce();
    pump_until([&] { return !announces.empty(); });
    release = true;

    EXPECT_FALSE(announces.empty()) << "the monitoring thread was blocked by the script";
}
```

`ScriptTestHarness` goes at the top of the same file:

```cpp
/// A MonitorWorker on its own thread, over the in-memory bus, exactly as
/// main() wires one -- so these tests exercise the real threading rather than
/// a convenient approximation of it.
class ScriptTestHarness {
public:
    ScriptTestHarness(MessageBus& bus, std::string host_id, bool allow_scripts,
                      std::unique_ptr<lm::platform::IScriptRunner> runner) {
        lm::platform::ProbeSet probes;
        probes.resources = std::make_unique<lm::platform::FakeResourceProbe>();

        auto host_probes = std::make_unique<lm::platform::HostProbes>(
            std::move(host_id), std::move(probes), lm::core::platform_capabilities());

        worker_ = new MonitorWorker(std::move(host_probes), make_in_memory_client(bus),
                                    std::move(runner), allow_scripts);
        thread_ = new QThread();
        worker_->moveToThread(thread_);
        thread_->start();
        QMetaObject::invokeMethod(worker_, "start", Qt::QueuedConnection);
    }

    ~ScriptTestHarness() {
        thread_->quit();
        thread_->wait();
        // Deleted synchronously: the thread is joined, so nothing can still be
        // touching it, and deleteLater() would post to a queue nobody drains.
        delete worker_;
        delete thread_;
    }

    void force_announce() {
        QMetaObject::invokeMethod(worker_, "announce", Qt::QueuedConnection);
    }

private:
    MonitorWorker* worker_ = nullptr;
    QThread* thread_ = nullptr;
};
```

`announce` must be a slot (or `Q_INVOKABLE`) for `force_announce` to reach it by name.

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lab_monitor_client_tests.exe --gtest_filter="ClientScripts.*"`
Expected: compile error — `MonitorWorker` has no script support.

- [ ] **Step 3: Extend MonitorWorker**

In `apps/client/monitor_worker.hpp`, add to the constructor a
`std::unique_ptr<lm::platform::IScriptRunner> runner` and a `bool allow_scripts`, plus:

```cpp
private slots:
    void on_script_command(const lm::transport::ScriptCommand& command);

private:
    std::unique_ptr<lm::platform::IScriptRunner> runner_;
    bool allow_scripts_ = false;
    /// run_ids already executed. Volatile durability prevents replay across a
    /// restart; this prevents a redelivered sample running twice within one.
    std::set<std::string> executed_;
    /// One at a time. A queue would be a promise about ordering and completion
    /// that a machine which may be rebooted cannot keep.
    std::atomic<bool> running_{false};
```

In `monitor_worker.cpp`, subscribe in `start()`:

```cpp
    transport_->on_script_command([this](const lm::transport::ScriptCommand& command) {
        // Queued, so the handler returns to the DDS thread immediately.
        QMetaObject::invokeMethod(
            this, [this, command] { on_script_command(command); }, Qt::QueuedConnection);
    });
```

and implement:

```cpp
void MonitorWorker::on_script_command(const lm::transport::ScriptCommand& command) {
    if (command.host_id != probes_->host_id()) {
        return;  // addressed to somebody else
    }

    const auto refuse = [this, &command](const std::string& reason) {
        spdlog::info("refusing script run {} ({}): {}", command.run_id, command.script_name,
                     reason);
        lm::transport::ScriptResultMessage message;
        message.host_id = probes_->host_id();
        message.run_id = command.run_id;
        message.status = lm::core::ScriptStatus::Refused;
        message.refusal_reason = reason;
        transport_->publish_script_result(message);
    };

    if (!allow_scripts_ || runner_ == nullptr) {
        refuse("this machine is not enrolled for script execution (--allow-scripts)");
        return;
    }
    if (!executed_.insert(command.run_id).second) {
        // Already done. Silent by design: the server has the first result, and
        // a second one would make the run view contradict itself.
        return;
    }
    if (running_.exchange(true)) {
        executed_.erase(command.run_id);  // it did not run, so let a retry through
        refuse("another script is already running on this machine");
        return;
    }

    spdlog::info("running script {} (run {})", command.script_name, command.run_id);

    // On its own thread: this one carries the 10 s announce, and a 60 s script
    // blocking it would push this host past its liveliness lease.
    const std::string body = command.script_body;
    const auto timeout = std::chrono::seconds(command.timeout_seconds);
    const std::string run_id = command.run_id;
    const std::string name = command.script_name;
    std::thread([this, body, timeout, run_id, name] {
        const lm::core::ScriptOutcome outcome = runner_->run(body, timeout);
        QMetaObject::invokeMethod(
            this,
            [this, outcome, run_id, name] {
                publish_script_outcome(run_id, name, outcome);
                running_ = false;
            },
            Qt::QueuedConnection);
    }).detach();
}
```

with a `publish_script_outcome` private helper that fills a `ScriptResultMessage` from the
outcome (including `has_reported`, `reported_ok`, `reported_message`), logs the finish at
`info` with the exit code and duration, and publishes it.

Advertise the capabilities where `capabilities` is built in `main.cpp`:

```cpp
        if (options.allow_scripts) {
            capabilities.add(lm::core::Capability::Scripts);
        }
        if (lm::platform::is_elevated()) {
            capabilities.add(lm::core::Capability::Elevated);
        }
        spdlog::info("  scripts     : {}",
                     options.allow_scripts
                         ? (lm::platform::is_elevated() ? "enabled, elevated"
                                                        : "enabled, NOT elevated -- installs "
                                                          "and uninstalls will fail")
                         : "disabled (start with --allow-scripts to enrol)");
```

and add `--allow-scripts` to `parse_options` as a `po::bool_switch`.

Add `tests/test_script_execution.cpp` to the `lab_monitor_client_tests` sources.

- [ ] **Step 4: Reconfigure, build, run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lab_monitor_client_tests
build\windows\bin\Debug\lab_monitor_client_tests.exe --gtest_filter="ClientScripts.*"
```
Expected: PASS, 5 tests.

- [ ] **Step 5: Commit**

```bash
git add apps/client
git commit -m "feat: the client executes script commands it is enrolled for

Off the monitoring thread, on one of its own. That thread carries the 10 s
announce, and a 60 s script blocking it would push the host past its
liveliness lease -- the fleet would watch it go Offline mid-run and then
come back. A test holds a fake runner open and asserts the announce still
goes out.

Refusals are published, not swallowed. An operator whose script did
nothing needs to be told the machine is not enrolled; silence is the one
response that cannot be acted on.

A repeated run_id executes once. Volatile durability stops replay across a
restart but not redelivery within a session, and running an uninstall
twice is the failure that guards against. A refusal for 'already running'
un-records the id, so a genuine retry is not swallowed as a duplicate."
```

---

### Task 8: Elevation via a scheduled task

**Files:**
- Modify: `apps/client/main.cpp`
- Create: `apps/client/autostart_windows.cpp`, `apps/client/autostart.hpp`
- Modify: `apps/client/CMakeLists.txt`
- Modify: `apps/client/detail_window.cpp`, `apps/client/detail_window.hpp`

**Interfaces:**
- Consumes: `lm::platform::is_elevated()`
- Produces: `install_autostart_task() -> std::string` (empty on success, else the reason),
  `uninstall_autostart_task() -> std::string`

- [ ] **Step 1: Add the commands**

Create `apps/client/autostart.hpp`:

```cpp
#pragma once

#include <string>

/// Registers a logon task that starts this executable with highest privileges.
///
/// This is how the agent obtains elevation without a UAC prompt. The manifest
/// is deliberately *not* requireAdministrator, which would prompt on every
/// launch including this one.
///
/// Precondition, and it is absolute: "highest privileges" elevates the logged-in
/// user's own token. On a machine whose user is not a local administrator there
/// is no administrator token to elevate to, and the task runs unelevated.
///
/// Returns an empty string on success, or a message describing what went wrong.
[[nodiscard]] std::string install_autostart_task();
[[nodiscard]] std::string uninstall_autostart_task();
```

Implement in `apps/client/autostart_windows.cpp` by invoking `schtasks.exe`:

```
schtasks /Create /F /TN "LabMonitorClient" /TR "\"<exe path>\" --allow-scripts"
         /SC ONLOGON /RL HIGHEST
```

Capture the exit code and stderr; a non-zero exit becomes the returned message. Add
`--install-autostart` and `--uninstall-autostart` to `parse_options`, handled before
anything else in `main()` and returning immediately.

- [ ] **Step 2: Warn in the detail window when not elevated**

Add to `DetailWindow` a banner label, shown only when
`lm::platform::is_elevated()` is false:

```cpp
    // Said on screen as well as in the log, because the person who will hit
    // this is the one looking at the machine after a script failed on it.
    elevation_banner_->setText(
        QStringLiteral("Not running elevated \u2014 scripts that install or uninstall "
                       "will fail. Run --install-autostart from an administrator "
                       "prompt to fix this."));
    elevation_banner_->setStyleSheet(QStringLiteral("color: %1;").arg(lm::ui::Theme::kOffline));
    elevation_banner_->setVisible(!lm::platform::is_elevated());
```

- [ ] **Step 3: Write the failing test**

Append to `apps/client/tests/test_detail_window.cpp`:

```cpp
TEST(DetailWindowElevation, ShowsTheBannerOnlyWhenNotElevated) {
    DetailWindow window(QStringLiteral("PC-001"));
    auto* banner = window.findChild<QLabel*>(QStringLiteral("ElevationBanner"));
    ASSERT_NE(banner, nullptr) << "no elevation banner";

    // The test process's own elevation decides which way this goes, so assert
    // the relationship rather than a fixed value.
    EXPECT_EQ(banner->isVisibleTo(&window), !lm::platform::is_elevated());
}
```

- [ ] **Step 4: Build and run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lab_monitor_client_tests
build\windows\bin\Debug\lab_monitor_client_tests.exe --gtest_filter="DetailWindowElevation.*"
```
Expected: PASS.

- [ ] **Step 5: Verify by hand**

From an elevated prompt: `lab_monitor_client.exe --install-autostart`, then sign out and
back in, and confirm from the log's startup banner that it reports
`scripts : enabled, elevated`. Then `--uninstall-autostart` and confirm the task is gone
with `schtasks /Query /TN LabMonitorClient`.

- [ ] **Step 6: Commit**

```bash
git add apps/client
git commit -m "feat: obtain elevation through a logon task, and say when it is missing

schtasks with /RL HIGHEST starts the agent elevated at logon with no UAC
prompt. The manifest is deliberately left alone: requireAdministrator would
prompt on every launch, including the logon one, which is the thing being
avoided.

The precondition is absolute and is documented where somebody will read it:
highest privileges elevates the logged-in user's own token, so on a machine
whose user is not a local administrator the task runs unelevated. That case
is not silent -- the startup banner says so, the detail window shows it, and
the capability is withheld so the server can warn before dispatching rather
than after."
```

---

### Task 9: The run model

**Files:**
- Create: `apps/server/script_run.hpp`, `apps/server/script_run.cpp`
- Modify: `apps/server/CMakeLists.txt`
- Test: `apps/server/tests/test_script_run.cpp` (create)

**Interfaces:**
- Consumes: `lm::core::ScriptStatus`
- Produces: `ScriptRun`, `RunTarget`, `TargetState`, `ScriptRun::apply_result`,
  `ScriptRun::apply_deadline`, `ScriptRun::tally`

- [ ] **Step 1: Write the failing test**

Create `apps/server/tests/test_script_run.cpp`:

```cpp
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
```

- [ ] **Step 2: Run it and watch it fail**

Run: `build\windows\bin\Debug\lab_monitor_server_tests.exe --gtest_filter="ScriptRunModel.*"`
Expected: compile error — `script_run.hpp` does not exist.

- [ ] **Step 3: Write the model**

Create `apps/server/script_run.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "lm/core/script.hpp"
#include "lm/transport/messages.hpp"

/// Where one host has got to within a run.
enum class TargetState {
    Pending,     ///< chosen, nothing sent yet
    Dispatched,  ///< command published, waiting
    Completed,   ///< ran, reported success
    Failed,      ///< ran, reported failure
    Refused,     ///< the host would not, or could not, be asked
    NoResponse   ///< nothing came back before the deadline
};

[[nodiscard]] std::string to_string(TargetState state);

struct RunTarget {
    lm::core::HostId host_id;
    TargetState state = TargetState::Pending;
    /// Why, for Refused; free for the others.
    std::string detail;
    /// Present once a result arrived, so the view can show output and exit code.
    std::optional<lm::transport::ScriptResultMessage> result;
};

struct RunTally {
    std::size_t pending = 0;
    std::size_t dispatched = 0;
    std::size_t completed = 0;
    std::size_t failed = 0;
    std::size_t refused = 0;
    std::size_t no_response = 0;
};

/// One dispatch: a script, the hosts it went to, and what each of them made of
/// it.
///
/// Pure on purpose -- no Qt, no transport, no clock. apply_deadline() takes no
/// argument and simply converts every still-dispatched target, so the caller
/// owns the timing and every path including the deadline is testable without
/// waiting for one.
///
/// The grouping is for *observation*, not atomicity: half a fleet can succeed
/// while half fails, and there is no rollback. The spec says so plainly and so
/// does this, because "transaction" invites the other reading.
struct ScriptRun {
    std::string run_id;
    std::string script_name;
    std::string script_body;
    std::chrono::system_clock::time_point issued_at;
    std::uint32_t timeout_seconds = 120;
    std::vector<RunTarget> targets;

    void mark_dispatched(const lm::core::HostId& host_id);
    /// For a host that cannot be asked at all: not Online, or not enrolled.
    /// No command is sent, because the topic is Volatile and there is no queue
    /// to hold one until the machine comes back.
    void refuse_at_dispatch(const lm::core::HostId& host_id, std::string reason);
    /// Ignores a result for a host this run never targeted.
    void apply_result(const lm::transport::ScriptResultMessage& result);
    /// Every target still Dispatched becomes NoResponse. Answered ones are
    /// untouched.
    void apply_deadline();

    [[nodiscard]] RunTally tally() const;
    [[nodiscard]] bool is_finished() const;
};
```

Implement `script_run.cpp` straightforwardly: `mark_dispatched` and `refuse_at_dispatch`
find the target by host id and set its state; `apply_result` finds it and maps
`lm::core::ScriptStatus` onto `TargetState` (`Completed`→`Completed`, `Failed`→`Failed`,
`Refused`→`Refused` carrying `refusal_reason` into `detail`, `Error`→`Failed`), storing
the message in `result`; `tally` counts; `is_finished` is true when no target is `Pending`
or `Dispatched`.

Add `script_run.cpp` to the `lab_monitor_server` sources **and** to the
`lab_monitor_server_tests` sources, and `tests/test_script_run.cpp` to the test sources.

- [ ] **Step 4: Reconfigure, build, run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lab_monitor_server_tests
build\windows\bin\Debug\lab_monitor_server_tests.exe --gtest_filter="ScriptRunModel.*"
```
Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add apps/server
git commit -m "feat: the run model, as a pure state machine

A run is the transaction an operator thinks in: one script, one outcome
per targeted host, updating as results arrive. Kept free of Qt, of the
transport and of the clock -- apply_deadline() takes no argument and simply
converts every still-dispatched target -- so every path including the
deadline is testable without waiting for one.

Refused and NoResponse stay distinct all the way through the tally,
because they call for different actions: one is a configuration problem on
a machine that is working, the other may be a machine that is not."
```

---

### Task 10: Dispatch and correlation in ServerController

**Files:**
- Modify: `apps/server/server_controller.hpp`, `apps/server/server_controller.cpp`
- Test: `apps/server/tests/test_script_dispatch.cpp` (create)

**Interfaces:**
- Consumes: `ScriptRun`, `IServerTransport::publish_script_command`,
  `IServerTransport::on_script_result`
- Produces: `ServerController::start_script_run(name, body, hosts, timeout) -> QString`
  (the run id), signals `script_run_changed(QString run_id)`,
  `const std::vector<ScriptRun>& script_runs() const`

- [ ] **Step 1: Write the failing test**

Create `apps/server/tests/test_script_dispatch.cpp`:

```cpp
#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>
#include <vector>

#include "lm/transport/in_memory_transport.hpp"
#include "server_controller.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

/// A controller over an in-memory bus, with the bus reachable so a test can
/// play the part of a client: announcing hosts and publishing results.
struct Harness {
    MessageBus bus;
    QTemporaryDir dir;
    std::unique_ptr<ServerController> controller;
    std::vector<ScriptCommand> dispatched;

    Harness() {
        EXPECT_TRUE(dir.isValid());
        controller = std::make_unique<ServerController>(make_in_memory_server(bus), dir.path());
        bus.subscribe_script_command(
            [this](const ScriptCommand& command) { dispatched.push_back(command); });
        controller->start();
    }

    ~Harness() { controller->stop(); }

    /// Brings a host into the fleet as Online, with the capabilities given.
    void announce(const std::string& host, Capabilities caps) {
        const auto client = make_in_memory_client(bus);
        ClientAnnounce message;
        message.host_id = host;
        message.agent_version = "test";
        message.capabilities = caps.raw();
        client->publish_announce(message);
        controller->add_expected_host(host, "");
        QApplication::processEvents();
    }

    void publish_result(const std::string& host, const std::string& run_id, ScriptStatus status) {
        const auto client = make_in_memory_client(bus);
        ScriptResultMessage message;
        message.host_id = host;
        message.run_id = run_id;
        message.status = status;
        client->publish_script_result(message);
        QApplication::processEvents();
    }
};

Capabilities enrolled() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Scripts);
    return caps;
}

const RunTarget& target_for(const ScriptRun& run, const std::string& host) {
    const auto found = std::ranges::find(run.targets, host, &RunTarget::host_id);
    EXPECT_NE(found, run.targets.end());
    return *found;
}

}  // namespace

TEST(ScriptDispatch, SendsOneCommandPerTargetedHost) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    const QString run_id = harness.controller->start_script_run(
        "(custom script)", "exit 0", {"PC-001", "PC-002"}, 60);

    ASSERT_EQ(harness.dispatched.size(), 2u);
    EXPECT_NE(harness.dispatched[0].host_id, harness.dispatched[1].host_id)
        << "each host gets its own sample, keyed by host";
    EXPECT_EQ(harness.dispatched[0].run_id, run_id.toStdString());
    EXPECT_EQ(harness.dispatched[1].run_id, run_id.toStdString());
    EXPECT_EQ(harness.dispatched[0].script_body, "exit 0");
}

TEST(ScriptDispatch, RefusesAHostThatIsNotOnlineWithoutSendingAnything) {
    // The topic is Volatile, so there is no queue to hold a command until the
    // machine returns. Promising delivery would be a lie, so it is refused up
    // front and the operator is told immediately rather than after a deadline.
    Harness harness;
    harness.controller->add_expected_host("PC-gone", "");
    QApplication::processEvents();

    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-gone"}, 60);

    EXPECT_TRUE(harness.dispatched.empty()) << "nothing may be sent to an absent host";
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(run.run_id, run_id.toStdString());
    EXPECT_EQ(target_for(run, "PC-gone").state, TargetState::Refused);
    EXPECT_FALSE(target_for(run, "PC-gone").detail.empty());
}

TEST(ScriptDispatch, RefusesAHostWithoutTheScriptsCapability) {
    // The opt-in is the whole bound on this feature, so the server honours it
    // too rather than sending and letting the client decline -- an un-enrolled
    // machine should never receive a script body at all.
    Harness harness;
    Capabilities bare;
    bare.add(Capability::Resources);
    harness.announce("PC-001", bare);

    harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    EXPECT_TRUE(harness.dispatched.empty());
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Refused);
    EXPECT_NE(target_for(run, "PC-001").detail.find("enrol"), std::string::npos)
        << target_for(run, "PC-001").detail;
}

TEST(ScriptDispatch, CorrelatesAResultBackToItsRunAndHost) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    const QString run_id = harness.controller->start_script_run(
        "(custom script)", "exit 0", {"PC-001", "PC-002"}, 60);

    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Completed);

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-002").state, TargetState::Completed);
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Dispatched)
        << "one host answering must not move the others";
}

TEST(ScriptDispatch, IgnoresAResultForARunItDoesNotKnow) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    harness.publish_result("PC-001", "some-other-run", ScriptStatus::Completed);

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Dispatched)
        << "a stray run_id must not move this run";
    EXPECT_EQ(run.run_id, run_id.toStdString());
}

TEST(ScriptDispatch, EmitsRunChangedWhenATargetMoves) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_run_changed);
    ASSERT_TRUE(spy.isValid());

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    ASSERT_GT(spy.count(), 0) << "the view has no other way to know a result arrived";
    EXPECT_EQ(spy.front().at(0).toString(), run_id);
}
```

- [ ] **Step 2: Run it and watch it fail**

Expected: compile error — `start_script_run` is not a member.

- [ ] **Step 3: Implement dispatch**

`start_script_run` generates a run id (a timestamp plus a short random suffix), builds the
`ScriptRun`, and for each requested host:

- not `Online` in the current fleet → `refuse_at_dispatch(host, "host is not online")`
- `Online` but without `Capability::Scripts` → `refuse_at_dispatch(host, "not enrolled for
  script execution")`
- otherwise publish a `ScriptCommand` and `mark_dispatched(host)`

then logs the run at `info` with the script name and the host count, stores it, and emits
`script_run_changed`.

`on_script_result` finds the run by `run_id`, calls `apply_result`, logs the outcome at
`info`, and emits `script_run_changed`. A single-shot `QTimer` per run, set to
`timeout_seconds + 15s`, calls `apply_deadline()` — the margin covers the client's own
timeout plus transit, so a script that used its full budget is not called non-responsive
while its result is in flight.

- [ ] **Step 4: Build and run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lab_monitor_server_tests
build\windows\bin\Debug\lab_monitor_server_tests.exe --gtest_filter="ScriptDispatch.*"
```
Expected: PASS, 6 tests.

- [ ] **Step 5: Commit**

```bash
git add apps/server
git commit -m "feat: dispatch script runs and correlate the results

A host that is not Online, or not enrolled, is refused at dispatch without
a command being sent -- the topic is Volatile, so there is no queue and
promising delivery to an absent machine would be a lie.

The deadline timer allows the client's own timeout plus fifteen seconds,
so a script that used its full budget is not called non-responsive while
its result is still in flight."
```

---

### Task 11: The Scripts tab — editor, host selection, Run

**Files:**
- Create: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Modify: `apps/server/fleet_window.hpp`, `apps/server/fleet_window.cpp`
- Modify: `apps/server/CMakeLists.txt`
- Test: `apps/server/tests/test_scripts_tab.cpp` (create)

**Interfaces:**
- Consumes: `ServerController::start_script_run`, `FleetModel`
- Produces: `ScriptsTab` widget, added to `FleetWindow`'s tab bar after Templates

- [ ] **Step 1: Write the failing test**

Create `apps/server/tests/test_scripts_tab.cpp`:

```cpp
TEST(ScriptsTab, OpensWithTheStarterTemplateRatherThanAnEmptyBox) {
    // The template is the only place the LM-RESULT convention is taught:
    // documentation nobody reads versus boilerplate that gets edited.
    Harness harness;
    auto* editor = scripts_editor(harness);
    ASSERT_NE(editor, nullptr);

    const QString text = editor->toPlainText();
    EXPECT_NE(text.indexOf(QStringLiteral("LM-RESULT")), -1) << text.toStdString();
    EXPECT_NE(text.indexOf(QStringLiteral("exit 0")), -1);
    EXPECT_NE(text.indexOf(QStringLiteral("exit 1")), -1)
        << "the failure path is the half people get wrong";
}

TEST(ScriptsTab, ListsEveryFleetHostWithACheckbox) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    QListWidget* hosts = host_list(harness);
    ASSERT_NE(hosts, nullptr);
    ASSERT_EQ(hosts->count(), 2);
    for (int i = 0; i < hosts->count(); ++i) {
        EXPECT_TRUE(hosts->item(i)->flags() & Qt::ItemIsUserCheckable);
    }
}

TEST(ScriptsTab, SelectAllAndClearMoveEveryCheckbox) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    button(harness, QStringLiteral("SelectAllButton"))->click();
    EXPECT_EQ(checked_hosts(harness).size(), 2);

    button(harness, QStringLiteral("ClearButton"))->click();
    EXPECT_EQ(checked_hosts(harness).size(), 0);
}

TEST(ScriptsTab, DisablesRunUntilAHostIsSelected) {
    // Run with nothing targeted is a no-op that looks like a failure. The
    // button says so by being unavailable rather than by doing nothing.
    Harness harness;
    harness.announce("PC-001", enrolled());

    EXPECT_FALSE(button(harness, QStringLiteral("RunButton"))->isEnabled());

    check_hosts(harness, {"PC-001"});
    EXPECT_TRUE(button(harness, QStringLiteral("RunButton"))->isEnabled());
}

TEST(ScriptsTab, ResetToTemplateRestoresTheStarter) {
    Harness harness;
    QPlainTextEdit* editor = scripts_editor(harness);
    const QString original = editor->toPlainText();

    editor->setPlainText(QStringLiteral("Remove-Item C:\\ -Recurse"));
    button(harness, QStringLiteral("ResetTemplateButton"))->click();

    EXPECT_EQ(editor->toPlainText(), original);
}

TEST(ScriptsTab, StartsARunWithTheEditorsBodyAndTheCheckedHosts) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    scripts_editor(harness)->setPlainText(QStringLiteral("Write-Output 'hi'\nexit 0\n"));
    check_hosts(harness, {"PC-002"});

    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();

    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(run.script_body, "Write-Output 'hi'\nexit 0\n");
    ASSERT_EQ(run.targets.size(), 1u) << "only the checked host is targeted";
    EXPECT_EQ(run.targets.front().host_id, "PC-002");
}
```

`Harness` here is the one from Task 10 with a `FleetWindow` added, plus these helpers at
the top of the file: `scripts_editor(harness)` finds the `QPlainTextEdit` named
`ScriptEditor`; `host_list(harness)` finds the `QListWidget` named `HostList`;
`button(harness, name)` finds a `QPushButton` by object name; `checked_hosts(harness)`
returns the host ids whose items are checked; `check_hosts(harness, {...})` ticks the
named rows. `enrolled()` is Task 10's capability helper.

- [ ] **Step 2: Run it and watch it fail**

Expected: compile error — `scripts_tab.hpp` does not exist.

- [ ] **Step 3: Build the tab**

`ScriptsTab` is a `QWidget` with, left to right: the script area (a `QPlainTextEdit` in a
fixed-width font, seeded with the starter template from the spec, plus **Reset to
template**), the host list (a `QListWidget` of checkable items with **Select all** and
**Clear**), and a Run row showing `Run on N hosts`.

Every control carries an `objectName` so the tests above can find it — `ScriptEditor`,
`HostList`, `SelectAllButton`, `ClearButton`, `RunButton`, `TargetCountLabel`.

The starter template is a single `constexpr const char*` in `scripts_tab.cpp`, copied
verbatim from §8 of the spec.

Hosts that cannot comply are listed with a suffix saying why — `PC-003 — not enrolled` —
and start unchecked.

Add the tab to `FleetWindow` after Templates and before Log.

- [ ] **Step 4: Build and run**

```
"C:\Program Files\...\cmake.exe" --preset windows
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lab_monitor_server_tests
build\windows\bin\Debug\lab_monitor_server_tests.exe --gtest_filter="ScriptsTab.*"
```
Expected: PASS, 8 tests.

- [ ] **Step 5: Look at it on screen**

Add a temporary test that grabs the window with
`QApplication::primaryScreen()->grabWindow(harness.window->winId())` and saves a PNG, open
it, and confirm the tab reads as intended. `QWidget::render()` cannot see a stale backing
store, so this is the only way to catch a repaint artefact. Delete the temporary test
afterwards.

- [ ] **Step 6: Commit**

```bash
git add apps/server
git commit -m "feat: the Scripts tab -- editor, host selection and Run

The editor opens on a working template rather than an empty box. It runs
as-is and does nothing, so pressing Run on an untouched template is safe
and demonstrates the whole path, and it shows both the success and the
failure branch because the failure one is what people get wrong. This is
the only place the LM-RESULT convention is taught: boilerplate that
already does it correctly gets edited, whereas documentation describing it
would be read by nobody.

Hosts that cannot comply are listed, explained and unchecked rather than
hidden -- an operator should see that a machine is not enrolled, not
wonder where it went. The target count sits beside Run, because the blast
radius must be readable without counting checkboxes."
```

---

### Task 12: The live run view

**Files:**
- Modify: `apps/server/scripts_tab.hpp`, `apps/server/scripts_tab.cpp`
- Test: `apps/server/tests/test_scripts_tab.cpp`

**Interfaces:**
- Consumes: `ServerController::script_runs()`, `script_run_changed`
- Produces: nothing further

- [ ] **Step 1: Write the failing test**

Append to `apps/server/tests/test_scripts_tab.cpp`:

```cpp
namespace {

QTableWidget* run_targets(const Harness& harness) {
    return harness.window->findChild<QTableWidget*>(QStringLiteral("RunTargets"));
}

QStringList outcome_column(const Harness& harness) {
    QStringList outcomes;
    QTableWidget* table = run_targets(harness);
    for (int row = 0; row < table->rowCount(); ++row) {
        outcomes << table->item(row, 1)->text();
    }
    return outcomes;
}

}  // namespace

TEST(ScriptRunView, ShowsEveryTargetAsPendingTheMomentRunIsPressed) {
    // Somebody who just dispatched to ninety machines needs to see that it
    // started -- not a blank pane until the first result lands.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});

    press_run(harness);

    ASSERT_EQ(run_targets(harness)->rowCount(), 2);
    for (const QString& outcome : outcome_column(harness)) {
        EXPECT_EQ(outcome.toStdString(), "Dispatched");
    }
}

TEST(ScriptRunView, MovesATargetInPlaceWhenItsResultArrives) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    const QStringList outcomes = outcome_column(harness);
    ASSERT_EQ(outcomes.size(), 2);
    EXPECT_TRUE(outcomes.contains(QStringLiteral("Completed")));
    EXPECT_TRUE(outcomes.contains(QStringLiteral("Dispatched")))
        << "the host that has not answered must not move";
}

TEST(ScriptRunView, SummarisesTheRunWithACountPerOutcome) {
    // On a large run the tally is what is read; the rows are what is drilled
    // into afterwards.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Failed);

    auto* summary = harness.window->findChild<QLabel*>(QStringLiteral("RunSummary"));
    ASSERT_NE(summary, nullptr);
    const std::string text = summary->text().toStdString();
    EXPECT_NE(text.find("1 completed"), std::string::npos) << text;
    EXPECT_NE(text.find("1 failed"), std::string::npos) << text;
}

TEST(ScriptRunView, ShowsTheOutputOfTheSelectedTarget) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    check_hosts(harness, {"PC-001"});
    const QString run_id = press_run(harness);

    ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = run_id.toStdString();
    result.status = ScriptStatus::Completed;
    result.stdout_text = "cleaned 3 files";
    harness.publish_result_message(result);

    run_targets(harness)->selectRow(0);
    QApplication::processEvents();

    auto* output = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("RunOutput"));
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output->toPlainText().indexOf(QStringLiteral("cleaned 3 files")), -1)
        << output->toPlainText().toStdString();
}

TEST(ScriptRunView, ShowsARefusalsReasonRatherThanItsOutput) {
    // A refused host has no output; the reason is the whole of what happened,
    // and an empty pane would read as "ran and printed nothing".
    Harness harness;
    Capabilities bare;
    bare.add(Capability::Resources);
    harness.announce("PC-001", bare);
    check_hosts(harness, {"PC-001"});
    press_run(harness);

    run_targets(harness)->selectRow(0);
    QApplication::processEvents();

    auto* output = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("RunOutput"));
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output->toPlainText().indexOf(QStringLiteral("enrol")), -1)
        << output->toPlainText().toStdString();
}
```

`check_hosts(harness, {...})` ticks the named rows in the host list; `press_run(harness)`
clicks the Run button and returns the run id from
`harness.controller->script_runs().back().run_id`; `publish_result_message` is the
`Harness` helper from Task 10 taking a full message. Add all three to the shared harness
at the top of `test_scripts_tab.cpp`.

- [ ] **Step 2: Run it and watch it fail**

Expected: FAIL — the run view does not exist.

- [ ] **Step 3: Build the view**

A `QTableWidget` (`objectName` `RunTargets`) with Host, Outcome and Detail columns, a
summary label (`RunSummary`) reading `12 completed · 2 failed · 1 no response`, and an
output pane (`RunOutput`, read-only, fixed-width) for the selected target.

Colour each outcome with `Theme::color_for(...)`'s existing palette — `Completed` green,
`Failed` red, `Refused` amber, `NoResponse` the muted grey — so the run view and the fleet
table do not disagree about what a colour means.

Rebuild the table on `script_run_changed` for the displayed run only. Refused targets show
their reason in Detail and in the output pane, since they have no output.

- [ ] **Step 4: Build and run**

```
"C:\Program Files\...\cmake.exe" --build --preset windows-debug --target lab_monitor_server_tests
build\windows\bin\Debug\lab_monitor_server_tests.exe --gtest_filter="ScriptRunView.*"
```
Expected: PASS, 5 tests.

- [ ] **Step 5: Run the whole suite**

```
"C:\Program Files\...\cmake.exe" --build --preset windows-debug
"C:\Program Files\...\ctest.exe" --preset windows-debug
```
Expected: 8/8 binaries pass.

- [ ] **Step 6: Add the end-to-end integration case**

Spec §10 asks for one case running the whole stack. Append to the integration-gated
`libs/transport/tests/test_dds_loopback.cpp` — or a new `apps/client` integration target
if that is where the worker is reachable from:

```cpp
TEST(FastDdsScripts, ARealCommandRunsARealScriptAndTheResultComesBack) {
    // Everything except the two GUIs: a real DDS domain, the real worker, the
    // real PowerShell runner. Every other test in this feature substitutes one
    // of those, so this is the only one that would notice them disagreeing.
    DdsConfig config;
    config.domain_id = 72;
    const auto server = make_dds_server(config);

    std::vector<ScriptResultMessage> results;
    server->on_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    lm::platform::ProbeSet probes;
    probes.resources = std::make_unique<lm::platform::FakeResourceProbe>();
    auto host_probes = std::make_unique<lm::platform::HostProbes>(
        "PC-integration", std::move(probes), lm::core::platform_capabilities());

    auto* worker = new MonitorWorker(std::move(host_probes), make_dds_client(config),
                                     lm::platform::make_script_runner(),
                                     /*allow_scripts=*/true);
    auto* thread = new QThread();
    worker->moveToThread(thread);
    thread->start();
    QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);

    ScriptCommand command;
    command.host_id = "PC-integration";
    command.run_id = "run-e2e";
    command.script_name = "(custom script)";
    command.script_body = "Write-Output 'ran end to end'
exit 0
";
    command.timeout_seconds = 30;

    // Published in a loop: discovery races a single publish.
    for (int i = 0; i < 60 && results.empty(); ++i) {
        server->publish_script_command(command);
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    thread->quit();
    thread->wait();
    delete worker;
    delete thread;

    ASSERT_FALSE(results.empty()) << "no result came back";
    EXPECT_EQ(results.front().status, lm::core::ScriptStatus::Completed);
    EXPECT_NE(results.front().stdout_text.find("ran end to end"), std::string::npos)
        << results.front().stdout_text;
}
```

Build with `-DLM_BUILD_INTEGRATION_TESTS=ON`, run it, then set it back to `OFF` and
re-run the full suite.

- [ ] **Step 7: Verify the whole loop by hand**

Start the server, and a client with `--allow-scripts`. In the Scripts tab, press Run on
the untouched template and confirm the target moves Pending → Completed with the
template's `completed` message. Then run `exit 1` and confirm it reads Failed. Then stop
the client and run again, confirming the host is Refused as not online.

- [ ] **Step 8: Commit**

```bash
git add apps/server
git commit -m "feat: watch a run complete host by host

The run appears with every target Pending the moment Run is pressed:
somebody who just dispatched to ninety machines needs to see that it
started, not a blank pane until the first result lands.

The tally sits above the rows because on a large run the counts are what
is read and the rows are what is drilled into. Outcomes take their colours
from Theme, so the run view and the fleet table cannot disagree about what
red means. A refused host shows its reason where output would be, having
none."
```

---

## Deferred to phase 2

Per §11 of the spec, and deliberately not in this plan: the share folder tree, script
preview, the list becoming the default view with the editor behind a **Custom script…**
button, and persisted run history with operator-driven cleanup. Nothing above needs
revisiting to add them — the dispatch path, the wire format and the run model are
unchanged by where a body came from or by whether a run is written to disk.
