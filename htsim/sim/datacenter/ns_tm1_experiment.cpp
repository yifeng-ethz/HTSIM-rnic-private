// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_tm1_experiment.h"

#include "pipe.h"
#include "rnic_breakout_topology.h"
#include "rnic_max_min_allocator.h"
#include "rnic_prbs_pacer.h"
#include "rnic_wide_integer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
constexpr uint64_t kByteRateDenominator = UINT64_C(8000000000000);
uint64_t duration(uint64_t bytes, uint64_t bps) {
    const RnicWideInteger numerator = static_cast<RnicWideInteger>(bytes) * kByteRateDenominator;
    return static_cast<uint64_t>((numerator + bps - 1) / bps);
}
uint64_t mix(uint64_t x) {
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

class Experiment;
class DataPacket final : public Packet {
public:
    DataPacket(PacketFlow& flow, const Route& route, packetid_t id,
               uint64_t payload, uint64_t created, uint32_t egress, Experiment& owner);
    PktPriority priority() const override { return PRIO_LO; }
    void free() override;
    void delivered();
    uint64_t payload, frame, created;
    uint32_t egress;
    bool terminal{false};
private:
    Experiment& owner;
};

class Receiver final : public PacketSink {
public:
    void receivePacket(Packet& packet) override { static_cast<DataPacket&>(packet).delivered(); }
    const string& nodename() override { return name; }
private:
    string name{"tm1-experiment-receiver"};
};

// Legacy Pipe assumes callers drain before destruction. This experiment can
// unwind from a dispatch error, so its propagation stages also cancel and
// terminate retained packets while packet storage and its ledger are alive.
class ExperimentPipe final : public Pipe {
public:
    ExperimentPipe(uint64_t delay, EventList& events) : Pipe(delay, events) {}
    void receivePacket(Packet& packet) override {
        const auto previous_count = _count;
        try {
            Pipe::receivePacket(packet);
        } catch (...) {
            // Pipe increments its ring count before growing the vector.
            // A failed growth must not turn an uninserted slot into an
            // owned packet during exception cleanup.
            _count = previous_count;
            if (!previous_count) eventlist().cancelPendingSource(*this);
            throw;
        }
    }
    ~ExperimentPipe() override {
        eventlist().cancelPendingSource(*this);
        for (int index = 0; index < _count; ++index)
            _inflight_v[(_next_pop + index) % _size].pkt->free();
    }
};

struct Flow {
    uint64_t id, remaining, rate{0}, credit{0}, credit_remainder{0};
    uint32_t source_lane, egress;
    const Route* route{nullptr};
    uint64_t planned_payload{0};
    std::deque<uint64_t> payloads;
};

class Grant final : public EventSource {
public:
    Grant(Experiment& owner, Flow& flow, uint64_t payload, uint64_t at);
    ~Grant() override { eventlist().cancelPendingSource(*this); }
    void doNextEvent() override;
private:
    Experiment& owner;
    Flow& flow;
    uint64_t payload;
};

class LaneLottery final : public EventSource {
public:
    LaneLottery(Experiment& owner, uint32_t lane);
    ~LaneLottery() override { eventlist().cancelPendingSource(*this); }
    void doNextEvent() override;
private:
    Experiment& owner;
    uint32_t lane;
    RnicPrbsPacer prbs;
    RnicWireSerializationClock clock;
    size_t next_flow{0};
};

class Calendar final : public EventSource {
public:
    explicit Calendar(Experiment& owner);
    ~Calendar() override { eventlist().cancelPendingSource(*this); }
    void doNextEvent() override;
private:
    Experiment& owner;
    uint64_t index{0};
    size_t next_flow{0};
    std::vector<RnicPrbsPacer> prbs;
    std::vector<RnicWireSerializationClock> source_clock, destination_clock;
};

class Experiment {
public:
    explicit Experiment(const NsTm1ExperimentConfig& config)
        : config(config), events(EventList::getTheEventList()),
          traffic_manager(events, config.switch_config,
              [](const Packet& p) { return static_cast<const DataPacket&>(p).frame; }) {
        if (config.source != "4x25" && config.source != "1x100")
            throw std::invalid_argument("source must be 4x25 or 1x100");
        if (config.receiver != "4x25" && config.receiver != "1x100")
            throw std::invalid_argument("receiver must be 4x25 or 1x100");
        if (config.policy != "quota_dd" && config.policy != "quota_prbs" &&
            config.policy != "prbs" && config.policy != "unpaced")
            throw std::invalid_argument("invalid policy");
        if (config.routing != "balanced" && config.routing != "collision" && config.routing != "hash")
            throw std::invalid_argument("invalid routing");
        if (config.mtu <= 64 || config.mtu > 65000 || !config.payload_bytes ||
            !config.window_ps || !config.rate_scale_ppm || config.rate_scale_ppm > 1000000 ||
            !config.fraction_ppm || config.fraction_ppm > 1000000)
            throw std::invalid_argument("invalid experiment size, window or rate");
        if (EventList::trafficEventCount() || EventList::nextEventTime())
            throw std::logic_error("TM1 experiment requires a drained event list");
        EventList::setEndtime(0);
        start_ps = NsTm1Switch::roundLaunch(EventList::now(), config.switch_config.tick_ps);
        source_lanes = config.source == "4x25" ? 4 : 1;
        destination_lanes = config.receiver == "4x25" ? 4 : 1;
        source_bps = (source_lanes == 4 ? UINT64_C(25000000000) : UINT64_C(100000000000)) *
                     config.rate_scale_ppm / 1000000;
        destination_bps = (destination_lanes == 4 ? UINT64_C(25000000000) : UINT64_C(100000000000)) *
                          config.rate_scale_ppm / 1000000;
        const auto source_budget_bps = source_bps * config.fraction_ppm / 1000000;
        const auto destination_budget_bps = destination_bps * config.fraction_ppm / 1000000;
        if (!source_bps || !destination_bps || !source_budget_bps || !destination_budget_bps)
            throw std::invalid_argument("TM1 effective lane capacity or budget rounds to zero");
        result.receiver_capacity_bps = destination_bps * destination_lanes;
        result.requested_payload_bytes = config.payload_bytes * 4;
        for (uint32_t lane = 0; lane < destination_lanes; ++lane) {
            traffic_manager.addEgress(destination_bps, config.xpe_mask);
            downlinks.emplace_back(std::make_unique<ExperimentPipe>(100000, events));
        }
        flows.reserve(16);
        for (uint32_t id = 0; id < 4 * source_lanes; ++id) {
            source_queues.emplace_back(std::make_unique<RnicFinitePriorityQueue>(events,
                source_bps, config.source_queue_bytes,
                [](const Packet& p) { return static_cast<const DataPacket&>(p).frame; },
                [this](const Packet&, uint64_t) { ++result.source_drops; }));
            uplinks.emplace_back(std::make_unique<ExperimentPipe>(100000, events));
        }
        const auto groups = std::max(source_lanes, destination_lanes);
        for (uint32_t node = 0; node < 4; ++node) for (uint32_t lane = 0; lane < groups; ++lane) {
            const auto id = node * groups + lane;
            const auto physical_source = node * source_lanes + lane % source_lanes;
            const uint32_t balanced = lane % destination_lanes;
            const uint32_t egress = config.routing == "collision" ? 0 :
                config.routing == "hash" ? mix(config.seed ^ id) % destination_lanes : balanced;
            flows.push_back(Flow{id, 0, 0, 0, 0, physical_source, egress, nullptr, 0, {}});
            auto route = std::make_unique<Route>();
            route->push_back(source_queues.at(physical_source).get());
            route->push_back(uplinks.at(physical_source).get());
            route->push_back(&traffic_manager.ingress(physical_source, egress));
            route->push_back(downlinks.at(egress).get());
            route->push_back(&receiver);
            flows.back().route = route.get();
            routes.push_back(std::move(route));
        }
        // Packetize each sender once, then stripe whole frames with sender
        // phase rotation. Lane splitting never invents extra tail packets.
        for (uint32_t node = 0; node < 4; ++node) {
            uint64_t remaining = config.payload_bytes, packet_index = 0;
            while (remaining) {
                const auto payload = std::min(remaining, uint64_t(config.mtu - 64));
                auto& flow = flows.at(node * groups + (packet_index + node) % groups);
                flow.payloads.push_back(payload);
                flow.remaining += payload;
                result.expected_wire_bytes += payload + 102;
                remaining -= payload;
                ++packet_index;
            }
        }
        RnicMaxMinAllocator::CapacityMap source_capacity, destination_capacity;
        std::vector<RnicMaxMinFlow> demands;
        for (const auto& flow : flows) {
            if (!flow.remaining) continue;
            source_capacity[flow.source_lane] = source_budget_bps;
            destination_capacity[flow.egress] = destination_budget_bps;
            demands.push_back(RnicMaxMinFlow{flow.id, flow.source_lane, flow.egress, std::nullopt});
        }
        const auto allocation = RnicMaxMinAllocator::allocate(demands, source_capacity, destination_capacity);
        std::vector<uint64_t> source_sum(4 * source_lanes), destination_sum(destination_lanes);
        result.allocation_constraints = true;
        for (auto& flow : flows) {
            if (!flow.remaining) continue;
            flow.rate = allocation.at(flow.id);
            if (!flow.rate) throw std::invalid_argument("TM1 nonempty flow allocation rounds to zero");
            result.allocated_wire_bps += flow.rate;
            source_sum[flow.source_lane] += flow.rate;
            destination_sum[flow.egress] += flow.rate;
        }
        for (const auto rate : source_sum)
            result.allocation_constraints &= rate <= source_bps * config.fraction_ppm / 1000000;
        for (const auto rate : destination_sum)
            result.allocation_constraints &= rate <= destination_bps * config.fraction_ppm / 1000000;
    }

    uint64_t headPayload(const Flow& flow) const { return flow.payloads.front(); }
    void consume(Flow& flow, uint64_t payload) { flow.remaining -= payload; flow.payloads.pop_front(); }
    uint64_t wireForPayload(uint64_t payload) const { return payload + 102; }
    void emit(Flow& flow, uint64_t payload) {
        auto packet = std::make_unique<DataPacket>(packet_flow, *flow.route,
            static_cast<packetid_t>(packets.size() + 1), payload, EventList::now(), flow.egress, *this);
        ++result.generated_packets;
        result.generated_payload_bytes += payload;
        result.generated_wire_bytes += packet->size();
        DataPacket* raw = packet.get();
        packets.push_back(std::move(packet));
        raw->sendOn();
    }
    void terminal(DataPacket& packet, bool drop) {
        if (packet.terminal) throw std::logic_error("TM1 packet terminated twice");
        packet.terminal = true;
        if (drop) {
            ++result.dropped_packets;
            result.dropped_payload_bytes += packet.payload;
        } else {
            ++result.delivered_packets;
            result.delivered_payload_bytes += packet.payload;
            result.delivered_wire_bytes += packet.size();
            result.receiver_lane_wire_bytes[packet.egress] += packet.size();
            result.completion_ps = EventList::now() - start_ps;
            latencies.push_back(EventList::now() - packet.created);
        }
    }
    NsTm1ExperimentResult run() {
        if (config.policy == "quota_dd" || config.policy == "quota_prbs")
            calendar = std::make_unique<Calendar>(*this);
        else for (uint32_t lane = 0; lane < 4 * source_lanes; ++lane)
            lotteries.emplace_back(std::make_unique<LaneLottery>(*this, lane));
        while (EventList::doNextEvent()) {}
        result.switch_drops = traffic_manager.counters().dropped_packets;
        result.pending_packets = 0;
        for (const auto& packet : packets) if (!packet->terminal) ++result.pending_packets;
        // The switch count and packet terminal ledger describe the same
        // objects. A drained event list must leave both counts at zero.
        result.conservation = !result.pending_packets && !traffic_manager.pendingPackets() &&
            result.generated_packets == result.delivered_packets + result.dropped_packets;
        result.complete_payload = result.delivered_payload_bytes == config.payload_bytes * 4;
        for (const auto& source : source_queues)
            result.source_max_bytes = std::max(result.source_max_bytes, source->highWatermarkChargedBytes());
        for (uint32_t lane = 0; lane < destination_lanes; ++lane) {
            const auto& stats = traffic_manager.statistics(lane);
            result.data_max_cells = std::max(result.data_max_cells, stats.maximum_cells[2]);
            result.receiver_lane_max_queue_cells[lane] = stats.maximum_cells[2];
            result.queue_wait_max_ps = std::max(result.queue_wait_max_ps, stats.maximum_queue_wait_ps);
            result.buffered_residence_max_ps = std::max(result.buffered_residence_max_ps,
                stats.maximum_buffered_residence_ps);
        }
        for (const auto cells : traffic_manager.maximumCells()) result.xpe_max_cells = std::max(result.xpe_max_cells, cells);
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            RnicWideInteger total = 0;
            for (const auto latency : latencies) total += latency;
            result.latency_mean_ps = static_cast<uint64_t>(total / latencies.size());
            result.latency_p99_ps = latencies[(latencies.size() * 99 + 99) / 100 - 1];
            result.latency_max_ps = latencies.back();
        }
        for (uint32_t lane = 0; lane < destination_lanes; ++lane)
            if (result.receiver_lane_wire_bytes[lane]) result.minimum_receiver_floor_ps =
                std::max(result.minimum_receiver_floor_ps,
                    duration(result.receiver_lane_wire_bytes[lane], destination_bps));
        if (result.delivered_packets) result.minimum_receiver_floor_ps += 200000 + config.switch_config.processing_ps;
        if (!result.conservation || !result.allocation_constraints)
            throw std::logic_error("TM1 fatal conservation or lane allocation guard failed");
        return result;
    }

    const NsTm1ExperimentConfig config;
    EventList& events;
    // Queue destructors terminate retained packets. Declaration order keeps
    // their callback state, packet objects and routes alive through teardown.
    NsTm1ExperimentResult result;
    uint64_t start_ps{0}, source_bps{0}, destination_bps{0};
    uint32_t source_lanes{0}, destination_lanes{0};
    std::vector<uint64_t> latencies;
    PacketFlow packet_flow{nullptr};
    Receiver receiver;
    std::vector<Flow> flows;
    std::vector<std::unique_ptr<Route>> routes;
    std::vector<std::unique_ptr<DataPacket>> packets;
    NsTm1Switch traffic_manager;
    std::vector<std::unique_ptr<RnicFinitePriorityQueue>> source_queues;
    std::vector<std::unique_ptr<ExperimentPipe>> uplinks, downlinks;
    std::unique_ptr<Calendar> calendar;
    std::vector<std::unique_ptr<LaneLottery>> lotteries;
    std::vector<std::unique_ptr<Grant>> grants;
};

