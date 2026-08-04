// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
    static void duplicateCurrentGapNack(RnicCollectiveNetworkRuntime& runtime,
                                        AtlahsFlowId flow_id) {
        runtime.duplicateCurrentGapNackForTesting(flow_id);
    }

    static void duplicateCurrentRetransmission(RnicCollectiveNetworkRuntime& runtime,
                                               AtlahsFlowId flow_id) {
        runtime.duplicateCurrentRetransmissionForTesting(flow_id);
    }

    static void dropOriginalData(RnicCollectiveNetworkRuntime& runtime,
                                 AtlahsFlowId flow_id,
                                 std::uint64_t packet_index) {
        runtime.dropOriginalDataForTesting(flow_id, packet_index);
    }

    static void dropDataAttempt(RnicCollectiveNetworkRuntime& runtime,
                                AtlahsFlowId flow_id,
                                std::uint64_t packet_index,
                                std::uint32_t transmission_attempt) {
        runtime.dropDataAttemptForTesting(flow_id, packet_index, transmission_attempt);
    }

    static void duplicateOriginalData(RnicCollectiveNetworkRuntime& runtime,
                                      AtlahsFlowId flow_id,
                                      std::uint64_t packet_index) {
        runtime.duplicateOriginalDataForTesting(flow_id, packet_index);
    }

    static void replayResolvedGapNack(RnicCollectiveNetworkRuntime& runtime,
                                      AtlahsFlowId flow_id,
                                      std::uint64_t packet_index) {
        runtime.replayResolvedGapNackForTesting(flow_id, packet_index);
    }

    static void redeclareFlow(RnicCollectiveNetworkRuntime& runtime, AtlahsFlowId flow_id) {
        runtime.redeclareFlowForTesting(flow_id);
    }

    static std::uint64_t maxOriginalRelease(const RnicCollectiveNetworkRuntime& runtime,
                                            AtlahsFlowId flow_id) {
        return runtime.maxOriginalReleaseForTesting(flow_id);
    }

    static std::uint64_t finalOriginalRelease(const RnicCollectiveNetworkRuntime& runtime,
                                              AtlahsFlowId flow_id) {
        return runtime.finalOriginalReleaseForTesting(flow_id);
    }

    static std::optional<std::uint64_t> publishedRetireDeadline(
        const RnicCollectiveNetworkRuntime& runtime,
        AtlahsFlowId flow_id) {
        return runtime.publishedRetireDeadlineForTesting(flow_id);
    }

    static std::optional<std::uint64_t> firstGapObservation(
        const RnicCollectiveNetworkRuntime& runtime,
        AtlahsFlowId flow_id) {
        return runtime.firstGapObservationForTesting(flow_id);
    }

    static std::optional<std::uint64_t> firstGapDecision(
        const RnicCollectiveNetworkRuntime& runtime,
        AtlahsFlowId flow_id) {
        return runtime.firstGapDecisionForTesting(flow_id);
    }

    static std::optional<std::uint64_t> retryDispatch(const RnicCollectiveNetworkRuntime& runtime,
                                                      AtlahsFlowId flow_id,
                                                      std::uint32_t transmission_attempt) {
        return runtime.retryDispatchForTesting(flow_id, transmission_attempt);
    }
};

namespace {

class TwoTierCollectiveFixture {
public:
    explicit TwoTierCollectiveFixture(std::uint64_t link_capacity_bps = speedFromGbps(100),
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
        topology = std::make_unique<FatTreeTopology>(&topology_config, nullptr, &events, nullptr);
    }

    RnicCollectiveNetworkConfig runtimeConfig() {
        return {
            access_wire_capacity_bps,
            RnicDataPacketizationConfig(1000, 64),
            RnicRingCamConfig{timeFromUs(4.096), timeFromNs(16), 1 << 20},
            0x123456789abcdef0ULL,
            timeFromUs(10.0),
            RnicCollectiveController::kDefaultMarginPpm,
            64,
            [this](std::uint32_t source, std::uint32_t destination, const RnicPacketExtent&) {
                return topology_config.get_two_point_diameter_latency(
                    static_cast<int>(source), static_cast<int>(destination));
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
                throw std::logic_error("test EventList emptied before its predicate");
            }
        }
        throw std::logic_error("test exceeded its event budget");
    }

    void drainRuntime(RnicCollectiveNetworkRuntime& runtime) {
        stepUntil([&runtime] { return !runtime.hasPendingPhysicalWork(); });
        runtime.validateQuiescent();
    }

    EventList& events;
    std::uint64_t access_wire_capacity_bps;
    FatTreeTopologyCfg topology_config;
    std::unique_ptr<FatTreeTopology> topology;
};

class CallbackEvent final : public EventSource {
public:
    CallbackEvent(EventList& event_list, std::uint64_t when_ps, std::function<void()> callback)
        : EventSource(event_list, "rnic-cn-test-callback"), callback_(std::move(callback)) {
        EventList::sourceIsPending(*this, when_ps);
    }

