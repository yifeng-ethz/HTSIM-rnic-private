// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_packetized_manifold_runtime.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using Wide = unsigned __int128;

std::uint64_t checkedAdd(
        std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

}  // namespace

RnicPacketizedManifoldRuntime::RnicPacketizedManifoldRuntime(
        EventList& event_list,
        RateBps node_link_capacity_bps,
        RnicDataPacketizationConfig packetization,
        TimePs propagation_delay_ps)
    : EventSource(event_list, "rnic-packetized-manifold-runtime"),
      _node_link_capacity_bps(node_link_capacity_bps),
      _packetization(std::move(packetization)),
      _propagation_delay_ps(propagation_delay_ps) {
    // Validate the homogeneous fixed-envelope geometry without starting a
    // live busy period. The calendar itself owns the simulator-resolution
    // restrictions for one full wire quantum.
    static_cast<void>(RnicPacketizedSlotCalendar(
        _node_link_capacity_bps,
        _packetization.maxWirePacketBytes(),
        _propagation_delay_ps));
}

RnicPacketizedManifoldRuntime::RnicPacketizedManifoldRuntime(
        EventList& event_list,
        RateBps node_link_capacity_bps,
        std::uint64_t max_wire_packet_bytes,
        TimePs propagation_delay_ps)
    : RnicPacketizedManifoldRuntime(
          event_list,
          node_link_capacity_bps,
          RnicDataPacketizationConfig(max_wire_packet_bytes),
          propagation_delay_ps) {}

RnicPacketizedManifoldRuntime::~RnicPacketizedManifoldRuntime() {
    if (_event_handle.has_value()) {
        EventList::cancelPendingSourceByHandle(*this, *_event_handle);
    }
}

void RnicPacketizedManifoldRuntime::setup(
        std::uint32_t node_count,
        CompletionHandler complete_flow) {
    if (_is_setup) {
        throw std::logic_error("packetized manifold runtime is already set up");
    }
    if (node_count == 0) {
        throw std::invalid_argument(
            "packetized manifold runtime requires at least one node");
    }
    if (!complete_flow) {
        throw std::invalid_argument(
            "packetized manifold runtime requires a completion handler");
    }

    _node_count = node_count;
    _complete_flow = std::move(complete_flow);
    _is_setup = true;
}

void RnicPacketizedManifoldRuntime::send(
        const AtlahsFlowRequest& request) {
    requireSetup();
    const TimePs now_ps = EventList::now();
    if (request.start_time_ps != now_ps) {
        throw std::invalid_argument(
            "packetized manifold flow start time must equal event-list time");
    }
    if (request.source >= _node_count || request.destination >= _node_count) {
        throw std::out_of_range(
            "packetized manifold flow node is outside the configured range");
    }
    if (_flows.count(request.flow_id) != 0) {
        throw std::invalid_argument(
            "duplicate packetized manifold runtime flow id");
    }

    const auto ledger = packetLedger(request.payload_bytes, _packetization);
    FlowState new_flow;
    new_flow.request = request;
    new_flow.total_packet_count = ledger.first;
    new_flow.total_wire_bytes = ledger.second;

    FlowMap candidate_flows = _flows;
    candidate_flows.emplace(request.flow_id, new_flow);
    std::optional<RnicPacketizedSlotCalendar> candidate_calendar = _calendar;
    bool candidate_has_positive_grant = _has_positive_grant;
    ZeroDeliveryMap candidate_zero_deliveries = _pending_zero_deliveries;

    if (request.payload_bytes == 0) {
        FlowState& state = candidate_flows.at(request.flow_id);
        state.source_completion_time_ps = now_ps;
        const TimePs delivery_time = checkedAdd(
            now_ps,
            _propagation_delay_ps,
            "packetized manifold zero-flow delivery time overflow");
        candidate_zero_deliveries.emplace(delivery_time, request.flow_id);
    } else {
        if (!candidate_calendar.has_value()) {
            candidate_calendar.emplace(
                _node_link_capacity_bps,
                _packetization.maxWirePacketBytes(),
                _propagation_delay_ps,
                now_ps);
        } else if (!_has_positive_grant
                   && candidate_calendar->nextSlotStartPs() <= now_ps) {
            candidate_calendar->rebaseIdle(now_ps);
        }

        candidate_has_positive_grant = recomputeAllocation(
            *candidate_calendar, candidate_flows);
        validateNextSlot(
            *candidate_calendar,
            candidate_flows,
            candidate_has_positive_grant);
    }

    const std::optional<TimePs> candidate_next_event = earliestEventTime(
        candidate_calendar,
        candidate_has_positive_grant,
        candidate_flows,
        _pending_source_completions,
        _pending_deliveries,
        candidate_zero_deliveries,
        now_ps);

    _flows = std::move(candidate_flows);
    _calendar = std::move(candidate_calendar);
    _has_positive_grant = candidate_has_positive_grant;
    _pending_zero_deliveries = std::move(candidate_zero_deliveries);
    reschedule(candidate_next_event);
}

