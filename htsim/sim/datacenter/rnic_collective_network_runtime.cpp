// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_network_runtime.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "rnic_collective_route.h"
#include "rnic_port.h"

namespace {

using Wide = unsigned __int128;

std::uint64_t checkedAdd(
        std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

bool ledgersEqual(const RnicCollectiveFinalLedger& lhs,
                  const RnicCollectiveFinalLedger& rhs) noexcept {
    return lhs.total_payload_bytes == rhs.total_payload_bytes
           && lhs.total_wire_bytes == rhs.total_wire_bytes
           && lhs.total_data_packets == rhs.total_data_packets;
}

bool extentsEqual(const RnicPacketExtent& lhs,
                  const RnicPacketExtent& rhs) noexcept {
    return lhs.payloadBytes() == rhs.payloadBytes()
           && lhs.wireBytes() == rhs.wireBytes();
}

RnicCollectiveFinalLedger packetLedger(
        std::uint64_t payload_bytes,
        const RnicDataPacketizationConfig& packetization) {
    if (payload_bytes == 0) {
        return {0, 0, 0};
    }
    const std::uint64_t payload_quantum = packetization.maxPayloadBytes();
    const std::uint64_t packet_count =
        (payload_bytes - 1) / payload_quantum + 1;
    const Wide total_wire = static_cast<Wide>(payload_bytes)
                            + static_cast<Wide>(packet_count)
                                  * packetization.dataHeaderBytes();
    if (total_wire > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "rnic-cn final wire-byte ledger overflow");
    }
    return {payload_bytes,
            static_cast<std::uint64_t>(total_wire),
            packet_count};
}

const char* admissionName(RnicRingCamAdmission admission) noexcept {
    switch (admission) {
    case RnicRingCamAdmission::Early:
        return "early";
    case RnicRingCamAdmission::Admitted:
        return "admitted";
    case RnicRingCamAdmission::Late:
        return "late";
    case RnicRingCamAdmission::Overflow:
        return "overflow";
    }
    return "invalid";
}

// ATLAHS owns exactly one selected network profile per EventList. A second
// physical CN runtime would also defeat the global same-time fabric barrier:
// both sources could keep yielding to one another forever.
std::set<const EventList*> active_collective_network_event_lists;

}  // namespace

struct RnicCollectiveNetworkRuntime::Impl {
    using TimePs = std::uint64_t;

    struct FlowState {
        FlowState(const AtlahsFlowRequest& flow_request,
                  RnicCollectiveFinalLedger ledger)
            : request(flow_request),
              final_ledger(ledger),
              packet_flow(nullptr),
              sender_gate(flow_request.flow_id) {}

        AtlahsFlowRequest request;
        RnicCollectiveFinalLedger final_ledger;
        PacketFlow packet_flow;
        RnicSenderGrantGate sender_gate;
        std::uint64_t source_payload_bytes_dispatched = 0;
        std::uint64_t source_wire_bytes_dispatched = 0;
        std::uint64_t source_data_packets_dispatched = 0;
        std::uint64_t delivered_payload_bytes = 0;
        std::uint64_t delivered_wire_bytes = 0;
        std::uint64_t delivered_data_packets = 0;
        bool declaration_dispatched = false;
        bool declaration_observed = false;
        bool retire_control_queued = false;
        bool retire_dispatched = false;
        bool retire_received = false;
        bool retirement_queued = false;
        bool receiver_retired = false;
        bool completion_notified = false;
        std::optional<TimePs> delivery_completion_time_ps;
        std::optional<TimePs> retirement_completion_time_ps;
    };

    struct MembershipBatch {
        std::set<AtlahsFlowId> declared_flow_ids;
        std::set<AtlahsFlowId> retired_flow_ids;
    };

    struct OutstandingWave {
        RnicCollectiveGrantWave wave;
        RnicCollectiveMembershipDelta delta;
    };

    struct ControlFrame {
        RnicCollectivePacketKind kind;
        AtlahsFlowId flow_id;
        std::uint32_t source;
        std::uint32_t destination;
        std::uint64_t wire_bytes;
        std::optional<RnicCollectiveGrant> grant;
        std::optional<RnicCollectiveFinalLedger> final_ledger;
        TimePs eligible_time_ps;
        bool begins_control_busy_period;
        bool inherits_exact_serializer_boundary;
    };

    struct SerializedFrame {
        RnicCollectivePacketKind kind;
        AtlahsFlowId flow_id;
        std::uint32_t source;
        std::uint32_t destination;
        std::uint64_t wire_bytes;
        std::optional<RnicCollectiveDataMetadata> data;
        std::optional<RnicCollectiveGrant> grant;
        std::optional<RnicCollectiveFinalLedger> final_ledger;
    };

    struct DestinationData {
        AtlahsFlowId flow_id;
        std::uint32_t destination;
        RnicCollectiveDataMetadata data;
    };

    struct EndpointArrival {
        TimePs arrival_time_ps;
        std::uint64_t lifecycle_id;
        RnicCollectivePacketKind kind;
        AtlahsFlowId flow_id;
        std::uint32_t source;
        std::uint32_t destination;
        std::optional<RnicCollectiveDataMetadata> data;
        std::optional<RnicCollectiveGrant> grant;
        std::optional<RnicCollectiveFinalLedger> final_ledger;
    };

    class Endpoint final : public PacketSink {
    public:
        Endpoint(Impl& owner, std::uint32_t node_id)
            : _owner(owner),
              _node_id(node_id),
              _name("RnicCollectiveEndpoint" + std::to_string(node_id)) {}

        void receivePacket(Packet& packet) override {
            _owner.stageEndpointArrival(_node_id, packet);
        }

        const string& nodename() override { return _name; }

    private:
        Impl& _owner;
        std::uint32_t _node_id;
        string _name;
    };

    struct NodeState {
        NodeState(Impl& owner,
                  std::uint32_t node_id,
                  const RnicCollectiveNetworkConfig& config)
            : endpoint(owner, node_id),
              node(node_id,
                   config.access_wire_capacity_bps,
                   config.packetization,
                   config.global_prbs_seed,
                   config.ring_cam),
              controller(config.access_wire_capacity_bps,
                         config.control_deadline_ps,
                         config.margin_ppm) {}

        Endpoint endpoint;
        RnicNode node;
        RnicCollectiveController controller;
        std::deque<ControlFrame> control_queue;
        std::map<TimePs, MembershipBatch> membership_batches;
        std::optional<OutstandingWave> outstanding_wave;
    };

    class PacketObserver final
        : public RnicCollectivePacketLifecycleObserver {
    public:
        explicit PacketObserver(Impl& owner) : _owner(&owner) {}

        void observe(
                const RnicCollectivePacketObservation& observation) noexcept
                override {
            if (_owner != nullptr) {
                _owner->observeLifecycle(observation);
            }
        }

        void detach() noexcept { _owner = nullptr; }

    private:
        Impl* _owner;
    };

    Impl(RnicCollectiveNetworkRuntime& runtime,
         EventList& event_list,
         FatTreeTopology& clos,
         RnicCollectiveNetworkConfig runtime_config)
        : owner(runtime),
          events(event_list),
          topology(clos),
          config(std::move(runtime_config)) {
        validateConfiguration();
        const std::uint32_t count = topology.cfg().no_of_servers();
        nodes.reserve(count);
        for (std::uint32_t node_id = 0; node_id < count; ++node_id) {
            nodes.push_back(
                std::make_unique<NodeState>(*this, node_id, config));
        }
        route_provider =
            std::make_unique<RnicCollectiveRouteProvider>(topology);
        packet_observer = std::make_shared<PacketObserver>(*this);
    }

