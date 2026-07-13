// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_packetized_manifold.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Calendar = RnicPacketizedSlotCalendar;
using Grant = RnicPacketizedGrant;
using Reservation = RnicPacketizedReservation;

constexpr uint64_t kCapacity = UINT64_C(12000000000);
constexpr uint64_t kQuantumBytes = 1500;
constexpr uint64_t kExactSlotPs = 1000000;

std::vector<uint64_t> flowIds(const std::vector<Reservation>& reservations) {
    std::vector<uint64_t> ids;
    for (const Reservation& reservation : reservations) {
        ids.push_back(reservation.flowId());
    }
    return ids;
}

TEST(RnicPacketizedSlotCalendarTest, UsesCumulativeRationalSlotBoundaries) {
    Calendar calendar(UINT64_C(3000000000000), 1, 7);
    calendar.beginEpoch(0, {{1, 10, 20, UINT64_C(3000000000000)}});

    const std::vector<uint64_t> expected_boundaries{0, 3, 6, 8, 11};
    for (size_t slot = 0; slot + 1 < expected_boundaries.size(); ++slot) {
        const auto reservations = calendar.reserveNextSlot();
        ASSERT_EQ(reservations.size(), 1u);
        EXPECT_EQ(reservations[0].sourceSlotStartPs(), expected_boundaries[slot]);
        EXPECT_EQ(reservations[0].sourceSlotEndPs(), expected_boundaries[slot + 1]);
    }
    EXPECT_EQ(calendar.nextSlotStartPs(), 11u);
}

TEST(RnicPacketizedSlotCalendarTest, SymmetricIncastTracksMaxMinWithinOneQuantum) {
    Calendar calendar(kCapacity, kQuantumBytes, 0);
    calendar.beginMaxMinEpoch(0,
                              {{1, 10, 20}, {2, 11, 20}, {3, 12, 20}});

    std::map<uint64_t, uint64_t> reserved_bytes;
    for (int slot = 0; slot < 60; ++slot) {
        const auto reservations = calendar.reserveNextSlot();
        ASSERT_EQ(reservations.size(), 1u);
        reserved_bytes[reservations[0].flowId()] +=
            reservations[0].reservedWireBytes();
    }

    const auto minimum = std::min({reserved_bytes[1], reserved_bytes[2], reserved_bytes[3]});
    const auto maximum = std::max({reserved_bytes[1], reserved_bytes[2], reserved_bytes[3]});
    EXPECT_LE(maximum - minimum, kQuantumBytes);
}

TEST(RnicPacketizedSlotCalendarTest, FindsMaximumMatchingInsteadOfGreedySingleEdge) {
    Calendar calendar(kCapacity, kQuantumBytes, 0);
    calendar.beginMaxMinEpoch(0,
                              {{1, 10, 20}, {2, 10, 21}, {3, 11, 20}});

    const auto first = calendar.reserveNextSlot();
    EXPECT_EQ(flowIds(first), (std::vector<uint64_t>{2, 3}));

    const auto second = calendar.reserveNextSlot();
    EXPECT_EQ(flowIds(second), (std::vector<uint64_t>{1}));
}

TEST(RnicPacketizedSlotCalendarTest, NeverOverlapsSourceOrDestinationInASlot) {
    Calendar calendar(kCapacity, kQuantumBytes, 100);
    calendar.beginEpoch(0,
                        {{1, 10, 20, kCapacity / 2},
                         {2, 10, 21, kCapacity / 2},
                         {3, 11, 20, kCapacity / 2},
                         {4, 11, 21, kCapacity / 2}});

    for (int slot = 0; slot < 20; ++slot) {
        const auto reservations = calendar.reserveNextSlot();
        std::set<uint32_t> sources;
        std::set<uint32_t> destinations;
        for (const Reservation& reservation : reservations) {
            EXPECT_TRUE(sources.insert(reservation.sourceNode()).second);
            EXPECT_TRUE(destinations.insert(reservation.destinationNode()).second);
        }
    }
}

