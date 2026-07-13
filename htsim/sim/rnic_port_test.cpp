// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_port.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

TEST(RnicTxPortTest, DataIsIneligibleUntilTheGrantGateOpens) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 1000, 500);
    port.setWireRateGrant(10, 100000000000ULL);

    const RnicTxOpportunity gated = port.dispatchOpportunity(0);
    EXPECT_FALSE(gated.packet.has_value());

    port.setDataEligible(10, true);
    const RnicTxOpportunity active = port.dispatchOpportunity(gated.end_ps);
    ASSERT_TRUE(active.packet.has_value());
    EXPECT_EQ(active.packet->flow_id, 10u);
}

TEST(RnicTxPortTest, ReportsWhetherAnyGrantedDataCanDispatch) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 1000, 0);
    EXPECT_FALSE(port.hasDispatchableData());

    port.setWireRateGrant(10, 100000000000ULL);
    EXPECT_FALSE(port.hasDispatchableData());
    port.setDataEligible(10, true);
    EXPECT_TRUE(port.hasDispatchableData());

    const auto packet = port.dispatchOpportunity(0);
    ASSERT_TRUE(packet.packet.has_value());
    EXPECT_FALSE(port.hasDispatchableData());
}

TEST(RnicTxPortTest, ControlAndDataShareOnePhysicalSerializer) {
    RnicTxPort port(1, 8000000000000ULL, 1000, 7);
    port.addFlow(10, 1000, 0);
    port.setWireRateGrant(10, 8000000000000ULL);
    port.setDataEligible(10, true);

    const auto control = port.dispatchControl(0, 64);
    EXPECT_EQ(control.start_ps, 0u);
    EXPECT_EQ(control.end_ps, 64u);
    EXPECT_EQ(port.nextDataOpportunityPs(), 0u);
    EXPECT_EQ(port.physicalSerializerAvailablePs(), 64u);

    const auto data = port.dispatchOpportunity(control.end_ps);
    ASSERT_TRUE(data.packet.has_value());
    EXPECT_EQ(data.start_ps, control.end_ps);
    EXPECT_EQ(data.end_ps, 1064u);
    EXPECT_EQ(port.nextDataOpportunityPs(), data.end_ps);
    EXPECT_EQ(port.physicalSerializerAvailablePs(), data.end_ps);
}

TEST(RnicTxPortTest, ControlCanUseAReservedPrbsIdleInterval) {
    RnicTxPort port(1, 8000000000000ULL, 1000, 7);
    port.addFlow(10, 1000, 0);

    const auto idle = port.dispatchOpportunity(0);
    EXPECT_FALSE(idle.packet.has_value());
    EXPECT_EQ(idle.start_ps, 0u);
    EXPECT_EQ(idle.end_ps, 1000u);
    EXPECT_EQ(port.nextDataOpportunityPs(), 1000u);
    EXPECT_EQ(port.physicalSerializerAvailablePs(), 0u);

    const auto control = port.dispatchControl(100, 64);
    EXPECT_EQ(control.start_ps, 100u);
    EXPECT_EQ(control.end_ps, 164u);
    EXPECT_EQ(port.nextDataOpportunityPs(), 1000u);

    port.setWireRateGrant(10, 8000000000000ULL);
    port.setDataEligible(10, true);
    const auto data = port.dispatchOpportunity(1000);
    ASSERT_TRUE(data.packet.has_value());
    EXPECT_EQ(data.start_ps, 1000u);
    EXPECT_EQ(data.end_ps, 2000u);
}

TEST(RnicTxPortTest, LongControlDelaysButDoesNotConsumeDataLottery) {
    RnicTxPort port(1, 8000000000000ULL, 1000, 7);
    port.addFlow(10, 1000, 0);

    const auto idle = port.dispatchOpportunity(0);
    ASSERT_FALSE(idle.packet.has_value());
    const auto control = port.dispatchControl(900, 200);
    EXPECT_EQ(control.end_ps, 1100u);

    port.setWireRateGrant(10, 8000000000000ULL);
    port.setDataEligible(10, true);
    EXPECT_THROW(port.dispatchOpportunity(1000), std::invalid_argument);
    const auto data = port.dispatchOpportunity(control.end_ps);
    ASSERT_TRUE(data.packet.has_value());
    EXPECT_EQ(data.start_ps, 1100u);
    EXPECT_EQ(data.end_ps, 2100u);
}

