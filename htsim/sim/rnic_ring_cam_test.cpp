// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ring_cam.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Admission = RnicRingCamAdmission;
using Packet = RnicRingCamPacket;

RnicRingCam makeCam(uint64_t capacity = 10000,
                    uint64_t window_ps = 100,
                    uint64_t tick_ps = 10) {
    return RnicRingCam({window_ps, tick_ps, capacity});
}

std::vector<uint64_t> packetIds(const std::vector<RnicRingCamRelease>& releases) {
    std::vector<uint64_t> ids;
    ids.reserve(releases.size());
    for (const RnicRingCamRelease& release : releases) {
        ids.push_back(release.packet.packet_id);
    }
    return ids;
}

TEST(RnicRingCamTest, ClassifiesEveryAdmissionBoundary) {
    {
        RnicRingCam cam = makeCam();
        const auto result = cam.processArrival({1, 10, 100, 90, {100, 100}});
        EXPECT_EQ(result.admission, Admission::Early);
        EXPECT_EQ(cam.wireOccupancyBytes(), 0u);
    }
    {
        RnicRingCam cam = makeCam();
        const auto result = cam.processArrival({1, 10, 100, 100, {100, 100}});
        EXPECT_EQ(result.admission, Admission::Admitted);
        EXPECT_EQ(result.logical_release_ps, 200u);
    }
    {
        RnicRingCam cam = makeCam();
        const auto result = cam.processArrival({1, 10, 100, 190, {100, 100}});
        EXPECT_EQ(result.admission, Admission::Admitted);
        EXPECT_EQ(result.logical_release_ps, 200u);
    }
    {
        RnicRingCam cam = makeCam();
        const auto result = cam.processArrival({1, 10, 100, 200, {100, 100}});
        ASSERT_EQ(result.admission, Admission::Admitted);
        ASSERT_EQ(result.logical_release_ps, 200u);
        EXPECT_EQ(cam.wireOccupancyBytes(), 100u);

        const auto releases = cam.advanceTo(200);
        ASSERT_EQ(releases.size(), 1u);
        EXPECT_EQ(releases.front().packet.packet_id, 1u);
        EXPECT_EQ(releases.front().logical_release_ps, 200u);
        EXPECT_EQ(cam.wireOccupancyBytes(), 0u);
    }
    {
        RnicRingCam cam = makeCam();
        const auto result = cam.processArrival({1, 10, 100, 201, {100, 100}});
        EXPECT_EQ(result.admission, Admission::Late);
        EXPECT_FALSE(result.logical_release_ps.has_value());
        EXPECT_EQ(cam.wireOccupancyBytes(), 0u);
    }
}

TEST(RnicRingCamTest, ReleasesBeforeClassifyingAnArrivalAtTheSameTime) {
    RnicRingCam cam = makeCam(100);
    ASSERT_EQ(cam.processArrival({1, 10, 0, 0, {100, 100}}).admission,
              Admission::Admitted);

    const auto result = cam.processArrival({2, 20, 100, 100, {100, 100}});

    ASSERT_EQ(result.released_before_admission.size(), 1u);
    EXPECT_EQ(result.released_before_admission.front().packet.packet_id, 1u);
    EXPECT_EQ(result.admission, Admission::Admitted);
    EXPECT_EQ(cam.wireOccupancyBytes(), 100u);
    EXPECT_EQ(cam.packetCount(), 1u);
}

TEST(RnicRingCamTest, QuantizesTheStampPlusWindowReleaseEdgeUpward) {
    RnicRingCam cam = makeCam(10000, 100, 16);

    const auto result = cam.processArrival({1, 10, 1, 1, {100, 100}});

    ASSERT_EQ(result.admission, Admission::Admitted);
    ASSERT_TRUE(result.logical_release_ps.has_value());
    EXPECT_EQ(*result.logical_release_ps, 112u);
    EXPECT_TRUE(cam.advanceTo(111).empty());
    const auto releases = cam.advanceTo(112);
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases.front().logical_release_ps, 112u);
    EXPECT_GE(releases.front().logical_release_ps, 101u);
    EXPECT_LT(releases.front().logical_release_ps - 101u, 16u);
}

