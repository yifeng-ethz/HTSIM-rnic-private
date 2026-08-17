// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ss_dragonfly_fabric.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

// splitmix64: the fixed-increment mixer used across the RNIC seeds.  This
// derivation is fabric-local so per-packet route hashes are reproducible
// from (routing seed, flow, sequence) alone.
std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

std::uint64_t parseTopoUnsigned(const std::string& key, const std::string& text) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument("dragonfly topology key '" + key +
                                    "' requires an unsigned value");
    }
    std::size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
        throw std::invalid_argument("dragonfly topology key '" + key +
                                    "' has a malformed value '" + text + "'");
    }
    return static_cast<std::uint64_t>(value);
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

void SsDragonflyConfig::validate() const {
    const bool canonical = routers_per_group >= 1 && global_links_per_router >= 1 &&
                           group_count == static_cast<std::uint32_t>(routers_per_group) *
                                                  global_links_per_router +
                                              1;
    if (!canonical && !singleSwitch()) {
        throw std::invalid_argument(
            "ss-dragonfly supports the canonical g = a*h + 1 dragonfly or the "
            "single-switch instance g = 1, a = 1, h = 0");
    }
    if (hosts_per_router == 0) {
        throw std::invalid_argument("ss-dragonfly requires at least one host per router");
    }
    if (singleSwitch() && hosts_per_router < 2) {
        throw std::invalid_argument(
            "the single-switch ss-dragonfly instance needs two hosts to form a pair");
    }
    if (host_link.rate_bps == 0 || (!singleSwitch() && local_link.rate_bps == 0) ||
        (!singleSwitch() && global_link.rate_bps == 0)) {
        throw std::invalid_argument("ss-dragonfly link classes require positive rates");
    }
    if (rosetta_shared_buffer_bytes <= 0) {
        throw std::invalid_argument("ss-dragonfly requires a positive rosetta shared buffer");
    }
    if (maximum_wire_packet_bytes == 0 ||
        maximum_wire_packet_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("ss-dragonfly wire extents must fit HTSIM's uint16_t");
    }
    if (static_cast<std::uint64_t>(rosetta_shared_buffer_bytes) < maximum_wire_packet_bytes) {
        throw std::invalid_argument(
            "ss-dragonfly shared buffer cannot hold a single maximum packet");
    }
    if (near_end_bytes_per_level == 0 || far_end_bytes_per_level == 0 ||
        downstream_bytes_per_level == 0) {
        throw std::invalid_argument("ss-dragonfly congestion quantizers require positive steps");
    }
    if (!singleSwitch() &&
        (advertisement_period_ps == 0 || advertisement_delay_ps == 0 ||
         maximum_downstream_age_ps == 0)) {
        throw std::invalid_argument(
            "ss-dragonfly advertisement period, delay, and age must be positive");
    }
    for (const htsim::DragonflyAdaptiveBias& bias : adaptive_biases) {
        bias.validate();
    }
}

