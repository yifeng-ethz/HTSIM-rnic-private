// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_fluid_manifold.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace {

constexpr uint64_t SECOND_PS = 1000000000000ULL;

TEST(RnicFluidManifoldTest, SingleFlowCompletionIncludesOnlyFixedPropagation) {
    RnicFluidManifold manifold({{1, 80}}, {{2, 80}}, 1234);
    manifold.addFlow({10, 1, 2, 10}, 0);

    EXPECT_EQ(manifold.flow(10).rate_bps, 80u);
    ASSERT_TRUE(manifold.nextServiceCompletionTime().has_value());
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), SECOND_PS);

    manifold.advanceTo(SECOND_PS);
    const RnicFluidFlowSnapshot result = manifold.flow(10);
    EXPECT_EQ(result.service_completion_time_ps, SECOND_PS);
    EXPECT_EQ(result.delivery_completion_time_ps, SECOND_PS + 1234);
    EXPECT_EQ(manifold.activeFlowCount(), 0u);
}

TEST(RnicFluidManifoldTest, SymmetricIncastUsesDestinationMaxMinShare) {
    RnicFluidManifold manifold({{1, 80}, {2, 80}}, {{3, 80}}, 0);
    manifold.addFlow({10, 1, 3, 10}, 0);
    manifold.addFlow({11, 2, 3, 10}, 0);

    EXPECT_EQ(manifold.flow(10).rate_bps, 40u);
    EXPECT_EQ(manifold.flow(11).rate_bps, 40u);
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), 2 * SECOND_PS);

    manifold.advanceTo(2 * SECOND_PS);
    EXPECT_EQ(manifold.flow(10).delivery_completion_time_ps, 2 * SECOND_PS);
    EXPECT_EQ(manifold.flow(11).delivery_completion_time_ps, 2 * SECOND_PS);
}

TEST(RnicFluidManifoldTest, JoinChangesRatesAtTheSameTimestamp) {
    RnicFluidManifold manifold({{1, 80}, {2, 80}}, {{3, 80}}, 0);
    manifold.addFlow({10, 1, 3, 10}, 0);
    const uint64_t first_epoch = manifold.allocationEpoch();
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), SECOND_PS);

    manifold.addFlow({11, 2, 3, 10}, SECOND_PS / 2);
    EXPECT_GT(manifold.allocationEpoch(), first_epoch);
    EXPECT_EQ(manifold.flow(10).rate_bps, 40u);
    EXPECT_EQ(manifold.flow(11).rate_bps, 40u);
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), 3 * SECOND_PS / 2);

    manifold.advanceTo(2 * SECOND_PS);
    EXPECT_EQ(manifold.flow(10).service_completion_time_ps, 3 * SECOND_PS / 2);
    EXPECT_EQ(manifold.flow(11).service_completion_time_ps, 2 * SECOND_PS);
}

TEST(RnicFluidManifoldTest, DemandCapReleasesCapacityAfterCompletion) {
    RnicFluidManifold manifold({{1, 80}, {2, 80}}, {{3, 80}, {4, 80}}, 0);
    manifold.addFlow({10, 1, 3, 5, 20}, 0);
    manifold.addFlow({11, 1, 4, 30}, 0);

    EXPECT_EQ(manifold.flow(10).rate_bps, 20u);
    EXPECT_EQ(manifold.flow(11).rate_bps, 60u);
    manifold.advanceTo(2 * SECOND_PS);
    EXPECT_EQ(manifold.flow(10).service_completion_time_ps, 2 * SECOND_PS);
    EXPECT_EQ(manifold.flow(11).rate_bps, 80u);
}

TEST(RnicFluidManifoldTest, FixedPropagationDoesNotChangeAfterLaterJoins) {
    RnicFluidManifold manifold({{1, 80}, {2, 80}}, {{3, 80}, {4, 80}}, 500);
    manifold.addFlow({10, 1, 3, 10}, 0);
    manifold.advanceTo(SECOND_PS);
    ASSERT_EQ(manifold.flow(10).delivery_completion_time_ps, SECOND_PS + 500);

    manifold.addFlow({11, 2, 4, 10}, SECOND_PS + 100);
    EXPECT_EQ(manifold.flow(10).delivery_completion_time_ps, SECOND_PS + 500);
}

