#include "atlahs_htsim_api.h"
#include "atlahs_event.h"
#include "datacenter/fat_tree_topology.h"

#include "logsim-interface.h"
#include "lgs/LogGOPSim.hpp"

#include <stdexcept>

namespace {

void incrementAuthorityCounter(
        std::uint64_t& counter, const char* message) {
    if (counter == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(message);
    }
    ++counter;
}

AtlahsWqeCompletionProjection legacyProjection(
        const AtlahsWqeRecord& wqe) {
    if (!wqe.cq_post_sequence.has_value()
        || !wqe.cq_consume_sequence.has_value()) {
        throw std::logic_error(
            "ATLAHS completed WQE without CQ post and consume records");
    }
    return AtlahsWqeCompletionProjection{
        wqe.wqe_id,
        wqe.sq_id,
        wqe.rq_id,
        wqe.cq_id,
        wqe.sq_post_sequence,
        wqe.sq_dispatch_sequence,
        *wqe.cq_post_sequence,
        *wqe.cq_consume_sequence,
        wqe.transport_kind,
        wqe.transport_object_id};
}

}  // namespace

void AtlahsHtsimApi::setLogSimInterface(LogSimInterface* logsim_interface) {
    _logsim_interface = logsim_interface;
    if (_logsim_interface != nullptr) {
        _logsim_interface->setNetworkTiming(
            _flow_runtime ? AtlahsNetworkTiming::RuntimeOwned
                          : AtlahsNetworkTiming::LegacyLogSimGap);
    }
}

void AtlahsHtsimApi::setFlowRuntime(
    std::unique_ptr<AtlahsFlowRuntime> runtime) {
    if (!_pending_flows.empty()) {
        throw std::logic_error("cannot replace an ATLAHS runtime with pending flows");
    }
    if (_flow_runtime_setup || _active_wqe_authority.has_value()
        || _wqe_ledger != nullptr) {
        throw std::logic_error(
            "cannot replace an ATLAHS runtime after WQE setup");
    }
    _flow_runtime = std::move(runtime);
    if (_logsim_interface != nullptr) {
        _logsim_interface->setNetworkTiming(
            _flow_runtime ? AtlahsNetworkTiming::RuntimeOwned
                          : AtlahsNetworkTiming::LegacyLogSimGap);
    }
}

void AtlahsHtsimApi::refreshNativeAuthorityCounters() {
    if (_flow_runtime == nullptr
        || !_active_wqe_authority.has_value()
        || *_active_wqe_authority
               != AtlahsWqeAuthorityMode::NativeRuntime) {
        throw std::logic_error(
            "ATLAHS native authority observation has no structural runtime");
    }
    const AtlahsWqeAuthorityCounters observed =
        _flow_runtime->observedWqeAuthorityCounters();
    if (observed.legacy_ledger_constructed != 0
        || observed.legacy_posts != 0
        || observed.legacy_aborts != 0
        || observed.legacy_mutations != 0) {
        throw std::logic_error(
            "ATLAHS structural runtime reported legacy lifecycle activity");
    }
    _authority_counters = observed;
}

