#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lm/transport/codec.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

template <typename Message>
Message round_trip(const Message& original) {
    const std::vector<std::uint8_t> bytes = encode(original);
    EXPECT_FALSE(bytes.empty());
    Message decoded;
    EXPECT_TRUE(decode(bytes, decoded));
    return decoded;
}

}  // namespace

TEST(Codec, ClientAnnounceRoundTrips) {
    ClientAnnounce original;
    original.host_id = "PC-001";
    original.agent_version = "0.1.0";
    original.capabilities = platform_capabilities().raw();

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ClientAnnounceHandlesEmptyStrings) {
    const ClientAnnounce original;
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ResourceSampleRoundTripsWithMultipleDisks) {
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.cpu_percent = 37.25;
    original.sample.mem_total_bytes = 17'179'869'184ull;
    original.sample.mem_used_bytes = 9'000'000'000ull;
    original.sample.disks = {DiskUsage{"C:\\", 500'107'862'016ull, 123'456'789'012ull},
                             DiskUsage{"D:\\", 2'000'398'934'016ull, 1'500'000'000'000ull}};

    const ResourceSampleMessage decoded = round_trip(original);
    EXPECT_EQ(decoded, original);
    EXPECT_DOUBLE_EQ(decoded.sample.cpu_percent, 37.25);
}

TEST(Codec, ResourceSampleRoundTripsWithNoDisks) {
    ResourceSampleMessage original;
    original.host_id = "HEADLESS";
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ResourceSampleRoundTripsNetworkAdapters) {
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.adapters = {
        NetworkAdapter{"{4A2B-...}", "Intel(R) Ethernet I219-LM", AdapterType::Ethernet, true},
        NetworkAdapter{"{9F31-...}", "Intel(R) Wi-Fi 6 AX201", AdapterType::WiFi, false},
        // The case this feature exists for: a dial-up entry that is defined but
        // not dialled, which no adapter enumeration would report at all.
        NetworkAdapter{"Site VPN", "RAS entry Site VPN", AdapterType::Modem, false},
    };

    const ResourceSampleMessage decoded = round_trip(original);
    EXPECT_EQ(decoded, original);
    ASSERT_EQ(decoded.sample.adapters.size(), 3u);
    EXPECT_EQ(decoded.sample.adapters[2].type, AdapterType::Modem);
    EXPECT_FALSE(decoded.sample.adapters[2].connected);
}

TEST(Codec, ResourceSampleRoundTripsDisksAndAdaptersTogether) {
    ResourceSampleMessage original;
    original.host_id = "PC-002";
    original.sample.disks = {DiskUsage{"C:\\", 1000, 250}};
    original.sample.adapters = {NetworkAdapter{"lo", "Loopback", AdapterType::Loopback, true}};

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ResourceSampleRejectsAnAdapterTypeItDoesNotKnow) {
    // Enum values off the wire are integers until checked, exactly as
    // CheckStatus is in a compliance report.
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.adapters = {NetworkAdapter{"eth0", "NIC", AdapterType::Ethernet, true}};
    std::vector<std::uint8_t> bytes = encode(original);

    // The type byte is the only 0..7 value near the tail; find and corrupt it
    // by walking back from the trailing `connected` byte.
    ASSERT_GE(bytes.size(), 2u);
    bytes[bytes.size() - 2] = 250;

    ResourceSampleMessage decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, ResourceSampleRejectsATruncatedAdapterList) {
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.adapters = {
        NetworkAdapter{"eth0", "Onboard NIC", AdapterType::Ethernet, true},
        NetworkAdapter{"wlan0", "Wireless", AdapterType::WiFi, true},
    };
    std::vector<std::uint8_t> bytes = encode(original);
    bytes.resize(bytes.size() - 6);

    ResourceSampleMessage decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, TemplateBundleEnvelopeRoundTrips) {
    TemplateBundleMessage original;
    original.revision = 18'446'744'073'709'551'615ull;  // max uint64
    original.hash = "0123456789abcdef";
    original.json = R"({"revision":42,"templates":[]})";

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ComplianceReportRoundTripsEveryStatus) {
    ComplianceReportMessage original;
    original.report.host_id = "PC-001";
    original.report.applied_revision = 9;
    original.report.results = {
        CheckResult{"r1", CheckStatus::Pass, "running", ""},
        CheckResult{"r2", CheckStatus::Fail, "not running", ""},
        CheckResult{"r3", CheckStatus::NotApplicable, "not supported on this platform", ""},
        CheckResult{"r4", CheckStatus::Error, "read failed", "ERROR_ACCESS_DENIED"}};

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ComplianceReportRoundTripsWithNoResults) {
    ComplianceReportMessage original;
    original.report.host_id = "PC-001";
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, HandlesNonAsciiAndEmbeddedSeparators) {
    ClientAnnounce original;
    original.host_id = "PC-\xC3\x98-001";
    original.agent_version = "0.1.0\nbuild\ttab";
    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, DecodeRejectsTruncatedPayload) {
    ClientAnnounce original;
    original.host_id = "PC-001";
    original.agent_version = "0.1.0";
    std::vector<std::uint8_t> bytes = encode(original);
    ASSERT_GT(bytes.size(), 4u);
    bytes.resize(bytes.size() / 2);

    ClientAnnounce decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, DecodeRejectsEmptyPayload) {
    ClientAnnounce decoded;
    EXPECT_FALSE(decode(std::vector<std::uint8_t>{}, decoded));
}

TEST(Codec, DecodeRejectsHugeDiskCountWithoutAllocating) {
    ResourceSampleMessage original;
    original.host_id = "HEADLESS";
    std::vector<std::uint8_t> bytes = encode(original);
    ASSERT_GE(bytes.size(), 4u);
    // original has zero disks, so the trailing four bytes are the (zero) disk
    // count with nothing serialised behind them. Corrupt just those bytes to
    // claim billions of disks while leaving no bytes for FastCDR to read.
    for (std::size_t i = bytes.size() - 4; i < bytes.size(); ++i) {
        bytes[i] = 0xFF;
    }

    ResourceSampleMessage decoded;
    bool ok = true;
    EXPECT_NO_THROW(ok = decode(bytes, decoded));
    EXPECT_FALSE(ok);
}

TEST(Codec, DecodeRejectsHugeResultCountWithoutAllocating) {
    ComplianceReportMessage original;
    original.report.host_id = "PC-001";
    std::vector<std::uint8_t> bytes = encode(original);
    ASSERT_GE(bytes.size(), 4u);
    // original has zero results, so the trailing four bytes are the (zero)
    // result count with nothing serialised behind them.
    for (std::size_t i = bytes.size() - 4; i < bytes.size(); ++i) {
        bytes[i] = 0xFF;
    }

    ComplianceReportMessage decoded;
    bool ok = true;
    EXPECT_NO_THROW(ok = decode(bytes, decoded));
    EXPECT_FALSE(ok);
}

TEST(Codec, KeysComeFromTheHostId) {
    ClientAnnounce announce;
    announce.host_id = "PC-001";
    EXPECT_EQ(key_of(announce), "PC-001");

    ResourceSampleMessage sample;
    sample.host_id = "PC-002";
    EXPECT_EQ(key_of(sample), "PC-002");

    ComplianceReportMessage report;
    report.report.host_id = "PC-003";
    EXPECT_EQ(key_of(report), "PC-003");
}
