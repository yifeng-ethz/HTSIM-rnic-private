// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "simllm_htsim_network_port.h"

namespace {

using htsim::simllm_rnic::HtsimNetworkPort;
using htsim::simllm_rnic::HtsimNetworkPortConfig;
using simllm::rnic::DropLocation;
using simllm::rnic::DropReason;
using simllm::rnic::NetworkEventKind;
using simllm::rnic::NetworkSubmitStatus;
using simllm::rnic::NetworkTxDescriptor;

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

TEST(HtsimNetworkPortTest, RejectsDeferredPacketAndControlVocabulary) {
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

}  // namespace
