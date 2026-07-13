// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_ROUTE_H
#define RNIC_COLLECTIVE_ROUTE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "route.h"

class FatTreeTopology;
class FatTreeTopologyCfg;
class PacketSink;
class RnicPacketExtent;

// Physical no-load age from the source RNIC serializer boundary to the
// destination endpoint.  The calibration mirrors the exact two-tier route:
// propagation and switch-pipeline latency plus every remaining Tomahawk 3
// egress serialization at the packet's actual wire extent.
std::uint64_t rnicCollectiveNoQueueTransitPs(
    const FatTreeTopologyCfg& topology_config,
    std::uint32_t source,
    std::uint32_t destination,
    const RnicPacketExtent& extent);

// Owns the explicit source-serialized routes used by the collective-network
// profile.  FatTreeTopology's legacy host-to-host paths begin with a source
// queue.  RnicTxPort already models that physical serialization, so these
// routes omit exactly that queue and begin with the existing host-to-ToR Pipe.
//
// A route is cached per ordered endpoint pair.  All pairs targeting one node
// are bound to the same supplied, stable, node-scoped destination sink. Cached
// Route objects and the returned enumeration remain valid until this provider
// is destroyed. The referenced topology and every bound endpoint must outlive
// the provider and all packets that use its routes.
class RnicCollectiveRouteProvider {
public:
    using NodeId = std::uint32_t;
    using RouteView = std::vector<const Route*>;

    explicit RnicCollectiveRouteProvider(FatTreeTopology& topology);
    ~RnicCollectiveRouteProvider();

    RnicCollectiveRouteProvider(const RnicCollectiveRouteProvider&) = delete;
    RnicCollectiveRouteProvider& operator=(
        const RnicCollectiveRouteProvider&) = delete;
    RnicCollectiveRouteProvider(RnicCollectiveRouteProvider&&) = delete;
    RnicCollectiveRouteProvider& operator=(
        RnicCollectiveRouteProvider&&) = delete;

    // Returns paths in FatTreeTopology's deterministic enumeration order.
    // The first lookup builds and owns the suffix routes; later lookups return
    // the identical view. A destination node cannot be rebound to a different
    // endpoint, even through a route from another source.
    const RouteView& routes(
        NodeId source,
        NodeId destination,
        PacketSink& destination_endpoint);

    std::uint32_t nodeCount() const noexcept { return _node_count; }
    std::size_t cachedPairCount() const noexcept { return _route_sets.size(); }

private:
    using PairKey = std::pair<NodeId, NodeId>;

    struct RouteSet {
        PacketSink* destination_endpoint = nullptr;
        std::vector<std::unique_ptr<Route>> owned_routes;
        RouteView route_view;
    };

    void validatePair(NodeId source, NodeId destination) const;
    RouteSet buildRouteSet(
        NodeId source,
        NodeId destination,
        PacketSink& destination_endpoint);

    FatTreeTopology& _topology;
    std::uint32_t _node_count;
    std::vector<PacketSink*> _destination_endpoints;
    std::map<PairKey, RouteSet> _route_sets;
};

#endif  // RNIC_COLLECTIVE_ROUTE_H