TEST(RnicTxPortTest, ControlBeforeFractionalIdleBoundaryDoesNotShiftDataClock) {
    RnicTxPort port(1, 9000000000000ULL, 2, 7);
    port.addFlow(10, 2, 0);

    const auto idle = port.dispatchOpportunity(0);  // Exact end: 16/9 ps.
    ASSERT_FALSE(idle.packet.has_value());
    ASSERT_EQ(idle.end_ps, 2u);
    const auto control = port.dispatchControl(0, 1);  // Exact end: 8/9 ps.
    ASSERT_EQ(control.end_ps, 1u);

    port.setWireRateGrant(10, 9000000000000ULL);
    port.setDataEligible(10, true);
    const auto data = port.dispatchOpportunity(idle.end_ps);
    ASSERT_TRUE(data.packet.has_value());
    EXPECT_EQ(data.end_ps, 4u);  // Exact end remains 32/9 ps.

    // This probe ends exactly at 8 ps only if the later virtual boundary won.
    EXPECT_EQ(port.dispatchControl(data.end_ps, 5).end_ps, 8u);
}

TEST(RnicTxPortTest, SameCeilControlOverhangShiftsFractionalDataBoundary) {
    RnicTxPort port(1, 9000000000000ULL, 2, 7);
    port.addFlow(10, 2, 0);

    const auto idle = port.dispatchOpportunity(0);  // Exact end: 16/9 ps.
    ASSERT_FALSE(idle.packet.has_value());
    ASSERT_EQ(idle.end_ps, 2u);
    const auto control = port.dispatchControl(1, 1);  // Exact end: 17/9 ps.
    ASSERT_EQ(control.end_ps, idle.end_ps);

    port.setWireRateGrant(10, 9000000000000ULL);
    port.setDataEligible(10, true);
    const auto data = port.dispatchOpportunity(idle.end_ps);
    ASSERT_TRUE(data.packet.has_value());
    EXPECT_EQ(data.end_ps, 4u);  // Exact end: 33/9 ps.

    // Comparing only published ceil ticks would retain 32/9 and end at 8 ps.
    EXPECT_EQ(port.dispatchControl(data.end_ps, 5).end_ps, 9u);
}

TEST(RnicTxPortTest, OneNodePortNeverOverlapsWireOpportunitiesAndStampsAtDispatch) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 2000, 500);
    port.setWireRateGrant(10, 100000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    ASSERT_TRUE(first.packet.has_value());
    EXPECT_EQ(first.packet->eta_ps, first.start_ps + 500);
    EXPECT_EQ(first.end_ps - first.start_ps, 80000u);

    EXPECT_THROW(port.dispatchOpportunity(first.end_ps - 1), std::invalid_argument);
    const RnicTxOpportunity second = port.dispatchOpportunity(first.end_ps);
    ASSERT_TRUE(second.packet.has_value());
    EXPECT_EQ(second.start_ps, first.end_ps);
    EXPECT_EQ(second.packet->payload_byte_offset, 1000u);
    EXPECT_TRUE(port.sourcePayloadDispatched(10));
}

TEST(RnicTxPortTest, LocalMaxMinCapsOversubscribedReceiverGrants) {
    RnicTxPort port(1, 100, 1, 7);
    port.addFlow(10, 100, 0);
    port.addFlow(11, 100, 0);
    port.setDataEligible(10, true);
    port.setDataEligible(11, true);
    port.setWireRateGrant(10, 20);
    port.setWireRateGrant(11, 100);

    EXPECT_EQ(port.effectiveWireRateBps(10), 20u);
    EXPECT_EQ(port.effectiveWireRateBps(11), 80u);
}

