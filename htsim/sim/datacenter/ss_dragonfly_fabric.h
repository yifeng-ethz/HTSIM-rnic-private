// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SS_DRAGONFLY_FABRIC_H
#define SS_DRAGONFLY_FABRIC_H

#include <array>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <vector>

#include "dragonfly_progressive_congestion.h"
#include "dragonfly_progressive_routing.h"
#include "dragonfly_route_state_store.h"
#include "eventlist.h"
#include "network.h"
#include "ns_rosetta_switch.h"
#include "pipe.h"
#include "queue.h"

// A physical Slingshot-class dragonfly fabric: one ns-rosetta style
// input-buffered request/grant switch per dragonfly router, progressive
// per-hop adaptive route lookups after the modeled switch-pipeline delay,
// and causally delivered congestion signals.
//
// Status: hosted, calibration pending.  The construction and the routing
// mechanism follow the public record (see docs/ss-dragonfly-fabric); no
// accuracy or fidelity claim against the Merlin Slingshot fabric is made
// until the capture-driven calibration wave lands.
//
// Supported geometries this wave:
//   - the canonical maximum-size dragonfly, g = a*h + 1, local full mesh,
//     exactly one bidirectional global connection per group pair; and
//   - the degenerate single-switch instance g = 1, a = 1, h = 0, in which
//     every endpoint pair shares one router and routing is trivial.
// General g < a*h + 1 global arrangements are intentionally out of scope
// until the progressive routing core grows a proved general connector map.

class SsDragonflyTopology;

struct SsDragonflyLinkClassConfig {
    linkspeed_bps rate_bps{0};
    simtime_picosec propagation_ps{0};
};

struct SsDragonflyConfig {
    // Standard dragonfly terms: p hosts per router, a routers per group,
    // h global links per router, g groups.
    std::uint32_t hosts_per_router{0};
    std::uint16_t routers_per_group{0};
    std::uint16_t global_links_per_router{0};
    std::uint16_t group_count{0};

    SsDragonflyLinkClassConfig host_link{};
    SsDragonflyLinkClassConfig local_link{};
    SsDragonflyLinkClassConfig global_link{};
    simtime_picosec switch_pipeline_delay_ps{0};
    mem_b rosetta_shared_buffer_bytes{0};

    std::uint64_t maximum_wire_packet_bytes{4160};

    // Adaptive class biases follow the shift-plus-additive structure of
    // US 9137143 B2; the default values are calibrated abstractions, not
    // published hardware constants.
    std::array<htsim::DragonflyAdaptiveBias, 4> adaptive_biases{{
        {{0, 0}, {1, 1}},
        {{0, 0}, {1, 1}},
        {{0, 0}, {1, 1}},
        {{0, 0}, {1, 1}},
    }};

    // Named calibrated abstractions for the three congestion signals and
    // the neighbor load exchange.  The public record fixes the mechanism
    // (three remapped four-bit signals; high-frequency neighbor updates)
    // but not these quantities.
    std::uint64_t near_end_bytes_per_level{8320};
    std::uint64_t far_end_bytes_per_level{8320};
    std::uint64_t downstream_bytes_per_level{33280};
    std::uint64_t advertisement_period_ps{100000};
    std::uint64_t advertisement_delay_ps{300000};
    std::uint64_t maximum_downstream_age_ps{400000};

    std::uint64_t routing_seed{UINT64_C(0x5510d5a17f4b81c)};

    bool singleSwitch() const noexcept {
        return group_count == 1 && routers_per_group == 1 && global_links_per_router == 0;
    }
    std::uint32_t routerCount() const noexcept {
        return static_cast<std::uint32_t>(routers_per_group) * group_count;
    }
    std::uint32_t hostCount() const noexcept {
        return hosts_per_router * routerCount();
    }
    void validate() const;
};

// Parse the dragonfly topology file dialect of the existing -topo path.
// The first non-comment line must be "topology dragonfly".
SsDragonflyConfig loadSsDragonflyConfig(std::istream& file);
SsDragonflyConfig loadSsDragonflyConfigFile(const std::string& path);
// True when the file's first non-comment token pair names this dialect;
// callers use it to dispatch between Clos and dragonfly -topo files.
bool isSsDragonflyTopologyFile(const std::string& path);

// One pooled wire packet for the fabric sanity studies.  DATA-class low
// priority, whole wire extents, no transport protocol attached.
class SsDragonflyPacket final : public Packet {
public:
    static SsDragonflyPacket* newPacket(PacketFlow& flow,
                                        const Route& route,
                                        packetid_t packet_id,
                                        std::uint32_t source,
                                        std::uint32_t destination,
                                        mem_b wire_bytes,
                                        mem_b payload_bytes);

    ~SsDragonflyPacket() override = default;

    mem_b payloadBytes() const noexcept { return _payload_bytes; }

