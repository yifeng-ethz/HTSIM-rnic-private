// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ss_runtime.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "ns_rosetta_switch.h"
#include "rnic_ss_packet.h"
#include "rnic_ss_route.h"

namespace {

using TimePs = std::uint64_t;
using Wide = unsigned __int128;

std::uint64_t saturatedAdd(std::uint64_t left,
                           std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t checkedAdd(std::uint64_t left,
                         std::uint64_t right,
                         const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error(message);
    }
    return left + right;
}

}  // namespace

void RnicSsRuntimeConfig::validate() const {
    if (access_wire_capacity_bps == 0) {
        throw std::invalid_argument("rnic-ss requires a positive link rate");
    }
    if (packetization.maxWirePacketBytes()
            > std::numeric_limits<std::uint16_t>::max()
        || control_wire_bytes == 0
        || control_wire_bytes > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument(
            "rnic-ss packet extents must fit HTSIM's uint16_t wire field");
    }
    selective_repeat.validate();
    path_selection.validate();
    credit.validate();
    if (q_lo_bytes >= q_hi_bytes || q_hi_bytes == 0) {
        throw std::invalid_argument("rnic-ss requires 0 <= Q_lo < Q_hi");
    }
    if (credit_quantum_packets == 0) {
        throw std::invalid_argument(
            "rnic-ss credit quantum must contain at least one packet");
    }
    const Wide initial_credit =
        static_cast<Wide>(credit_quantum_packets)
        * packetization.maxWirePacketBytes();
    if (initial_credit > credit.maximum_credit_ahead_wire_bytes) {
        throw std::invalid_argument(
            "rnic-ss initial credit quantum exceeds credit-ahead bound");
    }
    if (state_trace_csv.has_value() && state_trace_csv->empty()) {
        throw std::invalid_argument(
            "rnic-ss state trace CSV path must be nonempty");
    }
}

struct RnicSsRuntime::Impl {
    struct PacketRecord {
        AtlahsFlowId flow_id;
        std::uint64_t payload_offset;
        RnicPacketExtent extent;
    };

    struct FlowState {
        explicit FlowState(const AtlahsFlowRequest& value)
            : request(value), packet_flow(nullptr) {
            packet_flow.set_flowid(static_cast<flowid_t>(value.flow_id));
        }

        AtlahsFlowRequest request;
        PacketFlow packet_flow;
        std::uint64_t source_payload_bytes_packetized{0};
        std::uint64_t delivered_payload_bytes{0};
        std::uint64_t delivered_data_packets{0};
        std::uint64_t new_packets_sent{0};
        std::uint64_t retransmitted_packets_sent{0};
        std::uint64_t acknowledged_packets{0};
        bool completion_notified{false};
    };

    struct PendingRetransmission {
        RnicSsRetransmissionReason reason;
    };

    struct PairState {
        PairState(RnicSsEndpointPair value,
                  const RnicSsRuntimeConfig& config)
            : pair(value),
              ledger(value, config.selective_repeat),
              scoreboard(0),
              selector(config.path_selection),
              credit(value, config.credit),
              observed_service_rate_bps(config.access_wire_capacity_bps) {}

        RnicSsEndpointPair pair;
        RnicSsSelectiveRepeatLedger ledger;
        RnicSsSackScoreboard scoreboard;
        RnicSsHystereticPathSelector selector;
        RnicSsPairCreditState credit;
        std::vector<AtlahsFlowId> flow_ids;
        std::size_t round_robin_cursor{0};
        std::map<RnicSsSequence, PacketRecord> packets;
        std::map<RnicSsSequence, PendingRetransmission> retransmissions;
        std::set<RnicSsSequence> sack_retry_guard;
        std::optional<std::uint8_t> current_path;
        std::optional<std::uint8_t> ordered_path;
        std::uint64_t telemetry_sequence{0};
        // This is sender-local observation, not an allocation oracle.  Once
        // selective backpressure begins, cumulative service credits and their
        // physical arrival times estimate the pair's delivered service rate.
        std::uint64_t observed_service_rate_bps;
        std::optional<TimePs> last_credit_feedback_ps;
        std::uint64_t last_credit_granted_total{0};
    };

    struct ControlAction {
        RnicSsWirePacketMetadata metadata;
        std::optional<std::uint8_t> preferred_path;
    };

    struct RtoDeadline {
        TimePs deadline_ps;
        RnicSsEndpointPair pair;
        RnicSsSequence sequence;
        TimePs last_sent_at_ps;
        std::uint32_t transmission_count;
    };

    struct LaterRtoDeadline {
        bool operator()(const RtoDeadline& left,
                        const RtoDeadline& right) const noexcept {
            return std::tie(left.deadline_ps,
                            left.pair.source,
                            left.pair.destination,
                            left.sequence,
                            left.transmission_count)
                > std::tie(right.deadline_ps,
                           right.pair.source,
                           right.pair.destination,
                           right.sequence,
                           right.transmission_count);
        }
    };

    struct ContributorState {
        std::uint64_t epoch;
        std::uint64_t granted_total;
        std::uint64_t pending_served_bytes{0};
        std::uint32_t pending_served_packets{0};
    };

    struct DestinationEgressState {
        bool backpressure_enabled{false};
        std::uint64_t next_epoch{0};
        std::uint64_t enable_epoch{0};
        std::map<RnicSsEndpointPair, ContributorState> contributors;
    };

    class Endpoint final : public PacketSink {
    public:
        Endpoint(Impl& owner, std::uint32_t node)
            : _owner(owner),
              _node(node),
              _name("RnicSsEndpoint" + std::to_string(node)) {}

        void receivePacket(Packet& base) override {
            auto* packet = dynamic_cast<RnicSsPacket*>(&base);
            if (packet == nullptr) {
                throw std::invalid_argument(
                    "rnic-ss endpoint received another protocol");
            }
            if (packet->metadata().wire_destination != _node) {
                throw std::logic_error(
                    "rnic-ss packet reached the wrong endpoint");
            }
            _owner.receiveAtEndpoint(*packet);
            packet->consumeAtEndpoint();
        }

        const string& nodename() override { return _name; }

    private:
        Impl& _owner;
        std::uint32_t _node;
        std::string _name;
    };

    class PacketObserver final : public RnicSsPacketLifecycleObserver {
    public:
        explicit PacketObserver(Impl& owner) : _owner(&owner) {}
        void detach() noexcept { _owner = nullptr; }
        void observe(
                const RnicSsPacketTermination& termination) noexcept override {
            if (_owner != nullptr) {
                _owner->packetTerminated(termination);
            }
        }

    private:
        Impl* _owner;
    };

    class SwitchObserver final : public NsRosettaBacklogObserver {
    public:
        SwitchObserver(Impl& owner, NsRosetta& sw)
            : _owner(&owner), _switch(&sw) {}
        void detach() noexcept { _owner = nullptr; }
        void observe(
                const NsRosettaBacklogObservation& observation)
                noexcept override {
            if (_owner == nullptr) {
                return;
            }
            try {
                _owner->observeSwitch(*_switch, observation);
            } catch (...) {
                _owner->deferFailure(std::current_exception());
            }
        }

    private:
        Impl* _owner;
        NsRosetta* _switch;
    };

