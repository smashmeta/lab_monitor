#include <gtest/gtest.h>

#include <memory>

#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

/// A key that exists on every Windows installation, used as a stable fixture.
constexpr const char* kCurrentVersionKey = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

RegistryRule rule_for(std::string key_path, std::string value_name,
                      RegistryHive hive = RegistryHive::LocalMachine) {
    RegistryRule rule;
    rule.hive = hive;
    rule.key_path = std::move(key_path);
    rule.value_name = std::move(value_name);
    rule.match = RegistryMatch::Exists;
    return rule;
}

}  // namespace

TEST(WindowsRegistryProbe, ReadsAnExistingStringValue) {
    const auto probe = make_registry_probe();
    ASSERT_NE(probe, nullptr);

    const RegistryValue value = probe->read(rule_for(kCurrentVersionKey, "ProductName"));

    EXPECT_TRUE(value.exists);
    EXPECT_TRUE(value.error.empty()) << value.error;
    // Every Windows edition's ProductName contains "Windows".
    EXPECT_NE(value.data.find("Windows"), std::string::npos) << "data was: " << value.data;
}

TEST(WindowsRegistryProbe, RendersADwordAsItsDecimalValue) {
    const auto probe = make_registry_probe();

    const RegistryValue value =
        probe->read(rule_for(kCurrentVersionKey, "CurrentMajorVersionNumber"));

    EXPECT_TRUE(value.exists);
    EXPECT_TRUE(value.error.empty()) << value.error;
    // A DWORD must render as a plain decimal string so rule authors can write
    // Equals "10" rather than guessing at a binary rendering.
    EXPECT_EQ(value.data, "10");
}

TEST(WindowsRegistryProbe, AMissingValueIsAbsentNotAnError) {
    const auto probe = make_registry_probe();

    const RegistryValue value =
        probe->read(rule_for(kCurrentVersionKey, "LabMonitorNoSuchValue_7f3a"));

    // Absent must not be reported as an error: a MustBeAbsent rule has to be
    // able to pass, and evaluate() turns a non-empty error into CheckStatus::Error.
    EXPECT_FALSE(value.exists);
    EXPECT_TRUE(value.error.empty()) << value.error;
}

TEST(WindowsRegistryProbe, AMissingKeyIsAbsentNotAnError) {
    const auto probe = make_registry_probe();

    const RegistryValue value =
        probe->read(rule_for("SOFTWARE\\LabMonitorNoSuchKey_7f3a\\Nested", "AnyValue"));

    EXPECT_FALSE(value.exists);
    EXPECT_TRUE(value.error.empty()) << value.error;
}

TEST(WindowsRegistryProbe, ReadsFromCurrentUserHive) {
    const auto probe = make_registry_probe();

    // HKCU\Environment exists for every interactive user; TEMP is always set.
    const RegistryValue value =
        probe->read(rule_for("Environment", "TEMP", RegistryHive::CurrentUser));

    EXPECT_TRUE(value.exists);
    EXPECT_TRUE(value.error.empty()) << value.error;
    EXPECT_FALSE(value.data.empty());
}

TEST(WindowsRegistryProbe, ReportsTheDefaultValueWhenValueNameIsEmpty) {
    const auto probe = make_registry_probe();

    // An empty value name addresses the key's unnamed default value. This must
    // not be confused with "no value requested".
    const RegistryValue value = probe->read(rule_for("SOFTWARE\\Classes\\.txt", ""));

    EXPECT_TRUE(value.error.empty()) << value.error;
    // .txt's default value is its ProgID; present on any normal install, but
    // assert only that the read succeeded rather than on a specific ProgID.
    EXPECT_TRUE(value.exists);
}
