#include "cart_publisher.hpp"

#include <string>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>
#include <fastdds/utils/IPLocator.hpp>

#include <memory>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicData.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicDataFactory.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicPubSubType.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicType.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilder.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilderFactory.hpp>
#include <fastdds/dds/xtypes/dynamic_types/MemberDescriptor.hpp>
#include <fastdds/dds/xtypes/dynamic_types/TypeDescriptor.hpp>

namespace cart {
namespace {

using namespace eprosima::fastdds::dds;
namespace xtypes = eprosima::fastdds::dds::xtypes;

constexpr std::uint32_t kUnbounded = static_cast<std::uint32_t>(LENGTH_UNLIMITED);

traits<DynamicType>::ref_type unbounded_string() {
    return DynamicTypeBuilderFactory::get_instance()->create_string_type(kUnbounded)->build();
}

void add_member(const traits<DynamicTypeBuilder>::ref_type& builder, const char* name,
                const traits<DynamicType>::ref_type& type) {
    MemberDescriptor::_ref_type member{traits<MemberDescriptor>::make_shared()};
    member->name(name);
    member->type(type);
    builder->add_member(member);
}

traits<DynamicTypeBuilder>::ref_type structure(const char* name) {
    TypeDescriptor::_ref_type descriptor{traits<TypeDescriptor>::make_shared()};
    descriptor->kind(xtypes::TK_STRUCTURE);
    descriptor->name(name);
    return DynamicTypeBuilderFactory::get_instance()->create_type(descriptor);
}

/// struct CartLine { string sku; double price; long quantity; };
/// struct ShoppingCart { string status; long unit_count; double total;
///                       sequence<CartLine> items_; };
///
/// `items_` keeps the trailing underscore from the motivating example, so a
/// rule copied out of that conversation addresses this without editing. Both a
/// sequence and two scalars are published on purpose: it lets one fixture
/// exercise `items_.length`, a numeric comparison on `total`, a text match on
/// `status`, and a nested read through `items_[0].sku`.
traits<DynamicType>::ref_type build_cart_type() {
    auto* factory = DynamicTypeBuilderFactory::get_instance().get();

    const auto line = structure("CartLine");
    if (!line) {
        return {};
    }
    add_member(line, "sku", unbounded_string());
    add_member(line, "price", factory->get_primitive_type(xtypes::TK_FLOAT64));
    add_member(line, "quantity", factory->get_primitive_type(xtypes::TK_INT32));
    const auto line_type = line->build();

    const auto cart = structure("ShoppingCart");
    if (!cart || !line_type) {
        return {};
    }
    add_member(cart, "status", unbounded_string());
    add_member(cart, "unit_count", factory->get_primitive_type(xtypes::TK_INT32));
    add_member(cart, "total", factory->get_primitive_type(xtypes::TK_FLOAT64));
    add_member(cart, "items_", factory->create_sequence_type(line_type, kUnbounded)->build());
    return cart->build();
}

}  // namespace

struct Publisher::Impl {
    DomainParticipant* participant = nullptr;
    DataWriter* writer = nullptr;
    traits<DynamicType>::ref_type type;