SsDragonflyConfig loadSsDragonflyConfig(std::istream& file) {
    SsDragonflyConfig config;
    bool marker_seen = false;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream tokens(line);
        std::string key;
        if (!(tokens >> key) || key.front() == '#') {
            continue;
        }
        key = lowered(key);
        std::string value;
        if (!(tokens >> value)) {
            throw std::invalid_argument("dragonfly topology key '" + key + "' has no value");
        }
        std::string trailing;
        if (tokens >> trailing && trailing.front() != '#') {
            throw std::invalid_argument("dragonfly topology key '" + key +
                                        "' has trailing tokens");
        }
        if (!marker_seen) {
            if (key != "topology" || lowered(value) != "dragonfly") {
                throw std::invalid_argument(
                    "dragonfly topology files must start with 'topology dragonfly'");
            }
            marker_seen = true;
            continue;
        }
        if (key == "hosts_per_router") {
            config.hosts_per_router = static_cast<std::uint32_t>(parseTopoUnsigned(key, value));
        } else if (key == "routers_per_group") {
            config.routers_per_group = static_cast<std::uint16_t>(parseTopoUnsigned(key, value));
        } else if (key == "global_links_per_router") {
            config.global_links_per_router =
                static_cast<std::uint16_t>(parseTopoUnsigned(key, value));
        } else if (key == "groups") {
            config.group_count = static_cast<std::uint16_t>(parseTopoUnsigned(key, value));
        } else if (key == "host_link_gbps") {
            config.host_link.rate_bps = parseTopoUnsigned(key, value) * UINT64_C(1000000000);
        } else if (key == "local_link_gbps") {
            config.local_link.rate_bps = parseTopoUnsigned(key, value) * UINT64_C(1000000000);
        } else if (key == "global_link_gbps") {
            config.global_link.rate_bps = parseTopoUnsigned(key, value) * UINT64_C(1000000000);
        } else if (key == "host_link_latency_ps") {
            config.host_link.propagation_ps = parseTopoUnsigned(key, value);
        } else if (key == "local_link_latency_ps") {
            config.local_link.propagation_ps = parseTopoUnsigned(key, value);
        } else if (key == "global_link_latency_ps") {
            config.global_link.propagation_ps = parseTopoUnsigned(key, value);
        } else if (key == "switch_pipeline_ps") {
            config.switch_pipeline_delay_ps = parseTopoUnsigned(key, value);
        } else if (key == "rosetta_buffer_bytes") {
            config.rosetta_shared_buffer_bytes =
                static_cast<mem_b>(parseTopoUnsigned(key, value));
        } else if (key == "max_wire_packet_bytes") {
            config.maximum_wire_packet_bytes = parseTopoUnsigned(key, value);
        } else if (key == "near_end_bytes_per_level") {
            config.near_end_bytes_per_level = parseTopoUnsigned(key, value);
        } else if (key == "far_end_bytes_per_level") {
            config.far_end_bytes_per_level = parseTopoUnsigned(key, value);
        } else if (key == "downstream_bytes_per_level") {
            config.downstream_bytes_per_level = parseTopoUnsigned(key, value);
        } else if (key == "advertisement_period_ps") {
            config.advertisement_period_ps = parseTopoUnsigned(key, value);
        } else if (key == "advertisement_delay_ps") {
            config.advertisement_delay_ps = parseTopoUnsigned(key, value);
        } else if (key == "maximum_downstream_age_ps") {
            config.maximum_downstream_age_ps = parseTopoUnsigned(key, value);
        } else if (key == "routing_seed") {
            config.routing_seed = parseTopoUnsigned(key, value);
        } else {
            throw std::invalid_argument("unknown dragonfly topology key '" + key + "'");
        }
    }
    if (!marker_seen) {
        throw std::invalid_argument(
            "dragonfly topology files must start with 'topology dragonfly'");
    }
    config.validate();
    return config;
}

SsDragonflyConfig loadSsDragonflyConfigFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::invalid_argument("cannot open dragonfly topology file '" + path + "'");
    }
    return loadSsDragonflyConfig(file);
}

bool isSsDragonflyTopologyFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream tokens(line);
        std::string key;
        if (!(tokens >> key) || key.front() == '#') {
            continue;
        }
        std::string value;
        return lowered(key) == "topology" && (tokens >> value) &&
               lowered(value) == "dragonfly";
    }
    return false;
}

PacketDB<SsDragonflyPacket> SsDragonflyPacket::_packet_db;

SsDragonflyPacket* SsDragonflyPacket::newPacket(PacketFlow& flow,
                                                const Route& route,
                                                packetid_t packet_id,
                                                std::uint32_t source,
                                                std::uint32_t destination,
                                                mem_b wire_bytes,
                                                mem_b payload_bytes) {
    if (route.size() == 0) {
        throw std::invalid_argument("ss-dragonfly packet requires a physical route");
    }
    if (wire_bytes <= 0 || wire_bytes > std::numeric_limits<std::uint16_t>::max() ||
        payload_bytes < 0 || payload_bytes > wire_bytes) {
        throw std::invalid_argument("ss-dragonfly packet extents are invalid");
    }
    SsDragonflyPacket* packet = _packet_db.allocPacket();
    packet->set_route(flow, route, static_cast<int>(wire_bytes), packet_id);
    packet->_type = IP;
    packet->_is_header = false;
    packet->_bounced = false;
    packet->_src = source;
    packet->_dst = destination;
    packet->_pathid = UINT32_MAX;
    packet->_hop_count = 0;
    packet->_direction = NONE;
    packet->_next_routed_hop = nullptr;
    packet->_ingressqueue = nullptr;
    packet->_path_len = static_cast<std::uint32_t>(route.size());
    packet->_payload_bytes = payload_bytes;
    packet->_fabric = nullptr;
    return packet;
}

