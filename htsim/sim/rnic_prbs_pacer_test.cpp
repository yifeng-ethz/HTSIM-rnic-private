// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_prbs_pacer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

TEST(RnicPrbsPacerTest, ManifestIdentifiesStableGeneratorAndNodeSeed) {
    RnicPrbsPacer pacer(1234, 56);
    const RnicPrbsManifest& manifest = pacer.manifest();

    EXPECT_EQ(manifest.algorithm_name, "galois-lfsr64");
    EXPECT_EQ(manifest.algorithm_version, 1u);
    EXPECT_EQ(manifest.polynomial, "x^64+x^63+x^61+x^60+1");
    EXPECT_EQ(manifest.feedback_mask, UINT64_C(0xd800000000000000));
    EXPECT_EQ(manifest.seed_derivation, "splitmix64-pair-v1");
    EXPECT_EQ(manifest.global_seed, 1234u);
    EXPECT_EQ(manifest.node_id, 56u);
    EXPECT_NE(manifest.derived_node_seed, 0u);
    EXPECT_EQ(pacer.state(), manifest.derived_node_seed);
}

TEST(RnicPrbsPacerTest, GaloisStepUsesDocumentedPolynomialConvention) {
    // Seed derivation is separately tested; derive a known state through the
    // public stream and reproduce its next step using the manifest mask.
    RnicPrbsPacer pacer(9, 4);
    const uint64_t initial = pacer.state();
    const uint64_t expected =
        (initial >> 1) ^ ((initial & 1) ? RnicPrbsPacer::kFeedbackMask : 0);

    EXPECT_EQ(pacer.nextPrbsWord(), expected);
    EXPECT_NE(pacer.state(), 0u);
}

TEST(RnicPrbsPacerTest, FrozenGoldenVectorProtectsReplayCompatibility) {
    RnicPrbsPacer pacer(1234, 56);
    constexpr std::array<uint64_t, 4> kExpectedWords{
        UINT64_C(0x660326f99e058495),
        UINT64_C(0xeb01937ccf02c24a),
        UINT64_C(0x7580c9be67816125),
        UINT64_C(0xe2c064df33c0b092),
    };

    EXPECT_EQ(pacer.manifest().derived_node_seed, UINT64_C(0xcc064df33c0b092a));
    for (const uint64_t expected_word : kExpectedWords) {
        EXPECT_EQ(pacer.nextPrbsWord(), expected_word);
    }
}

TEST(RnicPrbsPacerTest, NodeSeedsAreAlwaysNonzeroForRepresentativeInputs) {
    for (uint64_t global_seed = 0; global_seed < 32; ++global_seed) {
        for (uint64_t node_id = 0; node_id < 32; ++node_id) {
            RnicPrbsPacer pacer(global_seed, node_id);
            EXPECT_NE(pacer.state(), 0u);
            for (int step = 0; step < 64; ++step) {
                EXPECT_NE(pacer.nextPrbsWord(), 0u);
            }
        }
    }
}

TEST(RnicPrbsPacerTest, SameSeedAndNodeReplayExactly) {
    RnicPrbsPacer first(987654321, 17);
    RnicPrbsPacer replay(987654321, 17);
    const std::vector<RnicPrbsCandidate> candidates{{30, 200}, {10, 300}};

    EXPECT_EQ(first.manifest().derived_node_seed, replay.manifest().derived_node_seed);
    for (int opportunity = 0; opportunity < 1000; ++opportunity) {
        EXPECT_EQ(first.selectEqualWireQuantum(candidates, 1000),
                  replay.selectEqualWireQuantum(candidates, 1000));
        EXPECT_EQ(first.state(), replay.state());
    }
}

TEST(RnicPrbsPacerTest, ChangingNodeChangesOnlyThatNodeStream) {
    RnicPrbsPacer node_seven(42, 7);
    RnicPrbsPacer node_seven_replay(42, 7);
    RnicPrbsPacer node_eight(42, 8);

    EXPECT_NE(node_seven.manifest().derived_node_seed,
              node_eight.manifest().derived_node_seed);
    bool streams_differ = false;
    for (int step = 0; step < 64; ++step) {
        const uint64_t seven_word = node_seven.nextPrbsWord();
        EXPECT_EQ(seven_word, node_seven_replay.nextPrbsWord());
        streams_differ = streams_differ || seven_word != node_eight.nextPrbsWord();
    }
    EXPECT_TRUE(streams_differ);
}

