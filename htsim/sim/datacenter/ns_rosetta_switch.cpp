// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_rosetta_switch.h"

#include "fat_tree_topology.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

NsRosettaIngressPort::NsRosettaIngressPort(NsRosetta& owner, uint32_t ingress_id, std::string name)
    : _owner(owner), _ingress_id(ingress_id), _name(std::move(name)) {}

void NsRosettaIngressPort::receivePacket(Packet& pkt) {
    _owner.receive_from_physical_ingress(pkt, _ingress_id);
}

NsRosettaEgressSerializer::NsRosettaEgressSerializer(linkspeed_bps bitrate,
                                                     EventList& eventlist,
                                                     QueueLogger* logger)
    : BaseQueue(bitrate, eventlist, logger) {
    _nodename = "ns-rosetta-egress-serializer";
}

void NsRosettaEgressSerializer::bind(NsRosetta& owner, uint32_t egress_id) {
    if (_owner != nullptr) {
        throw std::logic_error("ns-rosetta egress serializer already bound");
    }
    _owner = &owner;
    _egress_id = egress_id;
}

void NsRosettaEgressSerializer::receivePacket(Packet& pkt) {
    if (_owner == nullptr) {
        throw std::logic_error("unbound ns-rosetta egress serializer");
    }
    if (_authorized_dispatch != &pkt) {
        throw std::logic_error("ns-rosetta egress serializer cannot bypass request/grant");
    }
    if (_packet_in_service != nullptr) {
        throw std::logic_error("ns-rosetta request/grant dispatched onto a busy serializer");
    }

    _authorized_dispatch = nullptr;
    _packet_in_service = &pkt;
    const simtime_picosec serialization_time = drainTime(&pkt);
    if (EventList::now() > std::numeric_limits<simtime_picosec>::max() - serialization_time) {
        _packet_in_service = nullptr;
        throw std::overflow_error("ns-rosetta serialization completion time overflow");
    }
    _service_completion_time = EventList::now() + serialization_time;
    eventlist().sourceIsPendingRel(*this, serialization_time);
}

void NsRosettaEgressSerializer::dispatch(Packet& pkt) {
    if (_authorized_dispatch != nullptr) {
        throw std::logic_error("ns-rosetta egress serializer has a pending dispatch");
    }

    _authorized_dispatch = &pkt;
    try {
        PacketSink* selected_sink = pkt.sendOn();
        if (selected_sink != this || _authorized_dispatch != nullptr) {
            throw std::logic_error(
                "ns-rosetta packet route did not consume its granted "
                "egress dispatch");
        }
    } catch (...) {
        _authorized_dispatch = nullptr;
        throw;
    }
}

void NsRosettaEgressSerializer::doNextEvent() {
    if (_packet_in_service == nullptr) {
        throw std::logic_error("idle ns-rosetta serializer received an event");
    }

    Packet* packet = _packet_in_service;
    const simtime_picosec serialization_time = drainTime(packet);
    _packet_in_service = nullptr;
    _service_completion_time = 0;

    packet->flow().logTraffic(*packet, *this, TrafficLogger::PKT_DEPART);
    if (_logger != nullptr) {
        _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *packet);
    }
    log_packet_send(serialization_time);

    // The owner retains an immutable summary for completion accounting; the
    // downstream endpoint may recycle the packet during sendOn().
    packet->sendOn();
    _owner->egress_serialization_complete(_egress_id);
}

mem_b NsRosettaEgressSerializer::queuesize() const {
    if (_owner == nullptr) {
        return in_service_bytes();
    }
    return _owner->egress_backlog_bytes(_egress_id);
}

mem_b NsRosettaEgressSerializer::maxsize() const {
    return _owner == nullptr ? 0 : _owner->shared_buffer_capacity();
}

mem_b NsRosettaEgressSerializer::in_service_bytes() const {
    return _packet_in_service == nullptr ? 0 : _packet_in_service->size();
}

simtime_picosec NsRosettaEgressSerializer::remaining_service_time_ps() const {
    if (_packet_in_service == nullptr || _service_completion_time <= EventList::now()) {
        return 0;
    }
    return _service_completion_time - EventList::now();
}

