// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>
#include <limits>

#include "rnic_breakout_topology.h"

namespace {
class Sink final : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        ids.push_back(packet.id());
        times.push_back(EventList::now());
    }
    const string& nodename() override { return name; }
    string name{"breakout_test_sink"};
    std::vector<packetid_t> ids;
    std::vector<std::uint64_t> times;
};

class Frame final : public Packet {
public:
    Frame(PacketFlow& flow, const Route& route, packetid_t id,
          std::uint32_t bytes, PktPriority priority = PRIO_LO) : prio(priority) {
        set_route(flow, route, static_cast<int>(bytes), id);
    }
    PktPriority priority() const override { return prio; }
    void free() override { dropped = true; }
    PktPriority prio;
    bool dropped{false};
};

std::uint64_t wireBudgetCharge(const Packet& p) { return p.size(); }
void drain() { while (EventList::doNextEvent()) {} }
RnicBreakoutTopologyConfig config(std::uint64_t queue = 65536) {
    return {3, 4, 25000000000ULL, 65536, queue, 100000, 200000, 100000};
}

TEST(RnicBreakoutTopologyTest, IndependentLanesMatchExactPhysicalSerialization) {
    RnicBreakoutTopology topology(EventList::getTheEventList(), config(), wireBudgetCharge);
    Sink sink;
    PacketFlow flow(nullptr);
    std::vector<std::unique_ptr<Frame>> packets;
    const auto start = EventList::now();
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
        packets.emplace_back(std::make_unique<Frame>(
            flow, topology.route(0, 1, lane, lane, sink), lane, 1538));
        packets.back()->sendOn();
    }
    drain();
    ASSERT_EQ(sink.times.size(), 4U);
    for (const auto time : sink.times) EXPECT_EQ(time - start, 1384320U);
    EXPECT_EQ(topology.noQueueTransitPs(1538), 1384320U);
    EXPECT_EQ(topology.noQueueTransitPs(1538, true), 892160U);
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
        EXPECT_EQ(topology.sourceQueue(0, lane).highWatermarkChargedBytes(), 1538U);
        EXPECT_EQ(topology.egressQueue(1, lane).highWatermarkChargedBytes(), 1538U);
    }
}

TEST(RnicBreakoutTopologyTest, SuffixOmitsExactlyOneSourceSerializer) {
    RnicBreakoutTopology topology(EventList::getTheEventList(), config(), wireBudgetCharge);
    Sink sink;
    PacketFlow flow(nullptr);
    const auto start = EventList::now();
    Frame packet(flow, topology.route(0, 1, 0, 0, sink, true), 1, 1538);
    packet.sendOn();
    drain();
    ASSERT_EQ(sink.times.size(), 1U);
    EXPECT_EQ(sink.times.front() - start, topology.noQueueTransitPs(1538, true));
    EXPECT_EQ(topology.sourceQueue(0, 0).highWatermarkChargedBytes(), 0U);
}

TEST(RnicBreakoutTopologyTest, CollidedHashDropsWholeFramesAtFiniteEgress) {
    std::vector<packetid_t> dropped;
    RnicBreakoutTopology topology(EventList::getTheEventList(), config(1538), wireBudgetCharge,
        [&](const Packet& packet, std::uint64_t) { dropped.push_back(packet.id()); });
    Sink sink;
    PacketFlow flow(nullptr);
    std::vector<std::unique_ptr<Frame>> frames;
    const RnicBreakoutTopology::LaneHash collapsed =
        [](std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t) { return 0; };
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
        frames.emplace_back(std::make_unique<Frame>(flow,
            topology.hashedRoute(0, 1, lane, 1000 + lane, {0, 1, 2, 3}, collapsed, sink), lane, 1538));
        frames.back()->sendOn();
    }
    drain();
    EXPECT_EQ(sink.ids, std::vector<packetid_t>({0}));
    EXPECT_EQ(dropped, std::vector<packetid_t>({1, 2, 3}));
    EXPECT_EQ(topology.egressQueue(1, 0).droppedPackets(), 3U);
    EXPECT_EQ(topology.egressQueue(1, 0).highWatermarkChargedBytes(), 1538U);
}