    void doNextEvent() override { callback_(); }

private:
    std::function<void()> callback_;
};

TEST(RnicCollectiveNetworkRuntimeTest, DeclareAndGoOpensDataImmediatelyAtTheLedgerAllocation) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    std::map<AtlahsFlowId, std::uint64_t> completion_times;
    runtime.setup(32, [&](AtlahsFlowId flow_id) {
        completions.push_back(flow_id);
        completion_times.emplace(flow_id, EventList::now());
    });
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());

    const AtlahsFlowRequest request{0x100000001ULL, 0, 31, 2000, EventList::now(), 7};
    runtime.send(request);
    EXPECT_TRUE(runtime.hasPendingPhysicalWork());
    EXPECT_FALSE(runtime.flow(request.flow_id).declaration_dispatched);
    EXPECT_EQ(runtime.flow(request.flow_id).sender_phase, RnicSenderGrantGate::Phase::Idle);
    EXPECT_EQ(runtime.flow(request.flow_id).source_payload_bytes_dispatched, 0U);

    fixture.stepUntil([&] { return runtime.flow(request.flow_id).declaration_dispatched; });
    const RnicCollectiveFlowSnapshot declared = runtime.flow(request.flow_id);
    // Declare-and-go: the gate is Active the moment the DECLARE leaves the
    // source serializer. wire = 2192, budget = 225000, nflow = 9743, and
    // per book 1.4 the sole fractional declarer paces exactly its request
    // floor(9e10 * 9743 / 1e6), never the undersubscribed receiver surplus.
    EXPECT_EQ(declared.sender_phase, RnicSenderGrantGate::Phase::Active);
    EXPECT_EQ(declared.declared_nflow_ppm, 9743U);
    EXPECT_EQ(declared.current_wire_rate_bps, UINT64_C(876870000));

    // RETIRE bypasses the DATA Ring-CAM and arrives first. It must not
    // remove receiver membership until the exact DATA ledger reaches the
    // shared destination serializer boundary.
    fixture.stepUntil([&] { return runtime.flow(request.flow_id).retire_received; });
    const RnicCollectiveFlowSnapshot retired_early = runtime.flow(request.flow_id);
    EXPECT_FALSE(retired_early.delivery_completion_time_ps.has_value());
    EXPECT_FALSE(retired_early.receiver_retired);
    EXPECT_EQ(runtime.receiverActiveFlowCount(request.destination), 1U);

    // Delivery, retirement, and completion may collapse into one event now
    // that no join gate or lease outlives the exact RX ledger.
    fixture.stepUntil([&] { return runtime.flow(request.flow_id).completion_notified; });

    fixture.drainRuntime(runtime);
    const RnicCollectiveFlowSnapshot completed = runtime.flow(request.flow_id);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{request.flow_id}));
    EXPECT_EQ(completion_times.at(request.flow_id), *completed.delivery_completion_time_ps);
    EXPECT_EQ(completed.delivered_payload_bytes, 2000U);
    EXPECT_EQ(completed.delivered_wire_bytes, 2192U);
    EXPECT_EQ(completed.delivered_data_packets, 3U);
    EXPECT_EQ(completed.source_payload_bytes_dispatched, 2000U);
    EXPECT_EQ(completed.source_wire_bytes_dispatched, 2192U);
    EXPECT_FALSE(runtime.hasPendingPhysicalWork());
    EXPECT_EQ(completed.source_data_packets_dispatched, 3U);
    ASSERT_TRUE(completed.retirement_completion_time_ps.has_value());
    EXPECT_GE(*completed.retirement_completion_time_ps, *completed.delivery_completion_time_ps);
    EXPECT_TRUE(completed.receiver_retired);
    EXPECT_EQ(completed.sender_phase, RnicSenderGrantGate::Phase::Retired);
    // Every feedback ACK the receiver generated was physically delivered;
    // releases inside the first dwnd window, whose boundary snapshot
    // precedes the first admission, generate none.
    EXPECT_EQ(completed.rate_feedback_acks_received,
              completed.rate_feedback_acks_generated);
    EXPECT_LE(completed.rate_feedback_acks_generated, completed.delivered_data_packets);
    EXPECT_FALSE(runtime.node(request.source).txPort().contains(request.flow_id));
    EXPECT_EQ(runtime.receiverActiveFlowCount(request.destination), 0U);
    EXPECT_EQ(runtime.pendingFabricPacketCount(), 0U);
    EXPECT_EQ(runtime.pendingDestinationDataCount(), 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, SimultaneousIncastReadsOneLedgerSumWithoutOversend) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    std::vector<AtlahsFlowId> flow_ids;
    for (std::uint32_t source = 0; source < 4; ++source) {
        const AtlahsFlowId flow_id = 0x200000000ULL + source;
        flow_ids.push_back(flow_id);
        runtime.send({flow_id, source, 31, 2000000, EventList::now(), source});
    }

    // The whole same-timestamp DECLARE batch registers in the shared
    // reservation ledger before any gate opens, so all four whole-flow
    // joiners start at margin * C / 4 with no cold-start overshoot.
    fixture.stepUntil([&] {
        for (const AtlahsFlowId flow_id : flow_ids) {
            if (runtime.flow(flow_id).sender_phase != RnicSenderGrantGate::Phase::Active) {
                return false;
            }
        }
        return true;
    });
    std::uint64_t aggregate_grant = 0;
    for (const AtlahsFlowId flow_id : flow_ids) {
        EXPECT_EQ(runtime.flow(flow_id).current_wire_rate_bps, UINT64_C(22500000000));
        aggregate_grant += runtime.flow(flow_id).current_wire_rate_bps;
    }
    EXPECT_EQ(aggregate_grant, speedFromGbps(90));
    // The gates open at DECLARE dispatch; receiver membership follows one
    // one-way transit later.
    fixture.stepUntil([&] { return runtime.receiverActiveFlowCount(31) == 4; });

    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions.size(), flow_ids.size());
    for (const AtlahsFlowId flow_id : flow_ids) {
        const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
        EXPECT_TRUE(flow.receiver_retired);
        // Feedback rides every resequenced ACK once a nonempty boundary
        // snapshot exists; each generated ACK is physically delivered.
        EXPECT_GT(flow.rate_feedback_acks_generated, 0U);
        EXPECT_EQ(flow.rate_feedback_acks_received, flow.rate_feedback_acks_generated);
        EXPECT_LE(flow.rate_feedback_acks_generated, flow.delivered_data_packets);
    }
    // The per-dwnd reservation invariant is observable: no late admission
    // and no gap NACK in a valid run, including startup.
    const RnicCollectiveRecoveryStatistics& recovery = runtime.recoveryStatistics();
    EXPECT_EQ(recovery.late_data_packets, 0U);
    EXPECT_EQ(recovery.gap_nacks_dispatched, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, IncumbentStepsDownAtADwndBoundaryWhenAJoinerDeclares) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, fixture.runtimeConfig());
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId incumbent = 0x210000001ULL;
    constexpr AtlahsFlowId joiner = 0x210000002ULL;
    runtime.send({incumbent, 0, 31, 2000000, EventList::now(), 1});
    fixture.stepUntil([&] {
        return runtime.flow(incumbent).sender_phase == RnicSenderGrantGate::Phase::Active;
    });
    // A sole whole flow owns the whole margin-derated bottleneck.
    EXPECT_EQ(runtime.flow(incumbent).current_wire_rate_bps, speedFromGbps(90));
    fixture.stepUntil([&] {
        return runtime.flow(incumbent).rate_feedback_acks_received != 0;
    });

    // wire = 22472 fits the 225000-byte control-round-trip budget, so the
    // joiner declares nflow = 99876 ppm and takes only that fraction.
    runtime.send({joiner, 1, 31, 21000, EventList::now(), 2});
    fixture.stepUntil([&] { return runtime.flow(joiner).declaration_dispatched; });
    const RnicCollectiveFlowSnapshot joined = runtime.flow(joiner);
    EXPECT_EQ(joined.sender_phase, RnicSenderGrantGate::Phase::Active);
    // floor(floor(9e16 / 1099876) * 99876 / 1e6).
    EXPECT_EQ(joined.current_wire_rate_bps, UINT64_C(8172594001));

    // The incumbent applies the joined-membership snapshot at a sender-local
    // dwnd boundary: floor(9e16 / 1099876) scaled by one whole flow.
    fixture.stepUntil([&] {
        return runtime.flow(incumbent).current_wire_rate_bps == UINT64_C(81827405998);
    });
    // After the joiner retires, later snapshots restore the sole-flow rate.
    fixture.stepUntil([&] { return runtime.flow(joiner).receiver_retired; });
    fixture.stepUntil([&] {
        return runtime.flow(incumbent).current_wire_rate_bps == speedFromGbps(90);
    });
    fixture.drainRuntime(runtime);
    const RnicCollectiveRecoveryStatistics& recovery = runtime.recoveryStatistics();
    EXPECT_EQ(recovery.late_data_packets, 0U);
    EXPECT_EQ(recovery.gap_nacks_dispatched, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, RetryProceedsAcrossAMembershipEpochChange) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.maximum_retransmissions = 2;
    config.retransmission_rto_ps = timeFromUs(100.0);
    RnicCollectiveNetworkRuntime runtime(
        fixture.events, *fixture.topology, std::move(config));
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId incumbent = 0x220000001ULL;
    constexpr AtlahsFlowId joiner = 0x220000002ULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(
        runtime, incumbent, 0);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(
        runtime, incumbent, 0, 1);
    runtime.send({incumbent, 0, 31, 500, EventList::now(), 1});
    fixture.stepUntil([&] {
        return runtime.flow(incumbent).deterministic_retransmissions == 1;
    });
    ASSERT_EQ(runtime.flow(incumbent).source_payload_bytes_dispatched, 500U);

    runtime.send({joiner, 1, 31, 2000000, EventList::now(), 2});
    fixture.stepUntil([&] {
        return runtime.flow(joiner).sender_phase ==
               RnicSenderGrantGate::Phase::Active;
    });

    // Membership changed under the incumbent's open gap. DECLAREs never
    // expire, so nothing gates the bounded watchdog retry: attempt two is
    // dispatched after the RTO and closes the flow.
    fixture.stepUntil([&] {
        return runtime.flow(incumbent).deterministic_retransmissions == 2;
    });
    fixture.drainRuntime(runtime);
    const RnicCollectiveFlowSnapshot retried = runtime.flow(incumbent);
    EXPECT_EQ(retried.delivered_payload_bytes, 500U);
    EXPECT_EQ(retried.deterministic_retransmissions, 2U);
    EXPECT_EQ(retried.maximum_retry_attempt_observed, 2U);
    EXPECT_TRUE(retried.receiver_retired);
    EXPECT_TRUE(runtime.flow(joiner).receiver_retired);
}