    Impl(RnicSsRuntime& runtime_owner,
         EventList& events,
         FatTreeTopology& physical_topology,
         RnicSsRuntimeConfig value)
        : owner(runtime_owner),
          event_list(events),
          topology(physical_topology),
          config(std::move(value)),
          route_provider(topology),
          packet_observer(std::make_shared<PacketObserver>(*this)),
          control_flow(nullptr),
          state_trace(config.state_trace_csv.has_value()) {
        config.validate();
        validateSwitchBufferThresholds();
        control_flow.set_flowid(0);
        attachSwitchObservers(topology.switches_lp);
        attachSwitchObservers(topology.switches_up);
        if (!topology.switches_c.empty()) {
            throw std::invalid_argument(
                "rnic-ss controlled model rejects a third switch tier");
        }
    }

    void validateSwitchBufferThresholds() const {
        const mem_b leaf_buffer =
            topology.cfg().ns_rosetta_shared_buffer_capacity(TOR_TIER);
        const mem_b spine_buffer =
            topology.cfg().ns_rosetta_shared_buffer_capacity(AGG_TIER);
        if (leaf_buffer <= 0 || spine_buffer <= 0
            || config.q_hi_bytes > static_cast<std::uint64_t>(leaf_buffer)
            || config.q_hi_bytes > static_cast<std::uint64_t>(spine_buffer)) {
            throw std::invalid_argument(
                "rnic-ss Q_hi exceeds an ns-rosetta shared buffer");
        }
    }