simtime_picosec NsRosettaEgressSerializer::serialization_delay_for_bytes(mem_b bytes) const {
    if (bytes < 0) {
        throw std::invalid_argument("ns-rosetta cannot serialize a negative byte count");
    }
    const auto unsigned_bytes = static_cast<uint64_t>(bytes);
    if (_ps_per_byte != 0 &&
        unsigned_bytes > std::numeric_limits<simtime_picosec>::max() / _ps_per_byte) {
        throw std::overflow_error("ns-rosetta backlog serialization delay overflow");
    }
    return unsigned_bytes * _ps_per_byte;
}

void NsRosettaEgressSerializer::note_packet_enqueued(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    if (_logger != nullptr) {
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);
    }
}

void NsRosettaEgressSerializer::note_shared_buffer_drop(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);
    // Admission is switch-wide, so per-egress loggers cannot safely account
    // for a drop caused by occupancy on a different output.
}

NsRosetta::NsRosetta(EventList& eventlist,
                     const string& name,
                     switch_type type,
                     uint32_t id,
                     simtime_picosec switch_delay,
                     FatTreeTopology* topology,
                     mem_b shared_buffer_capacity)
    : FatTreeSwitch(eventlist, name, type, id, switch_delay, topology),
      _shared_buffer_capacity(shared_buffer_capacity) {
    if (_shared_buffer_capacity <= 0) {
        throw std::invalid_argument("ns-rosetta shared-buffer capacity must be positive");
    }
}

int NsRosetta::addPort(BaseQueue* queue) {
    auto* serializer = dynamic_cast<NsRosettaEgressSerializer*>(queue);
    if (serializer == nullptr) {
        throw std::invalid_argument("ns-rosetta switch ports must be physical egress serializers");
    }

    const int egress_id = FatTreeSwitch::addPort(queue);
    serializer->bind(*this, static_cast<uint32_t>(egress_id));

    EgressState state;
    state.serializer = serializer;
    _egresses.push_back(std::move(state));
    assert(static_cast<size_t>(egress_id) == _egresses.size() - 1);
    return egress_id;
}

PacketSink* NsRosetta::create_physical_ingress(const string& name) {
    const uint32_t ingress_id = static_cast<uint32_t>(_ingresses.size());
    IngressState state;
    state.port = std::make_unique<NsRosettaIngressPort>(*this, ingress_id, name);
    PacketSink* result = state.port.get();
    _ingresses.push_back(std::move(state));
    return result;
}

void NsRosetta::receive_from_physical_ingress(Packet& pkt, uint32_t ingress_id) {
    if (pkt.type() == ETH_PAUSE) {
        throw std::invalid_argument("ns-rosetta does not support PAUSE/PFC packets");
    }
    validate_ingress(ingress_id);
    if (!_pipeline_ingress.emplace(&pkt, ingress_id).second) {
        throw std::logic_error("packet entered the ns-rosetta switch pipeline twice");
    }

    schedule_through_switch_pipeline(pkt);
}

void NsRosetta::receivePacket(Packet& pkt) {
    if (pkt.type() == ETH_PAUSE) {
        throw std::invalid_argument("ns-rosetta does not support PAUSE/PFC packets");
    }

    auto pending = _pipeline_ingress.find(&pkt);
    if (pending == _pipeline_ingress.end()) {
        throw std::logic_error("ns-rosetta packets must enter through a physical ingress");
    }
    const uint32_t ingress_id = pending->second;
    _pipeline_ingress.erase(pending);

    NsRosettaEgressSerializer& egress = resolve_selected_egress(pkt);
    enqueue(pkt, ingress_id, egress);
}

size_t NsRosetta::traffic_class(Packet::PktPriority priority) {
    switch (priority) {
        case Packet::PRIO_HI:
            return 0;
        case Packet::PRIO_MID:
            return 1;
        case Packet::PRIO_LO:
        case Packet::PRIO_NONE:
            return 2;
    }
    throw std::invalid_argument("unknown packet priority");
}

NsRosetta::PacketSummary NsRosetta::summarize(Packet& pkt,
                                              uint32_t ingress_id,
                                              uint32_t egress_id,
                                              size_t selected_traffic_class) {
    return PacketSummary{
        ingress_id, egress_id, selected_traffic_class, {pkt.src(), pkt.dst()}, pkt.flow_id(),
        pkt.id(),   pkt.size()};
}

