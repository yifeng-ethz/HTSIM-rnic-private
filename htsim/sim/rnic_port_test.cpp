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

TEST(RnicTxPortTest,
     RemovesOnlyCommittedRetiredStateAndPreservesRemainingRates) {
    RnicTxPort port(1, 100, 1, 7);
    port.addFlow(10, 1, 0);
    port.setWireRateGrant(10, 100);
    port.setDataEligible(10, true);
    const RnicTxOpportunity terminal = port.dispatchOpportunity(0);
    ASSERT_TRUE(terminal.packet.has_value());
    ASSERT_EQ(terminal.packet->flow_id, 10u);
    ASSERT_TRUE(port.sourcePayloadDispatched(10));

    port.addFlow(11, 100, 0);
    port.addFlow(12, 100, 0);
    port.setDataEligible(11, true);
    port.setDataEligible(12, true);
    port.setWireRateGrant(11, 20);
    port.setWireRateGrant(12, 100);
    ASSERT_EQ(port.effectiveWireRateBps(11), 20u);
    ASSERT_EQ(port.effectiveWireRateBps(12), 80u);

    port.setDataEligible(10, false);
    port.setWireRateGrant(10, 0);
    const uint64_t data_boundary = port.nextDataOpportunityPs();
    const uint64_t wire_boundary = port.physicalSerializerAvailablePs();

    port.removeRetiredFlow(10);

    EXPECT_FALSE(port.contains(10));
    EXPECT_EQ(port.flowCount(), 2u);
    EXPECT_EQ(port.effectiveWireRateBps(11), 20u);
    EXPECT_EQ(port.effectiveWireRateBps(12), 80u);
    EXPECT_EQ(port.nextDataOpportunityPs(), data_boundary);
    EXPECT_EQ(port.physicalSerializerAvailablePs(), wire_boundary);
    EXPECT_TRUE(port.hasDispatchableData());
    EXPECT_THROW(port.sourcePayloadDispatched(10), std::out_of_range);
}