TEST(RnicTxPortTest, PrbsSharesOnePhysicalPortAcrossFlows) {
    RnicTxPort port(9, 100000000000ULL, 1000, 20260713);
    port.addFlow(10, 1000000000, 0);
    port.addFlow(11, 1000000000, 0);
    port.setDataEligible(10, true);
    port.setDataEligible(11, true);
    port.setWireRateGrant(10, 30000000000ULL);
    port.setWireRateGrant(11, 70000000000ULL);

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

TEST(RnicTxPortTest, FullRateShortFlowUsesExactFinalWireSerialization) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 100, 0);
    port.setWireRateGrant(10, 100000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity only = port.dispatchOpportunity(0);
    ASSERT_TRUE(only.packet.has_value());
    EXPECT_EQ(only.packet->extent.payloadBytes(), 100u);
    EXPECT_EQ(only.packet->extent.wireBytes(), 100u);
    EXPECT_EQ(only.end_ps - only.start_ps, 8000u);
    EXPECT_TRUE(port.sourcePayloadDispatched(10));
}

TEST(RnicTxPortTest, HeaderlessFinalTailIsNeverPaddedToMaximumWireSize) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 1100, 0);
    port.setWireRateGrant(10, 100000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    const RnicTxOpportunity last = port.dispatchOpportunity(first.end_ps);
    ASSERT_TRUE(first.packet.has_value());
    ASSERT_TRUE(last.packet.has_value());
    EXPECT_EQ(first.packet->extent.payloadBytes(), 1000u);
    EXPECT_EQ(first.packet->extent.wireBytes(), 1000u);
    EXPECT_EQ(last.start_ps, first.end_ps);
    EXPECT_EQ(last.packet->extent.payloadBytes(), 100u);
    EXPECT_EQ(last.packet->extent.wireBytes(), 100u);
    EXPECT_EQ(last.end_ps - last.start_ps, 8000u);
}

