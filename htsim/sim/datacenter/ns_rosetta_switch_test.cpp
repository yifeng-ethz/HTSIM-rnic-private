// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fat_tree_switch_factory.h"
#include "fat_tree_topology.h"
#include "eth_pause_packet.h"
#include "ns_rosetta_switch.h"

namespace {

constexpr linkspeed_bps kLinkSpeed = 8000000000ULL;
constexpr uint16_t kPacketBytes = 100;
constexpr mem_b kDefaultSharedBuffer = 4096;

class TestPacket : public Packet {
public:
    TestPacket(PacketFlow& flow, const Route& route, packetid_t packet_id,
               PktPriority priority, uint32_t source, uint32_t destination,
               uint16_t bytes = kPacketBytes)
        : _priority(priority) {
        set_route(flow, route, bytes, packet_id);
        set_src(source);
        set_dst(destination);
    }

    PktPriority priority() const override { return _priority; }
    void free() override { dropped = true; }

    bool dropped{false};

private:
    PktPriority _priority;
};

class RecordingSink : public PacketSink {
public:
    struct Arrival {
        packetid_t packet_id;
        simtime_picosec time;
    };

    void receivePacket(Packet& pkt) override {
        arrivals.push_back({pkt.id(), EventList::now()});
    }
    const string& nodename() override { return _name; }

    simtime_picosec arrival_time(packetid_t packet_id) const {
        auto arrival = std::find_if(
            arrivals.begin(), arrivals.end(),
            [packet_id](const Arrival& value) {
                return value.packet_id == packet_id;
            });
        if (arrival == arrivals.end()) {
            throw std::out_of_range("packet was not delivered");
        }
        return arrival->time;
    }

    std::vector<Arrival> arrivals;

private:
    string _name{"RecordingSink"};
};

class RecordingBacklogObserver final
    : public NsRosettaBacklogObserver {
public:
    RecordingBacklogObserver() { observations.reserve(32); }

    void observe(
        const NsRosettaBacklogObservation& observation) noexcept override {
        observations.push_back(observation);
    }

    std::vector<NsRosettaBacklogObservation> observations;
};

class CallbackEvent final : public EventSource {
public:
    CallbackEvent(EventList& eventlist, simtime_picosec delay,
                  std::function<void()> callback)
        : EventSource(eventlist, "ns-rosetta-test-callback"),
          _callback(std::move(callback)) {
        eventlist.sourceIsPendingRel(*this, delay);
    }

    void doNextEvent() override { _callback(); }

private:
    std::function<void()> _callback;
};

class NsRosettaHarness {
public:
    explicit NsRosettaHarness(mem_b shared_buffer_capacity =
                                  kDefaultSharedBuffer,
                              FatTreeSwitch::switch_type type =
                                  FatTreeSwitch::TOR,
                              simtime_picosec switch_delay = 0)
        : eventlist(EventList::getTheEventList()),
          switch_owner(FatTreeSwitchFactory::create(
              FatTreeSwitchModel::NsRosetta, eventlist,
              "ns-rosetta-test", type, 0, switch_delay, nullptr,
              shared_buffer_capacity)),
          traffic_manager(dynamic_cast<NsRosetta*>(switch_owner.get())) {
        if (traffic_manager == nullptr) {
            throw std::logic_error(
                "factory did not construct ns-rosetta switch");
        }
    }

    NsRosettaEgressSerializer& add_egress(
        QueueLogger* logger = nullptr) {
        auto serializer = std::make_unique<NsRosettaEgressSerializer>(
            kLinkSpeed, eventlist, logger);
        NsRosettaEgressSerializer* result = serializer.get();
        serializers.push_back(std::move(serializer));
        traffic_manager->addPort(result);
        return *result;
    }

    NsRosettaIngressPort& add_ingress(const string& name) {
        auto* ingress = dynamic_cast<NsRosettaIngressPort*>(
            traffic_manager->create_physical_ingress(name));
        if (ingress == nullptr) {
            throw std::logic_error(
                "ns-rosetta ingress adapter was not created");
        }
        return *ingress;
    }

