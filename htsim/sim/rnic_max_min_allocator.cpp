// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_max_min_allocator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
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
using WideInteger = unsigned __int128;

static_assert(std::numeric_limits<size_t>::digits <= 128,
              "size_t must fit the exact rational integer type");
constexpr WideInteger kWideIntegerMaximum =
    std::numeric_limits<WideInteger>::max();

WideInteger checked_add(WideInteger lhs,
                        WideInteger rhs,
                        const char* operation) {
    if (rhs > kWideIntegerMaximum - lhs) {
        throw std::overflow_error(std::string("exact max-min ") + operation
                                  + " overflow");
    }
    return lhs + rhs;
}

WideInteger checked_subtract(WideInteger lhs,
                             WideInteger rhs,
                             const char* operation) {
    if (rhs > lhs) {
        throw std::logic_error(std::string("exact max-min ") + operation
                               + " underflow");
    }
    return lhs - rhs;
}

WideInteger checked_multiply(WideInteger lhs,
                             WideInteger rhs,
                             const char* operation) {
    if (lhs != 0 && rhs > kWideIntegerMaximum / lhs) {
        throw std::overflow_error(std::string("exact max-min ") + operation
                                  + " overflow");
    }
    return lhs * rhs;
}

WideInteger greatest_common_divisor(WideInteger lhs, WideInteger rhs) {
    while (rhs != 0) {
        const WideInteger remainder = lhs % rhs;
        lhs = rhs;
        rhs = remainder;
    }
    return lhs;
}

struct DoubleWideInteger {
    // Little-endian base-2^64 limbs.
    std::array<uint64_t, 4> limbs{};
};

DoubleWideInteger multiply_double_wide(WideInteger lhs, WideInteger rhs) {
    const uint64_t lhs_low = static_cast<uint64_t>(lhs);
    const uint64_t lhs_high = static_cast<uint64_t>(lhs >> 64);
    const uint64_t rhs_low = static_cast<uint64_t>(rhs);
    const uint64_t rhs_high = static_cast<uint64_t>(rhs >> 64);

    const WideInteger low_low =
        static_cast<WideInteger>(lhs_low) * rhs_low;
    const WideInteger low_high =
        static_cast<WideInteger>(lhs_low) * rhs_high;
    const WideInteger high_low =
        static_cast<WideInteger>(lhs_high) * rhs_low;
    const WideInteger high_high =
        static_cast<WideInteger>(lhs_high) * rhs_high;

    DoubleWideInteger result;
    result.limbs[0] = static_cast<uint64_t>(low_low);

    const WideInteger middle = checked_add(
        checked_add(low_low >> 64,
                    static_cast<uint64_t>(low_high),
                    "double-wide multiplication"),
        static_cast<uint64_t>(high_low),
        "double-wide multiplication");
    result.limbs[1] = static_cast<uint64_t>(middle);

    const WideInteger upper_middle = checked_add(
        checked_add(
            checked_add(low_high >> 64,
                        high_low >> 64,
                        "double-wide multiplication"),
            static_cast<uint64_t>(high_high),
            "double-wide multiplication"),
        middle >> 64,
        "double-wide multiplication");
    result.limbs[2] = static_cast<uint64_t>(upper_middle);

    const WideInteger upper = checked_add(high_high >> 64,
                                          upper_middle >> 64,
                                          "double-wide multiplication");
    if (upper > std::numeric_limits<uint64_t>::max()) {
        throw std::logic_error("double-wide multiplication produced a 257th bit");
    }
    result.limbs[3] = static_cast<uint64_t>(upper);
    return result;
}

bool double_wide_less_than(const DoubleWideInteger& lhs, WideInteger rhs) {
    if (lhs.limbs[3] != 0 || lhs.limbs[2] != 0) {
        return false;
    }
    const WideInteger lhs_low =
        (static_cast<WideInteger>(lhs.limbs[1]) << 64) | lhs.limbs[0];
    return lhs_low < rhs;
}

