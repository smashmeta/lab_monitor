#include "lm/transport/fast_dds_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fastdds/dds/core/Types.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/rtps/attributes/RTPSParticipantAttributes.hpp>
#include <fastdds/rtps/attributes/ResourceManagement.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/common/LocatorList.hpp>
#include <fastdds/utils/IPLocator.hpp>

#include "lm/core/types.hpp"
#include "topic_data_type.hpp"

namespace lm::transport {
namespace {

using namespace eprosima::fastdds::dds;  // NOLINT: this TU is DDS glue code end to end.

// --- small helpers -----------------------------------------------------

void log_error(std::string_view where, std::string_view what) {
    std::cerr << "[lm::transport::fastdds] " << where << ": " << what << '\n';
}

Duration_t to_duration(std::chrono::milliseconds ms) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(ms);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(ms - secs);
    return Duration_t(static_cast<std::int32_t>(secs.count()), static_cast<std::uint32_t>(nanos.count()));
}

// --- QoS builders, one per row of the task-11 table ---------------------
//
// Reliability/durability/history compatibility between a DataWriter and a
// DataReader is negotiated from the writer's *offered* QoS against the
// reader's *requested* QoS (DDS "RxO"), not from TopicQos -- so TopicQos is
// left at TOPIC_QOS_DEFAULT everywhere and the table below is applied
// directly to each DataWriterQos/DataReaderQos.
//
// history_memory_policy is forced to DYNAMIC_RESERVE_MEMORY_MODE on every
// endpoint, instead of Fast DDS's default PREALLOCATED_WITH_REALLOC_MEMORY_MODE.
// All four messages are unbounded (variable-length strings/vectors), so their
// actual wire size varies a lot sample to sample; DYNAMIC_RESERVE allocates
// exactly the bytes each sample needs instead of growing (and keeping) one
// realloc'd buffer sized to the largest message ever seen. See
// topic_data_type.hpp for the other half of making unbounded types work with
// this Fast DDS version: max_serialized_type_size cannot be left at the
// documented "0 means unbounded" -- a real crash was tracked down to that,
// independent of this memory-policy choice; see the comment there.
void use_dynamic_history_memory(RTPSEndpointQos& endpoint) {
    endpoint.history_memory_policy = eprosima::fastdds::rtps::DYNAMIC_RESERVE_MEMORY_MODE;
}

DataWriterQos reliable_transient_local_writer_qos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    use_dynamic_history_memory(qos.endpoint());
    return qos;
}

DataReaderQos reliable_transient_local_reader_qos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    use_dynamic_history_memory(qos.endpoint());
    return qos;
}

DataWriterQos best_effort_volatile_writer_qos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    use_dynamic_history_memory(qos.endpoint());
    return qos;
}

DataReaderQos best_effort_volatile_reader_qos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    use_dynamic_history_memory(qos.endpoint());
    return qos;
}

// Reliable, so a command is not dropped in transit -- but VOLATILE, so a
// client that joins later never receives one. See ScriptCommand in
// messages.hpp: changing this to TRANSIENT_LOCAL would make a rebooted
// machine silently re-run whatever it missed while it was down. Both script
// topics (ScriptCommand and ScriptResult) share this reliability/durability
// pair -- a result is an event about one run, exactly as much as the command
// that triggered it, and a late-joining server has no more business
// replaying an old result than a late-joining client has replaying an old
// command. They do NOT share history depth -- see script_command_writer_qos/
// script_command_reader_qos below, which override it for ScriptCommand only.
DataWriterQos reliable_volatile_writer_qos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    use_dynamic_history_memory(qos.endpoint());
    return qos;
}

DataReaderQos reliable_volatile_reader_qos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = VOLATILE_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    use_dynamic_history_memory(qos.endpoint());
    return qos;
}

// ScriptCommand only, at a wider depth than the shared default above.
// RELIABLE + KEEP_LAST(1) on a topic keyed by host_id keeps only the latest
// unacknowledged sample *per instance*, so a second command to the same host
// can evict an earlier one still in flight before a slow/reconnecting reader
// has taken it. That is reachable in ordinary use -- an operator runs a
// script, sees it fail, immediately runs a fix -- and it would surface as a
// run that silently never returns a result: the most confusing symptom this
// feature can produce, since nothing errors, a result just never comes back.
// 16 is generous headroom without going to KEEP_ALL, which has no bound and
// can stall the writer on one slow reader. Do not "tidy" this back to 1 to
// match reliable_volatile_writer_qos's depth -- it is deliberately different.
// ScriptResult stays at depth 1: a result is not superseded by a later one
// the way a command can be.
DataWriterQos script_command_writer_qos() {
    DataWriterQos qos = reliable_volatile_writer_qos();
    qos.history().depth = 16;
    return qos;
}

