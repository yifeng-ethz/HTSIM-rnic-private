// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "atlahs_htsim_api.h"
#include "datacenter/dcqcn_atlahs_runtime.h"
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
     AbiV2ProjectsRealPacketIssuesIntoTheNativeWqeTimeline) {
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
            RnicDataPacketizationConfig(4096, 64),
            0});
    SimllmAtlahsRuntimeConfig config = runtimeConfig(1000);
    config.port.network_abi_version =
        simllm::rnic::kNetworkPortAbiVersionV2;
    config.port.data_header_bytes = 64;
    config.port.max_wire_packet_bytes = 4096;
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, std::move(config), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    graph_node_properties node = flow();
    node.size = 5000;
    api.Send(SendEvent(3, 1, node.size, node.tag, 0), node);
    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 100U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    ASSERT_EQ(native->device(3).records().size(), 1U);
    const auto& record = native->device(3).records().front();
    EXPECT_EQ(record.timeline.first_packet_at_ps, 1000U);
    EXPECT_EQ(record.timeline.last_packet_at_ps, 144200U);
    EXPECT_EQ(record.timeline.network_outcome_at_ps, 185480U);
    EXPECT_EQ(native->networkPort().packetEvents().size(), 8U);
    EXPECT_EQ(native->networkPort().packetEvents().front().kind,
              simllm::rnic::NetworkEventKind::PacketTxStarted);
    std::uint32_t tx_starts[2]{};
    std::uint32_t tx_finishes[2]{};
    std::uint32_t rx_arrivals[2]{};
    std::uint32_t deliveries[2]{};
    for (const simllm::rnic::NetworkEvent& event :
         native->networkPort().packetEvents()) {
        ASSERT_LT(event.packet_index, 2U);
        switch (event.kind) {
        case simllm::rnic::NetworkEventKind::PacketTxStarted:
            ++tx_starts[event.packet_index];
            break;
        case simllm::rnic::NetworkEventKind::PacketTxFinished:
            ++tx_finishes[event.packet_index];
            break;
        case simllm::rnic::NetworkEventKind::PacketRxArrived:
            ++rx_arrivals[event.packet_index];
            break;
        case simllm::rnic::NetworkEventKind::Delivered:
            ++deliveries[event.packet_index];
            break;
        default:
            ADD_FAILURE() << "partial-packet cell emitted an unexpected event";
            break;
        }
        if (event.packet_index == 0) {
            EXPECT_EQ(event.payload_offset_bytes, 0U);
            EXPECT_EQ(event.payload_bytes, 4032U);
            EXPECT_EQ(event.wire_bytes, 4096U);
            if (event.kind
                == simllm::rnic::NetworkEventKind::PacketTxStarted) {
                EXPECT_EQ(event.event_time_ps, 1000U);
            }
            if (event.kind
                == simllm::rnic::NetworkEventKind::PacketTxFinished) {
                EXPECT_EQ(event.event_time_ps, 82920U);
            }
            if (event.kind
                == simllm::rnic::NetworkEventKind::PacketRxArrived) {
                EXPECT_EQ(event.event_time_ps, 82920U);
            }
            if (event.kind == simllm::rnic::NetworkEventKind::Delivered) {
                EXPECT_EQ(event.event_time_ps, 164840U);
            }
        } else {
            EXPECT_EQ(event.packet_index, 1U);
            EXPECT_EQ(event.payload_offset_bytes, 4032U);
            EXPECT_EQ(event.payload_bytes, 968U);
            EXPECT_EQ(event.wire_bytes, 1032U);
            if (event.kind
                == simllm::rnic::NetworkEventKind::PacketTxStarted) {
                EXPECT_EQ(event.event_time_ps, 144200U);
            }
            if (event.kind
                == simllm::rnic::NetworkEventKind::PacketTxFinished) {
                EXPECT_EQ(event.event_time_ps, 164840U);
            }
            if (event.kind
                == simllm::rnic::NetworkEventKind::PacketRxArrived) {
                EXPECT_EQ(event.event_time_ps, 164840U);
            }
            if (event.kind
                == simllm::rnic::NetworkEventKind::Delivered) {
                EXPECT_EQ(event.event_time_ps, 185480U);
            }
        }
    }
    for (std::uint32_t packet_index = 0; packet_index < 2; ++packet_index) {
        EXPECT_EQ(tx_starts[packet_index], 1U);
        EXPECT_EQ(tx_finishes[packet_index], 1U);
        EXPECT_EQ(rx_arrivals[packet_index], 1U);
        EXPECT_EQ(deliveries[packet_index], 1U);
    }
    EXPECT_EQ(api.completion_count, 1U);
    EXPECT_NO_THROW(native->validateQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     AbiV2RelaysRealDcqcnPolicyAndFabricControlEvents) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 64;

    DcqcnAtlahsRuntimeConfig network_config;
    const std::filesystem::path topology =
        std::filesystem::path(__FILE__).parent_path() /
        "../../experiments/rnic_multibaseline/topologies/clos_64_400g.topo";
    network_config.topology_file = topology.lexically_normal().string();
    network_config.ns_tm3_shared_buffer_bytes = 1024 * 1024;
    network_config.ns_tm3_egress_buffer_bytes = 1024 * 1024;
    network_config.ecn_kmin_bytes = 0;
    network_config.ecn_kmax_bytes = 4096;
    network_config.ecn_pmax_ppm = 1000000;
    network_config.ecn_seed = 9;
    network_config.pfc_low_threshold_bytes = 4096;
    network_config.pfc_high_threshold_bytes = 8192;
    network_config.packet_event_observations = true;
    network_config.congestion_event_observations = true;
    network_config.pfc_event_observations = true;
    auto network = std::make_unique<DcqcnAtlahsRuntime>(
        event_list, network_config, 64);

    SimllmAtlahsRuntimeConfig config = runtimeConfig(0, "dcqcn");
    config.port.endpoint_count = 64;
    config.port.network_abi_version =
        simllm::rnic::kNetworkPortAbiVersionV2;
    config.port.data_header_bytes = 64;
    config.port.max_wire_packet_bytes = 4096;
    config.port.congestion = true;
    config.port.control_frames = true;
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, std::move(config), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    for (std::uint32_t source = 0; source < 8; ++source) {
        graph_node_properties node = flow();
        node.host = source;
        node.offset = 100 + source;
        node.target = 63;
        node.size = 64 * 1024;
        node.tag = 9;
        api.Send(
            SendEvent(source, 63, node.size, node.tag, 0), node);
    }
    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 100000U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    EXPECT_EQ(api.completion_count, 8U);
    EXPECT_TRUE(native->networkPort().capabilities().packet_attempt_events);
    EXPECT_TRUE(native->networkPort().capabilities().ecn_cnp_events);
    EXPECT_TRUE(native->networkPort().capabilities().policy_update_events);
    EXPECT_TRUE(native->networkPort().capabilities().pfc_events);
    EXPECT_FALSE(native->networkPort().capabilities().dynamic_link_events);
    const auto& packet_events = native->networkPort().packetEvents();
    const auto& control_events = native->networkPort().controlEvents();
    const auto count_control = [&](simllm::rnic::NetworkEventKind kind) {
        return std::count_if(
            control_events.begin(), control_events.end(),
            [kind](const simllm::rnic::NetworkEvent& event) {
                return event.kind == kind;
            });
    };
    EXPECT_GT(count_control(simllm::rnic::NetworkEventKind::EcnMarked), 0);
    EXPECT_GT(count_control(simllm::rnic::NetworkEventKind::CnpReceived), 0);
    EXPECT_GT(count_control(simllm::rnic::NetworkEventKind::RateUpdated), 8);
    EXPECT_GT(
        count_control(simllm::rnic::NetworkEventKind::PfcFrameSubmitted),
        0);
    EXPECT_GT(count_control(simllm::rnic::NetworkEventKind::PfcPaused), 0);
    EXPECT_GT(count_control(simllm::rnic::NetworkEventKind::PfcResumed), 0);

    bool reduced_rate_seen = false;
    bool retained_cnp_seen = false;
    for (const simllm::rnic::NetworkEvent& event : control_events) {
        if (event.kind == simllm::rnic::NetworkEventKind::RateUpdated
            && event.has_effective_rate
            && event.effective_rate_bps
                   < network_config.endpoint_link_bps) {
            reduced_rate_seen = true;
        }
        if (event.kind != simllm::rnic::NetworkEventKind::CnpReceived) {
            continue;
        }
        const auto delivered = std::find_if(
            packet_events.begin(), packet_events.end(),
            [&](const simllm::rnic::NetworkEvent& candidate) {
                return candidate.kind
                           == simllm::rnic::NetworkEventKind::Delivered
                    && candidate.token == event.token
                    && candidate.event_time_ps <= event.event_time_ps;
            });
        const auto terminal = std::find_if(
            native->networkPort().terminals().begin(),
            native->networkPort().terminals().end(),
            [&](const htsim::simllm_rnic::HtsimTerminalToken& candidate) {
                return candidate.wqe_id == event.wqe_id;
            });
        if (delivered != packet_events.end()
            && terminal != native->networkPort().terminals().end()
            && event.event_time_ps < terminal->at_ps) {
            retained_cnp_seen = true;
        }
    }
    EXPECT_TRUE(reduced_rate_seen);
    EXPECT_TRUE(retained_cnp_seen);
    EXPECT_NO_THROW(native->validateQuiescent());
}

