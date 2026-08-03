// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_packet.h"

#include <limits>
#include <stdexcept>
#include <utility>

PacketDB<RnicCollectivePacket> RnicCollectivePacket::_packetdb;
std::uint64_t RnicCollectivePacket::_next_lifecycle_id = 1;

std::uint16_t RnicCollectivePacket::checkedWireBytes(std::uint64_t wire_bytes) {
    if (wire_bytes == 0 || wire_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("rnic-cn packet wire extent must fit exactly in uint16_t");
    }
    return static_cast<std::uint16_t>(wire_bytes);
}

void RnicCollectivePacket::validateRoute(const Route& route) {
    if (route.size() == 0) {
        throw std::invalid_argument("rnic-cn packet requires a nonempty explicit route");
    }
    if (route.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("rnic-cn packet route length exceeds the HTSIM hop ledger");
    }
}

void RnicCollectivePacket::validateFinalLedger(const RnicCollectiveFinalLedger& final_ledger) {
    if (final_ledger.total_data_packets == 0) {
        if (final_ledger.total_payload_bytes != 0 || final_ledger.total_wire_bytes != 0) {
            throw std::invalid_argument("rnic-cn empty DATA ledger must have zero byte totals");
        }
        return;
    }

    if (final_ledger.total_payload_bytes == 0) {
        throw std::invalid_argument("rnic-cn nonempty DATA ledger requires payload bytes");
    }
    if (final_ledger.total_wire_bytes < final_ledger.total_payload_bytes) {
        throw std::invalid_argument("rnic-cn final wire ledger cannot be smaller than payload");
    }
    if (final_ledger.total_data_packets > final_ledger.total_payload_bytes) {
        throw std::invalid_argument(
            "rnic-cn DATA ledger requires positive payload in every packet");
    }
    if (final_ledger.total_data_packets > final_ledger.total_wire_bytes) {
        throw std::invalid_argument("rnic-cn DATA ledger requires positive wire extent per packet");
    }
}

void RnicCollectivePacket::validateData(const RnicCollectiveDataMetadata& metadata) {
    validateFinalLedger(metadata.final_ledger);
    const RnicCollectiveFinalLedger& ledger = metadata.final_ledger;
    if (ledger.total_data_packets == 0) {
        throw std::invalid_argument("rnic-cn DATA packet cannot use an empty final ledger");
    }
    if (metadata.extent.payloadBytes() == 0) {
        throw std::invalid_argument("rnic-cn DATA packet must advance the payload ledger");
    }
    checkedWireBytes(metadata.extent.wireBytes());
    if (metadata.packet_index >= ledger.total_data_packets) {
        throw std::out_of_range("rnic-cn DATA packet index exceeds its final ledger");
    }
    if ((metadata.packet_index == 0) != (metadata.payload_byte_offset == 0)) {
        throw std::invalid_argument("rnic-cn DATA packet zero index and zero offset must agree");
    }
    if (metadata.payload_byte_offset < metadata.packet_index) {
        throw std::invalid_argument("rnic-cn DATA offset cannot fit preceding positive payloads");
    }
    if (metadata.payload_byte_offset > ledger.total_payload_bytes ||
        metadata.extent.payloadBytes() >
            ledger.total_payload_bytes - metadata.payload_byte_offset) {
        throw std::invalid_argument("rnic-cn DATA payload extent exceeds its final ledger");
    }

    const std::uint64_t payload_end = metadata.payload_byte_offset + metadata.extent.payloadBytes();
    const std::uint64_t remaining_payload = ledger.total_payload_bytes - payload_end;
    const std::uint64_t remaining_packets = ledger.total_data_packets - metadata.packet_index - 1;
    if (remaining_payload < remaining_packets) {
        throw std::invalid_argument("rnic-cn DATA final ledger cannot fit remaining packets");
    }
    if ((remaining_packets == 0) != (remaining_payload == 0)) {
        throw std::invalid_argument("rnic-cn final DATA index and payload end must agree");
    }

    const std::uint64_t other_packets = ledger.total_data_packets - 1;
    if (metadata.extent.wireBytes() > ledger.total_wire_bytes ||
        other_packets > ledger.total_wire_bytes - metadata.extent.wireBytes()) {
        throw std::invalid_argument("rnic-cn DATA wire extent cannot fit its final ledger");
    }
}

