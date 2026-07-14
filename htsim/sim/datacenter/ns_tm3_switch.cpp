// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_tm3_switch.h"

#include "eth_pause_packet.h"
#include "fat_tree_topology.h"
#include "ns_tm3_dcqcn_policy.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <stdexcept>
#include <utility>

NsTm3IngressPort::NsTm3IngressPort(NsTm3Switch& owner, uint32_t ingress_id, std::string name)
    : _owner(owner), _ingress_id(ingress_id), _name(std::move(name)) {}

void NsTm3IngressPort::receivePacket(Packet& pkt) {
    _owner.receive_from_physical_ingress(pkt, _ingress_id);
}

NsTm3EgressSerializer::NsTm3EgressSerializer(linkspeed_bps bitrate,
                                             EventList& eventlist,
                                             QueueLogger* logger)
    : BaseQueue(bitrate, eventlist, logger) {
    _nodename = "ns-tm3-egress-serializer";
}

void NsTm3EgressSerializer::bind(NsTm3Switch& owner, uint32_t egress_id) {
    if (_owner != nullptr) {
        throw std::logic_error("ns-tm3 egress serializer already bound");
    }
    _owner = &owner;
    _egress_id = egress_id;
}

void NsTm3EgressSerializer::receivePacket(Packet& pkt) {
    if (_owner == nullptr) {
        throw std::logic_error("unbound ns-tm3 egress serializer");
    }
    if (pkt.type() == ETH_PAUSE) {
        const NsTm3DcqcnPolicy* policy = _owner->dcqcn_policy();
        if (policy == nullptr || !policy->config().pfc_enabled) {
            throw std::invalid_argument(
                "ns-tm3 egress accepts PFC only with the opt-in DCQCN "
                "policy");
        }
        const auto& pause = static_cast<const EthPausePacket&>(pkt);
        _data_paused = pause.sleepTime() > 0;
        pkt.free();
        _owner->egress_pause_state_changed(_egress_id);
        return;
    }
    if (_authorized_dispatch != &pkt) {
        throw std::logic_error("ns-tm3 egress serializer cannot bypass the traffic manager");
    }
    if (_packet_in_service != nullptr) {
        throw std::logic_error("ns-tm3 traffic manager dispatched onto a busy serializer");
    }

    _authorized_dispatch = nullptr;
    _packet_in_service = &pkt;
    eventlist().sourceIsPendingRel(*this, drainTime(&pkt));
}

void NsTm3EgressSerializer::dispatch(Packet& pkt) {
    if (_authorized_dispatch != nullptr) {
        throw std::logic_error("ns-tm3 egress serializer has a pending dispatch");
    }

    _authorized_dispatch = &pkt;
    try {
        PacketSink* selected_sink = pkt.sendOn();
        if (selected_sink != this || _authorized_dispatch != nullptr) {
            throw std::logic_error(
                "ns-tm3 packet route did not consume its authorized "
                "egress dispatch");
        }
    } catch (...) {
        _authorized_dispatch = nullptr;
        throw;
    }
}

void NsTm3EgressSerializer::doNextEvent() {
    if (_packet_in_service == nullptr) {
        throw std::logic_error("idle ns-tm3 serializer received an event");
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

mem_b NsTm3EgressSerializer::queuesize() const {
    if (_owner == nullptr) {
        return in_service_bytes();
    }
    return _owner->egress_backlog_bytes(_egress_id);
}

mem_b NsTm3EgressSerializer::maxsize() const {
    return _owner == nullptr ? 0 : _owner->egress_buffer_capacity();
}

mem_b NsTm3EgressSerializer::in_service_bytes() const {
    return _packet_in_service == nullptr ? 0 : _packet_in_service->size();
}

void NsTm3EgressSerializer::note_packet_enqueued(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_ARRIVE);
    if (_logger != nullptr) {
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, pkt);
    }
}

