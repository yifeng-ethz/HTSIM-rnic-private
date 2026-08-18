// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ss_dragonfly_load.h"

#include <limits>
#include <stdexcept>

#include "../rnic_wide_integer.h"

namespace {

std::uint64_t parseLoadUnsigned(const std::string& field, const std::string& text) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument("-flow field '" + field +
                                    "' requires an unsigned value");
    }
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument("-flow field '" + field + "' has a malformed value '" +
                                    text + "'");
    }
    if (consumed != text.size()) {
        throw std::invalid_argument("-flow field '" + field + "' has a malformed value '" +
                                    text + "'");
    }
    return static_cast<std::uint64_t>(value);
}

}  // namespace

SsDragonflySourceMode parseSsDragonflySourceMode(const std::string& text) {
    if (text == "open") {
        return SsDragonflySourceMode::OpenLoop;
    }
    if (text == "paced") {
        return SsDragonflySourceMode::Paced;
    }
    if (text == "closed") {
        return SsDragonflySourceMode::ClosedLoop;
    }
    throw std::invalid_argument("-source expects open, paced, or closed");
}

SsDragonflyLoadFlowSpec parseSsDragonflyFlowSpec(const std::string& text) {
    SsDragonflyLoadFlowSpec spec;
    bool source_seen = false;
    bool destination_seen = false;
    bool start_seen = false;
    std::size_t position = 0;
    while (position <= text.size()) {
        const std::size_t comma = text.find(',', position);
        const std::string token = text.substr(
            position, comma == std::string::npos ? std::string::npos : comma - position);
        position = comma == std::string::npos ? text.size() + 1 : comma + 1;
        if (token.empty()) {
            throw std::invalid_argument("-flow has an empty field in '" + text + "'");
        }
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos || equals == 0 || equals + 1 == token.size()) {
            throw std::invalid_argument("-flow field '" + token +
                                        "' is not a key=value pair");
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (key == "src") {
            if (source_seen) {
                throw std::invalid_argument("-flow repeats field 'src'");
            }
            source_seen = true;
            spec.source = static_cast<std::uint32_t>(parseLoadUnsigned(key, value));
        } else if (key == "dst") {
            if (destination_seen) {
                throw std::invalid_argument("-flow repeats field 'dst'");
            }
            destination_seen = true;
            spec.destination = static_cast<std::uint32_t>(parseLoadUnsigned(key, value));
        } else if (key == "start_ps") {
            if (start_seen) {
                throw std::invalid_argument("-flow repeats field 'start_ps'");
            }
            start_seen = true;
            spec.start_ps = parseLoadUnsigned(key, value);
        } else if (key == "offered_bps") {
            if (spec.offered_bps.has_value()) {
                throw std::invalid_argument("-flow repeats field 'offered_bps'");
            }
            spec.offered_bps = parseLoadUnsigned(key, value);
        } else if (key == "think_ps") {
            if (spec.think_ps.has_value()) {
                throw std::invalid_argument("-flow repeats field 'think_ps'");
            }
            spec.think_ps = parseLoadUnsigned(key, value);
        } else {
            throw std::invalid_argument("-flow has an unknown field '" + key + "'");
        }
    }
    if (!source_seen || !destination_seen) {
        throw std::invalid_argument("-flow requires both src= and dst=");
    }
    return spec;
}

std::uint64_t ssDragonflyChunkPackets(std::uint64_t chunk_payload_bytes,
                                      std::uint64_t payload_per_packet) {
    if (chunk_payload_bytes == 0 || payload_per_packet == 0) {
        throw std::invalid_argument("chunk arithmetic requires positive byte counts");
    }
    return (chunk_payload_bytes + payload_per_packet - 1) / payload_per_packet;
}

