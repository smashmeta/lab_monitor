#include "autostart.hpp"

// NOMINMAX before windows.h, matching libs/platform/src/windows: this file
// must not depend on the build already defining it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// Folder-scoped rather than a bare name: an unqualified "LabMonitorClient"
/// sits at the task-store root, where /Create /F would silently replace any
/// third-party task that happened to have chosen the same name, and /Delete
/// would remove whatever holds that name without checking it points at this
/// executable. schtasks creates the folder on demand.
constexpr wchar_t kTaskPath[] = L"\\LabMonitor\\Client";

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

/// Names the exact handles the child may inherit, rather than letting
/// bInheritHandles hand it every inheritable handle already open in this
/// process. Copied from script_runner_windows.cpp's InheritList of the same
/// name: that file solved this exact problem for the same reason (a
/// CreateProcessW call that needs to redirect a pipe but not become a
/// dumping ground for whatever else this process happens to have open), and
/// there is nothing about schtasks.exe that calls for a different answer.
class InheritList {
public:
    InheritList() = default;
    ~InheritList() {
        if (list_ != nullptr) {
            DeleteProcThreadAttributeList(list_);
        }
    }

    InheritList(const InheritList&) = delete;
    InheritList& operator=(const InheritList&) = delete;

    bool build(HANDLE* handles, std::size_t count) {
        SIZE_T size = 0;
        // Documented to fail while reporting the size it needs.
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0) {
            return false;
        }
        buffer_ = std::make_unique<std::byte[]>(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer_.get());
        if (InitializeProcThreadAttributeList(list, 1, 0, &size) == 0) {
            return false;
        }
        list_ = list;
        return UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                         count * sizeof(HANDLE), nullptr, nullptr) != 0;
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

private:
    std::unique_ptr<std::byte[]> buffer_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
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

/// For log lines only -- everything that actually runs the command stays in
/// wide strings throughout. Windows paths are practically always ASCII, so
/// this is not on any correctness-critical path; it exists purely so
/// spdlog (narrow-string only) can say what this file just did.
[[nodiscard]] std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                            nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string narrowed(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrowed.data(),
                        needed, nullptr, nullptr);
    return narrowed;
}

/// The inverse of narrow(), for the option strings main() parsed out of a
/// narrow argv and this file has to put back on a wide command line.
[[nodiscard]] std::wstring widen(const std::string& narrowed) {
    if (narrowed.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, narrowed.data(),
                                           static_cast<int>(narrowed.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrowed.data(), static_cast<int>(narrowed.size()), wide.data(),
                        needed);
    return wide;
}

/// The command line the registered task will run: this executable's own path
/// followed by the arguments it should be started with.
///
/// Returned in its plain form -- ordinary quotes -- because this is both what
/// schtasks will store as the task action and what gets logged. Escaping it
/// for schtasks's own command line is escaped_for_schtasks()'s job, kept
/// separate so the two are never confused for one another.
///
/// An argument containing a space (a --config path under Program Files, say)
/// is quoted; one without is left bare, so the registered line reads the way
/// the operator typed it.
[[nodiscard]] std::wstring task_command_line(const std::wstring& exe,
                                             const std::vector<std::string>& arguments) {
    std::wstring line = L"\"" + exe + L"\"";
    for (const std::string& argument : arguments) {
        const std::wstring wide = widen(argument);
        line += L' ';
        if (wide.find(L' ') == std::wstring::npos) {
            line += wide;
        } else {
            line += L"\"" + wide + L"\"";
        }
    }
    return line;
}

/// The /TR value sits inside a quoted argument on schtasks.exe's own command
/// line, so every quote that has to reach the stored task action must arrive
/// there as \" -- which is how the original single-argument form was written
/// by hand. Doing it here means task_command_line() never has to think about
/// it.
[[nodiscard]] std::wstring escaped_for_schtasks(const std::wstring& line) {
    std::wstring escaped;
    escaped.reserve(line.size() + 8);
    for (const wchar_t character : line) {
        if (character == L'"') {
            escaped += L'\\';
        }
        escaped += character;
    }
    return escaped;
}

struct ProcessResult {
    bool started = false;
    /// Set only when started is false: what GetLastError() said about
    /// whichever step failed (CreatePipe, SetHandleInformation, the handle
    /// allowlist, or CreateProcessW itself), so "could not start schtasks.exe"
    /// is not the whole story.
    DWORD start_error = 0;
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
        result.start_error = GetLastError();
        return result;
    }
    // Only the child's end is inheritable; ours must not be, or our own copy
    // keeps the pipe open after the child exits and the read below never sees
    // EOF. The return value is checked, not fired-and-forgotten: if this call
    // failed, the child would inherit our read end too, and the drain thread
    // below would never see EOF -- drain.join() would hang forever rather
    // than this function simply failing.
    if (SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0) == 0) {
        result.start_error = GetLastError();
        return result;
    }

    // NUL as stdin, matching script_runner_windows.cpp: with
    // STARTF_USESTDHANDLES set, an unset hStdInput hands the child an invalid
    // handle rather than nothing in particular. Harmless for `schtasks /F`,
    // which never reads stdin, but there is no reason to leave it unset when
    // the fix already exists in this codebase.
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                           OPEN_EXISTING, 0, nullptr));

    // Names exactly the two handles schtasks.exe is meant to receive --
    // its stdout/stderr pipe and NUL for stdin -- rather than letting
    // bInheritHandles=TRUE hand it every inheritable handle open in this
    // process, which today is only QApplication's but is unbounded by
    // construction rather than by design.
    std::array<HANDLE, 2> inherited{write_end.get(), nul.get()};
    const std::size_t inherited_count = nul ? inherited.size() : inherited.size() - 1;
    InheritList inherit_list;
    if (!inherit_list.build(inherited.data(), inherited_count)) {
        result.start_error = GetLastError();
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul.get();
    startup.StartupInfo.hStdOutput = write_end.get();
    startup.StartupInfo.hStdError = write_end.get();
    startup.lpAttributeList = inherit_list.get();

    PROCESS_INFORMATION process_info{};
    // lpApplicationName left null: command_line's first token is already the
    // full quoted path built by the caller, so there is nothing here for a
    // hijack to redirect.
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr,
                                        /*bInheritHandles=*/TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &startup.StartupInfo, &process_info);
    if (created == 0) {
        result.start_error = GetLastError();
        // read_end, write_end, nul and inherit_list all clean up via their
        // own destructors on this return -- nothing further to release here.
        return result;
    }

    // Our copies of the child's ends go now: while either stays open, the
    // drain thread below never sees the pipe close, however long ago
    // schtasks exited.
    write_end.reset();
    nul.reset();

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
        return "could not start schtasks.exe (Windows error " +
              std::to_string(result.start_error) + ")";
    }
    const std::string output = trimmed(result.output);
    return "schtasks failed (exit " + std::to_string(result.exit_code) +
          "), most likely because this was not run from an administrator prompt" +
          (output.empty() ? std::string() : ": " + output);
}

}  // namespace

