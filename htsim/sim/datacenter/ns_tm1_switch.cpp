// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_tm1_switch.h"
#include "rnic_wide_integer.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {
uint64_t add(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) throw std::overflow_error("TM1 arithmetic overflow");
    return a + b;
}
size_t priorityIndex(const Packet& packet) {
    switch (packet.priority()) {
    case Packet::PRIO_HI: return 0;
    case Packet::PRIO_MID: return 1;
    case Packet::PRIO_LO: return 2;
    default: throw std::invalid_argument("TM1 needs explicit packet priority");
    }
}
}

class NsTm1Switch::Egress final : public EventSource {
public:
    Egress(NsTm1Switch& owner, uint32_t id, uint64_t bps, uint8_t mask)
        : EventSource(owner._events, "ns-tm1-egress"), owner(owner), id(id),
          mask(mask), wire(bps) {}
    void doNextEvent() override {
        armed = false;
        if (active) owner.complete(*this);
        else owner.start(*this);
    }
    NsTm1Switch& owner;
    uint32_t id;
    uint8_t mask;
    RnicWireSerializationClock wire;
    std::array<std::deque<Entry>, 3> queues;
    std::optional<Entry> active;
    NsTm1EgressStatistics stats;
    bool armed{false};
};

NsTm1IngressPort::NsTm1IngressPort(NsTm1Switch& owner, uint32_t ingress,
                                   uint32_t egress)
    : _owner(owner), _ingress(ingress), _egress(egress),
      _name("ns-tm1-ingress-" + std::to_string(ingress) + "-" + std::to_string(egress)) {}

void NsTm1IngressPort::receivePacket(Packet& packet) {
    _owner.receive(packet, _ingress, _egress);
}

NsTm1Switch::NsTm1Switch(EventList& events, NsTm1Config config, FrameBytes frame_bytes)
    : _events(events), _config(config), _frame_bytes(std::move(frame_bytes)) {
    if (!_frame_bytes || !config.tick_ps || !config.alpha_denominator ||
        !config.data_queue_bytes || !config.control_queue_bytes)
        throw std::invalid_argument("invalid TM1 configuration");
    for (size_t i = 0; i < 4; ++i)
        if (config.unavailable_cells[i] > config.advertised_cells[i])
            throw std::invalid_argument("TM1 unavailable cells exceed advertised capacity");
}

NsTm1Switch::~NsTm1Switch() {
    for (auto& egress : _egresses) {
        _events.cancelPendingSource(*egress);
        if (egress->active) {
            release(*egress, *egress->active);
            egress->active->packet->free();
        }
        for (auto& queue : egress->queues) for (auto& entry : queue) {
            release(*egress, entry);
            entry.packet->free();
        }
    }
}

uint64_t NsTm1Switch::cellsForFrame(uint64_t frame_bytes, uint64_t overhead) {
    if (!frame_bytes) throw std::invalid_argument("TM1 frame must be nonempty");
    const auto bytes = add(frame_bytes, overhead);
    return bytes / NsTm1Config::cell_bytes + (bytes % NsTm1Config::cell_bytes != 0);
}

uint64_t NsTm1Switch::roundLaunch(uint64_t time_ps, uint64_t tick_ps) {
    if (!tick_ps) throw std::invalid_argument("TM1 launch tick must be nonzero");
    const auto remainder = time_ps % tick_ps;
    return remainder ? add(time_ps, tick_ps - remainder) : time_ps;
}

uint32_t NsTm1Switch::addEgress(uint64_t capacity_bps, uint8_t mask) {
    if (!mask || mask > 15) throw std::invalid_argument("TM1 XPE mask must be in [1,15]");
    const auto id = static_cast<uint32_t>(_egresses.size());
    _egresses.emplace_back(std::make_unique<Egress>(*this, id, capacity_bps, mask));
    return id;
}

NsTm1IngressPort& NsTm1Switch::ingress(uint32_t ingress_id, uint32_t egress_id) {
    if (egress_id >= _egresses.size()) throw std::out_of_range("TM1 egress ID");
    _ingresses.emplace_back(std::make_unique<NsTm1IngressPort>(*this, ingress_id, egress_id));
    return *_ingresses.back();
}

void NsTm1Switch::drop(Packet& packet, uint64_t& reason) {
    ++reason;
    ++_counters.dropped_packets;
    _counters.dropped_wire_bytes += packet.size();
    packet.free();
}

