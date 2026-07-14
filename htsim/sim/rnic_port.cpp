// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_port.h"

#include <algorithm>
#include <iterator>
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

void RnicTxPort::addFlow(uint64_t flow_id,
                         uint64_t payload_size_bytes,
                         TransitCalibration calibrated_transit_ps) {
    if (!calibrated_transit_ps) {
        throw std::invalid_argument("RNIC TX flow requires a transit calibration");
    }
    FlowState state{flow_id, payload_size_bytes, std::move(calibrated_transit_ps)};
    if (!_flows.emplace(flow_id, state).second) {
        throw std::invalid_argument("duplicate flow on RNIC TX port");
    }
    recomputeEffectiveRates();
}

void RnicTxPort::addFlow(uint64_t flow_id,
                         uint64_t payload_size_bytes,
                         uint64_t calibrated_transit_ps) {
    addFlow(flow_id, payload_size_bytes,
            [calibrated_transit_ps](const RnicPacketExtent&) { return calibrated_transit_ps; });
}

void RnicTxPort::setWireRateGrant(uint64_t flow_id, uint64_t wire_rate_grant_bps) {
    requireFlow(flow_id).wire_rate_grant_bps = wire_rate_grant_bps;
    recomputeEffectiveRates();
}

void RnicTxPort::setDataEligible(uint64_t flow_id, bool eligible) {
    requireFlow(flow_id).data_eligible = eligible;
    recomputeEffectiveRates();
}

void RnicTxPort::setSelectiveRepairPending(uint64_t flow_id, bool pending) {
    FlowState& state = requireFlow(flow_id);
    state.selective_repair_pending = pending;
    recomputeEffectiveRates();
}

void RnicTxPort::removeRetiredFlow(uint64_t flow_id) {
    const auto flow = _flows.find(flow_id);
    if (flow == _flows.end()) {
        throw std::out_of_range("unknown flow on RNIC TX port");
    }

    const FlowState& state = flow->second;
    if (state.payload_bytes_dispatched != state.payload_size_bytes) {
        throw std::logic_error("cannot remove RNIC TX flow before source payload dispatch");
    }
    if (state.data_eligible) {
        throw std::logic_error("cannot remove RNIC TX flow while DATA remains eligible");
    }
    if (state.selective_repair_pending) {
        throw std::logic_error("cannot remove RNIC TX flow with a pending selective repair");
    }
    if (state.wire_rate_grant_bps != 0) {
        throw std::logic_error("cannot remove RNIC TX flow with a nonzero wire-rate grant");
    }
    if (state.effective_wire_rate_bps != 0) {
        throw std::logic_error("retired RNIC TX flow retains an effective wire rate");
    }

    // All validation precedes the only mutation.  The proven terminal state
    // is excluded from recomputeEffectiveRates()'s active set, so erasing it
    // preserves every remaining flow's already-current effective allocation.
    _flows.erase(flow);
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

bool RnicTxPort::hasSelectiveRepairPending(uint64_t flow_id) const {
    return requireFlow(flow_id).selective_repair_pending;
}

bool RnicTxPort::hasDispatchableData() const {
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.data_eligible &&
            (state.payload_bytes_dispatched < state.payload_size_bytes ||
             state.selective_repair_pending) &&
            state.effective_wire_rate_bps > 0) {
            return true;
        }
    }
    return false;
}

uint64_t RnicTxPort::nextWireOpportunityPs() const {
    return std::max(nextDataOpportunityPs(), physicalSerializerAvailablePs());
}

RnicTxOpportunity RnicTxPort::dispatchOpportunity(uint64_t requested_start_ps) {
    return dispatchOpportunity(requested_start_ps, {});
}

