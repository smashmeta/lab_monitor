#include <gtest/gtest.h>

#include <array>

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
    // Shown wherever a rule or a run says why it could not proceed. Asserting
    // only that the name is non-empty pins nothing: to_string() ends in a
    // "Unknown" fallthrough, so a missing switch arm would satisfy it. The
    // real guard against that is C4062 under /W4-as-errors -- a compiler
    // setting, not a test -- so the names themselves are asserted here.
    EXPECT_EQ(to_string(Capability::Scripts), "Scripts");
    EXPECT_EQ(to_string(Capability::Elevated), "Elevated");
}

TEST(Capabilities, TheNewBitsDoNotCollideWithTheExisting) {
    // The raw value rides the announce as a bitmask, so a collision would make
    // one capability silently imply another.
    //
    // Adding both new bits to a set and checking it grew does not test that:
    // give Scripts the same bit as Dds and a set already holding Dds loses
    // nothing, while Elevated alone still grows it -- the assertion passes
    // with the exact collision it is named for. Each new bit has to be looked
    // at on its own, in a set that holds nothing else.
    constexpr std::array kAll{Capability::Resources, Capability::Processes, Capability::Services,
                              Capability::Registry,  Capability::Network,   Capability::Dds,
                              Capability::Scripts,   Capability::Elevated};

    for (const Capability bit : {Capability::Scripts, Capability::Elevated}) {
        Capabilities only;
        only.add(bit);
        EXPECT_TRUE(only.has(bit));
        for (const Capability other : kAll) {
            if (other == bit) {
                continue;
            }
            EXPECT_FALSE(only.has(other))
                << to_string(bit) << " shares a bit with " << to_string(other);
        }
    }
}
