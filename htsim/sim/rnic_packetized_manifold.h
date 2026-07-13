// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PACKETIZED_MANIFOLD_H
#define RNIC_PACKETIZED_MANIFOLD_H

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "rnic_max_min_allocator.h"

struct RnicPacketizedGrant {
    using FlowId = RnicMaxMinFlow::FlowId;
    using NodeId = RnicMaxMinFlow::NodeId;
    using RateBps = RnicMaxMinFlow::RateBps;

    FlowId flow_id;
    NodeId source_node;
    NodeId destination_node;
    RateBps rate_bps;
};

class RnicPacketizedSlotCalendar;

// An immutable value describing one full-wire-quantum reservation. The null
// manifold adds no route, internal queue, loss, marking, or backpressure.
class RnicPacketizedReservation {
public:
    using FlowId = RnicPacketizedGrant::FlowId;
    using NodeId = RnicPacketizedGrant::NodeId;
    using TimePs = uint64_t;

    uint64_t slotIndex() const noexcept { return _slot_index; }
    uint64_t allocationEpoch() const noexcept { return _allocation_epoch; }
    FlowId flowId() const noexcept { return _flow_id; }
    NodeId sourceNode() const noexcept { return _source_node; }
    NodeId destinationNode() const noexcept { return _destination_node; }

    TimePs sourceSlotStartPs() const noexcept { return _source_slot_start_ps; }
    TimePs sourceSlotEndPs() const noexcept { return _source_slot_end_ps; }
    TimePs manifoldEntryPs() const noexcept { return _manifold_entry_ps; }
    TimePs manifoldExitPs() const noexcept { return _manifold_exit_ps; }
    TimePs destinationSlotStartPs() const noexcept {
        return _destination_slot_start_ps;
    }
    TimePs destinationSlotEndPs() const noexcept { return _destination_slot_end_ps; }
    uint64_t chargedWireBytes() const noexcept { return _charged_wire_bytes; }

private:
    friend class RnicPacketizedSlotCalendar;

    RnicPacketizedReservation(uint64_t slot_index,
                              uint64_t allocation_epoch,
                              FlowId flow_id,
                              NodeId source_node,
                              NodeId destination_node,
                              TimePs source_slot_start_ps,
                              TimePs source_slot_end_ps,
                              TimePs manifold_entry_ps,
                              TimePs manifold_exit_ps,
                              TimePs destination_slot_start_ps,
                              TimePs destination_slot_end_ps,
                              uint64_t charged_wire_bytes);

    uint64_t _slot_index;
    uint64_t _allocation_epoch;
    FlowId _flow_id;
    NodeId _source_node;
    NodeId _destination_node;
    TimePs _source_slot_start_ps;
    TimePs _source_slot_end_ps;
    TimePs _manifold_entry_ps;
    TimePs _manifold_exit_ps;
    TimePs _destination_slot_start_ps;
    TimePs _destination_slot_end_ps;
    uint64_t _charged_wire_bytes;
};

// Deterministic version-1 packet calendar for the validated homogeneous-C,
// fixed-wire-quantum null-network scope. A caller supplies a complete active,
// backlogged grant snapshot at each epoch. A packetization wrapper must handle
// a short final packet with its exact serialization time; this calendar accepts
// and charges only complete wire quanta and makes no flow-completion claim.
class RnicPacketizedSlotCalendar {
public:
    using FlowId = RnicPacketizedGrant::FlowId;
    using NodeId = RnicPacketizedGrant::NodeId;
    using RateBps = RnicPacketizedGrant::RateBps;
    using TimePs = RnicPacketizedReservation::TimePs;

    static constexpr uint32_t kSchedulerVersion = 1;

    RnicPacketizedSlotCalendar(RateBps access_capacity_bps,
                               uint64_t wire_quantum_bytes,
                               TimePs fixed_propagation_delay_ps,
                               TimePs first_slot_start_ps = 0);

    // Replaces the complete grant snapshot at the next unreserved slot. Credit
    // is preserved for surviving flow IDs with unchanged endpoints; new flows
    // start with zero debt/credit and are eligible in their first positive-rate
    // slot, bounding startup service lead to less than one wire quantum.
    void beginEpoch(uint64_t effective_slot,
                    const std::vector<RnicPacketizedGrant>& grants);

    // Convenience entry point using the shared progressive max-min allocator
    // and homogeneous source/destination capacities.
    void beginMaxMinEpoch(uint64_t effective_slot,
                          const std::vector<RnicMaxMinFlow>& active_flows);

    // Adds one grant quantum to each signed credit, selects a deterministic
    // maximum-cardinality matching over positive-credit flows, and reserves
    // one non-overlapping source/destination slot for every selected edge.
    std::vector<RnicPacketizedReservation> reserveNextSlot();

    RateBps accessCapacityBps() const noexcept { return _access_capacity_bps; }
    uint64_t wireQuantumBytes() const noexcept { return _wire_quantum_bytes; }
    TimePs fixedPropagationDelayPs() const noexcept {
        return _fixed_propagation_delay_ps;
    }
    uint64_t nextSlotIndex() const noexcept { return _next_slot_index; }
    TimePs nextSlotStartPs() const noexcept { return _next_slot_start_ps; }
    uint64_t allocationEpoch() const noexcept { return _allocation_epoch; }

private:
    struct SignedCredit {
        bool negative = false;
        uint64_t magnitude = 0;
    };

    struct FlowState {
        RnicPacketizedGrant grant;
        SignedCredit credit;
    };

    struct BoundaryState {
        TimePs floor_ps = 0;
        uint64_t remainder = 0;
    };

    using Adjacency = std::map<NodeId, std::vector<FlowId>>;
    using DestinationMatching = std::map<NodeId, FlowId>;

    static void addCredit(SignedCredit& credit, uint64_t amount);
    static void subtractCredit(SignedCredit& credit, uint64_t amount);
    static int compareCredit(const SignedCredit& lhs, const SignedCredit& rhs) noexcept;
    static bool hasPositiveCredit(const SignedCredit& credit) noexcept;

    BoundaryState advanceBoundary(BoundaryState boundary) const;
    TimePs absoluteBoundary(const BoundaryState& boundary) const;
    std::vector<FlowId> maximumCardinalityMatching() const;
    bool augmentSource(NodeId source,
                       const Adjacency& adjacency,
                       std::map<NodeId, bool>& visited_destinations,
                       DestinationMatching& matching) const;
    void validateGrantSnapshot(const std::vector<RnicPacketizedGrant>& grants) const;
    void advanceCredits();

    RateBps _access_capacity_bps;
    uint64_t _wire_quantum_bytes;
    TimePs _fixed_propagation_delay_ps;
    TimePs _first_slot_start_ps;
    uint64_t _serialization_numerator;
    TimePs _boundary_floor_increment_ps;
    uint64_t _boundary_remainder_increment;
    BoundaryState _next_boundary;
    uint64_t _next_slot_index = 0;
    TimePs _next_slot_start_ps;
    uint64_t _allocation_epoch = 0;
    std::map<FlowId, FlowState> _flows;
    std::map<FlowId, std::pair<NodeId, NodeId>> _known_endpoints;
};

#endif
