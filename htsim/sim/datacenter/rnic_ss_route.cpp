// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ss_route.h"

#include <limits>
#include <stdexcept>

#include "fat_tree_topology.h"

namespace {

class LegacyRoutes final {
public:
    explicit LegacyRoutes(std::vector<const Route*>* routes)
        : _routes(routes) {}
    ~LegacyRoutes() {
        if (_routes == nullptr) {
            return;
        }
        for (const Route* route : *_routes) {
            delete route;
        }
        delete _routes;
    }
    const std::vector<const Route*>* get() const noexcept { return _routes; }

private:
    std::vector<const Route*>* _routes;
};

}  // namespace

RnicSsRouteProvider::RnicSsRouteProvider(FatTreeTopology& topology)
    : _topology(topology),
      _node_count(topology.cfg().no_of_servers()),
      _destination_endpoints(_node_count, nullptr) {
    if (topology.cfg().get_tiers() != 2
        || topology.cfg().switch_model() != FatTreeSwitchModel::NsRosetta) {
        throw std::invalid_argument(
            "rnic-ss route provider requires a two-tier ns-rosetta Clos");
    }
    if (_node_count == 0) {
        throw std::invalid_argument("rnic-ss route provider requires nodes");
    }
}

RnicSsRouteProvider::~RnicSsRouteProvider() = default;

const RnicSsRouteProvider::RouteView& RnicSsRouteProvider::routes(
        std::uint32_t source,
        std::uint32_t destination,
        PacketSink& destination_endpoint) {
    validatePair(source, destination);
    PacketSink*& bound = _destination_endpoints[destination];
    if (bound != nullptr && bound != &destination_endpoint) {
        throw std::invalid_argument(
            "rnic-ss destination endpoint was rebound");
    }
    const Pair pair{source, destination};
    auto existing = _route_sets.find(pair);
    if (existing != _route_sets.end()) {
        if (existing->second.endpoint != &destination_endpoint) {
            throw std::invalid_argument("rnic-ss route endpoint was rebound");
        }
        return existing->second.view;
    }
    auto [it, inserted] = _route_sets.emplace(
        pair, build(source, destination, destination_endpoint));
    if (!inserted) {
        throw std::logic_error("rnic-ss route insertion failed");
    }
    bound = &destination_endpoint;
    return it->second.view;
}

void RnicSsRouteProvider::validatePair(
        std::uint32_t source, std::uint32_t destination) const {
    if (source >= _node_count || destination >= _node_count) {
        throw std::out_of_range("rnic-ss route endpoint is outside the Clos");
    }
    if (source == destination) {
        throw std::invalid_argument("rnic-ss requires distinct endpoints");
    }
}

RnicSsRouteProvider::RouteSet RnicSsRouteProvider::build(
        std::uint32_t source,
        std::uint32_t destination,
        PacketSink& endpoint) {
    LegacyRoutes legacy(_topology.get_bidir_paths(source, destination, false));
    const std::vector<const Route*>* routes = legacy.get();
    if (routes == nullptr || routes->empty()) {
        throw std::logic_error("rnic-ss Clos returned no physical route");
    }
    if (routes->size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        throw std::overflow_error("rnic-ss path count exceeds Route domain");
    }

    RouteSet result;
    result.endpoint = &endpoint;
    result.owned.reserve(routes->size());
    result.view.reserve(routes->size());
    const int path_count = static_cast<int>(routes->size());
    for (std::size_t path = 0; path < routes->size(); ++path) {
        const Route* legacy_route = routes->at(path);
        if (legacy_route == nullptr || legacy_route->size() == 0) {
            throw std::logic_error("rnic-ss Clos returned a malformed route");
        }
        auto owned = std::make_unique<Route>();
        for (PacketSink* sink : *legacy_route) {
            owned->push_back(sink);
        }
        owned->push_back(&endpoint);
        owned->set_path_id(static_cast<int>(path), path_count);
        result.view.push_back(owned.get());
        result.owned.push_back(std::move(owned));
    }
    return result;
}
