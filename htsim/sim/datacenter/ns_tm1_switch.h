// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NS_TM1_SWITCH_H
#define NS_TM1_SWITCH_H

#include "eventlist.h"
#include "network.h"
#include "rnic_wire_serialization.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class NsTm1Admission { Static, Dynamic };

struct NsTm1Config {
    // OpenBCM TM1 advertises 20165 cells per XPE. Physical CFAP reserve is
    // a separate accounting domain and is not subtracted from this value.
    static constexpr uint64_t cell_bytes = 208;
    std::array<uint64_t, 4> advertised_cells{{20165, 20165, 20165, 20165}};
    // Unknown installed NX-OS reservations are explicit sensitivity inputs.
    std::array<uint64_t, 4> unavailable_cells{{0, 0, 0, 0}};
    uint64_t internal_overhead_bytes{0};
    uint64_t data_queue_bytes{262144};
    uint64_t control_queue_bytes{8192};
    NsTm1Admission admission{NsTm1Admission::Static};
    uint64_t alpha_numerator{1};
    uint64_t alpha_denominator{2};
    // In dynamic mode zero disables the additional static data hard cap.
    uint64_t dynamic_data_hard_cap_bytes{0};
    uint64_t processing_ps{450000};
    uint64_t tick_ps{8000};
};

struct NsTm1Counters {
    uint64_t admitted_packets{0}, admitted_wire_bytes{0};
    uint64_t delivered_packets{0}, delivered_wire_bytes{0};
    uint64_t dropped_packets{0}, dropped_wire_bytes{0};
    uint64_t queue_drops{0}, pool_drops{0}, dynamic_drops{0};
};

struct NsTm1EgressStatistics {
    std::array<uint64_t, 3> occupancy_cells{{0, 0, 0}};
    std::array<uint64_t, 3> maximum_cells{{0, 0, 0}};
    uint64_t maximum_queue_wait_ps{0};
    uint64_t maximum_buffered_residence_ps{0};
    uint64_t total_queue_wait_ps{0};
    uint64_t service_wire_bytes{0};
};

class NsTm1Switch;

// One route adapter names physical ingress and egress IDs explicitly. It
// receives the complete source frame; the default model is store-forward.
class NsTm1IngressPort final : public PacketSink {
public:
    NsTm1IngressPort(NsTm1Switch& owner, uint32_t ingress, uint32_t egress);
    void receivePacket(Packet& packet) override;
    const string& nodename() override { return _name; }
private:
    NsTm1Switch& _owner;
    uint32_t _ingress, _egress;
    string _name;
};

// Behavioral, opt-in TM1 surrogate. A configured mask checks and charges
// the full frame in EVERY selected XPE. This conservative replicated charge
// is a sensitivity model, not a claim about physical cell placement. Cells
// remain charged through the last serialized bit. Preamble and IFG belong
// to Packet::size() wire bytes, not the supplied FCS-inclusive frame extent.
class NsTm1Switch {
public:
    using FrameBytes = std::function<uint64_t(const Packet&)>;
    NsTm1Switch(EventList& events, NsTm1Config config, FrameBytes frame_bytes);
    ~NsTm1Switch();
    NsTm1Switch(const NsTm1Switch&) = delete;
    NsTm1Switch& operator=(const NsTm1Switch&) = delete;
    uint32_t addEgress(uint64_t capacity_bps, uint8_t xpe_mask);
    NsTm1IngressPort& ingress(uint32_t ingress_id, uint32_t egress_id);
    void receive(Packet& packet, uint32_t ingress_id, uint32_t egress_id);
    static uint64_t cellsForFrame(uint64_t frame_bytes, uint64_t overhead = 0);
    static uint64_t roundLaunch(uint64_t time_ps, uint64_t tick_ps);
    const NsTm1Config& config() const { return _config; }
    const NsTm1Counters& counters() const { return _counters; }
    const NsTm1EgressStatistics& statistics(uint32_t egress) const;
    const std::array<uint64_t, 4>& occupiedCells() const { return _occupied; }
    const std::array<uint64_t, 4>& maximumCells() const { return _maximum; }
    uint64_t pendingPackets() const;

private:
    friend class NsTm1IngressPort;
    class Egress;
    struct Entry {
        Packet* packet;
        uint64_t cells, arrived_ps, ready_ps;
        uint32_t ingress;
        size_t priority;
    };
    void start(Egress& egress);
    void complete(Egress& egress);
    void release(const Egress& egress, const Entry& entry);
    void drop(Packet& packet, uint64_t& reason);
    EventList& _events;
    NsTm1Config _config;
    FrameBytes _frame_bytes;
    NsTm1Counters _counters;
    std::array<uint64_t, 4> _occupied{{0, 0, 0, 0}}, _maximum{{0, 0, 0, 0}};
    std::vector<std::unique_ptr<Egress>> _egresses;
    std::vector<std::unique_ptr<NsTm1IngressPort>> _ingresses;
};

#endif
