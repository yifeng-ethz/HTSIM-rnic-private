// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "eventlist.h"
#include "simllm_htsim_network_port.h"

namespace {

using htsim::simllm_rnic::HtsimNetworkPort;
using htsim::simllm_rnic::HtsimNetworkPortConfig;
using simllm::rnic::DropLocation;
using simllm::rnic::DropReason;
using simllm::rnic::NetworkEventKind;
using simllm::rnic::NetworkEventScope;
using simllm::rnic::NetworkSubmitStatus;
using simllm::rnic::NetworkTxDescriptor;

class ThrowingFlowRuntime final : public AtlahsFlowRuntime {
public:
    void setup(
            std::uint32_t,
            CompletionHandler complete_flow) override {
        completion_ = std::move(complete_flow);
    }

    void send(const AtlahsFlowRequest& request) override {
        requests.push_back(request);
        throw std::runtime_error("injected htsim admission failure");
    }

    bool hasPendingPhysicalWork() const noexcept override { return false; }

    CompletionHandler completion_;
    std::vector<AtlahsFlowRequest> requests;
};

class DeferredFlowRuntime final : public AtlahsFlowRuntime {
public:
    void setup(
            std::uint32_t,
            CompletionHandler complete_flow) override {
        completion_ = std::move(complete_flow);
    }

    void send(const AtlahsFlowRequest& request) override {
        requests.push_back(request);
    }

    bool hasPendingPhysicalWork() const noexcept override {
        return requests.size() != completed;
    }

    void completeAll() {
        for (; completed < requests.size(); ++completed) {
            completion_(requests[completed].flow_id);
        }
    }

    CompletionHandler completion_;
    std::vector<AtlahsFlowRequest> requests;
    std::size_t completed{0};
};

class VocabularyFlowRuntime final : public AtlahsFlowRuntime {
public:
    AtlahsRuntimeEventCapabilities
    eventCapabilities() const noexcept override {
        return {true, true, true, true, true};
    }

    void setEventHandler(EventHandler handler) override {
        events_ = std::move(handler);
    }

    void setup(
            std::uint32_t,
            CompletionHandler complete_flow) override {
        completion_ = std::move(complete_flow);
    }

    void send(const AtlahsFlowRequest& request) override {
        AtlahsRuntimeEvent event;
        event.flow_id = request.flow_id;
        event.packet_index = 0;
        event.payload_offset_bytes = 0;
        event.payload_bytes = request.payload_bytes;
        event.wire_bytes = request.payload_bytes;
        event.packet_kind = AtlahsRuntimePacketKind::Data;
        event.kind = AtlahsRuntimeEventKind::PacketTxStarted;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::EcnMarked;
        event.ecn_marked = true;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::CnpReceived;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::PacketTxFinished;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::PacketRxArrived;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::PacketDelivered;
        events_(event);

        event = AtlahsRuntimeEvent{};
        event.flow_id = request.flow_id;
        event.policy_context_token = 9001;
        event.kind = AtlahsRuntimeEventKind::EligibilityUpdated;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::RateUpdated;
        event.has_effective_rate = true;
        event.effective_rate_bps = 400000000000ULL;
        events_(event);

        event = AtlahsRuntimeEvent{};
        event.kind = AtlahsRuntimeEventKind::PfcFrameSubmitted;
        event.source = 3;
        event.destination = 1;
        event.link_id = 17;
        event.priority = 3;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::PfcPaused;
        event.pause_quanta = 64;
        events_(event);
        event.kind = AtlahsRuntimeEventKind::PfcResumed;
        event.pause_quanta = 0;
        events_(event);

        event = AtlahsRuntimeEvent{};
        event.kind = AtlahsRuntimeEventKind::LinkStateChanged;
        event.link_id = 17;
        event.link_state = AtlahsRuntimeLinkState::Down;
        events_(event);
        completion_(request.flow_id);
    }

