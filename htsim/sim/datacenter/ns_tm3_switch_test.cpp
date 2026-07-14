// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fat_tree_switch_factory.h"
#include "fat_tree_topology.h"
#include "loggers.h"
#include "ns_tm3_switch.h"

namespace {

constexpr linkspeed_bps kLinkSpeed = 8000000000ULL;
constexpr uint16_t kPacketBytes = 100;
constexpr mem_b kDefaultSharedBuffer = 4096;

class TestPacket : public Packet {
public:
    TestPacket(PacketFlow& flow,
               const Route& route,
               packetid_t packet_id,
               PktPriority priority,
               uint16_t bytes = kPacketBytes)
        : _priority(priority) {
        set_route(flow, route, bytes, packet_id);
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

    void receivePacket(Packet& pkt) override { arrivals.push_back({pkt.id(), EventList::now()}); }
    const string& nodename() override { return _name; }

    simtime_picosec arrival_time(packetid_t packet_id) const {
        auto arrival = std::find_if(
            arrivals.begin(), arrivals.end(),
            [packet_id](const Arrival& value) { return value.packet_id == packet_id; });
        if (arrival == arrivals.end()) {
            throw std::out_of_range("packet was not delivered");
        }
        return arrival->time;
    }

    std::vector<Arrival> arrivals;

private:
    string _name{"RecordingSink"};
};

class RecordingQueueObserver final : public NsTm3QueueObserver {
public:
    RecordingQueueObserver() { observations.reserve(32); }

    void observe(const NsTm3QueueObservation& observation) noexcept override {
        observations.push_back(observation);
    }

    std::vector<NsTm3QueueObservation> observations;
};

class NsTm3Harness {
public:
    explicit NsTm3Harness(mem_b shared_buffer_capacity = kDefaultSharedBuffer,
                          FatTreeSwitch::switch_type type = FatTreeSwitch::TOR,
                          simtime_picosec switch_delay = 0)
        : eventlist(EventList::getTheEventList()),
          switch_owner(FatTreeSwitchFactory::create(FatTreeSwitchModel::NsTm3,
                                                    eventlist,
                                                    "ns-tm3-test",
                                                    type,
                                                    0,
                                                    switch_delay,
                                                    nullptr,
                                                    shared_buffer_capacity)),
          traffic_manager(dynamic_cast<NsTm3Switch*>(switch_owner.get())) {
        if (traffic_manager == nullptr) {
            throw std::logic_error("factory did not construct ns-tm3 switch");
        }
    }

    NsTm3EgressSerializer& add_egress(QueueLogger* logger = nullptr) {
        auto serializer = std::make_unique<NsTm3EgressSerializer>(kLinkSpeed, eventlist, logger);
        NsTm3EgressSerializer* result = serializer.get();
        serializers.push_back(std::move(serializer));
        traffic_manager->addPort(result);
        return *result;
    }

    NsTm3IngressPort& add_ingress(const string& name) {
        auto* ingress =
            dynamic_cast<NsTm3IngressPort*>(traffic_manager->create_physical_ingress(name));
        if (ingress == nullptr) {
            throw std::logic_error("ns-tm3 ingress adapter was not created");
        }
        return *ingress;
    }

    static void drain_all_events() {
        while (EventList::doNextEvent()) {
        }
    }