TEST(RnicTxPortTest, RetiredFlowRemovalRejectsEveryPrematureStateAtomically) {
    RnicTxPort port(1, 100, 1, 7);
    port.addFlow(10, 2, 0);
    port.addFlow(11, 100, 0);
    port.setWireRateGrant(11, 100);
    port.setDataEligible(11, true);
    ASSERT_EQ(port.effectiveWireRateBps(11), 100u);

    EXPECT_THROW(port.removeRetiredFlow(10), std::logic_error);
    EXPECT_TRUE(port.contains(10));
    EXPECT_EQ(port.flowCount(), 2u);
    EXPECT_EQ(port.flowPayloadBytesDispatched(10), 0u);
    EXPECT_EQ(port.effectiveWireRateBps(11), 100u);

    // A zero-payload flow is source-complete, but retirement cleanup still
    // requires both independent runtime gates to be closed.
    port.addFlow(20, 0, 0);
    port.setDataEligible(20, true);
    EXPECT_THROW(port.removeRetiredFlow(20), std::logic_error);
    EXPECT_TRUE(port.contains(20));
    EXPECT_EQ(port.flowCount(), 3u);
    EXPECT_EQ(port.effectiveWireRateBps(11), 100u);

    port.setDataEligible(20, false);
    port.setWireRateGrant(20, 1);
    EXPECT_THROW(port.removeRetiredFlow(20), std::logic_error);
    EXPECT_TRUE(port.contains(20));
    EXPECT_EQ(port.flowCount(), 3u);
    EXPECT_EQ(port.effectiveWireRateBps(11), 100u);

    port.setWireRateGrant(20, 0);
    port.removeRetiredFlow(20);
    EXPECT_FALSE(port.contains(20));
    EXPECT_EQ(port.flowCount(), 2u);
    EXPECT_THROW(port.removeRetiredFlow(20), std::out_of_range);
    EXPECT_EQ(port.flowCount(), 2u);
    EXPECT_EQ(port.effectiveWireRateBps(11), 100u);
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

TEST(RnicTxPortTest,
     OneNodePortNeverOverlapsWireOpportunitiesAndStampsAtRouteInjection) {
    RnicTxPort port(1, 100000000000ULL, 1000, 7);
    port.addFlow(10, 2000, 500);
    port.setWireRateGrant(10, 100000000000ULL);
    port.setDataEligible(10, true);

    const RnicTxOpportunity first = port.dispatchOpportunity(0);
    ASSERT_TRUE(first.packet.has_value());
    EXPECT_EQ(first.packet->eta_ps, first.end_ps + 500);
    EXPECT_EQ(first.end_ps - first.start_ps, 80000u);

    EXPECT_THROW(port.dispatchOpportunity(first.end_ps - 1), std::invalid_argument);
    const RnicTxOpportunity second = port.dispatchOpportunity(first.end_ps);
    ASSERT_TRUE(second.packet.has_value());
    EXPECT_EQ(second.start_ps, first.end_ps);
    EXPECT_EQ(second.packet->payload_byte_offset, 1000u);
    EXPECT_TRUE(port.sourcePayloadDispatched(10));
}

TEST(RnicTxPortTest, RouteInjectionEtaOverflowIsTransactional) {
    RnicTxPort port(1, 8000000000000ULL, 1, 7);
    port.addFlow(10, 1, std::numeric_limits<uint64_t>::max());
    port.setWireRateGrant(10, 8000000000000ULL);
    port.setDataEligible(10, true);

    EXPECT_THROW(port.dispatchOpportunity(0), std::overflow_error);
    EXPECT_EQ(port.flowPayloadBytesDispatched(10), 0u);
    EXPECT_EQ(port.nextDataOpportunityPs(), 0u);
    EXPECT_EQ(port.physicalSerializerAvailablePs(), 0u);

    // The failed DATA transaction did not reserve the shared physical wire.
    EXPECT_EQ(port.dispatchControl(0, 1).end_ps, 1u);
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

TEST(RnicRxPortTest, NextEventMovesFromLogicalReleaseToExactCompletion) {
    RnicRxPort port(8000000000000ULL, {100, 10, 128});
    EXPECT_FALSE(port.nextEventTimePs().has_value());

    const RnicRingCamPacket packet{7, 10, 0, 0, {100, 128}};
    const RnicRxArrivalResult arrival = port.processArrival(packet);
    ASSERT_EQ(arrival.admission, RnicRingCamAdmission::Admitted);
    EXPECT_TRUE(arrival.packets_completed_through_arrival.empty());
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 100u);

    const RnicRxAdvanceResult before_release =
        port.advanceToWithCompletions(99);
    EXPECT_TRUE(before_release.serializations_scheduled.empty());
    EXPECT_TRUE(before_release.packets_completed.empty());
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 100u);

    const RnicRxAdvanceResult released = port.advanceToWithCompletions(100);
    ASSERT_EQ(released.serializations_scheduled.size(), 1u);
    EXPECT_TRUE(released.packets_completed.empty());
    EXPECT_EQ(released.serializations_scheduled[0].serializer_start_ps, 100u);
    EXPECT_EQ(released.serializations_scheduled[0].serializer_end_ps, 228u);
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 228u);

    const RnicRxAdvanceResult before_completion =
        port.advanceToWithCompletions(227);
    EXPECT_TRUE(before_completion.serializations_scheduled.empty());
    EXPECT_TRUE(before_completion.packets_completed.empty());
    EXPECT_EQ(port.deliveredPayloadBytes(10), 0u);
    EXPECT_EQ(port.deliveredWireBytes(10), 0u);
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 228u);

    const RnicRxAdvanceResult completed = port.advanceToWithCompletions(228);
    EXPECT_TRUE(completed.serializations_scheduled.empty());
    ASSERT_EQ(completed.packets_completed.size(), 1u);
    const RnicRxPacketCompletion& exact = completed.packets_completed[0];
    EXPECT_EQ(exact.serializer_end_ps, 228u);
    EXPECT_EQ(exact.packet.packet_id, packet.packet_id);
    EXPECT_EQ(exact.packet.flow_id, packet.flow_id);
    EXPECT_EQ(exact.packet.eta_ps, packet.eta_ps);
    EXPECT_EQ(exact.packet.arrival_ps, packet.arrival_ps);
    EXPECT_EQ(exact.packet.extent.payloadBytes(), 100u);
    EXPECT_EQ(exact.packet.extent.wireBytes(), 128u);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 100u);
    EXPECT_EQ(port.deliveredWireBytes(10), 128u);
    EXPECT_FALSE(port.nextEventTimePs().has_value());
}

TEST(RnicRxPortTest, NextEventPrefersReleaseBeforePendingCompletion) {
    RnicRxPort port(8000000000000ULL, {100, 10, 192});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {100, 128}}).admission,
              RnicRingCamAdmission::Admitted);
    ASSERT_EQ(port.processArrival({2, 20, 50, 50, {40, 64}}).admission,
              RnicRingCamAdmission::Admitted);

    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 100u);
    const RnicRxAdvanceResult first_release =
        port.advanceToWithCompletions(100);
    ASSERT_EQ(first_release.serializations_scheduled.size(), 1u);
    EXPECT_EQ(first_release.serializations_scheduled[0].serializer_end_ps, 228u);

    // The second logical release is the next event even though the first
    // destination serialization is still in progress.
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 150u);
    const RnicRxAdvanceResult second_release =
        port.advanceToWithCompletions(150);
    ASSERT_EQ(second_release.serializations_scheduled.size(), 1u);
    EXPECT_EQ(second_release.serializations_scheduled[0].serializer_start_ps,
              228u);
    EXPECT_TRUE(second_release.packets_completed.empty());
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 228u);
}

