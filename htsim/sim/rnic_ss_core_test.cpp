// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ss_core.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr RnicSsEndpointPair kPair{3, 11};
constexpr RnicSsCongestionDomainId kDomain = 42;

RnicSsWirePacketMetadata wirePacket(RnicSsPacketKind kind,
                                    RnicSsNodeId source,
                                    RnicSsNodeId destination,
                                    RnicSsPacketPayload payload,
                                    std::uint32_t wire_bytes = 64) {
    return {kind, source, destination, wire_bytes, std::move(payload)};
}

TEST(RnicSsWireMetadataTest, ValidatesEveryPhysicalPacketKind) {
    const std::vector<RnicSsWirePacketMetadata> packets{
        wirePacket(RnicSsPacketKind::DATA, 3, 11, RnicSsDataMetadata{kPair, 0, 32, 7}, 96),
        wirePacket(
            RnicSsPacketKind::ACK_SACK, 11, 3,
            RnicSsAckSackMetadata{kPair, 1, {0b100, 0}, RnicSsDelayedLoadSample{2, 100, 1000, 9}}),
        wirePacket(RnicSsPacketKind::TELEMETRY, 4, 3, RnicSsTelemetryMetadata{{2, 100, 1000, 9}}),
        wirePacket(RnicSsPacketKind::BP_ENABLE, 11, 3,
                   RnicSsBackpressureMetadata{kPair, kDomain, 1, 4096}),
        wirePacket(RnicSsPacketKind::BP_DISABLE, 11, 3,
                   RnicSsBackpressureMetadata{kPair, kDomain, 2, 0}),
        wirePacket(RnicSsPacketKind::CREDIT, 11, 3, RnicSsCreditMetadata{kPair, kDomain, 1, 8192}),
    };

    for (const auto& packet : packets) {
        EXPECT_NO_THROW(validateRnicSsWirePacket(packet));
    }
}

TEST(RnicSsWireMetadataTest, RejectsWrongPayloadAndNonPhysicalDirection) {
    EXPECT_THROW(validateRnicSsWirePacket(wirePacket(RnicSsPacketKind::ACK_SACK, 11, 3,
                                                     RnicSsCreditMetadata{kPair, kDomain, 1, 64})),
                 std::invalid_argument);
    EXPECT_THROW(
        validateRnicSsWirePacket(wirePacket(RnicSsPacketKind::BP_ENABLE, 3, 11,
                                            RnicSsBackpressureMetadata{kPair, kDomain, 1, 64})),
        std::invalid_argument);
    EXPECT_THROW(
        validateRnicSsWirePacket(wirePacket(RnicSsPacketKind::BP_DISABLE, 11, 3,
                                            RnicSsBackpressureMetadata{kPair, kDomain, 2, 1})),
        std::invalid_argument);
    EXPECT_THROW(validateRnicSsWirePacket(wirePacket(RnicSsPacketKind::BP_ENABLE, 11, 3,
                                                     RnicSsBackpressureMetadata{kPair, 0, 1, 64})),
                 std::invalid_argument);
}

TEST(RnicSsSackScoreboardTest, AdvancesAcrossAReorderedHole) {
    RnicSsSackScoreboard scoreboard;

    EXPECT_EQ(scoreboard.observe(2).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.observe(1).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.nextExpectedSequence(), 0U);
    EXPECT_EQ(scoreboard.bitmap(), (std::array<std::uint64_t, 2>{0b110U, 0}));

    const RnicSsReceiveResult result = scoreboard.observe(0);
    EXPECT_EQ(result.disposition, RnicSsReceiveDisposition::NEW_IN_ORDER);
    EXPECT_EQ(result.cumulative_advance, 3U);
    EXPECT_EQ(scoreboard.nextExpectedSequence(), 3U);
    EXPECT_EQ(scoreboard.bitmap(), (std::array<std::uint64_t, 2>{0, 0}));
    EXPECT_EQ(scoreboard.observe(1).disposition, RnicSsReceiveDisposition::DUPLICATE);
}