RnicTxOpportunity RnicTxPort::dispatchOpportunity(
    uint64_t requested_start_ps,
    const std::vector<RnicTxRepairCandidate>& repair_heads) {
    if (requested_start_ps < nextWireOpportunityPs()) {
        throw std::invalid_argument("RNIC TX opportunities cannot overlap");
    }

    std::map<uint64_t, const RnicTxRepairCandidate*> repair_by_flow;
    for (const RnicTxRepairCandidate& repair : repair_heads) {
        const FlowState& state = requireFlow(repair.flow_id);
        if (!state.selective_repair_pending) {
            throw std::invalid_argument("RNIC TX repair head is not marked pending");
        }
        if (repair.extent.wireBytes() > _packetization.maxWirePacketBytes()) {
            throw std::invalid_argument("RNIC TX repair exceeds the DATA packet extent");
        }
        if (!repair_by_flow.emplace(repair.flow_id, &repair).second) {
            throw std::invalid_argument("RNIC TX received two repair heads for one flow");
        }
    }

    std::vector<RnicPrbsWireCandidate> candidates;
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        const auto repair = repair_by_flow.find(state.flow_id);
        const bool has_repair = repair != repair_by_flow.end();
        if (state.selective_repair_pending != has_repair) {
            throw std::logic_error("RNIC TX pending repair has no matching per-flow head");
        }
        const bool has_fresh = state.payload_bytes_dispatched < state.payload_size_bytes;
        if (state.data_eligible && (has_repair || has_fresh) && state.effective_wire_rate_bps > 0) {
            const uint64_t head_wire_bytes =
                has_repair ? repair->second->extent.wireBytes() : headExtent(state).wireBytes();
            candidates.push_back({state.flow_id, state.effective_wire_rate_bps, head_wire_bytes});
        }
    }

    RnicPrbsPacer next_pacer = _pacer;
    const std::optional<uint64_t> selected = next_pacer.selectWireEvent(
        candidates, _access_capacity_bps, _packetization.maxWirePacketBytes());
    std::optional<RnicTxPacket> packet;
    uint64_t event_wire_bytes = _packetization.maxWirePacketBytes();
    FlowState* selected_state = nullptr;
    const RnicTxRepairCandidate* selected_repair = nullptr;
    std::optional<RnicPacketExtent> selected_extent;
    if (selected.has_value()) {
        FlowState& state = requireFlow(*selected);
        selected_state = &state;
        const auto repair = repair_by_flow.find(state.flow_id);
        if (repair != repair_by_flow.end()) {
            selected_repair = repair->second;
            selected_extent = selected_repair->extent;
        } else {
            if (state.packet_index == std::numeric_limits<uint64_t>::max()) {
                throw std::overflow_error("RNIC TX packet index overflow");
            }
            selected_extent = headExtent(state);
        }
        event_wire_bytes = selected_extent->wireBytes();
    }

    RnicWireSerializationClock next_wire_serializer = _wire_serializer;
    RnicWireSerializationClock next_data_opportunity_serializer = _data_opportunity_serializer;
    RnicWireSerializationInterval interval;
    if (selected_state != nullptr) {
        next_wire_serializer.synchronizeAvailableWith(next_data_opportunity_serializer);
        interval = next_wire_serializer.serialize(requested_start_ps, event_wire_bytes);
        next_data_opportunity_serializer = next_wire_serializer;
    } else {
        // The PRBS idle outcome advances only the virtual opportunity clock.
        // The physical wire is known idle at this event boundary and remains
        // available to a later high-priority control arrival.
        next_wire_serializer.rebaseIdle(requested_start_ps);
        interval = next_data_opportunity_serializer.serialize(requested_start_ps, event_wire_bytes);
    }

    // The explicit physical route begins at the RNIC-to-leaf pipe because this
    // port has already modeled the source link serializer.  Stamp ETA at that
    // route-injection boundary, then add only calibrated no-queue transit.
    // Compute it before committing either serializer so overflow remains
    // transactional.
    std::optional<uint64_t> selected_eta_ps;
    if (selected_state != nullptr) {
        const uint64_t calibrated_transit_ps =
            selected_state->calibrated_transit_ps(*selected_extent);
        selected_eta_ps = checkedAdd(interval.end_ps, calibrated_transit_ps,
                                     "RNIC TX eligibility timestamp overflow");
    }

    _pacer = next_pacer;
    _wire_serializer = next_wire_serializer;
    _data_opportunity_serializer = next_data_opportunity_serializer;
    if (selected_state != nullptr) {
        FlowState& state = *selected_state;
        if (selected_repair != nullptr) {
            packet = RnicTxPacket{state.flow_id,
                                  selected_repair->packet_index,
                                  selected_repair->payload_byte_offset,
                                  *selected_extent,
                                  interval.start_ps,
                                  interval.end_ps,
                                  *selected_eta_ps,
                                  RnicTxPacketKind::SelectiveRepair};
        } else {
            packet = RnicTxPacket{
                state.flow_id,    state.packet_index,         state.payload_bytes_dispatched,
                *selected_extent, interval.start_ps,          interval.end_ps,
                *selected_eta_ps, RnicTxPacketKind::FreshData};
            state.payload_bytes_dispatched += selected_extent->payloadBytes();
            ++state.packet_index;
            if (state.payload_bytes_dispatched == state.payload_size_bytes) {
                recomputeEffectiveRates();
            }
        }
    }

    return {interval.start_ps, interval.end_ps, packet};
}

