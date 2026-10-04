// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_breakout_topology.h"

#include <limits>
#include <stdexcept>

namespace {
std::uint64_t checkedAdd(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        throw std::overflow_error("breakout transit timestamp overflow");
    }
    return a + b;
}

std::size_t priorityIndex(const Packet& packet) {
    switch (packet.priority()) {
    case Packet::PRIO_HI: return 0;
    case Packet::PRIO_MID: return 1;
    case Packet::PRIO_LO: return 2;
    default: throw std::invalid_argument("finite output requires explicit packet priority");
    }
}
}

RnicFinitePriorityQueue::RnicFinitePriorityQueue(
        EventList& events, std::uint64_t capacity_bps,
        std::uint64_t capacity_bytes, StorageCharge storage_charge, DropObserver drop)
    : EventSource(events, "finite_priority_output"),
      _serializer(capacity_bps), _capacity(capacity_bytes),
      _storage_charge(std::move(storage_charge)), _drop(std::move(drop)) {
    if (_capacity == 0 || !_storage_charge) {
        throw std::invalid_argument("finite output requires nonzero byte capacity");
    }
}

RnicFinitePriorityQueue::~RnicFinitePriorityQueue() {
    eventlist().cancelPendingSource(*this);
    if (_active != nullptr) _active->free();
    for (auto& queue : _queues) {
        for (const auto& entry : queue) entry.packet->free();
    }
}

void RnicFinitePriorityQueue::receivePacket(Packet& packet) {
    const auto priority = priorityIndex(packet);
    if (packet.size() == 0) {
        throw std::invalid_argument("finite output requires nonempty packet");
    }
    const auto charge = _storage_charge(packet);
    if (charge == 0) throw std::invalid_argument("finite output requires nonzero storage charge");
    if (charge > _capacity - _occupancy) {
        ++_drops;
        if (_drop) _drop(packet, EventList::now());
        packet.free();
        return;
    }
    _queues[priority].push_back({&packet, charge});
    _occupancy += charge;
    _high_watermark = std::max(_high_watermark, _occupancy);
    if (_active == nullptr) startService();
}

void RnicFinitePriorityQueue::startService() {
    for (auto& queue : _queues) {
        if (!queue.empty()) {
            _active = queue.front().packet;
            _active_charge = queue.front().charge;
            queue.pop_front();
            const auto interval = _serializer.serialize(EventList::now(), _active->size());
            eventlist().sourceIsPending(*this, interval.end_ps);
            return;
        }
    }
    // Observe a true idle boundary so later eligibility cannot inherit a
    // sub-picosecond fractional boundary from the preceding busy period.
    _serializer.rebaseIdle(EventList::now());
}

void RnicFinitePriorityQueue::doNextEvent() {
    if (_active == nullptr) throw std::logic_error("finite output has no active frame");
    Packet* packet = _active;
    _occupancy -= _active_charge;
    _active = nullptr;
    // Select the next head after downstream delivery, allowing same-boundary
    // local arrivals to participate without preempting the completed frame.
    packet->sendOn();
    if (_active == nullptr) startService();
}