void SsDragonflyPacket::free() {
    if (_fabric != nullptr) {
        SsDragonflyTopology* fabric = _fabric;
        _fabric = nullptr;
        fabric->notePacketDropped(*this);
    }
    _packet_db.freePacket(this);
}

void SsDragonflyPacket::strip_payload(std::uint16_t) {
    throw std::logic_error("ss-dragonfly packets cannot be trimmed");
}

void SsDragonflyPacket::bounce() {
    throw std::logic_error("ss-dragonfly packets do not use PFC bounce");
}

void SsDragonflyPacket::unbounce(std::uint16_t) {
    throw std::logic_error("ss-dragonfly packets do not use PFC bounce");
}

SsDragonflySwitch::SsDragonflySwitch(EventList& eventlist,
                                     const std::string& name,
                                     std::uint32_t router_id,
                                     simtime_picosec pipeline_delay_ps,
                                     mem_b shared_buffer_bytes,
                                     SsDragonflyTopology& fabric)
    : NsRosetta(eventlist,
                name,
                FatTreeSwitch::TOR,
                router_id,
                pipeline_delay_ps,
                nullptr,
                shared_buffer_bytes),
      _router_id(router_id),
      _fabric(fabric) {}

void SsDragonflySwitch::receivePacket(Packet& pkt) {
    // The progressive lookup runs here, after the modeled switch-pipeline
    // delay, so state that physically arrived during the pipeline interval
    // is visible; NsRosetta then applies admission and request/grant on the
    // selected local egress.
    _fabric.routeAtSwitch(*this, pkt);
    NsRosetta::receivePacket(pkt);
}

void SsDragonflyTopology::LinkObserver::observe(
    const NsRosettaBacklogObservation& observation) noexcept {
    try {
        _fabric.observeTransition(_router_id, observation);
    } catch (...) {
        _fabric.deferFailure(std::current_exception());
    }
}

void SsDragonflyTopology::ControlPlane::doNextEvent() {
    _fabric.controlPlaneEvent();
}

SsDragonflyTopology::SsDragonflyTopology(EventList& eventlist, SsDragonflyConfig config)
    : _eventlist(eventlist), _config(std::move(config)) {
    _config.validate();
    if (!_config.singleSwitch()) {
        htsim::DragonflyProgressiveConfig routing_config;
        routing_config.routers_per_group = _config.routers_per_group;
        routing_config.global_links_per_router = _config.global_links_per_router;
        routing_config.adaptive_biases = _config.adaptive_biases;
        _routing = std::make_unique<htsim::DragonflyProgressiveRouting>(routing_config);
        if (_routing->topology().groupCount() != _config.group_count) {
            throw std::logic_error("ss-dragonfly group count deviates from the canonical core");
        }
    }
    buildFabric();
}

SsDragonflyTopology::~SsDragonflyTopology() {
    if (_control_plane != nullptr) {
        EventList::cancelPendingSource(*_control_plane);
    }
}

