#pragma once

#include <string>
#include <vector>

/// Registers a logon task that starts this executable with highest privileges.
///
/// This is how the agent obtains elevation without a UAC prompt. The manifest
/// is deliberately *not* requireAdministrator, which would prompt on every
/// launch including this one.
///
/// `arguments` is the command line the registered task will pass to this
/// executable, built by main() from the options this invocation was given.
/// That mapping is deliberate and load-bearing in two directions:
///
///  - The task must join the same DDS domain, read the same config and log at
///    the same level as the command that installed it. A task line carrying
///    only the exe path silently joins domain 0, and since the client starts
///    hidden the only symptom is a machine that never appears in the fleet.
///  - Elevation is not enrolment. `--allow-scripts` is what enrols a machine
///    for remote script execution, and per the design spec (§2) that is a
///    deliberate, per-machine act. It is carried into the task only when the
///    operator actually passed it, so installing elevation does not also,
///    permanently, turn on remote code execution.
///
/// Precondition, and it is absolute: "highest privileges" elevates the logged-in
/// user's own token. On a machine whose user is not a local administrator there
/// is no administrator token to elevate to, and the task runs unelevated.
///
/// Returns an empty string on success, or a message describing what went wrong.
[[nodiscard]] std::string install_autostart_task(const std::vector<std::string>& arguments);
[[nodiscard]] std::string uninstall_autostart_task();
