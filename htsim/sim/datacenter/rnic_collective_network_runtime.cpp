// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_network_runtime.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "ns_tm3_switch.h"
#include "rnic_collective_route.h"
#include "rnic_port.h"

namespace {

using Wide = unsigned __int128;

std::uint64_t checkedAdd(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

bool ledgersEqual(const RnicCollectiveFinalLedger& lhs,
                  const RnicCollectiveFinalLedger& rhs) noexcept {
    return lhs.total_payload_bytes == rhs.total_payload_bytes &&
           lhs.total_wire_bytes == rhs.total_wire_bytes &&
           lhs.total_data_packets == rhs.total_data_packets;
}

bool extentsEqual(const RnicPacketExtent& lhs, const RnicPacketExtent& rhs) noexcept {
    return lhs.payloadBytes() == rhs.payloadBytes() && lhs.wireBytes() == rhs.wireBytes();
}

bool logicalDataEqual(const RnicCollectiveDataMetadata& lhs,
                      const RnicCollectiveDataMetadata& rhs) noexcept {
    return lhs.packet_index == rhs.packet_index &&
           lhs.payload_byte_offset == rhs.payload_byte_offset &&
           extentsEqual(lhs.extent, rhs.extent) && ledgersEqual(lhs.final_ledger, rhs.final_ledger);
}

std::uint64_t ceilToTick(std::uint64_t value_ps, std::uint64_t tick_ps) {
    const std::uint64_t remainder = value_ps % tick_ps;
    if (remainder == 0) {
        return value_ps;
    }
    return checkedAdd(value_ps, tick_ps - remainder, "rnic-cn tick ceiling overflow");
}

// Margin-derated bottleneck capacity floor(margin_ppm * C / 1e6): the shared
// wire rate when total registered membership is exactly one whole flow.
std::uint64_t marginDeratedCapacityBps(const RnicCollectiveNetworkConfig& config) {
    const Wide derated = static_cast<Wide>(config.access_wire_capacity_bps) *
                         config.margin_ppm / RnicCollectiveController::kPartsPerMillion;
    return static_cast<std::uint64_t>(derated);
}

// Shared deterministic reservation ledger, one per receiver. It models the
// ex-ante declaration schedule (the control plane the full-deterministic
// design assumes; fixed run constants C and S_max are the sanctioned
// fallback): a sender registers (flow_id, nflow_ppm) at DECLARE dispatch and
// reads back its full allocation for its first reachable window, and the
// receiver retires the entry when membership retirement commits. The
// receiver's window-frozen snapshots transport the same totals in band, so
// the sender and receiver views differ only by in-flight DECLAREs.
class RnicCollectiveReservationLedger {
public:
    explicit RnicCollectiveReservationLedger(std::uint64_t margin_derated_capacity_bps)
        : _margin_derated_capacity_bps(margin_derated_capacity_bps) {
        if (_margin_derated_capacity_bps == 0) {
            throw std::invalid_argument(
                "rnic-cn reservation ledger requires a derated capacity");
        }
    }

    void registerDeclaration(std::uint64_t flow_id, std::uint32_t nflow_ppm) {
        if (nflow_ppm == 0 || nflow_ppm > RnicCollectiveController::kFullFlowPpm) {
            throw std::invalid_argument(
                "rnic-cn ledger nflow_ppm must be in [1, one flow]");
        }
        if (nflow_ppm >
            std::numeric_limits<std::uint64_t>::max() - _registered_nflow_ppm_sum) {
            throw std::overflow_error("rnic-cn ledger nflow sum overflow");
        }
        if (!_registered_nflow_by_flow.emplace(flow_id, nflow_ppm).second) {
            throw std::logic_error("rnic-cn ledger duplicates a declaration");
        }
        _registered_nflow_ppm_sum += nflow_ppm;
    }

    void retire(std::uint64_t flow_id) {
        const auto entry = _registered_nflow_by_flow.find(flow_id);
        if (entry == _registered_nflow_by_flow.end()) {
            throw std::logic_error(
                "rnic-cn ledger retirement targets an unregistered flow");
        }
        _registered_nflow_ppm_sum -= entry->second;
        _registered_nflow_by_flow.erase(entry);
    }

    // NFLOW_UPDATE (book 1.4): mutate one registered declaration's
    // magnitude. The caller guards against retired membership.
    void updateDeclaration(std::uint64_t flow_id, std::uint32_t nflow_ppm) {
        if (nflow_ppm == 0 || nflow_ppm > RnicCollectiveController::kFullFlowPpm) {
            throw std::invalid_argument(
                "rnic-cn ledger nflow_ppm must be in [1, one flow]");
        }
        const auto entry = _registered_nflow_by_flow.find(flow_id);
        if (entry == _registered_nflow_by_flow.end()) {
            throw std::logic_error(
                "rnic-cn ledger update targets an unregistered flow");
        }
        _registered_nflow_ppm_sum -= entry->second;
        if (nflow_ppm >
            std::numeric_limits<std::uint64_t>::max() - _registered_nflow_ppm_sum) {
            throw std::overflow_error("rnic-cn ledger nflow sum overflow");
        }
        _registered_nflow_ppm_sum += nflow_ppm;
        entry->second = nflow_ppm;
    }

    // Shared per-whole-flow rate margin * C * 1e6 / sum(registered nflow);
    // each sender scales it by its own fraction, so the registered
    // allocations sum to margin * C exactly.
    std::uint64_t sharedWireRateBps() const {
        if (_registered_nflow_ppm_sum == 0) {
            throw std::logic_error("rnic-cn ledger has no registered reservations");
        }
        return _margin_derated_capacity_bps *
               RnicCollectiveController::kPartsPerMillion / _registered_nflow_ppm_sum;
    }

    bool empty() const noexcept { return _registered_nflow_by_flow.empty(); }

private:
    std::uint64_t _margin_derated_capacity_bps;
    std::uint64_t _registered_nflow_ppm_sum{0};
    std::map<std::uint64_t, std::uint32_t> _registered_nflow_by_flow;
};

RnicCollectiveFinalLedger packetLedger(std::uint64_t payload_bytes,
                                       const RnicDataPacketizationConfig& packetization) {
    if (payload_bytes == 0) {
        return {0, 0, 0};
    }
    const std::uint64_t payload_quantum = packetization.maxPayloadBytes();
    const std::uint64_t packet_count = (payload_bytes - 1) / payload_quantum + 1;
    const Wide total_wire = static_cast<Wide>(payload_bytes) +
                            static_cast<Wide>(packet_count) * packetization.dataHeaderBytes();
    if (total_wire > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("rnic-cn final wire-byte ledger overflow");
    }
    return {payload_bytes, static_cast<std::uint64_t>(total_wire), packet_count};
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

const char* studyTierName(uint32_t switch_type) noexcept {
    switch (static_cast<FatTreeSwitch::switch_type>(switch_type)) {
        case FatTreeSwitch::NONE:
            return "none";
        case FatTreeSwitch::TOR:
            return "leaf";
        case FatTreeSwitch::AGG:
            return "spine";
        case FatTreeSwitch::CORE:
            return "core";
    }
    return "unknown";
}

std::string renderNsTm3QueueDiagnostic(const FatTreeTopology& topology) {
    struct PortSample {
        NsTm3Switch* sw{nullptr};
        uint32_t egress_id{0};
        mem_b current_backlog{0};
        NsTm3EgressStatistics statistics;
    };
    std::optional<PortSample> largest_wait;
    std::optional<PortSample> largest_buffer;
    std::optional<PortSample> largest_current;

    const auto inspect = [&](const std::vector<Switch*>& switches) {
        for (Switch* base : switches) {
            auto* sw = dynamic_cast<NsTm3Switch*>(base);
            if (sw == nullptr) {
                continue;
            }
            for (uint32_t egress_id = 0; egress_id < sw->physical_egress_count(); ++egress_id) {
                PortSample sample{sw, egress_id, sw->egress_backlog_bytes(egress_id),
                                  sw->egress_statistics(egress_id)};
                if (!largest_wait.has_value() || sample.statistics.max_queue_wait_ps >
                                                     largest_wait->statistics.max_queue_wait_ps) {
                    largest_wait = sample;
                }
                if (!largest_buffer.has_value() ||
                    sample.statistics.buffered_high_watermark >
                        largest_buffer->statistics.buffered_high_watermark) {
                    largest_buffer = sample;
                }
                if (!largest_current.has_value() ||
                    sample.current_backlog > largest_current->current_backlog) {
                    largest_current = sample;
                }
            }
        }
    };
    inspect(topology.switches_lp);
    inspect(topology.switches_up);
    inspect(topology.switches_c);

    const auto portName = [](const PortSample& sample) {
        std::ostringstream name;
        name << studyTierName(sample.sw->getType()) << ':' << sample.sw->getID()
             << ":egress:" << sample.egress_id;
        return name.str();
    };
    std::ostringstream diagnostic;
    if (largest_wait.has_value()) {
        diagnostic << " max_wait_port=" << portName(*largest_wait)
                   << " max_wait_ps=" << largest_wait->statistics.max_queue_wait_ps
                   << " max_wait_at_ps=" << largest_wait->statistics.max_queue_wait_observed_ps
                   << " max_wait_ingress=" << largest_wait->statistics.max_queue_wait_ingress_id
                   << " max_wait_flow_id=" << largest_wait->statistics.max_queue_wait_flow_id
                   << " max_wait_packet_id=" << largest_wait->statistics.max_queue_wait_packet_id;
    }
    if (largest_buffer.has_value()) {
        diagnostic << " max_buffer_port=" << portName(*largest_buffer)
                   << " max_buffered_bytes=" << largest_buffer->statistics.buffered_high_watermark
                   << " max_backlog_bytes=" << largest_buffer->statistics.backlog_high_watermark;
    }
    if (largest_current.has_value()) {
        diagnostic << " current_max_port=" << portName(*largest_current)
                   << " current_backlog_bytes=" << largest_current->current_backlog;
    }
    return diagnostic.str();
}

// ATLAHS owns exactly one selected network profile per EventList. Two physical
// runtimes could keep yielding to one another forever while each waits for all
// other same-time fabric sources to settle.
std::set<const EventList*> active_collective_network_event_lists;

}  // namespace

struct RnicCollectiveNetworkRuntime::Impl {
    using TimePs = std::uint64_t;

    struct RxMissingPacket {
        RnicCollectiveDataMetadata logical_data;
        std::uint32_t last_failed_attempt{0};
        std::uint32_t last_nack_attempt{0};
        std::optional<std::uint32_t> highest_late_attempt;
        bool decision_pending{false};
        bool nack_outstanding{false};
        std::optional<std::uint64_t> accepted_lifecycle;
    };

    struct RxAdmittedPacket {
        std::uint64_t lifecycle_id;
        std::uint32_t transmission_attempt;
        TimePs logical_release_ps;
    };

    struct TxRetryState {
        RnicCollectiveDataMetadata logical_data;
        std::uint32_t highest_authorized_attempt{0};
        std::uint32_t highest_dispatched_attempt{0};
        std::uint32_t timeout_fired_through_attempt{0};
        std::uint32_t resolved_through_attempt{0};
    };

    struct RetryTimeout {
        AtlahsFlowId flow_id;
        std::uint64_t packet_index;
        std::uint32_t transmission_attempt;
    };

    struct ReadyPacket {
        std::uint64_t lifecycle_id;
        packetid_t htsim_packet_id;
        RnicCollectiveDataMetadata data;
        TimePs serializer_end_ps;
    };

    struct FlowState {
        FlowState(const AtlahsFlowRequest& flow_request, RnicCollectiveFinalLedger ledger)
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
        std::uint64_t max_original_release_ps = 0;
        std::uint64_t final_original_release_ps = 0;
        std::uint64_t delivered_payload_bytes = 0;
        std::uint64_t delivered_wire_bytes = 0;
        std::uint64_t delivered_data_packets = 0;
        RnicCollectiveRecoveryStatistics recovery;
        std::map<std::uint64_t, RxMissingPacket> rx_missing_packets;
        std::map<std::uint64_t, RxAdmittedPacket> admitted_packets;
        std::map<std::uint64_t, TxRetryState> tx_retry_states;
        std::uint64_t next_resequence_packet = 0;
        std::set<std::uint64_t> resequenced_out_of_order;
        std::map<std::uint64_t, ReadyPacket> ready_packets;
        std::deque<RnicCollectiveDataMetadata> retransmission_queue;
        std::map<std::uint32_t, TimePs> first_retry_dispatch_ps;
        std::uint32_t declared_nflow_ppm = 0;
        // Small-WQE budget-rule cap: the RTT rebalancer never raises a
        // declaration beyond it (book 1.4).
        std::uint32_t nflow_budget_cap_ppm = 0;
        std::uint64_t nflow_updates_dispatched = 0;
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
        std::optional<TimePs> published_retire_gap_detection_ps;
        std::optional<TimePs> first_gap_observation_ps;
        std::optional<TimePs> first_gap_decision_ps;
        std::uint64_t rate_feedback_acks_generated{0};
        std::uint64_t rate_feedback_acks_received{0};
    };

    struct MembershipChangeSet {
        std::map<AtlahsFlowId, std::uint32_t> declared_nflow_by_flow;
        std::set<AtlahsFlowId> retired_flow_ids;
    };

    struct ControlFrame {
        RnicCollectivePacketKind kind;
        AtlahsFlowId flow_id;
        std::uint32_t source;
        std::uint32_t destination;
        std::uint64_t wire_bytes;
        std::optional<RnicCollectiveDeclareMetadata> declaration;
        std::optional<RnicCollectiveGrant> grant;
        std::optional<RnicCollectiveRetireMetadata> retire;
        TimePs eligible_time_ps;
        bool begins_control_busy_period;
        bool inherits_exact_serializer_boundary;
        std::optional<RnicCollectiveGapNackMetadata> gap_nack;
        std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved;
    };

    struct SerializedFrame {
        RnicCollectivePacketKind kind;
        AtlahsFlowId flow_id;
        std::uint32_t source;
        std::uint32_t destination;
        std::uint64_t wire_bytes;
        std::optional<RnicCollectiveDataMetadata> data;
        std::optional<RnicCollectiveDeclareMetadata> declaration;
        std::optional<RnicCollectiveGrant> grant;
        std::optional<RnicCollectiveRetireMetadata> retire;
        std::optional<RnicCollectiveGapNackMetadata> gap_nack;
        bool retransmission{false};
        std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved;
        bool duplicate_test_copy{false};
    };

    struct GapDecision {
        AtlahsFlowId flow_id;
        RnicCollectiveGapNackMetadata gap_nack;
    };

    struct DestinationData {
        AtlahsFlowId flow_id;
        std::uint32_t destination;
        packetid_t htsim_packet_id;
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
        std::optional<RnicCollectiveDeclareMetadata> declaration;
        std::optional<RnicCollectiveGrant> grant;
        std::optional<RnicCollectiveRetireMetadata> retire;
        std::optional<RnicCollectiveGapNackMetadata> gap_nack;
        std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved;
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
        NodeState(Impl& owner, std::uint32_t node_id, const RnicCollectiveNetworkConfig& config)
            : endpoint(owner, node_id),
              node(node_id,
                   config.access_wire_capacity_bps,
                   config.packetization,
                   config.global_prbs_seed,
                   config.ring_cam),
              controller(config.access_wire_capacity_bps,
                         config.control_deadline_ps,
                         config.margin_ppm),
              reservation_ledger(marginDeratedCapacityBps(config)) {}

        Endpoint endpoint;
        RnicNode node;
        RnicCollectiveController controller;
        RnicCollectiveReservationLedger reservation_ledger;
        std::deque<ControlFrame> control_queue;
        std::set<AtlahsFlowId> retransmission_flow_ids;
        std::map<TimePs, MembershipChangeSet> membership_changes;
        // The dwnd-boundary rate snapshot, frozen lazily: the first freeze
        // request inside a window captures the controller state before any
        // same-window membership mutation is applied.
        std::uint64_t frozen_snapshot_window{0};
        std::optional<RnicCollectiveRateSnapshot> frozen_snapshot;
        // Sender-side egress composition state (book 1.4): the active flows
        // this node sources, grouped by destination, and the next boundary
        // of the RTT rebalancer.
        std::map<std::uint32_t, std::set<AtlahsFlowId>> egress_flows_by_destination;
        std::optional<TimePs> next_egress_rebalance_ps;
    };

    class PacketObserver final : public RnicCollectivePacketLifecycleObserver {
    public:
        explicit PacketObserver(Impl& owner) : _owner(&owner) {}

        void observe(const RnicCollectivePacketObservation& observation) noexcept override {
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
            nodes.push_back(std::make_unique<NodeState>(*this, node_id, config));
        }
        route_provider = std::make_unique<RnicCollectiveRouteProvider>(topology);
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
    void observeLifecycle(const RnicCollectivePacketObservation& observation) noexcept;
    void wakeAt(TimePs when);
    void wakeAtNoexcept(TimePs when) noexcept;
    void fail(std::exception_ptr error) noexcept;
    void throwDeferredFailure();

    const Route& selectRoute(std::uint32_t source, std::uint32_t destination);
    packetid_t takePacketId();
    void launchDueFrames(TimePs now_ps);
    void launchFrame(const SerializedFrame& frame);
    void processEndpointArrivals(TimePs now_ps, std::vector<AtlahsFlowId>& completions);
    void processDataArrival(const EndpointArrival& arrival, std::vector<AtlahsFlowId>& completions);
    void processGapNackArrival(const EndpointArrival& arrival);
    void processGapResolvedArrival(const EndpointArrival& arrival, TimePs now_ps);
    RnicCollectiveDataMetadata logicalData(const FlowState& flow, std::uint64_t packet_index) const;
    void scheduleGapNack(FlowState& flow,
                         const RnicCollectiveDataMetadata& data,
                         TimePs decision_time_ps,
                         bool observed_late_packet = false);
    void detectGapThrough(FlowState& flow,
                          std::uint64_t successor_packet_index,
                          TimePs decision_time_ps);
    void processRxReleases(const std::vector<RnicRxScheduledSerialization>& released);
    void processRxRelease(const RnicRxScheduledSerialization& released);
    void processDueTailGapAudits(TimePs now_ps);
    void queueDueGapNacks(TimePs now_ps);
    void processDueRetryTimeouts(TimePs now_ps);
    void queueGapResolved(FlowState& flow,
                          const RxMissingPacket& missing,
                          std::uint32_t acknowledged_attempt,
                          TimePs eligible_time_ps);
    void authorizeRetry(FlowState& flow, TxRetryState& state, std::uint32_t transmission_attempt);
    void cancelRetryTimeouts(AtlahsFlowId flow_id,
                             std::uint64_t packet_index,
                             std::uint32_t through_attempt);
    void duplicateCurrentGapNackForTesting(AtlahsFlowId flow_id);
    void duplicateCurrentRetransmissionForTesting(AtlahsFlowId flow_id);
    void dropOriginalDataForTesting(AtlahsFlowId flow_id, std::uint64_t packet_index);
    void dropDataAttemptForTesting(AtlahsFlowId flow_id,
                                   std::uint64_t packet_index,
                                   std::uint32_t transmission_attempt);
    void duplicateOriginalDataForTesting(AtlahsFlowId flow_id, std::uint64_t packet_index);
    void replayResolvedGapNackForTesting(AtlahsFlowId flow_id, std::uint64_t packet_index);
    void redeclareFlowForTesting(AtlahsFlowId flow_id);
    bool queuedRetransmissionIsCurrent(const FlowState& flow,
                                       const RnicCollectiveDataMetadata& retransmission) const;
    void pruneStaleQueuedRetransmissions(FlowState& flow);
    void settleReceivePorts(TimePs now_ps, std::vector<AtlahsFlowId>& completions);
    void processRxCompletions(const std::vector<RnicRxPacketCompletion>& completed,
                              std::vector<AtlahsFlowId>& completions);
    void processRxCompletion(const RnicRxPacketCompletion& completion,
                             std::vector<AtlahsFlowId>& completions);
    void drainReadyPackets(FlowState& flow, std::vector<AtlahsFlowId>& completions);
    void maybeQueueRetirement(FlowState& flow, TimePs now_ps);
    void queueRetireControl(FlowState& flow,
                            TimePs eligible_time_ps,
                            bool inherits_exact_serializer_boundary);
    void enqueueControl(NodeState& source, ControlFrame frame);

    void beginMembershipChanges(TimePs now_ps);
    void beginMembershipChange(NodeState& receiver,
                               TimePs observation_time_ps,
                               MembershipChangeSet change_set);
    void queueAcceptFrames(const std::vector<RnicCollectiveGrant>& accepts,
                           std::uint32_t receiver_node,
                           TimePs receiver_observation_time_ps);
    void queueRateFeedback(FlowState& flow, TimePs receiver_feedback_time_ps);
    const RnicCollectiveRateSnapshot& frozenSnapshotFor(NodeState& receiver,
                                                        TimePs receiver_time_ps);
    TimePs governedBoundaryPs(TimePs receiver_time_ps) const;
    void activateDeclaredFlows();
    void applyFeedbackArrival(FlowState& flow, RnicSenderFeedbackOutcome outcome);
    void processDueRateActivations(TimePs now_ps);
    std::uint32_t egressFairSharePpm(std::size_t destination_count) const;
    std::uint64_t requestedRateBps(std::uint32_t nflow_ppm) const;
    void processDueEgressRebalances(TimePs now_ps);
    void rebalanceEgress(NodeState& source, TimePs now_ps);

    std::exception_ptr notifyCompletions(const std::vector<AtlahsFlowId>& completions);
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
    std::multimap<TimePs, GapDecision> pending_gap_decisions;
    std::multimap<TimePs, AtlahsFlowId> pending_tail_gap_audits;
    std::multimap<TimePs, RetryTimeout> pending_retry_timeouts;
    // Sender-local dwnd boundaries at which a held window snapshot becomes
    // the pacing rate.
    std::multimap<TimePs, AtlahsFlowId> pending_rate_activations;
    // Same-timestamp DECLARE launches whose gates open after the whole batch
    // has registered in the reservation ledger.
    std::vector<AtlahsFlowId> declared_flows_pending_activation;
    std::map<std::uint64_t, DestinationData> destination_data;
    std::vector<EndpointArrival> endpoint_arrivals;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> route_cursors;
    std::set<std::uint64_t> live_packet_lifecycles;
    std::optional<RnicCollectivePacketObservation> fatal_control_drop;
    std::exception_ptr deferred_failure;
    std::optional<EventList::Handle> event_handle;
    std::uint64_t next_htsim_packet_id = 1;
    RnicCollectiveRecoveryStatistics recovery_statistics;
    std::optional<std::string> first_late_data_diagnostic;
    std::optional<TimePs> first_retransmission_dispatch_ps;
    std::optional<std::string> first_retransmission_launch_diagnostic;
    std::map<std::uint32_t, std::uint64_t> late_data_by_attempt;
    std::map<std::uint32_t, std::uint64_t> retransmissions_by_attempt;
    std::set<std::tuple<AtlahsFlowId, std::uint64_t, std::uint32_t>> data_drops_for_testing;
    std::set<std::pair<AtlahsFlowId, std::uint64_t>> original_duplicates_for_testing;
    std::uint64_t gap_resolved_dispatched{0};
    std::uint64_t gap_resolved_received{0};
};

void RnicCollectiveNetworkRuntime::Impl::validateConfiguration() const {
    const FatTreeTopologyCfg& clos = topology.cfg();
    if (clos.get_tiers() != 2) {
        throw std::invalid_argument("rnic-cn runtime requires a two-tier Clos");
    }
    if (clos.switch_model() != FatTreeSwitchModel::NsTm3) {
        throw std::invalid_argument("rnic-cn runtime requires ns-tm3 switches");
    }
    if (clos.uses_pause_flow_control()) {
        throw std::invalid_argument("rnic-cn runtime does not permit PFC/lossless input queues");
    }
    if (clos.no_of_servers() == 0) {
        throw std::invalid_argument("rnic-cn runtime requires at least one Clos node");
    }
    if (config.access_wire_capacity_bps == 0) {
        throw std::invalid_argument("rnic-cn access wire capacity must be nonzero");
    }
    constexpr std::uint64_t htsim_one_byte_per_ps_bps = UINT64_C(8000000000000);
    if (config.access_wire_capacity_bps > htsim_one_byte_per_ps_bps) {
        throw std::invalid_argument("rnic-cn access capacity exceeds HTSIM's physical queue clock");
    }
    if (clos.downlink_speed(TOR_TIER) != config.access_wire_capacity_bps) {
        throw std::invalid_argument("rnic-cn endpoint capacity must equal the physical host link");
    }
    if (config.control_deadline_ps == 0) {
        throw std::invalid_argument("rnic-cn homogeneous control deadline must be nonzero");
    }
    if (!config.calibrated_transit_ps) {
        throw std::invalid_argument(
            "rnic-cn requires packet-specific no-queue transit calibration");
    }
    if (config.control_wire_bytes == 0 ||
        config.control_wire_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("rnic-cn control extent must fit exactly in uint16_t");
    }
    if (config.maximum_retransmissions == 0) {
        throw std::invalid_argument("rnic-cn maximum retransmissions must be positive");
    }
    if (config.retransmission_rto_ps == 0) {
        throw std::invalid_argument("rnic-cn retransmission RTO must be positive");
    }
    if (config.packetization.maxWirePacketBytes() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("rnic-cn DATA extent must fit exactly in uint16_t");
    }
    // Distinct full-envelope boundaries must not collapse onto one simulator
    // tick. Short controls/final tails may share a published ceiling and are
    // handled as explicit same-time microphases.
    const Wide full_packet_ps =
        static_cast<Wide>(config.packetization.maxWirePacketBytes()) * 8 * 1000000000000ULL;
    if (full_packet_ps < config.access_wire_capacity_bps) {
        throw std::invalid_argument("rnic-cn full DATA envelope is shorter than one picosecond");
    }

    const Wide window_numerator =
        static_cast<Wide>(config.access_wire_capacity_bps) * config.ring_cam.delay_window_ps;
    constexpr Wide byte_denominator = static_cast<Wide>(8) * 1000000000000ULL;
    const Wide window_bytes = (window_numerator + byte_denominator - 1) / byte_denominator;
    const Wide required_capacity = window_bytes + config.packetization.maxWirePacketBytes();
    if (required_capacity > std::numeric_limits<std::uint64_t>::max() ||
        config.ring_cam.wire_byte_capacity < static_cast<std::uint64_t>(required_capacity)) {
        throw std::invalid_argument("rnic-cn Ring-CAM violates W >= ceil(C*Delta/8) + M");
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
    // destroying those owners while the caller could continue the EventList
    // would turn a later fabric event into UAF. During exception unwinding the
    // EventList itself is also being torn down; do not mask the causal
    // exception with std::terminate in that one diagnostic path.
    if (!live_packet_lifecycles.empty() && std::uncaught_exceptions() == 0) {
        std::terminate();
    }
    if (event_list_registered) {
        if (active_collective_network_event_lists.erase(&events) != 1) {
            std::terminate();
        }
        event_list_registered = false;
    }
}

void RnicCollectiveNetworkRuntime::Impl::setup(std::uint32_t node_count,
                                               CompletionHandler completion_handler) {
    if (is_setup) {
        throw std::logic_error("rnic-cn runtime is already set up");
    }
    if (node_count != nodes.size()) {
        throw std::invalid_argument("rnic-cn ATLAHS node count does not match the Clos");
    }
    if (!completion_handler) {
        throw std::invalid_argument("rnic-cn runtime requires a completion handler");
    }
    if (!active_collective_network_event_lists.insert(&events).second) {
        throw std::logic_error("only one active rnic-cn runtime is permitted per EventList");
    }
    event_list_registered = true;
    setup_node_count = node_count;
    complete_flow = std::move(completion_handler);
    is_setup = true;
}

namespace {

// Membership contribution of one sender in ppm of a flow. A transfer that
// fits inside one control round trip (2 * one-way deadline) of full granted
// service reserves only its proportional share, rounded up; everything
// larger declares a whole flow. One rule, no mode switch.
std::uint32_t declaredNflowPpm(const RnicCollectiveNetworkConfig& config,
                               std::uint64_t total_wire_bytes) {
    const std::uint64_t granted_bits_per_second =
        config.access_wire_capacity_bps / RnicCollectiveController::kPartsPerMillion *
        config.margin_ppm;
    const std::uint64_t round_trip_ps = 2 * config.control_deadline_ps;
    // bytes of granted service in one control round trip
    const std::uint64_t budget_bytes =
        granted_bits_per_second / 8 * round_trip_ps / 1000000000000ULL;
    if (budget_bytes == 0 || total_wire_bytes >= budget_bytes) {
        return RnicCollectiveController::kFullFlowPpm;
    }
    const std::uint64_t ppm =
        (total_wire_bytes * RnicCollectiveController::kPartsPerMillion +
         budget_bytes - 1) /
        budget_bytes;
    return static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, ppm));
}

}  // namespace

void RnicCollectiveNetworkRuntime::Impl::send(const AtlahsFlowRequest& request) {
    if (!is_setup) {
        throw std::logic_error("rnic-cn runtime has not been set up");
    }
    if (failed) {
        throw std::logic_error("rnic-cn runtime is in a terminal failure state");
    }
    if (request.start_time_ps != EventList::now()) {
        throw std::invalid_argument("rnic-cn flow start time must equal event-list time");
    }
    if (request.source >= setup_node_count || request.destination >= setup_node_count) {
        throw std::out_of_range("rnic-cn flow node is outside the configured Clos");
    }
    if (request.source == request.destination) {
        throw std::invalid_argument("rnic-cn physical flow requires distinct endpoint nodes");
    }
    if (flows.count(request.flow_id) != 0) {
        throw std::invalid_argument("duplicate rnic-cn ATLAHS flow id");
    }

    const RnicCollectiveFinalLedger final_ledger =
        packetLedger(request.payload_bytes, config.packetization);
    auto state = std::make_unique<FlowState>(request, final_ledger);
    state->nflow_budget_cap_ppm = declaredNflowPpm(config, final_ledger.total_wire_bytes);
    // Sender egress composition (book 1.4): the initial declaration
    // requests C_egress / n_dest over the destinations with pending work at
    // this moment, composed with the small-WQE budget rule by taking the
    // smaller of the two.
    NodeState& source = requireNode(request.source);
    source.egress_flows_by_destination[request.destination].insert(request.flow_id);
    state->declared_nflow_ppm =
        std::min(state->nflow_budget_cap_ppm,
                 egressFairSharePpm(source.egress_flows_by_destination.size()));
    ControlFrame declaration{
        RnicCollectivePacketKind::DECLARE,
        request.flow_id,
        request.source,
        request.destination,
        config.control_wire_bytes,
        RnicCollectiveDeclareMetadata{state->declared_nflow_ppm},
        std::nullopt,
        std::nullopt,
        EventList::now(),
        false,
        false,
    };

    // Prepare queue/map allocations before adding the port entry; all normal
    // validation failures are therefore transactional.
    const auto remove_egress_entry = [&source, &request]() {
        const auto by_destination =
            source.egress_flows_by_destination.find(request.destination);
        if (by_destination != source.egress_flows_by_destination.end()) {
            by_destination->second.erase(request.flow_id);
            if (by_destination->second.empty()) {
                source.egress_flows_by_destination.erase(by_destination);
            }
        }
    };
    enqueueControl(source, declaration);
    const auto inserted = flows.emplace(request.flow_id, std::move(state));
    if (!inserted.second) {
        source.control_queue.pop_back();
        remove_egress_entry();
        throw std::logic_error("rnic-cn flow insertion failed");
    }
    try {
        const RnicCollectiveNetworkConfig::TransitCalibration calibration =
            config.calibrated_transit_ps;
        source.node.txPort().addFlow(
            request.flow_id, request.payload_bytes,
            [calibration, source_id = request.source,
             destination_id = request.destination](const RnicPacketExtent& extent) {
                return calibration(source_id, destination_id, extent);
            });
    } catch (...) {
        flows.erase(inserted.first);
        source.control_queue.pop_back();
        remove_egress_entry();
        throw;
    }
    wakeAt(EventList::now());
}

RnicCollectiveNetworkRuntime::Impl::FlowState& RnicCollectiveNetworkRuntime::Impl::requireFlow(
    AtlahsFlowId flow_id) {
    const auto flow = flows.find(flow_id);
    if (flow == flows.end()) {
        throw std::out_of_range("unknown rnic-cn flow id");
    }
    return *flow->second;
}

const RnicCollectiveNetworkRuntime::Impl::FlowState&
RnicCollectiveNetworkRuntime::Impl::requireFlow(AtlahsFlowId flow_id) const {
    const auto flow = flows.find(flow_id);
    if (flow == flows.end()) {
        throw std::out_of_range("unknown rnic-cn flow id");
    }
    return *flow->second;
}

RnicCollectiveNetworkRuntime::Impl::NodeState& RnicCollectiveNetworkRuntime::Impl::requireNode(
    std::uint32_t node_id) {
    if (node_id >= nodes.size()) {
        throw std::out_of_range("unknown rnic-cn node id");
    }
    return *nodes[node_id];
}

const RnicCollectiveNetworkRuntime::Impl::NodeState&
RnicCollectiveNetworkRuntime::Impl::requireNode(std::uint32_t node_id) const {
    if (node_id >= nodes.size()) {
        throw std::out_of_range("unknown rnic-cn node id");
    }
    return *nodes[node_id];
}

void RnicCollectiveNetworkRuntime::Impl::stageEndpointArrival(std::uint32_t node_id,
                                                              Packet& packet) {
    if (failed) {
        throw std::logic_error("rnic-cn endpoint received a packet after terminal failure");
    }
    auto* collective = dynamic_cast<RnicCollectivePacket*>(&packet);
    if (collective == nullptr) {
        throw std::invalid_argument("rnic-cn endpoint received a foreign packet type");
    }
    if (collective->destination() != node_id) {
        throw std::invalid_argument("rnic-cn packet reached the wrong node endpoint");
    }

    EndpointArrival arrival{
        EventList::now(),     collective->lifecycleId(),
        collective->kind(),   collective->atlahsFlowId(),
        collective->source(), collective->destination(),
        std::nullopt,         std::nullopt,
        std::nullopt,         std::nullopt,
    };
    switch (collective->kind()) {
        case RnicCollectivePacketKind::DATA:
            arrival.data = collective->data();
            break;
        case RnicCollectivePacketKind::ACCEPT:
        case RnicCollectivePacketKind::GRANT_UPDATE:
            arrival.grant = collective->grant();
            break;
        case RnicCollectivePacketKind::GAP_NACK:
            arrival.gap_nack = collective->gapNack();
            break;
        case RnicCollectivePacketKind::GAP_RESOLVED:
            arrival.gap_resolved = collective->gapResolved();
            break;
        case RnicCollectivePacketKind::RETIRE:
            arrival.retire = collective->retire();
            break;
        case RnicCollectivePacketKind::DECLARE:
        case RnicCollectivePacketKind::NFLOW_UPDATE:
            arrival.declaration = collective->declaration();
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
        if (observation.lifecycle == RnicCollectivePacketLifecycle::CREATED) {
            if (!live_packet_lifecycles.insert(observation.lifecycle_id).second) {
                fail(std::make_exception_ptr(
                    std::logic_error("duplicate rnic-cn packet lifecycle creation")));
                wakeAtNoexcept(EventList::now());
            }
            return;
        }

        if (live_packet_lifecycles.erase(observation.lifecycle_id) != 1) {
            fail(std::make_exception_ptr(
                std::logic_error("unknown rnic-cn terminal packet lifecycle")));
            wakeAtNoexcept(EventList::now());
            return;
        }
        if (observation.lifecycle == RnicCollectivePacketLifecycle::FABRIC_DROP) {
            if (observation.kind == RnicCollectivePacketKind::DATA) {
                // This side table is simulator bookkeeping, not receiver
                // observability.  The receiver learns the loss only when a
                // later post-resequencing packet (or RETIRE) exposes the gap.
                destination_data.erase(observation.lifecycle_id);
            } else if (!fatal_control_drop.has_value()) {
                fatal_control_drop = observation;
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
        throw std::logic_error("cannot schedule a failed rnic-cn runtime");
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
    const EventList::Handle handle = EventList::sourceIsPendingGetHandle(owner, when);
    if (handle == EventList::nullHandle()) {
        throw std::runtime_error("rnic-cn runtime event lies outside the EventList horizon");
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

void RnicCollectiveNetworkRuntime::Impl::fail(std::exception_ptr error) noexcept {
    if (!deferred_failure) {
        deferred_failure = std::move(error);
    }
}

void RnicCollectiveNetworkRuntime::Impl::throwDeferredFailure() {
    if (deferred_failure) {
        std::rethrow_exception(deferred_failure);
    }
    if (fatal_control_drop.has_value()) {
        throw std::runtime_error("rnic-cn fabric dropped control lifecycle " +
                                 std::to_string(fatal_control_drop->lifecycle_id) + " for flow " +
                                 std::to_string(fatal_control_drop->flow_id));
    }
}

const Route& RnicCollectiveNetworkRuntime::Impl::selectRoute(std::uint32_t source,
                                                             std::uint32_t destination) {
    NodeState& receiver = requireNode(destination);
    const auto& routes = route_provider->routes(source, destination, receiver.endpoint);
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
    if (next_htsim_packet_id > std::numeric_limits<packetid_t>::max()) {
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
        throw std::logic_error("rnic-cn source serialization completed in the past");
    }

    std::vector<SerializedFrame> due;
    due.reserve(static_cast<std::size_t>(std::distance(pending_launches.begin(), due_end)));
    for (auto item = pending_launches.begin(); item != due_end; ++item) {
        if (item->first != now_ps) {
            throw std::logic_error("rnic-cn launch batch spans multiple timestamps");
        }
        due.push_back(item->second);
    }
    pending_launches.erase(pending_launches.begin(), due_end);
    for (const SerializedFrame& frame : due) {
        launchFrame(frame);
    }
    activateDeclaredFlows();
}

void RnicCollectiveNetworkRuntime::Impl::launchFrame(const SerializedFrame& frame) {
    FlowState& flow = requireFlow(frame.flow_id);
    const Route& route = selectRoute(frame.source, frame.destination);
    const packetid_t packet_id = takePacketId();
    RnicCollectivePacket* packet = nullptr;

    switch (frame.kind) {
        case RnicCollectivePacketKind::DATA:
            if (!frame.data.has_value()) {
                throw std::logic_error("rnic-cn DATA launch lost its metadata");
            }
            packet = RnicCollectivePacket::newData(flow.packet_flow, route, packet_id,
                                                   frame.flow_id, frame.source, frame.destination,
                                                   *frame.data, packet_observer);
            break;
        case RnicCollectivePacketKind::DECLARE:
            if (!frame.declaration.has_value()) {
                throw std::logic_error("rnic-cn DECLARE launch lost its nflow metadata");
            }
            if (!flow.declaration_dispatched) {
                if (frame.declaration->nflow_ppm != flow.declared_nflow_ppm) {
                    throw std::logic_error(
                        "rnic-cn DECLARE launch conflicts with its stored nflow");
                }
                requireNode(flow.request.destination)
                    .reservation_ledger.registerDeclaration(frame.flow_id,
                                                            frame.declaration->nflow_ppm);
                flow.declaration_dispatched = true;
                declared_flows_pending_activation.push_back(frame.flow_id);
            }
            packet = RnicCollectivePacket::newDeclare(
                flow.packet_flow, route, packet_id, frame.flow_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.declaration, packet_observer);
            break;
        case RnicCollectivePacketKind::NFLOW_UPDATE:
            if (!frame.declaration.has_value()) {
                throw std::logic_error("rnic-cn NFLOW_UPDATE launch lost its nflow metadata");
            }
            packet = RnicCollectivePacket::newNflowUpdate(
                flow.packet_flow, route, packet_id, frame.flow_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.declaration, packet_observer);
            break;
        case RnicCollectivePacketKind::ACCEPT:
            if (!frame.grant.has_value()) {
                throw std::logic_error("rnic-cn ACCEPT launch lost its grant");
            }
            packet = RnicCollectivePacket::newAccept(
                flow.packet_flow, route, packet_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.grant, packet_observer);
            break;
        case RnicCollectivePacketKind::GRANT_UPDATE:
            if (!frame.grant.has_value()) {
                throw std::logic_error("rnic-cn UPDATE launch lost its grant");
            }
            packet = RnicCollectivePacket::newGrantUpdate(
                flow.packet_flow, route, packet_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.grant, packet_observer);
            break;
        case RnicCollectivePacketKind::GAP_NACK:
            if (!frame.gap_nack.has_value()) {
                throw std::logic_error("rnic-cn GAP_NACK launch lost its packet range");
            }
            packet = RnicCollectivePacket::newGapNack(
                flow.packet_flow, route, packet_id, frame.flow_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.gap_nack, packet_observer);
            ++flow.recovery.gap_nacks_dispatched;
            ++recovery_statistics.gap_nacks_dispatched;
            break;
        case RnicCollectivePacketKind::GAP_RESOLVED:
            if (!frame.gap_resolved.has_value()) {
                throw std::logic_error("rnic-cn GAP_RESOLVED launch lost its packet range");
            }
            packet = RnicCollectivePacket::newGapResolved(
                flow.packet_flow, route, packet_id, frame.flow_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.gap_resolved, packet_observer);
            ++gap_resolved_dispatched;
            break;
        case RnicCollectivePacketKind::RETIRE:
            if (!frame.retire.has_value()) {
                throw std::logic_error("rnic-cn RETIRE launch lost its metadata");
            }
            if (flow.retire_dispatched) {
                throw std::logic_error("rnic-cn dispatched RETIRE twice");
            }
            flow.retire_dispatched = true;
            packet = RnicCollectivePacket::newRetire(
                flow.packet_flow, route, packet_id, frame.flow_id, frame.source, frame.destination,
                frame.wire_bytes, *frame.retire, packet_observer);
            break;
    }

    if (route.path_id() < 0) {
        packet->free();
        throw std::logic_error("rnic-cn route has a negative path id");
    }
    packet->set_pathid(static_cast<std::uint32_t>(route.path_id()));
    if (frame.kind == RnicCollectivePacketKind::DATA) {
        if (frame.retransmission) {
            const auto retry = flow.tx_retry_states.find(frame.data->packet_index);
            if (retry == flow.tx_retry_states.end() ||
                frame.data->transmission_attempt > retry->second.highest_dispatched_attempt ||
                frame.data->transmission_attempt > retry->second.highest_authorized_attempt ||
                frame.data->transmission_attempt <= retry->second.resolved_through_attempt ||
                !logicalDataEqual(retry->second.logical_data, *frame.data)) {
                packet->free();
                throw std::logic_error(
                    "rnic-cn deterministic retransmission launch lost its sender state");
            }
            if (!first_retransmission_launch_diagnostic.has_value()) {
                std::ostringstream diagnostic;
                diagnostic << "flow_id=" << frame.flow_id
                           << " packet_index=" << frame.data->packet_index
                           << " attempt=" << frame.data->transmission_attempt
                           << " lifecycle=" << packet->lifecycleId()
                           << " htsim_packet_id=" << packet_id << " launch_ps=" << EventList::now()
                           << " eta_ps=" << frame.data->eta_ps;
                first_retransmission_launch_diagnostic = diagnostic.str();
            }
        }
        const DestinationData metadata{frame.flow_id, frame.destination, packet_id, *frame.data};
        if (!destination_data.emplace(packet->lifecycleId(), metadata).second) {
            packet->free();
            throw std::logic_error("duplicate rnic-cn destination lifecycle metadata");
        }
    }

    const bool inject_data_drop =
        frame.kind == RnicCollectivePacketKind::DATA &&
        data_drops_for_testing.erase(std::make_tuple(frame.flow_id, frame.data->packet_index,
                                                     frame.data->transmission_attempt)) != 0;
    const bool duplicate_original = frame.kind == RnicCollectivePacketKind::DATA &&
                                    frame.data->transmission_attempt == 0 &&
                                    !frame.duplicate_test_copy &&
                                    original_duplicates_for_testing.erase(std::make_pair(
                                        frame.flow_id, frame.data->packet_index)) != 0;
    if (duplicate_original) {
        SerializedFrame duplicate = frame;
        duplicate.duplicate_test_copy = true;
        pending_launches.emplace(EventList::now(), std::move(duplicate));
    }
    if (inject_data_drop) {
        packet->free();
    } else {
        packet->sendOn();
    }
    if (frame.kind == RnicCollectivePacketKind::DATA && !frame.retransmission &&
        !frame.duplicate_test_copy && frame.data->isFinalPacket()) {
        queueRetireControl(flow, EventList::now(), true);
    }
}

void RnicCollectiveNetworkRuntime::Impl::enqueueControl(NodeState& source, ControlFrame frame) {
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
    if (flow.source_payload_bytes_dispatched != flow.final_ledger.total_payload_bytes ||
        flow.source_wire_bytes_dispatched != flow.final_ledger.total_wire_bytes ||
        flow.source_data_packets_dispatched != flow.final_ledger.total_data_packets) {
        throw std::logic_error("rnic-cn RETIRE queued before exact source DATA closure");
    }
    NodeState& source = requireNode(flow.request.source);
    const std::uint64_t gap_detection_ps =
        flow.final_ledger.total_data_packets == 0 ? std::uint64_t{0} : flow.max_original_release_ps;
    flow.published_retire_gap_detection_ps = gap_detection_ps;
    enqueueControl(source,
                   {RnicCollectivePacketKind::RETIRE, flow.request.flow_id, flow.request.source,
                    flow.request.destination, config.control_wire_bytes, std::nullopt, std::nullopt,
                    RnicCollectiveRetireMetadata{flow.final_ledger, gap_detection_ps},
                    eligible_time_ps, false, inherits_exact_serializer_boundary});
    flow.retire_control_queued = true;
}

void RnicCollectiveNetworkRuntime::Impl::processEndpointArrivals(
    TimePs now_ps,
    std::vector<AtlahsFlowId>& completions) {
    std::vector<EndpointArrival> arrivals;
    arrivals.swap(endpoint_arrivals);
    for (const EndpointArrival& arrival : arrivals) {
        if (arrival.arrival_time_ps != now_ps) {
            throw std::logic_error("rnic-cn endpoint arrival escaped its timestamp microphase");
        }
        FlowState& flow = requireFlow(arrival.flow_id);
        if (arrival.source != flow.request.source ||
            arrival.destination != flow.request.destination) {
            const bool reverse_control = arrival.kind == RnicCollectivePacketKind::ACCEPT ||
                                         arrival.kind == RnicCollectivePacketKind::GRANT_UPDATE ||
                                         arrival.kind == RnicCollectivePacketKind::GAP_NACK ||
                                         arrival.kind == RnicCollectivePacketKind::GAP_RESOLVED;
            if (!reverse_control || arrival.source != flow.request.destination ||
                arrival.destination != flow.request.source) {
                throw std::invalid_argument(
                    "rnic-cn packet endpoints do not match its ATLAHS flow");
            }
        }

        switch (arrival.kind) {
            case RnicCollectivePacketKind::DATA:
                processDataArrival(arrival, completions);
                break;
            case RnicCollectivePacketKind::DECLARE: {
                if (!arrival.declaration.has_value()) {
                    throw std::logic_error("rnic-cn receiver observed DECLARE without nflow");
                }
                // The current runtime starts one physical L4 flow per request.
                if (arrival.declaration->nflow_ppm == 0 ||
                    arrival.declaration->nflow_ppm >
                        RnicCollectiveController::kFullFlowPpm) {
                    throw std::invalid_argument(
                        "rnic-cn DECLARE nflow_ppm must be in [1, one flow]");
                }
                NodeState& receiver =
                    requireNode(flow.request.destination);
                if (flow.declaration_observed) {
                    // DECLAREs never expire: a repeat for an active flow is
                    // an idempotent no-op that re-sends the current window
                    // feedback, and a repeat for a retired flow is counted
                    // and ignored, never a throw.
                    if (!receiver.controller.contains(flow.request.flow_id) ||
                        flow.receiver_retired) {
                        ++recovery_statistics.stale_declarations_ignored;
                    } else {
                        queueRateFeedback(flow, now_ps);
                    }
                    break;
                }
                MembershipChangeSet& change =
                    receiver.membership_changes[now_ps];
                if (!change.declared_nflow_by_flow
                         .emplace(flow.request.flow_id, arrival.declaration->nflow_ppm)
                         .second) {
                    throw std::logic_error(
                        "rnic-cn membership change duplicates DECLARE");
                }
                flow.declaration_observed = true;
                break;
            }
            case RnicCollectivePacketKind::NFLOW_UPDATE: {
                if (!arrival.declaration.has_value()) {
                    throw std::logic_error(
                        "rnic-cn receiver observed NFLOW_UPDATE without nflow");
                }
                NodeState& receiver = requireNode(flow.request.destination);
                // Freeze the current window first: the update takes effect
                // in snapshots from the boundary of the window after its
                // arrival (book 1.4).
                frozenSnapshotFor(receiver, now_ps);
                if (flow.receiver_retired ||
                    !receiver.controller.updateDeclaration(
                        flow.request.flow_id, arrival.declaration->nflow_ppm)) {
                    ++recovery_statistics.stale_nflow_updates_ignored;
                    break;
                }
                receiver.reservation_ledger.updateDeclaration(
                    flow.request.flow_id, arrival.declaration->nflow_ppm);
                break;
            }
            case RnicCollectivePacketKind::ACCEPT:
                if (!arrival.grant.has_value()) {
                    throw std::logic_error("rnic-cn ACCEPT lost its explicit-rate grant");
                }
                applyFeedbackArrival(flow,
                                     flow.sender_gate.receiveAccept(*arrival.grant, now_ps));
                break;
            case RnicCollectivePacketKind::GRANT_UPDATE:
                if (!arrival.grant.has_value()) {
                    throw std::logic_error(
                        "rnic-cn resequenced ACK lost its explicit-rate grant");
                }
                ++flow.rate_feedback_acks_received;
                applyFeedbackArrival(flow,
                                     flow.sender_gate.applyRateFeedback(*arrival.grant, now_ps));
                break;
            case RnicCollectivePacketKind::GAP_NACK:
                processGapNackArrival(arrival);
                break;
            case RnicCollectivePacketKind::GAP_RESOLVED:
                processGapResolvedArrival(arrival, now_ps);
                break;
            case RnicCollectivePacketKind::RETIRE:
                if (!arrival.retire.has_value() ||
                    !ledgersEqual(arrival.retire->final_ledger, flow.final_ledger)) {
                    throw std::invalid_argument("rnic-cn RETIRE final ledger is inconsistent");
                }
                if (flow.retire_received) {
                    throw std::logic_error("rnic-cn receiver observed RETIRE twice");
                }
                flow.retire_received = true;
                if (flow.final_ledger.total_data_packets == 0) {
                    if (flow.delivery_completion_time_ps.has_value()) {
                        throw std::logic_error("rnic-cn zero-DATA flow completed twice");
                    }
                    flow.delivery_completion_time_ps = now_ps;
                    completions.push_back(flow.request.flow_id);
                } else if (!flow.delivery_completion_time_ps.has_value()) {
                    // RETIRE closes the sequence, but it does not manufacture a
                    // missing-packet entry for every original DATA packet still
                    // inside the Ring-CAM.  Audit the remaining post-resequence
                    // frontier once, at the published tail deadline.  RX releases
                    // at that timestamp are processed before this audit.
                    pending_tail_gap_audits.emplace(
                        std::max(now_ps, arrival.retire->final_gap_detection_ps),
                        flow.request.flow_id);
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
        throw std::logic_error("rnic-cn endpoint DATA has no lifecycle side-table entry");
    }
    const DestinationData& expected = pending->second;
    const packetid_t htsim_packet_id = expected.htsim_packet_id;
    const RnicCollectiveDataMetadata& received = *arrival.data;
    const RnicCollectiveDataMetadata& sent = expected.data;
    if (received.transmission_attempt > config.maximum_retransmissions) {
        throw std::invalid_argument("rnic-cn DATA exceeds the configured retry guard");
    }
    if (expected.flow_id != arrival.flow_id || expected.destination != arrival.destination ||
        received.packet_index != sent.packet_index ||
        received.payload_byte_offset != sent.payload_byte_offset ||
        received.eta_ps != sent.eta_ps ||
        received.transmission_attempt != sent.transmission_attempt ||
        !extentsEqual(received.extent, sent.extent) ||
        !ledgersEqual(received.final_ledger, sent.final_ledger)) {
        throw std::invalid_argument("rnic-cn endpoint DATA changed in the fabric");
    }

    FlowState& flow = requireFlow(arrival.flow_id);
    const bool already_delivered = received.packet_index < flow.delivered_data_packets;
    const bool already_ready = flow.ready_packets.count(received.packet_index) != 0;
    const bool already_admitted = flow.admitted_packets.count(received.packet_index) != 0;
    const bool already_resequenced =
        received.packet_index < flow.next_resequence_packet ||
        flow.resequenced_out_of_order.count(received.packet_index) != 0;
    auto missing = flow.rx_missing_packets.find(received.packet_index);
    bool stale_attempt =
        received.transmission_attempt != 0 && missing == flow.rx_missing_packets.end();
    bool accepted_attempt_already_present = false;
    if (missing != flow.rx_missing_packets.end()) {
        if (!logicalDataEqual(missing->second.logical_data, received)) {
            throw std::invalid_argument("rnic-cn duplicate DATA conflicts with its missing range");
        }
        accepted_attempt_already_present = missing->second.accepted_lifecycle.has_value();
        if (missing->second.highest_late_attempt.has_value() &&
            received.transmission_attempt <= *missing->second.highest_late_attempt) {
            stale_attempt = true;
        } else if (received.transmission_attempt != 0 && missing->second.last_nack_attempt == 0) {
            throw std::logic_error("rnic-cn DATA retransmission escaped receiver NACK state");
        }
    }
    if (already_delivered || already_ready || already_admitted || already_resequenced ||
        stale_attempt || accepted_attempt_already_present) {
        destination_data.erase(pending);
        ++flow.recovery.duplicate_data_packets_ignored;
        ++recovery_statistics.duplicate_data_packets_ignored;
        return;
    }

    NodeState& receiver = requireNode(arrival.destination);
    const RnicRingCamPacket ring_packet{
        arrival.lifecycle_id,    arrival.flow_id, received.eta_ps,
        arrival.arrival_time_ps, received.extent,
    };
    const RnicRxArrivalResult result = receiver.node.rxPort().processArrival(ring_packet);
    processRxReleases(result.serializations_scheduled_before_admission);
    processRxCompletions(result.packets_completed_through_arrival, completions);
    if (result.admission != RnicRingCamAdmission::Admitted) {
        destination_data.erase(pending);
        if (result.admission == RnicRingCamAdmission::Late) {
            if (!first_late_data_diagnostic.has_value()) {
                std::ostringstream diagnostic;
                diagnostic << "flow_id=" << arrival.flow_id
                           << " packet_index=" << received.packet_index
                           << " attempt=" << received.transmission_attempt
                           << " lifecycle=" << arrival.lifecycle_id
                           << " htsim_packet_id=" << htsim_packet_id << " source=" << arrival.source
                           << " destination=" << arrival.destination
                           << " eta_ps=" << received.eta_ps
                           << " arrival_ps=" << arrival.arrival_time_ps
                           << " age_ps=" << arrival.arrival_time_ps - received.eta_ps
                           << renderNsTm3QueueDiagnostic(topology);
                first_late_data_diagnostic = diagnostic.str();
            }
            ++flow.recovery.late_data_packets;
            ++recovery_statistics.late_data_packets;
            ++late_data_by_attempt[received.transmission_attempt];
            scheduleGapNack(flow, received, arrival.arrival_time_ps, true);
            return;
        }
        std::ostringstream message;
        message << "rnic-cn Ring-CAM rejected DATA as " << admissionName(result.admission)
                << " flow_id=" << arrival.flow_id << " packet_index=" << received.packet_index
                << " htsim_packet_id=" << htsim_packet_id << " source=" << arrival.source
                << " destination=" << arrival.destination << " eta_ps=" << received.eta_ps
                << " arrival_ps=" << arrival.arrival_time_ps;
        if (arrival.arrival_time_ps >= received.eta_ps) {
            message << " queueing_age_ps=" << arrival.arrival_time_ps - received.eta_ps;
        }
        message << renderNsTm3QueueDiagnostic(topology);
        throw std::runtime_error(message.str());
    }
    if (!result.logical_release_ps.has_value()) {
        throw std::logic_error("rnic-cn admitted DATA has no Ring-CAM release time");
    }
    if (!flow.admitted_packets
             .emplace(received.packet_index,
                      RxAdmittedPacket{arrival.lifecycle_id, received.transmission_attempt,
                                       *result.logical_release_ps})
             .second) {
        throw std::logic_error("rnic-cn admitted DATA tracker duplicates a logical packet");
    }
    missing = flow.rx_missing_packets.find(received.packet_index);
    if (missing != flow.rx_missing_packets.end()) {
        missing->second.accepted_lifecycle = arrival.lifecycle_id;
    }
}

RnicCollectiveDataMetadata RnicCollectiveNetworkRuntime::Impl::logicalData(
    const FlowState& flow,
    std::uint64_t packet_index) const {
    if (packet_index >= flow.final_ledger.total_data_packets) {
        throw std::out_of_range("rnic-cn logical DATA index exceeds the final ledger");
    }
    const Wide offset_wide =
        static_cast<Wide>(packet_index) * config.packetization.maxPayloadBytes();
    if (offset_wide > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("rnic-cn logical DATA offset overflow");
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(offset_wide);
    if (offset >= flow.final_ledger.total_payload_bytes) {
        throw std::logic_error("rnic-cn packetization conflicts with the final ledger");
    }
    return {packet_index,
            offset,
            config.packetization.packetize(flow.final_ledger.total_payload_bytes - offset),
            0,
            flow.final_ledger,
            0,
    };
}

void RnicCollectiveNetworkRuntime::Impl::scheduleGapNack(FlowState& flow,
                                                         const RnicCollectiveDataMetadata& data,
                                                         TimePs decision_time_ps,
                                                         bool observed_late_packet) {
    if (decision_time_ps < EventList::now()) {
        throw std::logic_error("rnic-cn gap-NACK decision was scheduled in the past");
    }
    if (data.transmission_attempt >= config.maximum_retransmissions) {
        std::ostringstream message;
        message << "rnic-cn deterministic retransmission exhausted maximum attempts"
                << " flow_id=" << flow.request.flow_id << " packet_index=" << data.packet_index
                << " attempt=" << data.transmission_attempt
                << " maximum=" << config.maximum_retransmissions << " eta_ps=" << data.eta_ps
                << " late_data_packets=" << recovery_statistics.late_data_packets
                << " gap_nacks_received=" << recovery_statistics.gap_nacks_received
                << " deterministic_retransmissions="
                << recovery_statistics.deterministic_retransmissions
                << " deterministic_retransmission_wire_bytes="
                << recovery_statistics.deterministic_retransmission_wire_bytes;
        message << " late_attempts={";
        bool first_attempt = true;
        for (const auto& [attempt, count] : late_data_by_attempt) {
            if (!first_attempt) {
                message << ',';
            }
            message << attempt << ':' << count;
            first_attempt = false;
        }
        message << "} retransmission_attempts={";
        first_attempt = true;
        for (const auto& [attempt, count] : retransmissions_by_attempt) {
            if (!first_attempt) {
                message << ',';
            }
            message << attempt << ':' << count;
            first_attempt = false;
        }
        message << '}';
        if (first_retransmission_dispatch_ps.has_value()) {
            message << " first_retransmission_dispatch_ps=" << *first_retransmission_dispatch_ps;
        }
        if (first_late_data_diagnostic.has_value()) {
            message << " first_late={" << *first_late_data_diagnostic << '}';
        }
        if (first_retransmission_launch_diagnostic.has_value()) {
            message << " first_retransmission={" << *first_retransmission_launch_diagnostic << '}';
        }
        message << renderNsTm3QueueDiagnostic(topology);
        throw std::runtime_error(message.str());
    }
    if (data.transmission_attempt == std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("rnic-cn deterministic retransmission attempt overflow");
    }
    if (!flow.first_gap_observation_ps.has_value()) {
        flow.first_gap_observation_ps = EventList::now();
    }

    auto missing = flow.rx_missing_packets.find(data.packet_index);
    if (missing == flow.rx_missing_packets.end()) {
        RnicCollectiveDataMetadata logical = data;
        logical.eta_ps = 0;
        logical.transmission_attempt = 0;
        RxMissingPacket state{std::move(logical), data.transmission_attempt};
        if (observed_late_packet) {
            state.highest_late_attempt = data.transmission_attempt;
        }
        missing = flow.rx_missing_packets.emplace(data.packet_index, std::move(state)).first;
    } else {
        RxMissingPacket& state = missing->second;
        if (!logicalDataEqual(state.logical_data, data) || state.accepted_lifecycle.has_value()) {
            throw std::logic_error("rnic-cn failed retransmission conflicts with its gap state");
        }
        if (observed_late_packet && state.highest_late_attempt.has_value() &&
            data.transmission_attempt <= *state.highest_late_attempt) {
            return;
        }
        if (data.transmission_attempt <= state.last_failed_attempt &&
            (state.decision_pending || state.nack_outstanding)) {
            if (observed_late_packet) {
                state.highest_late_attempt = data.transmission_attempt;
            }
            return;
        }
        if (data.transmission_attempt != 0 && data.transmission_attempt < state.last_nack_attempt) {
            throw std::logic_error("rnic-cn failed retry has no matching receiver NACK");
        }
        state.last_failed_attempt = data.transmission_attempt;
        if (observed_late_packet) {
            state.highest_late_attempt = data.transmission_attempt;
        }
        if (data.transmission_attempt >= state.last_nack_attempt) {
            state.nack_outstanding = false;
        }
    }

    RxMissingPacket& state = missing->second;
    if (state.decision_pending) {
        return;
    }
    state.decision_pending = true;
    const std::uint32_t requested_attempt = data.transmission_attempt + 1;
    const RnicCollectiveGapNackMetadata gap_nack{
        data.packet_index, data.payload_byte_offset, data.extent, requested_attempt};
    pending_gap_decisions.emplace(decision_time_ps, GapDecision{flow.request.flow_id, gap_nack});
}

void RnicCollectiveNetworkRuntime::Impl::detectGapThrough(FlowState& flow,
                                                          std::uint64_t successor_packet_index,
                                                          TimePs decision_time_ps) {
    if (successor_packet_index > flow.final_ledger.total_data_packets) {
        throw std::out_of_range("rnic-cn successor index exceeds the final DATA ledger");
    }
    for (std::uint64_t packet_index = flow.next_resequence_packet;
         packet_index < successor_packet_index; ++packet_index) {
        if (flow.resequenced_out_of_order.count(packet_index) != 0 ||
            flow.rx_missing_packets.count(packet_index) != 0 ||
            flow.admitted_packets.count(packet_index) != 0) {
            continue;
        }
        scheduleGapNack(flow, logicalData(flow, packet_index), decision_time_ps);
    }
}

void RnicCollectiveNetworkRuntime::Impl::processRxReleases(
    const std::vector<RnicRxScheduledSerialization>& released) {
    for (const RnicRxScheduledSerialization& packet : released) {
        processRxRelease(packet);
    }
}

void RnicCollectiveNetworkRuntime::Impl::processRxRelease(
    const RnicRxScheduledSerialization& released) {
    const auto pending = destination_data.find(released.release.packet.packet_id);
    if (pending == destination_data.end()) {
        throw std::logic_error("rnic-cn post-resequence DATA has no lifecycle metadata");
    }
    const DestinationData& metadata = pending->second;
    FlowState& flow = requireFlow(metadata.flow_id);
    const std::uint64_t packet_index = metadata.data.packet_index;

    const auto admitted = flow.admitted_packets.find(packet_index);
    if (admitted == flow.admitted_packets.end() ||
        admitted->second.lifecycle_id != released.release.packet.packet_id ||
        admitted->second.transmission_attempt != metadata.data.transmission_attempt ||
        admitted->second.logical_release_ps != released.release.logical_release_ps) {
        throw std::logic_error("rnic-cn post-resequence release escaped its admitted tracker");
    }

    auto missing = flow.rx_missing_packets.find(packet_index);
    if (missing != flow.rx_missing_packets.end()) {
        if (!missing->second.accepted_lifecycle.has_value() ||
            *missing->second.accepted_lifecycle != released.release.packet.packet_id) {
            throw std::logic_error("rnic-cn gap closed by an unauthenticated DATA lifecycle");
        }
        if (missing->second.last_nack_attempt != 0) {
            queueGapResolved(
                flow, missing->second,
                std::max(missing->second.last_nack_attempt, metadata.data.transmission_attempt),
                EventList::now());
        }
        flow.rx_missing_packets.erase(missing);
    }
    flow.admitted_packets.erase(admitted);

    // Feedback rides the resequenced stream: the ACK of every released DATA
    // packet carries the receiver's window-frozen (n_hat, wire_rate).
    queueRateFeedback(flow, released.release.logical_release_ps);

    if (packet_index < flow.next_resequence_packet ||
        flow.resequenced_out_of_order.count(packet_index) != 0) {
        throw std::logic_error("rnic-cn Ring-CAM released a duplicate logical packet");
    }
    if (packet_index > flow.next_resequence_packet) {
        detectGapThrough(flow, packet_index, released.release.logical_release_ps);
        flow.resequenced_out_of_order.insert(packet_index);
        return;
    }

    ++flow.next_resequence_packet;
    while (flow.resequenced_out_of_order.erase(flow.next_resequence_packet) != 0) {
        ++flow.next_resequence_packet;
    }
}

void RnicCollectiveNetworkRuntime::Impl::processDueTailGapAudits(TimePs now_ps) {
    const auto due_end = pending_tail_gap_audits.upper_bound(now_ps);
    for (auto audit = pending_tail_gap_audits.begin(); audit != due_end; ++audit) {
        if (audit->first != now_ps) {
            throw std::logic_error("rnic-cn RETIRE tail audit escaped its deadline");
        }
        FlowState& flow = requireFlow(audit->second);
        if (!flow.retire_received) {
            throw std::logic_error("rnic-cn tail audit ran before RETIRE observation");
        }
        if (flow.delivery_completion_time_ps.has_value()) {
            continue;
        }
        detectGapThrough(flow, flow.final_ledger.total_data_packets, now_ps);
    }
    pending_tail_gap_audits.erase(pending_tail_gap_audits.begin(), due_end);
}

void RnicCollectiveNetworkRuntime::Impl::queueDueGapNacks(TimePs now_ps) {
    const auto due_end = pending_gap_decisions.upper_bound(now_ps);
    for (auto decision = pending_gap_decisions.begin(); decision != due_end; ++decision) {
        if (decision->first != now_ps) {
            throw std::logic_error("rnic-cn gap-NACK escaped its decision timestamp");
        }
        FlowState& flow = requireFlow(decision->second.flow_id);
        auto missing = flow.rx_missing_packets.find(decision->second.gap_nack.packet_index);
        if (missing == flow.rx_missing_packets.end()) {
            // The packet crossed the post-resequencing interface before the
            // decision boundary.  No physical NACK existed, so this is a
            // canceled local decision rather than a duplicate control.
            continue;
        }
        RxMissingPacket& state = missing->second;
        const RnicCollectiveGapNackMetadata& gap_nack = decision->second.gap_nack;
        if (!state.decision_pending || state.accepted_lifecycle.has_value() ||
            state.last_failed_attempt + 1 != gap_nack.requested_transmission_attempt) {
            // As above, stale pre-dispatch state is not a physical duplicate.
            continue;
        }
        state.decision_pending = false;
        state.nack_outstanding = true;
        state.last_nack_attempt = gap_nack.requested_transmission_attempt;
        if (!flow.first_gap_decision_ps.has_value()) {
            flow.first_gap_decision_ps = now_ps;
        }
        NodeState& receiver = requireNode(flow.request.destination);
        enqueueControl(receiver,
                       {RnicCollectivePacketKind::GAP_NACK, flow.request.flow_id,
                        flow.request.destination, flow.request.source, config.control_wire_bytes,
                        std::nullopt, std::nullopt, std::nullopt, now_ps, false, false, gap_nack});
    }
    pending_gap_decisions.erase(pending_gap_decisions.begin(), due_end);
}

void RnicCollectiveNetworkRuntime::Impl::processGapNackArrival(const EndpointArrival& arrival) {
    if (!arrival.gap_nack.has_value()) {
        throw std::logic_error("rnic-cn sender observed GAP_NACK without a packet range");
    }
    FlowState& flow = requireFlow(arrival.flow_id);
    ++flow.recovery.gap_nacks_received;
    ++recovery_statistics.gap_nacks_received;
    const RnicCollectiveGapNackMetadata& gap_nack = *arrival.gap_nack;
    const RnicCollectiveDataMetadata logical = logicalData(flow, gap_nack.packet_index);
    if (logical.payload_byte_offset != gap_nack.payload_byte_offset ||
        !extentsEqual(logical.extent, gap_nack.extent)) {
        throw std::invalid_argument("rnic-cn GAP_NACK changed its logical packet range");
    }
    if (gap_nack.requested_transmission_attempt > config.maximum_retransmissions) {
        throw std::logic_error("rnic-cn GAP_NACK exceeds the maximum retry guard");
    }

    auto retry = flow.tx_retry_states.find(gap_nack.packet_index);
    if (retry == flow.tx_retry_states.end()) {
        retry = flow.tx_retry_states.emplace(gap_nack.packet_index, TxRetryState{logical}).first;
    } else if (!logicalDataEqual(retry->second.logical_data, logical)) {
        throw std::logic_error("rnic-cn sender retry state changed its logical packet");
    }
    TxRetryState& state = retry->second;
    if (gap_nack.requested_transmission_attempt <= state.resolved_through_attempt ||
        gap_nack.requested_transmission_attempt <= state.highest_authorized_attempt) {
        ++flow.recovery.duplicate_gap_nacks_ignored;
        ++recovery_statistics.duplicate_gap_nacks_ignored;
        return;
    }
    if (flow.receiver_retired) {
        throw std::logic_error("rnic-cn retired sender received an uncanceled GAP_NACK");
    }
    if (gap_nack.requested_transmission_attempt != state.highest_authorized_attempt + 1) {
        throw std::logic_error("rnic-cn GAP_NACK skipped a deterministic retry attempt");
    }
    if (gap_nack.requested_transmission_attempt > 1) {
        cancelRetryTimeouts(flow.request.flow_id, gap_nack.packet_index,
                            gap_nack.requested_transmission_attempt - 1);
    }
    authorizeRetry(flow, state, gap_nack.requested_transmission_attempt);
}

void RnicCollectiveNetworkRuntime::Impl::authorizeRetry(FlowState& flow,
                                                        TxRetryState& state,
                                                        std::uint32_t transmission_attempt) {
    if (transmission_attempt == 0 || transmission_attempt > config.maximum_retransmissions ||
        transmission_attempt != state.highest_authorized_attempt + 1 ||
        transmission_attempt <= state.resolved_through_attempt) {
        throw std::logic_error("rnic-cn invalid deterministic retry authorization");
    }
    RnicCollectiveDataMetadata retransmission = state.logical_data;
    retransmission.eta_ps = 0;
    retransmission.transmission_attempt = transmission_attempt;
    NodeState& source = requireNode(flow.request.source);
    RnicTxPort& tx = source.node.txPort();
    if (tx.nextWireOpportunityPs() <= EventList::now()) {
        tx.rebaseDataClassIdle(EventList::now());
    }
    flow.retransmission_queue.push_back(std::move(retransmission));
    source.retransmission_flow_ids.insert(flow.request.flow_id);
    tx.setRetransmissionPending(flow.request.flow_id, true);
    state.highest_authorized_attempt = transmission_attempt;
}

void RnicCollectiveNetworkRuntime::Impl::cancelRetryTimeouts(AtlahsFlowId flow_id,
                                                             std::uint64_t packet_index,
                                                             std::uint32_t through_attempt) {
    auto timeout = pending_retry_timeouts.begin();
    while (timeout != pending_retry_timeouts.end()) {
        if (timeout->second.flow_id == flow_id && timeout->second.packet_index == packet_index &&
            timeout->second.transmission_attempt <= through_attempt) {
            timeout = pending_retry_timeouts.erase(timeout);
        } else {
            ++timeout;
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::queueGapResolved(FlowState& flow,
                                                          const RxMissingPacket& missing,
                                                          std::uint32_t acknowledged_attempt,
                                                          TimePs eligible_time_ps) {
    if (missing.last_nack_attempt == 0 || acknowledged_attempt < missing.last_nack_attempt ||
        acknowledged_attempt > config.maximum_retransmissions) {
        throw std::logic_error("rnic-cn cannot resolve a gap before a physical NACK exists");
    }
    NodeState& receiver = requireNode(flow.request.destination);
    enqueueControl(
        receiver,
        {RnicCollectivePacketKind::GAP_RESOLVED, flow.request.flow_id, flow.request.destination,
         flow.request.source, config.control_wire_bytes, std::nullopt, std::nullopt, std::nullopt,
         eligible_time_ps, false, false, std::nullopt,
         RnicCollectiveGapResolvedMetadata{missing.logical_data.packet_index,
                                           missing.logical_data.payload_byte_offset,
                                           missing.logical_data.extent, acknowledged_attempt}});
}

void RnicCollectiveNetworkRuntime::Impl::processGapResolvedArrival(const EndpointArrival& arrival,
                                                                   TimePs now_ps) {
    if (!arrival.gap_resolved.has_value()) {
        throw std::logic_error("rnic-cn sender observed GAP_RESOLVED without a packet range");
    }
    ++gap_resolved_received;
    FlowState& flow = requireFlow(arrival.flow_id);
    const RnicCollectiveGapResolvedMetadata& resolved = *arrival.gap_resolved;
    if (resolved.acknowledged_transmission_attempt > config.maximum_retransmissions) {
        throw std::logic_error("rnic-cn GAP_RESOLVED exceeds the retry guard");
    }
    const RnicCollectiveDataMetadata logical = logicalData(flow, resolved.packet_index);
    if (logical.payload_byte_offset != resolved.payload_byte_offset ||
        !extentsEqual(logical.extent, resolved.extent)) {
        throw std::invalid_argument("rnic-cn GAP_RESOLVED changed its logical packet range");
    }
    auto retry = flow.tx_retry_states.find(resolved.packet_index);
    if (retry == flow.tx_retry_states.end()) {
        retry = flow.tx_retry_states.emplace(resolved.packet_index, TxRetryState{logical}).first;
    }
    TxRetryState& state = retry->second;
    if (state.logical_data.payload_byte_offset != resolved.payload_byte_offset ||
        !extentsEqual(state.logical_data.extent, resolved.extent)) {
        throw std::invalid_argument("rnic-cn GAP_RESOLVED changed its logical packet range");
    }
    state.highest_authorized_attempt =
        std::max(state.highest_authorized_attempt, resolved.acknowledged_transmission_attempt);
    if (resolved.acknowledged_transmission_attempt > state.resolved_through_attempt) {
        state.resolved_through_attempt = resolved.acknowledged_transmission_attempt;
        cancelRetryTimeouts(flow.request.flow_id, resolved.packet_index,
                            resolved.acknowledged_transmission_attempt);
        pruneStaleQueuedRetransmissions(flow);
    }
    maybeQueueRetirement(flow, now_ps);
}

void RnicCollectiveNetworkRuntime::Impl::processDueRetryTimeouts(TimePs now_ps) {
    const auto due_end = pending_retry_timeouts.upper_bound(now_ps);
    std::vector<RetryTimeout> due;
    for (auto timeout = pending_retry_timeouts.begin(); timeout != due_end; ++timeout) {
        if (timeout->first != now_ps) {
            throw std::logic_error("rnic-cn retry timeout escaped its deadline");
        }
        due.push_back(timeout->second);
    }
    pending_retry_timeouts.erase(pending_retry_timeouts.begin(), due_end);

    for (const RetryTimeout& timeout : due) {
        FlowState& flow = requireFlow(timeout.flow_id);
        auto retry = flow.tx_retry_states.find(timeout.packet_index);
        if (retry == flow.tx_retry_states.end()) {
            continue;
        }
        TxRetryState& state = retry->second;
        if (timeout.transmission_attempt <= state.resolved_through_attempt ||
            timeout.transmission_attempt < state.highest_dispatched_attempt ||
            timeout.transmission_attempt < state.highest_authorized_attempt ||
            timeout.transmission_attempt <= state.timeout_fired_through_attempt) {
            continue;
        }
        if (timeout.transmission_attempt != state.highest_dispatched_attempt) {
            throw std::logic_error("rnic-cn retry timeout precedes physical dispatch");
        }
        state.timeout_fired_through_attempt = timeout.transmission_attempt;
        if (timeout.transmission_attempt >= config.maximum_retransmissions) {
            std::ostringstream message;
            message << "rnic-cn deterministic retransmission exhausted "
                       "maximum attempts after sender RTO"
                    << " flow_id=" << flow.request.flow_id
                    << " packet_index=" << timeout.packet_index
                    << " attempt=" << timeout.transmission_attempt
                    << " maximum=" << config.maximum_retransmissions;
            throw std::runtime_error(message.str());
        }
        authorizeRetry(flow, state, timeout.transmission_attempt + 1);
    }
}

void RnicCollectiveNetworkRuntime::Impl::duplicateCurrentGapNackForTesting(AtlahsFlowId flow_id) {
    FlowState& flow = requireFlow(flow_id);
    if (flow.rx_missing_packets.size() != 1) {
        throw std::logic_error("rnic-cn duplicate-NACK test seam requires one missing packet");
    }
    RxMissingPacket& state = flow.rx_missing_packets.begin()->second;
    if (!state.nack_outstanding || state.decision_pending || state.last_nack_attempt == 0) {
        throw std::logic_error("rnic-cn duplicate-NACK test seam requires an in-flight NACK");
    }
    const RnicCollectiveGapNackMetadata gap_nack{
        state.logical_data.packet_index, state.logical_data.payload_byte_offset,
        state.logical_data.extent, state.last_nack_attempt};
    NodeState& receiver = requireNode(flow.request.destination);
    enqueueControl(receiver, {RnicCollectivePacketKind::GAP_NACK, flow.request.flow_id,
                              flow.request.destination, flow.request.source,
                              config.control_wire_bytes, std::nullopt, std::nullopt, std::nullopt,
                              EventList::now(), false, false, gap_nack});
    wakeAt(EventList::now());
}

void RnicCollectiveNetworkRuntime::Impl::duplicateCurrentRetransmissionForTesting(
    AtlahsFlowId flow_id) {
    FlowState& flow = requireFlow(flow_id);
    if (flow.tx_retry_states.size() != 1) {
        throw std::logic_error("rnic-cn duplicate-DATA test seam requires one sender retry state");
    }
    TxRetryState& state = flow.tx_retry_states.begin()->second;
    if (state.highest_dispatched_attempt == 0 ||
        state.highest_dispatched_attempt > state.highest_authorized_attempt ||
        state.highest_dispatched_attempt <= state.resolved_through_attempt) {
        throw std::logic_error(
            "rnic-cn duplicate-DATA test seam requires an in-flight retransmission");
    }
    RnicCollectiveDataMetadata retransmission = state.logical_data;
    retransmission.eta_ps = 0;
    retransmission.transmission_attempt = state.highest_dispatched_attempt;
    NodeState& source = requireNode(flow.request.source);
    RnicTxPort& tx = source.node.txPort();
    if (tx.nextWireOpportunityPs() <= EventList::now()) {
        tx.rebaseDataClassIdle(EventList::now());
    }
    flow.retransmission_queue.push_back(std::move(retransmission));
    source.retransmission_flow_ids.insert(flow.request.flow_id);
    tx.setRetransmissionPending(flow.request.flow_id, true);
    wakeAt(EventList::now());
}

void RnicCollectiveNetworkRuntime::Impl::dropOriginalDataForTesting(AtlahsFlowId flow_id,
                                                                    std::uint64_t packet_index) {
    dropDataAttemptForTesting(flow_id, packet_index, 0);
}

void RnicCollectiveNetworkRuntime::Impl::dropDataAttemptForTesting(
    AtlahsFlowId flow_id,
    std::uint64_t packet_index,
    std::uint32_t transmission_attempt) {
    if (!data_drops_for_testing.emplace(flow_id, packet_index, transmission_attempt).second) {
        throw std::logic_error("rnic-cn DATA-drop test seam duplicates an armed packet");
    }
}

void RnicCollectiveNetworkRuntime::Impl::duplicateOriginalDataForTesting(
    AtlahsFlowId flow_id,
    std::uint64_t packet_index) {
    if (!original_duplicates_for_testing.emplace(flow_id, packet_index).second) {
        throw std::logic_error("rnic-cn original-duplicate test seam is already armed");
    }
}

void RnicCollectiveNetworkRuntime::Impl::redeclareFlowForTesting(AtlahsFlowId flow_id) {
    FlowState& flow = requireFlow(flow_id);
    if (!flow.declaration_dispatched) {
        throw std::logic_error("rnic-cn re-DECLARE seam requires a dispatched declaration");
    }
    NodeState& source = requireNode(flow.request.source);
    enqueueControl(source,
                   {RnicCollectivePacketKind::DECLARE, flow.request.flow_id,
                    flow.request.source, flow.request.destination, config.control_wire_bytes,
                    RnicCollectiveDeclareMetadata{flow.declared_nflow_ppm},
                    std::nullopt, std::nullopt, EventList::now(), false, false});
    wakeAt(EventList::now());
}

void RnicCollectiveNetworkRuntime::Impl::replayResolvedGapNackForTesting(
    AtlahsFlowId flow_id,
    std::uint64_t packet_index) {
    FlowState& flow = requireFlow(flow_id);
    const auto retry = flow.tx_retry_states.find(packet_index);
    if (retry == flow.tx_retry_states.end() || retry->second.resolved_through_attempt == 0) {
        throw std::logic_error("rnic-cn stale-NACK seam requires a physically resolved gap");
    }
    const RnicCollectiveDataMetadata logical = logicalData(flow, packet_index);
    const RnicCollectiveGapNackMetadata stale_nack{logical.packet_index,
                                                   logical.payload_byte_offset, logical.extent,
                                                   retry->second.resolved_through_attempt};
    NodeState& receiver = requireNode(flow.request.destination);
    enqueueControl(receiver, {RnicCollectivePacketKind::GAP_NACK, flow.request.flow_id,
                              flow.request.destination, flow.request.source,
                              config.control_wire_bytes, std::nullopt, std::nullopt, std::nullopt,
                              EventList::now(), false, false, stale_nack});
    wakeAt(EventList::now());
}

bool RnicCollectiveNetworkRuntime::Impl::queuedRetransmissionIsCurrent(
    const FlowState& flow,
    const RnicCollectiveDataMetadata& retransmission) const {
    const auto retry = flow.tx_retry_states.find(retransmission.packet_index);
    return retry != flow.tx_retry_states.end() &&
           retransmission.transmission_attempt <= retry->second.highest_authorized_attempt &&
           retransmission.transmission_attempt > retry->second.resolved_through_attempt &&
           logicalDataEqual(retry->second.logical_data, retransmission);
}

void RnicCollectiveNetworkRuntime::Impl::pruneStaleQueuedRetransmissions(FlowState& flow) {
    auto retransmission = flow.retransmission_queue.begin();
    while (retransmission != flow.retransmission_queue.end()) {
        if (queuedRetransmissionIsCurrent(flow, *retransmission)) {
            ++retransmission;
        } else {
            retransmission = flow.retransmission_queue.erase(retransmission);
        }
    }
    NodeState& source = requireNode(flow.request.source);
    RnicTxPort& tx = source.node.txPort();
    const bool pending = !flow.retransmission_queue.empty();
    if (pending) {
        source.retransmission_flow_ids.insert(flow.request.flow_id);
    } else {
        source.retransmission_flow_ids.erase(flow.request.flow_id);
    }
    if (tx.hasRetransmissionPending(flow.request.flow_id) != pending) {
        tx.setRetransmissionPending(flow.request.flow_id, pending);
    }
}

void RnicCollectiveNetworkRuntime::Impl::settleReceivePorts(
    TimePs now_ps,
    std::vector<AtlahsFlowId>& completions) {
    for (const auto& node_state : nodes) {
        const std::optional<TimePs> next = node_state->node.rxPort().nextEventTimePs();
        if (!next.has_value() || *next > now_ps) {
            continue;
        }
        if (*next < now_ps) {
            throw std::logic_error("rnic-cn RX internal event was scheduled in the past");
        }
        const RnicRxAdvanceResult result =
            node_state->node.rxPort().advanceToWithCompletions(now_ps);
        processRxReleases(result.serializations_scheduled);
        processRxCompletions(result.packets_completed, completions);
        const std::optional<TimePs> remaining = node_state->node.rxPort().nextEventTimePs();
        if (remaining.has_value() && *remaining <= now_ps) {
            throw std::logic_error("rnic-cn RX event did not advance past its boundary");
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
    const auto pending = destination_data.find(completion.packet.packet_id);
    if (pending == destination_data.end()) {
        throw std::logic_error("rnic-cn RX completed unknown lifecycle metadata");
    }
    const DestinationData metadata = pending->second;
    FlowState& flow = requireFlow(metadata.flow_id);
    if (metadata.destination != flow.request.destination ||
        completion.packet.flow_id != metadata.flow_id ||
        completion.packet.eta_ps != metadata.data.eta_ps ||
        !extentsEqual(completion.packet.extent, metadata.data.extent)) {
        throw std::invalid_argument("rnic-cn RX completion metadata is inconsistent");
    }
    if (!ledgersEqual(metadata.data.final_ledger, flow.final_ledger)) {
        throw std::invalid_argument("rnic-cn DATA packet carries a conflicting final ledger");
    }
    destination_data.erase(pending);

    if (metadata.data.packet_index >= flow.next_resequence_packet &&
        flow.resequenced_out_of_order.count(metadata.data.packet_index) == 0) {
        throw std::logic_error("rnic-cn DATA completed before post-resequence observation");
    }

    if (metadata.data.packet_index < flow.delivered_data_packets ||
        flow.ready_packets.count(metadata.data.packet_index) != 0) {
        ++flow.recovery.duplicate_data_packets_ignored;
        ++recovery_statistics.duplicate_data_packets_ignored;
        return;
    }
    if (!flow.ready_packets
             .emplace(metadata.data.packet_index,
                      ReadyPacket{completion.packet.packet_id, metadata.htsim_packet_id,
                                  metadata.data, completion.serializer_end_ps})
             .second) {
        throw std::logic_error("rnic-cn RX ready ledger duplicates a logical packet");
    }
    drainReadyPackets(flow, completions);
}

void RnicCollectiveNetworkRuntime::Impl::drainReadyPackets(FlowState& flow,
                                                           std::vector<AtlahsFlowId>& completions) {
    while (true) {
        auto ready = flow.ready_packets.find(flow.delivered_data_packets);
        if (ready == flow.ready_packets.end()) {
            return;
        }
        const ReadyPacket packet = ready->second;
        if (packet.data.payload_byte_offset != flow.delivered_payload_bytes ||
            !ledgersEqual(packet.data.final_ledger, flow.final_ledger)) {
            throw std::logic_error("rnic-cn ready DATA conflicts with the exact RX ledger");
        }

        const std::uint64_t next_payload =
            checkedAdd(flow.delivered_payload_bytes, packet.data.extent.payloadBytes(),
                       "rnic-cn delivered payload ledger overflow");
        const std::uint64_t next_wire =
            checkedAdd(flow.delivered_wire_bytes, packet.data.extent.wireBytes(),
                       "rnic-cn delivered wire ledger overflow");
        const std::uint64_t next_packets =
            checkedAdd(flow.delivered_data_packets, 1, "rnic-cn delivered packet ledger overflow");
        if (next_payload > flow.final_ledger.total_payload_bytes ||
            next_wire > flow.final_ledger.total_wire_bytes ||
            next_packets > flow.final_ledger.total_data_packets) {
            throw std::logic_error("rnic-cn RX completion exceeded its final ledger");
        }

        flow.ready_packets.erase(ready);
        flow.delivered_payload_bytes = next_payload;
        flow.delivered_wire_bytes = next_wire;
        flow.delivered_data_packets = next_packets;
        if (next_packets != flow.final_ledger.total_data_packets) {
            continue;
        }
        if (next_payload != flow.final_ledger.total_payload_bytes ||
            next_wire != flow.final_ledger.total_wire_bytes ||
            flow.delivery_completion_time_ps.has_value() || !flow.rx_missing_packets.empty() ||
            !flow.admitted_packets.empty() ||
            flow.next_resequence_packet != flow.final_ledger.total_data_packets ||
            !flow.resequenced_out_of_order.empty() || !flow.ready_packets.empty()) {
            throw std::logic_error("rnic-cn final RX ledger retains a gap or duplicate");
        }
        flow.delivery_completion_time_ps =
            std::max(packet.serializer_end_ps, static_cast<TimePs>(EventList::now()));
        completions.push_back(flow.request.flow_id);
        maybeQueueRetirement(flow, *flow.delivery_completion_time_ps);
        return;
    }
}

void RnicCollectiveNetworkRuntime::Impl::maybeQueueRetirement(FlowState& flow, TimePs now_ps) {
    if (!flow.retire_received || flow.retirement_queued) {
        return;
    }
    if (flow.delivered_payload_bytes != flow.final_ledger.total_payload_bytes ||
        flow.delivered_wire_bytes != flow.final_ledger.total_wire_bytes ||
        flow.delivered_data_packets != flow.final_ledger.total_data_packets) {
        return;
    }
    if (!flow.rx_missing_packets.empty() || !flow.admitted_packets.empty() ||
        !flow.ready_packets.empty() || !flow.retransmission_queue.empty()) {
        return;
    }
    for (const auto& retry : flow.tx_retry_states) {
        if (retry.second.resolved_through_attempt < retry.second.highest_authorized_attempt) {
            return;
        }
    }
    if (!flow.declaration_observed) {
        throw std::logic_error("rnic-cn RETIRE became ready before DECLARE observation");
    }

    NodeState& receiver = requireNode(flow.request.destination);
    MembershipChangeSet& change = receiver.membership_changes[now_ps];
    if (!change.retired_flow_ids.insert(flow.request.flow_id).second) {
        throw std::logic_error("rnic-cn membership change duplicates RETIRE");
    }
    flow.retirement_queued = true;
}

void RnicCollectiveNetworkRuntime::Impl::processDueRateActivations(TimePs now_ps) {
    const auto due_end = pending_rate_activations.upper_bound(now_ps);
    for (auto due = pending_rate_activations.begin(); due != due_end; ++due) {
        if (due->first != now_ps) {
            throw std::logic_error("rnic-cn rate activation escaped its dwnd boundary");
        }
        FlowState& flow = requireFlow(due->second);
        // A superseded schedule or an already retired sender leaves a stale
        // record; the gate rejects it.
        if (!flow.sender_gate.activateScheduledRate(now_ps)) {
            continue;
        }
        requireNode(flow.request.source)
            .node.txPort()
            .setWireRateGrant(flow.request.flow_id, flow.sender_gate.currentWireRateBps());
    }
    pending_rate_activations.erase(pending_rate_activations.begin(), due_end);
}

std::uint32_t RnicCollectiveNetworkRuntime::Impl::egressFairSharePpm(
    std::size_t destination_count) const {
    if (destination_count == 0) {
        throw std::logic_error("rnic-cn egress composition requires a destination");
    }
    // Book 1.4: the fair-share request C_egress / n_dest expressed as a flow
    // fraction of the margin-derated receiver capacity. C_egress and
    // C_receiver are the same run constant today but remain distinct
    // quantities.
    const std::uint64_t egress_capacity_bps = config.access_wire_capacity_bps;
    const std::uint64_t receiver_derated_bps = marginDeratedCapacityBps(config);
    const Wide numerator = static_cast<Wide>(egress_capacity_bps) *
                           RnicCollectiveController::kPartsPerMillion;
    const Wide denominator =
        static_cast<Wide>(receiver_derated_bps) * destination_count;
    const Wide rounded = (numerator + denominator / 2) / denominator;
    if (rounded >= RnicCollectiveController::kFullFlowPpm) {
        return RnicCollectiveController::kFullFlowPpm;
    }
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(rounded));
}

std::uint64_t RnicCollectiveNetworkRuntime::Impl::requestedRateBps(
    std::uint32_t nflow_ppm) const {
    return static_cast<std::uint64_t>(
        static_cast<Wide>(marginDeratedCapacityBps(config)) * nflow_ppm /
        RnicCollectiveController::kPartsPerMillion);
}

void RnicCollectiveNetworkRuntime::Impl::processDueEgressRebalances(TimePs now_ps) {
    for (const auto& node_state : nodes) {
        NodeState& source = *node_state;
        if (!source.next_egress_rebalance_ps.has_value() ||
            *source.next_egress_rebalance_ps > now_ps) {
            continue;
        }
        if (*source.next_egress_rebalance_ps < now_ps) {
            throw std::logic_error("rnic-cn egress rebalance escaped its RTT boundary");
        }
        rebalanceEgress(source, now_ps);
        if (source.egress_flows_by_destination.empty()) {
            source.next_egress_rebalance_ps.reset();
        } else {
            const TimePs rtt_ps = checkedAdd(config.control_deadline_ps,
                                             config.control_deadline_ps,
                                             "rnic-cn rebalancer RTT overflow");
            source.next_egress_rebalance_ps =
                checkedAdd(now_ps, rtt_ps, "rnic-cn rebalancer boundary overflow");
            wakeAt(*source.next_egress_rebalance_ps);
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::rebalanceEgress(NodeState& source, TimePs now_ps) {
    // The RTT rebalancer (book 1.4): the single deliberately negative-
    // feedback element in the system, scoped to reclaiming sender-egress
    // waste at RTT (2 * dwnd) granularity. The per-window fast path stays
    // feedforward and pacing still changes only at window boundaries.
    struct Lane {
        FlowState* flow;
        std::uint64_t requested_bps;
        std::uint64_t granted_bps;
    };
    std::vector<Lane> lanes;
    Wide granted_sum = 0;
    for (const auto& by_destination : source.egress_flows_by_destination) {
        for (const AtlahsFlowId flow_id : by_destination.second) {
            FlowState& flow = requireFlow(flow_id);
            if (!flow.declaration_dispatched || flow.receiver_retired ||
                flow.sender_gate.phase() != RnicSenderGrantGate::Phase::Active) {
                continue;
            }
            const Lane lane{&flow, requestedRateBps(flow.declared_nflow_ppm),
                            flow.sender_gate.currentWireRateBps()};
            granted_sum += lane.granted_bps;
            lanes.push_back(lane);
        }
    }
    const std::uint64_t egress_capacity_bps = config.access_wire_capacity_bps;
    if (lanes.empty() || granted_sum >= egress_capacity_bps) {
        return;
    }
    const std::uint64_t slack_bps =
        egress_capacity_bps - static_cast<std::uint64_t>(granted_sum);

    // Slack goes proportionally to lanes whose grant equaled their request
    // (no oversubscription observed) and that still have declaration room;
    // under-granted lanes keep their current declaration.
    Wide satisfied_requested_sum = 0;
    for (const Lane& lane : lanes) {
        if (lane.granted_bps >= lane.requested_bps &&
            lane.flow->declared_nflow_ppm < lane.flow->nflow_budget_cap_ppm) {
            satisfied_requested_sum += lane.requested_bps;
        }
    }
    if (satisfied_requested_sum == 0) {
        return;
    }
    for (const Lane& lane : lanes) {
        if (lane.granted_bps < lane.requested_bps ||
            lane.flow->declared_nflow_ppm >= lane.flow->nflow_budget_cap_ppm) {
            continue;
        }
        const Wide extra_bps = static_cast<Wide>(slack_bps) * lane.requested_bps /
                               satisfied_requested_sum;
        const Wide new_rate_bps = static_cast<Wide>(lane.requested_bps) + extra_bps;
        const Wide derated = marginDeratedCapacityBps(config);
        Wide raised = (new_rate_bps * RnicCollectiveController::kPartsPerMillion +
                       derated / 2) /
                      derated;
        if (raised > lane.flow->nflow_budget_cap_ppm) {
            raised = lane.flow->nflow_budget_cap_ppm;
        }
        const std::uint32_t new_nflow_ppm =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(raised));
        if (new_nflow_ppm <= lane.flow->declared_nflow_ppm) {
            continue;
        }
        FlowState& flow = *lane.flow;
        flow.declared_nflow_ppm = new_nflow_ppm;
        ++flow.nflow_updates_dispatched;
        flow.sender_gate.updateOwnNflow(new_nflow_ppm);
        enqueueControl(source,
                       {RnicCollectivePacketKind::NFLOW_UPDATE, flow.request.flow_id,
                        flow.request.source, flow.request.destination,
                        config.control_wire_bytes,
                        RnicCollectiveDeclareMetadata{new_nflow_ppm},
                        std::nullopt, std::nullopt, now_ps, false, false});
    }
}

void RnicCollectiveNetworkRuntime::Impl::applyFeedbackArrival(
    FlowState& flow,
    RnicSenderFeedbackOutcome outcome) {
    if (outcome == RnicSenderFeedbackOutcome::AppliedNow) {
        requireNode(flow.request.source)
            .node.txPort()
            .setWireRateGrant(flow.request.flow_id, flow.sender_gate.currentWireRateBps());
        return;
    }
    if (outcome == RnicSenderFeedbackOutcome::Scheduled) {
        const std::optional<std::uint64_t> activation =
            flow.sender_gate.scheduledActivationTimePs();
        if (!activation.has_value()) {
            throw std::logic_error("rnic-cn scheduled feedback lost its dwnd boundary");
        }
        pending_rate_activations.emplace(*activation, flow.request.flow_id);
        wakeAt(*activation);
    }
}

void RnicCollectiveNetworkRuntime::Impl::beginMembershipChanges(TimePs now_ps) {
    for (const auto& node_state : nodes) {
        NodeState& receiver = *node_state;
        while (!receiver.membership_changes.empty()) {
            auto first = receiver.membership_changes.begin();
            if (first->first > now_ps) {
                break;
            }
            MembershipChangeSet change_set = std::move(first->second);
            receiver.membership_changes.erase(first);
            beginMembershipChange(receiver, now_ps, std::move(change_set));
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::beginMembershipChange(
        NodeState& receiver,
        TimePs observation_time_ps,
        MembershipChangeSet change_set) {
    RnicCollectiveMembershipDelta delta;
    delta.declarations.reserve(change_set.declared_nflow_by_flow.size());
    for (const auto& declaration : change_set.declared_nflow_by_flow) {
        delta.declarations.push_back({declaration.first, declaration.second});
    }
    delta.retired_flow_ids.assign(change_set.retired_flow_ids.begin(),
                                  change_set.retired_flow_ids.end());
    if (delta.declarations.empty() && delta.retired_flow_ids.empty()) {
        throw std::logic_error("rnic-cn membership change is empty");
    }
    for (const RnicCollectiveMembershipDeclaration& declaration : delta.declarations) {
        const AtlahsFlowId flow_id = declaration.flow_id;
        const FlowState& flow = requireFlow(flow_id);
        if (&requireNode(flow.request.destination) != &receiver ||
            receiver.controller.contains(flow_id)) {
            throw std::logic_error(
                "rnic-cn DECLARE targets invalid receiver state");
        }
    }
    for (const AtlahsFlowId flow_id : delta.retired_flow_ids) {
        const FlowState& flow = requireFlow(flow_id);
        if (&requireNode(flow.request.destination) != &receiver ||
            !receiver.controller.contains(flow_id)) {
            throw std::logic_error(
                "rnic-cn RETIRE targets invalid receiver state");
        }
        for (const auto& retry : flow.tx_retry_states) {
            if (retry.second.resolved_through_attempt <
                retry.second.highest_authorized_attempt) {
                throw std::logic_error(
                    "rnic-cn retirement overtook sender retry closure");
            }
        }
        if (!flow.retransmission_queue.empty()) {
            throw std::logic_error(
                "rnic-cn retirement retains a retry queue");
        }
    }

    // Freeze this window's snapshot before the mutation: feedback generated
    // anywhere inside the window reflects the state at its boundary.
    const RnicCollectiveRateSnapshot snapshot =
        frozenSnapshotFor(receiver, observation_time_ps);

    std::optional<RnicCollectiveMembershipUpdate> update =
        receiver.controller.updateMembership(delta);
    if (!update.has_value()) {
        throw std::logic_error("rnic-cn physical membership event caused no transition");
    }
    const std::uint32_t receiver_node =
        delta.declarations.empty()
            ? requireFlow(delta.retired_flow_ids.front()).request.destination
            : requireFlow(delta.declarations.front().flow_id).request.destination;

    // The ACCEPT is the joiner's first feedback packet and carries the same
    // frozen window snapshot as every same-window resequenced ACK. An empty
    // boundary snapshot produces no feedback; the joiner keeps its startup
    // rate until the first nonempty snapshot returns.
    std::vector<RnicCollectiveGrant> accepts;
    if (snapshot.n_hat_ppm != 0) {
        accepts.reserve(update->accepted_flow_ids.size());
        const TimePs governed_boundary = governedBoundaryPs(observation_time_ps);
        for (const AtlahsFlowId flow_id : update->accepted_flow_ids) {
            accepts.push_back(
                receiver.controller.acceptFor(flow_id, snapshot, governed_boundary));
        }
    }

    // A retirement is observed only after this sender has drained all DATA
    // and recovery state. Removing it before advertising a higher rate is
    // therefore safe, and does not require a decrease-first holdoff.
    for (const AtlahsFlowId flow_id : delta.retired_flow_ids) {
        FlowState& flow = requireFlow(flow_id);
        if (flow.receiver_retired) {
            throw std::logic_error("rnic-cn receiver committed retirement twice");
        }
        flow.sender_gate.receiverRetirementCommitted();
        receiver.reservation_ledger.retire(flow_id);
        NodeState& sender_node = requireNode(flow.request.source);
        RnicTxPort& tx = sender_node.node.txPort();
        tx.setDataEligible(flow_id, false);
        tx.setWireRateGrant(flow_id, 0);
        tx.removeRetiredFlow(flow_id);
        const auto by_destination =
            sender_node.egress_flows_by_destination.find(flow.request.destination);
        if (by_destination != sender_node.egress_flows_by_destination.end()) {
            by_destination->second.erase(flow_id);
            if (by_destination->second.empty()) {
                sender_node.egress_flows_by_destination.erase(by_destination);
            }
        }
        if (sender_node.egress_flows_by_destination.empty()) {
            sender_node.next_egress_rebalance_ps.reset();
        }
        flow.receiver_retired = true;
        flow.retirement_completion_time_ps = observation_time_ps;
        auto activation = pending_rate_activations.begin();
        while (activation != pending_rate_activations.end()) {
            if (activation->second == flow_id) {
                activation = pending_rate_activations.erase(activation);
            } else {
                ++activation;
            }
        }
    }

    if (!accepts.empty()) {
        queueAcceptFrames(accepts, receiver_node, observation_time_ps);
    }
}

void RnicCollectiveNetworkRuntime::Impl::queueAcceptFrames(
    const std::vector<RnicCollectiveGrant>& accepts,
    std::uint32_t receiver_node,
    TimePs receiver_observation_time_ps) {
    NodeState& source = requireNode(receiver_node);
    for (const RnicCollectiveGrant& grant : accepts) {
        const FlowState& flow = requireFlow(grant.flow_id);
        if (flow.request.destination != receiver_node) {
            throw std::logic_error(
                "rnic-cn admission spans multiple receivers");
        }
        enqueueControl(source, {RnicCollectivePacketKind::ACCEPT, grant.flow_id, receiver_node,
                                flow.request.source,
                                config.control_wire_bytes, std::nullopt, grant, std::nullopt,
                                receiver_observation_time_ps, false, false});
    }
}

const RnicCollectiveRateSnapshot& RnicCollectiveNetworkRuntime::Impl::frozenSnapshotFor(
    NodeState& receiver,
    TimePs receiver_time_ps) {
    const std::uint64_t window_index = receiver_time_ps / config.control_deadline_ps;
    if (!receiver.frozen_snapshot.has_value() ||
        receiver.frozen_snapshot_window < window_index) {
        receiver.frozen_snapshot = receiver.controller.rateSnapshot();
        receiver.frozen_snapshot_window = window_index;
    }
    return *receiver.frozen_snapshot;
}

RnicCollectiveNetworkRuntime::Impl::TimePs
RnicCollectiveNetworkRuntime::Impl::governedBoundaryPs(TimePs receiver_time_ps) const {
    // A snapshot frozen at window k governs receiver arrivals from boundary
    // (k + 2) * dwnd, i.e. the sender-side effect lands 1.5 RTT after the
    // launch the snapshot observed.
    const TimePs window_start =
        receiver_time_ps - receiver_time_ps % config.control_deadline_ps;
    const TimePs one_window = checkedAdd(window_start, config.control_deadline_ps,
                                         "rnic-cn governed dwnd boundary overflow");
    return checkedAdd(one_window, config.control_deadline_ps,
                      "rnic-cn governed dwnd boundary overflow");
}

void RnicCollectiveNetworkRuntime::Impl::activateDeclaredFlows() {
    if (declared_flows_pending_activation.empty()) {
        return;
    }
    std::vector<AtlahsFlowId> declared;
    declared.swap(declared_flows_pending_activation);
    for (const AtlahsFlowId flow_id : declared) {
        FlowState& flow = requireFlow(flow_id);
        // Declare-and-go with no ramping: the whole same-timestamp DECLARE
        // batch registers in the reservation ledger before any gate opens,
        // so simultaneous joiners read one consistent ledger sum and start
        // at their full allocation immediately, capped at the declared
        // request (book 1.4).
        flow.sender_gate.declarationDispatched(
            requireNode(flow.request.destination).reservation_ledger.sharedWireRateBps(),
            flow.declared_nflow_ppm, config.control_deadline_ps,
            marginDeratedCapacityBps(config));
        NodeState& source = requireNode(flow.request.source);
        RnicTxPort& tx = source.node.txPort();
        tx.setWireRateGrant(flow_id, flow.sender_gate.currentWireRateBps());
        tx.setDataEligible(flow_id, true);
        if (flow.final_ledger.total_data_packets == 0 && !flow.retire_control_queued) {
            queueRetireControl(flow, EventList::now(), true);
        }
        if (!source.next_egress_rebalance_ps.has_value()) {
            // Once per RTT (2 * dwnd), aligned to the sender's window clock.
            const TimePs rtt_ps = checkedAdd(config.control_deadline_ps,
                                             config.control_deadline_ps,
                                             "rnic-cn rebalancer RTT overflow");
            const TimePs next = checkedAdd(
                (EventList::now() / rtt_ps) * rtt_ps, rtt_ps,
                "rnic-cn rebalancer boundary overflow");
            source.next_egress_rebalance_ps = next;
            wakeAt(next);
        }
    }
}

void RnicCollectiveNetworkRuntime::Impl::queueRateFeedback(
    FlowState& flow,
    TimePs receiver_feedback_time_ps) {
    NodeState& receiver = requireNode(flow.request.destination);
    if (!receiver.controller.contains(flow.request.flow_id)) {
        return;
    }
    const RnicCollectiveRateSnapshot& snapshot =
        frozenSnapshotFor(receiver, receiver_feedback_time_ps);
    if (snapshot.n_hat_ppm == 0) {
        // The boundary preceded the first admission; the sender keeps its
        // startup rate until a nonempty snapshot returns.
        return;
    }
    const RnicCollectiveGrant grant = receiver.controller.feedbackFor(
        flow.request.flow_id, snapshot,
        governedBoundaryPs(receiver_feedback_time_ps));
    enqueueControl(receiver,
                   {RnicCollectivePacketKind::GRANT_UPDATE, flow.request.flow_id,
                    flow.request.destination, flow.request.source,
                    config.control_wire_bytes, std::nullopt, grant, std::nullopt,
                    receiver_feedback_time_ps, false, false});
    ++flow.rate_feedback_acks_generated;
}

std::exception_ptr RnicCollectiveNetworkRuntime::Impl::notifyCompletions(
    const std::vector<AtlahsFlowId>& completions) {
    std::exception_ptr first_error;
    std::set<AtlahsFlowId> unique;
    for (const AtlahsFlowId flow_id : completions) {
        if (!unique.insert(flow_id).second) {
            if (!first_error) {
                first_error = std::make_exception_ptr(
                    std::logic_error("rnic-cn completion batch duplicates a flow"));
            }
            continue;
        }
        FlowState& flow = requireFlow(flow_id);
        if (!flow.delivery_completion_time_ps.has_value() || flow.completion_notified) {
            if (!first_error) {
                first_error = std::make_exception_ptr(
                    std::logic_error("rnic-cn flow completion state is inconsistent"));
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
            throw std::logic_error("rnic-cn control became dispatchable before eligibility");
        }
        if (frame.begins_control_busy_period && !frame.inherits_exact_serializer_boundary &&
            frame.eligible_time_ps == now_ps) {
            // Equality with availablePs() may hide a fractional boundary
            // before now. A control that first became eligible at this
            // published timestamp cannot serialize retroactively. Controls
            // already queued behind it keep the cumulative rational clock.
            tx.rebasePhysicalIdle(now_ps);
        }
        const RnicWireSerializationInterval interval = tx.dispatchControl(now_ps, frame.wire_bytes);
        pending_launches.emplace(
            interval.end_ps,
            SerializedFrame{frame.kind, frame.flow_id, frame.source, frame.destination,
                            frame.wire_bytes, std::nullopt, frame.declaration, frame.grant,
                            frame.retire, frame.gap_nack, false, frame.gap_resolved, false});
        source.control_queue.pop_front();
        same_time_completion = same_time_completion || interval.end_ps == now_ps;
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

        std::vector<RnicTxRetransmissionCandidate> retransmission_heads;
        retransmission_heads.reserve(source.retransmission_flow_ids.size());
        for (const AtlahsFlowId retransmission_flow_id : source.retransmission_flow_ids) {
            const FlowState& candidate_flow = requireFlow(retransmission_flow_id);
            if (candidate_flow.request.source != source.node.nodeId() ||
                candidate_flow.retransmission_queue.empty() ||
                !tx.hasRetransmissionPending(retransmission_flow_id)) {
                throw std::logic_error("rnic-cn node-local retransmission index is inconsistent");
            }
            const RnicCollectiveDataMetadata& retransmission =
                candidate_flow.retransmission_queue.front();
            if (!queuedRetransmissionIsCurrent(candidate_flow, retransmission)) {
                throw std::logic_error(
                    "rnic-cn deterministic retransmission queue lost its gap state");
            }
            retransmission_heads.push_back(
                {candidate_flow.request.flow_id, retransmission.packet_index,
                 retransmission.payload_byte_offset, retransmission.extent});
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

        const RnicTxOpportunity opportunity = tx.dispatchOpportunity(now_ps, retransmission_heads);
        same_time_completion = same_time_completion || opportunity.end_ps == now_ps;
        if (!opportunity.packet.has_value()) {
            continue;
        }
        const RnicTxPacket& transmitted = *opportunity.packet;
        FlowState& flow = requireFlow(transmitted.flow_id);
        if (flow.request.source != source.node.nodeId()) {
            throw std::logic_error("rnic-cn TX port returned inconsistent DATA ownership");
        }

        if (transmitted.kind == RnicTxPacketKind::Retransmission) {
            if (flow.retransmission_queue.empty()) {
                throw std::logic_error("rnic-cn TX selected an absent retransmission head");
            }
            RnicCollectiveDataMetadata data = flow.retransmission_queue.front();
            if (!queuedRetransmissionIsCurrent(flow, data) ||
                transmitted.packet_index != data.packet_index ||
                transmitted.payload_byte_offset != data.payload_byte_offset ||
                !extentsEqual(transmitted.extent, data.extent)) {
                throw std::logic_error("rnic-cn TX selected a conflicting retransmission head");
            }
            flow.retransmission_queue.pop_front();
            if (flow.retransmission_queue.empty()) {
                if (source.retransmission_flow_ids.erase(flow.request.flow_id) != 1) {
                    throw std::logic_error(
                        "rnic-cn retransmission head is absent from its node index");
                }
                tx.setRetransmissionPending(flow.request.flow_id, false);
            } else if (source.retransmission_flow_ids.count(flow.request.flow_id) != 1 ||
                       !tx.hasRetransmissionPending(flow.request.flow_id)) {
                throw std::logic_error(
                    "rnic-cn remaining retransmission queue lost its node index");
            }
            data.eta_ps = transmitted.eta_ps;
            auto retry = flow.tx_retry_states.find(data.packet_index);
            if (retry == flow.tx_retry_states.end() ||
                data.transmission_attempt > retry->second.highest_authorized_attempt ||
                data.transmission_attempt <= retry->second.resolved_through_attempt) {
                throw std::logic_error("rnic-cn dispatched retry escaped sender state");
            }
            retry->second.highest_dispatched_attempt =
                std::max(retry->second.highest_dispatched_attempt, data.transmission_attempt);
            // This is a bounded, physical watchdog: its epoch starts only
            // after the retry has consumed the source serializer.  It is not
            // normal-path receiver inference and it does not make control
            // packets reliable; a routed GAP_RESOLVED or newer GAP_NACK is
            // still required to cancel/supersede it before expiry.
            pending_retry_timeouts.emplace(
                checkedAdd(opportunity.end_ps, config.retransmission_rto_ps,
                           "rnic-cn retransmission RTO overflow"),
                RetryTimeout{flow.request.flow_id, data.packet_index, data.transmission_attempt});
            flow.first_retry_dispatch_ps.emplace(data.transmission_attempt, now_ps);
            if (!first_retransmission_dispatch_ps.has_value()) {
                first_retransmission_dispatch_ps = now_ps;
            }
            pending_launches.emplace(
                opportunity.end_ps,
                SerializedFrame{RnicCollectivePacketKind::DATA, flow.request.flow_id,
                                flow.request.source, flow.request.destination,
                                data.extent.wireBytes(), data, std::nullopt, std::nullopt,
                                std::nullopt, std::nullopt, true});
            ++flow.recovery.deterministic_retransmissions;
            ++recovery_statistics.deterministic_retransmissions;
            ++retransmissions_by_attempt[data.transmission_attempt];
            flow.recovery.deterministic_retransmission_wire_bytes = checkedAdd(
                flow.recovery.deterministic_retransmission_wire_bytes, data.extent.wireBytes(),
                "rnic-cn per-flow deterministic retransmission wire counter overflow");
            recovery_statistics.deterministic_retransmission_wire_bytes =
                checkedAdd(recovery_statistics.deterministic_retransmission_wire_bytes,
                           data.extent.wireBytes(),
                           "rnic-cn deterministic retransmission wire counter overflow");
            flow.recovery.maximum_retry_attempt_observed =
                std::max(flow.recovery.maximum_retry_attempt_observed, data.transmission_attempt);
            recovery_statistics.maximum_retry_attempt_observed = std::max(
                recovery_statistics.maximum_retry_attempt_observed, data.transmission_attempt);
            continue;
        }

        if (transmitted.kind != RnicTxPacketKind::FreshData ||
            transmitted.payload_byte_offset != flow.source_payload_bytes_dispatched ||
            transmitted.packet_index != flow.source_data_packets_dispatched) {
            throw std::logic_error("rnic-cn TX port returned inconsistent fresh DATA");
        }
        const std::uint64_t next_payload =
            checkedAdd(flow.source_payload_bytes_dispatched, transmitted.extent.payloadBytes(),
                       "rnic-cn source payload ledger overflow");
        const std::uint64_t next_wire =
            checkedAdd(flow.source_wire_bytes_dispatched, transmitted.extent.wireBytes(),
                       "rnic-cn source wire ledger overflow");
        const std::uint64_t next_packets = checkedAdd(flow.source_data_packets_dispatched, 1,
                                                      "rnic-cn source packet ledger overflow");
        if (next_payload > flow.final_ledger.total_payload_bytes ||
            next_wire > flow.final_ledger.total_wire_bytes ||
            next_packets > flow.final_ledger.total_data_packets) {
            throw std::logic_error("rnic-cn source DATA exceeded its final ledger");
        }
        const RnicCollectiveDataMetadata metadata{
            transmitted.packet_index, transmitted.payload_byte_offset,
            transmitted.extent,       transmitted.eta_ps,
            flow.final_ledger,         0,
        };
        pending_launches.emplace(
            opportunity.end_ps,
            SerializedFrame{RnicCollectivePacketKind::DATA, flow.request.flow_id,
                            flow.request.source, flow.request.destination,
                            transmitted.extent.wireBytes(), metadata, std::nullopt, std::nullopt,
                            std::nullopt});
        flow.source_payload_bytes_dispatched = next_payload;
        flow.source_wire_bytes_dispatched = next_wire;
        flow.source_data_packets_dispatched = next_packets;
        const std::uint64_t original_release_ps =
            ceilToTick(checkedAdd(transmitted.eta_ps, config.ring_cam.delay_window_ps,
                                  "rnic-cn original ETA plus Delta overflow"),
                       config.ring_cam.release_tick_ps);
        flow.max_original_release_ps = std::max(flow.max_original_release_ps, original_release_ps);
        flow.final_original_release_ps = original_release_ps;
    }
    return same_time_completion;
}

std::optional<RnicCollectiveNetworkRuntime::Impl::TimePs>
RnicCollectiveNetworkRuntime::Impl::nextEventTime(TimePs now_ps) const {
    std::optional<TimePs> next;
    const auto consider = [now_ps, &next](TimePs candidate) {
        if (candidate < now_ps) {
            throw std::logic_error("rnic-cn next event calculation produced past work");
        }
        if (!next.has_value() || candidate < *next) {
            next = candidate;
        }
    };

    if (!pending_launches.empty()) {
        consider(pending_launches.begin()->first);
    }
    if (!pending_gap_decisions.empty()) {
        consider(pending_gap_decisions.begin()->first);
    }
    if (!pending_tail_gap_audits.empty()) {
        consider(pending_tail_gap_audits.begin()->first);
    }
    if (!pending_retry_timeouts.empty()) {
        consider(pending_retry_timeouts.begin()->first);
    }
    if (!pending_rate_activations.empty()) {
        consider(pending_rate_activations.begin()->first);
    }
    if (!endpoint_arrivals.empty() || fatal_control_drop.has_value() || deferred_failure) {
        consider(now_ps);
    }
    for (const auto& node_state : nodes) {
        const NodeState& state = *node_state;
        const std::optional<TimePs> rx = state.node.rxPort().nextEventTimePs();
        if (rx.has_value()) {
            consider(*rx);
        }
        if (!state.membership_changes.empty()) {
            consider(std::max(now_ps, state.membership_changes.begin()->first));
        }
        if (state.next_egress_rebalance_ps.has_value()) {
            consider(*state.next_egress_rebalance_ps);
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
    if (!pending_launches.empty() || !pending_gap_decisions.empty() ||
        !pending_tail_gap_audits.empty() || !pending_retry_timeouts.empty() ||
        !pending_rate_activations.empty() ||
        !destination_data.empty() || !endpoint_arrivals.empty() ||
        !live_packet_lifecycles.empty() || fatal_control_drop.has_value() || deferred_failure ||
        event_handle.has_value()) {
        return true;
    }
    for (const auto& node_state : nodes) {
        const NodeState& state = *node_state;
        if (!state.control_queue.empty() || !state.retransmission_flow_ids.empty() ||
            !state.membership_changes.empty() ||
            state.controller.activeFlowCount() != 0 ||
            !state.reservation_ledger.empty() ||
            state.next_egress_rebalance_ps.has_value() ||
            state.node.rxPort().nextEventTimePs().has_value()) {
            return true;
        }
    }
    for (const auto& item : flows) {
        if (!item.second->receiver_retired || !item.second->rx_missing_packets.empty() ||
            !item.second->admitted_packets.empty() ||
            item.second->next_resequence_packet != item.second->final_ledger.total_data_packets ||
            !item.second->resequenced_out_of_order.empty() || !item.second->ready_packets.empty() ||
            !item.second->retransmission_queue.empty()) {
            return true;
        }
        for (const auto& retry : item.second->tx_retry_states) {
            if (retry.second.resolved_through_attempt < retry.second.highest_authorized_attempt) {
                return true;
            }
        }
    }
    return false;
}

void RnicCollectiveNetworkRuntime::Impl::validateQuiescent() const {
    if (failed) {
        throw std::logic_error("failed rnic-cn runtime cannot be declared quiescent");
    }
    if (hasPendingWork()) {
        throw std::logic_error("rnic-cn runtime still has physical work");
    }
    if (!data_drops_for_testing.empty() || !original_duplicates_for_testing.empty()) {
        throw std::logic_error("rnic-cn quiescent runtime retains an armed DATA test seam");
    }
    for (const auto& item : flows) {
        const FlowState& flow = *item.second;
        if (!flow.completion_notified || !flow.receiver_retired ||
            !flow.rx_missing_packets.empty() || !flow.admitted_packets.empty() ||
            flow.next_resequence_packet != flow.final_ledger.total_data_packets ||
            !flow.resequenced_out_of_order.empty() || !flow.ready_packets.empty() ||
            !flow.retransmission_queue.empty() ||
            flow.sender_gate.phase() != RnicSenderGrantGate::Phase::Retired ||
            requireNode(flow.request.source).node.txPort().contains(flow.request.flow_id)) {
            throw std::logic_error("rnic-cn quiescent flow retains live endpoint state");
        }
        for (const auto& retry : flow.tx_retry_states) {
            if (retry.second.resolved_through_attempt < retry.second.highest_authorized_attempt) {
                throw std::logic_error("rnic-cn quiescent flow retains an unresolved retry");
            }
        }
    }
    if (recovery_statistics.gap_nacks_dispatched != recovery_statistics.gap_nacks_received) {
        throw std::logic_error("rnic-cn quiescent recovery retains a physical GAP_NACK");
    }
    if (gap_resolved_dispatched != gap_resolved_received) {
        throw std::logic_error("rnic-cn quiescent recovery retains a physical GAP_RESOLVED");
    }
}

void RnicCollectiveNetworkRuntime::Impl::doNextEvent() {
    event_handle.reset();
    const TimePs now_ps = EventList::now();
    try {
        if (failed) {
            throw std::logic_error("failed rnic-cn runtime received another event");
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
        // It must complete before control, RX, and membership decisions at
        // this time.
        if (EventList::hasPendingSourceAt(now_ps)) {
            wakeAt(now_ps);
            return;
        }
        throwDeferredFailure();

        std::vector<AtlahsFlowId> completions;
        // Boundary-scheduled rate changes precede same-time arrivals so a
        // snapshot never activates behind newer feedback at one timestamp;
        // the RTT rebalancer then reads the boundary-fresh grants.
        processDueRateActivations(now_ps);
        processDueEgressRebalances(now_ps);
        processEndpointArrivals(now_ps, completions);
        settleReceivePorts(now_ps, completions);
        processDueTailGapAudits(now_ps);
        queueDueGapNacks(now_ps);
        processDueRetryTimeouts(now_ps);
        beginMembershipChanges(now_ps);
        const std::exception_ptr completion_error = notifyCompletions(completions);

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
        } else if (hasPendingWork() && live_packet_lifecycles.empty()) {
            throw std::logic_error("rnic-cn physical work has no future event");
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

RnicCollectiveNetworkRuntime::RnicCollectiveNetworkRuntime(EventList& event_list,
                                                           FatTreeTopology& topology,
                                                           RnicCollectiveNetworkConfig config)
    : EventSource(event_list, "rnic-collective-network-runtime"),
      _impl(std::make_unique<Impl>(*this, event_list, topology, std::move(config))) {}

RnicCollectiveNetworkRuntime::~RnicCollectiveNetworkRuntime() {
    if (_impl) {
        _impl->shutdown();
    }
}

void RnicCollectiveNetworkRuntime::setup(std::uint32_t node_count,
                                         CompletionHandler complete_flow) {
    _impl->setup(node_count, std::move(complete_flow));
}

void RnicCollectiveNetworkRuntime::send(const AtlahsFlowRequest& request) {
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

bool RnicCollectiveNetworkRuntime::contains(AtlahsFlowId flow_id) const noexcept {
    return _impl->flows.count(flow_id) != 0;
}

RnicCollectiveFlowSnapshot RnicCollectiveNetworkRuntime::flow(AtlahsFlowId flow_id) const {
    const Impl::FlowState& state = _impl->requireFlow(flow_id);
    return {state.request,
            state.final_ledger,
            state.sender_gate.phase(),
            state.sender_gate.currentWireRateBps(),
            state.sender_gate.membershipEpoch(),
            state.declared_nflow_ppm,
            state.nflow_updates_dispatched,
            state.rate_feedback_acks_generated,
            state.rate_feedback_acks_received,
            state.source_payload_bytes_dispatched,
            state.source_wire_bytes_dispatched,
            state.source_data_packets_dispatched,
            state.delivered_payload_bytes,
            state.delivered_wire_bytes,
            state.delivered_data_packets,
            state.recovery.late_data_packets,
            state.recovery.gap_nacks_dispatched,
            state.recovery.gap_nacks_received,
            state.recovery.deterministic_retransmissions,
            state.recovery.deterministic_retransmission_wire_bytes,
            state.recovery.duplicate_gap_nacks_ignored,
            state.recovery.duplicate_data_packets_ignored,
            state.recovery.maximum_retry_attempt_observed,
            state.rx_missing_packets.size(),
            state.ready_packets.size(),
            state.declaration_dispatched,
            state.retire_dispatched,
            state.retire_received,
            state.retirement_queued,
            state.receiver_retired,
            state.completion_notified,
            state.delivery_completion_time_ps,
            state.retirement_completion_time_ps};
}

std::size_t RnicCollectiveNetworkRuntime::receiverActiveFlowCount(std::uint32_t node_id) const {
    return _impl->requireNode(node_id).controller.activeFlowCount();
}

std::size_t RnicCollectiveNetworkRuntime::pendingFabricPacketCount() const noexcept {
    return _impl->live_packet_lifecycles.size();
}

std::size_t RnicCollectiveNetworkRuntime::pendingDestinationDataCount() const noexcept {
    return _impl->destination_data.size();
}

const RnicCollectiveRecoveryStatistics& RnicCollectiveNetworkRuntime::recoveryStatistics()
    const noexcept {
    return _impl->recovery_statistics;
}

bool RnicCollectiveNetworkRuntime::hasPendingPhysicalWork() const noexcept {
    return _impl->hasPendingWork();
}

const RnicNode& RnicCollectiveNetworkRuntime::node(std::uint32_t node_id) const {
    return _impl->requireNode(node_id).node;
}

void RnicCollectiveNetworkRuntime::validateQuiescent() const {
    _impl->validateQuiescent();
}

void RnicCollectiveNetworkRuntime::doNextEvent() {
    _impl->doNextEvent();
}

void RnicCollectiveNetworkRuntime::duplicateCurrentGapNackForTesting(AtlahsFlowId flow_id) {
    _impl->duplicateCurrentGapNackForTesting(flow_id);
}

void RnicCollectiveNetworkRuntime::duplicateCurrentRetransmissionForTesting(AtlahsFlowId flow_id) {
    _impl->duplicateCurrentRetransmissionForTesting(flow_id);
}

void RnicCollectiveNetworkRuntime::dropOriginalDataForTesting(AtlahsFlowId flow_id,
                                                              std::uint64_t packet_index) {
    _impl->dropOriginalDataForTesting(flow_id, packet_index);
}

void RnicCollectiveNetworkRuntime::dropDataAttemptForTesting(AtlahsFlowId flow_id,
                                                             std::uint64_t packet_index,
                                                             std::uint32_t transmission_attempt) {
    _impl->dropDataAttemptForTesting(flow_id, packet_index, transmission_attempt);
}

void RnicCollectiveNetworkRuntime::duplicateOriginalDataForTesting(AtlahsFlowId flow_id,
                                                                   std::uint64_t packet_index) {
    _impl->duplicateOriginalDataForTesting(flow_id, packet_index);
}

void RnicCollectiveNetworkRuntime::replayResolvedGapNackForTesting(AtlahsFlowId flow_id,
                                                                   std::uint64_t packet_index) {
    _impl->replayResolvedGapNackForTesting(flow_id, packet_index);
}

std::uint64_t RnicCollectiveNetworkRuntime::maxOriginalReleaseForTesting(
    AtlahsFlowId flow_id) const {
    return _impl->requireFlow(flow_id).max_original_release_ps;
}

std::uint64_t RnicCollectiveNetworkRuntime::finalOriginalReleaseForTesting(
    AtlahsFlowId flow_id) const {
    return _impl->requireFlow(flow_id).final_original_release_ps;
}

std::optional<std::uint64_t> RnicCollectiveNetworkRuntime::publishedRetireDeadlineForTesting(
    AtlahsFlowId flow_id) const {
    return _impl->requireFlow(flow_id).published_retire_gap_detection_ps;
}

std::optional<std::uint64_t> RnicCollectiveNetworkRuntime::firstGapObservationForTesting(
    AtlahsFlowId flow_id) const {
    return _impl->requireFlow(flow_id).first_gap_observation_ps;
}

std::optional<std::uint64_t> RnicCollectiveNetworkRuntime::firstGapDecisionForTesting(
    AtlahsFlowId flow_id) const {
    return _impl->requireFlow(flow_id).first_gap_decision_ps;
}

std::optional<std::uint64_t> RnicCollectiveNetworkRuntime::retryDispatchForTesting(
    AtlahsFlowId flow_id,
    std::uint32_t transmission_attempt) const {
    const Impl::FlowState& flow = _impl->requireFlow(flow_id);
    const auto dispatch = flow.first_retry_dispatch_ps.find(transmission_attempt);
    if (dispatch == flow.first_retry_dispatch_ps.end()) {
        return std::nullopt;
    }
    return dispatch->second;
}

void RnicCollectiveNetworkRuntime::redeclareFlowForTesting(AtlahsFlowId flow_id) {
    _impl->redeclareFlowForTesting(flow_id);
}