void NsTm1Switch::receive(Packet& packet, uint32_t ingress_id, uint32_t egress_id) {
    Egress& egress = *_egresses.at(egress_id);
    const size_t priority = priorityIndex(packet);
    const auto cells = cellsForFrame(_frame_bytes(packet), _config.internal_overhead_bytes);
    if (!packet.size()) throw std::invalid_argument("TM1 wire frame must be nonempty");
    const auto prospective = add(egress.stats.occupancy_cells[priority], cells);
    const auto control_prospective = priority == 2 ? 0 : add(cells,
        add(egress.stats.occupancy_cells[0], egress.stats.occupancy_cells[1]));
    const auto cap_bytes = priority == 2 ? _config.data_queue_bytes : _config.control_queue_bytes;
    if ((priority != 2 || _config.admission == NsTm1Admission::Static) &&
        (priority == 2 ? prospective : control_prospective) > cap_bytes / NsTm1Config::cell_bytes) {
        drop(packet, _counters.queue_drops); return;
    }
    uint64_t minimum_free_after = UINT64_MAX;
    for (size_t domain = 0; domain < 4; ++domain) if (egress.mask & (1u << domain)) {
        const auto capacity = _config.advertised_cells[domain] - _config.unavailable_cells[domain];
        if (cells > capacity - _occupied[domain]) {
            drop(packet, _counters.pool_drops); return;
        }
        minimum_free_after = std::min(minimum_free_after, capacity - _occupied[domain] - cells);
    }
    if (priority == 2 && _config.admission == NsTm1Admission::Dynamic) {
        const RnicWideInteger scaled = static_cast<RnicWideInteger>(minimum_free_after) *
                                       _config.alpha_numerator;
        if (static_cast<RnicWideInteger>(prospective) * _config.alpha_denominator > scaled ||
            (_config.dynamic_data_hard_cap_bytes && prospective >
             _config.dynamic_data_hard_cap_bytes / NsTm1Config::cell_bytes)) {
            drop(packet, _counters.dynamic_drops); return;
        }
    }
    const auto now = EventList::now();
    egress.queues[priority].push_back(Entry{&packet, cells, now,
        roundLaunch(add(now, _config.processing_ps), _config.tick_ps), ingress_id, priority});
    egress.stats.occupancy_cells[priority] = prospective;
    egress.stats.maximum_cells[priority] = std::max(egress.stats.maximum_cells[priority], prospective);
    for (size_t domain = 0; domain < 4; ++domain) if (egress.mask & (1u << domain)) {
        _occupied[domain] += cells;
        _maximum[domain] = std::max(_maximum[domain], _occupied[domain]);
    }
    ++_counters.admitted_packets;
    _counters.admitted_wire_bytes += packet.size();
    if (!egress.active) start(egress);
}

void NsTm1Switch::start(Egress& egress) {
    if (egress.active) return;
    if (egress.armed) {
        _events.cancelPendingSource(egress);
        egress.armed = false;
    }
    uint64_t next_ready = UINT64_MAX;
    for (auto& queue : egress.queues) if (!queue.empty()) {
        next_ready = std::min(next_ready, queue.front().ready_ps);
        if (queue.front().ready_ps > EventList::now()) continue;
        egress.active = queue.front();
        queue.pop_front();
        const auto wait = EventList::now() - egress.active->ready_ps;
        egress.stats.maximum_buffered_residence_ps = std::max(egress.stats.maximum_buffered_residence_ps,
            EventList::now() - egress.active->arrived_ps);
        egress.stats.maximum_queue_wait_ps = std::max(egress.stats.maximum_queue_wait_ps, wait);
        egress.stats.total_queue_wait_ps += wait;
        const auto interval = egress.wire.serialize(EventList::now(), egress.active->packet->size());
        _events.sourceIsPending(egress, interval.end_ps);
        egress.armed = true;
        return;
    }
    if (next_ready != UINT64_MAX) {
        _events.sourceIsPending(egress, next_ready);
        egress.armed = true;
    } else egress.wire.rebaseIdle(EventList::now());
}

void NsTm1Switch::release(const Egress& egress, const Entry& entry) {
    for (size_t domain = 0; domain < 4; ++domain) if (egress.mask & (1u << domain)) {
        if (_occupied[domain] < entry.cells) throw std::logic_error("TM1 XPE accounting underflow");
        _occupied[domain] -= entry.cells;
    }
}

void NsTm1Switch::complete(Egress& egress) {
    const auto entry = *egress.active;
    release(egress, entry);
    egress.stats.occupancy_cells[entry.priority] -= entry.cells;
    egress.stats.service_wire_bytes += entry.packet->size();
    ++_counters.delivered_packets;
    _counters.delivered_wire_bytes += entry.packet->size();
    egress.active.reset();
    entry.packet->sendOn();
    start(egress);
}

const NsTm1EgressStatistics& NsTm1Switch::statistics(uint32_t egress) const {
    return _egresses.at(egress)->stats;
}

uint64_t NsTm1Switch::pendingPackets() const {
    uint64_t count = 0;
    for (const auto& egress : _egresses) {
        count += egress->active.has_value();
        for (const auto& queue : egress->queues) count += queue.size();
    }
    return count;
}