TEST(RnicTxPortTest, DataHeaderReducesPayloadQuantumAndChargesEveryPacket) {
    RnicTxPort port(
        1, 8000000000000ULL, RnicDataPacketizationConfig(1000, 40), 7);
    port.addFlow(10, 2000, 0);
    port.setWireRateGrant(10, 8000000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    const RnicTxOpportunity second = port.dispatchOpportunity(first.end_ps);
    const RnicTxOpportunity last = port.dispatchOpportunity(second.end_ps);
    ASSERT_TRUE(first.packet.has_value());
    ASSERT_TRUE(second.packet.has_value());
    ASSERT_TRUE(last.packet.has_value());

    EXPECT_EQ(port.maxWirePacketBytes(), 1000u);
    EXPECT_EQ(port.dataHeaderBytes(), 40u);
    EXPECT_EQ(port.maxDataPayloadBytes(), 960u);
    EXPECT_EQ(first.packet->payload_byte_offset, 0u);
    EXPECT_EQ(first.packet->extent.payloadBytes(), 960u);
    EXPECT_EQ(first.packet->extent.wireBytes(), 1000u);
    EXPECT_EQ(second.packet->payload_byte_offset, 960u);
    EXPECT_EQ(second.start_ps, first.end_ps);
    EXPECT_EQ(second.packet->extent.payloadBytes(), 960u);
    EXPECT_EQ(second.packet->extent.wireBytes(), 1000u);
    EXPECT_EQ(last.packet->payload_byte_offset, 1920u);
    EXPECT_EQ(last.start_ps, second.end_ps);
    EXPECT_EQ(last.packet->extent.payloadBytes(), 80u);
    EXPECT_EQ(last.packet->extent.wireBytes(), 120u);
    EXPECT_EQ(last.end_ps - last.start_ps, 120u);
    EXPECT_TRUE(port.sourcePayloadDispatched(10));
}

TEST(RnicTxPortTest, CumulativeWireClockPreventsPerPacketCeilDrift) {
    RnicTxPort port(1, 3000000000000ULL, 1, 7);
    port.addFlow(10, 3, 0);
    port.setWireRateGrant(10, 3000000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    const RnicTxOpportunity second = port.dispatchOpportunity(first.end_ps);
    const RnicTxOpportunity third = port.dispatchOpportunity(second.end_ps);

    EXPECT_EQ(first.end_ps, 3u);
    EXPECT_EQ(second.start_ps, first.end_ps);
    EXPECT_EQ(second.end_ps, 6u);
    EXPECT_EQ(third.start_ps, second.end_ps);
    EXPECT_EQ(third.end_ps, 8u);
    EXPECT_EQ(port.nextWireOpportunityPs(), 8u);
}

TEST(RnicTxPortTest, ZeroPayloadFlowIsSourceCompleteWithoutDataDelivery) {
    RnicTxPort port(1, 8000000000000ULL, 1000, 7);
    port.addFlow(10, 0, 0);
    port.setWireRateGrant(10, 8000000000000ULL);
    port.setDataEligible(10, true);

    EXPECT_TRUE(port.sourcePayloadDispatched(10));
    EXPECT_EQ(port.flowPayloadBytesDispatched(10), 0u);
    const RnicTxOpportunity idle = port.dispatchOpportunity(0);
    EXPECT_FALSE(idle.packet.has_value());
    EXPECT_EQ(idle.end_ps - idle.start_ps, 1000u);
}

TEST(RnicDataPacketizationConfigTest, RejectsInvalidSizesAndNeverOverflowsTail) {
    EXPECT_THROW(RnicDataPacketizationConfig(0), std::invalid_argument);
    EXPECT_THROW(RnicDataPacketizationConfig(1000, 1000), std::invalid_argument);
    EXPECT_THROW(RnicDataPacketizationConfig(1000, 1001), std::invalid_argument);

    RnicDataPacketizationConfig config(
        std::numeric_limits<uint64_t>::max(),
        std::numeric_limits<uint64_t>::max() - 1);
    EXPECT_THROW(config.packetize(0), std::invalid_argument);
    const RnicPacketExtent extent =
        config.packetize(std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(extent.payloadBytes(), 1u);
    EXPECT_EQ(extent.wireBytes(), std::numeric_limits<uint64_t>::max());
}

TEST(RnicRxPortTest, SharedRingCamFeedsOneNonOverlappingSerializer) {
    RnicRxPort port(100000000000ULL, {100, 10, 10000});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {1000, 1000}}).admission,
              RnicRingCamAdmission::Admitted);
    ASSERT_EQ(port.processArrival({2, 20, 0, 0, {1000, 1000}}).admission,
              RnicRingCamAdmission::Admitted);

    const auto scheduled = port.advanceTo(100);
    ASSERT_EQ(scheduled.size(), 2u);
    EXPECT_EQ(scheduled[0].serializer_start_ps, 100u);
    EXPECT_EQ(scheduled[1].serializer_start_ps, scheduled[0].serializer_end_ps);
    EXPECT_GT(scheduled[1].serializer_end_ps, scheduled[1].serializer_start_ps);
    EXPECT_EQ(port.ringCam().wireOccupancyBytes(), 0u);
    EXPECT_EQ(port.pendingSerializerWireBytes(), 2000u);
    EXPECT_EQ(port.pendingSerializerHighWatermarkWireBytes(), 2000u);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 0u);
    EXPECT_EQ(port.deliveredWireBytes(10), 0u);
    EXPECT_EQ(port.deliveredPayloadBytes(20), 0u);
    EXPECT_EQ(port.deliveredWireBytes(20), 0u);
    port.advanceTo(scheduled.back().serializer_end_ps);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 1000u);
    EXPECT_EQ(port.deliveredWireBytes(10), 1000u);
    EXPECT_EQ(port.deliveredPayloadBytes(20), 1000u);
    EXPECT_EQ(port.deliveredWireBytes(20), 1000u);
    EXPECT_EQ(port.pendingSerializerWireBytes(), 0u);
    EXPECT_EQ(port.ringCam().wireHighWatermarkBytes(), 2000u);
}

TEST(RnicRxPortTest, SameTimeReleaseFreesSharedCamBeforeAdmission) {
    RnicRxPort port(100000000000ULL, {100, 10, 1000});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {1000, 1000}}).admission,
              RnicRingCamAdmission::Admitted);

    const RnicRxArrivalResult result =
        port.processArrival({2, 20, 100, 100, {1000, 1000}});
    ASSERT_EQ(result.serializations_scheduled_before_admission.size(), 1u);
    EXPECT_EQ(result.admission, RnicRingCamAdmission::Admitted);
    EXPECT_EQ(port.ringCam().wireOccupancyBytes(), 1000u);
}

