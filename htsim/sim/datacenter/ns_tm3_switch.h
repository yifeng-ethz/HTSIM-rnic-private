// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NS_TM3_SWITCH_H
#define NS_TM3_SWITCH_H

#include "fat_tree_switch.h"
#include "queue.h"

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

class NsTm3Switch;
class NsTm3DcqcnPolicy;
struct NsTm3DcqcnPolicyConfig;

enum class NsTm3VoqArbitration {
    // Select the oldest head packet across ingress VoQs.  This is the
    // output-FIFO service model used by the CN network-calculus argument.
    OldestHeadFirst,
    // Retained as an explicit sensitivity: each non-empty physical ingress
    // receives one whole-packet turn, independent of its offered rate.
    IngressRoundRobin,
};

struct NsTm3BufferCounters {
    uint64_t admitted_packets{0};
    mem_b admitted_bytes{0};
    uint64_t dequeued_packets{0};
    mem_b dequeued_bytes{0};
    uint64_t dropped_packets{0};
    mem_b dropped_bytes{0};
    uint64_t shared_pool_dropped_packets{0};
    mem_b shared_pool_dropped_bytes{0};
    uint64_t egress_domain_dropped_packets{0};
    mem_b egress_domain_dropped_bytes{0};
};

// Cumulative diagnostics for one physical output. Buffered bytes exclude the
// packet currently in serialization; backlog includes it. The packet fields
// identify the observation that established max_queue_wait_ps.
struct NsTm3EgressStatistics {
    mem_b buffered_high_watermark{0};
    mem_b backlog_high_watermark{0};
    simtime_picosec max_queue_wait_ps{0};
    simtime_picosec max_queue_wait_observed_ps{0};
    uint32_t max_queue_wait_ingress_id{UINT32_MAX};
    uint32_t max_queue_wait_flow_id{0};
    packetid_t max_queue_wait_packet_id{0};
};

enum class NsTm3QueueTransition {
    Enqueued,
    Dequeued,
    Dropped,
    SerializationCompleted,
};

// Exact event-boundary state for one physical output.  Buffered bytes are the
// switch-owned VoQ occupancy; backlog also includes the packet currently on
// the wire.  The observer is optional and has no effect on arbitration.
struct NsTm3QueueObservation {
    NsTm3QueueTransition transition;
    simtime_picosec time_ps;
    FatTreeSwitch::switch_type switch_type;
    uint32_t switch_id;
    uint32_t ingress_id;
    uint32_t egress_id;
    Packet::PktPriority priority;
    uint32_t flow_id;
    packetid_t packet_id;
    mem_b packet_bytes;
    mem_b egress_buffered_bytes;
    mem_b egress_in_service_bytes;
    mem_b egress_backlog_bytes;
    mem_b shared_buffer_occupancy_bytes;
};

class NsTm3QueueObserver {
public:
    virtual ~NsTm3QueueObserver() = default;
    virtual void observe(const NsTm3QueueObservation& observation) = 0;
};

class NsTm3IngressPort : public PacketSink {
public:
    NsTm3IngressPort(NsTm3Switch& owner, uint32_t ingress_id, std::string name);

    void receivePacket(Packet& pkt) override;
    const string& nodename() override { return _name; }
    uint32_t ingress_id() const { return _ingress_id; }

private:
    NsTm3Switch& _owner;
    uint32_t _ingress_id;
    std::string _name;
};

// Several physical ingress ports can deliver a packet in the very same
// picosecond, and the switch pipeline behind them is one shared queue that
// has to take them one at a time.  The order it takes them in decides who
// gets the buffer space a departing packet has just freed, because a
// congested egress frees exactly one packet of room per packet time.  A
// fixed order therefore hands every contested slot to the same port for as
// long as the congestion lasts.  This arbiter presents same-instant
// arrivals in a rotating port order instead, which is what an ingress
// arbiter does in silicon and what a shared buffer needs if its loss is to
// be shared.  A port that arrives alone is a round of one at the same
// instant, so uncontested traffic keeps both its order and its timing.
class NsTm3IngressArbiter : public EventSource {
public:
    NsTm3IngressArbiter(NsTm3Switch& owner, EventList& eventlist, const string& name);
    ~NsTm3IngressArbiter() override;

