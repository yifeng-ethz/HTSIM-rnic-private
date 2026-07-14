// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "rnic_collective_network_runtime.h"
#include "rnic_port.h"
#include "rnic_wire_serialization.h"

namespace {

class TwoTierCollectiveFixture {
public:
    explicit TwoTierCollectiveFixture(
            std::uint64_t link_capacity_bps = speedFromGbps(100))
        : events(EventList::getTheEventList()),
          access_wire_capacity_bps(link_capacity_bps),
          topology_config(2,
                          32,
                          access_wire_capacity_bps,
                          1 << 20,
                          timeFromNs(100),
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

}  // namespace