bool AtlahsHtsimApi::completeFlow(AtlahsFlowId flow_id) {
    auto pending_it = _pending_flows.find(flow_id);
    if (pending_it == _pending_flows.end()) {
        return false;
    }

    if (_eventlist == nullptr) {
        throw std::logic_error(
            "ATLAHS completed a runtime-owned flow without an EventList");
    }
    const simtime_picosec completion_time_ps = _eventlist->now();
    if (completion_time_ps < pending_it->second.request.start_time_ps) {
        throw std::logic_error(
            "ATLAHS runtime-owned flow completed before it started");
    }
    if (!_active_wqe_authority.has_value()) {
        throw std::logic_error(
            "ATLAHS completed a runtime-owned flow before authority setup");
    }

    AtlahsWqeCompletionProjection projection;
    if (*_active_wqe_authority
        == AtlahsWqeAuthorityMode::LegacyLedger) {
        if (_wqe_ledger == nullptr
            || !pending_it->second.legacy_wqe_id.has_value()) {
            throw std::logic_error(
                "ATLAHS legacy flow completed without WQE setup");
        }
        projection = legacyProjection(_wqe_ledger->complete(
            *pending_it->second.legacy_wqe_id,
            completion_time_ps));
        incrementAuthorityCounter(
            _authority_counters.legacy_mutations,
            "ATLAHS legacy-mutation counter overflows");
    } else {
        if (_wqe_ledger != nullptr
            || pending_it->second.legacy_wqe_id.has_value()) {
            throw std::logic_error(
                "ATLAHS structural flow reached the legacy WQE ledger");
        }
        const auto native_projection =
            _flow_runtime->completionProjection(flow_id);
        if (!native_projection.has_value()) {
            throw std::logic_error(
                "ATLAHS native runtime omitted its completion projection");
        }
        projection = *native_projection;
        refreshNativeAuthorityCounters();
    }

    // Remove before notifying LogSimInterface.  EventFinished is synchronous,
    // and the local copy keeps event.node alive throughout that call while a
    // duplicate/re-entrant completion observes the flow as already complete.
    PendingFlow pending = std::move(pending_it->second);
    _pending_flows.erase(pending_it);
    _completed_flows.push_back(AtlahsCompletedFlowRecord{
        pending.request.flow_id,
        pending.request.source,
        pending.request.destination,
        pending.request.payload_bytes,
        pending.request.start_time_ps,
        completion_time_ps,
        pending.request.tag,
        projection.wqe_id,
        projection.sq_id,
        projection.rq_id,
        projection.cq_id,
        projection.sq_post_sequence,
        projection.sq_dispatch_sequence,
        projection.cq_post_sequence,
        projection.cq_consume_sequence,
        projection.transport_kind,
        projection.transport_object_id});

    EventOver event(
        static_cast<int>(pending.request.source),
        static_cast<int>(pending.request.destination),
        pending.request.payload_bytes,
        static_cast<int>(pending.request.tag),
        pending.request.start_time_ps,
        AtlahsEventType::SEND_EVENT_OVER);
    event.node = &pending.node;
    EventFinished(event);
    return true;
}

void AtlahsHtsimApi::validateWqeQuiescent() const {
    if (_flow_runtime == nullptr) {
        return;
    }
    if (!_flow_runtime_setup || !_active_wqe_authority.has_value()) {
        throw std::logic_error(
            "ATLAHS runtime authority was not set up");
    }
    if (!_pending_flows.empty()) {
        throw std::logic_error(
            "ATLAHS API still has pending flows at WQE quiescence");
    }
    if (_flow_runtime->hasPendingPhysicalWork()) {
        throw std::logic_error(
            "ATLAHS runtime retains physical work at WQE quiescence");
    }
    if (*_active_wqe_authority
        == AtlahsWqeAuthorityMode::NativeRuntime) {
        if (_wqe_ledger != nullptr) {
            throw std::logic_error(
                "ATLAHS structural runtime constructed the legacy WQE ledger");
        }
        const AtlahsWqeAuthorityCounters observed =
            _flow_runtime->observedWqeAuthorityCounters();
        if (observed.native_session_constructed != 1
            || observed.legacy_ledger_constructed != 0
            || observed.legacy_posts != 0
            || observed.legacy_aborts != 0
            || observed.legacy_mutations != 0
            || observed.native_posts
                   != _completed_flows.size()
            || _authority_counters.native_session_constructed
                   != observed.native_session_constructed
            || _authority_counters.legacy_ledger_constructed
                   != observed.legacy_ledger_constructed
            || _authority_counters.native_posts != observed.native_posts
            || _authority_counters.legacy_posts != observed.legacy_posts
            || _authority_counters.legacy_aborts != observed.legacy_aborts
            || _authority_counters.legacy_mutations
                   != observed.legacy_mutations) {
            throw std::logic_error(
                "ATLAHS structural authority counters are not observed");
        }
        return;
    }
    if (_wqe_ledger == nullptr) {
        throw std::logic_error(
            "ATLAHS bypass runtime has no WQE ledger at quiescence");
    }
    if (_authority_counters.native_session_constructed != 0
        || _authority_counters.native_posts != 0
        || _authority_counters.legacy_ledger_constructed != 1
        || _authority_counters.legacy_posts
               != _completed_flows.size()
                      + _authority_counters.legacy_aborts
        || _authority_counters.legacy_mutations
               != _authority_counters.legacy_posts
                      + _completed_flows.size()
                      + _authority_counters.legacy_aborts) {
        throw std::logic_error(
            "ATLAHS legacy authority counters are not observed");
    }
    _wqe_ledger->validateQuiescent();
    if (_wqe_ledger->completedCount() != _completed_flows.size()) {
        throw std::logic_error(
            "ATLAHS completed WQE count disagrees with completion rows");
    }
    for (const AtlahsCompletedFlowRecord& flow : _completed_flows) {
        const AtlahsWqeRecord& wqe = _wqe_ledger->wqe(flow.wqe_id);
        if (!wqe.completed() || wqe.flow_id != flow.flow_id) {
            throw std::logic_error(
                "ATLAHS completion row disagrees with its WQE record");
        }
    }
}
    
