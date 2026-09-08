// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
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

    static std::optional<std::uint64_t> terminalResolution(
        const RnicCollectiveNetworkRuntime& runtime, AtlahsFlowId flow_id,
        std::uint64_t packet_index) {
        return runtime.terminalResolutionForTesting(flow_id, packet_index);
    }

    static bool initialGrantReceived(const RnicCollectiveNetworkRuntime& runtime,
                                     AtlahsFlowId flow_id) {
        return runtime.initialGrantReceivedForTesting(flow_id);
    }

    static std::uint64_t pacerState(const RnicTxPort& port) {
        return port._pacer.state();
    }

    static void replayGapResolved(RnicCollectiveNetworkRuntime& runtime,
                                  AtlahsFlowId flow_id, std::uint64_t packet_index) {
        runtime.replayGapResolvedForTesting(flow_id, packet_index);
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
    // source serializer, at the sole-sender ledger allocation. wire = 2192,
    // budget = 225000, nflow = 9743; the isolated fractional flow absorbs
    // the receiver's whole surplus (book 1.4 pacing correction):
    // floor(floor(9e16 / 9743) * 9743 / 1e6).
    EXPECT_EQ(declared.sender_phase, RnicSenderGrantGate::Phase::Active);
    EXPECT_EQ(declared.declared_nflow_ppm, 9743U);
    EXPECT_EQ(declared.current_wire_rate_bps, UINT64_C(89999999999));
    EXPECT_EQ(runtime.node(request.source).txPort().effectiveWireRateBps(request.flow_id),
              UINT64_C(89999999999));

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
    // The whole transfer resequences inside the first dwnd window, whose
    // boundary snapshot precedes the first admission, so no feedback packet
    // is generated for it.
    EXPECT_EQ(completed.rate_feedback_acks_generated, 0U);
    EXPECT_EQ(completed.rate_feedback_acks_received, 0U);
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
    // oversubscribed by two whole-flow competitors; destination 30 is
    // shared with a fractional competitor from node 3, whose own three-way
    // fan-out composes it down to 370370 ppm, keeping 30 undersubscribed.
    constexpr AtlahsFlowId hungry = 0x260000001ULL;
    constexpr AtlahsFlowId satisfied = 0x260000002ULL;
    constexpr AtlahsFlowId competitor_one = 0x260000003ULL;
    constexpr AtlahsFlowId competitor_two = 0x260000004ULL;
    constexpr AtlahsFlowId filler_one = 0x260000005ULL;
    constexpr AtlahsFlowId filler_two = 0x260000006ULL;
    constexpr AtlahsFlowId fractional_peer = 0x260000007ULL;
    runtime.send({hungry, 0, 31, 2000000, EventList::now(), 1});
    runtime.send({satisfied, 0, 30, 2000000, EventList::now(), 2});
    runtime.send({competitor_one, 1, 31, 2000000, EventList::now(), 3});
    runtime.send({competitor_two, 2, 31, 2000000, EventList::now(), 4});
    runtime.send({filler_one, 3, 28, 2000000, EventList::now(), 5});
    runtime.send({filler_two, 3, 29, 2000000, EventList::now(), 6});
    runtime.send({fractional_peer, 3, 30, 2000000, EventList::now(), 7});

    fixture.stepUntil([&] {
        return runtime.flow(hungry).declaration_dispatched &&
               runtime.flow(satisfied).declaration_dispatched &&
               runtime.flow(fractional_peer).declaration_dispatched;
    });
    // Egress composition at DECLARE time: each flow requests
    // round(1e6 * (C_egress / n_dest) / (margin * C_receiver)) over the
    // destinations pending at its own send.
    EXPECT_EQ(runtime.flow(hungry).declared_nflow_ppm, 1000000U);
    EXPECT_EQ(runtime.flow(satisfied).declared_nflow_ppm, 555556U);
    EXPECT_EQ(runtime.flow(fractional_peer).declared_nflow_ppm, 370370U);
    // Grants are the receivers' exact allocations; the ledger read at the
    // satisfied lane's own dispatch preceded its peer's registration, so it
    // starts at the sole-member surplus.
    EXPECT_EQ(runtime.flow(hungry).current_wire_rate_bps, UINT64_C(30000000000));
    EXPECT_EQ(runtime.flow(satisfied).current_wire_rate_bps, UINT64_C(89999999999));
    // Pacing intersects the snapshot grant with the receiver ledger's
    // current allocation: the peer's registration already trimmed the
    // satisfied lane to floor(floor(9e16 / 925926) * 555556 / 1e6), so
    // node 0's paces sum to 8.4e10 and the port scale stays 1. Node 3's
    // three lanes carry ledger allocations 9e10, 89999999999 and
    // 35999961120 (sum 215999961119 > C_egress), so the port normalization
    // scales each by C_egress / sum.
    EXPECT_EQ(runtime.node(0).txPort().effectiveWireRateBps(hungry),
              UINT64_C(30000000000));
    EXPECT_EQ(runtime.node(0).txPort().effectiveWireRateBps(satisfied),
              UINT64_C(54000038879));
    EXPECT_EQ(runtime.node(3).txPort().effectiveWireRateBps(filler_one),
              UINT64_C(41666674166));
    EXPECT_EQ(runtime.node(3).txPort().effectiveWireRateBps(fractional_peer),
              UINT64_C(16666651666));

    // First RTT boundary (2 * dwnd): the window snapshot has settled the
    // satisfied lane's grant to floor(floor(9e16 / 925926) * 555556 / 1e6)
    // = 54000038879, above its request 50000040000, so it is the one lane
    // with room; the hungry lane is under-granted and keeps its
    // declaration. slack = C_egress - 3e10 - 54000038879, and the raise is
    // round(1e6 * (50000040000 + slack) / 9e10) = 733333 via NFLOW_UPDATE.
    fixture.stepUntil([&] {
        return runtime.flow(satisfied).nflow_updates_dispatched == 1;
    });
    EXPECT_EQ(runtime.flow(satisfied).declared_nflow_ppm, 733333U);
    EXPECT_EQ(runtime.flow(hungry).declared_nflow_ppm, 1000000U);

    // Within two RTTs the raised declaration has passed through the
    // receiver's window snapshots: n_hat = 733333 + 370370 and the adopted
    // fraction give floor(floor(9e16 / 1103703) * 733333 / 1e6).
    fixture.stepUntil([&] {
        return runtime.flow(satisfied).current_wire_rate_bps == UINT64_C(59798668663);
    });
    EXPECT_LE(EventList::now(), timeFromUs(40.0));

    fixture.drainRuntime(runtime);
    // Converged: exactly one update on the satisfied lane, none on the
    // hungry lane, and zero recovery events anywhere in the run.
    EXPECT_EQ(runtime.flow(satisfied).nflow_updates_dispatched, 1U);
    EXPECT_EQ(runtime.flow(satisfied).declared_nflow_ppm, 733333U);
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
    const std::uint64_t send_time_ps = EventList::now();
    runtime.send({data_flow_id, 0, 31, 6, EventList::now(), 1});
    fixture.stepUntil(
        [&] { return runtime.flow(data_flow_id).source_data_packets_dispatched == 1; });
    const std::uint64_t data_start_ps = EventList::now();
    const std::uint64_t published_data_end_ps =
        runtime.node(0).txPort().physicalSerializerAvailablePs();
    // Declare-and-go serializes the flow's own DECLARE first, so the DATA
    // boundary carries the control byte's exact rational residue.
    RnicWireSerializationClock data_clock(capacity_bps);
    data_clock.serialize(send_time_ps, 1);
    EXPECT_EQ(data_clock.serialize(data_start_ps, 6).end_ps, published_data_end_ps);

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

TEST(RnicCollectiveNetworkRuntimeTest, ResolvingOnePacketKeepsAnotherPacketsWatchdogArmed) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.maximum_retransmissions = 2;
    config.retransmission_rto_ps = timeFromUs(20.0);
    const auto rto = config.retransmission_rto_ps;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, std::move(config));
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId id) { completions.push_back(id); });
    constexpr AtlahsFlowId delayed = 0x70000000cULL;
    constexpr AtlahsFlowId resolved = 0x70000000dULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, delayed, 0);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, delayed, 0, 1);
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, resolved, 0);
    runtime.send({delayed, 0, 31, 500, EventList::now(), 15});
    runtime.send({resolved, 1, 30, 500, EventList::now(), 16});
    fixture.stepUntil([&] { return runtime.flow(resolved).receiver_retired; });
    EXPECT_FALSE(runtime.flow(delayed).receiver_retired);
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{resolved, delayed}));
    EXPECT_EQ(runtime.flow(resolved).deterministic_retransmissions, 1U);
    EXPECT_EQ(runtime.flow(delayed).deterministic_retransmissions, 2U);
    const auto first = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, delayed, 1);
    const auto second = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, delayed, 2);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_GE(*second, *first + rto);
    EXPECT_NO_THROW(runtime.validateQuiescent());
}

