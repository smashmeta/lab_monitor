#include "lm/transport/codec.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <fastcdr/exceptions/Exception.h>

namespace lm::transport {
namespace {

using eprosima::fastcdr::Cdr;
using eprosima::fastcdr::FastBuffer;

/// Serialises with a callable that writes into a Cdr, returning the bytes.
template <typename Body>
std::vector<std::uint8_t> serialise(Body&& body) {
    FastBuffer buffer;
    Cdr writer(buffer);
    std::forward<Body>(body)(writer);
    const auto length = writer.get_serialized_data_length();
    const auto* data = reinterpret_cast<const std::uint8_t*>(buffer.getBuffer());
    return std::vector<std::uint8_t>(data, data + length);
}

/// Deserialises with a callable that reads from a Cdr. Returns false if the
/// payload is empty or FastCDR reports it ran off the end.
template <typename Body>
bool deserialise(std::span<const std::uint8_t> bytes, Body&& body) {
    if (bytes.empty()) {
        return false;
    }
    try {
        FastBuffer buffer(const_cast<char*>(reinterpret_cast<const char*>(bytes.data())),
                          bytes.size());
        Cdr reader(buffer);
        std::forward<Body>(body)(reader);
        return true;
    } catch (const eprosima::fastcdr::exception::Exception&) {
        // FastCDR's own exception hierarchy (e.g. NotEnoughMemoryException on a
        // truncated buffer). Note: in the installed FastCDR version this class
        // does NOT derive from std::exception, so it needs its own catch clause
        // in addition to the one below.
        return false;
    } catch (const std::exception&) {
        // Defence in depth: an unvalidated element count read off the wire (see
        // the bounded reserve()/push_back() loops below) must never be able to
        // turn into an escaping std::bad_alloc or std::length_error. Any such
        // failure becomes a clean `false` instead of taking down the caller.
        return false;
    }
}

void write_disk(Cdr& writer, const core::DiskUsage& disk) {
    writer << disk.mount << disk.total_bytes << disk.free_bytes;
}

void read_disk(Cdr& reader, core::DiskUsage& disk) {
    reader >> disk.mount >> disk.total_bytes >> disk.free_bytes;
}

void write_adapter(Cdr& writer, const core::NetworkAdapter& adapter) {
    writer << adapter.name << adapter.description << adapter.id
           << static_cast<std::uint8_t>(adapter.type) << static_cast<std::uint8_t>(adapter.link);
}

bool read_adapter(Cdr& reader, core::NetworkAdapter& adapter) {
    std::uint8_t type = 0;
    std::uint8_t link = 0;
    reader >> adapter.name >> adapter.description >> adapter.id >> type >> link;
    // Same rule as read_result's status: an enum from the wire is an integer
    // until it has been checked to be one of ours.
    if (type > static_cast<std::uint8_t>(core::AdapterType::Other) ||
        link > static_cast<std::uint8_t>(core::LinkState::Faulted)) {
        return false;
    }
    adapter.type = static_cast<core::AdapterType>(type);
    adapter.link = static_cast<core::LinkState>(link);
    return true;
}

void write_result(Cdr& writer, const core::CheckResult& result) {
    writer << result.rule_id << static_cast<std::uint8_t>(result.status) << result.observed
           << result.message;
}

bool read_result(Cdr& reader, core::CheckResult& result) {
    std::uint8_t status = 0;
    reader >> result.rule_id >> status >> result.observed >> result.message;
    if (status > static_cast<std::uint8_t>(core::CheckStatus::Error)) {
        return false;
    }
    result.status = static_cast<core::CheckStatus>(status);
    return true;
}

}  // namespace

// --- ClientAnnounce --------------------------------------------------------

std::vector<std::uint8_t> encode(const ClientAnnounce& message) {
    return serialise([&](Cdr& writer) {
        writer << message.host_id << message.agent_version << message.capabilities
               << message.paused;
    });
}

bool decode(std::span<const std::uint8_t> bytes, ClientAnnounce& out) {
    ClientAnnounce parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.host_id >> parsed.agent_version >> parsed.capabilities;
        // The paused flag was appended to this message after the three fields
        // shipped, so a client built before it sends a buffer that ends here.
        // Reading it in its own guard makes that a false rather than a failed
        // decode -- and a failed decode would drop the announce outright, which
        // is the only carrier of a client's capabilities, so an older agent
        // would disappear from the fleet with nothing on screen to say why.
        // A wrong pause flag is a far smaller wrong than a missing machine.
        try {
            reader >> parsed.paused;
        } catch (const eprosima::fastcdr::exception::Exception&) {
            parsed.paused = false;
        } catch (const std::exception&) {
            parsed.paused = false;
        }
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
}

// --- ResourceSampleMessage -------------------------------------------------

std::vector<std::uint8_t> encode(const ResourceSampleMessage& message) {
    return serialise([&](Cdr& writer) {
        writer << message.host_id << message.sample.cpu_percent << message.sample.mem_total_bytes
               << message.sample.mem_used_bytes
               << static_cast<std::uint32_t>(message.sample.disks.size());
        for (const core::DiskUsage& disk : message.sample.disks) {
            write_disk(writer, disk);
        }
        writer << static_cast<std::uint32_t>(message.sample.adapters.size());
        for (const core::NetworkAdapter& adapter : message.sample.adapters) {
            write_adapter(writer, adapter);
        }
    });
}

bool decode(std::span<const std::uint8_t> bytes, ResourceSampleMessage& out) {
    ResourceSampleMessage parsed;
    bool adapters_valid = true;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        std::uint32_t disk_count = 0;
        reader >> parsed.host_id >> parsed.sample.cpu_percent >> parsed.sample.mem_total_bytes >>
            parsed.sample.mem_used_bytes >> disk_count;
        // A corrupt count must never drive the allocation directly: reserve only
        // a bounded amount up front. If the payload genuinely holds that many
        // elements the vector grows normally as they are read; if it does not,
        // FastCDR throws on the first missing element and deserialise() turns
        // that into a clean `false` instead of an escaping std::bad_alloc.
        constexpr std::uint32_t kReserveCap = 1024;
        parsed.sample.disks.reserve(std::min(disk_count, kReserveCap));
        for (std::uint32_t i = 0; i < disk_count; ++i) {
            core::DiskUsage disk;
            read_disk(reader, disk);
            parsed.sample.disks.push_back(std::move(disk));
        }

        std::uint32_t adapter_count = 0;
        reader >> adapter_count;
        parsed.sample.adapters.reserve(std::min(adapter_count, kReserveCap));
        for (std::uint32_t i = 0; i < adapter_count; ++i) {
            core::NetworkAdapter adapter;
            if (!read_adapter(reader, adapter)) {
                adapters_valid = false;
            }
            parsed.sample.adapters.push_back(std::move(adapter));
        }
    });
    if (ok && adapters_valid) {
        out = std::move(parsed);
        return true;
    }
    return false;
}

