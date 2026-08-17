#include <winsock2.h>
// winsock2.h must precede windows.h, and iphlpapi.h needs both.
#include <windows.h>

#include <iphlpapi.h>
#include <ras.h>
#include <raserror.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "lm/platform/probes.hpp"
#include "network_probe_windows.hpp"

namespace lm::platform {
namespace {

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

/// IANA ifType values, as reported in IP_ADAPTER_ADDRESSES::IfType.
core::AdapterType map_if_type(IFTYPE if_type) {
    switch (if_type) {
        case IF_TYPE_ETHERNET_CSMACD:
            return core::AdapterType::Ethernet;
        case IF_TYPE_IEEE80211:
            return core::AdapterType::WiFi;
        case IF_TYPE_SOFTWARE_LOOPBACK:
            return core::AdapterType::Loopback;
        case IF_TYPE_PPP:
            return core::AdapterType::Ppp;
        case IF_TYPE_TUNNEL:
            return core::AdapterType::Tunnel;
        case IF_TYPE_ISO88025_TOKENRING:
        case IF_TYPE_ATM:
        case IF_TYPE_IEEE1394:
            return core::AdapterType::Other;
        default:
            // Windows also reports modem-family types that have no constant in
            // every SDK; these are the two that matter for dial-up hardware.
            if (if_type == 23 /* IF_TYPE_PPP over an ISDN/modem port */ ||
                if_type == 108 /* IF_TYPE_PPP_MULTILINK_BUNDLE */) {
                return core::AdapterType::Modem;
            }
            return core::AdapterType::Other;
    }
}

/// GetAdaptersAddresses wants a buffer it can size itself; 16 KB covers a
/// typical machine and the loop grows it when it does not.
std::vector<core::NetworkAdapter> enumerate_interfaces() {
    std::vector<core::NetworkAdapter> adapters;

    ULONG size = 16 * 1024;
    std::unique_ptr<std::byte[]> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer = std::make_unique<std::byte[]>(size);
        result = GetAdaptersAddresses(
            AF_UNSPEC,
            // Skips are for cost: none of the address families are wanted, and
            // the DNS lookup would be a round trip per call.
            //
            // Deliberately NOT GAA_FLAG_INCLUDE_ALL_INTERFACES. That returns
            // every NDIS interface, which means one extra entry per *filter
            // driver bound to each card* -- "…-QoS Packet Scheduler-0000",
            // "…-WFP Native MAC Layer LightWeight Filter-0000", one per Npcap
            // binding. On a developer machine that turned 12 adapters into 40,
            // none of the extras being a thing anyone would call an adapter.
            GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.get()), &size);
    }
    if (result != NO_ERROR) {
        return adapters;
    }

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.get()); adapter != nullptr;
         adapter = adapter->Next) {
        core::NetworkAdapter entry;
        // AdapterName is the GUID; FriendlyName is what Network Connections
        // shows. The GUID is the stable identity, so it is the name and the
        // friendly one joins the description.
        entry.name = adapter->AdapterName != nullptr ? std::string(adapter->AdapterName) : "";
        entry.description = narrow(adapter->Description);
        const std::string friendly = narrow(adapter->FriendlyName);
        if (!friendly.empty() && friendly != entry.description) {
            entry.description = friendly + " (" + entry.description + ")";
        }
        entry.type = map_if_type(adapter->IfType);
        entry.connected = adapter->OperStatus == IfOperStatusUp;
        adapters.push_back(std::move(entry));
    }
    return adapters;
}

/// The entry's own type, which RasEnumEntries does not report -- it hands back
/// names and little else, so each entry has to be asked individually. Without
/// this every phonebook entry would be labelled a modem, and a VPN is not one.
core::AdapterType ras_entry_type(const wchar_t* phonebook, const wchar_t* entry_name) {
    RASENTRYW entry{};
    entry.dwSize = sizeof(RASENTRYW);
    DWORD entry_size = sizeof(RASENTRYW);
    if (RasGetEntryPropertiesW(phonebook, entry_name, &entry, &entry_size, nullptr, nullptr) !=
        ERROR_SUCCESS) {
        return core::AdapterType::Modem;  // it is in a phonebook; assume dial-up
    }
    switch (entry.dwType) {
        case RASET_Phone:     return core::AdapterType::Modem;
        case RASET_Vpn:       return core::AdapterType::Tunnel;
        case RASET_Broadband: return core::AdapterType::Ppp;
        default:              return core::AdapterType::Other;
    }
}

}  // namespace

