#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace lm::core {

using HostId = std::string;
using RuleId = std::string;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

enum class RuleKind { Process, Service, Registry };
enum class Presence { MustBePresent, MustBeAbsent };
enum class CheckStatus { Pass, Fail, NotApplicable, Error };
enum class ServiceState { Running, Stopped, Unknown };

enum class Capability : std::uint32_t {
    Resources = 1u << 0,
    Processes = 1u << 1,
    Services  = 1u << 2,
    Registry  = 1u << 3,
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

/// The capabilities of the platform this binary was compiled for.
[[nodiscard]] Capabilities platform_capabilities();

/// Maps a rule kind onto the capability required to evaluate it.
[[nodiscard]] Capability required_capability(RuleKind kind);

}  // namespace lm::core
