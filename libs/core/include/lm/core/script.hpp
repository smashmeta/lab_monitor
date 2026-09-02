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

    /// Cleared when the shell never launched at all -- no temporary script,
    /// no pipes, no process. There is then no exit code to judge and nothing
    /// the script itself could have said, which is why status() has to know.
    bool started = true;

    /// Error when the run never happened as a run: the shell would not start,
    /// or it was killed for overrunning its timeout. Both are read ahead of the
    /// exit code, because the code in those cases was chosen by the runner
    /// rather than reached by the script -- reporting it as Failed would state
    /// a verdict nothing ever gave.
    ///
    /// Otherwise Completed only when the process exited 0 *and* did not report
    /// a failure.
    ///
    /// The asymmetry is deliberate. A reported failure overrides a zero exit
    /// code, because PowerShell readily exits 0 after catching its own error.
    /// A reported success does not override a non-zero exit code, because that
    /// is the process itself saying it failed -- which is also exactly what a
    /// crash occurring after the marker was written would look like.
    [[nodiscard]] ScriptStatus status() const;
};

}  // namespace lm::core
