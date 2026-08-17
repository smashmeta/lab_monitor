#include <gtest/gtest.h>

#include <algorithm>
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

TEST(WindowsNetworkProbe, ReportsAtLeastOneAdapter) {
    // Any machine running this has something. An empty result means the
    // enumeration failed rather than that the machine is bare.
    EXPECT_FALSE(enumerate_once().empty());
}

TEST(WindowsNetworkProbe, LeavesOutTheInterfacesNetworkConnectionsHides) {
    // The list is meant to match the Network Connections folder, and that
    // folder shows neither the software loopback pseudo-interface nor the
    // Teredo/ISATAP tunnel ones. Their presence would mean the probe had
    // fallen back to enumerating raw interfaces.
    for (const NetworkAdapter& adapter : enumerate_once()) {
        EXPECT_NE(adapter.type, AdapterType::Loopback) << adapter.name;
        EXPECT_EQ(adapter.description.find("Loopback Pseudo-Interface"), std::string::npos)
            << adapter.name;
    }
}

TEST(WindowsNetworkProbe, NamesTheAdapterTheWayWindowsDoesNotByItsGuid) {
    // "smash-wifi", not "{FC41A3EF-...}". The GUID is still carried, as the id.
    for (const NetworkAdapter& adapter : enumerate_once()) {
        EXPECT_FALSE(adapter.name.empty()) << "an adapter with no name cannot be identified";
        EXPECT_FALSE(adapter.name.starts_with("{"))
            << adapter.name << " looks like a GUID rather than a connection name";
        EXPECT_FALSE(adapter.description.empty()) << adapter.name;
    }
}

TEST(WindowsNetworkProbe, CarriesTheGuidAsTheIdentifier) {
    for (const NetworkAdapter& adapter : enumerate_once()) {
        EXPECT_FALSE(adapter.id.empty()) << adapter.name;
    }
}

TEST(WindowsNetworkProbe, IdsAreUnique) {
    // The id is the identity used to tell two identically-named connections
    // apart; neither the name nor the description is guaranteed distinct.
    std::vector<NetworkAdapter> adapters = enumerate_once();
    std::ranges::sort(adapters, {}, &NetworkAdapter::id);
    const auto duplicate = std::ranges::adjacent_find(adapters, {}, &NetworkAdapter::id);
    EXPECT_EQ(duplicate, adapters.end())
        << "duplicate adapter id: " << (duplicate != adapters.end() ? duplicate->id : "");
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
            EXPECT_NE(adapter.link, LinkState::Connected)
                << "a dialled entry should have come from the interface list instead";
        }
    }
    SUCCEED() << adapters.size() << " adapters reported on this machine";
}
