// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_tm1_switch.h"
#include "ns_tm1_experiment.h"
#include "rnic_breakout_topology.h"
#include "pipe.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace {
class TestPacket final : public Packet {
public:
    TestPacket(PacketFlow& flow, const Route& route, uint32_t id, uint16_t wire,
               uint64_t frame, PktPriority priority = PRIO_LO)
        : frame(frame), prio(priority) { set_route(flow, route, wire, id); }
    void free() override { dropped = true; }
    PktPriority priority() const override { return prio; }
    uint64_t frame;
    bool dropped{false};
    PktPriority prio;
};
class Sink final : public PacketSink {
public:
    void receivePacket(Packet& p) override { ids.push_back(p.id()); times.push_back(EventList::now()); }
    const string& nodename() override { return name; }
    std::vector<uint32_t> ids;
    std::vector<uint64_t> times;
    string name{"tm1-test-sink"};
};
struct Harness {
    explicit Harness(NsTm1Config c = {}) : events(EventList::getTheEventList()),
        base(EventList::now()), model(events, c,
        [](const Packet& p) { return static_cast<const TestPacket&>(p).frame; }) {
        EventList::setEndtime(0);
    }
    Route& route(uint8_t mask = 1, uint64_t bps = 8000000000) {
        const auto egress = model.addEgress(bps, mask);
        auto r = std::make_unique<Route>();
        r->push_back(&model.ingress(egress, egress)); r->push_back(&sink);
        routes.push_back(std::move(r)); return *routes.back();
    }
    TestPacket& packet(Route& route, uint16_t wire, uint64_t frame,
                       Packet::PktPriority priority = Packet::PRIO_LO) {
        packets.emplace_back(std::make_unique<TestPacket>(flow, route, packets.size() + 1,
            wire, frame, priority));
        return *packets.back();
    }
    void drain() { while (EventList::doNextEvent()) {} }
    EventList& events;
    uint64_t base;
    PacketFlow flow{nullptr};
    Sink sink;
    NsTm1Switch model;
    std::vector<std::unique_ptr<Route>> routes;
    std::vector<std::unique_ptr<TestPacket>> packets;
};
NsTm1Config immediate() { NsTm1Config c; c.processing_ps = 0; c.tick_ps = 1; return c; }
}

TEST(NsTm1Cells, BoundariesAndWireStorageDistinction) {
    EXPECT_EQ(NsTm1Switch::cellsForFrame(208), 1u);
    EXPECT_EQ(NsTm1Switch::cellsForFrame(209), 2u);
    EXPECT_EQ(NsTm1Switch::cellsForFrame(1518), 8u);
    EXPECT_EQ(NsTm1Switch::cellsForFrame(9018), 44u);
    EXPECT_EQ(NsTm1Switch::cellsForFrame(208, 64), 2u);
    EXPECT_EQ(262144 / NsTm1Config::cell_bytes, 1260u);
    EXPECT_THROW(NsTm1Switch::cellsForFrame(0), std::invalid_argument);
}

TEST(NsTm1Cells, LaunchQuantizationNeverEarly) {
    EXPECT_EQ(NsTm1Switch::roundLaunch(8000, 8000), 8000u);
    EXPECT_EQ(NsTm1Switch::roundLaunch(8001, 8000), 16000u);
    EXPECT_EQ(NsTm1Switch::roundLaunch(450000, 8000), 456000u);
    EXPECT_THROW(NsTm1Switch::roundLaunch(1, 0), std::invalid_argument);
}

TEST(NsTm1Buffer, StaticQueueFloorIncludesActivePacket) {
    auto c = immediate(); c.processing_ps = 1000000;
    Harness h(c); auto& r = h.route();
    for (int i = 0; i < 158; ++i) h.packet(r, 1538, 1518).sendOn();
    EXPECT_EQ(h.model.counters().admitted_packets, 157u);
    EXPECT_EQ(h.model.counters().queue_drops, 1u);
    EXPECT_EQ(h.model.statistics(0).occupancy_cells[2], 1256u);
    h.drain();
    EXPECT_EQ(h.sink.ids.size(), 157u);
    EXPECT_EQ(h.model.pendingPackets(), 0u);
    EXPECT_EQ(h.model.occupiedCells()[0], 0u);
}

TEST(NsTm1Buffer, DataAndControlCapsAreIndependent) {
    auto c = immediate(); c.data_queue_bytes = 208; c.control_queue_bytes = 208;
    Harness h(c); auto& r = h.route();
    h.packet(r, 228, 208).sendOn();
    auto& excess = h.packet(r, 228, 208); excess.sendOn();
    h.packet(r, 228, 208, Packet::PRIO_HI).sendOn();
    auto& extra_control = h.packet(r, 228, 208, Packet::PRIO_MID); extra_control.sendOn();
    EXPECT_TRUE(excess.dropped);
    EXPECT_TRUE(extra_control.dropped);
    EXPECT_EQ(h.model.statistics(0).occupancy_cells[0], 1u);
    EXPECT_EQ(h.model.statistics(0).occupancy_cells[2], 1u);
    h.drain(); EXPECT_EQ(h.sink.ids, (std::vector<uint32_t>{1, 3}));
}