    ~Impl() = default;

    void validateConfiguration() const;
    void shutdown() noexcept;
    void setup(std::uint32_t node_count, CompletionHandler complete_flow);
    void send(const AtlahsFlowRequest& request);
    void doNextEvent();

    FlowState& requireFlow(AtlahsFlowId flow_id);
    const FlowState& requireFlow(AtlahsFlowId flow_id) const;
    NodeState& requireNode(std::uint32_t node_id);
    const NodeState& requireNode(std::uint32_t node_id) const;

    void stageEndpointArrival(std::uint32_t node_id, Packet& packet);
    void observeLifecycle(
        const RnicCollectivePacketObservation& observation) noexcept;
    void wakeAt(TimePs when);
    void wakeAtNoexcept(TimePs when) noexcept;
    void fail(std::exception_ptr error) noexcept;
    void throwDeferredFailure();

    const Route& selectRoute(std::uint32_t source,
                             std::uint32_t destination);
    packetid_t takePacketId();
    void launchDueFrames(TimePs now_ps);
    void launchFrame(const SerializedFrame& frame);
    void processEndpointArrivals(
        TimePs now_ps, std::vector<AtlahsFlowId>& completions);
    void processDataArrival(
        const EndpointArrival& arrival,
        std::vector<AtlahsFlowId>& completions);
    void settleReceivePorts(
        TimePs now_ps, std::vector<AtlahsFlowId>& completions);
    void processRxCompletions(
        const std::vector<RnicRxPacketCompletion>& completed,
        std::vector<AtlahsFlowId>& completions);
    void processRxCompletion(
        const RnicRxPacketCompletion& completion,
        std::vector<AtlahsFlowId>& completions);
    void maybeQueueRetirement(FlowState& flow, TimePs now_ps);
    void queueRetireControl(FlowState& flow,
                            TimePs eligible_time_ps,
                            bool inherits_exact_serializer_boundary);
    void enqueueControl(NodeState& source, ControlFrame frame);

    void activateGrantWaves(TimePs now_ps);
    void beginMembershipWaves(TimePs now_ps);
    void beginMembershipWave(NodeState& receiver,
                             TimePs observation_time_ps,
                             MembershipBatch batch);
    void queueGrantFrames(const RnicCollectiveGrantWave& wave,
                          std::uint32_t receiver_node);

    std::exception_ptr notifyCompletions(
        const std::vector<AtlahsFlowId>& completions);
    bool dispatchControls(TimePs now_ps);
    bool dispatchData(TimePs now_ps);
    std::optional<TimePs> nextEventTime(TimePs now_ps) const;
    bool hasPendingWork() const noexcept;
    void validateQuiescent() const;

    RnicCollectiveNetworkRuntime& owner;
    EventList& events;
    FatTreeTopology& topology;
    RnicCollectiveNetworkConfig config;
    bool is_setup = false;
    bool event_list_registered = false;
    bool failed = false;
    std::uint32_t setup_node_count = 0;
    CompletionHandler complete_flow;

    // Node endpoints are constructed before the route provider and therefore
    // destroyed after it. Flow PacketFlow objects are stable heap objects.
    std::vector<std::unique_ptr<NodeState>> nodes;
    std::unique_ptr<RnicCollectiveRouteProvider> route_provider;
    std::map<AtlahsFlowId, std::unique_ptr<FlowState>> flows;
    std::shared_ptr<PacketObserver> packet_observer;

    std::multimap<TimePs, SerializedFrame> pending_launches;
    std::map<std::uint64_t, DestinationData> destination_data;
    std::vector<EndpointArrival> endpoint_arrivals;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t>
        route_cursors;
    std::set<std::uint64_t> live_packet_lifecycles;
    std::optional<RnicCollectivePacketObservation> fatal_drop;
    std::exception_ptr deferred_failure;
    std::optional<EventList::Handle> event_handle;
    std::uint64_t next_htsim_packet_id = 1;
};

void RnicCollectiveNetworkRuntime::Impl::validateConfiguration() const {
    const FatTreeTopologyCfg& clos = topology.cfg();
    if (clos.get_tiers() != 2) {
        throw std::invalid_argument(
            "rnic-cn runtime requires a two-tier Clos");
    }
    if (clos.switch_model() != FatTreeSwitchModel::Tomahawk3) {
        throw std::invalid_argument(
            "rnic-cn runtime requires Tomahawk3 switches");
    }
    if (clos.uses_pause_flow_control()) {
        throw std::invalid_argument(
            "rnic-cn runtime does not permit PFC/lossless input queues");
    }
    if (clos.no_of_servers() == 0) {
        throw std::invalid_argument(
            "rnic-cn runtime requires at least one Clos node");
    }
    if (config.access_wire_capacity_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn access wire capacity must be nonzero");
    }
    constexpr std::uint64_t htsim_one_byte_per_ps_bps =
        UINT64_C(8000000000000);
    if (config.access_wire_capacity_bps
        > htsim_one_byte_per_ps_bps) {
        throw std::invalid_argument(
            "rnic-cn access capacity exceeds HTSIM's physical queue clock");
    }
    if (clos.downlink_speed(TOR_TIER)
        != config.access_wire_capacity_bps) {
        throw std::invalid_argument(
            "rnic-cn endpoint capacity must equal the physical host link");
    }
    if (config.control_deadline_ps == 0) {
        throw std::invalid_argument(
            "rnic-cn homogeneous control deadline must be nonzero");
    }
    if (!config.calibrated_transit_ps) {
        throw std::invalid_argument(
            "rnic-cn requires fixed path-transit calibration");
    }
    if (config.control_wire_bytes == 0
        || config.control_wire_bytes
               > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument(
            "rnic-cn control extent must fit exactly in uint16_t");
    }
    if (config.packetization.maxWirePacketBytes()
        > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument(
            "rnic-cn DATA extent must fit exactly in uint16_t");
    }

    // Distinct full-envelope boundaries must not collapse onto one simulator
    // tick. Short controls/final tails may share a published ceiling and are
    // handled as explicit same-time microphases.
    const Wide full_packet_ps =
        static_cast<Wide>(config.packetization.maxWirePacketBytes())
        * 8 * 1000000000000ULL;
    if (full_packet_ps < config.access_wire_capacity_bps) {
        throw std::invalid_argument(
            "rnic-cn full DATA envelope is shorter than one picosecond");
    }

    const Wide window_numerator =
        static_cast<Wide>(config.access_wire_capacity_bps)
        * config.ring_cam.delay_window_ps;
    constexpr Wide byte_denominator =
        static_cast<Wide>(8) * 1000000000000ULL;
    const Wide window_bytes =
        (window_numerator + byte_denominator - 1) / byte_denominator;
    const Wide required_capacity =
        window_bytes + config.packetization.maxWirePacketBytes();
    if (required_capacity > std::numeric_limits<std::uint64_t>::max()
        || config.ring_cam.wire_byte_capacity
               < static_cast<std::uint64_t>(required_capacity)) {
        throw std::invalid_argument(
            "rnic-cn Ring-CAM violates W >= ceil(C*Delta/8) + M");
    }
}

void RnicCollectiveNetworkRuntime::Impl::shutdown() noexcept {
    if (event_handle.has_value()) {
        EventList::cancelPendingSourceByHandle(owner, *event_handle);
        event_handle.reset();
    }
    if (packet_observer) {
        packet_observer->detach();
    }
    // A routed packet retains raw Route/Endpoint/PacketFlow pointers. Quietly
    // destroying those owners would turn a later fabric event into UAF.
    if (!live_packet_lifecycles.empty()) {
        std::terminate();
    }
    if (event_list_registered) {
        if (active_collective_network_event_lists.erase(&events) != 1) {
            std::terminate();
        }
        event_list_registered = false;
    }
}