    EventList& eventlist;
    std::vector<std::unique_ptr<NsTm3EgressSerializer>> serializers;
    std::unique_ptr<FatTreeSwitch> switch_owner;
    NsTm3Switch* traffic_manager;
};

Route route_via(NsTm3EgressSerializer& serializer, RecordingSink& sink) {
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

TEST(NsTm3SwitchTest, SeparatesHeadOfLineBlockingByEgressVoq) {
    NsTm3Harness harness;
    NsTm3EgressSerializer& blocked_egress = harness.add_egress();
    NsTm3EgressSerializer& free_egress = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route blocked_route = route_via(blocked_egress, sink);
    Route free_route = route_via(free_egress, sink);
    PacketFlow flow(nullptr);
    TestPacket blocked_first(flow, blocked_route, 1, Packet::PRIO_LO, 200);
    TestPacket blocked_second(flow, blocked_route, 2, Packet::PRIO_LO, 200);
    TestPacket escapes_hol(flow, free_route, 3, Packet::PRIO_LO, 100);

    const simtime_picosec start_time = EventList::now();
    ingress.receivePacket(blocked_first);
    ingress.receivePacket(blocked_second);
    ingress.receivePacket(escapes_hol);
    NsTm3Harness::drain_all_events();

    EXPECT_LT(sink.arrival_time(3), sink.arrival_time(2));
    EXPECT_EQ(sink.arrival_time(3), start_time + timeFromNs(100));
    EXPECT_EQ(sink.arrival_time(2), start_time + timeFromNs(400));
}

TEST(NsTm3SwitchTest, DefaultsToOldestHeadAcrossIngressVoqs) {
    NsTm3Harness harness;
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress_0 = harness.add_ingress("ingress-0");
    NsTm3IngressPort& ingress_1 = harness.add_ingress("ingress-1");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket first_0(flow, route, 1, Packet::PRIO_LO);
    TestPacket second_0(flow, route, 2, Packet::PRIO_LO);
    TestPacket first_1(flow, route, 3, Packet::PRIO_LO);
    TestPacket second_1(flow, route, 4, Packet::PRIO_LO);

    ingress_0.receivePacket(first_0);
    ingress_0.receivePacket(second_0);
    ingress_1.receivePacket(first_1);
    ingress_1.receivePacket(second_1);
    NsTm3Harness::drain_all_events();

    EXPECT_EQ(arrival_order(sink), (std::vector<packetid_t>{1, 2, 3, 4}));
}

TEST(NsTm3SwitchTest, RetainsIngressRoundRobinAsAnExplicitSensitivity) {
    NsTm3Harness harness;
    harness.traffic_manager->set_voq_arbitration(NsTm3VoqArbitration::IngressRoundRobin);
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress_0 = harness.add_ingress("ingress-0");
    NsTm3IngressPort& ingress_1 = harness.add_ingress("ingress-1");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket first_0(flow, route, 1, Packet::PRIO_LO);
    TestPacket second_0(flow, route, 2, Packet::PRIO_LO);
    TestPacket first_1(flow, route, 3, Packet::PRIO_LO);
    TestPacket second_1(flow, route, 4, Packet::PRIO_LO);

    ingress_0.receivePacket(first_0);
    ingress_0.receivePacket(second_0);
    ingress_1.receivePacket(first_1);
    ingress_1.receivePacket(second_1);
    NsTm3Harness::drain_all_events();

    EXPECT_EQ(arrival_order(sink), (std::vector<packetid_t>{1, 3, 2, 4}));
}

TEST(NsTm3SwitchTest, SerializesIndependentEgressesConcurrently) {
    NsTm3Harness harness;
    NsTm3EgressSerializer& egress_0 = harness.add_egress();
    NsTm3EgressSerializer& egress_1 = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route_0 = route_via(egress_0, sink);
    Route route_1 = route_via(egress_1, sink);
    PacketFlow flow(nullptr);
    TestPacket packet_0(flow, route_0, 1, Packet::PRIO_LO);
    TestPacket packet_1(flow, route_1, 2, Packet::PRIO_LO);

    const simtime_picosec start_time = EventList::now();
    ingress.receivePacket(packet_0);
    ingress.receivePacket(packet_1);
    NsTm3Harness::drain_all_events();

    EXPECT_EQ(sink.arrival_time(1), sink.arrival_time(2));
    EXPECT_EQ(sink.arrival_time(1), start_time + timeFromNs(100));
}

TEST(NsTm3SwitchTest, AppliesConfiguredSwitchPipelineDelay) {
    const simtime_picosec kSwitchDelay = timeFromNs(37u);
    NsTm3Harness harness(kDefaultSharedBuffer, FatTreeSwitch::TOR, kSwitchDelay);
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket packet(flow, route, 1, Packet::PRIO_LO);

    const simtime_picosec start_time = EventList::now();
    ingress.receivePacket(packet);
    NsTm3Harness::drain_all_events();

    EXPECT_EQ(sink.arrival_time(1), start_time + kSwitchDelay + timeFromNs(100));
}

TEST(NsTm3SwitchTest, AppliesStrictPriorityOnlyAtPacketBoundaries) {
    NsTm3Harness harness;
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket low_in_service(flow, route, 1, Packet::PRIO_LO);
    TestPacket low_waiting(flow, route, 2, Packet::PRIO_LO);
    TestPacket high_waiting(flow, route, 3, Packet::PRIO_HI);

    const simtime_picosec start_time = EventList::now();
    ingress.receivePacket(low_in_service);
    ingress.receivePacket(low_waiting);
    ingress.receivePacket(high_waiting);
    NsTm3Harness::drain_all_events();

    EXPECT_EQ(arrival_order(sink), (std::vector<packetid_t>{1, 3, 2}));
    EXPECT_EQ(sink.arrival_time(1), start_time + timeFromNs(100));
    EXPECT_EQ(sink.arrival_time(3), start_time + timeFromNs(200));
    EXPECT_EQ(sink.arrival_time(2), start_time + timeFromNs(300));
}

TEST(NsTm3SwitchTest, ConservesOneSharedBufferAccountingDomain) {
    NsTm3Harness harness(300);
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket first(flow, route, 1, Packet::PRIO_LO);
    TestPacket second(flow, route, 2, Packet::PRIO_LO);
    TestPacket third(flow, route, 3, Packet::PRIO_LO);

    ingress.receivePacket(first);
    ingress.receivePacket(second);
    ingress.receivePacket(third);
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());

    const NsTm3BufferCounters& active = harness.traffic_manager->buffer_counters();
    EXPECT_EQ(active.admitted_bytes, 300);
    EXPECT_EQ(active.dequeued_bytes, 100);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_occupancy(), 200);
    EXPECT_EQ(active.admitted_bytes - active.dequeued_bytes,
              harness.traffic_manager->shared_buffer_occupancy());
    EXPECT_EQ(harness.traffic_manager->egress_buffered_bytes(0), 200);
    EXPECT_EQ(harness.traffic_manager->egress_backlog_bytes(0), 300);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_high_watermark(), 200);

