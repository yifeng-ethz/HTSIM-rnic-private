// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_FLUID_MANIFOLD_H
#define RNIC_FLUID_MANIFOLD_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

#include "rnic_max_min_allocator.h"

struct RnicFluidFlowSpec {
    using FlowId = RnicMaxMinFlow::FlowId;
    using NodeId = RnicMaxMinFlow::NodeId;
    using RateBps = RnicMaxMinFlow::RateBps;

    FlowId flow_id;
    NodeId source_node;
    NodeId destination_node;
    uint64_t size_bytes;
    std::optional<RateBps> demand_cap_bps = std::nullopt;
};

struct RnicFluidFlowSnapshot {
    RnicFluidFlowSpec spec;
    long double remaining_bits;
    uint64_t rate_bps;
    std::optional<uint64_t> service_completion_time_ps;
    std::optional<uint64_t> delivery_completion_time_ps;

    bool active() const { return !service_completion_time_ps.has_value(); }
};

// Piecewise-linear fluid service for the topology-free RNIC profile.
//
// All active flows share source uplink and destination downlink constraints via
// the same progressive max-min allocator as the packetized null-network model.
// The manifold adds only one fixed propagation shift after the last bit is
// serviced. It has no packet, acknowledgement, resequencing, or pacer state.
class RnicFluidManifold {
public:
    using FlowId = RnicFluidFlowSpec::FlowId;
    using CapacityMap = RnicMaxMinAllocator::CapacityMap;
    using TimePs = uint64_t;

    RnicFluidManifold(CapacityMap source_uplink_capacity_bps,
                      CapacityMap destination_downlink_capacity_bps,
                      TimePs propagation_delay_ps);

    void addFlow(const RnicFluidFlowSpec& flow, TimePs now_ps);
    void advanceTo(TimePs now_ps);

    TimePs now() const { return _now_ps; }
    TimePs propagationDelay() const { return _propagation_delay_ps; }
    uint64_t allocationEpoch() const { return _allocation_epoch; }
    size_t activeFlowCount() const;

    bool contains(FlowId flow_id) const;
    RnicFluidFlowSnapshot flow(FlowId flow_id) const;
    std::optional<TimePs> nextServiceCompletionTime() const;

private:
    struct FlowState {
        RnicFluidFlowSpec spec;
        long double remaining_bits = 0.0L;
        uint64_t rate_bps = 0;
        std::optional<TimePs> service_completion_time_ps;
        std::optional<TimePs> delivery_completion_time_ps;
    };

    static constexpr long double kCompletionEpsilonBits = 1e-9L;
    static constexpr long double kPicosecondsPerSecond = 1.0e12L;

    void serveUntil(TimePs time_ps);
    void completeServicedFlows();
    void recomputeAllocation();
    TimePs deliveryTimeFor(TimePs service_completion_time_ps) const;

    CapacityMap _source_uplink_capacity_bps;
    CapacityMap _destination_downlink_capacity_bps;
    TimePs _propagation_delay_ps;
    TimePs _now_ps = 0;
    uint64_t _allocation_epoch = 0;
    std::map<FlowId, FlowState> _flows;
};

#endif
