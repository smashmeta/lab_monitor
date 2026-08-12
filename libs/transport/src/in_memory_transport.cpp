#include "lm/transport/in_memory_transport.hpp"

#include <utility>

namespace lm::transport {
namespace {

template <typename Message>
void deliver(const std::vector<std::function<void(const Message&)>>& handlers,
             const Message& message) {
    for (const auto& handler : handlers) {
        if (handler) {
            handler(message);
        }
    }
}

class InMemoryClientTransport : public IClientTransport {
public:
    explicit InMemoryClientTransport(MessageBus& bus) : bus_(bus) {}

    void publish_announce(const ClientAnnounce& message) override { bus_.publish_announce(message); }
    void publish_resources(const ResourceSampleMessage& message) override {
        bus_.publish_resources(message);
    }
    void publish_report(const ComplianceReportMessage& message) override {
        bus_.publish_report(message);
    }

    void on_bundle(std::function<void(const TemplateBundleMessage&)> handler) override {
        bus_.subscribe_bundle(std::move(handler));
    }

    void on_connection_changed(std::function<void(ConnectionState)> handler) override {
        if (handler) {
            handler(ConnectionState::Connected);
        }
    }

    [[nodiscard]] ConnectionState state() const override { return ConnectionState::Connected; }

private:
    MessageBus& bus_;
};

class InMemoryServerTransport : public IServerTransport {
public:
    explicit InMemoryServerTransport(MessageBus& bus) : bus_(bus) {}

    void publish_bundle(const TemplateBundleMessage& message) override {
        bus_.publish_bundle(message);
    }

    void on_announce(std::function<void(const ClientAnnounce&)> handler) override {
        bus_.subscribe_announce(std::move(handler));
    }
    void on_resources(std::function<void(const ResourceSampleMessage&)> handler) override {
        bus_.subscribe_resources(std::move(handler));
    }
    void on_report(std::function<void(const ComplianceReportMessage&)> handler) override {
        bus_.subscribe_report(std::move(handler));
    }
    void on_client_lost(std::function<void(const core::HostId&)>) override {
        // The in-memory bus has no liveliness concept; clients never disappear.
    }

    [[nodiscard]] ConnectionState state() const override { return ConnectionState::Connected; }

private:
    MessageBus& bus_;
};

}  // namespace

void MessageBus::publish_announce(const ClientAnnounce& message) {
    deliver(announce_handlers_, message);
}

void MessageBus::publish_resources(const ResourceSampleMessage& message) {
    deliver(resource_handlers_, message);
}

void MessageBus::publish_report(const ComplianceReportMessage& message) {
    deliver(report_handlers_, message);
}

void MessageBus::publish_bundle(const TemplateBundleMessage& message) {
    retained_bundle_ = message;
    deliver(bundle_handlers_, message);
}

void MessageBus::subscribe_announce(std::function<void(const ClientAnnounce&)> handler) {
    announce_handlers_.push_back(std::move(handler));
}

void MessageBus::subscribe_resources(std::function<void(const ResourceSampleMessage&)> handler) {
    resource_handlers_.push_back(std::move(handler));
}

void MessageBus::subscribe_report(std::function<void(const ComplianceReportMessage&)> handler) {
    report_handlers_.push_back(std::move(handler));
}

void MessageBus::subscribe_bundle(std::function<void(const TemplateBundleMessage&)> handler) {
    if (handler && retained_bundle_) {
        handler(*retained_bundle_);
    }
    bundle_handlers_.push_back(std::move(handler));
}

std::unique_ptr<IClientTransport> make_in_memory_client(MessageBus& bus) {
    return std::make_unique<InMemoryClientTransport>(bus);
}

std::unique_ptr<IServerTransport> make_in_memory_server(MessageBus& bus) {
    return std::make_unique<InMemoryServerTransport>(bus);
}

}  // namespace lm::transport