bool RnicPacketizedManifoldRuntime::hasPendingPhysicalWork() const noexcept {
    for (const auto& item : _flows) {
        if (!item.second.completion_notified) {
            return true;
        }
    }
    return false;
}

std::size_t RnicPacketizedManifoldRuntime::backloggedFlowCount() const noexcept {
    return backloggedFlowCount(_flows);
}

std::size_t RnicPacketizedManifoldRuntime::backloggedFlowCount(
        const FlowMap& flows) noexcept {
    std::size_t count = 0;
    for (const auto& item : flows) {
        if (item.second.sourceBacklogged()) {
            ++count;
        }
    }
    return count;
}

bool RnicPacketizedManifoldRuntime::contains(
        AtlahsFlowId flow_id) const noexcept {
    return _flows.count(flow_id) != 0;
}

RnicPacketizedFlowSnapshot RnicPacketizedManifoldRuntime::flow(
        AtlahsFlowId flow_id) const {
    const auto item = _flows.find(flow_id);
    if (item == _flows.end()) {
        throw std::out_of_range("unknown packetized manifold runtime flow id");
    }
    const FlowState& state = item->second;
    return {state.request,
            state.total_packet_count,
            state.total_wire_bytes,
            state.wire_rate_grant_bps,
            state.packets_reserved,
            state.payload_bytes_reserved,
            state.wire_bytes_reserved,
            state.packets_source_serialized,
            state.payload_bytes_source_serialized,
            state.wire_bytes_source_serialized,
            state.packets_delivered,
            state.payload_bytes_delivered,
            state.wire_bytes_delivered,
            state.source_completion_time_ps,
            state.delivery_completion_time_ps,
            state.completion_notified};
}

std::optional<RnicPacketizedManifoldRuntime::TimePs>
RnicPacketizedManifoldRuntime::nextSlotStartPs() const noexcept {
    if (!_calendar.has_value() || !_has_positive_grant
        || backloggedFlowCount() == 0) {
        return std::nullopt;
    }
    return _calendar->nextSlotStartPs();
}

void RnicPacketizedManifoldRuntime::doNextEvent() {
    // EventList erased the current handle immediately before dispatch.
    _event_handle.reset();
    const TimePs now_ps = EventList::now();

    const std::vector<AtlahsFlowId> completed = settleDueEvents(now_ps);
    const std::exception_ptr completion_error = notifyCompletions(completed);

    // Completion callbacks run first. A re-entrant send at this timestamp can
    // therefore join the still-uncommitted envelope that opens now.
    reserveSlotAt(now_ps);
    reschedule(earliestEventTime(now_ps));

    if (completion_error) {
        std::rethrow_exception(completion_error);
    }
}

void RnicPacketizedManifoldRuntime::requireSetup() const {
    if (!_is_setup) {
        throw std::logic_error(
            "packetized manifold runtime has not been set up");
    }
}