    // Takes one arrival for presentation at the end of the current instant.
    void offer(Packet& pkt, uint32_t ingress_id);
    void doNextEvent() override;
    // The arbiter carries no payload of its own and never holds a packet
    // across an instant, so it is not a traffic event.
    bool isTraffic() override { return false; }

private:
    struct Offer {
        uint32_t ingress_id;
        Packet* packet;
    };

    NsTm3Switch& _owner;
    std::vector<Offer> _offers;
    bool _armed{false};
    uint32_t _next_grant_ingress{0};
};

// An ns-tm3 egress has no independent packet buffer. It only represents
// physical link serialization; all waiting packets remain in switch-owned
// ingress/egress/class VoQs until this serializer becomes idle.
class NsTm3EgressSerializer : public BaseQueue {
public:
    NsTm3EgressSerializer(linkspeed_bps bitrate, EventList& eventlist, QueueLogger* logger);

    void bind(NsTm3Switch& owner, uint32_t egress_id);
    void receivePacket(Packet& pkt) override;
    void doNextEvent() override;

    mem_b queuesize() const override;
    mem_b maxsize() const override;

    bool is_busy() const { return _packet_in_service != nullptr; }
    bool data_is_paused() const { return _data_paused; }
    // Cascade depth of the pause currently holding this egress (0 when not
    // paused). Recorded from the arriving PFC frame per the
    // comparator-realism ruling.
    uint32_t active_pause_cascade_depth() const {
        return _data_paused ? _active_pause_cascade_depth : 0;
    }
    mem_b in_service_bytes() const;
    uint32_t egress_id() const { return _egress_id; }
    NsTm3Switch* owner() const { return _owner; }

    void note_packet_enqueued(Packet& pkt);

private:
    friend class NsTm3Switch;

    // The serializer is a physical service element, not a public queueing
    // path. Only its owning traffic manager may advance a packet into it.
    void dispatch(Packet& pkt);
    void note_buffer_drop(Packet& pkt);

    NsTm3Switch* _owner{nullptr};
    uint32_t _egress_id{UINT32_MAX};
    Packet* _authorized_dispatch{nullptr};
    Packet* _packet_in_service{nullptr};
    bool _data_paused{false};
    uint32_t _active_pause_cascade_depth{0};
};

class NsTm3Switch : public FatTreeSwitch {
public:
    NsTm3Switch(EventList& eventlist,
                const string& name,
                switch_type type,
                uint32_t id,
                simtime_picosec switch_delay,
                FatTreeTopology* topology,
                mem_b shared_buffer_capacity);
    ~NsTm3Switch() override;

    int addPort(BaseQueue* queue) override;
    PacketSink* create_physical_ingress(const string& name) override;
    void receivePacket(Packet& pkt) override;

    void receive_from_physical_ingress(Packet& pkt, uint32_t ingress_id);
    void egress_serialization_complete(uint32_t egress_id);
    void egress_pause_state_changed(uint32_t egress_id);
    // Highest cascade depth among this switch's currently paused egresses
    // (0 when none is paused); the local policy adds one when it emits a
    // pause of its own, so PFC pause trees are measurable.
    uint32_t max_active_egress_pause_depth() const;
    void configure_dcqcn_policy(const NsTm3DcqcnPolicyConfig& config);
    NsTm3DcqcnPolicy* dcqcn_policy() { return _dcqcn_policy.get(); }
    const NsTm3DcqcnPolicy* dcqcn_policy() const { return _dcqcn_policy.get(); }
    void set_queue_observer(std::shared_ptr<NsTm3QueueObserver> observer) {
        _queue_observer = std::move(observer);
    }
    void set_voq_arbitration(NsTm3VoqArbitration arbitration);
    NsTm3VoqArbitration voq_arbitration() const noexcept { return _voq_arbitration; }

