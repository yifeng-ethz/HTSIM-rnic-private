// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_DATA_RECOVERY_H
#define RNIC_DATA_RECOVERY_H

#include <cstdint>
#include <limits>
#include <stdexcept>

enum class RnicCnDataRecovery { None, Deadline, Exponential };

inline std::uint64_t rnicCnRetryProbeIntervalPs(RnicCnDataRecovery recovery,
                                               std::uint64_t base_interval_ps,
                                               std::uint32_t preceding_attempt) {
    if (base_interval_ps == 0 || preceding_attempt == 0) {
        throw std::invalid_argument("rnic-cn probe interval and attempt must be positive");
    }
    if (recovery != RnicCnDataRecovery::Exponential) {
        return base_interval_ps;
    }
    const auto shift = preceding_attempt - 1;
    if (shift >= 64 ||
        base_interval_ps > (std::numeric_limits<std::uint64_t>::max() >> shift)) {
        throw std::invalid_argument("rnic-cn exponential probe interval overflows uint64_t");
    }
    return base_interval_ps << shift;
}

#endif