    ~Impl() {
        if (participant != nullptr) {
            participant->delete_contained_entities();
            DomainParticipantFactory::get_instance()->delete_participant(participant);
        }
    }
};

namespace {

/// Participant QoS confining this publisher to the machine it runs on.
///
/// The whitelist is the whole mechanism: a UDPv4 transport told to use only
/// 127.0.0.1 binds no other adapter, so nothing this participant sends leaves
/// the host and nothing from another host reaches it. That has to replace the
/// builtin transports rather than sit alongside them -- interfaceWhiteList is
/// a property of a transport descriptor, and the builtin UDPv4 is created with
/// no whitelist at all, so leaving it in place would keep every adapter open.
///
/// Shared memory is added back deliberately. It cannot leave the machine by
/// construction, so it costs nothing in isolation, and dropping it would make
/// this participant slower than the default for the one case it exists to
/// serve -- two processes on one PC.
///
/// The localhost initial peer is what makes discovery reliable rather than
/// lucky. Fast DDS discovers over multicast by default, and loopback multicast
/// is the least dependable corner of the stack on Windows; naming 127.0.0.1 as
/// an initial peer adds a unicast announcement to the well-known ports on this
/// host, which is exactly and only where the reader can be.
eprosima::fastdds::dds::DomainParticipantQos localhost_only_qos() {
    using eprosima::fastdds::rtps::IPLocator;
    using eprosima::fastdds::rtps::Locator_t;
    using eprosima::fastdds::rtps::SharedMemTransportDescriptor;
    using eprosima::fastdds::rtps::UDPv4TransportDescriptor;

    eprosima::fastdds::dds::DomainParticipantQos qos =
        eprosima::fastdds::dds::PARTICIPANT_QOS_DEFAULT;
    qos.transport().use_builtin_transports = false;

    auto udp = std::make_shared<UDPv4TransportDescriptor>();
    udp->interfaceWhiteList.emplace_back("127.0.0.1");
    qos.transport().user_transports.push_back(udp);
    qos.transport().user_transports.push_back(std::make_shared<SharedMemTransportDescriptor>());

    Locator_t peer;
    peer.kind = LOCATOR_KIND_UDPv4;
    // Port 0 means "every participant id on that address", so the reader is
    // found whatever order the two processes were started in.
    peer.port = 0;
    IPLocator::setIPv4(peer, 127, 0, 0, 1);
    qos.wire_protocol().builtin.initialPeersList.push_back(peer);

    return qos;
}

}  // namespace

Publisher::Publisher() : impl_(std::make_unique<Impl>()) {}
Publisher::~Publisher() = default;

std::string Publisher::start(std::uint32_t domain_id, const std::string& topic_name,
                             bool localhost_only) {
    impl_->type = build_cart_type();
    if (!impl_->type) {
        return "the ShoppingCart type could not be built";
    }

    impl_->participant = DomainParticipantFactory::get_instance()->create_participant(
        domain_id, localhost_only ? localhost_only_qos() : PARTICIPANT_QOS_DEFAULT);
    if (impl_->participant == nullptr) {
        return "could not join DDS domain " + std::to_string(domain_id);
    }

    TypeSupport support(new DynamicPubSubType(impl_->type));
    support.register_type(impl_->participant);

    Topic* topic = impl_->participant->create_topic(topic_name, support.get_type_name(),
                                                     TOPIC_QOS_DEFAULT);
    eprosima::fastdds::dds::Publisher* publisher =
        impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (topic == nullptr || publisher == nullptr) {
        return "could not open topic \"" + topic_name + "\"";
    }

    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    impl_->writer = publisher->create_datawriter(topic, qos);
    if (impl_->writer == nullptr) {
        return "could not create a writer on \"" + topic_name + "\"";
    }

    return {};
}

bool Publisher::publish(const State& state) {
    if (impl_->writer == nullptr) {
        return false;
    }

    auto data = DynamicDataFactory::get_instance()->create_data(impl_->type);
    data->set_string_value(data->get_member_id_by_name("status"), state.status);
    data->set_int32_value(data->get_member_id_by_name("unit_count"), unit_count(state));
    data->set_float64_value(data->get_member_id_by_name("total"), total(state));

    auto items = data->loan_value(data->get_member_id_by_name("items_"));
    if (!items) {
        return false;
    }
    for (std::size_t i = 0; i < state.items.size(); ++i) {
        auto line = items->loan_value(static_cast<MemberId>(i));
        if (!line) {
            break;
        }
        line->set_string_value(line->get_member_id_by_name("sku"), state.items[i].sku);
        line->set_float64_value(line->get_member_id_by_name("price"), state.items[i].price);
        line->set_int32_value(line->get_member_id_by_name("quantity"), state.items[i].quantity);
        items->return_loaned_value(line);
    }
    data->return_loaned_value(items);

    return impl_->writer->write(&data) == RETCODE_OK;
}

}  // namespace cart