RnicBreakoutTopology::RnicBreakoutTopology(
        EventList& events, RnicBreakoutTopologyConfig config,
        RnicFinitePriorityQueue::StorageCharge storage_charge,
        RnicFinitePriorityQueue::DropObserver drop)
    : _config(config), _endpoints(config.endpoints, nullptr) {
    if (config.endpoints < 2 || config.lanes_per_endpoint == 0 || config.lane_capacity_bps == 0) {
        throw std::invalid_argument("breakout requires multiple endpoints and nonzero lanes/rate");
    }
    if ((config.endpoints_per_leaf == 0) != (config.spines == 0)) {
        throw std::invalid_argument("Clos requires both endpoints per leaf and spine count");
    }
    const auto ingress_delay = checkedAdd(config.uplink_propagation_ps, config.switch_processing_ps);
    const auto fabric_delay = config.spines == 0 ? 0 : checkedAdd(
        config.inter_switch_propagation_ps, config.switch_processing_ps);
    for (std::uint32_t node = 0; node < config.endpoints; ++node) {
        for (std::uint32_t lane = 0; lane < config.lanes_per_endpoint; ++lane) {
            _source.emplace_back(std::make_unique<RnicFinitePriorityQueue>(
                events, config.lane_capacity_bps, config.source_queue_bytes, storage_charge, drop));
            _egress.emplace_back(std::make_unique<RnicFinitePriorityQueue>(
                events, config.lane_capacity_bps, config.switch_egress_queue_bytes, storage_charge, drop));
            _uplink.emplace_back(std::make_unique<Pipe>(ingress_delay, events));
            _downlink.emplace_back(std::make_unique<Pipe>(config.downlink_propagation_ps, events));
        }
    }
    if (config.spines != 0) {
        _leaves = 1 + (config.endpoints - 1) / config.endpoints_per_leaf;
        for (std::uint32_t leaf = 0; leaf < _leaves; ++leaf) {
            for (std::uint32_t spine = 0; spine < config.spines; ++spine) {
                for (std::uint32_t lane = 0; lane < config.lanes_per_endpoint; ++lane) {
                    _leaf_uplink.emplace_back(std::make_unique<RnicFinitePriorityQueue>(
                        events, config.lane_capacity_bps, config.switch_egress_queue_bytes,
                        storage_charge, drop));
                    _spine_egress.emplace_back(std::make_unique<RnicFinitePriorityQueue>(
                        events, config.lane_capacity_bps, config.switch_egress_queue_bytes,
                        storage_charge, drop));
                    _leaf_to_spine.emplace_back(std::make_unique<Pipe>(fabric_delay, events));
                    _spine_to_leaf.emplace_back(std::make_unique<Pipe>(fabric_delay, events));
                }
            }
        }
    }
}

std::size_t RnicBreakoutTopology::index(std::uint32_t node, std::uint32_t lane) const {
    if (node >= _config.endpoints || lane >= _config.lanes_per_endpoint) {
        throw std::out_of_range("breakout endpoint or lane out of range");
    }
    return static_cast<std::size_t>(node) * _config.lanes_per_endpoint + lane;
}

std::size_t RnicBreakoutTopology::fabricIndex(
        std::uint32_t leaf, std::uint32_t spine, std::uint32_t lane) const {
    if (leaf >= _leaves || spine >= _config.spines || lane >= _config.lanes_per_endpoint) {
        throw std::out_of_range("Clos leaf, spine or lane out of range");
    }
    return (static_cast<std::size_t>(leaf) * _config.spines + spine) *
        _config.lanes_per_endpoint + lane;
}

bool RnicBreakoutTopology::crossesLeaves(
        std::uint32_t source, std::uint32_t destination) const {
    index(source, 0);
    index(destination, 0);
    return _config.endpoints_per_leaf != 0 &&
        source / _config.endpoints_per_leaf != destination / _config.endpoints_per_leaf;
}

const Route& RnicBreakoutTopology::route(
        std::uint32_t source, std::uint32_t destination,
        std::uint32_t source_lane, std::uint32_t destination_lane,
        PacketSink& endpoint, bool source_already_serialized, std::uint32_t spine) {
    const auto si = index(source, source_lane);
    const auto di = index(destination, destination_lane);
    if (source == destination) throw std::invalid_argument("breakout requires distinct endpoints");
    if (_endpoints[destination] != nullptr && _endpoints[destination] != &endpoint) {
        throw std::invalid_argument("breakout destination already bound to another sink");
    }
    if ((_config.spines == 0 && spine != 0) ||
        (_config.spines != 0 && spine >= _config.spines)) {
        throw std::out_of_range("Clos selected spine out of range");
    }
    const bool inter_leaf = crossesLeaves(source, destination);
    const Key key{source, destination, source_lane, destination_lane,
                  source_already_serialized, inter_leaf ? spine : 0};
    auto found = _routes.find(key);
    if (found != _routes.end()) return *found->second;
    auto route = std::make_unique<Route>();
    if (!source_already_serialized) route->push_back(_source[si].get());
    route->push_back(_uplink[si].get());
    if (inter_leaf) {
        const auto up = fabricIndex(source / _config.endpoints_per_leaf, spine, source_lane);
        const auto down = fabricIndex(destination / _config.endpoints_per_leaf, spine, destination_lane);
        route->push_back(_leaf_uplink[up].get());
        route->push_back(_leaf_to_spine[up].get());
        route->push_back(_spine_egress[down].get());
        route->push_back(_spine_to_leaf[down].get());
    }
    route->push_back(_egress[di].get());
    route->push_back(_downlink[di].get());
    route->push_back(&endpoint);
    const auto inserted = _routes.emplace(key, std::move(route));
    _endpoints[destination] = &endpoint;
    return *inserted.first->second;
}

