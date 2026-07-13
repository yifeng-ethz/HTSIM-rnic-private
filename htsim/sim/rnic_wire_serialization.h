// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_WIRE_SERIALIZATION_H
#define RNIC_WIRE_SERIALIZATION_H

#include <cstdint>
#include <limits>
#include <stdexcept>

struct RnicWireSerializationInterval {
    uint64_t start_ps;
    uint64_t end_ps;
};

// An exact rational clock for one physical wire serializer. Event timestamps
// are the ceilings of exact boundaries, while the fractional remainder is
// retained across back-to-back packets. This avoids adding one rounding tick
// per packet. A genuinely idle interval starts a new rational busy period at
// its integral earliest_start_ps boundary.
class RnicWireSerializationClock {
public:
    explicit RnicWireSerializationClock(uint64_t wire_capacity_bps)
        : _wire_capacity_bps(wire_capacity_bps) {
        if (wire_capacity_bps == 0) {
            throw std::invalid_argument(
                "RNIC wire serializer capacity must be nonzero");
        }
    }

    RnicWireSerializationInterval serialize(
            uint64_t earliest_start_ps, uint64_t wire_bytes) {
        if (wire_bytes == 0) {
            throw std::invalid_argument(
                "RNIC wire serializer requires a nonempty wire extent");
        }
        Boundary start = _available;
        // Equality with the rounded available timestamp is back-to-back at the
        // simulator's resolution. Reset the rational remainder only when at
        // least one whole simulator tick is observably idle.
        if (earliest_start_ps > ceilBoundary(start)) {
            start = {earliest_start_ps, 0};
        }

        using Wide = unsigned __int128;
        const Wide numerator = static_cast<Wide>(wire_bytes)
                               * kSerializationNumeratorPerByte;
        const Wide floor_increment = numerator / _wire_capacity_bps;
        if (floor_increment
            > static_cast<Wide>(std::numeric_limits<uint64_t>::max()
                                - start.floor_ps)) {
            throw std::overflow_error(
                "RNIC wire serialization timestamp overflow");
        }

        Boundary end = start;
        end.floor_ps += static_cast<uint64_t>(floor_increment);

        const uint64_t remainder_increment =
            static_cast<uint64_t>(numerator % _wire_capacity_bps);
        if (remainder_increment != 0) {
            const uint64_t carry_threshold =
                _wire_capacity_bps - remainder_increment;
            if (end.remainder >= carry_threshold) {
                end.remainder -= carry_threshold;
                end.floor_ps = checkedAdd(
                    end.floor_ps,
                    1,
                    "RNIC wire serialization timestamp overflow");
            } else {
                end.remainder += remainder_increment;
            }
        }

        const RnicWireSerializationInterval interval{
            ceilBoundary(start), ceilBoundary(end)};
        _available = end;
        return interval;
    }

    uint64_t availablePs() const { return ceilBoundary(_available); }
    uint64_t wireCapacityBps() const noexcept { return _wire_capacity_bps; }

private:
    static constexpr uint64_t kSerializationNumeratorPerByte =
        UINT64_C(8000000000000);

    struct Boundary {
        uint64_t floor_ps = 0;
        uint64_t remainder = 0;
    };

    static uint64_t checkedAdd(
            uint64_t lhs, uint64_t rhs, const char* message) {
        if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
            throw std::overflow_error(message);
        }
        return lhs + rhs;
    }

    static uint64_t ceilBoundary(const Boundary& boundary) {
        return checkedAdd(
            boundary.floor_ps,
            boundary.remainder == 0 ? 0 : 1,
            "RNIC wire serialization timestamp overflow");
    }

    uint64_t _wire_capacity_bps;
    Boundary _available;
};

#endif