TEST(RnicCollectiveNetworkRuntimeTest, EgressCompositionAndRttRebalancerReclaimSlack) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    runtime.setup(32, [](AtlahsFlowId) {});

    // Sender 0 fans out to two destinations. Destination 31 is
    // oversubscribed by two whole-flow competitors; destination 30 is sole.
    constexpr AtlahsFlowId hungry = 0x260000001ULL;
    constexpr AtlahsFlowId satisfied = 0x260000002ULL;
    constexpr AtlahsFlowId competitor_one = 0x260000003ULL;
    constexpr AtlahsFlowId competitor_two = 0x260000004ULL;
    runtime.send({hungry, 0, 31, 2000000, EventList::now(), 1});
    runtime.send({satisfied, 0, 30, 2000000, EventList::now(), 2});
    runtime.send({competitor_one, 1, 31, 2000000, EventList::now(), 3});
    runtime.send({competitor_two, 2, 31, 2000000, EventList::now(), 4});

    fixture.stepUntil([&] {
        return runtime.flow(hungry).declaration_dispatched &&
               runtime.flow(satisfied).declaration_dispatched;
    });
    // Egress composition at DECLARE time: the first flow saw one pending
    // destination (whole flow); the second saw two and requested
    // round(1e6 * (C_egress / 2) / (margin * C_receiver)) = 555556 ppm.
    EXPECT_EQ(runtime.flow(hungry).declared_nflow_ppm, 1000000U);
    EXPECT_EQ(runtime.flow(satisfied).declared_nflow_ppm, 555556U);
    // Ledger allocations: 31 carries three whole-ish flows, 30 is sole and
    // request-capped.
    EXPECT_EQ(runtime.flow(hungry).current_wire_rate_bps, UINT64_C(30000000000));
    EXPECT_EQ(runtime.flow(satisfied).current_wire_rate_bps, UINT64_C(50000040000));

    // First RTT boundary (2 * dwnd): the hungry lane is under-granted and
    // keeps its declaration; the satisfied lane absorbs the egress slack
    // C_egress - 3e10 - 50000040000 and re-declares
    // round(1e6 * 7e10 / 9e10) = 777778 via NFLOW_UPDATE.
    fixture.stepUntil([&] {
        return runtime.flow(satisfied).nflow_updates_dispatched == 1;
    });
    EXPECT_EQ(runtime.flow(satisfied).declared_nflow_ppm, 777778U);
    EXPECT_EQ(runtime.flow(hungry).declared_nflow_ppm, 1000000U);

    // Within two RTTs the raised declaration has passed through the
    // receiver's window snapshots and paces the satisfied lane.
    fixture.stepUntil([&] {
        return runtime.flow(satisfied).current_wire_rate_bps == UINT64_C(70000020000);
    });
    EXPECT_LE(EventList::now(), timeFromUs(40.0));

    fixture.drainRuntime(runtime);
    // Converged: exactly one update, no oscillation, and zero recovery
    // events anywhere in the run.
    EXPECT_EQ(runtime.flow(satisfied).nflow_updates_dispatched, 1U);
    EXPECT_EQ(runtime.flow(satisfied).declared_nflow_ppm, 777778U);
    EXPECT_EQ(runtime.flow(hungry).nflow_updates_dispatched, 0U);
    const RnicCollectiveRecoveryStatistics& recovery = runtime.recoveryStatistics();
    EXPECT_EQ(recovery.late_data_packets, 0U);
    EXPECT_EQ(recovery.gap_nacks_dispatched, 0U);
    EXPECT_EQ(recovery.stale_declarations_ignored, 0U);
    EXPECT_EQ(recovery.stale_nflow_updates_ignored, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, DeclareForARetiredFlowIsIgnoredWithoutThrow) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x230000001ULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 1});
    fixture.drainRuntime(runtime);
    ASSERT_TRUE(runtime.flow(flow_id).receiver_retired);
    ASSERT_EQ(runtime.recoveryStatistics().stale_declarations_ignored, 0U);

    // A repeat DECLARE for retired membership is counted and dropped; the
    // receiver never throws and the runtime returns to quiescence.
    RnicCollectiveNetworkRuntimeTestPeer::redeclareFlow(runtime, flow_id);
    fixture.drainRuntime(runtime);
    EXPECT_EQ(runtime.recoveryStatistics().stale_declarations_ignored, 1U);
    EXPECT_EQ(runtime.receiverActiveFlowCount(31), 0U);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    EXPECT_EQ(runtime.flow(flow_id).sender_phase, RnicSenderGrantGate::Phase::Retired);
}