DataReaderQos script_command_reader_qos() {
    DataReaderQos qos = reliable_volatile_reader_qos();
    qos.history().depth = 16;
    return qos;
}

/// ClientAnnounce carries Liveliness AUTOMATIC with a lease taken from
/// DdsConfig. The reader's requested lease is set to the same value as the
/// writer's offered lease so the two remain RxO-compatible (requested must
/// be >= offered).
DataWriterQos announce_writer_qos(std::chrono::milliseconds lease) {
    DataWriterQos qos = reliable_transient_local_writer_qos();
    qos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
    qos.liveliness().lease_duration = to_duration(lease);
    // Left at the QoS's own default, announcement_period is "infinite",
    // which fails Fast DDS's writer QoS consistency check (it requires
    // announcement_period < lease_duration -- see LivelinessQosPolicy's own
    // "@warning ... advisable to be less than 0.7*lease_duration"). 0.7 of
    // the lease is exactly that recommended margin.
    qos.liveliness().announcement_period = to_duration((lease * 7) / 10);
    return qos;
}

DataReaderQos announce_reader_qos(std::chrono::milliseconds lease) {
    DataReaderQos qos = reliable_transient_local_reader_qos();
    qos.liveliness().kind = AUTOMATIC_LIVELINESS_QOS;
    qos.liveliness().lease_duration = to_duration(lease);
    return qos;
}

// --- topic/type names ----------------------------------------------------
// Shared verbatim between client and server so discovery matches.

constexpr const char* kAnnounceTopicName = "lm.transport.ClientAnnounce";
constexpr const char* kResourceTopicName = "lm.transport.ResourceSample";
constexpr const char* kBundleTopicName = "lm.transport.TemplateBundle";
constexpr const char* kReportTopicName = "lm.transport.ComplianceReport";
constexpr const char* kScriptCommandTopicName = "lm.transport.ScriptCommand";
constexpr const char* kScriptResultTopicName = "lm.transport.ScriptResult";

constexpr const char* kAnnounceTypeName = "lm::transport::ClientAnnounce";
constexpr const char* kResourceTypeName = "lm::transport::ResourceSampleMessage";
constexpr const char* kBundleTypeName = "lm::transport::TemplateBundleMessage";
constexpr const char* kReportTypeName = "lm::transport::ComplianceReportMessage";
constexpr const char* kScriptCommandTypeName = "lm::transport::ScriptCommand";
constexpr const char* kScriptResultTypeName = "lm::transport::ScriptResultMessage";

// --- participant / topic construction ------------------------------------

DomainParticipant* create_participant(DomainParticipantFactory& factory, const DdsConfig& config) {
    DomainParticipantQos qos = PARTICIPANT_QOS_DEFAULT;
    for (const std::string& peer : config.initial_peers) {
        eprosima::fastdds::rtps::Locator_t locator;
        locator.kind = LOCATOR_KIND_UDPv4;
        // Port 0 tells Fast DDS to try this host at the domain's default
        // discovery ports, which is what we want for an explicit peer entry.
        locator.port = 0;
        if (!eprosima::fastdds::rtps::IPLocator::setIPv4(locator, peer)) {
            log_error("initial_peers", "not a valid IPv4 address, skipped: " + peer);
            continue;
        }
        qos.wire_protocol().builtin.initialPeersList.push_back(locator);
    }
    return factory.create_participant(static_cast<DomainId_t>(config.domain_id), qos);
}

/// Registers the codec-backed TopicDataType for Message under type_name and
/// creates the local Topic proxy for it. Returns nullptr on failure so
/// callers can fail construction cleanly.
template <typename Message, bool Keyed>
Topic* make_topic(DomainParticipant& participant, const std::string& topic_name,
                   const std::string& type_name) {
    TypeSupport type_support(new MessageTopicDataType<Message, Keyed>(type_name));
    if (participant.register_type(type_support) != RETCODE_OK) {
        log_error(type_name, "register_type failed");
        return nullptr;
    }
    return participant.create_topic(topic_name, type_name, TOPIC_QOS_DEFAULT);
}

}  // namespace

// ===========================================================================
// DdsClientTransport
// ===========================================================================

namespace {

class DdsClientTransport;
class DdsServerTransport;

/// Forwards on_publication_matched into Owner::handle_match_change. Shared by
/// every DataWriter on both the client and server side -- match-count
/// tracking is identical regardless of which topic the writer carries.
template <typename Owner>
class MatchWriterListener final : public DataWriterListener {
public:
    explicit MatchWriterListener(Owner& owner) : owner_(owner) {}

