// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_packetized_manifold.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace {

constexpr uint64_t kSerializationNumeratorPerByte = UINT64_C(8000000000000);

uint64_t checkedAdd(uint64_t lhs, uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

}  // namespace

RnicPacketizedReservation::RnicPacketizedReservation(
        uint64_t slot_index,
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
        uint64_t charged_wire_bytes)
    : _slot_index(slot_index),
      _allocation_epoch(allocation_epoch),
      _flow_id(flow_id),
      _source_node(source_node),
      _destination_node(destination_node),
      _source_slot_start_ps(source_slot_start_ps),
      _source_slot_end_ps(source_slot_end_ps),
      _manifold_entry_ps(manifold_entry_ps),
      _manifold_exit_ps(manifold_exit_ps),
      _destination_slot_start_ps(destination_slot_start_ps),
      _destination_slot_end_ps(destination_slot_end_ps),
      _charged_wire_bytes(charged_wire_bytes) {}

RnicPacketizedSlotCalendar::RnicPacketizedSlotCalendar(
        RateBps access_capacity_bps,
        uint64_t wire_quantum_bytes,
        TimePs fixed_propagation_delay_ps,
        TimePs first_slot_start_ps)
    : _access_capacity_bps(access_capacity_bps),
      _wire_quantum_bytes(wire_quantum_bytes),
      _fixed_propagation_delay_ps(fixed_propagation_delay_ps),
      _first_slot_start_ps(first_slot_start_ps),
      _serialization_numerator(0),
      _boundary_floor_increment_ps(0),
      _boundary_remainder_increment(0),
      _next_slot_start_ps(first_slot_start_ps) {
    if (access_capacity_bps == 0) {
        throw std::invalid_argument("packetized manifold access capacity must be nonzero");
    }
    if (wire_quantum_bytes == 0) {
        throw std::invalid_argument("packetized manifold wire quantum must be nonzero");
    }
    if (wire_quantum_bytes
        > std::numeric_limits<uint64_t>::max() / kSerializationNumeratorPerByte) {
        throw std::overflow_error("packetized manifold serialization numerator overflow");
    }

    _serialization_numerator = wire_quantum_bytes * kSerializationNumeratorPerByte;
    if (_serialization_numerator < access_capacity_bps) {
        throw std::invalid_argument(
            "packetized manifold wire slot is shorter than one simulator tick");
    }
    _boundary_floor_increment_ps = _serialization_numerator / access_capacity_bps;
    _boundary_remainder_increment = _serialization_numerator % access_capacity_bps;

    const BoundaryState first_end = advanceBoundary(_next_boundary);
    if (absoluteBoundary(first_end) == first_slot_start_ps) {
        throw std::invalid_argument("packetized manifold wire slot rounds to zero time");
    }
}

void RnicPacketizedSlotCalendar::beginEpoch(
        uint64_t effective_slot,
        const std::vector<RnicPacketizedGrant>& grants) {
    if (effective_slot != _next_slot_index) {
        throw std::invalid_argument(
            "packetized manifold epoch must begin at the next unreserved slot");
    }
    validateGrantSnapshot(grants);

    std::map<FlowId, FlowState> next_flows;
    for (const RnicPacketizedGrant& grant : grants) {
        SignedCredit credit;
        const auto existing = _flows.find(grant.flow_id);
        if (existing != _flows.end()) {
            if (existing->second.grant.source_node != grant.source_node
                || existing->second.grant.destination_node != grant.destination_node) {
                throw std::invalid_argument(
                    "packetized manifold cannot mutate endpoints for a surviving flow");
            }
            credit = existing->second.credit;
        }

        const auto known = _known_endpoints.find(grant.flow_id);
        if (known != _known_endpoints.end()
            && (known->second.first != grant.source_node
                || known->second.second != grant.destination_node)) {
            throw std::invalid_argument(
                "packetized manifold flow id cannot be reused with different endpoints");
        }
        next_flows.emplace(grant.flow_id, FlowState{grant, credit});
    }

    if (_allocation_epoch == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("packetized manifold allocation epoch overflow");
    }
    for (const RnicPacketizedGrant& grant : grants) {
        _known_endpoints.emplace(
            grant.flow_id, std::make_pair(grant.source_node, grant.destination_node));
    }
    _flows = std::move(next_flows);
    ++_allocation_epoch;
}

