#include "autostart.hpp"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// The Scheduled Task mechanism (schtasks.exe, /RL HIGHEST) is Windows-only --
// see autostart.hpp and autostart_windows.cpp for the real implementation and
// why it exists at all. This build has never had a Linux script runner
// either (see CLAUDE.md, Known gaps), so there is nothing for elevation to
// unlock here yet; a stub keeps the client linking rather than the
// non-Windows leg carrying its own copy of --install-autostart handling that
// nobody has exercised.

std::string install_autostart_task(const std::vector<std::string>& /*arguments*/) {
    const std::string message = "autostart is not supported on this platform";
    spdlog::error("install-autostart: {}", message);
    return message;
}

std::string uninstall_autostart_task() {
    const std::string message = "autostart is not supported on this platform";
    spdlog::error("uninstall-autostart: {}", message);
    return message;
}
