#include "lm/transport/dds_probe.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicData.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicDataFactory.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicPubSubType.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicType.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilder.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilderFactory.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeMember.hpp>
#include <fastdds/dds/xtypes/dynamic_types/MemberDescriptor.hpp>
#include <fastdds/dds/xtypes/type_representation/ITypeObjectRegistry.hpp>
#include <fastdds/dds/xtypes/type_representation/TypeObject.hpp>

namespace lm::transport {
namespace {

using namespace eprosima::fastdds::dds;
namespace xtypes = eprosima::fastdds::dds::xtypes;

core::DdsTopicSample failed(std::string message) {
    core::DdsTopicSample sample;
    sample.error = std::move(message);
    return sample;
}

/// Waits for a writer on one named topic and remembers the type it advertised.
///
/// Discovery is asynchronous and arrives on a Fast DDS thread, so everything
/// here is behind a mutex and the caller blocks on the condition variable
/// rather than polling.
class TopicWatcher : public DomainParticipantListener {
public:
    explicit TopicWatcher(std::string topic_name) : topic_name_(std::move(topic_name)) {}

    void on_data_writer_discovery(DomainParticipant* /*participant*/,
                                  eprosima::fastdds::rtps::WriterDiscoveryStatus /*reason*/,
                                  const PublicationBuiltinTopicData& info,
                                  bool& should_be_ignored) override {
        should_be_ignored = false;
        if (std::string(info.topic_name.c_str()) != topic_name_) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            found_ = true;
            type_name_ = info.type_name.c_str();
            // The type *identifier*, not just its name. A name only resolves
            // if the type happens to be registered locally under it, which is
            // never true for a type this process has never compiled against --
            // the identifier is what discovery actually gives us to look up.
            type_information_ = info.type_information.type_information;
        }
        signal_.notify_all();
    }

    /// True if a writer showed up within the wait.
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        signal_.wait_for(lock, timeout, [this] { return found_; });
        return found_;
    }

    [[nodiscard]] std::string type_name() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return type_name_;
    }

    [[nodiscard]] xtypes::TypeInformation type_information() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return type_information_;
    }

private:
    std::string topic_name_;
    std::mutex mutex_;
    std::condition_variable signal_;
    bool found_ = false;
    std::string type_name_;
    xtypes::TypeInformation type_information_;
};

/// Wakes the caller when a sample lands, so the read is a wait rather than a
/// sleep-and-hope.
class SampleWatcher : public DataReaderListener {
public:
    void on_data_available(DataReader* /*reader*/) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            arrived_ = true;
        }
        signal_.notify_all();
    }

    /// Recorded so a read that finds nothing can say *why*: a reader that never
    /// matched the writer is a QoS or type mismatch, while one that matched and
    /// still saw nothing means the writer genuinely has not published. Those
    /// are different problems and the operator can only act on the second.
    void on_subscription_matched(DataReader* /*reader*/,
                                 const SubscriptionMatchedStatus& status) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        matched_ = status.current_count > 0;
    }

    [[nodiscard]] bool matched() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return matched_;
    }

    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        signal_.wait_for(lock, timeout, [this] { return arrived_; });
        return arrived_;
    }

private:
    std::mutex mutex_;
    std::condition_variable signal_;
    bool arrived_ = false;
    bool matched_ = false;
};

