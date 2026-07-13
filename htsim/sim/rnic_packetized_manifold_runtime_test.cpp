// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_packetized_manifold_runtime.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::uint64_t kSecondPs = UINT64_C(1000000000000);

EventList& testEventList() {
    EventList& event_list = EventList::getTheEventList();
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    return event_list;
}

AtlahsFlowRequest request(AtlahsFlowId flow_id,
                          std::uint32_t source,
                          std::uint32_t destination,
                          std::uint64_t payload_bytes) {
    return {flow_id,
            source,
            destination,
            payload_bytes,
            EventList::now(),
            0};
}

class CallbackEvent final : public EventSource {
public:
    CallbackEvent(EventList& event_list,
                  simtime_picosec when,
                  std::function<void()> callback)
        : EventSource(event_list, "packetized-runtime-test-callback"),
          _callback(std::move(callback)) {
        EventList::sourceIsPending(*this, when);
    }

    void doNextEvent() override { _callback(); }

private:
    std::function<void()> _callback;
};

TEST(RnicPacketizedManifoldRuntimeTest,
     ExactTailLedgersCompleteOnlyAtDestinationSerializer) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    constexpr std::uint64_t propagation_ps = 123;
    std::vector<std::pair<AtlahsFlowId, std::uint64_t>> completed;

    RnicPacketizedManifoldRuntime runtime(
        event_list,
        80,
        RnicDataPacketizationConfig(10, 2),
        propagation_ps);
    runtime.setup(2, [&](AtlahsFlowId flow_id) {
        completed.emplace_back(flow_id, EventList::now());
    });
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());
    runtime.send(request(10, 0, 1, 18));
    EXPECT_TRUE(runtime.hasPendingPhysicalWork());

    RnicPacketizedFlowSnapshot snapshot = runtime.flow(10);
    EXPECT_EQ(snapshot.total_packet_count, 3U);
    EXPECT_EQ(snapshot.total_wire_bytes, 24U);
    EXPECT_EQ(snapshot.wire_rate_grant_bps, 80U);
    ASSERT_EQ(runtime.nextSlotStartPs(), base);

    ASSERT_TRUE(EventList::doNextEvent());
    snapshot = runtime.flow(10);
    EXPECT_EQ(snapshot.packets_reserved, 1U);
    EXPECT_EQ(snapshot.payload_bytes_reserved, 8U);
    EXPECT_EQ(snapshot.wire_bytes_reserved, 10U);
    EXPECT_EQ(snapshot.packets_source_serialized, 0U);
    EXPECT_TRUE(completed.empty());

    while (EventList::doNextEvent()) {
    }

    snapshot = runtime.flow(10);
    EXPECT_EQ(snapshot.packets_reserved, 3U);
    EXPECT_EQ(snapshot.payload_bytes_reserved, 18U);
    EXPECT_EQ(snapshot.wire_bytes_reserved, 24U);
    EXPECT_EQ(snapshot.packets_source_serialized, 3U);
    EXPECT_EQ(snapshot.payload_bytes_source_serialized, 18U);
    EXPECT_EQ(snapshot.wire_bytes_source_serialized, 24U);
    EXPECT_EQ(snapshot.packets_delivered, 3U);
    EXPECT_EQ(snapshot.payload_bytes_delivered, 18U);
    EXPECT_EQ(snapshot.wire_bytes_delivered, 24U);
    EXPECT_EQ(snapshot.source_completion_time_ps, base + 3 * kSecondPs);
    EXPECT_EQ(snapshot.delivery_completion_time_ps,
              base + 34 * kSecondPs / 10 + propagation_ps);
    EXPECT_EQ(completed,
              (std::vector<std::pair<AtlahsFlowId, std::uint64_t>>{
                  {10, base + 34 * kSecondPs / 10 + propagation_ps}}));
    EXPECT_TRUE(snapshot.completion_notified);
    EXPECT_EQ(runtime.backloggedFlowCount(), 0U);
    EXPECT_EQ(runtime.pendingSourcePacketCount(), 0U);
    EXPECT_EQ(runtime.pendingDeliveryPacketCount(), 0U);
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());
}

