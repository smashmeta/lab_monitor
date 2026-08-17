#pragma once

#include <vector>

#include "lm/core/host_facts.hpp"

/// Windows-only internals of the network probe, exposed for testing.
///
/// The RAS half is the part worth testing on its own: a machine that happens to
/// have no dial-up or VPN entries configured exercises none of it through the
/// public probe, and creating entries in the user's real phonebook to fix that
/// would be changing their network configuration. Both functions here take a
/// phonebook path so a test can point them at a temporary one.
namespace lm::platform::windows_detail {

/// Dial-up, VPN and broadband entries from a RAS phonebook, each typed from its
/// own properties and marked connected only if it is currently dialled.
///
/// `phonebook` is a .pbk path, or nullptr for the user's default phonebook.
[[nodiscard]] std::vector<core::NetworkAdapter> enumerate_ras_entries(const wchar_t* phonebook);

}  // namespace lm::platform::windows_detail