    bool hasPendingPhysicalWork() const noexcept override { return false; }

private:
    CompletionHandler completion_;
    EventHandler events_;
};

NetworkTxDescriptor descriptor(
        std::uint64_t wqe_id, std::uint64_t flow_id) {
    NetworkTxDescriptor value;
    value.wqe_id = wqe_id;
    value.wr_id = wqe_id + 100;
    value.flow_id = flow_id;
    value.flow_tag = 7;
    value.policy_context_token = 9001;
    value.source = 3;
    value.destination = 1;
    value.qpn = 17;
    value.traffic_class = 3;
    value.payload_bytes = 4096;
    value.eligible_at_ps = 0;
    return value;
}

TEST(HtsimNetworkPortTest, SerializesWholeFlowsAndConservesTokens) {
    HtsimNetworkPortConfig config;
    config.capacity = 1;
    config.link_rate_bps = 400000000000ULL;
    config.endpoint_count = 4;
    HtsimNetworkPort port(config);

    const auto first = port.trySubmit(descriptor(1, 101), 0);
    ASSERT_EQ(first.status, NetworkSubmitStatus::Accepted);
    EXPECT_EQ(first.token, 1U);
    ASSERT_EQ(port.issued().size(), 1U);
    EXPECT_EQ(port.issued().front().port_tx_at_ps, 0U);

    const auto busy = port.trySubmit(descriptor(2, 102), 0);
    EXPECT_EQ(busy.status, NetworkSubmitStatus::Busy);
    EXPECT_TRUE(busy.has_retry_time);
    EXPECT_EQ(busy.retry_at_ps, 81920U);
    EXPECT_TRUE(port.takeDue(81919).empty());

    const auto first_terminal = port.takeDue(81920);
    ASSERT_EQ(first_terminal.size(), 1U);
    EXPECT_EQ(first_terminal.front().kind, NetworkEventKind::Delivered);
    EXPECT_EQ(first_terminal.front().event_time_ps, 81920U);

    const auto second = port.trySubmit(descriptor(2, 102), 81920);
    ASSERT_EQ(second.status, NetworkSubmitStatus::Accepted);
    EXPECT_NE(first.token, second.token);
    const auto second_terminal = port.takeDue(163840);
    ASSERT_EQ(second_terminal.size(), 1U);
    EXPECT_EQ(second_terminal.front().event_time_ps, 163840U);
    EXPECT_TRUE(port.liveTokens().empty());
    EXPECT_FALSE(port.hasPendingPhysicalWork());
    EXPECT_EQ(port.issued().size(), 2U);
    EXPECT_EQ(port.terminals().size(), 2U);
}

TEST(HtsimNetworkPortTest, CapacityLanesFinishAtTheSameBoundary) {
    HtsimNetworkPortConfig config;
    config.capacity = 2;
    config.link_rate_bps = 400000000000ULL;
    config.endpoint_count = 4;
    HtsimNetworkPort port(config);

    EXPECT_EQ(port.trySubmit(descriptor(1, 201), 0).status,
              NetworkSubmitStatus::Accepted);
    EXPECT_EQ(port.trySubmit(descriptor(2, 202), 0).status,
              NetworkSubmitStatus::Accepted);
    const auto events = port.takeDue(81920);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].event_time_ps, 81920U);
    EXPECT_EQ(events[1].event_time_ps, 81920U);
    EXPECT_EQ(port.ownerSource(events[0].token), 3U);
    EXPECT_EQ(port.ownerSource(events[1].token), 3U);
}

TEST(HtsimNetworkPortTest, ControlledDropKeepsTypedFabricEvidence) {
    HtsimNetworkPortConfig config;
    config.drop_first = true;
    config.endpoint_count = 4;
    HtsimNetworkPort port(config);

    ASSERT_EQ(port.trySubmit(descriptor(1, 301), 0).status,
              NetworkSubmitStatus::Accepted);
    const auto events = port.takeDue(81920);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().kind, NetworkEventKind::Dropped);
    EXPECT_EQ(events.front().drop_location, DropLocation::Fabric);
    EXPECT_EQ(events.front().drop_reason, DropReason::Injected);
    EXPECT_TRUE(port.liveTokens().empty());
}