TEST(RnicFluidManifoldTest, TimeShiftChangesOnlyAbsoluteEventTimes) {
    RnicFluidManifold baseline({{1, 80}}, {{2, 80}}, 100);
    baseline.addFlow({10, 1, 2, 10}, 0);

    RnicFluidManifold shifted({{1, 80}}, {{2, 80}}, 100);
    shifted.advanceTo(7 * SECOND_PS);
    shifted.addFlow({10, 1, 2, 10}, 7 * SECOND_PS);

    EXPECT_EQ(*shifted.nextServiceCompletionTime() - *baseline.nextServiceCompletionTime(),
              7 * SECOND_PS);
}

TEST(RnicFluidManifoldTest, PreservesPayloadBeyondFloatingPointMantissaExactly) {
    constexpr uint64_t capacity_bps = 400000000000ULL;
    constexpr uint64_t payload_bytes = (uint64_t{1} << 53) + 1;
    constexpr uint64_t exact_completion_ps = 180143985094819860ULL;
    RnicFluidManifold manifold({{1, capacity_bps}}, {{2, capacity_bps}}, 0);
    manifold.addFlow({10, 1, 2, payload_bytes}, 0);

    ASSERT_TRUE(manifold.projectedServiceCompletionTime(10).has_value());
    EXPECT_EQ(*manifold.projectedServiceCompletionTime(10), exact_completion_ps);
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), exact_completion_ps);

    manifold.advanceTo(exact_completion_ps - 1);
    EXPECT_TRUE(manifold.flow(10).active());
    manifold.advanceTo(exact_completion_ps);
    EXPECT_EQ(manifold.flow(10).service_completion_time_ps, exact_completion_ps);
}

TEST(RnicFluidManifoldTest, UsesExactCeilingAtNonIntegralServiceBoundary) {
    constexpr uint64_t exact_completion_ps = 4509333333333334ULL;
    RnicFluidManifold manifold({{1, 3}}, {{2, 3}}, 0);
    manifold.addFlow({10, 1, 2, 1691}, 0);

    EXPECT_EQ(*manifold.nextServiceCompletionTime(), exact_completion_ps);
    manifold.advanceTo(exact_completion_ps - 1);
    EXPECT_TRUE(manifold.flow(10).active());
    manifold.advanceTo(exact_completion_ps);
    EXPECT_EQ(manifold.flow(10).service_completion_time_ps, exact_completion_ps);
}

TEST(RnicFluidManifoldTest, CarriesExactServiceDebtAcrossAJoinEpoch) {
    RnicFluidManifold manifold({{1, 10}, {2, 10}}, {{3, 10}}, 0);
    manifold.addFlow({10, 1, 3, 1}, 0);
    manifold.addFlow({11, 2, 3, 1}, 1);

    EXPECT_EQ(manifold.flow(10).rate_bps, 5U);
    EXPECT_EQ(manifold.flow(11).rate_bps, 5U);
    EXPECT_EQ(*manifold.projectedServiceCompletionTime(10), 1599999999999ULL);
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), 1599999999999ULL);

    manifold.advanceTo(1599999999999ULL);
    EXPECT_EQ(manifold.flow(10).service_completion_time_ps, 1599999999999ULL);
    EXPECT_TRUE(manifold.flow(11).active());
    EXPECT_EQ(manifold.flow(11).rate_bps, 10U);
    EXPECT_EQ(*manifold.projectedServiceCompletionTime(11), 1600000000000ULL);
    EXPECT_EQ(*manifold.nextServiceCompletionTime(), 1600000000000ULL);

    manifold.advanceTo(1600000000000ULL);
    EXPECT_EQ(manifold.flow(11).service_completion_time_ps, 1600000000000ULL);
}