void RnicCollectivePacket::validateDeclaration(const RnicCollectiveDeclareMetadata& metadata) {
    if (metadata.nflow_ppm == 0 || metadata.nflow_ppm > 1000000) {
        throw std::invalid_argument(
            "rnic-cn DECLARE nflow_ppm must be in [1, one flow]");
    }
}

void RnicCollectivePacket::validateGrant(const RnicCollectiveGrant& grant,
                                         RnicCollectivePacketKind packet_kind) {
    RnicCollectiveGrantKind expected_kind;
    if (packet_kind == RnicCollectivePacketKind::ACCEPT) {
        expected_kind = RnicCollectiveGrantKind::Accept;
    } else if (packet_kind == RnicCollectivePacketKind::GRANT_UPDATE) {
        expected_kind = RnicCollectiveGrantKind::Update;
    } else {
        throw std::logic_error("rnic-cn grant validation requires a grant packet kind");
    }

    if (grant.kind != expected_kind) {
        throw std::invalid_argument("rnic-cn grant kind does not match its wire packet kind");
    }
    if (grant.membership_epoch == 0) {
        throw std::invalid_argument("rnic-cn in-band grant requires a nonzero membership epoch");
    }
    if (grant.n_hat == 0 || grant.wire_rate_bps == 0) {
        throw std::invalid_argument("rnic-cn in-band grant requires positive N_hat and wire rate");
    }
    if (grant.effective_time_ps == 0) {
        throw std::invalid_argument(
            "rnic-cn in-band grant requires a governed dwnd boundary");
    }
    // feedback_deadline_ps and lease_expiry_ps are vestigial, kept for
    // wire-format stability; leases were removed and both fields are always
    // 0. Removal is tracked in the algorithm book (section 3, D2).
    if (grant.feedback_deadline_ps != 0 || grant.lease_expiry_ps != 0) {
        throw std::invalid_argument(
            "rnic-cn in-band grant carries nonzero vestigial lease fields");
    }
}

void RnicCollectivePacket::validateGapNack(const RnicCollectiveGapNackMetadata& gap_nack) {
    if (gap_nack.extent.payloadBytes() == 0 || gap_nack.extent.wireBytes() == 0) {
        throw std::invalid_argument("rnic-cn GAP_NACK requires a nonempty logical packet");
    }
    checkedWireBytes(gap_nack.extent.wireBytes());
    if (gap_nack.requested_transmission_attempt == 0) {
        throw std::invalid_argument("rnic-cn GAP_NACK requires a nonzero transmission attempt");
    }
}

void RnicCollectivePacket::validateGapResolved(
    const RnicCollectiveGapResolvedMetadata& gap_resolved) {
    if (gap_resolved.extent.payloadBytes() == 0 || gap_resolved.extent.wireBytes() == 0) {
        throw std::invalid_argument("rnic-cn GAP_RESOLVED requires a nonempty logical packet");
    }
    checkedWireBytes(gap_resolved.extent.wireBytes());
    if (gap_resolved.acknowledged_transmission_attempt == 0) {
        throw std::invalid_argument("rnic-cn GAP_RESOLVED requires a nonzero retry attempt");
    }
}

std::uint64_t RnicCollectivePacket::takeLifecycleId() {
    if (_next_lifecycle_id == 0) {
        throw std::overflow_error("rnic-cn packet lifecycle id exhausted");
    }
    return _next_lifecycle_id++;
}