void RnicCollectiveNetworkRuntime::Impl::setup(
        std::uint32_t node_count,
        CompletionHandler completion_handler) {
    if (is_setup) {
        throw std::logic_error("rnic-cn runtime is already set up");
    }
    if (node_count != nodes.size()) {
        throw std::invalid_argument(
            "rnic-cn ATLAHS node count does not match the Clos");
    }
    if (!completion_handler) {
        throw std::invalid_argument(
            "rnic-cn runtime requires a completion handler");
    }
    if (!active_collective_network_event_lists.insert(&events).second) {
        throw std::logic_error(
            "only one active rnic-cn runtime is permitted per EventList");
    }
    event_list_registered = true;
    setup_node_count = node_count;
    complete_flow = std::move(completion_handler);
    is_setup = true;
}

void RnicCollectiveNetworkRuntime::Impl::send(
        const AtlahsFlowRequest& request) {
    if (!is_setup) {
        throw std::logic_error("rnic-cn runtime has not been set up");
    }
    if (failed) {
        throw std::logic_error("rnic-cn runtime is in a terminal failure state");
    }
    if (request.start_time_ps != EventList::now()) {
        throw std::invalid_argument(
            "rnic-cn flow start time must equal event-list time");
    }
    if (request.source >= setup_node_count
        || request.destination >= setup_node_count) {
        throw std::out_of_range(
            "rnic-cn flow node is outside the configured Clos");
    }
    if (request.source == request.destination) {
        throw std::invalid_argument(
            "rnic-cn physical flow requires distinct endpoint nodes");
    }
    if (flows.count(request.flow_id) != 0) {
        throw std::invalid_argument("duplicate rnic-cn ATLAHS flow id");
    }

    const TimePs calibrated_transit =
        config.calibrated_transit_ps(request.source, request.destination);
    const RnicCollectiveFinalLedger final_ledger =
        packetLedger(request.payload_bytes, config.packetization);
    auto state = std::make_unique<FlowState>(request, final_ledger);
    ControlFrame declaration{
        RnicCollectivePacketKind::DECLARE,
        request.flow_id,
        request.source,
        request.destination,
        config.control_wire_bytes,
        std::nullopt,
        std::nullopt,
        EventList::now(),
        false,
        false,
    };

    // Prepare queue/map allocations before adding the port entry; all normal
    // validation failures are therefore transactional.
    NodeState& source = requireNode(request.source);
    enqueueControl(source, declaration);
    const auto inserted = flows.emplace(request.flow_id, std::move(state));
    if (!inserted.second) {
        source.control_queue.pop_back();
        throw std::logic_error("rnic-cn flow insertion failed");
    }
    try {
        source.node.txPort().addFlow(
            request.flow_id, request.payload_bytes, calibrated_transit);
    } catch (...) {
        flows.erase(inserted.first);
        source.control_queue.pop_back();
        throw;
    }
    wakeAt(EventList::now());
}

RnicCollectiveNetworkRuntime::Impl::FlowState&
RnicCollectiveNetworkRuntime::Impl::requireFlow(AtlahsFlowId flow_id) {
    const auto flow = flows.find(flow_id);
    if (flow == flows.end()) {
        throw std::out_of_range("unknown rnic-cn flow id");
    }
    return *flow->second;
}

const RnicCollectiveNetworkRuntime::Impl::FlowState&
RnicCollectiveNetworkRuntime::Impl::requireFlow(
        AtlahsFlowId flow_id) const {
    const auto flow = flows.find(flow_id);
    if (flow == flows.end()) {
        throw std::out_of_range("unknown rnic-cn flow id");
    }
    return *flow->second;
}

RnicCollectiveNetworkRuntime::Impl::NodeState&
RnicCollectiveNetworkRuntime::Impl::requireNode(std::uint32_t node_id) {
    if (node_id >= nodes.size()) {
        throw std::out_of_range("unknown rnic-cn node id");
    }
    return *nodes[node_id];
}

const RnicCollectiveNetworkRuntime::Impl::NodeState&
RnicCollectiveNetworkRuntime::Impl::requireNode(
        std::uint32_t node_id) const {
    if (node_id >= nodes.size()) {
        throw std::out_of_range("unknown rnic-cn node id");
    }
    return *nodes[node_id];
}