TEST(HtsimNetworkPortTest,
     AbiV2PacketizesTheActualUnboundSerializerTimeline) {
    HtsimNetworkPortConfig config;
    config.network_abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    config.link_rate_bps = 400000000000ULL;
    config.max_wire_packet_bytes = 4096;
    config.endpoint_count = 4;
    HtsimNetworkPort port(config);
    NetworkTxDescriptor value = descriptor(1, 351);
    value.abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    value.payload_bytes = 8192;

    const auto accepted = port.trySubmit(value, 1000);
    ASSERT_EQ(accepted.status, NetworkSubmitStatus::Accepted);
    EXPECT_EQ(port.capabilities().abi_version,
              simllm::rnic::kNetworkPortAbiVersionV2);
    EXPECT_TRUE(port.capabilities().packet_attempt_events);

    const auto first = port.takeDue(1000);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first.front().kind, NetworkEventKind::PacketTxStarted);
    EXPECT_EQ(first.front().scope, NetworkEventScope::PacketAttempt);
    EXPECT_EQ(first.front().parent_token, accepted.token);
    EXPECT_NE(first.front().token, accepted.token);

    const auto rest = port.takeDue(164840);
    ASSERT_EQ(rest.size(), 8U);
    EXPECT_EQ(rest.back().scope, NetworkEventScope::FlowExtent);
    EXPECT_EQ(rest.back().kind, NetworkEventKind::Delivered);
    EXPECT_EQ(rest.back().event_time_ps, 164840U);
    ASSERT_EQ(port.packetEvents().size(), 8U);
    EXPECT_EQ(port.packetEvents()[4].kind,
              NetworkEventKind::PacketTxStarted);
    EXPECT_EQ(port.packetEvents()[4].event_time_ps, 82920U);
    EXPECT_EQ(port.terminals().size(), 1U);
    EXPECT_FALSE(port.hasPendingPhysicalWork());
}

TEST(HtsimNetworkPortTest,
     AbiV2RelaysEveryAdvertisedRuntimeObservationWithoutFabrication) {
    EventList event_list;
    (void)event_list;
    VocabularyFlowRuntime runtime;
    HtsimNetworkPortConfig config;
    config.network_abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    config.endpoint_count = 4;
    config.congestion = true;
    config.control_frames = true;
    config.dynamic_link_events = true;
    HtsimNetworkPort port(config);
    std::vector<std::uint64_t> ready_times;
    port.bindRuntime(
        runtime,
        4,
        4,
        [&](std::uint64_t at_ps) { ready_times.push_back(at_ps); });
    const auto capabilities = port.capabilities();
    EXPECT_TRUE(capabilities.packet_attempt_events);
    EXPECT_TRUE(capabilities.ecn_cnp_events);
    EXPECT_TRUE(capabilities.policy_update_events);
    EXPECT_TRUE(capabilities.pfc_events);
    EXPECT_TRUE(capabilities.dynamic_link_events);

    NetworkTxDescriptor value = descriptor(1, 361);
    value.abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    const auto accepted = port.trySubmit(value, 0);
    ASSERT_EQ(accepted.status, NetworkSubmitStatus::Accepted);
    const auto events = port.takeDue(0);
    ASSERT_EQ(events.size(), 13U);
    const std::vector<NetworkEventKind> expected{
        NetworkEventKind::PacketTxStarted,
        NetworkEventKind::EcnMarked,
        NetworkEventKind::CnpReceived,
        NetworkEventKind::PacketTxFinished,
        NetworkEventKind::PacketRxArrived,
        NetworkEventKind::Delivered,
        NetworkEventKind::EligibilityUpdated,
        NetworkEventKind::RateUpdated,
        NetworkEventKind::PfcFrameSubmitted,
        NetworkEventKind::PfcPaused,
        NetworkEventKind::PfcResumed,
        NetworkEventKind::LinkStateChanged,
        NetworkEventKind::Delivered,
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(events[index].kind, expected[index]);
    }
    EXPECT_EQ(events.front().scope, NetworkEventScope::PacketAttempt);
    EXPECT_EQ(events[1].scope, NetworkEventScope::TransportControl);
    EXPECT_EQ(events.back().scope, NetworkEventScope::FlowExtent);
    EXPECT_EQ(events.front().parent_token, accepted.token);
    EXPECT_EQ(events[1].token, events.front().token);
    EXPECT_EQ(port.packetEvents().size(), 4U);
    EXPECT_EQ(port.controlEvents().size(), 8U);
    EXPECT_EQ(port.terminals().size(), 1U);
    EXPECT_EQ(ready_times.size(), events.size());
    EXPECT_FALSE(port.hasPendingPhysicalWork());
}