TEST(RnicPacketizedManifoldRuntimeTest,
     SameTimestampJoinImmediatelyChangesCentralMaxMinTable) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    RnicPacketizedManifoldRuntime runtime(event_list, 120, 15, 0);
    runtime.setup(3, [](AtlahsFlowId) {});

    runtime.send(request(20, 0, 2, 30));
    EXPECT_EQ(runtime.flow(20).wire_rate_grant_bps, 120U);
    runtime.send(request(21, 1, 2, 30));
    EXPECT_EQ(runtime.flow(20).wire_rate_grant_bps, 60U);
    EXPECT_EQ(runtime.flow(21).wire_rate_grant_bps, 60U);
    EXPECT_EQ(runtime.nextSlotStartPs(), base);

    ASSERT_TRUE(EventList::doNextEvent());
    const std::uint64_t first_reserved =
        runtime.flow(20).packets_reserved + runtime.flow(21).packets_reserved;
    EXPECT_EQ(first_reserved, 1U);

    while (EventList::doNextEvent()) {
    }
}

TEST(RnicPacketizedManifoldRuntimeTest,
     SameTimestampJoinDoesNotRewriteCommittedEnvelope) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    RnicPacketizedManifoldRuntime runtime(event_list, 120, 15, 0);
    runtime.setup(3, [](AtlahsFlowId) {});

    runtime.send(request(22, 0, 2, 30));
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_EQ(EventList::now(), base);
    ASSERT_EQ(runtime.flow(22).packets_reserved, 1U);

    // The slot event already committed the envelope at this timestamp. A
    // later same-time join updates only the next unreserved envelope.
    runtime.send(request(23, 1, 2, 30));
    EXPECT_EQ(runtime.flow(22).packets_reserved, 1U);
    EXPECT_EQ(runtime.flow(23).packets_reserved, 0U);
    EXPECT_EQ(runtime.flow(22).wire_rate_grant_bps, 60U);
    EXPECT_EQ(runtime.flow(23).wire_rate_grant_bps, 60U);
    EXPECT_GT(*runtime.nextSlotStartPs(), base);

    while (EventList::doNextEvent()) {
    }
}

TEST(RnicPacketizedManifoldRuntimeTest,
     IndependentEndpointPairsReserveAndCompleteInParallel) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    std::vector<AtlahsFlowId> completed;
    RnicPacketizedManifoldRuntime runtime(event_list, 80, 10, 0);
    runtime.setup(4, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });

    runtime.send(request(30, 0, 2, 10));
    runtime.send(request(31, 1, 3, 10));
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(runtime.flow(30).packets_reserved, 1U);
    EXPECT_EQ(runtime.flow(31).packets_reserved, 1U);
    EXPECT_EQ(runtime.backloggedFlowCount(), 0U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + kSecondPs);
    EXPECT_TRUE(completed.empty());
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + 2 * kSecondPs);
    EXPECT_EQ(completed, (std::vector<AtlahsFlowId>{30, 31}));
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicPacketizedManifoldRuntimeTest,
     ZeroPayloadEmitsNoDataAndCompletesAfterPropagationOnly) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    constexpr std::uint64_t propagation_ps = 777;
    std::vector<AtlahsFlowId> completed;
    RnicPacketizedManifoldRuntime runtime(event_list, 80, 10, propagation_ps);
    runtime.setup(2, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });

    runtime.send(request(40, 0, 1, 0));
    const RnicPacketizedFlowSnapshot before = runtime.flow(40);
    EXPECT_EQ(before.total_packet_count, 0U);
    EXPECT_EQ(before.total_wire_bytes, 0U);
    EXPECT_EQ(before.source_completion_time_ps, base);
    EXPECT_EQ(runtime.pendingSourcePacketCount(), 0U);
    EXPECT_EQ(runtime.pendingDeliveryPacketCount(), 0U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + propagation_ps);
    EXPECT_EQ(completed, (std::vector<AtlahsFlowId>{40}));
    EXPECT_EQ(runtime.flow(40).delivery_completion_time_ps,
              base + propagation_ps);
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicPacketizedManifoldRuntimeTest,
     StopsWhileIdleAndRebasesTheNextBusyPeriod) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    std::vector<std::pair<AtlahsFlowId, std::uint64_t>> completed;
    RnicPacketizedManifoldRuntime runtime(event_list, 80, 10, 0);
    runtime.setup(2, [&](AtlahsFlowId flow_id) {
        completed.emplace_back(flow_id, EventList::now());
    });
    runtime.send(request(50, 0, 1, 10));

    CallbackEvent later(event_list, base + 10 * kSecondPs, [&] {
        runtime.send(request(51, 0, 1, 10));
        EXPECT_EQ(runtime.nextSlotStartPs(), EventList::now());
    });
    while (EventList::doNextEvent()) {
    }

    EXPECT_EQ(completed,
              (std::vector<std::pair<AtlahsFlowId, std::uint64_t>>{
                  {50, base + 2 * kSecondPs},
                  {51, base + 12 * kSecondPs}}));
}