TEST(SimllmAtlahsFlowRuntimeTest,
     AbiV2RelaysTimestampedDynamicEndpointLinkTransitions) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    CapturingApi api;
    api.setEventList(&event_list);
    api.total_nodes = 64;

    DcqcnAtlahsRuntimeConfig network_config;
    const std::filesystem::path topology =
        std::filesystem::path(__FILE__).parent_path() /
        "../../experiments/rnic_multibaseline/topologies/clos_64_400g.topo";
    network_config.topology_file = topology.lexically_normal().string();
    network_config.packet_event_observations = true;
    network_config.dynamic_link_event_observations = true;
    network_config.dynamic_link_transitions = {
        DcqcnDynamicLinkTransition{1, 0, 1000, false},
        DcqcnDynamicLinkTransition{1, 0, 201000, true},
    };
    auto network = std::make_unique<DcqcnAtlahsRuntime>(
        event_list, network_config, 64);

    SimllmAtlahsRuntimeConfig config = runtimeConfig(0, "dcqcn");
    config.port.endpoint_count = 64;
    config.port.network_abi_version =
        simllm::rnic::kNetworkPortAbiVersionV2;
    config.port.data_header_bytes = 64;
    config.port.max_wire_packet_bytes = 4096;
    config.port.dynamic_link_events = true;
    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, std::move(config), std::move(network));
    SimllmAtlahsFlowRuntime* native = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    graph_node_properties node = flow();
    node.host = 0;
    node.target = 63;
    node.size = 64 * 1024;
    api.Send(SendEvent(0, 63, node.size, node.tag, 0), node);
    std::size_t iterations = 0;
    while (api.runtimeHasPendingPhysicalWork()) {
        ASSERT_LT(++iterations, 100000U);
        ASSERT_TRUE(EventList::doNextEvent());
    }

    std::vector<simllm::rnic::Picoseconds> transitions;
    for (const simllm::rnic::NetworkEvent& event :
         native->networkPort().controlEvents()) {
        if (event.kind
            == simllm::rnic::NetworkEventKind::LinkStateChanged) {
            transitions.push_back(event.event_time_ps);
        }
    }
    EXPECT_EQ(transitions,
              (std::vector<simllm::rnic::Picoseconds>{1000, 201000}));
    EXPECT_TRUE(native->networkPort().capabilities().dynamic_link_events);
    EXPECT_EQ(api.completion_count, 1U);
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
