// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PACKET_EXTENT_H
#define RNIC_PACKET_EXTENT_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

// The two independent byte ledgers carried by every packetized RNIC profile.
// payload_bytes advances the application flow; wire_bytes consumes physical
// serializer and buffer capacity. A wire packet may contain no application
// payload (for example, a control packet), but it must occupy the wire and
// cannot carry more payload than its complete on-wire representation.
class RnicPacketExtent {
public:
    RnicPacketExtent(uint64_t payload_bytes_value, uint64_t wire_bytes_value)
        : _payload_bytes(payload_bytes_value), _wire_bytes(wire_bytes_value) {
        if (_wire_bytes == 0) {
            throw std::invalid_argument(
                "RNIC packet extent must occupy at least one wire byte");
        }
        if (_payload_bytes > _wire_bytes) {
            throw std::invalid_argument(
                "RNIC packet payload cannot exceed its wire extent");
        }
    }

    uint64_t payloadBytes() const noexcept { return _payload_bytes; }
    uint64_t wireBytes() const noexcept { return _wire_bytes; }

private:
    uint64_t _payload_bytes;
    uint64_t _wire_bytes;
};

// Packetization policy for DATA frames. max_wire_packet_bytes includes the
// per-packet DATA header; therefore the maximum application payload in one
// frame is max_wire_packet_bytes - data_header_bytes.
class RnicDataPacketizationConfig {
public:
    explicit RnicDataPacketizationConfig(
            uint64_t max_wire_packet_bytes,
            uint64_t data_header_bytes = 0)
        : _max_wire_packet_bytes(max_wire_packet_bytes),
          _data_header_bytes(data_header_bytes) {
        if (_max_wire_packet_bytes == 0) {
            throw std::invalid_argument(
                "RNIC DATA maximum wire packet size must be nonzero");
        }
        if (_data_header_bytes >= _max_wire_packet_bytes) {
            throw std::invalid_argument(
                "RNIC DATA header must be smaller than the maximum wire packet");
        }
    }

    uint64_t maxWirePacketBytes() const noexcept {
        return _max_wire_packet_bytes;
    }
    uint64_t dataHeaderBytes() const noexcept { return _data_header_bytes; }
    uint64_t maxPayloadBytes() const noexcept {
        return _max_wire_packet_bytes - _data_header_bytes;
    }

    RnicPacketExtent packetize(uint64_t remaining_payload_bytes) const {
        if (remaining_payload_bytes == 0) {
            throw std::invalid_argument(
                "RNIC DATA packetizer requires positive remaining payload");
        }
        const uint64_t payload_bytes =
            std::min(remaining_payload_bytes, maxPayloadBytes());
        if (_data_header_bytes
            > std::numeric_limits<uint64_t>::max() - payload_bytes) {
            throw std::overflow_error("RNIC DATA wire extent overflow");
        }
        const uint64_t wire_bytes = payload_bytes + _data_header_bytes;
        if (wire_bytes > _max_wire_packet_bytes) {
            throw std::logic_error(
                "RNIC DATA packetizer exceeded its maximum wire packet size");
        }
        return {payload_bytes, wire_bytes};
    }

private:
    uint64_t _max_wire_packet_bytes;
    uint64_t _data_header_bytes;
};

#endif
