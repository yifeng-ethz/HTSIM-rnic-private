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
using Transmission = RnicPacketizedTransmission;

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

TEST(RnicPacketizedSlotCalendarTest, CoversEverySaturatedEndpointInEverySlot) {
    constexpr uint64_t capacity = 120;
    Calendar calendar(capacity, 1, 0);
    calendar.beginEpoch(0,
                        {{1, 0, 2, capacity / 2},
                         {2, 0, 3, capacity / 2},
                         {3, 1, 0, capacity / 2},
                         {4, 1, 1, capacity / 2},
                         {5, 2, 1, capacity / 2},
                         {6, 2, 3, capacity / 2}});

    std::map<uint64_t, uint64_t> packet_count;
    for (uint64_t slot = 0; slot < 1000; ++slot) {
        const auto reservations = calendar.reserveNextSlot();
        ASSERT_EQ(reservations.size(), 3u) << "slot " << slot;

        std::set<uint32_t> sources;
        std::set<uint32_t> destinations;
        for (const Reservation& reservation : reservations) {
            sources.insert(reservation.sourceNode());
            destinations.insert(reservation.destinationNode());
            ++packet_count[reservation.flowId()];
        }
        EXPECT_EQ(sources, (std::set<uint32_t>{0, 1, 2})) << "slot " << slot;
        EXPECT_NE(destinations.count(1), 0u) << "slot " << slot;
        EXPECT_NE(destinations.count(3), 0u) << "slot " << slot;
    }

    for (uint64_t flow_id = 1; flow_id <= 6; ++flow_id) {
        EXPECT_EQ(packet_count[flow_id], 500u) << "flow " << flow_id;
    }
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

TEST(RnicPacketizedSlotCalendarTest, RebaseIdleStartsNewRationalBusyPeriod) {
    constexpr uint64_t kSevenTerabits = UINT64_C(7000000000000);
    Calendar calendar(kSevenTerabits, 3, 5);
    calendar.beginEpoch(0, {{1, 10, 20, kSevenTerabits}});
    calendar.reserveNextSlot();

    const uint64_t preserved_slot = calendar.nextSlotIndex();
    ASSERT_EQ(preserved_slot, 1u);
    ASSERT_EQ(calendar.nextSlotStartPs(), 4u);
    calendar.beginEpoch(preserved_slot, {});
    const uint64_t preserved_epoch = calendar.allocationEpoch();

    calendar.rebaseIdle(100);
    EXPECT_EQ(calendar.nextSlotIndex(), preserved_slot);
    EXPECT_EQ(calendar.allocationEpoch(), preserved_epoch);
    EXPECT_EQ(calendar.nextSlotStartPs(), 100u);

    calendar.beginEpoch(preserved_slot, {{1, 10, 20, kSevenTerabits}});
    const Reservation reservation = calendar.reserveNextSlot().at(0);
    EXPECT_EQ(reservation.slotIndex(), preserved_slot);
    EXPECT_EQ(reservation.sourceSlotStartPs(), 100u);
    EXPECT_EQ(reservation.sourceSlotEndPs(), 104u);

    const Transmission full =
        reservation.materializePacket(RnicPacketExtent(3, 3));
    EXPECT_EQ(full.sourceSerializationStartPs(), 100u);
    EXPECT_EQ(full.sourceSerializationEndPs(), 104u);
}

TEST(RnicPacketizedSlotCalendarTest, RebaseIdleValidatesStateTransactionally) {
    constexpr uint64_t kSevenTerabits = UINT64_C(7000000000000);
    Calendar calendar(kSevenTerabits, 3, 5);
    calendar.beginEpoch(0, {{1, 10, 20, kSevenTerabits}});

    EXPECT_THROW(calendar.rebaseIdle(100), std::logic_error);
    EXPECT_EQ(calendar.nextSlotIndex(), 0u);
    EXPECT_EQ(calendar.nextSlotStartPs(), 0u);

    calendar.reserveNextSlot();
    calendar.beginEpoch(calendar.nextSlotIndex(), {});
    const uint64_t preserved_slot = calendar.nextSlotIndex();
    const uint64_t preserved_epoch = calendar.allocationEpoch();
    const uint64_t preserved_start = calendar.nextSlotStartPs();
    ASSERT_GT(preserved_start, 0u);

    EXPECT_THROW(calendar.rebaseIdle(preserved_start - 1), std::invalid_argument);
    EXPECT_EQ(calendar.nextSlotIndex(), preserved_slot);
    EXPECT_EQ(calendar.allocationEpoch(), preserved_epoch);
    EXPECT_EQ(calendar.nextSlotStartPs(), preserved_start);

    calendar.rebaseIdle(100);
    EXPECT_THROW(calendar.beginEpoch(
                     preserved_slot, {{1, 10, 21, kSevenTerabits}}),
                 std::invalid_argument);
    EXPECT_EQ(calendar.nextSlotIndex(), preserved_slot);
    EXPECT_EQ(calendar.allocationEpoch(), preserved_epoch);
    EXPECT_EQ(calendar.nextSlotStartPs(), 100u);
}

TEST(RnicPacketizedSlotCalendarTest, RebaseIdlePreservesDormantZeroGrantFlow) {
    Calendar calendar(1, 1, 0);
    calendar.beginEpoch(0, {{1, 10, 20, 0}});
    const uint64_t preserved_epoch = calendar.allocationEpoch();

    calendar.rebaseIdle(100);
    EXPECT_EQ(calendar.nextSlotIndex(), 0U);
    EXPECT_EQ(calendar.allocationEpoch(), preserved_epoch);
    EXPECT_EQ(calendar.nextSlotStartPs(), 100U);

    calendar.beginEpoch(0, {{1, 10, 20, 1}});
    const Reservation reservation = calendar.reserveNextSlot().at(0);
    EXPECT_EQ(reservation.flowId(), 1U);
    EXPECT_EQ(reservation.sourceSlotStartPs(), 100U);
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

TEST(RnicPacketizedSlotCalendarTest, CopiedIdentityRegistryIsIsolatedOnWrite) {
    Calendar original(kCapacity, kQuantumBytes, 0);
    original.beginEpoch(0, {{1, 10, 20, kCapacity}});
    Calendar fork = original;

    EXPECT_NO_THROW(fork.beginEpoch(
        0,
        {{1, 10, 20, kCapacity}, {2, 30, 40, kCapacity}}));
    EXPECT_NO_THROW(original.beginEpoch(
        0,
        {{1, 10, 20, kCapacity}, {2, 31, 41, kCapacity}}));

    EXPECT_THROW(
        fork.beginEpoch(
            0,
            {{1, 10, 20, kCapacity}, {2, 31, 41, kCapacity}}),
        std::invalid_argument);
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

TEST(RnicPacketizedReservationTest, MaterializesFromExactNonintegralBoundaries) {
    constexpr uint64_t kSevenTerabits = UINT64_C(7000000000000);
    Calendar calendar(kSevenTerabits, 3, 5);
    calendar.beginEpoch(0, {{1, 10, 20, kSevenTerabits}});

    const Reservation reservation = calendar.reserveNextSlot().at(0);
    const Transmission packet =
        reservation.materializePacket(RnicPacketExtent(1, 1));

    // Exact boundaries, in ps, are source [16/7, 24/7], manifold exit
    // 59/7, and destination end 67/7. Each exported event timestamp is the
    // ceiling of that cumulative boundary.
    EXPECT_EQ(packet.sourceSerializationStartPs(), 3u);
    EXPECT_EQ(packet.sourceSerializationEndPs(), 4u);
    EXPECT_EQ(packet.manifoldEntryPs(), 4u);
    EXPECT_EQ(packet.manifoldExitPs(), 9u);
    EXPECT_EQ(packet.destinationSerializationStartPs(), 9u);
    EXPECT_EQ(packet.destinationSerializationEndPs(), 10u);

    // Starting a fresh integer serializer at ceil(59/7) would instead end at
    // 11 ps. The destination must retain the cumulative rational boundary.
    EXPECT_NE(packet.destinationSerializationEndPs(),
              packet.destinationSerializationStartPs() + 2);
}

TEST(RnicPacketizedReservationTest, RightAlignsShortTailInSourceEnvelope) {
    Calendar calendar(kCapacity, kQuantumBytes, 11);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity}});

    const Reservation reservation = calendar.reserveNextSlot().at(0);
    const Transmission tail =
        reservation.materializePacket(RnicPacketExtent(80, 100));

    // The 100-byte duration is 66,666 2/3 ps. Subtracting its rounded-up
    // integer duration from the 1,000,000 ps envelope end would incorrectly
    // produce 933,333 ps; exact right alignment rounds to 933,334 ps.
    EXPECT_EQ(tail.sourceSerializationStartPs(), 933334u);
    EXPECT_EQ(tail.sourceSerializationEndPs(), kExactSlotPs);
    EXPECT_GT(tail.sourceSerializationStartPs(),
              reservation.sourceSlotStartPs());
    EXPECT_EQ(tail.sourceSerializationEndPs(), reservation.sourceSlotEndPs());
    EXPECT_EQ(tail.manifoldEntryPs(), tail.sourceSerializationEndPs());
}