    void on_publication_matched(DataWriter* /*writer*/, const PublicationMatchedStatus& info) override {
        owner_.handle_match_change(info.current_count_change);
    }

private:
    Owner& owner_;
};

/// Forwards on_subscription_matched into Owner::handle_match_change, with no
/// on_data_available handling at all. Used for the two script-topic readers,
/// which are polled from a dedicated thread instead (see
/// DdsClientTransport::poll_script_commands / DdsServerTransport::
/// poll_script_results) -- this listener exists solely so those readers still
/// contribute to the owner's connection-state match count the same way every
/// other endpoint in this file does.
template <typename Owner>
class MatchReaderListener final : public DataReaderListener {
public:
    explicit MatchReaderListener(Owner& owner) : owner_(owner) {}

    void on_subscription_matched(DataReader* /*reader*/, const SubscriptionMatchedStatus& info) override {
        owner_.handle_match_change(info.current_count_change);
    }

private:
    Owner& owner_;
};

/// Forwards on_data_available (draining every available sample) and
/// on_subscription_matched into Owner member functions. Used for every
/// DataReader that does not also need liveliness tracking (only
/// ClientAnnounce's reader does, see AnnounceReaderListener below).
template <typename Owner, typename Message>
class SimpleReaderListener final : public DataReaderListener {
public:
    SimpleReaderListener(Owner& owner, void (Owner::*on_message)(const Message&))
        : owner_(owner), on_message_(on_message) {}

    void on_data_available(DataReader* reader) override {
        Message sample;
        SampleInfo info;
        // Drain everything available. RETCODE_NO_DATA ends the batch
        // normally. Any other non-OK code is most likely a rejected
        // (malformed) sample -- logged at the codec boundary inside
        // MessageTopicDataType::deserialize -- which take_next_sample
        // already consumes, so `continue` (not `break`) is correct: a single
        // hostile/corrupt sample from any host on the network must never
        // stop later, well-formed samples in the same batch from being
        // delivered (confirmed empirically by the
        // MalformedSampleDoesNotWedgeLaterValidSamples integration test).
        // But a non-OK code could instead be a persistent reader-level
        // error not tied to any one queued sample (e.g. a resource or
        // precondition failure) that would never clear on retry; bound the
        // consecutive failures so that case degrades to abandoning the
        // batch instead of spinning this listener thread at 100% CPU
        // forever. Reset on every successful take so an occasional
        // malformed sample interleaved with valid ones never trips it.
        constexpr int kMaxConsecutiveFailures = 64;
        int consecutive_failures = 0;
        for (;;) {
            const ReturnCode_t rc = reader->take_next_sample(&sample, &info);
            if (rc == RETCODE_NO_DATA) {
                break;
            }
            if (rc != RETCODE_OK) {
                if (++consecutive_failures >= kMaxConsecutiveFailures) {
                    log_error("reader", "too many consecutive failures; abandoning this batch");
                    break;
                }
                continue;
            }
            consecutive_failures = 0;
            if (info.valid_data) {
                (owner_.*on_message_)(sample);
            }
        }
    }

    void on_subscription_matched(DataReader* /*reader*/, const SubscriptionMatchedStatus& info) override {
        owner_.handle_match_change(info.current_count_change);
    }

private:
    Owner& owner_;
    void (Owner::*on_message_)(const Message&);
};

/// Bespoke listener for the server's ClientAnnounce reader: on top of the
/// data/match handling above it also tracks liveliness so a client that
/// stops asserting liveliness can be reported through on_client_lost.
class AnnounceReaderListener final : public DataReaderListener {
public:
    explicit AnnounceReaderListener(DdsServerTransport& owner) : owner_(owner) {}

    void on_data_available(DataReader* reader) override;
    void on_subscription_matched(DataReader* reader, const SubscriptionMatchedStatus& info) override;
    void on_liveliness_changed(DataReader* reader, const LivelinessChangedStatus& status) override;

private:
    DdsServerTransport& owner_;
};

class DdsClientTransport final : public IClientTransport {
public:
    explicit DdsClientTransport(const DdsConfig& config)
        : announce_writer_listener_(*this),
          resource_writer_listener_(*this),
          report_writer_listener_(*this),
          bundle_reader_listener_(*this, &DdsClientTransport::handle_bundle),
          script_result_writer_listener_(*this),
          script_command_reader_listener_(*this) {
        factory_ = DomainParticipantFactory::get_shared_instance();
        participant_ = create_participant(*factory_, config);
        if (participant_ == nullptr) {
            throw std::runtime_error("lm::transport: failed to create Fast DDS participant (client)");
        }

        publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
        subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        announce_topic_ = make_topic<ClientAnnounce, true>(*participant_, kAnnounceTopicName, kAnnounceTypeName);
        resource_topic_ =
            make_topic<ResourceSampleMessage, true>(*participant_, kResourceTopicName, kResourceTypeName);
        report_topic_ =
            make_topic<ComplianceReportMessage, true>(*participant_, kReportTopicName, kReportTypeName);
        bundle_topic_ =
            make_topic<TemplateBundleMessage, false>(*participant_, kBundleTopicName, kBundleTypeName);
        script_command_topic_ =
            make_topic<ScriptCommand, true>(*participant_, kScriptCommandTopicName, kScriptCommandTypeName);
        script_result_topic_ =
            make_topic<ScriptResultMessage, true>(*participant_, kScriptResultTopicName, kScriptResultTypeName);

        if (publisher_ == nullptr || subscriber_ == nullptr || announce_topic_ == nullptr ||
            resource_topic_ == nullptr || report_topic_ == nullptr || bundle_topic_ == nullptr ||
            script_command_topic_ == nullptr || script_result_topic_ == nullptr) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
            throw std::runtime_error("lm::transport: failed to set up Fast DDS client entities");
        }

