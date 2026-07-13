// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_port.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

constexpr uint64_t kPicosecondsPerSecond = 1000000000000ULL;

uint64_t checkedAdd(uint64_t lhs, uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

uint64_t serializationTimePs(uint64_t bytes, uint64_t capacity_bps) {
    if (capacity_bps == 0) {
        throw std::invalid_argument("RNIC physical port capacity must be nonzero");
    }
    if (bytes > std::numeric_limits<uint64_t>::max() / 8) {
        throw std::overflow_error("RNIC packet bit count overflow");
    }
    const uint64_t bits = bytes * 8;
    if (bits > std::numeric_limits<uint64_t>::max() / kPicosecondsPerSecond) {
        // The simulator's packet sizes are many orders of magnitude smaller,
        // so rejecting this case is preferable to silently losing precision.
        throw std::overflow_error("RNIC serialization numerator overflow");
    }
    const uint64_t numerator = bits * kPicosecondsPerSecond;
    return numerator / capacity_bps + (numerator % capacity_bps != 0);
}

}  // namespace

RnicTxPort::RnicTxPort(uint64_t node_id,
                       uint64_t access_capacity_bps,
                       uint64_t wire_quantum_bytes,
                       uint64_t global_prbs_seed)
    : _access_capacity_bps(access_capacity_bps),
      _wire_quantum_bytes(wire_quantum_bytes),
      _wire_opportunity_duration_ps(
          serializationTimePs(wire_quantum_bytes, access_capacity_bps)),
      _pacer(global_prbs_seed, node_id) {
    if (wire_quantum_bytes == 0) {
        throw std::invalid_argument("RNIC TX wire quantum must be nonzero");
    }
}

void RnicTxPort::addFlow(
        uint64_t flow_id, uint64_t size_bytes, uint64_t calibrated_transit_ps) {
    const FlowState state{flow_id, size_bytes, calibrated_transit_ps};
    if (!_flows.emplace(flow_id, state).second) {
        throw std::invalid_argument("duplicate flow on RNIC TX port");
    }
    recomputeEffectiveRates();
}

void RnicTxPort::setGrant(uint64_t flow_id, uint64_t grant_bps) {
    requireFlow(flow_id).grant_bps = grant_bps;
    recomputeEffectiveRates();
}

void RnicTxPort::setDataEligible(uint64_t flow_id, bool eligible) {
    requireFlow(flow_id).data_eligible = eligible;
    recomputeEffectiveRates();
}

bool RnicTxPort::contains(uint64_t flow_id) const {
    return _flows.count(flow_id) != 0;
}

bool RnicTxPort::flowComplete(uint64_t flow_id) const {
    const FlowState& state = requireFlow(flow_id);
    return state.bytes_dispatched == state.size_bytes;
}

uint64_t RnicTxPort::flowBytesDispatched(uint64_t flow_id) const {
    return requireFlow(flow_id).bytes_dispatched;
}

uint64_t RnicTxPort::effectiveRateBps(uint64_t flow_id) const {
    return requireFlow(flow_id).effective_rate_bps;
}

RnicTxOpportunity RnicTxPort::dispatchOpportunity(uint64_t requested_start_ps) {
    if (requested_start_ps < _next_wire_opportunity_ps) {
        throw std::invalid_argument("RNIC TX opportunities cannot overlap");
    }
    const uint64_t end_ps = checkedAdd(
        requested_start_ps,
        _wire_opportunity_duration_ps,
        "RNIC TX opportunity time overflow");

    std::vector<RnicPrbsCandidate> candidates;
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.data_eligible && state.bytes_dispatched < state.size_bytes
            && state.effective_rate_bps > 0) {
            candidates.push_back({state.flow_id, state.effective_rate_bps});
        }
    }

    const std::optional<uint64_t> selected =
        _pacer.selectEqualWireQuantum(candidates, _access_capacity_bps);
    std::optional<RnicTxPacket> packet;
    if (selected.has_value()) {
        FlowState& state = requireFlow(*selected);
        const uint64_t remaining = state.size_bytes - state.bytes_dispatched;
        const uint64_t payload_bytes = std::min(remaining, _wire_quantum_bytes);
        const uint64_t eta_ps = checkedAdd(
            requested_start_ps,
            state.calibrated_transit_ps,
            "RNIC TX eligibility timestamp overflow");
        packet = RnicTxPacket{state.flow_id,
                              state.packet_index,
                              state.bytes_dispatched,
                              payload_bytes,
                              _wire_quantum_bytes,
                              requested_start_ps,
                              end_ps,
                              eta_ps};
        state.bytes_dispatched += payload_bytes;
        ++state.packet_index;
        if (state.bytes_dispatched == state.size_bytes) {
            recomputeEffectiveRates();
        }
    }

    _next_wire_opportunity_ps = end_ps;
    return {requested_start_ps, end_ps, packet};
}