    // The owning fabric binds itself at registration and unbinds at
    // delivery, so a physical drop anywhere on the route releases the
    // packet's sidecar route state before the pooled pointer is recycled.
    void bindFabric(SsDragonflyTopology* fabric) noexcept { _fabric = fabric; }

    PktPriority priority() const override { return Packet::PRIO_LO; }
    void free() override;
    void strip_payload(std::uint16_t) override;
    void bounce() override;
    void unbounce(std::uint16_t) override;

private:
    friend class PacketDB<SsDragonflyPacket>;
    SsDragonflyPacket() = default;

    mem_b _payload_bytes{0};
    SsDragonflyTopology* _fabric{nullptr};

    static PacketDB<SsDragonflyPacket> _packet_db;
};

// Physical router: an ns-rosetta request/grant switch whose post-pipeline
// entry performs the progressive route lookup and installs the selected
// one-hop physical route before admission.  The lookup deliberately runs
// after the modeled switch-pipeline delay so a congestion advertisement
// arriving during that interval is visible to the decision.
class SsDragonflySwitch final : public NsRosetta {
public:
    SsDragonflySwitch(EventList& eventlist,
                      const std::string& name,
                      std::uint32_t router_id,
                      simtime_picosec pipeline_delay_ps,
                      mem_b shared_buffer_bytes,
                      SsDragonflyTopology& fabric);

    void receivePacket(Packet& pkt) override;

    std::uint32_t routerId() const noexcept { return _router_id; }

private:
    std::uint32_t _router_id;
    SsDragonflyTopology& _fabric;
};

struct SsDragonflyStatistics {
    std::uint64_t injected_packets{0};
    std::uint64_t delivered_packets{0};
    std::uint64_t delivered_payload_bytes{0};
    std::uint64_t dropped_packets{0};
    std::uint64_t minimal_deliveries{0};
    std::uint64_t non_minimal_deliveries{0};
    std::uint64_t undecided_deliveries{0};
    std::uint64_t advertisements_consumed{0};
    std::uint8_t maximum_router_hops_observed{0};
};

// Delivery observation: the terminal route state is handed over exactly
// once per delivered packet, before the pooled packet is recycled.
using SsDragonflyDeliveryObserver =
    std::function<void(const SsDragonflyPacket&,
                       const htsim::DragonflyRouteState&,
                       simtime_picosec)>;

// Per-hop observation of every progressive route decision, for the
// deterministic fixtures.  Not invoked by the single-switch instance,
// which performs no table lookup.
using SsDragonflyRouteDecisionObserver =
    std::function<void(std::uint32_t router,
                       const SsDragonflyPacket&,
                       const htsim::DragonflyRoutingDecision&)>;

class SsDragonflyTopology {
public:
    SsDragonflyTopology(EventList& eventlist, SsDragonflyConfig config);
    ~SsDragonflyTopology();

    SsDragonflyTopology(const SsDragonflyTopology&) = delete;
    SsDragonflyTopology& operator=(const SsDragonflyTopology&) = delete;

    const SsDragonflyConfig& config() const noexcept { return _config; }
    std::uint32_t hostCount() const noexcept { return _config.hostCount(); }
    std::uint32_t routerCount() const noexcept { return _config.routerCount(); }
    std::uint32_t routerOfHost(std::uint32_t host) const;

    // The injection route runs host serializer, host cable, and source
    // router physical ingress.  registerPacket must run on each packet
    // immediately before it is sent on this route.
    const Route& injectionRoute(std::uint32_t host) const;
    void registerPacket(SsDragonflyPacket& pkt,
                        htsim::DragonflyRoutingControl control,
                        std::uint64_t route_hash);
    // Deterministic per-packet route hash: splitmix64 over the fabric
    // routing seed, the flow identity, and the packet sequence.
    std::uint64_t packetRouteHash(std::uint32_t flow_id, std::uint64_t sequence) const;

    void setDeliveryObserver(SsDragonflyDeliveryObserver observer) {
        _delivery_observer = std::move(observer);
    }
    void setRouteDecisionObserver(SsDragonflyRouteDecisionObserver observer) {
        _route_decision_observer = std::move(observer);
    }

    const SsDragonflyStatistics& statistics() const noexcept { return _statistics; }
    const htsim::DragonflyProgressiveRouting* routing() const noexcept {
        return _routing.get();
    }
    SsDragonflySwitch& router(std::uint32_t router_id);
    void validateQuiescent() const;

    // Called by SsDragonflySwitch on its post-pipeline entry.
    void routeAtSwitch(SsDragonflySwitch& sw, Packet& pkt);
    // Called by SsDragonflyPacket::free when a bound packet is physically
    // dropped; releases the sidecar route state and counts the drop.
    void notePacketDropped(SsDragonflyPacket& pkt);
    // A failure raised inside a noexcept switch-observation boundary is
    // deferred here and rethrown at the next lookup or quiescence check.
    void deferFailure(std::exception_ptr failure) noexcept;

private:
    void rethrowDeferredFailure() const;
    static constexpr std::uint64_t kNoLinkTransport = UINT64_MAX;