std::pair<std::uint64_t, std::uint64_t>
RnicPacketizedManifoldRuntime::packetLedger(
        std::uint64_t payload_bytes,
        const RnicDataPacketizationConfig& packetization) {
    if (payload_bytes == 0) {
        return {0, 0};
    }
    const std::uint64_t payload_quantum = packetization.maxPayloadBytes();
    const std::uint64_t packet_count =
        (payload_bytes - 1) / payload_quantum + 1;
    const Wide total_wire = static_cast<Wide>(payload_bytes)
                            + static_cast<Wide>(packet_count)
                                  * packetization.dataHeaderBytes();
    if (total_wire > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "packetized manifold total wire-byte ledger overflow");
    }
    return {packet_count, static_cast<std::uint64_t>(total_wire)};
}

std::vector<RnicMaxMinFlow> RnicPacketizedManifoldRuntime::activeFlows(
        const FlowMap& flows) const {
    std::vector<RnicMaxMinFlow> active;
    active.reserve(flows.size());
    for (const auto& item : flows) {
        const FlowState& state = item.second;
        if (state.sourceBacklogged()) {
            active.push_back({state.request.flow_id,
                              state.request.source,
                              state.request.destination});
        }
    }
    return active;
}

bool RnicPacketizedManifoldRuntime::recomputeAllocation(
        RnicPacketizedSlotCalendar& calendar,
        FlowMap& flows) const {
    const std::vector<RnicMaxMinFlow> active = activeFlows(flows);
    RnicMaxMinAllocator::CapacityMap source_capacities;
    RnicMaxMinAllocator::CapacityMap destination_capacities;
    for (const RnicMaxMinFlow& flow : active) {
        source_capacities.emplace(flow.source_node, _node_link_capacity_bps);
        destination_capacities.emplace(
            flow.destination_node, _node_link_capacity_bps);
    }
    const RnicMaxMinAllocator::AllocationMap allocation =
        RnicMaxMinAllocator::allocate(
            active, source_capacities, destination_capacities);

    std::vector<RnicPacketizedGrant> grants;
    grants.reserve(active.size());
    for (auto& item : flows) {
        item.second.wire_rate_grant_bps = 0;
    }

    bool has_positive_grant = false;
    for (const RnicMaxMinFlow& flow : active) {
        const RateBps rate = allocation.at(flow.flow_id);
        flows.at(flow.flow_id).wire_rate_grant_bps = rate;
        grants.push_back(
            {flow.flow_id, flow.source_node, flow.destination_node, rate});
        has_positive_grant = has_positive_grant || rate != 0;
    }
    calendar.beginEpoch(calendar.nextSlotIndex(), grants);
    return has_positive_grant;
}

void RnicPacketizedManifoldRuntime::validateNextSlot(
        const RnicPacketizedSlotCalendar& calendar,
        const FlowMap& flows,
        bool has_positive_grant) const {
    if (!has_positive_grant) {
        return;
    }

    RnicPacketizedSlotCalendar preview = calendar;
    const std::vector<RnicPacketizedReservation> reservations =
        preview.reserveNextSlot();
    for (const RnicPacketizedReservation& reservation : reservations) {
        const auto item = flows.find(reservation.flowId());
        if (item == flows.end() || !item->second.sourceBacklogged()) {
            throw std::logic_error(
                "packetized manifold calendar selected a non-backlogged flow");
        }
        const FlowState& state = item->second;
        const RnicPacketExtent extent = _packetization.packetize(
            state.request.payload_bytes - state.payload_bytes_reserved);
        static_cast<void>(reservation.materializePacket(extent));
    }
}