TEST(RnicRxPortTest, PayloadDeliveryAndWireServiceUseIndependentLedgers) {
    RnicRxPort port(8000000000000ULL, {100, 10, 192});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {100, 128}}).admission,
              RnicRingCamAdmission::Admitted);
    ASSERT_EQ(port.processArrival({2, 10, 0, 0, {40, 64}}).admission,
              RnicRingCamAdmission::Admitted);

    const auto scheduled = port.advanceTo(100);
    ASSERT_EQ(scheduled.size(), 2u);
    EXPECT_EQ(scheduled[0].release.packet.extent.payloadBytes(), 100u);
    EXPECT_EQ(scheduled[0].release.packet.extent.wireBytes(), 128u);
    EXPECT_EQ(scheduled[0].serializer_end_ps
                  - scheduled[0].serializer_start_ps,
              128u);
    EXPECT_EQ(scheduled[1].serializer_end_ps
                  - scheduled[1].serializer_start_ps,
              64u);
    EXPECT_EQ(port.ringCam().wireHighWatermarkBytes(), 192u);
    EXPECT_EQ(port.pendingSerializerWireBytes(), 192u);
    EXPECT_EQ(port.pendingSerializerHighWatermarkWireBytes(), 192u);

    port.advanceTo(scheduled.back().serializer_end_ps);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 140u);
    EXPECT_EQ(port.deliveredWireBytes(10), 192u);
    EXPECT_EQ(port.pendingSerializerWireBytes(), 0u);
}

TEST(RnicRxPortTest, ScheduledSerializationDeliversOnlyAtItsEndBoundary) {
    RnicRxPort port(8000000000000ULL, {100, 10, 128});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {100, 128}}).admission,
              RnicRingCamAdmission::Admitted);

    const auto scheduled = port.advanceTo(100);
    ASSERT_EQ(scheduled.size(), 1u);
    ASSERT_GT(scheduled[0].serializer_end_ps, 0u);

    port.advanceTo(scheduled[0].serializer_end_ps - 1);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 0u);
    EXPECT_EQ(port.deliveredWireBytes(10), 0u);

    port.advanceTo(scheduled[0].serializer_end_ps);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 100u);
    EXPECT_EQ(port.deliveredWireBytes(10), 128u);
}

TEST(RnicWireSerializationClockTest, RetainsRemainderAcrossBackToBackPackets) {
    RnicWireSerializationClock serializer(3000000000000ULL);

    const auto first = serializer.serialize(0, 1);
    const auto second = serializer.serialize(first.end_ps, 1);
    const auto third = serializer.serialize(second.end_ps, 1);

    EXPECT_EQ(first.start_ps, 0u);
    EXPECT_EQ(first.end_ps, 3u);
    EXPECT_EQ(second.start_ps, first.end_ps);
    EXPECT_EQ(second.end_ps, 6u);
    EXPECT_EQ(third.start_ps, second.end_ps);
    EXPECT_EQ(third.end_ps, 8u);
    EXPECT_EQ(serializer.availablePs(), 8u);

    const auto after_idle = serializer.serialize(20, 1);
    EXPECT_EQ(after_idle.start_ps, 20u);
    EXPECT_EQ(after_idle.end_ps, 23u);
}

TEST(RnicWireSerializationClockTest, ExplicitIdleRebaseDropsFractionAtEquality) {
    RnicWireSerializationClock serializer(7000000000000ULL);
    const auto first = serializer.serialize(0, 3);
    ASSERT_EQ(first.end_ps, 4u);

    serializer.rebaseIdle(first.end_ps);
    const auto after_idle = serializer.serialize(first.end_ps, 1);
    EXPECT_EQ(after_idle.start_ps, 4u);
    EXPECT_EQ(after_idle.end_ps, 6u);
}

