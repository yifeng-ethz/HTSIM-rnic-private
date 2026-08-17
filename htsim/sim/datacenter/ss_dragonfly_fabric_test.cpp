// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// Deterministic fixtures for the ss-dragonfly fabric.  Every exact value
// asserted here was pre-registered by hand in
// docs/ss-dragonfly-fabric/fixture-expectations.md before the first run
// of the corresponding fixture.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "eventlist.h"
#include "ss_dragonfly_fabric.h"

namespace {

constexpr simtime_picosec kSerializationPs = 166400;
constexpr mem_b kWireBytes = 4160;
constexpr mem_b kPayloadBytes = 4096;

EventList& testEventList() {
    EventList& event_list = EventList::getTheEventList();
    while (EventList::doNextEvent()) {
    }
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    return event_list;
}

// The gtest binary shares one EventList whose clock never rewinds, so every
// fixture measures time from its own origin: the pre-registered values in
// docs/ss-dragonfly-fabric/fixture-expectations.md are offsets from the
// fixture's injection origin ("t = 0" in the document).
void runUntil(simtime_picosec stop_ps) {
    while (true) {
        const std::optional<simtime_picosec> next = EventList::nextEventTime();
        if (!next.has_value() || *next > stop_ps) {
            return;
        }
        ASSERT_TRUE(EventList::doNextEvent());
    }
}

SsDragonflyConfig studyConfig() {
    // Mirrors topologies/ss_dragonfly/p2a2h1g3_200g.topo.
    SsDragonflyConfig config;
    config.hosts_per_router = 2;
    config.routers_per_group = 2;
    config.global_links_per_router = 1;
    config.group_count = 3;
    config.host_link = {200000000000ULL, 300000};
    config.local_link = {200000000000ULL, 300000};
    config.global_link = {200000000000ULL, 1000000};
    config.switch_pipeline_delay_ps = 350000;
    config.rosetta_shared_buffer_bytes = 4194304;
    config.maximum_wire_packet_bytes = 4160;
    config.near_end_bytes_per_level = 8320;
    config.far_end_bytes_per_level = 8320;
    config.downstream_bytes_per_level = 33280;
    config.advertisement_period_ps = 10000000;
    config.advertisement_delay_ps = 1000000;
    config.maximum_downstream_age_ps = 40000000;
    config.routing_seed = UINT64_C(383530136519245852);
    return config;
}

struct DecisionRecord {
    std::uint32_t router;
    htsim::DragonflyDecisionOutcome outcome;
    std::optional<htsim::DragonflyPortId> port;
    std::vector<htsim::DragonflyCandidate> candidates;
};

struct DeliveryRecord {
    std::uint32_t source;
    std::uint32_t destination;
    simtime_picosec at_ps;
    htsim::DragonflyRouteClass route_class;
    std::uint8_t router_hops;
    std::uint8_t global_hops;
};

class FixtureHarness {
public:
    explicit FixtureHarness(SsDragonflyConfig config)
        : _event_list(testEventList()),
          _origin_ps(EventList::now()),
          _fabric(_event_list, config),
          _flow(nullptr) {
        _flow.set_flowid(1);
        _fabric.setDeliveryObserver([this](const SsDragonflyPacket& packet,
                                           const htsim::DragonflyRouteState& state,
                                           simtime_picosec at_ps) {
            deliveries.push_back(DeliveryRecord{packet.src(), packet.dst(),
                                                at_ps - _origin_ps, state.route_class,
                                                state.router_hops, state.global_hops});
        });
        _fabric.setRouteDecisionObserver(
            [this](std::uint32_t router, const SsDragonflyPacket& packet,
                   const htsim::DragonflyRoutingDecision& decision) {
                decisionsByDestination[packet.dst()].push_back(
                    DecisionRecord{router, decision.outcome, decision.output_port,
                                   decision.candidates});
            });
    }

