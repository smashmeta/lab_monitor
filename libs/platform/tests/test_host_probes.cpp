#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lm/platform/fakes.hpp"
#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

Rule process_rule(RuleId id, std::string exe) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

Rule registry_rule(RuleId id, std::string key_path, std::string value_name) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = RegistryRule{RegistryHive::LocalMachine, std::move(key_path),
                                std::move(value_name), RegistryMatch::Exists, ""};
    return rule;
}

Rule service_rule(RuleId id, std::string name) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = ServiceRule{std::move(name), std::nullopt};
    return rule;
}

/// Builds a HostProbes wired to fakes, handing back raw pointers so the test can
/// inspect call counts afterwards.
struct Harness {
    FakeResourceProbe* resources = nullptr;
    FakeProcessProbe* processes = nullptr;
    FakeServiceProbe* services = nullptr;
    FakeRegistryProbe* registry = nullptr;
    FakeNetworkProbe* network = nullptr;
    FakeDdsProbe* dds = nullptr;
    std::unique_ptr<HostProbes> probes;

    explicit Harness(Capabilities caps = Capabilities{}
                                             .add(Capability::Resources)
                                             .add(Capability::Processes)
                                             .add(Capability::Services)
                                             .add(Capability::Registry)
                                             .add(Capability::Network)
                                             .add(Capability::Dds)) {
        auto resource_probe = std::make_unique<FakeResourceProbe>();
        auto process_probe = std::make_unique<FakeProcessProbe>();
        auto service_probe = std::make_unique<FakeServiceProbe>();
        auto registry_probe = std::make_unique<FakeRegistryProbe>();
        auto network_probe = std::make_unique<FakeNetworkProbe>();
        auto dds_probe = std::make_unique<FakeDdsProbe>();

        resources = resource_probe.get();
        processes = process_probe.get();
        services = service_probe.get();
        registry = registry_probe.get();
        network = network_probe.get();
        dds = dds_probe.get();

        ProbeSet set;
        set.resources = std::move(resource_probe);
        set.processes = std::move(process_probe);
        set.services = std::move(service_probe);
        set.registry = std::move(registry_probe);
        set.network = std::move(network_probe);
        set.dds = std::move(dds_probe);

        probes = std::make_unique<HostProbes>("PC-001", std::move(set), caps);
    }
};

TemplateBundle bundle_for_pc001(std::vector<Rule> rules) {
    TemplateBundle bundle;
    bundle.revision = 1;
    Template tmpl;
    tmpl.name = "Lab Workstation";
    tmpl.rules = std::move(rules);
    bundle.templates = {tmpl};
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    return bundle;
}

}  // namespace

TEST(HostProbes, AlwaysSamplesResources) {
    Harness harness;
    harness.resources->next.cpu_percent = 42.0;

    const HostFacts facts = harness.probes->collect(TemplateBundle{});

    EXPECT_EQ(facts.host_id, "PC-001");
    EXPECT_DOUBLE_EQ(facts.resources.cpu_percent, 42.0);
    EXPECT_EQ(harness.resources->calls, 1);
}

TEST(HostProbes, AlwaysEnumeratesAdaptersWithTheCapability) {
    // Unlike processes and registry, adapters are not probed lazily against the
    // rules: no rule references them, and their link state is wanted on every
    // tick regardless of what the host has been assigned.
    Harness harness;
    harness.network->next = {NetworkAdapter{"eth0", "Onboard NIC", "{eth0-guid}", AdapterType::Ethernet, LinkState::Connected}};

    const ResourceSample sample = harness.probes->sample_resources();

    ASSERT_EQ(sample.adapters.size(), 1u);
    EXPECT_EQ(sample.adapters.front().description, "Onboard NIC");
    EXPECT_EQ(harness.network->calls, 1);
}

TEST(HostProbes, ReportsNoAdaptersWithoutTheNetworkCapability) {
    Harness harness(Capabilities{}.add(Capability::Resources));
    harness.network->next = {NetworkAdapter{"eth0", "Onboard NIC", "{eth0-guid}", AdapterType::Ethernet, LinkState::Connected}};

    const ResourceSample sample = harness.probes->sample_resources();

    EXPECT_TRUE(sample.adapters.empty());
    EXPECT_EQ(harness.network->calls, 0);
    EXPECT_FALSE(harness.probes->capabilities().has(Capability::Network));
}

