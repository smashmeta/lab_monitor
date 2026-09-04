#include <gtest/gtest.h>

#include "lm/platform/probes.hpp"

TEST(Elevation, AnswersWithoutThrowing) {
    // The value depends on how the test runner was launched, so the assertion
    // is that the question is answerable at all -- a throwing or hanging
    // implementation would take the agent's startup with it.
    EXPECT_NO_THROW({
        const bool elevated = lm::platform::is_elevated();
        (void)elevated;
    });
}

TEST(Elevation, IsStableWithinAProcess) {
    // A process's token does not change under it, so two calls must agree.
    // If they ever disagree the capability advertised on the announce would
    // flap, and the server would show a host gaining and losing elevation.
    EXPECT_EQ(lm::platform::is_elevated(), lm::platform::is_elevated());
}
