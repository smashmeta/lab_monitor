#include <sys/statvfs.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lm/platform/probes.hpp"

namespace lm::platform {
namespace {

/// Filesystems that are not real storage and would otherwise clutter the
/// view, plus network filesystems: Windows filters disks on
/// GetDriveTypeW(...) != DRIVE_FIXED, which excludes remote drives, so
/// nfs/nfs4/cifs/smbfs are excluded here to match -- without this, a
/// hard-mounted, currently-unreachable NFS/CIFS share makes the statvfs()
/// call in sample_disks() below block for the RPC/SMB timeout, stalling the
/// 2 s resource-sampling tick and making a healthy machine read Offline.
bool is_pseudo_filesystem(const std::string& type) {
    static const std::vector<std::string> kPseudo = {
        "proc",   "sysfs",     "devtmpfs", "devpts", "tmpfs",   "cgroup",  "cgroup2",
        "pstore", "securityfs", "debugfs",  "tracefs", "mqueue", "hugetlbfs", "overlay",
        "squashfs", "autofs",  "binfmt_misc", "configfs", "fusectl", "bpf", "ramfs",
        "nfs", "nfs4", "cifs", "smbfs"};
    for (const std::string& pseudo : kPseudo) {
        if (type == pseudo) {
            return true;
        }
    }
    // FUSE mounts report as "fuse.<helper>" (e.g. "fuse.sshfs", "fuse.encfs"
    // -- the suffix names whichever userspace helper mounted it), never a
    // single fixed string, so this needs a prefix check rather than another
    // kPseudo entry. Many of these are themselves network filesystems
    // (sshfs, davfs, etc.) with the same unreachable-host blocking-statvfs
    // risk as nfs/cifs above.
    constexpr std::string_view kFusePrefix = "fuse.";
    if (type.compare(0, kFusePrefix.size(), kFusePrefix) == 0) {
        return true;
    }
    return false;
}

class LinuxResourceProbe : public IResourceProbe {
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
        std::ifstream stat("/proc/stat");
        if (!stat) {
            return 0.0;
        }

        std::string line;
        if (!std::getline(stat, line) || line.rfind("cpu ", 0) != 0) {
            return 0.0;
        }

        std::istringstream fields(line.substr(4));
        std::uint64_t value = 0;
        std::uint64_t total = 0;
        std::uint64_t idle = 0;
        for (int index = 0; fields >> value; ++index) {
            total += value;
            // Fields 3 and 4 are idle and iowait.
            if (index == 3 || index == 4) {
                idle += value;
            }
        }

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
        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo) {
            return;
        }

        std::uint64_t total_kb = 0;
        std::uint64_t available_kb = 0;
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        while (meminfo >> key >> value >> unit) {
            if (key == "MemTotal:") {
                total_kb = value;
            } else if (key == "MemAvailable:") {
                available_kb = value;
                break;
            }
        }

        result.mem_total_bytes = total_kb * 1024;
        result.mem_used_bytes = (total_kb - available_kb) * 1024;
    }

    static std::vector<core::DiskUsage> sample_disks() {
        std::vector<core::DiskUsage> disks;

        std::ifstream mounts("/proc/mounts");
        if (!mounts) {
            return disks;
        }

        std::string device;
        std::string mount_point;
        std::string type;
        std::string remainder;
        while (mounts >> device >> mount_point >> type) {
            std::getline(mounts, remainder);
            if (is_pseudo_filesystem(type)) {
                continue;
            }

            struct statvfs stats {};
            if (statvfs(mount_point.c_str(), &stats) != 0 || stats.f_blocks == 0) {
                continue;
            }

            core::DiskUsage usage;
            usage.mount = mount_point;
            usage.total_bytes = static_cast<std::uint64_t>(stats.f_blocks) * stats.f_frsize;
            usage.free_bytes = static_cast<std::uint64_t>(stats.f_bavail) * stats.f_frsize;
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
    return std::make_unique<LinuxResourceProbe>();
}

}  // namespace lm::platform