void NsTm3EgressSerializer::note_buffer_drop(Packet& pkt) {
    pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);

    // Admission belongs jointly to a switch-wide shared pool and a local
    // egress domain.  Several legacy queue loggers assume local backlog >=
    // drop size, which is false for shared-pool drops.  NsTm3BufferCounters
    // are therefore the authoritative, domain-specific drop ledger.
}

NsTm3Switch::NsTm3Switch(EventList& eventlist,
                         const string& name,
                         switch_type type,
                         uint32_t id,
                         simtime_picosec switch_delay,
                         FatTreeTopology* topology,
                         mem_b shared_buffer_capacity)
    : FatTreeSwitch(eventlist, name, type, id, switch_delay, topology),
      _shared_buffer_capacity(shared_buffer_capacity),
      _egress_buffer_capacity(shared_buffer_capacity) {
    if (_shared_buffer_capacity <= 0) {
        throw std::invalid_argument("ns-tm3 shared-buffer capacity must be positive");
    }
}

NsTm3Switch::~NsTm3Switch() = default;

void NsTm3Switch::configure_dcqcn_policy(const NsTm3DcqcnPolicyConfig& config) {
    if (_dcqcn_policy != nullptr) {
        throw std::logic_error("ns-tm3 DCQCN policy already configured");
    }
    const std::uint64_t ecn_domain_id = (static_cast<std::uint64_t>(getType()) << 32) | getID();
    _dcqcn_policy = std::make_unique<NsTm3DcqcnPolicy>(eventlist(), config, ecn_domain_id);
}

void NsTm3Switch::set_voq_arbitration(NsTm3VoqArbitration arbitration) {
    if (_shared_buffer_occupancy != 0) {
        throw std::logic_error("ns-tm3 cannot change VoQ arbitration with buffered packets");
    }
    for (const EgressState& egress : _egresses) {
        if (egress.serializer->is_busy()) {
            throw std::logic_error("ns-tm3 cannot change VoQ arbitration while serializing");
        }
    }
    _voq_arbitration = arbitration;
}

void NsTm3Switch::set_egress_buffer_capacity(mem_b capacity) {
    if (capacity <= 0 || capacity > _shared_buffer_capacity) {
        throw std::invalid_argument(
            "ns-tm3 egress-buffer capacity must be positive and no larger "
            "than the switch-wide shared pool");
    }
    if (_shared_buffer_occupancy != 0 || !_pipeline_ingress.empty()) {
        throw std::logic_error(
            "ns-tm3 cannot change egress-buffer capacity with packets in "
            "the traffic manager");
    }
    for (const EgressState& egress : _egresses) {
        if (egress.serializer->is_busy()) {
            throw std::logic_error(
                "ns-tm3 cannot change egress-buffer capacity while "
                "serializing");
        }
    }
    _egress_buffer_capacity = capacity;
}

int NsTm3Switch::addPort(BaseQueue* queue) {
    auto* serializer = dynamic_cast<NsTm3EgressSerializer*>(queue);
    if (serializer == nullptr) {
        throw std::invalid_argument("ns-tm3 switch ports must be physical egress serializers");
    }

    const int egress_id = FatTreeSwitch::addPort(queue);
    serializer->bind(*this, static_cast<uint32_t>(egress_id));

    EgressState state;
    state.serializer = serializer;
    _egresses.push_back(std::move(state));
    assert(static_cast<size_t>(egress_id) == _egresses.size() - 1);
    return egress_id;
}

PacketSink* NsTm3Switch::create_physical_ingress(const string& name) {
    const uint32_t ingress_id = static_cast<uint32_t>(_physical_ingresses.size());
    auto ingress = std::make_unique<NsTm3IngressPort>(*this, ingress_id, name);
    PacketSink* result = ingress.get();
    _physical_ingresses.push_back(std::move(ingress));
    return result;
}

