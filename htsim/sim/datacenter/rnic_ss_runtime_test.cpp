// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fat_tree_topology.h"
#include "ns_rosetta_switch.h"
#include "rnic_atlahs_runtime_factory.h"
#include "rnic_ss_runtime.h"

namespace {

constexpr std::uint64_t kLinkBps = UINT64_C(400000000000);
constexpr std::uint32_t kNodeCount = 64;

EventList& testEventList() {
    EventList& event_list = EventList::getTheEventList();
    while (EventList::doNextEvent()) {
    }
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    return event_list;
}

std::unique_ptr<FatTreeTopologyCfg> topologyConfig(mem_b shared_buffer_bytes = 64 << 20) {
    const std::filesystem::path topology_file =
        std::filesystem::path(__FILE__).parent_path() /
        "../../../experiments/rnic_multibaseline/topologies/clos_64_400g.topo";
    auto config = FatTreeTopologyCfg::load(topology_file.lexically_normal().string(),
                                           shared_buffer_bytes, COMPOSITE, FAIR_PRIO);
    if (!config) {
        throw std::runtime_error("failed to load rnic-ss test topology");
    }
    config->set_ns_rosetta_shared_buffer_capacity(shared_buffer_bytes);
    return config;
}

RnicSsRuntimeConfig runtimeConfig(bool unordered = false) {
    return RnicSsRuntimeConfig{
        kLinkBps,
        RnicDataPacketizationConfig(4160, 64),
        64,
        RnicSsSelectiveRepeatConfig{128, timeFromMs(50), 8},
        RnicSsPathSelectionConfig{8, 4, 0, timeFromUs(20U), 0},
        RnicSsCreditConfig{1 << 20},
        1000,
        0,
        1,
        0,
        UINT64_C(0x123456789abcdef0),
        unordered,
        false,
    };
}

void drain(RnicSsRuntime& runtime, std::size_t maximum_events = 500000) {
    std::size_t events = 0;
    while (runtime.hasPendingPhysicalWork() && events < maximum_events) {
        ASSERT_TRUE(EventList::doNextEvent());
        ++events;
    }
    ASSERT_LT(events, maximum_events);
}

TEST(RnicSsRuntimeTest, PhysicalAckBackpressureAndCreditsShareTheSourceSerializer) {
    EventList& event_list = testEventList();
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, runtimeConfig(),
                                         topologyConfig());
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    std::vector<AtlahsFlowId> completions;
    runtime->setup(kNodeCount, [&completions](AtlahsFlowId id) { completions.push_back(id); });

    const AtlahsFlowId flow_id = UINT64_C(0x100000001);
    runtime->send({flow_id, 0, 63, 16000, EventList::now(), 7});
    drain(*runtime);

    ASSERT_EQ(completions, std::vector<AtlahsFlowId>{flow_id});
    runtime->validateQuiescent();
    const RnicSsFlowSnapshot flow = runtime->flow(flow_id);
    EXPECT_EQ(flow.delivered_payload_bytes, 16000U);
    EXPECT_TRUE(flow.completion_notified);

    const RnicSsRuntimeStatistics& stats = runtime->statistics();
    EXPECT_GT(stats.source_data_serializer_packets, 0U);
    EXPECT_GT(stats.source_ack_serializer_packets, 0U);
    EXPECT_GT(stats.source_bp_serializer_packets, 0U);
    EXPECT_GT(stats.source_credit_serializer_packets, 0U);
    EXPECT_EQ(stats.source_priority_violations, 0U);
    EXPECT_EQ(stats.fabric_drops, 0U);
    EXPECT_EQ(stats.sack_retransmissions, 0U);
    EXPECT_EQ(stats.rto_retransmissions, 0U);
    EXPECT_EQ(stats.rto_deadline_pushes, stats.new_data_packets);
    EXPECT_EQ(stats.rto_deadline_stale_pops, stats.rto_deadline_pushes);
    EXPECT_EQ(stats.rto_deadline_due_pops, 0U);
    EXPECT_GT(stats.rto_deadline_heap_high_watermark, 0U);
    EXPECT_EQ(stats.physical_forward_data_observation_ps, 4332800U);
    EXPECT_EQ(stats.physical_last_bp_enable_feedback_ps, 4910080U);
    EXPECT_EQ(stats.physical_bound_control_loop_ps, 9242880U);
    EXPECT_EQ(stats.maximum_bp_enable_fan_in, 448U);
    EXPECT_EQ(stats.maximum_switch_endpoint_pairs, 3584U);
    EXPECT_EQ(stats.maximum_switch_physical_ingresses, 16U);
    EXPECT_EQ(stats.maximum_switch_physical_egresses, 16U);
    EXPECT_EQ(stats.control_loop_window_packets, 128U);
    EXPECT_FALSE(stats.configured_window_below_control_loop);
    EXPECT_EQ(stats.analytical_queue_bound_bytes, 10717864U);
    EXPECT_GT(stats.source_prbs_data_opportunities, 0U);