        announce_writer_ = publisher_->create_datawriter(announce_topic_, announce_writer_qos(config.liveliness_lease),
                                                           &announce_writer_listener_);
        resource_writer_ =
            publisher_->create_datawriter(resource_topic_, best_effort_volatile_writer_qos(), &resource_writer_listener_);
        report_writer_ =
            publisher_->create_datawriter(report_topic_, reliable_transient_local_writer_qos(), &report_writer_listener_);
        bundle_reader_ = subscriber_->create_datareader(bundle_topic_, reliable_transient_local_reader_qos(),
                                                          &bundle_reader_listener_);
        script_result_writer_ = publisher_->create_datawriter(script_result_topic_, reliable_volatile_writer_qos(),
                                                                &script_result_writer_listener_);
        script_command_reader_ = subscriber_->create_datareader(script_command_topic_, script_command_reader_qos(),
                                                                  &script_command_reader_listener_);

        if (announce_writer_ == nullptr || resource_writer_ == nullptr || report_writer_ == nullptr ||
            bundle_reader_ == nullptr || script_result_writer_ == nullptr || script_command_reader_ == nullptr) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
            throw std::runtime_error("lm::transport: failed to create Fast DDS client writers/readers");
        }

        // Started only once every entity above is known-good: the loop below
        // dereferences script_command_reader_ unconditionally, so it must
        // never run against a reader that failed to construct. std::thread's
        // constructor can itself throw (e.g. std::system_error on thread
        // exhaustion) -- unlike every failure above, that throw would not run
        // this destructor (a constructor exception never does), so it needs
        // the same manual cleanup the nullptr checks above already do, or the
        // participant and everything it owns leaks.
        try {
            script_command_poll_thread_ = std::thread(&DdsClientTransport::poll_script_commands, this);
        } catch (...) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
            throw;
        }
    }

    DdsClientTransport(const DdsClientTransport&) = delete;
    DdsClientTransport& operator=(const DdsClientTransport&) = delete;
    DdsClientTransport(DdsClientTransport&&) = delete;
    DdsClientTransport& operator=(DdsClientTransport&&) = delete;

    ~DdsClientTransport() override {
        // Stop and join the poll thread first: it dereferences
        // script_command_reader_ on every iteration, so it must not still be
        // running when delete_contained_entities() below frees that reader.
        stop_script_poll_.store(true, std::memory_order_release);
        if (script_command_poll_thread_.joinable()) {
            script_command_poll_thread_.join();
        }
        if (participant_ != nullptr) {
            // Deletes the writers/readers/publisher/subscriber/topics first,
            // so none of our listeners (destroyed right after, as members)
            // can be invoked again once this call returns.
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
        }
    }

    void publish_announce(const ClientAnnounce& message) override { (void)announce_writer_->write(&message); }
    void publish_resources(const ResourceSampleMessage& message) override {
        (void)resource_writer_->write(&message);
    }
    void publish_report(const ComplianceReportMessage& message) override { (void)report_writer_->write(&message); }

    void publish_script_result(const ScriptResultMessage& message) override {
        (void)script_result_writer_->write(&message);
    }

    /// Mirrors TransientLocal durability at the application level: a bundle
    /// already received (from before this handler was registered, which can
    /// race with discovery) is replayed synchronously.
    void on_bundle(std::function<void(const TemplateBundleMessage&)> handler) override {
        std::optional<TemplateBundleMessage> retained;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_bundle_ = handler;
            retained = retained_bundle_;
        }
        if (handler && retained) {
            handler(*retained);
        }
    }

    /// Commands are delivered from poll_script_commands() below, on its own
    /// thread -- never replayed on registration, unlike on_bundle() above:
    /// ScriptCommand is Volatile, so there is nothing retained to replay.
    void on_script_command(std::function<void(const ScriptCommand&)> handler) override {
        std::lock_guard<std::mutex> lock(mutex_);
        on_script_command_ = std::move(handler);
    }

    /// Fast DDS enables entities (and can start delivering callbacks) as soon
    /// as they are created in the constructor above, which is before the
    /// caller has a chance to call this setter -- so handle_match_change()
    /// below can read on_connection_changed_ concurrently with this write.
    /// Guarded by mutex_ for that reason (see handle_match_change).
    void on_connection_changed(std::function<void(ConnectionState)> handler) override {
        std::lock_guard<std::mutex> lock(mutex_);
        on_connection_changed_ = std::move(handler);
    }

    [[nodiscard]] ConnectionState state() const override { return state_.load(std::memory_order_acquire); }

    // --- called from the listener helpers above; not part of IClientTransport ---

    void handle_match_change(std::int32_t delta) {
        const int new_count = matched_entities_.fetch_add(delta, std::memory_order_relaxed) + delta;
        const ConnectionState new_state = new_count > 0 ? ConnectionState::Connected : ConnectionState::Disconnected;
        const ConnectionState old_state = state_.exchange(new_state, std::memory_order_acq_rel);
        if (old_state == new_state) {
            return;
        }
        // Copy the handler out under the lock, then invoke it unlocked: a
        // handler that calls back into this transport (e.g. registers a new
        // handler of its own) must not deadlock against mutex_.
        std::function<void(ConnectionState)> handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = on_connection_changed_;
        }
        if (handler) {
            handler(new_state);
        }
    }

    void handle_bundle(const TemplateBundleMessage& message) {
        std::function<void(const TemplateBundleMessage&)> handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            retained_bundle_ = message;
            handler = on_bundle_;
        }
        if (handler) {
            handler(message);
        }
    }

    /// Runs for the lifetime of this transport, on its own thread. Polling
    /// take_next_sample() directly -- rather than reading inside
    /// on_data_available, as SimpleReaderListener does for the other three
    /// subscribed topics -- is deliberate here: a Fast DDS reader listener
    /// can report a match and then simply never fire, which cost this
    /// project a day once already (see dds_probe.cpp's Look::run(), which
    /// hit exactly that). Asking the reader directly sidesteps it.
    void poll_script_commands() {
        constexpr auto kPollInterval = std::chrono::milliseconds(20);
        // Same reasoning as SimpleReaderListener's drain loop: a malformed
        // sample from a hostile or buggy host must not wedge this thread,
        // but a run of failures that are not tied to any one sample (a
        // resource/precondition error) must not spin it at 100% CPU forever
        // either.
        constexpr int kMaxConsecutiveFailures = 64;
        int consecutive_failures = 0;
        while (!stop_script_poll_.load(std::memory_order_acquire)) {
            ScriptCommand sample;
            SampleInfo info;
            const ReturnCode_t rc = script_command_reader_->take_next_sample(&sample, &info);
            if (rc == RETCODE_NO_DATA) {
                consecutive_failures = 0;
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (rc != RETCODE_OK) {
                if (++consecutive_failures >= kMaxConsecutiveFailures) {
                    log_error("ScriptCommand reader", "too many consecutive failures; pausing briefly");
                    consecutive_failures = 0;
                }
                // Slept on every failure, not only once the cap is hit --
                // otherwise a persistent (non-sample-specific) reader error
                // free-spins take_next_sample() up to kMaxConsecutiveFailures
                // times back to back before the first pause, the same 100%-
                // CPU risk the cap exists to prevent.
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            consecutive_failures = 0;
            if (!info.valid_data) {
                continue;
            }
            std::function<void(const ScriptCommand&)> handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handler = on_script_command_;
            }
            if (handler) {
                handler(sample);
            }
        }
    }

private:
    std::shared_ptr<DomainParticipantFactory> factory_;
    DomainParticipant* participant_ = nullptr;
    Publisher* publisher_ = nullptr;
    Subscriber* subscriber_ = nullptr;
    Topic* announce_topic_ = nullptr;
    Topic* resource_topic_ = nullptr;
    Topic* report_topic_ = nullptr;
    Topic* bundle_topic_ = nullptr;
    Topic* script_command_topic_ = nullptr;
    Topic* script_result_topic_ = nullptr;
    DataWriter* announce_writer_ = nullptr;
    DataWriter* resource_writer_ = nullptr;
    DataWriter* report_writer_ = nullptr;
    DataReader* bundle_reader_ = nullptr;
    DataWriter* script_result_writer_ = nullptr;
    DataReader* script_command_reader_ = nullptr;

    MatchWriterListener<DdsClientTransport> announce_writer_listener_;
    MatchWriterListener<DdsClientTransport> resource_writer_listener_;
    MatchWriterListener<DdsClientTransport> report_writer_listener_;
    SimpleReaderListener<DdsClientTransport, TemplateBundleMessage> bundle_reader_listener_;
    MatchWriterListener<DdsClientTransport> script_result_writer_listener_;
    MatchReaderListener<DdsClientTransport> script_command_reader_listener_;

    // Set once the poll thread must stop and joined in the destructor --
    // see ~DdsClientTransport for why that has to happen before entities are
    // torn down.
    std::atomic<bool> stop_script_poll_{false};
    std::thread script_command_poll_thread_;

    std::atomic<int> matched_entities_{0};
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};

    // Guards every handler below (and retained_bundle_): Fast DDS listener
    // threads can start invoking handle_bundle()/handle_match_change() as
    // soon as the writers/readers are created in the constructor, which is
    // before on_bundle()/on_connection_changed() are necessarily called, so
    // reads and writes of these std::functions are genuinely concurrent, not
    // just theoretically racy.
    std::mutex mutex_;
    std::optional<TemplateBundleMessage> retained_bundle_;
    std::function<void(const TemplateBundleMessage&)> on_bundle_;
    std::function<void(const ScriptCommand&)> on_script_command_;
    std::function<void(ConnectionState)> on_connection_changed_;
};

