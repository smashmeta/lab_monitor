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
        // name is the renameable one from Network Connections; id is the GUID.
        NetworkAdapter{"smash-lan", "Intel(R) Ethernet I219-LM", "{4A2B-0000}",
                        AdapterType::Ethernet, LinkState::Connected},
        // Enabled with nothing plugged in — the state Windows draws a cross on,
        // and the one a bool could not distinguish from "switched off".
        NetworkAdapter{"smash-wifi", "Intel(R) Wi-Fi 6 AX201", "{9F31-0000}", AdapterType::WiFi,
                        LinkState::NoMedia},
        // The case this feature exists for: a dial-up entry that is defined but
        // not dialled, which no adapter enumeration would report at all.
        NetworkAdapter{"Lab Dialup", "RAS entry Lab Dialup", "RAS entry Lab Dialup",
                        AdapterType::Modem, LinkState::Disconnected},
    };

    const ResourceSampleMessage decoded = round_trip(original);
    EXPECT_EQ(decoded, original);
    ASSERT_EQ(decoded.sample.adapters.size(), 3u);
    EXPECT_EQ(decoded.sample.adapters[1].link, LinkState::NoMedia);
    EXPECT_EQ(decoded.sample.adapters[2].link, LinkState::Disconnected);
}

TEST(Codec, ResourceSampleRejectsALinkStateItDoesNotKnow) {
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.adapters = {
        NetworkAdapter{"eth0", "NIC", "{eth0-guid}", AdapterType::Ethernet, LinkState::Connected}};
    std::vector<std::uint8_t> bytes = encode(original);

    // The link byte is the last one written for an adapter.
    ASSERT_FALSE(bytes.empty());
    bytes.back() = 200;

    ResourceSampleMessage decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, ResourceSampleRoundTripsDisksAndAdaptersTogether) {
    ResourceSampleMessage original;
    original.host_id = "PC-002";
    original.sample.disks = {DiskUsage{"C:\\", 1000, 250}};
    original.sample.adapters = {NetworkAdapter{"lo", "Loopback", "{lo-guid}", AdapterType::Loopback, LinkState::Connected}};

    EXPECT_EQ(round_trip(original), original);
}

TEST(Codec, ResourceSampleRejectsAnAdapterTypeItDoesNotKnow) {
    // Enum values off the wire are integers until checked, exactly as
    // CheckStatus is in a compliance report.
    ResourceSampleMessage original;
    original.host_id = "PC-001";
    original.sample.adapters = {NetworkAdapter{"eth0", "NIC", "{eth0-guid}", AdapterType::Ethernet, LinkState::Connected}};
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
        NetworkAdapter{"eth0", "Onboard NIC", "{eth0-guid}", AdapterType::Ethernet, LinkState::Connected},
        NetworkAdapter{"wlan0", "Wireless", "{wlan0-guid}", AdapterType::WiFi, LinkState::Connected},
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

TEST(Codec, ClientAnnounceCarriesThePauseFlag) {
    ClientAnnounce original;
    original.host_id = "PC-001";
    original.agent_version = "1.2.3";
    original.capabilities = 7;
    original.paused = true;

    ClientAnnounce decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_TRUE(decoded.paused);
    EXPECT_EQ(decoded, original);
}

TEST(Codec, ClientAnnounceWithoutAPauseFieldStillDecodes) {
    // What a client built before the flag existed puts on the wire: the same
    // three fields and then the end of the buffer. Dropping such an announce
    // would take the machine off the fleet entirely -- the announce is the only
    // carrier of its capabilities -- so a short buffer has to mean "not
    // paused", not "malformed".
    ClientAnnounce original;
    original.host_id = "PC-001";
    original.agent_version = "1.2.3";
    original.capabilities = 7;
    original.paused = true;

    std::vector<std::uint8_t> bytes = encode(original);
    ASSERT_FALSE(bytes.empty());
    bytes.pop_back();  // drop the trailing bool, leaving an old-format payload

    ClientAnnounce decoded;
    ASSERT_TRUE(decode(bytes, decoded)) << "an old announce must not fail to decode";
    EXPECT_FALSE(decoded.paused);
    EXPECT_EQ(decoded.host_id, "PC-001");
    EXPECT_EQ(decoded.agent_version, "1.2.3");
    EXPECT_EQ(decoded.capabilities, 7u);
}

TEST(Codec, ScriptCommandRoundTrips) {
    ScriptCommand original;
    original.host_id = "PC-001";
    original.run_id = "run-7f3a";
    original.script_name = "Maintenance/Clear-TempFiles.ps1";
    original.script_body = "Write-Output \"hello\"\nexit 0\n";
    original.timeout_seconds = 120;

    ScriptCommand decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded, original);
}

TEST(Codec, ScriptCommandSurvivesABodyWithEveryAwkwardCharacter) {
    // A PowerShell script is not a tidy identifier: quotes, backslashes,
    // newlines and UTF-8 all appear routinely, and a codec that mangles any of
    // them corrupts the script silently on its way to a hundred machines.
    ScriptCommand original;
    original.host_id = "PC-001";
    original.run_id = "run-1";
    original.script_name = "(custom script)";
    original.script_body = "$p = \"C:\\Program Files\\Acme\"\r\n# \xc3\xa5\xc3\xa4\xc3\xb6\nexit 0";

    ScriptCommand decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded.script_body, original.script_body);
}

TEST(Codec, ScriptResultRoundTrips) {
    ScriptResultMessage original;
    original.host_id = "PC-001";
    original.run_id = "run-7f3a";
    original.status = lm::core::ScriptStatus::Failed;
    original.exit_code = 3;
    original.has_reported = true;
    original.reported_ok = false;
    original.reported_message = "3 of 5 packages failed";
    original.stdout_text = "line one\nline two";
    original.stderr_text = "something went wrong";
    original.duration_ms = 4200;

    ScriptResultMessage decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded, original);
}

TEST(Codec, ScriptResultCarriesARefusalWithItsReason) {
    ScriptResultMessage original;
    original.host_id = "PC-001";
    original.run_id = "run-7f3a";
    original.status = lm::core::ScriptStatus::Refused;
    original.refusal_reason = "not enrolled for scripts";

    ScriptResultMessage decoded;
    ASSERT_TRUE(decode(encode(original), decoded));
    EXPECT_EQ(decoded.status, lm::core::ScriptStatus::Refused);
    EXPECT_EQ(decoded.refusal_reason, "not enrolled for scripts");
}

TEST(Codec, ATruncatedScriptCommandIsRejected) {
    // Unlike ClientAnnounce, there is no tolerance here. A half-read command is
    // a half-read script body, and running that would be worse than running
    // nothing at all.
    ScriptCommand original;
    original.host_id = "PC-001";
    original.run_id = "run-1";
    original.script_body = "exit 0";

    std::vector<std::uint8_t> bytes = encode(original);
    bytes.resize(bytes.size() / 2);

    ScriptCommand decoded;
    EXPECT_FALSE(decode(bytes, decoded));
}

TEST(Codec, ScriptMessagesAreKeyedByHost) {
    ScriptCommand command;
    command.host_id = "PC-001";
    EXPECT_EQ(key_of(command), "PC-001");

    ScriptResultMessage result;
    result.host_id = "PC-002";
    EXPECT_EQ(key_of(result), "PC-002");
}
