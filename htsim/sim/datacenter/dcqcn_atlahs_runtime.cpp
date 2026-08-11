// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dcqcn_atlahs_runtime.h"

#include "atlahs_goodput_trace.h"
#include "atlahs_state_trace.h"
#include "cnppacket.h"
#include "dcqcn.h"
#include "eth_pause_packet.h"
#include "eventlist.h"
#include "fat_tree_topology.h"
#include "ns_tm3_dcqcn_policy.h"
#include "ns_tm3_switch.h"
#include "pipe.h"
#include "queue.h"
#include "rocepacket.h"
#include "route.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace {

std::uint64_t splitmix64(std::uint64_t value) {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

std::uint64_t transportLivePacketCount() {
    return RocePacket::live_packet_count() + RoceAck::live_packet_count() +
           RoceNack::live_packet_count() + CNPPacket::live_packet_count() +
           EthPausePacket::live_packet_count();
}

class DcqcnHostQueue final : public BaseQueue {
public:
    using DataBoundaryObserver =
        std::function<void(Packet&, AtlahsRuntimeEventKind)>;

    DcqcnHostQueue(linkspeed_bps bitrate,
                   mem_b configured_capacity,
                   EventList& event_list,
                   std::string name,
                   DataBoundaryObserver data_boundary_observer)
        : BaseQueue(bitrate, event_list, nullptr),
          _configured_capacity(configured_capacity),
          _data_boundary_observer(std::move(data_boundary_observer)) {
        _nodename = std::move(name);
    }

    void register_sender(PacketSink& sender) { _data_senders.push_back(&sender); }

    void set_link_up(bool link_up) {
        if (_link_up == link_up) {
            throw std::logic_error(
                "DCQCN host link repeated a dynamic state");
        }
        _link_up = link_up;
        if (_link_up && _in_service == nullptr) {
            begin_service();
        }
    }

    void receivePacket(Packet& packet) override {
        if (packet.type() == ETH_PAUSE) {
            const auto& pause = static_cast<const EthPausePacket&>(packet);
            const bool paused = pause.sleepTime() > 0;
            _data_paused = paused;
            for (PacketSink* sender : _data_senders) {
                sender->receivePacket(*EthPausePacket::newpkt(paused ? 1 : 0, pause.senderID()));
            }
            packet.free();
            if (!_data_paused && _in_service == nullptr) {
                begin_service();
            }
            return;
        }

        packet.flow().logTraffic(packet, *this, TrafficLogger::PKT_ARRIVE);
        if (packet.priority() == Packet::PRIO_HI) {
            _high.push_back(&packet);
        } else if (packet.priority() == Packet::PRIO_LO) {
            _low[packet.flow_id()].push_back(&packet);
        } else {
            throw std::invalid_argument("DCQCN host queue accepts only high control or low DATA");
        }
        _buffered_bytes += packet.size();
        _high_watermark = std::max(_high_watermark, _buffered_bytes);
        if (_in_service == nullptr) {
            begin_service();
        }
    }

    void doNextEvent() override {
        if (_in_service == nullptr) {
            throw std::logic_error("idle DCQCN host queue received an event");
        }
        Packet* packet = _in_service;
        _in_service = nullptr;
        if (packet->type() == ROCE && _data_boundary_observer) {
            _data_boundary_observer(
                *packet, AtlahsRuntimeEventKind::PacketTxFinished);
        }
        if (_buffered_bytes < packet->size()) {
            throw std::logic_error("DCQCN host queue accounting underflow");
        }
        _buffered_bytes -= packet->size();
        packet->flow().logTraffic(*packet, *this, TrafficLogger::PKT_DEPART);
        log_packet_send(drainTime(packet));
        packet->sendOn();
        begin_service();
    }

    mem_b queuesize() const override { return _buffered_bytes; }
    mem_b maxsize() const override { return _configured_capacity; }
    mem_b queuesize_high_watermark() const override { return _high_watermark; }

private:
    Packet* select_low() {
        if (_data_paused || !_link_up || _low.empty()) {
            return nullptr;
        }
        auto selected = _has_last_low_flow ? _low.upper_bound(_last_low_flow) : _low.begin();
        if (selected == _low.end()) {
            selected = _low.begin();
        }
        _has_last_low_flow = true;
        _last_low_flow = selected->first;
        Packet* packet = selected->second.front();
        selected->second.pop_front();
        if (selected->second.empty()) {
            _low.erase(selected);
        }
        return packet;
    }

    void begin_service() {
        if (_in_service != nullptr) {
            return;
        }
        if (!_high.empty()) {
            _in_service = _high.front();
            _high.pop_front();
        } else {
            _in_service = select_low();
        }
        if (_in_service != nullptr) {
            if (_in_service->type() == ROCE && _data_boundary_observer) {
                _data_boundary_observer(
                    *_in_service, AtlahsRuntimeEventKind::PacketTxStarted);
            }
            eventlist().sourceIsPendingRel(*this, drainTime(_in_service));
        }
    }

    mem_b _configured_capacity;
    mem_b _buffered_bytes{0};
    mem_b _high_watermark{0};
    bool _data_paused{false};
    bool _link_up{true};
    Packet* _in_service{nullptr};
    std::deque<Packet*> _high;
    std::map<flowid_t, std::deque<Packet*>> _low;
    bool _has_last_low_flow{false};
    flowid_t _last_low_flow{0};
    std::vector<PacketSink*> _data_senders;
    DataBoundaryObserver _data_boundary_observer;
};

class DcqcnAtlahsSrc final : public DCQCNSrc {
public:
    using Completion = std::function<void()>;
    using RuntimeObserver = std::function<void(AtlahsRuntimeEvent)>;

    DcqcnAtlahsSrc(EventList& event_list,
                   linkspeed_bps rate,
                   AtlahsFlowId atlahs_flow_id,
                   std::uint32_t source,
                   std::uint32_t destination,
                   std::uint64_t policy_context_token,
                   AtlahsStateTrace* state_trace,
                   RuntimeObserver runtime_observer,
                   Completion completion)
        : DCQCNSrc(nullptr, nullptr, event_list, rate),
          _atlahs_flow_id(atlahs_flow_id),
          _source(source),
          _destination(destination),
          _policy_context_token(policy_context_token),
          _state_trace(state_trace),
          _runtime_observer(std::move(runtime_observer)),
          _completion(std::move(completion)),
          _last_progress_ps(event_list.now()) {
        setStateObserver([this](const char* event) { observeState(event); });
    }

    void processAck(const RoceAck& ack) override {
        const std::uint64_t previous_ack = _last_acked;
        DCQCNSrc::processAck(ack);
        if (_last_acked > previous_ack) {
            _last_progress_ps = eventlist().now();
            trace("ack-progress");
        }
        if (done() && !_completion_notified) {
            _completion_notified = true;
            trace("completion");
            _completion();
        }
    }

    void processNack(const RoceNack& nack) override {
        DCQCNSrc::processNack(nack);
        _last_progress_ps = eventlist().now();
        trace(nack.is_selective() ? "sr-nack" : "gbn-nack");
    }

    void processPause(const EthPausePacket& pause) override {
        RoceSrc::processPause(pause);
        emitEligibility();
        trace(pause.sleepTime() > 0 ? "pause" : "resume");
    }

    void processCNP(const CNPPacket& cnp) override {
        if (_runtime_observer) {
            AtlahsRuntimeEvent event;
            event.kind = AtlahsRuntimeEventKind::CnpReceived;
            event.flow_id = _atlahs_flow_id;
            event.event_time_ps = eventlist().now();
            if (cnp.cause_seqno() == 0) {
                throw std::logic_error(
                    "DCQCN physical CNP lost its packet correlation");
            }
            event.packet_index = cnp.cause_seqno() - 1;
            event.source = _source;
            event.destination = _destination;
            _runtime_observer(event);
        }
        DCQCNSrc::processCNP(cnp);
    }

    bool check_silent_rto(simtime_picosec now, simtime_picosec rto) {
        if (done() || !has_outstanding_data() || now < _last_progress_ps ||
            now - _last_progress_ps < rto) {
            return false;
        }
        _drops += static_cast<std::uint32_t>(_highest_sent - _last_acked);
        _highest_sent = _last_acked;
        _last_progress_ps = now;
        schedulePacingAt(now);
        trace("silent-rto");
        // RTO recovery is a loss event; it induces the CNP-style cut
        // (comparator-realism ruling).
        applyLossRateCut("rto-loss-rate-cut");
        return true;
    }

    void traceFlowStart() {
        observeState("flow-start");
        emitEligibility();
    }

private:
    void observeState(const char* event) {
        trace(event);
        if (!_runtime_observer) {
            return;
        }
        const linkspeed_bps rate = current_rate();
        if (_last_reported_rate.has_value()
            && *_last_reported_rate == rate) {
            return;
        }
        _last_reported_rate = rate;
        AtlahsRuntimeEvent observation;
        observation.kind = AtlahsRuntimeEventKind::RateUpdated;
        observation.flow_id = _atlahs_flow_id;
        observation.event_time_ps = eventlist().now();
        observation.policy_context_token = _policy_context_token;
        observation.source = _source;
        observation.destination = _destination;
        observation.effective_at_ps = eventlist().now();
        observation.has_effective_rate = true;
        observation.effective_rate_bps = rate;
        _runtime_observer(observation);
    }

    void emitEligibility() {
        if (!_runtime_observer) {
            return;
        }
        const bool eligible = _state_send != PAUSED && !done();
        if (_last_reported_eligibility.has_value()
            && *_last_reported_eligibility == eligible) {
            return;
        }
        _last_reported_eligibility = eligible;
        AtlahsRuntimeEvent observation;
        observation.kind = AtlahsRuntimeEventKind::EligibilityUpdated;
        observation.flow_id = _atlahs_flow_id;
        observation.event_time_ps = eventlist().now();
        observation.policy_context_token = _policy_context_token;
        observation.source = _source;
        observation.destination = _destination;
        observation.effective_at_ps = eventlist().now();
        observation.has_effective_rate = true;
        observation.effective_rate_bps =
            eligible ? current_rate() : UINT64_C(0);
        _runtime_observer(observation);
    }

    void trace(const char* event) {
        if (_state_trace == nullptr || !_state_trace->enabled()) {
            return;
        }
        const bool paused = _state_send == PAUSED;
        const bool effective = !paused && !done();
        _state_trace->append({eventlist().now(), _atlahs_flow_id, _source, _destination, event,
                              current_rate(), effective ? current_rate() : UINT64_C(0), alpha(),
                              paused, _new_packets_sent, _rtx_packets_sent, _acked_packets});
    }

    AtlahsFlowId _atlahs_flow_id;
    std::uint32_t _source;
    std::uint32_t _destination;
    std::uint64_t _policy_context_token;
    AtlahsStateTrace* _state_trace;
    RuntimeObserver _runtime_observer;
    Completion _completion;
    simtime_picosec _last_progress_ps;
    bool _completion_notified{false};
    std::optional<linkspeed_bps> _last_reported_rate;
    std::optional<bool> _last_reported_eligibility;
};

class DcqcnAtlahsSink final : public DCQCNSink {
public:
    using DataObserver = std::function<void(Packet&)>;

    DcqcnAtlahsSink(EventList& event_list,
                    AtlahsFlowId atlahs_flow_id,
                    std::uint32_t source,
                    std::uint32_t destination,
                    AtlahsGoodputTrace* goodput_trace,
                    DataObserver data_observer)
        : DCQCNSink(event_list),
          _atlahs_flow_id(atlahs_flow_id),
          _source(source),
          _destination(destination),
          _goodput_trace(goodput_trace),
          _data_observer(std::move(data_observer)) {}

    void receivePacket(Packet& packet) override {
        if (packet.type() != ROCE) {
            DCQCNSink::receivePacket(packet);
            return;
        }
        const auto& data = static_cast<const RocePacket&>(packet);
        const bool newly_delivered = data.seqno() == _cumulative_ack + 1;
        if (packet.size() < RocePacket::ACKSIZE) {
            throw std::logic_error("DCQCN DATA is smaller than its wire header");
        }
        const std::uint64_t payload_bytes =
            static_cast<std::uint64_t>(packet.size() - RocePacket::ACKSIZE);
        if (_data_observer) {
            _data_observer(packet);
        }
        DCQCNSink::receivePacket(packet);
        if (newly_delivered) {
            _goodput_trace->record(EventList::now(), _atlahs_flow_id, _source, _destination,
                                   payload_bytes);
        }
    }

private:
    AtlahsFlowId _atlahs_flow_id;
    std::uint32_t _source;
    std::uint32_t _destination;
    AtlahsGoodputTrace* _goodput_trace;
    DataObserver _data_observer;
};

class CallbackQueueObserver final : public NsTm3QueueObserver {
public:
    using Callback =
        std::function<void(const NsTm3QueueObservation&)>;

    explicit CallbackQueueObserver(Callback callback)
        : _callback(std::move(callback)) {}

    void observe(const NsTm3QueueObservation& observation) override {
        _callback(observation);
    }

private:
    Callback _callback;
};

class DcqcnDynamicLinkSource final : public EventSource {
public:
    using TransitionHandler =
        std::function<void(const DcqcnDynamicLinkTransition&)>;

    DcqcnDynamicLinkSource(
            EventList& event_list,
            std::vector<DcqcnDynamicLinkTransition> transitions,
            TransitionHandler handler)
        : EventSource(event_list, "DCQCN dynamic endpoint link"),
          _transitions(std::move(transitions)),
          _handler(std::move(handler)) {}

    ~DcqcnDynamicLinkSource() override {
        if (_handle.has_value()) {
            eventlist().cancelPendingSourceByHandle(*this, *_handle);
        }
    }

    void start() {
        if (_started || _transitions.empty()) {
            throw std::logic_error(
                "DCQCN dynamic link source has an invalid start");
        }
        _started = true;
        arm(_transitions.front().transition_at_ps);
    }

    void doNextEvent() override {
        _handle.reset();
        if (_next >= _transitions.size()
            || _transitions[_next].transition_at_ps != eventlist().now()) {
            throw std::logic_error(
                "DCQCN dynamic link source fired off boundary");
        }
        const simtime_picosec now = eventlist().now();
        do {
            _handler(_transitions[_next]);
            ++_next;
        } while (_next < _transitions.size()
                 && _transitions[_next].transition_at_ps == now);
        if (_next < _transitions.size()) {
            arm(_transitions[_next].transition_at_ps);
        }
    }

    bool pending() const noexcept { return _handle.has_value(); }

private:
    void arm(simtime_picosec when) {
        const EventList::Handle handle =
            eventlist().sourceIsPendingGetHandle(*this, when);
        if (handle != EventList::nullHandle()) {
            _handle = handle;
        }
    }

    std::vector<DcqcnDynamicLinkTransition> _transitions;
    TransitionHandler _handler;
    std::size_t _next{0};
    bool _started{false};
    std::optional<EventList::Handle> _handle;
};

std::uint32_t checkedWireFlowId(std::uint64_t value) {
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("DCQCN wire flow-id space exhausted");
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

class DcqcnAtlahsRuntime::Impl final : public EventSource {
public:
    struct FlowObservation {
        AtlahsFlowRequest request;
        std::uint32_t wire_flow_id;
        std::uint64_t payload_quantum_bytes;
    };

    struct PacketLookupKey {
        std::uint32_t wire_flow_id;
        packetid_t packet_id;

        bool operator<(const PacketLookupKey& other) const noexcept {
            return std::tie(wire_flow_id, packet_id)
                   < std::tie(other.wire_flow_id, other.packet_id);
        }
    };

    struct SequenceKey {
        AtlahsFlowId flow_id;
        std::uint64_t sequence;

        bool operator<(const SequenceKey& other) const noexcept {
            return std::tie(flow_id, sequence)
                   < std::tie(other.flow_id, other.sequence);
        }
    };

    struct PacketObservation {
        AtlahsFlowId flow_id;
        std::uint64_t packet_index;
        std::uint32_t transmission_attempt;
        std::uint64_t payload_offset_bytes;
        std::uint64_t payload_bytes;
        std::uint64_t wire_bytes;
        AtlahsRuntimePacketKind packet_kind;
        std::uint32_t source;
        std::uint32_t destination;
    };

    Impl(EventList& event_list, DcqcnAtlahsRuntimeConfig config, std::uint32_t physical_node_count)
        : EventSource(event_list, "DCQCN ATLAHS RTO scanner"),
          event_list(event_list),
          config(std::move(config)),
          physical_node_count(physical_node_count),
          state_trace(this->config.state_trace_csv.has_value()),
          goodput_trace(this->config.goodput_trace_bin_ps) {
        validate_config();

        Packet::set_packet_size(
            static_cast<int>(this->config.max_wire_packet_bytes - this->config.data_header_bytes));
        RoceSink::ooo_enabled = false;
        RoceSrc::setMinRTO(
            static_cast<std::uint32_t>(this->config.silent_loss_rto_ps / UINT64_C(1000000)));
        DCQCNSrc::setMinRate(this->config.dcqcn_min_rate_bps);
        DCQCNSrc::setLossRateCut(this->config.loss_rate_cut);

        std::ifstream topology_probe(this->config.topology_file);
        if (!topology_probe.is_open()) {
            throw std::invalid_argument("cannot open DCQCN topology file '" +
                                        this->config.topology_file + "'");
        }
        topology_config =
            FatTreeTopologyCfg::load(this->config.topology_file,
                                     this->config.ns_tm3_shared_buffer_bytes, COMPOSITE, FAIR_PRIO);
        if (topology_config == nullptr) {
            throw std::runtime_error("DCQCN topology loader returned no configuration");
        }
        if (topology_config->get_tiers() != 2 ||
            topology_config->no_of_nodes() != physical_node_count) {
            throw std::invalid_argument(
                "DCQCN comparator requires the matching two-tier GOAL "
                "node count");
        }
        if (topology_config->downlink_speed(TOR_TIER) != this->config.endpoint_link_bps ||
            topology_config->downlink_speed(AGG_TIER) != this->config.endpoint_link_bps ||
            topology_config->bundlesize(AGG_TIER) != 1) {
            throw std::invalid_argument(
                "DCQCN topology must use one equal-rate link per Clos edge");
        }
        topology_config->set_switch_model(FatTreeSwitchModel::NsTm3);
        topology_config->set_ns_tm3_shared_buffer_capacity(this->config.ns_tm3_shared_buffer_bytes);
        topology =
            std::make_unique<FatTreeTopology>(topology_config.get(), nullptr, &event_list, nullptr);

        if (this->config.packet_event_observations) {
            queue_observer = std::make_shared<CallbackQueueObserver>(
                [this](const NsTm3QueueObservation& observation) {
                    observe_queue(observation);
                });
        }

        const NsTm3DcqcnPolicyConfig policy_config{true,
                                                   this->config.ecn_kmin_bytes,
                                                   this->config.ecn_kmax_bytes,
                                                   this->config.ecn_pmax_ppm,
                                                   this->config.ecn_seed,
                                                   this->config.pfc_enabled,
                                                   this->config.pfc_low_threshold_bytes,
                                                   this->config.pfc_high_threshold_bytes,
                                                   this->config.endpoint_link_bps};
        configure_switches(topology->switches_lp, this->config.ns_tm3_egress_buffer_bytes,
                           policy_config);
        configure_switches(topology->switches_up, this->config.ns_tm3_egress_buffer_bytes,
                           policy_config);
        configure_switches(topology->switches_c, this->config.ns_tm3_egress_buffer_bytes,
                           policy_config);
    }

    ~Impl() override {
        if (scanner_armed) {
            EventList::cancelPendingSource(*this);
        }
    }

    void setup(std::uint32_t node_count, CompletionHandler handler) {
        if (setup_complete) {
            throw std::logic_error("DCQCN ATLAHS runtime setup twice");
        }
        if (node_count != physical_node_count || !handler) {
            throw std::invalid_argument(
                "DCQCN ATLAHS setup requires the resolved GOAL node count "
                "and a completion handler");
        }
        completion_handler = std::move(handler);
        host_queues.reserve(node_count);
        for (std::uint32_t node = 0; node < node_count; ++node) {
            DcqcnHostQueue::DataBoundaryObserver boundary_observer;
            if (config.packet_event_observations) {
                boundary_observer =
                    [this](Packet& packet, AtlahsRuntimeEventKind kind) {
                        observe_host_boundary(packet, kind);
                    };
            }
            host_queues.push_back(std::make_unique<DcqcnHostQueue>(
                config.endpoint_link_bps, config.ns_tm3_shared_buffer_bytes, event_list,
                "dcqcn-host-serializer-" + std::to_string(node),
                std::move(boundary_observer)));
        }
        if (!config.dynamic_link_transitions.empty()) {
            dynamic_link_source = std::make_unique<DcqcnDynamicLinkSource>(
                event_list,
                config.dynamic_link_transitions,
                [this](const DcqcnDynamicLinkTransition& transition) {
                    apply_dynamic_link_transition(transition);
                });
            dynamic_link_source->start();
        }
        setup_complete = true;
    }

    AtlahsRuntimeEventCapabilities event_capabilities() const noexcept {
        return AtlahsRuntimeEventCapabilities{
            config.packet_event_observations,
            config.congestion_event_observations,
            config.congestion_event_observations,
            config.pfc_enabled && config.pfc_event_observations,
            config.dynamic_link_event_observations
                && !config.dynamic_link_transitions.empty()};
    }

    void set_event_handler(EventHandler handler) {
        if (setup_complete) {
            throw std::logic_error(
                "DCQCN ATLAHS event handler must be installed before setup");
        }
        const AtlahsRuntimeEventCapabilities capabilities =
            event_capabilities();
        const bool any_capability = capabilities.packet_attempt_events
            || capabilities.ecn_cnp_events
            || capabilities.policy_update_events
            || capabilities.pfc_events
            || capabilities.dynamic_link_events;
        if (handler && !any_capability) {
            throw std::invalid_argument(
                "DCQCN ATLAHS runtime has no enabled event producer");
        }
        event_handler = std::move(handler);
    }

    void send(const AtlahsFlowRequest& request) {
        if (!setup_complete) {
            throw std::logic_error("DCQCN ATLAHS send before setup");
        }
        if (request.start_time_ps != event_list.now()) {
            throw std::invalid_argument("DCQCN ATLAHS runtime owns send timing in picoseconds");
        }
        if (request.source >= physical_node_count || request.destination >= physical_node_count ||
            request.source == request.destination) {
            throw std::out_of_range("invalid DCQCN ATLAHS endpoint pair");
        }
        if (config.congestion_event_observations
            && request.policy_context_token == 0) {
            throw std::invalid_argument(
                "DCQCN control observations require a policy-context token");
        }
        if (!known_flow_ids.insert(request.flow_id).second) {
            throw std::logic_error("duplicate DCQCN ATLAHS flow ID");
        }
        if (request.payload_bytes == 0) {
            ++completed_flows;
            completion_handler(request.flow_id);
            return;
        }

        const bool scanner_was_idle = active_flow_ids.empty();
        active_flow_ids.insert(request.flow_id);

        const std::uint32_t wire_flow_id =
            checkedWireFlowId(next_wire_flow_id++);
        DcqcnAtlahsSrc::RuntimeObserver source_observer;
        if (config.congestion_event_observations) {
            source_observer = [this](AtlahsRuntimeEvent event) {
                observe_source_event(std::move(event));
            };
        }
        DcqcnAtlahsSink::DataObserver sink_observer;
        if (config.packet_event_observations) {
            sink_observer = [this](Packet& packet) {
                observe_sink_arrival(packet);
            };
        }

        auto source = std::make_unique<DcqcnAtlahsSrc>(
            event_list, config.endpoint_link_bps, request.flow_id, request.source,
            request.destination, request.policy_context_token, &state_trace,
            std::move(source_observer),
            [this, flow_id = request.flow_id]() { complete(flow_id); });
        auto sink = std::make_unique<DcqcnAtlahsSink>(
            event_list, request.flow_id, request.source, request.destination,
            &goodput_trace, std::move(sink_observer));
        source->setName("dcqcn-" + std::to_string(request.source) + "-" +
                        std::to_string(request.destination) + "-" +
                        std::to_string(request.flow_id));
        sink->DataReceiver::setName("dcqcn-sink-" + std::to_string(request.source) + "-" +
                                    std::to_string(request.destination));
        source->set_dst(request.destination);
        sink->set_src(request.source);
        source->set_flowsize(request.payload_bytes);
        sink->configure_selective_repeat(
            config.selective_repeat ? config.sr_window_packets : 0);

        Route* forward = make_route(request.source, request.destination, request.flow_id, *sink);
        Route* reverse =
            make_route(request.destination, request.source,
                       splitmix64(request.flow_id ^ UINT64_C(0xd1b54a32d192ed03)), *source);
        source->connect(forward, reverse, *sink, TRIGGER_START);
        source->set_flowid(wire_flow_id);
        source->setPath(static_cast<std::uint32_t>(forward->path_id()));
        host_queues[request.source]->register_sender(*source);

        const auto flow_inserted = flows_by_wire_id.emplace(
            wire_flow_id,
            FlowObservation{
                request,
                wire_flow_id,
                static_cast<std::uint64_t>(
                    config.max_wire_packet_bytes
                    - config.data_header_bytes)});
        const auto reverse_inserted = wire_id_by_flow.emplace(
            request.flow_id, wire_flow_id);
        if (!flow_inserted.second || !reverse_inserted.second) {
            throw std::logic_error(
                "DCQCN event flow correlation is not unique");
        }

        sources.push_back(std::move(source));
        sinks.push_back(std::move(sink));
        sources.back()->startflow();
        sources.back()->traceFlowStart();

        if (scanner_was_idle && !scanner_armed) {
            arm_scanner();
        }
    }

    bool has_pending_physical_work() const noexcept {
        const bool transport_timer_pending =
            std::any_of(sources.begin(), sources.end(), [](const auto& source) {
                return source->pacing_event_pending() || source->cc_timer_pending();
            });
        const bool cnp_timer_pending = std::any_of(
            sinks.begin(), sinks.end(), [](const auto& sink) { return sink->cnp_timer_pending(); });
        const bool dynamic_link_pending = dynamic_link_source != nullptr
            && dynamic_link_source->pending();
        const bool pending = !active_flow_ids.empty()
            || transportLivePacketCount() != 0
            || transport_timer_pending
            || cnp_timer_pending
            || scanner_armed
            || dynamic_link_pending;
        if (pending && EventList::getPendingSources().empty()) {
            std::cerr << "[DCQCN pending] active=" << active_flow_ids.size()
                      << " data=" << RocePacket::live_packet_count()
                      << " ack=" << RoceAck::live_packet_count()
                      << " nack=" << RoceNack::live_packet_count()
                      << " cnp=" << CNPPacket::live_packet_count()
                      << " pfc=" << EthPausePacket::live_packet_count() << '\n';
        }
        return pending;
    }

    std::uint64_t sum_policy_counter(
        const std::function<std::uint64_t(const NsTm3DcqcnPolicyCounters&)>& select)
        const noexcept {
        std::uint64_t total = 0;
        const auto add = [&](const std::vector<Switch*>& switches) {
            for (Switch* base : switches) {
                const auto* ns_tm3 = dynamic_cast<const NsTm3Switch*>(base);
                if (ns_tm3 != nullptr && ns_tm3->dcqcn_policy() != nullptr) {
                    total += select(ns_tm3->dcqcn_policy()->counters());
                }
            }
        };
        add(topology->switches_lp);
        add(topology->switches_up);
        add(topology->switches_c);
        return total;
    }

    std::uint64_t sum_buffer_counter(
        const std::function<std::uint64_t(const NsTm3BufferCounters&)>& select) const noexcept {
        std::uint64_t total = 0;
        const auto add = [&](const std::vector<Switch*>& switches) {
            for (Switch* base : switches) {
                const auto* ns_tm3 = dynamic_cast<const NsTm3Switch*>(base);
                if (ns_tm3 != nullptr) {
                    total += select(ns_tm3->buffer_counters());
                }
            }
        };
        add(topology->switches_lp);
        add(topology->switches_up);
        add(topology->switches_c);
        return total;
    }

    std::uint64_t dropped_packets() const noexcept {
        return sum_buffer_counter(
            [](const NsTm3BufferCounters& counters) { return counters.dropped_packets; });
    }

    void doNextEvent() override {
        scanner_armed = false;
        const simtime_picosec now = event_list.now();
        for (const auto& source : sources) {
            if (source->check_silent_rto(now, config.silent_loss_rto_ps)) {
                ++silent_rtos;
            }
        }
        if (!active_flow_ids.empty()) {
            arm_scanner();
        }
    }

    EventList& event_list;
    DcqcnAtlahsRuntimeConfig config;
    std::uint32_t physical_node_count;
    std::unique_ptr<FatTreeTopologyCfg> topology_config;
    std::unique_ptr<FatTreeTopology> topology;
    std::vector<std::unique_ptr<DcqcnHostQueue>> host_queues;
    std::vector<std::unique_ptr<Route>> routes;
    std::vector<std::unique_ptr<DCQCNSink>> sinks;
    std::vector<std::unique_ptr<DcqcnAtlahsSrc>> sources;
    std::unique_ptr<DcqcnDynamicLinkSource> dynamic_link_source;
    std::shared_ptr<CallbackQueueObserver> queue_observer;
    std::map<std::uint32_t, FlowObservation> flows_by_wire_id;
    std::map<AtlahsFlowId, std::uint32_t> wire_id_by_flow;
    std::map<PacketLookupKey, PacketObservation> live_packets;
    std::map<SequenceKey, std::uint32_t> next_attempt_by_sequence;
    std::map<SequenceKey, std::uint32_t> marked_attempt_by_sequence;
    std::set<AtlahsFlowId> known_flow_ids;
    std::set<AtlahsFlowId> active_flow_ids;
    CompletionHandler completion_handler;
    EventHandler event_handler;
    bool setup_complete{false};
    bool scanner_armed{false};
    std::uint64_t next_wire_flow_id{1};
    std::uint64_t completed_flows{0};
    std::uint64_t silent_rtos{0};
    AtlahsStateTrace state_trace;
    AtlahsGoodputTrace goodput_trace;

private:
    void emit_event(const AtlahsRuntimeEvent& event) {
        if (!event_handler) {
            throw std::logic_error(
                "DCQCN event producer has no installed relay");
        }
        if (event.event_time_ps != event_list.now()) {
            throw std::logic_error(
                "DCQCN event producer left its physical boundary");
        }
        event_handler(event);
    }

    AtlahsRuntimeEvent packet_event(
            const PacketObservation& packet,
            AtlahsRuntimeEventKind kind) const {
        AtlahsRuntimeEvent event;
        event.kind = kind;
        event.flow_id = packet.flow_id;
        event.event_time_ps = event_list.now();
        event.packet_index = packet.packet_index;
        event.transmission_attempt = packet.transmission_attempt;
        event.payload_offset_bytes = packet.payload_offset_bytes;
        event.payload_bytes = packet.payload_bytes;
        event.wire_bytes = packet.wire_bytes;
        event.packet_kind = packet.packet_kind;
        event.source = packet.source;
        event.destination = packet.destination;
        return event;
    }

    void observe_host_boundary(
            Packet& packet, AtlahsRuntimeEventKind kind) {
        if (!config.packet_event_observations || packet.type() != ROCE) {
            return;
        }
        const auto& data = static_cast<const RocePacket&>(packet);
        const auto flow = flows_by_wire_id.find(packet.flow_id());
        if (flow == flows_by_wire_id.end()) {
            throw std::logic_error(
                "DCQCN host observation has no flow correlation");
        }
        const PacketLookupKey lookup{packet.flow_id(), packet.id()};
        if (kind == AtlahsRuntimeEventKind::PacketTxStarted) {
            if (packet.size() < config.data_header_bytes
                || data.seqno() == 0) {
                throw std::logic_error(
                    "DCQCN host observation has invalid packet geometry");
            }
            const SequenceKey sequence{
                flow->second.request.flow_id, data.seqno()};
            std::uint32_t& next_attempt =
                next_attempt_by_sequence[sequence];
            if (next_attempt == std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error(
                    "DCQCN packet-attempt index overflows");
            }
            const std::uint32_t attempt = next_attempt++;
            const std::uint64_t packet_index = data.seqno() - 1;
            if (packet_index
                > std::numeric_limits<std::uint64_t>::max()
                      / flow->second.payload_quantum_bytes) {
                throw std::overflow_error(
                    "DCQCN packet payload offset overflows");
            }
            const PacketObservation observation{
                flow->second.request.flow_id,
                packet_index,
                attempt,
                packet_index * flow->second.payload_quantum_bytes,
                static_cast<std::uint64_t>(
                    packet.size() - config.data_header_bytes),
                packet.size(),
                attempt == 0
                    ? AtlahsRuntimePacketKind::Data
                    : AtlahsRuntimePacketKind::Retransmission,
                flow->second.request.source,
                flow->second.request.destination};
            if (!live_packets.emplace(lookup, observation).second) {
                throw std::logic_error(
                    "DCQCN host reused a live wire packet identity");
            }
            emit_event(packet_event(observation, kind));
            return;
        }
        if (kind != AtlahsRuntimeEventKind::PacketTxFinished) {
            throw std::logic_error(
                "DCQCN host emitted an invalid packet boundary");
        }
        const auto observed = live_packets.find(lookup);
        if (observed == live_packets.end()) {
            throw std::logic_error(
                "DCQCN TX finish predates its physical TX start");
        }
        emit_event(packet_event(observed->second, kind));
    }

    void observe_sink_arrival(Packet& packet) {
        if (!config.packet_event_observations || packet.type() != ROCE) {
            return;
        }
        const PacketLookupKey lookup{packet.flow_id(), packet.id()};
        const auto observed = live_packets.find(lookup);
        if (observed == live_packets.end()) {
            throw std::logic_error(
                "DCQCN sink arrival has no live packet attempt");
        }
        emit_event(packet_event(
            observed->second, AtlahsRuntimeEventKind::PacketRxArrived));
        emit_event(packet_event(
            observed->second, AtlahsRuntimeEventKind::PacketDelivered));
        live_packets.erase(observed);
    }

    std::uint64_t physical_link_id(
            std::uint64_t domain_id,
            std::uint32_t ingress_id) const noexcept {
        std::uint64_t value = splitmix64(
            domain_id ^ (static_cast<std::uint64_t>(ingress_id) << 32)
            ^ UINT64_C(0x243f6a8885a308d3));
        return value == 0 ? UINT64_C(1) : value;
    }

    std::uint64_t drop_resource_id(
            const NsTm3QueueObservation& observation) const noexcept {
        std::uint64_t value = splitmix64(
            (static_cast<std::uint64_t>(observation.switch_type) << 56)
            ^ (static_cast<std::uint64_t>(observation.switch_id) << 24)
            ^ observation.egress_id
            ^ UINT64_C(0x13198a2e03707344));
        return value == 0 ? UINT64_C(1) : value;
    }

    void observe_queue(const NsTm3QueueObservation& observation) {
        if (!config.packet_event_observations
            || observation.transition != NsTm3QueueTransition::Dropped) {
            return;
        }
        const PacketLookupKey lookup{
            observation.flow_id, observation.packet_id};
        const auto packet = live_packets.find(lookup);
        if (packet == live_packets.end()) {
            return;
        }
        AtlahsRuntimeEvent event = packet_event(
            packet->second, AtlahsRuntimeEventKind::PacketDropped);
        event.drop_location = AtlahsRuntimeDropLocation::Fabric;
        event.drop_reason = AtlahsRuntimeDropReason::QueueOverflow;
        event.drop_resource_id = drop_resource_id(observation);
        event.drop_evidence = AtlahsRuntimeDropEvidence::Observed;
        emit_event(event);
        live_packets.erase(packet);
    }

    void observe_policy(
            const NsTm3DcqcnPolicyObservation& observation) {
        if (observation.kind
                == NsTm3DcqcnPolicyObservationKind::EcnMarked) {
            if (!config.congestion_event_observations) {
                return;
            }
            const PacketLookupKey lookup{
                observation.flow_id, observation.packet_id};
            const auto packet = live_packets.find(lookup);
            if (packet == live_packets.end()) {
                throw std::logic_error(
                    "DCQCN ECN mark has no live packet attempt");
            }
            AtlahsRuntimeEvent event = packet_event(
                packet->second, AtlahsRuntimeEventKind::EcnMarked);
            event.ecn_marked = true;
            marked_attempt_by_sequence[SequenceKey{
                packet->second.flow_id,
                packet->second.packet_index + 1}] =
                packet->second.transmission_attempt;
            emit_event(event);
            return;
        }
        if (!config.pfc_event_observations) {
            return;
        }
        const auto flow = flows_by_wire_id.find(observation.flow_id);
        if (flow == flows_by_wire_id.end()) {
            throw std::logic_error(
                "DCQCN PFC observation has no causal data flow");
        }
        AtlahsRuntimeEvent event;
        switch (observation.kind) {
        case NsTm3DcqcnPolicyObservationKind::PfcFrameSubmitted:
            event.kind = AtlahsRuntimeEventKind::PfcFrameSubmitted;
            break;
        case NsTm3DcqcnPolicyObservationKind::PfcPaused:
            event.kind = AtlahsRuntimeEventKind::PfcPaused;
            break;
        case NsTm3DcqcnPolicyObservationKind::PfcResumed:
            event.kind = AtlahsRuntimeEventKind::PfcResumed;
            break;
        case NsTm3DcqcnPolicyObservationKind::EcnMarked:
            throw std::logic_error(
                "DCQCN ECN observation escaped its event path");
        }
        event.event_time_ps = observation.time_ps;
        event.source = flow->second.request.source;
        event.destination = flow->second.request.destination;
        event.link_id = physical_link_id(
            observation.policy_domain_id, observation.ingress_id);
        event.priority = static_cast<std::uint8_t>(Packet::PRIO_LO);
        event.pause_quanta = observation.pause ? 1 : 0;
        emit_event(event);
    }

    void observe_source_event(AtlahsRuntimeEvent event) {
        if (!config.congestion_event_observations) {
            return;
        }
        if (event.kind == AtlahsRuntimeEventKind::CnpReceived) {
            const SequenceKey sequence{
                event.flow_id, event.packet_index + 1};
            const auto marked = marked_attempt_by_sequence.find(sequence);
            if (marked == marked_attempt_by_sequence.end()) {
                throw std::logic_error(
                    "DCQCN CNP has no marked packet attempt");
            }
            event.transmission_attempt = marked->second;
        } else if (event.kind != AtlahsRuntimeEventKind::RateUpdated
                   && event.kind
                       != AtlahsRuntimeEventKind::EligibilityUpdated) {
            throw std::logic_error(
                "DCQCN source emitted an invalid control observation");
        }
        emit_event(event);
    }

    void apply_dynamic_link_transition(
            const DcqcnDynamicLinkTransition& transition) {
        host_queues.at(transition.source)->set_link_up(transition.link_up);
        if (!config.dynamic_link_event_observations) {
            return;
        }
        AtlahsRuntimeEvent event;
        event.kind = AtlahsRuntimeEventKind::LinkStateChanged;
        event.event_time_ps = transition.transition_at_ps;
        event.source = transition.source;
        event.destination = transition.source;
        event.link_id = transition.link_id;
        event.link_state = transition.link_up
            ? AtlahsRuntimeLinkState::Up
            : AtlahsRuntimeLinkState::Down;
        event.effective_at_ps = transition.transition_at_ps;
        if (transition.link_up) {
            event.has_effective_rate = true;
            event.effective_rate_bps = config.endpoint_link_bps;
        }
        emit_event(event);
    }

    void validate_config() const {
        if (physical_node_count == 0 || config.topology_file.empty()) {
            throw std::invalid_argument("DCQCN ATLAHS requires nodes and a topology file");
        }
        if (config.endpoint_link_bps == 0 ||
            config.max_wire_packet_bytes <= config.data_header_bytes ||
            (config.packet_event_observations &&
             config.data_header_bytes != RocePacket::ACKSIZE) ||
            config.ns_tm3_shared_buffer_bytes <= 0 || config.ns_tm3_egress_buffer_bytes <= 0 ||
            config.ns_tm3_egress_buffer_bytes > config.ns_tm3_shared_buffer_bytes ||
            config.ecn_kmin_bytes < 0 || config.ecn_kmax_bytes <= config.ecn_kmin_bytes ||
            config.ecn_pmax_ppm == 0 || config.ecn_pmax_ppm > UINT32_C(1000000) ||
            config.ecn_kmax_bytes >= config.ns_tm3_egress_buffer_bytes ||
            config.silent_loss_rto_ps == 0 || config.dcqcn_min_rate_bps == 0 ||
            config.dcqcn_min_rate_bps > config.endpoint_link_bps) {
            throw std::invalid_argument("invalid DCQCN ATLAHS model config");
        }
        // PFC thresholds bind only in ECN+PFC mode; with pfc off the
        // buffer overflow drop path is the deliberate behavior.
        if (config.pfc_enabled &&
            (config.pfc_low_threshold_bytes <= 0 ||
             config.pfc_low_threshold_bytes >= config.pfc_high_threshold_bytes ||
             config.pfc_high_threshold_bytes >= config.ns_tm3_shared_buffer_bytes)) {
            throw std::invalid_argument("invalid DCQCN ATLAHS PFC thresholds");
        }
        if (config.sr_window_packets == 0 ||
            config.sr_window_packets > roceMaxReorder) {
            throw std::invalid_argument(
                "DCQCN selective-repeat window must be in [1, tracking capacity]");
        }
        if (config.goodput_trace_csv.has_value() !=
            (config.goodput_trace_bin_ps != 0)) {
            throw std::invalid_argument(
                "DCQCN goodput trace requires both path and positive bin width");
        }
        if (config.goodput_trace_csv.has_value() && config.goodput_trace_csv->empty()) {
            throw std::invalid_argument("DCQCN goodput trace CSV path must be nonempty");
        }
        if ((config.congestion_event_observations
             || config.pfc_event_observations)
            && !config.packet_event_observations) {
            throw std::invalid_argument(
                "DCQCN correlated controls require packet observations");
        }
        if (config.pfc_event_observations && !config.pfc_enabled) {
            throw std::invalid_argument(
                "DCQCN PFC observations require the physical PFC policy");
        }
        if (config.dynamic_link_event_observations
            && config.dynamic_link_transitions.empty()) {
            throw std::invalid_argument(
                "DCQCN dynamic-link observations require transitions");
        }
        std::map<std::uint32_t, bool> link_state;
        std::map<std::uint32_t, std::uint64_t> link_identity;
        simtime_picosec previous_time = 0;
        bool first_transition = true;
        for (const DcqcnDynamicLinkTransition& transition :
             config.dynamic_link_transitions) {
            if (transition.link_id == 0
                || transition.source >= physical_node_count
                || transition.transition_at_ps < event_list.now()
                || (!first_transition
                    && transition.transition_at_ps < previous_time)) {
                throw std::invalid_argument(
                    "invalid DCQCN dynamic-link transition");
            }
            const auto identity = link_identity.emplace(
                transition.source, transition.link_id);
            if (!identity.second
                && identity.first->second != transition.link_id) {
                throw std::invalid_argument(
                    "DCQCN endpoint changed dynamic-link identity");
            }
            bool& current = link_state.emplace(
                transition.source, true).first->second;
            if (current == transition.link_up) {
                throw std::invalid_argument(
                    "DCQCN dynamic link repeated its current state");
            }
            current = transition.link_up;
            previous_time = transition.transition_at_ps;
            first_transition = false;
        }
    }

    void configure_switches(const std::vector<Switch*>& switches,
                            mem_b egress_buffer_capacity,
                            const NsTm3DcqcnPolicyConfig& policy_config) {
        for (Switch* base : switches) {
            auto* ns_tm3 = dynamic_cast<NsTm3Switch*>(base);
            if (ns_tm3 == nullptr) {
                throw std::logic_error("DCQCN topology contains a non-ns-tm3 switch");
            }
            ns_tm3->set_egress_buffer_capacity(egress_buffer_capacity);
            ns_tm3->configure_dcqcn_policy(policy_config);
            if (queue_observer) {
                ns_tm3->set_queue_observer(queue_observer);
            }
            if (config.congestion_event_observations
                || config.pfc_event_observations) {
                ns_tm3->dcqcn_policy()->set_observer(
                    [this](const NsTm3DcqcnPolicyObservation& observation) {
                        observe_policy(observation);
                    });
            }
        }
    }

    Route* make_route(std::uint32_t source,
                      std::uint32_t destination,
                      std::uint64_t entropy,
                      PacketSink& endpoint) {
        const std::uint32_t source_leaf = topology_config->HOST_POD_SWITCH(source);
        const std::uint32_t destination_leaf = topology_config->HOST_POD_SWITCH(destination);
        auto route = std::make_unique<Route>();
        route->push_back(host_queues[source].get());
        route->push_back(topology->pipes_ns_nlp[source][source_leaf][0]);
        route->push_back(topology->queues_ns_nlp[source][source_leaf][0]->getRemoteEndpoint());

        int path_count = 1;
        int path_id = 0;
        if (source_leaf != destination_leaf) {
            path_count = static_cast<int>(topology_config->getNAGG());
            if (path_count <= 0) {
                throw std::logic_error("DCQCN Clos has no spine path");
            }
            path_id = static_cast<int>(splitmix64(entropy ^ config.ecmp_seed ^
                                                  (static_cast<std::uint64_t>(source) << 32) ^
                                                  destination) %
                                       static_cast<std::uint64_t>(path_count));
            const std::uint32_t spine = static_cast<std::uint32_t>(path_id);
            route->push_back(topology->queues_nlp_nup[source_leaf][spine][0]);
            route->push_back(topology->pipes_nlp_nup[source_leaf][spine][0]);
            route->push_back(topology->queues_nlp_nup[source_leaf][spine][0]->getRemoteEndpoint());
            route->push_back(topology->queues_nup_nlp[spine][destination_leaf][0]);
            route->push_back(topology->pipes_nup_nlp[spine][destination_leaf][0]);
            route->push_back(
                topology->queues_nup_nlp[spine][destination_leaf][0]->getRemoteEndpoint());
        }
        route->push_back(topology->queues_nlp_ns[destination_leaf][destination][0]);
        route->push_back(topology->pipes_nlp_ns[destination_leaf][destination][0]);
        route->push_back(&endpoint);
        route->set_path_id(path_id, path_count);

        Route* result = route.get();
        routes.push_back(std::move(route));
        return result;
    }

    void complete(AtlahsFlowId flow_id) {
        if (active_flow_ids.erase(flow_id) != 1) {
            throw std::logic_error("duplicate DCQCN flow completion");
        }
        if (active_flow_ids.empty() && scanner_armed) {
            EventList::cancelPendingSource(*this);
            scanner_armed = false;
        }
        ++completed_flows;
        completion_handler(flow_id);
    }

    void arm_scanner() {
        const simtime_picosec interval =
            std::min(config.silent_loss_rto_ps, static_cast<simtime_picosec>(UINT64_C(1000000000)));
        event_list.sourceIsPendingRel(*this, interval);
        scanner_armed = true;
    }
};

DcqcnAtlahsRuntime::DcqcnAtlahsRuntime(EventList& event_list,
                                       DcqcnAtlahsRuntimeConfig config,
                                       std::uint32_t physical_node_count)
    : _impl(std::make_unique<Impl>(event_list, std::move(config), physical_node_count)) {}

DcqcnAtlahsRuntime::~DcqcnAtlahsRuntime() = default;

void DcqcnAtlahsRuntime::setup(std::uint32_t node_count, CompletionHandler complete_flow) {
    _impl->setup(node_count, std::move(complete_flow));
}

void DcqcnAtlahsRuntime::send(const AtlahsFlowRequest& request) {
    _impl->send(request);
}

bool DcqcnAtlahsRuntime::hasPendingPhysicalWork() const noexcept {
    return _impl->has_pending_physical_work();
}

AtlahsRuntimeEventCapabilities
DcqcnAtlahsRuntime::eventCapabilities() const noexcept {
    return _impl->event_capabilities();
}

void DcqcnAtlahsRuntime::setEventHandler(EventHandler handler) {
    _impl->set_event_handler(std::move(handler));
}

FatTreeTopology& DcqcnAtlahsRuntime::topology() noexcept {
    return *_impl->topology;
}

const FatTreeTopology& DcqcnAtlahsRuntime::topology() const noexcept {
    return *_impl->topology;
}

FatTreeTopologyCfg& DcqcnAtlahsRuntime::topology_config() noexcept {
    return *_impl->topology_config;
}

const FatTreeTopologyCfg& DcqcnAtlahsRuntime::topology_config() const noexcept {
    return *_impl->topology_config;
}

const DcqcnAtlahsRuntimeConfig& DcqcnAtlahsRuntime::config() const noexcept {
    return _impl->config;
}

std::uint64_t DcqcnAtlahsRuntime::completed_flow_count() const noexcept {
    return _impl->completed_flows;
}

std::uint64_t DcqcnAtlahsRuntime::silent_rto_count() const noexcept {
    return _impl->silent_rtos;
}

std::uint64_t DcqcnAtlahsRuntime::loss_rate_cut_count() const noexcept {
    std::uint64_t total = 0;
    for (const auto& source : _impl->sources) {
        total += source->loss_rate_cut_count();
    }
    return total;
}

std::uint64_t DcqcnAtlahsRuntime::ecn_marked_packet_count() const noexcept {
    return _impl->sum_policy_counter(
        [](const NsTm3DcqcnPolicyCounters& counters) { return counters.ecn_marked_packets; });
}

std::uint64_t DcqcnAtlahsRuntime::pfc_pause_count() const noexcept {
    return _impl->sum_policy_counter(
        [](const NsTm3DcqcnPolicyCounters& counters) { return counters.pause_frames; });
}

std::uint64_t DcqcnAtlahsRuntime::pfc_resume_count() const noexcept {
    return _impl->sum_policy_counter(
        [](const NsTm3DcqcnPolicyCounters& counters) { return counters.resume_frames; });
}

std::uint64_t DcqcnAtlahsRuntime::pfc_paused_wall_ps_total() const noexcept {
    return _impl->sum_policy_counter([](const NsTm3DcqcnPolicyCounters& counters) {
        return static_cast<std::uint64_t>(counters.paused_wall_ps);
    });
}

std::uint32_t DcqcnAtlahsRuntime::pfc_max_cascade_depth() const noexcept {
    std::uint32_t depth = 0;
    const auto scan = [&](const std::vector<Switch*>& switches) {
        for (Switch* base : switches) {
            const auto* ns_tm3 = dynamic_cast<const NsTm3Switch*>(base);
            if (ns_tm3 != nullptr && ns_tm3->dcqcn_policy() != nullptr) {
                depth = std::max(depth,
                                 ns_tm3->dcqcn_policy()->counters().max_pause_cascade_depth);
            }
        }
    };
    scan(_impl->topology->switches_lp);
    scan(_impl->topology->switches_up);
    scan(_impl->topology->switches_c);
    return depth;
}

std::string DcqcnAtlahsRuntime::renderPfcPortMetricsManifest() const {
    // PFC storm observability (comparator-realism ruling): measurement
    // only, one line per switch and per port with any pause activity.
    std::ostringstream manifest;
    const auto tier_name = [](uint32_t switch_type) -> const char* {
        switch (static_cast<FatTreeSwitch::switch_type>(switch_type)) {
            case FatTreeSwitch::TOR:
                return "leaf";
            case FatTreeSwitch::AGG:
                return "spine";
            case FatTreeSwitch::CORE:
                return "core";
            case FatTreeSwitch::NONE:
                break;
        }
        return "unknown";
    };
    const auto render = [&](const std::vector<Switch*>& switches) {
        for (Switch* base : switches) {
            auto* ns_tm3 = dynamic_cast<NsTm3Switch*>(base);
            if (ns_tm3 == nullptr || ns_tm3->dcqcn_policy() == nullptr) {
                continue;
            }
            const NsTm3DcqcnPolicyCounters& counters = ns_tm3->dcqcn_policy()->counters();
            if (counters.pause_frames == 0) {
                continue;
            }
            const std::string switch_name =
                std::string(tier_name(ns_tm3->getType())) + ":" +
                std::to_string(ns_tm3->getID());
            manifest << "[DCQCN manifest] dcqcn_pfc_switch switch=" << switch_name
                     << " dcqcn_pfc_pause_frames=" << counters.pause_frames
                     << " dcqcn_pfc_resume_frames=" << counters.resume_frames
                     << " dcqcn_pfc_paused_wall_ps=" << counters.paused_wall_ps
                     << " dcqcn_pfc_max_cascade_depth=" << counters.max_pause_cascade_depth
                     << '\n';
            for (const NsTm3DcqcnPfcPortMetrics& port :
                 ns_tm3->dcqcn_policy()->pfc_port_metrics()) {
                manifest << "[DCQCN manifest] dcqcn_pfc_port switch=" << switch_name
                         << " ingress=" << port.ingress_id
                         << " dcqcn_pfc_pause_frames=" << port.pause_frames
                         << " dcqcn_pfc_resume_frames=" << port.resume_frames
                         << " dcqcn_pfc_paused_wall_ps=" << port.paused_wall_ps
                         << " dcqcn_pfc_max_cascade_depth=" << port.max_pause_cascade_depth
                         << '\n';
            }
        }
    };
    render(_impl->topology->switches_lp);
    render(_impl->topology->switches_up);
    render(_impl->topology->switches_c);
    return manifest.str();
}

std::uint64_t DcqcnAtlahsRuntime::dropped_packet_count() const noexcept {
    return _impl->dropped_packets();
}

std::uint64_t DcqcnAtlahsRuntime::shared_pool_dropped_packet_count() const noexcept {
    return _impl->sum_buffer_counter(
        [](const NsTm3BufferCounters& counters) { return counters.shared_pool_dropped_packets; });
}

std::uint64_t DcqcnAtlahsRuntime::egress_domain_dropped_packet_count() const noexcept {
    return _impl->sum_buffer_counter(
        [](const NsTm3BufferCounters& counters) { return counters.egress_domain_dropped_packets; });
}

std::size_t DcqcnAtlahsRuntime::state_trace_row_count() const noexcept {
    return _impl->state_trace.size();
}

void DcqcnAtlahsRuntime::writeStateTraceCsv() const {
    if (!_impl->config.state_trace_csv.has_value()) {
        throw std::logic_error("DCQCN state trace was not requested");
    }
    if (_impl->has_pending_physical_work()) {
        throw std::logic_error("DCQCN state trace may only be written at quiescence");
    }
    _impl->state_trace.writeCsvAtomically(*_impl->config.state_trace_csv);
}

std::size_t DcqcnAtlahsRuntime::goodput_trace_row_count() const noexcept {
    return _impl->goodput_trace.size();
}

void DcqcnAtlahsRuntime::writeGoodputTraceCsv() const {
    if (!_impl->config.goodput_trace_csv.has_value()) {
        throw std::logic_error("DCQCN goodput trace was not requested");
    }
    if (_impl->has_pending_physical_work()) {
        throw std::logic_error("DCQCN goodput trace may only be written at quiescence");
    }
    _impl->goodput_trace.writeCsvAtomically(*_impl->config.goodput_trace_csv);
}

std::string renderDcqcnAtlahsManifest(const DcqcnAtlahsRuntimeConfig& config,
                                      std::uint32_t physical_node_count,
                                      const std::string& goal_file,
                                      const std::string& completion_csv,
                                      const std::string& state_trace_csv,
                                      const char* resolved_rank_mapping) {
    std::ostringstream manifest;
    manifest << "[DCQCN manifest] schema=dcqcn-atlahs-model-v3"
             << " profile=dcqcn"
             << " goal=" << goal_file
             << " completion_csv=" << (completion_csv.empty() ? "off" : completion_csv)
             << " state_trace_csv=" << (state_trace_csv.empty() ? "off" : state_trace_csv)
             << " goodput_trace_csv="
             << (config.goodput_trace_csv.has_value() ? *config.goodput_trace_csv : "off")
             << " goodput_trace_bin_ps=" << config.goodput_trace_bin_ps
             << " resolved_rank_mapping=" << resolved_rank_mapping
             << " physical_nodes=" << physical_node_count << '\n';
    manifest << "[DCQCN manifest] topology=" << config.topology_file
             << " clos_tiers=2 switch=ns-tm3"
             << " routing=flow-hashed-ecmp"
             << " ecmp_seed=" << config.ecmp_seed
             << " endpoint_link_bps=" << config.endpoint_link_bps
             << " shared_buffer_bytes=" << config.ns_tm3_shared_buffer_bytes
             << " shared_buffer_scope=switch-wide"
             << " egress_buffer_bytes=" << config.ns_tm3_egress_buffer_bytes
             << " egress_buffer_scope=per-physical-egress"
             << " buffer_residency=queued-voq-excludes-egress-serializer" << '\n';
    manifest << "[DCQCN manifest] transport=rocev2-dcqcn"
             << " recovery="
             << (config.selective_repeat ? "selective-repeat-limited" : "go-back-n")
             << " sr_window_packets=" << config.sr_window_packets
             << " sr_fallback=go-back-n-beyond-window"
             << " loss_rate_cut=" << (config.loss_rate_cut ? "on" : "off")
             << " cnp_interval_ps=" << DCQCNSink::_cnp_interval
             << " cnp_timer=single-coalesced-event-source"
             << " cc_update_period_ps=" << DCQCNSrc::_cc_update_period
             << " cc_timer=dedicated-coalesced-event-source"
             << " pacing_timer=single-coalesced-event-source"
             << " dcqcn_min_rate_bps=" << config.dcqcn_min_rate_bps
             << " silent_loss_rto_ps=" << config.silent_loss_rto_ps
             << " max_wire_packet_bytes=" << config.max_wire_packet_bytes
             << " data_header_bytes=" << config.data_header_bytes
             << " final_packet=exact-payload-tail" << '\n';
    manifest << "[DCQCN manifest] ecn=ns-tm3-egress-selection-red"
             << " ecn_kmin_bytes=" << config.ecn_kmin_bytes
             << " ecn_kmax_bytes=" << config.ecn_kmax_bytes
             << " ecn_pmax_ppm=" << config.ecn_pmax_ppm << " ecn_seed=" << config.ecn_seed
             << " ecn_sampler=packet-switch-egress-hash"
             << " pfc="
             << (config.pfc_enabled ? "data-priority-only" : "off-ecn-only-drop-on-overflow")
             << " pfc_meter_scope=per-physical-ingress"
             << " pfc_low_bytes=" << config.pfc_low_threshold_bytes
             << " pfc_high_bytes=" << config.pfc_high_threshold_bytes
             << " pfc_delivery=dedicated-link-local-reverse-serializer"
             << " pfc_reverse_sharing=not-shared-with-reverse-data"
             << " pfc_reverse_order=fifo-serialize-then-propagate"
             << " pfc_wire_bytes=64"
             << " pfc_preemption=none"
             << " control_priority=strict-nonpreemptive" << '\n';
    return manifest.str();
}
