#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace lm::core {

using HostId = std::string;
using RuleId = std::string;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class RuleKind { Process, Service, Registry, Network };

/// How an expected count is compared against the observed one.
enum class Comparison { AtLeast, Exactly, AtMost };
enum class Presence { MustBePresent, MustBeAbsent };
enum class CheckStatus { Pass, Fail, NotApplicable, Error };
enum class ServiceState { Running, Stopped, Unknown };

/// How a network adapter attaches, coarse enough to be worth reading at a
/// glance. Anything a platform reports that does not map here becomes `Other`,
/// which is distinct from `Unknown`: Other means "we saw a type we do not
/// name", Unknown means "the platform did not say".
enum class AdapterType { Unknown, Ethernet, WiFi, Loopback, Ppp, Tunnel, Modem, Other };

/// What an adapter's link is actually doing. A boolean cannot say the useful
/// part: an Ethernet port with nothing plugged into it, a card the operator
/// switched off, and a dial-up entry sitting idle are three different
/// situations needing three different responses, and all three are "not
/// connected". Windows draws the middle one with a red cross for that reason.
enum class LinkState {
    Unknown,       ///< the platform did not say
    Connected,     ///< up and carrying traffic
    NoMedia,       ///< enabled, but nothing plugged in — the red cross
    Disconnected,  ///< defined and idle; a dial-up entry that is not dialled
    Connecting,    ///< negotiating, authenticating, or on the way down
    Disabled,      ///< switched off, by the operator or by policy
    Faulted,       ///< absent, malfunctioning, or refusing to authenticate
};

/// True only for LinkState::Connected. Named rather than compared inline so
/// "how many are up" has one definition.
[[nodiscard]] constexpr bool is_up(LinkState state) { return state == LinkState::Connected; }

enum class Capability : std::uint32_t {
    Resources = 1u << 0,
    Processes = 1u << 1,
    Services  = 1u << 2,
    Registry  = 1u << 3,
    /// Network adapter inventory. A client without it reports no adapters,
    /// which the server must not read as "this machine has none".
    Network   = 1u << 4,
};

/// A set of capabilities a client advertises. Rules whose required capability
/// is absent evaluate to CheckStatus::NotApplicable rather than Pass or Fail.
class Capabilities {
public:
    Capabilities() = default;
    explicit Capabilities(std::uint32_t raw) : raw_(raw) {}

    Capabilities& add(Capability c) {
        raw_ |= static_cast<std::uint32_t>(c);
        return *this;
    }

    [[nodiscard]] bool has(Capability c) const {
        return (raw_ & static_cast<std::uint32_t>(c)) != 0u;
    }

    [[nodiscard]] std::uint32_t raw() const { return raw_; }

    friend bool operator==(const Capabilities&, const Capabilities&) = default;

private:
    std::uint32_t raw_ = 0;
};

/// Human-readable name of a single capability, used when reporting that a rule
/// could not be checked because the client does not provide that data.
[[nodiscard]] std::string to_string(Capability capability);

[[nodiscard]] std::string to_string(AdapterType type);
[[nodiscard]] std::string to_string(LinkState state);
/// Reads as part of a sentence: "at least 2 connected".
[[nodiscard]] std::string to_string(Comparison comparison);
[[nodiscard]] bool satisfies(int observed, Comparison comparison, int expected);

/// The capabilities of the platform this binary was compiled for.
[[nodiscard]] Capabilities platform_capabilities();

/// Maps a rule kind onto the capability required to evaluate it.
[[nodiscard]] Capability required_capability(RuleKind kind);

}  // namespace lm::core