void SsDragonflyTopology::buildFabric() {
    const std::uint32_t routers = _config.routerCount();
    const std::uint32_t hosts = _config.hostCount();
    const std::uint32_t p = _config.hosts_per_router;
    const std::uint32_t fabric_ports_per_router =
        _config.singleSwitch()
            ? 0
            : static_cast<std::uint32_t>(_config.routers_per_group - 1) +
                  _config.global_links_per_router;

    _switches.reserve(routers);
    _link_observers.reserve(routers);
    _hop_routes.resize(routers);
    _ingress_adapters.assign(
        static_cast<std::size_t>(routers) * (p + fabric_ports_per_router), nullptr);
    _ingress_link_index.assign(routers,
                               std::vector<std::uint64_t>(p + fabric_ports_per_router,
                                                          kNoLinkTransport));
    _port_link_index.assign(
        routers, std::vector<std::uint64_t>(fabric_ports_per_router, kNoLinkTransport));

    for (std::uint32_t router = 0; router < routers; ++router) {
        auto sw = std::make_unique<SsDragonflySwitch>(
            _eventlist, "ss-dragonfly-router-" + std::to_string(router), router,
            _config.switch_pipeline_delay_ps, _config.rosetta_shared_buffer_bytes, *this);
        auto observer = std::make_shared<LinkObserver>(*this, router);
        sw->set_backlog_observer(observer);
        _link_observers.push_back(std::move(observer));
        _switches.push_back(std::move(sw));
    }

    // Physical ingress adapters: host cables first, then one adapter per
    // reciprocal fabric port, so ingress p + q on a router receives from
    // the neighbor reached through its own port q.
    for (std::uint32_t router = 0; router < routers; ++router) {
        for (std::uint32_t host = 0; host < p; ++host) {
            _ingress_adapters[static_cast<std::size_t>(router) * (p + fabric_ports_per_router) +
                              host] =
                _switches[router]->create_physical_ingress(
                    "ss-dragonfly-ingress-r" + std::to_string(router) + "-h" +
                    std::to_string(host));
        }
        for (std::uint32_t port = 0; port < fabric_ports_per_router; ++port) {
            _ingress_adapters[static_cast<std::size_t>(router) * (p + fabric_ports_per_router) +
                              p + port] =
                _switches[router]->create_physical_ingress(
                    "ss-dragonfly-ingress-r" + std::to_string(router) + "-p" +
                    std::to_string(port));
        }
    }

    // Egress serializers, cables, and cached one-hop routes.
    for (std::uint32_t router = 0; router < routers; ++router) {
        _hop_routes[router].resize(p + fabric_ports_per_router);
        for (std::uint32_t host = 0; host < p; ++host) {
            const std::uint32_t host_id = router * p + host;
            auto serializer = std::make_unique<NsRosettaEgressSerializer>(
                _config.host_link.rate_bps, _eventlist, nullptr);
            _switches[router]->addPort(serializer.get());
            auto pipe = std::make_unique<Pipe>(_config.host_link.propagation_ps, _eventlist);
            auto sink = std::make_unique<HostSink>(*this, host_id);
            auto route = std::make_unique<Route>();
            route->push_back(serializer.get());
            route->push_back(pipe.get());
            route->push_back(sink.get());
            _hop_routes[router][host] = std::move(route);
            _serializers.push_back(std::move(serializer));
            _pipes.push_back(std::move(pipe));
            _host_sinks.push_back(std::move(sink));
        }
        for (std::uint32_t port = 0; port < fabric_ports_per_router; ++port) {
            const htsim::DragonflyPort& core_port =
                _routing->topology().port(router, static_cast<htsim::DragonflyPortId>(port));
            const SsDragonflyLinkClassConfig& link_class =
                core_port.kind == htsim::DragonflyLinkKind::Global ? _config.global_link
                                                                   : _config.local_link;
            auto serializer = std::make_unique<NsRosettaEgressSerializer>(
                link_class.rate_bps, _eventlist, nullptr);
            _switches[router]->addPort(serializer.get());
            auto pipe = std::make_unique<Pipe>(link_class.propagation_ps, _eventlist);
            const htsim::DragonflyPortId reciprocal = _routing->topology().reciprocalPort(
                router, static_cast<htsim::DragonflyPortId>(port));
            PacketSink* neighbor_ingress =
                _ingress_adapters[static_cast<std::size_t>(core_port.next_router) *
                                      (p + fabric_ports_per_router) +
                                  p + reciprocal];
            auto route = std::make_unique<Route>();
            route->push_back(serializer.get());
            route->push_back(pipe.get());
            route->push_back(neighbor_ingress);
            _hop_routes[router][p + port] = std::move(route);
            _serializers.push_back(std::move(serializer));
            _pipes.push_back(std::move(pipe));

            DirectedLink link;
            link.upstream_router = router;
            link.upstream_port = static_cast<htsim::DragonflyPortId>(port);
            link.downstream_router = core_port.next_router;
            link.downstream_ingress = p + reciprocal;
            htsim::DragonflyProgressiveCongestionConfig congestion_config{
                htsim::DragonflyFourBitRemap::uniform(_config.near_end_bytes_per_level),
                htsim::DragonflyFourBitRemap::uniform(_config.far_end_bytes_per_level),
                htsim::DragonflyFourBitRemap::uniform(_config.downstream_bytes_per_level),
                _config.advertisement_delay_ps,
                _config.maximum_downstream_age_ps};
            link.congestion = std::make_unique<htsim::DragonflyProgressiveCongestion>(
                congestion_config);
            const std::uint64_t link_index = _links.size();
            _links.push_back(std::move(link));
            _port_link_index[router][port] = link_index;
            _ingress_link_index[core_port.next_router][p + reciprocal] = link_index;
        }
    }

    // Host injection paths.
    _host_queues.reserve(hosts);
    _host_pipes.reserve(hosts);
    _injection_routes.reserve(hosts);
    for (std::uint32_t host = 0; host < hosts; ++host) {
        const std::uint32_t router = host / p;
        auto queue = std::make_unique<Queue>(
            _config.host_link.rate_bps,
            static_cast<mem_b>(64 * _config.maximum_wire_packet_bytes), _eventlist, nullptr);
        auto pipe = std::make_unique<Pipe>(_config.host_link.propagation_ps, _eventlist);
        auto route = std::make_unique<Route>();
        route->push_back(queue.get());
        route->push_back(pipe.get());
        route->push_back(
            _ingress_adapters[static_cast<std::size_t>(router) * (p + fabric_ports_per_router) +
                              host % p]);
        _injection_routes.push_back(std::move(route));
        _host_queues.push_back(std::move(queue));
        _host_pipes.push_back(std::move(pipe));
    }

    if (!_config.singleSwitch()) {
        _control_plane = std::make_unique<ControlPlane>(_eventlist, *this);
        _next_advertisement_tick_ps = _config.advertisement_period_ps;
        _eventlist.sourceIsPending(*_control_plane, _next_advertisement_tick_ps);
    }
}

