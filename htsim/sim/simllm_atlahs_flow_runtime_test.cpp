// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "atlahs_htsim_api.h"
#include "datacenter/fat_tree_topology.h"
#include "datacenter/rnic_atlahs_runtime_factory.h"
#include "simllm_atlahs_flow_runtime.h"

namespace {

using htsim::simllm_rnic::HtsimNetworkPort;
using htsim::simllm_rnic::HtsimNetworkPortConfig;
using htsim::simllm_rnic::SimllmAtlahsFlowRuntime;
using htsim::simllm_rnic::SimllmAtlahsRuntimeConfig;
using htsim::simllm_rnic::defaultSimllmAtlahsDeviceConfig;
using htsim::simllm_rnic::makeComposedSimllmAtlahsFlowRuntime;

class CapturingApi final : public AtlahsHtsimApi {
public:
    void EventFinished(const EventOver& event) override {
        ++completion_count;
        completed_flow = *event.node;
    }

    std::uint64_t completion_count{0};
    graph_node_properties completed_flow{};
};

class CapturingNetworkRuntime final : public AtlahsFlowRuntime {
public:
    explicit CapturingNetworkRuntime(
            AtlahsTransportKind transport = AtlahsTransportKind::None,
            bool complete_synchronously = false)
        : transport_(transport),
          complete_synchronously_(complete_synchronously) {}

    void setup(
            std::uint32_t node_count,
            CompletionHandler complete_flow) override {
        if (setup_) {
            throw std::logic_error("capturing runtime setup twice");
        }
        setup_ = true;
        node_count_ = node_count;
        complete_flow_ = std::move(complete_flow);
    }

    void send(const AtlahsFlowRequest& request) override {
        if (!setup_) {
            throw std::logic_error("capturing runtime send before setup");
        }
        requests.push_back(request);
        pending_ = true;
        if (complete_synchronously_) {
            emit(request.flow_id);
        }
    }

    bool hasPendingPhysicalWork() const noexcept override {
        return pending_;
    }

    AtlahsTransportKind transportKind() const noexcept override {
        return transport_;
    }

    void emit(AtlahsFlowId flow_id) {
        complete_flow_(flow_id);
        pending_ = false;
    }