void AtlahsHtsimApi::Send(const SendEvent &event, graph_node_properties elem) {
    //std::cout << "AtlahsHtsimApi: Sending event" << std::endl;


    // New additions
    /* UecSrc::_rss_params                   = {8, timeFromUs(200.), UecSrc::MEAN_RTT, 3, 0, 0, 25};
    UecSrc::_flowbender_params            = {0.05, 1};
    UecSrc::_uss_params                   = {8, 3};
    UecSrc::ecmp_background_traffic_nodes = 0;
    UecSrc::_load_balancing_algo = UecSrc::RSS; */
    int to = event.getTo();
    int from = event.getFrom();
    int tag = event.getTag();
    uint64_t size = event.getSizeBytes();
    size = size * 1;    

    if (_flow_runtime) {
        const int physical_from = getHtsimNodeNumber(from, elem.nic);
        const int physical_to = getHtsimNodeNumber(to, elem.nic);
        if (physical_from < 0 || physical_to < 0) {
            throw std::invalid_argument("ATLAHS physical node IDs must be non-negative");
        }

        AtlahsFlowRequest request;
        request.flow_id = makeAtlahsFlowId(elem.host, elem.offset);
        request.source = static_cast<std::uint32_t>(physical_from);
        request.destination = static_cast<std::uint32_t>(physical_to);
        request.payload_bytes = size;
        request.start_time_ps = event.getStartTimeEvent();
        request.tag = elem.tag;

        if (!_flow_runtime_setup || !_active_wqe_authority.has_value()) {
            throw std::logic_error(
                "ATLAHS runtime-owned send before WQE setup");
        }
        if (_pending_flows.count(request.flow_id) != 0) {
            throw std::logic_error("duplicate ATLAHS GOAL host/offset flow ID");
        }

        std::optional<AtlahsWqeId> legacy_wqe_id;
        if (*_active_wqe_authority
            == AtlahsWqeAuthorityMode::LegacyLedger) {
            if (_wqe_ledger == nullptr) {
                throw std::logic_error(
                    "ATLAHS bypass send has no WQE ledger");
            }
            legacy_wqe_id = _wqe_ledger->postAndDispatch(
                request.flow_id,
                request.source,
                request.destination,
                request.payload_bytes,
                request.start_time_ps);
            incrementAuthorityCounter(
                _authority_counters.legacy_posts,
                "ATLAHS legacy-post counter overflows");
            incrementAuthorityCounter(
                _authority_counters.legacy_mutations,
                "ATLAHS legacy-mutation counter overflows");
        } else if (_wqe_ledger != nullptr) {
            throw std::logic_error(
                "ATLAHS structural send reached the legacy WQE ledger");
        }
        try {
            PendingFlow pending{elem, request, legacy_wqe_id};
            const auto inserted =
                _pending_flows.emplace(request.flow_id, std::move(pending));
            if (!inserted.second) {
                throw std::logic_error(
                    "duplicate ATLAHS GOAL host/offset flow ID");
            }
            _flow_runtime->send(request);
            if (*_active_wqe_authority
                == AtlahsWqeAuthorityMode::NativeRuntime) {
                refreshNativeAuthorityCounters();
            }
        } catch (...) {
            _pending_flows.erase(request.flow_id);
            if (_active_wqe_authority.has_value()
                && *_active_wqe_authority
                       == AtlahsWqeAuthorityMode::NativeRuntime) {
                refreshNativeAuthorityCounters();
            }
            if (_wqe_ledger != nullptr && legacy_wqe_id.has_value()
                && _wqe_ledger->contains(*legacy_wqe_id)
                && !_wqe_ledger->wqe(*legacy_wqe_id).completed()) {
                _wqe_ledger->abort(*legacy_wqe_id);
                incrementAuthorityCounter(
                    _authority_counters.legacy_aborts,
                    "ATLAHS legacy-abort counter overflows");
                incrementAuthorityCounter(
                    _authority_counters.legacy_mutations,
                    "ATLAHS legacy-mutation counter overflows");
            }
            throw;
        }
        return;
    }

    simtime_picosec transmission_delay =
            (Packet::data_packet_size() * 8 / speedAsGbps(linkspeed) * _topo->cfg().get_diameter() *
             1000) +
            (UecBasePacket::get_ack_size() * 8 / speedAsGbps(linkspeed) * _topo->cfg().get_diameter() *
             1000);
        simtime_picosec base_rtt_bw_two_points =
            2 * _topo->cfg().get_two_point_diameter_latency(from, to) + transmission_delay;

    

    from = getHtsimNodeNumber(from, elem.nic);
    to = getHtsimNodeNumber(to, elem.nic);

    simtime_picosec flow_duration = size * 8 / 200 * 1000;

    if (from == to) {
        std::cerr << "Error: Send event from and to the same node" << std::endl;
        exit(0);
    }

    if (_logsim_interface->get_protocol() == UEC_PROTOCOL) { 
        TrafficLoggerSimple* traffic_logger = NULL;

        // Construct a fresh multipath instance per flow
        if (!mp_factory) {
            std::cerr << "Error: Multipath not set in AtlahsHtsimApi" << std::endl;
            exit(0);
        }
        auto per_flow_mp = mp_factory();

        UecSrc *uecSrc = new UecSrc(traffic_logger, *_eventlist, std::move(per_flow_mp), *uec_nics.at(from), 1);

        // setFlowsize is the correct method name
        uecSrc->setFlowsize(size);
        uecSrc->initNscc(cwnd_b, base_rtt_bw_two_points);


        uecSrc->setName("uec_" + std::to_string(from) + "_" + std::to_string(to));
        uecSrc->from = from;
        uecSrc->to = to;
        uecSrc->tag = tag;
        uecSrc->send_size = size;
        uecSrc->_atlahs_api = this;

        UecSink *uecSink = new UecSink(traffic_logger,
                                  linkspeed,
                                  1.1,
                                  UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                  *_eventlist,
                                  *uec_nics.at(to),
                                  1);
        uecSink->setName("uec_sink_Rand");
        uecSink->from_sink = from;
        uecSink->to_sink = to;
        uecSink->tag_sink = tag;

        uecSrc->set_dst(to);
        uecSrc->setSrc(from);
        uecSrc->setDst(to);
        uecSink->set_src(from);

        Route* srctotor = new Route();
        srctotor->push_back(_topo->queues_ns_nlp[from][_topo->cfg().HOST_POD_SWITCH(from)][0]);
        srctotor->push_back(_topo->pipes_ns_nlp[from][_topo->cfg().HOST_POD_SWITCH(from)][0]);
        srctotor->push_back(_topo->queues_ns_nlp[from][_topo->cfg().HOST_POD_SWITCH(from)][0]->getRemoteEndpoint());

        Route* dsttotor = new Route();
        dsttotor->push_back(_topo->queues_ns_nlp[to][_topo->cfg().HOST_POD_SWITCH(to)][0]);
        dsttotor->push_back(_topo->pipes_ns_nlp[to][_topo->cfg().HOST_POD_SWITCH(to)][0]);
        dsttotor->push_back(_topo->queues_ns_nlp[to][_topo->cfg().HOST_POD_SWITCH(to)][0]->getRemoteEndpoint());

        graph_node_properties* node_copy = new graph_node_properties(elem);
        uecSrc->lgs_node = node_copy;
        //uecSrc->connect(srctotor, dsttotor, *uecSink, _eventlist->now());

        uecSrc->connectPort(0, *srctotor, *dsttotor, *uecSink, _eventlist->now());

        //register src and snk to receive packets from their respective TORs. 
        assert(_topo->switches_lp[_topo->cfg().HOST_POD_SWITCH(from)]);
        assert(_topo->switches_lp[_topo->cfg().HOST_POD_SWITCH(from)]);
        _topo->switches_lp[_topo->cfg().HOST_POD_SWITCH(from)]->addHostPort(
                        from, uecSink->flowId(), uecSrc->getPort(0));
        _topo->switches_lp[_topo->cfg().HOST_POD_SWITCH(to)]->addHostPort(
                        to, uecSrc->flowId(), uecSink->getPort(0));

    }
    // TODO: Move this stuff to a CreateConnection function inside UEC. 
    // TODO: Support different tranports, not just UEC
}