TEST(RnicPacketizedSlotCalendarTest, ResidenceIsFixedUnderLoad) {
    constexpr uint64_t kPropagationPs = 4321;
    Calendar calendar(kCapacity, kQuantumBytes, kPropagationPs);
    calendar.beginEpoch(0,
                        {{1, 10, 20, kCapacity / 2},
                         {2, 11, 21, kCapacity / 2}});

    std::map<uint32_t, uint64_t> source_available;
    std::map<uint32_t, uint64_t> destination_available;
    for (int slot = 0; slot < 40; ++slot) {
        for (const Reservation& reservation : calendar.reserveNextSlot()) {
            EXPECT_GE(reservation.sourceSlotStartPs(),
                      source_available[reservation.sourceNode()]);
            EXPECT_GE(reservation.destinationSlotStartPs(),
                      destination_available[reservation.destinationNode()]);
            source_available[reservation.sourceNode()] = reservation.sourceSlotEndPs();
            destination_available[reservation.destinationNode()] =
                reservation.destinationSlotEndPs();
            EXPECT_EQ(reservation.manifoldEntryPs(), reservation.sourceSlotEndPs());
            EXPECT_EQ(reservation.manifoldExitPs() - reservation.manifoldEntryPs(),
                      kPropagationPs);
            EXPECT_EQ(reservation.destinationSlotStartPs(),
                      reservation.manifoldExitPs());
        }
    }
}

TEST(RnicPacketizedSlotCalendarTest, RateChangePreservesIncumbentCreditDebt) {
    Calendar calendar(kCapacity, kQuantumBytes, 0);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity / 4}});
    ASSERT_EQ(flowIds(calendar.reserveNextSlot()), (std::vector<uint64_t>{1}));

    calendar.beginEpoch(calendar.nextSlotIndex(),
                        {{1, 10, 20, kCapacity / 2}});
    EXPECT_TRUE(calendar.reserveNextSlot().empty());
    EXPECT_EQ(flowIds(calendar.reserveNextSlot()), (std::vector<uint64_t>{1}));

    calendar.beginEpoch(calendar.nextSlotIndex(), {});
    EXPECT_TRUE(calendar.reserveNextSlot().empty());
}

TEST(RnicPacketizedSlotCalendarTest, TimeShiftChangesOnlyAbsoluteTimes) {
    constexpr uint64_t kShiftPs = 9999;
    Calendar baseline(kCapacity, kQuantumBytes, 123, 0);
    Calendar shifted(kCapacity, kQuantumBytes, 123, kShiftPs);
    const std::vector<Grant> grants{{1, 10, 20, kCapacity}};
    baseline.beginEpoch(0, grants);
    shifted.beginEpoch(0, grants);

    const Reservation first = baseline.reserveNextSlot().at(0);
    const Reservation second = shifted.reserveNextSlot().at(0);
    EXPECT_EQ(second.sourceSlotStartPs() - first.sourceSlotStartPs(), kShiftPs);
    EXPECT_EQ(second.sourceSlotEndPs() - first.sourceSlotEndPs(), kShiftPs);
    EXPECT_EQ(second.manifoldExitPs() - first.manifoldExitPs(), kShiftPs);
    EXPECT_EQ(second.destinationSlotEndPs() - first.destinationSlotEndPs(),
              kShiftPs);
}

TEST(RnicPacketizedSlotCalendarTest, EpochAffectsOnlyUnreservedSlots) {
    Calendar calendar(kCapacity, kQuantumBytes, 0);
    calendar.beginEpoch(0, {{10, 1, 3, kCapacity}});
    const Reservation committed = calendar.reserveNextSlot().at(0);
    const uint64_t old_epoch = committed.allocationEpoch();

    calendar.beginEpoch(calendar.nextSlotIndex(),
                        {{10, 1, 3, kCapacity / 2},
                         {5, 2, 3, kCapacity / 2}});
    EXPECT_GT(calendar.allocationEpoch(), old_epoch);
    const Reservation next = calendar.reserveNextSlot().at(0);

    EXPECT_EQ(committed.flowId(), 10u);
    EXPECT_EQ(committed.allocationEpoch(), old_epoch);
    EXPECT_EQ(committed.sourceSlotStartPs(), 0u);
    EXPECT_EQ(next.flowId(), 5u);
    EXPECT_EQ(next.allocationEpoch(), calendar.allocationEpoch());
    EXPECT_EQ(next.sourceSlotStartPs(), committed.sourceSlotEndPs());
}