    bool setup_{false};
    bool pending_{false};
    std::uint32_t node_count_{0};
    CompletionHandler complete_flow_;
    std::vector<AtlahsFlowRequest> requests;

private:
    AtlahsTransportKind transport_;
    bool complete_synchronously_;
};

SimllmAtlahsRuntimeConfig runtimeConfig(
        std::uint64_t doorbell_service_ps,
        const std::string& policy = "rnic-nn") {
    SimllmAtlahsRuntimeConfig config;
    config.session_id = "htsim9-native-test";
    config.transport_policy = policy;
    config.seed = 19;
    config.topology_identity = "tier-a-zero-hop";
    config.htsim_source_revision = "test-htsim-revision";
    config.simllm_source_revision = "test-simllm-revision";
    config.device = defaultSimllmAtlahsDeviceConfig();
    config.device.work_queue.doorbell_service_ps = doorbell_service_ps;
    config.port.endpoint_count = 4;
    config.port.link_rate_bps = 400000000000ULL;
    return config;
}

graph_node_properties flow() {
    graph_node_properties value{};
    value.host = 3;
    value.offset = 42;
    value.target = 1;
    value.size = 4096;
    value.tag = 7;
    value.nic = 0;
    value.type = OP_SEND;
    return value;
}

TEST(SimllmAtlahsFlowRuntimeTest,
     StructuralSessionNeverConstructsTheLegacyLedger) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;

    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list,
        runtimeConfig(1000),
        std::make_unique<CapturingNetworkRuntime>(
            AtlahsTransportKind::None, true));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    ASSERT_TRUE(api.activeWqeAuthority().has_value());
    EXPECT_EQ(*api.activeWqeAuthority(),
              AtlahsWqeAuthorityMode::NativeRuntime);
    EXPECT_EQ(api.wqeLedger(), nullptr);
    ASSERT_TRUE(native->sessionConfigRecord().hardware_config_sha256.has_value());

    const graph_node_properties node = flow();
    api.Send(SendEvent(3, 1, node.size, node.tag, 0), node);
    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 100U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    EXPECT_EQ(EventList::now(), 1000U);
    EXPECT_EQ(api.completion_count, 1U);
    EXPECT_EQ(api.completed_flow.host, 3U);
    ASSERT_EQ(api.completedFlows().size(), 1U);
    EXPECT_EQ(api.completedFlows().front().completion_time_ps, 1000U);
    EXPECT_EQ(api.completedFlows().front().rq_id, 0U);
    EXPECT_NE(api.completedFlows().front().wqe_id, 0U);
    EXPECT_EQ(api.completedFlows().front().transport_object_id, 0U);
    EXPECT_EQ(api.wqeLedger(), nullptr);

    const auto& counters = native->authorityCounters();
    EXPECT_EQ(counters.native_session_constructed, 1U);
    EXPECT_EQ(counters.native_posts, 1U);
    EXPECT_EQ(counters.legacy_ledger_constructed, 0U);
    EXPECT_EQ(counters.legacy_mutations, 0U);
    const auto& api_counters = api.authorityCounters();
    EXPECT_EQ(api_counters.native_session_constructed, 1U);
    EXPECT_EQ(api_counters.native_posts, 1U);
    EXPECT_EQ(api_counters.legacy_ledger_constructed, 0U);
    EXPECT_EQ(api_counters.legacy_posts, 0U);
    EXPECT_EQ(api_counters.legacy_aborts, 0U);
    EXPECT_EQ(api_counters.legacy_mutations, 0U);
    EXPECT_EQ(native->runRecord().submitted_flows, 1U);
    EXPECT_EQ(native->runRecord().completed_flows, 1U);
    EXPECT_TRUE(native->runRecord().quiescent);
    EXPECT_TRUE(native->networkPort().liveTokens().empty());
    EXPECT_NO_THROW(native->validateQuiescent());
    EXPECT_NO_THROW(api.validateWqeQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     ComposedFactoryProjectsTheNativeDescriptorIntoTheHtsimRuntime) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;

    auto network = std::make_unique<CapturingNetworkRuntime>(
        AtlahsTransportKind::None, true);
    CapturingNetworkRuntime* captured = network.get();
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, runtimeConfig(1000), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const graph_node_properties node = flow();
    api.Send(SendEvent(3, 1, node.size, node.tag, 0), node);
    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 100U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    ASSERT_EQ(captured->requests.size(), 1U);
    const AtlahsFlowRequest& request = captured->requests.front();
    EXPECT_EQ(request.flow_id, makeAtlahsFlowId(node.host, node.offset));
    EXPECT_EQ(request.source, 3U);
    EXPECT_EQ(request.destination, 1U);
    EXPECT_EQ(request.payload_bytes, 4096U);
    EXPECT_EQ(request.start_time_ps, 1000U);
    EXPECT_EQ(request.tag, 7U);
    EXPECT_EQ(captured->node_count_, 4U);
    EXPECT_TRUE(native->networkPort().hasBoundRuntime());
    EXPECT_EQ(EventList::now(), 1000U);
    EXPECT_EQ(api.completion_count, 1U);
    EXPECT_NO_THROW(native->validateQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     ExistingHtsimRuntimeFactoryFeedsTheComposedPort) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;

    auto network = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::PacketizedManifold,
        RnicPacketizedManifoldRuntimeConfig{
            UINT64_C(400000000000),
            RnicDataPacketizationConfig(4096, 0),
            0});
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, runtimeConfig(0), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const graph_node_properties node = flow();
    api.Send(SendEvent(3, 1, node.size, node.tag, 0), node);
    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 100U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    EXPECT_EQ(api.completion_count, 1U);
    EXPECT_GT(EventList::now(), 0U);
    EXPECT_TRUE(native->networkPort().hasBoundRuntime());
    EXPECT_EQ(native->networkPort().issued().size(), 1U);
    EXPECT_EQ(native->networkPort().terminals().size(), 1U);
    EXPECT_EQ(api.wqeLedger(), nullptr);
    EXPECT_NO_THROW(native->validateQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     RuntimeBoundOverlappingFlowsRetainNativeFifoWithoutAdapterDrops) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;

    auto network = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::PacketizedManifold,
        RnicPacketizedManifoldRuntimeConfig{
            UINT64_C(400000000000),
            RnicDataPacketizationConfig(4096, 0),
            0});
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, runtimeConfig(0), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    std::vector<graph_node_properties> nodes(3, flow());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        nodes[index].offset = static_cast<std::uint32_t>(42 + index);
        nodes[index].target = static_cast<std::uint32_t>(index);
        api.Send(
            SendEvent(
                3,
                static_cast<int>(index),
                nodes[index].size,
                nodes[index].tag,
                0),
            nodes[index]);
    }

    EXPECT_EQ(native->networkPort().config().capacity, 1U);
    EXPECT_EQ(native->networkPort().effectiveCapacity(), 256U);
    ASSERT_EQ(native->networkPort().issued().size(), 3U);
    for (const auto& issued : native->networkPort().issued()) {
        EXPECT_EQ(issued.accepted_at_ps, 0U);
    }

    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 1000U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    ASSERT_EQ(api.completedFlows().size(), 3U);
    ASSERT_EQ(native->networkPort().terminals().size(), 3U);
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_EQ(api.completedFlows()[index].sq_post_sequence, index + 1);
        EXPECT_EQ(api.completedFlows()[index].sq_dispatch_sequence, index + 1);
        EXPECT_EQ(api.completedFlows()[index].transport_object_id, 0U);
        EXPECT_EQ(native->networkPort().terminals()[index].kind,
                  simllm::rnic::NetworkEventKind::Delivered);
        if (index != 0) {
            EXPECT_LT(api.completedFlows()[index - 1].completion_time_ps,
                      api.completedFlows()[index].completion_time_ps);
        }
    }
    EXPECT_NO_THROW(native->validateQuiescent());
    EXPECT_NO_THROW(api.validateWqeQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     CompletionProjectionUsesStableDirectedTransportIdentity) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;

    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list,
        runtimeConfig(0, "rnic-cn"),
        std::make_unique<CapturingNetworkRuntime>(
            AtlahsTransportKind::RnicCnLinkPair, true));
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const graph_node_properties node = flow();
    api.Send(SendEvent(3, 1, node.size, node.tag, 0), node);
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_TRUE(EventList::doNextEvent());
    }

    ASSERT_EQ(api.completedFlows().size(), 1U);
    EXPECT_EQ(api.completedFlows().front().transport_kind,
              AtlahsTransportKind::RnicCnLinkPair);
    EXPECT_EQ(
        api.completedFlows().front().transport_object_id,
        atlahsTransportObjectId(
            4, AtlahsTransportKind::RnicCnLinkPair, 3, 1));
    EXPECT_NE(api.completedFlows().front().transport_object_id, 1U);
}