    static void drain_all_events() {
        while (EventList::doNextEvent()) {
        }
    }

    EventList& eventlist;
    std::vector<std::unique_ptr<NsRosettaEgressSerializer>> serializers;
    std::unique_ptr<FatTreeSwitch> switch_owner;
    NsRosetta* traffic_manager;
};

Route route_via(NsRosettaEgressSerializer& serializer,
                RecordingSink& sink) {
    Route route;
    route.push_back(&serializer);
    route.push_back(&sink);
    return route;
}

std::vector<packetid_t> arrival_order(const RecordingSink& sink) {
    std::vector<packetid_t> result;
    for (const RecordingSink::Arrival& arrival : sink.arrivals) {
        result.push_back(arrival.packet_id);
    }
    return result;
}

TEST(NsRosettaSwitchTest,
     RequestGrantPreventsOneIngressFromDrivingTwoEgressesAtOnce) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& egress_0 = harness.add_egress();
    NsRosettaEgressSerializer& egress_1 = harness.add_egress();
    NsRosettaIngressPort& ingress_0 = harness.add_ingress("ingress-0");
    NsRosettaIngressPort& ingress_1 = harness.add_ingress("ingress-1");
    RecordingSink sink;
    Route route_0 = route_via(egress_0, sink);
    Route route_1 = route_via(egress_1, sink);
    PacketFlow flow(nullptr);
    TestPacket ingress_0_first(flow, route_0, 1, Packet::PRIO_LO, 0, 10);
    TestPacket ingress_0_second(flow, route_1, 2, Packet::PRIO_LO, 0, 11);
    TestPacket independent_ingress(flow, route_1, 3, Packet::PRIO_LO, 1,
                                   11);

    const simtime_picosec start_time = EventList::now();
    ingress_0.receivePacket(ingress_0_first);
    ingress_0.receivePacket(ingress_0_second);
    ingress_1.receivePacket(independent_ingress);
    NsRosettaHarness::drain_all_events();

    EXPECT_EQ(sink.arrival_time(1), start_time + timeFromNs(100));
    EXPECT_EQ(sink.arrival_time(3), start_time + timeFromNs(100));
    EXPECT_EQ(sink.arrival_time(2), start_time + timeFromNs(200));
}

TEST(NsRosettaSwitchTest, PerEgressVoqsAvoidHeadOfLineBlocking) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& blocked_egress = harness.add_egress();
    NsRosettaEgressSerializer& free_egress = harness.add_egress();
    NsRosettaIngressPort& blocker_ingress =
        harness.add_ingress("blocker-ingress");
    NsRosettaIngressPort& tested_ingress =
        harness.add_ingress("tested-ingress");
    RecordingSink sink;
    Route blocked_route = route_via(blocked_egress, sink);
    Route free_route = route_via(free_egress, sink);
    PacketFlow flow(nullptr);
    TestPacket blocker(flow, blocked_route, 1, Packet::PRIO_LO, 0, 8, 200);
    TestPacket queued_behind_blocker(flow, blocked_route, 2,
                                     Packet::PRIO_LO, 1, 8);
    TestPacket escapes_hol(flow, free_route, 3, Packet::PRIO_LO, 1, 9);

    blocker_ingress.receivePacket(blocker);
    tested_ingress.receivePacket(queued_behind_blocker);
    tested_ingress.receivePacket(escapes_hol);
    NsRosettaHarness::drain_all_events();

    EXPECT_LT(sink.arrival_time(3), sink.arrival_time(2));
}

