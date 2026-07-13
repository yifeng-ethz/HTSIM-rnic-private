// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_route.h"

#include <limits>
#include <stdexcept>

#include "fat_tree_topology.h"
#include "rnic_packet_extent.h"

namespace {

// get_bidir_paths() transfers a heap vector containing heap Route objects.
// Keep that legacy ownership bounded to one build operation and release both
// layers on every success or exception path.
class LegacyRouteOwner {
public:
    explicit LegacyRouteOwner(std::vector<const Route*>* routes)
        : _routes(routes) {}

    ~LegacyRouteOwner() {
        if (_routes == nullptr) {
            return;
        }
        for (const Route* route : *_routes) {
            delete route;
        }
        delete _routes;
    }

    LegacyRouteOwner(const LegacyRouteOwner&) = delete;
    LegacyRouteOwner& operator=(const LegacyRouteOwner&) = delete;

    const std::vector<const Route*>* get() const noexcept { return _routes; }

private:
    std::vector<const Route*>* _routes;
};

std::uint64_t checkedAdd(
        std::uint64_t lhs,
        std::uint64_t rhs,
        const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

std::uint64_t queueDrainTimePs(
        std::uint64_t wire_bytes, linkspeed_bps bitrate) {
    if (bitrate == 0) {
        throw std::invalid_argument(
            "rnic-cn transit calibration requires a nonzero link rate");
    }

    // BaseQueue computes an integral ps_per_byte once, then drainTime()
    // multiplies that value by the packet size.  Keep the same truncation
    // semantics without floating-point arithmetic, and check the multiply.
    constexpr std::uint64_t serialization_numerator_per_byte =
        UINT64_C(8000000000000);
    const std::uint64_t ps_per_byte =
        serialization_numerator_per_byte / bitrate;
    if (ps_per_byte != 0
        && wire_bytes
               > std::numeric_limits<std::uint64_t>::max() / ps_per_byte) {
        throw std::overflow_error(
            "rnic-cn transit serialization time overflow");
    }
    return wire_bytes * ps_per_byte;
}

}  // namespace

std::uint64_t rnicCollectiveNoQueueTransitPs(
        const FatTreeTopologyCfg& topology_config,
        std::uint32_t source,
        std::uint32_t destination,
        const RnicPacketExtent& extent) {
    if (topology_config.get_tiers() != 2) {
        throw std::invalid_argument(
            "rnic-cn transit calibration requires a two-tier Clos");
    }
    if (source >= topology_config.no_of_servers()
        || destination >= topology_config.no_of_servers()) {
        throw std::out_of_range(
            "rnic-cn transit calibration endpoint is outside the Clos");
    }
    if (source == destination) {
        throw std::invalid_argument(
            "rnic-cn transit calibration requires distinct endpoints");
    }

    std::uint64_t transit_ps =
        topology_config.get_two_point_diameter_latency(
            static_cast<int>(source), static_cast<int>(destination));
    const std::uint64_t tor_downlink_serialization_ps =
        queueDrainTimePs(
            extent.wireBytes(),
            topology_config.downlink_speed(TOR_TIER));

    if (topology_config.HOST_POD_SWITCH(source)
        != topology_config.HOST_POD_SWITCH(destination)) {
        const std::uint64_t inter_switch_serialization_ps =
            queueDrainTimePs(
                extent.wireBytes(),
                topology_config.downlink_speed(AGG_TIER));
        transit_ps = checkedAdd(
            transit_ps,
            inter_switch_serialization_ps,
            "rnic-cn transit calibration overflow");
        transit_ps = checkedAdd(
            transit_ps,
            inter_switch_serialization_ps,
            "rnic-cn transit calibration overflow");
    }

    return checkedAdd(
        transit_ps,
        tor_downlink_serialization_ps,
        "rnic-cn transit calibration overflow");
}

RnicCollectiveRouteProvider::RnicCollectiveRouteProvider(
        FatTreeTopology& topology)
    : _topology(topology),
      _node_count(topology.cfg().no_of_servers()),
      _destination_endpoints(_node_count, nullptr) {
    const FatTreeTopologyCfg& config = _topology.cfg();
    if (config.get_tiers() != 2) {
        throw std::invalid_argument(
            "rnic-cn route provider requires a two-tier Clos");
    }
    if (config.switch_model() != FatTreeSwitchModel::Tomahawk3) {
        throw std::invalid_argument(
            "rnic-cn route provider requires Tomahawk 3 switches");
    }
    if (_node_count == 0) {
        throw std::invalid_argument(
            "rnic-cn route provider requires at least one node");
    }
}

RnicCollectiveRouteProvider::~RnicCollectiveRouteProvider() = default;

const RnicCollectiveRouteProvider::RouteView&
RnicCollectiveRouteProvider::routes(
        NodeId source,
        NodeId destination,
        PacketSink& destination_endpoint) {
    validatePair(source, destination);
    PacketSink* const bound_endpoint = _destination_endpoints[destination];
    if (bound_endpoint != nullptr && bound_endpoint != &destination_endpoint) {
        throw std::invalid_argument(
            "rnic-cn destination node is already bound to another endpoint");
    }
    const PairKey key{source, destination};
    const auto existing = _route_sets.find(key);
    if (existing != _route_sets.end()) {
        if (existing->second.destination_endpoint != &destination_endpoint) {
            throw std::invalid_argument(
                "rnic-cn route pair is already bound to another endpoint");
        }
        return existing->second.route_view;
    }

    RouteSet route_set =
        buildRouteSet(source, destination, destination_endpoint);
    const auto inserted = _route_sets.emplace(key, std::move(route_set));
    if (!inserted.second) {
        throw std::logic_error("rnic-cn route cache insertion failed");
    }
    _destination_endpoints[destination] = &destination_endpoint;
    return inserted.first->second.route_view;
}

void RnicCollectiveRouteProvider::validatePair(
        NodeId source, NodeId destination) const {
    if (source >= _node_count || destination >= _node_count) {
        throw std::out_of_range(
            "rnic-cn route endpoint is outside the Clos node range");
    }
    if (source == destination) {
        throw std::invalid_argument(
            "rnic-cn Clos routes require distinct source and destination nodes");
    }
}

RnicCollectiveRouteProvider::RouteSet
RnicCollectiveRouteProvider::buildRouteSet(
        NodeId source,
        NodeId destination,
        PacketSink& destination_endpoint) {
    LegacyRouteOwner legacy_owner(
        _topology.get_bidir_paths(source, destination, false));
    const std::vector<const Route*>* legacy_routes = legacy_owner.get();
    if (legacy_routes == nullptr || legacy_routes->empty()) {
        throw std::logic_error(
            "rnic-cn Clos returned no explicit route for a valid node pair");
    }
    if (legacy_routes->size()
        > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::overflow_error(
            "rnic-cn path count exceeds the Route path-id field");
    }

    const std::uint32_t source_tor =
        _topology.cfg().HOST_POD_SWITCH(source);
    BaseQueue* const expected_source_queue =
        _topology.queues_ns_nlp[source][source_tor][0];
    Pipe* const expected_source_pipe =
        _topology.pipes_ns_nlp[source][source_tor][0];
    const int path_count = static_cast<int>(legacy_routes->size());

    RouteSet result;
    result.destination_endpoint = &destination_endpoint;
    result.owned_routes.reserve(legacy_routes->size());
    result.route_view.reserve(legacy_routes->size());

    for (std::size_t path_index = 0;
         path_index < legacy_routes->size();
         ++path_index) {
        const Route* const legacy_route = legacy_routes->at(path_index);
        if (legacy_route == nullptr || legacy_route->size() < 2) {
            throw std::logic_error(
                "rnic-cn Clos returned a malformed explicit route");
        }
        if (legacy_route->at(0) != expected_source_queue) {
            throw std::logic_error(
                "rnic-cn route does not begin with the legacy source queue");
        }
        if (legacy_route->at(1) != expected_source_pipe) {
            throw std::logic_error(
                "rnic-cn route source queue is not followed by its host Pipe");
        }

        auto suffix = std::make_unique<Route>();
        for (std::size_t sink_index = 1;
             sink_index < legacy_route->size();
             ++sink_index) {
            suffix->push_back(legacy_route->at(sink_index));
        }
        suffix->push_back(&destination_endpoint);
        suffix->set_path_id(static_cast<int>(path_index), path_count);

        result.route_view.push_back(suffix.get());
        result.owned_routes.push_back(std::move(suffix));
    }

    return result;
}