TEST(RnicCollectiveNetworkRuntimeTest, RedeclareForAnActiveFlowIsAnIdempotentFeedbackNoOp) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId flow_id = 0x240000001ULL;
    runtime.send({flow_id, 0, 31, 2000000, EventList::now(), 1});
    // An arrived ACK may still be held for its dwnd boundary; wait until the
    // snapshot has actually been applied.
    fixture.stepUntil([&] { return runtime.flow(flow_id).membership_epoch == 1; });

    RnicCollectiveNetworkRuntimeTestPeer::redeclareFlow(runtime, flow_id);
    fixture.drainRuntime(runtime);
    // The repeat DECLARE re-sent the current window feedback without any
    // membership mutation: one epoch, no stale-declaration count, and every
    // generated ACK was delivered.
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.membership_epoch, 1U);
    EXPECT_EQ(runtime.recoveryStatistics().stale_declarations_ignored, 0U);
    EXPECT_EQ(flow.rate_feedback_acks_received, flow.rate_feedback_acks_generated);
    EXPECT_GT(flow.rate_feedback_acks_generated, 0U);
    EXPECT_TRUE(flow.receiver_retired);
}

TEST(RnicCollectiveNetworkRuntimeTest, ZeroPayloadUsesPhysicalDeclareAndRetireWithoutData) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    const AtlahsFlowRequest request{0x300000001ULL, 1, 30, 0, EventList::now(), 0};
    runtime.send(request);
    fixture.stepUntil([&] { return !completions.empty(); });
    const RnicCollectiveFlowSnapshot logical = runtime.flow(request.flow_id);
    EXPECT_TRUE(logical.retire_received);
    EXPECT_EQ(logical.delivered_payload_bytes, 0U);
    EXPECT_EQ(logical.delivered_wire_bytes, 0U);
    EXPECT_EQ(logical.delivered_data_packets, 0U);
    EXPECT_TRUE(logical.receiver_retired);

    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{request.flow_id}));
    EXPECT_TRUE(runtime.flow(request.flow_id).receiver_retired);
    EXPECT_EQ(runtime.flow(request.flow_id).rate_feedback_acks_generated, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, CompletionCallbackCanSynchronouslyStartAnotherFlow) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
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
            runtime.send({second_flow_id, 0, 31, 1500, EventList::now(), 12});
        }
    });

    runtime.send({first_flow_id, 0, 31, 1000, EventList::now(), 11});
    fixture.drainRuntime(runtime);

    ASSERT_TRUE(second_flow_started);
    ASSERT_EQ(completion_counts.size(), 2U);
    EXPECT_EQ(completion_counts.at(first_flow_id), 1U);
    EXPECT_EQ(completion_counts.at(second_flow_id), 1U);
    ASSERT_EQ(completion_times.size(), 2U);

    const RnicCollectiveFlowSnapshot first = runtime.flow(first_flow_id);
    const RnicCollectiveFlowSnapshot second = runtime.flow(second_flow_id);
    EXPECT_EQ(second.request.start_time_ps, completion_times.at(first_flow_id));
    EXPECT_EQ(completion_times.at(first_flow_id), *first.delivery_completion_time_ps);
    EXPECT_EQ(completion_times.at(second_flow_id), *second.delivery_completion_time_ps);
    EXPECT_TRUE(first.receiver_retired);
    EXPECT_TRUE(second.receiver_retired);
    EXPECT_EQ(first.sender_phase, RnicSenderGrantGate::Phase::Retired);
    EXPECT_EQ(second.sender_phase, RnicSenderGrantGate::Phase::Retired);
    EXPECT_FALSE(runtime.node(0).txPort().contains(first_flow_id));
    EXPECT_FALSE(runtime.node(0).txPort().contains(second_flow_id));
    EXPECT_EQ(runtime.pendingFabricPacketCount(), 0U);
    EXPECT_EQ(runtime.pendingDestinationDataCount(), 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, RejectsASecondActiveRuntimeInsteadOfSameTimeLivelock) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime first(fixture.events, *fixture.topology, fixture.runtimeConfig());
    RnicCollectiveNetworkRuntime second(fixture.events, *fixture.topology, fixture.runtimeConfig());
    first.setup(32, [](AtlahsFlowId) {});

    EXPECT_THROW(second.setup(32, [](AtlahsFlowId) {}), std::logic_error);
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
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId data_flow_id = 0x500000001ULL;
    constexpr AtlahsFlowId declaration_flow_id = 0x500000002ULL;
    runtime.send({data_flow_id, 0, 31, 6, EventList::now(), 1});
    fixture.stepUntil(
        [&] { return runtime.flow(data_flow_id).source_data_packets_dispatched == 1; });
    // The request-capped pace launches the DATA on a fractional pacer
    // boundary, so the port's published availability is the ground truth
    // for the busy interval the second flow's DECLARE must not preempt.
    const std::uint64_t published_data_end_ps =
        runtime.node(0).txPort().physicalSerializerAvailablePs();
    EXPECT_GT(published_data_end_ps, EventList::now());

    CallbackEvent declare_at_boundary(fixture.events, published_data_end_ps, [&] {
        runtime.send({declaration_flow_id, 0, 31, 0, EventList::now(), 2});
    });
    fixture.stepUntil([&] {
        return runtime.contains(declaration_flow_id) &&
               runtime.flow(declaration_flow_id).declaration_dispatched;
    });

    RnicWireSerializationClock fresh_control_clock(capacity_bps);
    const std::uint64_t causal_control_end_ps =
        fresh_control_clock.serialize(published_data_end_ps, 1).end_ps;
    EXPECT_EQ(EventList::now(), causal_control_end_ps);
    fixture.drainRuntime(runtime);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     LateTailUsesOnePhysicalGapNackAndOneDeterministicRetransmission) {
    // Keep the production Delta=4.096 us and margin=0.9.  A deliberately
    // stale first calibration makes only the original one-packet tail late;
    // the retry uses the construction-equivalent packet-specific baseline.
    TwoTierCollectiveFixture fixture(speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    auto calibration_calls = std::make_shared<std::uint32_t>(0);
    config.calibrated_transit_ps = [&fixture, calibration_calls](
                                       std::uint32_t source, std::uint32_t destination,
                                       const RnicPacketExtent& extent) -> std::uint64_t {
        if ((*calibration_calls)++ == 0) {
            return std::uint64_t{0};
        }
        return rnicCollectiveNoQueueTransitPs(fixture.topology_config, source, destination, extent);
    };
    config.maximum_retransmissions = 2;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

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
    EXPECT_EQ(flow.deterministic_retransmissions, 1U);
    EXPECT_EQ(flow.deterministic_retransmission_wire_bytes, 564U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(flow.missing_data_packets, 0U);
    EXPECT_EQ(flow.ready_out_of_order_packets, 0U);
    EXPECT_TRUE(flow.retire_received);
    EXPECT_TRUE(flow.receiver_retired);
    const RnicCollectiveRecoveryStatistics& recovery = runtime.recoveryStatistics();
    EXPECT_EQ(recovery.late_data_packets, 1U);
    EXPECT_EQ(recovery.gap_nacks_dispatched, 1U);
    EXPECT_EQ(recovery.gap_nacks_received, 1U);
    EXPECT_EQ(recovery.deterministic_retransmissions, 1U);
    EXPECT_EQ(recovery.duplicate_gap_nacks_ignored, 0U);
    EXPECT_EQ(recovery.duplicate_data_packets_ignored, 0U);
    EXPECT_EQ(RnicCollectiveNetworkRuntimeTestPeer::firstGapObservation(runtime, flow_id),
              RnicCollectiveNetworkRuntimeTestPeer::firstGapDecision(runtime, flow_id));
}

TEST(RnicCollectiveNetworkRuntimeTest, PostResequenceSuccessorDetectsARealMiddlePacketDrop) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000004ULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 1);
    runtime.send({flow_id, 0, 31, 2500, EventList::now(), 7});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.delivered_payload_bytes, 2500U);
    EXPECT_EQ(flow.delivered_data_packets, 3U);
    EXPECT_EQ(flow.late_data_packets, 0U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.deterministic_retransmissions, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_TRUE(flow.receiver_retired);
    ASSERT_TRUE(
        RnicCollectiveNetworkRuntimeTestPeer::firstGapObservation(runtime, flow_id).has_value());
    ASSERT_TRUE(
        RnicCollectiveNetworkRuntimeTestPeer::firstGapDecision(runtime, flow_id).has_value());
    // Equation (20) uses epsilon=0 here: the complete release batch is
    // processed before the same-tick physical NACK decision.
    EXPECT_EQ(RnicCollectiveNetworkRuntimeTestPeer::firstGapObservation(runtime, flow_id),
              RnicCollectiveNetworkRuntimeTestPeer::firstGapDecision(runtime, flow_id));
}

