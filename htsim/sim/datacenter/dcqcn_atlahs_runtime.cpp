// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dcqcn_atlahs_runtime.h"

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
    return RocePacket::live_packet_count()
           + RoceAck::live_packet_count()
           + RoceNack::live_packet_count()
           + CNPPacket::live_packet_count()
           + EthPausePacket::live_packet_count();
}

class DcqcnHostQueue final : public BaseQueue {
public:
    DcqcnHostQueue(linkspeed_bps bitrate,
                   mem_b configured_capacity,
                   EventList& event_list,
                   std::string name)
        : BaseQueue(bitrate, event_list, nullptr),
          _configured_capacity(configured_capacity) {
        _nodename = std::move(name);
    }

    void register_sender(PacketSink& sender) {
        _data_senders.push_back(&sender);
    }

    void receivePacket(Packet& packet) override {
        if (packet.type() == ETH_PAUSE) {
            const auto& pause = static_cast<const EthPausePacket&>(packet);
            const bool paused = pause.sleepTime() > 0;
            _data_paused = paused;
            for (PacketSink* sender : _data_senders) {
                sender->receivePacket(
                    *EthPausePacket::newpkt(paused ? 1 : 0,
                                            pause.senderID()));
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
            throw std::invalid_argument(
                "DCQCN host queue accepts only high control or low DATA");
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
        if (_buffered_bytes < packet->size()) {
            throw std::logic_error("DCQCN host queue accounting underflow");
        }
        _buffered_bytes -= packet->size();
        packet->flow().logTraffic(
            *packet, *this, TrafficLogger::PKT_DEPART);
        log_packet_send(drainTime(packet));
        packet->sendOn();
        begin_service();
    }

    mem_b queuesize() const override {
        return _buffered_bytes;
    }
    mem_b maxsize() const override { return _configured_capacity; }
    mem_b queuesize_high_watermark() const override {
        return _high_watermark;
    }

private:
    Packet* select_low() {
        if (_data_paused || _low.empty()) {
            return nullptr;
        }
        auto selected = _has_last_low_flow
            ? _low.upper_bound(_last_low_flow)
            : _low.begin();
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
            eventlist().sourceIsPendingRel(*this, drainTime(_in_service));
        }
    }

    mem_b _configured_capacity;
    mem_b _buffered_bytes{0};
    mem_b _high_watermark{0};
    bool _data_paused{false};
    Packet* _in_service{nullptr};
    std::deque<Packet*> _high;
    std::map<flowid_t, std::deque<Packet*>> _low;
    bool _has_last_low_flow{false};
    flowid_t _last_low_flow{0};
    std::vector<PacketSink*> _data_senders;
};

class DcqcnAtlahsSrc final : public DCQCNSrc {
public:
    using Completion = std::function<void()>;

    DcqcnAtlahsSrc(EventList& event_list,
                   linkspeed_bps rate,
                   AtlahsFlowId atlahs_flow_id,
                   std::uint32_t source,
                   std::uint32_t destination,
                   AtlahsStateTrace* state_trace,
                   Completion completion)
        : DCQCNSrc(nullptr, nullptr, event_list, rate),
          _atlahs_flow_id(atlahs_flow_id),
          _source(source),
          _destination(destination),
          _state_trace(state_trace),
          _completion(std::move(completion)),
          _last_progress_ps(event_list.now()) {
        setStateObserver(
            [this](const char* event) { trace(event); });
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
        trace("gbn-nack");
    }

    void processPause(const EthPausePacket& pause) override {
        RoceSrc::processPause(pause);
        trace(pause.sleepTime() > 0 ? "pause" : "resume");
    }

    bool check_silent_rto(simtime_picosec now,
                          simtime_picosec rto) {
        if (done() || !has_outstanding_data()
            || now < _last_progress_ps
            || now - _last_progress_ps < rto) {
            return false;
        }
        _drops += static_cast<std::uint32_t>(_highest_sent - _last_acked);
        _highest_sent = _last_acked;
        _last_progress_ps = now;
        schedulePacingAt(now);
        trace("silent-rto");
        return true;
    }

