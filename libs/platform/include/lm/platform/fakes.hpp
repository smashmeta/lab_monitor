#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "lm/platform/probes.hpp"

namespace lm::platform {

class FakeResourceProbe : public IResourceProbe {
public:
    core::ResourceSample next;
    int calls = 0;

    core::ResourceSample sample() override {
        ++calls;
        return next;
    }
};

class FakeProcessProbe : public IProcessProbe {
public:
    std::vector<core::ProcessInfo> next;
    int calls = 0;

    std::vector<core::ProcessInfo> enumerate() override {
        ++calls;
        return next;
    }
};

class FakeServiceProbe : public IServiceProbe {
public:
    std::vector<core::ServiceInfo> next;
    int calls = 0;

    std::vector<core::ServiceInfo> enumerate() override {
        ++calls;
        return next;
    }
};

class FakeNetworkProbe : public INetworkProbe {
public:
    std::vector<core::NetworkAdapter> next;
    int calls = 0;

    std::vector<core::NetworkAdapter> enumerate() override {
        ++calls;
        return next;
    }
};

class FakeRegistryProbe : public IRegistryProbe {
public:
    /// Keyed by core::registry_key(). Unlisted keys read back as absent.
    std::map<std::string, core::RegistryValue> values;
    /// Every key this probe was asked for, in order.
    std::vector<std::string> reads;

    core::RegistryValue read(const core::RegistryRule& rule) override {
        const std::string key = core::registry_key(rule);
        reads.push_back(key);
        const auto found = values.find(key);
        return found == values.end() ? core::RegistryValue{} : found->second;
    }
};

class FakeDdsProbe : public IDdsProbe {
public:
    /// Keyed by core::dds_key(). An unlisted topic reads back as not on the
    /// bus, which is a legitimate answer rather than a failure.
    std::map<std::string, core::DdsTopicSample> topics;
    /// Every (domain, topic) this probe was asked for, in order. The point of
    /// recording it is that a template with several rules against one basket
    /// must still open one reader, not one per rule.
    std::vector<std::string> looks;

    core::DdsTopicSample look(std::uint32_t domain_id, const std::string& topic_name) override {
        const std::string key = core::dds_key(domain_id, topic_name);
        looks.push_back(key);
        const auto found = topics.find(key);
        return found == topics.end() ? core::DdsTopicSample{} : found->second;
    }
};

class FakeScriptRunner : public IScriptRunner {
public:
    core::ScriptOutcome next;
    std::vector<std::string> bodies;
    /// Blocks until released, for testing that execution does not sit on the
    /// monitoring thread.
    std::function<void()> before_returning;

    core::ScriptOutcome run(const std::string& body, std::chrono::seconds) override {
        bodies.push_back(body);
        if (before_returning) {
            before_returning();
        }
        return next;
    }
};

}  // namespace lm::platform