TEST(NsRosettaSwitchTest, AppliesStrictClassPriorityAtPacketBoundaries) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& egress = harness.add_egress();
    NsRosettaIngressPort& ingress_0 = harness.add_ingress("ingress-0");
    NsRosettaIngressPort& ingress_1 = harness.add_ingress("ingress-1");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket low_in_service(flow, route, 1, Packet::PRIO_LO, 0, 9);
    TestPacket low_waiting(flow, route, 2, Packet::PRIO_LO, 0, 9);
    TestPacket high_waiting(flow, route, 3, Packet::PRIO_HI, 1, 9);

    ingress_0.receivePacket(low_in_service);
    ingress_0.receivePacket(low_waiting);
    ingress_1.receivePacket(high_waiting);
    NsRosettaHarness::drain_all_events();

    EXPECT_EQ(arrival_order(sink),
              (std::vector<packetid_t>{1, 3, 2}));
}

TEST(NsRosettaSwitchTest, RoundRobinsIngressRequestsForOneEgress) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& egress = harness.add_egress();
    NsRosettaIngressPort& ingress_0 = harness.add_ingress("ingress-0");
    NsRosettaIngressPort& ingress_1 = harness.add_ingress("ingress-1");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket first_0(flow, route, 1, Packet::PRIO_LO, 0, 9);
    TestPacket second_0(flow, route, 2, Packet::PRIO_LO, 0, 9);
    TestPacket first_1(flow, route, 3, Packet::PRIO_LO, 1, 9);
    TestPacket second_1(flow, route, 4, Packet::PRIO_LO, 1, 9);

    ingress_0.receivePacket(first_0);
    ingress_0.receivePacket(second_0);
    ingress_1.receivePacket(first_1);
    ingress_1.receivePacket(second_1);
    NsRosettaHarness::drain_all_events();

    EXPECT_EQ(arrival_order(sink),
              (std::vector<packetid_t>{1, 3, 2, 4}));
}

TEST(NsRosettaSwitchTest, ExposesBoundedLocalRequestAndPathLoadSamples) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& egress_0 = harness.add_egress();
    NsRosettaEgressSerializer& egress_1 = harness.add_egress();
    NsRosettaIngressPort& ingress_0 = harness.add_ingress("ingress-0");
    NsRosettaIngressPort& ingress_1 = harness.add_ingress("ingress-1");
    RecordingSink sink;
    Route route_0 = route_via(egress_0, sink);
    Route route_1 = route_via(egress_1, sink);
    PacketFlow flow(nullptr);
    TestPacket in_service(flow, route_0, 1, Packet::PRIO_LO, 0, 8);
    TestPacket same_input_waiting(flow, route_1, 2, Packet::PRIO_HI, 0, 8);
    TestPacket same_output_waiting(flow, route_0, 3, Packet::PRIO_LO, 1, 8);

    ingress_0.receivePacket(in_service);
    ingress_0.receivePacket(same_input_waiting);
    ingress_1.receivePacket(same_output_waiting);
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());

    const NsRosettaRequestDepth high_depth =
        harness.traffic_manager->request_depth(1, Packet::PRIO_HI);
    EXPECT_EQ(high_depth.requesting_ingresses, 1);
    EXPECT_EQ(high_depth.queued_packets, 1);
    EXPECT_EQ(high_depth.queued_bytes, 100);

    const NsRosettaPathLoad output_0 =
        harness.traffic_manager->sample_path_load(0);
    EXPECT_EQ(output_0.requesting_ingresses, 1);
    EXPECT_EQ(output_0.queued_packets, 1);
    EXPECT_EQ(output_0.buffered_bytes, 100);
    EXPECT_EQ(output_0.in_service_bytes, 100);
    EXPECT_EQ(output_0.backlog_bytes, 200);
    EXPECT_EQ(output_0.backlog_delay_ps, timeFromNs(200));
    EXPECT_EQ(output_0.shared_buffer_occupancy, 200);
    EXPECT_EQ(output_0.shared_buffer_capacity, kDefaultSharedBuffer);

    EXPECT_EQ(harness.traffic_manager->voq_buffered_bytes(
                  0, 1, Packet::PRIO_HI),
              100);
    EXPECT_EQ(harness.traffic_manager->ingress_buffered_bytes(0), 100);
    EXPECT_EQ(harness.traffic_manager->ingress_backlog_bytes(0), 200);
    EXPECT_EQ(harness.traffic_manager->egress_pair_backlog_bytes(
                  0, {0, 8}),
              100);
    EXPECT_EQ(harness.traffic_manager->egress_pair_backlog_bytes(
                  0, {1, 8}),
              100);
    EXPECT_EQ(harness.traffic_manager->egress_pair_backlog_bytes(
                  1, {0, 8}),
              100);
    EXPECT_EQ(harness.traffic_manager->pair_backlog_bytes({0, 8}), 200);
    EXPECT_THROW(harness.traffic_manager->sample_path_load(2),
                 std::out_of_range);
    EXPECT_THROW(harness.traffic_manager->voq_buffered_bytes(
                     2, 0, Packet::PRIO_LO),
                 std::out_of_range);

    NsRosettaHarness::drain_all_events();
}

