// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_SS_ENDPOINT_PAIR_H
#define RNIC_SS_ENDPOINT_PAIR_H

#include <cstdint>

using RnicSsNodeId = std::uint32_t;

// Canonical identity for one directed RNIC-SS source/destination pair.
// Switch accounting and endpoint control share this value type so congestion
// observations do not require a translation table or fabric-wide lookup.
struct RnicSsEndpointPair {
    RnicSsNodeId source;
    RnicSsNodeId destination;

    bool operator==(const RnicSsEndpointPair& other) const noexcept {
        return source == other.source && destination == other.destination;
    }
    bool operator!=(const RnicSsEndpointPair& other) const noexcept { return !(*this == other); }
    bool operator<(const RnicSsEndpointPair& other) const noexcept {
        return source < other.source || (source == other.source && destination < other.destination);
    }
};

#endif
