#include <windows.h>
// psapi.h and tlhelp32.h must follow windows.h.
#include <psapi.h>
#include <tlhelp32.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lm/platform/probes.hpp"

#pragma comment(lib, "version.lib")

namespace lm::platform {
namespace {

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return std::string{};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return std::string{};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed - 1, nullptr, nullptr);
    return out;
}

/// Full path of a running process, or empty when it cannot be opened. Protected
/// and elevated processes routinely refuse, which is expected rather than an error.
std::wstring executable_path(DWORD pid) {
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return std::wstring{};
    }

    wchar_t buffer[MAX_PATH] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);

    return ok ? std::wstring(buffer, size) : std::wstring{};
}

/// Reads the FileVersion from a binary's version resource. Returns nullopt when
/// the file has no version resource or cannot be read — never a fabricated zero,
/// because evaluate() reports an unreadable version as Error, which is honest,
/// whereas 0.0.0.0 would silently fail a >= constraint.
std::optional<core::Version> file_version(const std::wstring& path) {
    if (path.empty()) {
        return std::nullopt;
    }

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return std::nullopt;
    }

    std::vector<BYTE> block(size);
    if (GetFileVersionInfoW(path.c_str(), 0, size, block.data()) == 0) {
        return std::nullopt;
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) == 0 ||
        info == nullptr || info_size == 0 || info->dwSignature != 0xFEEF04BDu) {
        return std::nullopt;
    }

    core::Version version;
    version.parts = {static_cast<int>(HIWORD(info->dwFileVersionMS)),
                     static_cast<int>(LOWORD(info->dwFileVersionMS)),
                     static_cast<int>(HIWORD(info->dwFileVersionLS)),
                     static_cast<int>(LOWORD(info->dwFileVersionLS))};
    return version;
}

class WindowsProcessProbe : public IProcessProbe {
public:
    std::vector<core::ProcessInfo> enumerate() override {
        std::vector<core::ProcessInfo> processes;

        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return processes;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Process32FirstW(snapshot, &entry) == 0) {
            CloseHandle(snapshot);
            return processes;
        }

        do {
            core::ProcessInfo info;
            // szExeFile is already the bare file name, which is what rules match.
            info.executable = narrow(entry.szExeFile);
            if (info.executable.empty()) {
                continue;
            }
            info.version = file_version(executable_path(entry.th32ProcessID));
            processes.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry) != 0);

        CloseHandle(snapshot);
        return processes;
    }
};

}  // namespace

std::unique_ptr<IProcessProbe> make_process_probe() {
    return std::make_unique<WindowsProcessProbe>();
}

}  // namespace lm::platform