    NsTm3Harness::drain_all_events();
    const NsTm3BufferCounters& drained = harness.traffic_manager->buffer_counters();
    EXPECT_EQ(drained.admitted_bytes, drained.dequeued_bytes);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_occupancy(), 0);
    EXPECT_EQ(harness.traffic_manager->egress_backlog_bytes(0), 0);
}

TEST(NsTm3SwitchTest, TracesExactEgressBacklogAndMaximumQueueWait) {
    NsTm3Harness harness(400);
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    flow.set_flowid(77);
    TestPacket first(flow, route, 1, Packet::PRIO_LO);
    TestPacket second(flow, route, 2, Packet::PRIO_LO);
    TestPacket third(flow, route, 3, Packet::PRIO_LO);
    auto observer = std::make_shared<RecordingQueueObserver>();
    harness.traffic_manager->set_queue_observer(observer);

    ingress.receivePacket(first);
    ingress.receivePacket(second);
    ingress.receivePacket(third);
    NsTm3Harness::drain_all_events();

    ASSERT_EQ(observer->observations.size(), 9U);
    EXPECT_EQ(observer->observations[0].transition, NsTm3QueueTransition::Enqueued);
    EXPECT_EQ(observer->observations[1].transition, NsTm3QueueTransition::Dequeued);
    const auto peak =
        std::max_element(observer->observations.begin(), observer->observations.end(),
                         [](const NsTm3QueueObservation& left, const NsTm3QueueObservation& right) {
                             return left.egress_backlog_bytes < right.egress_backlog_bytes;
                         });
    ASSERT_NE(peak, observer->observations.end());
    EXPECT_EQ(peak->egress_backlog_bytes, 300);
    EXPECT_EQ(peak->egress_buffered_bytes, 200);
    EXPECT_EQ(peak->egress_in_service_bytes, 100);
    EXPECT_EQ(peak->flow_id, 77U);
    EXPECT_EQ(peak->egress_id, 0U);

    const NsTm3EgressStatistics& statistics = harness.traffic_manager->egress_statistics(0);
    EXPECT_EQ(statistics.buffered_high_watermark, 200);
    EXPECT_EQ(statistics.backlog_high_watermark, 300);
    EXPECT_EQ(statistics.max_queue_wait_ps, timeFromNs(200));
    EXPECT_EQ(statistics.max_queue_wait_ingress_id, 0U);
    EXPECT_EQ(statistics.max_queue_wait_flow_id, 77U);
    EXPECT_EQ(statistics.max_queue_wait_packet_id, 3U);
    EXPECT_EQ(observer->observations.back().transition,
              NsTm3QueueTransition::SerializationCompleted);
    EXPECT_EQ(observer->observations.back().egress_backlog_bytes, 0);
}