TEST(RnicWireSerializationClockTest, IdleRebaseRejectsBackwardTimeTransactionally) {
    RnicWireSerializationClock serializer(7000000000000ULL);
    EXPECT_EQ(serializer.serialize(0, 3).end_ps, 4u);
    const uint64_t preserved = serializer.availablePs();
    EXPECT_THROW(serializer.rebaseIdle(preserved - 1), std::invalid_argument);
    EXPECT_EQ(serializer.availablePs(), preserved);

    const auto second = serializer.serialize(preserved, 1);
    EXPECT_EQ(second.end_ps, 5u);
}

TEST(RnicWireSerializationClockTest, SynchronizesAtLaterExactBoundary) {
    RnicWireSerializationClock first(7000000000000ULL);
    RnicWireSerializationClock second(7000000000000ULL);
    EXPECT_EQ(first.serialize(0, 3).end_ps, 4u);   // 24/7 ps.
    EXPECT_EQ(second.serialize(0, 2).end_ps, 3u);  // 16/7 ps.

    second.synchronizeAvailableWith(first);
    EXPECT_EQ(first.availablePs(), 4u);
    EXPECT_EQ(second.availablePs(), 4u);
    EXPECT_EQ(first.serialize(4, 1).end_ps, 5u);
    EXPECT_EQ(second.serialize(4, 1).end_ps, 5u);

    RnicWireSerializationClock mismatched(8000000000000ULL);
    EXPECT_THROW(first.synchronizeAvailableWith(mismatched),
                 std::invalid_argument);
}

TEST(RnicWireSerializationClockTest, RejectsInvalidAndOverflowingInputs) {
    EXPECT_THROW(RnicWireSerializationClock(0), std::invalid_argument);

    RnicWireSerializationClock serializer(8000000000000ULL);
    EXPECT_THROW(serializer.serialize(0, 0), std::invalid_argument);
    EXPECT_THROW(
        serializer.serialize(std::numeric_limits<uint64_t>::max(), 1),
        std::overflow_error);
}

TEST(RnicWireSerializationClockTest, AcceptsMaximumExtentWhenTimestampFits) {
    RnicWireSerializationClock serializer(std::numeric_limits<uint64_t>::max());
    const auto interval =
        serializer.serialize(0, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(interval.start_ps, 0u);
    EXPECT_EQ(interval.end_ps, 8000000000000ULL);
}

TEST(RnicWireSerializationClockTest, SerializesZeroPayloadControlWireExtent) {
    const RnicPacketExtent control(0, 64);
    EXPECT_EQ(control.payloadBytes(), 0u);
    EXPECT_EQ(control.wireBytes(), 64u);

    RnicWireSerializationClock serializer(8000000000000ULL);
    const auto interval = serializer.serialize(0, control.wireBytes());
    EXPECT_EQ(interval.end_ps - interval.start_ps, 64u);
}

TEST(RnicNodeTest, OwnsExactlyOneSharedTxAndRxPort) {
    RnicNode node(42, 100000000000ULL, 1000, 7, {100, 10, 10000});
    EXPECT_EQ(node.nodeId(), 42u);
    EXPECT_EQ(node.txPort().prbsManifest().node_id, 42u);
    EXPECT_EQ(&node.txPort(), &node.txPort());
    EXPECT_EQ(&node.rxPort(), &node.rxPort());
}

TEST(RnicNodeTest, SourceDispatchCompletionDoesNotImplyDestinationDelivery) {
    RnicNode node(42, 8000000000000ULL, 1000, 7, {100, 10, 10000});
    node.txPort().addFlow(10, 100, 0);
    node.txPort().setWireRateGrant(10, 8000000000000ULL);
    node.txPort().setDataEligible(10, true);

    const RnicTxOpportunity dispatched = node.txPort().dispatchOpportunity(0);
    ASSERT_TRUE(dispatched.packet.has_value());
    EXPECT_TRUE(node.txPort().sourcePayloadDispatched(10));
    EXPECT_EQ(node.rxPort().deliveredPayloadBytes(10), 0u);
    EXPECT_EQ(node.rxPort().deliveredWireBytes(10), 0u);
}

}  // namespace
