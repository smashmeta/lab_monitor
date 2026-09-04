#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "lm/transport/transport.hpp"

namespace lm::transport {

/// An in-process stand-in for a DDS domain. Delivery is synchronous, so tests
/// need no waiting or polling. Retains the last published bundle so clients
/// attaching afterwards still receive it.
class MessageBus {
public:
    void publish_announce(const ClientAnnounce& message);
    void publish_resources(const ResourceSampleMessage& message);
    void publish_report(const ComplianceReportMessage& message);
    void publish_bundle(const TemplateBundleMessage& message);
    void publish_script_command(const ScriptCommand& message);
    void publish_script_result(const ScriptResultMessage& message);

    void subscribe_announce(std::function<void(const ClientAnnounce&)> handler);
    void subscribe_resources(std::function<void(const ResourceSampleMessage&)> handler);
    void subscribe_report(std::function<void(const ComplianceReportMessage&)> handler);
    /// Immediately replays the retained bundle, if any.
    void subscribe_bundle(std::function<void(const TemplateBundleMessage&)> handler);
    /// Deliberately no replay, matching the topic's Volatile durability.
    void subscribe_script_command(std::function<void(const ScriptCommand&)> handler);
    void subscribe_script_result(std::function<void(const ScriptResultMessage&)> handler);

private:
    std::vector<std::function<void(const ClientAnnounce&)>> announce_handlers_;
    std::vector<std::function<void(const ResourceSampleMessage&)>> resource_handlers_;
    std::vector<std::function<void(const ComplianceReportMessage&)>> report_handlers_;
    std::vector<std::function<void(const TemplateBundleMessage&)>> bundle_handlers_;
    std::vector<std::function<void(const ScriptCommand&)>> script_command_handlers_;
    std::vector<std::function<void(const ScriptResultMessage&)>> script_result_handlers_;
    std::optional<TemplateBundleMessage> retained_bundle_;
};

[[nodiscard]] std::unique_ptr<IClientTransport> make_in_memory_client(MessageBus& bus);
[[nodiscard]] std::unique_ptr<IServerTransport> make_in_memory_server(MessageBus& bus);

}  // namespace lm::transport