void RnicTxPort::recomputeEffectiveRates() {
    std::vector<FlowState*> active;
    uint64_t allocated_bps = 0;
    for (auto& item : _flows) {
        FlowState& state = item.second;
        state.effective_rate_bps = 0;
        if (state.data_eligible && state.bytes_dispatched < state.size_bytes
            && state.grant_bps > 0) {
            active.push_back(&state);
        }
    }

    // Progressive filling with the receiver grants acting as per-flow demand
    // caps. This is the local physical-NIC arbitration used only when several
    // independent receiver grants oversubscribe one source access link.
    while (!active.empty() && allocated_bps < _access_capacity_bps) {
        const uint64_t remaining_capacity = _access_capacity_bps - allocated_bps;
        const uint64_t fair_increment = remaining_capacity / active.size();
        if (fair_increment == 0) {
            break;
        }

        bool froze_flow = false;
        std::vector<FlowState*> still_active;
        still_active.reserve(active.size());
        for (FlowState* state : active) {
            const uint64_t remaining_grant = state->grant_bps - state->effective_rate_bps;
            if (remaining_grant <= fair_increment) {
                state->effective_rate_bps += remaining_grant;
                allocated_bps += remaining_grant;
                froze_flow = true;
            } else {
                still_active.push_back(state);
            }
        }

        if (!froze_flow) {
            for (FlowState* state : active) {
                state->effective_rate_bps += fair_increment;
                allocated_bps += fair_increment;
            }
            break;
        }
        active = std::move(still_active);
    }
}

RnicTxPort::FlowState& RnicTxPort::requireFlow(uint64_t flow_id) {
    const auto flow = _flows.find(flow_id);
    if (flow == _flows.end()) {
        throw std::out_of_range("unknown flow on RNIC TX port");
    }
    return flow->second;
}

const RnicTxPort::FlowState& RnicTxPort::requireFlow(uint64_t flow_id) const {
    const auto flow = _flows.find(flow_id);
    if (flow == _flows.end()) {
        throw std::out_of_range("unknown flow on RNIC TX port");
    }
    return flow->second;
}

RnicRxPort::RnicRxPort(
        uint64_t access_capacity_bps, RnicRingCamConfig ring_cam_config)
    : _access_capacity_bps(access_capacity_bps), _ring_cam(ring_cam_config) {
    if (access_capacity_bps == 0) {
        throw std::invalid_argument("RNIC RX access capacity must be nonzero");
    }
}

RnicRxArrivalResult RnicRxPort::processArrival(const RnicRingCamPacket& packet) {
    RnicRingCamArrivalResult result = _ring_cam.processArrival(packet);
    std::vector<RnicRxDelivery> deliveries = serialize(result.released_before_admission);
    accountDeliveriesThrough(packet.arrival_ps);
    return {result.admission, result.logical_release_ps, std::move(deliveries)};
}

std::vector<RnicRxDelivery> RnicRxPort::advanceTo(uint64_t now_ps) {
    std::vector<RnicRxDelivery> deliveries = serialize(_ring_cam.advanceTo(now_ps));
    accountDeliveriesThrough(now_ps);
    return deliveries;
}

uint64_t RnicRxPort::deliveredBytes(uint64_t flow_id) const {
    const auto delivered = _delivered_bytes_by_flow.find(flow_id);
    return delivered == _delivered_bytes_by_flow.end() ? 0 : delivered->second;
}

std::vector<RnicRxDelivery> RnicRxPort::serialize(
        const std::vector<RnicRingCamRelease>& logical_releases) {
    std::vector<RnicRxDelivery> deliveries;
    deliveries.reserve(logical_releases.size());
    for (const RnicRingCamRelease& release : logical_releases) {
        const uint64_t start_ps = std::max(
            _serializer_available_ps, release.logical_release_ps);
        const uint64_t end_ps = checkedAdd(
            start_ps,
            serializationTimePs(release.packet.wire_bytes, _access_capacity_bps),
            "RNIC RX serializer time overflow");
        _serializer_available_ps = end_ps;

        _pending_deliveries.emplace(end_ps, release.packet);
        deliveries.push_back({release, start_ps, end_ps});
    }
    return deliveries;
}

void RnicRxPort::accountDeliveriesThrough(uint64_t now_ps) {
    auto delivery = _pending_deliveries.begin();
    while (delivery != _pending_deliveries.end() && delivery->first <= now_ps) {
        const RnicRingCamPacket& packet = delivery->second;
        uint64_t& delivered = _delivered_bytes_by_flow[packet.flow_id];
        delivered = checkedAdd(
            delivered, packet.wire_bytes, "RNIC RX delivered-byte counter overflow");
        delivery = _pending_deliveries.erase(delivery);
    }
}