TEST(RnicBreakoutTopologyTest, SourcePortHashCanSpreadButNeverPromisesDiversity) {
    RnicBreakoutTopology topology(EventList::getTheEventList(), config(), wireBudgetCharge);
    Sink sink;
    const RnicBreakoutTopology::LaneHash mapping =
        [](std::uint32_t, std::uint32_t, std::uint16_t port, std::uint32_t lanes) {
            return port % lanes;
        };
    const auto& constant = topology.hashedRoute(0, 1, 0, 1000, {0, 1, 2, 3}, mapping, sink);
    EXPECT_EQ(&constant, &topology.hashedRoute(0, 1, 0, 1000, {0, 1, 2, 3}, mapping, sink));
    EXPECT_EQ(&constant, &topology.route(0, 1, 0, 0, sink));
    EXPECT_NE(&constant, &topology.hashedRoute(0, 1, 0, 1001, {0, 1, 2, 3}, mapping, sink));
    EXPECT_EQ(&topology.hashedRoute(0, 1, 0, 1000, {0}, mapping, sink),
              &topology.hashedRoute(0, 1, 0, 1001, {0}, mapping, sink));
    EXPECT_EQ(&topology.hashedRoute(0, 1, 0, 1001, {1, 3}, mapping, sink),
              &topology.route(0, 1, 0, 3, sink));
    EXPECT_THROW(topology.hashedRoute(0, 1, 0, 1, {}, mapping, sink), std::invalid_argument);
    Sink other;
    EXPECT_THROW(topology.route(2, 1, 0, 0, other), std::invalid_argument);
    EXPECT_THROW(topology.hashedRoute(0, 1, 0, 1, {0}, {}, sink), std::invalid_argument);
    const RnicBreakoutTopology::LaneHash invalid =
        [](std::uint32_t, std::uint32_t, std::uint16_t, std::uint32_t n) { return n; };
    EXPECT_THROW(topology.hashedRoute(0, 1, 0, 1, {0}, invalid, sink), std::out_of_range);
}

RnicBreakoutTopologyConfig closConfig(std::uint64_t rate = 25000000000ULL,
                                      std::uint64_t capacity = 262144) {
    return {12, 4, rate, 65536, capacity, 100000, 200000, 100000, 4, 4, 100000};
}

TEST(RnicBreakoutTopologyTest, RejectsOverflowingInterSwitchDelayBeforeAllocation) {
    auto cfg = closConfig();
    cfg.inter_switch_propagation_ps = std::numeric_limits<std::uint64_t>::max();
    EXPECT_THROW(RnicBreakoutTopology(EventList::getTheEventList(), cfg, wireBudgetCharge),
                 std::overflow_error);
}

TEST(RnicBreakoutTopologyTest, ClosUnloadedPathsHaveExactlyFourSerializers) {
    for (const auto rate : {10000000000ULL, 25000000000ULL}) {
        RnicBreakoutTopology topology(EventList::getTheEventList(), closConfig(rate), wireBudgetCharge);
        Sink local, remote, suffix;
        PacketFlow flow(nullptr);
        auto start = EventList::now();
        Frame a(flow, topology.route(0, 1, 0, 0, local), 1, 1538);
        a.sendOn(); drain();
        const auto leaf = rate == 10000000000ULL ? 2860800ULL : 1384320ULL;
        ASSERT_EQ(local.times.size(), 1U);
        EXPECT_EQ(local.times.front() - start, leaf);
        EXPECT_EQ(topology.noQueueTransitPs(1538, 0, 1), leaf);
        start = EventList::now();
        Frame b(flow, topology.route(0, 4, 0, 0, remote, false, 2), 2, 1538);
        b.sendOn(); drain();
        const auto cross = rate == 10000000000ULL ? 5921600ULL : 2968640ULL;
        ASSERT_EQ(remote.times.size(), 1U);
        EXPECT_EQ(remote.times.front() - start, cross);
        EXPECT_EQ(topology.noQueueTransitPs(1538, 0, 4), cross);
        start = EventList::now();
        Frame c(flow, topology.route(0, 8, 0, 0, suffix, true, 1), 3, 1538);
        c.sendOn(); drain();
        const auto serialization = rate == 10000000000ULL ? 1230400ULL : 492160ULL;
        ASSERT_EQ(suffix.times.size(), 1U);
        EXPECT_EQ(suffix.times.front() - start, cross - serialization);
    }
}

