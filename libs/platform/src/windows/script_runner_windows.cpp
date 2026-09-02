// NOMINMAX before windows.h: its min/max macros are function-like and swallow
// even a qualified std::min(a, b) at the call below. Guarded because the
// build already defines it -- and this file must not depend on that.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "lm/core/script.hpp"
#include "lm/platform/probes.hpp"

namespace lm::platform {
namespace {

/// 64 KB per stream. A script that prints a megabyte is one whose author will
/// be glad the fleet did not try to carry it -- and the cap is enforced here
/// rather than at the wire so the memory is never allocated either.
constexpr std::size_t kMaxStreamBytes = 64u * 1024u;

void append_capped(std::string& target, const char* data, std::size_t length) {
    if (target.size() >= kMaxStreamBytes) {
        return;
    }
    const std::size_t room = kMaxStreamBytes - target.size();
    target.append(data, std::min(room, length));
    if (target.size() >= kMaxStreamBytes) {
        // Visible, not silent: an operator reading a truncated log has to be
        // able to tell it from a script that simply stopped talking.
        target += "\n[output truncated at 64 KB]";
    }
}

/// Closes exactly once, on every path out of run() -- and there are several
/// early returns above the point where the process even exists.
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

/// The scope guard the temporary script needs. The body has to reach disk
/// because powershell.exe -File takes a path, and every route out of run() --
/// the failure returns above the launch included -- has to take the file with
/// it, or a machine accumulates one .ps1 per script it was ever sent.
class TempScriptFile {
public:
    explicit TempScriptFile(std::wstring path) : path_(std::move(path)) {}
    ~TempScriptFile() {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

    TempScriptFile(const TempScriptFile&) = delete;
    TempScriptFile& operator=(const TempScriptFile&) = delete;

    [[nodiscard]] const std::wstring& path() const { return path_; }
    [[nodiscard]] bool valid() const { return !path_.empty(); }

private:
    std::wstring path_;
};

/// Names the exact handles the child may inherit, rather than letting
/// bInheritHandles hand it every inheritable handle in the process. Two
/// concurrent runs would otherwise each inherit the other's pipe write end,
/// and a reader would never see EOF after its own child exited -- a hang in
/// the join below, with nothing anywhere to say why.
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

[[nodiscard]] std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point since) {
    const auto elapsed = std::chrono::steady_clock::now() - since;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

/// Everything that goes wrong before the shell is running reads the same way
/// to the caller: an exit code no PowerShell script produces, and the reason
/// where an operator will see it. core::ScriptOutcome has no separate "could
/// not start" flag -- timed_out is the only signal of that shape, and setting
/// it here would claim a timeout that never happened.
[[nodiscard]] core::ScriptOutcome could_not_start(const std::string& reason, DWORD error,
                                                  std::chrono::steady_clock::time_point started) {
    core::ScriptOutcome outcome;
    outcome.exit_code = -1;
    outcome.stderr_text =
        "lab_monitor: " + reason + " (Windows error " + std::to_string(error) + ")";
    outcome.duration_ms = elapsed_ms(started);
    return outcome;
}

[[nodiscard]] std::wstring executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    const std::size_t slash = buffer.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : buffer.substr(0, slash);
}

[[nodiscard]] std::wstring temp_directory() {
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const DWORD written = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    std::wstring path(buffer.data(), written);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

[[nodiscard]] bool write_all(HANDLE file, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t done = 0;
    while (done < size) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 20));
        if (WriteFile(file, bytes + done, chunk, &written, nullptr) == 0 || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

/// Writes the body beside the executable, falling back to the user's temp
/// directory. Beside the executable is where everything else this agent writes
/// lives, but an install under Program Files is read-only -- and a script
/// runner that cannot write its script is a feature that silently does
/// nothing, so it falls back rather than failing.
[[nodiscard]] std::wstring write_temp_script(const std::string& body) {
    static std::atomic<unsigned> sequence{0};
    const std::wstring name = L"\\lm-script-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(sequence.fetch_add(1)) + L".ps1";

    const std::array<std::wstring, 2> directories{executable_directory(), temp_directory()};
    for (const std::wstring& directory : directories) {
        if (directory.empty()) {
            continue;
        }
        const std::wstring path = directory + name;
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if (!file) {
            continue;
        }
        // A BOM, because powershell.exe reads a .ps1 without one as the ANSI
        // code page: a body carrying anything outside ASCII would then run as
        // mojibake rather than as what was authored.
        static constexpr std::array<unsigned char, 3> kUtf8Bom{0xEF, 0xBB, 0xBF};
        if (write_all(file.get(), kUtf8Bom.data(), kUtf8Bom.size()) &&
            write_all(file.get(), body.data(), body.size())) {
            return path;
        }
        file.reset();
        DeleteFileW(path.c_str());
    }
    return {};
}

/// The absolute path, not a bare name: this runs scripts, sometimes elevated,
/// and resolving the shell through PATH is exactly the search a hijack needs.
[[nodiscard]] std::wstring powershell_path() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const UINT written = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return L"powershell.exe";
    }
    std::wstring path(buffer.data(), written);
    path += L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return L"powershell.exe";
    }
    return path;
}

void drain_pipe(HANDLE pipe, std::string& into) {
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == 0) {
            return;  // the child closed its end, or a kill broke the pipe
        }
        if (read == 0) {
            return;
        }
        // Reading continues past the cap rather than stopping: an unread pipe
        // fills, and a full pipe blocks the script rather than capping it.
        append_capped(into, buffer.data(), read);
    }
}

class WindowsScriptRunner : public IScriptRunner {
public:
    core::ScriptOutcome run(const std::string& body, std::chrono::seconds timeout) override;
};