// ===========================================================================
// DdsServerTransport
// ===========================================================================

class DdsServerTransport final : public IServerTransport {
public:
    explicit DdsServerTransport(const DdsConfig& config)
        : bundle_writer_listener_(*this),
          announce_reader_listener_(*this),
          resource_reader_listener_(*this, &DdsServerTransport::handle_resources),
          report_reader_listener_(*this, &DdsServerTransport::handle_report),
          script_command_writer_listener_(*this),
          script_result_reader_listener_(*this) {
        factory_ = DomainParticipantFactory::get_shared_instance();
        participant_ = create_participant(*factory_, config);
        if (participant_ == nullptr) {
            throw std::runtime_error("lm::transport: failed to create Fast DDS participant (server)");
        }

        publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
        subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        announce_topic_ = make_topic<ClientAnnounce, true>(*participant_, kAnnounceTopicName, kAnnounceTypeName);
        resource_topic_ =
            make_topic<ResourceSampleMessage, true>(*participant_, kResourceTopicName, kResourceTypeName);
        report_topic_ =
            make_topic<ComplianceReportMessage, true>(*participant_, kReportTopicName, kReportTypeName);
        bundle_topic_ =
            make_topic<TemplateBundleMessage, false>(*participant_, kBundleTopicName, kBundleTypeName);
        script_command_topic_ =
            make_topic<ScriptCommand, true>(*participant_, kScriptCommandTopicName, kScriptCommandTypeName);
        script_result_topic_ =
            make_topic<ScriptResultMessage, true>(*participant_, kScriptResultTopicName, kScriptResultTypeName);

        if (publisher_ == nullptr || subscriber_ == nullptr || announce_topic_ == nullptr ||
            resource_topic_ == nullptr || report_topic_ == nullptr || bundle_topic_ == nullptr ||
            script_command_topic_ == nullptr || script_result_topic_ == nullptr) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
            throw std::runtime_error("lm::transport: failed to set up Fast DDS server entities");
        }

