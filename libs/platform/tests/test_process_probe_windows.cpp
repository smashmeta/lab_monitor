#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

bool contains_executable(const std::vector<ProcessInfo>& processes, std::string_view wanted) {
    return std::any_of(processes.begin(), processes.end(), [&](const ProcessInfo& info) {
        return info.executable.size() == wanted.size() &&
               std::equal(info.executable.begin(), info.executable.end(), wanted.begin(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    });
}

const ProcessInfo* find_executable(const std::vector<ProcessInfo>& processes,
                                   std::string_view wanted) {
    const auto found = std::find_if(processes.begin(), processes.end(), [&](const ProcessInfo& i) {
        return i.executable.size() == wanted.size() &&
               std::equal(i.executable.begin(), i.executable.end(), wanted.begin(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    });
    return found == processes.end() ? nullptr : &*found;
}

}  // namespace

TEST(WindowsProcessProbe, EnumeratesRunningProcesses) {
    const auto probe = make_process_probe();
    ASSERT_NE(probe, nullptr);

    const std::vector<ProcessInfo> processes = probe->enumerate();

    // Any live Windows session has well over a handful of processes.
    EXPECT_GT(processes.size(), 10u);
}

TEST(WindowsProcessProbe, FindsThisTestProcessItself) {
    const auto probe = make_process_probe();

    const std::vector<ProcessInfo> processes = probe->enumerate();

    // The strongest available assertion: the probe must see the very process
    // running the assertion. A probe that silently returned a stale or empty
    // list would fail here.
    EXPECT_TRUE(contains_executable(processes, "lm_platform_tests.exe"));
}

TEST(WindowsProcessProbe, ReportsBareExecutableNamesNotFullPaths) {
    const auto probe = make_process_probe();

    const std::vector<ProcessInfo> processes = probe->enumerate();

    ASSERT_FALSE(processes.empty());
    for (const ProcessInfo& info : processes) {
        EXPECT_FALSE(info.executable.empty());
        // Rules are authored as "antivirus.exe", so the probe must not report
        // "C:\Program Files\...\antivirus.exe" or matching would never succeed.
        EXPECT_EQ(info.executable.find('\\'), std::string::npos) << info.executable;
        EXPECT_EQ(info.executable.find('/'), std::string::npos) << info.executable;
    }
}

TEST(WindowsProcessProbe, ReadsAFileVersionForAKnownSystemBinary) {
    const auto probe = make_process_probe();

    const std::vector<ProcessInfo> processes = probe->enumerate();

    // explorer.exe is running in any interactive session and always carries a
    // version resource, so it exercises the version-reading path end to end.
    const ProcessInfo* explorer = find_executable(processes, "explorer.exe");
    ASSERT_NE(explorer, nullptr) << "explorer.exe not found; is this an interactive session?";
    ASSERT_TRUE(explorer->version.has_value()) << "no version read for explorer.exe";
    ASSERT_FALSE(explorer->version->parts.empty());
    // Windows 10/11 file versions start at 10.
    EXPECT_GE(explorer->version->parts.front(), 6);
}

TEST(WindowsProcessProbe, LeavesVersionUnsetRatherThanGuessingWhenUnreadable) {
    const auto probe = make_process_probe();

    const std::vector<ProcessInfo> processes = probe->enumerate();

    // Protected system processes cannot be opened for a version read. Whatever
    // the probe cannot determine must come back as nullopt, never as a
    // fabricated 0.0.0.0 -- evaluate() reports "version unreadable" as Error,
    // which is honest, whereas a fake zero would silently fail a >= rule.
    ASSERT_FALSE(processes.empty());
    for (const ProcessInfo& info : processes) {
        if (info.version.has_value()) {
            EXPECT_FALSE(info.version->parts.empty()) << info.executable;
        }
    }
    SUCCEED();
}
