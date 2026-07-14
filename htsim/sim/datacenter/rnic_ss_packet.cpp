// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ss_packet.h"

#include <limits>
#include <stdexcept>
#include <utility>

PacketDB<RnicSsPacket> RnicSsPacket::_packet_db;

RnicSsPacket* RnicSsPacket::newPacket(
        PacketFlow& flow,
        const Route& route,
        packetid_t packet_id,
        RnicSsWirePacketMetadata metadata,
        AtlahsFlowId atlahs_flow_id,
        std::uint64_t payload_byte_offset,
        bool retransmission,
        std::shared_ptr<RnicSsPacketLifecycleObserver> observer) {
    if (route.size() == 0) {
        throw std::invalid_argument("rnic-ss packet requires a physical route");
    }
    if (!observer) {
        throw std::invalid_argument("rnic-ss packet requires a lifecycle observer");
    }
    validateRnicSsWirePacket(metadata);
    if (metadata.wire_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("rnic-ss wire extent exceeds HTSIM Packet");
    }

    RnicSsPacket* packet = _packet_db.allocPacket();
    packet->initialize(flow, route, packet_id, std::move(metadata),
                       atlahs_flow_id, payload_byte_offset, retransmission,
                       std::move(observer));
    return packet;
}

void RnicSsPacket::initialize(
        PacketFlow& flow,
        const Route& route,
        packetid_t packet_id,
        RnicSsWirePacketMetadata metadata,
        AtlahsFlowId atlahs_flow_id,
        std::uint64_t payload_byte_offset,
        bool retransmission,
        std::shared_ptr<RnicSsPacketLifecycleObserver> observer) {
    if (_pool_state != PoolState::Recycled || ref_count() != 1) {
        throw std::logic_error("rnic-ss packet pool returned a live packet");
    }
    Packet::set_route(flow, route, static_cast<int>(metadata.wire_bytes),
                      packet_id);
    _type = IP;
    _is_header = false;
    _bounced = false;
    _src = metadata.wire_source;
    _dst = metadata.wire_destination;
    _pathid = UINT32_MAX;
    _hop_count = 0;
    _direction = NONE;
    _next_routed_hop = nullptr;
    _ingressqueue = nullptr;
    _path_len = static_cast<std::uint32_t>(route.size());

    _metadata = std::move(metadata);
    _atlahs_flow_id = atlahs_flow_id;
    _payload_byte_offset = payload_byte_offset;
    _retransmission = retransmission;
    _accumulated_queue_delay_ps = 0;
    _load_observed_at_ps = 0;
    _observer = std::move(observer);
    _pool_state = PoolState::InFlight;
}

void RnicSsPacket::addQueueResidence(
        std::uint64_t queue_delay_ps,
        std::uint64_t observed_at_ps) noexcept {
    if (queue_delay_ps
        > std::numeric_limits<std::uint64_t>::max()
              - _accumulated_queue_delay_ps) {
        _accumulated_queue_delay_ps =
            std::numeric_limits<std::uint64_t>::max();
    } else {
        _accumulated_queue_delay_ps += queue_delay_ps;
    }
    if (observed_at_ps > _load_observed_at_ps) {
        _load_observed_at_ps = observed_at_ps;
    }
}

Packet::PktPriority RnicSsPacket::priority() const {
    return kind() == RnicSsPacketKind::DATA
        ? Packet::PRIO_LO : Packet::PRIO_HI;
}

void RnicSsPacket::consumeAtEndpoint() {
    if (_pool_state != PoolState::InFlight) {
        throw std::logic_error("rnic-ss packet was consumed twice");
    }
    if (route() == nullptr || nexthop() != route()->size()) {
        throw std::logic_error("rnic-ss packet was consumed before route end");
    }
    terminate(false);
}

void RnicSsPacket::free() {
    terminate(true);
}

void RnicSsPacket::terminate(bool fabric_drop) {
    if (_pool_state != PoolState::InFlight || !_observer) {
        throw std::logic_error("rnic-ss packet terminal state is invalid");
    }
    const RnicSsPacketTermination termination{id(), fabric_drop};
    std::shared_ptr<RnicSsPacketLifecycleObserver> observer =
        std::move(_observer);
    _metadata.reset();
    _pool_state = PoolState::Recycled;
    _packet_db.freePacket(this);
    observer->observe(termination);
}

void RnicSsPacket::strip_payload(std::uint16_t) {
    throw std::logic_error("rnic-ss packets cannot be trimmed");
}

void RnicSsPacket::bounce() {
    throw std::logic_error("rnic-ss packets do not use PFC bounce");
}

void RnicSsPacket::unbounce(std::uint16_t) {
    throw std::logic_error("rnic-ss packets do not use PFC bounce");
}