namespace windows_detail {

std::vector<core::NetworkAdapter> enumerate_ras_entries(const wchar_t* phonebook) {
    std::vector<core::NetworkAdapter> adapters;

    // Which entries are dialled right now, so the rest can be marked down.
    std::vector<std::wstring> connected;
    {
        std::vector<RASCONNW> connections(8);
        connections[0].dwSize = sizeof(RASCONNW);
        DWORD bytes = static_cast<DWORD>(connections.size() * sizeof(RASCONNW));
        DWORD count = 0;
        DWORD status = RasEnumConnectionsW(connections.data(), &bytes, &count);
        if (status == ERROR_BUFFER_TOO_SMALL) {
            connections.assign(bytes / sizeof(RASCONNW) + 1, RASCONNW{});
            connections[0].dwSize = sizeof(RASCONNW);
            bytes = static_cast<DWORD>(connections.size() * sizeof(RASCONNW));
            status = RasEnumConnectionsW(connections.data(), &bytes, &count);
        }
        if (status == ERROR_SUCCESS) {
            for (DWORD i = 0; i < count && i < connections.size(); ++i) {
                connected.emplace_back(connections[i].szEntryName);
            }
        }
    }

    std::vector<RASENTRYNAMEW> entries(16);
    entries[0].dwSize = sizeof(RASENTRYNAMEW);
    DWORD bytes = static_cast<DWORD>(entries.size() * sizeof(RASENTRYNAMEW));
    DWORD count = 0;
    DWORD status = RasEnumEntriesW(nullptr, phonebook, entries.data(), &bytes, &count);
    if (status == ERROR_BUFFER_TOO_SMALL) {
        entries.assign(bytes / sizeof(RASENTRYNAMEW) + 1, RASENTRYNAMEW{});
        entries[0].dwSize = sizeof(RASENTRYNAMEW);
        bytes = static_cast<DWORD>(entries.size() * sizeof(RASENTRYNAMEW));
        status = RasEnumEntriesW(nullptr, phonebook, entries.data(), &bytes, &count);
    }
    // ERROR_SUCCESS with count 0 is the normal "no phonebook entries" answer;
    // anything else means RAS could not be asked, which is not worth failing
    // the whole sample over -- the interface list is still good.
    if (status != ERROR_SUCCESS) {
        return adapters;
    }

    for (DWORD i = 0; i < count && i < entries.size(); ++i) {
        const std::wstring entry_name(entries[i].szEntryName);
        core::NetworkAdapter entry;
        entry.name = narrow(entry_name.c_str());
        entry.description = "RAS entry " + entry.name;
        entry.type = ras_entry_type(phonebook, entry_name.c_str());
        entry.connected = std::ranges::find(connected, entry_name) != connected.end();
        adapters.push_back(std::move(entry));
    }
    return adapters;
}

}  // namespace windows_detail

namespace {

class WindowsNetworkProbe : public INetworkProbe {
public:
    std::vector<core::NetworkAdapter> enumerate() override {
        std::vector<core::NetworkAdapter> adapters = enumerate_interfaces();

        // A dialled RAS entry appears in both enumerations -- once as a live
        // PPP interface, once as a phonebook entry. Keep the interface, which
        // carries the real link state, and add only entries with no interface
        // behind them. Matching is on the entry name, which is what Windows
        // uses for the interface's friendly name while it is connected.
        for (core::NetworkAdapter& ras : windows_detail::enumerate_ras_entries(nullptr)) {
            const bool already_listed =
                std::ranges::any_of(adapters, [&](const core::NetworkAdapter& existing) {
                    return existing.description.find(ras.name) != std::string::npos;
                });
            if (!already_listed) {
                adapters.push_back(std::move(ras));
            }
        }
        return adapters;
    }
};

}  // namespace

std::unique_ptr<INetworkProbe> make_network_probe() {
    return std::make_unique<WindowsNetworkProbe>();
}

}  // namespace lm::platform
