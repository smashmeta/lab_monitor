#include <gtest/gtest.h>

#include <cmath>
#include <utility>

#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

TEST(ResourceProbe, FirstSampleReportsZeroCpuByDesign) {
    const auto probe = make_resource_probe();
    ASSERT_NE(probe, nullptr);
    EXPECT_DOUBLE_EQ(probe->sample().cpu_percent, 0.0);
}

TEST(ResourceProbe, CpuStaysWithinRangeAcrossSamples) {
    const auto probe = make_resource_probe();
    probe->sample();

    // Burn a little CPU so the second delta is non-degenerate.
    volatile double sink = 0.0;
    for (int i = 1; i < 4'000'000; ++i) {
        sink += std::sqrt(static_cast<double>(i));
    }

    const ResourceSample sample = probe->sample();
    EXPECT_GE(sample.cpu_percent, 0.0);
    EXPECT_LE(sample.cpu_percent, 100.0);
    EXPECT_FALSE(std::isnan(sample.cpu_percent));
}

TEST(ResourceProbe, ReportsPlausibleMemory) {
    const auto probe = make_resource_probe();
    const ResourceSample sample = probe->sample();

    EXPECT_GT(sample.mem_total_bytes, 0u);
    EXPECT_LE(sample.mem_used_bytes, sample.mem_total_bytes);
    // Any machine running this test has at least 256 MB.
    EXPECT_GT(sample.mem_total_bytes, 256ull * 1024 * 1024);
}

TEST(ResourceProbe, ReportsAtLeastOneVolume) {
    const auto probe = make_resource_probe();
    const ResourceSample sample = probe->sample();

    ASSERT_FALSE(sample.disks.empty());
    for (const DiskUsage& disk : sample.disks) {
        EXPECT_FALSE(disk.mount.empty());
        EXPECT_GT(disk.total_bytes, 0u);
        EXPECT_LE(disk.free_bytes, disk.total_bytes);
        EXPECT_GE(disk.used_percent(), 0.0);
        EXPECT_LE(disk.used_percent(), 100.0);
    }
}

TEST(DiskUsage, UsedPercentHandlesZeroTotalWithoutDividingByZero) {
    const DiskUsage empty{"/none", 0, 0};
    EXPECT_DOUBLE_EQ(empty.used_percent(), 0.0);
}

TEST(PlatformProbes, ProvideResourcesAndMatchDeclaredCapabilities) {
    ProbeSet probes = make_platform_probes();
    ASSERT_NE(probes.resources, nullptr);

    HostProbes host{local_host_name(), std::move(probes), platform_capabilities()};
    EXPECT_TRUE(host.capabilities().has(Capability::Resources));
    EXPECT_FALSE(host.host_id().empty());
}
