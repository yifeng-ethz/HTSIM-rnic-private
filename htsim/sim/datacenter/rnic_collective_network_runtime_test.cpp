// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "rnic_collective_network_runtime.h"
#include "rnic_collective_route.h"
#include "rnic_port.h"
#include "rnic_wire_serialization.h"

class RnicCollectiveNetworkRuntimeTestPeer {
public:
    static void duplicateCurrentGapNack(
            RnicCollectiveNetworkRuntime& runtime,
            AtlahsFlowId flow_id) {
        runtime.duplicateCurrentGapNackForTesting(flow_id);
    }

    static void duplicateCurrentRepair(
            RnicCollectiveNetworkRuntime& runtime,
            AtlahsFlowId flow_id) {
        runtime.duplicateCurrentRepairForTesting(flow_id);
    }
};

namespace {

class TwoTierCollectiveFixture {
public:
    explicit TwoTierCollectiveFixture(
            std::uint64_t link_capacity_bps = speedFromGbps(100),
            std::uint64_t hop_latency_ps = timeFromNs(100))
        : events(EventList::getTheEventList()),
          access_wire_capacity_bps(link_capacity_bps),
          topology_config(2,
                          32,
                          access_wire_capacity_bps,
                          1 << 20,
                          hop_latency_ps,
                          0,
                          COMPOSITE,
                          FAIR_PRIO) {
        while (EventList::doNextEvent()) {
        }
        topology_config.set_switch_model(FatTreeSwitchModel::NsTm3);
        topology_config.set_ns_tm3_shared_buffer_capacity(1 << 20);
        topology = std::make_unique<FatTreeTopology>(
            &topology_config, nullptr, &events, nullptr);
    }

    RnicCollectiveNetworkConfig runtimeConfig() {
        return {
            access_wire_capacity_bps,
            RnicDataPacketizationConfig(1000, 64),
            RnicRingCamConfig{timeFromUs(4.096),
                              timeFromNs(16),
                              1 << 20},
            0x123456789abcdef0ULL,
            timeFromUs(10.0),
            RnicCollectiveController::kDefaultMarginPpm,
            64,
            [this](std::uint32_t source,
                   std::uint32_t destination,
                   const RnicPacketExtent&) {
                return topology_config.get_two_point_diameter_latency(
                    static_cast<int>(source),
                    static_cast<int>(destination));
            },
        };
    }

    void stepUntil(const std::function<bool()>& predicate) {
        constexpr std::size_t maximum_events = 2000000;
        for (std::size_t event = 0; event < maximum_events; ++event) {
            if (predicate()) {
                return;
            }
            if (!EventList::doNextEvent()) {
                throw std::logic_error(
                    "test EventList emptied before its predicate");
            }
        }
        throw std::logic_error("test exceeded its event budget");
    }

    void drainRuntime(RnicCollectiveNetworkRuntime& runtime) {
        stepUntil([&runtime] {
            return !runtime.hasPendingPhysicalWork();
        });
        runtime.validateQuiescent();
    }

    EventList& events;
    std::uint64_t access_wire_capacity_bps;
    FatTreeTopologyCfg topology_config;
    std::unique_ptr<FatTreeTopology> topology;
};

class CallbackEvent final : public EventSource {
public:
    CallbackEvent(EventList& event_list,
                  std::uint64_t when_ps,
                  std::function<void()> callback)
        : EventSource(event_list, "rnic-cn-test-callback"),
          callback_(std::move(callback)) {
        EventList::sourceIsPending(*this, when_ps);
    }