DoubleWideInteger subtract_from_double_wide(DoubleWideInteger lhs,
                                            WideInteger rhs) {
    const std::array<uint64_t, 4> rhs_limbs{
        static_cast<uint64_t>(rhs),
        static_cast<uint64_t>(rhs >> 64),
        0,
        0,
    };

    bool borrow = false;
    for (size_t i = 0; i < lhs.limbs.size(); ++i) {
        const uint64_t original = lhs.limbs[i];
        const uint64_t subtrahend = rhs_limbs[i];
        lhs.limbs[i] = original - subtrahend - static_cast<uint64_t>(borrow);
        const bool next_borrow = original < subtrahend
                                 || (borrow && original == subtrahend);
        borrow = next_borrow;
    }
    if (borrow) {
        throw std::logic_error("double-wide subtraction underflow");
    }
    return lhs;
}

bool double_wide_bit(const DoubleWideInteger& value, unsigned bit) {
    return ((value.limbs.at(bit / 64) >> (bit % 64)) & 1U) != 0;
}

struct DoubleWideDivision {
    WideInteger quotient;
    WideInteger remainder;
};

DoubleWideDivision divide_double_wide(const DoubleWideInteger& dividend,
                                      WideInteger divisor,
                                      bool require_quotient_to_fit) {
    if (divisor == 0) {
        throw std::logic_error("exact max-min division by zero");
    }

    WideInteger quotient = 0;
    WideInteger remainder = 0;
    for (int bit = 255; bit >= 0; --bit) {
        const bool remainder_high_bit = (remainder >> 127) != 0;
        remainder <<= 1;
        if (double_wide_bit(dividend, static_cast<unsigned>(bit))) {
            remainder |= 1;
        }

        const bool subtract = remainder_high_bit || remainder >= divisor;
        if (!subtract) {
            continue;
        }

        if (remainder_high_bit) {
            // The captured high bit represents 2^128.  Because the old
            // remainder was below divisor, the exact post-subtraction result
            // is below divisor and therefore fits back into 128 bits.
            const WideInteger high_unit_minus_divisor =
                checked_add(checked_subtract(kWideIntegerMaximum,
                                             divisor,
                                             "double-wide division"),
                            1,
                            "double-wide division");
            remainder = checked_add(remainder,
                                    high_unit_minus_divisor,
                                    "double-wide division");
        } else {
            remainder = checked_subtract(remainder,
                                         divisor,
                                         "double-wide division");
        }

        if (!require_quotient_to_fit) {
            continue;
        }
        if (bit >= 128) {
            throw std::overflow_error(
                "exact max-min normalized rational numerator overflow");
        }
        quotient |= static_cast<WideInteger>(1) << bit;
    }
    return {quotient, remainder};
}

WideInteger double_wide_modulo(const DoubleWideInteger& value,
                               WideInteger divisor) {
    return divide_double_wide(value, divisor, false).remainder;
}

WideInteger add_modulo(WideInteger lhs,
                       WideInteger rhs,
                       WideInteger modulus) {
    if (modulus == 0 || lhs >= modulus || rhs >= modulus) {
        throw std::logic_error("invalid exact max-min modular addition");
    }
    const WideInteger distance_to_modulus =
        checked_subtract(modulus, rhs, "modular addition");
    if (lhs >= distance_to_modulus) {
        return checked_subtract(lhs, distance_to_modulus, "modular addition");
    }
    return checked_add(lhs, rhs, "modular addition");
}

// Exact normalized, nonnegative rational.  The supported domain consists of
// operations whose normalized numerator and denominator each fit 128 bits.
// Wider temporary products are represented explicitly; there is no
// floating-point fallback.
class ExactRate {
public:
    ExactRate() = default;
    explicit ExactRate(RateBps integer) : _numerator(integer) {}