const Route& RnicBreakoutTopology::hashedRoute(
        std::uint32_t source, std::uint32_t destination,
        std::uint32_t source_lane, std::uint16_t udp_source_port,
        const std::vector<std::uint32_t>& forwarding_group,
        const LaneHash& hash, PacketSink& endpoint, bool source_already_serialized) {
    if (!hash) throw std::invalid_argument("breakout ECMP requires an explicit hash mapping");
    if (forwarding_group.empty()) throw std::invalid_argument("breakout forwarding group is empty");
    for (auto lane : forwarding_group) index(destination, lane);
    const auto member = hash(source, destination, udp_source_port, forwarding_group.size());
    if (member >= forwarding_group.size()) throw std::out_of_range("breakout hash member out of range");
    return route(source, destination, source_lane, forwarding_group[member],
                 endpoint, source_already_serialized);
}

const RnicFinitePriorityQueue& RnicBreakoutTopology::sourceQueue(
        std::uint32_t node, std::uint32_t lane) const { return *_source[index(node, lane)]; }
const RnicFinitePriorityQueue& RnicBreakoutTopology::egressQueue(
        std::uint32_t node, std::uint32_t lane) const { return *_egress[index(node, lane)]; }

const RnicFinitePriorityQueue& RnicBreakoutTopology::leafUplinkQueue(
        std::uint32_t leaf, std::uint32_t spine, std::uint32_t lane) const {
    return *_leaf_uplink[fabricIndex(leaf, spine, lane)];
}

const RnicFinitePriorityQueue& RnicBreakoutTopology::spineEgressQueue(
        std::uint32_t spine, std::uint32_t leaf, std::uint32_t lane) const {
    return *_spine_egress[fabricIndex(leaf, spine, lane)];
}

std::uint64_t RnicBreakoutTopology::noQueueTransitPs(
        std::uint64_t wire_bytes, bool source_already_serialized) const {
    RnicWireSerializationClock clock(_config.lane_capacity_bps);
    const auto serialization = clock.serialize(0, wire_bytes).end_ps;
    auto result = checkedAdd(_config.uplink_propagation_ps, _config.switch_processing_ps);
    result = checkedAdd(result, _config.downlink_propagation_ps);
    result = checkedAdd(result, serialization);
    return source_already_serialized ? result : checkedAdd(result, serialization);
}

std::uint64_t RnicBreakoutTopology::noQueueTransitPs(
        std::uint64_t wire_bytes, std::uint32_t source, std::uint32_t destination,
        bool source_already_serialized) const {
    auto result = noQueueTransitPs(wire_bytes, source_already_serialized);
    if (!crossesLeaves(source, destination)) return result;
    RnicWireSerializationClock clock(_config.lane_capacity_bps);
    const auto serialization = clock.serialize(0, wire_bytes).end_ps;
    const auto switch_hop = checkedAdd(_config.inter_switch_propagation_ps,
                                      _config.switch_processing_ps);
    for (int hop = 0; hop < 2; ++hop) {
        result = checkedAdd(result, serialization);
        result = checkedAdd(result, switch_hop);
    }
    return result;
}