    void doNextEvent() override { callback_(); }

private:
    std::function<void()> callback_;
};

TEST(RnicCollectiveNetworkRuntimeTest,
     PhysicalAcceptGatesDataAndRetireWaitsForRxLedger) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    std::map<AtlahsFlowId, std::uint64_t> completion_times;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
        completion_times.emplace(flow_id, EventList::now());
    });
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());

    const AtlahsFlowRequest request{
        0x100000001ULL, 0, 31, 2000, EventList::now(), 7};
    runtime.send(request);
    EXPECT_TRUE(runtime.hasPendingPhysicalWork());
    EXPECT_FALSE(runtime.flow(request.flow_id).declaration_dispatched);
    EXPECT_EQ(runtime.flow(request.flow_id).source_payload_bytes_dispatched,
              0U);

    fixture.stepUntil([&] {
        return runtime.flow(request.flow_id).sender_phase
               == RnicSenderGrantGate::Phase::AcceptPendingEffectiveTime;
    });
    const RnicCollectiveFlowSnapshot gated = runtime.flow(request.flow_id);
    EXPECT_EQ(gated.current_wire_rate_bps, 0U);
    EXPECT_EQ(gated.source_payload_bytes_dispatched, 0U);
    EXPECT_FALSE(gated.completion_notified);

    // RETIRE bypasses the DATA Ring-CAM and can arrive first. It must not
    // remove receiver membership until the exact DATA ledger reaches the
    // shared destination serializer boundary.
    fixture.stepUntil([&] {
        return runtime.flow(request.flow_id).retire_received;
    });
    const RnicCollectiveFlowSnapshot retired_early =
        runtime.flow(request.flow_id);
    EXPECT_FALSE(retired_early.delivery_completion_time_ps.has_value());
    EXPECT_FALSE(retired_early.receiver_retired);
    EXPECT_EQ(runtime.receiverActiveFlowCount(request.destination), 1U);

    fixture.stepUntil([&] {
        return runtime.flow(request.flow_id).completion_notified;
    });
    EXPECT_TRUE(runtime.hasPendingPhysicalWork());

    fixture.drainRuntime(runtime);
    const RnicCollectiveFlowSnapshot completed = runtime.flow(request.flow_id);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{request.flow_id}));
    EXPECT_EQ(completion_times.at(request.flow_id),
              *completed.delivery_completion_time_ps);
    EXPECT_EQ(completed.delivered_payload_bytes, 2000U);
    EXPECT_EQ(completed.delivered_wire_bytes, 2192U);
    EXPECT_EQ(completed.delivered_data_packets, 3U);
    EXPECT_EQ(completed.source_payload_bytes_dispatched, 2000U);
    EXPECT_EQ(completed.source_wire_bytes_dispatched, 2192U);
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());
    EXPECT_EQ(completed.source_data_packets_dispatched, 3U);
    ASSERT_TRUE(completed.retirement_completion_time_ps.has_value());
    EXPECT_GT(*completed.retirement_completion_time_ps,
              *completed.delivery_completion_time_ps);
    EXPECT_TRUE(completed.receiver_retired);
    EXPECT_EQ(completed.sender_phase,
              RnicSenderGrantGate::Phase::Retired);
    EXPECT_FALSE(runtime.node(request.source)
                     .txPort().contains(request.flow_id));
    EXPECT_EQ(runtime.receiverActiveFlowCount(request.destination), 0U);
    EXPECT_EQ(runtime.pendingFabricPacketCount(), 0U);
    EXPECT_EQ(runtime.pendingDestinationDataCount(), 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     ConcurrentIncastGrantsNeverExceedReceiverMargin) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
    });

    std::vector<AtlahsFlowId> flow_ids;
    for (std::uint32_t source = 0; source < 4; ++source) {
        const AtlahsFlowId flow_id = 0x200000000ULL + source;
        flow_ids.push_back(flow_id);
        runtime.send({flow_id,
                      source,
                      31,
                      200000,
                      EventList::now(),
                      source});
    }

    fixture.stepUntil([&] {
        for (const AtlahsFlowId flow_id : flow_ids) {
            if (runtime.flow(flow_id).sender_phase
                != RnicSenderGrantGate::Phase::Active) {
                return false;
            }
        }
        return true;
    });
    std::uint64_t aggregate_grant = 0;
    for (const AtlahsFlowId flow_id : flow_ids) {
        aggregate_grant += runtime.flow(flow_id).current_wire_rate_bps;
    }
    EXPECT_LE(aggregate_grant, speedFromGbps(90));
    EXPECT_EQ(runtime.receiverActiveFlowCount(31), 4U);

    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions.size(), flow_ids.size());
    for (const AtlahsFlowId flow_id : flow_ids) {
        EXPECT_TRUE(runtime.flow(flow_id).receiver_retired);
    }
}