std::string install_autostart_task(const std::vector<std::string>& arguments) {
    const std::wstring schtasks = schtasks_path();
    if (schtasks.empty()) {
        const std::string message = "schtasks.exe is not present under System32";
        spdlog::error("install-autostart: {}", message);
        return message;
    }
    const std::wstring exe = this_executable_path();
    if (exe.empty()) {
        const std::string message = "could not determine this executable's own path";
        spdlog::error("install-autostart: {}", message);
        return message;
    }

    // Built from the options this invocation was given, never hard-coded --
    // see autostart.hpp for why elevation must not smuggle in enrolment, and
    // why dropping --domain-id makes a machine vanish from the fleet.
    const std::wstring task_line = task_command_line(exe, arguments);

    // Logged before the attempt, not just the outcome: the exact command
    // being registered is the one thing an operator cannot see any other way,
    // and it is what will run elevated at every logon from here on.
    spdlog::info("install-autostart: registering \"{}\" to run {} at logon, elevated",
                 narrow(kTaskPath), narrow(task_line));

    // /RL HIGHEST is what starts this task elevated at logon with no UAC
    // prompt -- see autostart.hpp. Registering a task at that run level is
    // itself an elevated operation, so a non-elevated caller fails here with
    // schtasks's own "Access is denied" rather than silently registering an
    // unelevated task.
    const std::wstring command = L"\"" + schtasks + L"\" /Create /F /TN \"" + kTaskPath +
                                 L"\" /TR \"" + escaped_for_schtasks(task_line) +
                                 L"\" /SC ONLOGON /RL HIGHEST";

    const ProcessResult result = run_capturing_output(command);
    if (!result.started || result.exit_code != 0) {
        const std::string message = failure_message(result);
        spdlog::error("install-autostart: {}", message);
        return message;
    }
    spdlog::info("install-autostart: succeeded");
    return {};
}

std::string uninstall_autostart_task() {
    const std::wstring schtasks = schtasks_path();
    if (schtasks.empty()) {
        const std::string message = "schtasks.exe is not present under System32";
        spdlog::error("uninstall-autostart: {}", message);
        return message;
    }

    spdlog::info("uninstall-autostart: removing \"{}\"", narrow(kTaskPath));

    const std::wstring command = L"\"" + schtasks + L"\" /Delete /F /TN \"" + kTaskPath + L"\"";

    const ProcessResult result = run_capturing_output(command);
    if (!result.started || result.exit_code != 0) {
        const std::string message = failure_message(result);
        spdlog::error("uninstall-autostart: {}", message);
        return message;
    }
    spdlog::info("uninstall-autostart: succeeded");
    return {};
}
