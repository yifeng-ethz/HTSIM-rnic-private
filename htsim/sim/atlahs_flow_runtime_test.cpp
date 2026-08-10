#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "atlahs_htsim_api.h"
#include "logsim-interface.h"

namespace {

class FakeFlowRuntime final : public AtlahsFlowRuntime {
public:
    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override {
        setup_count++;
        configured_node_count = node_count;
        completion = std::move(complete_flow);
    }

    void send(const AtlahsFlowRequest& request) override {
        requests.push_back(request);
        if (complete_synchronously) {
            completion(request.flow_id);
        }
        if (throw_from_send) {
            throw std::runtime_error("injected runtime send failure");
        }
    }

    bool hasPendingPhysicalWork() const noexcept override {
        return pending_physical_work;
    }

    AtlahsTransportKind transportKind() const noexcept override {
        return transport_kind;
    }

    void complete(AtlahsFlowId flow_id) {
        ASSERT_TRUE(static_cast<bool>(completion));
        completion(flow_id);
    }

    int setup_count = 0;
    std::uint32_t configured_node_count = 0;
    CompletionHandler completion;
    std::vector<AtlahsFlowRequest> requests;
    bool pending_physical_work = false;
    bool complete_synchronously = false;
    bool throw_from_send = false;
    AtlahsTransportKind transport_kind = AtlahsTransportKind::None;
};

class CapturingAtlahsHtsimApi final : public AtlahsHtsimApi {
public:
    void EventFinished(const EventOver& event) override {
        completion_count++;
        completed_flow = *event.node;
        completed_payload_bytes = event.getSizeBytes();
        completed_start_time_ps = event.getStartTimeEvent();
    }

    int completion_count = 0;
    graph_node_properties completed_flow{};
    std::uint64_t completed_payload_bytes = 0;
    std::uint64_t completed_start_time_ps = 0;
};

class CallbackEventSource final : public EventSource {
public:
    CallbackEventSource(EventList& event_list,
                        std::function<void()> callback)
        : EventSource(event_list, "ATLAHS boundary callback"),
          callback(std::move(callback)) {}

    void doNextEvent() override {
        ++dispatch_count;
        callback();
    }