void RnicPacketizedSlotCalendar::beginMaxMinEpoch(
        uint64_t effective_slot,
        const std::vector<RnicMaxMinFlow>& active_flows) {
    RnicMaxMinAllocator::CapacityMap source_capacities;
    RnicMaxMinAllocator::CapacityMap destination_capacities;
    for (const RnicMaxMinFlow& flow : active_flows) {
        source_capacities.emplace(flow.source_node, _access_capacity_bps);
        destination_capacities.emplace(flow.destination_node, _access_capacity_bps);
    }

    const RnicMaxMinAllocator::AllocationMap allocation =
        RnicMaxMinAllocator::allocate(
            active_flows, source_capacities, destination_capacities);
    std::vector<RnicPacketizedGrant> grants;
    grants.reserve(active_flows.size());
    for (const RnicMaxMinFlow& flow : active_flows) {
        grants.push_back({flow.flow_id,
                          flow.source_node,
                          flow.destination_node,
                          allocation.at(flow.flow_id)});
    }
    beginEpoch(effective_slot, grants);
}

std::vector<RnicPacketizedReservation>
RnicPacketizedSlotCalendar::reserveNextSlot() {
    if (_next_slot_index == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("packetized manifold slot index overflow");
    }
    const BoundaryState source_end_boundary = advanceBoundary(_next_boundary);
    const BoundaryState destination_end_boundary = advanceBoundary(source_end_boundary);
    const TimePs source_start_ps = absoluteBoundary(_next_boundary);
    const TimePs source_end_ps = absoluteBoundary(source_end_boundary);
    const TimePs destination_unshifted_end_ps =
        absoluteBoundary(destination_end_boundary);
    const TimePs manifold_entry_ps = source_end_ps;
    const TimePs manifold_exit_ps = checkedAdd(
        manifold_entry_ps,
        _fixed_propagation_delay_ps,
        "packetized manifold exit time overflow");
    const TimePs destination_start_ps = manifold_exit_ps;
    const TimePs destination_end_ps = checkedAdd(
        destination_unshifted_end_ps,
        _fixed_propagation_delay_ps,
        "packetized manifold destination time overflow");

    advanceCredits();
    const std::vector<FlowId> selected_flows = maximumCardinalityMatching();
    std::vector<RnicPacketizedReservation> reservations;
    reservations.reserve(selected_flows.size());
    for (const FlowId flow_id : selected_flows) {
        FlowState& state = _flows.at(flow_id);
        subtractCredit(state.credit, _access_capacity_bps);
        reservations.push_back(RnicPacketizedReservation{
            _next_slot_index,
            _allocation_epoch,
            state.grant.flow_id,
            state.grant.source_node,
            state.grant.destination_node,
            source_start_ps,
            source_end_ps,
            manifold_entry_ps,
            manifold_exit_ps,
            destination_start_ps,
            destination_end_ps,
            _wire_quantum_bytes});
    }

    _next_boundary = source_end_boundary;
    _next_slot_start_ps = source_end_ps;
    ++_next_slot_index;
    return reservations;
}

void RnicPacketizedSlotCalendar::addCredit(SignedCredit& credit, uint64_t amount) {
    if (credit.negative) {
        if (amount >= credit.magnitude) {
            credit.magnitude = amount - credit.magnitude;
            credit.negative = false;
        } else {
            credit.magnitude -= amount;
        }
        return;
    }
    credit.magnitude = checkedAdd(
        credit.magnitude, amount, "packetized manifold positive credit overflow");
}

void RnicPacketizedSlotCalendar::subtractCredit(
        SignedCredit& credit, uint64_t amount) {
    if (credit.negative) {
        credit.magnitude = checkedAdd(
            credit.magnitude, amount, "packetized manifold negative credit overflow");
        return;
    }
    if (credit.magnitude >= amount) {
        credit.magnitude -= amount;
    } else {
        credit.magnitude = amount - credit.magnitude;
        credit.negative = true;
    }
}

int RnicPacketizedSlotCalendar::compareCredit(
        const SignedCredit& lhs, const SignedCredit& rhs) noexcept {
    if (lhs.negative != rhs.negative) {
        return lhs.negative ? -1 : 1;
    }
    if (lhs.magnitude == rhs.magnitude) {
        return 0;
    }
    if (lhs.negative) {
        return lhs.magnitude < rhs.magnitude ? 1 : -1;
    }
    return lhs.magnitude > rhs.magnitude ? 1 : -1;
}

bool RnicPacketizedSlotCalendar::hasPositiveCredit(
        const SignedCredit& credit) noexcept {
    return !credit.negative && credit.magnitude > 0;
}