NsRosettaEgressSerializer& NsRosetta::resolve_selected_egress(Packet& pkt) {
    if (pkt.route() != nullptr && pkt.nexthop() < pkt.route()->size()) {
        auto* egress = dynamic_cast<NsRosettaEgressSerializer*>(pkt.route()->at(pkt.nexthop()));
        if (egress == nullptr || egress->owner() != this) {
            throw std::logic_error(
                "ns-rosetta physical ingress is not followed by a local "
                "physical egress");
        }
        return *egress;
    }

    Route* selected_route = FatTreeSwitch::getNextHop(pkt, nullptr);
    if (selected_route == nullptr || selected_route->size() == 0) {
        throw std::logic_error("ns-rosetta FIB selected an empty route");
    }
    pkt.set_route(*selected_route);

    auto* egress = dynamic_cast<NsRosettaEgressSerializer*>(selected_route->at(0));
    if (egress == nullptr || egress->owner() != this) {
        throw std::logic_error("ns-rosetta FIB did not select a local physical egress");
    }
    return *egress;
}

void NsRosetta::enqueue(Packet& pkt, uint32_t ingress_id, NsRosettaEgressSerializer& egress) {
    const size_t selected_traffic_class = traffic_class(pkt.priority());
    const PacketSummary packet =
        summarize(pkt, ingress_id, egress.egress_id(), selected_traffic_class);

    // Ordered route binding happens before the first DATA packet traverses the
    // node-to-leaf cable.  Once that low-priority DATA packet reaches the
    // selected source-leaf egress, real queue state replaces the provisional
    // local reservation.  Reverse-flow ACK/SACK, credit, and backpressure
    // packets can have the same wire {src,dst}; their strict high priority
    // must never consume or validate a DATA reservation.
    if (pkt.priority() == Packet::PRIO_LO) {
        consume_ordered_route_reservation(packet.pair, packet.egress_id);
    }

    if (packet.packet_bytes > _shared_buffer_capacity ||
        _shared_buffer_occupancy > _shared_buffer_capacity - packet.packet_bytes) {
        _buffer_counters.dropped_packets++;
        _buffer_counters.dropped_bytes += packet.packet_bytes;
        egress.note_shared_buffer_drop(pkt);
        emit_transition(NsRosettaQueueTransition::Dropped, packet);
        pkt.free();
        return;
    }

    IngressState& ingress = ingress_state(ingress_id);
    EgressState& output = egress_state(egress.egress_id());
    Voq& voq = output.traffic_classes[selected_traffic_class].by_ingress[ingress_id];
    voq.packets.push_back(&pkt);
    voq.buffered_bytes += packet.packet_bytes;
    ingress.buffered_bytes += packet.packet_bytes;
    output.buffered_bytes += packet.packet_bytes;
    output.pairs[packet.pair].buffered_bytes += packet.packet_bytes;
    _pairs[packet.pair].buffered_bytes += packet.packet_bytes;
    _shared_buffer_occupancy += packet.packet_bytes;
    _shared_buffer_high_watermark =
        std::max(_shared_buffer_high_watermark, _shared_buffer_occupancy);
    _buffer_counters.admitted_packets++;
    _buffer_counters.admitted_bytes += packet.packet_bytes;
    egress.note_packet_enqueued(pkt);

    emit_transition(NsRosettaQueueTransition::Enqueued, packet);
    run_arbitration();
}

void NsRosetta::run_arbitration() {
    std::vector<bool> ingress_matched(_ingresses.size(), false);
    std::vector<bool> egress_matched(_egresses.size(), false);
    std::vector<Grant> matches;

    while (true) {
        std::vector<std::vector<Grant>> grants_by_ingress(_ingresses.size());
        for (uint32_t egress_id = 0; egress_id < _egresses.size(); ++egress_id) {
            const EgressState& egress = _egresses[egress_id];
            if (egress_matched[egress_id] || egress.serializer->is_busy() ||
                egress.active_grant.has_value()) {
                continue;
            }
            const std::optional<Grant> grant = select_egress_grant(egress_id, ingress_matched);
            if (grant.has_value()) {
                grants_by_ingress[grant->ingress_id].push_back(*grant);
            }
        }

        size_t accepted_this_round = 0;
        for (uint32_t ingress_id = 0; ingress_id < _ingresses.size(); ++ingress_id) {
            if (ingress_matched[ingress_id] || ingress_state(ingress_id).grant_busy ||
                grants_by_ingress[ingress_id].empty()) {
                continue;
            }
            const Grant accepted = accept_input_grant(ingress_id, grants_by_ingress[ingress_id]);
            ingress_matched[ingress_id] = true;
            egress_matched[accepted.egress_id] = true;
            matches.push_back(accepted);
            accepted_this_round++;
        }

        if (accepted_this_round == 0) {
            break;
        }
    }

    for (const Grant& match : matches) {
        dispatch_grant(match);
    }
}

