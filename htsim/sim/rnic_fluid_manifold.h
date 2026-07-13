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
    uint64_t rate_bps;
    std::optional<uint64_t> service_completion_time_ps;
    std::optional<uint64_t> delivery_completion_time_ps;

    bool active() const { return !service_completion_time_ps.has_value(); }
};

// Piecewise-linear fluid service for the topology-free RNIC profile.
//
// All active flows share source uplink and destination downlink constraints via
// the same progressive max-min allocator as the packetized manifold model.
// The allocator solves the ideal max-min vector exactly, then applies its
// declared componentwise whole-bps floor. This class keeps payload service
// exact at those executable grants.
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
    std::optional<TimePs> projectedServiceCompletionTime(FlowId flow_id) const;
    std::optional<TimePs> nextServiceCompletionTime() const;

private:
    // Exact service debt in bit*ps/s.  A payload starts with
    // (payload_bits * 10^12 ps/s); service subtracts
    // (rate_bits_per_second * elapsed_ps).  A uint64 payload needs fewer than
    // 107 bits, while any uint64 rate-times-duration product fits in 128 bits.
    using ServiceDebt = unsigned __int128;

    struct FlowState {
        RnicFluidFlowSpec spec;
        ServiceDebt remaining_service_debt = 0;
        uint64_t rate_bps = 0;
        std::optional<TimePs> service_completion_time_ps;
        std::optional<TimePs> delivery_completion_time_ps;
    };

    static constexpr ServiceDebt kPicosecondsPerSecond =
        static_cast<ServiceDebt>(1000000000000ULL);

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
