#include <gtest/gtest.h>

#include <windows.h>

#include <ras.h>
#include <raserror.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "windows/network_probe_windows.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

/// A RAS phonebook in a temp directory, so these tests never touch the real one
/// under %APPDATA%. Creating an entry there would be editing the machine's
/// actual network configuration, which a test has no business doing.
class TemporaryPhonebook {
public:
    TemporaryPhonebook() {
        path_ = std::filesystem::temp_directory_path() /
                ("lm_test_phonebook_" + std::to_string(GetCurrentProcessId()) + ".pbk");
        wide_ = path_.wstring();
    }

    ~TemporaryPhonebook() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryPhonebook(const TemporaryPhonebook&) = delete;
    TemporaryPhonebook& operator=(const TemporaryPhonebook&) = delete;

    [[nodiscard]] const wchar_t* path() const { return wide_.c_str(); }

    /// Writes one entry, returning the RAS status so a caller can skip when the
    /// machine will not accept it. `type` is RASET_Phone, RASET_Vpn, ...
    DWORD add_entry(const wchar_t* name, DWORD type, const wchar_t* device_type,
                    const wchar_t* device_name) {
        RASENTRYW entry{};
        entry.dwSize = sizeof(RASENTRYW);
        entry.dwType = type;
        entry.dwFramingProtocol = RASFP_Ppp;
        entry.dwfNetProtocols = RASNP_Ip;
        entry.dwEncryptionType = ET_Optional;
        entry.dwVpnStrategy = VS_Default;
        entry.dwRedialCount = 0;
        wcscpy_s(entry.szLocalPhoneNumber, L"5551234");
        wcscpy_s(entry.szDeviceType, device_type);
        wcscpy_s(entry.szDeviceName, device_name);
        return RasSetEntryPropertiesW(wide_.c_str(), name, &entry, sizeof(entry), nullptr, 0);
    }

private:
    std::filesystem::path path_;
    std::wstring wide_;
};

const NetworkAdapter* find_named(const std::vector<NetworkAdapter>& adapters,
                                 const std::string& name) {
    const auto found = std::ranges::find(adapters, name, &NetworkAdapter::name);
    return found == adapters.end() ? nullptr : &*found;
}

}  // namespace

TEST(RasEntries, AnEmptyPhonebookYieldsNothing) {
    const TemporaryPhonebook phonebook;
    EXPECT_TRUE(windows_detail::enumerate_ras_entries(phonebook.path()).empty());
}

TEST(RasEntries, ReportsADialUpEntryThatIsNotDialled) {
    // The whole point of reading the phonebook: an entry that is not connected
    // has no network interface behind it, so GetAdaptersAddresses cannot see it
    // at all. It still exists, and "defined but down" is worth reporting.
    TemporaryPhonebook phonebook;
    const DWORD status =
        phonebook.add_entry(L"Lab Dialup", RASET_Phone, L"modem", L"Standard 56000 bps Modem");
    if (status != ERROR_SUCCESS) {
        GTEST_SKIP() << "RasSetEntryProperties refused a dial-up entry (status " << status
                     << "); this machine has no modem device to attach one to";
    }

    const std::vector<NetworkAdapter> adapters =
        windows_detail::enumerate_ras_entries(phonebook.path());

    const NetworkAdapter* entry = find_named(adapters, "Lab Dialup");
    ASSERT_NE(entry, nullptr) << "the disconnected entry was not reported";
    EXPECT_EQ(entry->type, AdapterType::Modem);
    EXPECT_FALSE(entry->connected);
    EXPECT_EQ(entry->description, "RAS entry Lab Dialup");
}

TEST(RasEntries, TypesAVpnEntryAsATunnelRatherThanAModem) {
    // RasEnumEntries reports names and little else, so each entry's type has to
    // be read separately. Without that every phonebook entry would be labelled
    // a modem, and a VPN is not one.
    TemporaryPhonebook phonebook;
    const DWORD status =
        phonebook.add_entry(L"Site VPN", RASET_Vpn, L"vpn", L"WAN Miniport (IKEv2)");
    if (status != ERROR_SUCCESS) {
        GTEST_SKIP() << "RasSetEntryProperties refused a VPN entry (status " << status << ")";
    }

    // Bound to a local: find_named returns a pointer into this vector, and a
    // temporary would be gone before the assertions read through it.
    const std::vector<NetworkAdapter> adapters =
        windows_detail::enumerate_ras_entries(phonebook.path());
    const NetworkAdapter* entry = find_named(adapters, "Site VPN");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->type, AdapterType::Tunnel);
    EXPECT_FALSE(entry->connected);
}

TEST(RasEntries, ReportsEveryEntryInThePhonebook) {
    TemporaryPhonebook phonebook;
    if (phonebook.add_entry(L"Entry One", RASET_Vpn, L"vpn", L"WAN Miniport (IKEv2)") !=
            ERROR_SUCCESS ||
        phonebook.add_entry(L"Entry Two", RASET_Vpn, L"vpn", L"WAN Miniport (IKEv2)") !=
            ERROR_SUCCESS) {
        GTEST_SKIP() << "RasSetEntryProperties refused an entry on this machine";
    }

    const std::vector<NetworkAdapter> adapters =
        windows_detail::enumerate_ras_entries(phonebook.path());

    EXPECT_EQ(adapters.size(), 2u);
    EXPECT_NE(find_named(adapters, "Entry One"), nullptr);
    EXPECT_NE(find_named(adapters, "Entry Two"), nullptr);
}
