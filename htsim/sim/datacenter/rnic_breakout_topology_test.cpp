// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

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