TEST(NsRosettaSwitchTest, PathLoadReportsResidualNotWholePacketService) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& egress = harness.add_egress();
    NsRosettaIngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket packet(flow, route, 1, Packet::PRIO_LO, 0, 8, 200);

    ingress.receivePacket(packet);
    ASSERT_TRUE(EventList::doNextEvent());
    std::optional<NsRosettaPathLoad> midpoint;
    CallbackEvent sample(EventList::getTheEventList(), timeFromNs(50), [&] {
        midpoint = harness.traffic_manager->sample_path_load(0);
    });
    NsRosettaHarness::drain_all_events();

    ASSERT_TRUE(midpoint.has_value());
    EXPECT_EQ(midpoint->in_service_bytes, 200);
    EXPECT_EQ(midpoint->backlog_delay_ps, timeFromNs(150));
}

TEST(NsRosettaSwitchTest,
     ObserverReportsDestinationEgressAndEndpointPairTransitions) {
    NsRosettaHarness harness;
    NsRosettaEgressSerializer& egress = harness.add_egress();
    NsRosettaIngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    flow.set_flowid(77);
    auto observer = std::make_shared<RecordingBacklogObserver>();
    harness.traffic_manager->set_backlog_observer(observer);
    TestPacket packet(flow, route, 91, Packet::PRIO_MID, 5, 9);

    ingress.receivePacket(packet);
    NsRosettaHarness::drain_all_events();

    ASSERT_EQ(observer->observations.size(), 3);
    EXPECT_EQ(observer->observations[0].transition,
              NsRosettaQueueTransition::Enqueued);
    EXPECT_EQ(observer->observations[1].transition,
              NsRosettaQueueTransition::Granted);
    EXPECT_EQ(observer->observations[2].transition,
              NsRosettaQueueTransition::SerializationCompleted);
    for (const NsRosettaBacklogObservation& observation :
         observer->observations) {
        EXPECT_EQ(observation.switch_type, FatTreeSwitch::TOR);
        EXPECT_EQ(observation.ingress_id, 0);
        EXPECT_EQ(observation.egress_id, 0);
        EXPECT_EQ(observation.traffic_class, 1);
        EXPECT_EQ(observation.pair,
                  (NsRosettaEndpointPair{5, 9}));
        EXPECT_EQ(observation.flow_id, 77);
        EXPECT_EQ(observation.packet_id, 91);
    }
    EXPECT_EQ(observer->observations[0].pair_backlog_bytes, 100);
    EXPECT_EQ(observer->observations[1].pair_backlog_bytes, 100);
    EXPECT_EQ(observer->observations[2].pair_backlog_bytes, 0);
    EXPECT_EQ(harness.traffic_manager->pair_backlog_bytes({5, 9}), 0);
}

