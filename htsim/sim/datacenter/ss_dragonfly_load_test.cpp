// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// Deterministic fixtures for the ss-dragonfly load harness (HTSIM-29,
// HTSIM-30).  Every exact value asserted in the F7 to F12 fixtures was
// pre-registered by hand in
// docs/ss-dragonfly-fabric/fixture-expectations.md (load-harness
// section) before this file existed.  The remaining tests are mechanism
// and parsing checks, not pre-registered fixtures.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "eventlist.h"
#include "ss_dragonfly_fabric.h"
#include "ss_dragonfly_load.h"

namespace {

constexpr std::uint64_t kMerlinWireBytes = 9038;
constexpr std::uint64_t kMerlinHeaderBytes = 90;
constexpr std::uint64_t kChunk8MiB = 8388608;
constexpr simtime_picosec kMerlinStageSumPs = 1673040;
constexpr simtime_picosec kMerlinChunkServicePs = 340417280;  // F for N = 938

EventList& testEventList() {
    EventList& event_list = EventList::getTheEventList();
    while (EventList::doNextEvent()) {
    }
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    return event_list;
}

void runUntil(simtime_picosec stop_ps) {
    while (true) {
        const std::optional<simtime_picosec> next = EventList::nextEventTime();
        if (!next.has_value() || *next > stop_ps) {
            return;
        }
        ASSERT_TRUE(EventList::doNextEvent());
    }
}

// Mirrors topologies/ss_dragonfly/merlin_a100_5node_4x200g.topo with the
// wave-19 capture framing (wire 9038), per the load-fixture fabric
// registration.
SsDragonflyConfig merlinLoadConfig() {
    SsDragonflyConfig config;
    config.hosts_per_router = 20;
    config.routers_per_group = 1;
    config.global_links_per_router = 0;
    config.group_count = 1;
    config.host_link = {200000000000ULL, 300000};
    config.switch_pipeline_delay_ps = 350000;
    config.rosetta_shared_buffer_bytes = 4194304;
    config.maximum_wire_packet_bytes = kMerlinWireBytes;
    config.routing_seed = UINT64_C(383530136519245852);
    return config;
}

// Mirrors topologies/ss_dragonfly/p2a2h1g3_200g.topo (F12 runs there).
SsDragonflyConfig studyConfig() {
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

struct LoadDeliveryRecord {
    std::uint32_t source;
    std::uint32_t destination;
    simtime_picosec at_ps;  // relative to the harness origin
    htsim::DragonflyRouteClass route_class;
    std::uint8_t router_hops;
    std::uint8_t global_hops;
};

// Shares one never-rewinding event list across fixtures, so times are
// offsets from the harness's construction origin exactly as in the F1 to
// F6 fixture harness.
class LoadFixtureHarness {
public:
    explicit LoadFixtureHarness(SsDragonflyConfig config)
        : _event_list(testEventList()),
          _origin_ps(EventList::now()),
          _fabric(_event_list, config) {
        _fabric.setDeliveryObserver([this](const SsDragonflyPacket& packet,
                                           const htsim::DragonflyRouteState& state,
                                           simtime_picosec at_ps) {
            deliveries.push_back(LoadDeliveryRecord{
                packet.src(), packet.dst(), at_ps - _origin_ps, state.route_class,
                state.router_hops, state.global_hops});
            dispatch.noteDelivery(packet, at_ps);
        });
        _fabric.setDropObserver(
            [this](const SsDragonflyPacket&, simtime_picosec at_ps) {
                dispatch.noteDrop(at_ps);
            });
    }

    EventList& eventList() { return _event_list; }
    SsDragonflyTopology& fabric() { return _fabric; }
    simtime_picosec originPs() const { return _origin_ps; }
    simtime_picosec absolute(simtime_picosec offset_ps) const {
        return _origin_ps + offset_ps;
    }

    SsDragonflyPacedSource& addPaced(std::uint32_t flow_id,
                                     std::uint32_t source,
                                     std::uint32_t destination,
                                     simtime_picosec start_offset_ps,
                                     simtime_picosec stop_offset_ps,
                                     std::uint64_t wire_bytes,
                                     std::uint64_t header_bytes,
                                     std::uint64_t chunk_packets,
                                     std::uint64_t offered_bps) {
        auto source_ptr = std::make_unique<SsDragonflyPacedSource>(
            _event_list, _fabric, flow_id, source, destination,
            absolute(start_offset_ps), absolute(stop_offset_ps),
            htsim::DragonflyRoutingControl::Adaptive0, true, wire_bytes, header_bytes,
            chunk_packets, offered_bps);
        SsDragonflyPacedSource& reference = *source_ptr;
        dispatch.addSource(reference);
        _sources.push_back(std::move(source_ptr));
        return reference;
    }

    SsDragonflyClosedLoopSource& addClosed(std::uint32_t flow_id,
                                           std::uint32_t source,
                                           std::uint32_t destination,
                                           simtime_picosec start_offset_ps,
                                           simtime_picosec stop_offset_ps,
                                           std::uint64_t wire_bytes,
                                           std::uint64_t header_bytes,
                                           std::uint64_t chunk_packets,
                                           std::uint64_t think_ps) {
        auto source_ptr = std::make_unique<SsDragonflyClosedLoopSource>(
            _event_list, _fabric, flow_id, source, destination,
            absolute(start_offset_ps), absolute(stop_offset_ps),
            htsim::DragonflyRoutingControl::Adaptive0, true, wire_bytes, header_bytes,
            chunk_packets, think_ps);
        SsDragonflyClosedLoopSource& reference = *source_ptr;
        dispatch.addSource(reference);
        _sources.push_back(std::move(source_ptr));
        return reference;
    }

    // Chunk completion instants relative to the origin, in chunk order.
    std::vector<simtime_picosec> completionOffsets(
        const SsDragonflyLoadSource& source) const {
        std::vector<simtime_picosec> offsets;
        for (const SsDragonflyChunkCompletion& completion : source.completedChunks()) {
            offsets.push_back(completion.completion_ps - _origin_ps);
        }
        return offsets;
    }
    std::vector<simtime_picosec> firstInjectionOffsets(
        const SsDragonflyLoadSource& source) const {
        std::vector<simtime_picosec> offsets;
        for (const SsDragonflyChunkCompletion& completion : source.completedChunks()) {
            offsets.push_back(completion.first_injection_ps - _origin_ps);
        }
        return offsets;
    }

    std::vector<LoadDeliveryRecord> deliveries;
    SsDragonflyLoadDispatch dispatch;

private:
    EventList& _event_list;
    simtime_picosec _origin_ps;
    SsDragonflyTopology _fabric;
    std::vector<std::unique_ptr<SsDragonflyLoadSource>> _sources;
};

// F7: paced solo, exact chunk-completion sequence.
TEST(SsDragonflyLoadFixtureTest, F7PacedSoloExactChunkSequence) {
    LoadFixtureHarness harness(merlinLoadConfig());
    SsDragonflyPacedSource& flow = harness.addPaced(
        0, 16, 6, 0, 2100000000, kMerlinWireBytes, kMerlinHeaderBytes,
        ssDragonflyChunkPackets(kChunk8MiB, kMerlinWireBytes - kMerlinHeaderBytes),
        UINT64_C(100000000000));
    runUntil(harness.absolute(2200000000));

    EXPECT_EQ(flow.chunkPackets(), 938U);
    EXPECT_EQ(flow.injectedPackets(), 2905U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.injected_packets, 2905U);
    EXPECT_EQ(statistics.delivered_packets, 2905U);
    EXPECT_EQ(statistics.dropped_packets, 0U);
    EXPECT_EQ(harness.completionOffsets(flow),
              (std::vector<simtime_picosec>{679161520, 1357373040, 2035584560}));
    harness.fabric().validateQuiescent();
}

// F8: paced non-divisible rate, exact ceiling schedule.
TEST(SsDragonflyLoadFixtureTest, F8PacedCeilingScheduleExactDeliveries) {
    LoadFixtureHarness harness(merlinLoadConfig());
    SsDragonflyPacedSource& flow = harness.addPaced(
        0, 16, 6, 0, 15000000, kMerlinWireBytes, kMerlinHeaderBytes,
        ssDragonflyChunkPackets(26844, kMerlinWireBytes - kMerlinHeaderBytes),
        UINT64_C(30000000000));
    runUntil(harness.absolute(20000000));

    EXPECT_EQ(flow.chunkPackets(), 3U);
    EXPECT_EQ(flow.injectedPackets(), 7U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.delivered_packets, 7U);
    EXPECT_EQ(statistics.dropped_packets, 0U);
    std::vector<simtime_picosec> delivery_offsets;
    for (const LoadDeliveryRecord& record : harness.deliveries) {
        delivery_offsets.push_back(record.at_ps);
    }
    EXPECT_EQ(delivery_offsets,
              (std::vector<simtime_picosec>{1673040, 4083174, 6493307, 8903440,
                                            11313574, 13723707, 16133840}));
    EXPECT_EQ(harness.completionOffsets(flow),
              (std::vector<simtime_picosec>{6493307, 13723707}));
    harness.fabric().validateQuiescent();
}

// F9: closed-loop solo, exact cadence under a declared think time.
TEST(SsDragonflyLoadFixtureTest, F9ClosedLoopSoloCadence) {
    LoadFixtureHarness harness(merlinLoadConfig());
    SsDragonflyClosedLoopSource& flow = harness.addClosed(
        0, 16, 6, 0, 3600000000, kMerlinWireBytes, kMerlinHeaderBytes,
        ssDragonflyChunkPackets(kChunk8MiB, kMerlinWireBytes - kMerlinHeaderBytes),
        UINT64_C(1256438000));
    runUntil(harness.absolute(3700000000));

    EXPECT_EQ(flow.injectedPackets(), 2814U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.delivered_packets, 2814U);
    EXPECT_EQ(statistics.dropped_packets, 0U);
    EXPECT_EQ(harness.completionOffsets(flow),
              (std::vector<simtime_picosec>{340417280, 1937272560, 3534127840}));
    EXPECT_EQ(harness.firstInjectionOffsets(flow),
              (std::vector<simtime_picosec>{0, 1596855280, 3193710560}));
    harness.fabric().validateQuiescent();
}

// F10: two flows to distinct ports, exact independence (the i2 mirror).
TEST(SsDragonflyLoadFixtureTest, F10TwoFlowDistinctPortsIndependent) {
    LoadFixtureHarness harness(merlinLoadConfig());
    const std::uint64_t chunk_packets =
        ssDragonflyChunkPackets(kChunk8MiB, kMerlinWireBytes - kMerlinHeaderBytes);
    SsDragonflyClosedLoopSource& flow0 =
        harness.addClosed(0, 8, 5, 0, 4750000000, kMerlinWireBytes, kMerlinHeaderBytes,
                          chunk_packets, UINT64_C(1858227000));
    SsDragonflyClosedLoopSource& flow1 =
        harness.addClosed(1, 16, 6, 0, 4750000000, kMerlinWireBytes, kMerlinHeaderBytes,
                          chunk_packets, UINT64_C(1256438000));
    runUntil(harness.absolute(4800000000));

    EXPECT_EQ(flow0.injectedPackets(), 2814U);
    EXPECT_EQ(flow1.injectedPackets(), 2814U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.injected_packets, 5628U);
    EXPECT_EQ(statistics.delivered_packets, 5628U);
    EXPECT_EQ(statistics.dropped_packets, 0U);
    // Each flow's completion sequence equals its solo formula exactly:
    // zero cross-flow displacement through the shared switch.
    EXPECT_EQ(harness.completionOffsets(flow0),
              (std::vector<simtime_picosec>{340417280, 2539061560, 4737705840}));
    EXPECT_EQ(harness.completionOffsets(flow1),
              (std::vector<simtime_picosec>{340417280, 1937272560, 3534127840}));
    harness.fabric().validateQuiescent();
}

// F11: staggered join on the single switch, exact join-unharmed.
TEST(SsDragonflyLoadFixtureTest, F11StaggeredJoinSingleSwitchUnharmed) {
    LoadFixtureHarness harness(merlinLoadConfig());
    const std::uint64_t chunk_packets =
        ssDragonflyChunkPackets(kChunk8MiB, kMerlinWireBytes - kMerlinHeaderBytes);
    SsDragonflyPacedSource& flow0 =
        harness.addPaced(0, 8, 5, 0, 2600000000, kMerlinWireBytes, kMerlinHeaderBytes,
                         chunk_packets, UINT64_C(100000000000));
    SsDragonflyPacedSource& flow1 =
        harness.addPaced(1, 16, 6, 500000000, 2600000000, kMerlinWireBytes,
                         kMerlinHeaderBytes, chunk_packets, UINT64_C(100000000000));
    runUntil(harness.absolute(2700000000));

    EXPECT_EQ(flow0.injectedPackets(), 3596U);
    EXPECT_EQ(flow1.injectedPackets(), 2905U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.dropped_packets, 0U);
    EXPECT_EQ(harness.completionOffsets(flow0),
              (std::vector<simtime_picosec>{679161520, 1357373040, 2035584560}));
    EXPECT_EQ(harness.completionOffsets(flow1),
              (std::vector<simtime_picosec>{1179161520, 1857373040, 2535584560}));
    harness.fabric().validateQuiescent();
}

// F12: paced inter-group flow on the study fabric.
TEST(SsDragonflyLoadFixtureTest, F12PacedInterGroupMinimal) {
    LoadFixtureHarness harness(studyConfig());
    SsDragonflyPacedSource& flow = harness.addPaced(
        0, 0, 6, 0, 6400000, 4160, 64, ssDragonflyChunkPackets(40960, 4096),
        UINT64_C(100000000000));
    runUntil(harness.absolute(15000000));

    EXPECT_EQ(flow.chunkPackets(), 10U);
    EXPECT_EQ(flow.injectedPackets(), 20U);
    const SsDragonflyStatistics& statistics = harness.fabric().statistics();
    EXPECT_EQ(statistics.delivered_packets, 20U);
    EXPECT_EQ(statistics.dropped_packets, 0U);
    for (const LoadDeliveryRecord& record : harness.deliveries) {
        EXPECT_EQ(record.route_class, htsim::DragonflyRouteClass::Minimal);
        EXPECT_EQ(record.router_hops, 1U);
        EXPECT_EQ(record.global_hops, 1U);
    }
    EXPECT_EQ(harness.completionOffsets(flow),
              (std::vector<simtime_picosec>{5794400, 9122400}));
    harness.fabric().validateQuiescent();
}

// Mechanism and parsing checks below; not pre-registered fixtures.

TEST(SsDragonflyLoadParsingTest, FlowSpecParsesAllFields) {
    const SsDragonflyLoadFlowSpec spec = parseSsDragonflyFlowSpec(
        "src=8,dst=5,start_ps=278092,offered_bps=130000000000,think_ps=7");
    EXPECT_EQ(spec.source, 8U);
    EXPECT_EQ(spec.destination, 5U);
    EXPECT_EQ(spec.start_ps, 278092U);
    ASSERT_TRUE(spec.offered_bps.has_value());
    EXPECT_EQ(*spec.offered_bps, UINT64_C(130000000000));
    ASSERT_TRUE(spec.think_ps.has_value());
    EXPECT_EQ(*spec.think_ps, 7U);

    const SsDragonflyLoadFlowSpec minimal = parseSsDragonflyFlowSpec("src=1,dst=2");
    EXPECT_EQ(minimal.start_ps, 0U);
    EXPECT_FALSE(minimal.offered_bps.has_value());
    EXPECT_FALSE(minimal.think_ps.has_value());
}

TEST(SsDragonflyLoadParsingTest, FlowSpecRejectsMalformedInput) {
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("dst=1"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1,dst=2,src=3"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1,dst=2,rate=5"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1,,dst=2"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1,dst"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1,dst="), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=1,dst=2x"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=-1,dst=2"), std::invalid_argument);
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=+1,dst=2"), std::invalid_argument);
    // Host fields are 32-bit; a wider value must be rejected, not
    // silently narrowed.
    EXPECT_THROW(parseSsDragonflyFlowSpec("src=4294967296,dst=2"),
                 std::invalid_argument);
}

TEST(SsDragonflyLoadParsingTest, SourceModeParses) {
    EXPECT_EQ(parseSsDragonflySourceMode("open"), SsDragonflySourceMode::OpenLoop);
    EXPECT_EQ(parseSsDragonflySourceMode("paced"), SsDragonflySourceMode::Paced);
    EXPECT_EQ(parseSsDragonflySourceMode("closed"), SsDragonflySourceMode::ClosedLoop);
    EXPECT_THROW(parseSsDragonflySourceMode("greedy"), std::invalid_argument);
}

TEST(SsDragonflyLoadArithmeticTest, ChunkPacketsCeils) {
    EXPECT_EQ(ssDragonflyChunkPackets(8388608, 8948), 938U);
    EXPECT_EQ(ssDragonflyChunkPackets(26844, 8948), 3U);
    EXPECT_EQ(ssDragonflyChunkPackets(1, 8948), 1U);
    EXPECT_THROW(ssDragonflyChunkPackets(0, 8948), std::invalid_argument);
    EXPECT_THROW(ssDragonflyChunkPackets(8948, 0), std::invalid_argument);
}

TEST(SsDragonflyLoadArithmeticTest, PacedOffsetIsExactCeilingOfTotal) {
    // Divisible rate: exact multiples, no rounding at all.
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(100000000000), 0), 0U);
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(100000000000), 1), 723040U);
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(100000000000), 2905), 2100431200U);
    // Non-divisible rate: per-total ceiling, so every third packet is
    // exact and no drift accumulates (the F8 schedule).
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(30000000000), 1), 2410134U);
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(30000000000), 2), 4820267U);
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(30000000000), 3), 7230400U);
    EXPECT_EQ(ssDragonflyPacedOffset(9038, UINT64_C(30000000000), 6), 14460800U);
    EXPECT_THROW(ssDragonflyPacedOffset(9038, 0, 1), std::invalid_argument);
    EXPECT_THROW(ssDragonflyPacedOffset(0, 1, 1), std::invalid_argument);
    // The 64-bit wire-bits product is safe only because wire extents are
    // held to HTSIM's uint16_t packet bound; the function enforces it.
    EXPECT_THROW(ssDragonflyPacedOffset(65536, UINT64_C(100000000000), 1),
                 std::invalid_argument);
}

