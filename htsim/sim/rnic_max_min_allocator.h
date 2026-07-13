// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_MAX_MIN_ALLOCATOR_H
#define RNIC_MAX_MIN_ALLOCATOR_H

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

struct RnicMaxMinFlow {
    using FlowId = uint64_t;
    using NodeId = uint32_t;
    using RateBps = uint64_t;

    FlowId flow_id;
    NodeId source_node;
    NodeId destination_node;
    std::optional<RateBps> demand_cap_bps = std::nullopt;
};

class RnicMaxMinAllocator {
public:
    using FlowId = RnicMaxMinFlow::FlowId;
    using NodeId = RnicMaxMinFlow::NodeId;
    using RateBps = RnicMaxMinFlow::RateBps;
    using CapacityMap = std::map<NodeId, RateBps>;
    using AllocationMap = std::map<FlowId, RateBps>;

    // Computes an unweighted max-min allocation over source access uplinks,
    // destination access downlinks, and optional per-flow demand caps. The
    // continuous allocation is solved with exact rational arithmetic, then
    // each rate is independently rounded down to whole bps. Fractional
    // leftovers remain idle rather than being assigned by flow-ID priority.
    //
    // Exact rational numerators and denominators are limited to 128 bits.
    // Inputs whose normalized intermediate values exceed that supported domain
    // throw std::overflow_error; the allocator never falls back to floating
    // point or returns an approximate allocation.
    static AllocationMap allocate(const std::vector<RnicMaxMinFlow>& active_flows,
                                  const CapacityMap& source_uplink_capacity_bps,
                                  const CapacityMap& destination_downlink_capacity_bps);
};

#endif