TEST(RnicPacketizedSlotCalendarTest, LowGrantCreatesSlotsNotPositiveGapJitter) {
    Calendar calendar(kCapacity, kQuantumBytes, 0);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity / 4}});

    std::vector<uint64_t> occupied_slots;
    for (uint64_t slot = 0; slot < 17; ++slot) {
        if (!calendar.reserveNextSlot().empty()) {
            occupied_slots.push_back(slot);
        }
    }
    EXPECT_EQ(occupied_slots, (std::vector<uint64_t>{0, 4, 8, 12, 16}));
}

TEST(RnicPacketizedSlotCalendarTest, InputPermutationHasIdenticalReplay) {
    Calendar first(kCapacity, kQuantumBytes, 17);
    Calendar second(kCapacity, kQuantumBytes, 17);
    const std::vector<Grant> ascending{{1, 10, 20, kCapacity / 2},
                                       {2, 10, 21, kCapacity / 2},
                                       {3, 11, 20, kCapacity / 2}};
    const std::vector<Grant> descending{{3, 11, 20, kCapacity / 2},
                                        {2, 10, 21, kCapacity / 2},
                                        {1, 10, 20, kCapacity / 2}};
    first.beginEpoch(0, ascending);
    second.beginEpoch(0, descending);

    for (int slot = 0; slot < 100; ++slot) {
        const auto lhs = first.reserveNextSlot();
        const auto rhs = second.reserveNextSlot();
        EXPECT_EQ(flowIds(lhs), flowIds(rhs));
        ASSERT_EQ(lhs.size(), rhs.size());
        for (size_t i = 0; i < lhs.size(); ++i) {
            EXPECT_EQ(lhs[i].sourceSlotStartPs(), rhs[i].sourceSlotStartPs());
            EXPECT_EQ(lhs[i].destinationSlotEndPs(), rhs[i].destinationSlotEndPs());
        }
    }
}

TEST(RnicPacketizedSlotCalendarTest, ReservesOnlyFullWireQuanta) {
    Calendar calendar(kCapacity, kQuantumBytes, 50);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity}});
    const Reservation reservation = calendar.reserveNextSlot().at(0);

    EXPECT_EQ(reservation.reservedWireBytes(), kQuantumBytes);
    EXPECT_EQ(reservation.sourceSlotEndPs() - reservation.sourceSlotStartPs(),
              kExactSlotPs);
    EXPECT_GT(reservation.destinationSlotEndPs(), reservation.manifoldExitPs());
}

TEST(RnicPacketizedSlotCalendarTest, PacketizedServiceNeverBeatsFluidCapacityFloor) {
    Calendar calendar(kCapacity, kQuantumBytes, 1000);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity}});

    Reservation last = calendar.reserveNextSlot().at(0);
    for (int packet = 1; packet < 20; ++packet) {
        last = calendar.reserveNextSlot().at(0);
    }

    const uint64_t fluid_service_floor_ps = 20 * kExactSlotPs;
    EXPECT_GE(last.sourceSlotEndPs(), fluid_service_floor_ps);
    EXPECT_GE(last.destinationSlotEndPs(), fluid_service_floor_ps + 1000);
}

TEST(RnicPacketizedSlotCalendarTest, RejectsInvalidConfigurationAndSnapshots) {
    EXPECT_THROW(Calendar(0, 1500, 0), std::invalid_argument);
    EXPECT_THROW(Calendar(kCapacity, 0, 0), std::invalid_argument);
    EXPECT_THROW(Calendar(std::numeric_limits<uint64_t>::max(), 1, 0),
                 std::invalid_argument);
    EXPECT_THROW(
        Calendar(1, std::numeric_limits<uint64_t>::max(), 0),
        std::overflow_error);

    Calendar calendar(kCapacity, kQuantumBytes, 0);
    EXPECT_THROW(calendar.beginEpoch(1, {}), std::invalid_argument);
    EXPECT_THROW(calendar.beginEpoch(0, {{1, 1, 2, 10}, {1, 3, 4, 10}}),
                 std::invalid_argument);
    EXPECT_THROW(calendar.beginEpoch(0,
                                     {{1, 1, 2, kCapacity},
                                      {2, 1, 3, 1}}),
                 std::invalid_argument);

    calendar.beginEpoch(0, {{1, 1, 2, kCapacity}});
    calendar.reserveNextSlot();
    EXPECT_THROW(calendar.beginEpoch(calendar.nextSlotIndex(),
                                     {{1, 9, 2, kCapacity}}),
                 std::invalid_argument);
}

}  // namespace
