// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "dragonfly_progressive_congestion.h"

namespace {

using htsim::DragonflyAdvertisementDisposition;
using htsim::DragonflyDownstreamAdvertisement;
using htsim::DragonflyFourBitRemap;
using htsim::DragonflyProgressiveCongestion;
using htsim::DragonflyProgressiveCongestionConfig;

TEST(DragonflyFourBitRemapTest, ThresholdsAreMonotonicAndSaturate) {
    DragonflyFourBitRemap::Thresholds thresholds{};
    for (std::size_t index = 0; index < thresholds.size(); ++index) {
        thresholds[index] = static_cast<std::uint64_t>((index + 1) * 10);
    }
    const DragonflyFourBitRemap remap(thresholds);

    EXPECT_EQ(remap.map(0), 0U);
    EXPECT_EQ(remap.map(9), 0U);
    EXPECT_EQ(remap.map(10), 1U);
    EXPECT_EQ(remap.map(149), 14U);
    EXPECT_EQ(remap.map(150), 15U);
    EXPECT_EQ(remap.map(std::numeric_limits<std::uint64_t>::max()), 15U);
}

TEST(DragonflyFourBitRemapTest, RejectsInvalidOrOverflowingThresholds) {
    auto thresholds = DragonflyFourBitRemap::uniform(10).thresholds();
    thresholds[7] = thresholds[6];
    EXPECT_THROW((DragonflyFourBitRemap{thresholds}), std::invalid_argument);

    thresholds = DragonflyFourBitRemap::uniform(10).thresholds();
    thresholds[0] = 0;
    EXPECT_THROW((DragonflyFourBitRemap{thresholds}), std::invalid_argument);
    EXPECT_THROW(DragonflyFourBitRemap::uniform(0), std::invalid_argument);
    EXPECT_THROW(DragonflyFourBitRemap::uniform(std::numeric_limits<std::uint64_t>::max() / 15 + 1),
                 std::overflow_error);
}

TEST(DragonflyProgressiveCongestionTest, SendArrivalAndCreditLifecycleExcludesWireBytes) {
    DragonflyProgressiveCongestion signal;

    signal.messageSent(1, 100);
    signal.messageSent(2, 40);
    EXPECT_EQ(signal.outstandingSentBytes(), 140U);
    EXPECT_EQ(signal.inFlightBytes(), 140U);
    EXPECT_EQ(signal.inFlightMessageCount(), 2U);
    EXPECT_EQ(signal.farEndResidentBytes(), 0U);

    EXPECT_EQ(signal.messageArrived(2), 40U);
    EXPECT_EQ(signal.inFlightBytes(), 100U);
    EXPECT_EQ(signal.farEndResidentBytes(), 40U);

    signal.creditReturned(30);
    EXPECT_EQ(signal.outstandingSentBytes(), 110U);
    EXPECT_EQ(signal.farEndResidentBytes(), 10U);
    EXPECT_THROW(signal.creditReturned(11), std::underflow_error);

    EXPECT_EQ(signal.messageArrived(1), 100U);
    EXPECT_EQ(signal.inFlightMessageCount(), 0U);
    EXPECT_EQ(signal.farEndResidentBytes(), 110U);
    signal.creditReturned(110);
    EXPECT_EQ(signal.outstandingSentBytes(), 0U);
    EXPECT_EQ(signal.farEndResidentBytes(), 0U);
}

TEST(DragonflyProgressiveCongestionTest, RejectsMalformedCounterTransitions) {
    DragonflyProgressiveCongestionConfig zero_delay;
    zero_delay.minimum_downstream_delay_ps = 0;
    EXPECT_THROW(DragonflyProgressiveCongestion{zero_delay}, std::invalid_argument);

    DragonflyProgressiveCongestionConfig zero_maximum_age;
    zero_maximum_age.maximum_downstream_age_ps = 0;
    EXPECT_THROW(DragonflyProgressiveCongestion{zero_maximum_age}, std::invalid_argument);

    DragonflyProgressiveCongestion signal;
    signal.setNearEndWaitingBytes(std::numeric_limits<std::uint64_t>::max());
    EXPECT_THROW(signal.addNearEndWaitingBytes(1), std::overflow_error);
    signal.setNearEndWaitingBytes(5);
    EXPECT_THROW(signal.removeNearEndWaitingBytes(6), std::underflow_error);

    EXPECT_THROW(signal.messageSent(1, 0), std::invalid_argument);
    signal.messageSent(1, 1);
    EXPECT_THROW(signal.messageSent(1, 1), std::invalid_argument);
    EXPECT_THROW(signal.messageArrived(2), std::invalid_argument);
    EXPECT_THROW(signal.creditReturned(1), std::underflow_error);
    EXPECT_THROW(signal.creditReturned(0), std::invalid_argument);

    DragonflyProgressiveCongestion overflow;
    overflow.messageSent(1, std::numeric_limits<std::uint64_t>::max());
    EXPECT_THROW(overflow.messageSent(2, 1), std::overflow_error);
    EXPECT_EQ(overflow.outstandingSentBytes(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(overflow.inFlightMessageCount(), 1U);
}

TEST(DragonflyProgressiveCongestionTest, AdvertisementCannotBeConsumedBeforePhysicalArrival) {
    DragonflyProgressiveCongestionConfig config;
    config.minimum_downstream_delay_ps = 20;
    DragonflyProgressiveCongestion signal(config);
    const auto advertisement = signal.makeDownstreamAdvertisement(7, 100, 80);
    EXPECT_EQ(advertisement.earliest_arrival_time_ps, 120U);

    EXPECT_EQ(signal.consumeDownstreamAdvertisement(advertisement, 119),
              DragonflyAdvertisementDisposition::NotYetPhysicallyArrived);
    EXPECT_FALSE(signal.hasDownstreamAdvertisement());
    EXPECT_EQ(signal.components(119).downstream, 0U);

    EXPECT_EQ(signal.consumeDownstreamAdvertisement(advertisement, 120),
              DragonflyAdvertisementDisposition::Accepted);
    ASSERT_TRUE(signal.downstreamConsumptionTimePs().has_value());
    EXPECT_EQ(*signal.downstreamConsumptionTimePs(), 120U);

    DragonflyDownstreamAdvertisement impossible{8, 200, 219, 90};
    EXPECT_THROW(signal.consumeDownstreamAdvertisement(impossible, 220), std::invalid_argument);
    const auto future_observation = signal.makeDownstreamAdvertisement(9, 300, 90);
    EXPECT_THROW(signal.consumeDownstreamAdvertisement(future_observation, 299),
                 std::invalid_argument);
    EXPECT_THROW(
        signal.makeDownstreamAdvertisement(9, std::numeric_limits<std::uint64_t>::max() - 19, 0),
        std::overflow_error);
}

TEST(DragonflyProgressiveCongestionTest,
     RejectsStaleAndReorderedAdvertisementsWithoutChangingSignal) {
    DragonflyProgressiveCongestionConfig config;
    config.minimum_downstream_delay_ps = 10;
    DragonflyProgressiveCongestion signal(config);

    const auto first = signal.makeDownstreamAdvertisement(10, 100, 80);
    EXPECT_EQ(signal.consumeDownstreamAdvertisement(first, 110),
              DragonflyAdvertisementDisposition::Accepted);

    const auto older_sequence = signal.makeDownstreamAdvertisement(9, 110, 900);
    EXPECT_EQ(signal.consumeDownstreamAdvertisement(older_sequence, 120),
              DragonflyAdvertisementDisposition::StaleSequence);
    EXPECT_EQ(signal.downstreamWaitingBytes(), 80U);

    const auto older_observation = signal.makeDownstreamAdvertisement(11, 99, 700);
    EXPECT_EQ(signal.consumeDownstreamAdvertisement(older_observation, 120),
              DragonflyAdvertisementDisposition::StaleObservation);
    EXPECT_EQ(signal.downstreamWaitingBytes(), 80U);

    const auto next = signal.makeDownstreamAdvertisement(11, 100, 60);
    EXPECT_EQ(signal.consumeDownstreamAdvertisement(next, 120),
              DragonflyAdvertisementDisposition::Accepted);
    ASSERT_TRUE(signal.downstreamAdvertisement().has_value());
    EXPECT_EQ(signal.downstreamAdvertisement()->sequence, 11U);
    EXPECT_EQ(signal.downstreamWaitingBytes(), 60U);
}

TEST(DragonflyProgressiveCongestionTest, ComponentsUseIndependentRemapsAndSaturatingComposition) {
    DragonflyProgressiveCongestionConfig config;
    config.near_end_remap = DragonflyFourBitRemap::uniform(10);
    config.far_end_remap = DragonflyFourBitRemap::uniform(20);
    config.downstream_remap = DragonflyFourBitRemap::uniform(5);
    config.minimum_downstream_delay_ps = 25;
    config.maximum_downstream_age_ps = 100;
    DragonflyProgressiveCongestion signal(config);

    signal.setNearEndWaitingBytes(35);
    signal.messageSent(1, 100);
    signal.messageArrived(1);
    signal.messageSent(2, 60);
    const auto advertisement = signal.makeDownstreamAdvertisement(1, 1000, 36);
    ASSERT_EQ(signal.consumeDownstreamAdvertisement(advertisement, 1025),
              DragonflyAdvertisementDisposition::Accepted);

    const auto components = signal.components(1025);
    EXPECT_EQ(components.near_end, 3U);
    EXPECT_EQ(components.far_end, 5U);
    EXPECT_EQ(components.downstream, 7U);
    EXPECT_EQ(components.compose(), 15U);

    signal.creditReturned(100);
    EXPECT_EQ(signal.farEndResidentBytes(), 0U);
    EXPECT_EQ(signal.inFlightBytes(), 60U);
    EXPECT_EQ(signal.components(1025).far_end, 0U);
}

TEST(DragonflyProgressiveCongestionTest,
     DownstreamSignalIsFreshThroughExactAgeBoundaryThenExpires) {
    DragonflyProgressiveCongestionConfig config;
    config.minimum_downstream_delay_ps = 20;
    config.maximum_downstream_age_ps = 50;
    config.downstream_remap = DragonflyFourBitRemap::uniform(10);
    DragonflyProgressiveCongestion signal(config);

    const auto advertisement = signal.makeDownstreamAdvertisement(1, 100, 35);
    ASSERT_EQ(signal.consumeDownstreamAdvertisement(advertisement, 120),
              DragonflyAdvertisementDisposition::Accepted);

    EXPECT_EQ(signal.components(149).downstream, 3U);
    EXPECT_EQ(signal.components(150).downstream, 3U);
    EXPECT_EQ(signal.components(151).downstream, 0U);

    // The stored message remains inspectable for diagnostics, but cannot
    // contribute to routing after it expires.
    EXPECT_EQ(signal.downstreamWaitingBytes(), 35U);
    ASSERT_TRUE(signal.downstreamAdvertisement().has_value());
    EXPECT_THROW(signal.components(150), std::invalid_argument);
}

TEST(DragonflyProgressiveCongestionTest, RejectsEventTimeRegression) {
    DragonflyProgressiveCongestionConfig config;
    config.minimum_downstream_delay_ps = 10;
    config.maximum_downstream_age_ps = 100;
    DragonflyProgressiveCongestion signal(config);

    const auto first = signal.makeDownstreamAdvertisement(1, 100, 20);
    ASSERT_EQ(signal.consumeDownstreamAdvertisement(first, 110),
              DragonflyAdvertisementDisposition::Accepted);
    EXPECT_EQ(signal.components(120).downstream, 15U);

    const auto second = signal.makeDownstreamAdvertisement(2, 110, 10);
    EXPECT_THROW(signal.consumeDownstreamAdvertisement(second, 119), std::invalid_argument);
}

}  // namespace