    std::uint64_t serializationPs(std::uint64_t bytes) const {
        constexpr Wide ps_per_wire_byte_numerator =
            static_cast<Wide>(8) * UINT64_C(1000000000000);
        const Wide numerator = static_cast<Wide>(bytes)
                               * ps_per_wire_byte_numerator;
        const Wide result =
            (numerator + config.access_wire_capacity_bps - 1)
            / config.access_wire_capacity_bps;
        if (result > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "rnic-ss serialization calibration overflow");
        }
        return static_cast<std::uint64_t>(result);
    }

    std::uint64_t maximumNoLoadOneWayPs(std::uint32_t count) const {
        std::uint64_t result = 0;
        for (std::uint32_t source = 0; source < count; ++source) {
            for (std::uint32_t destination = 0;
                 destination < count; ++destination) {
                if (source == destination) {
                    continue;
                }
                result = std::max(
                    result,
                    static_cast<std::uint64_t>(
                        topology.cfg().get_two_point_diameter_latency(
                            static_cast<int>(source),
                            static_cast<int>(destination))));
            }
        }
        return result;
    }

    void validatePhysicalEnvelope(std::uint32_t count) {
        // A cross-leaf two-tier route contains the host serializer and three
        // ns-rosetta egress serializers.  The analytical bound uses a full
        // no-load forward DATA path even though Q_hi is observed before the
        // destination-link serialization; that deliberate overestimate also
        // accounts for forward in-flight DATA.
        constexpr std::uint64_t serializers_per_cross_leaf_path = 4;
        const std::uint64_t fan_in = count > 0 ? count - 1 : 0;
        statistics.maximum_bp_enable_fan_in = fan_in;
        const std::uint64_t diameter_ps = maximumNoLoadOneWayPs(count);
        const std::uint64_t data_serialization_ps = serializationPs(
            config.packetization.maxWirePacketBytes());
        const std::uint64_t control_serialization_ps =
            serializationPs(config.control_wire_bytes);

        const Wide forward_data_wide =
            static_cast<Wide>(diameter_ps)
            + static_cast<Wide>(serializers_per_cross_leaf_path)
                  * data_serialization_ps;
        // BP_ENABLE is strict-priority but non-preemptive, so it can wait
        // behind one already-started maximum DATA envelope at every reverse
        // serializer.  All contributor controls originate at the destination
        // endpoint.  The last one therefore sees F control serializations at
        // that source serializer and its own serialization at each of the
        // remaining H-1 stages: F + H - 1 control envelopes in total.
        const std::uint64_t serialized_bp_controls =
            fan_in == 0
                ? serializers_per_cross_leaf_path
                : fan_in + serializers_per_cross_leaf_path - 1;
        const Wide feedback_wide =
            static_cast<Wide>(diameter_ps)
            + static_cast<Wide>(serializers_per_cross_leaf_path)
                  * data_serialization_ps
            + static_cast<Wide>(serialized_bp_controls)
                  * control_serialization_ps;
        const Wide control_loop_wide = forward_data_wide + feedback_wide;
        if (forward_data_wide > std::numeric_limits<std::uint64_t>::max()
            || feedback_wide > std::numeric_limits<std::uint64_t>::max()
            || control_loop_wide
                   > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "rnic-ss analytical control-loop bound overflow");
        }
        statistics.physical_forward_data_observation_ps =
            static_cast<std::uint64_t>(forward_data_wide);
        statistics.physical_last_bp_enable_feedback_ps =
            static_cast<std::uint64_t>(feedback_wide);
        statistics.physical_bound_control_loop_ps =
            static_cast<std::uint64_t>(control_loop_wide);

        constexpr Wide wire_bits_per_second_denominator =
            static_cast<Wide>(8) * UINT64_C(1000000000000);
        const Wide control_loop_bytes =
            (static_cast<Wide>(config.access_wire_capacity_bps)
                 * statistics.physical_bound_control_loop_ps
             + wire_bits_per_second_denominator - 1)
            / wire_bits_per_second_denominator;
        const Wide one_control_loop_bytes =
            control_loop_bytes + config.packetization.maxWirePacketBytes();
        const Wide raw_control_loop_packets =
            (one_control_loop_bytes
                 + config.packetization.maxWirePacketBytes() - 1)
            / config.packetization.maxWirePacketBytes();
        if (raw_control_loop_packets == 0
            || raw_control_loop_packets
                   > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "rnic-ss control-loop outstanding window overflow");
        }
        // Round the analytical loop envelope to an RTL-friendly power-of-two
        // selective-repeat scoreboard.  The canonical controlled Clos remains
        // within the 128-packet SACK window.
        std::uint64_t rounded_control_loop_packets = 1;
        const std::uint64_t raw_packets =
            static_cast<std::uint64_t>(raw_control_loop_packets);
        while (rounded_control_loop_packets < raw_packets) {
            if (rounded_control_loop_packets
                > std::numeric_limits<std::uint64_t>::max() / 2) {
                throw std::overflow_error(
                    "rnic-ss rounded control-loop window overflow");
            }
            rounded_control_loop_packets *= 2;
        }
        statistics.control_loop_window_packets =
            rounded_control_loop_packets;
        if (config.selective_repeat.window_packets
            < statistics.control_loop_window_packets) {
            throw std::invalid_argument(
                "rnic-ss outstanding window is below the rounded analytical "
                "control-loop DATA envelope");
        }
        statistics.configured_window_below_control_loop =
            config.selective_repeat.window_packets
            < statistics.control_loop_window_packets;

        const Wide maximum_data =
            config.packetization.maxWirePacketBytes();
        const Wide sum_windows =
            static_cast<Wide>(fan_in)
            * config.selective_repeat.window_packets * maximum_data;
        const Wide excess_rate =
            fan_in > 1
                ? static_cast<Wide>(fan_in - 1)
                      * config.access_wire_capacity_bps
                : 0;
        const Wide reaction_bytes =
            (excess_rate * statistics.physical_bound_control_loop_ps
             + wire_bits_per_second_denominator - 1)
            / wire_bits_per_second_denominator
            + static_cast<Wide>(fan_in) * maximum_data;
        const Wide burst = std::min(sum_windows, reaction_bytes);
        const Wide queue_bound = config.q_hi_bytes + burst;
        if (queue_bound > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "rnic-ss analytical queue envelope overflow");
        }
        statistics.analytical_queue_bound_bytes =
            static_cast<std::uint64_t>(queue_bound);
        const std::uint64_t available = static_cast<std::uint64_t>(
            std::min(
                topology.cfg().ns_rosetta_shared_buffer_capacity(TOR_TIER),
                topology.cfg().ns_rosetta_shared_buffer_capacity(AGG_TIER)));
        if (!config.allow_loss_stress
            && statistics.analytical_queue_bound_bytes > available) {
            throw std::invalid_argument(
                "rnic-ss shared buffer is below the controlled-Clos "
                "analytical network-calculus envelope");
        }
    }

    ~Impl() { shutdown(); }

    void attachSwitchObservers(const std::vector<Switch*>& switches) {
        for (Switch* base : switches) {
            auto* sw = dynamic_cast<NsRosetta*>(base);
            if (sw == nullptr) {
                throw std::invalid_argument(
                    "rnic-ss topology contains a non-ns-rosetta switch");
            }
            auto observer = std::make_shared<SwitchObserver>(*this, *sw);
            sw->set_backlog_observer(observer);
            switch_observers.emplace_back(sw, std::move(observer));
        }
    }

    void shutdown() noexcept {
        if (event_handle.has_value()) {
            EventList::cancelPendingSourceByHandle(owner, *event_handle);
            event_handle.reset();
        }
        for (auto& [sw, observer] : switch_observers) {
            sw->set_backlog_observer(nullptr);
            observer->detach();
        }
        switch_observers.clear();
        packet_observer->detach();
    }

    void setup(std::uint32_t count, CompletionHandler completion) {
        if (setup_complete) {
            throw std::logic_error("rnic-ss runtime setup is one-shot");
        }
        if (count != route_provider.nodeCount()) {
            throw std::invalid_argument(
                "rnic-ss node count differs from its physical Clos");
        }
        if (!completion) {
            throw std::invalid_argument(
                "rnic-ss runtime requires a completion callback");
        }
        validatePhysicalEnvelope(count);
        endpoints.reserve(count);
        for (std::uint32_t node = 0; node < count; ++node) {
            endpoints.push_back(std::make_unique<Endpoint>(*this, node));
        }
        node_count = count;
        complete_flow = std::move(completion);
        setup_complete = true;
    }

    PairState& ensurePair(RnicSsEndpointPair pair) {
        auto existing = pairs.find(pair);
        if (existing != pairs.end()) {
            return *existing->second;
        }
        auto state = std::make_unique<PairState>(pair, config);
        PairState& result = *state;
        pairs.emplace(pair, std::move(state));
        return result;
    }

    PairState& requirePair(RnicSsEndpointPair pair) {
        auto it = pairs.find(pair);
        if (it == pairs.end()) {
            throw std::out_of_range("rnic-ss endpoint pair is unknown");
        }
        return *it->second;
    }

    FlowState& requireFlow(AtlahsFlowId flow_id) {
        auto it = flows.find(flow_id);
        if (it == flows.end()) {
            throw std::out_of_range("rnic-ss ATLAHS flow is unknown");
        }
        return *it->second;
    }

    const FlowState& requireFlow(AtlahsFlowId flow_id) const {
        auto it = flows.find(flow_id);
        if (it == flows.end()) {
            throw std::out_of_range("rnic-ss ATLAHS flow is unknown");
        }
        return *it->second;
    }

    std::size_t activeSourceFlowCount(const PairState& pair) const {
        return static_cast<std::size_t>(std::count_if(
            pair.flow_ids.begin(), pair.flow_ids.end(),
            [this](AtlahsFlowId flow_id) {
                const FlowState& flow = requireFlow(flow_id);
                return flow.source_payload_bytes_packetized
                    < flow.request.payload_bytes;
            }));
    }

    void traceFlow(const PairState& pair,
                   const FlowState& flow,
                   const char* event) {
        if (!state_trace.enabled()) {
            return;
        }
        const bool source_active =
            flow.source_payload_bytes_packetized
            < flow.request.payload_bytes;
        const std::size_t active = activeSourceFlowCount(pair);
        const std::uint64_t effective_rate =
            source_active && active != 0
                ? pair.observed_service_rate_bps / active : 0;
        state_trace.append(
            {EventList::now(),
             flow.request.flow_id,
             flow.request.source,
             flow.request.destination,
             event,
             config.access_wire_capacity_bps,
             effective_rate,
             std::nullopt,
             std::nullopt,
             flow.new_packets_sent,
             flow.retransmitted_packets_sent,
             flow.acknowledged_packets});
    }

    void tracePair(const PairState& pair, const char* event) {
        for (AtlahsFlowId flow_id : pair.flow_ids) {
            traceFlow(pair, requireFlow(flow_id), event);
        }
    }

    bool updateObservedServiceRate(
            PairState& pair,
            std::uint64_t granted_total,
            TimePs now_ps) {
        if (!pair.last_credit_feedback_ps.has_value()) {
            pair.last_credit_feedback_ps = now_ps;
            pair.last_credit_granted_total = granted_total;
            return false;
        }
        const TimePs epoch_ps = *pair.last_credit_feedback_ps;
        if (now_ps <= epoch_ps
            || granted_total <= pair.last_credit_granted_total) {
            return false;
        }
        const std::uint64_t delta_bytes =
            granted_total - pair.last_credit_granted_total;
        const Wide numerator = static_cast<Wide>(delta_bytes)
                               * 8 * UINT64_C(1000000000000);
        // Use the cumulative service since this BP epoch rather than one
        // credit quantum.  A quantum is intentionally bursty; its point rate
        // is often line rate even when the contributor's sustained share is
        // C/N.  The cumulative estimate remains entirely sender-local while
        // exposing that physically observed share to the sparse trace.
        const Wide measured = numerator / (now_ps - epoch_ps);
        const std::uint64_t rate = measured
                > config.access_wire_capacity_bps
            ? config.access_wire_capacity_bps
            : static_cast<std::uint64_t>(measured);
        if (rate == pair.observed_service_rate_bps) {
            return false;
        }
        pair.observed_service_rate_bps = rate;
        return true;
    }

    void send(const AtlahsFlowRequest& request) {
        throwDeferredFailure();
        if (!setup_complete) {
            throw std::logic_error("rnic-ss send precedes setup");
        }
        if (request.source >= node_count || request.destination >= node_count
            || request.source == request.destination) {
            throw std::invalid_argument(
                "rnic-ss flow endpoints are invalid");
        }
        if (request.payload_bytes == 0) {
            throw std::invalid_argument(
                "rnic-ss ATLAHS flow must carry payload");
        }
        if (request.start_time_ps > EventList::now()) {
            throw std::invalid_argument(
                "rnic-ss flow arrived before its ATLAHS start time");
        }
        auto flow = std::make_unique<FlowState>(request);
        FlowState* value = flow.get();
        if (!flows.emplace(request.flow_id, std::move(flow)).second) {
            throw std::invalid_argument("duplicate rnic-ss ATLAHS flow ID");
        }
        PairState& pair = ensurePair({request.source, request.destination});
        pair.flow_ids.push_back(request.flow_id);
        for (AtlahsFlowId flow_id : pair.flow_ids) {
            FlowState& member = requireFlow(flow_id);
            traceFlow(pair, member,
                      flow_id == request.flow_id
                          ? "flow-start" : "pair-flow-join");
        }
        (void)value;
        pumpPair(pair, EventList::now());
        refreshEvent();
    }

    std::optional<std::pair<std::size_t, FlowState*>> nextSourceFlow(
            PairState& pair) {
        if (pair.flow_ids.empty()) {
            return std::nullopt;
        }
        for (std::size_t offset = 0; offset < pair.flow_ids.size(); ++offset) {
            const std::size_t index =
                (pair.round_robin_cursor + offset) % pair.flow_ids.size();
            FlowState& flow = requireFlow(pair.flow_ids[index]);
            if (flow.source_payload_bytes_packetized
                < flow.request.payload_bytes) {
                return std::make_pair(index, &flow);
            }
        }
        return std::nullopt;
    }

    bool sequenceWindowHasRoom(const PairState& pair) const noexcept {
        if (!pair.ledger.canSendNewPacket()) {
            return false;
        }
        if (pair.ledger.outstanding().empty()) {
            return true;
        }
        return pair.ledger.nextSequence()
                   - pair.ledger.outstanding().begin()->first < 128;
    }

    std::uint8_t choosePath(PairState& pair,
                            RnicSsSequence sequence,
                            const RnicSsRouteProvider::RouteView& routes,
                            TimePs now_ps,
                            bool retransmission) {
        if (routes.size() == 1) {
            pair.current_path = 0;
            pair.ordered_path = 0;
            return 0;
        }
        if (routes.size() != 8) {
            throw std::invalid_argument(
                "rnic-ss inter-leaf route must expose eight spine paths");
        }
        if (!config.unordered_packet_routing
            && pair.ordered_path.has_value()) {
            return *pair.ordered_path;
        }

        const std::uint64_t retry_salt = retransmission
            ? UINT64_C(0x9e3779b97f4a7c15) : 0;
        const std::uint64_t selection_key =
            (config.unordered_packet_routing ? sequence : 0)
            ^ config.routing_seed ^ retry_salt;
        const RnicSsPathDecision delayed = pair.selector.select(
            pair.pair, selection_key, now_ps,
            config.unordered_packet_routing ? pair.current_path
                                            : std::nullopt);

        std::array<std::uint64_t, 4> scores{};
        std::size_t best = 0;
        for (std::size_t index = 0; index < delayed.candidates.size(); ++index) {
            const std::uint8_t path = delayed.candidates[index];
            NsRosettaEgressSerializer* local_egress = nullptr;
            for (PacketSink* sink : *routes[path]) {
                local_egress =
                    dynamic_cast<NsRosettaEgressSerializer*>(sink);
                if (local_egress != nullptr) {
                    break;
                }
            }
            if (local_egress == nullptr || local_egress->owner() == nullptr) {
                throw std::logic_error(
                    "rnic-ss route lacks a source-local ns-rosetta egress");
            }
            const NsRosettaPathLoad local =
                local_egress->owner()->sample_path_load(
                    local_egress->egress_id());
            scores[index] = saturatedAdd(
                delayed.candidate_queue_delay_ps[index],
                local.backlog_delay_ps);
            if (index != 0
                && (scores[index] < scores[best]
                    || (scores[index] == scores[best]
                        && path < delayed.candidates[best]))) {
                best = index;
            }
        }

        if (config.unordered_packet_routing && pair.current_path.has_value()) {
            auto current = std::find(delayed.candidates.begin(),
                                     delayed.candidates.end(),
                                     *pair.current_path);
            if (current != delayed.candidates.end()) {
                const std::size_t index = static_cast<std::size_t>(
                    std::distance(delayed.candidates.begin(), current));
                if (scores[index]
                    <= saturatedAdd(scores[best],
                                    config.path_selection
                                        .hysteresis_queue_delay_ps)) {
                    best = index;
                }
            }
        }
        const std::uint8_t selected = delayed.candidates[best];
        pair.current_path = selected;
        if (!config.unordered_packet_routing) {
            pair.ordered_path = selected;
        }
        return selected;
    }

    packetid_t nextPacketId() {
        if (next_packet_id == std::numeric_limits<packetid_t>::max()) {
            throw std::overflow_error("rnic-ss HTSIM packet ID exhausted");
        }
        return next_packet_id++;
    }

    void injectPacket(PacketFlow& packet_flow,
                      const Route& route,
                      RnicSsWirePacketMetadata metadata,
                      AtlahsFlowId flow_id,
                      std::uint64_t payload_offset,
                      bool retransmission) {
        BaseQueue* const expected_source_serializer =
            topology.queues_ns_nlp[metadata.wire_source]
                                     [topology.cfg().HOST_POD_SWITCH(
                                         metadata.wire_source)][0];
        if (route.size() == 0 || route.at(0) != expected_source_serializer) {
            throw std::logic_error(
                "rnic-ss packet bypassed its shared physical source serializer");
        }
        const packetid_t packet_id = nextPacketId();
        if (live_packets.count(packet_id) != 0) {
            throw std::logic_error("rnic-ss live packet ID collided");
        }
        RnicSsPacket* packet = RnicSsPacket::newPacket(
            packet_flow, route, packet_id, std::move(metadata), flow_id,
            payload_offset, retransmission, packet_observer);
        live_packets.emplace(packet_id, packet);
        const bool data = packet->kind() == RnicSsPacketKind::DATA;
        if ((data && packet->priority() != Packet::PRIO_LO)
            || (!data && packet->priority() != Packet::PRIO_HI)) {
            statistics.source_priority_violations++;
            packet->free();
            throw std::logic_error(
                "rnic-ss physical source priority classification failed");
        }
        switch (packet->kind()) {
        case RnicSsPacketKind::DATA:
            statistics.source_data_serializer_packets++;
            break;
        case RnicSsPacketKind::ACK_SACK:
            statistics.source_ack_serializer_packets++;
            break;
        case RnicSsPacketKind::BP_ENABLE:
        case RnicSsPacketKind::BP_DISABLE:
            statistics.source_bp_serializer_packets++;
            break;
        case RnicSsPacketKind::CREDIT:
            statistics.source_credit_serializer_packets++;
            break;
        case RnicSsPacketKind::TELEMETRY:
            break;
        }
        packet->sendOn();
    }

    void sendNewData(PairState& pair,
                     FlowState& flow,
                     std::size_t flow_index,
                     TimePs now_ps) {
        const RnicPacketExtent extent = config.packetization.packetize(
            flow.request.payload_bytes
            - flow.source_payload_bytes_packetized);
        if (extent.wireBytes() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("rnic-ss DATA wire extent overflow");
        }
        if (!pair.credit.canSend(
                static_cast<std::uint32_t>(extent.wireBytes()))) {
            return;
        }
        const RnicSsSequence sequence = pair.ledger.nextSequence();
        const auto& routes = route_provider.routes(
            pair.pair.source, pair.pair.destination,
            *endpoints[pair.pair.destination]);
        const std::uint8_t path =
            choosePath(pair, sequence, routes, now_ps, false);
        const RnicSsSequence recorded = pair.ledger.recordNewTransmission(
            static_cast<std::uint32_t>(extent.wireBytes()), now_ps);
        if (recorded != sequence) {
            throw std::logic_error("rnic-ss sequence allocation changed");
        }
        scheduleRto(pair, sequence);
        pair.credit.consumeForData(
            static_cast<std::uint32_t>(extent.wireBytes()));
        const std::uint64_t payload_offset =
            flow.source_payload_bytes_packetized;
        if (!pair.packets.emplace(
                sequence,
                PacketRecord{flow.request.flow_id, payload_offset, extent})
                 .second) {
            throw std::logic_error("rnic-ss DATA record collided");
        }
        flow.source_payload_bytes_packetized += extent.payloadBytes();
        pair.round_robin_cursor =
            (flow_index + 1) % pair.flow_ids.size();

        RnicSsWirePacketMetadata metadata{
            RnicSsPacketKind::DATA,
            pair.pair.source,
            pair.pair.destination,
            static_cast<std::uint32_t>(extent.wireBytes()),
            RnicSsDataMetadata{
                pair.pair, sequence,
                static_cast<std::uint32_t>(extent.payloadBytes()), path}};
        injectPacket(flow.packet_flow, *routes[path], std::move(metadata),
                     flow.request.flow_id, payload_offset, false);
        flow.new_packets_sent++;
        statistics.new_data_packets++;
    }

    bool sendRetransmission(PairState& pair,
                            RnicSsSequence sequence,
                            RnicSsRetransmissionReason reason,
                            TimePs now_ps) {
        auto record = pair.packets.find(sequence);
        if (record == pair.packets.end()) {
            return true;
        }
        const PacketRecord& data = record->second;
        if (!pair.credit.canSend(
                static_cast<std::uint32_t>(data.extent.wireBytes()))) {
            return false;
        }
        FlowState& flow = requireFlow(data.flow_id);
        const auto& routes = route_provider.routes(
            pair.pair.source, pair.pair.destination,
            *endpoints[pair.pair.destination]);
        const std::uint8_t path =
            choosePath(pair, sequence, routes, now_ps, true);
        pair.ledger.recordRetransmission(sequence, now_ps, reason);
        scheduleRto(pair, sequence);
        pair.credit.consumeForData(
            static_cast<std::uint32_t>(data.extent.wireBytes()));
        RnicSsWirePacketMetadata metadata{
            RnicSsPacketKind::DATA,
            pair.pair.source,
            pair.pair.destination,
            static_cast<std::uint32_t>(data.extent.wireBytes()),
            RnicSsDataMetadata{
                pair.pair, sequence,
                static_cast<std::uint32_t>(data.extent.payloadBytes()), path}};
        injectPacket(flow.packet_flow, *routes[path], std::move(metadata),
                     data.flow_id, data.payload_offset, true);
        flow.retransmitted_packets_sent++;
        if (reason == RnicSsRetransmissionReason::SACK_HOLE) {
            statistics.sack_retransmissions++;
        } else {
            statistics.rto_retransmissions++;
        }
        return true;
    }

    void pumpPair(PairState& pair, TimePs now_ps) {
        const std::size_t active_before = activeSourceFlowCount(pair);
        while (true) {
            if (!pair.retransmissions.empty()) {
                auto retry = pair.retransmissions.begin();
                if (!sendRetransmission(pair, retry->first,
                                        retry->second.reason, now_ps)) {
                    break;
                }
                pair.retransmissions.erase(retry);
                continue;
            }
            if (!sequenceWindowHasRoom(pair)) {
                break;
            }
            const auto next = nextSourceFlow(pair);
            if (!next.has_value()) {
                break;
            }
            FlowState& flow = *next->second;
            const RnicPacketExtent extent = config.packetization.packetize(
                flow.request.payload_bytes
                - flow.source_payload_bytes_packetized);
            if (!pair.credit.canSend(
                    static_cast<std::uint32_t>(extent.wireBytes()))) {
                break;
            }
            sendNewData(pair, flow, next->first, now_ps);
        }
        if (activeSourceFlowCount(pair) != active_before) {
            tracePair(pair, "source-active-set-change");
        }
    }

    void receiveData(RnicSsPacket& packet) {
        const auto* data = std::get_if<RnicSsDataMetadata>(
            &packet.metadata().payload);
        if (data == nullptr) {
            throw std::logic_error("rnic-ss DATA metadata is malformed");
        }
        PairState& pair = requirePair(data->pair);
        FlowState& flow = requireFlow(packet.atlahsFlowId());
        if (flow.request.source != data->pair.source
            || flow.request.destination != data->pair.destination
            || packet.payloadByteOffset() > flow.request.payload_bytes
            || data->payload_bytes
                   > flow.request.payload_bytes - packet.payloadByteOffset()) {
            throw std::logic_error("rnic-ss DATA flow ledger is inconsistent");
        }

        const RnicSsReceiveResult received =
            pair.scoreboard.observe(data->sequence);
        if (received.disposition == RnicSsReceiveDisposition::NEW_IN_ORDER
            || received.disposition
                   == RnicSsReceiveDisposition::NEW_OUT_OF_ORDER) {
            flow.delivered_payload_bytes = checkedAdd(
                flow.delivered_payload_bytes, data->payload_bytes,
                "rnic-ss delivered payload overflow");
            flow.delivered_data_packets++;
            if (flow.delivered_payload_bytes > flow.request.payload_bytes) {
                throw std::logic_error(
                    "rnic-ss receiver exceeded the flow payload ledger");
            }
        }

        RnicSsAckSackMetadata ack = pair.scoreboard.snapshot(pair.pair);
        if (pair.telemetry_sequence
            == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("rnic-ss telemetry sequence exhausted");
        }
        ack.returned_load_sample = RnicSsDelayedLoadSample{
            data->path_id,
            packet.accumulatedQueueDelayPs(),
            EventList::now(),
            ++pair.telemetry_sequence};
        queueControl(
            checkedAdd(EventList::now(), config.extra_telemetry_delay_ps,
                       "rnic-ss telemetry delay overflow"),
            RnicSsWirePacketMetadata{
                RnicSsPacketKind::ACK_SACK,
                pair.pair.destination,
                pair.pair.source,
                static_cast<std::uint32_t>(config.control_wire_bytes),
                std::move(ack)},
            data->path_id);

        if (!flow.completion_notified
            && flow.delivered_payload_bytes == flow.request.payload_bytes) {
            flow.completion_notified = true;
            traceFlow(pair, flow, "completion");
            complete_flow(flow.request.flow_id);
        }
    }

    void receiveAck(const RnicSsAckSackMetadata& ack) {
        PairState& pair = requirePair(ack.forward_pair);
        if (ack.returned_load_sample.has_value()) {
            pair.selector.ingestLoadSample(
                *ack.returned_load_sample, EventList::now());
        }
        const RnicSsAckResult result = pair.ledger.applyAck(ack);
        for (const RnicSsOutstandingPacket& acknowledged :
             result.newly_acked) {
            const auto packet = pair.packets.find(acknowledged.sequence);
            if (packet == pair.packets.end()) {
                throw std::logic_error(
                    "rnic-ss ACK retired an unknown DATA record");
            }
            requireFlow(packet->second.flow_id).acknowledged_packets++;
            pair.packets.erase(acknowledged.sequence);
            pair.retransmissions.erase(acknowledged.sequence);
            pair.sack_retry_guard.erase(acknowledged.sequence);
        }
        for (const RnicSsOutstandingPacket& hole : result.reported_holes) {
            if (pair.sack_retry_guard.insert(hole.sequence).second) {
                pair.retransmissions.emplace(
                    hole.sequence,
                    PendingRetransmission{
                        RnicSsRetransmissionReason::SACK_HOLE});
            }
        }
        pumpPair(pair, EventList::now());
        refreshEvent();
    }

    void receiveControl(RnicSsPacket& packet) {
        switch (packet.kind()) {
        case RnicSsPacketKind::ACK_SACK: {
            const auto* ack = std::get_if<RnicSsAckSackMetadata>(
                &packet.metadata().payload);
            if (ack == nullptr) {
                throw std::logic_error("rnic-ss ACK payload is malformed");
            }
            receiveAck(*ack);
            return;
        }
        case RnicSsPacketKind::TELEMETRY: {
            const auto* telemetry = std::get_if<RnicSsTelemetryMetadata>(
                &packet.metadata().payload);
            if (telemetry == nullptr) {
                throw std::logic_error(
                    "rnic-ss telemetry payload is malformed");
            }
            const RnicSsEndpointPair forward{
                packet.metadata().wire_destination,
                packet.metadata().wire_source};
            requirePair(forward).selector.ingestLoadSample(
                telemetry->sample, EventList::now());
            return;
        }
        case RnicSsPacketKind::BP_ENABLE:
        case RnicSsPacketKind::BP_DISABLE: {
            const auto* bp = std::get_if<RnicSsBackpressureMetadata>(
                &packet.metadata().payload);
            if (bp == nullptr) {
                throw std::logic_error("rnic-ss BP payload is malformed");
            }
            PairState& pair = requirePair(bp->forward_pair);
            RnicSsControlApplyResult applied;
            if (packet.kind() == RnicSsPacketKind::BP_ENABLE) {
                applied = pair.credit.applyEnable(*bp);
                if (applied == RnicSsControlApplyResult::APPLIED) {
                    pair.last_credit_feedback_ps = EventList::now();
                    pair.last_credit_granted_total =
                        bp->granted_wire_bytes_total;
                    // The sender has entered a finite-credit epoch but has no
                    // service-rate observation yet.  Report zero until the
                    // first physical cumulative CREDIT establishes one.
                    pair.observed_service_rate_bps = 0;
                }
            } else {
                applied = pair.credit.applyDisable(*bp);
                if (applied == RnicSsControlApplyResult::APPLIED) {
                    pair.last_credit_feedback_ps.reset();
                    pair.last_credit_granted_total = 0;
                    pair.observed_service_rate_bps =
                        config.access_wire_capacity_bps;
                }
            }
            pumpPair(pair, EventList::now());
            if (applied == RnicSsControlApplyResult::APPLIED) {
                tracePair(
                    pair,
                    packet.kind() == RnicSsPacketKind::BP_ENABLE
                        ? "bp-enable" : "bp-disable");
            }
            refreshEvent();
            return;
        }
        case RnicSsPacketKind::CREDIT: {
            const auto* credit = std::get_if<RnicSsCreditMetadata>(
                &packet.metadata().payload);
            if (credit == nullptr) {
                throw std::logic_error(
                    "rnic-ss credit payload is malformed");
            }
            PairState& pair = requirePair(credit->forward_pair);
            const RnicSsControlApplyResult applied =
                pair.credit.applyCredit(*credit);
            const bool rate_changed =
                applied == RnicSsControlApplyResult::APPLIED
                && updateObservedServiceRate(
                    pair, credit->granted_wire_bytes_total,
                    EventList::now());
            pumpPair(pair, EventList::now());
            if (rate_changed) {
                tracePair(pair, "service-rate-change");
            }
            refreshEvent();
            return;
        }
        case RnicSsPacketKind::DATA:
            throw std::logic_error("rnic-ss DATA reached control handler");
        }
        throw std::logic_error("rnic-ss received unknown packet kind");
    }

    void receiveAtEndpoint(RnicSsPacket& packet) {
        throwDeferredFailure();
        if (packet.kind() == RnicSsPacketKind::DATA) {
            receiveData(packet);
        } else {
            receiveControl(packet);
        }
    }

    void queueControl(TimePs eligible_time,
                      RnicSsWirePacketMetadata metadata,
                      std::optional<std::uint8_t> preferred_path =
                          std::nullopt) {
        pending_controls.emplace(
            eligible_time,
            ControlAction{std::move(metadata), preferred_path});
        refreshEvent();
    }

    std::uint8_t stableControlPath(
            const RnicSsWirePacketMetadata& metadata,
            std::size_t route_count) const noexcept {
        if (route_count <= 1) {
            return 0;
        }
        RnicSsEndpointPair forward{
            metadata.wire_destination, metadata.wire_source};
        const std::uint64_t key =
            (static_cast<std::uint64_t>(forward.source) << 32)
            | forward.destination;
        return static_cast<std::uint8_t>(
            (key ^ config.routing_seed) % route_count);
    }

    void dispatchDueControls(TimePs now_ps) {
        auto end = pending_controls.upper_bound(now_ps);
        while (pending_controls.begin() != end) {
            auto action = pending_controls.begin();
            ControlAction value = std::move(action->second);
            pending_controls.erase(action);
            const auto& routes = route_provider.routes(
                value.metadata.wire_source,
                value.metadata.wire_destination,
                *endpoints[value.metadata.wire_destination]);
            std::uint8_t path = 0;
            if (value.preferred_path.has_value()
                && *value.preferred_path < routes.size()) {
                path = *value.preferred_path;
            } else {
                path = stableControlPath(value.metadata, routes.size());
            }
            const RnicSsPacketKind kind = value.metadata.kind;
            injectPacket(control_flow, *routes[path],
                         std::move(value.metadata), 0, 0, false);
            switch (kind) {
            case RnicSsPacketKind::ACK_SACK:
                statistics.ack_sack_packets++;
                break;
            case RnicSsPacketKind::BP_ENABLE:
                statistics.bp_enable_packets++;
                break;
            case RnicSsPacketKind::BP_DISABLE:
                statistics.bp_disable_packets++;
                break;
            case RnicSsPacketKind::CREDIT:
                statistics.credit_packets++;
                break;
            case RnicSsPacketKind::TELEMETRY:
            case RnicSsPacketKind::DATA:
                break;
            }
            end = pending_controls.upper_bound(now_ps);
        }
    }

    void enableContributor(DestinationEgressState& state,
                           RnicSsEndpointPair pair) {
        if (state.contributors.count(pair) != 0) {
            return;
        }
        const Wide initial_wide =
            static_cast<Wide>(config.credit_quantum_packets)
            * config.packetization.maxWirePacketBytes();
        const std::uint64_t initial =
            static_cast<std::uint64_t>(initial_wide);
        state.contributors.emplace(
            pair, ContributorState{state.enable_epoch, initial});
        queueControl(
            EventList::now(),
            RnicSsWirePacketMetadata{
                RnicSsPacketKind::BP_ENABLE,
                pair.destination,
                pair.source,
                static_cast<std::uint32_t>(config.control_wire_bytes),
                RnicSsBackpressureMetadata{
                    pair, state.enable_epoch, initial}});
    }

    void beginBackpressure(NsRosetta& sw,
                           std::uint32_t egress_id,
                           DestinationEgressState& state) {
        if (state.next_epoch == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("rnic-ss BP epoch exhausted");
        }
        state.backpressure_enabled = true;
        state.enable_epoch = ++state.next_epoch;
        for (const auto& [pair, pair_state] : pairs) {
            (void)pair_state;
            if (sw.egress_pair_backlog_bytes(egress_id, pair) > 0) {
                enableContributor(state, pair);
            }
        }
    }

    void endBackpressure(DestinationEgressState& state) {
        if (!state.backpressure_enabled) {
            return;
        }
        if (state.next_epoch == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("rnic-ss BP epoch exhausted");
        }
        const std::uint64_t disable_epoch = ++state.next_epoch;
        for (const auto& [pair, contributor] : state.contributors) {
            (void)contributor;
            queueControl(
                EventList::now(),
                RnicSsWirePacketMetadata{
                    RnicSsPacketKind::BP_DISABLE,
                    pair.destination,
                    pair.source,
                    static_cast<std::uint32_t>(config.control_wire_bytes),
                    RnicSsBackpressureMetadata{
                        pair, disable_epoch, 0}});
        }
        state.contributors.clear();
        state.backpressure_enabled = false;
        state.enable_epoch = 0;
    }

    void creditServicedPacket(DestinationEgressState& state,
                              RnicSsEndpointPair pair,
                              std::uint64_t wire_bytes) {
        auto contributor = state.contributors.find(pair);
        if (contributor == state.contributors.end()) {
            return;
        }
        ContributorState& value = contributor->second;
        value.pending_served_bytes = checkedAdd(
            value.pending_served_bytes, wire_bytes,
            "rnic-ss pending credit bytes overflow");
        value.pending_served_packets++;
        if (value.pending_served_packets < config.credit_quantum_packets) {
            return;
        }
        value.granted_total = checkedAdd(
            value.granted_total, value.pending_served_bytes,
            "rnic-ss cumulative credit overflow");
        value.pending_served_bytes = 0;
        value.pending_served_packets = 0;
        queueControl(
            EventList::now(),
            RnicSsWirePacketMetadata{
                RnicSsPacketKind::CREDIT,
                pair.destination,
                pair.source,
                static_cast<std::uint32_t>(config.control_wire_bytes),
                RnicSsCreditMetadata{
                    pair, value.epoch, value.granted_total}});
    }

    void observeSwitch(
            NsRosetta& sw,
            const NsRosettaBacklogObservation& observation) {
        const auto queue_key = std::make_pair(&sw, observation.packet_id);
        if (observation.transition == NsRosettaQueueTransition::Enqueued) {
            if (!queue_entry_times.emplace(queue_key, observation.time).second) {
                throw std::logic_error(
                    "rnic-ss packet entered one switch VoQ twice");
            }
        } else if (observation.transition
                   == NsRosettaQueueTransition::Granted) {
            auto entered = queue_entry_times.find(queue_key);
            if (entered == queue_entry_times.end()
                || observation.time < entered->second) {
                throw std::logic_error(
                    "rnic-ss grant lacks a causal VoQ admission");
            }
            auto live = live_packets.find(observation.packet_id);
            if (live != live_packets.end()) {
                live->second->addQueueResidence(
                    observation.time - entered->second, observation.time);
            }
            queue_entry_times.erase(entered);
        } else if (observation.transition
                   == NsRosettaQueueTransition::Dropped) {
            queue_entry_times.erase(queue_key);
        }

        auto live = live_packets.find(observation.packet_id);
        if (live == live_packets.end()
            || live->second->kind() != RnicSsPacketKind::DATA
            || observation.switch_type != FatTreeSwitch::TOR
            || topology.cfg().HOST_POD_SWITCH(
                   observation.pair.destination) != sw.getID()) {
            return;
        }

        const auto egress_key = std::make_pair(&sw, observation.egress_id);
        DestinationEgressState& state = destination_egresses[egress_key];
        if (!state.backpressure_enabled
            && observation.egress_backlog_bytes
                   >= static_cast<mem_b>(config.q_hi_bytes)) {
            beginBackpressure(sw, observation.egress_id, state);
        }
        if (state.backpressure_enabled
            && observation.transition == NsRosettaQueueTransition::Enqueued
            && observation.pair_backlog_bytes > 0) {
            enableContributor(state, observation.pair);
        }
        if (state.backpressure_enabled
            && observation.transition
                   == NsRosettaQueueTransition::SerializationCompleted) {
            creditServicedPacket(
                state, observation.pair, observation.packet_bytes);
        }
        if (state.backpressure_enabled
            && observation.egress_backlog_bytes
                   <= static_cast<mem_b>(config.q_lo_bytes)) {
            endBackpressure(state);
        }
    }

    void packetTerminated(
            const RnicSsPacketTermination& termination) noexcept {
        auto packet = live_packets.find(termination.packet_id);
        if (packet == live_packets.end()) {
            deferFailure(std::make_exception_ptr(std::logic_error(
                "rnic-ss packet terminated outside its lifecycle table")));
            return;
        }
        live_packets.erase(packet);
        if (termination.fabric_drop) {
            statistics.fabric_drops++;
        }
    }

    void scheduleRto(PairState& pair, RnicSsSequence sequence) {
        const auto record = pair.ledger.outstanding().find(sequence);
        if (record == pair.ledger.outstanding().end()) {
            throw std::logic_error(
                "rnic-ss cannot schedule RTO for a retired sequence");
        }
        const TimePs deadline = checkedAdd(
            record->second.last_sent_at_ps,
            config.selective_repeat.retransmission_timeout_ps,
            "rnic-ss RTO time overflow");
        rto_deadlines.push(
            RtoDeadline{deadline,
                        pair.pair,
                        sequence,
                        record->second.last_sent_at_ps,
                        record->second.transmission_count});
        statistics.rto_deadline_pushes++;
        statistics.rto_deadline_heap_high_watermark = std::max(
            statistics.rto_deadline_heap_high_watermark,
            static_cast<std::uint64_t>(rto_deadlines.size()));
    }

    bool currentRtoDeadline(const RtoDeadline& deadline) const {
        const auto pair_it = pairs.find(deadline.pair);
        if (pair_it == pairs.end()) {
            return false;
        }
        const PairState& pair = *pair_it->second;
        if (pair.retransmissions.count(deadline.sequence) != 0) {
            return false;
        }
        const auto record = pair.ledger.outstanding().find(deadline.sequence);
        return record != pair.ledger.outstanding().end()
            && record->second.last_sent_at_ps == deadline.last_sent_at_ps
            && record->second.transmission_count
                   == deadline.transmission_count;
    }

    void discardStaleRtoDeadlines() {
        while (!rto_deadlines.empty()
               && !currentRtoDeadline(rto_deadlines.top())) {
            rto_deadlines.pop();
            statistics.rto_deadline_stale_pops++;
        }
    }

    void processRtos(TimePs now_ps) {
        discardStaleRtoDeadlines();
        while (!rto_deadlines.empty()
               && rto_deadlines.top().deadline_ps <= now_ps) {
            const RtoDeadline deadline = rto_deadlines.top();
            rto_deadlines.pop();
            statistics.rto_deadline_due_pops++;
            PairState& pair = requirePair(deadline.pair);
            const std::optional<RnicSsOutstandingPacket> packet =
                pair.ledger.retransmissionCandidate(
                    deadline.sequence, now_ps);
            if (packet.has_value()
                && pair.retransmissions.count(packet->sequence) == 0) {
                pair.retransmissions.emplace(
                    packet->sequence,
                    PendingRetransmission{
                        RnicSsRetransmissionReason::RTO});
            }
            pumpPair(pair, now_ps);
            discardStaleRtoDeadlines();
        }
    }

    std::optional<TimePs> nextRtoTime() {
        discardStaleRtoDeadlines();
        return rto_deadlines.empty()
            ? std::nullopt
            : std::optional<TimePs>(rto_deadlines.top().deadline_ps);
    }

    void wakeAt(TimePs when) {
        if (when < EventList::now()) {
            when = EventList::now();
        }
        if (event_handle.has_value()) {
            if ((*event_handle)->first == when) {
                return;
            }
            EventList::cancelPendingSourceByHandle(owner, *event_handle);
            event_handle.reset();
        }
        event_handle = EventList::sourceIsPendingGetHandle(owner, when);
    }

    void refreshEvent() {
        std::optional<TimePs> next;
        if (!pending_controls.empty()) {
            next = pending_controls.begin()->first;
        }
        const std::optional<TimePs> rto = nextRtoTime();
        if (rto.has_value() && (!next.has_value() || *rto < *next)) {
            next = *rto;
        }
        if (deferred_failure) {
            next = EventList::now();
        }
        if (!next.has_value()) {
            if (event_handle.has_value()) {
                EventList::cancelPendingSourceByHandle(owner, *event_handle);
                event_handle.reset();
            }
            return;
        }
        wakeAt(*next);
    }

    void deferFailure(std::exception_ptr error) noexcept {
        if (!deferred_failure) {
            deferred_failure = std::move(error);
        }
        try {
            wakeAt(EventList::now());
        } catch (...) {
        }
    }

    void throwDeferredFailure() {
        if (deferred_failure) {
            std::exception_ptr error = std::move(deferred_failure);
            deferred_failure = nullptr;
            std::rethrow_exception(error);
        }
    }

    void doNextEvent() {
        event_handle.reset();
        const TimePs now_ps = EventList::now();
        throwDeferredFailure();
        if (EventList::hasPendingSourceAt(now_ps)) {
            wakeAt(now_ps);
            return;
        }
        dispatchDueControls(now_ps);
        processRtos(now_ps);
        throwDeferredFailure();
        refreshEvent();
    }

    bool hasPendingWork() const noexcept {
        if (!live_packets.empty() || !pending_controls.empty()
            || !queue_entry_times.empty() || !rto_deadlines.empty()
            || event_handle.has_value()
            || deferred_failure) {
            return true;
        }
        for (const auto& [pair_id, state_ptr] : pairs) {
            (void)pair_id;
            const PairState& pair = *state_ptr;
            if (!pair.ledger.outstanding().empty()
                || !pair.retransmissions.empty()) {
                return true;
            }
        }
        for (const auto& [flow_id, flow] : flows) {
            (void)flow_id;
            if (!flow->completion_notified
                || flow->source_payload_bytes_packetized
                       != flow->request.payload_bytes) {
                return true;
            }
        }
        for (const auto& [key, state] : destination_egresses) {
            (void)key;
            if (state.backpressure_enabled
                || !state.contributors.empty()) {
                return true;
            }
        }
        return false;
    }

    void validateQuiescent() const {
        if (hasPendingWork()) {
            throw std::logic_error("rnic-ss runtime still has physical work");
        }
        for (const auto& [flow_id, state] : flows) {
            (void)flow_id;
            if (!state->completion_notified
                || state->delivered_payload_bytes
                       != state->request.payload_bytes
                || state->source_payload_bytes_packetized
                       != state->request.payload_bytes) {
                throw std::logic_error(
                    "rnic-ss quiescent flow has an incomplete ledger");
            }
        }
        if (!config.allow_loss_stress
            && (statistics.fabric_drops != 0
                || statistics.rto_retransmissions != 0)) {
            throw std::logic_error(
                "normal lossless rnic-ss run observed a drop or RTO");
        }
        if (statistics.source_priority_violations != 0) {
            throw std::logic_error(
                "rnic-ss source serializer priority invariant failed");
        }
    }

    RnicSsRuntime& owner;
    EventList& event_list;
    FatTreeTopology& topology;
    RnicSsRuntimeConfig config;
    RnicSsRouteProvider route_provider;
    std::shared_ptr<PacketObserver> packet_observer;
    PacketFlow control_flow;
    bool setup_complete{false};
    std::uint32_t node_count{0};
    CompletionHandler complete_flow;
    packetid_t next_packet_id{1};
    RnicSsRuntimeStatistics statistics;
    AtlahsStateTrace state_trace;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    std::map<AtlahsFlowId, std::unique_ptr<FlowState>> flows;
    std::map<RnicSsEndpointPair, std::unique_ptr<PairState>> pairs;
    std::map<packetid_t, RnicSsPacket*> live_packets;
    std::multimap<TimePs, ControlAction> pending_controls;
    std::priority_queue<RtoDeadline,
                        std::vector<RtoDeadline>,
                        LaterRtoDeadline> rto_deadlines;
    std::map<std::pair<NsRosetta*, packetid_t>, TimePs> queue_entry_times;
    std::map<std::pair<NsRosetta*, std::uint32_t>, DestinationEgressState>
        destination_egresses;
    std::vector<std::pair<NsRosetta*, std::shared_ptr<SwitchObserver>>>
        switch_observers;
    std::optional<EventList::Handle> event_handle;
    std::exception_ptr deferred_failure;
};