    ASSERT_FALSE(session->physicalTopology()->switches_lp.empty());
    for (Switch* base : session->physicalTopology()->switches_lp) {
        const auto* sw = dynamic_cast<const NsRosetta*>(base);
        ASSERT_NE(sw, nullptr);
        EXPECT_EQ(sw->buffer_counters().dropped_packets, 0U);
    }
}

TEST(RnicSsRuntimeTest, OrderedModeCompletesWithoutRtoOrLoss) {
    EventList& event_list = testEventList();
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike,
                                         runtimeConfig(false), topologyConfig());
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    std::size_t completions = 0;
    runtime->setup(kNodeCount, [&completions](AtlahsFlowId) { ++completions; });
    runtime->send({UINT64_C(0x200000001), 1, 62, 8192, EventList::now(), 0});
    drain(*runtime);

    EXPECT_EQ(completions, 1U);
    runtime->validateQuiescent();
    EXPECT_EQ(runtime->statistics().sack_retransmissions, 0U);
    EXPECT_EQ(runtime->statistics().rto_retransmissions, 0U);
    EXPECT_EQ(runtime->statistics().fabric_drops, 0U);
}

TEST(RnicSsRuntimeTest, StateTraceUsesPhysicalCreditFeedbackAndInstallsAtQuiescence) {
    EventList& event_list = testEventList();
    const std::filesystem::path trace =
        std::filesystem::temp_directory_path() /
        ("rnic-ss-state-" + std::to_string(reinterpret_cast<std::uintptr_t>(&event_list)) + ".csv");
    std::filesystem::remove(trace);
    std::filesystem::remove(trace.string() + ".tmp");
    const std::filesystem::path goodput =
        std::filesystem::temp_directory_path() /
        ("rnic-ss-goodput-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&event_list)) + ".csv");
    std::filesystem::remove(goodput);
    std::filesystem::remove(goodput.string() + ".tmp");
    RnicSsRuntimeConfig config = runtimeConfig(false);
    config.state_trace_csv = trace.string();
    config.goodput_trace_csv = goodput.string();
    config.goodput_trace_bin_ps = 10000000;
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, std::move(config),
                                         topologyConfig());
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    runtime->setup(kNodeCount, [](AtlahsFlowId) {});
    for (std::uint32_t source = 0; source < 4; ++source) {
        runtime->send(
            {UINT64_C(0x210000001) + source, source, 63, 2U << 20, EventList::now(), source});
    }
    EXPECT_THROW(runtime->writeStateTraceCsv(), std::logic_error);
    EXPECT_THROW(runtime->writeGoodputTraceCsv(), std::logic_error);
    drain(*runtime);

    runtime->validateQuiescent();
    EXPECT_GT(runtime->stateTraceRowCount(), 2U);
    EXPECT_GT(runtime->goodputTraceRowCount(), 0U);
    runtime->writeStateTraceCsv();
    runtime->writeGoodputTraceCsv();
    EXPECT_TRUE(std::filesystem::is_regular_file(trace));
    EXPECT_FALSE(std::filesystem::exists(trace.string() + ".tmp"));
    std::ifstream input(trace);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find(",flow-start,"), std::string::npos);
    EXPECT_NE(text.find(",service-rate-change,"), std::string::npos);
    EXPECT_NE(text.find(",completion,"), std::string::npos);
    std::ifstream goodput_input(goodput);
    const std::string goodput_text((std::istreambuf_iterator<char>(goodput_input)),
                                   std::istreambuf_iterator<char>());
    EXPECT_NE(goodput_text.find("delivered_payload_bytes,goodput_bps"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(goodput.string() + ".tmp"));
    std::filesystem::remove(trace);
    std::filesystem::remove(goodput);
}

TEST(RnicSsRuntimeTest, ExplicitUnorderedSensitivityStillQuiesces) {
    EventList& event_list = testEventList();
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike,
                                         runtimeConfig(true), topologyConfig());
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    std::size_t completions = 0;
    runtime->setup(kNodeCount, [&completions](AtlahsFlowId) { ++completions; });
    runtime->send({UINT64_C(0x300000001), 2, 61, 8192, EventList::now(), 0});
    drain(*runtime);

    EXPECT_EQ(completions, 1U);
    runtime->validateQuiescent();
    EXPECT_EQ(runtime->statistics().rto_retransmissions, 0U);
    EXPECT_EQ(runtime->statistics().fabric_drops, 0U);
}

