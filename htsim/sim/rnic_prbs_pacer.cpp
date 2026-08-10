// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_prbs_pacer.h"
#include "rnic_wide_integer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

using Wide = RnicWideInteger;

constexpr unsigned kPreferredHazardFractionBits = 32;

struct WeightedCandidate {
    RnicPrbsPacer::FlowId flow_id;
    Wide weight;
};

uint64_t splitmix64(uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

bool scaledHazardWeight(uint64_t wire_rate_bps,
                        uint64_t idle_wire_bytes,
                        uint64_t head_wire_bytes,
                        unsigned fraction_bits,
                        Wide* weight) noexcept {
    const Wide numerator = static_cast<Wide>(wire_rate_bps)
                           * idle_wire_bytes;
    const Wide whole = numerator / head_wire_bytes;
    const Wide remainder = numerator % head_wire_bytes;
    const Wide maximum = ~static_cast<Wide>(0);
    if (whole > (maximum >> fraction_bits)) {
        return false;
    }

    // remainder < head_wire_bytes <= UINT64_MAX, so the shifted value fits
    // within 96 bits for the preferred Q32 representation.
    *weight = (whole << fraction_bits)
              + ((remainder << fraction_bits) / head_wire_bytes);
    return true;
}

bool buildScaledHazards(
        const std::vector<RnicPrbsWireCandidate>& candidates,
        uint64_t idle_wire_rate_bps,
        uint64_t idle_wire_bytes,
        unsigned fraction_bits,
        std::vector<WeightedCandidate>* weighted_candidates,
        Wide* total_weight) {
    const Wide maximum = ~static_cast<Wide>(0);
    if (static_cast<Wide>(idle_wire_rate_bps)
        > (maximum >> fraction_bits)) {
        return false;
    }

    weighted_candidates->clear();
    weighted_candidates->reserve(candidates.size());
    *total_weight = static_cast<Wide>(idle_wire_rate_bps)
                    << fraction_bits;
    for (const RnicPrbsWireCandidate& candidate : candidates) {
        Wide weight;
        if (!scaledHazardWeight(candidate.wire_rate_bps,
                                idle_wire_bytes,
                                candidate.head_wire_bytes,
                                fraction_bits,
                                &weight)) {
            return false;
        }
        if (weight > maximum - *total_weight) {
            return false;
        }
        weighted_candidates->push_back({candidate.flow_id, weight});
        *total_weight += weight;
    }
    return *total_weight != 0;
}

// Compares draw/2^64 with numerator/denominator exactly. Long division emits
// the binary expansion of the rational one bit at a time, avoiding a
// 64-by-128-bit (192-bit) cross product. draw is a nonzero word from the
// maximal-period LFSR; the finite-grid probability error is at most 2^-64.
bool binaryFractionBelowRatio(uint64_t draw,
                              Wide numerator,
                              Wide denominator) noexcept {
    if (numerator >= denominator) {
        return true;
    }

    Wide remainder = numerator;
    for (int bit_index = 63; bit_index >= 0; --bit_index) {
        // This is remainder = 2*remainder modulo denominator without ever
        // overflowing Wide. The comparison is equivalent to 2*r >= d.
        const bool ratio_bit = remainder >= denominator - remainder;
        if (ratio_bit) {
            remainder -= denominator - remainder;
        } else {
            remainder += remainder;
        }

        const bool draw_bit = ((draw >> bit_index) & UINT64_C(1)) != 0;
        if (draw_bit != ratio_bit) {
            return !draw_bit;
        }
    }

    // Equal 64-bit prefixes compare below a non-terminating rational tail,
    // but not below an exactly equal dyadic rational.
    return remainder != 0;
}

}  // namespace

RnicPrbsPacer::RnicPrbsPacer(uint64_t global_seed, uint64_t node_id)
    : _manifest{kAlgorithmName,
                kAlgorithmVersion,
                "x^64+x^63+x^61+x^60+1",
                kFeedbackMask,
                kWordExtraction,
                kLfsrStepsPerWord,
                kBoundedDraw,
                "splitmix64-pair-v1",
                global_seed,
                node_id,
                deriveNodeSeed(global_seed, node_id)},
      _state(_manifest.derived_node_seed) {}

uint64_t RnicPrbsPacer::nextPrbsWord() {
    uint64_t word = 0;
    for (uint32_t bit_index = 0; bit_index < kLfsrStepsPerWord;
         ++bit_index) {
        const uint64_t output_bit = _state & UINT64_C(1);
        _state >>= 1;
        if (output_bit != 0) {
            _state ^= kFeedbackMask;
        }
        word |= output_bit << bit_index;
    }
    return word;
}