RnicSsRuntime::RnicSsRuntime(EventList& event_list,
                             FatTreeTopology& topology,
                             RnicSsRuntimeConfig config)
    : EventSource(event_list, "rnic-ss-runtime"),
      _impl(std::make_unique<Impl>(
          *this, event_list, topology, std::move(config))) {}

RnicSsRuntime::~RnicSsRuntime() = default;

void RnicSsRuntime::setup(
        std::uint32_t node_count, CompletionHandler complete_flow) {
    _impl->setup(node_count, std::move(complete_flow));
}

void RnicSsRuntime::send(const AtlahsFlowRequest& request) {
    _impl->send(request);
}

bool RnicSsRuntime::hasPendingPhysicalWork() const noexcept {
    return _impl->hasPendingWork();
}

bool RnicSsRuntime::isSetup() const noexcept {
    return _impl->setup_complete;
}

std::size_t RnicSsRuntime::flowCount() const noexcept {
    return _impl->flows.size();
}

RnicSsFlowSnapshot RnicSsRuntime::flow(AtlahsFlowId flow_id) const {
    const Impl::FlowState& flow = _impl->requireFlow(flow_id);
    return {flow.request,
            flow.source_payload_bytes_packetized,
            flow.delivered_payload_bytes,
            flow.delivered_data_packets,
            flow.completion_notified};
}

const RnicSsRuntimeStatistics& RnicSsRuntime::statistics() const noexcept {
    return _impl->statistics;
}

void RnicSsRuntime::validateQuiescent() const {
    _impl->validateQuiescent();
}

std::size_t RnicSsRuntime::stateTraceRowCount() const noexcept {
    return _impl->state_trace.size();
}

void RnicSsRuntime::writeStateTraceCsv() const {
    if (!_impl->config.state_trace_csv.has_value()) {
        throw std::logic_error("rnic-ss state trace was not requested");
    }
    _impl->validateQuiescent();
    _impl->state_trace.writeCsvAtomically(*_impl->config.state_trace_csv);
}

void RnicSsRuntime::doNextEvent() {
    _impl->doNextEvent();
}
