// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_fluid_manifold_runtime.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::uint64_t kSecondPs = 1000000000000ULL;

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
        : EventSource(event_list, "fluid-runtime-test-callback"),
          _callback(std::move(callback)) {
        EventList::sourceIsPending(*this, when);
    }

    void doNextEvent() override { _callback(); }

private:
    std::function<void()> _callback;
};

TEST(RnicFluidManifoldRuntimeTest,
     JoinImmediatelyReallocatesAndDeliveryAddsOnlyPropagation) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    constexpr std::uint64_t propagation_ps = 123;
    std::vector<AtlahsFlowId> completed;

    RnicFluidManifoldRuntime runtime(event_list, 80, propagation_ps);
    runtime.setup(3, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });
    runtime.send(request(10, 0, 2, 10));
    EXPECT_EQ(runtime.flow(10).rate_bps, 80U);

    CallbackEvent join(event_list, base + kSecondPs / 2, [&] {
        runtime.send(request(11, 1, 2, 10));
    });
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_EQ(EventList::now(), base + kSecondPs / 2);
    EXPECT_EQ(runtime.flow(10).rate_bps, 40U);
    EXPECT_EQ(runtime.flow(11).rate_bps, 40U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + 3 * kSecondPs / 2);
    EXPECT_TRUE(completed.empty());
    EXPECT_EQ(runtime.flow(10).service_completion_time_ps,
              base + 3 * kSecondPs / 2);
    EXPECT_EQ(runtime.flow(10).delivery_completion_time_ps,
              base + 3 * kSecondPs / 2 + propagation_ps);
    EXPECT_EQ(runtime.flow(11).rate_bps, 80U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + 3 * kSecondPs / 2 + propagation_ps);
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({10}));

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + 2 * kSecondPs);
    EXPECT_EQ(runtime.flow(11).service_completion_time_ps, base + 2 * kSecondPs);
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + 2 * kSecondPs + propagation_ps);
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({10, 11}));
    EXPECT_EQ(runtime.activeFlowCount(), 0U);
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicFluidManifoldRuntimeTest,
     SameTimestampJoinSettlesCompletedServiceBeforeReallocation) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    std::vector<AtlahsFlowId> completed;

    RnicFluidManifoldRuntime runtime(event_list, 80, 0);
    runtime.setup(3, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });

    // Insert the join first so it executes before the runtime's service event
    // at the same timestamp.  send() must cancel that stale event, settle flow
    // 20, and allocate the full link to flow 21 without double notification.
    CallbackEvent join(event_list, base + kSecondPs, [&] {
        runtime.send(request(21, 1, 2, 10));
    });
    runtime.send(request(20, 0, 2, 10));

    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_EQ(EventList::now(), base + kSecondPs);
    EXPECT_EQ(runtime.flow(20).service_completion_time_ps, base + kSecondPs);
    EXPECT_EQ(runtime.flow(21).rate_bps, 80U);
    EXPECT_TRUE(completed.empty());

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + kSecondPs);
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({20}));

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + 2 * kSecondPs);
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({20, 21}));
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicFluidManifoldRuntimeTest,
     ZeroByteAndReentrantSameTimestampCompletionsAreNotifiedOnce) {
    EventList& event_list = testEventList();
    std::vector<AtlahsFlowId> completed;
    RnicFluidManifoldRuntime runtime(event_list, 400000000000ULL, 0);

    runtime.setup(2, [&](AtlahsFlowId flow_id) {
        completed.push_back(flow_id);
        if (flow_id == 30) {
            runtime.send(request(31, 1, 0, 0));
        }
    });
    runtime.send(request(30, 0, 1, 0));
    EXPECT_EQ(runtime.activeFlowCount(), 0U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({30}));
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({30, 31}));
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicFluidManifoldRuntimeTest,
     ExactLargePayloadCompletesAfterServiceThenPropagation) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    constexpr std::uint64_t capacity_bps = 400000000000ULL;
    constexpr std::uint64_t payload_bytes = (std::uint64_t{1} << 53) + 1;
    constexpr std::uint64_t service_duration_ps = 180143985094819860ULL;
    constexpr std::uint64_t propagation_ps = 37;
    std::vector<std::pair<AtlahsFlowId, std::uint64_t>> completed;

    RnicFluidManifoldRuntime runtime(event_list, capacity_bps, propagation_ps);
    runtime.setup(2, [&](AtlahsFlowId flow_id) {
        completed.emplace_back(flow_id, EventList::now());
    });
    runtime.send(request(35, 0, 1, payload_bytes));
    EXPECT_EQ(runtime.flow(35).spec.size_bytes, payload_bytes);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + service_duration_ps);
    EXPECT_TRUE(completed.empty());
    EXPECT_EQ(runtime.flow(35).service_completion_time_ps,
              base + service_duration_ps);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + service_duration_ps + propagation_ps);
    EXPECT_EQ(completed,
              (std::vector<std::pair<AtlahsFlowId, std::uint64_t>>{
                  {35, base + service_duration_ps + propagation_ps}}));
    EXPECT_FALSE(EventList::doNextEvent());
}

