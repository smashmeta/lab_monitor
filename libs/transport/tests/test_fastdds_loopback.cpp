#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/rtps/common/InstanceHandle.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>

#include "lm/transport/fast_dds_transport.hpp"

using namespace lm::core;
using namespace lm::transport;
using namespace std::chrono_literals;

namespace {

/// Domain 42 keeps this test off the default domain other tooling might use.
DdsConfig loopback_config() {
    DdsConfig config;
    config.domain_id = 42;
    return config;
}

/// Polls until the predicate holds or the timeout expires. Returns whether it held.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return predicate();
}

// --- malformed-payload harness -------------------------------------------
//
// fast_dds_transport.hpp deliberately exposes no way to inject raw bytes
// through DdsClientTransport/DdsServerTransport (Tasks 13/14 bind to that
// header's shape, and it stays untouched here). To put a genuinely
// undecodable payload on the wire the way a hostile or buggy host on the
// network would, this harness talks to Fast DDS directly: a second,
// independent DomainParticipant with its own DataWriter, registered under
// the *same* topic name and type name DdsServerTransport uses internally for
// ResourceSampleMessage so discovery matches it against the real server's
// reader, but backed by a TopicDataType whose serialize() always emits a
// fixed, too-short-to-be-valid-CDR payload.
//
// "lm.transport.ResourceSample" / "lm::transport::ResourceSampleMessage" are
// private constants inside fast_dds_transport.cpp's anonymous namespace
// (kResourceTopicName / kResourceTypeName); they are duplicated here rather
// than exposed through the header, since topic/type naming is exactly the
// kind of internal detail that header must not grow just for a test.
constexpr const char* kResourceTopicName = "lm.transport.ResourceSample";
constexpr const char* kResourceTypeName = "lm::transport::ResourceSampleMessage";

/// Always serialises to two fixed bytes -- far too short to contain even the
/// length prefix of ResourceSampleMessage::host_id, let alone the rest of
/// the struct, so lm::transport::decode() is guaranteed to reject it.
class GarbageResourceSampleType : public eprosima::fastdds::dds::TopicDataType {
public:
    GarbageResourceSampleType() {
        set_name(kResourceTypeName);
        is_compute_key_provided = true;
        // Same nonzero-seed requirement as MessageTopicDataType (see
        // topic_data_type.hpp) -- Fast DDS 3.4.1 crashes create_datawriter
        // when this is left at 0, regardless of what serialize() does.
        max_serialized_type_size = static_cast<std::uint32_t>(kGarbageBytes.size());
    }

    bool serialize(const void* const /*data*/, eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        std::memcpy(payload.data, kGarbageBytes.data(), kGarbageBytes.size());
        payload.length = static_cast<std::uint32_t>(kGarbageBytes.size());
        return true;
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& /*payload*/, void* /*data*/) override {
        return false;
    }

    std::uint32_t calculate_serialized_size(
        const void* const /*data*/, eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        return static_cast<std::uint32_t>(kGarbageBytes.size());
    }

    void* create_data() override { return new char; }
    void delete_data(void* data) override { delete static_cast<char*>(data); }

    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t& /*payload*/,
                      eprosima::fastdds::rtps::InstanceHandle_t& handle, bool /*force_md5*/) override {
        return fixed_key(handle);
    }
    bool compute_key(const void* const /*data*/, eprosima::fastdds::rtps::InstanceHandle_t& handle,
                      bool /*force_md5*/) override {
        return fixed_key(handle);
    }

private:
    static bool fixed_key(eprosima::fastdds::rtps::InstanceHandle_t& handle) {
        for (std::size_t i = 0; i < 16; ++i) {
            handle.value[i] = static_cast<unsigned char>(i);
        }
        return true;
    }

    static constexpr std::array<std::uint8_t, 2> kGarbageBytes{0xDE, 0xAD};
};

