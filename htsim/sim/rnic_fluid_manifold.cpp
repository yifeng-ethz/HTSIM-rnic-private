// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_fluid_manifold.h"

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
    state.remaining_service_debt =
        static_cast<ServiceDebt>(flow.size_bytes)
        * static_cast<ServiceDebt>(8)
        * kPicosecondsPerSecond;
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

        // Delivery is part of the completion transition.  Validate its fixed
        // propagation shift before changing time or service debt so an
        // unrepresentable delivery cannot partially advance the manifold.
        static_cast<void>(deliveryTimeFor(*next_completion));
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
            state.rate_bps,
            state.service_completion_time_ps,
            state.delivery_completion_time_ps};
}

std::optional<RnicFluidManifold::TimePs>
RnicFluidManifold::projectedServiceCompletionTime(FlowId flow_id) const {
    const auto item = _flows.find(flow_id);
    if (item == _flows.end()) {
        throw std::out_of_range("unknown fluid flow id");
    }
    const FlowState& state = item->second;
    if (state.service_completion_time_ps.has_value() || state.rate_bps == 0) {
        return std::nullopt;
    }
    if (state.remaining_service_debt == 0) {
        throw std::logic_error("active fluid flow has no remaining service");
    }

    const ServiceDebt rate = static_cast<ServiceDebt>(state.rate_bps);
    ServiceDebt duration = state.remaining_service_debt / rate;
    if (state.remaining_service_debt % rate != 0) {
        ++duration;
    }

    const TimePs maximum_duration = std::numeric_limits<TimePs>::max() - _now_ps;
    if (duration > static_cast<ServiceDebt>(maximum_duration)) {
        throw std::overflow_error("fluid service completion time overflow");
    }
    return _now_ps + static_cast<TimePs>(duration);
}

std::optional<RnicFluidManifold::TimePs> RnicFluidManifold::nextServiceCompletionTime() const {
    std::optional<TimePs> next;
    for (const auto& item : _flows) {
        const std::optional<TimePs> completion =
            projectedServiceCompletionTime(item.first);
        if (completion.has_value() && (!next.has_value() || *completion < *next)) {
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
        const ServiceDebt served =
            static_cast<ServiceDebt>(state.rate_bps)
            * static_cast<ServiceDebt>(duration_ps);
        if (served >= state.remaining_service_debt) {
            state.remaining_service_debt = 0;
        } else {
            state.remaining_service_debt -= served;
        }
    }
    _now_ps = time_ps;
}

void RnicFluidManifold::completeServicedFlows() {
    bool has_new_completion = false;
    for (const auto& item : _flows) {
        const FlowState& state = item.second;
        if (!state.service_completion_time_ps.has_value()
            && state.remaining_service_debt == 0) {
            has_new_completion = true;
            break;
        }
    }
    if (!has_new_completion) {
        return;
    }

    // All flows completed in this event share the same last-bit time and fixed
    // propagation.  Compute it once before mutating any flow so tied
    // completions commit together or not at all.
    const TimePs delivery_completion_time_ps = deliveryTimeFor(_now_ps);
    for (auto& item : _flows) {
        FlowState& state = item.second;
        if (state.service_completion_time_ps.has_value()
            || state.remaining_service_debt != 0) {
            continue;
        }
        state.rate_bps = 0;
        state.service_completion_time_ps = _now_ps;
        state.delivery_completion_time_ps = delivery_completion_time_ps;
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
