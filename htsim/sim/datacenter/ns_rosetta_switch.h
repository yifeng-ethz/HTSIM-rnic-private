// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NS_ROSETTA_SWITCH_H
#define NS_ROSETTA_SWITCH_H

#include "fat_tree_switch.h"
#include "queue.h"
#include "rnic_ss_core.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class NsRosetta;

// Switch attribution and RNIC control use one canonical directed endpoint-pair
// identity; no translation table or global lookup is involved.
using NsRosettaEndpointPair = RnicSsEndpointPair;

struct NsRosettaBufferCounters {
    uint64_t admitted_packets{0};
    mem_b admitted_bytes{0};
    uint64_t granted_packets{0};
    mem_b granted_bytes{0};
    uint64_t dropped_packets{0};
    mem_b dropped_bytes{0};
};

struct NsRosettaRequestDepth {
    uint32_t requesting_ingresses{0};
    uint64_t queued_packets{0};
    mem_b queued_bytes{0};
};

// A route selector may sample this switch-local state.  The snapshot neither
// predicts future arrivals nor imports state from another switch.
struct NsRosettaPathLoad {
    simtime_picosec sampled_at{0};
    uint32_t egress_id{0};
    uint32_t requesting_ingresses{0};
    uint64_t queued_packets{0};
    mem_b buffered_bytes{0};
    mem_b in_service_bytes{0};
    mem_b backlog_bytes{0};
    simtime_picosec backlog_delay_ps{0};
    mem_b shared_buffer_occupancy{0};
    mem_b shared_buffer_capacity{0};
};

enum class NsRosettaQueueTransition {
    Enqueued,
    Granted,
    Dropped,
    SerializationCompleted,
};

// This observation is intentionally a physical, switch-local seam.  A future
// transport may turn destination-egress transitions into delayed packets, but
// the switch does not deliver telemetry or invoke sender control directly.
struct NsRosettaBacklogObservation {
    NsRosettaQueueTransition transition;
    simtime_picosec time;
    FatTreeSwitch::switch_type switch_type;
    uint32_t switch_id;
    uint32_t ingress_id;
    uint32_t egress_id;
    size_t traffic_class;
    NsRosettaEndpointPair pair;
    uint32_t flow_id;
    packetid_t packet_id;
    mem_b packet_bytes;
    mem_b voq_buffered_bytes;
    mem_b ingress_buffered_bytes;
    mem_b ingress_backlog_bytes;
    mem_b egress_buffered_bytes;
    mem_b egress_backlog_bytes;
    // The pair fields are this endpoint pair's contribution to the observed
    // egress, rather than an aggregate across the switch's other paths.
    mem_b pair_buffered_bytes;
    mem_b pair_backlog_bytes;
    mem_b shared_buffer_occupancy;
};

class NsRosettaBacklogObserver {
public:
    virtual ~NsRosettaBacklogObserver() = default;
    virtual void observe(const NsRosettaBacklogObservation& observation) noexcept = 0;
};

class NsRosettaIngressPort : public PacketSink {
public:
    NsRosettaIngressPort(NsRosetta& owner, uint32_t ingress_id, std::string name);

    void receivePacket(Packet& pkt) override;
    const string& nodename() override { return _name; }
    uint32_t ingress_id() const { return _ingress_id; }

private:
    NsRosetta& _owner;
    uint32_t _ingress_id;
    std::string _name;
};

// An ns-rosetta egress is a physical serializer, not an independently
// buffered queue.  Waiting packets remain in switch-owned input VoQs.
class NsRosettaEgressSerializer : public BaseQueue {
public:
    NsRosettaEgressSerializer(linkspeed_bps bitrate, EventList& eventlist, QueueLogger* logger);

    void bind(NsRosetta& owner, uint32_t egress_id);
    void receivePacket(Packet& pkt) override;
    void doNextEvent() override;

    mem_b queuesize() const override;
    mem_b maxsize() const override;

    bool is_busy() const { return _packet_in_service != nullptr; }
    mem_b in_service_bytes() const;
    simtime_picosec remaining_service_time_ps() const;
    simtime_picosec serialization_delay_for_bytes(mem_b bytes) const;
    uint32_t egress_id() const { return _egress_id; }
    NsRosetta* owner() const { return _owner; }

    void note_packet_enqueued(Packet& pkt);

private:
    friend class NsRosetta;

    void dispatch(Packet& pkt);
    void note_shared_buffer_drop(Packet& pkt);

    NsRosetta* _owner{nullptr};
    uint32_t _egress_id{UINT32_MAX};
    Packet* _authorized_dispatch{nullptr};
    Packet* _packet_in_service{nullptr};
    simtime_picosec _service_completion_time{0};
};

class NsRosetta : public FatTreeSwitch {
public:
    NsRosetta(EventList& eventlist,
              const string& name,
              switch_type type,
              uint32_t id,
              simtime_picosec switch_delay,
              FatTreeTopology* topology,
              mem_b shared_buffer_capacity);
    ~NsRosetta() override = default;