TEST(NsTm3SwitchTest, DropsOnlyWhenSharedBufferCapacityIsExceeded) {
    NsTm3Harness harness(150);
    NsTm3EgressSerializer& egress = harness.add_egress();
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket first(flow, route, 1, Packet::PRIO_LO);
    TestPacket buffered(flow, route, 2, Packet::PRIO_LO);
    TestPacket dropped(flow, route, 3, Packet::PRIO_LO);

    ingress.receivePacket(first);
    ingress.receivePacket(buffered);
    ingress.receivePacket(dropped);
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());

    const NsTm3BufferCounters& counters = harness.traffic_manager->buffer_counters();
    EXPECT_EQ(counters.admitted_packets, 2);
    EXPECT_EQ(counters.dropped_packets, 1);
    EXPECT_EQ(counters.dropped_bytes, 100);
    EXPECT_TRUE(dropped.dropped);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_occupancy(), 100);
    EXPECT_EQ(counters.admitted_bytes - counters.dequeued_bytes,
              harness.traffic_manager->shared_buffer_occupancy());

    NsTm3Harness::drain_all_events();
    EXPECT_EQ(arrival_order(sink), (std::vector<packetid_t>{1, 2}));
}

TEST(NsTm3SwitchTest, RejectsRoutesThatBypassTheTrafficManager) {
    NsTm3Harness harness;
    NsTm3EgressSerializer& egress = harness.add_egress();
    RecordingSink sink;
    Route route = route_via(egress, sink);
    PacketFlow flow(nullptr);
    TestPacket packet(flow, route, 1, Packet::PRIO_LO);

    EXPECT_THROW(egress.receivePacket(packet), std::logic_error);
    EXPECT_EQ(harness.traffic_manager->buffer_counters().admitted_packets, 0);
    EXPECT_TRUE(sink.arrivals.empty());
}

TEST(NsTm3SwitchTest, SharedDropDoesNotMisreportAnEmptyEgressQueue) {
    EventList& eventlist = EventList::getTheEventList();
    QueueLoggerSampling empty_egress_logger(timeFromUs(uint32_t{10}), eventlist);
    // Consume the logger's immediate initialization event, then remove its
    // periodic sample before draining this finite test.
    ASSERT_TRUE(EventList::doNextEvent());

    NsTm3Harness harness(100);
    NsTm3EgressSerializer& busy_egress = harness.add_egress();
    NsTm3EgressSerializer& empty_egress = harness.add_egress(&empty_egress_logger);
    NsTm3IngressPort& ingress = harness.add_ingress("ingress-0");
    RecordingSink sink;
    Route busy_route = route_via(busy_egress, sink);
    Route empty_route = route_via(empty_egress, sink);
    PacketFlow flow(nullptr);
    TestPacket in_service(flow, busy_route, 1, Packet::PRIO_LO);
    TestPacket fills_shared_buffer(flow, busy_route, 2, Packet::PRIO_LO);
    TestPacket dropped_at_empty_egress(flow, empty_route, 3, Packet::PRIO_LO);

    ingress.receivePacket(in_service);
    ingress.receivePacket(fills_shared_buffer);
    ingress.receivePacket(dropped_at_empty_egress);
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());

    EXPECT_TRUE(dropped_at_empty_egress.dropped);
    EXPECT_EQ(empty_egress.queuesize(), 0);
    EXPECT_EQ(harness.traffic_manager->buffer_counters().dropped_packets, 1);

    EventList::cancelPendingSource(empty_egress_logger);
    NsTm3Harness::drain_all_events();
}

TEST(NsTm3SwitchTest, FactoryConstructsEveryClosTier) {
    EventList& eventlist = EventList::getTheEventList();
    for (FatTreeSwitch::switch_type type :
         {FatTreeSwitch::TOR, FatTreeSwitch::AGG, FatTreeSwitch::CORE}) {
        auto switch_instance = FatTreeSwitchFactory::create(
            FatTreeSwitchModel::NsTm3, eventlist, "ns-tm3-tier", type, static_cast<uint32_t>(type),
            0, nullptr, kDefaultSharedBuffer);
        auto* ns_tm3 = dynamic_cast<NsTm3Switch*>(switch_instance.get());
        ASSERT_NE(ns_tm3, nullptr);
        EXPECT_EQ(ns_tm3->getType(), type);
    }
}

