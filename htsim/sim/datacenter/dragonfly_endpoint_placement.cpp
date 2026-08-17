// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dragonfly_endpoint_placement.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace htsim {
namespace {

bool fitsUint32(std::uint64_t value) {
    return value <= std::numeric_limits<std::uint32_t>::max();
}

}  // namespace

DragonflyPhysicalEndpointId DragonflyEndpointPlacement::physicalEndpointForRank(
    DragonflyLogicalRankId logical_rank) const {
    if (logical_rank >= _logical_to_physical.size()) {
        throw std::out_of_range("Dragonfly logical rank is out of range");
    }
    return _logical_to_physical[logical_rank];
}

std::optional<DragonflyLogicalRankId> DragonflyEndpointPlacement::logicalRankForPhysicalEndpoint(
    DragonflyPhysicalEndpointId physical_endpoint) const {
    if (physical_endpoint >= _physical_to_logical.size()) {
        throw std::out_of_range("Dragonfly physical endpoint is out of range");
    }
    return _physical_to_logical[physical_endpoint];
}

bool DragonflyEndpointPlacement::isActive(DragonflyPhysicalEndpointId physical_endpoint) const {
    return logicalRankForPhysicalEndpoint(physical_endpoint).has_value();
}

std::uint32_t DragonflyEndpointPlacement::activeCountForGroup(std::uint32_t group) const {
    if (group >= _active_counts_by_group.size()) {
        throw std::out_of_range("Dragonfly group is out of range");
    }
    return _active_counts_by_group[group];
}

std::string DragonflyEndpointPlacementEnsemble::geometryValidationError(
    DragonflyEndpointGeometry geometry) {
    if (geometry.hosts_per_router == 0 || geometry.routers_per_group == 0 ||
        geometry.global_links_per_router == 0) {
        return "Dragonfly p, a, and h must all be positive";
    }

    const std::uint64_t group_count =
        static_cast<std::uint64_t>(geometry.routers_per_group) * geometry.global_links_per_router +
        1;
    const std::uint64_t router_count = group_count * geometry.routers_per_group;
    const std::uint64_t endpoints_per_group =
        static_cast<std::uint64_t>(geometry.hosts_per_router) * geometry.routers_per_group;
    const std::uint64_t physical_endpoint_count = group_count * endpoints_per_group;

    if (!fitsUint32(group_count) || !fitsUint32(router_count) || !fitsUint32(endpoints_per_group) ||
        !fitsUint32(physical_endpoint_count)) {
        return "Dragonfly placement geometry exceeds 32-bit identifiers";
    }
    if (endpoints_per_group != group_count - 1) {
        return "Balanced placement requires endpoints_per_group == "
               "group_count - 1";
    }
    return {};
}

DragonflyEndpointPlacementEnsemble::DragonflyEndpointPlacementEnsemble(
    DragonflyEndpointGeometry geometry)
    : _geometry(geometry) {
    const std::string error = geometryValidationError(geometry);
    if (!error.empty()) {
        throw std::invalid_argument(error);
    }

    _group_count = geometry.routers_per_group * geometry.global_links_per_router + 1;
    _router_count = _group_count * geometry.routers_per_group;
    _endpoints_per_group = geometry.hosts_per_router * geometry.routers_per_group;
    _physical_endpoint_count = _group_count * _endpoints_per_group;
    _logical_rank_count = _physical_endpoint_count - (_group_count - 1);

    _placements.reserve(_group_count);
    for (std::uint32_t placement_index = 0; placement_index < _group_count; ++placement_index) {
        DragonflyEndpointPlacement placement;
        placement._index = placement_index;
        placement._full_group = placement_index;
        placement._physical_to_logical.resize(_physical_endpoint_count);
        placement._active_counts_by_group.assign(_group_count, _endpoints_per_group - 1);
        placement._active_counts_by_group[placement_index] = _endpoints_per_group;
        placement._idle_endpoints.reserve(_group_count - 1);

        for (std::uint32_t group = 0; group < _group_count; ++group) {
            if (group == placement_index) {
                continue;
            }

            // placement_index - group runs through 1..G-1 modulo G as the
            // full group rotates. Subtracting one maps those differences
            // bijectively onto every local endpoint index in the group.
            const std::uint32_t difference =
                (placement_index + _group_count - group) % _group_count;
            const std::uint32_t local_endpoint = difference - 1;
            placement._idle_endpoints.push_back(group * _endpoints_per_group + local_endpoint);
        }
        std::sort(placement._idle_endpoints.begin(), placement._idle_endpoints.end());

        placement._active_endpoints.reserve(_logical_rank_count);
        placement._logical_to_physical.reserve(_logical_rank_count);
        std::size_t next_idle = 0;
        for (DragonflyPhysicalEndpointId physical_endpoint = 0;
             physical_endpoint < _physical_endpoint_count; ++physical_endpoint) {
            if (next_idle < placement._idle_endpoints.size() &&
                placement._idle_endpoints[next_idle] == physical_endpoint) {
                ++next_idle;
                continue;
            }
            placement._active_endpoints.push_back(physical_endpoint);
        }

        // Keep the active set in stable physical order, but rotate logical
        // rank blocks with the full group.  Rank 0 therefore starts in each
        // group exactly once, and every rank visits every group exactly once
        // over the complete G-placement ensemble.
        for (std::uint32_t group_offset = 0; group_offset < _group_count; ++group_offset) {
            const std::uint32_t group = (placement_index + group_offset) % _group_count;
            const DragonflyPhysicalEndpointId group_begin = group * _endpoints_per_group;
            const DragonflyPhysicalEndpointId group_end = group_begin + _endpoints_per_group;
            for (DragonflyPhysicalEndpointId physical_endpoint = group_begin;
                 physical_endpoint < group_end; ++physical_endpoint) {
                if (!std::binary_search(placement._active_endpoints.begin(),
                                        placement._active_endpoints.end(), physical_endpoint)) {
                    continue;
                }
                const auto logical_rank =
                    static_cast<DragonflyLogicalRankId>(placement._logical_to_physical.size());
                placement._logical_to_physical.push_back(physical_endpoint);
                placement._physical_to_logical[physical_endpoint] = logical_rank;
            }
        }
        _placements.push_back(std::move(placement));
    }

    if (!validate()) {
        throw std::logic_error("Constructed Dragonfly endpoint placement ensemble is invalid");
    }
}