TEST(NsRosettaSwitchTest, SharedBufferDropsWithoutPauseOrBackpressure) {
    NsRosettaHarness harness(100);
    NsRosettaEgressSerializer& egress = harness.add_egress();
    NsRosettaIngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket in_service(flow, route, 1, Packet::PRIO_LO, 0, 7);
    TestPacket buffered(flow, route, 2, Packet::PRIO_LO, 0, 7);
    TestPacket dropped(flow, route, 3, Packet::PRIO_LO, 0, 7);

    ingress.receivePacket(in_service);
    ingress.receivePacket(buffered);
    ingress.receivePacket(dropped);
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());

    EXPECT_TRUE(dropped.dropped);
    EXPECT_EQ(harness.traffic_manager->buffer_counters().dropped_packets, 1);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_occupancy(), 100);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_high_watermark(), 100);

    NsRosettaHarness::drain_all_events();
    EXPECT_EQ(arrival_order(sink),
              (std::vector<packetid_t>{1, 2}));
}

TEST(NsRosettaSwitchTest, FactoryConstructsEveryClosTier) {
    EventList& eventlist = EventList::getTheEventList();
    for (FatTreeSwitch::switch_type type :
         {FatTreeSwitch::TOR, FatTreeSwitch::AGG, FatTreeSwitch::CORE}) {
        auto switch_instance = FatTreeSwitchFactory::create(
            FatTreeSwitchModel::NsRosetta, eventlist, "ns-rosetta-tier",
            type, static_cast<uint32_t>(type), 0, nullptr,
            kDefaultSharedBuffer);
        auto* rosetta = dynamic_cast<NsRosetta*>(switch_instance.get());
        ASSERT_NE(rosetta, nullptr);
        EXPECT_EQ(rosetta->getType(), type);
    }
}

TEST(NsRosettaSwitchTest, FatTreeBuildsPhysicalRosettaPorts) {
    EventList& eventlist = EventList::getTheEventList();
    FatTreeTopologyCfg cfg(2, 32, speedFromGbps(100), 32768,
                           timeFromNs(1000), 0, COMPOSITE, FAIR_PRIO);
    cfg.set_switch_model(FatTreeSwitchModel::NsRosetta);
    cfg.set_ns_rosetta_shared_buffer_capacity(65536);
    FatTreeTopology topology(&cfg, nullptr, &eventlist, nullptr);

    for (Switch* switch_instance : topology.switches_lp) {
        EXPECT_NE(dynamic_cast<NsRosetta*>(switch_instance), nullptr);
    }
    for (Switch* switch_instance : topology.switches_up) {
        EXPECT_NE(dynamic_cast<NsRosetta*>(switch_instance), nullptr);
    }

    std::unique_ptr<std::vector<const Route*>> paths(
        topology.get_bidir_paths(0, 31, false));
    ASSERT_FALSE(paths->empty());
    size_t ingress_adapters = 0;
    size_t egress_serializers = 0;
    for (PacketSink* sink : *paths->front()) {
        if (dynamic_cast<NsRosettaIngressPort*>(sink) != nullptr) {
            ingress_adapters++;
        }
        if (dynamic_cast<NsRosettaEgressSerializer*>(sink) != nullptr) {
            egress_serializers++;
        }
    }
    EXPECT_GT(ingress_adapters, 0);
    EXPECT_EQ(ingress_adapters, egress_serializers);
}

TEST(NsRosettaSwitchTest, ExplicitlyRejectsPauseQueueModes) {
    EventList& eventlist = EventList::getTheEventList();
    for (queue_type queue_mode :
         {LOSSLESS, LOSSLESS_INPUT, LOSSLESS_INPUT_ECN}) {
        FatTreeTopologyCfg cfg(2, 32, speedFromGbps(100), 32768,
                               timeFromNs(1000), 0, queue_mode,
                               FAIR_PRIO);
        cfg.set_switch_model(FatTreeSwitchModel::NsRosetta);

        EXPECT_THROW(FatTreeTopology(&cfg, nullptr, &eventlist, nullptr),
                     std::invalid_argument);
    }
}

TEST(NsRosettaSwitchTest, ExplicitlyRejectsPausePackets) {
    NsRosettaHarness harness;
    NsRosettaIngressPort& ingress = harness.add_ingress("ingress-0");
    EthPausePacket* pause = EthPausePacket::newpkt(100, 0);

    EXPECT_THROW(ingress.receivePacket(*pause), std::invalid_argument);
    pause->free();
}

}  // namespace