void AtlahsHtsimApi::Recv(const RecvEvent &event) {
    // No Op for HTSIM
}

void AtlahsHtsimApi::Calc(const ComputeAtlahsEvent &event) {
    // Done Directly in lgs_interface for now
}

void AtlahsHtsimApi::Setup() {
    if (_flow_runtime) {
        if (total_nodes < 0) {
            throw std::invalid_argument("ATLAHS node count must be non-negative");
        }
        if (_flow_runtime_setup || _active_wqe_authority.has_value()
            || _wqe_ledger != nullptr) {
            throw std::logic_error("ATLAHS WQE runtime setup twice");
        }
        const AtlahsWqeAuthorityMode authority =
            _flow_runtime->wqeAuthorityMode();
        std::unique_ptr<AtlahsWqeLedger> candidate_ledger;
        if (authority == AtlahsWqeAuthorityMode::LegacyLedger) {
            candidate_ledger = std::make_unique<AtlahsWqeLedger>(
                static_cast<std::uint32_t>(total_nodes),
                _flow_runtime->transportKind());
        }
        _flow_runtime->setup(
            static_cast<std::uint32_t>(total_nodes),
            [this](AtlahsFlowId flow_id) { completeFlow(flow_id); });

        AtlahsWqeAuthorityCounters observed;
        if (authority == AtlahsWqeAuthorityMode::NativeRuntime) {
            observed = _flow_runtime->observedWqeAuthorityCounters();
            if (observed.native_session_constructed != 1
                || observed.legacy_ledger_constructed != 0
                || observed.legacy_posts != 0
                || observed.legacy_aborts != 0
                || observed.legacy_mutations != 0) {
                throw std::logic_error(
                    "ATLAHS structural runtime did not report construction");
            }
        }

        _wqe_ledger = std::move(candidate_ledger);
        _active_wqe_authority = authority;
        _flow_runtime_setup = true;
        if (authority == AtlahsWqeAuthorityMode::LegacyLedger) {
            incrementAuthorityCounter(
                _authority_counters.legacy_ledger_constructed,
                "ATLAHS legacy-ledger counter overflows");
        } else {
            _authority_counters = observed;
        }
        return;
    }

    printf("No of nodes %d\n", total_nodes);

    if (_logsim_interface->get_protocol() == EQDS_PROTOCOL) {
        /* for (size_t ix = 0; ix < total_nodes; ix++){
            printf("Setting up node %d\n", ix);
            pacersEQDS.push_back(new EqdsPullPacer(linkspeed, 0.99, EqdsSrc::_mtu, *_eventlist));   
            nics.push_back(new EqdsNIC(*_eventlist, linkspeed));
        } */
    } else if (_logsim_interface->get_protocol() == NDP_PROTOCOL) {
        /* for (size_t ix = 0; ix < total_nodes; ix++)
            pacersNDP.push_back(new NdpPullPacer(*_eventlist,  linkspeed, 0.99));    */
    }


    for (size_t ix = 0; ix < total_nodes; ix++) {
        uec_pacers.push_back(new UecPullPacer(linkspeed,
                                          0.99,
                                          UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                          *_eventlist,
                                          1));

        UecNIC* nic = new UecNIC(ix, *_eventlist, linkspeed, 1);
        uec_nics.push_back(nic);
    }
    
}

void AtlahsHtsimApi::EventFinished(const EventOver &event) {
    //std::cout << "AtlahsHtsimApi: Event is over" << std::endl;

    if (AtlahsEventType::SEND_EVENT_OVER == event.getEventType()) {
        //_logsim_interface->flow_over(*(event.getPacket()));
        _logsim_interface->flow_over(event);
    } else if (AtlahsEventType::COMPUTE_EVENT_OVER == event.getEventType()) {
        _logsim_interface->compute_over(1);
    } else {
        abort();
    }
}