std::uint64_t alignFixtureEpoch(TwoTierCollectiveFixture& fixture) {
    // EventList is a process-wide clock. Start each comparison at the same
    // control-window and receive-tick phase, then compare elapsed times.
    const auto period = timeFromUs(10.0);
    const auto epoch = (EventList::now() / period + 1) * period;
    bool aligned = false;
    CallbackEvent boundary(fixture.events, epoch, [&] { aligned = true; });
    fixture.stepUntil([&] { return aligned; });
    return epoch;
}

struct PromptRecoveryObservation {
    std::uint64_t completion;
    std::uint64_t first_dispatch;
    std::uint64_t second_dispatch;
    RnicCollectiveRecoveryStatistics statistics;
};

PromptRecoveryObservation runPromptTail(std::uint64_t deadline, std::uint32_t windows,
                                       std::uint64_t legacy_timeout) {
    TwoTierCollectiveFixture fixture;
    const auto epoch = alignFixtureEpoch(fixture);
    auto config = fixture.runtimeConfig();
    config.control_deadline_ps = deadline;
    config.data_recovery = RnicCnDataRecovery::Deadline;
    config.retry_probe_windows = windows;
    config.retransmission_rto_ps = legacy_timeout;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    constexpr AtlahsFlowId flow_id = 0x710000001ULL;
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId id) { completions.push_back(id); });
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, flow_id, 0);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, flow_id, 0, 1);
    runtime.send({flow_id, 0, 31, 500, EventList::now(), 1});
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{flow_id}));
    const auto flow = runtime.flow(flow_id);
    EXPECT_EQ(flow.delivered_payload_bytes, 500U);
    EXPECT_EQ(flow.delivered_wire_bytes, 564U);
    EXPECT_EQ(flow.delivered_data_packets, 1U);
    EXPECT_EQ(flow.deterministic_retransmissions, 2U);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probes, 1U);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probe_wire_bytes, 564U);
    const auto first = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, flow_id, 1);
    const auto second = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, flow_id, 2);
    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(second.has_value());
    EXPECT_GE(second.value(), first.value() + 564U * 80U + deadline * windows);
    EXPECT_LT(second.value(), first.value() + 564U * 80U + deadline * windows + timeFromUs(1.0));
    return {flow.delivery_completion_time_ps.value() - epoch, first.value() - epoch,
            second.value() - epoch, runtime.recoveryStatistics()};
}

