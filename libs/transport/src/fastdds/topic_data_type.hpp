#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <utility>

#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/InstanceHandle.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fastdds/utils/md5.hpp>

#include "lm/transport/codec.hpp"

namespace lm::transport {
namespace detail {

/// A malformed/truncated payload is expected input from any host on the
/// network (Task 10's decode() is hardened specifically for this), so it is
/// logged here -- at the point of rejection, with the offending type name --
/// rather than treated as exceptional.
inline void log_decode_rejected(const std::string& type_name, std::size_t payload_length) {
    std::cerr << "[lm::transport::fastdds] " << type_name << ": rejected a " << payload_length
              << "-byte payload that failed to decode\n";
}

}  // namespace detail

/// Adapts a message type to Fast DDS by forwarding to the free-function codec
/// (Task 10). Every override here is a thin forwarder; no message-specific
/// logic belongs in this class.
///
/// Keyed is true for the three topics keyed by host id (ClientAnnounce,
/// ResourceSample, ComplianceReport). TemplateBundle is the only Keyed=false
/// instantiation, so the `key_of(message)` call below is only ever
/// instantiated for message types that actually have an overload -- the
/// `if constexpr` keeps the Keyed=false branch from needing one.
template <typename Message, bool Keyed>
class MessageTopicDataType : public eprosima::fastdds::dds::TopicDataType {
public:
    explicit MessageTopicDataType(std::string type_name) {
        set_name(std::move(type_name));
        is_compute_key_provided = Keyed;
        // All four messages carry unbounded fields (std::string / std::vector),
        // so is_bounded() correctly stays false (the base class default) and
        // max_serialized_type_size would documentedly be 0 ("use 0" per
        // TopicDataType.hpp's own comment on that member). In practice,
        // though, Fast DDS 3.4.1's writer/reader history setup
        // (DataWriterHistory::to_history_attributes /
        // TopicPayloadPool::get(), see rtps/history/TopicPayloadPool.cpp)
        // feeds this value straight into the payload pool's initial size and
        // returns a *null* pool whenever it is exactly 0 -- regardless of
        // memory policy -- which the caller then dereferences unchecked,
        // crashing create_datawriter/create_datareader on the very first
        // call. A default-constructed message's serialised size is a real,
        // honest lower bound for the type (never a lie about its true
        // unboundedness, since is_bounded() is what callers are supposed to
        // check), and it is always > 0 for these four messages, so it is
        // used here purely as that required nonzero seed.
        max_serialized_type_size = static_cast<std::uint32_t>(encode(Message{}).size());
    }

    bool serialize(const void* const data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        const auto bytes = encode(*static_cast<const Message*>(data));
        if (bytes.size() > payload.max_size) {
            return false;
        }
        if (!bytes.empty()) {
            std::memcpy(payload.data, bytes.data(), bytes.size());
        }
        payload.length = static_cast<std::uint32_t>(bytes.size());
        return true;
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* message = static_cast<Message*>(data);
        const std::span<const std::uint8_t> bytes(payload.data, payload.length);
        // decode() never throws and reports malformed/truncated input by
        // returning false; the caller (Fast DDS) is expected to treat that as
        // a rejected sample rather than something to propagate as an
        // exception. See fast_dds_transport.cpp's reader listeners for how
        // that rejection is drained (continue, not break) without wedging
        // the reader on later, well-formed samples.
        if (decode(bytes, *message)) {
            return true;
        }
        detail::log_decode_rejected(get_name(), bytes.size());
        return false;
    }

    std::uint32_t calculate_serialized_size(
        const void* const data,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        return static_cast<std::uint32_t>(encode(*static_cast<const Message*>(data)).size());
    }

    void* create_data() override { return new Message(); }

    void delete_data(void* data) override { delete static_cast<Message*>(data); }

    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t& payload,
                      eprosima::fastdds::rtps::InstanceHandle_t& handle, bool force_md5) override {
        if constexpr (!Keyed) {
            (void)payload;
            (void)handle;
            (void)force_md5;
            return false;
        } else {
            Message message;
            const std::span<const std::uint8_t> bytes(payload.data, payload.length);
            if (!decode(bytes, message)) {
                return false;
            }
            return hash_key(message, handle);
        }
    }

    bool compute_key(const void* const data, eprosima::fastdds::rtps::InstanceHandle_t& handle,
                      bool force_md5) override {
        (void)force_md5;  // Keys are always MD5-hashed here regardless of length.
        if constexpr (!Keyed) {
            (void)data;
            (void)handle;
            return false;
        } else {
            return hash_key(*static_cast<const Message*>(data), handle);
        }
    }

private:
    /// Hashes key_of(message) into the 16-byte instance handle with MD5, the
    /// same algorithm Fast DDS's own generated types use for keys wider than
    /// 16 bytes. Only instantiated when Keyed is true, so message types
    /// without a key_of() overload (TemplateBundle) never need one.
    static bool hash_key(const Message& message, eprosima::fastdds::rtps::InstanceHandle_t& handle) {
        const std::string key = key_of(message);
        eprosima::fastdds::MD5 md5;
        md5.update(key.c_str(), static_cast<eprosima::fastdds::MD5::size_type>(key.size()));
        md5.finalize();
        for (std::size_t i = 0; i < 16; ++i) {
            handle.value[i] = md5.digest[i];
        }
        return true;
    }
};

}  // namespace lm::transport