        bundle_writer_ =
            publisher_->create_datawriter(bundle_topic_, reliable_transient_local_writer_qos(), &bundle_writer_listener_);
        announce_reader_ = subscriber_->create_datareader(announce_topic_, announce_reader_qos(config.liveliness_lease),
                                                            &announce_reader_listener_);
        resource_reader_ = subscriber_->create_datareader(resource_topic_, best_effort_volatile_reader_qos(),
                                                            &resource_reader_listener_);
        report_reader_ = subscriber_->create_datareader(report_topic_, reliable_transient_local_reader_qos(),
                                                          &report_reader_listener_);
        script_command_writer_ = publisher_->create_datawriter(script_command_topic_, script_command_writer_qos(),
                                                                 &script_command_writer_listener_);
        script_result_reader_ = subscriber_->create_datareader(script_result_topic_, reliable_volatile_reader_qos(),
                                                                 &script_result_reader_listener_);

        if (bundle_writer_ == nullptr || announce_reader_ == nullptr || resource_reader_ == nullptr ||
            report_reader_ == nullptr || script_command_writer_ == nullptr || script_result_reader_ == nullptr) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
            throw std::runtime_error("lm::transport: failed to create Fast DDS server writers/readers");
        }

        // Started only once every entity above is known-good: the loop below
        // dereferences script_result_reader_ unconditionally, so it must
        // never run against a reader that failed to construct. See the
        // matching try/catch in DdsClientTransport's constructor for why
        // std::thread's own possible throw needs this manual cleanup too.
        try {
            script_result_poll_thread_ = std::thread(&DdsServerTransport::poll_script_results, this);
        } catch (...) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
            throw;
        }
    }

    DdsServerTransport(const DdsServerTransport&) = delete;
    DdsServerTransport& operator=(const DdsServerTransport&) = delete;
    DdsServerTransport(DdsServerTransport&&) = delete;
    DdsServerTransport& operator=(DdsServerTransport&&) = delete;

    ~DdsServerTransport() override {
        // Stop and join the poll thread first: it dereferences
        // script_result_reader_ on every iteration, so it must not still be
        // running when delete_contained_entities() below frees that reader.
        stop_script_poll_.store(true, std::memory_order_release);
        if (script_result_poll_thread_.joinable()) {
            script_result_poll_thread_.join();
        }
        if (participant_ != nullptr) {
            (void)participant_->delete_contained_entities();
            (void)factory_->delete_participant(participant_);
        }
    }

    void publish_bundle(const TemplateBundleMessage& message) override {
        // No manual "push to all clients" step: TRANSIENT_LOCAL + KEEP_LAST(1)
        // on this writer is what makes Fast DDS deliver the retained sample
        // to a client that joins after this call.
        (void)bundle_writer_->write(&message);
    }

    void publish_script_command(const ScriptCommand& message) override {
        (void)script_command_writer_->write(&message);
    }

    void on_announce(std::function<void(const ClientAnnounce&)> handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        on_announce_ = std::move(handler);
    }
    void on_resources(std::function<void(const ResourceSampleMessage&)> handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        on_resources_ = std::move(handler);
    }
    void on_report(std::function<void(const ComplianceReportMessage&)> handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        on_report_ = std::move(handler);
    }
    /// Results are delivered from poll_script_results() below, on its own
    /// thread.
    void on_script_result(std::function<void(const ScriptResultMessage&)> handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        on_script_result_ = std::move(handler);
    }
    void on_client_lost(std::function<void(const core::HostId&)> handler) override {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        on_client_lost_ = std::move(handler);
    }

    [[nodiscard]] ConnectionState state() const override { return state_.load(std::memory_order_acquire); }

    // --- called from the listener helpers above; not part of IServerTransport ---

    void handle_match_change(std::int32_t delta) {
        const int new_count = matched_entities_.fetch_add(delta, std::memory_order_relaxed) + delta;
        const ConnectionState new_state = new_count > 0 ? ConnectionState::Connected : ConnectionState::Disconnected;
        state_.store(new_state, std::memory_order_release);
    }

    void handle_announce(const ClientAnnounce& message, const InstanceHandle_t& publication_handle) {
        {
            std::lock_guard<std::mutex> lock(live_hosts_mutex_);
            live_hosts_[publication_handle] = message.host_id;
        }
        // Copy the handler out under handlers_mutex_, then invoke it
        // unlocked -- see the note on DdsClientTransport::handle_match_change
        // for why (Fast DDS listener threads can reach this concurrently
        // with on_announce() being called from the owning application).
        std::function<void(const ClientAnnounce&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler = on_announce_;
        }
        if (handler) {
            handler(message);
        }
    }

    void handle_resources(const ResourceSampleMessage& message) {
        std::function<void(const ResourceSampleMessage&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler = on_resources_;
        }
        if (handler) {
            handler(message);
        }
    }

    void handle_report(const ComplianceReportMessage& message) {
        std::function<void(const ComplianceReportMessage&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler = on_report_;
        }
        if (handler) {
            handler(message);
        }
    }

    /// A negative alive_count_change means a previously-alive writer just
    /// stopped asserting liveliness. last_publication_handle names which one;
    /// it is the same handle SampleInfo::publication_handle reports for
    /// samples from that writer, which is how handle_announce() populated
    /// live_hosts_ above.
    void handle_liveliness_changed(const LivelinessChangedStatus& status) {
        if (status.alive_count_change >= 0) {
            return;
        }
        core::HostId lost_host;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(live_hosts_mutex_);
            const auto it = live_hosts_.find(status.last_publication_handle);
            if (it != live_hosts_.end()) {
                lost_host = it->second;
                live_hosts_.erase(it);
                found = true;
            }
        }
        if (!found) {
            return;
        }
        std::function<void(const core::HostId&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler = on_client_lost_;
        }
        if (handler) {
            handler(lost_host);
        }
    }

    /// Mirrors DdsClientTransport::poll_script_commands -- same reasoning
    /// (a Fast DDS reader listener can match and still never fire, so this
    /// is polled from a dedicated thread rather than driven by
    /// on_data_available), same drain-and-bound-failures shape.
    void poll_script_results() {
        constexpr auto kPollInterval = std::chrono::milliseconds(20);
        constexpr int kMaxConsecutiveFailures = 64;
        int consecutive_failures = 0;
        while (!stop_script_poll_.load(std::memory_order_acquire)) {
            ScriptResultMessage sample;
            SampleInfo info;
            const ReturnCode_t rc = script_result_reader_->take_next_sample(&sample, &info);
            if (rc == RETCODE_NO_DATA) {
                consecutive_failures = 0;
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (rc != RETCODE_OK) {
                if (++consecutive_failures >= kMaxConsecutiveFailures) {
                    log_error("ScriptResult reader", "too many consecutive failures; pausing briefly");
                    consecutive_failures = 0;
                }
                // See the identical comment in DdsClientTransport::
                // poll_script_commands: sleep on every failure, not only at
                // the cap.
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            consecutive_failures = 0;
            if (!info.valid_data) {
                continue;
            }
            std::function<void(const ScriptResultMessage&)> handler;
            {
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                handler = on_script_result_;
            }
            if (handler) {
                handler(sample);
            }
        }
    }

private:
    std::shared_ptr<DomainParticipantFactory> factory_;
    DomainParticipant* participant_ = nullptr;
    Publisher* publisher_ = nullptr;
    Subscriber* subscriber_ = nullptr;
    Topic* announce_topic_ = nullptr;
    Topic* resource_topic_ = nullptr;
    Topic* report_topic_ = nullptr;
    Topic* bundle_topic_ = nullptr;
    Topic* script_command_topic_ = nullptr;
    Topic* script_result_topic_ = nullptr;
    DataWriter* bundle_writer_ = nullptr;
    DataReader* announce_reader_ = nullptr;
    DataReader* resource_reader_ = nullptr;
    DataReader* report_reader_ = nullptr;
    DataWriter* script_command_writer_ = nullptr;
    DataReader* script_result_reader_ = nullptr;

    MatchWriterListener<DdsServerTransport> bundle_writer_listener_;
    AnnounceReaderListener announce_reader_listener_;
    SimpleReaderListener<DdsServerTransport, ResourceSampleMessage> resource_reader_listener_;
    SimpleReaderListener<DdsServerTransport, ComplianceReportMessage> report_reader_listener_;
    MatchWriterListener<DdsServerTransport> script_command_writer_listener_;
    MatchReaderListener<DdsServerTransport> script_result_reader_listener_;

    // Set once the poll thread must stop and joined in the destructor --
    // see ~DdsServerTransport for why that has to happen before entities are
    // torn down.
    std::atomic<bool> stop_script_poll_{false};
    std::thread script_result_poll_thread_;

    std::atomic<int> matched_entities_{0};
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};

    std::mutex live_hosts_mutex_;
    std::map<InstanceHandle_t, core::HostId> live_hosts_;

    // Guards on_announce_/on_resources_/on_report_/on_client_lost_: writers
    // and readers are created (and can start firing) inside the constructor,
    // before the owning application has necessarily called on_announce() et
    // al. -- see DdsClientTransport's mutex_ for the identical reasoning.
    std::mutex handlers_mutex_;
    std::function<void(const ClientAnnounce&)> on_announce_;
    std::function<void(const ResourceSampleMessage&)> on_resources_;
    std::function<void(const ComplianceReportMessage&)> on_report_;
    std::function<void(const ScriptResultMessage&)> on_script_result_;
    std::function<void(const core::HostId&)> on_client_lost_;
};