simtime_picosec ssDragonflyPacedOffset(std::uint64_t wire_bytes,
                                       std::uint64_t offered_bps,
                                       std::uint64_t packet_index) {
    if (wire_bytes == 0 || offered_bps == 0) {
        throw std::invalid_argument("paced offsets require positive wire bytes and rate");
    }
    // ceil(packet_index * wire_bytes * 8e12 / offered_bps), exact: the
    // wire-bits-times-picoseconds product fits 64 bits (wire extents are
    // bounded by uint16_t), and the index multiply widens to 128 bits.
    const std::uint64_t bit_picoseconds = wire_bytes * UINT64_C(8000000000000);
    const RnicWideInteger numerator = RnicWideInteger(bit_picoseconds) * packet_index;
    const RnicWideInteger quotient =
        (numerator + RnicWideInteger(offered_bps - 1)) / RnicWideInteger(offered_bps);
    if (quotient > RnicWideInteger(std::numeric_limits<simtime_picosec>::max())) {
        throw std::invalid_argument("paced offset exceeds representable simulated time");
    }
    return static_cast<simtime_picosec>(static_cast<std::uint64_t>(quotient));
}

SsDragonflyLoadSource::SsDragonflyLoadSource(EventList& eventlist,
                                             const std::string& name,
                                             SsDragonflyTopology& fabric,
                                             std::uint32_t flow_id,
                                             std::uint32_t source,
                                             std::uint32_t destination,
                                             simtime_picosec start_abs_ps,
                                             simtime_picosec stop_abs_ps,
                                             htsim::DragonflyRoutingControl control,
                                             bool per_packet_hash,
                                             std::uint64_t wire_bytes,
                                             std::uint64_t header_bytes,
                                             std::uint64_t chunk_packets)
    : EventSource(eventlist, name),
      _fabric(fabric),
      _flow_id(flow_id),
      _source(source),
      _destination(destination),
      _start_ps(start_abs_ps),
      _stop_ps(stop_abs_ps),
      _control(control),
      _per_packet_hash(per_packet_hash),
      _wire_bytes(wire_bytes),
      _header_bytes(header_bytes),
      _chunk_packets(chunk_packets),
      _packet_flow(nullptr) {
    if (header_bytes >= wire_bytes) {
        throw std::invalid_argument("load source header bytes must leave payload room");
    }
    _packet_flow.set_flowid(flow_id + 1);
    eventlist.sourceIsPending(*this, start_abs_ps);
}

SsDragonflyLoadSource::~SsDragonflyLoadSource() {
    EventList::cancelPendingSource(*this);
}

void SsDragonflyLoadSource::injectOnePacket() {
    // The chunk dispatch reads sequences back from the 32-bit packet id,
    // so refuse to wrap rather than corrupt chunk accounting.
    if (_sequence > std::numeric_limits<packetid_t>::max()) {
        throw std::logic_error("load source packet sequence exceeds the 32-bit packet id");
    }
    if (_chunk_packets > 0 && _sequence % _chunk_packets == 0) {
        _chunk_first_injection.push_back(EventList::now());
        _chunk_delivered.push_back(0);
    }
    const std::uint64_t hash =
        _fabric.packetRouteHash(_flow_id, _per_packet_hash ? _sequence : 0);
    SsDragonflyPacket* packet = SsDragonflyPacket::newPacket(
        _packet_flow, _fabric.injectionRoute(_source),
        static_cast<packetid_t>(_sequence), _source, _destination,
        static_cast<mem_b>(_wire_bytes),
        static_cast<mem_b>(_wire_bytes - _header_bytes));
    _fabric.registerPacket(*packet, _control, hash);
    _sequence++;
    packet->sendOn();
}

void SsDragonflyLoadSource::noteDelivered(std::uint64_t sequence, simtime_picosec at_ps) {
    if (_chunk_packets == 0) {
        return;
    }
    const std::uint64_t chunk = sequence / _chunk_packets;
    if (chunk >= _chunk_delivered.size() || sequence >= _sequence) {
        throw std::logic_error("load source saw a delivery it never injected");
    }
    _chunk_delivered[chunk]++;
    if (_chunk_delivered[chunk] == _chunk_packets) {
        const SsDragonflyChunkCompletion completion{
            chunk, _chunk_first_injection[chunk], at_ps};
        _completions.push_back(completion);
        onChunkCompleted(completion);
    }
}