TEST(RnicBreakoutTopologyTest, ClosSameLeafPreservesLoadedLegacyTimestamps) {
    std::vector<std::vector<std::uint64_t>> times;
    for (const bool clos : {false, true}) {
        auto cfg = closConfig();
        if (!clos) { cfg.endpoints_per_leaf = 0; cfg.spines = 0; }
        RnicBreakoutTopology topology(EventList::getTheEventList(), cfg, wireBudgetCharge);
        Sink sink;
        PacketFlow flow(nullptr);
        std::vector<std::unique_ptr<Frame>> packets;
        const auto start = EventList::now();
        for (std::uint32_t id = 0; id < 8; ++id) {
            packets.emplace_back(std::make_unique<Frame>(flow,
                topology.route(id % 2, 2, id % 4, 0, sink, false, clos ? id % 4 : 0), id, 1538));
            packets.back()->sendOn();
        }
        drain();
        std::vector<std::uint64_t> relative;
        for (const auto time : sink.times) relative.push_back(time - start);
        ASSERT_EQ(relative.size(), 8U);
        times.push_back(relative);
        if (clos) {
            for (std::uint32_t spine = 0; spine < 4; ++spine) {
                EXPECT_EQ(topology.leafUplinkQueue(0, spine, 0).highWatermarkChargedBytes(), 0U);
                EXPECT_EQ(&topology.route(0, 2, 0, 0, sink, false, spine),
                          &topology.route(0, 2, 0, 0, sink));
            }
        }
    }
    EXPECT_EQ(times[0], times[1]);
}

TEST(RnicBreakoutTopologyTest, ClosUplinkIsSharedAcrossSourceEndpoints) {
    RnicBreakoutTopology topology(EventList::getTheEventList(), closConfig(), wireBudgetCharge);
    Sink a, b;
    PacketFlow flow(nullptr);
    const auto start = EventList::now();
    Frame first(flow, topology.route(0, 4, 0, 0, a), 1, 1538);
    Frame second(flow, topology.route(1, 5, 0, 0, b), 2, 1538);
    first.sendOn(); second.sendOn(); drain();
    ASSERT_EQ(a.times.size(), 1U); ASSERT_EQ(b.times.size(), 1U);
    EXPECT_EQ(a.times.front() - start, 2968640U);
    EXPECT_EQ(b.times.front() - start, 3460800U);
    EXPECT_EQ(topology.leafUplinkQueue(0, 0, 0).highWatermarkChargedBytes(), 3076U);
}

TEST(RnicBreakoutTopologyTest, ClosSpineDownlinkIsSharedAcrossSourceLeaves) {
    RnicBreakoutTopology topology(EventList::getTheEventList(), closConfig(), wireBudgetCharge);
    Sink a, b;
    PacketFlow flow(nullptr);
    const auto start = EventList::now();
    Frame first(flow, topology.route(0, 8, 0, 0, a), 1, 1538);
    Frame second(flow, topology.route(4, 9, 0, 0, b), 2, 1538);
    first.sendOn(); second.sendOn(); drain();
    ASSERT_EQ(a.times.size(), 1U); ASSERT_EQ(b.times.size(), 1U);
    EXPECT_EQ(a.times.front() - start, 2968640U);
    EXPECT_EQ(b.times.front() - start, 3460800U);
    EXPECT_EQ(topology.spineEgressQueue(0, 2, 0).highWatermarkChargedBytes(), 3076U);
    EXPECT_EQ(topology.leafUplinkQueue(0, 0, 0).highWatermarkChargedBytes(), 1538U);
    EXPECT_EQ(topology.leafUplinkQueue(1, 0, 0).highWatermarkChargedBytes(), 1538U);
}

