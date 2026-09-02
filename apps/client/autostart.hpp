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