TEST(RnicPacketizedManifoldRuntimeTest,
     RebaseUsesArrivalWhenFractionalBoundaryCeilEqualsNow) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    constexpr std::uint64_t capacity = UINT64_C(7000000000000);
    RnicPacketizedManifoldRuntime runtime(event_list, capacity, 3, 0);
    runtime.setup(2, [](AtlahsFlowId) {});
    runtime.send(request(52, 0, 1, 3));

    // Reserve the first packet. Its exact terminal boundary is base + 24/7 ps,
    // whose published timestamp is base + 4 ps.
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_EQ(runtime.flow(52).packets_reserved, 1U);

    CallbackEvent arrival(event_list, base + 4, [&] {
        runtime.send(request(53, 0, 1, 1));
    });
    while (EventList::doNextEvent()) {
    }

    // The second busy period starts exactly at the observable arrival time.
    // Reusing the previous exact 24/7-ps boundary would complete at 7/8 ps.
    EXPECT_EQ(runtime.flow(53).source_completion_time_ps, base + 8);
    EXPECT_EQ(runtime.flow(53).delivery_completion_time_ps, base + 9);
}

TEST(RnicPacketizedManifoldRuntimeTest,
     WholeBpsFloorCanLeavePacketFlowsDormantWithoutIdleEvents) {
    EventList& event_list = testEventList();
    RnicPacketizedManifoldRuntime runtime(event_list, 1, 1, 0);
    runtime.setup(3, [](AtlahsFlowId) {});

    runtime.send(request(60, 0, 2, 1));
    runtime.send(request(61, 1, 2, 1));
    EXPECT_EQ(runtime.flow(60).wire_rate_grant_bps, 0U);
    EXPECT_EQ(runtime.flow(61).wire_rate_grant_bps, 0U);
    EXPECT_FALSE(runtime.nextSlotStartPs().has_value());
    EXPECT_TRUE(runtime.hasPendingPhysicalWork());
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicPacketizedManifoldRuntimeTest,
     PositiveIndependentFlowRebasesPastDormantZeroGrantEpoch) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    RnicPacketizedManifoldRuntime runtime(event_list, 1, 1, 0);
    runtime.setup(5, [](AtlahsFlowId) {});
    runtime.send(request(62, 0, 2, 1));
    runtime.send(request(63, 1, 2, 1));
    ASSERT_EQ(runtime.flow(62).wire_rate_grant_bps, 0U);
    ASSERT_EQ(runtime.flow(63).wire_rate_grant_bps, 0U);

    bool admitted = false;
    CallbackEvent later(event_list, base + 100, [&] {
        EXPECT_NO_THROW(runtime.send(request(64, 3, 4, 1)));
        admitted = runtime.contains(64);
        EXPECT_EQ(runtime.flow(64).wire_rate_grant_bps, 1U);
        EXPECT_EQ(runtime.nextSlotStartPs(), EventList::now());
    });
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(admitted);
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(runtime.flow(64).packets_reserved, 1U);
    EXPECT_EQ(runtime.flow(62).packets_reserved, 0U);
    EXPECT_EQ(runtime.flow(63).packets_reserved, 0U);
}

