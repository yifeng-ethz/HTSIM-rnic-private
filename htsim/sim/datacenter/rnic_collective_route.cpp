// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_route.h"

#include <limits>
#include <stdexcept>

#include "fat_tree_topology.h"

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

}  // namespace

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