void NsTm3Switch::receive_from_physical_ingress(Packet& pkt, uint32_t ingress_id) {
    if (pkt.type() == ETH_PAUSE) {
        throw std::invalid_argument("ns-tm3 model does not support PAUSE/PFC packets");
    }
    if (ingress_id >= _physical_ingresses.size()) {
        throw std::out_of_range("unknown ns-tm3 physical ingress");
    }
    if (!_pipeline_ingress.emplace(&pkt, ingress_id).second) {
        throw std::logic_error("packet entered the ns-tm3 switch pipeline twice");
    }
    if (_dcqcn_policy != nullptr) {
        _dcqcn_policy->observe_physical_ingress(pkt, ingress_id);
    }

    schedule_through_switch_pipeline(pkt);
}

void NsTm3Switch::receivePacket(Packet& pkt) {
    if (pkt.type() == ETH_PAUSE) {
        throw std::invalid_argument("ns-tm3 model does not support PAUSE/PFC packets");
    }

    auto pending = _pipeline_ingress.find(&pkt);
    if (pending == _pipeline_ingress.end()) {
        throw std::logic_error("ns-tm3 packets must enter through a physical ingress");
    }
    const uint32_t ingress_id = pending->second;
    _pipeline_ingress.erase(pending);

    NsTm3EgressSerializer& egress = resolve_selected_egress(pkt);
    enqueue(pkt, ingress_id, egress);
}

