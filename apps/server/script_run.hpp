#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

/// The inverse of to_string(TargetState). nullopt for anything else, so a
/// hand-edited or future file is rejected rather than silently read as Pending.
[[nodiscard]] std::optional<TargetState> target_state_from_string(std::string_view text);

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