DataPacket::DataPacket(PacketFlow& flow, const Route& route, packetid_t id,
                       uint64_t payload, uint64_t created, uint32_t egress, Experiment& owner)
    : payload(payload), frame(payload + 82), created(created), egress(egress), owner(owner) {
    set_route(flow, route, static_cast<int>(payload + 102), id);
}
void DataPacket::free() { owner.terminal(*this, true); }
void DataPacket::delivered() { owner.terminal(*this, false); }

Grant::Grant(Experiment& owner, Flow& flow, uint64_t payload, uint64_t at)
    : EventSource(owner.events, "tm1-preinstalled-grant"), owner(owner), flow(flow), payload(payload) {
    eventlist().sourceIsPending(*this, at);
}
void Grant::doNextEvent() { owner.emit(flow, payload); }

LaneLottery::LaneLottery(Experiment& owner, uint32_t lane)
    : EventSource(owner.events, "tm1-independent-lane-opportunity"), owner(owner), lane(lane),
      prbs(owner.config.seed, lane), clock(owner.source_bps) {
    eventlist().sourceIsPending(*this, owner.start_ps);
}
void LaneLottery::doNextEvent() {
    std::vector<RnicPrbsWireCandidate> candidates;
    Flow* selected = nullptr;
    for (auto& flow : owner.flows) if (flow.source_lane == lane && flow.remaining) {
        candidates.push_back({flow.id, flow.rate, owner.wireForPayload(owner.headPayload(flow))});
        if (!selected) selected = &flow;
    }
    if (candidates.empty()) return;
    const auto idle_wire = uint64_t(owner.config.mtu + 38);
    bool transmit = owner.config.policy == "unpaced";
    if (transmit) {
        const auto choice = candidates[next_flow++ % candidates.size()].flow_id;
        selected = &owner.flows.at(choice);
    }
    if (!transmit) {
        const auto choice = prbs.selectWireEvent(candidates, owner.source_bps, idle_wire);
        transmit = choice.has_value();
        if (choice) selected = &owner.flows.at(*choice);
    }
    const auto payload = owner.headPayload(*selected), wire = owner.wireForPayload(payload);
    if (transmit) { owner.consume(*selected, payload); owner.emit(*selected, payload); }
    const auto interval = clock.serialize(EventList::now(), transmit ? wire : idle_wire);
    if (std::any_of(owner.flows.begin(), owner.flows.end(), [this](const Flow& flow) {
        return flow.source_lane == lane && flow.remaining;
    })) eventlist().sourceIsPending(*this,
        NsTm1Switch::roundLaunch(interval.end_ps, owner.config.switch_config.tick_ps));
}