core::ScriptOutcome WindowsScriptRunner::run(const std::string& body,
                                             std::chrono::seconds timeout) {
    const auto started = std::chrono::steady_clock::now();

    const TempScriptFile script(write_temp_script(body));
    if (!script.valid()) {
        const DWORD error = GetLastError();
        return could_not_start("could not write the temporary script file", error, started);
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    Handle out_read;
    Handle out_write;
    Handle err_read;
    Handle err_write;
    if (CreatePipe(out_read.out(), out_write.out(), &inheritable, 0) == 0 ||
        CreatePipe(err_read.out(), err_write.out(), &inheritable, 0) == 0) {
        const DWORD error = GetLastError();
        return could_not_start("could not create the output pipes", error, started);
    }
    // Only the child's ends are inheritable; ours must not be, or a write end
    // outlives the child and the read never reaches EOF.
    SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);

    // NUL as stdin, so a script that reads it gets EOF at once instead of
    // inheriting whatever this process had. -NonInteractive already refuses
    // the prompt; this covers the reads that do not go through the host.
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                           OPEN_EXISTING, 0, nullptr));

    std::array<HANDLE, 3> inherited{out_write.get(), err_write.get(), nul.get()};
    const std::size_t inherited_count = nul ? inherited.size() : inherited.size() - 1;
    InheritList inherit_list;
    if (!inherit_list.build(inherited.data(), inherited_count)) {
        const DWORD error = GetLastError();
        return could_not_start("could not build the child's handle list", error, started);
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul.get();
    startup.StartupInfo.hStdOutput = out_write.get();
    startup.StartupInfo.hStdError = err_write.get();
    startup.lpAttributeList = inherit_list.get();

    // The job exists before the process so the process can be assigned to it
    // while still suspended: anything the script starts is then inside the job
    // too, which is what makes the timeout kill a tree rather than a shell. A
    // script that launched an installer would otherwise leave the installer
    // running after the timeout "killed" it.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    bool tree_kill = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        tree_kill = SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                            sizeof(limits)) != 0;
    }

    // -NonInteractive is load-bearing: without it a script that prompts sits
    // there until the timeout with nothing in its output to say why, which is
    // the least diagnosable failure this feature can produce.
    std::wstring command = L"\"" + powershell_path() +
                           L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                           script.path() + L"\"";

    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       nullptr, &startup.StartupInfo, &process_info) == 0) {
        const DWORD error = GetLastError();
        return could_not_start("could not start powershell.exe", error, started);
    }
    const Handle process(process_info.hProcess);
    const Handle main_thread(process_info.hThread);

    // Our copies of the write ends go now: while any one of them is open the
    // readers below never see the pipe close, however long ago the script
    // finished.
    out_write.reset();
    err_write.reset();
    nul.reset();

    std::string note;
    if (tree_kill && AssignProcessToJobObject(job.get(), process.get()) == 0) {
        tree_kill = false;
    }
    if (!tree_kill) {
        note =
            "lab_monitor: this run is not in a job object, so a timeout can stop the "
            "shell but not what it started\n";
    }

    // Separate threads, because draining the two pipes in turn deadlocks the
    // moment the one not being read fills: the script blocks writing to it and
    // never reaches the output this thread is waiting for.
    core::ScriptOutcome outcome;
    std::thread out_drain([&] { drain_pipe(out_read.get(), outcome.stdout_text); });
    std::thread err_drain([&] { drain_pipe(err_read.get(), outcome.stderr_text); });

    if (ResumeThread(main_thread.get()) == static_cast<DWORD>(-1)) {
        // Nothing to do but let the wait below time out and kill it: a
        // suspended process writes nothing and never exits.
        note += "lab_monitor: the script process could not be resumed\n";
    }

    constexpr std::int64_t kMaxWaitMs = static_cast<std::int64_t>(INFINITE) - 1;
    const std::int64_t seconds = timeout.count();
    // A zero or negative timeout allows no time at all, which the wait reports
    // as a timeout on the first look -- the honest reading of "no time".
    const std::int64_t wait_ms = seconds <= 0                  ? 0
                                 : seconds > kMaxWaitMs / 1000 ? kMaxWaitMs
                                                               : seconds * 1000;

    const DWORD waited = WaitForSingleObject(process.get(), static_cast<DWORD>(wait_ms));
    if (waited == WAIT_TIMEOUT) {
        outcome.timed_out = true;
        // A killed run has no exit code of its own, and 0 would read as success.
        outcome.exit_code = 1;
        if (!tree_kill) {
            TerminateProcess(process.get(), 1u);
        }
    } else if (waited == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(process.get(), &code) != 0) {
            outcome.exit_code = static_cast<std::int32_t>(code);
        }
    } else {
        outcome.exit_code = -1;
        note += "lab_monitor: waiting on the script process failed\n";
        if (!tree_kill) {
            TerminateProcess(process.get(), 1u);
        }
    }

    // Closing the job here, ahead of the join, is what guarantees the join
    // finishes: a grandchild still holding the inherited write end keeps the
    // pipe open long after the shell exited, and a reader would block on it
    // for as long as that process lived. Nothing outlives its own script.
    job.reset();

    out_drain.join();
    err_drain.join();

    outcome.stderr_text.insert(0, note);
    outcome.reported = core::parse_reported_result(outcome.stdout_text);
    outcome.duration_ms = elapsed_ms(started);
    return outcome;
}

}  // namespace

std::unique_ptr<IScriptRunner> make_script_runner() {
    return std::make_unique<WindowsScriptRunner>();
}

}  // namespace lm::platform