    void traceFlowStart() { trace("flow-start"); }

private:
    void trace(const char* event) {
        if (_state_trace == nullptr || !_state_trace->enabled()) {
            return;
        }
        const bool paused = _state_send == PAUSED;
        const bool effective = !paused && !done();
        _state_trace->append(
            {eventlist().now(),
             _atlahs_flow_id,
             _source,
             _destination,
             event,
             current_rate(),
             effective ? current_rate() : UINT64_C(0),
             alpha(),
             paused,
             _new_packets_sent,
             _rtx_packets_sent,
             _acked_packets});
    }

    AtlahsFlowId _atlahs_flow_id;
    std::uint32_t _source;
    std::uint32_t _destination;
    AtlahsStateTrace* _state_trace;
    Completion _completion;
    simtime_picosec _last_progress_ps;
    bool _completion_notified{false};
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
    Impl(EventList& event_list,
         DcqcnAtlahsRuntimeConfig config,
         std::uint32_t physical_node_count)
        : EventSource(event_list, "DCQCN ATLAHS RTO scanner"),
          event_list(event_list),
          config(std::move(config)),
          physical_node_count(physical_node_count),
          state_trace(this->config.state_trace_csv.has_value()) {
        validate_config();

        Packet::set_packet_size(
            static_cast<int>(this->config.max_wire_packet_bytes
                             - this->config.data_header_bytes));
        RoceSink::ooo_enabled = false;
        RoceSrc::setMinRTO(static_cast<std::uint32_t>(
            this->config.silent_loss_rto_ps / UINT64_C(1000000)));
        DCQCNSrc::setMinRate(this->config.dcqcn_min_rate_bps);

        std::ifstream topology_probe(this->config.topology_file);
        if (!topology_probe.is_open()) {
            throw std::invalid_argument(
                "cannot open DCQCN topology file '"
                + this->config.topology_file + "'");
        }
        topology_config = FatTreeTopologyCfg::load(
            this->config.topology_file,
            this->config.ns_tm3_shared_buffer_bytes,
            COMPOSITE,
            FAIR_PRIO);
        if (topology_config == nullptr) {
            throw std::runtime_error(
                "DCQCN topology loader returned no configuration");
        }
        if (topology_config->get_tiers() != 2
            || topology_config->no_of_nodes() != physical_node_count) {
            throw std::invalid_argument(
                "DCQCN comparator requires the matching two-tier GOAL "
                "node count");
        }
        if (topology_config->downlink_speed(TOR_TIER)
                != this->config.endpoint_link_bps
            || topology_config->downlink_speed(AGG_TIER)
                != this->config.endpoint_link_bps
            || topology_config->bundlesize(AGG_TIER) != 1) {
            throw std::invalid_argument(
                "DCQCN topology must use one equal-rate link per Clos edge");
        }
        topology_config->set_switch_model(FatTreeSwitchModel::NsTm3);
        topology_config->set_ns_tm3_shared_buffer_capacity(
            this->config.ns_tm3_shared_buffer_bytes);
        topology = std::make_unique<FatTreeTopology>(
            topology_config.get(), nullptr, &event_list, nullptr);

        const NsTm3DcqcnPolicyConfig policy_config{
            true,
            this->config.ecn_kmin_bytes,
            this->config.ecn_kmax_bytes,
            this->config.ecn_pmax_ppm,
            this->config.ecn_seed,
            true,
            this->config.pfc_low_threshold_bytes,
            this->config.pfc_high_threshold_bytes,
            this->config.endpoint_link_bps};
        configure_switches(topology->switches_lp, policy_config);
        configure_switches(topology->switches_up, policy_config);
        configure_switches(topology->switches_c, policy_config);
    }

    ~Impl() override {
        if (scanner_armed) {
            EventList::cancelPendingSource(*this);
        }
    }

    void setup(std::uint32_t node_count,
               CompletionHandler handler) {
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
            host_queues.push_back(std::make_unique<DcqcnHostQueue>(
                config.endpoint_link_bps,
                config.ns_tm3_shared_buffer_bytes,
                event_list,
                "dcqcn-host-serializer-" + std::to_string(node)));
        }
        setup_complete = true;
    }

