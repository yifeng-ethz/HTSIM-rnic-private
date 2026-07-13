// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PRBS_PACER_H
#define RNIC_PRBS_PACER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct RnicPrbsCandidate {
    using FlowId = uint64_t;
    using WireRateBps = uint64_t;

    FlowId flow_id;
    WireRateBps wire_rate_bps;
};

struct RnicPrbsWireCandidate {
    using FlowId = RnicPrbsCandidate::FlowId;
    using WireRateBps = RnicPrbsCandidate::WireRateBps;

    FlowId flow_id;
    WireRateBps wire_rate_bps;
    uint64_t head_wire_bytes;
};

struct RnicPrbsManifest {
    std::string algorithm_name;
    uint32_t algorithm_version;
    std::string polynomial;
    uint64_t feedback_mask;
    std::string seed_derivation;
    uint64_t global_seed;
    uint64_t node_id;
    uint64_t derived_node_seed;
};

// A node-scoped packet-opportunity selector for RnicTxPort. Each call selects
// at most one flow head for the next access-link wire opportunity.
class RnicPrbsPacer {
public:
    using FlowId = RnicPrbsCandidate::FlowId;
    using WireRateBps = RnicPrbsCandidate::WireRateBps;

    // This right-shifting Galois LFSR uses the primitive polynomial
    // x^64 + x^63 + x^61 + x^60 + 1 and has period 2^64 - 1. Its nonzero
    // state advances as s' = (s >> 1) ^ (-(s & 1) & kFeedbackMask).
    static constexpr uint64_t kFeedbackMask = UINT64_C(0xd800000000000000);
    static constexpr uint32_t kAlgorithmVersion = 1;

    RnicPrbsPacer(uint64_t global_seed, uint64_t node_id);

    // Returns the post-step nonzero LFSR state. This method is exposed so
    // deterministic manifests and replay tests can identify the exact
    // generator convention; zero is forbidden because it is absorbing.
    uint64_t nextPrbsWord();

    // Candidates must represent equal serialized wire quanta. Selection
    // probability is wire_rate_bps / wire_capacity_bps; unused capacity is an
    // idle outcome. RnicTxPort uses selectWireEvent() when a short final packet
    // or another variable-size wire quantum is present.
    std::optional<FlowId> selectEqualWireQuantum(
        const std::vector<RnicPrbsCandidate>& candidates,
        WireRateBps wire_capacity_bps);

    // Selects one variable-size wire event. idle_wire_bytes is the wire extent
    // whose serialization defines an idle outcome. If every head has that
    // same size, this delegates verbatim to selectEqualWireQuantum() to
    // preserve replay.
    // Otherwise adaptive fixed-point hazards approximate r_i/l_i and the idle
    // remainder divided by idle_wire_bytes; no post-selection gap is added.
    std::optional<FlowId> selectWireEvent(
        const std::vector<RnicPrbsWireCandidate>& candidates,
        WireRateBps wire_capacity_bps,
        uint64_t idle_wire_bytes);

    const RnicPrbsManifest& manifest() const noexcept;
    uint64_t state() const noexcept;

private:
    static uint64_t deriveNodeSeed(uint64_t global_seed, uint64_t node_id) noexcept;
    uint64_t nextBounded(uint64_t exclusive_upper_bound);

    RnicPrbsManifest _manifest;
    uint64_t _state;
};

#endif