    SsDragonflyTopology& fabric() { return _fabric; }
    simtime_picosec originPs() const { return _origin_ps; }
    simtime_picosec absolute(simtime_picosec offset_ps) const {
        return _origin_ps + offset_ps;
    }

    void inject(std::uint32_t source,
                std::uint32_t destination,
                htsim::DragonflyRoutingControl control,
                std::uint64_t sequence) {
        SsDragonflyPacket* packet = SsDragonflyPacket::newPacket(
            _flow, _fabric.injectionRoute(source), static_cast<packetid_t>(_next_packet_id++),
            source, destination, kWireBytes, kPayloadBytes);
        _fabric.registerPacket(*packet, control,
                               _fabric.packetRouteHash(packet->flow_id(), sequence));
        packet->sendOn();
    }

    std::vector<DeliveryRecord> deliveries;
    std::map<std::uint32_t, std::vector<DecisionRecord>> decisionsByDestination;

private:
    EventList& _event_list;
    simtime_picosec _origin_ps;
    SsDragonflyTopology _fabric;
    PacketFlow _flow;
    std::uint64_t _next_packet_id{0};
};

// A one-shot injector so a probe can enter at an exact simulated time.
class ProbeInjector final : public EventSource {
public:
    ProbeInjector(EventList& event_list,
                  FixtureHarness& harness,
                  std::uint32_t source,
                  std::uint32_t destination,
                  htsim::DragonflyRoutingControl control,
                  simtime_picosec at_offset_ps)
        : EventSource(event_list, "ss-dragonfly-probe"),
          _harness(harness),
          _source(source),
          _destination(destination),
          _control(control) {
        event_list.sourceIsPending(*this, harness.absolute(at_offset_ps));
    }

    void doNextEvent() override {
        _harness.inject(_source, _destination, _control, 0);
    }

private:
    FixtureHarness& _harness;
    std::uint32_t _source;
    std::uint32_t _destination;
    htsim::DragonflyRoutingControl _control;
};

// Open-loop line-rate source for the congestion-response background.
class FloodSource final : public EventSource {
public:
    FloodSource(EventList& event_list,
                FixtureHarness& harness,
                std::uint32_t source,
                std::uint32_t destination,
                simtime_picosec stop_offset_ps)
        : EventSource(event_list, "ss-dragonfly-flood"),
          _harness(harness),
          _source(source),
          _destination(destination),
          _stop_ps(harness.absolute(stop_offset_ps)) {
        event_list.sourceIsPending(*this, harness.absolute(0));
    }

    void doNextEvent() override {
        if (EventList::now() >= _stop_ps) {
            return;
        }
        // The background is pinned to its minimal path so the r0 global
        // egress actually saturates; an adaptive background self-balances
        // away from the hotspot and never builds the registered backlog
        // (see the corrections section of fixture-expectations.md).
        _harness.inject(_source, _destination,
                        htsim::DragonflyRoutingControl::DeterministicMinimal, 0);
        _sequence++;
        injected++;
        eventlist().sourceIsPendingRel(*this, kSerializationPs);
    }