Calendar::Calendar(Experiment& owner)
    : EventSource(owner.events, "tm1-preinstalled-calendar"), owner(owner) {
    for (uint32_t lane = 0; lane < 4 * owner.source_lanes; ++lane) {
        source_clock.emplace_back(owner.source_bps);
        prbs.emplace_back(owner.config.seed, lane);
    }
    for (uint32_t i = 0; i < owner.destination_lanes; ++i) destination_clock.emplace_back(owner.destination_bps);
    eventlist().sourceIsPending(*this, owner.start_ps);
}

void Calendar::doNextEvent() {
    const auto begin = owner.start_ps + index * owner.config.window_ps;
    const auto end = begin + owner.config.window_ps;
    ++index;
    ++owner.result.total_windows;
    uint64_t initial_credit = 0, accrued = 0, wire_granted = 0;
    for (auto& flow : owner.flows) if (flow.remaining) {
        initial_credit += flow.credit;
        owner.result.maximum_deficit_carry_bytes = std::max(owner.result.maximum_deficit_carry_bytes, flow.credit);
        const RnicWideInteger budget = static_cast<RnicWideInteger>(flow.rate) * owner.config.window_ps +
                                       flow.credit_remainder;
        const auto bytes = static_cast<uint64_t>(budget / kByteRateDenominator);
        flow.credit_remainder = static_cast<uint64_t>(budget % kByteRateDenominator);
        flow.credit += bytes;
        accrued += bytes;
    }
    size_t failed = 0;
    while (failed < owner.flows.size()) {
        size_t selected = next_flow++ % owner.flows.size();
        if (owner.config.policy == "quota_prbs") {
            // Node/lane-specific PRBS words reorder a finite set of legal
            // remaining whole-packet quotas. No lottery may exceed credit.
            uint64_t best = UINT64_MAX;
            bool found = false;
            for (size_t i = 0; i < owner.flows.size(); ++i) {
                const auto& flow = owner.flows[i];
                if (!flow.remaining || flow.credit < owner.wireForPayload(owner.headPayload(flow))) continue;
                const auto launch = NsTm1Switch::roundLaunch(std::max({begin,
                    source_clock[flow.source_lane].availablePs(), destination_clock[flow.egress].availablePs()}),
                    owner.config.switch_config.tick_ps);
                if (launch >= end) continue;
                const auto word = prbs[flow.source_lane].nextPrbsWord();
                if (!found || word < best) { best = word; selected = i; found = true; }
            }
            if (!found) break;
        }
        auto& flow = owner.flows[selected];
        if (!flow.remaining) { ++failed; continue; }
        const auto payload = owner.headPayload(flow), wire = owner.wireForPayload(payload);
        if (flow.credit < wire) { ++failed; continue; }
        const auto launch = NsTm1Switch::roundLaunch(std::max({begin,
            source_clock[flow.source_lane].availablePs(), destination_clock[flow.egress].availablePs()}),
            owner.config.switch_config.tick_ps);
        if (launch >= end) { ++failed; continue; }
        source_clock[flow.source_lane].serialize(launch, wire);
        destination_clock[flow.egress].serialize(launch, wire);
        owner.grants.emplace_back(std::make_unique<Grant>(owner, flow, payload, launch));
        owner.consume(flow, payload);
        flow.credit -= wire;
        wire_granted += wire;
        failed = 0;
    }
    owner.result.maximum_window_grant_wire_bytes = std::max(owner.result.maximum_window_grant_wire_bytes, wire_granted);
    owner.result.maximum_window_budget_excess_bytes = std::max(owner.result.maximum_window_budget_excess_bytes,
        wire_granted > accrued ? wire_granted - accrued : 0);
    if (wire_granted > accrued + initial_credit) throw std::logic_error("TM1 quota overspend");
    if (std::any_of(owner.flows.begin(), owner.flows.end(), [](const Flow& flow) { return flow.remaining != 0; })) {
        if (index > UINT64_C(10000000)) throw std::logic_error("TM1 calendar did not make progress");
        eventlist().sourceIsPending(*this, end);
    }
}
}

