// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_prbs_pacer.h"
#include "rnic_ring_cam.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct AggregateTrace {
    std::vector<uint64_t> packets_by_slot;
    uint64_t packet_count = 0;
};

AggregateTrace makeNodeDistinctPrbsTrace(uint64_t global_seed,
                                         uint64_t sender_count,
                                         uint64_t slot_count) {
    std::vector<RnicPrbsPacer> pacers;
    pacers.reserve(sender_count);
    for (uint64_t sender = 0; sender < sender_count; ++sender) {
        pacers.emplace_back(global_seed, sender);
    }

    AggregateTrace trace{std::vector<uint64_t>(slot_count, 0), 0};
    for (uint64_t slot = 0; slot < slot_count; ++slot) {
        for (uint64_t sender = 0; sender < sender_count; ++sender) {
            const bool selected = pacers[sender]
                                      .selectEqualWireQuantum(
                                          {{sender + 1, 1}}, sender_count)
                                      .has_value();
            trace.packets_by_slot[slot] += selected ? 1 : 0;
            trace.packet_count += selected ? 1 : 0;
        }
    }
    return trace;
}

AggregateTrace makeGrantedPrbsTrace(uint64_t global_seed,
                                    uint64_t sender_count,
                                    uint64_t sender_wire_rate,
                                    uint64_t wire_capacity,
                                    uint64_t slot_count) {
    std::vector<RnicPrbsPacer> pacers;
    pacers.reserve(sender_count);
    for (uint64_t sender = 0; sender < sender_count; ++sender) {
        pacers.emplace_back(global_seed, sender);
    }

    AggregateTrace trace{std::vector<uint64_t>(slot_count, 0), 0};
    for (uint64_t slot = 0; slot < slot_count; ++slot) {
        for (uint64_t sender = 0; sender < sender_count; ++sender) {
            const bool selected = pacers[sender]
                                      .selectEqualWireQuantum(
                                          {{sender + 1, sender_wire_rate}},
                                          wire_capacity)
                                      .has_value();
            trace.packets_by_slot[slot] += selected ? 1 : 0;
            trace.packet_count += selected ? 1 : 0;
        }
    }
    return trace;
}

double mean(const std::vector<uint64_t>& values) {
    const uint64_t sum = std::accumulate(values.begin(), values.end(), uint64_t{0});
    return static_cast<double>(sum) / static_cast<double>(values.size());
}

double variance(const std::vector<uint64_t>& values) {
    const double average = mean(values);
    double squared_error = 0.0;
    for (const uint64_t value : values) {
        const double error = static_cast<double>(value) - average;
        squared_error += error * error;
    }
    return squared_error / static_cast<double>(values.size());
}

uint64_t peak(const std::vector<uint64_t>& values) {
    return *std::max_element(values.begin(), values.end());
}

double lagOneCorrelation(const std::vector<uint64_t>& values) {
    const double average = mean(values);
    const double value_variance = variance(values);
    double covariance = 0.0;
    for (size_t index = 1; index < values.size(); ++index) {
        covariance +=
            (static_cast<double>(values[index]) - average)
            * (static_cast<double>(values[index - 1]) - average);
    }
    covariance /= static_cast<double>(values.size() - 1);
    return covariance / value_variance;
}

uint64_t maximumUnitServiceBacklog(const std::vector<uint64_t>& arrivals) {
    uint64_t backlog = 0;
    uint64_t maximum_backlog = 0;
    for (const uint64_t arrival_count : arrivals) {
        backlog += arrival_count;
        if (backlog != 0) {
            --backlog;
        }
        maximum_backlog = std::max(maximum_backlog, backlog);
    }
    return maximum_backlog;
}

TEST(RnicBurstTrendTest, Block64WordsHaveRenewalLikeShortRangeStatistics) {
    constexpr uint64_t kSenderCount = 8;
    constexpr uint64_t kSenderWireRate = 45;
    constexpr uint64_t kWireCapacity = 400;
    constexpr uint64_t kSlotCount = 100000;
    const AggregateTrace trace =
        makeGrantedPrbsTrace(20260713,
                             kSenderCount,
                             kSenderWireRate,
                             kWireCapacity,
                             kSlotCount);

    // This is a deterministic replay-quality guard, not a queueing bound.
    // The retired v1 generator exposed register states separated by only one
    // bit transition, so successive lottery draws were strongly linearly
    // related. At this exact 8:1, 0.9C operating point it produces a large
    // positive lag-one correlation and a materially larger ideal-server
    // backlog than this block64 replay.
    EXPECT_NEAR(mean(trace.packets_by_slot), 0.9, 0.01);
    EXPECT_LT(std::abs(lagOneCorrelation(trace.packets_by_slot)), 0.02);
    EXPECT_LT(maximumUnitServiceBacklog(trace.packets_by_slot), 45u);
}

