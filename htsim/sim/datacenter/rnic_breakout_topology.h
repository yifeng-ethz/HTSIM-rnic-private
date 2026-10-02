// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_BREAKOUT_TOPOLOGY_H
#define RNIC_BREAKOUT_TOPOLOGY_H

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "eventlist.h"
#include "network.h"
#include "pipe.h"
#include "rnic_wire_serialization.h"

// Finite whole-frame, nonpreemptive strict-priority output service. Occupancy
// uses an explicit stored-byte charge and includes the transmitting frame. Packet::size() must include the wire extent
// charged by the experiment, including any modeled preamble and interframe gap.
class RnicFinitePriorityQueue final : public EventSource, public PacketSink {
public:
    using StorageCharge = std::function<std::uint64_t(const Packet&)>;
    using DropObserver = std::function<void(const Packet&, std::uint64_t)>;

    RnicFinitePriorityQueue(EventList& events, std::uint64_t capacity_bps,
                            std::uint64_t capacity_bytes, StorageCharge storage_charge, DropObserver drop = {});
    ~RnicFinitePriorityQueue() override;
    void receivePacket(Packet& packet) override;
    void doNextEvent() override;
    const string& nodename() override { return _name; }
    std::uint64_t occupancyChargedBytes() const { return _occupancy; }
    std::uint64_t highWatermarkChargedBytes() const { return _high_watermark; }
    std::uint64_t droppedPackets() const { return _drops; }
    std::uint64_t capacityChargedBytes() const { return _capacity; }

private:
    void startService();
    string _name{"finite_priority_output"};
    RnicWireSerializationClock _serializer;
    std::uint64_t _capacity;
    StorageCharge _storage_charge;
    DropObserver _drop;
    struct Entry { Packet* packet; std::uint64_t charge; };
    std::array<std::deque<Entry>, 3> _queues;
    Packet* _active{nullptr};
    std::uint64_t _active_charge{0};
    std::uint64_t _occupancy{0};
    std::uint64_t _high_watermark{0};
    std::uint64_t _drops{0};
};

struct RnicBreakoutTopologyConfig {
    std::uint32_t endpoints;
    std::uint32_t lanes_per_endpoint;
    std::uint64_t lane_capacity_bps;
    std::uint64_t source_queue_bytes;
    std::uint64_t switch_egress_queue_bytes;
    std::uint64_t uplink_propagation_ps;
    std::uint64_t switch_processing_ps;
    std::uint64_t downlink_propagation_ps;
};

// A single store-and-forward switch with independently serialized full-duplex
// lanes on every endpoint. There is no additional aggregate-capacity wire.
// Forwarding groups list explicitly reachable physical egress lanes. A breakout
// cable alone creates no multipath group. The supplied hash returns a group
// member index and is experimental configuration, not an assertion about an
// unmeasured switch. All destination lanes share one logical endpoint sink.
// Topology, endpoint sinks and cached routes must outlive routed packets; drain
// the event list before destroying them.
class RnicBreakoutTopology {
public:
    using LaneHash = std::function<std::uint32_t(
        std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t)>;

    RnicBreakoutTopology(EventList& events, RnicBreakoutTopologyConfig config,
                         RnicFinitePriorityQueue::StorageCharge storage_charge,
                         RnicFinitePriorityQueue::DropObserver drop = {});
    const Route& route(std::uint32_t source, std::uint32_t destination,
                       std::uint32_t source_lane, std::uint32_t destination_lane,
                       PacketSink& endpoint, bool source_already_serialized = false);
    const Route& hashedRoute(std::uint32_t source, std::uint32_t destination,
                             std::uint32_t source_lane, std::uint16_t udp_source_port,
                             const std::vector<std::uint32_t>& forwarding_group,
                             const LaneHash& hash, PacketSink& endpoint,
                             bool source_already_serialized = false);
    const RnicFinitePriorityQueue& sourceQueue(std::uint32_t node, std::uint32_t lane) const;
    const RnicFinitePriorityQueue& egressQueue(std::uint32_t node, std::uint32_t lane) const;
    std::uint64_t noQueueTransitPs(std::uint64_t wire_bytes,
                                  bool source_already_serialized = false) const;

private:
    std::size_t index(std::uint32_t node, std::uint32_t lane) const;
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, bool>;
    RnicBreakoutTopologyConfig _config;
    std::vector<std::unique_ptr<RnicFinitePriorityQueue>> _source;
    std::vector<std::unique_ptr<RnicFinitePriorityQueue>> _egress;
    std::vector<std::unique_ptr<Pipe>> _uplink;
    std::vector<std::unique_ptr<Pipe>> _downlink;
    std::vector<PacketSink*> _endpoints;
    std::map<Key, std::unique_ptr<Route>> _routes;
};

#endif