RnicWireSerializationInterval RnicTxPort::dispatchControl(uint64_t requested_start_ps,
                                                          uint64_t wire_bytes) {
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

void RnicTxPort::rebaseDataClassIdle(uint64_t now_ps) {
    if (now_ps < nextWireOpportunityPs()) {
        throw std::invalid_argument("RNIC TX DATA class cannot rebase before availability");
    }
    _wire_serializer.rebaseIdle(now_ps);
    _data_opportunity_serializer.rebaseIdle(now_ps);
}

RnicPacketExtent RnicTxPort::headExtent(const FlowState& state) const {
    if (state.payload_bytes_dispatched >= state.payload_size_bytes) {
        throw std::logic_error("RNIC TX completed flow has no DATA head");
    }
    return _packetization.packetize(state.payload_size_bytes - state.payload_bytes_dispatched);
}

void RnicTxPort::recomputeEffectiveRates() {
    std::vector<FlowState*> active;
    uint64_t allocated_bps = 0;
    for (auto& item : _flows) {
        FlowState& state = item.second;
        state.effective_wire_rate_bps = 0;
        if (state.data_eligible &&
            (state.payload_bytes_dispatched < state.payload_size_bytes ||
             state.selective_repair_pending) &&
            state.wire_rate_grant_bps > 0) {
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

RnicRxPort::RnicRxPort(uint64_t access_capacity_bps, RnicRingCamConfig ring_cam_config)
    : _wire_serializer(access_capacity_bps), _ring_cam(ring_cam_config) {}

RnicRxArrivalResult RnicRxPort::processArrival(const RnicRingCamPacket& packet) {
    RnicRingCamArrivalResult result = _ring_cam.processArrival(packet);
    std::vector<RnicRxScheduledSerialization> scheduled =
        scheduleSerializations(result.released_before_admission);
    updateLogicalReleaseTracking(result.released_before_admission, result.logical_release_ps);
    std::vector<RnicRxPacketCompletion> completed = accountDeliveriesThrough(packet.arrival_ps);
    return {result.admission, result.logical_release_ps, std::move(scheduled),
            std::move(completed)};
}

RnicRxAdvanceResult RnicRxPort::advanceToWithCompletions(uint64_t now_ps) {
    const std::vector<RnicRingCamRelease> released = _ring_cam.advanceTo(now_ps);
    std::vector<RnicRxScheduledSerialization> scheduled = scheduleSerializations(released);
    updateLogicalReleaseTracking(released, std::nullopt);
    std::vector<RnicRxPacketCompletion> completed = accountDeliveriesThrough(now_ps);
    return {std::move(scheduled), std::move(completed)};
}

std::vector<RnicRxScheduledSerialization> RnicRxPort::advanceTo(uint64_t now_ps) {
    RnicRxAdvanceResult result = advanceToWithCompletions(now_ps);
    return std::move(result.serializations_scheduled);
}

std::optional<uint64_t> RnicRxPort::nextEventTimePs() const {
    std::optional<uint64_t> next_event;
    if (!_pending_logical_release_counts.empty()) {
        next_event = _pending_logical_release_counts.begin()->first;
    }
    if (!_pending_serializations.empty() &&
        (!next_event.has_value() || _pending_serializations.begin()->first < *next_event)) {
        next_event = _pending_serializations.begin()->first;
    }
    return next_event;
}

uint64_t RnicRxPort::deliveredPayloadBytes(uint64_t flow_id) const {
    const auto delivered = _delivered_payload_bytes_by_flow.find(flow_id);
    return delivered == _delivered_payload_bytes_by_flow.end() ? 0 : delivered->second;
}

uint64_t RnicRxPort::deliveredWireBytes(uint64_t flow_id) const {
    const auto delivered = _delivered_wire_bytes_by_flow.find(flow_id);
    return delivered == _delivered_wire_bytes_by_flow.end() ? 0 : delivered->second;
}

std::vector<RnicRxScheduledSerialization> RnicRxPort::scheduleSerializations(
    const std::vector<RnicRingCamRelease>& logical_releases) {
    RnicWireSerializationClock next_serializer = _wire_serializer;
    uint64_t next_pending_wire_bytes = _pending_serializer_wire_bytes;
    uint64_t next_high_watermark = _pending_serializer_high_watermark_wire_bytes;

    std::vector<RnicRxScheduledSerialization> scheduled;
    scheduled.reserve(logical_releases.size());
    std::multimap<uint64_t, RnicRingCamPacket> pending_additions;
    for (const RnicRingCamRelease& release : logical_releases) {
        const RnicWireSerializationInterval interval = next_serializer.serialize(
            release.logical_release_ps, release.packet.extent.wireBytes());
        next_pending_wire_bytes =
            checkedAdd(next_pending_wire_bytes, release.packet.extent.wireBytes(),
                       "RNIC RX pending serializer wire-byte counter overflow");
        next_high_watermark = std::max(next_high_watermark, next_pending_wire_bytes);
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

std::vector<RnicRxPacketCompletion> RnicRxPort::accountDeliveriesThrough(uint64_t now_ps) {
    const auto delivery_end = _pending_serializations.upper_bound(now_ps);
    if (delivery_end == _pending_serializations.begin()) {
        return {};
    }

    struct DeliveryTotals {
        uint64_t payload_bytes;
        uint64_t wire_bytes;
    };

    // Prepare every counter update, completion record, and missing map node
    // before changing live byte ledgers or erasing a pending packet.  This
    // keeps a multi-packet completion batch transactional on overflow.
    std::vector<RnicRxPacketCompletion> completed;
    completed.reserve(
        static_cast<size_t>(std::distance(_pending_serializations.begin(), delivery_end)));
    std::map<uint64_t, DeliveryTotals> next_totals_by_flow;
    uint64_t completed_wire_bytes = 0;
    for (auto delivery = _pending_serializations.begin(); delivery != delivery_end; ++delivery) {
        const RnicRingCamPacket& packet = delivery->second;
        auto totals = next_totals_by_flow.find(packet.flow_id);
        if (totals == next_totals_by_flow.end()) {
            totals =
                next_totals_by_flow
                    .emplace(packet.flow_id, DeliveryTotals{deliveredPayloadBytes(packet.flow_id),
                                                            deliveredWireBytes(packet.flow_id)})
                    .first;
        }
        totals->second.payload_bytes =
            checkedAdd(totals->second.payload_bytes, packet.extent.payloadBytes(),
                       "RNIC RX delivered-payload counter overflow");
        totals->second.wire_bytes = checkedAdd(totals->second.wire_bytes, packet.extent.wireBytes(),
                                               "RNIC RX delivered-wire counter overflow");
        completed_wire_bytes = checkedAdd(completed_wire_bytes, packet.extent.wireBytes(),
                                          "RNIC RX completed wire-byte batch overflow");
        completed.push_back({packet, delivery->first});
    }
    if (completed_wire_bytes > _pending_serializer_wire_bytes) {
        throw std::logic_error("RNIC RX pending serializer occupancy underflow");
    }

    std::map<uint64_t, uint64_t> payload_additions;
    std::map<uint64_t, uint64_t> wire_additions;
    for (const auto& update : next_totals_by_flow) {
        if (_delivered_payload_bytes_by_flow.count(update.first) == 0) {
            payload_additions.emplace(update.first, update.second.payload_bytes);
        }
        if (_delivered_wire_bytes_by_flow.count(update.first) == 0) {
            wire_additions.emplace(update.first, update.second.wire_bytes);
        }
    }

    for (const auto& update : next_totals_by_flow) {
        auto delivered_payload = _delivered_payload_bytes_by_flow.find(update.first);
        if (delivered_payload != _delivered_payload_bytes_by_flow.end()) {
            delivered_payload->second = update.second.payload_bytes;
        }
        auto delivered_wire = _delivered_wire_bytes_by_flow.find(update.first);
        if (delivered_wire != _delivered_wire_bytes_by_flow.end()) {
            delivered_wire->second = update.second.wire_bytes;
        }
    }
    _delivered_payload_bytes_by_flow.merge(payload_additions);
    _delivered_wire_bytes_by_flow.merge(wire_additions);
    _pending_serializer_wire_bytes -= completed_wire_bytes;
    _pending_serializations.erase(_pending_serializations.begin(), delivery_end);
    return completed;
}

void RnicRxPort::updateLogicalReleaseTracking(const std::vector<RnicRingCamRelease>& released,
                                              std::optional<uint64_t> admitted_release_ps) {
    struct CountUpdate {
        uint64_t next_count;
        bool existed;
    };

    std::map<uint64_t, CountUpdate> updates;
    const auto prepare = [this, &updates](uint64_t release_ps) {
        auto update = updates.find(release_ps);
        if (update != updates.end()) {
            return update;
        }
        const auto current = _pending_logical_release_counts.find(release_ps);
        return updates
            .emplace(
                release_ps,
                CountUpdate{current == _pending_logical_release_counts.end() ? 0 : current->second,
                            current != _pending_logical_release_counts.end()})
            .first;
    };

    for (const RnicRingCamRelease& release : released) {
        auto update = prepare(release.logical_release_ps);
        if (update->second.next_count == 0) {
            throw std::logic_error("RNIC RX logical release tracking underflow");
        }
        --update->second.next_count;
    }
    if (admitted_release_ps.has_value()) {
        auto update = prepare(*admitted_release_ps);
        update->second.next_count =
            checkedAdd(update->second.next_count, 1, "RNIC RX logical release count overflow");
    }

    std::map<uint64_t, uint64_t> additions;
    for (const auto& update : updates) {
        if (!update.second.existed && update.second.next_count != 0) {
            additions.emplace(update.first, update.second.next_count);
        }
    }

    for (const auto& update : updates) {
        if (!update.second.existed) {
            continue;
        }
        auto current = _pending_logical_release_counts.find(update.first);
        if (update.second.next_count == 0) {
            _pending_logical_release_counts.erase(current);
        } else {
            current->second = update.second.next_count;
        }
    }
    _pending_logical_release_counts.merge(additions);
}
