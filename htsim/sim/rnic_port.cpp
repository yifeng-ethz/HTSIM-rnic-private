// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_port.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

uint64_t checkedAdd(uint64_t lhs, uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

}  // namespace

RnicTxPort::RnicTxPort(uint64_t node_id,
                       uint64_t access_capacity_bps,
                       uint64_t max_wire_packet_bytes,
                       uint64_t global_prbs_seed)
    : RnicTxPort(node_id,
                 access_capacity_bps,
                 RnicDataPacketizationConfig(max_wire_packet_bytes),
                 global_prbs_seed) {}

RnicTxPort::RnicTxPort(uint64_t node_id,
                       uint64_t access_capacity_bps,
                       RnicDataPacketizationConfig packetization,
                       uint64_t global_prbs_seed)
    : _access_capacity_bps(access_capacity_bps),
      _packetization(std::move(packetization)),
      _wire_serializer(access_capacity_bps),
      _data_opportunity_serializer(access_capacity_bps),
      _pacer(global_prbs_seed, node_id) {}

void RnicTxPort::addFlow(
        uint64_t flow_id,
        uint64_t payload_size_bytes,
        uint64_t calibrated_transit_ps) {
    const FlowState state{flow_id, payload_size_bytes, calibrated_transit_ps};
    if (!_flows.emplace(flow_id, state).second) {
        throw std::invalid_argument("duplicate flow on RNIC TX port");
    }
    recomputeEffectiveRates();
}

void RnicTxPort::setWireRateGrant(
        uint64_t flow_id, uint64_t wire_rate_grant_bps) {
    requireFlow(flow_id).wire_rate_grant_bps = wire_rate_grant_bps;
    recomputeEffectiveRates();
}

void RnicTxPort::setDataEligible(uint64_t flow_id, bool eligible) {
    requireFlow(flow_id).data_eligible = eligible;
    recomputeEffectiveRates();
}

bool RnicTxPort::contains(uint64_t flow_id) const {
    return _flows.count(flow_id) != 0;
}

bool RnicTxPort::sourcePayloadDispatched(uint64_t flow_id) const {
    const FlowState& state = requireFlow(flow_id);
    return state.payload_bytes_dispatched == state.payload_size_bytes;
}

uint64_t RnicTxPort::flowPayloadBytesDispatched(uint64_t flow_id) const {
    return requireFlow(flow_id).payload_bytes_dispatched;
}

uint64_t RnicTxPort::effectiveWireRateBps(uint64_t flow_id) const {
    return requireFlow(flow_id).effective_wire_rate_bps;
}

bool RnicTxPort::hasDispatchableData() const {
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.data_eligible
            && state.payload_bytes_dispatched < state.payload_size_bytes
            && state.effective_wire_rate_bps > 0) {
            return true;
        }
    }
    return false;
}

uint64_t RnicTxPort::nextWireOpportunityPs() const {
    return std::max(nextDataOpportunityPs(), physicalSerializerAvailablePs());
}

