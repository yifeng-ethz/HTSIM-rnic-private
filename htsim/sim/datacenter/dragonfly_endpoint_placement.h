// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DRAGONFLY_ENDPOINT_PLACEMENT_H
#define DRAGONFLY_ENDPOINT_PLACEMENT_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace htsim {

using DragonflyPhysicalEndpointId = std::uint32_t;
using DragonflyLogicalRankId = std::uint32_t;

struct DragonflyEndpointGeometry {
    std::uint32_t hosts_per_router{0};
    std::uint32_t routers_per_group{0};
    std::uint32_t global_links_per_router{0};
};

// One member of a balanced placement ensemble. Physical endpoint IDs follow
// DragonflyTopology's numbering: endpoints on a router are consecutive, and
// routers in a group are consecutive. Active endpoint IDs remain sorted for
// set-oriented consumers. Logical ranks instead traverse groups cyclically
// from the placement's full group, rotating communication roles as well as
// the active physical set across the ensemble.
class DragonflyEndpointPlacement {
public:
    std::uint32_t index() const { return _index; }
    std::uint32_t fullGroup() const { return _full_group; }
    std::uint32_t logicalRankCount() const {
        return static_cast<std::uint32_t>(_logical_to_physical.size());
    }
    std::uint32_t physicalEndpointCount() const {
        return static_cast<std::uint32_t>(_physical_to_logical.size());
    }

    DragonflyPhysicalEndpointId physicalEndpointForRank(DragonflyLogicalRankId logical_rank) const;
    std::optional<DragonflyLogicalRankId> logicalRankForPhysicalEndpoint(
        DragonflyPhysicalEndpointId physical_endpoint) const;
    bool isActive(DragonflyPhysicalEndpointId physical_endpoint) const;
    std::uint32_t activeCountForGroup(std::uint32_t group) const;

    const std::vector<DragonflyPhysicalEndpointId>& logicalToPhysical() const {
        return _logical_to_physical;
    }
    const std::vector<DragonflyPhysicalEndpointId>& activeEndpoints() const {
        return _active_endpoints;
    }
    const std::vector<DragonflyPhysicalEndpointId>& idleEndpoints() const {
        return _idle_endpoints;
    }
    const std::vector<std::uint32_t>& activeCountsByGroup() const {
        return _active_counts_by_group;
    }

private:
    friend class DragonflyEndpointPlacementEnsemble;

    std::uint32_t _index{0};
    std::uint32_t _full_group{0};
    std::vector<DragonflyPhysicalEndpointId> _logical_to_physical;
    std::vector<std::optional<DragonflyLogicalRankId>> _physical_to_logical;
    std::vector<DragonflyPhysicalEndpointId> _active_endpoints;
    std::vector<DragonflyPhysicalEndpointId> _idle_endpoints;
    std::vector<std::uint32_t> _active_counts_by_group;
};

// Builds the complete rotation used by the 64-rank p2a4h2 experiments. More
// generally, the construction applies exactly when endpoints_per_group equals
// group_count - 1. That equality is necessary: each of G placements has G-1
// idle endpoints, while every one of the G * endpoints_per_group physical
// endpoints must be idle exactly once.
class DragonflyEndpointPlacementEnsemble {
public:
    explicit DragonflyEndpointPlacementEnsemble(DragonflyEndpointGeometry geometry);

    static std::string geometryValidationError(DragonflyEndpointGeometry geometry);

    const DragonflyEndpointGeometry& geometry() const { return _geometry; }
    std::uint32_t hostsPerRouter() const { return _geometry.hosts_per_router; }
    std::uint32_t routersPerGroup() const { return _geometry.routers_per_group; }
    std::uint32_t globalLinksPerRouter() const { return _geometry.global_links_per_router; }
    std::uint32_t groupCount() const { return _group_count; }
    std::uint32_t routerCount() const { return _router_count; }
    std::uint32_t endpointsPerGroup() const { return _endpoints_per_group; }
    std::uint32_t physicalEndpointCount() const { return _physical_endpoint_count; }
    std::uint32_t logicalRankCount() const { return _logical_rank_count; }
    std::uint32_t placementCount() const { return static_cast<std::uint32_t>(_placements.size()); }

    const DragonflyEndpointPlacement& placement(std::uint32_t placement_index) const;

    std::uint32_t groupForPhysicalEndpoint(DragonflyPhysicalEndpointId physical_endpoint) const;
    std::uint32_t routerForPhysicalEndpoint(DragonflyPhysicalEndpointId physical_endpoint) const;
    std::uint32_t hostSlotForPhysicalEndpoint(DragonflyPhysicalEndpointId physical_endpoint) const;

    // Rechecks the complete ensemble, including the cross-placement rotation.
    bool validate() const;

private:
    DragonflyEndpointGeometry _geometry;
    std::uint32_t _group_count{0};
    std::uint32_t _router_count{0};
    std::uint32_t _endpoints_per_group{0};
    std::uint32_t _physical_endpoint_count{0};
    std::uint32_t _logical_rank_count{0};
    std::vector<DragonflyEndpointPlacement> _placements;
};

}  // namespace htsim

#endif