std::uint32_t SsDragonflyTopology::routerOfHost(std::uint32_t host) const {
    if (host >= hostCount()) {
        throw std::invalid_argument("ss-dragonfly host index is out of range");
    }
    return host / _config.hosts_per_router;
}

const Route& SsDragonflyTopology::injectionRoute(std::uint32_t host) const {
    if (host >= hostCount()) {
        throw std::invalid_argument("ss-dragonfly host index is out of range");
    }
    return *_injection_routes[host];
}

std::uint64_t SsDragonflyTopology::packetRouteHash(std::uint32_t flow_id,
                                                   std::uint64_t sequence) const {
    return splitmix64(splitmix64(_config.routing_seed ^ flow_id) ^ sequence);
}

void SsDragonflyTopology::registerPacket(SsDragonflyPacket& pkt,
                                         htsim::DragonflyRoutingControl control,
                                         std::uint64_t route_hash) {
    const std::uint32_t source = pkt.src();
    const std::uint32_t destination = pkt.dst();
    if (source >= hostCount() || destination >= hostCount() || source == destination) {
        throw std::invalid_argument("ss-dragonfly packet endpoints are invalid");
    }
    htsim::DragonflyRouteState state;
    if (_routing != nullptr) {
        state = _routing->initialize(routerOfHost(source), routerOfHost(destination), control,
                                     route_hash);
    } else {
        state.source_router = 0;
        state.target_router = 0;
        state.expected_current_router = 0;
        state.control = control;
        state.route_hash = route_hash;
    }
    _route_state_store.insert(pkt, state);
    pkt.bindFabric(this);
    _statistics.injected_packets++;
}

SsDragonflySwitch& SsDragonflyTopology::router(std::uint32_t router_id) {
    if (router_id >= _switches.size()) {
        throw std::invalid_argument("ss-dragonfly router index is out of range");
    }
    return *_switches[router_id];
}

void SsDragonflyTopology::refreshRouterCongestion(std::uint32_t router_id) {
    if (_routing == nullptr) {
        return;
    }
    const simtime_picosec now = EventList::now();
    for (std::size_t port = 0; port < _port_link_index[router_id].size(); ++port) {
        DirectedLink& link = _links[_port_link_index[router_id][port]];
        link.congestion->setNearEndWaitingBytes(static_cast<std::uint64_t>(
            _switches[router_id]->egress_backlog_bytes(egressIndexForPort(
                static_cast<htsim::DragonflyPortId>(port)))));
        _routing->setPortCongestion(router_id,
                                    static_cast<htsim::DragonflyPortId>(port),
                                    link.congestion->components(now));
    }
}