const DragonflyEndpointPlacement& DragonflyEndpointPlacementEnsemble::placement(
    std::uint32_t placement_index) const {
    if (placement_index >= _placements.size()) {
        throw std::out_of_range("Dragonfly placement index is out of range");
    }
    return _placements[placement_index];
}

std::uint32_t DragonflyEndpointPlacementEnsemble::groupForPhysicalEndpoint(
    DragonflyPhysicalEndpointId physical_endpoint) const {
    if (physical_endpoint >= _physical_endpoint_count) {
        throw std::out_of_range("Dragonfly physical endpoint is out of range");
    }
    return physical_endpoint / _endpoints_per_group;
}

std::uint32_t DragonflyEndpointPlacementEnsemble::routerForPhysicalEndpoint(
    DragonflyPhysicalEndpointId physical_endpoint) const {
    if (physical_endpoint >= _physical_endpoint_count) {
        throw std::out_of_range("Dragonfly physical endpoint is out of range");
    }
    return physical_endpoint / _geometry.hosts_per_router;
}

std::uint32_t DragonflyEndpointPlacementEnsemble::hostSlotForPhysicalEndpoint(
    DragonflyPhysicalEndpointId physical_endpoint) const {
    if (physical_endpoint >= _physical_endpoint_count) {
        throw std::out_of_range("Dragonfly physical endpoint is out of range");
    }
    return physical_endpoint % _geometry.hosts_per_router;
}

bool DragonflyEndpointPlacementEnsemble::validate() const {
    if (!geometryValidationError(_geometry).empty() || _placements.size() != _group_count) {
        return false;
    }

    std::vector<std::uint32_t> full_group_frequency(_group_count, 0);
    std::vector<std::uint32_t> idle_frequency(_physical_endpoint_count, 0);
    std::vector<std::vector<std::uint32_t>> logical_group_frequency(
        _logical_rank_count, std::vector<std::uint32_t>(_group_count, 0));

    for (std::uint32_t index = 0; index < _placements.size(); ++index) {
        const auto& placement = _placements[index];
        if (placement._index != index || placement._full_group >= _group_count ||
            placement._logical_to_physical.size() != _logical_rank_count ||
            placement._active_endpoints.size() != _logical_rank_count ||
            placement._idle_endpoints.size() != _group_count - 1 ||
            placement._physical_to_logical.size() != _physical_endpoint_count ||
            placement._active_counts_by_group.size() != _group_count ||
            !std::is_sorted(placement._active_endpoints.begin(),
                            placement._active_endpoints.end()) ||
            !std::is_sorted(placement._idle_endpoints.begin(), placement._idle_endpoints.end())) {
            return false;
        }

        auto logical_endpoint_set = placement._logical_to_physical;
        std::sort(logical_endpoint_set.begin(), logical_endpoint_set.end());
        if (logical_endpoint_set != placement._active_endpoints) {
            return false;
        }
        ++full_group_frequency[placement._full_group];

        std::vector<bool> seen(_physical_endpoint_count, false);
        for (std::uint32_t rank = 0; rank < placement._logical_to_physical.size(); ++rank) {
            const auto physical = placement._logical_to_physical[rank];
            if (physical >= _physical_endpoint_count || seen[physical] ||
                placement._physical_to_logical[physical] != rank) {
                return false;
            }
            seen[physical] = true;
            ++logical_group_frequency[rank][physical / _endpoints_per_group];
        }
        for (const auto physical : placement._idle_endpoints) {
            if (physical >= _physical_endpoint_count || seen[physical] ||
                placement._physical_to_logical[physical].has_value()) {
                return false;
            }
            seen[physical] = true;
            ++idle_frequency[physical];
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
            return false;
        }

        for (std::uint32_t group = 0; group < _group_count; ++group) {
            const std::uint32_t expected =
                group == placement._full_group ? _endpoints_per_group : _endpoints_per_group - 1;
            if (placement._active_counts_by_group[group] != expected) {
                return false;
            }
        }
    }

    return std::all_of(full_group_frequency.begin(), full_group_frequency.end(),
                       [](std::uint32_t frequency) { return frequency == 1; }) &&
           std::all_of(idle_frequency.begin(), idle_frequency.end(),
                       [](std::uint32_t frequency) { return frequency == 1; }) &&
           std::all_of(logical_group_frequency.begin(), logical_group_frequency.end(),
                       [](const std::vector<std::uint32_t>& frequencies) {
                           return std::all_of(
                               frequencies.begin(), frequencies.end(),
                               [](std::uint32_t frequency) { return frequency == 1; });
                       });
}

}  // namespace htsim