TEST(RnicCollectiveNetworkRuntimeTest, PromptTailProbeScalesWithDeadlineAndIgnoresLegacyTimeout) {
    for (const std::uint64_t deadline : {timeFromUs(5.0), timeFromUs(10.0)}) {
        const auto short_probe = runPromptTail(deadline, 2, timeFromMs(50));
        const auto same_probe = runPromptTail(deadline, 2, timeFromMs(100));
        const auto long_probe = runPromptTail(deadline, 4, timeFromMs(50));
        EXPECT_EQ(short_probe.completion, same_probe.completion);
        EXPECT_EQ(short_probe.first_dispatch, same_probe.first_dispatch);
        EXPECT_EQ(short_probe.second_dispatch, same_probe.second_dispatch);
        EXPECT_EQ(short_probe.statistics.deterministic_retransmission_wire_bytes,
                  same_probe.statistics.deterministic_retransmission_wire_bytes);
        EXPECT_GE(long_probe.second_dispatch - long_probe.first_dispatch,
                  short_probe.second_dispatch - short_probe.first_dispatch + 2 * deadline - 80000);
        EXPECT_LE(long_probe.second_dispatch - long_probe.first_dispatch,
                  short_probe.second_dispatch - short_probe.first_dispatch + 2 * deadline + 80000);
        EXPECT_LT(long_probe.completion, timeFromMs(1));
    }
}

TEST(RnicCollectiveNetworkRuntimeTest, AuthenticatedLateRetryUsesPhysicalArrivalAndOneRxSerializer) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    // The fixture's physical switch serializers add more than this window.
    // Its original remains a strict late rejection; the retry has fresh ETA.
    config.ring_cam.delay_window_ps = 1;
    config.data_recovery = RnicCnDataRecovery::Deadline;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId id) { completions.push_back(id); });
    constexpr AtlahsFlowId id = 0x710000002ULL;
    runtime.send({id, 0, 31, 500, EventList::now(), 1});
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{id}));
    EXPECT_EQ(runtime.flow(id).late_data_packets, 1U);
    EXPECT_EQ(runtime.recoveryStatistics().late_retry_admissions, 1U);
    EXPECT_EQ(runtime.flow(id).deterministic_retransmissions, 1U);
    EXPECT_EQ(runtime.node(31).rxPort().deliveredPayloadBytes(id), 500U);
    EXPECT_EQ(runtime.node(31).rxPort().deliveredWireBytes(id), 564U);
    EXPECT_EQ(runtime.node(31).rxPort().ringCam().wireOccupancyBytes(), 0U);
}

TEST(RnicCollectiveNetworkRuntimeTest, OlderSuccessfulRetryClosesNewerInFlightProbes) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.data_recovery = RnicCnDataRecovery::Deadline;
    config.control_deadline_ps = timeFromUs(1.0);
    config.retry_probe_windows = 2;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    constexpr AtlahsFlowId id = 0x710000003ULL;
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, id, 0);
    runtime.send({id, 0, 31, 500, EventList::now(), 1});
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{id}));
    EXPECT_GE(runtime.flow(id).deterministic_retransmissions, 2U);
    EXPECT_GE(runtime.flow(id).duplicate_data_packets_ignored, 1U);
    EXPECT_EQ(runtime.flow(id).delivered_data_packets, 1U);
    RnicCollectiveNetworkRuntimeTestPeer::replayResolvedGapNack(runtime, id, 0);
    RnicCollectiveNetworkRuntimeTestPeer::replayGapResolved(runtime, id, 0);
    RnicCollectiveNetworkRuntimeTestPeer::replayGapResolved(runtime, id, 0);
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions.size(), 1U);
    EXPECT_NO_THROW(runtime.validateQuiescent());
}