void RnicPacketizedManifoldRuntime::reserveSlotAt(TimePs now_ps) {
    if (!_calendar.has_value() || !_has_positive_grant
        || backloggedFlowCount() == 0) {
        return;
    }
    if (_calendar->nextSlotStartPs() > now_ps) {
        return;
    }
    if (_calendar->nextSlotStartPs() < now_ps) {
        throw std::logic_error("packetized manifold slot event is in the past");
    }

    RnicPacketizedSlotCalendar candidate_calendar = *_calendar;
    FlowMap selected_flows;
    PacketEventMap source_additions;
    PacketEventMap delivery_additions;
    bool candidate_has_positive_grant = _has_positive_grant;

    const std::vector<RnicPacketizedReservation> reservations =
        candidate_calendar.reserveNextSlot();
    bool active_set_changed = false;
    for (const RnicPacketizedReservation& reservation : reservations) {
        const auto live = _flows.find(reservation.flowId());
        if (live == _flows.end()) {
            throw std::logic_error(
                "packetized manifold calendar selected an unknown flow");
        }
        const auto selected = selected_flows.emplace(
            reservation.flowId(), live->second);
        if (!selected.second) {
            throw std::logic_error(
                "packetized manifold selected one flow twice in an envelope");
        }
        FlowState& state = selected.first->second;
        if (!state.sourceBacklogged()) {
            throw std::logic_error(
                "packetized manifold reserved a completed source flow");
        }
        if (state.packets_reserved >= state.total_packet_count) {
            throw std::logic_error(
                "packetized manifold packet reservation count overflow");
        }

        const std::uint64_t remaining_payload =
            state.request.payload_bytes - state.payload_bytes_reserved;
        const RnicPacketExtent extent =
            _packetization.packetize(remaining_payload);
        const RnicPacketizedTransmission transmission =
            reservation.materializePacket(extent);
        if (transmission.sourceSerializationStartPs() < now_ps) {
            throw std::logic_error(
                "packetized manifold committed a source packet in the past");
        }

        const std::uint64_t packet_index = state.packets_reserved;
        state.packets_reserved = checkedAdd(
            state.packets_reserved,
            1,
            "packetized manifold reserved-packet ledger overflow");
        state.payload_bytes_reserved = checkedAdd(
            state.payload_bytes_reserved,
            extent.payloadBytes(),
            "packetized manifold reserved-payload ledger overflow");
        state.wire_bytes_reserved = checkedAdd(
            state.wire_bytes_reserved,
            extent.wireBytes(),
            "packetized manifold reserved-wire ledger overflow");
        if (state.packets_reserved > state.total_packet_count
            || state.payload_bytes_reserved > state.request.payload_bytes
            || state.wire_bytes_reserved > state.total_wire_bytes) {
            throw std::logic_error(
                "packetized manifold reservation exceeded its exact ledger");
        }

        const PacketEvent event{state.request.flow_id, packet_index, extent};
        source_additions.emplace(
            transmission.sourceSerializationEndPs(), event);
        delivery_additions.emplace(
            transmission.destinationSerializationEndPs(), event);
        active_set_changed = active_set_changed || !state.sourceBacklogged();
    }

    if (active_set_changed) {
        FlowMap candidate_flows = _flows;
        for (const auto& item : selected_flows) {
            candidate_flows.at(item.first) = item.second;
        }
        candidate_has_positive_grant = recomputeAllocation(
            candidate_calendar, candidate_flows);
        validateNextSlot(
            candidate_calendar,
            candidate_flows,
            candidate_has_positive_grant);
        _flows = std::move(candidate_flows);
    } else {
        // The grant snapshot is unchanged. Commit only the flows selected by
        // this parallel matching; completed history and unrelated flows are
        // not copied once per packet envelope.
        for (auto& item : selected_flows) {
            _flows.at(item.first) = std::move(item.second);
        }
    }

    *_calendar = std::move(candidate_calendar);
    _pending_source_completions.merge(source_additions);
    _pending_deliveries.merge(delivery_additions);
    _has_positive_grant = candidate_has_positive_grant;
}