TEST(NsTm3SwitchTest, PreservesFatTreeFibPathSelection) {
    EventList& eventlist = EventList::getTheEventList();
    FatTreeTopologyCfg cfg(2, 32, speedFromGbps(100), 32768, timeFromNs(1000), 0, COMPOSITE,
                           FAIR_PRIO);
    cfg.set_switch_model(FatTreeSwitchModel::NsTm3);
    cfg.set_ns_tm3_shared_buffer_capacity(65536);
    FatTreeTopology topology(&cfg, nullptr, &eventlist, nullptr);

    constexpr uint32_t source_host = 0;
    constexpr uint32_t destination_host = 1;
    const uint32_t source_tor = cfg.HOST_POD_SWITCH(source_host);
    ASSERT_EQ(source_tor, cfg.HOST_POD_SWITCH(destination_host));

    RecordingSink destination;
    PacketFlow flow(nullptr);
    flow.set_flowid(17);
    topology.switches_lp[source_tor]->addHostPort(destination_host, flow.flow_id(), &destination);

    Route source_to_tor;
    source_to_tor.push_back(topology.queues_ns_nlp[source_host][source_tor][0]);
    source_to_tor.push_back(topology.pipes_ns_nlp[source_host][source_tor][0]);
    source_to_tor.push_back(
        topology.queues_ns_nlp[source_host][source_tor][0]->getRemoteEndpoint());

    TestPacket packet(flow, source_to_tor, 1, Packet::PRIO_LO);
    packet.set_src(source_host);
    packet.set_dst(destination_host);
    packet.sendOn();
    NsTm3Harness::drain_all_events();

    ASSERT_EQ(destination.arrivals.size(), 1);
    EXPECT_EQ(destination.arrivals.front().packet_id, 1);
}

TEST(NsTm3SwitchTest, ThreeTierRoutesUsePhysicalIngressAdapters) {
    EventList& eventlist = EventList::getTheEventList();
    FatTreeTopologyCfg cfg(3, 128, speedFromGbps(100), 32768, timeFromNs(1000), 0, COMPOSITE,
                           FAIR_PRIO);
    cfg.set_switch_model(FatTreeSwitchModel::NsTm3);
    cfg.set_ns_tm3_shared_buffer_capacity(65536);
    FatTreeTopology topology(&cfg, nullptr, &eventlist, nullptr);

    for (Switch* switch_instance : topology.switches_lp) {
        EXPECT_NE(dynamic_cast<NsTm3Switch*>(switch_instance), nullptr);
    }
    for (Switch* switch_instance : topology.switches_up) {
        EXPECT_NE(dynamic_cast<NsTm3Switch*>(switch_instance), nullptr);
    }
    for (Switch* switch_instance : topology.switches_c) {
        EXPECT_NE(dynamic_cast<NsTm3Switch*>(switch_instance), nullptr);
    }

    std::unique_ptr<std::vector<const Route*>> paths(topology.get_bidir_paths(0, 127, false));
    ASSERT_FALSE(paths->empty());
    const Route* route = paths->front();
    size_t ingress_adapters = 0;
    size_t egress_serializers = 0;
    for (PacketSink* sink : *route) {
        if (dynamic_cast<NsTm3IngressPort*>(sink) != nullptr) {
            ingress_adapters++;
        }
        if (dynamic_cast<NsTm3EgressSerializer*>(sink) != nullptr) {
            egress_serializers++;
        }
    }
    EXPECT_EQ(ingress_adapters, 5);
    EXPECT_EQ(egress_serializers, 5);

    RecordingSink destination;
    Route delivery_route(*route, destination);
    PacketFlow flow(nullptr);
    TestPacket packet(flow, delivery_route, 1, Packet::PRIO_LO);

    packet.sendOn();
    NsTm3Harness::drain_all_events();

    ASSERT_EQ(destination.arrivals.size(), 1);
    EXPECT_EQ(destination.arrivals.front().packet_id, 1);
}

TEST(NsTm3SwitchTest, ExplicitlyRejectsPauseQueueModes) {
    EventList& eventlist = EventList::getTheEventList();
    for (queue_type queue_mode : {LOSSLESS, LOSSLESS_INPUT, LOSSLESS_INPUT_ECN}) {
        FatTreeTopologyCfg cfg(2, 32, speedFromGbps(100), 32768, timeFromNs(1000), 0, queue_mode,
                               FAIR_PRIO);
        cfg.set_switch_model(FatTreeSwitchModel::NsTm3);

        EXPECT_THROW(FatTreeTopology(&cfg, nullptr, &eventlist, nullptr), std::invalid_argument);
    }
}

}  // namespace