TEST(RnicSsRuntimeTest, RejectsBufferBelowAnalyticalControlledClosEnvelope) {
    EventList& event_list = testEventList();
    RnicSsRuntimeConfig config = runtimeConfig();
    config.q_hi_bytes = 4U << 20;
    config.q_lo_bytes = 2U << 20;
    config.credit_quantum_packets = 4;
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, std::move(config),
                                         topologyConfig(16 << 20));
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    EXPECT_THROW(runtime->setup(kNodeCount, [](AtlahsFlowId) {}), std::invalid_argument);
}

TEST(RnicSsRuntimeTest, CanonicalAnalyticalBoundIncludesForwardDataAndSerializedFanIn) {
    EventList& event_list = testEventList();
    RnicSsRuntimeConfig config = runtimeConfig();
    config.q_hi_bytes = 4U << 20;
    config.q_lo_bytes = 2U << 20;
    config.credit_quantum_packets = 4;
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, std::move(config),
                                         topologyConfig(32 << 20));
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    runtime->setup(kNodeCount, [](AtlahsFlowId) {});

    const RnicSsRuntimeStatistics& stats = runtime->statistics();
    EXPECT_EQ(stats.physical_forward_data_observation_ps, 4332800U);
    EXPECT_EQ(stats.physical_last_bp_enable_feedback_ps, 4910080U);
    EXPECT_EQ(stats.physical_bound_control_loop_ps, 9242880U);
    EXPECT_EQ(stats.maximum_bp_enable_fan_in, 448U);
    EXPECT_EQ(stats.control_loop_window_packets, 128U);
    EXPECT_EQ(stats.analytical_queue_bound_bytes, 20502208U);
    EXPECT_EQ(stats.analytical_shared_reaction_bytes, 18603200U);
    EXPECT_EQ(stats.minimum_shared_pressure_high_bytes, 14951232U);
    EXPECT_EQ(stats.maximum_shared_pressure_high_bytes, 17246528U);
    EXPECT_LT(stats.analytical_queue_bound_bytes, 32U << 20);
}

TEST(RnicSsRuntimeTest, ImmediateLocalReservationPressuresNextSimultaneousBinding) {
    EventList& event_list = testEventList();
    RnicSsRuntimeConfig config = runtimeConfig(false);
    const std::uint64_t routing_seed = config.routing_seed;
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, std::move(config),
                                         topologyConfig());
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    runtime->setup(kNodeCount, [](AtlahsFlowId) {});

    // Find two pairs sourced by different nodes on one leaf whose hashed
    // first choice is the same spine.  Their source serializers are
    // independent, so both bindings happen before either packet reaches the
    // leaf.  Only the source-leaf reservation can pressure the second choice.
    std::array<std::optional<RnicSsEndpointPair>, 8> first_pair_by_path{};
    std::optional<RnicSsEndpointPair> first_pair;
    std::optional<RnicSsEndpointPair> second_pair;
    std::uint8_t shared_first_choice = 0;
    for (std::uint32_t source = 0; source < 8 && !second_pair.has_value(); ++source) {
        for (std::uint32_t destination = 8; destination < kNodeCount; ++destination) {
            const RnicSsEndpointPair pair{source, destination};
            const auto candidates =
                RnicSsHystereticPathSelector::sampleFourOfEight(pair, routing_seed);
            auto& previous = first_pair_by_path[candidates[0]];
            if (previous.has_value() && previous->source != pair.source) {
                first_pair = *previous;
                second_pair = pair;
                shared_first_choice = candidates[0];
                break;
            }
            if (!previous.has_value()) {
                previous = pair;
            }
        }
    }
    ASSERT_TRUE(first_pair.has_value());
    ASSERT_TRUE(second_pair.has_value());

    runtime->send({UINT64_C(0x600000001), first_pair->source, first_pair->destination, 64U << 10,
                   EventList::now(), 0});
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_EQ(runtime->statistics().ordered_path_bindings, 1U);
    ASSERT_EQ(runtime->statistics().ordered_path_bindings_by_path[shared_first_choice], 1U);

    runtime->send({UINT64_C(0x600000002), second_pair->source, second_pair->destination, 64U << 10,
                   EventList::now(), 1});
    ASSERT_TRUE(EventList::doNextEvent());
    const RnicSsRuntimeStatistics& pressured = runtime->statistics();
    EXPECT_EQ(pressured.ordered_path_bindings, 2U);
    // With no packet arrival or telemetry event between sends, leaving this
    // count at one proves that the second pair saw the immediate local
    // reservation and selected another candidate.
    EXPECT_EQ(pressured.ordered_path_bindings_by_path[shared_first_choice], 1U);

    drain(*runtime);
    runtime->validateQuiescent();
}