TEST(RnicCollectiveNetworkRuntimeTest, CancelledQueuedProbeDoesNotCountAsPhysicalTransmission) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.data_recovery = RnicCnDataRecovery::Deadline;
    config.control_deadline_ps = timeFromUs(1.0);
    config.retry_probe_windows = 2;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    constexpr AtlahsFlowId id = 0x710000008ULL;
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, id, 0);
    runtime.send({id, 0, 31, 500, EventList::now(), 1});
    fixture.stepUntil([&] {
        return RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, 1).has_value();
    });
    const auto first = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, 1).value();
    // Real duplicate DECLARE packets occupy the same source serializer across
    // probe expiry. The earlier retry's physical resolution cancels its queued
    // successor before DATA can acquire that serializer.
    CallbackEvent busy(fixture.events, first + 564U * 80U + timeFromUs(2.0) - 1, [&] {
        for (unsigned duplicate = 0; duplicate < 1000; ++duplicate) {
            RnicCollectiveNetworkRuntimeTestPeer::redeclareFlow(runtime, id);
        }
    });
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{id}));
    EXPECT_EQ(runtime.flow(id).deterministic_retransmissions, 1U);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probes, 0U);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probe_wire_bytes, 0U);
    EXPECT_EQ(runtime.flow(id).delivered_payload_bytes, 500U);
}

TEST(RnicCollectiveNetworkRuntimeTest, PromptResolutionKeepsOtherFlowsAndGapsIndependent) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.data_recovery = RnicCnDataRecovery::Deadline;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId id) { completions.push_back(id); });
    constexpr AtlahsFlowId a = 0x710000004ULL;
    constexpr AtlahsFlowId b = 0x710000005ULL;
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, a, 0);
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, a, 2);
    RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, a, 2, 1);
    RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, b, 0);
    runtime.send({a, 0, 31, 2500, EventList::now(), 1});
    runtime.send({b, 1, 30, 500, EventList::now(), 2});
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions.size(), 2U);
    EXPECT_EQ(runtime.flow(a).delivered_payload_bytes, 2500U);
    EXPECT_EQ(runtime.flow(a).delivered_data_packets, 3U);
    EXPECT_EQ(runtime.flow(b).delivered_payload_bytes, 500U);
    EXPECT_EQ(runtime.flow(a).deterministic_retransmissions, 3U);
    EXPECT_EQ(runtime.flow(b).deterministic_retransmissions, 1U);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probes, 1U);
}

TEST(RnicCollectiveNetworkRuntimeTest, PromptRecoveryRetainsBoundedPermanentLossFailure) {
    for (const auto mode : {RnicCnDataRecovery::Deadline, RnicCnDataRecovery::Exponential}) {
        TwoTierCollectiveFixture fixture;
        auto config = fixture.runtimeConfig();
        config.data_recovery = mode;
        RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
        constexpr AtlahsFlowId id = 0x710000006ULL;
        runtime.setup(32, [](AtlahsFlowId) {});
        for (std::uint32_t attempt = 0; attempt <= 8; ++attempt) {
            RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, id, 0, attempt);
        }
        runtime.send({id, 0, 31, 500, EventList::now(), 1});
        fixture.stepUntil([&] { return runtime.flow(id).deterministic_retransmissions == 8; });
        const auto last = *RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, 8);
        bool nominal_probe_reached = false;
        CallbackEvent nominal_probe(fixture.events, last + 564U * 80U + timeFromUs(40.0),
                                    [&] { nominal_probe_reached = true; });
        fixture.stepUntil([&] { return nominal_probe_reached; });
        EXPECT_EQ(runtime.flow(id).deterministic_retransmissions, 8U);
        EXPECT_TRUE(runtime.hasPendingPhysicalWork());
        try {
            fixture.stepUntil([] { return false; });
            FAIL() << "permanent loss must fail at the configured limit";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("after sender RTO"), std::string::npos);
        }
        EXPECT_EQ(EventList::now(), last + 564U * 80U + config.retransmission_rto_ps);
        EXPECT_EQ(runtime.flow(id).deterministic_retransmissions, 8U);
        EXPECT_EQ(runtime.flow(id).delivered_payload_bytes, 0U);
    }
}

struct InitialWindowObservation {
    std::uint64_t completion;
    std::uint64_t before_grant;
    std::uint64_t holds;
    std::uint64_t accepts;
    std::optional<std::uint64_t> grant;
    std::optional<std::uint64_t> retry;
};