TEST(RnicSsSackScoreboardTest, BoundsReorderingToThePhysicalBitmap) {
    RnicSsSackScoreboard scoreboard(100);
    EXPECT_EQ(scoreboard.observe(163).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.observe(164).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.observe(227).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.observe(228).disposition, RnicSsReceiveDisposition::OUTSIDE_SACK_WINDOW);
    const auto ack = scoreboard.snapshot(kPair);
    EXPECT_EQ(ack.next_expected_sequence, 100U);
    EXPECT_EQ(ack.sack_bitmap,
              (std::array<std::uint64_t, 2>{UINT64_C(1) << 63, UINT64_C(1) | (UINT64_C(1) << 63)}));
    EXPECT_EQ(ack.sack_bitmap[0] & 1ULL, 0U);
}

TEST(RnicSsSackScoreboardTest, AdvancesAcrossWord63And64Boundary) {
    RnicSsSackScoreboard scoreboard;
    EXPECT_EQ(scoreboard.observe(63).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.observe(64).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    EXPECT_EQ(scoreboard.observe(127).disposition, RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    for (RnicSsSequence sequence = 1; sequence < 63; ++sequence) {
        EXPECT_EQ(scoreboard.observe(sequence).disposition,
                  RnicSsReceiveDisposition::NEW_OUT_OF_ORDER);
    }
    const RnicSsReceiveResult first = scoreboard.observe(0);
    EXPECT_EQ(first.cumulative_advance, 65U);
    EXPECT_EQ(scoreboard.nextExpectedSequence(), 65U);
    EXPECT_EQ(scoreboard.bitmap(), (std::array<std::uint64_t, 2>{UINT64_C(1) << 62, 0}));

    for (RnicSsSequence sequence = 65; sequence < 127; ++sequence) {
        scoreboard.observe(sequence);
    }
    EXPECT_EQ(scoreboard.nextExpectedSequence(), 128U);
    EXPECT_EQ(scoreboard.bitmap(), (std::array<std::uint64_t, 2>{0, 0}));
}

TEST(RnicSsSackScoreboardTest, UsesMaximumSequenceAsCumulativeSentinel) {
    constexpr auto last_data_sequence = std::numeric_limits<RnicSsSequence>::max() - 1;
    RnicSsSackScoreboard scoreboard(last_data_sequence);

    EXPECT_EQ(scoreboard.observe(last_data_sequence).disposition,
              RnicSsReceiveDisposition::NEW_IN_ORDER);
    EXPECT_EQ(scoreboard.nextExpectedSequence(), std::numeric_limits<RnicSsSequence>::max());
    EXPECT_EQ(scoreboard.bitmap(), (std::array<std::uint64_t, 2>{0, 0}));
    EXPECT_THROW(scoreboard.observe(std::numeric_limits<RnicSsSequence>::max()),
                 std::invalid_argument);
}

TEST(RnicSsSelectiveRepeatLedgerTest, SackLeavesOnlyTheMissingPackets) {
    RnicSsSelectiveRepeatLedger ledger(kPair, RnicSsSelectiveRepeatConfig{8, 100, 2});
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(ledger.recordNewTransmission(1000, 10), static_cast<std::uint64_t>(i));
    }

    // Cumulatively ACK 0 and selectively ACK 2 and 4.  Sequences 1 and 3
    // remain independently eligible for retransmission.
    const auto result = ledger.applyAck({kPair, 1, {(1ULL << 1) | (1ULL << 3), 0}});
    ASSERT_EQ(result.newly_acked.size(), 3U);
    EXPECT_EQ(result.newly_acked[0].sequence, 0U);
    EXPECT_EQ(result.newly_acked[1].sequence, 2U);
    EXPECT_EQ(result.newly_acked[2].sequence, 4U);
    ASSERT_EQ(ledger.outstandingPacketCount(), 2U);
    EXPECT_TRUE(ledger.outstanding().count(1));
    EXPECT_TRUE(ledger.outstanding().count(3));
    ASSERT_EQ(result.reported_holes.size(), 2U);
    EXPECT_EQ(result.reported_holes[0].sequence, 1U);
    EXPECT_EQ(result.reported_holes[1].sequence, 3U);

    ledger.recordRetransmission(1, 20, RnicSsRetransmissionReason::SACK_HOLE);
    EXPECT_EQ(ledger.outstanding().at(1).transmission_count, 2U);

    const auto retransmissions = ledger.retransmissionCandidates(110);
    ASSERT_EQ(retransmissions.size(), 1U);
    EXPECT_EQ(retransmissions[0].sequence, 3U);
    ledger.recordRetransmission(3, 110);
    EXPECT_EQ(ledger.outstanding().at(3).transmission_count, 2U);
    const auto post_sack_rto = ledger.retransmissionCandidates(150);
    ASSERT_EQ(post_sack_rto.size(), 1U);
    EXPECT_EQ(post_sack_rto[0].sequence, 1U);
}

TEST(RnicSsSelectiveRepeatLedgerTest, ExpiredPacketAfterFinalRetryIsATerminalFailure) {
    RnicSsSelectiveRepeatLedger ledger(kPair, RnicSsSelectiveRepeatConfig{1, 10, 1});
    EXPECT_EQ(ledger.recordNewTransmission(1000, 0), 0U);
    ASSERT_EQ(ledger.retransmissionCandidates(10).size(), 1U);
    ASSERT_TRUE(ledger.retransmissionCandidate(0, 10).has_value());
    EXPECT_FALSE(ledger.retransmissionCandidate(1, 10).has_value());
    ledger.recordRetransmission(0, 10);

    try {
        (void)ledger.retransmissionCandidates(20);
        FAIL() << "retry exhaustion must terminate the transport";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("retry budget exhausted"), std::string::npos);
        EXPECT_NE(message.find("source=3"), std::string::npos);
        EXPECT_NE(message.find("destination=11"), std::string::npos);
        EXPECT_NE(message.find("sequence=0"), std::string::npos);
        EXPECT_NE(message.find("retransmissions=1"), std::string::npos);
    }
    EXPECT_THROW((void)ledger.retransmissionCandidate(0, 20), std::runtime_error);
}