TEST(RnicSsRuntimeTest, SimultaneousOrderedPairsCoverAllSpinesWithBoundedLeafSkew) {
    EventList& event_list = testEventList();
    RnicSsRuntimeConfig config = runtimeConfig(false);
    config.q_hi_bytes = 4U << 20;
    config.q_lo_bytes = 2U << 20;
    config.credit_quantum_packets = 4;
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, std::move(config),
                                         topologyConfig());
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    std::size_t completions = 0;
    runtime->setup(kNodeCount, [&completions](AtlahsFlowId) { ++completions; });

    // One cross-leaf pair per node becomes eligible at the same timestamp.
    // The node-to-leaf cable has not delivered DATA yet, so only immediate
    // source-leaf route reservations can make these choices visible to one
    // another.  Eight choices per leaf should exercise every spine without
    // recreating the old lowest-numeric-spine startup hotspot.
    for (std::uint32_t source = 0; source < kNodeCount; ++source) {
        const std::uint32_t destination = (source + 8) % kNodeCount;
        runtime->send({UINT64_C(0x500000000) + source, source, destination, 64U << 10,
                       EventList::now(), source});
    }
    drain(*runtime, 1000000);

    EXPECT_EQ(completions, kNodeCount);
    runtime->validateQuiescent();
    const RnicSsRuntimeStatistics& stats = runtime->statistics();
    EXPECT_EQ(stats.ordered_path_bindings, kNodeCount);
    const auto bounds = std::minmax_element(stats.ordered_path_bindings_by_path.begin(),
                                            stats.ordered_path_bindings_by_path.end());
    EXPECT_GT(*bounds.first, 0U);
    EXPECT_LE(*bounds.second - *bounds.first, 3U);
    EXPECT_LE(stats.ordered_path_binding_max_leaf_skew, 2U);
    EXPECT_EQ(stats.fabric_drops, 0U);
    EXPECT_EQ(stats.rto_retransmissions, 0U);
    for (Switch* base : session->physicalTopology()->switches_lp) {
        const auto* leaf = dynamic_cast<const NsRosetta*>(base);
        ASSERT_NE(leaf, nullptr);
        EXPECT_EQ(leaf->active_ordered_route_reservations(), 0U);
    }
}

TEST(RnicSsRuntimeTest, SilentLossAfterFinalRetryRaisesOnceWithoutSameTimeLivelock) {
    EventList& event_list = testEventList();
    RnicSsRuntimeConfig config = runtimeConfig();
    config.q_hi_bytes = 1;
    config.q_lo_bytes = 0;
    config.allow_loss_stress = true;
    config.selective_repeat.retransmission_timeout_ps = timeFromUs(10U);
    config.selective_repeat.maximum_retransmissions = 1;
    auto session = makeRnicAtlahsRuntime(event_list, RnicProfile::SlingshotLike, std::move(config),
                                         topologyConfig(1));
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&session->implementation());
    ASSERT_NE(runtime, nullptr);
    runtime->setup(kNodeCount, [](AtlahsFlowId) {});
    runtime->send({UINT64_C(0x400000001), 0, 63, 1, EventList::now(), 0});

    std::string failure;
    std::size_t events = 0;
    while (failure.empty() && events < 100) {
        try {
            ASSERT_TRUE(EventList::doNextEvent());
            ++events;
        } catch (const std::runtime_error& error) {
            failure = error.what();
        }
    }
    EXPECT_NE(failure.find("retry budget exhausted"), std::string::npos);
    EXPECT_NE(failure.find("source=0"), std::string::npos);
    EXPECT_NE(failure.find("destination=63"), std::string::npos);
    EXPECT_LT(events, 100U);
    EXPECT_FALSE(EventList::hasPendingSourceAt(EventList::now()));
    EXPECT_EQ(runtime->statistics().rto_retransmissions, 1U);
    EXPECT_EQ(runtime->statistics().fabric_drops, 2U);
    EXPECT_EQ(runtime->statistics().rto_deadline_pushes, 2U);
    EXPECT_EQ(runtime->statistics().rto_deadline_due_pops, 2U);
}

}  // namespace
