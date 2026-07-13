// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ring_cam.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {

uint64_t checkedAdd(uint64_t lhs, uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

uint64_t ceilToMultiple(uint64_t value, uint64_t quantum) {
    const uint64_t remainder = value % quantum;
    if (remainder == 0) {
        return value;
    }
    return checkedAdd(value, quantum - remainder, "Ring-CAM release time overflow");
}

}  // namespace

RnicRingCam::RnicRingCam(RnicRingCamConfig config) : config_(config) {
    if (config_.delay_window_ps == 0) {
        throw std::invalid_argument("Ring-CAM delay window must be positive");
    }
    if (config_.release_tick_ps == 0) {
        throw std::invalid_argument("Ring-CAM release tick must be positive");
    }
}

bool RnicRingCam::ReleaseKey::operator<(const ReleaseKey& other) const {
    return std::tie(logical_release_ps, eta_ps, admission_sequence)
           < std::tie(other.logical_release_ps, other.eta_ps, other.admission_sequence);
}

std::vector<RnicRingCamRelease> RnicRingCam::advanceTo(uint64_t now_ps) {
    if (now_ps < current_time_ps_) {
        throw std::invalid_argument("Ring-CAM time cannot move backwards");
    }

    std::vector<RnicRingCamRelease> releases;
    auto entry = entries_.begin();
    while (entry != entries_.end() && entry->first.logical_release_ps <= now_ps) {
        if (entry->second.wire_bytes > occupancy_bytes_) {
            throw std::logic_error("Ring-CAM occupancy underflow");
        }
        occupancy_bytes_ -= entry->second.wire_bytes;
        releases.push_back({entry->second, entry->first.logical_release_ps});
        entry = entries_.erase(entry);
    }

    current_time_ps_ = now_ps;
    return releases;
}

RnicRingCamArrivalResult RnicRingCam::processArrival(const RnicRingCamPacket& packet) {
    if (packet.arrival_ps < current_time_ps_) {
        throw std::invalid_argument("Ring-CAM arrival precedes current time");
    }
    if (packet.wire_bytes == 0) {
        throw std::invalid_argument("Ring-CAM packet must occupy at least one wire byte");
    }

    RnicRingCamAdmission admission = RnicRingCamAdmission::Admitted;
    std::optional<uint64_t> logical_release_ps;
    if (packet.arrival_ps < packet.eta_ps) {
        admission = RnicRingCamAdmission::Early;
    } else {
        const uint64_t age_ps = packet.arrival_ps - packet.eta_ps;
        if (age_ps >= config_.delay_window_ps) {
            admission = RnicRingCamAdmission::Late;
        } else {
            const uint64_t release_edge_ps = checkedAdd(
                packet.eta_ps,
                config_.delay_window_ps,
                "Ring-CAM ETA plus delay window overflow");
            logical_release_ps = ceilToMultiple(release_edge_ps, config_.release_tick_ps);
            if (next_admission_sequence_ == std::numeric_limits<uint64_t>::max()) {
                throw std::overflow_error("Ring-CAM admission sequence overflow");
            }
        }
    }

    std::vector<RnicRingCamRelease> releases = advanceTo(packet.arrival_ps);

    if (admission == RnicRingCamAdmission::Admitted) {
        if (occupancy_bytes_ > config_.byte_capacity
            || packet.wire_bytes > config_.byte_capacity - occupancy_bytes_) {
            admission = RnicRingCamAdmission::Overflow;
            logical_release_ps.reset();
        } else {
            const ReleaseKey key{
                *logical_release_ps, packet.eta_ps, next_admission_sequence_++};
            entries_.emplace(key, packet);
            occupancy_bytes_ += packet.wire_bytes;
            high_watermark_bytes_ = std::max(high_watermark_bytes_, occupancy_bytes_);
        }
    }

    return {std::move(releases), admission, logical_release_ps};
}

RnicModuloTimestampAge classifyRnicModuloTimestampAge(
        uint64_t now_ticks,
        uint64_t eta_ticks,
        unsigned timestamp_bits,
        uint64_t window_ticks) {
    if (timestamp_bits < 2 || timestamp_bits > 63) {
        throw std::invalid_argument("modulo timestamp width must be in [2, 63]");
    }

    const uint64_t modulus = uint64_t{1} << timestamp_bits;
    const uint64_t half_range = modulus / 2;
    if (window_ticks == 0 || window_ticks >= half_range) {
        throw std::invalid_argument(
            "modulo admission window must be positive and smaller than half the range");
    }
    if (now_ticks >= modulus || eta_ticks >= modulus) {
        throw std::invalid_argument("modulo timestamp value exceeds its field width");
    }

    const uint64_t age_ticks = now_ticks >= eta_ticks
                                   ? now_ticks - eta_ticks
                                   : modulus - (eta_ticks - now_ticks);
    if (age_ticks < window_ticks) {
        return {RnicModuloTimestampRelation::Admitted, age_ticks};
    }
    if (age_ticks < half_range) {
        return {RnicModuloTimestampRelation::Late, age_ticks};
    }
    if (age_ticks > half_range) {
        return {RnicModuloTimestampRelation::Early, age_ticks};
    }
    return {RnicModuloTimestampRelation::Ambiguous, age_ticks};
}