TEST(RnicCollectiveNetworkRuntimeTest, RetireDetectsARealFinalPacketDropAtThePublishedDeadline) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000005ULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 0);
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 8});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.delivered_payload_bytes, 500U);
    EXPECT_EQ(flow.delivered_data_packets, 1U);
    EXPECT_EQ(flow.late_data_packets, 0U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.deterministic_retransmissions, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_TRUE(flow.receiver_retired);
    const auto published =
        RnicCollectiveNetworkRuntimeTestPeer::publishedRetireDeadline(runtime, flow_id);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(*published,
              RnicCollectiveNetworkRuntimeTestPeer::maxOriginalRelease(runtime, flow_id));
    EXPECT_EQ(RnicCollectiveNetworkRuntimeTestPeer::firstGapObservation(runtime, flow_id),
              published);
    EXPECT_EQ(RnicCollectiveNetworkRuntimeTestPeer::firstGapDecision(runtime, flow_id), published);
}

TEST(RnicCollectiveNetworkRuntimeTest, RepeatedLateRetransmissionStopsAtTheConfiguredLimit) {
    // Every transmission uses the same deliberately stale baseline, so both
    // the original and retry are late.  The retry guard must fail on attempt
    // one without silently widening Delta or the margin.
    TwoTierCollectiveFixture fixture(speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.calibrated_transit_ps = [](std::uint32_t, std::uint32_t,
                                      const RnicPacketExtent&) -> std::uint64_t { return 0; };
    config.maximum_retransmissions = 1;
    // A loose deadline keeps every recovery event inside the first few dwnd
    // windows; window snapshots do not participate in this retry bound.
    config.control_deadline_ps = timeFromUs(100.0);
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId flow_id = 0x700000002ULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 5});

    try {
        fixture.stepUntil([] { return false; });
        FAIL() << "expected bounded deterministic retransmission to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("deterministic retransmission exhausted maximum attempts"),
                  std::string::npos);
        EXPECT_NE(message.find("attempt=1"), std::string::npos);
        EXPECT_NE(message.find("maximum=1"), std::string::npos);
    }

    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.late_data_packets, 2U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.deterministic_retransmissions, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(flow.delivered_payload_bytes, 0U);
    EXPECT_EQ(flow.delivered_wire_bytes, 0U);
    EXPECT_EQ(flow.delivered_data_packets, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, DuplicatePhysicalGapNackAndRetransmissionAreIdempotent) {
    TwoTierCollectiveFixture fixture(speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    auto calibration_calls = std::make_shared<std::uint32_t>(0);
    config.calibrated_transit_ps = [&fixture, calibration_calls](
                                       std::uint32_t source, std::uint32_t destination,
                                       const RnicPacketExtent& extent) -> std::uint64_t {
        if ((*calibration_calls)++ == 0) {
            return 0;
        }
        return rnicCollectiveNoQueueTransitPs(fixture.topology_config, source, destination, extent);
    };
    config.maximum_retransmissions = 2;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000003ULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 6});
    fixture.stepUntil([&] { return runtime.flow(flow_id).gap_nacks_dispatched == 1; });
    RnicCollectiveNetworkRuntimeTestPeer::duplicateCurrentGapNack(runtime, flow_id);
    fixture.stepUntil([&] { return runtime.flow(flow_id).deterministic_retransmissions == 1; });
    RnicCollectiveNetworkRuntimeTestPeer::duplicateCurrentRetransmission(runtime, flow_id);
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
    EXPECT_EQ(flow.deterministic_retransmissions, 2U);
    EXPECT_EQ(flow.deterministic_retransmission_wire_bytes, 1128U);
    EXPECT_EQ(flow.duplicate_data_packets_ignored, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(flow.missing_data_packets, 0U);
    EXPECT_EQ(flow.ready_out_of_order_packets, 0U);
    EXPECT_TRUE(flow.receiver_retired);

    const RnicCollectiveRecoveryStatistics& recovery = runtime.recoveryStatistics();
    EXPECT_EQ(recovery.duplicate_gap_nacks_ignored, 1U);
    EXPECT_EQ(recovery.duplicate_data_packets_ignored, 1U);
    EXPECT_EQ(recovery.gap_nacks_dispatched, recovery.gap_nacks_received);
}