TEST(RnicPacketizedReservationTest, PreservesFixedResidenceForShortPacket) {
    constexpr uint64_t kSevenTerabits = UINT64_C(7000000000000);
    constexpr uint64_t kPropagationPs = 123;
    Calendar calendar(kSevenTerabits, 3, kPropagationPs);
    calendar.beginEpoch(0, {{1, 10, 20, kSevenTerabits}});

    const Transmission packet = calendar.reserveNextSlot()
                                    .at(0)
                                    .materializePacket(RnicPacketExtent(0, 1));

    EXPECT_EQ(packet.manifoldExitPs() - packet.manifoldEntryPs(),
              kPropagationPs);
    EXPECT_EQ(packet.destinationSerializationStartPs(), packet.manifoldExitPs());
}

TEST(RnicPacketizedReservationTest, DestinationUsesExactSameWireExtent) {
    Calendar calendar(kCapacity, kQuantumBytes, 11);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity}});

    const Transmission packet = calendar.reserveNextSlot()
                                    .at(0)
                                    .materializePacket(RnicPacketExtent(80, 100));

    EXPECT_EQ(packet.extent().payloadBytes(), 80u);
    EXPECT_EQ(packet.extent().wireBytes(), 100u);
    EXPECT_EQ(packet.destinationSerializationStartPs(), 1000011u);
    EXPECT_EQ(packet.destinationSerializationEndPs(), 1066678u);
}