TEST(SsDragonflyLoadMechanismTest, PacedSourceRejectsRatesBeyondTheHostLink) {
    LoadFixtureHarness harness(merlinLoadConfig());
    EXPECT_THROW(harness.addPaced(0, 16, 6, 0, 1000000, kMerlinWireBytes,
                                  kMerlinHeaderBytes, 0, UINT64_C(200000000001)),
                 std::invalid_argument);
    EXPECT_THROW(harness.addPaced(0, 16, 6, 0, 1000000, kMerlinWireBytes,
                                  kMerlinHeaderBytes, 0, 0),
                 std::invalid_argument);
}

TEST(SsDragonflyLoadMechanismTest, ClosedLoopRequiresAChunk) {
    LoadFixtureHarness harness(merlinLoadConfig());
    EXPECT_THROW(harness.addClosed(0, 16, 6, 0, 1000000, kMerlinWireBytes,
                                   kMerlinHeaderBytes, 0, 0),
                 std::invalid_argument);
}

TEST(SsDragonflyLoadMechanismTest, DispatchRejectsDuplicatePairs) {
    LoadFixtureHarness harness(merlinLoadConfig());
    harness.addPaced(0, 16, 6, 0, 1000000, kMerlinWireBytes, kMerlinHeaderBytes, 0,
                     UINT64_C(100000000000));
    EXPECT_THROW(harness.addPaced(1, 16, 6, 0, 1000000, kMerlinWireBytes,
                                  kMerlinHeaderBytes, 0, UINT64_C(100000000000)),
                 std::invalid_argument);
}

}  // namespace