TEST(RnicCollectiveNetworkRuntimeTest, Exact4097ByteTailKeepsProductionDeltaWithoutFalseGap) {
    TwoTierCollectiveFixture fixture(speedFromGbps(400));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.packetization = RnicDataPacketizationConfig(4160, 64);
    auto calibrated_extents = std::make_shared<std::vector<std::uint64_t>>();
    config.calibrated_transit_ps = [&fixture, calibrated_extents](std::uint32_t source,
                                                                  std::uint32_t destination,
                                                                  const RnicPacketExtent& extent) {
        calibrated_extents->push_back(extent.wireBytes());
        return rnicCollectiveNoQueueTransitPs(fixture.topology_config, source, destination, extent);
    };
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000006ULL;
    runtime.send({flow_id, 0, 31, 4097, EventList::now(), 9});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.source_data_packets_dispatched, 2U);
    EXPECT_EQ(flow.delivered_payload_bytes, 4097U);
    EXPECT_EQ(flow.late_data_packets, 0U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 0U);
    EXPECT_EQ(flow.deterministic_retransmissions, 0U);
    EXPECT_EQ(*calibrated_extents, (std::vector<std::uint64_t>{4160, 65}));
    const auto published =
        RnicCollectiveNetworkRuntimeTestPeer::publishedRetireDeadline(runtime, flow_id);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(*published,
              RnicCollectiveNetworkRuntimeTestPeer::maxOriginalRelease(runtime, flow_id));
    EXPECT_GE(*published,
              RnicCollectiveNetworkRuntimeTestPeer::finalOriginalRelease(runtime, flow_id));
}