std::optional<NsRosetta::Grant> NsRosetta::select_egress_grant(
    uint32_t egress_id,
    const std::vector<bool>& ingress_matched) const {
    const EgressState& egress = egress_state(egress_id);
    for (size_t selected_class = 0; selected_class < egress.traffic_classes.size();
         ++selected_class) {
        const TrafficClassVoqs& traffic_class_state = egress.traffic_classes[selected_class];
        const auto& requests = traffic_class_state.by_ingress;
        if (requests.empty()) {
            continue;
        }

        auto first = traffic_class_state.has_last_granted_ingress
                         ? requests.upper_bound(traffic_class_state.last_granted_ingress)
                         : requests.begin();
        if (first == requests.end()) {
            first = requests.begin();
        }
        auto candidate = first;
        do {
            const uint32_t ingress_id = candidate->first;
            if (!candidate->second.packets.empty() && !ingress_matched[ingress_id] &&
                !ingress_state(ingress_id).grant_busy) {
                return Grant{ingress_id, egress_id, selected_class};
            }
            ++candidate;
            if (candidate == requests.end()) {
                candidate = requests.begin();
            }
        } while (candidate != first);
    }
    return std::nullopt;
}

NsRosetta::Grant NsRosetta::accept_input_grant(uint32_t ingress_id,
                                               const std::vector<Grant>& grants) const {
    if (grants.empty()) {
        throw std::invalid_argument("ns-rosetta input cannot accept an empty grant set");
    }
    const IngressState& ingress = ingress_state(ingress_id);
    const size_t highest_class =
        std::min_element(grants.begin(), grants.end(), [](const Grant& left, const Grant& right) {
            return left.traffic_class < right.traffic_class;
        })->traffic_class;

    const Grant* wrapped = nullptr;
    const Grant* after_cursor = nullptr;
    for (const Grant& grant : grants) {
        if (grant.traffic_class != highest_class) {
            continue;
        }
        if (wrapped == nullptr || grant.egress_id < wrapped->egress_id) {
            wrapped = &grant;
        }
        if (ingress.has_last_accepted_egress && grant.egress_id > ingress.last_accepted_egress &&
            (after_cursor == nullptr || grant.egress_id < after_cursor->egress_id)) {
            after_cursor = &grant;
        }
    }
    const Grant* selected = after_cursor == nullptr ? wrapped : after_cursor;
    assert(selected != nullptr);
    return *selected;
}