TEST(HostProbes, DropsTheNetworkCapabilityWhenNoProbeIsSupplied) {
    // The same honesty rule the other probes follow: a capability is only
    // advertised when something can actually serve it.
    ProbeSet set;
    set.resources = std::make_unique<FakeResourceProbe>();
    HostProbes probes("PC-001", std::move(set),
                       Capabilities{}.add(Capability::Resources).add(Capability::Network));

    EXPECT_FALSE(probes.capabilities().has(Capability::Network));
    EXPECT_TRUE(probes.sample_resources().adapters.empty());
}

TEST(HostProbes, EmptyBundleProbesNothingElse) {
    Harness harness;
    (void)harness.probes->collect(TemplateBundle{});

    EXPECT_EQ(harness.processes->calls, 0);
    EXPECT_EQ(harness.services->calls, 0);
    EXPECT_TRUE(harness.registry->reads.empty());
}

TEST(HostProbes, EnumeratesProcessesOnlyWhenAProcessRuleExists) {
    Harness harness;
    (void)harness.probes->collect(bundle_for_pc001({service_rule("s1", "spooler")}));
    EXPECT_EQ(harness.processes->calls, 0);

    (void)harness.probes->collect(bundle_for_pc001({process_rule("p1", "a.exe")}));
    EXPECT_EQ(harness.processes->calls, 1);
    EXPECT_EQ(harness.services->calls, 1);  // one from the first collect
}

TEST(HostProbes, EnumeratesServicesOnlyWhenAServiceRuleExists) {
    Harness harness;
    (void)harness.probes->collect(bundle_for_pc001({process_rule("p1", "a.exe")}));
    EXPECT_EQ(harness.services->calls, 0);
}

TEST(HostProbes, IgnoresRulesAssignedToOtherHosts) {
    Harness harness;
    TemplateBundle bundle = bundle_for_pc001({process_rule("p1", "a.exe")});
    bundle.assignments.clear();
    bundle.assignments["PC-999"] = {"Lab Workstation"};

    (void)harness.probes->collect(bundle);
    EXPECT_EQ(harness.processes->calls, 0);
}

TEST(HostProbes, ReadsOnlyTheReferencedRegistryValues) {
    Harness harness;
    harness.registry->values["HKLM\\SOFTWARE\\Acme\\\\Version"] =
        RegistryValue{true, "4.2", ""};

    const HostFacts facts = harness.probes->collect(
        bundle_for_pc001({registry_rule("g1", "SOFTWARE\\Acme", "Version")}));

    EXPECT_EQ(harness.registry->reads,
              (std::vector<std::string>{"HKLM\\SOFTWARE\\Acme\\\\Version"}));
    ASSERT_TRUE(facts.registry.contains("HKLM\\SOFTWARE\\Acme\\\\Version"));
    EXPECT_EQ(facts.registry.at("HKLM\\SOFTWARE\\Acme\\\\Version").data, "4.2");
}

TEST(HostProbes, ReadsEachRegistryKeyOnlyOnceWhenTwoRulesShareIt) {
    Harness harness;
    (void)harness.probes->collect(bundle_for_pc001({registry_rule("g1", "SOFTWARE\\Acme", "Version"),
                                              registry_rule("g2", "SOFTWARE\\Acme", "Version")}));
    EXPECT_EQ(harness.registry->reads.size(), 1u);
}

TEST(HostProbes, SkipsRegistryEntirelyWithoutTheCapability) {
    Harness harness{Capabilities{}.add(Capability::Resources).add(Capability::Processes)};
    (void)harness.probes->collect(bundle_for_pc001({registry_rule("g1", "SOFTWARE\\Acme", "Version")}));
    EXPECT_TRUE(harness.registry->reads.empty());
}