/// Owns a second, independent participant/publisher/topic/writer purely to
/// publish malformed ResourceSampleMessage payloads onto the loopback
/// domain. RAII-cleaned up the same way DdsClientTransport/DdsServerTransport
/// are (delete_contained_entities() then delete_participant()).
class GarbageResourceSampleWriter {
public:
    explicit GarbageResourceSampleWriter(int domain_id) {
        factory_ = eprosima::fastdds::dds::DomainParticipantFactory::get_shared_instance();
        participant_ = factory_->create_participant(static_cast<eprosima::fastdds::dds::DomainId_t>(domain_id),
                                                      eprosima::fastdds::dds::PARTICIPANT_QOS_DEFAULT);
        if (participant_ == nullptr) {
            ADD_FAILURE() << "failed to create the garbage-writer participant";
            return;
        }
        publisher_ = participant_->create_publisher(eprosima::fastdds::dds::PUBLISHER_QOS_DEFAULT);
        eprosima::fastdds::dds::TypeSupport type_support(new GarbageResourceSampleType());
        if (participant_->register_type(type_support) != eprosima::fastdds::dds::RETCODE_OK) {
            ADD_FAILURE() << "failed to register the garbage TopicDataType";
            return;
        }
        topic_ = participant_->create_topic(kResourceTopicName, kResourceTypeName,
                                             eprosima::fastdds::dds::TOPIC_QOS_DEFAULT);
        eprosima::fastdds::dds::DataWriterQos qos = eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT;
        // Matches best_effort_volatile_writer_qos() in fast_dds_transport.cpp
        // so this writer is RxO-compatible with the real ResourceSample reader.
        qos.reliability().kind = eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
        qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
        qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
        qos.history().depth = 1;
        writer_ = publisher_ != nullptr && topic_ != nullptr
                      ? publisher_->create_datawriter(topic_, qos)
                      : nullptr;
    }

    ~GarbageResourceSampleWriter() {
        if (participant_ != nullptr) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
        }
    }

    GarbageResourceSampleWriter(const GarbageResourceSampleWriter&) = delete;
    GarbageResourceSampleWriter& operator=(const GarbageResourceSampleWriter&) = delete;

    [[nodiscard]] bool ready() const { return writer_ != nullptr; }

    [[nodiscard]] bool matched() const {
        if (writer_ == nullptr) {
            return false;
        }
        eprosima::fastdds::dds::PublicationMatchedStatus status;
        (void)writer_->get_publication_matched_status(status);
        return status.current_count > 0;
    }

    void publish_garbage() {
        char dummy = 0;
        (void)writer_->write(&dummy);
    }

private:
    std::shared_ptr<eprosima::fastdds::dds::DomainParticipantFactory> factory_;
    eprosima::fastdds::dds::DomainParticipant* participant_ = nullptr;
    eprosima::fastdds::dds::Publisher* publisher_ = nullptr;
    eprosima::fastdds::dds::Topic* topic_ = nullptr;
    eprosima::fastdds::dds::DataWriter* writer_ = nullptr;
};

}  // namespace

TEST(FastDdsLoopback, ServerDiscoversAnAnnouncingClient) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<bool> seen{false};
    server->on_announce([&](const ClientAnnounce& message) {
        if (message.host_id == "LOOPBACK-PC") {
            seen = true;
        }
    });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    ClientAnnounce announce;
    announce.host_id = "LOOPBACK-PC";
    announce.agent_version = "0.1.0";
    announce.capabilities = platform_capabilities().raw();
    client->publish_announce(announce);

    EXPECT_TRUE(wait_for([&] { return seen.load(); }));
}

TEST(FastDdsLoopback, ResourceSamplesReachTheServer) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<int> received{0};
    server->on_resources([&](const ResourceSampleMessage&) { ++received; });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    ResourceSampleMessage sample;
    sample.host_id = "LOOPBACK-PC";
    sample.sample.cpu_percent = 12.5;

    // ResourceSampleMessage travels BEST_EFFORT/VOLATILE (see
    // best_effort_volatile_writer_qos() in fast_dds_transport.cpp): there is
    // no retained history and no ACK/retry, so a sample published before
    // this specific writer/reader pair -- not just the participant-level
    // match client->state() reports above -- has finished discovery is
    // dropped for good. Publishing exactly once therefore lost this race
    // against discovery roughly 1 run in 4. Publish repeatedly, on every
    // poll of wait_for's predicate, until the server actually reports one
    // or the timeout expires -- a known-flaky test is worse than none.
    EXPECT_TRUE(wait_for([&] {
        client->publish_resources(sample);
        return received.load() > 0;
    }));
}

