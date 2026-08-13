#include <gtest/gtest.h>

#include "lm/core/types.hpp"

using namespace lm::core;

TEST(Capabilities, StartsEmpty) {
    const Capabilities caps;
    EXPECT_FALSE(caps.has(Capability::Resources));
    EXPECT_FALSE(caps.has(Capability::Registry));
}

TEST(Capabilities, AddIsIndependentPerFlag) {
    Capabilities caps;
    caps.add(Capability::Processes);
    EXPECT_TRUE(caps.has(Capability::Processes));
    EXPECT_FALSE(caps.has(Capability::Services));
}

TEST(Capabilities, RegistryOnlyOnWindows) {
    const Capabilities caps = platform_capabilities();
    EXPECT_TRUE(caps.has(Capability::Resources));
#ifdef _WIN32
    EXPECT_TRUE(caps.has(Capability::Registry));
#else
    EXPECT_FALSE(caps.has(Capability::Registry));
#endif
}

TEST(RequiredCapability, MapsEachRuleKind) {
    EXPECT_EQ(required_capability(RuleKind::Process), Capability::Processes);
    EXPECT_EQ(required_capability(RuleKind::Service), Capability::Services);
    EXPECT_EQ(required_capability(RuleKind::Registry), Capability::Registry);
}
