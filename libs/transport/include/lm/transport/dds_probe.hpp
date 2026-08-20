#pragma once

#include <chrono>
#include <memory>

#include "lm/platform/probes.hpp"

namespace lm::transport {

struct DdsProbeConfig {
    /// How long to wait for a writer to be discovered before concluding the
    /// topic is not on the bus. Discovery is not instantaneous, so a zero wait
    /// would report every topic as absent.
    std::chrono::milliseconds discovery_wait{2000};
    /// How long to wait for a sample once the topic *has* been found. Separate
    /// from the above because the two mean different things: no writer is a
    /// finding, no sample from a known writer is a check that could not be
    /// answered.
    std::chrono::milliseconds sample_wait{2000};
    /// How long to keep asking the registry for the type description after a
    /// writer has been discovered. The TypeLookup service fetches it from the
    /// remote participant asynchronously, so the first lookup routinely misses.
    std::chrono::milliseconds type_lookup_wait{2000};
};

/// A probe that reads a topic off an arbitrary DDS domain without being given
/// its IDL.
///
/// The type is rebuilt at runtime from what the publisher advertises in
/// discovery (XTypes TypeObject), so the only thing a rule has to name is the
/// domain, the topic, and a path into the data. The catch is that this depends
/// on the publisher actually advertising it: a writer that propagates only a
/// type *name* cannot be read this way, and the probe says so rather than
/// pretending the topic is empty.
///
/// Lives in lm_transport because that is where Fast DDS is already linked;
/// implements an interface declared in lm_platform, which stays DDS-free.
[[nodiscard]] std::unique_ptr<platform::IDdsProbe> make_dds_probe(DdsProbeConfig config = {});

}  // namespace lm::transport
