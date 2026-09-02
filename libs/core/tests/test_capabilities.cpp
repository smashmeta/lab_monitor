#include <gtest/gtest.h>

#include "lm/core/types.hpp"

using namespace lm::core;

TEST(Capabilities, CarriesScriptsAndElevatedIndependently) {
    Capabilities caps;
    caps.add(Capability::Scripts);

    EXPECT_TRUE(caps.has(Capability::Scripts));
    EXPECT_FALSE(caps.has(Capability::Elevated))
        << "enrolled for scripts is not the same as able to run them elevated";

    caps.add(Capability::Elevated);
    EXPECT_TRUE(caps.has(Capability::Elevated));
}

TEST(Capabilities, NamesTheNewOnes) {
    // Shown wherever a rule or a run says why it could not proceed.
    EXPECT_FALSE(to_string(Capability::Scripts).empty());
    EXPECT_FALSE(to_string(Capability::Elevated).empty());
}

TEST(Capabilities, TheNewBitsDoNotCollideWithTheExisting) {
    // The raw value rides the announce as a bitmask, so a collision would make
    // one capability silently imply another.
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Dds);
    const std::uint32_t before = caps.raw();

    caps.add(Capability::Scripts).add(Capability::Elevated);
    EXPECT_EQ(before & caps.raw(), before);
    EXPECT_NE(caps.raw(), before);
}