TEST(RnicFluidManifoldRuntimeTest,
     ValidatesLifecycleIdentityNodesTimeAndArithmeticTransactionally) {
    EventList& event_list = testEventList();
    EXPECT_THROW(RnicFluidManifoldRuntime(event_list, 0, 0), std::invalid_argument);

    {
        RnicFluidManifoldRuntime full_capacity(
            event_list, std::numeric_limits<std::uint64_t>::max(), 0);
        full_capacity.setup(2, [](AtlahsFlowId) {});
        EXPECT_NO_THROW(full_capacity.send(request(39, 0, 1, 1)));
        EXPECT_EQ(full_capacity.flow(39).rate_bps,
                  std::numeric_limits<std::uint64_t>::max());
    }

    constexpr std::uint64_t link_capacity_bps = 4000000000000ULL;
    RnicFluidManifoldRuntime runtime(event_list, link_capacity_bps, 17);
    EXPECT_THROW(runtime.send(request(40, 0, 1, 1)), std::logic_error);
    EXPECT_THROW(runtime.setup(0, [](AtlahsFlowId) {}), std::invalid_argument);
    EXPECT_THROW(runtime.setup(2, {}), std::invalid_argument);

    runtime.setup(2, [](AtlahsFlowId) {});
    EXPECT_EQ(runtime.nodeCount(), 2U);
    EXPECT_EQ(runtime.nodeLinkCapacity(), link_capacity_bps);
    EXPECT_EQ(runtime.propagationDelay(), 17U);
    EXPECT_THROW(runtime.setup(2, [](AtlahsFlowId) {}), std::logic_error);

    AtlahsFlowRequest wrong_time = request(40, 0, 1, 1);
    wrong_time.start_time_ps++;
    EXPECT_THROW(runtime.send(wrong_time), std::invalid_argument);
    EXPECT_THROW(runtime.send(request(40, 2, 1, 1)), std::out_of_range);
    EXPECT_THROW(runtime.send(request(40, 0, 2, 1)), std::out_of_range);

    constexpr std::uint64_t exact_large_payload =
        (std::uint64_t{1} << 53) + 1;
    runtime.send(request(40, 0, 1, exact_large_payload));
    EXPECT_EQ(runtime.flow(40).spec.size_bytes, exact_large_payload);
    EXPECT_THROW(runtime.send(request(40, 0, 1, 1)), std::invalid_argument);

    RnicFluidManifoldRuntime overflowing(event_list, 1, 0);
    overflowing.setup(2, [](AtlahsFlowId) {});
    EXPECT_THROW(
        overflowing.send(request(
            41, 0, 1, std::numeric_limits<std::uint64_t>::max())),
        std::overflow_error);
    EXPECT_FALSE(overflowing.contains(41));
}

TEST(RnicFluidManifoldRuntimeTest,
     SimultaneousIndependentFlowsUseBothAccessLinksAndCompleteTogether) {
    EventList& event_list = testEventList();
    const std::uint64_t base = EventList::now();
    std::vector<AtlahsFlowId> completed;
    RnicFluidManifoldRuntime runtime(event_list, 80, 0);
    runtime.setup(4, [&](AtlahsFlowId flow_id) { completed.push_back(flow_id); });

    runtime.send(request(50, 0, 2, 10));
    runtime.send(request(51, 1, 3, 10));
    EXPECT_EQ(runtime.flow(50).rate_bps, 80U);
    EXPECT_EQ(runtime.flow(51).rate_bps, 80U);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(EventList::now(), base + kSecondPs);
    EXPECT_EQ(completed, std::vector<AtlahsFlowId>({50, 51}));
    EXPECT_FALSE(EventList::doNextEvent());
}

}  // namespace