TEST(RnicCollectiveNetworkRuntimeTest, TwoIndependentOriginalGapsEachProduceOneExactRetry) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000007ULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 1);
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 2);
    runtime.send({flow_id, 0, 31, 4000, EventList::now(), 10});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.delivered_payload_bytes, 4000U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 2U);
    EXPECT_EQ(flow.gap_nacks_received, 2U);
    EXPECT_EQ(flow.deterministic_retransmissions, 2U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 1U);
}

TEST(RnicCollectiveNetworkRuntimeTest,
     DroppedPhysicalRetryUsesOnlyTheBoundedSenderWatchdogForAttemptTwo) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.maximum_retransmissions = 2;
    config.retransmission_rto_ps = timeFromUs(20.0);
    const std::uint64_t retry_rto_ps = config.retransmission_rto_ps;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000008ULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 0);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, flow_id, 0, 1);
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 11});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.deterministic_retransmissions, 2U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 2U);
    const auto first = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, flow_id, 1);
    const auto second = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, flow_id, 2);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_GE(*second, *first + retry_rto_ps);
}

TEST(RnicCollectiveNetworkRuntimeTest, DroppedPhysicalRetriesStopAtTheBoundedSenderWatchdogLimit) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    config.maximum_retransmissions = 2;
    config.retransmission_rto_ps = timeFromUs(20.0);
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    runtime.setup(32, [](AtlahsFlowId) {});

    constexpr AtlahsFlowId flow_id = 0x70000000bULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 0);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, flow_id, 0, 1);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, flow_id, 0, 2);
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 14});

    try {
        fixture.stepUntil([] { return false; });
        FAIL() << "expected the bounded sender watchdog to exhaust";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("exhausted maximum attempts after sender RTO"), std::string::npos);
        EXPECT_NE(message.find("attempt=2"), std::string::npos);
        EXPECT_NE(message.find("maximum=2"), std::string::npos);
    }

    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.gap_nacks_dispatched, 1U);
    EXPECT_EQ(flow.gap_nacks_received, 1U);
    EXPECT_EQ(flow.deterministic_retransmissions, 2U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 2U);
    EXPECT_EQ(flow.delivered_payload_bytes, 0U);
    EXPECT_EQ(flow.delivered_wire_bytes, 0U);
    EXPECT_EQ(flow.delivered_data_packets, 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, DuplicateOriginalBeforeReleaseIsIdempotent) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x700000009ULL;
    RnicCollectiveNetworkRuntimeTestPeer::duplicateOriginalData(runtime, flow_id, 0);
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 12});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.delivered_data_packets, 1U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 0U);
    EXPECT_EQ(flow.deterministic_retransmissions, 0U);
    EXPECT_EQ(flow.duplicate_data_packets_ignored, 1U);
}

