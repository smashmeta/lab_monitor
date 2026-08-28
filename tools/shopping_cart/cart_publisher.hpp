#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "cart_state.hpp"

namespace cart {

/// Publishes the cart on a DDS topic, as a third-party application would.
///
/// Built directly on Fast DDS rather than through `lm_transport`, on purpose:
/// this is the thing being *tested against*, and a fixture that shares the
/// product's transport code proves rather less than one that does not. The
/// only lab_monitor code this tool touches is the Qt theme.
///
/// The type is built at runtime through DynamicTypeBuilderFactory. That is not
/// a shortcut around the missing `fastddsgen` — it is what makes the fixture
/// honest, since the resulting TypeObject is exactly what the probe has to
/// rebuild the type from, with neither side compiled against the other.
class Publisher {
public:
    Publisher();
    ~Publisher();
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// Joins the domain and creates the writer. Returns an empty string on
    /// success, or a message fit to show the operator — a fixture that fails
    /// silently would send someone hunting through the probe for a fault that
    /// is on this side.
    /// Joins the domain and creates the writer. Returns an empty string on
    /// success, or a message describing what went wrong.
    ///
    /// With localhost_only set, the participant is confined to this machine:
    /// every PC running the fixture then has its own cart on the same domain,
    /// so one rule -- "items_.length equal to 2 on domain 42" -- means the
    /// same thing everywhere and each machine answers it about itself. Without
    /// it, several carts on one network are all on one bus and every client
    /// reads whichever it discovers first.
    [[nodiscard]] std::string start(std::uint32_t domain_id, const std::string& topic_name,
                                    bool localhost_only);

    /// Writes the cart. TRANSIENT_LOCAL, so a probe attaching afterwards still
    /// reads the current state rather than waiting for the next edit — which
    /// matters here, since a cart changes when somebody clicks, not on a timer.
    [[nodiscard]] bool publish(const State& state);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cart