// --- TemplateBundleMessage -------------------------------------------------

std::vector<std::uint8_t> encode(const TemplateBundleMessage& message) {
    return serialise(
        [&](Cdr& writer) { writer << message.revision << message.hash << message.json; });
}

bool decode(std::span<const std::uint8_t> bytes, TemplateBundleMessage& out) {
    TemplateBundleMessage parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.revision >> parsed.hash >> parsed.json;
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
}

// --- ComplianceReportMessage -----------------------------------------------

std::vector<std::uint8_t> encode(const ComplianceReportMessage& message) {
    return serialise([&](Cdr& writer) {
        writer << message.report.host_id << message.report.applied_revision
               << static_cast<std::uint32_t>(message.report.results.size());
        for (const core::CheckResult& result : message.report.results) {
            write_result(writer, result);
        }
    });
}

bool decode(std::span<const std::uint8_t> bytes, ComplianceReportMessage& out) {
    ComplianceReportMessage parsed;
    bool statuses_valid = true;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        std::uint32_t count = 0;
        reader >> parsed.report.host_id >> parsed.report.applied_revision >> count;
        // Same reasoning as the disk-count loop above: never allocate directly
        // off an unvalidated wire count.
        constexpr std::uint32_t kReserveCap = 1024;
        parsed.report.results.reserve(std::min(count, kReserveCap));
        for (std::uint32_t i = 0; i < count; ++i) {
            core::CheckResult result;
            if (!read_result(reader, result)) {
                statuses_valid = false;
            }
            parsed.report.results.push_back(std::move(result));
        }
    });
    if (ok && statuses_valid) {
        out = std::move(parsed);
        return true;
    }
    return false;
}

// --- keys ------------------------------------------------------------------

std::string key_of(const ClientAnnounce& message) { return message.host_id; }
std::string key_of(const ResourceSampleMessage& message) { return message.host_id; }
std::string key_of(const ComplianceReportMessage& message) { return message.report.host_id; }

}  // namespace lm::transport