TEST(RnicSsSelectiveRepeatLedgerTest, RejectsAckOfUnsentData) {
    RnicSsSelectiveRepeatLedger ledger(kPair);
    ledger.recordNewTransmission(64, 0);
    EXPECT_THROW(ledger.applyAck({kPair, 1, {1ULL << 2, 0}}), std::invalid_argument);
    EXPECT_EQ(ledger.outstandingPacketCount(), 1U);
    EXPECT_THROW(ledger.applyAck({kPair, 2, {0, 0}}), std::invalid_argument);
    EXPECT_EQ(ledger.outstandingPacketCount(), 1U);
}

TEST(RnicSsSelectiveRepeatLedgerTest, RejectsAckBeforeConfiguredSequenceSpace) {
    RnicSsSelectiveRepeatLedger ledger(kPair, {}, 100);
    EXPECT_EQ(ledger.recordNewTransmission(64, 0), 100U);

    EXPECT_THROW(ledger.applyAck({kPair, 99, {0, 0}}), std::invalid_argument);
    EXPECT_EQ(ledger.outstandingPacketCount(), 1U);
}

TEST(RnicSsSelectiveRepeatLedgerTest, SelectiveLossDetectionCrossesBits63_64And127) {
    RnicSsSelectiveRepeatLedger ledger(kPair, RnicSsSelectiveRepeatConfig{128, 100, 2});
    for (std::uint32_t sequence = 0; sequence < 128; ++sequence) {
        EXPECT_EQ(ledger.recordNewTransmission(1000, 10), sequence);
    }
    EXPECT_FALSE(ledger.canSendNewPacket());

    const auto result =
        ledger.applyAck({kPair, 0, {UINT64_C(1) << 63, UINT64_C(1) | (UINT64_C(1) << 63)}});
    ASSERT_EQ(result.newly_acked.size(), 3U);
    EXPECT_EQ(result.newly_acked[0].sequence, 63U);
    EXPECT_EQ(result.newly_acked[1].sequence, 64U);
    EXPECT_EQ(result.newly_acked[2].sequence, 127U);
    EXPECT_EQ(ledger.outstandingPacketCount(), 125U);
    ASSERT_EQ(result.reported_holes.size(), 125U);
    EXPECT_EQ(result.reported_holes.front().sequence, 0U);
    EXPECT_EQ(result.reported_holes.back().sequence, 126U);
}