/// Turns what discovery said about a type into the type description itself.
///
/// Two things make this more than one call. The complete TypeObject is the one
/// that carries member *names*, which a path like `items_.length` needs, but a
/// writer may only publish the minimal one -- so both identifiers are tried,
/// complete first. And the TypeLookup service fetches the description from the
/// remote participant asynchronously *after* discovery fires, so the first
/// lookup routinely misses; polling briefly is the difference between reading
/// the topic and declaring it unreadable.
bool fetch_type_object(const xtypes::TypeInformation& information, xtypes::TypeObject& out,
                       std::chrono::milliseconds budget) {
    auto& registry = DomainParticipantFactory::get_instance()->type_object_registry();
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + budget;

    // An identifier a writer never filled in is left at TK_NONE, and handing
    // one of those to the registry is not merely fruitless -- it faults. A
    // publisher with neither identifier set has advertised nothing, which the
    // caller reports as such.
    const auto usable = [](const xtypes::TypeIdentifier& id) {
        return id._d() != xtypes::TK_NONE;
    };

    do {
        const auto& complete = information.complete().typeid_with_size().type_id();
        if (usable(complete) && registry.get_type_object(complete, out) == RETCODE_OK) {
            return true;
        }
        // Only the minimal description reached us, so member names are hashes
        // rather than words. A path like items_.length cannot resolve against
        // that, but reporting the topic as unreadable is still better than
        // reporting it as empty.
        const auto& minimal = information.minimal().typeid_with_size().type_id();
        if (usable(minimal) && registry.get_type_object(minimal, out) == RETCODE_OK) {
            return true;
        }
        if (!usable(complete) && !usable(minimal)) {
            return false;  // nothing was advertised; waiting cannot help
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
}

nlohmann::json to_json(const traits<DynamicData>::ref_type& data);

/// One member of a structure, or one element of a collection, as JSON.
///
/// Scalars are read through the typed getter for their kind; anything with
/// structure is loaned and recursed into. Kinds this does not name -- unions,
/// maps, bitmasks -- become a string saying so rather than silently reading
/// as null, because a rule that quietly compares against nothing is worse than
/// one that says it could not look.
nlohmann::json member_to_json(const traits<DynamicData>::ref_type& data, MemberId id,
                              TypeKind kind) {
    switch (kind) {
        case xtypes::TK_BOOLEAN: {
            bool value = false;
            data->get_boolean_value(value, id);
            return value;
        }
        case xtypes::TK_BYTE:
        case xtypes::TK_UINT8:
        case xtypes::TK_UINT16:
        case xtypes::TK_UINT32: {
            std::uint32_t value = 0;
            data->get_uint32_value(value, id);
            return value;
        }
        case xtypes::TK_INT16:
        case xtypes::TK_INT32:
        case xtypes::TK_INT64:
        case xtypes::TK_ENUM: {
            std::int64_t value = 0;
            data->get_int64_value(value, id);
            return value;
        }
        case xtypes::TK_UINT64: {
            std::int64_t value = 0;
            data->get_int64_value(value, id);
            return value;
        }
        case xtypes::TK_FLOAT32:
        case xtypes::TK_FLOAT64: {
            double value = 0.0;
            data->get_float64_value(value, id);
            return value;
        }
        case xtypes::TK_CHAR8:
        case xtypes::TK_STRING8: {
            std::string value;
            data->get_string_value(value, id);
            return value;
        }
        case xtypes::TK_STRUCTURE:
        case xtypes::TK_SEQUENCE:
        case xtypes::TK_ARRAY: {
            const traits<DynamicData>::ref_type nested = data->loan_value(id);
            if (!nested) {
                return nullptr;
            }
            nlohmann::json result = to_json(nested);
            data->return_loaned_value(nested);
            return result;
        }
        default:
            return "(unsupported type)";
    }
}

/// Projects a whole sample -- or any nested structure or collection -- to JSON.
///
/// This is the one place the DDS type system is translated into something
/// lm_core can reason about. Everything downstream of it is pure and testable
/// against a JSON literal, which is the entire point of drawing the line here.
nlohmann::json to_json(const traits<DynamicData>::ref_type& data) {
    const traits<DynamicType>::ref_type type = data->type();
    if (!type) {
        return nullptr;
    }

    const TypeKind kind = type->get_kind();
    if (kind == xtypes::TK_SEQUENCE || kind == xtypes::TK_ARRAY) {
        TypeDescriptor::_ref_type descriptor{traits<TypeDescriptor>::make_shared()};
        TypeKind element_kind = xtypes::TK_NONE;
        if (type->get_descriptor(descriptor) == RETCODE_OK && descriptor->element_type()) {
            element_kind = descriptor->element_type()->get_kind();
        }
        nlohmann::json array = nlohmann::json::array();
        const std::uint32_t count = data->get_item_count();
        for (std::uint32_t i = 0; i < count; ++i) {
            array.push_back(member_to_json(data, static_cast<MemberId>(i), element_kind));
        }
        return array;
    }

    if (kind != xtypes::TK_STRUCTURE) {
        // A reader is created on a topic type, which is a structure in every
        // case this probe can reach; anything else means the sample was not
        // what discovery said it was.
        return nullptr;
    }

    nlohmann::json object = nlohmann::json::object();
    const std::uint32_t members = type->get_member_count();
    for (std::uint32_t i = 0; i < members; ++i) {
        traits<DynamicTypeMember>::ref_type member;
        if (type->get_member_by_index(member, i) != RETCODE_OK || !member) {
            continue;
        }
        MemberDescriptor::_ref_type descriptor{traits<MemberDescriptor>::make_shared()};
        if (member->get_descriptor(descriptor) != RETCODE_OK || !descriptor->type()) {
            continue;
        }
        object[member->get_name().to_string()] =
            member_to_json(data, member->get_id(), descriptor->type()->get_kind());
    }
    return object;
}

/// Owns one look at one topic. Everything it creates is torn down in the
/// destructor, in reverse order, because a probe that leaked a participant per
/// tick would exhaust the machine within an hour of a 30 s cadence.
class Look {
public:
    explicit Look(std::string topic_name) : watcher_(std::move(topic_name)) {}
    Look(const Look&) = delete;
    Look& operator=(const Look&) = delete;

    /// The two listeners are *members*, declared above participant_, and the
    /// entities are torn down in this body -- before any member is destroyed.
    /// They started as locals in run(), which left Fast DDS calling into freed
    /// listeners the moment run() returned: an access violation on the very
    /// first look. A listener must outlive the entity it is attached to.
    ~Look() {
        if (participant_ != nullptr) {
            participant_->delete_contained_entities();
            DomainParticipantFactory::get_instance()->delete_participant(participant_);
        }
    }

    core::DdsTopicSample run(std::uint32_t domain_id, const std::string& topic_name,
                             const DdsProbeConfig& config) {
        TopicWatcher& watcher = watcher_;
        participant_ = DomainParticipantFactory::get_instance()->create_participant(
            domain_id, PARTICIPANT_QOS_DEFAULT, &watcher);
        if (participant_ == nullptr) {
            return failed("could not join DDS domain " + std::to_string(domain_id));
        }

        core::DdsTopicSample sample;
        if (!watcher.wait(config.discovery_wait)) {
            // Not an error: nobody is publishing this topic, which is exactly
            // what a presence rule is asking about.
            return sample;
        }
        sample.topic_found = true;

        const std::string type_name = watcher.type_name();
        const xtypes::TypeInformation information = watcher.type_information();
        xtypes::TypeObject type_object;
        if (!fetch_type_object(information, type_object, config.type_lookup_wait)) {
            // The make-or-break case, and the one an operator has to be able to
            // act on: a publisher that propagates only a type *name* cannot be
            // read without its IDL. Say exactly that rather than reporting an
            // empty topic.
            sample.error = "the publisher of \"" + topic_name +
                           "\" does not advertise a type description for \"" + type_name +
                           "\", so its data cannot be read without its IDL";
            return sample;
        }

        traits<DynamicTypeBuilder>::ref_type builder =
            DynamicTypeBuilderFactory::get_instance()->create_type_w_type_object(type_object);
        if (!builder) {
            sample.error = "the type description for \"" + type_name + "\" could not be rebuilt";
            return sample;
        }
        const traits<DynamicType>::ref_type dynamic_type = builder->build();
        if (!dynamic_type) {
            sample.error = "the type \"" + type_name + "\" could not be built from its description";
            return sample;
        }

        TypeSupport support(new DynamicPubSubType(dynamic_type));
        support.register_type(participant_);

        Topic* topic = participant_->create_topic(topic_name, support.get_type_name(),
                                                   TOPIC_QOS_DEFAULT);
        if (topic == nullptr) {
            sample.error = "could not open topic \"" + topic_name + "\"";
            return sample;
        }

        Subscriber* subscriber =
            participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT, nullptr);
        if (subscriber == nullptr) {
            sample.error = "could not create a subscriber on domain " + std::to_string(domain_id);
            return sample;
        }

        // TRANSIENT_LOCAL asks for the last sample a writer kept, so a topic
        // that ticks slowly -- or has already gone quiet -- still answers.
        // Falling back to whatever the writer offers is deliberate: a reader
        // that insists on durability the writer does not provide simply never
        // matches, and the check would report "no sample" forever.
        DataReaderQos reader_qos = DATAREADER_QOS_DEFAULT;
        reader_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        reader_qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        reader_qos.history().kind = KEEP_LAST_HISTORY_QOS;
        reader_qos.history().depth = 1;

        DataReader* reader = subscriber->create_datareader(topic, reader_qos, &samples_);
        if (reader == nullptr) {
            sample.error = "could not create a reader on \"" + topic_name + "\"";
            return sample;
        }

        // Polled rather than driven by on_data_available. This is a one-shot
        // blocking read that already owns a deadline, so a listener adds a
        // second mechanism and a second way to be wrong -- and it was wrong:
        // with the callback as the trigger, a reader that had demonstrably
        // matched its writer sat through the whole wait without ever being
        // told data was there. Asking the reader directly is both simpler and
        // the thing that actually works.
        traits<DynamicData>::ref_type data =
            DynamicDataFactory::get_instance()->create_data(dynamic_type);
        SampleInfo info;
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + config.sample_wait;
        ReturnCode_t taken = RETCODE_NO_DATA;
        while (std::chrono::steady_clock::now() < deadline) {
            taken = reader->take_next_sample(&data, &info);
            if (taken == RETCODE_OK) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }

        if (taken != RETCODE_OK) {
            if (!samples_.matched()) {
                // Discovery saw the writer but the reader never matched it, so
                // this is not a quiet topic -- it is a reader that cannot be
                // joined to that writer at all.
                sample.error = "a writer for \"" + topic_name +
                               "\" was discovered but no reader could be matched to it";
            }
            // Otherwise: the topic is there, matched, and nothing has been
            // published. Nothing is wrong with the machine and nothing can be
            // said about the data.
            return sample;
        }
        if (!info.valid_data) {
            // A disposal or unregister notification carries no payload. The
            // topic is alive; there is simply no value to compare against.
            return sample;
        }

        try {
            sample.json = to_json(data).dump();
            sample.has_sample = true;
        } catch (const std::exception& error) {
            sample.error = std::string("the sample could not be projected to JSON: ") + error.what();
        }
        return sample;
    }

private:
    // Declared before participant_ so they are destroyed after it: see ~Look().
    TopicWatcher watcher_;
    SampleWatcher samples_;
    DomainParticipant* participant_ = nullptr;
};

class FastDdsProbe : public platform::IDdsProbe {
public:
    explicit FastDdsProbe(DdsProbeConfig config) : config_(config) {}

    core::DdsTopicSample look(std::uint32_t domain_id, const std::string& topic_name) override {
        // Total by contract: this runs on the client's worker thread, and an
        // escaping exception would take the whole agent down over one rule.
        try {
            Look look(topic_name);
            return look.run(domain_id, topic_name, config_);
        } catch (const std::exception& error) {
            return failed(std::string("reading the DDS bus failed: ") + error.what());
        } catch (...) {
            return failed("reading the DDS bus failed for an unknown reason");
        }
    }

private:
    DdsProbeConfig config_;
};

}  // namespace

std::unique_ptr<platform::IDdsProbe> make_dds_probe(DdsProbeConfig config) {
    return std::make_unique<FastDdsProbe>(config);
}

}  // namespace lm::transport
