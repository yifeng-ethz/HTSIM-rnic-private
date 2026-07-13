// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_RING_CAM_H
#define RNIC_RING_CAM_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

struct RnicRingCamPacket {
    uint64_t packet_id;
    uint64_t flow_id;
    uint64_t eta_ps;
    uint64_t arrival_ps;
    uint64_t wire_bytes;
};

struct RnicRingCamConfig {
    // The admission window Delta. A packet is admitted exactly when
    // 0 <= arrival_ps - eta_ps < delay_window_ps.
    uint64_t delay_window_ps;

    // The logical release quantum delta.
    uint64_t release_tick_ps;

    // Shared payload storage for all flows using this receive port.
    uint64_t byte_capacity;
};

enum class RnicRingCamAdmission {
    Early,
    Admitted,
    Late,
    Overflow,
};

struct RnicRingCamRelease {
    RnicRingCamPacket packet;
    uint64_t logical_release_ps;
};

struct RnicRingCamArrivalResult {
    // processArrival() releases every packet due at arrival_ps before it
    // classifies the new packet. This makes same-time event ordering part
    // of the API rather than a caller convention.
    std::vector<RnicRingCamRelease> released_before_admission;
    RnicRingCamAdmission admission;
    std::optional<uint64_t> logical_release_ps;
};

class RnicRingCam {
public:
    explicit RnicRingCam(RnicRingCamConfig config);

    // Advances monotonically and releases all packets whose logical release
    // time is at most now_ps. Releases are ordered by logical release tick,
    // then sender timestamp, then stable admission order.
    std::vector<RnicRingCamRelease> advanceTo(uint64_t now_ps);

    // Atomically performs advanceTo(packet.arrival_ps) and then classifies
    // packet. Consequently, releases at a timestamp always precede an
    // admission at that same timestamp.
    RnicRingCamArrivalResult processArrival(const RnicRingCamPacket& packet);

    uint64_t currentTimePs() const { return current_time_ps_; }
    uint64_t occupancyBytes() const { return occupancy_bytes_; }
    uint64_t highWatermarkBytes() const { return high_watermark_bytes_; }
    uint64_t byteCapacity() const { return config_.byte_capacity; }
    size_t packetCount() const { return entries_.size(); }

private:
    struct ReleaseKey {
        uint64_t logical_release_ps;
        uint64_t eta_ps;
        uint64_t admission_sequence;

        bool operator<(const ReleaseKey& other) const;
    };

    RnicRingCamConfig config_;
    uint64_t current_time_ps_ = 0;
    uint64_t occupancy_bytes_ = 0;
    uint64_t high_watermark_bytes_ = 0;
    uint64_t next_admission_sequence_ = 0;
    std::map<ReleaseKey, RnicRingCamPacket> entries_;
};

enum class RnicModuloTimestampRelation {
    Early,
    Admitted,
    Late,
    Ambiguous,
};

struct RnicModuloTimestampAge {
    RnicModuloTimestampRelation relation;
    uint64_t age_ticks;
};

// Classifies a finite-width timestamp with the usual half-range rule.
// window_ticks must be positive and strictly smaller than half the counter
// range. A modular age in [0, window_ticks) is admitted; an older timestamp
// below the half range is late; a timestamp in the forward half is early;
// the exactly-half-range case is explicitly ambiguous.
RnicModuloTimestampAge classifyRnicModuloTimestampAge(
    uint64_t now_ticks,
    uint64_t eta_ticks,
    unsigned timestamp_bits,
    uint64_t window_ticks);

#endif