NsTm1ExperimentResult runNsTm1Experiment(const NsTm1ExperimentConfig& config) {
    Experiment experiment(config);
    return experiment.run();
}

std::string nsTm1CsvHeader() {
    return "policy,source,receiver,routing,mtu,window_us,tick_ns,fraction,seed,payload_per_sender,rate_scale,internal_overhead,xpe_mask,unavailable_cells,admission,generated_packets,generated_payload_bytes,generated_wire_bytes,delivered_packets,delivered_payload_bytes,delivered_wire_bytes,dropped_packets,dropped_payload_bytes,pending_packets,source_drops,switch_drops,source_max_bytes,data_max_cells,xpe_max_cells,completion_ps,latency_mean_ps,latency_p99_ps,latency_max_ps,queue_wait_max_ps,allocated_wire_bps,receiver_capacity_bps,total_windows,max_window_grant_wire_bytes,max_window_budget_excess_bytes,max_deficit_carry_bytes,receiver_floor_ps,conservation,allocation_constraints,complete_payload,requested_payload_bytes,expected_wire_bytes,quota_overspend_bytes,makespan_ps,floor_ps,receiver_lane0_wire_bytes,receiver_lane1_wire_bytes,receiver_lane2_wire_bytes,receiver_lane3_wire_bytes,lane0_max_queue_cells,lane1_max_queue_cells,lane2_max_queue_cells,lane3_max_queue_cells,buffered_residence_max_ps";
}