InitialWindowObservation runInitialWindow(std::optional<std::uint64_t> budget,
                                          bool drop_original = false) {
    TwoTierCollectiveFixture fixture;
    const auto epoch = alignFixtureEpoch(fixture);
    auto config = fixture.runtimeConfig();
    config.initial_window_bytes = budget;
    config.initial_window_fan_in = budget.has_value() ? 1 : 0;
    config.data_recovery = drop_original ? RnicCnDataRecovery::Deadline : RnicCnDataRecovery::None;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    constexpr AtlahsFlowId id = 0x710000007ULL;
    runtime.setup(32, [](AtlahsFlowId) {});
    if (drop_original) {
        RnicCollectiveNetworkRuntimeTestPeer::dropOriginalData(runtime, id, 2);
    }
    runtime.send({id, 0, 31, 2500, EventList::now(), 1});
    std::uint64_t before_grant = 0;
    std::optional<std::uint64_t> grant;
    // Observe every event until the physical nonempty grant reaches this
    // sender, including retries and the control packet's full transit time.
    fixture.stepUntil([&] {
        const auto flow = runtime.flow(id);
        if (!RnicCollectiveNetworkRuntimeTestPeer::initialGrantReceived(runtime, id)) {
            before_grant = flow.source_wire_bytes_dispatched +
                           flow.deterministic_retransmission_wire_bytes;
        } else {
            grant = EventList::now() - epoch;
        }
        return grant.has_value() || flow.receiver_retired;
    });
    fixture.drainRuntime(runtime);
    EXPECT_EQ(runtime.flow(id).delivered_payload_bytes, 2500U);
    return {runtime.flow(id).delivery_completion_time_ps.value() - epoch, before_grant,
            runtime.recoveryStatistics().initial_window_holds,
            runtime.recoveryStatistics().initial_grants_dispatched, grant,
            RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, 1).has_value()
                ? std::optional<std::uint64_t>(
                    *RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, 1) - epoch)
                : std::nullopt};
}

TEST(RnicCollectiveNetworkRuntimeTest, FullInitialBudgetWithLossWaitsForPhysicalGrantBeforeRetry) {
    const auto result = runInitialWindow(2692, true);
    EXPECT_EQ(result.before_grant, 2692U);
    ASSERT_TRUE(result.grant.has_value());
    ASSERT_TRUE(result.retry.has_value());
    EXPECT_GE(*result.retry, *result.grant);
    EXPECT_GE(result.accepts, 1U);
    EXPECT_LT(result.completion, timeFromMs(1));
}

std::vector<std::vector<std::uint64_t>> runDormantWindowTrace(
    std::optional<std::uint64_t> budget,
    RnicCnDataRecovery recovery = RnicCnDataRecovery::None,
    std::optional<std::string> directory = std::nullopt) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.data_recovery = recovery;
    config.initial_window_bytes = budget;
    config.initial_window_fan_in = budget.has_value() ? 3 : 0;
    config.trace_directory = directory;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    const auto epoch = alignFixtureEpoch(fixture);
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId id) { completions.push_back(id); });
    const std::vector<AtlahsFlowId> ids{0x710000009ULL, 0x71000000aULL, 0x71000000bULL};
    runtime.send({ids[0], 0, 31, 2500, epoch, 1});
    runtime.send({ids[1], 0, 30, 511, epoch, 2});
    runtime.send({ids[2], 1, 31, 1800, epoch, 3});
    std::vector<std::vector<std::uint64_t>> trace;
    fixture.stepUntil([&] {
        std::vector<std::uint64_t> row{EventList::now() - epoch};
        for (const auto node : {0U, 1U, 30U, 31U}) {
            const auto& tx = runtime.node(node).txPort();
            row.push_back(RnicCollectiveNetworkRuntimeTestPeer::pacerState(tx));
            row.push_back(std::max(tx.nextWireOpportunityPs(), epoch) - epoch);
        }
        for (const auto id : ids) {
            const auto flow = runtime.flow(id);
            row.insert(row.end(), {flow.source_payload_bytes_dispatched,
                                   flow.source_wire_bytes_dispatched,
                                   flow.source_data_packets_dispatched,
                                   flow.delivered_payload_bytes, flow.delivered_wire_bytes,
                                   flow.delivered_data_packets, flow.current_wire_rate_bps});
        }
        row.insert(row.end(), completions.begin(), completions.end());
        trace.push_back(std::move(row));
        return !runtime.hasPendingPhysicalWork();
    });
    runtime.validateQuiescent();
    EXPECT_EQ(completions.size(), 3U);
    if (directory.has_value()) runtime.writeTrace();
    return trace;
}

TEST(RnicCollectiveNetworkRuntimeTest, DormantWindowPreservesEveryDispatchAndPacerObservation) {
    const auto baseline = runDormantWindowTrace(std::nullopt);
    EXPECT_EQ(baseline, runDormantWindowTrace(2692));
    EXPECT_EQ(baseline, runDormantWindowTrace(std::nullopt, RnicCnDataRecovery::Exponential));
    EXPECT_EQ(baseline, runDormantWindowTrace(2692, RnicCnDataRecovery::Exponential));
}

struct TraceDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (std::string("rnic-cn-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    TraceDirectory() {
        if (std::filesystem::exists(path) || std::filesystem::exists(path.string() + ".tmp"))
            throw std::runtime_error("trace test path already exists");
    }
    ~TraceDirectory() {
        std::filesystem::remove_all(path);
        std::filesystem::remove_all(path.string() + ".tmp");
    }
};

