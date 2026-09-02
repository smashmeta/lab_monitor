#include "script_run.hpp"

#include <algorithm>
#include <utility>

std::string to_string(TargetState state) {
    switch (state) {
        case TargetState::Pending:     return "Pending";
        case TargetState::Dispatched:  return "Dispatched";
        case TargetState::Completed:   return "Completed";
        case TargetState::Failed:      return "Failed";
        case TargetState::Refused:     return "Refused";
        case TargetState::NoResponse:  return "NoResponse";
    }
    return "Unknown";
}

namespace {

/// Every site that needs a target by host id goes through this, rather than
/// each caller writing its own find_if -- that is what keeps
/// "unknown host id" a single, deliberate no-op instead of three chances to
/// get it wrong.
RunTarget* find_target(std::vector<RunTarget>& targets, const lm::core::HostId& host_id) {
    const auto it = std::find_if(targets.begin(), targets.end(),
                                  [&](const RunTarget& target) { return target.host_id == host_id; });
    return it == targets.end() ? nullptr : &*it;
}

/// lm::core::ScriptStatus has no NoResponse or Refused-by-us-not-the-host
/// counterpart to Pending/Dispatched -- it only ever describes a run that
/// actually happened on the client, which is why Error maps to Failed here:
/// as far as an operator is concerned the run reached the host and something
/// went wrong, and the detail on the target says what.
TargetState state_for(lm::core::ScriptStatus status) {
    switch (status) {
        case lm::core::ScriptStatus::Completed: return TargetState::Completed;
        case lm::core::ScriptStatus::Failed:    return TargetState::Failed;
        case lm::core::ScriptStatus::Refused:   return TargetState::Refused;
        case lm::core::ScriptStatus::Error:     return TargetState::Failed;
    }
    return TargetState::Failed;
}

}  // namespace

void ScriptRun::mark_dispatched(const lm::core::HostId& host_id) {
    if (RunTarget* target = find_target(targets, host_id)) {
        target->state = TargetState::Dispatched;
    }
}

void ScriptRun::refuse_at_dispatch(const lm::core::HostId& host_id, std::string reason) {
    if (RunTarget* target = find_target(targets, host_id)) {
        target->state = TargetState::Refused;
        target->detail = std::move(reason);
    }
}

void ScriptRun::apply_result(const lm::transport::ScriptResultMessage& result) {
    RunTarget* target = find_target(targets, result.host_id);
    if (target == nullptr) {
        // A result for a host this run never targeted: stray traffic, or a
        // result meant for a different run entirely. Not this target's to take.
        return;
    }

    target->state = state_for(result.status);
    if (result.status == lm::core::ScriptStatus::Refused) {
        target->detail = result.refusal_reason;
    }
    target->result = result;
}

void ScriptRun::apply_deadline() {
    for (RunTarget& target : targets) {
        if (target.state == TargetState::Dispatched) {
            target.state = TargetState::NoResponse;
        }
    }
}

RunTally ScriptRun::tally() const {
    RunTally tally;
    for (const RunTarget& target : targets) {
        switch (target.state) {
            case TargetState::Pending:     ++tally.pending; break;
            case TargetState::Dispatched:  ++tally.dispatched; break;
            case TargetState::Completed:   ++tally.completed; break;
            case TargetState::Failed:      ++tally.failed; break;
            case TargetState::Refused:     ++tally.refused; break;
            case TargetState::NoResponse:  ++tally.no_response; break;
        }
    }
    return tally;
}

bool ScriptRun::is_finished() const {
    return std::none_of(targets.begin(), targets.end(), [](const RunTarget& target) {
        return target.state == TargetState::Pending || target.state == TargetState::Dispatched;
    });
}