TEST(RnicCollectiveNetworkRuntimeTest, PhysicalGapResolvedBeforeAReorderedGapNackLeavesATombstone) {
    TwoTierCollectiveFixture fixture;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology,
                                         fixture.runtimeConfig());
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x70000000cULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 0);
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 15});
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot resolved = runtime.flow(flow_id);
    ASSERT_TRUE(resolved.receiver_retired);
    ASSERT_EQ(resolved.deterministic_retransmissions, 1U);
    ASSERT_EQ(resolved.duplicate_gap_nacks_ignored, 0U);

    // Inject the stale control only after the successful retry's physical
    // GAP_RESOLVED has reached the sender.  RX state is already gone, so the
    // surviving TX tombstone, not a receiver-side shortcut, must absorb it.
    RnicCollectiveNetworkRuntimeTestPeer::replayResolvedGapNack(runtime, flow_id, 0);
    fixture.drainRuntime(runtime);

    const RnicCollectiveFlowSnapshot replayed = runtime.flow(flow_id);
    EXPECT_EQ(replayed.gap_nacks_dispatched, 2U);
    EXPECT_EQ(replayed.gap_nacks_received, 2U);
    EXPECT_EQ(replayed.duplicate_gap_nacks_ignored, 1U);
    EXPECT_EQ(replayed.deterministic_retransmissions, 1U);
    EXPECT_EQ(replayed.maximum_retry_attempt_observed, 1U);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
}

TEST(RnicCollectiveNetworkRuntimeTest, DuplicateLateRetryIsIgnoredBeforeAttemptTwoSucceeds) {
    TwoTierCollectiveFixture fixture(speedFromGbps(100), timeFromUs(2.0));
    RnicCollectiveNetworkConfig config = fixture.runtimeConfig();
    auto calibration_calls = std::make_shared<std::uint32_t>(0);
    config.calibrated_transit_ps = [&fixture, calibration_calls](
                                       std::uint32_t source, std::uint32_t destination,
                                       const RnicPacketExtent& extent) -> std::uint64_t {
        const std::uint32_t call = (*calibration_calls)++;
        if (call < 3) {
            return 0;
        }
        return rnicCollectiveNoQueueTransitPs(fixture.topology_config, source, destination, extent);
    };
    config.maximum_retransmissions = 2;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });

    constexpr AtlahsFlowId flow_id = 0x70000000aULL;
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 13});
    fixture.stepUntil([&] { return runtime.flow(flow_id).deterministic_retransmissions == 1; });
    RnicCollectiveNetworkRuntimeTestPeer::duplicateCurrentRetransmission(runtime, flow_id);
    fixture.drainRuntime(runtime);

    ASSERT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const RnicCollectiveFlowSnapshot flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.late_data_packets, 2U);
    EXPECT_EQ(flow.gap_nacks_dispatched, 2U);
    EXPECT_EQ(flow.gap_nacks_received, 2U);
    EXPECT_EQ(flow.deterministic_retransmissions, 3U);
    EXPECT_EQ(flow.duplicate_data_packets_ignored, 1U);
    EXPECT_EQ(flow.maximum_retry_attempt_observed, 2U);
}

}  // namespace
