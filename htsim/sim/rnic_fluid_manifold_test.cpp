// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_fluid_manifold.h"

#include <cstdint>
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
}

}  // namespace
