// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "tomahawk3_switch.h"

#include "fat_tree_topology.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

Tomahawk3IngressPort::Tomahawk3IngressPort(Tomahawk3Switch& owner,
                                           uint32_t ingress_id,
                                           std::string name)
    : _owner(owner),
      _ingress_id(ingress_id),
      _name(std::move(name)) {}

void Tomahawk3IngressPort::receivePacket(Packet& pkt) {
    _owner.receive_from_physical_ingress(pkt, _ingress_id);
}

Tomahawk3EgressSerializer::Tomahawk3EgressSerializer(linkspeed_bps bitrate,
                                           EventList& eventlist,
                                           QueueLogger* logger)
    : BaseQueue(bitrate, eventlist, logger) {
    _nodename = "Tomahawk3EgressSerializer";
}

void Tomahawk3EgressSerializer::bind(Tomahawk3Switch& owner,
                                uint32_t egress_id) {
    if (_owner != nullptr) {
        throw std::logic_error("Tomahawk3 egress serializer already bound");
    }
    _owner = &owner;
    _egress_id = egress_id;
}

void Tomahawk3EgressSerializer::receivePacket(Packet& pkt) {
    if (_owner == nullptr) {
        throw std::logic_error("unbound Tomahawk3 egress serializer");
    }
    if (_authorized_dispatch != &pkt) {
        throw std::logic_error(
            "Tomahawk3 egress serializer cannot bypass the traffic manager");
    }
    if (_packet_in_service != nullptr) {
        throw std::logic_error(
            "Tomahawk3 traffic manager dispatched onto a busy serializer");
    }

    _authorized_dispatch = nullptr;
    _packet_in_service = &pkt;
    eventlist().sourceIsPendingRel(*this, drainTime(&pkt));
}

void Tomahawk3EgressSerializer::dispatch(Packet& pkt) {
    if (_authorized_dispatch != nullptr) {
        throw std::logic_error(
            "Tomahawk3 egress serializer has a pending dispatch");
    }

    _authorized_dispatch = &pkt;
    try {
        PacketSink* selected_sink = pkt.sendOn();
        if (selected_sink != this || _authorized_dispatch != nullptr) {
            throw std::logic_error(
                "Tomahawk3 packet route did not consume its authorized "
                "egress dispatch");
        }
    } catch (...) {
        _authorized_dispatch = nullptr;
        throw;
    }
}

void Tomahawk3EgressSerializer::doNextEvent() {
    if (_packet_in_service == nullptr) {
        throw std::logic_error("idle Tomahawk3 serializer received an event");
    }

    Packet* packet = _packet_in_service;
    const simtime_picosec serialization_time = drainTime(packet);
    _packet_in_service = nullptr;

    packet->flow().logTraffic(*packet, *this, TrafficLogger::PKT_DEPART);
    if (_logger != nullptr) {
        _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *packet);
    }
    log_packet_send(serialization_time);

    packet->sendOn();
    _owner->egress_serialization_complete(_egress_id);
}

mem_b Tomahawk3EgressSerializer::queuesize() const {
    if (_owner == nullptr) {
        return in_service_bytes();
    }
    return _owner->egress_backlog_bytes(_egress_id);
}

mem_b Tomahawk3EgressSerializer::maxsize() const {
    return _owner == nullptr ? 0 : _owner->shared_buffer_capacity();
}

mem_b Tomahawk3EgressSerializer::in_service_bytes() const {
    return _packet_in_service == nullptr ? 0 : _packet_in_service->size();
}

void Tomahawk3EgressSerializer::note_packet_enqueued(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    if (_logger != nullptr) {
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);
    }
}

void Tomahawk3EgressSerializer::note_shared_buffer_drop(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);

    // Admission belongs to the switch-wide shared-buffer domain. A per-egress
    // queue logger cannot represent a drop caused by occupancy on other
    // egresses (and several legacy loggers assume local backlog >= drop size).
    // Tomahawk3BufferCounters are therefore the authoritative drop ledger.
}