TEST(RnicPacketizedManifoldRuntimeTest,
     ReentrantAndThrowingCallbacksRemainExactlyOnce) {
    EventList& event_list = testEventList();
    std::vector<AtlahsFlowId> completed;
    RnicPacketizedManifoldRuntime runtime(event_list, 80, 10, 0);
    runtime.setup(4, [&](AtlahsFlowId flow_id) {
        completed.push_back(flow_id);
        if (flow_id == 70) {
            runtime.send(request(72, 3, 0, 0));
            throw std::runtime_error("intentional completion failure");
        }
    });
    runtime.send(request(70, 0, 2, 10));
    runtime.send(request(71, 1, 3, 10));

    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_THROW(EventList::doNextEvent(), std::runtime_error);
    EXPECT_EQ(completed, (std::vector<AtlahsFlowId>{70, 71}));
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(completed, (std::vector<AtlahsFlowId>{70, 71, 72}));
    EXPECT_TRUE(runtime.flow(70).completion_notified);
    EXPECT_TRUE(runtime.flow(71).completion_notified);
    EXPECT_TRUE(runtime.flow(72).completion_notified);
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicPacketizedManifoldRuntimeTest,
     RejectsLifecycleIdentityLedgerAndTimeOverflowTransactionally) {
    EventList& event_list = testEventList();
    EXPECT_THROW(
        RnicPacketizedManifoldRuntime(event_list, 0, 10, 0),
        std::invalid_argument);

    RnicPacketizedManifoldRuntime runtime(
        event_list, 80, RnicDataPacketizationConfig(2, 1), 0);
    EXPECT_THROW(runtime.send(request(80, 0, 1, 1)), std::logic_error);
    EXPECT_THROW(runtime.setup(0, [](AtlahsFlowId) {}), std::invalid_argument);
    EXPECT_THROW(runtime.setup(2, {}), std::invalid_argument);
    runtime.setup(2, [](AtlahsFlowId) {});
    EXPECT_THROW(runtime.setup(2, [](AtlahsFlowId) {}), std::logic_error);

    AtlahsFlowRequest wrong_time = request(80, 0, 1, 1);
    ++wrong_time.start_time_ps;
    EXPECT_THROW(runtime.send(wrong_time), std::invalid_argument);
    EXPECT_THROW(runtime.send(request(80, 2, 1, 1)), std::out_of_range);
    EXPECT_THROW(runtime.send(request(80, 0, 2, 1)), std::out_of_range);
    EXPECT_THROW(runtime.flow(999), std::out_of_range);

    EXPECT_THROW(
        runtime.send(request(
            80, 0, 1, std::numeric_limits<std::uint64_t>::max())),
        std::overflow_error);
    EXPECT_FALSE(runtime.contains(80));

    const std::uint64_t now = EventList::now();
    RnicPacketizedManifoldRuntime overflowing(
        event_list,
        80,
        10,
        std::numeric_limits<std::uint64_t>::max() - now);
    overflowing.setup(2, [](AtlahsFlowId) {});
    EXPECT_THROW(overflowing.send(request(81, 0, 1, 10)),
                 std::overflow_error);
    EXPECT_FALSE(overflowing.contains(81));
}

TEST(RnicPacketizedManifoldRuntimeTest,
     DestructorCancelsItsOnlyPendingEvent) {
    EventList& event_list = testEventList();
    {
        RnicPacketizedManifoldRuntime runtime(event_list, 80, 10, 0);
        runtime.setup(2, [](AtlahsFlowId) {});
        runtime.send(request(90, 0, 1, 10));
    }
    EXPECT_FALSE(EventList::doNextEvent());
}

}  // namespace