TEST(RnicSsPairCreditStateTest, AppliesPhysicalEpochsAndCumulativeCredits) {
    RnicSsPairCreditState state(kPair, RnicSsCreditConfig{4096}, kDomain);
    EXPECT_TRUE(state.canSend(4000));

    EXPECT_EQ(state.applyEnable({kPair, kDomain, 7, 1500}), RnicSsControlApplyResult::APPLIED);
    EXPECT_TRUE(state.canSend(1500));
    EXPECT_FALSE(state.canSend(1501));
    state.consumeForData(1000);
    EXPECT_EQ(state.snapshot().availableWireBytes(), 500U);

    EXPECT_EQ(state.applyCredit({kPair, kDomain, 7, 3000}), RnicSsControlApplyResult::APPLIED);
    EXPECT_EQ(state.applyCredit({kPair, kDomain, 7, 2000}),
              RnicSsControlApplyResult::DUPLICATE_OR_STALE);
    EXPECT_EQ(state.snapshot().availableWireBytes(), 2000U);
    EXPECT_EQ(state.applyCredit({kPair, kDomain, 8, 4000}), RnicSsControlApplyResult::WRONG_EPOCH);

    EXPECT_EQ(state.applyDisable({kPair, kDomain, 8, 0}), RnicSsControlApplyResult::APPLIED);
    EXPECT_TRUE(state.canSend(std::numeric_limits<std::uint32_t>::max()));
    EXPECT_EQ(state.applyEnable({kPair, kDomain, 7, 1}),
              RnicSsControlApplyResult::DUPLICATE_OR_STALE);
    EXPECT_THROW(state.applyEnable({kPair, kDomain + 1, 9, 1}), std::invalid_argument);
}

TEST(RnicSsPairCreditStateTest, TableKeepsDirectedContributorsIndependent) {
    RnicSsPairCreditTable table(RnicSsCreditConfig{4096});
    auto& first = table.stateFor({1, 9});
    auto& second = table.stateFor({2, 9});
    first.applyEnable({{1, 9}, 1, 1, 1000});

    EXPECT_TRUE(first.snapshot().backpressure_enabled);
    EXPECT_FALSE(second.snapshot().backpressure_enabled);
    EXPECT_EQ(table.size(), 2U);
    EXPECT_EQ(table.find({1, 9}), &first);
    EXPECT_EQ(table.find({9, 1}), nullptr);
}

TEST(RnicSsPathSelectorTest, SamplesFourDistinctPathsDeterministically) {
    const auto first = RnicSsHystereticPathSelector::sampleFourOfEight(kPair, 1234);
    const auto repeat = RnicSsHystereticPathSelector::sampleFourOfEight(kPair, 1234);
    const auto other = RnicSsHystereticPathSelector::sampleFourOfEight(kPair, 1235);

    EXPECT_EQ(first, repeat);
    EXPECT_NE(first, other);
    const std::set<std::uint8_t> unique(first.begin(), first.end());
    EXPECT_EQ(unique.size(), 4U);
    EXPECT_TRUE(
        std::all_of(first.begin(), first.end(), [](std::uint8_t path) { return path < 8; }));
}

TEST(RnicSsPathSelectorTest, EqualScoresPreserveHashedOrderAndCoverEverySpine) {
    RnicSsPathSelectionConfig config;
    config.unknown_path_queue_delay_ps = 0;
    RnicSsHystereticPathSelector selector(config);
    const auto decision = selector.select(kPair, 1234, 0);
    EXPECT_EQ(decision.selected_path, decision.candidates[0]);

    std::array<std::uint64_t, 8> first_choice_counts{};
    for (std::uint32_t source = 0; source < 64; ++source) {
        for (std::uint32_t destination = 0; destination < 64; ++destination) {
            if (source == destination) {
                continue;
            }
            const auto candidates =
                RnicSsHystereticPathSelector::sampleFourOfEight({source, destination}, 1234);
            ++first_choice_counts[candidates[0]];
        }
    }
    const auto bounds = std::minmax_element(first_choice_counts.begin(), first_choice_counts.end());
    EXPECT_GT(*bounds.first, 0U);
    EXPECT_LT(*bounds.second - *bounds.first, 100U);
}

