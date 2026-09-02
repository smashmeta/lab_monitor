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
    // Ahead of the exit code, deliberately: a killed run carries a code the
    // runner picked, and a run that never started carries none at all, so
    // either would otherwise be reported as a verdict the script reached.
    if (!started || timed_out) {
        return ScriptStatus::Error;
    }
    if (exit_code != 0) {
        return ScriptStatus::Failed;
    }
    if (reported.has_value() && !reported->ok) {
        return ScriptStatus::Failed;
    }
    return ScriptStatus::Completed;
}

}  // namespace lm::core
