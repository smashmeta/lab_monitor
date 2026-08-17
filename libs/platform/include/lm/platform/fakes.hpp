#pragma once

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

}  // namespace lm::platform