    ExactRate add(const ExactRate& other) const {
        const WideInteger denominator_gcd =
            greatest_common_divisor(_denominator, other._denominator);
        const WideInteger lhs_factor = other._denominator / denominator_gcd;
        const WideInteger rhs_factor = _denominator / denominator_gcd;
        const DoubleWideInteger lhs_product =
            multiply_double_wide(_numerator, lhs_factor);
        const DoubleWideInteger rhs_product =
            multiply_double_wide(other._numerator, rhs_factor);

        WideInteger numerator_gcd = 1;
        if (denominator_gcd != 1) {
            const WideInteger lhs_modulo =
                double_wide_modulo(lhs_product, denominator_gcd);
            const WideInteger rhs_modulo =
                double_wide_modulo(rhs_product, denominator_gcd);
            const WideInteger numerator_modulo =
                add_modulo(lhs_modulo, rhs_modulo, denominator_gcd);
            numerator_gcd =
                greatest_common_divisor(numerator_modulo, denominator_gcd);
        }

        const DoubleWideDivision lhs_division =
            divide_double_wide(lhs_product, numerator_gcd, true);
        const DoubleWideDivision rhs_division =
            divide_double_wide(rhs_product, numerator_gcd, true);
        WideInteger numerator = checked_add(lhs_division.quotient,
                                            rhs_division.quotient,
                                            "rational addition");

        const WideInteger residue_threshold =
            checked_subtract(numerator_gcd,
                             rhs_division.remainder,
                             "rational residue addition");
        WideInteger combined_residue = 0;
        bool residue_carry = false;
        if (lhs_division.remainder >= residue_threshold) {
            combined_residue = checked_subtract(lhs_division.remainder,
                                                 residue_threshold,
                                                 "rational residue addition");
            residue_carry = true;
        } else {
            combined_residue = checked_add(lhs_division.remainder,
                                           rhs_division.remainder,
                                           "rational residue addition");
        }
        if (combined_residue != 0) {
            throw std::logic_error("exact rational normalization failed");
        }
        if (residue_carry) {
            numerator = checked_add(numerator, 1, "rational addition");
        }

        const WideInteger denominator = checked_multiply(
            _denominator / numerator_gcd,
            lhs_factor,
            "normalized rational denominator");
        return ExactRate(numerator, denominator, NormalizedTag{});
    }

    ExactRate subtractFromInteger(RateBps integer) const {
        DoubleWideInteger numerator = multiply_double_wide(
            static_cast<WideInteger>(integer), _denominator);
        if (double_wide_less_than(numerator, _numerator)) {
            throw std::logic_error("exact max-min residual capacity is negative");
        }
        numerator = subtract_from_double_wide(numerator, _numerator);

        const WideInteger numerator_modulo =
            double_wide_modulo(numerator, _denominator);
        const WideInteger common =
            greatest_common_divisor(numerator_modulo, _denominator);
        const DoubleWideDivision normalized_numerator =
            divide_double_wide(numerator, common, true);
        if (normalized_numerator.remainder != 0) {
            throw std::logic_error("exact rational subtraction normalization failed");
        }
        return ExactRate(normalized_numerator.quotient,
                         _denominator / common,
                         NormalizedTag{});
    }

    ExactRate divideBy(size_t divisor) const {
        if (divisor == 0) {
            throw std::logic_error("exact max-min division by zero");
        }
        const WideInteger wide_divisor = static_cast<WideInteger>(divisor);
        const WideInteger common =
            greatest_common_divisor(_numerator, wide_divisor);
        const WideInteger numerator = _numerator / common;
        const WideInteger denominator_factor = wide_divisor / common;
        const WideInteger denominator = checked_multiply(
            _denominator,
            denominator_factor,
            "normalized rational denominator");
        return ExactRate(numerator, denominator, NormalizedTag{});
    }

    RateBps floorToRateBps() const {
        const WideInteger integer = _numerator / _denominator;
        if (integer > std::numeric_limits<RateBps>::max()) {
            throw std::overflow_error("exact max-min whole-bps result overflow");
        }
        return static_cast<RateBps>(integer);
    }