RnicTxOpportunity RnicTxPort::dispatchOpportunity(uint64_t requested_start_ps) {
    if (requested_start_ps < nextWireOpportunityPs()) {
        throw std::invalid_argument("RNIC TX opportunities cannot overlap");
    }

    std::vector<RnicPrbsWireCandidate> candidates;
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.data_eligible
            && state.payload_bytes_dispatched < state.payload_size_bytes
            && state.effective_wire_rate_bps > 0) {
            candidates.push_back({state.flow_id,
                                  state.effective_wire_rate_bps,
                                  headExtent(state).wireBytes()});
        }
    }

    RnicPrbsPacer next_pacer = _pacer;
    const std::optional<uint64_t> selected =
        next_pacer.selectWireEvent(
            candidates,
            _access_capacity_bps,
            _packetization.maxWirePacketBytes());
    std::optional<RnicTxPacket> packet;
    uint64_t event_wire_bytes = _packetization.maxWirePacketBytes();
    FlowState* selected_state = nullptr;
    std::optional<RnicPacketExtent> selected_extent;
    std::optional<uint64_t> selected_eta_ps;
    if (selected.has_value()) {
        FlowState& state = requireFlow(*selected);
        if (state.packet_index == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("RNIC TX packet index overflow");
        }
        selected_state = &state;
        selected_extent = headExtent(state);
        event_wire_bytes = selected_extent->wireBytes();
        selected_eta_ps = checkedAdd(
            requested_start_ps,
            state.calibrated_transit_ps,
            "RNIC TX eligibility timestamp overflow");
    }

    RnicWireSerializationClock next_wire_serializer = _wire_serializer;
    RnicWireSerializationClock next_data_opportunity_serializer =
        _data_opportunity_serializer;
    RnicWireSerializationInterval interval;
    if (selected_state != nullptr) {
        next_wire_serializer.synchronizeAvailableWith(
            next_data_opportunity_serializer);
        interval = next_wire_serializer.serialize(
            requested_start_ps, event_wire_bytes);
        next_data_opportunity_serializer = next_wire_serializer;
    } else {
        // The PRBS idle outcome advances only the virtual opportunity clock.
        // The physical wire is known idle at this event boundary and remains
        // available to a later high-priority control arrival.
        next_wire_serializer.rebaseIdle(requested_start_ps);
        interval = next_data_opportunity_serializer.serialize(
            requested_start_ps, event_wire_bytes);
    }

    _pacer = next_pacer;
    _wire_serializer = next_wire_serializer;
    _data_opportunity_serializer = next_data_opportunity_serializer;
    if (selected_state != nullptr) {
        FlowState& state = *selected_state;
        packet = RnicTxPacket{state.flow_id,
                              state.packet_index,
                              state.payload_bytes_dispatched,
                              *selected_extent,
                              interval.start_ps,
                              interval.end_ps,
                              *selected_eta_ps};
        state.payload_bytes_dispatched += selected_extent->payloadBytes();
        ++state.packet_index;
        if (state.payload_bytes_dispatched == state.payload_size_bytes) {
            recomputeEffectiveRates();
        }
    }

    return {interval.start_ps, interval.end_ps, packet};
}

RnicWireSerializationInterval RnicTxPort::dispatchControl(
        uint64_t requested_start_ps, uint64_t wire_bytes) {
    if (requested_start_ps < physicalSerializerAvailablePs()) {
        throw std::invalid_argument("RNIC TX control frames cannot overlap");
    }
    RnicWireSerializationClock next_wire_serializer = _wire_serializer;
    const RnicWireSerializationInterval interval =
        next_wire_serializer.serialize(requested_start_ps, wire_bytes);
    _wire_serializer = next_wire_serializer;
    return interval;
}

void RnicTxPort::rebasePhysicalIdle(uint64_t now_ps) {
    _wire_serializer.rebaseIdle(now_ps);
}

RnicPacketExtent RnicTxPort::headExtent(const FlowState& state) const {
    if (state.payload_bytes_dispatched >= state.payload_size_bytes) {
        throw std::logic_error("RNIC TX completed flow has no DATA head");
    }
    return _packetization.packetize(
        state.payload_size_bytes - state.payload_bytes_dispatched);
}