TEST(RnicPacketizedReservationTest, AdjacentSlotsNeverOverlapAfterMaterialization) {
    constexpr uint64_t kSevenTerabits = UINT64_C(7000000000000);
    Calendar calendar(kSevenTerabits, 3, 5);
    calendar.beginEpoch(0, {{1, 10, 20, kSevenTerabits}});

    uint64_t previous_source_end = 0;
    uint64_t previous_destination_end = 0;
    for (uint64_t wire_bytes : {1u, 2u, 3u, 1u, 3u, 2u}) {
        const Reservation reservation = calendar.reserveNextSlot().at(0);
        const Transmission packet = reservation.materializePacket(
            RnicPacketExtent(wire_bytes, wire_bytes));

        EXPECT_GE(packet.sourceSerializationStartPs(), previous_source_end);
        EXPECT_GE(packet.destinationSerializationStartPs(),
                  previous_destination_end);
        EXPECT_GE(packet.sourceSerializationStartPs(),
                  reservation.sourceSlotStartPs());
        EXPECT_LE(packet.destinationSerializationEndPs(),
                  reservation.destinationSlotEndPs());
        previous_source_end = packet.sourceSerializationEndPs();
        previous_destination_end = packet.destinationSerializationEndPs();
    }
}

TEST(RnicPacketizedReservationTest, FullExtentMatchesReservationEnvelope) {
    constexpr uint64_t kSevenTerabits = UINT64_C(7000000000000);
    Calendar calendar(kSevenTerabits, 3, 17, 101);
    calendar.beginEpoch(0, {{7, 10, 20, kSevenTerabits}});

    calendar.reserveNextSlot();
    calendar.reserveNextSlot();
    const Reservation reservation = calendar.reserveNextSlot().at(0);
    const Transmission packet =
        reservation.materializePacket(RnicPacketExtent(2, 3));

    EXPECT_EQ(packet.slotIndex(), reservation.slotIndex());
    EXPECT_EQ(packet.allocationEpoch(), reservation.allocationEpoch());
    EXPECT_EQ(packet.flowId(), reservation.flowId());
    EXPECT_EQ(packet.sourceNode(), reservation.sourceNode());
    EXPECT_EQ(packet.destinationNode(), reservation.destinationNode());
    EXPECT_EQ(packet.sourceSerializationStartPs(),
              reservation.sourceSlotStartPs());
    EXPECT_EQ(packet.sourceSerializationEndPs(), reservation.sourceSlotEndPs());
    EXPECT_EQ(packet.manifoldEntryPs(), reservation.manifoldEntryPs());
    EXPECT_EQ(packet.manifoldExitPs(), reservation.manifoldExitPs());
    EXPECT_EQ(packet.destinationSerializationStartPs(),
              reservation.destinationSlotStartPs());
    EXPECT_EQ(packet.destinationSerializationEndPs(),
              reservation.destinationSlotEndPs());
}

TEST(RnicPacketizedReservationTest, RejectsExtentLargerThanEnvelopeTransactionally) {
    Calendar calendar(kCapacity, 100, 7);
    calendar.beginEpoch(0, {{1, 10, 20, kCapacity}});
    const Reservation reservation = calendar.reserveNextSlot().at(0);

    EXPECT_THROW(reservation.materializePacket(RnicPacketExtent(101, 101)),
                 std::invalid_argument);

    const Transmission valid =
        reservation.materializePacket(RnicPacketExtent(100, 100));
    EXPECT_EQ(valid.sourceSerializationStartPs(), reservation.sourceSlotStartPs());
    EXPECT_EQ(valid.destinationSerializationEndPs(),
              reservation.destinationSlotEndPs());
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

TEST(RnicPacketizedSlotCalendarTest, AcceptsWideWireQuantumWhenTimeFits) {
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    Calendar calendar(maximum, maximum, 0);
    calendar.beginEpoch(0, {{1, 10, 20, maximum}});

    const Reservation reservation = calendar.reserveNextSlot().at(0);
    const Transmission transmission = reservation.materializePacket(
        RnicPacketExtent(maximum, maximum));

    EXPECT_EQ(reservation.reservedWireBytes(), maximum);
    EXPECT_EQ(transmission.extent().wireBytes(), maximum);
    EXPECT_GT(transmission.sourceSerializationEndPs(), 0U);
    EXPECT_GT(transmission.destinationSerializationEndPs(),
              transmission.sourceSerializationEndPs());
}

}  // namespace