using TraceRow = std::map<std::string, std::string>;
std::vector<TraceRow> readTraceRows(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("missing trace table");
    auto split = [](const std::string& line) {
        std::vector<std::string> cells;
        std::istringstream stream(line + ',');
        std::string cell;
        while (std::getline(stream, cell, ',')) cells.push_back(cell);
        return cells;
    };
    std::string line;
    std::getline(input, line);
    const auto header = split(line);
    std::vector<TraceRow> rows;
    while (std::getline(input, line)) {
        const auto cells = split(line);
        if (cells.size() != header.size()) throw std::runtime_error("ragged trace table");
        TraceRow row;
        for (std::size_t i = 0; i < cells.size(); ++i) row.emplace(header[i], cells[i]);
        rows.push_back(std::move(row));
    }
    return rows;
}

TEST(RnicCollectiveNetworkRuntimeTest, TracePreservesDispatchPacerAndCompletionAndJoinsEveryHop) {
    TraceDirectory directory;
    const auto traced = runDormantWindowTrace(std::nullopt, RnicCnDataRecovery::None,
                                              directory.path.string());
    const auto boundary = EventList::now();
    EXPECT_EQ(traced, runDormantWindowTrace(std::nullopt));
    const auto manifest = readTraceRows(directory.path / "manifest.csv").at(0);
    EXPECT_EQ(manifest.at("status"), "complete");
    EXPECT_EQ(manifest.at("finalized"), "true");
    EXPECT_EQ(manifest.at("physical_quiescence"), "verified");
    EXPECT_EQ(std::stoull(manifest.at("physical_quiescence_time_ps")), boundary);
    std::set<std::uint64_t> sequences;
    std::map<std::string, TraceRow> packets;
    std::map<std::string, TraceRow> lifecycles;
    std::map<std::string, std::uint64_t> creations, terminals, hops;
    std::map<std::string, std::uint64_t> service_starts;
    for (const std::string table : {"flows", "packets", "queues", "events"}) {
        const auto rows = readTraceRows(directory.path / (table + ".csv"));
        EXPECT_EQ(rows.size(), std::stoull(manifest.at(table + "_rows")));
        for (const auto& row : rows) {
            EXPECT_EQ(row.at("schema"), "rnic-cn-trace-v1");
            EXPECT_TRUE(sequences.insert(std::stoull(row.at("sequence"))).second);
            if (table == "flows") {
                EXPECT_GT(std::stoull(row.at("flow_id")), UINT32_MAX);
            }
            if (table == "packets") {
                EXPECT_TRUE(packets.emplace(row.at("packet_id"), row).second);
                EXPECT_TRUE(lifecycles.emplace(row.at("lifecycle_id"), row).second);
                EXPECT_EQ(std::stoull(row.at("source_end_ps")) -
                              std::stoull(row.at("source_start_ps")),
                          std::stoull(row.at("wire_bytes")) * 80U);
                EXPECT_EQ(row.at("observed_at_ps"), row.at("source_end_ps"));
            }
            if (table == "queues") {
                const auto& packet = packets.at(row.at("packet_id"));
                for (const auto key : {"lifecycle_id", "flow_id", "packet_flow_id", "kind", "wire_bytes"})
                    EXPECT_EQ(row.at(key), packet.at(key));
                EXPECT_EQ(std::stoull(row.at("backlog_bytes")),
                          std::stoull(row.at("buffered_bytes")) + std::stoull(row.at("in_service_bytes")));
                const auto visit = row.at("packet_id") + ":" + row.at("switch_type") + ":" +
                                   row.at("switch_id") + ":" + row.at("egress_id");
                if (row.at("transition") == "service_start") {
                    EXPECT_TRUE(service_starts.emplace(visit, std::stoull(row.at("time_ps"))).second);
                }
                if (row.at("transition") == "service_end") {
                    EXPECT_EQ(std::stoull(row.at("time_ps")) - service_starts.at(visit),
                              std::stoull(row.at("wire_bytes")) * 80U);
                    EXPECT_EQ(row.at("in_service_bytes"), "0");
                    service_starts.erase(visit);
                    ++hops[row.at("packet_id")];
                }
            }
            if (table == "events") {
                const auto& event = row.at("event");
                if (event == "packet_created") ++creations[row.at("packet_id")];
                if (event == "endpoint_consumed") {
                    ++terminals[row.at("packet_id")];
                    EXPECT_EQ(std::stoull(row.at("detail")), 2U + 3U * hops[row.at("packet_id")]);
                }
                if (event == "rx_schedule") {
                    const auto& packet = lifecycles.at(row.at("lifecycle_id"));
                    const auto release = ((std::stoull(packet.at("eta_ps")) + 4096000U + 15999U) /
                                          16000U) * 16000U;
                    EXPECT_EQ(std::stoull(row.at("logical_release_ps")), release);
                    EXPECT_GE(std::stoull(row.at("service_start_ps")), release);
                    EXPECT_EQ(std::stoull(row.at("service_end_ps")) -
                                  std::stoull(row.at("service_start_ps")),
                              std::stoull(packet.at("wire_bytes")) * 80U);
                }
                if (event == "delivery" || event == "flow_complete") {
                    EXPECT_EQ(lifecycles.at(row.at("trigger_lifecycle_id")).at("flow_id"), row.at("flow_id"));
                }
            }
        }
    }
    EXPECT_TRUE(service_starts.empty());
    for (const auto& packet : packets) {
        EXPECT_EQ(creations[packet.first], 1U);
        EXPECT_EQ(terminals[packet.first], 1U);
    }
    EXPECT_EQ(sequences.size(), std::stoull(manifest.at("observation_count")));
    EXPECT_EQ(*sequences.begin(), 1U);
    EXPECT_EQ(*sequences.rbegin(), sequences.size());
    EXPECT_EQ(packets.size(), std::stoull(manifest.at("data_packet_count")) +
                              std::stoull(manifest.at("control_packet_count")));
}