// AnnounceReaderListener's bodies are defined here, after DdsServerTransport
// is a complete type.

void AnnounceReaderListener::on_data_available(DataReader* reader) {
    ClientAnnounce sample;
    SampleInfo info;
    // Same drain-past-rejects reasoning as SimpleReaderListener above -- a
    // malformed ClientAnnounce must not stop the rest of the batch from
    // being processed -- and the same bounded-failure safety net against a
    // persistent (non-sample-specific) reader error spinning this thread
    // forever. Reset on every successful take.
    constexpr int kMaxConsecutiveFailures = 64;
    int consecutive_failures = 0;
    for (;;) {
        const ReturnCode_t rc = reader->take_next_sample(&sample, &info);
        if (rc == RETCODE_NO_DATA) {
            break;
        }
        if (rc != RETCODE_OK) {
            if (++consecutive_failures >= kMaxConsecutiveFailures) {
                log_error("ClientAnnounce reader", "too many consecutive failures; abandoning this batch");
                break;
            }
            continue;
        }
        consecutive_failures = 0;
        if (info.valid_data) {
            owner_.handle_announce(sample, info.publication_handle);
        }
    }
}

void AnnounceReaderListener::on_subscription_matched(DataReader* /*reader*/, const SubscriptionMatchedStatus& info) {
    owner_.handle_match_change(info.current_count_change);
}

void AnnounceReaderListener::on_liveliness_changed(DataReader* /*reader*/, const LivelinessChangedStatus& status) {
    owner_.handle_liveliness_changed(status);
}

}  // namespace

std::unique_ptr<IClientTransport> make_dds_client(const DdsConfig& config) {
    return std::make_unique<DdsClientTransport>(config);
}

std::unique_ptr<IServerTransport> make_dds_server(const DdsConfig& config) {
    return std::make_unique<DdsServerTransport>(config);
}

}  // namespace lm::transport
