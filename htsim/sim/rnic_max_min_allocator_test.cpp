// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_max_min_allocator.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Allocator = RnicMaxMinAllocator;
using Flow = RnicMaxMinFlow;

TEST(RnicMaxMinAllocatorTest, EmptyInputProducesEmptyAllocation) {
    EXPECT_TRUE(Allocator::allocate({}, {}, {}).empty());
}

TEST(RnicMaxMinAllocatorTest, SingleFlowUsesTightestConstraint) {
    const std::vector<Flow> flows{{1, 10, 20, 70}};
    const auto allocation = Allocator::allocate(flows, {{10, 100}}, {{20, 80}});

    EXPECT_EQ(allocation.at(1), 70u);
}

TEST(RnicMaxMinAllocatorTest, SharedSourceCapacityIsSplitEqually) {
    const std::vector<Flow> flows{
        {2, 10, 22, std::nullopt},
        {1, 10, 21, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 100}}, {{21, 100}, {22, 100}});

    EXPECT_EQ(allocation.at(1), 50u);
    EXPECT_EQ(allocation.at(2), 50u);
}

TEST(RnicMaxMinAllocatorTest, SharedDestinationCapacityIsSplitEqually) {
    const std::vector<Flow> flows{
        {1, 10, 20, std::nullopt},
        {2, 11, 20, std::nullopt},
        {3, 12, 20, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 100}, {11, 100}, {12, 100}}, {{20, 90}});

    EXPECT_EQ(allocation.at(1), 30u);
    EXPECT_EQ(allocation.at(2), 30u);
    EXPECT_EQ(allocation.at(3), 30u);
}

TEST(RnicMaxMinAllocatorTest, DemandCappedFlowReleasesSharedCapacity) {
    const std::vector<Flow> flows{
        {1, 10, 21, 20},
        {2, 10, 22, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 100}}, {{21, 100}, {22, 100}});

    EXPECT_EQ(allocation.at(1), 20u);
    EXPECT_EQ(allocation.at(2), 80u);
}

TEST(RnicMaxMinAllocatorTest, ProgressivelyFillsAcrossCrossedBottlenecks) {
    const std::vector<Flow> flows{
        {1, 10, 20, std::nullopt},
        {2, 10, 21, std::nullopt},
        {3, 11, 20, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 100}, {11, 100}}, {{20, 60}, {21, 100}});

    EXPECT_EQ(allocation.at(1), 30u);
    EXPECT_EQ(allocation.at(2), 70u);
    EXPECT_EQ(allocation.at(3), 30u);
}

TEST(RnicMaxMinAllocatorTest, ReleasesCapacityAfterSourceAndDestinationBottlenecks) {
    const std::vector<Flow> flows{
        {1, 10, 20, std::nullopt},
        {2, 10, 21, std::nullopt},
        {3, 11, 20, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 10}, {11, 100}}, {{20, 20}, {21, 100}});

    EXPECT_EQ(allocation.at(1), 5u);
    EXPECT_EQ(allocation.at(2), 5u);
    EXPECT_EQ(allocation.at(3), 15u);
}

TEST(RnicMaxMinAllocatorTest, TiedFractionalRatesAreStableAcrossInputOrder) {
    std::vector<Flow> flows{
        {3, 10, 23, std::nullopt},
        {1, 10, 21, std::nullopt},
        {2, 10, 22, std::nullopt},
    };
    const Allocator::CapacityMap sources{{10, 100}};
    const Allocator::CapacityMap destinations{{21, 100}, {22, 100}, {23, 100}};

    const auto forward = Allocator::allocate(flows, sources, destinations);
    std::reverse(flows.begin(), flows.end());
    const auto reversed = Allocator::allocate(flows, sources, destinations);

    EXPECT_EQ(forward, reversed);
    EXPECT_EQ(forward.at(1), 33u);
    EXPECT_EQ(forward.at(2), 33u);
    EXPECT_EQ(forward.at(3), 33u);
}

TEST(RnicMaxMinAllocatorTest, ZeroCapacityFreezesAffectedFlowsAtZero) {
    const std::vector<Flow> flows{
        {1, 10, 20, std::nullopt},
        {2, 11, 21, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 0}, {11, 50}}, {{20, 100}, {21, 50}});

    EXPECT_EQ(allocation.at(1), 0u);
    EXPECT_EQ(allocation.at(2), 50u);
}

TEST(RnicMaxMinAllocatorTest, RejectsDuplicateFlowIds) {
    const std::vector<Flow> flows{
        {1, 10, 20, std::nullopt},
        {1, 11, 21, std::nullopt},
    };

    EXPECT_THROW(
        Allocator::allocate(flows, {{10, 100}, {11, 100}}, {{20, 100}, {21, 100}}),
        std::invalid_argument);
}

TEST(RnicMaxMinAllocatorTest, RejectsMissingAccessCapacity) {
    const std::vector<Flow> flows{{1, 10, 20, std::nullopt}};

    EXPECT_THROW(Allocator::allocate(flows, {}, {{20, 100}}), std::invalid_argument);
    EXPECT_THROW(Allocator::allocate(flows, {{10, 100}}, {}), std::invalid_argument);
}

}  // namespace
