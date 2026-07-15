// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_max_min_allocator.h"

#include <algorithm>
#include <cstdint>
#include <limits>
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

TEST(RnicMaxMinAllocatorTest, CarriesFractionalFrozenLoadIntoLaterBottleneck) {
    const std::vector<Flow> flows{
        {0, 1, 0, std::nullopt},
        {1, 0, 0, std::nullopt},
        {2, 1, 0, std::nullopt},
        {3, 1, 0, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{0, 147}, {1, 59}}, {{0, 82}});

    // The three source-1 flows freeze at exactly 59/3. Their exact aggregate
    // load is 59, so the remaining flow receives exactly 82 - 59 = 23 bps.
    EXPECT_EQ(allocation.at(0), 19u);
    EXPECT_EQ(allocation.at(1), 23u);
    EXPECT_EQ(allocation.at(2), 19u);
    EXPECT_EQ(allocation.at(3), 19u);
}

TEST(RnicMaxMinAllocatorTest, SupportsSingleFlowAtFullWidthCapacity) {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const std::vector<Flow> flows{{1, 10, 20, std::nullopt}};

    const auto allocation = Allocator::allocate(
        flows, {{10, maximum}}, {{20, maximum}});

    EXPECT_EQ(allocation.at(1), maximum);
}

TEST(RnicMaxMinAllocatorTest, SplitsFullWidthCapacityExactly) {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const std::vector<Flow> flows{
        {1, 10, 21, std::nullopt},
        {2, 10, 22, std::nullopt},
        {3, 10, 23, std::nullopt},
    };

    const auto allocation = Allocator::allocate(
        flows,
        {{10, maximum}},
        {{21, maximum}, {22, maximum}, {23, maximum}});

    constexpr uint64_t exact_third = maximum / 3;
    EXPECT_EQ(allocation.at(1), exact_third);
    EXPECT_EQ(allocation.at(2), exact_third);
    EXPECT_EQ(allocation.at(3), exact_third);
}

TEST(RnicMaxMinAllocatorTest, NormalizesFullWidthHalfRatesWithoutTransientOverflow) {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const std::vector<Flow> flows{
        {1, 10, 21, std::nullopt},
        {2, 10, 22, std::nullopt},
    };

    const auto allocation = Allocator::allocate(
        flows, {{10, maximum}}, {{21, maximum}, {22, maximum}});

    EXPECT_EQ(allocation.at(1), maximum / 2);
    EXPECT_EQ(allocation.at(2), maximum / 2);
}

TEST(RnicMaxMinAllocatorTest, PreservesCapacityAboveDoubleIntegerPrecision) {
    constexpr uint64_t capacity = (uint64_t{1} << 53) + 1;
    const std::vector<Flow> flows{{1, 10, 20, std::nullopt}};

    const auto allocation = Allocator::allocate(
        flows, {{10, capacity}}, {{20, capacity}});

    EXPECT_EQ(allocation.at(1), capacity);
}

