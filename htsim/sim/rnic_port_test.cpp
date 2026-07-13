// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_port.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

TEST(RnicTxPortTest, DataIsIneligibleUntilTheGrantGateOpens) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 1000, 500);
    port.setGrant(10, 100000000000ULL);

    const RnicTxOpportunity gated = port.dispatchOpportunity(0);
    EXPECT_FALSE(gated.packet.has_value());

    port.setDataEligible(10, true);
    const RnicTxOpportunity active = port.dispatchOpportunity(gated.end_ps);
    ASSERT_TRUE(active.packet.has_value());
    EXPECT_EQ(active.packet->flow_id, 10u);
}

TEST(RnicTxPortTest, OneNodePortNeverOverlapsWireOpportunitiesAndStampsAtDispatch) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 2000, 500);
    port.setGrant(10, 100000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    ASSERT_TRUE(first.packet.has_value());
    EXPECT_EQ(first.packet->eta_ps, first.start_ps + 500);
    EXPECT_EQ(first.end_ps - first.start_ps, port.wireOpportunityDurationPs());

    EXPECT_THROW(port.dispatchOpportunity(first.end_ps - 1), std::invalid_argument);
    const RnicTxOpportunity second = port.dispatchOpportunity(first.end_ps);
    ASSERT_TRUE(second.packet.has_value());
    EXPECT_EQ(second.start_ps, first.end_ps);
    EXPECT_EQ(second.packet->byte_offset, 1000u);
    EXPECT_TRUE(port.flowComplete(10));
}

TEST(RnicTxPortTest, LocalMaxMinCapsOversubscribedReceiverGrants) {
    RnicTxPort port(1, 100, 1, 7);
    port.addFlow(10, 100, 0);
    port.addFlow(11, 100, 0);
    port.setDataEligible(10, true);
    port.setDataEligible(11, true);
    port.setGrant(10, 20);
    port.setGrant(11, 100);

    EXPECT_EQ(port.effectiveRateBps(10), 20u);
    EXPECT_EQ(port.effectiveRateBps(11), 80u);
}

TEST(RnicTxPortTest, PrbsSharesOnePhysicalPortAcrossFlows) {
    RnicTxPort port(9, 100000000000ULL, 1000, 20260713);
    port.addFlow(10, 1000000000, 0);
    port.addFlow(11, 1000000000, 0);
    port.setDataEligible(10, true);
    port.setDataEligible(11, true);
    port.setGrant(10, 30000000000ULL);
    port.setGrant(11, 70000000000ULL);

    uint64_t time_ps = 0;
    uint64_t flow_ten_packets = 0;
    constexpr uint64_t opportunities = 100000;
    for (uint64_t i = 0; i < opportunities; ++i) {
        const RnicTxOpportunity opportunity = port.dispatchOpportunity(time_ps);
        ASSERT_TRUE(opportunity.packet.has_value());
        flow_ten_packets += opportunity.packet->flow_id == 10;
        time_ps = opportunity.end_ps;
    }
    EXPECT_NEAR(static_cast<double>(flow_ten_packets) / opportunities, 0.3, 0.01);
}

TEST(RnicTxPortTest, ShortFinalPacketConsumesAFullPhysicalOpportunity) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 1100, 0);
    port.setGrant(10, 100000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    const RnicTxOpportunity last = port.dispatchOpportunity(first.end_ps);
    ASSERT_TRUE(last.packet.has_value());
    EXPECT_EQ(last.packet->payload_bytes, 100u);
    EXPECT_EQ(last.packet->charged_wire_bytes, 1000u);
    EXPECT_EQ(last.end_ps - last.start_ps, first.end_ps - first.start_ps);
}

TEST(RnicRxPortTest, SharedRingCamFeedsOneNonOverlappingSerializer) {
    RnicRxPort port(100000000000ULL, {100, 10, 10000});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, 1000}).admission,
              RnicRingCamAdmission::Admitted);
    ASSERT_EQ(port.processArrival({2, 20, 0, 0, 1000}).admission,
              RnicRingCamAdmission::Admitted);

    const auto deliveries = port.advanceTo(100);
    ASSERT_EQ(deliveries.size(), 2u);
    EXPECT_EQ(deliveries[0].serializer_start_ps, 100u);
    EXPECT_EQ(deliveries[1].serializer_start_ps, deliveries[0].serializer_end_ps);
    EXPECT_GT(deliveries[1].serializer_end_ps, deliveries[1].serializer_start_ps);
    EXPECT_EQ(port.deliveredBytes(10), 0u);
    EXPECT_EQ(port.deliveredBytes(20), 0u);
    port.advanceTo(deliveries.back().serializer_end_ps);
    EXPECT_EQ(port.deliveredBytes(10), 1000u);
    EXPECT_EQ(port.deliveredBytes(20), 1000u);
    EXPECT_EQ(port.ringCam().highWatermarkBytes(), 2000u);
}

TEST(RnicRxPortTest, SameTimeReleaseFreesSharedCamBeforeAdmission) {
    RnicRxPort port(100000000000ULL, {100, 10, 1000});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, 1000}).admission,
              RnicRingCamAdmission::Admitted);

    const RnicRxArrivalResult result = port.processArrival({2, 20, 100, 100, 1000});
    ASSERT_EQ(result.deliveries_released_before_admission.size(), 1u);
    EXPECT_EQ(result.admission, RnicRingCamAdmission::Admitted);
    EXPECT_EQ(port.ringCam().occupancyBytes(), 1000u);
}

TEST(RnicNodeTest, OwnsExactlyOneSharedTxAndRxPort) {
    RnicNode node(42, 100000000000ULL, 1000, 7, {100, 10, 10000});
    EXPECT_EQ(node.nodeId(), 42u);
    EXPECT_EQ(node.txPort().prbsManifest().node_id, 42u);
    EXPECT_EQ(&node.txPort(), &node.txPort());
    EXPECT_EQ(&node.rxPort(), &node.rxPort());
}

}  // namespace