    int compare(const ExactRate& other) const {
        WideInteger lhs_numerator = _numerator;
        WideInteger lhs_denominator = _denominator;
        WideInteger rhs_numerator = other._numerator;
        WideInteger rhs_denominator = other._denominator;
        bool reversed = false;

        while (true) {
            const WideInteger lhs_quotient = lhs_numerator / lhs_denominator;
            const WideInteger rhs_quotient = rhs_numerator / rhs_denominator;
            if (lhs_quotient != rhs_quotient) {
                const bool lhs_is_less = reversed
                                             ? lhs_quotient > rhs_quotient
                                             : lhs_quotient < rhs_quotient;
                return lhs_is_less ? -1 : 1;
            }

            const WideInteger lhs_remainder = lhs_numerator % lhs_denominator;
            const WideInteger rhs_remainder = rhs_numerator % rhs_denominator;
            if (lhs_remainder == 0 || rhs_remainder == 0) {
                if (lhs_remainder == 0 && rhs_remainder == 0) {
                    return 0;
                }
                const int comparison = lhs_remainder == 0 ? -1 : 1;
                return reversed ? -comparison : comparison;
            }

            lhs_numerator = lhs_denominator;
            lhs_denominator = lhs_remainder;
            rhs_numerator = rhs_denominator;
            rhs_denominator = rhs_remainder;
            reversed = !reversed;
        }
    }

    bool operator<(const ExactRate& other) const { return compare(other) < 0; }
    bool operator==(const ExactRate& other) const { return compare(other) == 0; }

private:
    struct NormalizedTag {};

    ExactRate(WideInteger numerator,
              WideInteger denominator,
              NormalizedTag)
        : _numerator(numerator), _denominator(denominator) {
        if (_denominator == 0) {
            throw std::logic_error("exact rational has a zero denominator");
        }
    }

    WideInteger _numerator = 0;
    WideInteger _denominator = 1;
};

struct FlowState {
    Flow flow;
    ExactRate exact_rate;
    bool active = true;
};

struct ResourceState {
    RateBps capacity_bps;
    ExactRate frozen_load;
    size_t active_flow_count = 0;
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

void increment_active_count(ResourceState& resource) {
    if (resource.active_flow_count == std::numeric_limits<size_t>::max()) {
        throw std::overflow_error("exact max-min active-flow count overflow");
    }
    ++resource.active_flow_count;
}

void decrement_active_count(ResourceState& resource) {
    if (resource.active_flow_count == 0) {
        throw std::logic_error("exact max-min active-flow count underflow");
    }
    --resource.active_flow_count;
}

void consider_candidate(const ExactRate& candidate,
                        const ExactRate& current_level,
                        std::optional<ExactRate>& next_level) {
    if (candidate < current_level) {
        throw std::logic_error("exact max-min water level moved backwards");
    }
    if (!next_level.has_value() || candidate < *next_level) {
        next_level = candidate;
    }
}

}  // namespace