    mem_b shared_buffer_capacity() const { return _shared_buffer_capacity; }
    // The shared capacity is one switch-wide physical pool.  This separate
    // cap bounds the switch-owned VoQ bytes mapped to any one physical
    // egress; bytes already in serialization reside in neither domain.
    void set_egress_buffer_capacity(mem_b capacity);
    mem_b egress_buffer_capacity() const { return _egress_buffer_capacity; }
    mem_b shared_buffer_occupancy() const { return _shared_buffer_occupancy; }
    mem_b shared_buffer_high_watermark() const { return _shared_buffer_high_watermark; }
    const NsTm3BufferCounters& buffer_counters() const { return _buffer_counters; }
    mem_b egress_buffered_bytes(uint32_t egress_id) const;
    mem_b egress_backlog_bytes(uint32_t egress_id) const;
    size_t physical_egress_count() const { return _egresses.size(); }
    const NsTm3EgressStatistics& egress_statistics(uint32_t egress_id) const;
    size_t physical_ingress_count() const { return _physical_ingresses.size(); }
    // Drops charged to one physical ingress, over both admission domains.
    // Whether a shared buffer shares its loss is a question only the switch
    // can answer: an endpoint sees retransmissions, and go-back-N amplifies
    // those by an amount that depends on when each sender learned of a gap.
    uint64_t ingress_dropped_packets(uint32_t ingress_id) const;
    // The same count, frozen when the first loss notification crossed this
    // switch.  Before that moment no source has reacted, so every source is
    // still offering at the rate it started with and admission is the only
    // thing that can make the loss unequal.  Equal to the cumulative count
    // while no notification has crossed.
    uint64_t unreacted_ingress_dropped_packets(uint32_t ingress_id) const;
    bool loss_notification_seen() const noexcept { return _loss_notification_seen; }

private:
    friend class NsTm3IngressArbiter;

    static constexpr size_t kTrafficClassCount = 3;

    struct PacketSummary {
        uint32_t ingress_id;
        uint32_t egress_id;
        Packet::PktPriority priority;
        uint32_t flow_id;
        packetid_t packet_id;
        mem_b packet_bytes;
    };

    struct SelectedPacket {
        Packet* packet;
        uint32_t ingress_id;
    };

    struct TrafficClassVoqs {
        std::map<uint32_t, std::deque<Packet*>> packets_by_ingress;
        bool has_last_served_ingress{false};
        uint32_t last_served_ingress{0};
    };

    struct EgressState {
        NsTm3EgressSerializer* serializer{nullptr};
        std::array<TrafficClassVoqs, kTrafficClassCount> traffic_classes;
        mem_b buffered_bytes{0};
        std::unordered_map<Packet*, simtime_picosec> enqueue_time_ps;
        NsTm3EgressStatistics statistics;
        std::optional<PacketSummary> active_packet;
    };

    static size_t traffic_class(Packet::PktPriority priority);
    // Only the ingress arbiter hands a packet to the shared switch pipeline.
    void present_to_pipeline(Packet& pkt) { schedule_through_switch_pipeline(pkt); }
    NsTm3EgressSerializer& resolve_selected_egress(Packet& pkt);
    void enqueue(Packet& pkt, uint32_t ingress_id, NsTm3EgressSerializer& egress);
    std::optional<SelectedPacket> select_next_packet(EgressState& egress);
    void schedule_egress(uint32_t egress_id);
    void emit_queue_observation(NsTm3QueueTransition transition,
                                const PacketSummary& packet);
    EgressState& egress_state(uint32_t egress_id);
    const EgressState& egress_state(uint32_t egress_id) const;

    mem_b _shared_buffer_capacity;
    mem_b _egress_buffer_capacity;
    mem_b _shared_buffer_occupancy{0};
    mem_b _shared_buffer_high_watermark{0};
    NsTm3BufferCounters _buffer_counters;

    std::vector<std::unique_ptr<NsTm3IngressPort>> _physical_ingresses;
    std::vector<uint64_t> _ingress_dropped_packets;
    std::vector<uint64_t> _unreacted_ingress_dropped_packets;
    bool _loss_notification_seen{false};
    std::vector<EgressState> _egresses;
    std::unique_ptr<NsTm3IngressArbiter> _ingress_arbiter;
    std::unordered_map<Packet*, uint32_t> _pipeline_ingress;
    std::unique_ptr<NsTm3DcqcnPolicy> _dcqcn_policy;
    std::shared_ptr<NsTm3QueueObserver> _queue_observer;
    NsTm3VoqArbitration _voq_arbitration{NsTm3VoqArbitration::OldestHeadFirst};
};

#endif