Tomahawk3Switch::Tomahawk3Switch(EventList& eventlist, const string& name,
                                 switch_type type, uint32_t id,
                                 simtime_picosec switch_delay,
                                 FatTreeTopology* topology,
                                 mem_b shared_buffer_capacity)
    : FatTreeSwitch(eventlist, name, type, id, switch_delay, topology),
      _shared_buffer_capacity(shared_buffer_capacity) {
    if (_shared_buffer_capacity <= 0) {
        throw std::invalid_argument(
            "Tomahawk3 shared-buffer capacity must be positive");
    }
}

int Tomahawk3Switch::addPort(BaseQueue* queue) {
    auto* serializer = dynamic_cast<Tomahawk3EgressSerializer*>(queue);
    if (serializer == nullptr) {
        throw std::invalid_argument(
            "Tomahawk3 switch ports must be physical egress serializers");
    }

    const int egress_id = FatTreeSwitch::addPort(queue);
    serializer->bind(*this, static_cast<uint32_t>(egress_id));

    EgressState state;
    state.serializer = serializer;
    _egresses.push_back(std::move(state));
    assert(static_cast<size_t>(egress_id) == _egresses.size() - 1);
    return egress_id;
}

PacketSink* Tomahawk3Switch::create_physical_ingress(const string& name) {
    const uint32_t ingress_id =
        static_cast<uint32_t>(_physical_ingresses.size());
    auto ingress =
        std::make_unique<Tomahawk3IngressPort>(*this, ingress_id, name);
    PacketSink* result = ingress.get();
    _physical_ingresses.push_back(std::move(ingress));
    return result;
}

void Tomahawk3Switch::receive_from_physical_ingress(Packet& pkt,
                                                     uint32_t ingress_id) {
    if (pkt.type() == ETH_PAUSE) {
        throw std::invalid_argument(
            "Tomahawk3 model does not support PAUSE/PFC packets");
    }
    if (ingress_id >= _physical_ingresses.size()) {
        throw std::out_of_range("unknown Tomahawk3 physical ingress");
    }
    if (!_pipeline_ingress.emplace(&pkt, ingress_id).second) {
        throw std::logic_error(
            "packet entered the Tomahawk3 switch pipeline twice");
    }

    schedule_through_switch_pipeline(pkt);
}

void Tomahawk3Switch::receivePacket(Packet& pkt) {
    if (pkt.type() == ETH_PAUSE) {
        throw std::invalid_argument(
            "Tomahawk3 model does not support PAUSE/PFC packets");
    }

    auto pending = _pipeline_ingress.find(&pkt);
    if (pending == _pipeline_ingress.end()) {
        throw std::logic_error(
            "Tomahawk3 packets must enter through a physical ingress");
    }
    const uint32_t ingress_id = pending->second;
    _pipeline_ingress.erase(pending);

    Tomahawk3EgressSerializer& egress = resolve_selected_egress(pkt);
    enqueue(pkt, ingress_id, egress);
}

