#pragma once

#include <functional>

#include "lm/transport/messages.hpp"

namespace lm::transport {

/// The client half: publishes its own state, receives templates.
class IClientTransport {
public:
    virtual ~IClientTransport() = default;

    virtual void publish_announce(const ClientAnnounce& message) = 0;
    virtual void publish_resources(const ResourceSampleMessage& message) = 0;
    virtual void publish_report(const ComplianceReportMessage& message) = 0;

    /// Invoked immediately with the current bundle if one has already been
    /// published, mirroring TransientLocal durability.
    virtual void on_bundle(std::function<void(const TemplateBundleMessage&)> handler) = 0;
    virtual void on_connection_changed(std::function<void(ConnectionState)> handler) = 0;

    [[nodiscard]] virtual ConnectionState state() const = 0;
};

/// The server half: publishes templates, receives client state.
class IServerTransport {
public:
    virtual ~IServerTransport() = default;

    virtual void publish_bundle(const TemplateBundleMessage& message) = 0;

    virtual void on_announce(std::function<void(const ClientAnnounce&)> handler) = 0;
    virtual void on_resources(std::function<void(const ResourceSampleMessage&)> handler) = 0;
    virtual void on_report(std::function<void(const ComplianceReportMessage&)> handler) = 0;
    /// Fired when DDS liveliness reports a client has gone silent.
    virtual void on_client_lost(std::function<void(const core::HostId&)> handler) = 0;

    [[nodiscard]] virtual ConnectionState state() const = 0;
};

}  // namespace lm::transport