RnicPacketizedSlotCalendar::BoundaryState
RnicPacketizedSlotCalendar::advanceBoundary(BoundaryState boundary) const {
    boundary.floor_ps = checkedAdd(
        boundary.floor_ps,
        _boundary_floor_increment_ps,
        "packetized manifold rational boundary overflow");
    if (_boundary_remainder_increment != 0) {
        const uint64_t carry_threshold =
            _access_capacity_bps - _boundary_remainder_increment;
        if (boundary.remainder >= carry_threshold) {
            boundary.remainder -= carry_threshold;
            boundary.floor_ps = checkedAdd(
                boundary.floor_ps,
                1,
                "packetized manifold rational boundary overflow");
        } else {
            boundary.remainder += _boundary_remainder_increment;
        }
    }
    return boundary;
}

RnicPacketizedSlotCalendar::TimePs RnicPacketizedSlotCalendar::absoluteBoundary(
        const BoundaryState& boundary) const {
    const TimePs rounded_offset = checkedAdd(
        boundary.floor_ps,
        boundary.remainder == 0 ? 0 : 1,
        "packetized manifold rational boundary overflow");
    return checkedAdd(
        _first_slot_start_ps,
        rounded_offset,
        "packetized manifold absolute boundary overflow");
}

std::vector<RnicPacketizedSlotCalendar::FlowId>
RnicPacketizedSlotCalendar::maximumCardinalityMatching() const {
    Adjacency adjacency;
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.grant.rate_bps > 0 && hasPositiveCredit(state.credit)) {
            adjacency[state.grant.source_node].push_back(state.grant.flow_id);
        }
    }

    const auto flow_priority = [this](FlowId lhs, FlowId rhs) {
        const int comparison = compareCredit(_flows.at(lhs).credit, _flows.at(rhs).credit);
        return comparison == 0 ? lhs < rhs : comparison > 0;
    };
    for (auto& item : adjacency) {
        std::sort(item.second.begin(), item.second.end(), flow_priority);
    }

    std::vector<NodeId> sources;
    sources.reserve(adjacency.size());
    for (const auto& item : adjacency) {
        sources.push_back(item.first);
    }
    std::sort(sources.begin(), sources.end(), [&](NodeId lhs, NodeId rhs) {
        const FlowId lhs_first = adjacency.at(lhs).front();
        const FlowId rhs_first = adjacency.at(rhs).front();
        if (flow_priority(lhs_first, rhs_first)) {
            return true;
        }
        if (flow_priority(rhs_first, lhs_first)) {
            return false;
        }
        return lhs < rhs;
    });

    DestinationMatching matching;
    for (const NodeId source : sources) {
        std::map<NodeId, bool> visited_destinations;
        augmentSource(source, adjacency, visited_destinations, matching);
    }

    std::vector<FlowId> selected;
    selected.reserve(matching.size());
    for (const auto& item : matching) {
        selected.push_back(item.second);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
}

bool RnicPacketizedSlotCalendar::augmentSource(
        NodeId source,
        const Adjacency& adjacency,
        std::map<NodeId, bool>& visited_destinations,
        DestinationMatching& matching) const {
    const auto edges = adjacency.find(source);
    if (edges == adjacency.end()) {
        return false;
    }

    for (const FlowId flow_id : edges->second) {
        const NodeId destination = _flows.at(flow_id).grant.destination_node;
        if (visited_destinations[destination]) {
            continue;
        }
        visited_destinations[destination] = true;

        const auto existing = matching.find(destination);
        if (existing == matching.end()
            || augmentSource(_flows.at(existing->second).grant.source_node,
                             adjacency,
                             visited_destinations,
                             matching)) {
            matching[destination] = flow_id;
            return true;
        }
    }
    return false;
}

void RnicPacketizedSlotCalendar::validateGrantSnapshot(
        const std::vector<RnicPacketizedGrant>& grants) const {
    std::set<FlowId> flow_ids;
    std::map<NodeId, RateBps> rate_by_source;
    std::map<NodeId, RateBps> rate_by_destination;
    for (const RnicPacketizedGrant& grant : grants) {
        if (!flow_ids.insert(grant.flow_id).second) {
            throw std::invalid_argument("duplicate packetized manifold grant flow id");
        }

        RateBps& source_rate = rate_by_source[grant.source_node];
        RateBps& destination_rate = rate_by_destination[grant.destination_node];
        if (grant.rate_bps > _access_capacity_bps - source_rate
            || grant.rate_bps > _access_capacity_bps - destination_rate) {
            throw std::invalid_argument(
                "packetized manifold grant snapshot exceeds endpoint capacity");
        }
        source_rate += grant.rate_bps;
        destination_rate += grant.rate_bps;
    }
}

void RnicPacketizedSlotCalendar::advanceCredits() {
    for (auto& item : _flows) {
        addCredit(item.second.credit, item.second.grant.rate_bps);
    }
}
