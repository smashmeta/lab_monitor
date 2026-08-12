#include "lm/transport/codec.hpp"

#include <cstdint>
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
        return false;
    }
}

void write_disk(Cdr& writer, const core::DiskUsage& disk) {
    writer << disk.mount << disk.total_bytes << disk.free_bytes;
}

void read_disk(Cdr& reader, core::DiskUsage& disk) {
    reader >> disk.mount >> disk.total_bytes >> disk.free_bytes;
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
        writer << message.host_id << message.agent_version << message.capabilities;
    });
}

bool decode(std::span<const std::uint8_t> bytes, ClientAnnounce& out) {
    ClientAnnounce parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        reader >> parsed.host_id >> parsed.agent_version >> parsed.capabilities;
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
    });
}

bool decode(std::span<const std::uint8_t> bytes, ResourceSampleMessage& out) {
    ResourceSampleMessage parsed;
    const bool ok = deserialise(bytes, [&](Cdr& reader) {
        std::uint32_t disk_count = 0;
        reader >> parsed.host_id >> parsed.sample.cpu_percent >> parsed.sample.mem_total_bytes >>
            parsed.sample.mem_used_bytes >> disk_count;
        parsed.sample.disks.resize(disk_count);
        for (core::DiskUsage& disk : parsed.sample.disks) {
            read_disk(reader, disk);
        }
    });
    if (ok) {
        out = std::move(parsed);
    }
    return ok;
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
        parsed.report.results.resize(count);
        for (core::CheckResult& result : parsed.report.results) {
            if (!read_result(reader, result)) {
                statuses_valid = false;
            }
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