TEST(RnicFluidManifoldTest, DeliveryOverflowLeavesTiedCompletionsUnchanged) {
    constexpr uint64_t capacity_bps = 8000000000000ULL;
    constexpr uint64_t start_time_ps =
        std::numeric_limits<uint64_t>::max() - 1;
    constexpr uint64_t service_completion_time_ps =
        std::numeric_limits<uint64_t>::max();
    RnicFluidManifold manifold(
        {{1, capacity_bps}, {2, capacity_bps}},
        {{3, capacity_bps}, {4, capacity_bps}},
        1);
    manifold.advanceTo(start_time_ps);
    manifold.addFlow({10, 1, 3, 1}, start_time_ps);
    manifold.addFlow({11, 2, 4, 1}, start_time_ps);

    const uint64_t allocation_epoch = manifold.allocationEpoch();
    ASSERT_EQ(manifold.projectedServiceCompletionTime(10),
              service_completion_time_ps);
    ASSERT_EQ(manifold.projectedServiceCompletionTime(11),
              service_completion_time_ps);

    EXPECT_THROW(manifold.advanceTo(service_completion_time_ps),
                 std::overflow_error);

    EXPECT_EQ(manifold.now(), start_time_ps);
    EXPECT_EQ(manifold.allocationEpoch(), allocation_epoch);
    EXPECT_EQ(manifold.activeFlowCount(), 2U);
    for (uint64_t flow_id : {uint64_t{10}, uint64_t{11}}) {
        const RnicFluidFlowSnapshot snapshot = manifold.flow(flow_id);
        EXPECT_TRUE(snapshot.active());
        EXPECT_EQ(snapshot.rate_bps, capacity_bps);
        EXPECT_FALSE(snapshot.service_completion_time_ps.has_value());
        EXPECT_FALSE(snapshot.delivery_completion_time_ps.has_value());
        EXPECT_EQ(manifold.projectedServiceCompletionTime(flow_id),
                  service_completion_time_ps);
    }
}

TEST(RnicFluidManifoldTest, ZeroByteFlowHasExactZeroDebtAndPropagationOnly) {
    RnicFluidManifold manifold({{1, 80}}, {{2, 80}}, 123);
    manifold.addFlow({10, 1, 2, 0}, 77);

    const RnicFluidFlowSnapshot snapshot = manifold.flow(10);
    EXPECT_EQ(snapshot.service_completion_time_ps, 77U);
    EXPECT_EQ(snapshot.delivery_completion_time_ps, 200U);
    EXPECT_FALSE(snapshot.active());
    EXPECT_FALSE(manifold.projectedServiceCompletionTime(10).has_value());
    EXPECT_FALSE(manifold.nextServiceCompletionTime().has_value());
}

TEST(RnicFluidManifoldTest, ZeroCapacityHasNoInventedCompletion) {
    RnicFluidManifold manifold({{1, 0}}, {{2, 80}}, 0);
    manifold.addFlow({10, 1, 2, 10}, 0);

    EXPECT_EQ(manifold.flow(10).rate_bps, 0u);
    EXPECT_FALSE(manifold.nextServiceCompletionTime().has_value());
    manifold.advanceTo(10 * SECOND_PS);
    EXPECT_TRUE(manifold.flow(10).active());
}

TEST(RnicFluidManifoldTest, RejectsInvalidIdentityCapacityAndTime) {
    RnicFluidManifold manifold({{1, 80}}, {{2, 80}}, 0);
    manifold.addFlow({10, 1, 2, 10}, SECOND_PS);

    EXPECT_THROW(manifold.addFlow({10, 1, 2, 10}, SECOND_PS), std::invalid_argument);
    EXPECT_THROW(manifold.addFlow({11, 9, 2, 10}, SECOND_PS), std::invalid_argument);
    EXPECT_THROW(manifold.addFlow({12, 1, 9, 10}, SECOND_PS), std::invalid_argument);
    EXPECT_THROW(manifold.advanceTo(SECOND_PS - 1), std::invalid_argument);
    EXPECT_THROW(manifold.flow(999), std::out_of_range);
    EXPECT_THROW(manifold.projectedServiceCompletionTime(999), std::out_of_range);
}

}  // namespace