TEST(RnicRxPortTest, SameTimeReleaseAndCompletionAreBothReported) {
    RnicRxPort port(8000000000000ULL, {100, 10, 112});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {64, 80}}).admission,
              RnicRingCamAdmission::Admitted);
    ASSERT_EQ(port.processArrival({2, 20, 80, 80, {24, 32}}).admission,
              RnicRingCamAdmission::Admitted);

    const RnicRxAdvanceResult first = port.advanceToWithCompletions(100);
    ASSERT_EQ(first.serializations_scheduled.size(), 1u);
    ASSERT_EQ(first.serializations_scheduled[0].serializer_end_ps, 180u);
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 180u);

    const RnicRxAdvanceResult coincident =
        port.advanceToWithCompletions(180);
    ASSERT_EQ(coincident.serializations_scheduled.size(), 1u);
    ASSERT_EQ(coincident.packets_completed.size(), 1u);
    EXPECT_EQ(coincident.packets_completed[0].packet.packet_id, 1u);
    EXPECT_EQ(coincident.packets_completed[0].serializer_end_ps, 180u);
    EXPECT_EQ(coincident.serializations_scheduled[0].release.packet.packet_id,
              2u);
    EXPECT_EQ(coincident.serializations_scheduled[0].serializer_start_ps,
              180u);
    EXPECT_EQ(coincident.serializations_scheduled[0].serializer_end_ps, 212u);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 64u);
    EXPECT_EQ(port.deliveredPayloadBytes(20), 0u);
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 212u);
}

TEST(RnicRxPortTest, ArrivalReportsSerializerCompletionAtSameTimestamp) {
    RnicRxPort port(8000000000000ULL, {100, 10, 200});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {80, 100}}).admission,
              RnicRingCamAdmission::Admitted);
    const RnicRxAdvanceResult released = port.advanceToWithCompletions(100);
    ASSERT_EQ(released.serializations_scheduled.size(), 1u);
    ASSERT_EQ(released.serializations_scheduled[0].serializer_end_ps, 200u);

    const RnicRxArrivalResult arrival =
        port.processArrival({2, 20, 200, 200, {40, 50}});
    ASSERT_EQ(arrival.admission, RnicRingCamAdmission::Admitted);
    ASSERT_EQ(arrival.packets_completed_through_arrival.size(), 1u);
    EXPECT_EQ(arrival.packets_completed_through_arrival[0].packet.packet_id,
              1u);
    EXPECT_EQ(arrival.packets_completed_through_arrival[0].serializer_end_ps,
              200u);
    EXPECT_EQ(port.deliveredPayloadBytes(10), 80u);
    EXPECT_EQ(port.deliveredPayloadBytes(20), 0u);
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), 300u);
}

TEST(RnicRxPortTest, CompletionBatchOverflowDoesNotPartiallyCommit) {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    RnicRxPort port(maximum, {1, 1, maximum});
    ASSERT_EQ(port.processArrival({1, 10, 0, 0, {maximum, maximum}}).admission,
              RnicRingCamAdmission::Admitted);
    const RnicRxAdvanceResult first_release =
        port.advanceToWithCompletions(1);
    ASSERT_EQ(first_release.serializations_scheduled.size(), 1u);
    const uint64_t first_completion =
        first_release.serializations_scheduled[0].serializer_end_ps;
    ASSERT_EQ(port.advanceToWithCompletions(first_completion)
                  .packets_completed.size(),
              1u);
    ASSERT_EQ(port.deliveredPayloadBytes(10), maximum);
    ASSERT_EQ(port.deliveredWireBytes(10), maximum);

    ASSERT_EQ(port.processArrival(
                  {2, 20, first_completion, first_completion, {1, 1}}).admission,
              RnicRingCamAdmission::Admitted);
    ASSERT_EQ(port.processArrival(
                  {3, 10, first_completion, first_completion, {1, 1}}).admission,
              RnicRingCamAdmission::Admitted);
    const uint64_t second_release = first_completion + 1;
    const RnicRxAdvanceResult scheduled =
        port.advanceToWithCompletions(second_release);
    ASSERT_EQ(scheduled.serializations_scheduled.size(), 2u);
    ASSERT_EQ(scheduled.serializations_scheduled[0].serializer_end_ps,
              scheduled.serializations_scheduled[1].serializer_end_ps);
    const uint64_t completion =
        scheduled.serializations_scheduled.back().serializer_end_ps;

    EXPECT_THROW(port.advanceToWithCompletions(completion), std::overflow_error);
    EXPECT_EQ(port.deliveredPayloadBytes(20), 0u);
    EXPECT_EQ(port.deliveredWireBytes(20), 0u);
    EXPECT_EQ(port.deliveredPayloadBytes(10), maximum);
    EXPECT_EQ(port.deliveredWireBytes(10), maximum);
    EXPECT_EQ(port.pendingSerializerWireBytes(), 2u);
    ASSERT_TRUE(port.nextEventTimePs().has_value());
    EXPECT_EQ(*port.nextEventTimePs(), completion);
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