void NsRosetta::dispatch_grant(const Grant& grant) {
    IngressState& ingress = ingress_state(grant.ingress_id);
    EgressState& egress = egress_state(grant.egress_id);
    TrafficClassVoqs& traffic_class_state = egress.traffic_classes[grant.traffic_class];
    auto request = traffic_class_state.by_ingress.find(grant.ingress_id);
    if (request == traffic_class_state.by_ingress.end() || request->second.packets.empty()) {
        throw std::logic_error("ns-rosetta granted an empty VoQ");
    }
    if (ingress.grant_busy || egress.serializer->is_busy() || egress.active_grant.has_value()) {
        throw std::logic_error("ns-rosetta request/grant match is not free");
    }

    Packet* packet = request->second.packets.front();
    const PacketSummary summary =
        summarize(*packet, grant.ingress_id, grant.egress_id, grant.traffic_class);
    if (packet->route() == nullptr || packet->nexthop() >= packet->route()->size() ||
        packet->route()->at(packet->nexthop()) != egress.serializer) {
        throw std::logic_error("ns-rosetta granted packet does not target its serializer");
    }
    if (request->second.buffered_bytes < summary.packet_bytes ||
        ingress.buffered_bytes < summary.packet_bytes ||
        egress.buffered_bytes < summary.packet_bytes ||
        _shared_buffer_occupancy < summary.packet_bytes) {
        throw std::logic_error("ns-rosetta shared-buffer accounting underflow");
    }
    auto pair = _pairs.find(summary.pair);
    if (pair == _pairs.end() || pair->second.buffered_bytes < summary.packet_bytes) {
        throw std::logic_error("ns-rosetta pair accounting underflow");
    }
    auto egress_pair = egress.pairs.find(summary.pair);
    if (egress_pair == egress.pairs.end() ||
        egress_pair->second.buffered_bytes < summary.packet_bytes) {
        throw std::logic_error("ns-rosetta egress-pair accounting underflow");
    }

    request->second.packets.pop_front();
    request->second.buffered_bytes -= summary.packet_bytes;
    if (request->second.packets.empty()) {
        if (request->second.buffered_bytes != 0) {
            throw std::logic_error("ns-rosetta empty VoQ retains bytes");
        }
        traffic_class_state.by_ingress.erase(request);
    }
    traffic_class_state.has_last_granted_ingress = true;
    traffic_class_state.last_granted_ingress = grant.ingress_id;
    ingress.has_last_accepted_egress = true;
    ingress.last_accepted_egress = grant.egress_id;

    ingress.buffered_bytes -= summary.packet_bytes;
    ingress.in_service_bytes += summary.packet_bytes;
    ingress.grant_busy = true;
    egress.buffered_bytes -= summary.packet_bytes;
    egress.active_grant = summary;
    egress_pair->second.buffered_bytes -= summary.packet_bytes;
    egress_pair->second.in_service_bytes += summary.packet_bytes;
    pair->second.buffered_bytes -= summary.packet_bytes;
    pair->second.in_service_bytes += summary.packet_bytes;
    _shared_buffer_occupancy -= summary.packet_bytes;
    _buffer_counters.granted_packets++;
    _buffer_counters.granted_bytes += summary.packet_bytes;

    egress.serializer->dispatch(*packet);
    emit_transition(NsRosettaQueueTransition::Granted, summary);
}

void NsRosetta::egress_serialization_complete(uint32_t egress_id) {
    EgressState& egress = egress_state(egress_id);
    if (!egress.active_grant.has_value()) {
        throw std::logic_error("ns-rosetta serializer completed without an active grant");
    }
    const PacketSummary summary = *egress.active_grant;
    IngressState& ingress = ingress_state(summary.ingress_id);
    if (!ingress.grant_busy || ingress.in_service_bytes < summary.packet_bytes) {
        throw std::logic_error("ns-rosetta ingress grant accounting underflow");
    }
    auto pair = _pairs.find(summary.pair);
    if (pair == _pairs.end() || pair->second.in_service_bytes < summary.packet_bytes) {
        throw std::logic_error("ns-rosetta pair service accounting underflow");
    }
    auto egress_pair = egress.pairs.find(summary.pair);
    if (egress_pair == egress.pairs.end() ||
        egress_pair->second.in_service_bytes < summary.packet_bytes) {
        throw std::logic_error("ns-rosetta egress-pair service accounting underflow");
    }

    ingress.in_service_bytes -= summary.packet_bytes;
    ingress.grant_busy = false;
    pair->second.in_service_bytes -= summary.packet_bytes;
    egress_pair->second.in_service_bytes -= summary.packet_bytes;
    egress.active_grant.reset();

    emit_transition(NsRosettaQueueTransition::SerializationCompleted, summary);
    if (pair->second.buffered_bytes == 0 && pair->second.in_service_bytes == 0) {
        _pairs.erase(pair);
    }
    if (egress_pair->second.buffered_bytes == 0 && egress_pair->second.in_service_bytes == 0) {
        egress.pairs.erase(egress_pair);
    }
    run_arbitration();
}

void NsRosetta::emit_transition(NsRosettaQueueTransition transition, const PacketSummary& packet) {
    std::shared_ptr<NsRosettaBacklogObserver> observer = _backlog_observer;
    if (!observer) {
        return;
    }
    observer->observe(NsRosettaBacklogObservation{
        transition, EventList::now(), static_cast<FatTreeSwitch::switch_type>(getType()), getID(),
        packet.ingress_id, packet.egress_id, packet.traffic_class, packet.pair, packet.flow_id,
        packet.packet_id, packet.packet_bytes,
        voq_buffered_bytes(packet.ingress_id, packet.egress_id,
                           packet.traffic_class == 0   ? Packet::PRIO_HI
                           : packet.traffic_class == 1 ? Packet::PRIO_MID
                                                       : Packet::PRIO_LO),
        ingress_buffered_bytes(packet.ingress_id), ingress_backlog_bytes(packet.ingress_id),
        egress_buffered_bytes(packet.egress_id), egress_backlog_bytes(packet.egress_id),
        egress_pair_buffered_bytes(packet.egress_id, packet.pair),
        egress_pair_backlog_bytes(packet.egress_id, packet.pair), shared_buffer_occupancy()});
}