    void send(const AtlahsFlowRequest& request) {
        if (!setup_complete) {
            throw std::logic_error("DCQCN ATLAHS send before setup");
        }
        if (request.start_time_ps != event_list.now()) {
            throw std::invalid_argument(
                "DCQCN ATLAHS runtime owns send timing in picoseconds");
        }
        if (request.source >= physical_node_count
            || request.destination >= physical_node_count
            || request.source == request.destination) {
            throw std::out_of_range("invalid DCQCN ATLAHS endpoint pair");
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

        auto source = std::make_unique<DcqcnAtlahsSrc>(
            event_list,
            config.endpoint_link_bps,
            request.flow_id,
            request.source,
            request.destination,
            &state_trace,
            [this, flow_id = request.flow_id]() { complete(flow_id); });
        auto sink = std::make_unique<DCQCNSink>(event_list);
        source->setName(
            "dcqcn-" + std::to_string(request.source) + "-"
            + std::to_string(request.destination) + "-"
            + std::to_string(request.flow_id));
        sink->DataReceiver::setName(
            "dcqcn-sink-" + std::to_string(request.source) + "-"
            + std::to_string(request.destination));
        source->set_dst(request.destination);
        sink->set_src(request.source);
        source->set_flowsize(request.payload_bytes);

        Route* forward = make_route(
            request.source, request.destination, request.flow_id, *sink);
        Route* reverse = make_route(
            request.destination,
            request.source,
            splitmix64(request.flow_id ^ UINT64_C(0xd1b54a32d192ed03)),
            *source);
        source->connect(forward, reverse, *sink, TRIGGER_START);
        source->set_flowid(checkedWireFlowId(next_wire_flow_id++));
        source->setPath(static_cast<std::uint32_t>(forward->path_id()));
        host_queues[request.source]->register_sender(*source);

        sources.push_back(std::move(source));
        sinks.push_back(std::move(sink));
        sources.back()->startflow();
        sources.back()->traceFlowStart();

        if (scanner_was_idle && !scanner_armed) {
            arm_scanner();
        }
    }

    bool has_pending_physical_work() const noexcept {
        const bool transport_timer_pending = std::any_of(
            sources.begin(), sources.end(), [](const auto& source) {
                return source->pacing_event_pending()
                       || source->cc_timer_pending();
            });
        const bool cnp_timer_pending = std::any_of(
            sinks.begin(), sinks.end(), [](const auto& sink) {
                return sink->cnp_timer_pending();
            });
        const bool pending = !active_flow_ids.empty()
                             || transportLivePacketCount() != 0
                             || transport_timer_pending
                             || cnp_timer_pending
                             || scanner_armed;
        if (pending && EventList::getPendingSources().empty()) {
            std::cerr
                << "[DCQCN pending] active=" << active_flow_ids.size()
                << " data=" << RocePacket::live_packet_count()
                << " ack=" << RoceAck::live_packet_count()
                << " nack=" << RoceNack::live_packet_count()
                << " cnp=" << CNPPacket::live_packet_count()
                << " pfc=" << EthPausePacket::live_packet_count()
                << '\n';
        }
        return pending;
    }

    std::uint64_t sum_policy_counter(
            const std::function<std::uint64_t(
                const NsTm3DcqcnPolicyCounters&)>& select) const noexcept {
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

    std::uint64_t dropped_packets() const noexcept {
        std::uint64_t total = 0;
        const auto add = [&](const std::vector<Switch*>& switches) {
            for (Switch* base : switches) {
                const auto* ns_tm3 = dynamic_cast<const NsTm3Switch*>(base);
                if (ns_tm3 != nullptr) {
                    total += ns_tm3->buffer_counters().dropped_packets;
                }
            }
        };
        add(topology->switches_lp);
        add(topology->switches_up);
        add(topology->switches_c);
        return total;
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
    std::set<AtlahsFlowId> known_flow_ids;
    std::set<AtlahsFlowId> active_flow_ids;
    CompletionHandler completion_handler;
    bool setup_complete{false};
    bool scanner_armed{false};
    std::uint64_t next_wire_flow_id{1};
    std::uint64_t completed_flows{0};
    std::uint64_t silent_rtos{0};
    AtlahsStateTrace state_trace;

private:
    void validate_config() const {
        if (physical_node_count == 0 || config.topology_file.empty()) {
            throw std::invalid_argument(
                "DCQCN ATLAHS requires nodes and a topology file");
        }
        if (config.endpoint_link_bps == 0
            || config.max_wire_packet_bytes <= config.data_header_bytes
            || config.ns_tm3_shared_buffer_bytes <= 0
            || config.ecn_kmin_bytes < 0
            || config.ecn_kmax_bytes <= config.ecn_kmin_bytes
            || config.ecn_pmax_ppm == 0
            || config.ecn_pmax_ppm > UINT32_C(1000000)
            || config.pfc_low_threshold_bytes <= 0
            || config.pfc_low_threshold_bytes
                   >= config.pfc_high_threshold_bytes
            || config.pfc_high_threshold_bytes
                   >= config.ns_tm3_shared_buffer_bytes
            || config.silent_loss_rto_ps == 0
            || config.dcqcn_min_rate_bps == 0
            || config.dcqcn_min_rate_bps > config.endpoint_link_bps) {
            throw std::invalid_argument("invalid DCQCN ATLAHS model config");
        }
    }

    static void configure_switches(
            const std::vector<Switch*>& switches,
            const NsTm3DcqcnPolicyConfig& policy_config) {
        for (Switch* base : switches) {
            auto* ns_tm3 = dynamic_cast<NsTm3Switch*>(base);
            if (ns_tm3 == nullptr) {
                throw std::logic_error(
                    "DCQCN topology contains a non-ns-tm3 switch");
            }
            ns_tm3->configure_dcqcn_policy(policy_config);
        }
    }

    Route* make_route(std::uint32_t source,
                      std::uint32_t destination,
                      std::uint64_t entropy,
                      PacketSink& endpoint) {
        const std::uint32_t source_leaf =
            topology_config->HOST_POD_SWITCH(source);
        const std::uint32_t destination_leaf =
            topology_config->HOST_POD_SWITCH(destination);
        auto route = std::make_unique<Route>();
        route->push_back(host_queues[source].get());
        route->push_back(topology->pipes_ns_nlp[source][source_leaf][0]);
        route->push_back(
            topology->queues_ns_nlp[source][source_leaf][0]
                ->getRemoteEndpoint());

        int path_count = 1;
        int path_id = 0;
        if (source_leaf != destination_leaf) {
            path_count = static_cast<int>(topology_config->getNAGG());
            if (path_count <= 0) {
                throw std::logic_error("DCQCN Clos has no spine path");
            }
            path_id = static_cast<int>(
                splitmix64(entropy ^ config.ecmp_seed
                           ^ (static_cast<std::uint64_t>(source) << 32)
                           ^ destination)
                % static_cast<std::uint64_t>(path_count));
            const std::uint32_t spine =
                static_cast<std::uint32_t>(path_id);
            route->push_back(
                topology->queues_nlp_nup[source_leaf][spine][0]);
            route->push_back(
                topology->pipes_nlp_nup[source_leaf][spine][0]);
            route->push_back(
                topology->queues_nlp_nup[source_leaf][spine][0]
                    ->getRemoteEndpoint());
            route->push_back(
                topology->queues_nup_nlp[spine][destination_leaf][0]);
            route->push_back(
                topology->pipes_nup_nlp[spine][destination_leaf][0]);
            route->push_back(
                topology->queues_nup_nlp[spine][destination_leaf][0]
                    ->getRemoteEndpoint());
        }
        route->push_back(
            topology->queues_nlp_ns[destination_leaf][destination][0]);
        route->push_back(
            topology->pipes_nlp_ns[destination_leaf][destination][0]);
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
        const simtime_picosec interval = std::min(
            config.silent_loss_rto_ps,
            static_cast<simtime_picosec>(UINT64_C(1000000000)));
        event_list.sourceIsPendingRel(*this, interval);
        scanner_armed = true;
    }
};

DcqcnAtlahsRuntime::DcqcnAtlahsRuntime(
        EventList& event_list,
        DcqcnAtlahsRuntimeConfig config,
        std::uint32_t physical_node_count)
    : _impl(std::make_unique<Impl>(
          event_list, std::move(config), physical_node_count)) {}

DcqcnAtlahsRuntime::~DcqcnAtlahsRuntime() = default;

void DcqcnAtlahsRuntime::setup(
        std::uint32_t node_count,
        CompletionHandler complete_flow) {
    _impl->setup(node_count, std::move(complete_flow));
}

void DcqcnAtlahsRuntime::send(const AtlahsFlowRequest& request) {
    _impl->send(request);
}

bool DcqcnAtlahsRuntime::hasPendingPhysicalWork() const noexcept {
    return _impl->has_pending_physical_work();
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

std::uint64_t DcqcnAtlahsRuntime::ecn_marked_packet_count() const noexcept {
    return _impl->sum_policy_counter(
        [](const NsTm3DcqcnPolicyCounters& counters) {
            return counters.ecn_marked_packets;
        });
}

std::uint64_t DcqcnAtlahsRuntime::pfc_pause_count() const noexcept {
    return _impl->sum_policy_counter(
        [](const NsTm3DcqcnPolicyCounters& counters) {
            return counters.pause_frames;
        });
}

std::uint64_t DcqcnAtlahsRuntime::pfc_resume_count() const noexcept {
    return _impl->sum_policy_counter(
        [](const NsTm3DcqcnPolicyCounters& counters) {
            return counters.resume_frames;
        });
}

std::uint64_t DcqcnAtlahsRuntime::dropped_packet_count() const noexcept {
    return _impl->dropped_packets();
}

std::size_t DcqcnAtlahsRuntime::state_trace_row_count() const noexcept {
    return _impl->state_trace.size();
}

void DcqcnAtlahsRuntime::writeStateTraceCsv() const {
    if (!_impl->config.state_trace_csv.has_value()) {
        throw std::logic_error("DCQCN state trace was not requested");
    }
    if (_impl->has_pending_physical_work()) {
        throw std::logic_error(
            "DCQCN state trace may only be written at quiescence");
    }
    _impl->state_trace.writeCsvAtomically(
        *_impl->config.state_trace_csv);
}

std::string renderDcqcnAtlahsManifest(
        const DcqcnAtlahsRuntimeConfig& config,
        std::uint32_t physical_node_count,
    const std::string& goal_file,
    const std::string& completion_csv,
    const std::string& state_trace_csv,
    const char* resolved_rank_mapping) {
    std::ostringstream manifest;
    manifest
        << "[DCQCN manifest] schema=dcqcn-atlahs-model-v2"
        << " profile=dcqcn"
        << " goal=" << goal_file
        << " completion_csv="
        << (completion_csv.empty() ? "off" : completion_csv)
        << " state_trace_csv="
        << (state_trace_csv.empty() ? "off" : state_trace_csv)
        << " resolved_rank_mapping=" << resolved_rank_mapping
        << " physical_nodes=" << physical_node_count
        << '\n';
    manifest
        << "[DCQCN manifest] topology=" << config.topology_file
        << " clos_tiers=2 switch=ns-tm3"
        << " routing=flow-hashed-ecmp"
        << " ecmp_seed=" << config.ecmp_seed
        << " endpoint_link_bps=" << config.endpoint_link_bps
        << " shared_buffer_bytes=" << config.ns_tm3_shared_buffer_bytes
        << '\n';
    manifest
        << "[DCQCN manifest] transport=rocev2-dcqcn"
        << " recovery=go-back-n"
        << " cnp_interval_ps=" << DCQCNSink::_cnp_interval
        << " cnp_timer=single-coalesced-event-source"
        << " cc_update_period_ps=" << DCQCNSrc::_cc_update_period
        << " cc_timer=dedicated-coalesced-event-source"
        << " pacing_timer=single-coalesced-event-source"
        << " dcqcn_min_rate_bps=" << config.dcqcn_min_rate_bps
        << " silent_loss_rto_ps=" << config.silent_loss_rto_ps
        << " max_wire_packet_bytes=" << config.max_wire_packet_bytes
        << " data_header_bytes=" << config.data_header_bytes
        << " final_packet=exact-payload-tail"
        << '\n';
    manifest
        << "[DCQCN manifest] ecn=ns-tm3-egress-selection-red"
        << " ecn_kmin_bytes=" << config.ecn_kmin_bytes
        << " ecn_kmax_bytes=" << config.ecn_kmax_bytes
        << " ecn_pmax_ppm=" << config.ecn_pmax_ppm
        << " ecn_seed=" << config.ecn_seed
        << " ecn_sampler=packet-switch-egress-hash"
        << " pfc=data-priority-only"
        << " pfc_low_bytes=" << config.pfc_low_threshold_bytes
        << " pfc_high_bytes=" << config.pfc_high_threshold_bytes
        << " pfc_delivery=dedicated-link-local-reverse-serializer"
        << " pfc_reverse_sharing=not-shared-with-reverse-data"
        << " pfc_reverse_order=fifo-serialize-then-propagate"
        << " pfc_wire_bytes=64"
        << " pfc_preemption=none"
        << " control_priority=strict-nonpreemptive"
        << '\n';
    return manifest.str();
}