void RnicCollectiveNetworkRuntime::Impl::stageEndpointArrival(
        std::uint32_t node_id, Packet& packet) {
    if (failed) {
        throw std::logic_error(
            "rnic-cn endpoint received a packet after terminal failure");
    }
    auto* collective = dynamic_cast<RnicCollectivePacket*>(&packet);
    if (collective == nullptr) {
        throw std::invalid_argument(
            "rnic-cn endpoint received a foreign packet type");
    }
    if (collective->destination() != node_id) {
        throw std::invalid_argument(
            "rnic-cn packet reached the wrong node endpoint");
    }

    EndpointArrival arrival{
        EventList::now(),
        collective->lifecycleId(),
        collective->kind(),
        collective->atlahsFlowId(),
        collective->source(),
        collective->destination(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    switch (collective->kind()) {
    case RnicCollectivePacketKind::DATA:
        arrival.data = collective->data();
        arrival.final_ledger = collective->finalLedger();
        break;
    case RnicCollectivePacketKind::ACCEPT:
    case RnicCollectivePacketKind::GRANT_UPDATE:
        arrival.grant = collective->grant();
        break;
    case RnicCollectivePacketKind::RETIRE:
        arrival.final_ledger = collective->finalLedger();
        break;
    case RnicCollectivePacketKind::DECLARE:
        break;
    }

    endpoint_arrivals.push_back(arrival);
    try {
        collective->consumeAtEndpoint();
    } catch (...) {
        endpoint_arrivals.pop_back();
        throw;
    }
    wakeAt(arrival.arrival_time_ps);
}

void RnicCollectiveNetworkRuntime::Impl::observeLifecycle(
        const RnicCollectivePacketObservation& observation) noexcept {
    try {
        if (observation.lifecycle
            == RnicCollectivePacketLifecycle::CREATED) {
            if (!live_packet_lifecycles.insert(observation.lifecycle_id)
                     .second) {
                fail(std::make_exception_ptr(std::logic_error(
                    "duplicate rnic-cn packet lifecycle creation")));
                wakeAtNoexcept(EventList::now());
            }
            return;
        }

        if (live_packet_lifecycles.erase(observation.lifecycle_id) != 1) {
            fail(std::make_exception_ptr(std::logic_error(
                "unknown rnic-cn terminal packet lifecycle")));
            wakeAtNoexcept(EventList::now());
            return;
        }
        if (observation.lifecycle
            == RnicCollectivePacketLifecycle::FABRIC_DROP) {
            if (!fatal_drop.has_value()) {
                fatal_drop = observation;
            }
            wakeAtNoexcept(EventList::now());
        }
    } catch (...) {
        // Allocation failure is not recoverable from the observer's noexcept
        // contract; continuing would silently lose the physical packet ledger.
        std::terminate();
    }
}

void RnicCollectiveNetworkRuntime::Impl::wakeAt(TimePs when) {
    if (failed) {
        throw std::logic_error(
            "cannot schedule a failed rnic-cn runtime");
    }
    if (when < EventList::now()) {
        throw std::logic_error("rnic-cn runtime event is in the past");
    }
    if (event_handle.has_value()) {
        const TimePs current = (*event_handle)->first;
        if (current <= when) {
            return;
        }
        EventList::cancelPendingSourceByHandle(owner, *event_handle);
        event_handle.reset();
    }
    const EventList::Handle handle =
        EventList::sourceIsPendingGetHandle(owner, when);
    if (handle == EventList::nullHandle()) {
        throw std::runtime_error(
            "rnic-cn runtime event lies outside the EventList horizon");
    }
    event_handle = handle;
}

void RnicCollectiveNetworkRuntime::Impl::wakeAtNoexcept(TimePs when) noexcept {
    try {
        wakeAt(when);
    } catch (...) {
        fail(std::current_exception());
    }
}

void RnicCollectiveNetworkRuntime::Impl::fail(
        std::exception_ptr error) noexcept {
    if (!deferred_failure) {
        deferred_failure = std::move(error);
    }
}

void RnicCollectiveNetworkRuntime::Impl::throwDeferredFailure() {
    if (deferred_failure) {
        std::rethrow_exception(deferred_failure);
    }
    if (fatal_drop.has_value()) {
        throw std::runtime_error(
            "rnic-cn fabric dropped lifecycle "
            + std::to_string(fatal_drop->lifecycle_id)
            + " for flow " + std::to_string(fatal_drop->flow_id));
    }
}

const Route& RnicCollectiveNetworkRuntime::Impl::selectRoute(
        std::uint32_t source, std::uint32_t destination) {
    NodeState& receiver = requireNode(destination);
    const auto& routes = route_provider->routes(
        source, destination, receiver.endpoint);
    if (routes.empty()) {
        throw std::logic_error("rnic-cn route provider returned no paths");
    }
    const std::pair<std::uint32_t, std::uint32_t> key{source, destination};
    std::size_t& cursor = route_cursors[key];
    if (cursor >= routes.size()) {
        throw std::logic_error("rnic-cn route cursor escaped its path set");
    }
    const Route* selected = routes[cursor];
    cursor = (cursor + 1 == routes.size()) ? 0 : cursor + 1;
    if (selected == nullptr) {
        throw std::logic_error("rnic-cn selected a null explicit route");
    }
    return *selected;
}

packetid_t RnicCollectiveNetworkRuntime::Impl::takePacketId() {
    if (next_htsim_packet_id
        > std::numeric_limits<packetid_t>::max()) {
        throw std::overflow_error("rnic-cn exhausted HTSIM packet IDs");
    }
    return static_cast<packetid_t>(next_htsim_packet_id++);
}

void RnicCollectiveNetworkRuntime::Impl::launchDueFrames(TimePs now_ps) {
    const auto due_end = pending_launches.upper_bound(now_ps);
    if (due_end == pending_launches.begin()) {
        return;
    }
    if (pending_launches.begin()->first != now_ps) {
        throw std::logic_error(
            "rnic-cn source serialization completed in the past");
    }

    std::vector<SerializedFrame> due;
    due.reserve(static_cast<std::size_t>(
        std::distance(pending_launches.begin(), due_end)));
    for (auto item = pending_launches.begin(); item != due_end; ++item) {
        if (item->first != now_ps) {
            throw std::logic_error(
                "rnic-cn launch batch spans multiple timestamps");
        }
        due.push_back(item->second);
    }
    pending_launches.erase(pending_launches.begin(), due_end);
    for (const SerializedFrame& frame : due) {
        launchFrame(frame);
    }
}

void RnicCollectiveNetworkRuntime::Impl::launchFrame(
        const SerializedFrame& frame) {
    FlowState& flow = requireFlow(frame.flow_id);
    const Route& route = selectRoute(frame.source, frame.destination);
    const packetid_t packet_id = takePacketId();
    RnicCollectivePacket* packet = nullptr;

    switch (frame.kind) {
    case RnicCollectivePacketKind::DATA:
        if (!frame.data.has_value()) {
            throw std::logic_error("rnic-cn DATA launch lost its metadata");
        }
        packet = RnicCollectivePacket::newData(
            flow.packet_flow,
            route,
            packet_id,
            frame.flow_id,
            frame.source,
            frame.destination,
            *frame.data,
            packet_observer);
        break;
    case RnicCollectivePacketKind::DECLARE:
        if (flow.declaration_dispatched) {
            throw std::logic_error("rnic-cn dispatched DECLARE twice");
        }
        flow.sender_gate.declarationDispatched();
        flow.declaration_dispatched = true;
        packet = RnicCollectivePacket::newDeclare(
            flow.packet_flow,
            route,
            packet_id,
            frame.flow_id,
            frame.source,
            frame.destination,
            frame.wire_bytes,
            packet_observer);
        break;
    case RnicCollectivePacketKind::ACCEPT:
        if (!frame.grant.has_value()) {
            throw std::logic_error("rnic-cn ACCEPT launch lost its grant");
        }
        packet = RnicCollectivePacket::newAccept(
            flow.packet_flow,
            route,
            packet_id,
            frame.source,
            frame.destination,
            frame.wire_bytes,
            *frame.grant,
            packet_observer);
        break;
    case RnicCollectivePacketKind::GRANT_UPDATE:
        if (!frame.grant.has_value()) {
            throw std::logic_error(
                "rnic-cn UPDATE launch lost its grant");
        }
        packet = RnicCollectivePacket::newGrantUpdate(
            flow.packet_flow,
            route,
            packet_id,
            frame.source,
            frame.destination,
            frame.wire_bytes,
            *frame.grant,
            packet_observer);
        break;
    case RnicCollectivePacketKind::RETIRE:
        if (!frame.final_ledger.has_value()) {
            throw std::logic_error("rnic-cn RETIRE launch lost its ledger");
        }
        if (flow.retire_dispatched) {
            throw std::logic_error("rnic-cn dispatched RETIRE twice");
        }
        flow.retire_dispatched = true;
        packet = RnicCollectivePacket::newRetire(
            flow.packet_flow,
            route,
            packet_id,
            frame.flow_id,
            frame.source,
            frame.destination,
            frame.wire_bytes,
            *frame.final_ledger,
            packet_observer);
        break;
    }

    if (route.path_id() < 0) {
        packet->free();
        throw std::logic_error("rnic-cn route has a negative path id");
    }
    packet->set_pathid(static_cast<std::uint32_t>(route.path_id()));
    if (frame.kind == RnicCollectivePacketKind::DATA) {
        const DestinationData metadata{
            frame.flow_id, frame.destination, *frame.data};
        if (!destination_data.emplace(packet->lifecycleId(), metadata)
                 .second) {
            packet->free();
            throw std::logic_error(
                "duplicate rnic-cn destination lifecycle metadata");
        }
    }

    packet->sendOn();
    if (frame.kind == RnicCollectivePacketKind::DATA
        && frame.data->isFinalPacket()) {
        queueRetireControl(flow, EventList::now(), true);
    }
}

void RnicCollectiveNetworkRuntime::Impl::enqueueControl(
        NodeState& source, ControlFrame frame) {
    frame.begins_control_busy_period = source.control_queue.empty();
    source.control_queue.push_back(std::move(frame));
}

void RnicCollectiveNetworkRuntime::Impl::queueRetireControl(
        FlowState& flow,
        TimePs eligible_time_ps,
        bool inherits_exact_serializer_boundary) {
    if (flow.retire_control_queued) {
        throw std::logic_error("rnic-cn queued RETIRE twice");
    }
    if (flow.source_payload_bytes_dispatched
            != flow.final_ledger.total_payload_bytes
        || flow.source_wire_bytes_dispatched
               != flow.final_ledger.total_wire_bytes
        || flow.source_data_packets_dispatched
               != flow.final_ledger.total_data_packets) {
        throw std::logic_error(
            "rnic-cn RETIRE queued before exact source DATA closure");
    }
    NodeState& source = requireNode(flow.request.source);
    enqueueControl(
        source,
        {RnicCollectivePacketKind::RETIRE,
         flow.request.flow_id,
         flow.request.source,
         flow.request.destination,
         config.control_wire_bytes,
         std::nullopt,
         flow.final_ledger,
         eligible_time_ps,
         false,
         inherits_exact_serializer_boundary});
    flow.retire_control_queued = true;
}

void RnicCollectiveNetworkRuntime::Impl::processEndpointArrivals(
        TimePs now_ps, std::vector<AtlahsFlowId>& completions) {
    std::vector<EndpointArrival> arrivals;
    arrivals.swap(endpoint_arrivals);
    for (const EndpointArrival& arrival : arrivals) {
        if (arrival.arrival_time_ps != now_ps) {
            throw std::logic_error(
                "rnic-cn endpoint arrival escaped its timestamp microphase");
        }
        FlowState& flow = requireFlow(arrival.flow_id);
        if (arrival.source != flow.request.source
            || arrival.destination != flow.request.destination) {
            const bool reverse_grant =
                arrival.kind == RnicCollectivePacketKind::ACCEPT
                || arrival.kind
                       == RnicCollectivePacketKind::GRANT_UPDATE;
            if (!reverse_grant
                || arrival.source != flow.request.destination
                || arrival.destination != flow.request.source) {
                throw std::invalid_argument(
                    "rnic-cn packet endpoints do not match its ATLAHS flow");
            }
        }

        switch (arrival.kind) {
        case RnicCollectivePacketKind::DATA:
            processDataArrival(arrival, completions);
            break;
        case RnicCollectivePacketKind::DECLARE: {
            if (flow.declaration_observed) {
                throw std::logic_error(
                    "rnic-cn receiver observed DECLARE twice");
            }
            NodeState& receiver = requireNode(flow.request.destination);
            MembershipBatch& batch =
                receiver.membership_batches[now_ps];
            if (!batch.declared_flow_ids.insert(flow.request.flow_id).second) {
                throw std::logic_error(
                    "rnic-cn membership batch duplicates DECLARE");
            }
            flow.declaration_observed = true;
            break;
        }
        case RnicCollectivePacketKind::ACCEPT:
            if (!arrival.grant.has_value()
                || !flow.sender_gate.receiveAccept(
                    *arrival.grant, now_ps)) {
                throw std::logic_error(
                    "rnic-cn sender rejected a physical ACCEPT");
            }
            break;
        case RnicCollectivePacketKind::GRANT_UPDATE:
            if (!arrival.grant.has_value()
                || !flow.sender_gate.receiveGrantUpdate(
                    *arrival.grant, now_ps)) {
                throw std::logic_error(
                    "rnic-cn sender rejected a physical grant UPDATE");
            }
            break;
        case RnicCollectivePacketKind::RETIRE:
            if (!arrival.final_ledger.has_value()
                || !ledgersEqual(
                    *arrival.final_ledger, flow.final_ledger)) {
                throw std::invalid_argument(
                    "rnic-cn RETIRE final ledger is inconsistent");
            }
            if (flow.retire_received) {
                throw std::logic_error(
                    "rnic-cn receiver observed RETIRE twice");
            }
            flow.retire_received = true;
            if (flow.final_ledger.total_data_packets == 0) {
                if (flow.delivery_completion_time_ps.has_value()) {
                    throw std::logic_error(
                        "rnic-cn zero-DATA flow completed twice");
                }
                flow.delivery_completion_time_ps = now_ps;
                completions.push_back(flow.request.flow_id);
            }
            maybeQueueRetirement(flow, now_ps);
            break;
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::processDataArrival(
        const EndpointArrival& arrival,
        std::vector<AtlahsFlowId>& completions) {
    if (!arrival.data.has_value()) {
        throw std::logic_error("rnic-cn endpoint DATA lost its metadata");
    }
    const auto pending = destination_data.find(arrival.lifecycle_id);
    if (pending == destination_data.end()) {
        throw std::logic_error(
            "rnic-cn endpoint DATA has no lifecycle side-table entry");
    }
    const DestinationData& expected = pending->second;
    const RnicCollectiveDataMetadata& received = *arrival.data;
    const RnicCollectiveDataMetadata& sent = expected.data;
    if (expected.flow_id != arrival.flow_id
        || expected.destination != arrival.destination
        || received.packet_index != sent.packet_index
        || received.payload_byte_offset != sent.payload_byte_offset
        || received.eta_ps != sent.eta_ps
        || !extentsEqual(received.extent, sent.extent)
        || !ledgersEqual(received.final_ledger, sent.final_ledger)) {
        throw std::invalid_argument(
            "rnic-cn endpoint DATA changed in the fabric");
    }

    NodeState& receiver = requireNode(arrival.destination);
    const RnicRingCamPacket ring_packet{
        arrival.lifecycle_id,
        arrival.flow_id,
        received.eta_ps,
        arrival.arrival_time_ps,
        received.extent,
    };
    const RnicRxArrivalResult result =
        receiver.node.rxPort().processArrival(ring_packet);
    processRxCompletions(
        result.packets_completed_through_arrival, completions);
    if (result.admission != RnicRingCamAdmission::Admitted) {
        destination_data.erase(pending);
        throw std::runtime_error(
            std::string("rnic-cn Ring-CAM rejected DATA as ")
            + admissionName(result.admission));
    }
    if (!result.logical_release_ps.has_value()) {
        throw std::logic_error(
            "rnic-cn admitted DATA has no Ring-CAM release time");
    }
}

void RnicCollectiveNetworkRuntime::Impl::settleReceivePorts(
        TimePs now_ps, std::vector<AtlahsFlowId>& completions) {
    for (const auto& node_state : nodes) {
        const std::optional<TimePs> next =
            node_state->node.rxPort().nextEventTimePs();
        if (!next.has_value() || *next > now_ps) {
            continue;
        }
        if (*next < now_ps) {
            throw std::logic_error(
                "rnic-cn RX internal event was scheduled in the past");
        }
        const RnicRxAdvanceResult result =
            node_state->node.rxPort().advanceToWithCompletions(now_ps);
        processRxCompletions(result.packets_completed, completions);
        const std::optional<TimePs> remaining =
            node_state->node.rxPort().nextEventTimePs();
        if (remaining.has_value() && *remaining <= now_ps) {
            throw std::logic_error(
                "rnic-cn RX event did not advance past its boundary");
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::processRxCompletions(
        const std::vector<RnicRxPacketCompletion>& completed,
        std::vector<AtlahsFlowId>& completions) {
    for (const RnicRxPacketCompletion& completion : completed) {
        processRxCompletion(completion, completions);
    }
}

void RnicCollectiveNetworkRuntime::Impl::processRxCompletion(
        const RnicRxPacketCompletion& completion,
        std::vector<AtlahsFlowId>& completions) {
    const auto pending =
        destination_data.find(completion.packet.packet_id);
    if (pending == destination_data.end()) {
        throw std::logic_error(
            "rnic-cn RX completed unknown lifecycle metadata");
    }
    const DestinationData& metadata = pending->second;
    FlowState& flow = requireFlow(metadata.flow_id);
    if (metadata.destination != flow.request.destination
        || completion.packet.flow_id != metadata.flow_id
        || completion.packet.eta_ps != metadata.data.eta_ps
        || !extentsEqual(
            completion.packet.extent, metadata.data.extent)) {
        throw std::invalid_argument(
            "rnic-cn RX completion metadata is inconsistent");
    }
    if (metadata.data.packet_index != flow.delivered_data_packets
        || metadata.data.payload_byte_offset
               != flow.delivered_payload_bytes) {
        throw std::logic_error(
            "rnic-cn Ring-CAM did not restore per-flow DATA order");
    }
    if (!ledgersEqual(
            metadata.data.final_ledger, flow.final_ledger)) {
        throw std::invalid_argument(
            "rnic-cn DATA packet carries a conflicting final ledger");
    }

    const std::uint64_t next_payload = checkedAdd(
        flow.delivered_payload_bytes,
        metadata.data.extent.payloadBytes(),
        "rnic-cn delivered payload ledger overflow");
    const std::uint64_t next_wire = checkedAdd(
        flow.delivered_wire_bytes,
        metadata.data.extent.wireBytes(),
        "rnic-cn delivered wire ledger overflow");
    const std::uint64_t next_packets = checkedAdd(
        flow.delivered_data_packets,
        1,
        "rnic-cn delivered packet ledger overflow");
    if (next_payload > flow.final_ledger.total_payload_bytes
        || next_wire > flow.final_ledger.total_wire_bytes
        || next_packets > flow.final_ledger.total_data_packets) {
        throw std::logic_error(
            "rnic-cn RX completion exceeded its final ledger");
    }

    flow.delivered_payload_bytes = next_payload;
    flow.delivered_wire_bytes = next_wire;
    flow.delivered_data_packets = next_packets;
    destination_data.erase(pending);
    if (next_packets == flow.final_ledger.total_data_packets) {
        if (next_payload != flow.final_ledger.total_payload_bytes
            || next_wire != flow.final_ledger.total_wire_bytes
            || flow.delivery_completion_time_ps.has_value()) {
            throw std::logic_error(
                "rnic-cn final RX ledger is incomplete or duplicated");
        }
        flow.delivery_completion_time_ps = completion.serializer_end_ps;
        completions.push_back(flow.request.flow_id);
        maybeQueueRetirement(flow, completion.serializer_end_ps);
    }
}

void RnicCollectiveNetworkRuntime::Impl::maybeQueueRetirement(
        FlowState& flow, TimePs now_ps) {
    if (!flow.retire_received || flow.retirement_queued) {
        return;
    }
    if (flow.delivered_payload_bytes
            != flow.final_ledger.total_payload_bytes
        || flow.delivered_wire_bytes
               != flow.final_ledger.total_wire_bytes
        || flow.delivered_data_packets
               != flow.final_ledger.total_data_packets) {
        return;
    }
    if (!flow.declaration_observed) {
        throw std::logic_error(
            "rnic-cn RETIRE became ready before DECLARE observation");
    }

    NodeState& receiver = requireNode(flow.request.destination);
    MembershipBatch& batch = receiver.membership_batches[now_ps];
    if (!batch.retired_flow_ids.insert(flow.request.flow_id).second) {
        throw std::logic_error(
            "rnic-cn membership batch duplicates RETIRE");
    }
    flow.retirement_queued = true;
}

void RnicCollectiveNetworkRuntime::Impl::activateGrantWaves(TimePs now_ps) {
    for (const auto& node_state : nodes) {
        NodeState& receiver = *node_state;
        if (!receiver.outstanding_wave.has_value()) {
            continue;
        }
        OutstandingWave& outstanding = *receiver.outstanding_wave;
        if (outstanding.wave.effective_time_ps > now_ps) {
            continue;
        }
        if (outstanding.wave.effective_time_ps < now_ps) {
            throw std::logic_error(
                "rnic-cn grant wave missed its activation boundary");
        }

        std::vector<RnicSenderGrantGate*> gates;
        gates.reserve(outstanding.wave.grants.size());
        for (const RnicCollectiveGrant& grant : outstanding.wave.grants) {
            gates.push_back(&requireFlow(grant.flow_id).sender_gate);
        }
        RnicCollectiveGrantWaveBarrier::activate(
            outstanding.wave, gates, receiver.controller, now_ps);

        for (const RnicCollectiveGrant& grant : outstanding.wave.grants) {
            FlowState& flow = requireFlow(grant.flow_id);
            RnicTxPort& tx =
                requireNode(flow.request.source).node.txPort();
            tx.setWireRateGrant(grant.flow_id, grant.wire_rate_bps);
            tx.setDataEligible(grant.flow_id, true);
            if (flow.final_ledger.total_data_packets == 0
                && !flow.retire_control_queued) {
                queueRetireControl(flow, now_ps, false);
            }
        }

        for (const AtlahsFlowId flow_id :
             outstanding.delta.retired_flow_ids) {
            FlowState& flow = requireFlow(flow_id);
            if (flow.receiver_retired) {
                throw std::logic_error(
                    "rnic-cn receiver committed retirement twice");
            }
            flow.sender_gate.receiverRetirementCommitted();
            RnicTxPort& tx =
                requireNode(flow.request.source).node.txPort();
            tx.setDataEligible(flow_id, false);
            tx.setWireRateGrant(flow_id, 0);
            tx.removeRetiredFlow(flow_id);
            flow.receiver_retired = true;
            flow.retirement_completion_time_ps = now_ps;
        }
        receiver.outstanding_wave.reset();
    }
}

void RnicCollectiveNetworkRuntime::Impl::beginMembershipWaves(
        TimePs now_ps) {
    for (const auto& node_state : nodes) {
        NodeState& receiver = *node_state;
        if (receiver.controller.waveOutstanding()
            || receiver.outstanding_wave.has_value()) {
            continue;
        }

        while (!receiver.membership_batches.empty()) {
            auto first = receiver.membership_batches.begin();
            if (first->first > now_ps) {
                break;
            }
            MembershipBatch batch = std::move(first->second);
            receiver.membership_batches.erase(first);
            beginMembershipWave(
                receiver, now_ps, std::move(batch));
            if (receiver.outstanding_wave.has_value()) {
                break;
            }
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::beginMembershipWave(
        NodeState& receiver,
        TimePs observation_time_ps,
        MembershipBatch batch) {
    RnicCollectiveMembershipDelta delta;
    delta.declared_flow_ids.assign(
        batch.declared_flow_ids.begin(), batch.declared_flow_ids.end());
    delta.retired_flow_ids.assign(
        batch.retired_flow_ids.begin(), batch.retired_flow_ids.end());
    if (delta.declared_flow_ids.empty()
        && delta.retired_flow_ids.empty()) {
        throw std::logic_error("rnic-cn membership batch is empty");
    }
    for (const AtlahsFlowId flow_id : delta.declared_flow_ids) {
        const FlowState& flow = requireFlow(flow_id);
        if (&requireNode(flow.request.destination) != &receiver
            || receiver.controller.contains(flow_id)) {
            throw std::logic_error(
                "rnic-cn DECLARE batch targets invalid receiver state");
        }
    }
    for (const AtlahsFlowId flow_id : delta.retired_flow_ids) {
        const FlowState& flow = requireFlow(flow_id);
        if (&requireNode(flow.request.destination) != &receiver
            || !receiver.controller.contains(flow_id)) {
            throw std::logic_error(
                "rnic-cn RETIRE batch targets invalid receiver state");
        }
    }

    const std::optional<RnicCollectiveGrantWave> wave =
        receiver.controller.beginMembershipWave(
            delta, observation_time_ps);
    if (!wave.has_value()) {
        throw std::logic_error(
            "rnic-cn physical membership event caused no transition");
    }
    receiver.outstanding_wave = OutstandingWave{*wave, delta};
    const std::uint32_t receiver_node =
        delta.declared_flow_ids.empty()
            ? requireFlow(delta.retired_flow_ids.front()).request.destination
            : requireFlow(delta.declared_flow_ids.front()).request.destination;
    queueGrantFrames(*wave, receiver_node);
}

void RnicCollectiveNetworkRuntime::Impl::queueGrantFrames(
        const RnicCollectiveGrantWave& wave,
        std::uint32_t receiver_node) {
    NodeState& source = requireNode(receiver_node);
    for (const RnicCollectiveGrant& grant : wave.grants) {
        const FlowState& flow = requireFlow(grant.flow_id);
        if (flow.request.destination != receiver_node) {
            throw std::logic_error(
                "rnic-cn grant wave spans multiple receivers");
        }
        RnicCollectivePacketKind packet_kind;
        if (grant.kind == RnicCollectiveGrantKind::Accept) {
            packet_kind = RnicCollectivePacketKind::ACCEPT;
        } else if (grant.kind == RnicCollectiveGrantKind::Update) {
            packet_kind = RnicCollectivePacketKind::GRANT_UPDATE;
        } else {
            throw std::logic_error(
                "rnic-cn grant wave contains an invalid packet kind");
        }
        enqueueControl(
            source,
            {packet_kind,
             grant.flow_id,
             receiver_node,
             flow.request.source,
             config.control_wire_bytes,
             grant,
             std::nullopt,
             EventList::now(),
             false,
             false});
    }
}

std::exception_ptr RnicCollectiveNetworkRuntime::Impl::notifyCompletions(
        const std::vector<AtlahsFlowId>& completions) {
    std::exception_ptr first_error;
    std::set<AtlahsFlowId> unique;
    for (const AtlahsFlowId flow_id : completions) {
        if (!unique.insert(flow_id).second) {
            if (!first_error) {
                first_error = std::make_exception_ptr(std::logic_error(
                    "rnic-cn completion batch duplicates a flow"));
            }
            continue;
        }
        FlowState& flow = requireFlow(flow_id);
        if (!flow.delivery_completion_time_ps.has_value()
            || flow.completion_notified) {
            if (!first_error) {
                first_error = std::make_exception_ptr(std::logic_error(
                    "rnic-cn flow completion state is inconsistent"));
            }
            continue;
        }
        flow.completion_notified = true;
        try {
            complete_flow(flow_id);
        } catch (...) {
            if (!first_error) {
                first_error = std::current_exception();
            }
        }
    }
    return first_error;
}

bool RnicCollectiveNetworkRuntime::Impl::dispatchControls(TimePs now_ps) {
    bool same_time_completion = false;
    for (const auto& node_state : nodes) {
        NodeState& source = *node_state;
        if (source.control_queue.empty()) {
            continue;
        }
        RnicTxPort& tx = source.node.txPort();
        if (tx.physicalSerializerAvailablePs() > now_ps) {
            continue;
        }

        const ControlFrame frame = source.control_queue.front();
        if (frame.eligible_time_ps > now_ps) {
            throw std::logic_error(
                "rnic-cn control became dispatchable before eligibility");
        }
        if (frame.begins_control_busy_period
            && !frame.inherits_exact_serializer_boundary
            && frame.eligible_time_ps == now_ps) {
            // Equality with availablePs() may hide a fractional boundary
            // before now. A control that first became eligible at this
            // published timestamp cannot serialize retroactively. Controls
            // already queued behind it keep the cumulative rational clock.
            tx.rebasePhysicalIdle(now_ps);
        }
        const RnicWireSerializationInterval interval =
            tx.dispatchControl(now_ps, frame.wire_bytes);
        pending_launches.emplace(
            interval.end_ps,
            SerializedFrame{frame.kind,
                            frame.flow_id,
                            frame.source,
                            frame.destination,
                            frame.wire_bytes,
                            std::nullopt,
                            frame.grant,
                            frame.final_ledger});
        source.control_queue.pop_front();
        same_time_completion =
            same_time_completion || interval.end_ps == now_ps;
    }
    return same_time_completion;
}

bool RnicCollectiveNetworkRuntime::Impl::dispatchData(TimePs now_ps) {
    bool same_time_completion = false;
    for (const auto& node_state : nodes) {
        NodeState& source = *node_state;
        RnicTxPort& tx = source.node.txPort();
        if (!source.control_queue.empty()) {
            continue;
        }
        if (!tx.hasDispatchableData()) {
            if (tx.physicalSerializerAvailablePs() <= now_ps) {
                tx.rebasePhysicalIdle(now_ps);
            }
            continue;
        }
        if (tx.nextWireOpportunityPs() > now_ps) {
            continue;
        }

        const RnicTxOpportunity opportunity =
            tx.dispatchOpportunity(now_ps);
        same_time_completion =
            same_time_completion || opportunity.end_ps == now_ps;
        if (!opportunity.packet.has_value()) {
            continue;
        }
        const RnicTxPacket& transmitted = *opportunity.packet;
        FlowState& flow = requireFlow(transmitted.flow_id);
        if (flow.request.source != source.node.nodeId()
            || transmitted.payload_byte_offset
                   != flow.source_payload_bytes_dispatched
            || transmitted.packet_index
                   != flow.source_data_packets_dispatched) {
            throw std::logic_error(
                "rnic-cn TX port returned inconsistent DATA ownership");
        }
        const std::uint64_t next_payload = checkedAdd(
            flow.source_payload_bytes_dispatched,
            transmitted.extent.payloadBytes(),
            "rnic-cn source payload ledger overflow");
        const std::uint64_t next_wire = checkedAdd(
            flow.source_wire_bytes_dispatched,
            transmitted.extent.wireBytes(),
            "rnic-cn source wire ledger overflow");
        const std::uint64_t next_packets = checkedAdd(
            flow.source_data_packets_dispatched,
            1,
            "rnic-cn source packet ledger overflow");
        if (next_payload > flow.final_ledger.total_payload_bytes
            || next_wire > flow.final_ledger.total_wire_bytes
            || next_packets > flow.final_ledger.total_data_packets) {
            throw std::logic_error(
                "rnic-cn source DATA exceeded its final ledger");
        }
        const RnicCollectiveDataMetadata metadata{
            transmitted.packet_index,
            transmitted.payload_byte_offset,
            transmitted.extent,
            transmitted.eta_ps,
            flow.final_ledger,
        };
        pending_launches.emplace(
            opportunity.end_ps,
            SerializedFrame{RnicCollectivePacketKind::DATA,
                            flow.request.flow_id,
                            flow.request.source,
                            flow.request.destination,
                            transmitted.extent.wireBytes(),
                            metadata,
                            std::nullopt,
                            flow.final_ledger});
        flow.source_payload_bytes_dispatched = next_payload;
        flow.source_wire_bytes_dispatched = next_wire;
        flow.source_data_packets_dispatched = next_packets;
    }
    return same_time_completion;
}

std::optional<RnicCollectiveNetworkRuntime::Impl::TimePs>
RnicCollectiveNetworkRuntime::Impl::nextEventTime(TimePs now_ps) const {
    std::optional<TimePs> next;
    const auto consider = [now_ps, &next](TimePs candidate) {
        if (candidate < now_ps) {
            throw std::logic_error(
                "rnic-cn next event calculation produced past work");
        }
        if (!next.has_value() || candidate < *next) {
            next = candidate;
        }
    };

    if (!pending_launches.empty()) {
        consider(pending_launches.begin()->first);
    }
    if (!endpoint_arrivals.empty() || fatal_drop.has_value()
        || deferred_failure) {
        consider(now_ps);
    }
    for (const auto& node_state : nodes) {
        const NodeState& state = *node_state;
        const std::optional<TimePs> rx =
            state.node.rxPort().nextEventTimePs();
        if (rx.has_value()) {
            consider(*rx);
        }
        if (state.outstanding_wave.has_value()) {
            consider(state.outstanding_wave->wave.effective_time_ps);
        } else if (!state.membership_batches.empty()) {
            consider(std::max(
                now_ps, state.membership_batches.begin()->first));
        }

        const RnicTxPort& tx = state.node.txPort();
        if (!state.control_queue.empty()) {
            consider(std::max(now_ps, tx.physicalSerializerAvailablePs()));
        } else if (tx.hasDispatchableData()) {
            consider(std::max(now_ps, tx.nextWireOpportunityPs()));
        }
    }
    return next;
}

bool RnicCollectiveNetworkRuntime::Impl::hasPendingWork() const noexcept {
    if (!pending_launches.empty() || !destination_data.empty()
        || !endpoint_arrivals.empty() || !live_packet_lifecycles.empty()
        || fatal_drop.has_value() || deferred_failure
        || event_handle.has_value()) {
        return true;
    }
    for (const auto& node_state : nodes) {
        const NodeState& state = *node_state;
        if (!state.control_queue.empty()
            || !state.membership_batches.empty()
            || state.outstanding_wave.has_value()
            || state.controller.waveOutstanding()
            || state.controller.activeFlowCount() != 0
            || state.node.rxPort().nextEventTimePs().has_value()) {
            return true;
        }
    }
    for (const auto& item : flows) {
        if (!item.second->receiver_retired) {
            return true;
        }
    }
    return false;
}

void RnicCollectiveNetworkRuntime::Impl::validateQuiescent() const {
    if (failed) {
        throw std::logic_error(
            "failed rnic-cn runtime cannot be declared quiescent");
    }
    if (hasPendingWork()) {
        throw std::logic_error("rnic-cn runtime still has physical work");
    }
    for (const auto& item : flows) {
        const FlowState& flow = *item.second;
        if (!flow.completion_notified || !flow.receiver_retired
            || flow.sender_gate.phase()
                   != RnicSenderGrantGate::Phase::Retired
            || requireNode(flow.request.source)
                   .node.txPort().contains(flow.request.flow_id)) {
            throw std::logic_error(
                "rnic-cn quiescent flow retains live endpoint state");
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::doNextEvent() {
    event_handle.reset();
    const TimePs now_ps = EventList::now();
    try {
        if (failed) {
            throw std::logic_error(
                "failed rnic-cn runtime received another event");
        }

        // The executing source is already absent from EventList. Yielding here
        // makes physical arrivals at T independent of equivalent-key insertion
        // order while retaining exactly one coalesced runtime handle.
        if (EventList::hasPendingSourceAt(now_ps)) {
            wakeAt(now_ps);
            return;
        }
        throwDeferredFailure();

        launchDueFrames(now_ps);
        // A zero-latency physical stage may have been scheduled by sendOn().
        // It must complete before control/RX/barrier decisions at this time.
        if (EventList::hasPendingSourceAt(now_ps)) {
            wakeAt(now_ps);
            return;
        }
        throwDeferredFailure();

        std::vector<AtlahsFlowId> completions;
        processEndpointArrivals(now_ps, completions);
        settleReceivePorts(now_ps, completions);
        activateGrantWaves(now_ps);
        beginMembershipWaves(now_ps);
        const std::exception_ptr completion_error =
            notifyCompletions(completions);

        const bool control_at_same_time = dispatchControls(now_ps);
        bool data_at_same_time = false;
        if (!control_at_same_time) {
            data_at_same_time = dispatchData(now_ps);
        }
        if (control_at_same_time || data_at_same_time) {
            wakeAt(now_ps);
        }

        const std::optional<TimePs> next = nextEventTime(now_ps);
        if (next.has_value()) {
            wakeAt(*next);
        } else if (hasPendingWork()
                   && live_packet_lifecycles.empty()) {
            throw std::logic_error(
                "rnic-cn physical work has no future event");
        }
        if (completion_error) {
            std::rethrow_exception(completion_error);
        }
    } catch (...) {
        failed = true;
        if (event_handle.has_value()) {
            EventList::cancelPendingSourceByHandle(owner, *event_handle);
            event_handle.reset();
        }
        throw;
    }
}

RnicCollectiveNetworkRuntime::RnicCollectiveNetworkRuntime(
        EventList& event_list,
        FatTreeTopology& topology,
        RnicCollectiveNetworkConfig config)
    : EventSource(event_list, "rnic-collective-network-runtime"),
      _impl(std::make_unique<Impl>(
          *this, event_list, topology, std::move(config))) {}

RnicCollectiveNetworkRuntime::~RnicCollectiveNetworkRuntime() {
    if (_impl) {
        _impl->shutdown();
    }
}

void RnicCollectiveNetworkRuntime::setup(
        std::uint32_t node_count,
        CompletionHandler complete_flow) {
    _impl->setup(node_count, std::move(complete_flow));
}

void RnicCollectiveNetworkRuntime::send(
        const AtlahsFlowRequest& request) {
    _impl->send(request);
}

bool RnicCollectiveNetworkRuntime::isSetup() const noexcept {
    return _impl->is_setup;
}

std::uint32_t RnicCollectiveNetworkRuntime::nodeCount() const noexcept {
    return _impl->setup_node_count;
}

std::size_t RnicCollectiveNetworkRuntime::flowCount() const noexcept {
    return _impl->flows.size();
}

bool RnicCollectiveNetworkRuntime::contains(
        AtlahsFlowId flow_id) const noexcept {
    return _impl->flows.count(flow_id) != 0;
}

RnicCollectiveFlowSnapshot RnicCollectiveNetworkRuntime::flow(
        AtlahsFlowId flow_id) const {
    const Impl::FlowState& state = _impl->requireFlow(flow_id);
    return {state.request,
            state.final_ledger,
            state.sender_gate.phase(),
            state.sender_gate.currentWireRateBps(),
            state.source_payload_bytes_dispatched,
            state.source_wire_bytes_dispatched,
            state.source_data_packets_dispatched,
            state.delivered_payload_bytes,
            state.delivered_wire_bytes,
            state.delivered_data_packets,
            state.declaration_dispatched,
            state.retire_dispatched,
            state.retire_received,
            state.retirement_queued,
            state.receiver_retired,
            state.completion_notified,
            state.delivery_completion_time_ps,
            state.retirement_completion_time_ps};
}

std::size_t RnicCollectiveNetworkRuntime::receiverActiveFlowCount(
        std::uint32_t node_id) const {
    return _impl->requireNode(node_id).controller.activeFlowCount();
}

std::size_t
RnicCollectiveNetworkRuntime::pendingFabricPacketCount() const noexcept {
    return _impl->live_packet_lifecycles.size();
}

std::size_t
RnicCollectiveNetworkRuntime::pendingDestinationDataCount() const noexcept {
    return _impl->destination_data.size();
}

bool RnicCollectiveNetworkRuntime::hasPendingPhysicalWork() const noexcept {
    return _impl->hasPendingWork();
}

const RnicNode& RnicCollectiveNetworkRuntime::node(
        std::uint32_t node_id) const {
    return _impl->requireNode(node_id).node;
}

void RnicCollectiveNetworkRuntime::validateQuiescent() const {
    _impl->validateQuiescent();
}

void RnicCollectiveNetworkRuntime::doNextEvent() {
    _impl->doNextEvent();
}