TEST(RnicCollectiveNetworkRuntimeTest, TraceRejectsIncompleteAndFailedPublication) {
    TraceDirectory directory;
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.trace_directory = "";
    EXPECT_THROW(RnicCollectiveNetworkRuntime(fixture.events, *fixture.topology, config),
                 std::invalid_argument);
    config.trace_directory = directory.path.string();
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    EXPECT_THROW(RnicCollectiveNetworkRuntime(fixture.events, *fixture.topology, config),
                 std::runtime_error);
    runtime.setup(32, [](AtlahsFlowId) {});
    runtime.send({0x730000001ULL, 0, 31, 500, 0, 1});
    EXPECT_THROW(runtime.writeTrace(), std::logic_error);
    EXPECT_FALSE(std::filesystem::exists(directory.path));
    fixture.drainRuntime(runtime);
    std::filesystem::create_directory(directory.path.string() + ".tmp/manifest.csv");
    EXPECT_THROW(runtime.writeTrace(), std::ios_base::failure);
    EXPECT_FALSE(std::filesystem::exists(directory.path));
}

TEST(RnicCollectiveNetworkRuntimeTest, InitialWindowBoundsBytesAndZeroBudgetReceivesPhysicalGrant) {
    for (const std::uint64_t budget : {0U, 999U, 1000U, 2000U}) {
        const auto result = runInitialWindow(budget);
        EXPECT_LE(result.before_grant, budget);
        EXPECT_EQ(result.holds, 1U);
        EXPECT_GE(result.accepts, 1U);
        EXPECT_LT(result.completion, timeFromMs(1));
    }
    const auto off = runInitialWindow(std::nullopt);
    const auto dormant = runInitialWindow(2692);
    EXPECT_EQ(off.completion, dormant.completion);
    EXPECT_EQ(off.before_grant, dormant.before_grant);
    EXPECT_EQ(off.holds, 0U);
    EXPECT_EQ(dormant.holds, 0U);
}

std::uint64_t runExponentialLossPrefix(std::uint64_t deadline, std::uint32_t windows,
                                      std::uint32_t lost_prefix, std::uint64_t legacy_timeout,
                                      std::optional<std::string> directory = std::nullopt) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.data_recovery = RnicCnDataRecovery::Exponential;
    config.control_deadline_ps = deadline;
    config.retry_probe_windows = windows;
    config.retransmission_rto_ps = legacy_timeout;
    config.trace_directory = directory;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    const auto epoch = alignFixtureEpoch(fixture);
    constexpr AtlahsFlowId id = 0x720000001ULL;
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });
    for (std::uint32_t attempt = 0; attempt <= lost_prefix; ++attempt) {
        RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, id, 0, attempt);
    }
    runtime.send({id, 0, 31, 500, epoch, 1});
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{id}));
    EXPECT_EQ(runtime.flow(id).deterministic_retransmissions, lost_prefix + 1);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probes, lost_prefix);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probe_wire_bytes, lost_prefix * 564U);
    std::uint64_t observed_sum = 0;
    for (std::uint32_t attempt = 1; attempt <= lost_prefix; ++attempt) {
        const auto first = *RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, attempt);
        const auto next = *RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, attempt + 1);
        const auto expected = deadline * windows * (UINT64_C(1) << (attempt - 1));
        const auto interval = next - first - 564U * 80U;
        EXPECT_GE(interval, expected);
        EXPECT_LE(interval, expected + 80000);
        observed_sum += interval;
    }
    const auto expected_sum = deadline * windows * ((UINT64_C(1) << lost_prefix) - 1);
    EXPECT_GE(observed_sum, expected_sum);
    EXPECT_LE(observed_sum, expected_sum + lost_prefix * 80000U);
    if (directory.has_value()) runtime.writeTrace();
    return *runtime.flow(id).delivery_completion_time_ps - epoch;
}

TEST(RnicCollectiveNetworkRuntimeTest, ExponentialProbeIntervalsAndCumulativeWaitMatchFrozenPowers) {
    for (const auto deadline : {timeFromUs(5.0), timeFromUs(10.0)}) {
        for (const auto windows : {2U, 4U}) {
            for (const auto prefix : {1U, 3U, 7U}) {
                EXPECT_EQ(runExponentialLossPrefix(deadline, windows, prefix, timeFromMs(50)),
                          runExponentialLossPrefix(deadline, windows, prefix, timeFromMs(100)));
            }
        }
    }
}