TEST(FastDdsLoopback, LateJoiningClientReceivesTheRetainedBundle) {
    const auto server = make_dds_server(loopback_config());

    TemplateBundleMessage bundle;
    bundle.revision = 5;
    bundle.hash = "abc";
    bundle.json = R"({"revision":5,"hash":"abc","baseline":{"name":"b","rules":[]},
                      "templates":[],"assignments":{}})";
    server->publish_bundle(bundle);

    // Client created after the publish — TransientLocal must still deliver it.
    const auto client = make_dds_client(loopback_config());

    std::atomic<std::uint64_t> revision{0};
    client->on_bundle([&](const TemplateBundleMessage& message) { revision = message.revision; });

    EXPECT_TRUE(wait_for([&] { return revision.load() == 5u; }));
}

// Exercises the "any host on the network can send garbage" path the codec
// was hardened for (Task 10) end to end through the real DDS reader: a
// malformed ResourceSampleMessage is published first, from a hand-rolled
// second writer (GarbageResourceSampleWriter), followed by a genuinely valid
// one from the real client transport. If DdsServerTransport's reader drain
// loop wedges on the undecodable sample -- e.g. because take_next_sample()
// does not remove the offending change from reader history on a rejected
// deserialize() -- the valid sample would never arrive either.
TEST(FastDdsLoopback, MalformedSampleDoesNotWedgeLaterValidSamples) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<int> received{0};
    server->on_resources([&](const ResourceSampleMessage& message) {
        if (message.host_id == "LOOPBACK-VALID") {
            ++received;
        }
    });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    GarbageResourceSampleWriter garbage(loopback_config().domain_id);
    ASSERT_TRUE(garbage.ready());
    ASSERT_TRUE(wait_for([&] { return garbage.matched(); }))
        << "garbage writer never matched the server's ResourceSample reader";

    garbage.publish_garbage();
    // No event to poll for a deliberately-rejected sample (that is exactly
    // the point -- it never reaches on_resources), so this is a fixed
    // synchronisation delay rather than a condition wait: it exists purely
    // to make it overwhelmingly likely the malformed sample is processed by
    // the reader before the valid one below is published, on a same-machine
    // loopback domain.
    std::this_thread::sleep_for(200ms);

    ResourceSampleMessage valid;
    valid.host_id = "LOOPBACK-VALID";
    valid.sample.cpu_percent = 42.0;
    client->publish_resources(valid);

    EXPECT_TRUE(wait_for([&] { return received.load() > 0; }))
        << "valid sample never arrived after a malformed one on the same topic -- "
           "the reader appears to wedge on an undecodable payload";
}

TEST(FastDdsLoopback, AScriptCommandReachesTheClientAndItsResultComesBack) {
    // Declared before server/client (and so, by reverse-declaration-order
    // destruction, outlive them): both are captured by reference in handlers
    // that run on the transports' own poll threads. Either publish loop below
    // stops at the *first* arrival while duplicates already in flight can
    // still be delivered afterwards -- including during server/client's own
    // destruction -- so the vectors (and the mutex guarding them, alongside
    // every other test in this file's use of std::atomic for the same
    // cross-thread-read reason) must not be destroyed first.
    std::mutex mutex;
    std::vector<ScriptCommand> commands;
    std::vector<ScriptResultMessage> results;

    DdsConfig config;
    config.domain_id = 71;  // a domain of its own, away from the other tests
    const auto server = make_dds_server(config);
    const auto client = make_dds_client(config);

    client->on_script_command([&](const ScriptCommand& c) {
        const std::lock_guard<std::mutex> lock(mutex);
        commands.push_back(c);
    });
    server->on_script_result([&](const ScriptResultMessage& r) {
        const std::lock_guard<std::mutex> lock(mutex);
        results.push_back(r);
    });

    ScriptCommand command;
    command.host_id = "PC-001";
    command.run_id = "run-1";
    command.script_body = "exit 0";

    // Published in a loop: these topics are Reliable but discovery still races
    // a single publish, which is how FastDdsLoopback.ResourceSamplesReachThe
    // Server was de-flaked.
    for (int i = 0; i < 20; ++i) {
        server->publish_script_command(command);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::lock_guard<std::mutex> lock(mutex);
        if (!commands.empty()) {
            break;
        }
    }
    {
        const std::lock_guard<std::mutex> lock(mutex);
        ASSERT_FALSE(commands.empty()) << "no command arrived";
    }

    ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = "run-1";
    result.status = lm::core::ScriptStatus::Completed;
    for (int i = 0; i < 20; ++i) {
        client->publish_script_result(result);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::lock_guard<std::mutex> lock(mutex);
        if (!results.empty()) {
            break;
        }
    }
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_FALSE(results.empty()) << "no result came back";
    EXPECT_EQ(results.front().run_id, "run-1");
}