mem_b NsRosetta::voq_buffered_bytes(uint32_t ingress_id,
                                    uint32_t egress_id,
                                    Packet::PktPriority priority) const {
    validate_ingress(ingress_id);
    const EgressState& egress = egress_state(egress_id);
    const auto& requests = egress.traffic_classes[traffic_class(priority)].by_ingress;
    auto voq = requests.find(ingress_id);
    return voq == requests.end() ? 0 : voq->second.buffered_bytes;
}

mem_b NsRosetta::ingress_buffered_bytes(uint32_t ingress_id) const {
    return ingress_state(ingress_id).buffered_bytes;
}

mem_b NsRosetta::ingress_backlog_bytes(uint32_t ingress_id) const {
    const IngressState& ingress = ingress_state(ingress_id);
    return ingress.buffered_bytes + ingress.in_service_bytes;
}

mem_b NsRosetta::egress_buffered_bytes(uint32_t egress_id) const {
    return egress_state(egress_id).buffered_bytes;
}

mem_b NsRosetta::egress_backlog_bytes(uint32_t egress_id) const {
    const EgressState& egress = egress_state(egress_id);
    return egress.buffered_bytes + egress.serializer->in_service_bytes();
}

mem_b NsRosetta::egress_pair_buffered_bytes(uint32_t egress_id,
                                            const NsRosettaEndpointPair& pair) const {
    const EgressState& egress = egress_state(egress_id);
    auto state = egress.pairs.find(pair);
    return state == egress.pairs.end() ? 0 : state->second.buffered_bytes;
}

mem_b NsRosetta::egress_pair_backlog_bytes(uint32_t egress_id,
                                           const NsRosettaEndpointPair& pair) const {
    const EgressState& egress = egress_state(egress_id);
    auto state = egress.pairs.find(pair);
    return state == egress.pairs.end()
               ? 0
               : state->second.buffered_bytes + state->second.in_service_bytes;
}

mem_b NsRosetta::pair_buffered_bytes(const NsRosettaEndpointPair& pair) const {
    auto state = _pairs.find(pair);
    return state == _pairs.end() ? 0 : state->second.buffered_bytes;
}

mem_b NsRosetta::pair_backlog_bytes(const NsRosettaEndpointPair& pair) const {
    auto state = _pairs.find(pair);
    return state == _pairs.end() ? 0
                                 : state->second.buffered_bytes + state->second.in_service_bytes;
}

NsRosettaRequestDepth NsRosetta::request_depth(uint32_t egress_id,
                                               Packet::PktPriority priority) const {
    const EgressState& egress = egress_state(egress_id);
    NsRosettaRequestDepth depth;
    const auto& requests = egress.traffic_classes[traffic_class(priority)].by_ingress;
    for (const auto& request : requests) {
        if (request.second.packets.empty()) {
            continue;
        }
        depth.requesting_ingresses++;
        depth.queued_packets += request.second.packets.size();
        depth.queued_bytes += request.second.buffered_bytes;
    }
    return depth;
}

void NsRosetta::reserve_ordered_route(const NsRosettaEndpointPair& pair,
                                      uint32_t egress_id,
                                      mem_b wire_bytes) {
    if (pair.source == pair.destination) {
        throw std::invalid_argument("ns-rosetta ordered route reservation requires distinct nodes");
    }
    if (wire_bytes <= 0) {
        throw std::invalid_argument("ns-rosetta ordered route reservation must be positive");
    }
    EgressState& egress = egress_state(egress_id);
    if (egress.ordered_route_reserved_bytes > std::numeric_limits<mem_b>::max() - wire_bytes) {
        throw std::overflow_error("ns-rosetta ordered route reservation overflow");
    }
    auto [reservation, inserted] =
        _ordered_route_reservations.emplace(pair, OrderedRouteReservation{egress_id, wire_bytes});
    (void)reservation;
    if (!inserted) {
        throw std::logic_error("ns-rosetta endpoint pair already has a route reservation");
    }
    egress.ordered_route_reserved_bytes += wire_bytes;
}