TEST(RnicRingCamTest, OrdersSameTickPacketsByEtaThenStableAdmissionOrder) {
    RnicRingCam cam = makeCam(10000, 100, 16);
    ASSERT_EQ(cam.processArrival({1, 10, 1, 1, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(cam.processArrival({3, 30, 7, 7, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(cam.processArrival({2, 20, 7, 7, {100, 100}}).admission,
              Admission::Admitted);

    const auto releases = cam.advanceTo(112);

    EXPECT_EQ(packetIds(releases), (std::vector<uint64_t>{1, 3, 2}));
    for (const RnicRingCamRelease& release : releases) {
        EXPECT_EQ(release.logical_release_ps, 112u);
    }
}

TEST(RnicRingCamTest, MissingPacketIdDoesNotBlockLaterPackets) {
    RnicRingCam cam = makeCam();
    ASSERT_EQ(cam.processArrival({1, 10, 0, 0, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(cam.processArrival({3, 10, 10, 10, {100, 100}}).admission,
              Admission::Admitted);

    const auto releases = cam.advanceTo(110);

    EXPECT_EQ(packetIds(releases), (std::vector<uint64_t>{1, 3}));
}

TEST(RnicRingCamTest, AccountsSharedWireCapacityAndHighWatermarkAcrossFlows) {
    RnicRingCam cam = makeCam(1500);
    ASSERT_EQ(cam.processArrival({1, 10, 0, 0, {1000, 1000}}).admission,
              Admission::Admitted);
    EXPECT_EQ(cam.wireOccupancyBytes(), 1000u);

    const auto overflow = cam.processArrival({2, 20, 10, 10, {600, 600}});
    EXPECT_EQ(overflow.admission, Admission::Overflow);
    EXPECT_FALSE(overflow.logical_release_ps.has_value());
    EXPECT_EQ(cam.wireOccupancyBytes(), 1000u);

    ASSERT_EQ(cam.processArrival({3, 30, 20, 20, {500, 500}}).admission,
              Admission::Admitted);
    EXPECT_EQ(cam.wireOccupancyBytes(), 1500u);
    EXPECT_EQ(cam.wireHighWatermarkBytes(), 1500u);
    EXPECT_EQ(cam.packetCount(), 2u);

    EXPECT_EQ(cam.advanceTo(120).size(), 2u);
    EXPECT_EQ(cam.wireOccupancyBytes(), 0u);
    EXPECT_EQ(cam.wireHighWatermarkBytes(), 1500u);
}

TEST(RnicRingCamTest, PreservesPayloadWhileChargingOnlyWireOccupancy) {
    RnicRingCam cam = makeCam(128);

    const auto admission = cam.processArrival({1, 10, 0, 0, {100, 128}});
    ASSERT_EQ(admission.admission, Admission::Admitted);
    EXPECT_EQ(cam.wireOccupancyBytes(), 128u);
    EXPECT_EQ(cam.wireHighWatermarkBytes(), 128u);

    const auto releases = cam.advanceTo(100);
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases.front().packet.extent.payloadBytes(), 100u);
    EXPECT_EQ(releases.front().packet.extent.wireBytes(), 128u);
    EXPECT_EQ(cam.wireOccupancyBytes(), 0u);
}

TEST(RnicRingCamTest, ArrivalPermutationDoesNotChangeTimestampReleaseOrder) {
    RnicRingCam unjittered = makeCam();
    ASSERT_EQ(unjittered.processArrival({1, 10, 0, 0, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(unjittered.processArrival({2, 20, 10, 10, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(unjittered.processArrival({3, 30, 20, 20, {100, 100}}).admission,
              Admission::Admitted);

    RnicRingCam permuted = makeCam();
    ASSERT_EQ(permuted.processArrival({2, 20, 10, 10, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(permuted.processArrival({3, 30, 20, 20, {100, 100}}).admission,
              Admission::Admitted);
    ASSERT_EQ(permuted.processArrival({1, 10, 0, 90, {100, 100}}).admission,
              Admission::Admitted);

    EXPECT_EQ(packetIds(unjittered.advanceTo(120)),
              (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_EQ(packetIds(permuted.advanceTo(120)),
              (std::vector<uint64_t>{1, 2, 3}));
}

TEST(RnicRingCamTest, RejectsInvalidConfigurationAndNonMonotonicUse) {
    EXPECT_THROW(RnicRingCam({0, 1, 100}), std::invalid_argument);
    EXPECT_THROW(RnicRingCam({1, 0, 100}), std::invalid_argument);

    RnicRingCam cam = makeCam();
    EXPECT_THROW(RnicPacketExtent(0, 0), std::invalid_argument);
    EXPECT_THROW(RnicPacketExtent(2, 1), std::invalid_argument);
    cam.advanceTo(50);
    EXPECT_THROW(cam.advanceTo(49), std::invalid_argument);
    EXPECT_THROW(cam.processArrival({2, 1, 0, 49, {1, 1}}), std::invalid_argument);
}

TEST(RnicRingCamModuloTimestampTest, ExhaustivelyClassifiesAcrossSmallWrap) {
    constexpr unsigned bits = 4;
    constexpr uint64_t modulus = uint64_t{1} << bits;
    constexpr uint64_t half_range = modulus / 2;
    constexpr uint64_t window = 4;

    for (uint64_t now = 0; now < modulus; ++now) {
        for (uint64_t eta = 0; eta < modulus; ++eta) {
            const uint64_t age = now >= eta ? now - eta : modulus - (eta - now);
            RnicModuloTimestampRelation expected;
            if (age <= window) {
                expected = RnicModuloTimestampRelation::Admitted;
            } else if (age < half_range) {
                expected = RnicModuloTimestampRelation::Late;
            } else if (age > half_range) {
                expected = RnicModuloTimestampRelation::Early;
            } else {
                expected = RnicModuloTimestampRelation::Ambiguous;
            }

            const RnicModuloTimestampAge actual =
                classifyRnicModuloTimestampAge(now, eta, bits, window);
            EXPECT_EQ(actual.relation, expected) << "now=" << now << " eta=" << eta;
            EXPECT_EQ(actual.age_ticks, age) << "now=" << now << " eta=" << eta;
        }
    }

    EXPECT_EQ(classifyRnicModuloTimestampAge(0, 15, bits, window).relation,
              RnicModuloTimestampRelation::Admitted);
    EXPECT_EQ(classifyRnicModuloTimestampAge(4, 0, bits, window).relation,
              RnicModuloTimestampRelation::Admitted);
    EXPECT_EQ(classifyRnicModuloTimestampAge(5, 0, bits, window).relation,
              RnicModuloTimestampRelation::Late);
    EXPECT_EQ(classifyRnicModuloTimestampAge(15, 0, bits, window).relation,
              RnicModuloTimestampRelation::Early);
}

TEST(RnicRingCamModuloTimestampTest, RejectsAmbiguousConfiguration) {
    EXPECT_THROW(classifyRnicModuloTimestampAge(0, 0, 1, 1), std::invalid_argument);
    EXPECT_THROW(classifyRnicModuloTimestampAge(0, 0, 4, 0), std::invalid_argument);
    EXPECT_THROW(classifyRnicModuloTimestampAge(0, 0, 4, 8), std::invalid_argument);
    EXPECT_THROW(classifyRnicModuloTimestampAge(16, 0, 4, 4), std::invalid_argument);
}

}  // namespace