TEST(RnicCollectiveNetworkRuntimeTest,
     StateTraceIsSparseAndInstalledOnlyAfterQuiescence) {
    TwoTierCollectiveFixture fixture;
    const std::filesystem::path trace =
        std::filesystem::temp_directory_path()
        / ("rnic-cn-state-"
           + std::to_string(reinterpret_cast<std::uintptr_t>(&fixture))
           + ".csv");
    std::filesystem::remove(trace);
    std::filesystem::remove(trace.string() + ".tmp");
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.state_trace_csv = trace.string();
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
    });
    runtime.send({91, 0, 31, 1000, EventList::now(), 1});
    EXPECT_THROW(runtime.writeStateTraceCsv(), std::logic_error);
    fixture.drainRuntime(runtime);
    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{91}));
    EXPECT_GE(runtime.stateTraceRowCount(), 8U);
    runtime.writeStateTraceCsv();
    EXPECT_TRUE(std::filesystem::is_regular_file(trace));
    EXPECT_FALSE(std::filesystem::exists(trace.string() + ".tmp"));
    std::ifstream input(trace);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find(",flow-start,"), std::string::npos);
    EXPECT_NE(text.find(",declare-observed,"), std::string::npos);
    EXPECT_NE(text.find(",membership-join-wave,"), std::string::npos);
    EXPECT_NE(text.find(",grant-rate-change,"), std::string::npos);
    EXPECT_NE(text.find(",completion,"), std::string::npos);
    EXPECT_NE(text.find(",membership-exit-wave,"), std::string::npos);
    EXPECT_NE(text.find(",retired,"), std::string::npos);
    std::filesystem::remove(trace);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     ZeroPayloadUsesPhysicalDeclareAcceptAndRetireWithoutData) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
    });

    const AtlahsFlowRequest request{
        0x300000001ULL, 1, 30, 0, EventList::now(), 0};
    runtime.send(request);
    fixture.stepUntil([&] { return !completions.empty(); });
    const RnicCollectiveFlowSnapshot logical = runtime.flow(request.flow_id);
    EXPECT_TRUE(logical.retire_received);
    EXPECT_EQ(logical.delivered_payload_bytes, 0U);
    EXPECT_EQ(logical.delivered_wire_bytes, 0U);
    EXPECT_EQ(logical.delivered_data_packets, 0U);
    EXPECT_FALSE(logical.receiver_retired);

    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{request.flow_id}));
    EXPECT_TRUE(runtime.flow(request.flow_id).receiver_retired);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     CompletionCallbackCanSynchronouslyStartAnotherFlow) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    constexpr AtlahsFlowId first_flow_id = 0x400000001ULL;
    constexpr AtlahsFlowId second_flow_id = 0x400000002ULL;
    std::map<AtlahsFlowId, std::size_t> completion_counts;
    std::map<AtlahsFlowId, std::uint64_t> completion_times;
    bool second_flow_started = false;

    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        ++completion_counts[flow_id];
        completion_times.emplace(flow_id, EventList::now());
        if (flow_id == first_flow_id && !second_flow_started) {
            second_flow_started = true;
            runtime.send({second_flow_id,
                          0,
                          31,
                          1500,
                          EventList::now(),
                          12});
        }
    });

    runtime.send(
        {first_flow_id, 0, 31, 1000, EventList::now(), 11});
    fixture.drainRuntime(runtime);

    ASSERT_TRUE(second_flow_started);
    ASSERT_EQ(completion_counts.size(), 2U);
    EXPECT_EQ(completion_counts.at(first_flow_id), 1U);
    EXPECT_EQ(completion_counts.at(second_flow_id), 1U);
    ASSERT_EQ(completion_times.size(), 2U);

    const RnicCollectiveFlowSnapshot first = runtime.flow(first_flow_id);
    const RnicCollectiveFlowSnapshot second = runtime.flow(second_flow_id);
    EXPECT_EQ(second.request.start_time_ps,
              completion_times.at(first_flow_id));
    EXPECT_EQ(completion_times.at(first_flow_id),
              *first.delivery_completion_time_ps);
    EXPECT_EQ(completion_times.at(second_flow_id),
              *second.delivery_completion_time_ps);
    EXPECT_TRUE(first.receiver_retired);
    EXPECT_TRUE(second.receiver_retired);
    EXPECT_EQ(first.sender_phase, RnicSenderGrantGate::Phase::Retired);
    EXPECT_EQ(second.sender_phase, RnicSenderGrantGate::Phase::Retired);
    EXPECT_FALSE(runtime.node(0).txPort().contains(first_flow_id));
    EXPECT_FALSE(runtime.node(0).txPort().contains(second_flow_id));
    EXPECT_EQ(runtime.pendingFabricPacketCount(), 0U);
    EXPECT_EQ(runtime.pendingDestinationDataCount(), 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     RejectsASecondActiveRuntimeInsteadOfSameTimeLivelock) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime first(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    RnicCollectiveNetworkRuntime second(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    first.setup(32, [](AtlahsFlowId) {});

    EXPECT_THROW(
        second.setup(32, [](AtlahsFlowId) {}), std::logic_error);
    EXPECT_TRUE(first.isSetup());
    EXPECT_FALSE(second.isSetup());
    first.validateQuiescent();
}

TEST(RnicCollectiveNetworkRuntimeTest,
     NewlyEligibleControlDoesNotSerializeBeforePublishedBoundary) {
    constexpr std::uint64_t capacity_bps = 7000000000ULL;
    TwoTierCollectiveFixture fixture(capacity_bps);
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.packetization = RnicDataPacketizationConfig(6);
    config.control_wire_bytes = 1;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, std::move(config));
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId data_flow_id = 0x500000001ULL;
    constexpr AtlahsFlowId declaration_flow_id = 0x500000002ULL;
    runtime.send({data_flow_id, 0, 31, 6, EventList::now(), 1});
    fixture.stepUntil([&] {
        return runtime.flow(data_flow_id)
                   .source_data_packets_dispatched == 1;
    });
    const std::uint64_t data_start_ps = EventList::now();
    const std::uint64_t published_data_end_ps =
        runtime.node(0).txPort().physicalSerializerAvailablePs();
    RnicWireSerializationClock data_clock(capacity_bps);
    EXPECT_EQ(data_clock.serialize(data_start_ps, 6).end_ps,
              published_data_end_ps);

    CallbackEvent declare_at_boundary(
        fixture.events, published_data_end_ps, [&] {
            runtime.send({declaration_flow_id,
                          0,
                          31,
                          0,
                          EventList::now(),
                          2});
        });
    fixture.stepUntil([&] {
        return runtime.contains(declaration_flow_id)
               && runtime.flow(declaration_flow_id)
                      .declaration_dispatched;
    });

    RnicWireSerializationClock fresh_control_clock(capacity_bps);
    const std::uint64_t causal_control_end_ps =
        fresh_control_clock.serialize(published_data_end_ps, 1).end_ps;
    EXPECT_EQ(EventList::now(), causal_control_end_ps);
    fixture.drainRuntime(runtime);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     OptionalQueueTraceRecordsPhysicalEgressStateWithoutChangingCompletion) {
    const std::filesystem::path trace_path =
        std::filesystem::temp_directory_path()
        / "rnic-cn-ns-tm3-queue-trace-test.csv";
    std::filesystem::remove(trace_path);

    TwoTierCollectiveFixture fixture;
    {
        RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
        config.queue_trace_csv = trace_path.string();
        RnicCollectiveNetworkRuntime runtime(
            fixture.events, *fixture.topology, std::move(config));
        std::vector<AtlahsFlowId> completions;
        runtime.setup(32, [&](AtlahsFlowId flow_id) {
            completions.push_back(flow_id);
        });
        constexpr AtlahsFlowId flow_id = 0x600000001ULL;
        runtime.send({flow_id, 0, 31, 2000, EventList::now(), 3});
        fixture.drainRuntime(runtime);
        EXPECT_EQ(completions,
                  (std::vector<AtlahsFlowId>{flow_id}));
    }

    std::ifstream trace(trace_path);
    ASSERT_TRUE(trace.is_open());
    const std::string contents(
        (std::istreambuf_iterator<char>(trace)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find(
                  "time_ps,tier,switch_id,ingress_id,egress_id,"),
              std::string::npos);
    EXPECT_NE(contents.find(",enqueue,"), std::string::npos);
    EXPECT_NE(contents.find(",dequeue,"), std::string::npos);
    EXPECT_NE(contents.find(",serialization-complete,"),
              std::string::npos);
    EXPECT_NE(contents.find(",leaf,"), std::string::npos);
    std::filesystem::remove(trace_path);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     LateTailUsesOnePhysicalGapNackAndOneSelectiveRepair) {
    // Keep the production Delta=4.096 us and margin=0.9.  A deliberately
    // stale first calibration makes only the original one-packet tail late;
    // the retry uses the construction-equivalent packet-specific baseline.
    TwoTierCollectiveFixture fixture(
        speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    auto calibration_calls = std::make_shared<std::uint32_t>(0);
    config.calibrated_transit_ps =
        [&fixture, calibration_calls](
                std::uint32_t source,
                std::uint32_t destination,
                const RnicPacketExtent& extent) -> std::uint64_t {
            if ((*calibration_calls)++ == 0) {
                return std::uint64_t{0};
            }
            return rnicCollectiveNoQueueTransitPs(
                fixture.topology_config, source, destination, extent);
        };
    config.maximum_repair_retries = 2;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
    });

    constexpr AtlahsFlowId flow_id = 0x700000001ULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 4});
    fixture.drainRuntime(runtime);

    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.source_payload_bytes_dispatched, 500U);
    EXPECT_EQ(flow.source_data_packets_dispatched, 1U);
    EXPECT_EQ(flow.delivered_payload_bytes, 500U);
    EXPECT_EQ(flow.delivered_data_packets, 1U);
    EXPECT_EQ(flow.late_data_packets, 1U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.selective_retransmissions, 1U);
    EXPECT_EQ(flow.selective_retransmission_wire_bytes, 564U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(flow.missing_data_packets, 0U);
    EXPECT_EQ(flow.ready_out_of_order_packets, 0U);
    EXPECT_TRUE(flow.retire_received);
    EXPECT_TRUE(flow.receiver_retired);
    const RnicCollectiveRecoveryStatistics& recovery =
        runtime.recoveryStatistics();
    EXPECT_EQ(recovery.late_data_packets, 1U);
    EXPECT_EQ(recovery.gap_nacks_dispatched, 1U);
    EXPECT_EQ(recovery.gap_nacks_received, 1U);
    EXPECT_EQ(recovery.selective_retransmissions, 1U);
    EXPECT_EQ(recovery.duplicate_gap_nacks_ignored, 0U);
    EXPECT_EQ(recovery.duplicate_data_packets_ignored, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     RepeatedLateRepairStopsAtTheConfiguredRetryBound) {
    // Every transmission uses the same deliberately stale baseline, so both
    // the original and retry are late.  The retry guard must fail on attempt
    // one without silently widening Delta or the margin.
    TwoTierCollectiveFixture fixture(
        speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.calibrated_transit_ps =
        [](std::uint32_t,
           std::uint32_t,
           const RnicPacketExtent&) -> std::uint64_t {
            return 0;
        };
    config.maximum_repair_retries = 1;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, std::move(config));
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId flow_id = 0x700000002ULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 5});

    try {
        fixture.stepUntil([] { return false; });
        FAIL() << "expected the bounded selective repair to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find(
                      "selective repair exhausted maximum retries"),
                  std::string::npos);
        EXPECT_NE(message.find("attempt=1"), std::string::npos);
        EXPECT_NE(message.find("maximum=1"), std::string::npos);
    }

    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.late_data_packets, 2U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.selective_retransmissions, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(flow.delivered_payload_bytes, 0U);
    EXPECT_EQ(flow.delivered_wire_bytes, 0U);
    EXPECT_EQ(flow.delivered_data_packets, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     DuplicatePhysicalGapNackAndRepairAreIdempotent) {
    TwoTierCollectiveFixture fixture(
        speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    auto calibration_calls = std::make_shared<std::uint32_t>(0);
    config.calibrated_transit_ps =
        [&fixture, calibration_calls](
                std::uint32_t source,
                std::uint32_t destination,
                const RnicPacketExtent& extent) -> std::uint64_t {
            if ((*calibration_calls)++ == 0) {
                return 0;
            }
            return rnicCollectiveNoQueueTransitPs(
                fixture.topology_config, source, destination, extent);
        };
    config.maximum_repair_retries = 2;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
    });

    constexpr AtlahsFlowId flow_id = 0x700000003ULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 6});
    fixture.stepUntil([&] {
        return runtime.flow(flow_id).gap_nacks_dispatched == 1;
    });
    RnicCollectiveNetworkRuntimeTestPeer::duplicateCurrentGapNack(
        runtime, flow_id);
    fixture.stepUntil([&] {
        return runtime.flow(flow_id).selective_retransmissions == 1;
    });
    RnicCollectiveNetworkRuntimeTestPeer::duplicateCurrentRepair(
        runtime, flow_id);
    fixture.drainRuntime(runtime);

    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.source_payload_bytes_dispatched, 500U);
    EXPECT_EQ(flow.source_wire_bytes_dispatched, 564U);
    EXPECT_EQ(flow.source_data_packets_dispatched, 1U);
    EXPECT_EQ(flow.delivered_payload_bytes, 500U);
    EXPECT_EQ(flow.delivered_wire_bytes, 564U);
    EXPECT_EQ(flow.delivered_data_packets, 1U);
    EXPECT_EQ(flow.late_data_packets, 1U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 2U);
    EXPECT_EQ(flow.gap_nacks_received, 2U);
    EXPECT_EQ(flow.duplicate_gap_nacks_ignored, 1U);
    EXPECT_EQ(flow.selective_retransmissions, 2U);
    EXPECT_EQ(flow.selective_retransmission_wire_bytes, 1128U);
    EXPECT_EQ(flow.duplicate_data_packets_ignored, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(flow.missing_data_packets, 0U);
    EXPECT_EQ(flow.ready_out_of_order_packets, 0U);
    EXPECT_TRUE(flow.receiver_retired);

    const RnicCollectiveRecoveryStatistics& recovery =
        runtime.recoveryStatistics();
    EXPECT_EQ(recovery.duplicate_gap_nacks_ignored, 1U);
    EXPECT_EQ(recovery.duplicate_data_packets_ignored, 1U);
    EXPECT_EQ(recovery.gap_nacks_dispatched,
              recovery.gap_nacks_received);
}

}  // namespace