TEST(RnicBurstTrendTest, NodeDistinctPrbsSuppressesPhaseAlignedIncast) {
    constexpr uint64_t kSenderCount = 64;
    constexpr uint64_t kSlotCount = 65536;
    const AggregateTrace prbs =
        makeNodeDistinctPrbsTrace(20260713, kSenderCount, kSlotCount);

    std::vector<uint64_t> phase_aligned(kSlotCount, 0);
    for (uint64_t slot = 0; slot < kSlotCount; slot += kSenderCount) {
        phase_aligned[slot] = kSenderCount;
    }

    // Both schemes offer one aggregate packet per slot on average.
    // Node-distinct pseudorandom phases remove the N-packet periodic impulse
    // produced when every sender uses the same deterministic phase.
    EXPECT_NEAR(mean(prbs.packets_by_slot), 1.0, 0.04);
    EXPECT_DOUBLE_EQ(mean(phase_aligned), 1.0);
    EXPECT_LT(peak(prbs.packets_by_slot), peak(phase_aligned) / 4);
    EXPECT_LT(variance(prbs.packets_by_slot), variance(phase_aligned) / 10.0);
}

struct TimedPacket {
    RnicRingCamPacket packet;
};

TEST(RnicBurstTrendTest, RingCamRestoresPacedEnvelopeAfterTransitCompression) {
    constexpr uint64_t kSenderCount = 64;
    constexpr uint64_t kSlotCount = 4096;
    constexpr uint64_t kSlotPs = 1000;
    constexpr uint64_t kTransitSlots = 7;
    constexpr uint64_t kCompressionBlockSlots = 32;
    constexpr uint64_t kDelayWindowSlots = 128;
    constexpr uint64_t kWireBytes = 1500;

    const AggregateTrace dispatch =
        makeNodeDistinctPrbsTrace(20260713, kSenderCount, kSlotCount);
    std::vector<TimedPacket> arrivals;
    arrivals.reserve(dispatch.packet_count);

    uint64_t packet_id = 0;
    std::vector<RnicPrbsPacer> replay_pacers;
    replay_pacers.reserve(kSenderCount);
    for (uint64_t sender = 0; sender < kSenderCount; ++sender) {
        replay_pacers.emplace_back(20260713, sender);
    }
    for (uint64_t slot = 0; slot < kSlotCount; ++slot) {
        const uint64_t block_end_slot =
            ((slot / kCompressionBlockSlots) + 1) * kCompressionBlockSlots;
        for (uint64_t sender = 0; sender < kSenderCount; ++sender) {
            if (!replay_pacers[sender]
                     .selectEqualWireQuantum({{sender + 1, 1}}, kSenderCount)
                     .has_value()) {
                continue;
            }
            const uint64_t eta_ps = (slot + kTransitSlots) * kSlotPs;
            const uint64_t arrival_ps =
                (block_end_slot + kTransitSlots) * kSlotPs;
            arrivals.push_back({{packet_id++,
                                 sender + 1,
                                 eta_ps,
                                 arrival_ps,
                                 {kWireBytes, kWireBytes}}});
        }
    }
    ASSERT_EQ(arrivals.size(), dispatch.packet_count);
    std::sort(arrivals.begin(), arrivals.end(), [](const TimedPacket& lhs,
                                                   const TimedPacket& rhs) {
        return std::tie(lhs.packet.arrival_ps,
                        lhs.packet.eta_ps,
                        lhs.packet.packet_id)
               < std::tie(rhs.packet.arrival_ps,
                          rhs.packet.eta_ps,
                          rhs.packet.packet_id);
    });

    RnicRingCam ring_cam({kDelayWindowSlots * kSlotPs,
                          kSlotPs,
                          kSenderCount * kDelayWindowSlots * kWireBytes});
    std::map<uint64_t, uint64_t> raw_arrivals_by_slot;
    std::map<uint64_t, uint64_t> releases_by_slot;
    uint64_t released_packets = 0;
    const auto account_releases = [&](const std::vector<RnicRingCamRelease>& releases) {
        for (const RnicRingCamRelease& release : releases) {
            ++releases_by_slot[release.logical_release_ps / kSlotPs];
            ++released_packets;
        }
    };

    for (const TimedPacket& timed : arrivals) {
        ++raw_arrivals_by_slot[timed.packet.arrival_ps / kSlotPs];
        const RnicRingCamArrivalResult result = ring_cam.processArrival(timed.packet);
        account_releases(result.released_before_admission);
        ASSERT_EQ(result.admission, RnicRingCamAdmission::Admitted);
    }
    account_releases(ring_cam.advanceTo(
        (kSlotCount + kTransitSlots + kDelayWindowSlots + 1) * kSlotPs));

    ASSERT_EQ(released_packets, dispatch.packet_count);
    ASSERT_EQ(ring_cam.wireOccupancyBytes(), 0u);
    for (uint64_t slot = 0; slot < kSlotCount; ++slot) {
        const uint64_t release_slot = slot + kTransitSlots + kDelayWindowSlots;
        EXPECT_EQ(releases_by_slot[release_slot], dispatch.packets_by_slot[slot]);
    }

    uint64_t raw_peak = 0;
    for (const auto& item : raw_arrivals_by_slot) {
        raw_peak = std::max(raw_peak, item.second);
    }
    uint64_t release_peak = 0;
    for (const auto& item : releases_by_slot) {
        release_peak = std::max(release_peak, item.second);
    }

    EXPECT_EQ(release_peak, peak(dispatch.packets_by_slot));
    EXPECT_GT(raw_peak, release_peak * 2);
    EXPECT_LE(
        ring_cam.wireHighWatermarkBytes(), ring_cam.wireByteCapacity());
}

}  // namespace