TEST(RnicBreakoutTopologyTest, ClosIndependentSpinesConvergeAtDestination) {
    RnicBreakoutTopology topology(EventList::getTheEventList(), closConfig(), wireBudgetCharge);
    Sink a, b, same;
    PacketFlow flow(nullptr);
    auto start = EventList::now();
    Frame first(flow, topology.route(0, 4, 0, 0, a, false, 0), 1, 1538);
    Frame second(flow, topology.route(1, 5, 0, 0, b, false, 1), 2, 1538);
    first.sendOn(); second.sendOn(); drain();
    ASSERT_EQ(a.times.size(), 1U); ASSERT_EQ(b.times.size(), 1U);
    EXPECT_EQ(a.times.front() - start, 2968640U);
    EXPECT_EQ(b.times.front() - start, 2968640U);
    start = EventList::now();
    Frame third(flow, topology.route(0, 8, 0, 0, same, false, 0), 3, 1538);
    Frame fourth(flow, topology.route(1, 8, 0, 0, same, false, 1), 4, 1538);
    third.sendOn(); fourth.sendOn(); drain();
    EXPECT_EQ(same.times, std::vector<std::uint64_t>({start + 2968640, start + 3460800}));
    EXPECT_EQ(topology.egressQueue(8, 0).highWatermarkChargedBytes(), 3076U);
}

TEST(RnicBreakoutTopologyTest, ClosActiveFrameConsumesStoredUplinkCapacity) {
    std::vector<packetid_t> drops;
    auto charge = [](const Packet& packet) { return packet.size() - 20; };
    RnicBreakoutTopology topology(EventList::getTheEventList(), closConfig(25000000000ULL, 1518), charge,
        [&](const Packet& packet, std::uint64_t) { drops.push_back(packet.id()); });
    Sink a, b;
    PacketFlow flow(nullptr);
    Frame first(flow, topology.route(0, 4, 0, 0, a), 1, 1538);
    Frame second(flow, topology.route(1, 5, 0, 0, b), 2, 1538);
    first.sendOn(); second.sendOn(); drain();
    EXPECT_EQ(a.ids, std::vector<packetid_t>({1})); EXPECT_TRUE(b.ids.empty());
    EXPECT_EQ(drops, std::vector<packetid_t>({2}));
    EXPECT_EQ(topology.leafUplinkQueue(0, 0, 0).highWatermarkChargedBytes(), 1518U);
    EXPECT_EQ(topology.leafUplinkQueue(0, 0, 0).droppedPackets(), 1U);
}

TEST(RnicBreakoutTopologyTest, ClosValidatesExplicitSpineAndLeafIdentities) {
    auto cfg = closConfig();
    cfg.spines = 0;
    EXPECT_THROW(RnicBreakoutTopology(EventList::getTheEventList(), cfg, wireBudgetCharge),
                 std::invalid_argument);
    RnicBreakoutTopology topology(EventList::getTheEventList(), closConfig(), wireBudgetCharge);
    Sink sink;
    EXPECT_THROW(topology.route(0, 4, 0, 0, sink, false, 4), std::out_of_range);
    EXPECT_THROW(topology.leafUplinkQueue(3, 0, 0), std::out_of_range);
    EXPECT_THROW(topology.spineEgressQueue(0, 0, 4), std::out_of_range);
    EXPECT_THROW(topology.crossesLeaves(12, 0), std::out_of_range);
}

TEST(RnicFinitePriorityQueueTest, ControlWaitsForCurrentFrameThenPassesQueuedData) {
    RnicFinitePriorityQueue queue(EventList::getTheEventList(), 25000000000ULL, 4096, wireBudgetCharge);
    Sink sink;
    Route route;
    route.push_back(&queue);
    route.push_back(&sink);
    PacketFlow flow(nullptr);
    const auto start = EventList::now();
    Frame first(flow, route, 1, 1538);
    Frame second(flow, route, 2, 1538);
    Frame control(flow, route, 3, 128, Packet::PRIO_HI);
    first.sendOn(); second.sendOn(); control.sendOn();
    drain();
    EXPECT_EQ(sink.ids, std::vector<packetid_t>({1, 3, 2}));
    EXPECT_EQ(sink.times, std::vector<std::uint64_t>({start+492160, start+533120, start+1025280}));
    EXPECT_EQ(queue.highWatermarkChargedBytes(), 3204U);
    EXPECT_EQ(queue.occupancyChargedBytes(), 0U);
}