TEST(RnicCollectiveNetworkRuntimeTest, TraceBindsNackAndProbeAuthorizationsToPhysicalAttempts) {
    TraceDirectory directory;
    runExponentialLossPrefix(timeFromUs(10.0), 4, 1, timeFromMs(50), directory.path.string());
    std::map<std::string, TraceRow> packets;
    for (const auto& row : readTraceRows(directory.path / "packets.csv"))
        packets.emplace(row.at("lifecycle_id"), row);
    std::size_t drops = 0, nacks = 0, probes = 0;
    for (const auto& row : readTraceRows(directory.path / "events.csv")) {
        if (row.at("event") == "fabric_drop") ++drops;
        if (row.at("event") != "retry_authorized") continue;
        EXPECT_EQ(row.at("packet_index"), "0");
        if (row.at("detail") == "gap_nack") {
            ++nacks;
            EXPECT_EQ(row.at("attempt"), "1");
            EXPECT_EQ(packets.at(row.at("trigger_lifecycle_id")).at("kind"), "GAP_NACK");
        } else {
            ++probes;
            EXPECT_EQ(row.at("detail"), "probe_timeout");
            EXPECT_EQ(row.at("attempt"), "2");
            EXPECT_EQ(row.at("origin_attempt"), "1");
            EXPECT_EQ(row.at("deadline_ps"), row.at("time_ps"));
        }
    }
    EXPECT_EQ(drops, 2U);
    EXPECT_EQ(nacks, 1U);
    EXPECT_EQ(probes, 1U);
}

TEST(RnicCollectiveNetworkRuntimeTest, FinalPhysicalResolutionBeforeAtAndAfterNominalProbeSurvives) {
    for (const auto mode : {RnicCnDataRecovery::Deadline, RnicCnDataRecovery::Exponential}) {
        for (const std::int64_t epsilon : {-1, 0, 1}) {
            TwoTierCollectiveFixture fixture;
            alignFixtureEpoch(fixture);
            auto config = fixture.runtimeConfig();
            config.data_recovery = mode;
            config.control_deadline_ps = timeFromUs(5.0);
            config.retry_probe_windows = 2;
            config.ring_cam.release_tick_ps = 1;
            // Forward ETA adds four 100 ns hops. Resolution adds another four
            // hops and four 64-byte control serializations at 80 ps per byte.
            config.ring_cam.delay_window_ps = 9179520 + epsilon;
            RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
            constexpr AtlahsFlowId id = 0x720000002ULL;
            std::vector<AtlahsFlowId> completions;
            runtime.setup(32, [&](AtlahsFlowId flow_id) { completions.push_back(flow_id); });
            for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
                RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, id, 0, attempt);
            }
            runtime.send({id, 0, 31, 500, EventList::now(), 1});
            fixture.drainRuntime(runtime);
            const auto last = RnicCollectiveNetworkRuntimeTestPeer::retryDispatch(runtime, id, 8);
            const auto resolved = RnicCollectiveNetworkRuntimeTestPeer::terminalResolution(runtime, id, 0);
            ASSERT_TRUE(last.has_value());
            ASSERT_TRUE(resolved.has_value());
            EXPECT_EQ(*resolved, *last + 564U * 80U + timeFromUs(10.0) + epsilon);
            EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{id}));
            EXPECT_EQ(runtime.flow(id).deterministic_retransmissions, 8U);
            EXPECT_EQ(runtime.flow(id).delivered_data_packets, 1U);
        }
    }
}

TEST(RnicCollectiveNetworkRuntimeTest, SimultaneousExponentialGapsKeepIndependentAttemptHistories) {
    TwoTierCollectiveFixture fixture;
    auto config = fixture.runtimeConfig();
    config.data_recovery = RnicCnDataRecovery::Exponential;
    RnicCollectiveNetworkRuntime runtime(fixture.events, *fixture.topology, config);
    std::vector<AtlahsFlowId> completions;
    runtime.setup(32, [&](AtlahsFlowId id) { completions.push_back(id); });
    constexpr AtlahsFlowId a = 0x720000003ULL;
    constexpr AtlahsFlowId b = 0x720000004ULL;
    for (std::uint32_t attempt = 0; attempt <= 3; ++attempt) {
        RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, a, 0, attempt);
    }
    for (std::uint32_t attempt = 0; attempt <= 1; ++attempt) {
        RnicCollectiveNetworkRuntimeTestPeer::dropDataAttempt(runtime, b, 0, attempt);
    }
    runtime.send({a, 0, 31, 500, EventList::now(), 1});
    runtime.send({b, 1, 31, 500, EventList::now(), 2});
    fixture.drainRuntime(runtime);
    EXPECT_EQ(completions, (std::vector<AtlahsFlowId>{b, a}));
    EXPECT_EQ(runtime.flow(a).deterministic_retransmissions, 4U);
    EXPECT_EQ(runtime.flow(b).deterministic_retransmissions, 2U);
    EXPECT_EQ(runtime.recoveryStatistics().tail_probes, 4U);
    EXPECT_EQ(runtime.flow(a).delivered_payload_bytes, 500U);
    EXPECT_EQ(runtime.flow(b).delivered_payload_bytes, 500U);
}

}  // namespace