RnicMaxMinAllocator::AllocationMap RnicMaxMinAllocator::allocate(
        const std::vector<RnicMaxMinFlow>& active_flows,
        const CapacityMap& source_uplink_capacity_bps,
        const CapacityMap& destination_downlink_capacity_bps) {
    std::vector<FlowState> flows;
    flows.reserve(active_flows.size());

    std::set<FlowId> flow_ids;
    std::map<NodeId, ResourceState> sources;
    std::map<NodeId, ResourceState> destinations;
    for (const Flow& flow : active_flows) {
        if (!flow_ids.insert(flow.flow_id).second) {
            throw std::invalid_argument("duplicate active flow id "
                                        + std::to_string(flow.flow_id));
        }
        const RateBps source_capacity = require_capacity(
            source_uplink_capacity_bps, flow.source_node, "source uplink");
        const RateBps destination_capacity = require_capacity(
            destination_downlink_capacity_bps,
            flow.destination_node,
            "destination downlink");

        auto source = sources.try_emplace(
            flow.source_node, ResourceState{source_capacity, ExactRate(), 0});
        auto destination = destinations.try_emplace(
            flow.destination_node,
            ResourceState{destination_capacity, ExactRate(), 0});
        increment_active_count(source.first->second);
        increment_active_count(destination.first->second);
        flows.push_back({flow, ExactRate(), true});
    }

    std::sort(flows.begin(), flows.end(), [](const FlowState& lhs, const FlowState& rhs) {
        return lhs.flow.flow_id < rhs.flow.flow_id;
    });

    ExactRate current_level;
    size_t active_count = flows.size();
    while (active_count > 0) {
        std::map<NodeId, ExactRate> source_candidates;
        std::map<NodeId, ExactRate> destination_candidates;
        std::vector<std::optional<ExactRate>> demand_candidates(flows.size());
        std::optional<ExactRate> next_level;

        for (const auto& entry : sources) {
            const ResourceState& resource = entry.second;
            if (resource.active_flow_count == 0) {
                continue;
            }
            const ExactRate candidate = resource.frozen_load
                                            .subtractFromInteger(resource.capacity_bps)
                                            .divideBy(resource.active_flow_count);
            source_candidates.emplace(entry.first, candidate);
            consider_candidate(candidate, current_level, next_level);
        }

        for (const auto& entry : destinations) {
            const ResourceState& resource = entry.second;
            if (resource.active_flow_count == 0) {
                continue;
            }
            const ExactRate candidate = resource.frozen_load
                                            .subtractFromInteger(resource.capacity_bps)
                                            .divideBy(resource.active_flow_count);
            destination_candidates.emplace(entry.first, candidate);
            consider_candidate(candidate, current_level, next_level);
        }

        for (size_t i = 0; i < flows.size(); ++i) {
            const FlowState& state = flows[i];
            if (!state.active || !state.flow.demand_cap_bps.has_value()) {
                continue;
            }
            const ExactRate candidate(*state.flow.demand_cap_bps);
            demand_candidates[i] = candidate;
            consider_candidate(candidate, current_level, next_level);
        }

        if (!next_level.has_value()) {
            throw std::logic_error("unable to find an exact max-min water level");
        }

        std::vector<bool> freeze(flows.size(), false);
        size_t frozen_this_round = 0;
        for (size_t i = 0; i < flows.size(); ++i) {
            const FlowState& state = flows[i];
            if (!state.active) {
                continue;
            }
            const bool source_saturated =
                source_candidates.at(state.flow.source_node) == *next_level;
            const bool destination_saturated =
                destination_candidates.at(state.flow.destination_node) == *next_level;
            const bool demand_saturated = demand_candidates[i].has_value()
                                          && *demand_candidates[i] == *next_level;
            if (source_saturated || destination_saturated || demand_saturated) {
                freeze[i] = true;
                ++frozen_this_round;
            }
        }
        if (frozen_this_round == 0) {
            throw std::logic_error("exact max-min filling made no progress");
        }

        for (size_t i = 0; i < flows.size(); ++i) {
            if (!freeze[i]) {
                continue;
            }
            FlowState& state = flows[i];
            state.exact_rate = *next_level;
            state.active = false;

            ResourceState& source = sources.at(state.flow.source_node);
            ResourceState& destination = destinations.at(state.flow.destination_node);
            source.frozen_load = source.frozen_load.add(*next_level);
            destination.frozen_load = destination.frozen_load.add(*next_level);
            decrement_active_count(source);
            decrement_active_count(destination);
        }
        if (frozen_this_round > active_count) {
            throw std::logic_error("exact max-min active-flow count underflow");
        }
        active_count -= frozen_this_round;
        current_level = *next_level;
    }

    AllocationMap allocation;
    std::map<NodeId, RateBps> allocated_by_source;
    std::map<NodeId, RateBps> allocated_by_destination;
    for (const FlowState& state : flows) {
        const RateBps rate = state.exact_rate.floorToRateBps();
        allocation.emplace(state.flow.flow_id, rate);

        const RateBps source_capacity =
            source_uplink_capacity_bps.at(state.flow.source_node);
        const RateBps destination_capacity =
            destination_downlink_capacity_bps.at(state.flow.destination_node);
        const RateBps source_allocated = allocated_by_source[state.flow.source_node];
        const RateBps destination_allocated =
            allocated_by_destination[state.flow.destination_node];
        if (rate > source_capacity || source_allocated > source_capacity - rate
            || rate > destination_capacity
            || destination_allocated > destination_capacity - rate
            || (state.flow.demand_cap_bps.has_value()
                && rate > *state.flow.demand_cap_bps)) {
            throw std::logic_error("whole-bps max-min allocation is infeasible");
        }
        allocated_by_source[state.flow.source_node] += rate;
        allocated_by_destination[state.flow.destination_node] += rate;
    }

    return allocation;
}