TEST(RnicFinitePriorityQueueTest, StoredAdmissionAndWireServiceRemainIndependent) {
    auto storedCharge = [](const Packet& packet) { return packet.size() - 20; };
    RnicFinitePriorityQueue queue(EventList::getTheEventList(), 25000000000ULL, 1626, storedCharge);
    Sink sink;
    Route route;
    route.push_back(&queue); route.push_back(&sink);
    PacketFlow flow(nullptr);
    const auto start = EventList::now();
    Frame data(flow, route, 1, 1538);
    Frame control(flow, route, 2, 128, Packet::PRIO_HI);
    Frame overflow(flow, route, 3, 1538);
    data.sendOn(); control.sendOn(); overflow.sendOn();
    EXPECT_TRUE(overflow.dropped);
    EXPECT_EQ(queue.highWatermarkChargedBytes(), 1626U);
    drain();
    EXPECT_EQ(sink.times, std::vector<std::uint64_t>({start+492160, start+533120}));
    EXPECT_EQ(queue.droppedPackets(), 1U);
    EXPECT_THROW(RnicFinitePriorityQueue(EventList::getTheEventList(), 1, 1, {}), std::invalid_argument);
}

TEST(RnicFinitePriorityQueueTest, RationalWireTimeDoesNotAccumulateRoundingTicks) {
    RnicFinitePriorityQueue queue(EventList::getTheEventList(), 7000000000ULL, 100, wireBudgetCharge);
    Sink sink;
    Route route;
    route.push_back(&queue); route.push_back(&sink);
    PacketFlow flow(nullptr);
    std::vector<std::unique_ptr<Frame>> frames;
    const auto start = EventList::now();
    for (std::uint32_t i = 0; i < 7; ++i) {
        frames.emplace_back(std::make_unique<Frame>(flow, route, i, 1));
        frames.back()->sendOn();
    }
    drain();
    ASSERT_EQ(sink.times.size(), 7U);
    EXPECT_EQ(sink.times.back() - start, 8000U);
    EXPECT_THROW(RnicFinitePriorityQueue(EventList::getTheEventList(), 1, 0, wireBudgetCharge), std::invalid_argument);
}

TEST(RnicFinitePriorityQueueTest, ContinuingControlsInvalidateCapacityOnlyDataDeadline) {
    class Arrival final : public EventSource {
    public:
        explicit Arrival(Frame& packet) : EventSource(EventList::getTheEventList(), "arrival"), packet(packet) {}
        void doNextEvent() override { packet.sendOn(); }
        Frame& packet;
    };
    RnicFinitePriorityQueue queue(EventList::getTheEventList(), 25000000000ULL, 1794, wireBudgetCharge);
    Sink sink;
    Route route;
    route.push_back(&queue); route.push_back(&sink);
    PacketFlow flow(nullptr);
    std::vector<std::unique_ptr<Frame>> controls;
    std::vector<std::unique_ptr<Arrival>> arrivals;
    const auto start = EventList::now();
    controls.emplace_back(std::make_unique<Frame>(flow, route, 1, 128, Packet::PRIO_HI));
    Frame data(flow, route, 100, 1538);
    controls.front()->sendOn(); data.sendOn();
    for (std::uint32_t i = 1; i < 32; ++i) {
        controls.emplace_back(std::make_unique<Frame>(flow, route, i+1, 128, Packet::PRIO_HI));
        arrivals.emplace_back(std::make_unique<Arrival>(*controls.back()));
        EventList::sourceIsPending(*arrivals.back(), start+i*40960-1);
    }
    drain();
    ASSERT_EQ(sink.ids.size(), 33U);
    EXPECT_EQ(sink.ids.back(), 100U);
    EXPECT_EQ(sink.times.back()-start, 1802880U);
    EXPECT_GT(sink.times.back()-start, 1794U*320U);
    EXPECT_EQ(queue.droppedPackets(), 0U);
    EXPECT_EQ(queue.highWatermarkChargedBytes(), 1794U);
}
}
