// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "dragonfly_endpoint_placement.h"

namespace {

using htsim::DragonflyEndpointGeometry;
using htsim::DragonflyEndpointPlacementEnsemble;

constexpr DragonflyEndpointGeometry kP2A4H2{2, 4, 2};

TEST(DragonflyEndpointPlacementTest, DerivesCanonicalP2A4H2DimensionsAndSixtyFourRanks) {
    const DragonflyEndpointPlacementEnsemble ensemble(kP2A4H2);

    EXPECT_EQ(ensemble.hostsPerRouter(), 2U);
    EXPECT_EQ(ensemble.routersPerGroup(), 4U);
    EXPECT_EQ(ensemble.globalLinksPerRouter(), 2U);
    EXPECT_EQ(ensemble.groupCount(), 9U);
    EXPECT_EQ(ensemble.routerCount(), 36U);
    EXPECT_EQ(ensemble.endpointsPerGroup(), 8U);
    EXPECT_EQ(ensemble.physicalEndpointCount(), 72U);
    EXPECT_EQ(ensemble.logicalRankCount(), 64U);
    EXPECT_EQ(ensemble.placementCount(), 9U);
    EXPECT_TRUE(ensemble.validate());
}

TEST(DragonflyEndpointPlacementTest, EachPlacementHasOneFullGroupAndOneIdleEndpointElsewhere) {
    const DragonflyEndpointPlacementEnsemble ensemble(kP2A4H2);

    for (std::uint32_t placement_index = 0; placement_index < ensemble.placementCount();
         ++placement_index) {
        const auto& placement = ensemble.placement(placement_index);
        EXPECT_EQ(placement.index(), placement_index);
        EXPECT_EQ(placement.fullGroup(), placement_index);
        EXPECT_EQ(placement.logicalRankCount(), 64U);
        EXPECT_EQ(placement.physicalEndpointCount(), 72U);
        EXPECT_EQ(placement.activeEndpoints().size(), 64U);
        EXPECT_EQ(placement.idleEndpoints().size(), 8U);

        for (std::uint32_t group = 0; group < ensemble.groupCount(); ++group) {
            EXPECT_EQ(placement.activeCountForGroup(group), group == placement_index ? 8U : 7U);
            const auto idle_in_group =
                std::count_if(placement.idleEndpoints().begin(), placement.idleEndpoints().end(),
                              [&](std::uint32_t physical) {
                                  return ensemble.groupForPhysicalEndpoint(physical) == group;
                              });
            EXPECT_EQ(idle_in_group, group == placement_index ? 0 : 1);
        }
    }
}

TEST(DragonflyEndpointPlacementTest, FullGroupAndIdleEndpointsRotateExactlyOnce) {
    const DragonflyEndpointPlacementEnsemble ensemble(kP2A4H2);
    std::vector<std::uint32_t> full_group_frequency(ensemble.groupCount(), 0);
    std::vector<std::uint32_t> idle_frequency(ensemble.physicalEndpointCount(), 0);

    for (std::uint32_t index = 0; index < ensemble.placementCount(); ++index) {
        const auto& placement = ensemble.placement(index);
        ++full_group_frequency[placement.fullGroup()];
        for (const auto endpoint : placement.idleEndpoints()) {
            ++idle_frequency[endpoint];
        }
    }

    EXPECT_TRUE(std::all_of(full_group_frequency.begin(), full_group_frequency.end(),
                            [](std::uint32_t count) { return count == 1; }));
    EXPECT_TRUE(std::all_of(idle_frequency.begin(), idle_frequency.end(),
                            [](std::uint32_t count) { return count == 1; }));
}

TEST(DragonflyEndpointPlacementTest, LogicalAndPhysicalMappingsCoverSameSetAndAreMutualInverses) {
    const DragonflyEndpointPlacementEnsemble ensemble(kP2A4H2);

    for (std::uint32_t index = 0; index < ensemble.placementCount(); ++index) {
        const auto& placement = ensemble.placement(index);
        auto logical_endpoint_set = placement.logicalToPhysical();
        std::sort(logical_endpoint_set.begin(), logical_endpoint_set.end());
        EXPECT_EQ(logical_endpoint_set, placement.activeEndpoints());
        EXPECT_TRUE(
            std::is_sorted(placement.activeEndpoints().begin(), placement.activeEndpoints().end()));

        for (std::uint32_t rank = 0; rank < ensemble.logicalRankCount(); ++rank) {
            const auto physical = placement.physicalEndpointForRank(rank);
            EXPECT_EQ(placement.logicalRankForPhysicalEndpoint(physical), rank);
            EXPECT_TRUE(placement.isActive(physical));
        }
        for (const auto physical : placement.idleEndpoints()) {
            EXPECT_EQ(placement.logicalRankForPhysicalEndpoint(physical), std::nullopt);
            EXPECT_FALSE(placement.isActive(physical));
        }
    }
}

TEST(DragonflyEndpointPlacementTest, EveryLogicalRankRoleVisitsEveryGroupExactlyOnce) {
    const DragonflyEndpointPlacementEnsemble ensemble(kP2A4H2);
    std::vector<std::vector<std::uint32_t>> group_frequency_by_rank(
        ensemble.logicalRankCount(), std::vector<std::uint32_t>(ensemble.groupCount(), 0));

    for (std::uint32_t placement_index = 0; placement_index < ensemble.placementCount();
         ++placement_index) {
        const auto& placement = ensemble.placement(placement_index);
        EXPECT_EQ(ensemble.groupForPhysicalEndpoint(placement.physicalEndpointForRank(0)),
                  placement_index);

        for (std::uint32_t rank = 0; rank < ensemble.logicalRankCount(); ++rank) {
            const auto group =
                ensemble.groupForPhysicalEndpoint(placement.physicalEndpointForRank(rank));
            ++group_frequency_by_rank[rank][group];
        }
    }

    for (const auto& group_frequency : group_frequency_by_rank) {
        EXPECT_TRUE(std::all_of(group_frequency.begin(), group_frequency.end(),
                                [](std::uint32_t count) { return count == 1; }));
    }
}

TEST(DragonflyEndpointPlacementTest, ConstructionIsDeterministicAndUsesStablePhysicalNumbering) {
    const DragonflyEndpointPlacementEnsemble first(kP2A4H2);
    const DragonflyEndpointPlacementEnsemble replay(kP2A4H2);

    for (std::uint32_t index = 0; index < first.placementCount(); ++index) {
        EXPECT_EQ(first.placement(index).logicalToPhysical(),
                  replay.placement(index).logicalToPhysical());
        EXPECT_EQ(first.placement(index).idleEndpoints(), replay.placement(index).idleEndpoints());
    }

    EXPECT_EQ(first.placement(0).idleEndpoints(),
              (std::vector<std::uint32_t>{15, 22, 29, 36, 43, 50, 57, 64}));
    EXPECT_EQ(first.groupForPhysicalEndpoint(71), 8U);
    EXPECT_EQ(first.routerForPhysicalEndpoint(71), 35U);
    EXPECT_EQ(first.hostSlotForPhysicalEndpoint(71), 1U);
}

TEST(DragonflyEndpointPlacementTest, SupportsOtherBalancedMaximumSizeDragonflies) {
    const DragonflyEndpointPlacementEnsemble ensemble(DragonflyEndpointGeometry{3, 6, 3});

    EXPECT_EQ(ensemble.groupCount(), 19U);
    EXPECT_EQ(ensemble.endpointsPerGroup(), 18U);
    EXPECT_EQ(ensemble.physicalEndpointCount(), 342U);
    EXPECT_EQ(ensemble.logicalRankCount(), 324U);
    EXPECT_EQ(ensemble.placementCount(), 19U);
    EXPECT_TRUE(ensemble.validate());
}

TEST(DragonflyEndpointPlacementTest, RejectsZerosUnbalancedGeometriesAndOverflow) {
    EXPECT_FALSE(DragonflyEndpointPlacementEnsemble::geometryValidationError(
                     DragonflyEndpointGeometry{0, 4, 2})
                     .empty());
    EXPECT_THROW(DragonflyEndpointPlacementEnsemble(DragonflyEndpointGeometry{2, 4, 1}),
                 std::invalid_argument);
    EXPECT_THROW(DragonflyEndpointPlacementEnsemble(DragonflyEndpointGeometry{3, 3, 2}),
                 std::invalid_argument);
    EXPECT_THROW(DragonflyEndpointPlacementEnsemble(
                     DragonflyEndpointGeometry{std::numeric_limits<std::uint32_t>::max(), 2, 1}),
                 std::invalid_argument);
}

TEST(DragonflyEndpointPlacementTest, RejectsOutOfRangeLookups) {
    const DragonflyEndpointPlacementEnsemble ensemble(kP2A4H2);
    const auto& placement = ensemble.placement(0);

    EXPECT_THROW(ensemble.placement(9), std::out_of_range);
    EXPECT_THROW(placement.physicalEndpointForRank(64), std::out_of_range);
    EXPECT_THROW(placement.logicalRankForPhysicalEndpoint(72), std::out_of_range);
    EXPECT_THROW(placement.isActive(72), std::out_of_range);
    EXPECT_THROW(placement.activeCountForGroup(9), std::out_of_range);
    EXPECT_THROW(ensemble.groupForPhysicalEndpoint(72), std::out_of_range);
    EXPECT_THROW(ensemble.routerForPhysicalEndpoint(72), std::out_of_range);
    EXPECT_THROW(ensemble.hostSlotForPhysicalEndpoint(72), std::out_of_range);
}

}  // namespace
