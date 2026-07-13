// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_prbs_pacer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

uint64_t splitmix64(uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

}  // namespace

RnicPrbsPacer::RnicPrbsPacer(uint64_t global_seed, uint64_t node_id)
    : _manifest{"galois-lfsr64",
                kAlgorithmVersion,
                "x^64+x^63+x^61+x^60+1",
                kFeedbackMask,
                "splitmix64-pair-v1",
                global_seed,
                node_id,
                deriveNodeSeed(global_seed, node_id)},
      _state(_manifest.derived_node_seed) {}

uint64_t RnicPrbsPacer::nextPrbsWord() {
    const uint64_t least_significant_bit = _state & UINT64_C(1);
    _state >>= 1;
    if (least_significant_bit != 0) {
        _state ^= kFeedbackMask;
    }
    return _state;
}

std::optional<RnicPrbsPacer::FlowId> RnicPrbsPacer::selectEqualWireQuantum(
        const std::vector<RnicPrbsCandidate>& candidates,
        RateBps access_capacity_bps) {
    if (access_capacity_bps == 0) {
        throw std::invalid_argument("RNIC PRBS access capacity must be nonzero");
    }

    std::vector<RnicPrbsCandidate> ordered_candidates = candidates;
    std::sort(ordered_candidates.begin(),
              ordered_candidates.end(),
              [](const RnicPrbsCandidate& lhs, const RnicPrbsCandidate& rhs) {
                  return lhs.flow_id < rhs.flow_id;
              });

    RateBps total_rate_bps = 0;
    for (size_t i = 0; i < ordered_candidates.size(); ++i) {
        const RnicPrbsCandidate& candidate = ordered_candidates[i];
        if (i > 0 && candidate.flow_id == ordered_candidates[i - 1].flow_id) {
            throw std::invalid_argument("duplicate RNIC PRBS candidate flow id");
        }
        if (candidate.rate_bps > access_capacity_bps - total_rate_bps) {
            throw std::invalid_argument("RNIC PRBS candidate rates exceed access capacity");
        }
        total_rate_bps += candidate.rate_bps;
    }

    const uint64_t ticket = nextBounded(access_capacity_bps);
    RateBps cumulative_rate_bps = 0;
    for (const RnicPrbsCandidate& candidate : ordered_candidates) {
        cumulative_rate_bps += candidate.rate_bps;
        if (ticket < cumulative_rate_bps) {
            return candidate.flow_id;
        }
    }
    return std::nullopt;
}

const RnicPrbsManifest& RnicPrbsPacer::manifest() const noexcept {
    return _manifest;
}

uint64_t RnicPrbsPacer::state() const noexcept {
    return _state;
}

uint64_t RnicPrbsPacer::deriveNodeSeed(uint64_t global_seed, uint64_t node_id) noexcept {
    const uint64_t global_component = splitmix64(global_seed ^ UINT64_C(0x726e69632d707262));
    const uint64_t node_component = splitmix64(node_id ^ UINT64_C(0x732d6e6f64652d31));
    const uint64_t derived_seed = splitmix64(global_component ^ node_component);

    // Zero is the absorbing state of an LFSR. The fixed replacement is part of
    // seed-derivation version 1 and still depends on no runtime state.
    return derived_seed == 0 ? kFeedbackMask : derived_seed;
}

uint64_t RnicPrbsPacer::nextBounded(uint64_t exclusive_upper_bound) {
    // The maximal LFSR visits every value in [1, UINT64_MAX]. Rejection over
    // that exact population makes (word - 1) modulo the bound unbiased.
    const uint64_t population = std::numeric_limits<uint64_t>::max();
    const uint64_t accepted_population =
        population - (population % exclusive_upper_bound);

    uint64_t word;
    do {
        word = nextPrbsWord();
    } while (word > accepted_population);
    return (word - 1) % exclusive_upper_bound;
}