TEST(NsTm1Buffer, IndependentDomainsAndConservativeMasks) {
    auto c = immediate(); c.processing_ps = 1000000; c.advertised_cells.fill(1);
    Harness h(c); auto& a = h.route(1); auto& b = h.route(2); auto& both = h.route(3);
    h.packet(a, 228, 208).sendOn(); h.packet(b, 228, 208).sendOn();
    auto& denied = h.packet(both, 228, 208); denied.sendOn();
    EXPECT_TRUE(denied.dropped); EXPECT_EQ(h.model.counters().pool_drops, 1u);
    EXPECT_EQ(h.model.occupiedCells()[0], 1u); EXPECT_EQ(h.model.occupiedCells()[1], 1u);
    h.drain(); EXPECT_EQ(h.model.occupiedCells(), (std::array<uint64_t, 4>{{0, 0, 0, 0}}));
}

TEST(NsTm1Buffer, DynamicUsesProspectiveQueueAndFreeAfterAdmission) {
    auto c = immediate(); c.processing_ps = 1000000; c.advertised_cells.fill(10);
    c.admission = NsTm1Admission::Dynamic;
    Harness h(c); auto& r = h.route(3);
    h.packet(r, 620, 600).sendOn(); // 3 <= (10 - 3)/2.
    auto& denied = h.packet(r, 620, 600); denied.sendOn(); // 6 > (10 - 6)/2.
    EXPECT_TRUE(denied.dropped); EXPECT_EQ(h.model.counters().dynamic_drops, 1u);
    EXPECT_EQ(h.model.occupiedCells()[0], 3u); EXPECT_EQ(h.model.occupiedCells()[1], 3u);
    h.drain(); EXPECT_EQ(h.model.occupiedCells()[0], 0u);
}

TEST(NsTm1Service, HighCannotPreemptActiveLowAndPrecedesWaitingLow) {
    Harness h(immediate()); auto& r = h.route();
    h.packet(r, 1000, 980).sendOn();
    h.packet(r, 1000, 980).sendOn();
    h.packet(r, 100, 80, Packet::PRIO_HI).sendOn();
    h.drain();
    EXPECT_EQ(h.sink.ids, (std::vector<uint32_t>{1, 3, 2}));
    EXPECT_EQ(h.sink.times[0] - h.base, 1000000u);
    EXPECT_EQ(h.sink.times[1] - h.base, 1100000u);
    EXPECT_EQ(h.sink.times[2] - h.base, 2100000u);
}

TEST(NsTm1Service, IndependentEgressesOverlapAndWireDoesNotRoundToTick) {
    auto c = immediate(); c.tick_ps = 64000;
    Harness h(c); auto& a = h.route(1, 100000000000); auto& b = h.route(2, 100000000000);
    h.packet(a, 9038, 9018).sendOn(); h.packet(b, 9038, 9018).sendOn();
    h.drain();
    EXPECT_EQ(h.sink.times[0], h.sink.times[1]);
    const auto ready = NsTm1Switch::roundLaunch(h.base, c.tick_ps);
    EXPECT_EQ(h.sink.times[0] - ready, 723040u);
}

TEST(NsTm1Service, ExactUnloadedStoreForwardOracleAndHalfRate) {
    const auto run = [](uint64_t rate) {
        auto c = immediate(); c.processing_ps = 450000;
        Harness h(c); auto& egress = h.route(1, rate);
        RnicFinitePriorityQueue source(h.events, rate, 32768,
            [](const Packet& p) { return static_cast<const TestPacket&>(p).frame; });
        Pipe up(100000, h.events), down(100000, h.events);
        Route route; route.push_back(&source); route.push_back(&up);
        route.push_back(egress.at(0)); route.push_back(&down); route.push_back(&h.sink);
        h.packet(route, 9038, 9018).sendOn(); h.drain();
        return h.sink.times[0] - h.base;
    };
    EXPECT_EQ(run(25000000000), 6434320u);
    EXPECT_EQ(run(12500000000), 12218640u);
}