size_t Tomahawk3Switch::traffic_class(Packet::PktPriority priority) {
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

Tomahawk3EgressSerializer& Tomahawk3Switch::resolve_selected_egress(Packet& pkt) {
    if (pkt.route() != nullptr && pkt.nexthop() < pkt.route()->size()) {
        auto* egress =
            dynamic_cast<Tomahawk3EgressSerializer*>(pkt.route()->at(pkt.nexthop()));
        if (egress == nullptr || egress->owner() != this) {
            throw std::logic_error(
                "Tomahawk3 physical ingress is not followed by a local "
                "physical egress");
        }
        return *egress;
    }

    Route* selected_route = FatTreeSwitch::getNextHop(pkt, nullptr);
    if (selected_route == nullptr || selected_route->size() == 0) {
        throw std::logic_error("Tomahawk3 FIB selected an empty route");
    }
    pkt.set_route(*selected_route);

    auto* egress = dynamic_cast<Tomahawk3EgressSerializer*>(selected_route->at(0));
    if (egress == nullptr || egress->owner() != this) {
        throw std::logic_error(
            "Tomahawk3 FIB did not select a local physical egress");
    }
    return *egress;
}

void Tomahawk3Switch::enqueue(Packet& pkt, uint32_t ingress_id,
                              Tomahawk3EgressSerializer& egress) {
    const mem_b packet_bytes = pkt.size();
    if (packet_bytes > _shared_buffer_capacity ||
        _shared_buffer_occupancy >
            _shared_buffer_capacity - packet_bytes) {
        _buffer_counters.dropped_packets++;
        _buffer_counters.dropped_bytes += packet_bytes;
        egress.note_shared_buffer_drop(pkt);
        pkt.free();
        return;
    }

    EgressState& state = egress_state(egress.egress_id());
    state.traffic_classes[traffic_class(pkt.priority())]
        .packets_by_ingress[ingress_id]
        .push_back(&pkt);
    state.buffered_bytes += packet_bytes;
    _shared_buffer_occupancy += packet_bytes;
    _shared_buffer_high_watermark =
        std::max(_shared_buffer_high_watermark, _shared_buffer_occupancy);
    _buffer_counters.admitted_packets++;
    _buffer_counters.admitted_bytes += packet_bytes;
    egress.note_packet_enqueued(pkt);

    schedule_egress(egress.egress_id());
}

Packet* Tomahawk3Switch::select_next_packet(EgressState& egress) {
    for (TrafficClassVoqs& traffic_class : egress.traffic_classes) {
        auto& voqs = traffic_class.packets_by_ingress;
        if (voqs.empty()) {
            continue;
        }

        auto selected = traffic_class.has_last_served_ingress
                            ? voqs.upper_bound(
                                  traffic_class.last_served_ingress)
                            : voqs.begin();
        if (selected == voqs.end()) {
            selected = voqs.begin();
        }

        const uint32_t ingress_id = selected->first;
        Packet* packet = selected->second.front();
        selected->second.pop_front();
        if (selected->second.empty()) {
            voqs.erase(selected);
        }

        traffic_class.has_last_served_ingress = true;
        traffic_class.last_served_ingress = ingress_id;
        return packet;
    }
    return nullptr;
}

void Tomahawk3Switch::schedule_egress(uint32_t egress_id) {
    EgressState& state = egress_state(egress_id);
    if (state.serializer->is_busy()) {
        return;
    }

    Packet* packet = select_next_packet(state);
    if (packet == nullptr) {
        return;
    }

    const mem_b packet_bytes = packet->size();
    if (state.buffered_bytes < packet_bytes ||
        _shared_buffer_occupancy < packet_bytes) {
        throw std::logic_error("Tomahawk3 shared-buffer accounting underflow");
    }
    if (packet->route() == nullptr ||
        packet->nexthop() >= packet->route()->size() ||
        packet->route()->at(packet->nexthop()) != state.serializer) {
        throw std::logic_error(
            "Tomahawk3 dequeued packet does not target its serializer");
    }

    state.buffered_bytes -= packet_bytes;
    _shared_buffer_occupancy -= packet_bytes;
    _buffer_counters.dequeued_packets++;
    _buffer_counters.dequeued_bytes += packet_bytes;

    state.serializer->dispatch(*packet);
}

void Tomahawk3Switch::egress_serialization_complete(uint32_t egress_id) {
    schedule_egress(egress_id);
}

mem_b Tomahawk3Switch::egress_buffered_bytes(uint32_t egress_id) const {
    return egress_state(egress_id).buffered_bytes;
}

mem_b Tomahawk3Switch::egress_backlog_bytes(uint32_t egress_id) const {
    const EgressState& state = egress_state(egress_id);
    return state.buffered_bytes + state.serializer->in_service_bytes();
}

Tomahawk3Switch::EgressState& Tomahawk3Switch::egress_state(
    uint32_t egress_id) {
    if (egress_id >= _egresses.size()) {
        throw std::out_of_range("unknown Tomahawk3 physical egress");
    }
    return _egresses[egress_id];
}

const Tomahawk3Switch::EgressState& Tomahawk3Switch::egress_state(
    uint32_t egress_id) const {
    if (egress_id >= _egresses.size()) {
        throw std::out_of_range("unknown Tomahawk3 physical egress");
    }
    return _egresses[egress_id];
}
