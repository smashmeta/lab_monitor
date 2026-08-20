#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicData.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicDataFactory.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicPubSubType.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilderFactory.hpp>
#include <fastdds/dds/xtypes/dynamic_types/MemberDescriptor.hpp>
#include <fastdds/dds/xtypes/dynamic_types/TypeDescriptor.hpp>

#include "lm/core/json_path.hpp"
#include "lm/transport/dds_probe.hpp"

using namespace eprosima::fastdds::dds;
namespace xtypes = eprosima::fastdds::dds::xtypes;

namespace {

/// A domain of its own, so a developer running these while a real fleet is up
/// does not read that fleet's traffic -- or publish into it.
constexpr std::uint32_t kProbeDomain = 71;

/// Stands in for the sibling application the design is actually for: it
/// publishes a Basket without lab_monitor having ever seen its IDL.
///
/// Built through DynamicTypeBuilderFactory rather than fastddsgen, which is
/// what makes the test honest -- the probe has no compile-time knowledge of
/// this type either, and gets everything from discovery.
class BasketPublisher {
public:
    ~BasketPublisher() {
        if (participant_ != nullptr) {
            participant_->delete_contained_entities();
            DomainParticipantFactory::get_instance()->delete_participant(participant_);
        }
    }

    /// Returns false when the type could not be built, so a failure here is
    /// reported as a broken fixture rather than as a probe defect.
    bool start(const std::string& status, int item_count) {
        auto factory = DynamicTypeBuilderFactory::get_instance();

        // struct Item { string sku; };
        TypeDescriptor::_ref_type item_descriptor{traits<TypeDescriptor>::make_shared()};
        item_descriptor->kind(xtypes::TK_STRUCTURE);
        item_descriptor->name("Item");
        auto item_builder = factory->create_type(item_descriptor);
        if (!item_builder) {
            return false;
        }
        MemberDescriptor::_ref_type sku{traits<MemberDescriptor>::make_shared()};
        sku->name("sku");
        sku->type(factory->create_string_type(static_cast<std::uint32_t>(LENGTH_UNLIMITED))->build());
        item_builder->add_member(sku);
        const auto item_type = item_builder->build();

        // struct Basket { string status; sequence<Item> items_; };
        TypeDescriptor::_ref_type basket_descriptor{traits<TypeDescriptor>::make_shared()};
        basket_descriptor->kind(xtypes::TK_STRUCTURE);
        basket_descriptor->name("Basket");
        auto basket_builder = factory->create_type(basket_descriptor);
        if (!basket_builder) {
            return false;
        }
        MemberDescriptor::_ref_type status_member{traits<MemberDescriptor>::make_shared()};
        status_member->name("status");
        status_member->type(
            factory->create_string_type(static_cast<std::uint32_t>(LENGTH_UNLIMITED))->build());
        basket_builder->add_member(status_member);

        MemberDescriptor::_ref_type items_member{traits<MemberDescriptor>::make_shared()};
        items_member->name("items_");
        items_member->type(
            factory->create_sequence_type(item_type, static_cast<std::uint32_t>(LENGTH_UNLIMITED))
                ->build());
        basket_builder->add_member(items_member);

        type_ = basket_builder->build();
        if (!type_) {
            return false;
        }

        participant_ = DomainParticipantFactory::get_instance()->create_participant(
            kProbeDomain, PARTICIPANT_QOS_DEFAULT);
        if (participant_ == nullptr) {
            return false;
        }

        TypeSupport support(new DynamicPubSubType(type_));
        support.register_type(participant_);

        topic_ = participant_->create_topic("Basket", support.get_type_name(), TOPIC_QOS_DEFAULT);
        Publisher* publisher = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
        if (topic_ == nullptr || publisher == nullptr) {
            return false;
        }

        // TRANSIENT_LOCAL so the probe still gets the last value when it
        // attaches after the write -- the same reason the real bundle topic
        // uses it, and what lets this test not race.
        DataWriterQos writer_qos = DATAWRITER_QOS_DEFAULT;
        writer_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        writer_qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
        writer_ = publisher->create_datawriter(topic_, writer_qos);
        if (writer_ == nullptr) {
            return false;
        }

        return publish(status, item_count);
    }

private:
    bool publish(const std::string& status, int item_count) {
        auto data = DynamicDataFactory::get_instance()->create_data(type_);
        data->set_string_value(data->get_member_id_by_name("status"), status);

        auto items = data->loan_value(data->get_member_id_by_name("items_"));
        for (int i = 0; i < item_count; ++i) {
            auto element = items->loan_value(static_cast<MemberId>(i));
            element->set_string_value(element->get_member_id_by_name("sku"),
                                       "SKU-" + std::to_string(i));
            items->return_loaned_value(element);
        }
        data->return_loaned_value(items);

        return writer_->write(&data) == RETCODE_OK;
    }

    DomainParticipant* participant_ = nullptr;
    Topic* topic_ = nullptr;
    DataWriter* writer_ = nullptr;
    traits<DynamicType>::ref_type type_;
};

}  // namespace

TEST(DdsProbeLoopback, ReadsATopicItHasNeverSeenTheIdlFor) {
    // The claim the whole feature rests on: a rule names a domain, a topic and
    // a path, and nothing else. If this fails, no amount of rule design helps.
    BasketPublisher publisher;
    ASSERT_TRUE(publisher.start("Ready", 2)) << "fixture could not publish";

    const auto probe = lm::transport::make_dds_probe();
    const lm::core::DdsTopicSample sample = probe->look(kProbeDomain, "Basket");

    ASSERT_TRUE(sample.error.empty()) << sample.error;
    ASSERT_TRUE(sample.topic_found);
    ASSERT_TRUE(sample.has_sample) << "the topic was found but nothing was read";

    const nlohmann::json document = nlohmann::json::parse(sample.json);
    const auto count = lm::core::resolve_path(document, "items_.length");
    ASSERT_TRUE(count.has_value()) << count.error().message;
    EXPECT_EQ(*count, 2) << sample.json;

    const auto status = lm::core::resolve_path(document, "status");
    ASSERT_TRUE(status.has_value()) << status.error().message;
    EXPECT_EQ(*status, "Ready");

    const auto sku = lm::core::resolve_path(document, "items_[1].sku");
    ASSERT_TRUE(sku.has_value()) << sku.error().message;
    EXPECT_EQ(*sku, "SKU-1") << "nested structures inside a sequence must survive the projection";
}

TEST(DdsProbeLoopback, ReportsATopicNobodyPublishesAsAbsentRatherThanAnError) {
    // "Not on the bus" is a finding a rule can act on; an error is not.
    lm::transport::DdsProbeConfig config;
    config.discovery_wait = std::chrono::milliseconds{500};
    const auto probe = lm::transport::make_dds_probe(config);

    const lm::core::DdsTopicSample sample = probe->look(kProbeDomain, "NoSuchTopicHere");

    EXPECT_TRUE(sample.error.empty()) << sample.error;
    EXPECT_FALSE(sample.topic_found);
    EXPECT_FALSE(sample.has_sample);
}