    std::uint64_t injected{0};

private:
    FixtureHarness& _harness;
    std::uint32_t _source;
    std::uint32_t _destination;
    simtime_picosec _stop_ps;
    std::uint64_t _sequence{0};
};

std::string describeDecisions(const std::vector<DecisionRecord>& decisions) {
    std::ostringstream out;
    for (const DecisionRecord& record : decisions) {
        out << "(r" << record.router << ","
            << (record.outcome == htsim::DragonflyDecisionOutcome::Delivered
                    ? std::string("deliver")
                    : record.port.has_value() ? "p" + std::to_string(*record.port)
                                              : std::string("drop"));
        for (const htsim::DragonflyCandidate& candidate : record.candidates) {
            out << " "
                << (candidate.route_class == htsim::DragonflyRouteClass::Minimal ? "min"
                                                                                 : "non")
                << (candidate.entry.output_port.has_value()
                        ? "@p" + std::to_string(*candidate.entry.output_port)
                        : "@none")
                << ":c" << static_cast<unsigned>(candidate.congestion) << ":s"
                << static_cast<unsigned>(candidate.adjusted_score);
        }
        out << ")";
    }
    return out.str();
}

void expectDecisionSequence(const std::vector<DecisionRecord>& decisions,
                            const std::vector<std::pair<std::uint32_t,
                                                        std::optional<htsim::DragonflyPortId>>>&
                                expected) {
    ASSERT_EQ(decisions.size(), expected.size()) << describeDecisions(decisions);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(decisions[index].router, expected[index].first)
            << describeDecisions(decisions);
        if (expected[index].second.has_value()) {
            ASSERT_EQ(decisions[index].outcome, htsim::DragonflyDecisionOutcome::Forward);
            ASSERT_TRUE(decisions[index].port.has_value());
            EXPECT_EQ(*decisions[index].port, *expected[index].second)
                << describeDecisions(decisions);
        } else {
            EXPECT_EQ(decisions[index].outcome, htsim::DragonflyDecisionOutcome::Delivered)
                << describeDecisions(decisions);
        }
    }
}

TEST(SsDragonflyLoaderTest, ParsesCanonicalStudyTopology) {
    std::istringstream file(
        "topology dragonfly\n"
        "hosts_per_router 2\nrouters_per_group 2\nglobal_links_per_router 1\n"
        "groups 3\nhost_link_gbps 200\nlocal_link_gbps 200\nglobal_link_gbps 200\n"
        "host_link_latency_ps 300000\nlocal_link_latency_ps 300000\n"
        "global_link_latency_ps 1000000\nswitch_pipeline_ps 350000\n"
        "rosetta_buffer_bytes 4194304\n");
    const SsDragonflyConfig config = loadSsDragonflyConfig(file);
    EXPECT_EQ(config.hosts_per_router, 2U);
    EXPECT_EQ(config.routers_per_group, 2U);
    EXPECT_EQ(config.global_links_per_router, 1U);
    EXPECT_EQ(config.group_count, 3U);
    EXPECT_EQ(config.routerCount(), 6U);
    EXPECT_EQ(config.hostCount(), 12U);
    EXPECT_FALSE(config.singleSwitch());
}

TEST(SsDragonflyLoaderTest, RejectsNonCanonicalAndUnmarkedFiles) {
    std::istringstream wrong_groups(
        "topology dragonfly\nhosts_per_router 2\nrouters_per_group 2\n"
        "global_links_per_router 1\ngroups 4\nhost_link_gbps 200\n"
        "local_link_gbps 200\nglobal_link_gbps 200\nrosetta_buffer_bytes 65536\n");
    EXPECT_THROW(loadSsDragonflyConfig(wrong_groups), std::invalid_argument);

    std::istringstream unmarked("nodes 64\ntiers 2\n");
    EXPECT_THROW(loadSsDragonflyConfig(unmarked), std::invalid_argument);
}

