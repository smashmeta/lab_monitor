#include "autostart.hpp"

// NOMINMAX before windows.h, matching libs/platform/src/windows: this file
// must not depend on the build already defining it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr wchar_t kTaskName[] = L"LabMonitorClient";

/// Closes exactly once, on every path out -- mirrors
/// libs/platform/src/windows/script_runner_windows.cpp's Handle, which this
/// file otherwise has no dependency on (it is app-level, schtasks.exe is not
/// a script the fleet's script runner would ever be asked to execute).
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] HANDLE* out() { return &handle_; }
    [[nodiscard]] explicit operator bool() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

/// The absolute path under System32, never a bare name. Passed as the first
/// (quoted) token of the command line with no lpApplicationName, the same
/// convention script_runner_windows.cpp's powershell_path() uses: a bare
/// "schtasks" would be searched for in this process's own directory and the
/// current directory before PATH, which is exactly the lookup a hijack needs.
[[nodiscard]] std::wstring schtasks_path() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT written = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    std::wstring path(buffer.data(), written);
    path += L"\\schtasks.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

/// This process's own full path, for the /TR command the scheduled task runs
/// at logon.
[[nodiscard]] std::wstring this_executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

struct ProcessResult {
    bool started = false;
    DWORD exit_code = 0;
    /// stdout and stderr combined -- schtasks does not put anything on one
    /// that is not worth showing alongside the other, and a single pipe means
    /// a single drain thread rather than two.
    std::string output;
};

/// Runs one already-fully-quoted command line to completion.
///
/// A dedicated drain thread is what keeps this correct rather than merely
/// working today: schtasks' own output is a handful of lines, but a pipe
/// fills at a few KB, and a process writing to a full pipe with nobody
/// reading it blocks forever -- which would hang the caller rather than fail
/// it.
[[nodiscard]] ProcessResult run_capturing_output(std::wstring command_line) {
    ProcessResult result;

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    Handle read_end;
    Handle write_end;
    if (CreatePipe(read_end.out(), write_end.out(), &inheritable, 0) == 0) {
        return result;
    }
    // Only the child's end is inheritable; ours must not be, or our own copy
    // keeps the pipe open after the child exits and the read below never sees
    // EOF.
    SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end.get();
    startup.hStdError = write_end.get();
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION process_info{};
    // lpApplicationName left null: command_line's first token is already the
    // full quoted path built by the caller, so there is nothing here for a
    // hijack to redirect.
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr,
                                        /*bInheritHandles=*/TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &process_info);
    write_end.reset();  // our copy, whether or not the create succeeded
    if (created == 0) {
        return result;
    }

    const Handle process(process_info.hProcess);
    const Handle main_thread(process_info.hThread);

    std::thread drain([&] {
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD read = 0;
            if (ReadFile(read_end.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                        nullptr) == 0 ||
                read == 0) {
                return;  // the child closed its end: it has exited
            }
            result.output.append(buffer.data(), read);
        }
    });

    WaitForSingleObject(process.get(), INFINITE);
    result.started = true;
    DWORD code = 0;
    result.exit_code = GetExitCodeProcess(process.get(), &code) != 0 ? code : 1;

    // Nothing else holds the write end once the child has exited, so the
    // drain thread has already seen EOF or is about to; this simply waits
    // for that rather than racing it.
    drain.join();
    return result;
}

[[nodiscard]] std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// The exit code alone ("schtasks failed (exit 5)") is not something an
/// operator can act on; schtasks's own text usually says "Access is denied",
/// which is the actual reason -- almost always a non-elevated caller trying
/// to register a /RL HIGHEST task.
[[nodiscard]] std::string failure_message(const ProcessResult& result) {
    if (!result.started) {
        return "could not start schtasks.exe";
    }
    const std::string output = trimmed(result.output);
    return "schtasks failed (exit " + std::to_string(result.exit_code) +
          "), most likely because this was not run from an administrator prompt" +
          (output.empty() ? std::string() : ": " + output);
}

}  // namespace

std::string install_autostart_task() {
    const std::wstring schtasks = schtasks_path();
    if (schtasks.empty()) {
        return "schtasks.exe is not present under System32";
    }
    const std::wstring exe = this_executable_path();
    if (exe.empty()) {
        return "could not determine this executable's own path";
    }

    // /RL HIGHEST is what starts this task elevated at logon with no UAC
    // prompt -- see autostart.hpp. Registering a task at that run level is
    // itself an elevated operation, so a non-elevated caller fails here with
    // schtasks's own "Access is denied" rather than silently registering an
    // unelevated task.
    const std::wstring command = L"\"" + schtasks + L"\" /Create /F /TN \"" + kTaskName +
                                 L"\" /TR \"\\\"" + exe +
                                 L"\\\" --allow-scripts\" /SC ONLOGON /RL HIGHEST";

    const ProcessResult result = run_capturing_output(command);
    if (!result.started || result.exit_code != 0) {
        return failure_message(result);
    }
    return {};
}

std::string uninstall_autostart_task() {
    const std::wstring schtasks = schtasks_path();
    if (schtasks.empty()) {
        return "schtasks.exe is not present under System32";
    }

    const std::wstring command = L"\"" + schtasks + L"\" /Delete /F /TN \"" + kTaskName + L"\"";

    const ProcessResult result = run_capturing_output(command);
    if (!result.started || result.exit_code != 0) {
        return failure_message(result);
    }
    return {};
}