TEST(SimllmAtlahsFlowRuntimeTest,
     RuntimeCompletionRejectsUnknownAndDuplicateFlowTokensAtomically) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;

    auto network = std::make_unique<CapturingNetworkRuntime>();
    CapturingNetworkRuntime* captured = network.get();
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, runtimeConfig(0), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const graph_node_properties node = flow();
    api.Send(SendEvent(3, 1, node.size, node.tag, 0), node);
    ASSERT_EQ(captured->requests.size(), 1U);
    EXPECT_THROW(captured->emit(UINT64_C(999)), std::invalid_argument);
    EXPECT_TRUE(native->networkPort().terminals().empty());
    EXPECT_EQ(native->networkPort().liveTokens().size(), 1U);

    captured->emit(captured->requests.front().flow_id);
    EXPECT_THROW(
        captured->emit(captured->requests.front().flow_id),
        std::invalid_argument);
    EXPECT_TRUE(native->networkPort().terminals().empty());
    EXPECT_EQ(native->networkPort().liveTokens().size(), 1U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(api.completion_count, 1U);
    EXPECT_TRUE(native->networkPort().liveTokens().empty());
    EXPECT_NO_THROW(native->validateQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     ComposedFactoryRejectsPolicyAndTransportMismatch) {
    EventList event_list;
    SimllmAtlahsRuntimeConfig config = runtimeConfig(0, "rnic-cn");
    EXPECT_THROW(
        makeComposedSimllmAtlahsFlowRuntime(
            event_list,
            config,
            std::make_unique<CapturingNetworkRuntime>(
                AtlahsTransportKind::None)),
        std::invalid_argument);
}

TEST(SimllmAtlahsFlowRuntimeTest,
     DualAuthorityRejectsBeforeRuntimeConstruction) {
    EventList event_list;
    SimllmAtlahsRuntimeConfig config = runtimeConfig(0);
    config.authority.native_session_enabled = true;
    config.authority.legacy_ledger_enabled = true;
    try {
        SimllmAtlahsFlowRuntime runtime(
            event_list,
            config,
            std::make_unique<CapturingNetworkRuntime>());
        FAIL() << "dual authority construction unexpectedly succeeded";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(
            error.what(),
            "structural and bypass authorities are mutually exclusive");
    }
}

TEST(SimllmAtlahsFlowRuntimeTest,
     HardwareHashIsIndependentOfTransportPolicy) {
    HtsimNetworkPort port_nn(HtsimNetworkPortConfig{});
    HtsimNetworkPort port_cn(HtsimNetworkPortConfig{});
    HtsimNetworkPort port_dcqcn(HtsimNetworkPortConfig{});
    auto config = defaultSimllmAtlahsDeviceConfig();

    simllm::rnic::RnicDeviceAttachments nn_attachment;
    nn_attachment.network_port = &port_nn;
    simllm::rnic::RnicDeviceAttachments cn_attachment;
    cn_attachment.network_port = &port_cn;
    simllm::rnic::RnicDeviceAttachments dcqcn_attachment;
    dcqcn_attachment.network_port = &port_dcqcn;
    simllm::rnic::RnicDevice nn(config, nn_attachment);
    simllm::rnic::RnicDevice cn(config, cn_attachment);
    simllm::rnic::RnicDevice dcqcn(config, dcqcn_attachment);

    const auto nn_record = simllm::rnic::makeStructuralSessionConfigRecord(
        "nn", "rnic-nn", nn);
    const auto cn_record = simllm::rnic::makeStructuralSessionConfigRecord(
        "cn", "rnic-cn", cn);
    const auto dcqcn_record = simllm::rnic::makeStructuralSessionConfigRecord(
        "dcqcn", "dcqcn", dcqcn);
    EXPECT_EQ(nn_record.hardware_config_sha256,
              cn_record.hardware_config_sha256);
    EXPECT_EQ(nn_record.hardware_config_sha256,
              dcqcn_record.hardware_config_sha256);
}

}  // namespace