std::vector<AtlahsFlowId>
RnicPacketizedManifoldRuntime::settleDueEvents(TimePs now_ps) {
    FlowMap affected_flows;
    std::set<AtlahsFlowId> completed;

    const auto candidateFlow = [&](AtlahsFlowId flow_id) -> FlowState& {
        auto candidate = affected_flows.find(flow_id);
        if (candidate == affected_flows.end()) {
            const auto live = _flows.find(flow_id);
            if (live == _flows.end()) {
                throw std::logic_error(
                    "packetized manifold event references an unknown flow");
            }
            candidate = affected_flows.emplace(flow_id, live->second).first;
        }
        return candidate->second;
    };

    const auto source_end = _pending_source_completions.upper_bound(now_ps);
    for (auto source = _pending_source_completions.begin();
         source != source_end;
         ++source) {
        const TimePs completion_time = source->first;
        const PacketEvent& event = source->second;
        FlowState& state = candidateFlow(event.flow_id);
        if (event.packet_index != state.packets_source_serialized) {
            throw std::logic_error(
                "packetized manifold source packet order is inconsistent");
        }
        state.packets_source_serialized = checkedAdd(
            state.packets_source_serialized,
            1,
            "packetized manifold source-packet ledger overflow");
        state.payload_bytes_source_serialized = checkedAdd(
            state.payload_bytes_source_serialized,
            event.extent.payloadBytes(),
            "packetized manifold source-payload ledger overflow");
        state.wire_bytes_source_serialized = checkedAdd(
            state.wire_bytes_source_serialized,
            event.extent.wireBytes(),
            "packetized manifold source-wire ledger overflow");
        if (state.packets_source_serialized > state.packets_reserved
            || state.payload_bytes_source_serialized
                   > state.payload_bytes_reserved
            || state.wire_bytes_source_serialized > state.wire_bytes_reserved) {
            throw std::logic_error(
                "packetized manifold source service exceeded reservation");
        }
        if (state.packets_source_serialized == state.total_packet_count) {
            if (state.payload_bytes_source_serialized
                    != state.request.payload_bytes
                || state.wire_bytes_source_serialized
                    != state.total_wire_bytes) {
                throw std::logic_error(
                    "packetized manifold final source ledger is incomplete");
            }
            state.source_completion_time_ps = completion_time;
        }
    }

    const auto delivery_end = _pending_deliveries.upper_bound(now_ps);
    for (auto delivery = _pending_deliveries.begin();
         delivery != delivery_end;
         ++delivery) {
        const TimePs completion_time = delivery->first;
        const PacketEvent& event = delivery->second;
        FlowState& state = candidateFlow(event.flow_id);
        if (event.packet_index != state.packets_delivered) {
            throw std::logic_error(
                "packetized manifold destination packet order is inconsistent");
        }
        state.packets_delivered = checkedAdd(
            state.packets_delivered,
            1,
            "packetized manifold delivered-packet ledger overflow");
        state.payload_bytes_delivered = checkedAdd(
            state.payload_bytes_delivered,
            event.extent.payloadBytes(),
            "packetized manifold delivered-payload ledger overflow");
        state.wire_bytes_delivered = checkedAdd(
            state.wire_bytes_delivered,
            event.extent.wireBytes(),
            "packetized manifold delivered-wire ledger overflow");
        if (state.packets_delivered > state.packets_source_serialized
            || state.payload_bytes_delivered
                   > state.payload_bytes_source_serialized
            || state.wire_bytes_delivered
                   > state.wire_bytes_source_serialized) {
            throw std::logic_error(
                "packetized manifold delivery preceded source service");
        }
        if (state.packets_delivered == state.total_packet_count) {
            if (state.payload_bytes_delivered != state.request.payload_bytes
                || state.wire_bytes_delivered != state.total_wire_bytes) {
                throw std::logic_error(
                    "packetized manifold final delivery ledger is incomplete");
            }
            state.delivery_completion_time_ps = completion_time;
            if (!state.completion_notified) {
                state.completion_notified = true;
                completed.insert(state.request.flow_id);
            }
        }
    }

    const auto zero_end = _pending_zero_deliveries.upper_bound(now_ps);
    for (auto zero = _pending_zero_deliveries.begin();
         zero != zero_end;
         ++zero) {
        FlowState& state = candidateFlow(zero->second);
        if (state.total_packet_count != 0 || state.total_wire_bytes != 0) {
            throw std::logic_error(
                "packetized manifold zero-delivery event has DATA state");
        }
        state.delivery_completion_time_ps = zero->first;
        if (!state.completion_notified) {
            state.completion_notified = true;
            completed.insert(state.request.flow_id);
        }
    }

    // Arithmetic, identity, ordering, and final-ledger checks above complete
    // before the live state changes. Only flows touched at this timestamp are
    // copied; a long fixed-delay in-flight calendar is never cloned per event.
    for (auto& item : affected_flows) {
        _flows.at(item.first) = std::move(item.second);
    }
    _pending_source_completions.erase(
        _pending_source_completions.begin(), source_end);
    _pending_deliveries.erase(_pending_deliveries.begin(), delivery_end);
    _pending_zero_deliveries.erase(
        _pending_zero_deliveries.begin(), zero_end);
    return {completed.begin(), completed.end()};
}

