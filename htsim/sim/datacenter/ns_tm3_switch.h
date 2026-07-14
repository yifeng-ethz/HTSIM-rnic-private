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
#include <string>
#include <unordered_map>
#include <vector>

class NsTm3Switch;

struct NsTm3BufferCounters {
    uint64_t admitted_packets{0};
    mem_b admitted_bytes{0};
    uint64_t dequeued_packets{0};
    mem_b dequeued_bytes{0};
    uint64_t dropped_packets{0};
    mem_b dropped_bytes{0};
};

class NsTm3IngressPort : public PacketSink {
public:
    NsTm3IngressPort(NsTm3Switch& owner, uint32_t ingress_id,
                         std::string name);

    void receivePacket(Packet& pkt) override;
    const string& nodename() override { return _name; }
    uint32_t ingress_id() const { return _ingress_id; }

private:
    NsTm3Switch& _owner;
    uint32_t _ingress_id;
    std::string _name;
};

// An ns-tm3 egress has no independent packet buffer. It only represents
// physical link serialization; all waiting packets remain in switch-owned
// ingress/egress/class VoQs until this serializer becomes idle.
class NsTm3EgressSerializer : public BaseQueue {
public:
    NsTm3EgressSerializer(linkspeed_bps bitrate, EventList& eventlist,
                              QueueLogger* logger);

    void bind(NsTm3Switch& owner, uint32_t egress_id);
    void receivePacket(Packet& pkt) override;
    void doNextEvent() override;

    mem_b queuesize() const override;
    mem_b maxsize() const override;

    bool is_busy() const { return _packet_in_service != nullptr; }
    mem_b in_service_bytes() const;
    uint32_t egress_id() const { return _egress_id; }
    NsTm3Switch* owner() const { return _owner; }

    void note_packet_enqueued(Packet& pkt);

private:
    friend class NsTm3Switch;

    // The serializer is a physical service element, not a public queueing
    // path. Only its owning traffic manager may advance a packet into it.
    void dispatch(Packet& pkt);
    void note_shared_buffer_drop(Packet& pkt);

    NsTm3Switch* _owner{nullptr};
    uint32_t _egress_id{UINT32_MAX};
    Packet* _authorized_dispatch{nullptr};
    Packet* _packet_in_service{nullptr};
};

class NsTm3Switch : public FatTreeSwitch {
public:
    NsTm3Switch(EventList& eventlist, const string& name,
                    switch_type type, uint32_t id,
                    simtime_picosec switch_delay, FatTreeTopology* topology,
                    mem_b shared_buffer_capacity);
    ~NsTm3Switch() override = default;

    int addPort(BaseQueue* queue) override;
    PacketSink* create_physical_ingress(const string& name) override;
    void receivePacket(Packet& pkt) override;

    void receive_from_physical_ingress(Packet& pkt, uint32_t ingress_id);
    void egress_serialization_complete(uint32_t egress_id);

    mem_b shared_buffer_capacity() const { return _shared_buffer_capacity; }
    mem_b shared_buffer_occupancy() const { return _shared_buffer_occupancy; }
    mem_b shared_buffer_high_watermark() const {
        return _shared_buffer_high_watermark;
    }
    const NsTm3BufferCounters& buffer_counters() const {
        return _buffer_counters;
    }
    mem_b egress_buffered_bytes(uint32_t egress_id) const;
    mem_b egress_backlog_bytes(uint32_t egress_id) const;
    size_t physical_ingress_count() const { return _physical_ingresses.size(); }

private:
    static constexpr size_t kTrafficClassCount = 3;

    struct TrafficClassVoqs {
        std::map<uint32_t, std::deque<Packet*>> packets_by_ingress;
        bool has_last_served_ingress{false};
        uint32_t last_served_ingress{0};
    };

    struct EgressState {
        NsTm3EgressSerializer* serializer{nullptr};
        std::array<TrafficClassVoqs, kTrafficClassCount> traffic_classes;
        mem_b buffered_bytes{0};
    };

    static size_t traffic_class(Packet::PktPriority priority);
    NsTm3EgressSerializer& resolve_selected_egress(Packet& pkt);
    void enqueue(Packet& pkt, uint32_t ingress_id,
                 NsTm3EgressSerializer& egress);
    Packet* select_next_packet(EgressState& egress);
    void schedule_egress(uint32_t egress_id);
    EgressState& egress_state(uint32_t egress_id);
    const EgressState& egress_state(uint32_t egress_id) const;

    mem_b _shared_buffer_capacity;
    mem_b _shared_buffer_occupancy{0};
    mem_b _shared_buffer_high_watermark{0};
    NsTm3BufferCounters _buffer_counters;

    std::vector<std::unique_ptr<NsTm3IngressPort>> _physical_ingresses;
    std::vector<EgressState> _egresses;
    std::unordered_map<Packet*, uint32_t> _pipeline_ingress;
};

#endif
