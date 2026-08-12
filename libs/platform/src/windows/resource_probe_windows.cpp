#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "lm/platform/probes.hpp"

namespace lm::platform {
namespace {

std::uint64_t to_uint64(const FILETIME& value) {
    ULARGE_INTEGER converted;
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

class WindowsResourceProbe : public IResourceProbe {
public:
    core::ResourceSample sample() override {
        core::ResourceSample result;
        result.cpu_percent = sample_cpu();
        sample_memory(result);
        result.disks = sample_disks();
        return result;
    }

private:
    double sample_cpu() {
        FILETIME idle_time{};
        FILETIME kernel_time{};
        FILETIME user_time{};
        if (GetSystemTimes(&idle_time, &kernel_time, &user_time) == 0) {
            return 0.0;
        }

        const std::uint64_t idle = to_uint64(idle_time);
        // Kernel time already includes idle time.
        const std::uint64_t total = to_uint64(kernel_time) + to_uint64(user_time);

        if (!primed_) {
            primed_ = true;
            previous_idle_ = idle;
            previous_total_ = total;
            return 0.0;
        }

        const std::uint64_t idle_delta = idle - previous_idle_;
        const std::uint64_t total_delta = total - previous_total_;
        previous_idle_ = idle;
        previous_total_ = total;

        if (total_delta == 0) {
            return 0.0;
        }

        const double busy = static_cast<double>(total_delta - idle_delta);
        const double percent = 100.0 * busy / static_cast<double>(total_delta);
        return percent < 0.0 ? 0.0 : (percent > 100.0 ? 100.0 : percent);
    }

    static void sample_memory(core::ResourceSample& result) {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) == 0) {
            return;
        }
        result.mem_total_bytes = status.ullTotalPhys;
        result.mem_used_bytes = status.ullTotalPhys - status.ullAvailPhys;
    }

    static std::vector<core::DiskUsage> sample_disks() {
        std::vector<core::DiskUsage> disks;

        wchar_t buffer[512] = {};
        const DWORD length = GetLogicalDriveStringsW(
            static_cast<DWORD>(std::size(buffer)) - 1, buffer);
        if (length == 0) {
            return disks;
        }

        for (const wchar_t* drive = buffer; *drive != L'\0'; drive += wcslen(drive) + 1) {
            if (GetDriveTypeW(drive) != DRIVE_FIXED) {
                continue;
            }

            ULARGE_INTEGER free_to_caller{};
            ULARGE_INTEGER total{};
            ULARGE_INTEGER total_free{};
            if (GetDiskFreeSpaceExW(drive, &free_to_caller, &total, &total_free) == 0) {
                continue;
            }

            core::DiskUsage usage;
            // Drive strings are ASCII-safe ("C:\"), so a narrowing copy is sound.
            for (const wchar_t* c = drive; *c != L'\0'; ++c) {
                usage.mount.push_back(static_cast<char>(*c));
            }
            usage.total_bytes = total.QuadPart;
            usage.free_bytes = total_free.QuadPart;
            disks.push_back(std::move(usage));
        }

        return disks;
    }

    bool primed_ = false;
    std::uint64_t previous_idle_ = 0;
    std::uint64_t previous_total_ = 0;
};

}  // namespace

std::unique_ptr<IResourceProbe> make_resource_probe() {
    return std::make_unique<WindowsResourceProbe>();
}

}  // namespace lm::platform
