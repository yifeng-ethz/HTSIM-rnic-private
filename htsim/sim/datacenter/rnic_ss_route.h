// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_SS_ROUTE_H
#define RNIC_SS_ROUTE_H

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "route.h"

class FatTreeTopology;
class PacketSink;

// Owns full host-queue-to-endpoint routes for rnic-ss.  Unlike rnic-cn, the
// first route element is retained: that real host queue is the one physical
// source serializer shared by DATA and high-priority control packets.
class RnicSsRouteProvider {
public:
    using RouteView = std::vector<const Route*>;

    explicit RnicSsRouteProvider(FatTreeTopology& topology);
    ~RnicSsRouteProvider();

    RnicSsRouteProvider(const RnicSsRouteProvider&) = delete;
    RnicSsRouteProvider& operator=(const RnicSsRouteProvider&) = delete;

    const RouteView& routes(
        std::uint32_t source,
        std::uint32_t destination,
        PacketSink& destination_endpoint);
    std::uint32_t nodeCount() const noexcept { return _node_count; }

private:
    using Pair = std::pair<std::uint32_t, std::uint32_t>;
    struct RouteSet {
        PacketSink* endpoint{nullptr};
        std::vector<std::unique_ptr<Route>> owned;
        RouteView view;
    };

    RouteSet build(
        std::uint32_t source,
        std::uint32_t destination,
        PacketSink& endpoint);
    void validatePair(std::uint32_t source,
                      std::uint32_t destination) const;

    FatTreeTopology& _topology;
    std::uint32_t _node_count;
    std::vector<PacketSink*> _destination_endpoints;
    std::map<Pair, RouteSet> _route_sets;
};

#endif  // RNIC_SS_ROUTE_H
