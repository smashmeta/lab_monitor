#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

std::vector<NetworkAdapter> enumerate_once() {
    const std::unique_ptr<INetworkProbe> probe = make_network_probe();
    EXPECT_NE(probe, nullptr);
    return probe->enumerate();
}

}  // namespace

TEST(WindowsNetworkProbe, FindsAtLeastTheLoopbackInterface) {
    // Every Windows machine has one, so an empty result means the enumeration
    // failed rather than that the machine is bare.
    const std::vector<NetworkAdapter> adapters = enumerate_once();

    ASSERT_FALSE(adapters.empty());
    EXPECT_TRUE(std::ranges::any_of(adapters, [](const NetworkAdapter& adapter) {
        return adapter.type == AdapterType::Loopback;
    })) << "no loopback interface was reported";
}

TEST(WindowsNetworkProbe, GivesEveryAdapterANameAndADescription) {
    for (const NetworkAdapter& adapter : enumerate_once()) {
        EXPECT_FALSE(adapter.name.empty()) << "an adapter with no name cannot be identified";
        EXPECT_FALSE(adapter.description.empty()) << adapter.name;
    }
}

TEST(WindowsNetworkProbe, NamesAreUnique) {
    // The name is the identity used to tell two identical cards apart; the
    // description is not necessarily distinct.
    std::vector<NetworkAdapter> adapters = enumerate_once();
    std::ranges::sort(adapters, {}, &NetworkAdapter::name);
    const auto duplicate = std::ranges::adjacent_find(adapters, {}, &NetworkAdapter::name);
    EXPECT_EQ(duplicate, adapters.end())
        << "duplicate adapter name: " << (duplicate != adapters.end() ? duplicate->name : "");
}

TEST(WindowsNetworkProbe, ClassifiesEveryAdapterAsSomethingKnown) {
    for (const NetworkAdapter& adapter : enumerate_once()) {
        EXPECT_NE(adapter.type, AdapterType::Unknown)
            << adapter.description << " was left unclassified";
    }
}

TEST(WindowsNetworkProbe, IsStableAcrossBackToBackCalls) {
    // Nothing here is a delta against the previous call (unlike the CPU probe),
    // so two reads in a row must agree.
    EXPECT_EQ(enumerate_once().size(), enumerate_once().size());
}

TEST(WindowsNetworkProbe, ReportsRasEntriesAsModemsWhicheverWayTheyAreDialled) {
    // This machine may well have no dial-up or VPN entries, so the assertion is
    // conditional: what matters is that when one *is* reported it is typed as a
    // modem, and that a disconnected one is reported at all rather than being
    // dropped for having no live interface.
    const std::vector<NetworkAdapter> adapters = enumerate_once();
    for (const NetworkAdapter& adapter : adapters) {
        if (adapter.description.rfind("RAS entry ", 0) == 0) {
            EXPECT_EQ(adapter.type, AdapterType::Modem);
            EXPECT_FALSE(adapter.connected)
                << "a dialled entry should have come from the interface list instead";
        }
    }
    SUCCEED() << adapters.size() << " adapters reported on this machine";
}