TEST(SsDragonflyLoaderTest, SingleSwitchMerlinShapeLoadsAndDelivers) {
    SsDragonflyConfig config;
    config.hosts_per_router = 20;
    config.routers_per_group = 1;
    config.global_links_per_router = 0;
    config.group_count = 1;
    config.host_link = {200000000000ULL, 300000};
    config.switch_pipeline_delay_ps = 350000;
    config.rosetta_shared_buffer_bytes = 4194304;
    ASSERT_TRUE(config.singleSwitch());

    FixtureHarness harness(config);
    harness.inject(0, 5, htsim::DragonflyRoutingControl::Adaptive0, 0);
    runUntil(harness.absolute(10000000));

    ASSERT_EQ(harness.deliveries.size(), 1U);
    // Same stage sum as fixture F1: one switch between two 200G host links.
    EXPECT_EQ(harness.deliveries[0].at_ps, 1282800U);
    EXPECT_EQ(harness.deliveries[0].route_class, htsim::DragonflyRouteClass::Undecided);
    EXPECT_EQ(harness.deliveries[0].router_hops, 0U);
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFixtureTest, F1SameRouterDelivery) {
    FixtureHarness harness(studyConfig());
    harness.inject(0, 1, htsim::DragonflyRoutingControl::Adaptive0, 0);
    runUntil(harness.absolute(10000000));

    ASSERT_EQ(harness.deliveries.size(), 1U);
    EXPECT_EQ(harness.deliveries[0].at_ps, 1282800U);
    EXPECT_EQ(harness.deliveries[0].route_class, htsim::DragonflyRouteClass::Undecided);
    EXPECT_EQ(harness.deliveries[0].router_hops, 0U);
    EXPECT_EQ(harness.deliveries[0].global_hops, 0U);
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFixtureTest, F2SameGroupDelivery) {
    FixtureHarness harness(studyConfig());
    harness.inject(0, 2, htsim::DragonflyRoutingControl::Adaptive0, 0);
    runUntil(harness.absolute(10000000));

    ASSERT_EQ(harness.deliveries.size(), 1U);
    EXPECT_EQ(harness.deliveries[0].at_ps, 2099200U);
    EXPECT_EQ(harness.deliveries[0].route_class, htsim::DragonflyRouteClass::Minimal);
    EXPECT_EQ(harness.deliveries[0].router_hops, 1U);
    EXPECT_EQ(harness.deliveries[0].global_hops, 0U);
    expectDecisionSequence(harness.decisionsByDestination[2],
                           {{0, htsim::DragonflyPortId{0}}, {1, std::nullopt}});
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFixtureTest, F3MinimalToConnectorRouter) {
    FixtureHarness harness(studyConfig());
    harness.inject(0, 6, htsim::DragonflyRoutingControl::Adaptive0, 0);
    runUntil(harness.absolute(10000000));

    ASSERT_EQ(harness.deliveries.size(), 1U);
    EXPECT_EQ(harness.deliveries[0].at_ps, 2799200U);
    EXPECT_EQ(harness.deliveries[0].route_class, htsim::DragonflyRouteClass::Minimal);
    EXPECT_EQ(harness.deliveries[0].router_hops, 1U);
    EXPECT_EQ(harness.deliveries[0].global_hops, 1U);
    expectDecisionSequence(harness.decisionsByDestination[6],
                           {{0, htsim::DragonflyPortId{1}}, {3, std::nullopt}});
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFixtureTest, F4MinimalWithTargetGroupLocalHop) {
    FixtureHarness harness(studyConfig());
    harness.inject(0, 4, htsim::DragonflyRoutingControl::Adaptive0, 0);
    runUntil(harness.absolute(10000000));

    ASSERT_EQ(harness.deliveries.size(), 1U);
    EXPECT_EQ(harness.deliveries[0].at_ps, 3615600U);
    EXPECT_EQ(harness.deliveries[0].route_class, htsim::DragonflyRouteClass::Minimal);
    EXPECT_EQ(harness.deliveries[0].router_hops, 2U);
    EXPECT_EQ(harness.deliveries[0].global_hops, 1U);
    expectDecisionSequence(harness.decisionsByDestination[4],
                           {{0, htsim::DragonflyPortId{1}},
                            {3, htsim::DragonflyPortId{0}},
                            {2, std::nullopt}});
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFixtureTest, F5DeterministicNonMinimal) {
    FixtureHarness harness(studyConfig());
    harness.inject(0, 4, htsim::DragonflyRoutingControl::DeterministicNonMinimal, 0);
    runUntil(harness.absolute(10000000));

    ASSERT_EQ(harness.deliveries.size(), 1U);
    EXPECT_EQ(harness.deliveries[0].at_ps, 5948400U);
    EXPECT_EQ(harness.deliveries[0].route_class, htsim::DragonflyRouteClass::NonMinimal);
    EXPECT_EQ(harness.deliveries[0].router_hops, 4U);
    EXPECT_EQ(harness.deliveries[0].global_hops, 2U);
    expectDecisionSequence(harness.decisionsByDestination[4],
                           {{0, htsim::DragonflyPortId{0}},
                            {1, htsim::DragonflyPortId{1}},
                            {4, htsim::DragonflyPortId{0}},
                            {5, htsim::DragonflyPortId{1}},
                            {2, std::nullopt}});
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFixtureTest, F6DelayedAdvertisementShiftsProbeNonMinimal) {
    EventList& event_list = testEventList();
    FixtureHarness harness(studyConfig());

    FloodSource flood_a(event_list, harness, 0, 6, 30000000);
    FloodSource flood_b(event_list, harness, 1, 7, 30000000);
    ProbeInjector probe_a(event_list, harness, 2, 4,
                          htsim::DragonflyRoutingControl::Adaptive0, 5000000);
    ProbeInjector probe_b(event_list, harness, 2, 4,
                          htsim::DragonflyRoutingControl::Adaptive0, 20000000);
    runUntil(harness.absolute(70000000));

    EXPECT_EQ(flood_a.injected, 181U);
    EXPECT_EQ(flood_b.injected, 181U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.injected_packets, 364U);
    EXPECT_EQ(statistics.delivered_packets, 364U);
    EXPECT_EQ(statistics.dropped_packets, 0U);
    EXPECT_GE(statistics.advertisements_consumed, 12U);

    const std::vector<DecisionRecord>& probe_decisions = harness.decisionsByDestination[4];
    // Probe A before any advertisement: minimal through the congested
    // connector.  Probe B after the 11 us advertisement: non-minimal
    // through group 2.
    std::vector<DeliveryRecord> probe_deliveries;
    for (const DeliveryRecord& record : harness.deliveries) {
        if (record.destination == 4) {
            probe_deliveries.push_back(record);
        }
    }
    ASSERT_EQ(probe_deliveries.size(), 2U);
    EXPECT_EQ(probe_deliveries[0].route_class, htsim::DragonflyRouteClass::Minimal);
    EXPECT_EQ(probe_deliveries[0].router_hops, 3U);
    EXPECT_EQ(probe_deliveries[0].global_hops, 1U);
    EXPECT_LT(probe_deliveries[0].at_ps, 20000000U);
    EXPECT_EQ(probe_deliveries[1].route_class, htsim::DragonflyRouteClass::NonMinimal);
    EXPECT_EQ(probe_deliveries[1].router_hops, 3U);
    EXPECT_EQ(probe_deliveries[1].global_hops, 2U);
    EXPECT_LT(probe_deliveries[1].at_ps, 26000000U);

    expectDecisionSequence(
        probe_decisions,
        {{1, htsim::DragonflyPortId{0}},
         {0, htsim::DragonflyPortId{1}},
         {3, htsim::DragonflyPortId{0}},
         {2, std::nullopt},
         {1, htsim::DragonflyPortId{1}},
         {4, htsim::DragonflyPortId{0}},
         {5, htsim::DragonflyPortId{1}},
         {2, std::nullopt}});
    harness.fabric().validateQuiescent();
}

TEST(SsDragonflyFabricTest, AdmissionDropReleasesRouteState) {
    // Not a pre-registered behavioral fixture: a mechanism check that a
    // shared-buffer drop releases the packet's sidecar state.  A tiny
    // buffer forces drops under a two-source flood.
    EventList& event_list = testEventList();
    SsDragonflyConfig config = studyConfig();
    config.rosetta_shared_buffer_bytes = 8320;
    FixtureHarness harness(config);
    FloodSource flood_a(event_list, harness, 0, 6, 5000000);
    FloodSource flood_b(event_list, harness, 1, 7, 5000000);
    runUntil(harness.absolute(20000000));

    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_GT(statistics.dropped_packets, 0U);
    EXPECT_EQ(statistics.delivered_packets + statistics.dropped_packets,
              statistics.injected_packets);
    harness.fabric().validateQuiescent();
}

}  // namespace