TEST(NsTm1Experiment, GlobalPacketizationAndBalancedLaneConservation) {
    NsTm1ExperimentConfig c; c.policy = "quota_dd";
    auto r = runNsTm1Experiment(c);
    EXPECT_EQ(r.generated_packets, 2924u); EXPECT_EQ(r.expected_wire_bytes, 4492552u);
    EXPECT_EQ(r.generated_wire_bytes, r.expected_wire_bytes); EXPECT_TRUE(r.complete_payload);
    EXPECT_TRUE(r.conservation); EXPECT_TRUE(r.allocation_constraints); EXPECT_EQ(r.quota_overspend_bytes, 0u);
    EXPECT_LE(r.data_max_cells, 1260u); EXPECT_GE(r.completion_ps, r.minimum_receiver_floor_ps);
    for (const auto bytes : r.receiver_lane_wire_bytes) EXPECT_EQ(bytes, 1123138u);
    c.mtu = 9000; c.source = "1x100";
    r = runNsTm1Experiment(c);
    EXPECT_EQ(r.generated_packets, 472u); EXPECT_EQ(r.expected_wire_bytes, 4242448u);
    EXPECT_TRUE(r.complete_payload); EXPECT_TRUE(r.conservation);
    for (const auto bytes : r.receiver_lane_wire_bytes) EXPECT_EQ(bytes, 1060612u);
}

TEST(NsTm1Experiment, QuotaPrbsReplaysAndUnpacedCollisionLosesWholePackets) {
    NsTm1ExperimentConfig c; c.policy = "quota_prbs"; c.payload_bytes = 65536;
    const auto a = runNsTm1Experiment(c), b = runNsTm1Experiment(c);
    EXPECT_EQ(a.completion_ps, b.completion_ps); EXPECT_EQ(a.generated_packets, b.generated_packets);
    EXPECT_TRUE(a.complete_payload); EXPECT_EQ(a.quota_overspend_bytes, 0u);
    c.policy = "unpaced"; c.routing = "collision"; c.payload_bytes = 1048576;
    const auto r = runNsTm1Experiment(c);
    EXPECT_GT(r.dropped_packets, 0u); EXPECT_FALSE(r.complete_payload);
    EXPECT_TRUE(r.conservation); EXPECT_EQ(r.pending_packets, 0u); EXPECT_LE(r.data_max_cells, 1260u);
}

TEST(NsTm1Experiment, RejectsRoundedZeroBudgetsBeforeScheduling) {
    NsTm1ExperimentConfig c;
    c.rate_scale_ppm = 1; c.fraction_ppm = 1;
    EXPECT_THROW(runNsTm1Experiment(c), std::invalid_argument);
    EXPECT_FALSE(EventList::nextEventTime().has_value());
    EXPECT_EQ(EventList::trafficEventCount(), 0);
    c.fraction_ppm = 40; // A 1 bps lane budget rounds each four-way share to zero.
    EXPECT_THROW(runNsTm1Experiment(c), std::invalid_argument);
    EXPECT_FALSE(EventList::nextEventTime().has_value());
    c.rate_scale_ppm = 0;
    EXPECT_THROW(runNsTm1Experiment(c), std::invalid_argument);
    c.rate_scale_ppm = 1000000; c.fraction_ppm = 0;
    EXPECT_THROW(runNsTm1Experiment(c), std::invalid_argument);
}

TEST(NsTm1Experiment, DispatchExceptionCancelsPropagationAndRetainedSourcePackets) {
    NsTm1ExperimentConfig c;
    c.policy = "unpaced";
    c.switch_config.internal_overhead_bytes = UINT64_MAX;
    EXPECT_THROW(runNsTm1Experiment(c), std::overflow_error);
    EXPECT_FALSE(EventList::nextEventTime().has_value());
    EXPECT_EQ(EventList::trafficEventCount(), 0);
    c.switch_config.internal_overhead_bytes = 0; c.payload_bytes = 1024;
    EXPECT_TRUE(runNsTm1Experiment(c).complete_payload);
}

TEST(NsTm1Experiment, DispatchExceptionPreservesActiveSwitchPacketOwnership) {
    NsTm1ExperimentConfig c;
    c.policy = "unpaced"; c.payload_bytes = c.mtu - 64 + 1;
    // A short striped tail reaches and occupies the switch before a full
    // frame overflows the configured accounting extent. No huge allocation
    // is made: the capacities only permit this deliberately extreme charge.
    c.switch_config.internal_overhead_bytes = UINT64_MAX - 100;
    c.switch_config.data_queue_bytes = UINT64_MAX;
    c.switch_config.advertised_cells.fill(UINT64_MAX);
    EXPECT_THROW(runNsTm1Experiment(c), std::overflow_error);
    EXPECT_FALSE(EventList::nextEventTime().has_value());
    EXPECT_EQ(EventList::trafficEventCount(), 0);
    c.switch_config = NsTm1Config{}; c.payload_bytes = 1024;
    EXPECT_TRUE(runNsTm1Experiment(c).complete_payload);
}