std::string nsTm1CsvRow(const NsTm1ExperimentConfig& c, const NsTm1ExperimentResult& r) {
    std::ostringstream out;
    out << c.policy << ',' << c.source << ',' << c.receiver << ',' << c.routing << ',' << c.mtu << ','
        << c.window_ps / 1000000 << ',' << c.switch_config.tick_ps / 1000 << ','
        << double(c.fraction_ppm) / 1000000 << ',' << c.seed << ',' << c.payload_bytes << ','
        << double(c.rate_scale_ppm) / 1000000 << ',' << c.switch_config.internal_overhead_bytes << ','
        << unsigned(c.xpe_mask) << ',' << c.switch_config.unavailable_cells[0] << ','
        << (c.switch_config.admission == NsTm1Admission::Static ? "static" : "dynamic");
    out << ',' << r.generated_packets << ',' << r.generated_payload_bytes << ',' << r.generated_wire_bytes
        << ',' << r.delivered_packets << ',' << r.delivered_payload_bytes << ',' << r.delivered_wire_bytes
        << ',' << r.dropped_packets << ',' << r.dropped_payload_bytes << ',' << r.pending_packets
        << ',' << r.source_drops << ',' << r.switch_drops << ',' << r.source_max_bytes << ',' << r.data_max_cells
        << ',' << r.xpe_max_cells << ',' << r.completion_ps << ',' << r.latency_mean_ps << ',' << r.latency_p99_ps
        << ',' << r.latency_max_ps << ',' << r.queue_wait_max_ps << ',' << r.allocated_wire_bps
        << ',' << r.receiver_capacity_bps << ',' << r.total_windows << ',' << r.maximum_window_grant_wire_bytes
        << ',' << r.maximum_window_budget_excess_bytes << ',' << r.maximum_deficit_carry_bytes
        << ',' << r.minimum_receiver_floor_ps << ',' << r.conservation << ',' << r.allocation_constraints
        << ',' << r.complete_payload << ',' << r.requested_payload_bytes << ',' << r.expected_wire_bytes
        << ',' << r.quota_overspend_bytes << ',' << r.completion_ps << ',' << r.minimum_receiver_floor_ps;
    for (const auto bytes : r.receiver_lane_wire_bytes) out << ',' << bytes;
    for (const auto cells : r.receiver_lane_max_queue_cells) out << ',' << cells;
    out << ',' << r.buffered_residence_max_ps;
    return out.str();
}
