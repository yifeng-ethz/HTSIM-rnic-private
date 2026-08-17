// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DRAGONFLY_PROGRESSIVE_CONGESTION_H
#define DRAGONFLY_PROGRESSIVE_CONGESTION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

#include "dragonfly_progressive_routing.h"

namespace htsim {

// Fifteen strictly increasing thresholds divide an unsigned physical
// measurement into the sixteen values representable by a four-bit signal.
// A value below thresholds[0] maps to zero; a value at or above
// thresholds[14] saturates at fifteen.
class DragonflyFourBitRemap {
public:
    using Thresholds = std::array<std::uint64_t, 15>;

    explicit DragonflyFourBitRemap(Thresholds thresholds);

    static DragonflyFourBitRemap uniform(std::uint64_t units_per_level);

    std::uint8_t map(std::uint64_t value) const noexcept;
    const Thresholds& thresholds() const noexcept { return _thresholds; }

private:
    Thresholds _thresholds;
};

struct DragonflyProgressiveCongestionConfig {
    DragonflyFourBitRemap near_end_remap{DragonflyFourBitRemap::uniform(1)};
    DragonflyFourBitRemap far_end_remap{DragonflyFourBitRemap::uniform(1)};
    DragonflyFourBitRemap downstream_remap{DragonflyFourBitRemap::uniform(1)};

    // This lower bound represents physical propagation, serialization, and
    // any other delay that cannot be bypassed by an advertisement.
    std::uint64_t minimum_downstream_delay_ps{1};

    // Downstream state is usable through this age, measured from the remote
    // observation time.  At maximum_downstream_age_ps + 1 it contributes
    // zero, preventing an old advertisement from becoming a permanent queue
    // oracle when refresh traffic is lost.
    std::uint64_t maximum_downstream_age_ps{1'000'000};
};

using DragonflyCongestionMessageId = std::uint64_t;

struct DragonflyDownstreamAdvertisement {
    std::uint64_t sequence{0};
    std::uint64_t observation_time_ps{0};
    std::uint64_t earliest_arrival_time_ps{0};
    std::uint64_t waiting_bytes{0};
};

enum class DragonflyAdvertisementDisposition : std::uint8_t {
    Accepted,
    NotYetPhysicallyArrived,
    StaleSequence,
    StaleObservation,
};

// A deterministic, event-loop-independent model of the three congestion
// inputs used by progressive Dragonfly routing.  It exposes only locally
// measurable queue state, causally delivered credits, and physically arrived
// advertisements; it has no network-wide or instantaneous queue oracle.
class DragonflyProgressiveCongestion {
public:
    explicit DragonflyProgressiveCongestion(DragonflyProgressiveCongestionConfig config = {});

    const DragonflyProgressiveCongestionConfig& config() const noexcept { return _config; }

    void setNearEndWaitingBytes(std::uint64_t waiting_bytes) noexcept;
    void addNearEndWaitingBytes(std::uint64_t bytes);
    void removeNearEndWaitingBytes(std::uint64_t bytes);
    std::uint64_t nearEndWaitingBytes() const noexcept { return _near_end_waiting_bytes; }

    // Sending makes bytes outstanding and in flight at the same instant.
    // Only messageArrived() removes them from the in-flight term, so bytes on
    // a wire are never misclassified as resident in the far-end queue.
    void messageSent(DragonflyCongestionMessageId message_id, std::uint64_t bytes);
    std::uint64_t messageArrived(DragonflyCongestionMessageId message_id);
    void creditReturned(std::uint64_t bytes);

    std::uint64_t outstandingSentBytes() const noexcept { return _outstanding_sent_bytes; }
    std::uint64_t inFlightBytes() const noexcept { return _in_flight_bytes; }
    std::size_t inFlightMessageCount() const noexcept { return _in_flight_messages.size(); }
    std::uint64_t farEndResidentBytes() const;

    // Constructs the earliest legal arrival using the configured physical
    // delay.  A caller may provide a later arrival explicitly (for example,
    // to include non-preemptive serialization), but never an earlier one.
    DragonflyDownstreamAdvertisement makeDownstreamAdvertisement(std::uint64_t sequence,
                                                                 std::uint64_t observation_time_ps,
                                                                 std::uint64_t waiting_bytes) const;

    DragonflyAdvertisementDisposition consumeDownstreamAdvertisement(
        const DragonflyDownstreamAdvertisement& advertisement,
        std::uint64_t current_time_ps);

    bool hasDownstreamAdvertisement() const noexcept {
        return _downstream_advertisement.has_value();
    }
    std::optional<DragonflyDownstreamAdvertisement> downstreamAdvertisement() const noexcept {
        return _downstream_advertisement;
    }
    std::optional<std::uint64_t> downstreamConsumptionTimePs() const noexcept {
        return _downstream_consumption_time_ps;
    }
    std::uint64_t downstreamWaitingBytes() const noexcept {
        return _downstream_advertisement ? _downstream_advertisement->waiting_bytes : 0;
    }

    // Returns the congestion inputs at current_time_ps.  Calls on one model
    // instance must follow simulation time; time regression is rejected.
    // A downstream observation is fresh at the exact configured age boundary
    // and expires one picosecond later.
    DragonflyCongestionComponents components(std::uint64_t current_time_ps) const;

private:
    std::uint64_t minimumArrivalTime(std::uint64_t observation_time_ps) const;
    void validateAdvertisement(const DragonflyDownstreamAdvertisement& advertisement) const;
    void advanceTime(std::uint64_t current_time_ps) const;
    bool downstreamIsFresh(std::uint64_t current_time_ps) const;
    void assertInvariant() const;

    DragonflyProgressiveCongestionConfig _config;
    std::uint64_t _near_end_waiting_bytes{0};
    std::uint64_t _outstanding_sent_bytes{0};
    std::uint64_t _in_flight_bytes{0};
    std::map<DragonflyCongestionMessageId, std::uint64_t> _in_flight_messages;
    std::optional<DragonflyDownstreamAdvertisement> _downstream_advertisement;
    std::optional<std::uint64_t> _downstream_consumption_time_ps;
    mutable std::optional<std::uint64_t> _latest_time_ps;
};

}  // namespace htsim

#endif  // DRAGONFLY_PROGRESSIVE_CONGESTION_H