TEST(RnicMaxMinAllocatorTest, PreservesMultiStageRationalWaterLevels) {
    const std::vector<Flow> flows{
        // Source 0 freezes these two flows at 1/2.
        {0, 0, 0, std::nullopt},
        {1, 0, 1, std::nullopt},
        // Destination 0 then freezes these three flows at 5/6.
        {2, 1, 0, std::nullopt},
        {3, 2, 0, std::nullopt},
        {4, 3, 0, std::nullopt},
        // Source 1 finally freezes these five flows at 31/30.
        {5, 1, 2, std::nullopt},
        {6, 1, 3, std::nullopt},
        {7, 1, 4, std::nullopt},
        {8, 1, 5, std::nullopt},
        {9, 1, 6, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows,
        {{0, 1}, {1, 6}, {2, 100}, {3, 100}},
        {{0, 3}, {1, 100}, {2, 100}, {3, 100},
         {4, 100}, {5, 100}, {6, 100}});

    EXPECT_EQ(allocation.at(0), 0u);
    EXPECT_EQ(allocation.at(1), 0u);
    EXPECT_EQ(allocation.at(2), 0u);
    EXPECT_EQ(allocation.at(3), 0u);
    EXPECT_EQ(allocation.at(4), 0u);
    EXPECT_EQ(allocation.at(5), 1u);
    EXPECT_EQ(allocation.at(6), 1u);
    EXPECT_EQ(allocation.at(7), 1u);
    EXPECT_EQ(allocation.at(8), 1u);
    EXPECT_EQ(allocation.at(9), 1u);
}

TEST(RnicMaxMinAllocatorTest, FreezesExactSourceAndDestinationTiesTogether) {
    std::vector<Flow> flows{
        {3, 10, 20, std::nullopt},
        {1, 10, 21, std::nullopt},
        {2, 11, 20, std::nullopt},
    };
    const Allocator::CapacityMap sources{{10, 2}, {11, 100}};
    const Allocator::CapacityMap destinations{{20, 2}, {21, 100}};

    const auto forward = Allocator::allocate(flows, sources, destinations);
    std::reverse(flows.begin(), flows.end());
    const auto reversed = Allocator::allocate(flows, sources, destinations);

    EXPECT_EQ(forward, reversed);
    EXPECT_EQ(forward.at(1), 1u);
    EXPECT_EQ(forward.at(2), 1u);
    EXPECT_EQ(forward.at(3), 1u);
}

TEST(RnicMaxMinAllocatorTest, ComponentwiseFloorLeavesResidualIdle) {
    const std::vector<Flow> flows{
        {1, 10, 21, std::nullopt},
        {2, 10, 22, std::nullopt},
        {3, 10, 23, std::nullopt},
    };
    const auto allocation = Allocator::allocate(
        flows, {{10, 100}}, {{21, 100}, {22, 100}, {23, 100}});

    EXPECT_EQ(allocation.at(1), 33u);
    EXPECT_EQ(allocation.at(2), 33u);
    EXPECT_EQ(allocation.at(3), 33u);
    EXPECT_EQ(allocation.at(1) + allocation.at(2) + allocation.at(3), 99u);
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

TEST(RnicMaxMinAllocatorTest, ZeroSourceDestinationAndDemandTiesFreezeAtZero) {
    const std::vector<Flow> flows{
        {1, 10, 20, std::nullopt},
        {2, 11, 21, std::nullopt},
        {3, 12, 22, 0},
    };
    const auto allocation = Allocator::allocate(
        flows,
        {{10, 0}, {11, 100}, {12, 100}},
        {{20, 100}, {21, 0}, {22, 100}});

    EXPECT_EQ(allocation.at(1), 0u);
    EXPECT_EQ(allocation.at(2), 0u);
    EXPECT_EQ(allocation.at(3), 0u);
}

TEST(RnicMaxMinAllocatorTest, RejectsRationalOutsideChecked128BitDomain) {
    constexpr size_t stage_count = 128;
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    std::vector<Flow> flows;
    Allocator::CapacityMap sources;
    Allocator::CapacityMap destinations;

    const auto source_resource = [](size_t stage) {
        return static_cast<uint32_t>(1000 + stage);
    };
    const auto destination_resource = [](size_t stage) {
        return static_cast<uint32_t>(2000 + stage);
    };

    for (size_t stage = 0; stage < stage_count; ++stage) {
        const uint64_t capacity = static_cast<uint64_t>(stage + 1);
        if (stage % 2 == 0) {
            sources.emplace(source_resource(stage), capacity);
        } else {
            destinations.emplace(destination_resource(stage), capacity);
        }
    }

    uint64_t flow_id = 1;
    for (size_t stage = 0; stage < stage_count; ++stage) {
        if (stage % 2 == 0) {
            const uint32_t auxiliary_destination =
                static_cast<uint32_t>(300000 + 2 * stage);
            destinations.emplace(auxiliary_destination, maximum);
            flows.push_back({flow_id++,
                             source_resource(stage),
                             auxiliary_destination,
                             std::nullopt});
        } else {
            const uint32_t auxiliary_source =
                static_cast<uint32_t>(400000 + 2 * stage);
            sources.emplace(auxiliary_source, maximum);
            flows.push_back({flow_id++,
                             auxiliary_source,
                             destination_resource(stage),
                             std::nullopt});
        }

        if (stage + 1 < stage_count) {
            if (stage % 2 == 0) {
                flows.push_back({flow_id++,
                                 source_resource(stage),
                                 destination_resource(stage + 1),
                                 std::nullopt});
            } else {
                flows.push_back({flow_id++,
                                 source_resource(stage + 1),
                                 destination_resource(stage),
                                 std::nullopt});
            }
            continue;
        }

        // The final stage needs a second active flow but no outgoing bridge.
        const uint32_t auxiliary_source = 500000;
        sources.emplace(auxiliary_source, maximum);
        flows.push_back({flow_id++,
                         auxiliary_source,
                         destination_resource(stage),
                         std::nullopt});
    }

    EXPECT_THROW(Allocator::allocate(flows, sources, destinations),
                 std::overflow_error);
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