SsDragonflyPacedSource::SsDragonflyPacedSource(EventList& eventlist,
                                               SsDragonflyTopology& fabric,
                                               std::uint32_t flow_id,
                                               std::uint32_t source,
                                               std::uint32_t destination,
                                               simtime_picosec start_abs_ps,
                                               simtime_picosec stop_abs_ps,
                                               htsim::DragonflyRoutingControl control,
                                               bool per_packet_hash,
                                               std::uint64_t wire_bytes,
                                               std::uint64_t header_bytes,
                                               std::uint64_t chunk_packets,
                                               std::uint64_t offered_bps)
    : SsDragonflyLoadSource(eventlist,
                            "ss-dragonfly-paced-source-" + std::to_string(flow_id),
                            fabric,
                            flow_id,
                            source,
                            destination,
                            start_abs_ps,
                            stop_abs_ps,
                            control,
                            per_packet_hash,
                            wire_bytes,
                            header_bytes,
                            chunk_packets),
      _offered_bps(offered_bps) {
    if (offered_bps == 0 || offered_bps > fabric.config().host_link.rate_bps) {
        throw std::invalid_argument(
            "paced sources require 0 < offered_bps <= the host link rate");
    }
}

void SsDragonflyPacedSource::doNextEvent() {
    if (stopReached()) {
        return;
    }
    injectOnePacket();
    eventlist().sourceIsPending(
        *this,
        _start_ps + ssDragonflyPacedOffset(_wire_bytes, _offered_bps, injectedPackets()));
}

SsDragonflyClosedLoopSource::SsDragonflyClosedLoopSource(
    EventList& eventlist,
    SsDragonflyTopology& fabric,
    std::uint32_t flow_id,
    std::uint32_t source,
    std::uint32_t destination,
    simtime_picosec start_abs_ps,
    simtime_picosec stop_abs_ps,
    htsim::DragonflyRoutingControl control,
    bool per_packet_hash,
    std::uint64_t wire_bytes,
    std::uint64_t header_bytes,
    std::uint64_t chunk_packets,
    std::uint64_t think_ps)
    : SsDragonflyLoadSource(eventlist,
                            "ss-dragonfly-closed-source-" + std::to_string(flow_id),
                            fabric,
                            flow_id,
                            source,
                            destination,
                            start_abs_ps,
                            stop_abs_ps,
                            control,
                            per_packet_hash,
                            wire_bytes,
                            header_bytes,
                            chunk_packets),
      _think_ps(think_ps),
      _serialization_ps((wire_bytes * UINT64_C(8000000) * UINT64_C(1000000) +
                         fabric.config().host_link.rate_bps - 1) /
                        fabric.config().host_link.rate_bps) {
    if (chunk_packets == 0) {
        throw std::invalid_argument("closed-loop sources require a positive chunk size");
    }
}

void SsDragonflyClosedLoopSource::doNextEvent() {
    if (stopReached()) {
        // No injection at or after the stop time; a chunk-start event
        // landing here ends the loop for good.
        _halted = true;
        return;
    }
    if (_remaining_in_chunk == 0) {
        _remaining_in_chunk = _chunk_packets;
    }
    injectOnePacket();
    _remaining_in_chunk--;
    if (_remaining_in_chunk > 0) {
        eventlist().sourceIsPendingRel(*this, _serialization_ps);
    }
    // Otherwise the loop is closed: the next event is scheduled by
    // onChunkCompleted at the chunk's delivery plus the think time.
}

void SsDragonflyClosedLoopSource::onChunkCompleted(
    const SsDragonflyChunkCompletion& completion) {
    if (_halted) {
        return;
    }
    eventlist().sourceIsPending(*this, completion.completion_ps + _think_ps);
}

void SsDragonflyLoadDispatch::addSource(SsDragonflyLoadSource& source) {
    for (const SsDragonflyLoadSource* existing : _sources) {
        if (existing->source() == source.source() &&
            existing->destination() == source.destination()) {
            throw std::invalid_argument(
                "load flows must have pairwise distinct (source, destination) pairs");
        }
    }
    _sources.push_back(&source);
}

void SsDragonflyLoadDispatch::noteDelivery(const SsDragonflyPacket& packet,
                                           simtime_picosec at_ps) {
    for (SsDragonflyLoadSource* source : _sources) {
        if (source->source() == packet.src() && source->destination() == packet.dst()) {
            source->noteDelivered(packet.id(), at_ps);
            return;
        }
    }
    throw std::logic_error("delivered packet matches no load flow");
}

void SsDragonflyLoadDispatch::noteDrop(simtime_picosec at_ps) {
    if (!_first_drop_ps.has_value()) {
        _first_drop_ps = at_ps;
    }
    _dropped_packets++;
}