std::exception_ptr RnicPacketizedManifoldRuntime::notifyCompletions(
        const std::vector<AtlahsFlowId>& completed_flows) {
    std::exception_ptr first_error;
    for (AtlahsFlowId flow_id : completed_flows) {
        try {
            _complete_flow(flow_id);
        } catch (...) {
            if (!first_error) {
                first_error = std::current_exception();
            }
        }
    }
    return first_error;
}

std::optional<RnicPacketizedManifoldRuntime::TimePs>
RnicPacketizedManifoldRuntime::earliestEventTime(TimePs now_ps) const {
    return earliestEventTime(
        _calendar,
        _has_positive_grant,
        _flows,
        _pending_source_completions,
        _pending_deliveries,
        _pending_zero_deliveries,
        now_ps);
}

std::optional<RnicPacketizedManifoldRuntime::TimePs>
RnicPacketizedManifoldRuntime::earliestEventTime(
        const std::optional<RnicPacketizedSlotCalendar>& calendar,
        bool has_positive_grant,
        const FlowMap& flows,
        const PacketEventMap& pending_source_completions,
        const PacketEventMap& pending_deliveries,
        const ZeroDeliveryMap& pending_zero_deliveries,
        TimePs now_ps) const {
    std::optional<TimePs> earliest;
    const auto consider = [&](TimePs candidate) {
        if (candidate < now_ps) {
            throw std::logic_error(
                "packetized manifold event is scheduled in the past");
        }
        if (!earliest.has_value() || candidate < *earliest) {
            earliest = candidate;
        }
    };

    if (calendar.has_value() && has_positive_grant
        && backloggedFlowCount(flows) != 0) {
        consider(calendar->nextSlotStartPs());
    }
    if (!pending_source_completions.empty()) {
        consider(pending_source_completions.begin()->first);
    }
    if (!pending_deliveries.empty()) {
        consider(pending_deliveries.begin()->first);
    }
    if (!pending_zero_deliveries.empty()) {
        consider(pending_zero_deliveries.begin()->first);
    }
    return earliest;
}

void RnicPacketizedManifoldRuntime::reschedule(
        std::optional<TimePs> next_event_time) {
    if (_event_handle.has_value()) {
        EventList::cancelPendingSourceByHandle(*this, *_event_handle);
        _event_handle.reset();
    }
    if (!next_event_time.has_value()) {
        return;
    }

    EventList::Handle handle =
        EventList::sourceIsPendingGetHandle(*this, *next_event_time);
    if (handle != EventList::nullHandle()) {
        _event_handle = handle;
    }
}