RnicCollectivePacket* RnicCollectivePacket::newData(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    const RnicCollectiveDataMetadata& metadata,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateData(metadata);
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::DATA, flow_id, source,
                     destination, checkedWireBytes(metadata.extent.wireBytes()), metadata,
                     std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                     std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newDeclare(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint64_t wire_bytes,
    const RnicCollectiveDeclareMetadata& metadata,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateDeclaration(metadata);
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::DECLARE, flow_id,
                     source, destination, checkedWireBytes(wire_bytes), std::nullopt, metadata,
                     std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newAccept(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint64_t wire_bytes,
    const RnicCollectiveGrant& grant,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateGrant(grant, RnicCollectivePacketKind::ACCEPT);
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::ACCEPT, grant.flow_id,
                     source, destination, checkedWireBytes(wire_bytes), std::nullopt, std::nullopt,
                     grant, std::nullopt, std::nullopt, std::nullopt, std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newGrantUpdate(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint64_t wire_bytes,
    const RnicCollectiveGrant& grant,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateGrant(grant, RnicCollectivePacketKind::GRANT_UPDATE);
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::GRANT_UPDATE,
                     grant.flow_id, source, destination, checkedWireBytes(wire_bytes), std::nullopt,
                     std::nullopt, grant, std::nullopt, std::nullopt, std::nullopt,
                     std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newGapNack(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint64_t wire_bytes,
    const RnicCollectiveGapNackMetadata& gap_nack,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateGapNack(gap_nack);
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::GAP_NACK, flow_id,
                     source, destination, checkedWireBytes(wire_bytes), std::nullopt, std::nullopt,
                     std::nullopt, gap_nack, std::nullopt, std::nullopt, std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newGapResolved(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint64_t wire_bytes,
    const RnicCollectiveGapResolvedMetadata& gap_resolved,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateGapResolved(gap_resolved);
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::GAP_RESOLVED, flow_id,
                     source, destination, checkedWireBytes(wire_bytes), std::nullopt, std::nullopt,
                     std::nullopt, std::nullopt, std::nullopt, gap_resolved, std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newRetire(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint64_t wire_bytes,
    const RnicCollectiveRetireMetadata& retire,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateFinalLedger(retire.final_ledger);
    if (retire.final_ledger.total_data_packets != 0 && retire.final_gap_detection_ps == 0) {
        throw std::invalid_argument("rnic-cn nonempty RETIRE requires a tail-gap deadline");
    }
    return newPacket(flow, route, htsim_packet_id, RnicCollectivePacketKind::RETIRE, flow_id,
                     source, destination, checkedWireBytes(wire_bytes), std::nullopt, std::nullopt,
                     std::nullopt, std::nullopt, retire, std::nullopt, std::move(observer));
}

RnicCollectivePacket* RnicCollectivePacket::newPacket(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    RnicCollectivePacketKind kind,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint16_t wire_bytes,
    std::optional<RnicCollectiveDataMetadata> data,
    std::optional<RnicCollectiveDeclareMetadata> declaration,
    std::optional<RnicCollectiveGrant> grant,
    std::optional<RnicCollectiveGapNackMetadata> gap_nack,
    std::optional<RnicCollectiveRetireMetadata> retire,
    std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer) {
    validateRoute(route);
    if (!observer) {
        throw std::invalid_argument("rnic-cn packet requires a lifecycle observer");
    }

    const bool has_data = data.has_value();
    const bool has_declaration = declaration.has_value();
    const bool has_grant = grant.has_value();
    const bool has_gap_nack = gap_nack.has_value();
    const bool has_retire = retire.has_value();
    const bool has_gap_resolved = gap_resolved.has_value();
    switch (kind) {
        case RnicCollectivePacketKind::DATA:
            if (!has_data || has_declaration || has_grant || has_gap_nack || has_retire ||
                has_gap_resolved) {
                throw std::logic_error("rnic-cn DATA metadata shape is invalid");
            }
            break;
        case RnicCollectivePacketKind::DECLARE:
            if (has_data || !has_declaration || has_grant || has_gap_nack || has_retire ||
                has_gap_resolved) {
                throw std::logic_error("rnic-cn DECLARE metadata shape is invalid");
            }
            break;
        case RnicCollectivePacketKind::ACCEPT:
        case RnicCollectivePacketKind::GRANT_UPDATE:
            if (has_data || has_declaration || !has_grant || has_gap_nack || has_retire ||
                has_gap_resolved) {
                throw std::logic_error("rnic-cn grant metadata shape is invalid");
            }
            break;
        case RnicCollectivePacketKind::GAP_NACK:
            if (has_data || has_declaration || has_grant || !has_gap_nack || has_retire ||
                has_gap_resolved) {
                throw std::logic_error("rnic-cn GAP_NACK metadata shape is invalid");
            }
            break;
        case RnicCollectivePacketKind::GAP_RESOLVED:
            if (has_data || has_declaration || has_grant || has_gap_nack || has_retire ||
                !has_gap_resolved) {
                throw std::logic_error("rnic-cn GAP_RESOLVED metadata shape is invalid");
            }
            break;
        case RnicCollectivePacketKind::RETIRE:
            if (has_data || has_declaration || has_grant || has_gap_nack || !has_retire ||
                has_gap_resolved) {
                throw std::logic_error("rnic-cn RETIRE metadata shape is invalid");
            }
            break;
    }

    const std::uint64_t lifecycle_id = takeLifecycleId();
    RnicCollectivePacket* packet = _packetdb.allocPacket();
    packet->initialize(flow, route, htsim_packet_id, kind, flow_id, source, destination, wire_bytes,
                       std::move(data), std::move(declaration), std::move(grant),
                       std::move(gap_nack), std::move(retire), std::move(gap_resolved),
                       std::move(observer), lifecycle_id);
    return packet;
}

void RnicCollectivePacket::initialize(
    PacketFlow& flow,
    const Route& route,
    packetid_t htsim_packet_id,
    RnicCollectivePacketKind kind,
    AtlahsFlowId flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    std::uint16_t wire_bytes,
    std::optional<RnicCollectiveDataMetadata> data,
    std::optional<RnicCollectiveDeclareMetadata> declaration,
    std::optional<RnicCollectiveGrant> grant,
    std::optional<RnicCollectiveGapNackMetadata> gap_nack,
    std::optional<RnicCollectiveRetireMetadata> retire,
    std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved,
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer,
    std::uint64_t lifecycle_id) {
    if (_pool_state != PoolState::RECYCLED || ref_count() != 1) {
        throw std::logic_error("rnic-cn packet pool returned a live or shared packet");
    }

    Packet::set_route(flow, route, wire_bytes, htsim_packet_id);
    _type = IP;
    // Do not overload HTSIM's trim/header-only signal for complete control
    // frames.  Their wire identity and strict priority come from _kind.
    _is_header = false;
    _bounced = false;
    _src = source;
    _dst = destination;
    _pathid = UINT32_MAX;
    _hop_count = 0;
    _direction = NONE;
    _next_routed_hop = nullptr;
    _ingressqueue = nullptr;
    _path_len = static_cast<std::uint32_t>(route.size());

    _kind = kind;
    _flow_id = flow_id;
    _source = source;
    _destination = destination;
    _wire_bytes = wire_bytes;
    _lifecycle_id = lifecycle_id;
    _data = std::move(data);
    _declaration = std::move(declaration);
    _grant = std::move(grant);
    _gap_nack = std::move(gap_nack);
    _retire = std::move(retire);
    _gap_resolved = std::move(gap_resolved);
    _observer = std::move(observer);
    _pool_state = PoolState::IN_FLIGHT;

    _observer->observe(observation(RnicCollectivePacketLifecycle::CREATED));
}

const RnicCollectiveDataMetadata& RnicCollectivePacket::data() const {
    if (!_data.has_value()) {
        throw std::logic_error("rnic-cn packet does not carry DATA metadata");
    }
    return *_data;
}

const RnicCollectiveDeclareMetadata& RnicCollectivePacket::declaration() const {
    if (!_declaration.has_value()) {
        throw std::logic_error("rnic-cn packet does not carry DECLARE metadata");
    }
    return *_declaration;
}

const RnicCollectiveGrant& RnicCollectivePacket::grant() const {
    if (!_grant.has_value()) {
        throw std::logic_error("rnic-cn packet does not carry grant metadata");
    }
    return *_grant;
}

const RnicCollectiveGapNackMetadata& RnicCollectivePacket::gapNack() const {
    if (!_gap_nack.has_value()) {
        throw std::logic_error("rnic-cn packet does not carry GAP_NACK metadata");
    }
    return *_gap_nack;
}

const RnicCollectiveGapResolvedMetadata& RnicCollectivePacket::gapResolved() const {
    if (!_gap_resolved.has_value()) {
        throw std::logic_error("rnic-cn packet does not carry GAP_RESOLVED metadata");
    }
    return *_gap_resolved;
}

const RnicCollectiveRetireMetadata& RnicCollectivePacket::retire() const {
    if (!_retire.has_value()) {
        throw std::logic_error("rnic-cn packet does not carry RETIRE metadata");
    }
    return *_retire;
}

const RnicCollectiveFinalLedger& RnicCollectivePacket::finalLedger() const {
    if (_data.has_value()) {
        return _data->final_ledger;
    }
    if (_retire.has_value()) {
        return _retire->final_ledger;
    }
    throw std::logic_error("rnic-cn packet does not carry a final DATA ledger");
}

bool RnicCollectivePacket::isFinalDataPacket() const {
    return data().isFinalPacket();
}

Packet::PktPriority RnicCollectivePacket::priority() const {
    return _kind == RnicCollectivePacketKind::DATA ? Packet::PRIO_LO : Packet::PRIO_HI;
}

RnicCollectivePacketObservation RnicCollectivePacket::observation(
    RnicCollectivePacketLifecycle lifecycle) const {
    return {
        _lifecycle_id,
        lifecycle,
        _kind,
        _flow_id,
        _source,
        _destination,
        id(),
        _wire_bytes,
        nexthop(),
        _data.has_value() ? std::optional<std::uint64_t>(_data->packet_index) : std::nullopt,
    };
}

void RnicCollectivePacket::consumeAtEndpoint() {
    if (_pool_state != PoolState::IN_FLIGHT) {
        throw std::logic_error("rnic-cn packet endpoint consumption must occur exactly once");
    }
    if (route() == nullptr || nexthop() != route()->size()) {
        throw std::logic_error("rnic-cn packet cannot be consumed before its explicit route ends");
    }
    terminate(RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
}

void RnicCollectivePacket::free() {
    terminate(RnicCollectivePacketLifecycle::FABRIC_DROP);
}

void RnicCollectivePacket::terminate(RnicCollectivePacketLifecycle lifecycle) {
    if (lifecycle == RnicCollectivePacketLifecycle::CREATED) {
        throw std::logic_error("rnic-cn CREATED is not a terminal packet lifecycle");
    }
    if (_pool_state != PoolState::IN_FLIGHT) {
        throw std::logic_error("rnic-cn packet terminal operation must occur exactly once");
    }
    if (ref_count() != 1) {
        throw std::logic_error("rnic-cn packet primitive does not support shared references");
    }
    if (!_observer) {
        throw std::logic_error("rnic-cn packet cannot terminate without its observer");
    }
    if (size() != _wire_bytes) {
        throw std::logic_error("rnic-cn packet wire size changed while it was in flight");
    }

    const RnicCollectivePacketObservation terminal = observation(lifecycle);
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer = std::move(_observer);
    _data.reset();
    _declaration.reset();
    _grant.reset();
    _gap_nack.reset();
    _retire.reset();
    _gap_resolved.reset();
    _pool_state = PoolState::RECYCLED;

    // Recycle before invoking user code.  The observation owns every value it
    // needs, and no access to this object occurs after a reentrant allocation.
    _packetdb.freePacket(this);
    observer->observe(terminal);
}

void RnicCollectivePacket::strip_payload(std::uint16_t) {
    throw std::logic_error("rnic-cn packets cannot be trimmed");
}

void RnicCollectivePacket::bounce() {
    throw std::logic_error("rnic-cn packets have no bounce or retransmission path");
}

void RnicCollectivePacket::unbounce(std::uint16_t) {
    throw std::logic_error("rnic-cn packets have no bounce or retransmission path");
}

void RnicCollectivePacket::set_route(const Route&) {
    throw std::logic_error("rnic-cn packet explicit route is immutable in flight");
}

void RnicCollectivePacket::set_route(const Route*) {
    throw std::logic_error("rnic-cn packet explicit route is immutable in flight");
}

void RnicCollectivePacket::set_route(PacketFlow&, const Route&, int, packetid_t) {
    throw std::logic_error("rnic-cn packet explicit route is immutable in flight");
}