    std::function<void()> callback;
    std::uint64_t dispatch_count{0};
};

graph_node_properties makeFlow(std::uint32_t host,
                               std::uint32_t offset,
                               std::uint32_t destination,
                               std::uint64_t payload_bytes,
                               std::uint32_t tag) {
    graph_node_properties flow{};
    flow.host = host;
    flow.offset = offset;
    flow.target = destination;
    flow.size = payload_bytes;
    flow.tag = tag;
    flow.nic = 0;
    flow.type = OP_SEND;
    return flow;
}

TEST(AtlahsFlowRuntimeTest, SetupAndSendDoNotDereferenceLegacyTopology) {
    CapturingAtlahsHtsimApi api;
    api.total_nodes = 8;

    auto runtime = std::make_unique<FakeFlowRuntime>();
    FakeFlowRuntime* fake = runtime.get();
    api.setFlowRuntime(std::move(runtime));

    // Neither a topology nor a UEC/EventList object is installed.
    EXPECT_NO_THROW(api.Setup());
    EXPECT_EQ(fake->setup_count, 1);
    EXPECT_EQ(fake->configured_node_count, 8U);

    const auto flow = makeFlow(3, 17, 6, 4097, 91);
    const SendEvent event(3, 6, flow.size, flow.tag, 123456789);
    EXPECT_NO_THROW(api.Send(event, flow));

    ASSERT_EQ(fake->requests.size(), 1U);
    EXPECT_EQ(fake->requests.front().source, 3U);
    EXPECT_EQ(fake->requests.front().destination, 6U);
}

TEST(AtlahsFlowRuntimeTest, NetworkTimingIsLegacyUntilRuntimeInjection) {
    LogSimInterface logsim;
    CapturingAtlahsHtsimApi api;
    api.setLogSimInterface(&logsim);
    EXPECT_EQ(logsim.getNetworkTiming(), AtlahsNetworkTiming::LegacyLogSimGap);

    api.setFlowRuntime(std::make_unique<FakeFlowRuntime>());
    EXPECT_EQ(logsim.getNetworkTiming(), AtlahsNetworkTiming::RuntimeOwned);

    api.setFlowRuntime(nullptr);
    EXPECT_EQ(logsim.getNetworkTiming(), AtlahsNetworkTiming::LegacyLogSimGap);
}

TEST(AtlahsFlowRuntimeTest,
     ComputeBoundaryDrainsSameTimeEventsWithoutAdvancingPastIt) {
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    LogSimInterface logsim(
        nullptr,
        static_cast<TrafficLoggerSimple*>(nullptr),
        event_list,
        nullptr,
        nullptr);
    CapturingAtlahsHtsimApi api;
    api.setEventList(&event_list);
    api.setLogSimInterface(&logsim);
    logsim.htsim_api = &api;

    CallbackEventSource compute_boundary(
        event_list, [&]() { logsim.compute_over(1); });
    CallbackEventSource same_time_feedback(event_list, []() {});
    CallbackEventSource later_sentinel(event_list, []() {});
    const simtime_picosec boundary_ps = EventList::now() + 1000;

    logsim.compute_started = 1;
    EventList::sourceIsPending(compute_boundary, boundary_ps);
    EventList::sourceIsPending(same_time_feedback, boundary_ps);
    EventList::sourceIsPending(later_sentinel, boundary_ps + 1);

    logsim.htsim_simulate_until(-1);

    EXPECT_EQ(EventList::now(), boundary_ps);
    EXPECT_EQ(compute_boundary.dispatch_count, 1U);
    EXPECT_EQ(same_time_feedback.dispatch_count, 1U);
    EXPECT_EQ(later_sentinel.dispatch_count, 0U);
    EXPECT_EQ(logsim.compute_started, 0);
    EXPECT_FALSE(logsim.have_more);

    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(later_sentinel.dispatch_count, 1U);
}

TEST(AtlahsFlowRuntimeTest, PhysicalWorkQueryForwardsThroughApiAndLogSim) {
    LogSimInterface logsim;
    CapturingAtlahsHtsimApi api;
    api.setLogSimInterface(&logsim);
    logsim.htsim_api = &api;

    EXPECT_FALSE(api.runtimeHasPendingPhysicalWork());
    EXPECT_FALSE(logsim.runtimeHasPendingPhysicalWork());

    auto runtime = std::make_unique<FakeFlowRuntime>();
    FakeFlowRuntime* fake = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    EXPECT_FALSE(api.runtimeHasPendingPhysicalWork());
    EXPECT_FALSE(logsim.runtimeHasPendingPhysicalWork());

    fake->pending_physical_work = true;
    EXPECT_TRUE(api.runtimeHasPendingPhysicalWork());
    EXPECT_TRUE(logsim.runtimeHasPendingPhysicalWork());

    fake->pending_physical_work = false;
    EXPECT_FALSE(api.runtimeHasPendingPhysicalWork());
    EXPECT_FALSE(logsim.runtimeHasPendingPhysicalWork());
}

TEST(AtlahsFlowRuntimeTest, GoalLayoutMustMatchConfiguredPhysicalNodes) {
    CapturingAtlahsHtsimApi api;

    api.total_nodes = 16;
    EXPECT_EQ(api.configureGoalLayoutFromBinaryHeader(16, 8, 2)
                  .physical_node_count,
              16U);
    EXPECT_EQ(api.getGoalRankMapping(),
              AtlahsHtsimApi::GoalRankMapping::GpuRank);

    api.total_nodes = 32;
    EXPECT_EQ(api.configureGoalLayoutFromBinaryHeader(16, 16, 2)
                  .physical_node_count,
              32U);
    EXPECT_EQ(api.getGoalRankMapping(),
              AtlahsHtsimApi::GoalRankMapping::UniqueNic);

    api.total_nodes = 31;
    EXPECT_THROW(api.configureGoalLayoutFromBinaryHeader(16, 16, 2),
                 std::invalid_argument);
    api.total_nodes = 0;
    EXPECT_EQ(api.configureGoalLayoutFromBinaryHeader(7, 1, 1)
                  .physical_node_count,
              7U);
    EXPECT_EQ(api.total_nodes, 7);

    api.total_nodes = 0;
    EXPECT_THROW(api.configureGoalLayoutFromBinaryHeader(0, 1, 1),
                 std::invalid_argument);
}

TEST(AtlahsFlowRuntimeTest, LegacyPathAcceptsLargerTopologyRuntimePathDoesNot) {
    CapturingAtlahsHtsimApi api;

    // Legacy Clos/UEC invocation: an 8-rank schedule may occupy the first
    // hosts of a larger configured topology, exactly as before the seam.
    api.total_nodes = 1024;
    const AtlahsHtsimApi::GoalLayout layout =
        api.configureGoalLayoutFromBinaryHeader(8, 1, 1);
    EXPECT_EQ(layout.physical_node_count, 1024U);
    EXPECT_NO_THROW(api.validateGoalLayoutSnapshot(layout));

    // A runtime-owned profile is sized from the GOAL layout, so the strict
    // form rejects the same mismatch.
    api.total_nodes = 1024;
    EXPECT_THROW(api.configureGoalLayoutFromBinaryHeader(
                     8, 1, 1, /*require_exact_node_count=*/true),
                 std::invalid_argument);

    // Both forms reject a topology smaller than the schedule requires.
    api.total_nodes = 4;
    EXPECT_THROW(api.configureGoalLayoutFromBinaryHeader(8, 1, 1),
                 std::invalid_argument);
}

TEST(AtlahsFlowRuntimeTest, ExplicitGoalRankMappingOverridesHeuristic) {
    CapturingAtlahsHtsimApi api;
    api.total_nodes = 32;
    api.setGoalRankMappingOverride(
        AtlahsHtsimApi::GoalRankMapping::UniqueNic);

    const AtlahsHtsimApi::GoalLayout layout =
        api.configureGoalLayoutFromBinaryHeader(16, 8, 2);
    EXPECT_EQ(layout.rank_mapping,
              AtlahsHtsimApi::GoalRankMapping::UniqueNic);
    EXPECT_EQ(layout.physical_node_count, 32U);
    ASSERT_TRUE(api.getGoalRankMappingOverride().has_value());
    EXPECT_EQ(*api.getGoalRankMappingOverride(), layout.rank_mapping);
}

TEST(AtlahsFlowRuntimeTest,
     GoalLayoutSnapshotRejectsCallbackMappingAndNicMutations) {
    CapturingAtlahsHtsimApi api;
    api.total_nodes = 32;
    api.setGoalRankMappingOverride(
        AtlahsHtsimApi::GoalRankMapping::UniqueNic);
    const AtlahsHtsimApi::GoalLayout layout =
        api.configureGoalLayoutFromBinaryHeader(16, 8, 2);

    EXPECT_NO_THROW(api.validateGoalLayoutSnapshot(layout));

    api.setGoalRankMapping(AtlahsHtsimApi::GoalRankMapping::GpuRank);
    EXPECT_THROW(api.validateGoalLayoutSnapshot(layout), std::logic_error);
    api.setGoalRankMapping(layout.rank_mapping);

    api.setNumberNic(layout.nic_count + 1);
    EXPECT_THROW(api.validateGoalLayoutSnapshot(layout), std::logic_error);
    api.setNumberNic(layout.nic_count);

    ++api.total_nodes;
    EXPECT_THROW(api.validateGoalLayoutSnapshot(layout), std::logic_error);
}

TEST(AtlahsFlowRuntimeTest, DelegatesExactPayloadForConcurrentSameSourceFlows) {
    CapturingAtlahsHtsimApi api;
    api.total_nodes = 16;
    auto runtime = std::make_unique<FakeFlowRuntime>();
    FakeFlowRuntime* fake = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    constexpr std::uint64_t large_exact_payload = (std::uint64_t{1} << 33) + 17;
    const auto first = makeFlow(4, 100, 7, large_exact_payload, 5);
    const auto second = makeFlow(4, 101, 9, 73, 6);
    api.Send(SendEvent(4, 7, first.size, first.tag, 9000), first);
    api.Send(SendEvent(4, 9, second.size, second.tag, 9010), second);

    ASSERT_EQ(fake->requests.size(), 2U);
    EXPECT_EQ(fake->requests[0].payload_bytes, large_exact_payload);
    EXPECT_EQ(fake->requests[1].payload_bytes, 73U);
    EXPECT_EQ(fake->requests[0].source, fake->requests[1].source);
    EXPECT_NE(fake->requests[0].flow_id, fake->requests[1].flow_id);
    EXPECT_EQ(fake->requests[0].flow_id, makeAtlahsFlowId(4, 100));
    EXPECT_EQ(fake->requests[1].flow_id, makeAtlahsFlowId(4, 101));
    EXPECT_EQ(fake->requests[0].start_time_ps, 9000U);
    EXPECT_THROW(api.validateWqeQuiescent(), std::logic_error);
}

TEST(AtlahsFlowRuntimeTest, RoutesEachFlowCompletionExactlyOnce) {
    EventList event_list;
    CapturingAtlahsHtsimApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;
    auto runtime = std::make_unique<FakeFlowRuntime>();
    FakeFlowRuntime* fake = runtime.get();
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const auto flow = makeFlow(1, 42, 2, 12345, 77);
    api.Send(SendEvent(1, 2, flow.size, flow.tag, 0), flow);
    ASSERT_EQ(fake->requests.size(), 1U);

    fake->complete(fake->requests.front().flow_id);
    EXPECT_EQ(api.completion_count, 1);
    EXPECT_EQ(api.completed_flow.host, 1U);
    EXPECT_EQ(api.completed_flow.offset, 42U);
    EXPECT_EQ(api.completed_payload_bytes, 12345U);
    EXPECT_EQ(api.completed_start_time_ps, 0U);
    ASSERT_EQ(api.completedFlows().size(), 1U);
    const AtlahsCompletedFlowRecord& record = api.completedFlows().front();
    EXPECT_EQ(record.flow_id, fake->requests.front().flow_id);
    EXPECT_EQ(record.source, fake->requests.front().source);
    EXPECT_EQ(record.destination, fake->requests.front().destination);
    EXPECT_EQ(record.payload_bytes, 12345U);
    EXPECT_EQ(record.start_time_ps, 0U);
    EXPECT_EQ(record.completion_time_ps, event_list.now());
    EXPECT_EQ(record.fct_ps(), event_list.now());

    // A repeated runtime notification is harmless and cannot release GOAL a
    // second time.
    fake->complete(fake->requests.front().flow_id);
    EXPECT_EQ(api.completion_count, 1);
    EXPECT_EQ(api.completedFlows().size(), 1U);
}

TEST(AtlahsFlowRuntimeTest, SynchronousCompletionPublishesOneWqeBoundary) {
    EventList event_list;
    CapturingAtlahsHtsimApi api;
    api.setEventList(&event_list);
    api.total_nodes = 4;
    auto runtime = std::make_unique<FakeFlowRuntime>();
    runtime->complete_synchronously = true;
    runtime->transport_kind = AtlahsTransportKind::DcqcnQueuePair;
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const auto flow = makeFlow(1, 60, 3, 512, 9);
    EXPECT_NO_THROW(api.Send(SendEvent(1, 3, flow.size, flow.tag, 0), flow));
    EXPECT_EQ(api.completion_count, 1);
    ASSERT_NE(api.wqeLedger(), nullptr);
    EXPECT_EQ(api.wqeLedger()->outstandingCount(), 0U);
    EXPECT_EQ(api.wqeLedger()->completedCount(), 1U);

    ASSERT_EQ(api.completedFlows().size(), 1U);
    EXPECT_EQ(api.wqeLedger()->completedCount(),
              api.completedFlows().size());
    const AtlahsCompletedFlowRecord& completed = api.completedFlows().front();
    const AtlahsWqeRecord& wqe = api.wqeLedger()->wqe(completed.wqe_id);
    EXPECT_TRUE(wqe.completed());
    EXPECT_EQ(completed.sq_id, wqe.sq_id);
    EXPECT_EQ(completed.rq_id, wqe.rq_id);
    EXPECT_EQ(completed.cq_id, wqe.cq_id);
    EXPECT_EQ(completed.sq_post_sequence, 1U);
    EXPECT_EQ(completed.sq_dispatch_sequence, 1U);
    EXPECT_EQ(completed.cq_post_sequence, 1U);
    EXPECT_EQ(completed.cq_consume_sequence, 1U);
    EXPECT_EQ(completed.transport_kind,
              AtlahsTransportKind::DcqcnQueuePair);
    EXPECT_EQ(completed.transport_object_id,
              api.wqeLedger()->transportBinding(1, 3).object_id);
    EXPECT_EQ(wqe.post_time_ps, wqe.dispatch_time_ps);
    EXPECT_EQ(*wqe.completion_time_ps, event_list.now());
    EXPECT_EQ(api.wqeLedger()->completionQueue(1).pending_depth, 0U);
    const auto& counters = api.authorityCounters();
    EXPECT_EQ(counters.native_session_constructed, 0U);
    EXPECT_EQ(counters.legacy_ledger_constructed, 1U);
    EXPECT_EQ(counters.native_posts, 0U);
    EXPECT_EQ(counters.legacy_posts, 1U);
    EXPECT_EQ(counters.legacy_aborts, 0U);
    EXPECT_EQ(counters.legacy_mutations, 2U);
    EXPECT_NO_THROW(api.validateWqeQuiescent());
}

TEST(AtlahsFlowRuntimeTest, RuntimeSendThrowAbortsOutstandingWqe) {
    EventList event_list;
    CapturingAtlahsHtsimApi api;
    api.setEventList(&event_list);
    api.total_nodes = 2;
    auto runtime = std::make_unique<FakeFlowRuntime>();
    runtime->throw_from_send = true;
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const auto flow = makeFlow(0, 70, 1, 64, 11);
    EXPECT_THROW(api.Send(SendEvent(0, 1, flow.size, flow.tag, 0), flow),
                 std::runtime_error);
    ASSERT_NE(api.wqeLedger(), nullptr);
    EXPECT_EQ(api.wqeLedger()->outstandingCount(), 0U);
    EXPECT_EQ(api.wqeLedger()->completedCount(), 0U);
    EXPECT_FALSE(api.wqeLedger()->containsFlow(makeAtlahsFlowId(0, 70)));
    EXPECT_TRUE(api.completedFlows().empty());
    EXPECT_EQ(api.completion_count, 0);
    const auto& counters = api.authorityCounters();
    EXPECT_EQ(counters.legacy_ledger_constructed, 1U);
    EXPECT_EQ(counters.legacy_posts, 1U);
    EXPECT_EQ(counters.legacy_aborts, 1U);
    EXPECT_EQ(counters.legacy_mutations, 2U);
    EXPECT_NO_THROW(api.validateWqeQuiescent());
}

TEST(AtlahsFlowRuntimeTest,
     SynchronousCompletionThrowWithoutEventListRollsBackWqe) {
    CapturingAtlahsHtsimApi api;
    api.total_nodes = 2;
    auto runtime = std::make_unique<FakeFlowRuntime>();
    runtime->complete_synchronously = true;
    api.setFlowRuntime(std::move(runtime));
    api.Setup();

    const auto flow = makeFlow(0, 71, 1, 64, 12);
    EXPECT_THROW(api.Send(SendEvent(0, 1, flow.size, flow.tag, 0), flow),
                 std::logic_error);
    ASSERT_NE(api.wqeLedger(), nullptr);
    EXPECT_EQ(api.wqeLedger()->outstandingCount(), 0U);
    EXPECT_EQ(api.wqeLedger()->completedCount(), 0U);
    EXPECT_FALSE(api.wqeLedger()->containsFlow(makeAtlahsFlowId(0, 71)));
    EXPECT_TRUE(api.completedFlows().empty());
    EXPECT_NO_THROW(api.validateWqeQuiescent());
}

}  // namespace