    class HostSink final : public PacketSink {
    public:
        HostSink(SsDragonflyTopology& fabric, std::uint32_t host)
            : _fabric(fabric),
              _host(host),
              _name("ss-dragonfly-host-" + std::to_string(host)) {}
        void receivePacket(Packet& pkt) override { _fabric.deliverToHost(pkt, _host); }
        const string& nodename() override { return _name; }

    private:
        SsDragonflyTopology& _fabric;
        std::uint32_t _host;
        std::string _name;
    };

    // Drives the far-end resident estimate of every directed inter-router
    // link from the physical switch transitions on both sides of the cable.
    class LinkObserver final : public NsRosettaBacklogObserver {
    public:
        LinkObserver(SsDragonflyTopology& fabric, std::uint32_t router_id)
            : _fabric(fabric), _router_id(router_id) {}
        void observe(const NsRosettaBacklogObservation& observation) noexcept override;

    private:
        SsDragonflyTopology& _fabric;
        std::uint32_t _router_id;
    };

    // Emits the periodic neighbor load advertisements and consumes them at
    // their exact modeled arrival boundary.
    class ControlPlane final : public EventSource {
    public:
        ControlPlane(EventList& eventlist, SsDragonflyTopology& fabric)
            : EventSource(eventlist, "ss-dragonfly-control-plane"), _fabric(fabric) {}
        void doNextEvent() override;

    private:
        SsDragonflyTopology& _fabric;
    };

    struct PendingAdvertisement {
        simtime_picosec arrival_ps;
        std::uint64_t link_index;
        htsim::DragonflyDownstreamAdvertisement advertisement;
    };

    struct DirectedLink {
        std::uint32_t upstream_router{0};
        htsim::DragonflyPortId upstream_port{0};
        std::uint32_t downstream_router{0};
        std::uint32_t downstream_ingress{0};
        std::unique_ptr<htsim::DragonflyProgressiveCongestion> congestion;
        std::deque<std::pair<std::uint64_t, mem_b>> in_flight;
        std::uint64_t next_message_id{0};
        std::uint64_t next_advertisement_sequence{0};
    };

    void buildFabric();
    void refreshRouterCongestion(std::uint32_t router_id);
    void deliverToHost(Packet& pkt, std::uint32_t host);
    void observeTransition(std::uint32_t router_id,
                           const NsRosettaBacklogObservation& observation);
    void controlPlaneEvent();
    void emitAdvertisements();
    void scheduleControlPlane();
    std::uint32_t egressIndexForPort(htsim::DragonflyPortId port) const {
        return _config.hosts_per_router + port;
    }
    mem_b switchWaitingBytes(std::uint32_t router_id) const;

    EventList& _eventlist;
    SsDragonflyConfig _config;
    std::unique_ptr<htsim::DragonflyProgressiveRouting> _routing;
    htsim::DragonflyRouteStateStore _route_state_store;

    std::vector<std::unique_ptr<SsDragonflySwitch>> _switches;
    std::vector<std::shared_ptr<LinkObserver>> _link_observers;
    std::vector<std::unique_ptr<NsRosettaEgressSerializer>> _serializers;
    std::vector<std::unique_ptr<Pipe>> _pipes;
    std::vector<std::unique_ptr<Queue>> _host_queues;
    std::vector<std::unique_ptr<Pipe>> _host_pipes;
    std::vector<std::unique_ptr<HostSink>> _host_sinks;
    std::vector<std::unique_ptr<Route>> _injection_routes;
    // _hop_routes[router][egress index] ends at a host sink for host
    // egresses and at the neighbor physical ingress for fabric egresses.
    std::vector<std::vector<std::unique_ptr<Route>>> _hop_routes;
    std::vector<PacketSink*> _ingress_adapters;  // router * ingress-count grid

    std::vector<DirectedLink> _links;
    // (router, ingress index) -> link index feeding that ingress.
    std::vector<std::vector<std::uint64_t>> _ingress_link_index;
    // (router, core port) -> link index leaving that port.
    std::vector<std::vector<std::uint64_t>> _port_link_index;

    std::unique_ptr<ControlPlane> _control_plane;
    std::vector<PendingAdvertisement> _pending_advertisements;
    simtime_picosec _next_advertisement_tick_ps{0};

    SsDragonflyDeliveryObserver _delivery_observer;
    SsDragonflyRouteDecisionObserver _route_decision_observer;
    SsDragonflyStatistics _statistics;
    std::exception_ptr _deferred_failure;
};

#endif  // SS_DRAGONFLY_FABRIC_H