void SsDragonflyTopology::routeAtSwitch(SsDragonflySwitch& sw, Packet& pkt) {
    auto* packet = dynamic_cast<SsDragonflyPacket*>(&pkt);
    if (packet == nullptr) {
        throw std::invalid_argument("ss-dragonfly switches route only fabric packets");
    }
    rethrowDeferredFailure();
    const std::uint32_t router_id = sw.routerId();
    htsim::DragonflyRouteState& state = _route_state_store.mutate(*packet);
    if (_routing == nullptr) {
        if (routerOfHost(packet->dst()) != router_id) {
            throw std::logic_error("single-switch ss-dragonfly saw a foreign destination");
        }
        // No candidate was ever selected, so like a same-router delivery in
        // the canonical fabric the route class stays Undecided.
        state.phase = htsim::DragonflyRoutePhase::Delivered;
        packet->set_route(
            *_hop_routes[router_id][packet->dst() % _config.hosts_per_router]);
        return;
    }

    refreshRouterCongestion(router_id);
    const htsim::DragonflyRoutingDecision decision = _routing->nextHop(router_id, state);
    if (_route_decision_observer) {
        _route_decision_observer(router_id, *packet, decision);
    }
    switch (decision.outcome) {
        case htsim::DragonflyDecisionOutcome::Delivered: {
            if (routerOfHost(packet->dst()) != router_id) {
                throw std::logic_error(
                    "ss-dragonfly delivered a packet at the wrong router");
            }
            packet->set_route(
                *_hop_routes[router_id][packet->dst() % _config.hosts_per_router]);
            return;
        }
        case htsim::DragonflyDecisionOutcome::Forward: {
            if (!decision.output_port.has_value()) {
                throw std::logic_error("ss-dragonfly forward decision lacks an output port");
            }
            packet->set_route(
                *_hop_routes[router_id][egressIndexForPort(*decision.output_port)]);
            return;
        }
        case htsim::DragonflyDecisionOutcome::Dropped:
            break;
    }
    // No link failures are modeled this wave, so a routing drop can only be
    // a fabric integrity defect; fail loudly instead of freeing quietly.
    throw std::logic_error("ss-dragonfly routing dropped a packet in a failure-free fabric");
}

void SsDragonflyTopology::deliverToHost(Packet& pkt, std::uint32_t host) {
    auto* packet = dynamic_cast<SsDragonflyPacket*>(&pkt);
    if (packet == nullptr) {
        throw std::invalid_argument("ss-dragonfly hosts receive only fabric packets");
    }
    if (packet->dst() != host) {
        throw std::logic_error("ss-dragonfly packet reached the wrong host");
    }
    packet->bindFabric(nullptr);
    const htsim::DragonflyRouteState state = _route_state_store.eraseForDelivery(*packet);
    _statistics.delivered_packets++;
    _statistics.delivered_payload_bytes += static_cast<std::uint64_t>(packet->payloadBytes());
    switch (state.route_class) {
        case htsim::DragonflyRouteClass::Minimal:
            _statistics.minimal_deliveries++;
            break;
        case htsim::DragonflyRouteClass::NonMinimal:
            _statistics.non_minimal_deliveries++;
            break;
        case htsim::DragonflyRouteClass::Undecided:
            _statistics.undecided_deliveries++;
            break;
    }
    _statistics.maximum_router_hops_observed =
        std::max(_statistics.maximum_router_hops_observed, state.router_hops);
    if (_delivery_observer) {
        _delivery_observer(*packet, state, EventList::now());
    }
    packet->free();
}

void SsDragonflyTopology::notePacketDropped(SsDragonflyPacket& pkt) {
    htsim::DragonflyRouteState& state = _route_state_store.mutate(pkt);
    state.phase = htsim::DragonflyRoutePhase::Dropped;
    _route_state_store.eraseForDrop(pkt);
    _statistics.dropped_packets++;
}

