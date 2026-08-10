// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "atlahs_htsim_api.h"
#include "simllm_atlahs_flow_runtime.h"

namespace {

using htsim::simllm_rnic::HtsimNetworkPort;
using htsim::simllm_rnic::HtsimNetworkPortConfig;
using htsim::simllm_rnic::SimllmAtlahsFlowRuntime;
using htsim::simllm_rnic::SimllmAtlahsRuntimeConfig;
using htsim::simllm_rnic::defaultSimllmAtlahsDeviceConfig;

class CapturingApi final : public AtlahsHtsimApi {
public:
    void EventFinished(const EventOver& event) override {
        ++completion_count;
        completed_flow = *event.node;
    }

    std::uint64_t completion_count{0};
    graph_node_properties completed_flow{};
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

    auto runtime = std::make_unique<SimllmAtlahsFlowRuntime>(
        event_list, runtimeConfig(1000));
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

    EXPECT_EQ(EventList::now(), 82920U);
    EXPECT_EQ(api.completion_count, 1U);
    EXPECT_EQ(api.completed_flow.host, 3U);
    ASSERT_EQ(api.completedFlows().size(), 1U);
    EXPECT_EQ(api.completedFlows().front().completion_time_ps, 82920U);
    EXPECT_EQ(api.completedFlows().front().rq_id, 0U);
    EXPECT_NE(api.completedFlows().front().wqe_id, 0U);
    EXPECT_EQ(api.completedFlows().front().transport_object_id, 1U);
    EXPECT_EQ(api.wqeLedger(), nullptr);

    const auto& counters = native->authorityAudit().counters();
    EXPECT_EQ(counters.native_session_constructed, 1U);
    EXPECT_EQ(counters.native_posts, 1U);
    EXPECT_EQ(counters.legacy_ledger_constructed, 0U);
    EXPECT_EQ(counters.legacy_mutations, 0U);
    EXPECT_EQ(native->runRecord().submitted_flows, 1U);
    EXPECT_EQ(native->runRecord().completed_flows, 1U);
    EXPECT_TRUE(native->runRecord().quiescent);
    EXPECT_TRUE(native->networkPort().liveTokens().empty());
    EXPECT_NO_THROW(native->validateQuiescent());
    EXPECT_NO_THROW(api.validateWqeQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     DualAuthorityRejectsBeforeRuntimeConstruction) {
    EventList event_list;
    SimllmAtlahsRuntimeConfig config = runtimeConfig(0);
    config.authority.native_session_enabled = true;
    config.authority.legacy_ledger_enabled = true;
    try {
        SimllmAtlahsFlowRuntime runtime(event_list, config);
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