size_t NsTm3Switch::traffic_class(Packet::PktPriority priority) {
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

NsTm3EgressSerializer& NsTm3Switch::resolve_selected_egress(Packet& pkt) {
    if (pkt.route() != nullptr && pkt.nexthop() < pkt.route()->size()) {
        auto* egress = dynamic_cast<NsTm3EgressSerializer*>(pkt.route()->at(pkt.nexthop()));
        if (egress == nullptr || egress->owner() != this) {
            throw std::logic_error(
                "ns-tm3 physical ingress is not followed by a local "
                "physical egress");
        }
        return *egress;
    }

    Route* selected_route = FatTreeSwitch::getNextHop(pkt, nullptr);
    if (selected_route == nullptr || selected_route->size() == 0) {
        throw std::logic_error("ns-tm3 FIB selected an empty route");
    }
    pkt.set_route(*selected_route);

    auto* egress = dynamic_cast<NsTm3EgressSerializer*>(selected_route->at(0));
    if (egress == nullptr || egress->owner() != this) {
        throw std::logic_error("ns-tm3 FIB did not select a local physical egress");
    }
    return *egress;
}

void NsTm3Switch::enqueue(Packet& pkt, uint32_t ingress_id, NsTm3EgressSerializer& egress) {
    const mem_b packet_bytes = pkt.size();
    EgressState& state = egress_state(egress.egress_id());
    const PacketSummary packet{ingress_id,    egress.egress_id(), pkt.priority(),
                               pkt.flow_id(), pkt.id(),           packet_bytes};
    if (packet_bytes > _shared_buffer_capacity ||
        _shared_buffer_occupancy > _shared_buffer_capacity - packet_bytes) {
        _buffer_counters.dropped_packets++;
        _buffer_counters.dropped_bytes += packet_bytes;
        _buffer_counters.shared_pool_dropped_packets++;
        _buffer_counters.shared_pool_dropped_bytes += packet_bytes;
        egress.note_buffer_drop(pkt);
        emit_queue_observation(NsTm3QueueTransition::Dropped, packet);
        pkt.free();
        return;
    }
    if (packet_bytes > _egress_buffer_capacity ||
        state.buffered_bytes > _egress_buffer_capacity - packet_bytes) {
        _buffer_counters.dropped_packets++;
        _buffer_counters.dropped_bytes += packet_bytes;
        _buffer_counters.egress_domain_dropped_packets++;
        _buffer_counters.egress_domain_dropped_bytes += packet_bytes;
        egress.note_buffer_drop(pkt);
        emit_queue_observation(NsTm3QueueTransition::Dropped, packet);
        pkt.free();
        return;
    }

    state.traffic_classes[traffic_class(pkt.priority())].packets_by_ingress[ingress_id].push_back(
        &pkt);
    state.buffered_bytes += packet_bytes;
    _shared_buffer_occupancy += packet_bytes;
    if (!state.enqueue_time_ps.emplace(&pkt, EventList::now()).second) {
        throw std::logic_error("packet entered the same ns-tm3 egress VoQ twice");
    }
    state.statistics.buffered_high_watermark =
        std::max(state.statistics.buffered_high_watermark, state.buffered_bytes);
    state.statistics.backlog_high_watermark =
        std::max(state.statistics.backlog_high_watermark,
                 state.buffered_bytes + state.serializer->in_service_bytes());
    _shared_buffer_high_watermark =
        std::max(_shared_buffer_high_watermark, _shared_buffer_occupancy);
    _buffer_counters.admitted_packets++;
    _buffer_counters.admitted_bytes += packet_bytes;
    if (_dcqcn_policy != nullptr) {
        _dcqcn_policy->packet_enqueued(pkt, ingress_id);
    }
    egress.note_packet_enqueued(pkt);
    emit_queue_observation(NsTm3QueueTransition::Enqueued, packet);

    schedule_egress(egress.egress_id());
}

std::optional<NsTm3Switch::SelectedPacket> NsTm3Switch::select_next_packet(EgressState& egress) {
    for (std::size_t class_index = 0; class_index < egress.traffic_classes.size(); ++class_index) {
        if (class_index == traffic_class(Packet::PRIO_LO) && egress.serializer->data_is_paused()) {
            continue;
        }
        TrafficClassVoqs& traffic_class = egress.traffic_classes[class_index];
        auto& voqs = traffic_class.packets_by_ingress;
        if (voqs.empty()) {
            continue;
        }

        auto selected = voqs.begin();
        if (_voq_arbitration == NsTm3VoqArbitration::IngressRoundRobin) {
            selected = traffic_class.has_last_served_ingress
                           ? voqs.upper_bound(traffic_class.last_served_ingress)
                           : voqs.begin();
            if (selected == voqs.end()) {
                selected = voqs.begin();
            }
        } else {
            auto selected_time = egress.enqueue_time_ps.find(selected->second.front());
            if (selected_time == egress.enqueue_time_ps.end()) {
                throw std::logic_error("ns-tm3 oldest-HOL arbitration lost an enqueue time");
            }
            for (auto candidate = std::next(selected); candidate != voqs.end(); ++candidate) {
                auto candidate_time = egress.enqueue_time_ps.find(candidate->second.front());
                if (candidate_time == egress.enqueue_time_ps.end()) {
                    throw std::logic_error("ns-tm3 oldest-HOL arbitration lost an enqueue time");
                }
                if (candidate_time->second < selected_time->second) {
                    selected = candidate;
                    selected_time = candidate_time;
                }
            }
        }

        const uint32_t ingress_id = selected->first;
        Packet* packet = selected->second.front();
        selected->second.pop_front();
        if (selected->second.empty()) {
            voqs.erase(selected);
        }

        traffic_class.has_last_served_ingress = true;
        traffic_class.last_served_ingress = ingress_id;

        auto queued = egress.enqueue_time_ps.find(packet);
        if (queued == egress.enqueue_time_ps.end() || queued->second > EventList::now()) {
            throw std::logic_error("ns-tm3 egress lost its packet enqueue timestamp");
        }
        const simtime_picosec wait_ps = EventList::now() - queued->second;
        egress.enqueue_time_ps.erase(queued);
        if (wait_ps > egress.statistics.max_queue_wait_ps) {
            egress.statistics.max_queue_wait_ps = wait_ps;
            egress.statistics.max_queue_wait_observed_ps = EventList::now();
            egress.statistics.max_queue_wait_ingress_id = ingress_id;
            egress.statistics.max_queue_wait_flow_id = packet->flow_id();
            egress.statistics.max_queue_wait_packet_id = packet->id();
        }
        if (_dcqcn_policy != nullptr) {
            _dcqcn_policy->packet_selected(*packet, ingress_id, egress.serializer->egress_id(),
                                           egress.buffered_bytes);
        }
        return SelectedPacket{packet, ingress_id};
    }
    return std::nullopt;
}

void NsTm3Switch::schedule_egress(uint32_t egress_id) {
    EgressState& state = egress_state(egress_id);
    if (state.serializer->is_busy()) {
        return;
    }

    const std::optional<SelectedPacket> selected = select_next_packet(state);
    if (!selected.has_value()) {
        return;
    }
    Packet* packet = selected->packet;

    const mem_b packet_bytes = packet->size();
    if (state.buffered_bytes < packet_bytes || _shared_buffer_occupancy < packet_bytes) {
        throw std::logic_error("ns-tm3 shared-buffer accounting underflow");
    }
    if (packet->route() == nullptr || packet->nexthop() >= packet->route()->size() ||
        packet->route()->at(packet->nexthop()) != state.serializer) {
        throw std::logic_error("ns-tm3 dequeued packet does not target its serializer");
    }

    state.buffered_bytes -= packet_bytes;
    _shared_buffer_occupancy -= packet_bytes;
    _buffer_counters.dequeued_packets++;
    _buffer_counters.dequeued_bytes += packet_bytes;

    if (state.active_packet.has_value()) {
        throw std::logic_error("ns-tm3 idle serializer retained an active packet summary");
    }
    state.active_packet = PacketSummary{selected->ingress_id, egress_id,    packet->priority(),
                                        packet->flow_id(),    packet->id(), packet_bytes};
    state.serializer->dispatch(*packet);
    emit_queue_observation(NsTm3QueueTransition::Dequeued, *state.active_packet);
}

void NsTm3Switch::egress_serialization_complete(uint32_t egress_id) {
    EgressState& state = egress_state(egress_id);
    if (!state.active_packet.has_value()) {
        throw std::logic_error("ns-tm3 serializer completed without an active packet summary");
    }
    emit_queue_observation(NsTm3QueueTransition::SerializationCompleted, *state.active_packet);
    state.active_packet.reset();
    schedule_egress(egress_id);
}

void NsTm3Switch::egress_pause_state_changed(uint32_t egress_id) {
    schedule_egress(egress_id);
}

void NsTm3Switch::emit_queue_observation(NsTm3QueueTransition transition,
                                         const PacketSummary& packet) noexcept {
    std::shared_ptr<NsTm3QueueObserver> observer = _queue_observer;
    if (!observer) {
        return;
    }
    const EgressState& state = _egresses[packet.egress_id];
    const mem_b in_service = state.serializer->in_service_bytes();
    observer->observe(NsTm3QueueObservation{
        transition, EventList::now(), static_cast<FatTreeSwitch::switch_type>(getType()), getID(),
        packet.ingress_id, packet.egress_id, packet.priority, packet.flow_id, packet.packet_id,
        packet.packet_bytes, state.buffered_bytes, in_service, state.buffered_bytes + in_service,
        _shared_buffer_occupancy});
}

mem_b NsTm3Switch::egress_buffered_bytes(uint32_t egress_id) const {
    return egress_state(egress_id).buffered_bytes;
}

mem_b NsTm3Switch::egress_backlog_bytes(uint32_t egress_id) const {
    const EgressState& state = egress_state(egress_id);
    return state.buffered_bytes + state.serializer->in_service_bytes();
}

const NsTm3EgressStatistics& NsTm3Switch::egress_statistics(uint32_t egress_id) const {
    return egress_state(egress_id).statistics;
}

NsTm3Switch::EgressState& NsTm3Switch::egress_state(uint32_t egress_id) {
    if (egress_id >= _egresses.size()) {
        throw std::out_of_range("unknown ns-tm3 physical egress");
    }
    return _egresses[egress_id];
}

const NsTm3Switch::EgressState& NsTm3Switch::egress_state(uint32_t egress_id) const {
    if (egress_id >= _egresses.size()) {
        throw std::out_of_range("unknown ns-tm3 physical egress");
    }
    return _egresses[egress_id];
}