void SsDragonflyTopology::observeTransition(
    std::uint32_t router_id, const NsRosettaBacklogObservation& observation) {
    if (_routing == nullptr) {
        return;
    }
    switch (observation.transition) {
        case NsRosettaQueueTransition::SerializationCompleted: {
            if (observation.egress_id < _config.hosts_per_router) {
                return;
            }
            DirectedLink& link = _links[_port_link_index[router_id]
                                              [observation.egress_id -
                                               _config.hosts_per_router]];
            const std::uint64_t message_id = link.next_message_id++;
            link.congestion->messageSent(
                message_id, static_cast<std::uint64_t>(observation.packet_bytes));
            link.in_flight.emplace_back(message_id, observation.packet_bytes);
            return;
        }
        case NsRosettaQueueTransition::Enqueued:
        case NsRosettaQueueTransition::Dropped: {
            const std::uint64_t link_index =
                observation.ingress_id < _ingress_link_index[router_id].size()
                    ? _ingress_link_index[router_id][observation.ingress_id]
                    : kNoLinkTransport;
            if (link_index == kNoLinkTransport) {
                return;
            }
            DirectedLink& link = _links[link_index];
            if (link.in_flight.empty() ||
                link.in_flight.front().second != observation.packet_bytes) {
                throw std::logic_error(
                    "ss-dragonfly link transport lost track of an in-flight packet");
            }
            const std::uint64_t message_id = link.in_flight.front().first;
            link.in_flight.pop_front();
            link.congestion->messageArrived(message_id);
            if (observation.transition == NsRosettaQueueTransition::Dropped) {
                // The packet left the fabric at admission; its bytes are no
                // longer outstanding toward the neighbor.
                link.congestion->creditReturned(
                    static_cast<std::uint64_t>(observation.packet_bytes));
            }
            return;
        }
        case NsRosettaQueueTransition::Granted: {
            const std::uint64_t link_index =
                observation.ingress_id < _ingress_link_index[router_id].size()
                    ? _ingress_link_index[router_id][observation.ingress_id]
                    : kNoLinkTransport;
            if (link_index == kNoLinkTransport) {
                return;
            }
            _links[link_index].congestion->creditReturned(
                static_cast<std::uint64_t>(observation.packet_bytes));
            return;
        }
    }
}

mem_b SsDragonflyTopology::switchWaitingBytes(std::uint32_t router_id) const {
    return _switches[router_id]->shared_buffer_occupancy();
}

void SsDragonflyTopology::controlPlaneEvent() {
    const simtime_picosec now = EventList::now();
    // Consume every advertisement whose modeled arrival boundary is now.
    std::vector<PendingAdvertisement> retained;
    retained.reserve(_pending_advertisements.size());
    for (PendingAdvertisement& pending : _pending_advertisements) {
        if (pending.arrival_ps == now) {
            DirectedLink& link = _links[pending.link_index];
            const htsim::DragonflyAdvertisementDisposition disposition =
                link.congestion->consumeDownstreamAdvertisement(pending.advertisement, now);
            if (disposition != htsim::DragonflyAdvertisementDisposition::Accepted) {
                throw std::logic_error(
                    "ss-dragonfly rejected an in-order advertisement at its arrival");
            }
            _statistics.advertisements_consumed++;
        } else {
            retained.push_back(std::move(pending));
        }
    }
    _pending_advertisements = std::move(retained);

    if (now == _next_advertisement_tick_ps) {
        emitAdvertisements();
        _next_advertisement_tick_ps += _config.advertisement_period_ps;
    }
    scheduleControlPlane();
}

void SsDragonflyTopology::emitAdvertisements() {
    const simtime_picosec now = EventList::now();
    for (std::uint64_t link_index = 0; link_index < _links.size(); ++link_index) {
        DirectedLink& link = _links[link_index];
        // The downstream router advertises its switch-wide waiting bytes to
        // the upstream side of the link; this aggregate scope is a named
        // calibrated abstraction (see the design note).
        const htsim::DragonflyDownstreamAdvertisement advertisement =
            link.congestion->makeDownstreamAdvertisement(
                link.next_advertisement_sequence++, now,
                static_cast<std::uint64_t>(switchWaitingBytes(link.downstream_router)));
        _pending_advertisements.push_back(
            PendingAdvertisement{advertisement.earliest_arrival_time_ps, link_index,
                                 advertisement});
    }
}

void SsDragonflyTopology::scheduleControlPlane() {
    simtime_picosec next = _next_advertisement_tick_ps;
    for (const PendingAdvertisement& pending : _pending_advertisements) {
        next = std::min(next, pending.arrival_ps);
    }
    _eventlist.sourceIsPending(*_control_plane, next);
}

void SsDragonflyTopology::deferFailure(std::exception_ptr failure) noexcept {
    if (_deferred_failure == nullptr) {
        _deferred_failure = std::move(failure);
    }
}

void SsDragonflyTopology::rethrowDeferredFailure() const {
    if (_deferred_failure != nullptr) {
        std::rethrow_exception(_deferred_failure);
    }
}

void SsDragonflyTopology::validateQuiescent() const {
    rethrowDeferredFailure();
    _route_state_store.validateQuiescent();
    for (const DirectedLink& link : _links) {
        if (!link.in_flight.empty()) {
            throw std::logic_error("ss-dragonfly link transport is not quiescent");
        }
    }
}
