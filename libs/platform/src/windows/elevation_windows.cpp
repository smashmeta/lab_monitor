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
