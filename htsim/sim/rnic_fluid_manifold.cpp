// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_fluid_manifold.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

RnicFluidManifold::RnicFluidManifold(
        CapacityMap source_uplink_capacity_bps,
        CapacityMap destination_downlink_capacity_bps,
        TimePs propagation_delay_ps)
    : _source_uplink_capacity_bps(std::move(source_uplink_capacity_bps)),
      _destination_downlink_capacity_bps(std::move(destination_downlink_capacity_bps)),
      _propagation_delay_ps(propagation_delay_ps) {}

void RnicFluidManifold::addFlow(const RnicFluidFlowSpec& flow, TimePs now_ps) {
    if (now_ps < _now_ps) {
        throw std::invalid_argument("fluid manifold time cannot move backwards");
    }
    if (contains(flow.flow_id)) {
        throw std::invalid_argument("duplicate fluid flow id");
    }
    if (_source_uplink_capacity_bps.count(flow.source_node) == 0) {
        throw std::invalid_argument("missing source uplink capacity for fluid flow");
    }
    if (_destination_downlink_capacity_bps.count(flow.destination_node) == 0) {
        throw std::invalid_argument("missing destination downlink capacity for fluid flow");
    }

    advanceTo(now_ps);

    FlowState state;
    state.spec = flow;
    state.remaining_bits = static_cast<long double>(flow.size_bytes) * 8.0L;
    if (flow.size_bytes == 0) {
        state.service_completion_time_ps = now_ps;
        state.delivery_completion_time_ps = deliveryTimeFor(now_ps);
    }

    _flows.emplace(flow.flow_id, state);
    recomputeAllocation();
}

void RnicFluidManifold::advanceTo(TimePs now_ps) {
    if (now_ps < _now_ps) {
        throw std::invalid_argument("fluid manifold time cannot move backwards");
    }

    while (_now_ps < now_ps) {
        const std::optional<TimePs> next_completion = nextServiceCompletionTime();
        if (!next_completion.has_value() || *next_completion > now_ps) {
            serveUntil(now_ps);
            break;
        }

        serveUntil(*next_completion);
        completeServicedFlows();
        recomputeAllocation();
    }

    // A zero-duration call may still settle a zero-sized or exactly completed
    // flow before a same-timestamp join is applied.
    completeServicedFlows();
}

size_t RnicFluidManifold::activeFlowCount() const {
    size_t count = 0;
    for (const auto& item : _flows) {
        if (!item.second.service_completion_time_ps.has_value()) {
            ++count;
        }
    }
    return count;
}

bool RnicFluidManifold::contains(FlowId flow_id) const {
    return _flows.count(flow_id) != 0;
}

RnicFluidFlowSnapshot RnicFluidManifold::flow(FlowId flow_id) const {
    const auto item = _flows.find(flow_id);
    if (item == _flows.end()) {
        throw std::out_of_range("unknown fluid flow id");
    }
    const FlowState& state = item->second;
    return {state.spec,
            state.remaining_bits,
            state.rate_bps,
            state.service_completion_time_ps,
            state.delivery_completion_time_ps};
}

std::optional<RnicFluidManifold::TimePs> RnicFluidManifold::nextServiceCompletionTime() const {
    std::optional<TimePs> next;
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.service_completion_time_ps.has_value() || state.rate_bps == 0) {
            continue;
        }

        long double duration = std::ceil(
            state.remaining_bits * kPicosecondsPerSecond
            / static_cast<long double>(state.rate_bps));
        duration = std::max(1.0L, duration);
        const long double max_duration = static_cast<long double>(
            std::numeric_limits<TimePs>::max() - _now_ps);
        if (duration > max_duration) {
            throw std::overflow_error("fluid service completion time overflow");
        }
        const TimePs completion = _now_ps + static_cast<TimePs>(duration);
        if (!next.has_value() || completion < *next) {
            next = completion;
        }
    }
    return next;
}

void RnicFluidManifold::serveUntil(TimePs time_ps) {
    if (time_ps < _now_ps) {
        throw std::logic_error("fluid service interval is negative");
    }
    const TimePs duration_ps = time_ps - _now_ps;
    for (auto& item : _flows) {
        FlowState& state = item.second;
        if (state.service_completion_time_ps.has_value() || state.rate_bps == 0) {
            continue;
        }
        const long double served_bits = static_cast<long double>(state.rate_bps)
                                        * static_cast<long double>(duration_ps)
                                        / kPicosecondsPerSecond;
        state.remaining_bits = std::max(0.0L, state.remaining_bits - served_bits);
    }
    _now_ps = time_ps;
}

void RnicFluidManifold::completeServicedFlows() {
    for (auto& item : _flows) {
        FlowState& state = item.second;
        if (state.service_completion_time_ps.has_value()
            || state.remaining_bits > kCompletionEpsilonBits) {
            continue;
        }
        state.remaining_bits = 0.0L;
        state.rate_bps = 0;
        state.service_completion_time_ps = _now_ps;
        state.delivery_completion_time_ps = deliveryTimeFor(_now_ps);
    }
}

void RnicFluidManifold::recomputeAllocation() {
    std::vector<RnicMaxMinFlow> active_flows;
    active_flows.reserve(activeFlowCount());
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (state.service_completion_time_ps.has_value()) {
            continue;
        }
        active_flows.push_back({state.spec.flow_id,
                                state.spec.source_node,
                                state.spec.destination_node,
                                state.spec.demand_cap_bps});
    }

    const RnicMaxMinAllocator::AllocationMap allocation = RnicMaxMinAllocator::allocate(
        active_flows,
        _source_uplink_capacity_bps,
        _destination_downlink_capacity_bps);
    for (auto& item : _flows) {
        FlowState& state = item.second;
        const auto rate = allocation.find(state.spec.flow_id);
        state.rate_bps = rate == allocation.end() ? 0 : rate->second;
    }
    ++_allocation_epoch;
}

RnicFluidManifold::TimePs RnicFluidManifold::deliveryTimeFor(
        TimePs service_completion_time_ps) const {
    if (_propagation_delay_ps
        > std::numeric_limits<TimePs>::max() - service_completion_time_ps) {
        throw std::overflow_error("fluid delivery completion time overflow");
    }
    return service_completion_time_ps + _propagation_delay_ps;
}