TEST(HtsimNetworkPortTest, AbiV1RejectsPacketAndControlVocabulary) {
    HtsimNetworkPortConfig control_config;
    control_config.control_frames = true;
    EXPECT_THROW(
        static_cast<void>(HtsimNetworkPort{control_config}),
        std::invalid_argument);

    HtsimNetworkPortConfig congestion_config;
    congestion_config.congestion = true;
    EXPECT_THROW(
        static_cast<void>(HtsimNetworkPort{congestion_config}),
        std::invalid_argument);

    HtsimNetworkPortConfig link_config;
    link_config.dynamic_link_events = true;
    EXPECT_THROW(
        static_cast<void>(HtsimNetworkPort{link_config}),
        std::invalid_argument);

    HtsimNetworkPort port(HtsimNetworkPortConfig{});
    NetworkTxDescriptor packetized = descriptor(1, 401);
    packetized.extent_count = 2;
    EXPECT_THROW(port.trySubmit(packetized, 0), std::invalid_argument);

    NetworkTxDescriptor wrong_class = descriptor(2, 402);
    wrong_class.traffic_class = 4;
    EXPECT_THROW(port.trySubmit(wrong_class, 0), std::invalid_argument);
}

TEST(HtsimNetworkPortTest,
     AbiV2RejectsControlCapabilityWithoutARuntimeProducer) {
    HtsimNetworkPortConfig config;
    config.network_abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    config.dynamic_link_events = true;
    HtsimNetworkPort port(config);
    NetworkTxDescriptor value = descriptor(1, 451);
    value.abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    EXPECT_THROW(port.trySubmit(value, 0), std::invalid_argument);
}

TEST(HtsimNetworkPortTest,
     RuntimeSendFailureUnwindsEveryProvisionalCorrelation) {
    EventList event_list;
    ThrowingFlowRuntime runtime;
    HtsimNetworkPortConfig config;
    config.endpoint_count = 4;
    HtsimNetworkPort port(config);
    port.bindRuntime(runtime, 4, 4, [](std::uint64_t) {});

    EXPECT_THROW(port.trySubmit(descriptor(1, 501), 0), std::runtime_error);
    EXPECT_TRUE(port.issued().empty());
    EXPECT_TRUE(port.terminals().empty());
    EXPECT_TRUE(port.liveTokens().empty());
    EXPECT_FALSE(port.hasPendingPhysicalWork());

    EXPECT_THROW(port.trySubmit(descriptor(1, 501), 0), std::runtime_error);
    EXPECT_EQ(runtime.requests.size(), 2U);
    EXPECT_TRUE(port.issued().empty());
    EXPECT_TRUE(port.liveTokens().empty());
}

TEST(HtsimNetworkPortTest,
     RuntimeCapacityReplacesFixtureCapacityWithoutFabricatedDrops) {
    EventList event_list;
    DeferredFlowRuntime runtime;
    HtsimNetworkPortConfig config;
    config.capacity = 1;
    config.endpoint_count = 4;
    HtsimNetworkPort port(config);
    std::vector<std::uint64_t> ready_times;
    port.bindRuntime(
        runtime,
        4,
        2,
        [&](std::uint64_t at_ps) { ready_times.push_back(at_ps); });

    const auto first = port.trySubmit(descriptor(1, 601), 0);
    const auto second = port.trySubmit(descriptor(2, 602), 0);
    EXPECT_EQ(first.status, NetworkSubmitStatus::Accepted);
    EXPECT_EQ(second.status, NetworkSubmitStatus::Accepted);
    EXPECT_EQ(port.effectiveCapacity(), 2U);
    ASSERT_EQ(runtime.requests.size(), 2U);
    EXPECT_TRUE(port.terminals().empty());

    runtime.completeAll();
    EXPECT_EQ(ready_times, (std::vector<std::uint64_t>{0, 0}));
    const auto terminals = port.takeDue(0);
    ASSERT_EQ(terminals.size(), 2U);
    EXPECT_EQ(terminals[0].kind, NetworkEventKind::Delivered);
    EXPECT_EQ(terminals[1].kind, NetworkEventKind::Delivered);
    EXPECT_TRUE(port.liveTokens().empty());
}

TEST(HtsimNetworkPortTest, RuntimeBindingRejectsAdapterDropInjection) {
    DeferredFlowRuntime runtime;
    HtsimNetworkPortConfig config;
    config.drop_first = true;
    HtsimNetworkPort port(config);
    EXPECT_THROW(
        port.bindRuntime(runtime, 4, 4, [](std::uint64_t) {}),
        std::invalid_argument);
    EXPECT_FALSE(port.hasBoundRuntime());
}

}  // namespace
