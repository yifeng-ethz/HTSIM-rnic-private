// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_max_min_allocator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Flow = RnicMaxMinFlow;
using FlowId = RnicMaxMinAllocator::FlowId;
using NodeId = RnicMaxMinAllocator::NodeId;
using RateBps = RnicMaxMinAllocator::RateBps;
using CapacityMap = RnicMaxMinAllocator::CapacityMap;

struct FlowState {
    Flow flow;
    long double rate_bps = 0.0L;
    bool active = true;
};

const RateBps& require_capacity(const CapacityMap& capacities,
                                NodeId node,
                                const char* capacity_name) {
    auto capacity = capacities.find(node);
    if (capacity == capacities.end()) {
        throw std::invalid_argument(std::string("missing ") + capacity_name
                                    + " capacity for node " + std::to_string(node));
    }
    return capacity->second;
}

long double residual_capacity(long double capacity, long double used) {
    return used < capacity ? capacity - used : 0.0L;
}

}  // namespace

RnicMaxMinAllocator::AllocationMap RnicMaxMinAllocator::allocate(
        const std::vector<RnicMaxMinFlow>& active_flows,
        const CapacityMap& source_uplink_capacity_bps,
        const CapacityMap& destination_downlink_capacity_bps) {
    std::vector<FlowState> flows;
    flows.reserve(active_flows.size());

    std::set<FlowId> flow_ids;
    for (const Flow& flow : active_flows) {
        if (!flow_ids.insert(flow.flow_id).second) {
            throw std::invalid_argument("duplicate active flow id " + std::to_string(flow.flow_id));
        }
        require_capacity(source_uplink_capacity_bps, flow.source_node, "source uplink");
        require_capacity(destination_downlink_capacity_bps,
                         flow.destination_node,
                         "destination downlink");
        flows.push_back({flow, 0.0L, true});
    }

    std::sort(flows.begin(), flows.end(), [](const FlowState& lhs, const FlowState& rhs) {
        return lhs.flow.flow_id < rhs.flow.flow_id;
    });

    size_t active_count = flows.size();
    while (active_count > 0) {
        std::map<NodeId, size_t> active_by_source;
        std::map<NodeId, size_t> active_by_destination;
        std::map<NodeId, long double> used_by_source;
        std::map<NodeId, long double> used_by_destination;

        for (const FlowState& state : flows) {
            used_by_source[state.flow.source_node] += state.rate_bps;
            used_by_destination[state.flow.destination_node] += state.rate_bps;
            if (state.active) {
                active_by_source[state.flow.source_node]++;
                active_by_destination[state.flow.destination_node]++;
            }
        }

        std::map<NodeId, long double> source_increments;
        std::map<NodeId, long double> destination_increments;
        std::vector<long double> demand_increments(
            flows.size(), std::numeric_limits<long double>::infinity());
        long double increment = std::numeric_limits<long double>::infinity();

        for (const auto& entry : active_by_source) {
            const long double capacity = static_cast<long double>(
                require_capacity(source_uplink_capacity_bps, entry.first, "source uplink"));
            const long double candidate = residual_capacity(capacity, used_by_source[entry.first])
                                          / static_cast<long double>(entry.second);
            source_increments[entry.first] = candidate;
            increment = std::min(increment, candidate);
        }

        for (const auto& entry : active_by_destination) {
            const long double capacity = static_cast<long double>(
                require_capacity(destination_downlink_capacity_bps,
                                 entry.first,
                                 "destination downlink"));
            const long double candidate = residual_capacity(
                                              capacity, used_by_destination[entry.first])
                                          / static_cast<long double>(entry.second);
            destination_increments[entry.first] = candidate;
            increment = std::min(increment, candidate);
        }

        for (size_t i = 0; i < flows.size(); ++i) {
            const FlowState& state = flows[i];
            if (!state.active || !state.flow.demand_cap_bps.has_value()) {
                continue;
            }
            const long double demand = static_cast<long double>(*state.flow.demand_cap_bps);
            demand_increments[i] = residual_capacity(demand, state.rate_bps);
            increment = std::min(increment, demand_increments[i]);
        }

        if (!std::isfinite(increment) || increment < 0.0L) {
            throw std::logic_error("unable to find a finite max-min filling increment");
        }

        for (FlowState& state : flows) {
            if (state.active) {
                state.rate_bps += increment;
            }
        }

        size_t frozen_this_round = 0;
        for (size_t i = 0; i < flows.size(); ++i) {
            FlowState& state = flows[i];
            if (!state.active) {
                continue;
            }

            const bool source_saturated =
                source_increments.at(state.flow.source_node) == increment;
            const bool destination_saturated =
                destination_increments.at(state.flow.destination_node) == increment;
            const bool demand_saturated = demand_increments[i] == increment;
            if (source_saturated || destination_saturated || demand_saturated) {
                state.active = false;
                frozen_this_round++;
            }
        }

        if (frozen_this_round == 0) {
            throw std::logic_error("max-min filling made no progress");
        }
        active_count -= frozen_this_round;
    }

    AllocationMap allocation;
    std::map<NodeId, RateBps> allocated_by_source;
    std::map<NodeId, RateBps> allocated_by_destination;
    for (const FlowState& state : flows) {
        long double upper_bound = static_cast<long double>(
            source_uplink_capacity_bps.at(state.flow.source_node));
        upper_bound = std::min(
            upper_bound,
            static_cast<long double>(
                destination_downlink_capacity_bps.at(state.flow.destination_node)));
        if (state.flow.demand_cap_bps.has_value()) {
            upper_bound = std::min(
                upper_bound, static_cast<long double>(*state.flow.demand_cap_bps));
        }

        const long double bounded_rate = std::max(0.0L, std::min(state.rate_bps, upper_bound));
        const RateBps rate = static_cast<RateBps>(std::floor(bounded_rate));
        allocation.emplace(state.flow.flow_id, rate);

        const RateBps source_capacity = source_uplink_capacity_bps.at(state.flow.source_node);
        const RateBps destination_capacity =
            destination_downlink_capacity_bps.at(state.flow.destination_node);
        const RateBps source_allocated = allocated_by_source[state.flow.source_node];
        const RateBps destination_allocated =
            allocated_by_destination[state.flow.destination_node];
        if (rate > source_capacity || source_allocated > source_capacity - rate
            || rate > destination_capacity
            || destination_allocated > destination_capacity - rate) {
            throw std::logic_error("rounded max-min allocation exceeds an access capacity");
        }
        allocated_by_source[state.flow.source_node] += rate;
        allocated_by_destination[state.flow.destination_node] += rate;
    }

    return allocation;
}