std::optional<RnicPrbsPacer::FlowId> RnicPrbsPacer::selectEqualWireQuantum(
        const std::vector<RnicPrbsCandidate>& candidates,
        WireRateBps wire_capacity_bps) {
    if (wire_capacity_bps == 0) {
        throw std::invalid_argument("RNIC PRBS wire capacity must be nonzero");
    }

    std::vector<RnicPrbsCandidate> ordered_candidates = candidates;
    std::sort(ordered_candidates.begin(),
              ordered_candidates.end(),
              [](const RnicPrbsCandidate& lhs, const RnicPrbsCandidate& rhs) {
                  return lhs.flow_id < rhs.flow_id;
              });

    WireRateBps total_wire_rate_bps = 0;
    for (size_t i = 0; i < ordered_candidates.size(); ++i) {
        const RnicPrbsCandidate& candidate = ordered_candidates[i];
        if (i > 0 && candidate.flow_id == ordered_candidates[i - 1].flow_id) {
            throw std::invalid_argument("duplicate RNIC PRBS candidate flow id");
        }
        if (candidate.wire_rate_bps
            > wire_capacity_bps - total_wire_rate_bps) {
            throw std::invalid_argument(
                "RNIC PRBS candidate wire rates exceed wire capacity");
        }
        total_wire_rate_bps += candidate.wire_rate_bps;
    }

    const uint64_t ticket = nextBounded(wire_capacity_bps);
    WireRateBps cumulative_wire_rate_bps = 0;
    for (const RnicPrbsCandidate& candidate : ordered_candidates) {
        cumulative_wire_rate_bps += candidate.wire_rate_bps;
        if (ticket < cumulative_wire_rate_bps) {
            return candidate.flow_id;
        }
    }
    return std::nullopt;
}

std::optional<RnicPrbsPacer::FlowId> RnicPrbsPacer::selectWireEvent(
        const std::vector<RnicPrbsWireCandidate>& candidates,
        WireRateBps wire_capacity_bps,
        uint64_t idle_wire_bytes) {
    if (wire_capacity_bps == 0) {
        throw std::invalid_argument("RNIC PRBS wire capacity must be nonzero");
    }
    if (idle_wire_bytes == 0) {
        throw std::invalid_argument("RNIC PRBS idle wire quantum must be nonzero");
    }

    bool all_equal_wire_quanta = true;
    std::vector<RnicPrbsCandidate> equal_candidates;
    equal_candidates.reserve(candidates.size());
    for (const RnicPrbsWireCandidate& candidate : candidates) {
        if (candidate.head_wire_bytes == 0
            || candidate.head_wire_bytes > idle_wire_bytes) {
            throw std::invalid_argument(
                "RNIC PRBS head wire size must be in [1, idle quantum]");
        }
        all_equal_wire_quanta =
            all_equal_wire_quanta
            && candidate.head_wire_bytes == idle_wire_bytes;
        equal_candidates.push_back({candidate.flow_id, candidate.wire_rate_bps});
    }

    if (all_equal_wire_quanta) {
        // This is deliberately the old path, including its ordering,
        // validation, bounded draw, and exact LFSR-word consumption.
        return selectEqualWireQuantum(equal_candidates, wire_capacity_bps);
    }

    std::vector<RnicPrbsWireCandidate> ordered_candidates = candidates;
    std::sort(ordered_candidates.begin(),
              ordered_candidates.end(),
              [](const RnicPrbsWireCandidate& lhs,
                 const RnicPrbsWireCandidate& rhs) {
                  return lhs.flow_id < rhs.flow_id;
              });

    WireRateBps total_wire_rate_bps = 0;
    for (size_t i = 0; i < ordered_candidates.size(); ++i) {
        const RnicPrbsWireCandidate& candidate = ordered_candidates[i];
        if (i > 0 && candidate.flow_id == ordered_candidates[i - 1].flow_id) {
            throw std::invalid_argument("duplicate RNIC PRBS candidate flow id");
        }
        if (candidate.wire_rate_bps
            > wire_capacity_bps - total_wire_rate_bps) {
            throw std::invalid_argument(
                "RNIC PRBS candidate wire rates exceed wire capacity");
        }
        total_wire_rate_bps += candidate.wire_rate_bps;
    }

    // Approximate the size-normalized event hazards in adaptive fixed point:
    //   a_i = floor(2^q * r_i * M / l_i)
    //   a_idle = 2^q * (C - sum(r_i)).
    // q is the largest value at most 32 whose weights and sum fit in 128 bits.
    // Therefore each represented per-event hazard differs from r_i*M/l_i by
    // less than 2^-q. q=0 is reserved for the extreme uint64 domain where no
    // fractional scale fits; ordinary configurations use Q32.
    std::vector<WeightedCandidate> weighted_candidates;
    Wide total_weight = 0;
    bool weights_built = false;
    for (int fraction_bits = kPreferredHazardFractionBits;
         fraction_bits >= 0;
         --fraction_bits) {
        if (buildScaledHazards(
                ordered_candidates,
                wire_capacity_bps - total_wire_rate_bps,
                idle_wire_bytes,
                static_cast<unsigned>(fraction_bits),
                &weighted_candidates,
                &total_weight)) {
            weights_built = true;
            break;
        }
    }
    if (!weights_built) {
        throw std::overflow_error(
            "RNIC PRBS variable-size fixed-point weight overflow");
    }

    const uint64_t draw = nextPrbsWord();
    Wide cumulative_weight = 0;
    for (const WeightedCandidate& candidate : weighted_candidates) {
        cumulative_weight += candidate.weight;
        if (binaryFractionBelowRatio(draw,
                                     cumulative_weight,
                                     total_weight)) {
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
    // Every nonzero order-64 output block occurs once per maximal sequence.
    // Striding by 64 visits all phases because gcd(64, 2^64 - 1) is one.
    // Rejection over that exact nonzero population makes (word - 1) modulo
    // the bound unbiased.
    const uint64_t population = std::numeric_limits<uint64_t>::max();
    const uint64_t accepted_population =
        population - (population % exclusive_upper_bound);

    uint64_t word;
    do {
        word = nextPrbsWord();
    } while (word > accepted_population);
    return (word - 1) % exclusive_upper_bound;
}