void RnicTxPort::recomputeEffectiveRates() {
    std::vector<FlowState*> active;
    uint64_t allocated_bps = 0;
    for (auto& item : _flows) {
        FlowState& state = item.second;
        state.effective_wire_rate_bps = 0;
        if (state.data_eligible
            && state.payload_bytes_dispatched < state.payload_size_bytes
            && state.wire_rate_grant_bps > 0) {
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
            const uint64_t remaining_grant =
                state->wire_rate_grant_bps - state->effective_wire_rate_bps;
            if (remaining_grant <= fair_increment) {
                state->effective_wire_rate_bps += remaining_grant;
                allocated_bps += remaining_grant;
                froze_flow = true;
            } else {
                still_active.push_back(state);
            }
        }

        if (!froze_flow) {
            for (FlowState* state : active) {
                state->effective_wire_rate_bps += fair_increment;
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
    : _wire_serializer(access_capacity_bps), _ring_cam(ring_cam_config) {}

RnicRxArrivalResult RnicRxPort::processArrival(const RnicRingCamPacket& packet) {
    RnicRingCamArrivalResult result = _ring_cam.processArrival(packet);
    std::vector<RnicRxScheduledSerialization> scheduled =
        scheduleSerializations(result.released_before_admission);
    accountDeliveriesThrough(packet.arrival_ps);
    return {result.admission, result.logical_release_ps, std::move(scheduled)};
}

std::vector<RnicRxScheduledSerialization> RnicRxPort::advanceTo(uint64_t now_ps) {
    std::vector<RnicRxScheduledSerialization> scheduled =
        scheduleSerializations(_ring_cam.advanceTo(now_ps));
    accountDeliveriesThrough(now_ps);
    return scheduled;
}

uint64_t RnicRxPort::deliveredPayloadBytes(uint64_t flow_id) const {
    const auto delivered = _delivered_payload_bytes_by_flow.find(flow_id);
    return delivered == _delivered_payload_bytes_by_flow.end()
               ? 0
               : delivered->second;
}

uint64_t RnicRxPort::deliveredWireBytes(uint64_t flow_id) const {
    const auto delivered = _delivered_wire_bytes_by_flow.find(flow_id);
    return delivered == _delivered_wire_bytes_by_flow.end()
               ? 0
               : delivered->second;
}

std::vector<RnicRxScheduledSerialization> RnicRxPort::scheduleSerializations(
        const std::vector<RnicRingCamRelease>& logical_releases) {
    RnicWireSerializationClock next_serializer = _wire_serializer;
    uint64_t next_pending_wire_bytes = _pending_serializer_wire_bytes;
    uint64_t next_high_watermark =
        _pending_serializer_high_watermark_wire_bytes;

    std::vector<RnicRxScheduledSerialization> scheduled;
    scheduled.reserve(logical_releases.size());
    std::multimap<uint64_t, RnicRingCamPacket> pending_additions;
    for (const RnicRingCamRelease& release : logical_releases) {
        const RnicWireSerializationInterval interval =
            next_serializer.serialize(
                release.logical_release_ps, release.packet.extent.wireBytes());
        next_pending_wire_bytes = checkedAdd(
            next_pending_wire_bytes,
            release.packet.extent.wireBytes(),
            "RNIC RX pending serializer wire-byte counter overflow");
        next_high_watermark = std::max(
            next_high_watermark, next_pending_wire_bytes);
        scheduled.push_back({release, interval.start_ps, interval.end_ps});
        pending_additions.emplace(interval.end_ps, release.packet);
    }

    // All arithmetic and allocations complete before the live serializer
    // state changes. C++17 node merge then commits the prepared additions
    // without allocating, so a later extent cannot leave a partial batch.
    _pending_serializations.merge(pending_additions);
    _wire_serializer = next_serializer;
    _pending_serializer_wire_bytes = next_pending_wire_bytes;
    _pending_serializer_high_watermark_wire_bytes = next_high_watermark;
    return scheduled;
}

void RnicRxPort::accountDeliveriesThrough(uint64_t now_ps) {
    auto delivery = _pending_serializations.begin();
    while (delivery != _pending_serializations.end() && delivery->first <= now_ps) {
        const RnicRingCamPacket& packet = delivery->second;
        const uint64_t delivered_payload = deliveredPayloadBytes(packet.flow_id);
        const uint64_t delivered_wire = deliveredWireBytes(packet.flow_id);
        const uint64_t next_delivered_payload = checkedAdd(
            delivered_payload,
            packet.extent.payloadBytes(),
            "RNIC RX delivered-payload counter overflow");
        const uint64_t next_delivered_wire = checkedAdd(
            delivered_wire,
            packet.extent.wireBytes(),
            "RNIC RX delivered-wire counter overflow");
        if (packet.extent.wireBytes() > _pending_serializer_wire_bytes) {
            throw std::logic_error("RNIC RX pending serializer occupancy underflow");
        }
        _delivered_payload_bytes_by_flow[packet.flow_id] = next_delivered_payload;
        _delivered_wire_bytes_by_flow[packet.flow_id] = next_delivered_wire;
        _pending_serializer_wire_bytes -= packet.extent.wireBytes();
        delivery = _pending_serializations.erase(delivery);
    }
}