void NsRosetta::consume_ordered_route_reservation(const NsRosettaEndpointPair& pair,
                                                  uint32_t egress_id) {
    auto reservation = _ordered_route_reservations.find(pair);
    if (reservation == _ordered_route_reservations.end()) {
        return;
    }
    if (reservation->second.egress_id != egress_id) {
        throw std::logic_error("ns-rosetta ordered pair " + std::to_string(pair.source) + "->" +
                               std::to_string(pair.destination) + " arrived on egress " +
                               std::to_string(egress_id) +
                               " while its DATA reservation targets egress " +
                               std::to_string(reservation->second.egress_id));
    }
    EgressState& egress = egress_state(egress_id);
    if (egress.ordered_route_reserved_bytes < reservation->second.wire_bytes) {
        throw std::logic_error("ns-rosetta ordered route reservation accounting underflow");
    }
    egress.ordered_route_reserved_bytes -= reservation->second.wire_bytes;
    _ordered_route_reservations.erase(reservation);
}

mem_b NsRosetta::ordered_route_reserved_bytes(uint32_t egress_id) const {
    return egress_state(egress_id).ordered_route_reserved_bytes;
}

NsRosettaPathLoad NsRosetta::sample_path_load(uint32_t egress_id) const {
    const EgressState& egress = egress_state(egress_id);
    std::set<uint32_t> requesting_ingresses;
    uint64_t queued_packets = 0;
    for (const TrafficClassVoqs& traffic_class_state : egress.traffic_classes) {
        for (const auto& request : traffic_class_state.by_ingress) {
            if (!request.second.packets.empty()) {
                requesting_ingresses.insert(request.first);
                queued_packets += request.second.packets.size();
            }
        }
    }
    const mem_b in_service = egress.serializer->in_service_bytes();
    if (egress.buffered_bytes >
        std::numeric_limits<mem_b>::max() - egress.ordered_route_reserved_bytes) {
        throw std::overflow_error("ns-rosetta sampled route reservation overflow");
    }
    const mem_b waiting_bytes = egress.buffered_bytes + egress.ordered_route_reserved_bytes;
    const simtime_picosec buffered_delay =
        egress.serializer->serialization_delay_for_bytes(waiting_bytes);
    const simtime_picosec remaining_service = egress.serializer->remaining_service_time_ps();
    if (remaining_service > std::numeric_limits<simtime_picosec>::max() - buffered_delay) {
        throw std::overflow_error("ns-rosetta sampled path-load delay overflow");
    }
    return NsRosettaPathLoad{EventList::now(),
                             egress_id,
                             static_cast<uint32_t>(requesting_ingresses.size()),
                             queued_packets,
                             egress.buffered_bytes,
                             in_service,
                             egress.ordered_route_reserved_bytes,
                             waiting_bytes + in_service,
                             remaining_service + buffered_delay,
                             _shared_buffer_occupancy,
                             _shared_buffer_capacity};
}

void NsRosetta::validate_ingress(uint32_t ingress_id) const {
    if (ingress_id >= _ingresses.size()) {
        throw std::out_of_range("unknown ns-rosetta physical ingress");
    }
}

NsRosetta::IngressState& NsRosetta::ingress_state(uint32_t ingress_id) {
    validate_ingress(ingress_id);
    return _ingresses[ingress_id];
}

const NsRosetta::IngressState& NsRosetta::ingress_state(uint32_t ingress_id) const {
    validate_ingress(ingress_id);
    return _ingresses[ingress_id];
}

NsRosetta::EgressState& NsRosetta::egress_state(uint32_t egress_id) {
    if (egress_id >= _egresses.size()) {
        throw std::out_of_range("unknown ns-rosetta physical egress");
    }
    return _egresses[egress_id];
}

const NsRosetta::EgressState& NsRosetta::egress_state(uint32_t egress_id) const {
    if (egress_id >= _egresses.size()) {
        throw std::out_of_range("unknown ns-rosetta physical egress");
    }
    return _egresses[egress_id];
}