    int addPort(BaseQueue* queue) override;
    PacketSink* create_physical_ingress(const string& name) override;
    void receivePacket(Packet& pkt) override;

    void receive_from_physical_ingress(Packet& pkt, uint32_t ingress_id);
    void egress_serialization_complete(uint32_t egress_id);

    void set_backlog_observer(std::shared_ptr<NsRosettaBacklogObserver> observer) {
        _backlog_observer = std::move(observer);
    }

    mem_b shared_buffer_capacity() const { return _shared_buffer_capacity; }
    mem_b shared_buffer_occupancy() const { return _shared_buffer_occupancy; }
    mem_b shared_buffer_high_watermark() const { return _shared_buffer_high_watermark; }
    const NsRosettaBufferCounters& buffer_counters() const { return _buffer_counters; }

    size_t physical_ingress_count() const { return _ingresses.size(); }
    size_t physical_egress_count() const { return _egresses.size(); }

    mem_b voq_buffered_bytes(uint32_t ingress_id,
                             uint32_t egress_id,
                             Packet::PktPriority priority) const;
    mem_b ingress_buffered_bytes(uint32_t ingress_id) const;
    mem_b ingress_backlog_bytes(uint32_t ingress_id) const;
    mem_b egress_buffered_bytes(uint32_t egress_id) const;
    mem_b egress_backlog_bytes(uint32_t egress_id) const;
    mem_b egress_pair_buffered_bytes(uint32_t egress_id, const NsRosettaEndpointPair& pair) const;
    mem_b egress_pair_backlog_bytes(uint32_t egress_id, const NsRosettaEndpointPair& pair) const;
    mem_b pair_buffered_bytes(const NsRosettaEndpointPair& pair) const;
    mem_b pair_backlog_bytes(const NsRosettaEndpointPair& pair) const;
    NsRosettaRequestDepth request_depth(uint32_t egress_id, Packet::PktPriority priority) const;
    NsRosettaPathLoad sample_path_load(uint32_t egress_id) const;

private:
    static constexpr size_t kTrafficClassCount = 3;

    struct PacketSummary {
        uint32_t ingress_id;
        uint32_t egress_id;
        size_t traffic_class;
        NsRosettaEndpointPair pair;
        uint32_t flow_id;
        packetid_t packet_id;
        mem_b packet_bytes;
    };

    struct Voq {
        std::deque<Packet*> packets;
        mem_b buffered_bytes{0};
    };

    struct TrafficClassVoqs {
        std::map<uint32_t, Voq> by_ingress;
        bool has_last_granted_ingress{false};
        uint32_t last_granted_ingress{0};
    };

    struct IngressState {
        std::unique_ptr<NsRosettaIngressPort> port;
        mem_b buffered_bytes{0};
        mem_b in_service_bytes{0};
        bool grant_busy{false};
        bool has_last_accepted_egress{false};
        uint32_t last_accepted_egress{0};
    };

    struct PairState {
        mem_b buffered_bytes{0};
        mem_b in_service_bytes{0};
    };

    struct EgressState {
        NsRosettaEgressSerializer* serializer{nullptr};
        std::array<TrafficClassVoqs, kTrafficClassCount> traffic_classes;
        mem_b buffered_bytes{0};
        std::optional<PacketSummary> active_grant;
        std::map<NsRosettaEndpointPair, PairState> pairs;
    };

    struct Grant {
        uint32_t ingress_id;
        uint32_t egress_id;
        size_t traffic_class;
    };

    static size_t traffic_class(Packet::PktPriority priority);
    static PacketSummary summarize(Packet& pkt,
                                   uint32_t ingress_id,
                                   uint32_t egress_id,
                                   size_t traffic_class);
    NsRosettaEgressSerializer& resolve_selected_egress(Packet& pkt);
    void enqueue(Packet& pkt, uint32_t ingress_id, NsRosettaEgressSerializer& egress);
    void run_arbitration();
    std::optional<Grant> select_egress_grant(uint32_t egress_id,
                                             const std::vector<bool>& ingress_matched) const;
    Grant accept_input_grant(uint32_t ingress_id, const std::vector<Grant>& grants) const;
    void dispatch_grant(const Grant& grant);
    void emit_transition(NsRosettaQueueTransition transition, const PacketSummary& packet);
    void validate_ingress(uint32_t ingress_id) const;
    IngressState& ingress_state(uint32_t ingress_id);
    const IngressState& ingress_state(uint32_t ingress_id) const;
    EgressState& egress_state(uint32_t egress_id);
    const EgressState& egress_state(uint32_t egress_id) const;

    mem_b _shared_buffer_capacity;
    mem_b _shared_buffer_occupancy{0};
    mem_b _shared_buffer_high_watermark{0};
    NsRosettaBufferCounters _buffer_counters;

    std::vector<IngressState> _ingresses;
    std::vector<EgressState> _egresses;
    std::map<NsRosettaEndpointPair, PairState> _pairs;
    std::unordered_map<Packet*, uint32_t> _pipeline_ingress;
    std::shared_ptr<NsRosettaBacklogObserver> _backlog_observer;
};

#endif