TEST(RnicSsPathSelectorTest, UsesOnlyIngestedDelayedSamplesAndHysteresis) {
    RnicSsPathSelectionConfig config;
    config.hysteresis_queue_delay_ps = 20;
    config.maximum_sample_age_ps = 1000;
    config.unknown_path_queue_delay_ps = 10'000;
    RnicSsHystereticPathSelector selector(config);
    const auto candidates = RnicSsHystereticPathSelector::sampleFourOfEight(kPair, 55);

    // The best sampled path is 10 ps lower than the incumbent, inside the
    // 20-ps hysteresis band, so ordered routing stays on the incumbent.
    ASSERT_TRUE(selector.ingestLoadSample({candidates[0], 100, 900, 1}, 950));
    ASSERT_TRUE(selector.ingestLoadSample({candidates[1], 90, 900, 1}, 960));
    ASSERT_TRUE(selector.ingestLoadSample({candidates[2], 300, 900, 1}, 970));
    ASSERT_TRUE(selector.ingestLoadSample({candidates[3], 400, 900, 1}, 980));
    auto decision = selector.select(kPair, 55, 1000, candidates[0]);
    EXPECT_EQ(decision.selected_path, candidates[0]);
    EXPECT_TRUE(decision.retained_by_hysteresis);

    // A delayed physical update makes the alternative decisively better.
    ASSERT_TRUE(selector.ingestLoadSample({candidates[1], 70, 1100, 2}, 1150));
    decision = selector.select(kPair, 55, 1200, candidates[0]);
    EXPECT_EQ(decision.selected_path, candidates[1]);
    EXPECT_FALSE(decision.retained_by_hysteresis);
}

TEST(RnicSsPathSelectorTest, IgnoresReorderedTelemetryAndExpiresOldSamples) {
    RnicSsPathSelectionConfig config;
    config.maximum_sample_age_ps = 100;
    config.unknown_path_queue_delay_ps = 999;
    RnicSsHystereticPathSelector selector(config);
    const auto path = RnicSsHystereticPathSelector::sampleFourOfEight(kPair, 7)[0];

    EXPECT_TRUE(selector.ingestLoadSample({path, 10, 1000, 2}, 1050));
    EXPECT_FALSE(selector.ingestLoadSample({path, 1, 999, 99}, 1060));
    EXPECT_FALSE(selector.ingestLoadSample({path, 1, 1000, 1}, 1060));

    const auto fresh = selector.select(kPair, 7, 1099);
    const auto expired = selector.select(kPair, 7, 1101);
    const auto position = static_cast<std::size_t>(
        std::distance(fresh.candidates.begin(),
                      std::find(fresh.candidates.begin(), fresh.candidates.end(), path)));
    ASSERT_LT(position, 4U);
    EXPECT_TRUE(fresh.candidate_had_fresh_sample[position]);
    EXPECT_FALSE(expired.candidate_had_fresh_sample[position]);
    EXPECT_EQ(expired.candidate_queue_delay_ps[position], 999U);
}

TEST(RnicSsPathSelectorTest, CannotUseTelemetryBeforePhysicalArrival) {
    RnicSsHystereticPathSelector selector;
    const auto path = RnicSsHystereticPathSelector::sampleFourOfEight(kPair, 17)[0];
    ASSERT_TRUE(selector.ingestLoadSample({path, 10, 1000, 1}, 1100));

    EXPECT_THROW(selector.select(kPair, 17, 1099), std::invalid_argument);
    EXPECT_NO_THROW(selector.select(kPair, 17, 1100));
}

TEST(RnicSsConfigTest, RejectsNonFourOfEightAndUnboundedControlMistakes) {
    RnicSsPathSelectionConfig paths;
    paths.path_count = 4;
    EXPECT_THROW(paths.validate(), std::invalid_argument);
    EXPECT_THROW(RnicSsCreditConfig{0}.validate(), std::invalid_argument);
    EXPECT_THROW((RnicSsSelectiveRepeatConfig{129, 1, 1}.validate()), std::invalid_argument);
}

}  // namespace