TEST(HostProbes, MissingProbeImplementationIsTreatedAsNoCapability) {
    ProbeSet set;
    set.resources = std::make_unique<FakeResourceProbe>();
    // processes, services and registry deliberately left null.
    HostProbes probes{"PC-001", std::move(set), platform_capabilities()};

    EXPECT_FALSE(probes.capabilities().has(Capability::Processes));
    EXPECT_FALSE(probes.capabilities().has(Capability::Registry));
    EXPECT_TRUE(probes.capabilities().has(Capability::Resources));
}

TEST(HostProbes, SampleResourcesDoesNotTouchOtherProbes) {
    Harness harness;
    (void)harness.probes->sample_resources();
    EXPECT_EQ(harness.resources->calls, 1);
    EXPECT_EQ(harness.processes->calls, 0);
}

TEST(LocalHostName, IsNotEmpty) {
    EXPECT_FALSE(local_host_name().empty());
}

namespace {

Rule dds_topic_rule(RuleId id, std::uint32_t domain, std::string topic) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = DdsTopicRule{domain, std::move(topic)};
    return rule;
}

Rule dds_value_rule(RuleId id, std::uint32_t domain, std::string topic, std::string path) {
    Rule rule;
    rule.id = std::move(id);
    rule.payload = DdsValueRule{domain, std::move(topic), std::move(path), DdsMatch::Equals, "2"};
    return rule;
}

}  // namespace

TEST(HostProbes, LooksAtTheBusOnlyWhenADdsRuleExists) {
    Harness harness;
    (void)harness.probes->collect(bundle_for_pc001({process_rule("r1", "chrome.exe")}));
    EXPECT_TRUE(harness.dds->looks.empty()) << "no DDS rule, so no participant should be created";
}

TEST(HostProbes, LooksAtEachTopicOnlyOnceWhenSeveralRulesShareIt) {
    // Each look costs a participant and a discovery wait, so three rules about
    // one basket must not mean three of them.
    Harness harness;
    (void)harness.probes->collect(bundle_for_pc001({
        dds_topic_rule("r1", 42, "Basket"),
        dds_value_rule("r2", 42, "Basket", "items_.length"),
        dds_value_rule("r3", 42, "Basket", "status"),
    }));
    EXPECT_EQ(harness.dds->looks, std::vector<std::string>{dds_key(42, "Basket")});
}

TEST(HostProbes, TellsTheSameTopicOnTwoDomainsApart) {
    Harness harness;
    (void)harness.probes->collect(bundle_for_pc001({
        dds_topic_rule("r1", 42, "Basket"),
        dds_topic_rule("r2", 7, "Basket"),
    }));
    EXPECT_EQ(harness.dds->looks.size(), 2u);
}

TEST(HostProbes, RecordsWhatTheLookFound) {
    Harness harness;
    DdsTopicSample found;
    found.topic_found = true;
    found.has_sample = true;
    found.json = R"({"items_":[1,2]})";
    harness.dds->topics.emplace(dds_key(42, "Basket"), found);

    const HostFacts facts = harness.probes->collect(bundle_for_pc001({dds_topic_rule("r1", 42, "Basket")}));
    ASSERT_TRUE(facts.dds.contains(dds_key(42, "Basket")));
    EXPECT_EQ(facts.dds.at(dds_key(42, "Basket")).json, found.json);
}

TEST(HostProbes, SkipsTheBusEntirelyWithoutTheCapability) {
    Harness harness(Capabilities{}.add(Capability::Resources));
    const HostFacts facts =
        harness.probes->collect(bundle_for_pc001({dds_topic_rule("r1", 42, "Basket")}));
    EXPECT_TRUE(harness.dds->looks.empty());
    // Absent rather than empty: evaluate() reads "no entry" as "never probed",
    // which is an Error, and must not be confused with "the topic is not there".
    EXPECT_FALSE(facts.dds.contains(dds_key(42, "Basket")));
}

TEST(HostProbes, DropsTheDdsCapabilityWhenNoProbeIsSupplied) {
    // What --offline relies on: no probe means the client never claims it can
    // inspect a bus, and every DDS rule reports NotApplicable instead.
    ProbeSet set;
    set.resources = std::make_unique<FakeResourceProbe>();
    HostProbes probes("PC-001", std::move(set),
                      Capabilities{}.add(Capability::Resources).add(Capability::Dds));
    EXPECT_FALSE(probes.capabilities().has(Capability::Dds));
}