TEST(RnicPrbsPacerTest, RejectsDuplicateIdsAndRateSumAboveCapacity) {
    RnicPrbsPacer pacer(1, 2);

    EXPECT_THROW(pacer.selectEqualWireQuantum({{1, 10}, {1, 20}}, 100),
                 std::invalid_argument);
    EXPECT_THROW(pacer.selectEqualWireQuantum({{1, 60}, {2, 41}}, 100),
                 std::invalid_argument);
    EXPECT_THROW(pacer.selectEqualWireQuantum({}, 0), std::invalid_argument);
}

TEST(RnicPrbsPacerTest, CandidateOrderDoesNotChangeReplay) {
    RnicPrbsPacer first(55, 66);
    RnicPrbsPacer reordered(55, 66);
    const std::vector<RnicPrbsCandidate> ascending{{1, 100}, {2, 200}, {3, 300}};
    const std::vector<RnicPrbsCandidate> descending{{3, 300}, {2, 200}, {1, 100}};

    for (int opportunity = 0; opportunity < 1000; ++opportunity) {
        EXPECT_EQ(first.selectEqualWireQuantum(ascending, 1000),
                  reordered.selectEqualWireQuantum(descending, 1000));
    }
}

TEST(RnicPrbsPacerTest, FullRateCandidateNeverProducesIdleOpportunity) {
    RnicPrbsPacer pacer(314159, 26);
    for (int opportunity = 0; opportunity < 10000; ++opportunity) {
        EXPECT_EQ(pacer.selectEqualWireQuantum({{7, 400}}, 400),
                  std::optional<uint64_t>(7));
    }
}

TEST(RnicPrbsPacerTest, MeanSharesIncludeIdleRemainder) {
    RnicPrbsPacer pacer(20260713, 101);
    const std::vector<RnicPrbsCandidate> candidates{{1, 200}, {2, 300}};
    std::array<uint64_t, 3> counts{};
    constexpr uint64_t kOpportunities = 300000;

    for (uint64_t opportunity = 0; opportunity < kOpportunities; ++opportunity) {
        const std::optional<uint64_t> selected =
            pacer.selectEqualWireQuantum(candidates, 1000);
        if (!selected.has_value()) {
            counts[2]++;
        } else {
            counts[*selected - 1]++;
        }
    }

    EXPECT_NEAR(static_cast<double>(counts[0]) / kOpportunities, 0.2, 0.01);
    EXPECT_NEAR(static_cast<double>(counts[1]) / kOpportunities, 0.3, 0.01);
    EXPECT_NEAR(static_cast<double>(counts[2]) / kOpportunities, 0.5, 0.01);
}

TEST(RnicPrbsPacerTest, OpportunityLotteryDoesNotAddPositiveGapJitter) {
    RnicPrbsPacer pacer(777, 88);
    constexpr uint64_t kOpportunities = 200000;
    uint64_t selected_count = 0;
    bool saw_adjacent_selected_opportunities = false;
    bool previous_selected = false;

    for (uint64_t opportunity = 0; opportunity < kOpportunities; ++opportunity) {
        const bool selected = pacer.selectEqualWireQuantum({{1, 600}}, 1000).has_value();
        selected_count += selected;
        saw_adjacent_selected_opportunities =
            saw_adjacent_selected_opportunities || (selected && previous_selected);
        previous_selected = selected;
    }

    // A 60% grant occupies 60% of fixed wire opportunities. Adding a positive
    // random delay after each nominal gap would undershoot this grant.
    EXPECT_NEAR(static_cast<double>(selected_count) / kOpportunities, 0.6, 0.01);
    EXPECT_TRUE(saw_adjacent_selected_opportunities);
}

}  // namespace
