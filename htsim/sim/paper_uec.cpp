// -*- c-basic-offset: 4; tab-width: 8; indent-tabs-mode: t -*-
#include "paper_uec.h"
#include "ecn.h"
#include "queue.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <math.h>
#include <regex>
#include <stdio.h>
#include <utility>

#define timeInf 0

// Static Parameters
int PaperUecSrc::jump_to = 0;
double PaperUecSrc::kmax_double;
double PaperUecSrc::kmin_double;
std::string PaperUecSrc::queue_type = "composite";
std::string PaperUecSrc::algorithm_type = "standard_trimming";
bool PaperUecSrc::use_fast_drop = false;
int PaperUecSrc::fast_drop_rtt = 1;
bool PaperUecSrc::use_pacing = false;
simtime_picosec PaperUecSrc::pacing_delay = 0;
bool PaperUecSrc::do_jitter = false;
bool PaperUecSrc::do_exponential_gain = false;
bool PaperUecSrc::use_fast_increase = false;
uint64_t PaperUecSrc::_interdc_delay = 0;
bool PaperUecSrc::use_super_fast_increase = false;
int PaperUecSrc::target_rtt_percentage_over_base = 50;
bool PaperUecSrc::stop_after_quick = false;
double PaperUecSrc::y_gain = 1;
double PaperUecSrc::x_gain = 0.15;
double PaperUecSrc::z_gain = 1;
double PaperUecSrc::w_gain = 1;
double PaperUecSrc::quickadapt_lossless_rtt = 2.0;
bool PaperUecSrc::disable_case_4 = false;
bool PaperUecSrc::disable_case_3 = false;
double PaperUecSrc::starting_cwnd = 1;
double PaperUecSrc::bonus_drop = 1;
double PaperUecSrc::buffer_drop = 1.2;
int PaperUecSrc::ratio_os_stage_1 = 1;
double PaperUecSrc::decrease_on_nack = 1;
simtime_picosec PaperUecSrc::stop_pacing_after_rtt = 0;
int PaperUecSrc::reaction_delay = 1;
int PaperUecSrc::precision_ts = 1;
int PaperUecSrc::once_per_rtt = 0;
uint64_t PaperUecSrc::explicit_target_rtt = 0;
uint64_t PaperUecSrc::explicit_base_rtt = 0;
uint64_t PaperUecSrc::explicit_bdp = 0;
uint64_t PaperUecSrc::_switch_queue_size = 0;
double PaperUecSrc::exp_avg_ecn_value = 0.3;
double PaperUecSrc::exp_avg_rtt_value = 0.3;
double PaperUecSrc::exp_avg_alpha = 0.125;
bool PaperUecSrc::use_exp_avg_ecn = false;
bool PaperUecSrc::use_exp_avg_rtt = false;
int PaperUecSrc::adjust_packet_counts = 1;

RouteStrategy PaperUecSrc::_route_strategy = NOT_SET;
RouteStrategy PaperUecSink::_route_strategy = NOT_SET;

PaperUecSrc::PaperUecSrc(PaperUecLogger *logger, TrafficLogger *pktLogger, EventList &eventList, uint64_t rtt, uint64_t bdp,
               uint64_t queueDrainTime, int hops)
        : EventSource(eventList, "uec"), _logger(logger), _flow(pktLogger) {
    _mss = Packet::data_packet_size();
    _unacked = 0;
    _nodename = "uecsrc";

    _last_acked = 0;
    _highest_sent = 0;
    _use_good_entropies = false;
    _next_good_entropy = 0;

    _nack_rtx_pending = 0;
    current_ecn_rate = 0;

    // new CC variables
    _hop_count = hops;

    _base_rtt = rtt;

    target_rtt = _base_rtt * 1.5;

    if (precision_ts != 1) {
        _base_rtt = (((_base_rtt + precision_ts - 1) / precision_ts) * precision_ts);
    }

    _target_rtt = _base_rtt * ((target_rtt_percentage_over_base + 1) / 100.0 + 1);

    if (precision_ts != 1) {
        _target_rtt = (((_target_rtt + precision_ts - 1) / precision_ts) * precision_ts);
    }

    _rtt = _base_rtt;
    _rto = rtt + _hop_count * queueDrainTime + (rtt * 900000);
    _rto = _base_rtt * 3;
    _rto_margin = _rtt / 8;
    _rtx_timeout = timeInf;
    _rtx_timeout_pending = false;
    _rtx_pending = false;
    _crt_path = 0;
    _flow_size = _mss * 934;
    _trimming_enabled = true;

    _next_pathid = 1;

    _bdp = bdp;
    _queue_size = _bdp; // Temporary
    initial_x_gain = x_gain;
    initial_z_gain = z_gain;

    if (explicit_base_rtt != 0) {
        _base_rtt = explicit_base_rtt;
        _target_rtt = explicit_target_rtt;
        bdp = explicit_bdp * 1;
    }

    // internal_stop_pacing_rtt = 0;

    //printf("BDP and RTT: %lu %lu\n", _bdp, _base_rtt);

    _maxcwnd = _bdp * 1;
    _cwnd = _bdp;
    _consecutive_low_rtt = 0;
    target_window = _cwnd;
    _target_based_received = true;

    _max_good_entropies = 10; // TODO: experimental value
    _enableDistanceBasedRtx = false;
    f_flow_over_hook = nullptr;
    last_pac_change = 0;


    if (algorithm_type == "mprdma") {
        _cwnd = _cwnd / 2;
    } else if (algorithm_type == "min_cc") {
        _cwnd = 1 * _mss;
    } else if (algorithm_type == "no_cc") {
        _cwnd = _bdp;
    } else if (algorithm_type == "swift_like") {
        _cwnd = _cwnd;
    }
}


// Start the flow
void PaperUecSrc::doNextEvent() { 
    //printf("Starting Connection %s at %lu or %f -- Size %d\n", _name.c_str(), eventlist().now(), timeAsUs(eventlist().now()), _flow_size);
    startflow(); }

// Triggers for connection matrixes
void PaperUecSrc::set_end_trigger(Trigger &end_trigger) { _end_trigger = &end_trigger; }

// Update Network Parameters
void PaperUecSrc::updateParams() {
}

std::size_t PaperUecSrc::get_sent_packet_idx(uint32_t pkt_seqno) {
    for (std::size_t i = 0; i < _sent_packets.size(); ++i) {
        if (pkt_seqno == _sent_packets[i].seqno) {
            return i;
        }
    }
    return _sent_packets.size();
}

void PaperUecSrc::update_rtx_time() {
    _rtx_timeout = timeInf;
    for (const auto &sp : _sent_packets) {
        auto timeout = sp.timer;
        if (!sp.acked && !sp.nacked && !sp.timedOut && (timeout < _rtx_timeout || _rtx_timeout == timeInf)) {
            _rtx_timeout = timeout;
        }
    }
}

void PaperUecSrc::mark_received(PaperUecAck &pkt) {
    // cummulative ack
    if (pkt.seqno() == 1) {
        while (!_sent_packets.empty() && (_sent_packets[0].seqno <= pkt.ackno() || _sent_packets[0].acked)) {
            _sent_packets.erase(_sent_packets.begin());
        }
        update_rtx_time();
        return;
    }
    if (_sent_packets.empty() || _sent_packets[0].seqno > pkt.ackno()) {
        // duplicate ACK -- since we support OOO, this must be caused by
        // duplicate retransmission
        return;
    }
    auto i = get_sent_packet_idx(pkt.seqno());
    if (i == 0) {
        // this should not happen because of cummulative acks, but
        // shouldn't cause harm either
        do {
            _sent_packets.erase(_sent_packets.begin());
        } while (!_sent_packets.empty() && _sent_packets[0].acked);
    } else {
        assert(i < _sent_packets.size());
        auto timer = _sent_packets[i].timer;
        auto seqno = _sent_packets[i].seqno;
        auto nacked = _sent_packets[i].nacked;
        _sent_packets[i] = PaperSentPacket(timer, seqno, true, false, false);
        if (nacked) {
            --_nack_rtx_pending;
        }
        _last_acked = seqno + _mss - 1;
        if (_enableDistanceBasedRtx) {
            bool trigger = true;
            // TODO: this could be optimized with counters or bitsets,
            // but I'm doing this the simple way to avoid bugs while
            // we don't need the optimizations
            for (std::size_t k = 1; k < _sent_packets.size() / 2; ++k) {
                if (!_sent_packets[k].acked) {
                    trigger = false;
                    break;
                }
            }
            if (trigger) {
                // TODO: what's the proper way to act if this packet was
                // NACK'ed? Not super relevant right now as we are not enabling
                // this feature anyway
                _sent_packets[0].timer = eventlist().now();
                _rtx_timeout_pending = true;
            }
        }
    }
    update_rtx_time();
}

void PaperUecSrc::add_ack_path(const Route *rt) {
    for (auto &r : _good_entropies) {
        if (r == rt) {
            return;
        }
    }
    if (_good_entropies.size() < _max_good_entropies) {
        _good_entropies.push_back(rt);
    } else {
        _good_entropies[_next_good_entropy] = rt;
        ++_next_good_entropy;
        _next_good_entropy %= _max_good_entropies;
    }
}

void PaperUecSrc::set_traffic_logger(TrafficLogger *pktlogger) { _flow.set_logger(pktlogger); }

void PaperUecSrc::reduce_cwnd(uint64_t amount) {
    if (_cwnd >= amount + _mss) {
        _cwnd -= amount * 1;
    } else {
        _cwnd = _mss;
    }
}

void PaperUecSrc::reduce_unacked(uint64_t amount) {
    if (_unacked >= amount) {
        _unacked -= amount;
    } else {
        _unacked = 0;
    }
}

void PaperUecSrc::check_limits_cwnd() {
    // Upper Limit
    if (_cwnd > _maxcwnd) {
        _cwnd = _maxcwnd;
    }
    // Lower Limit
    if (_cwnd < _mss) {
        _cwnd = _mss;
    }
}


void PaperUecSrc::processNack(PaperUecNack &pkt) {  

    _nacks_received++;

    if (algorithm_type == "mprdma") {
        _cwnd = _cwnd / 2;
    } else if (algorithm_type == "min_cc") {
        _cwnd = 1 * _mss;
    } else if (algorithm_type == "no_cc") {
        _cwnd = _bdp;
    } else if (algorithm_type == "swift_like") {
        _cwnd = _cwnd / 2;
    }
    
    check_limits_cwnd();


    // mark corresponding packet for retransmission
    auto i = get_sent_packet_idx(pkt.seqno());
    //printf("i %d vs %d\n", i, _sent_packets.size());
    //fflush(stdout);
    assert(i < _sent_packets.size());

    assert(!_sent_packets[i].acked); // TODO: would it be possible for a packet
                                     // to receive a nack after being acked?
    if (!_sent_packets[i].nacked) {
        // ignore duplicate nacks for the same packet
        _sent_packets[i].nacked = true;
        ++_nack_rtx_pending;
    }

    bool success = resend_packet(i);
    if (!_rtx_pending && !success) {
        _rtx_pending = true;
    }
}

/* Choose a route for a particular packet */
int PaperUecSrc::choose_route() {

    switch (_route_strategy) {
    case PULL_BASED: {
        /* this case is basically SCATTER_PERMUTE, but avoiding bad paths. */

        assert(simple_numb_paths > 0);
        if (simple_numb_paths == 1) {
            // special case - no choice
            return 0;
        }
        // otherwise we've got a choice
        _crt_path++;
        if (_crt_path == simple_numb_paths) {
            // permute_paths();
            _crt_path = 0;
        }
        uint32_t path_id = _path_ids[_crt_path];
        _avoid_score[path_id] = _avoid_ratio[path_id];
        int ctr = 0;
        while (_avoid_score[path_id] > 0 /* && ctr < 2*/) {
            printf("as[%d]: %d\n", path_id, _avoid_score[path_id]);
            _avoid_score[path_id]--;
            ctr++;
            // re-choosing path
            cout << "re-choosing path " << path_id << endl;
            _crt_path++;
            if (_crt_path == simple_numb_paths) {
                // permute_paths();
                _crt_path = 0;
            }
            path_id = _path_ids[_crt_path];
            _avoid_score[path_id] = _avoid_ratio[path_id];
        }
        // cout << "AS: " << _avoid_score[path_id] << " AR: " <<
        // _avoid_ratio[path_id] << endl;
        assert(_avoid_score[path_id] == 0);
        break;
    }
    case SCATTER_RANDOM:
        // ECMP
        assert(simple_numb_paths > 0);
        _crt_path = random() % simple_numb_paths;
        break;
    case SCATTER_PERMUTE:
    case SCATTER_ECMP:
        // Cycle through a permutation.  Generally gets better load balancing
        // than SCATTER_RANDOM.
        _crt_path++;
        assert(simple_numb_paths > 0);
        if (_crt_path / 1 == simple_numb_paths) {
            // permute_paths();
            _crt_path = 0;
        }
        break;
    case ECMP_FIB:
        // Cycle through a permutation.  Generally gets better load balancing
        // than SCATTER_RANDOM.
        _crt_path++;
        if (_crt_path == simple_numb_paths) {
            // permute_paths();
            _crt_path = 0;
        }
        break;
    case ECMP_FIB_ECN: {
        // Cycle through a permutation, but use ECN to skip paths
        while (1) {
            _crt_path++;
            if (_crt_path == simple_numb_paths) {
                // permute_paths();
                _crt_path = 0;
            }
            if (_path_ecns[_path_ids[_crt_path]] > 0) {
                _path_ecns[_path_ids[_crt_path]]--;
            } else {
                // eventually we'll find one that's zero
                break;
            }
        }
        break;
    }
    case SINGLE_PATH:
        abort(); // not sure if this can ever happen - if it can, remove this
                 // line
    case REACTIVE_ECN:
        return _crt_path;
    case NOT_SET:
        abort(); // shouldn't be here at all
    default:
        abort();
        break;
    }

    return _crt_path / 1;
}

int PaperUecSrc::next_route() {
    // used for reactive ECN.
    // Just move on to the next path blindly
    _crt_path++;
    assert(simple_numb_paths > 0);
    if (_crt_path == simple_numb_paths) {
        // permute_paths();
        _crt_path = 0;
    }
    return _crt_path;
}

void PaperUecSrc::processAck(PaperUecAck &pkt, bool force_marked) {
    PaperUecAck::seq_t seqno = pkt.ackno();
    simtime_picosec ts = pkt.timestamp_sent;

    consecutive_nack = 0;
    bool marked = pkt.flags() & ECN_ECHO; // ECN was marked on data packet and echoed on ACK


    uint64_t now_time = 0;
    if (precision_ts == 1) {
        now_time = eventlist().now();
    } else {
        now_time = (((eventlist().now() + precision_ts - 1) / precision_ts) * precision_ts);
    }

    uint64_t rtt = now_time - ts;
    rtt = rtt / 1000;

    //printf("Sent at %lu - Received at %lu - RTT %lu\n", ts, now_time, rtt);

    if (rtt < _base_rtt) {
        _base_rtt = rtt;
        target_rtt = _base_rtt * 1.5;
        if (_atlahs_api != nullptr) {
            _bdp = (_base_rtt) * _atlahs_api->linkspeed_gbps / 8;
        } else {
            _bdp = (_base_rtt) * 200 / 8;
        }
        _maxcwnd = _bdp;
        _cwnd =  std::min(_cwnd, (uint32_t)_maxcwnd);
    }


    mark_received(pkt);

    count_total_ack++;
    if (marked) {
        count_total_ecn++;
        consecutive_good_medium = 0;
    }

    if (seqno >= _flow_size && _sent_packets.empty() && !_flow_finished) {
        _flow_finished = true;
        if (f_flow_over_hook) {
            f_flow_over_hook(pkt);
        }
        //printf("Completion Time Flow %s is %f - Start Time %f - Overall Time %f - Size %d\n", _name.c_str(), timeAsUs(eventlist().now() - _flow_start_time), timeAsUs(_flow_start_time), timeAsUs(eventlist().now()), _flow_size);

        /* printf("Overall Completion at %lu\n", GLOBAL_TIME); */
        if (_end_trigger) {
            _end_trigger->activate();
        }

        EventOver *flow_over = new EventOver(from, to, _flow_size, tag, eventlist().now(), AtlahsEventType::SEND_EVENT_OVER);
        flow_over->node = lgs_node;
        flow_over->start_time_event = _flow_start_time;

        //printf("Setting1 elem %d %d - target%d offset%d proc%d nic%d\n", flow_over->node->host, flow_over->node->target, flow_over->node->target, flow_over->node->offset, flow_over->node->proc, flow_over->node->nic);

        for (const Route* route : _paths) {
            delete route;
        }
        _paths.clear();   

        for (const Route* route : this->_sink->_paths) {
            delete route;
        }
        this->_sink->_paths.clear();

        if (_atlahs_api) {
            if (_atlahs_api->print_stats_flows) {
                _atlahs_api->flowInfos.push_back(FlowInfo(timeAsUs(_flow_start_time), timeAsUs(eventlist().now()), timeAsUs(eventlist().now() - _flow_start_time), _flow_size, _nacks_received, _cwnd));
            }
            _atlahs_api->EventFinished(*flow_over);
        }
            
        return;
    }

    if (seqno > _last_acked || true) { // TODO: new ack, we don't care about
                                       // ordering for now. Check later though
        if (seqno >= _highest_sent) {
            _highest_sent = seqno;
        }

        _last_acked = seqno;

        // printf("Window Is %d - From %d To %d\n", _cwnd, from, to);
        current_pkt++;
        // printf("Triggering ADJ\n");
        adjust_window(1, marked, rtt);

        acked_bytes += _mss;
        good_bytes += _mss;

        _effcwnd = _cwnd;
        // printf("Received From %d - Sending More\n", from);
        send_packets();
        return; // TODO: if no further code, this can be removed
    }
}

uint64_t PaperUecSrc::get_unacked() {
    // return _unacked;
    uint64_t missing = 0;
    for (const auto &sp : _sent_packets) {
        if (!sp.acked && !sp.nacked && !sp.timedOut) {
            missing += _mss;
        }
    }
    return missing;
}

void PaperUecSrc::receivePacket(Packet &pkt) {
    // every packet received represents one less packet in flight


    // logger dropped in the paper port; PaperUecLogger is only forward-declared
    // and the ATLAHS glue always passes a null logger.
    switch (pkt.type()) {
    case UEC_PAPER:
        // BTS
        if (_bts_enabled) {
            if (pkt.bounced()) {
                counter_consecutive_good_bytes = 0;
                increasing = false;
            }
        }
        break;
    case UECACK_PAPER:
        count_received++;
        total_pkt++;

        processAck(dynamic_cast<PaperUecAck &>(pkt), false);

        pkt.free();
        break;
    case ETH_PAUSE:
        // processPause((const EthPausePacket &)pkt);
        pkt.free();
        return;
    case UECNACK_PAPER:
        // fflush(stdout);
        total_nack++;
        if (_trimming_enabled) {
            _next_pathid = -1;
            count_received++;
            processNack(dynamic_cast<PaperUecNack &>(pkt));
            pkt.free();
        }
        break;
    default:
        std::cout << "unknown packet receive with type code: " << pkt.type() << "\n";
        return;
    }
    if (get_unacked() < _cwnd && _rtx_timeout_pending) {
        eventlist().sourceIsPendingRel(*this, 1000);
    }
}


void PaperUecSrc::adjust_window(simtime_picosec ts, bool ecn, simtime_picosec rtt) {

    if (algorithm_type == "mprdma") {
        if (ecn) {
            _cwnd -= _mss * 0.5;
        } else {
            _cwnd += _mss * _mss / _cwnd;
        }
    } else if (algorithm_type == "min_cc") {
        _cwnd = 1 * _mss;
    } else if (algorithm_type == "no_cc") {
        _cwnd = _bdp;
    } else if (algorithm_type == "swift_like") {

        //printf("Flow %s  - RTT %lu - Target RTT %lu\n", _name.c_str(), rtt, target_rtt);

        if (eventlist().now() - last_decrease > _base_rtt) {
            can_decrease = true;
        }
        
        if (rtt < target_rtt) {
            // Additive increase:
            if (_cwnd >= _mss) {
                _cwnd += ((_mss * 1.0) / _cwnd) * _mss;
            } else {
                std::cerr << "Window below 1 MSS. Not supported.\n";
                std::abort();
            }
        } else if (can_decrease) {
            // Multiplicative decrease:
            _cwnd *= std::max(1 - 1 * (rtt - (double)target_rtt) / rtt, 1 - 0.25);
            last_decrease = eventlist().now();
        }
    }
    //printf("Flow %s  - CWND %lu - ECN %d\n", _name.c_str(), _cwnd, ecn);

    
    check_limits_cwnd();
}

const string &PaperUecSrc::nodename() { return _nodename; }

void PaperUecSrc::connect(Route *routeout, Route *routeback, PaperUecSink &sink, simtime_picosec starttime) {
    if (_route_strategy == SINGLE_PATH || _route_strategy == ECMP_FIB || _route_strategy == ECMP_FIB_ECN ||
        _route_strategy == REACTIVE_ECN) {
        assert(routeout);
        _route = routeout;
    }

    _sink = &sink;
    _flow.set_id(get_id()); // identify the packet flow with the NDP source
                            // that generated it
    _flow._name = _name;
    _sink->connect(*this, routeback);

    /* printf("StartTime %s is %lu\n", _name.c_str(), starttime); */

    eventlist().sourceIsPending(*this, starttime);
}

void PaperUecSrc::startflow() {
    ideal_x = x_gain;
    _flow_start_time = eventlist().now();

    /* printf("Starting Flow from %d to %d tag %d - RTT %lu - Target %lu - "
           "Time "
           "%lu\n",
           from, to, tag, _base_rtt, _target_rtt, GLOBAL_TIME / 1000); */
    send_packets();
}

const Route *PaperUecSrc::get_path() {
    if (_use_good_entropies && !_good_entropies.empty()) {
        auto rt = _good_entropies.back();
        _good_entropies.pop_back();
        return rt;
    }

    // Means we want to select a random one out of all paths, the original
    // idea
    if (_num_entropies == -1) {
        _crt_path = random() % simple_numb_paths;
    } else {
        // Else we use our entropy array of a certain size and roud robin it
        _crt_path = _entropy_array[current_entropy];
        current_entropy = current_entropy + 1;
        current_entropy = current_entropy % _num_entropies;
    }

    total_routes = simple_numb_paths;
    return _paths.at(_crt_path);
}

void PaperUecSrc::map_entropies() {
    for (int i = 0; i < _num_entropies; i++) {
        _entropy_array.push_back(random() % simple_numb_paths);
    }
    printf("Printing my Paths: ");
    for (int i = 0; i < _num_entropies; i++) {
        printf("%d - ", _entropy_array[i]);
    }
    printf("\n");
}

void PaperUecSrc::send_packets() {

    if (_rtx_pending) {
        retransmit_packet();
    }
    unsigned c = _cwnd;

    while (get_unacked() + _mss <= c && _highest_sent < _flow_size) {

        
        uint64_t data_seq = 0;
        PaperUecPacket *p = PaperUecPacket::newpkt(_flow, *_route, _highest_sent + 1, data_seq, _mss, false, _dstaddr);

        p->set_route(*_route);
        int crt = choose_route();

        p->set_pathid(_path_ids[crt]);
        p->from = this->from;
        p->to = this->to;
        p->tag = this->tag;

        //p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);

        // send packet
        _highest_sent += _mss;
        _packets_sent += _mss;
        _unacked += _mss;

        p->timestamp_sent = eventlist().now();

        //printf("Flow %s - Send %lu - Cwnd %d\n", _name.c_str(), eventlist().now() / 1000, _cwnd);

/* 
        printf("Sending packet from %s (%d) size %d at %lu %d vs %d ~ %d vs %d -- %d %d\n", _name.c_str(), flow_id(), p->size(), eventlist().now(), get_unacked()
           + _mss, _cwnd, _highest_sent, _flow_size, get_unacked() + _mss <= c, _highest_sent < _flow_size); 
 */
        // Getting time until packet is really sent
        /* printf("Send on at %lu -- %d %d\n", GLOBAL_TIME / 1000, pause_send, stop_after_quick); */
        PacketSink *sink = p->sendOn();
        
        HostQueue *q = dynamic_cast<HostQueue *>(sink);
        assert(q);
        uint32_t service_time = q->serviceTime(*p);
        _sent_packets.push_back(PaperSentPacket(eventlist().now() + service_time + _rto, p->seqno(), false, false, false));


        if (_rtx_timeout == timeInf) {
            update_rtx_time();
        }
    }
}

void PaperUecSrc::set_paths(uint32_t no_of_paths) {
    if (_route_strategy != ECMP_FIB && _route_strategy != ECMP_FIB_ECN &&
        _route_strategy != REACTIVE_ECN) {
        cout << "Set paths uec (path_count) called with wrong route "
                "strategy "
             << _route_strategy << endl;
        abort();
    }

    //printf("Called set_paths with %d paths\n", no_of_paths);

    _path_ids.resize(no_of_paths);
    simple_numb_paths = no_of_paths;


    for (size_t i = 0; i < no_of_paths; i++) {
        _path_ids[i] = i;
    }
}

void PaperUecSrc::set_paths(vector<const Route *> *rt_list) {
    uint32_t no_of_paths = rt_list->size();
    switch (_route_strategy) {
    case NOT_SET:
    case SINGLE_PATH:
    case ECMP_FIB:
    case ECMP_FIB_ECN:
    case REACTIVE_ECN:
        // shouldn't call this with these strategies
        abort();
    case SCATTER_PERMUTE:
    case SCATTER_RANDOM:
    case PULL_BASED:
    case SCATTER_ECMP: {
        no_of_paths = min(_num_entropies, (int)no_of_paths);
        _path_ids.resize(no_of_paths);
        _paths.resize(no_of_paths);
        _original_paths.resize(no_of_paths);
        _path_acks.resize(no_of_paths);
        _path_ecns.resize(no_of_paths);
        _path_nacks.resize(no_of_paths);
        _bad_path.resize(no_of_paths);
        _avoid_ratio.resize(no_of_paths);
        _avoid_score.resize(no_of_paths);
#if 0 // DEBUG_PATH_STATS: debug-only counters not ported from the artifact
        _path_counts_new.resize(no_of_paths);
        _path_counts_rtx.resize(no_of_paths);
        _path_counts_rto.resize(no_of_paths);
#endif

        // generate a randomize sequence of 0 .. size of rt_list - 1
        vector<int> randseq(rt_list->size());
        if (_route_strategy == SCATTER_ECMP) {
            // randsec may have duplicates, as with ECMP
            // randomize_sequence(randseq);
        } else {
            // randsec will have no duplicates
            // permute_sequence(randseq);
        }

        for (size_t i = 0; i < no_of_paths; i++) {
            // we need to copy the route before adding endpoints, as
            // it may be used in the reverse direction too.
            // Pick a random route from the available ones
            Route *tmp = new Route(*(rt_list->at(randseq[i])), *_sink);
            // Route* tmp = new Route(*(rt_list->at(i)));
            tmp->add_endpoints(this, _sink);
            tmp->set_path_id(i, rt_list->size());
            _paths[i] = tmp;
            _path_ids[i] = i;
            _original_paths[i] = tmp;
#if 0 // DEBUG_PATH_STATS: debug-only counters not ported from the artifact
            _path_counts_new[i] = 0;
            _path_counts_rtx[i] = 0;
            _path_counts_rto[i] = 0;
#endif
            _path_acks[i] = 0;
            _path_ecns[i] = 0;
            _path_nacks[i] = 0;
            _avoid_ratio[i] = 0;
            _avoid_score[i] = 0;
            _bad_path[i] = false;
        }
        _crt_path = 0;
        // permute_paths();
        break;
    }
    default: {
        abort();
        break;
    }
    }
}

void PaperUecSrc::apply_timeout_penalty() {
    if (_trimming_enabled) {
        reduce_cwnd(_mss);
    } else {
        reduce_cwnd(_mss);
        //_cwnd = _mss;
    }
}

void PaperUecSrc::rtx_timer_hook(simtime_picosec now, simtime_picosec period) { retransmit_packet(); }


bool PaperUecSrc::resend_packet(std::size_t idx) {

    if (get_unacked() >= _cwnd) {
        return false;
    }

    assert(!_sent_packets[idx].acked);

    // this will cause retransmission not only of the offending
    // packet, but others close to timeout
    _rto_margin = _rtt / 2;

    _unacked += _mss;
    PaperUecPacket *p = PaperUecPacket::newpkt(_flow, *_route, _sent_packets[idx].seqno, 0, _mss, true, _dstaddr);

    p->set_route(*_route);
    int crt = choose_route();
    p->from = this->from;

    // printf("Resending to %d\n", this->from);

    p->set_pathid(_path_ids[crt]);
    p->timestamp_sent = eventlist().now();

    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATE);
    /* printf("Send on at %lu -- %d %d\n", GLOBAL_TIME / 1000, pause_send, stop_after_quick); */
    PacketSink *sink = p->sendOn();
    HostQueue *q = dynamic_cast<HostQueue *>(sink);
    assert(q);
    uint32_t service_time = q->serviceTime(*p);
    if (_sent_packets[idx].nacked) {
        --_nack_rtx_pending;
        _sent_packets[idx].nacked = false;
    }
    _sent_packets[idx].timer = eventlist().now() + service_time + _rto;
    _sent_packets[idx].timedOut = false;
    update_rtx_time();

    return true;
}

// retransmission for timeout
void PaperUecSrc::retransmit_packet() {
    _rtx_pending = false;
    for (std::size_t i = 0; i < _sent_packets.size(); ++i) {
        auto &sp = _sent_packets[i];
        if (_rtx_timeout_pending && !sp.acked && !sp.nacked && sp.timer <= eventlist().now() + _rto_margin) {
            _cwnd = _mss;
            sp.timedOut = true;
            reduce_unacked(_mss);
        }
        if (!sp.acked && (sp.timedOut || sp.nacked)) {
            if (!resend_packet(i)) {
                _rtx_pending = true;
            }
        }
    }
    _rtx_timeout_pending = false;
}

/**********
 * PaperUecSink *
 **********/

PaperUecSink::PaperUecSink() : DataReceiver("sink"), _cumulative_ack{0}, _drops{0} { _nodename = "uecsink"; }

void PaperUecSink::set_end_trigger(Trigger &end_trigger) { _end_trigger = &end_trigger; }

void PaperUecSink::send_nack(simtime_picosec ts, bool marked, PaperUecAck::seq_t seqno, PaperUecAck::seq_t ackno, const Route *rt,
                        int path_id, bool is_failed) {

    PaperUecNack *nack = PaperUecNack::newpkt(_src->_flow, *_route, seqno, ackno, 0, _srcaddr);

    // printf("Sending NACK at %lu\n", GLOBAL_TIME);
    nack->set_pathid(_path_ids[_crt_path]);
    _crt_path++;
    if (_crt_path == simple_numb_paths) {
        _crt_path = 0;
    }

    nack->flow().logTraffic(*nack, *this, TrafficLogger::PKT_CREATESEND);
    nack->set_ts(ts);
    if (marked) {
        nack->set_flags(ECN_ECHO);
    } else {
        nack->set_flags(0);
    }

    nack->sendOn();
}

bool PaperUecSink::already_received(PaperUecPacket &pkt) {
    PaperUecPacket::seq_t seqno = pkt.seqno();

    if (seqno <= _cumulative_ack) { // TODO: this assumes
                                    // that all data packets
                                    // have the same size
        return true;
    }
    for (auto it = _received.begin(); it != _received.end(); ++it) {
        if (seqno == *it) {
            return true; // packet received OOO
        }
    }
    return false;
}

void PaperUecSink::receivePacket(Packet &pkt) {


    switch (pkt.type()) {
    case UECACK_PAPER:
    case UECNACK_PAPER:
        // bounced, ignore
        pkt.free();
        return;
    case UEC_PAPER:
        // do what comes after the switch
        if (pkt.bounced()) {
            printf("Bounced at Sink, no sense\n");
        }
        break;
    default:
        std::cout << "unknown packet receive with type code: " << pkt.type() << "\n";
        pkt.free();

        return;
    }
    PaperUecPacket *p = dynamic_cast<PaperUecPacket *>(&pkt);
    PaperUecPacket::seq_t seqno = p->seqno();
    PaperUecPacket::seq_t ackno = p->seqno() + p->data_packet_size() - 1;

    simtime_picosec ts = p->timestamp_sent;

    bool marked = p->flags() & ECN_CE;

    // TODO: consider different ways to select paths
    auto crt_path = random() % 128;

    // packet was trimmed
    if (pkt.header_only()) {
        send_nack(1, marked, seqno, ackno, _paths.at(crt_path), pkt.pathid(), false);
        //pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_RCVDESTROY);
        p->free();
        return;
    }

    int size = p->data_packet_size();
    p->free();

    _packets += size;

    if (seqno == _cumulative_ack + 1) { // next expected seq no
        _cumulative_ack = seqno + size - 1;
        seqno = 1;

        // handling packets received OOO
        while (!_received.empty() && _received.front() == _cumulative_ack + 1) {
            _received.pop_front();
            _cumulative_ack += size; // this assumes that all
                                     // packets have the same size
        }
        ackno = _cumulative_ack;
    } else if (seqno < _cumulative_ack + 1) { // already ack'ed
        // this space intentionally left empty
        seqno = 1;
        ackno = _cumulative_ack;
    } else { // not the next expected sequence number
        // TODO: what to do when a future packet is
        // received?
        if (_received.empty()) {
            _received.push_front(seqno);
            _drops += (1000 + seqno - _cumulative_ack - 1) / 1000; // TODO: figure out what is this
                                                                   // calculating exactly
        } else if (seqno > _received.back()) {
            _received.push_back(seqno);
        } else {
            for (auto it = _received.begin(); it != _received.end(); ++it) {
                if (seqno == *it)
                    break; // bad retransmit
                if (seqno < (*it)) {
                    _received.insert(it, seqno);
                    break;
                }
            }
        }
    }

    int32_t path_id = p->pathid();

    send_ack(ts, marked, seqno, ackno, _paths.at(crt_path), NULL,
             path_id);
}

void PaperUecSink::send_ack(simtime_picosec ts, bool marked, PaperUecAck::seq_t seqno, PaperUecAck::seq_t ackno, const Route *rt,
                       const Route *inRoute, int path_id) {

    PaperUecAck *ack = 0;

    switch (_route_strategy) {
    case ECMP_FIB:
    case ECMP_FIB_ECN:
    case REACTIVE_ECN:
    case ECMP_RANDOM2_ECN:
    case ECMP_RANDOM_ECN:
        ack = PaperUecAck::newpkt(_src->_flow, *_route, seqno, ackno, 0, _srcaddr);

        ack->set_pathid(_path_ids[_crt_path]);
        _crt_path++;
        if (_crt_path == simple_numb_paths) {
            _crt_path = 0;
        }

        // set ECN echo only if that is selected strategy
        if (marked) {
            ack->set_flags(ECN_ECHO);
        } else {
            // printf("ACK - NO ECN\n");
            ack->set_flags(0);
        }

        break;
    case NOT_SET:
        abort();
    default:
        break;
    }
    assert(ack);

    ack->timestamp_sent = ts;
    ack->sendOn();
}

const string &PaperUecSink::nodename() { return _nodename; }

uint64_t PaperUecSink::cumulative_ack() { return _cumulative_ack; }

uint32_t PaperUecSink::drops() { return _drops; }

void PaperUecSink::connect(PaperUecSrc &src, const Route *route) {
    _src = &src;
    switch (_route_strategy) {
    case SINGLE_PATH:
    case ECMP_FIB:
    case ECMP_FIB_ECN:
    case REACTIVE_ECN:
    case ECMP_RANDOM2_ECN:
    case ECMP_RANDOM_ECN:
        assert(route);
        //("Setting route\n");
        _route = route;
        break;
    default:
        // do nothing we shouldn't be using this route - call
        // set_paths() to set routing information
        _route = NULL;
        break;
    }
}

void PaperUecSink::set_paths(uint32_t no_of_paths) {
    simple_numb_paths = no_of_paths;
    switch (_route_strategy) {
    case SCATTER_PERMUTE:

    case PULL_BASED:
    case SCATTER_ECMP:
    case SINGLE_PATH:
    case NOT_SET:
        abort();
    case SCATTER_RANDOM:
    case ECMP_FIB:
    case ECMP_FIB_ECN:
    case ECMP_RANDOM2_ECN:
    case REACTIVE_ECN:
        _paths.resize(no_of_paths);
        _path_ids.resize(no_of_paths);
        for (unsigned int i = 0; i < no_of_paths; i++) {
            _paths[i] = NULL;
            _path_ids[i] = i;
        }
        _crt_path = 0;
        // permute_paths();
        break;
    case ECMP_RANDOM_ECN:
        assert(simple_numb_paths == 0);
        _paths.resize(no_of_paths);
        _path_ids.resize(no_of_paths);
        for (unsigned int i = 0; i < no_of_paths; i++) {
            _paths[i] = NULL;
            _path_ids[i] = i;
        }
        _crt_path = 0;
        // permute_paths();
        break;
    default:
        break;
    }
}

/**********************
 * PaperUecRtxTimerScanner *
 **********************/

PaperUecRtxTimerScanner::PaperUecRtxTimerScanner(simtime_picosec scanPeriod, EventList &eventlist)
        : EventSource(eventlist, "RtxScanner"), _scanPeriod{scanPeriod} {
    eventlist.sourceIsPendingRel(*this, 0);
}

void PaperUecRtxTimerScanner::registerUec(PaperUecSrc &uecsrc) { _uecs.push_back(&uecsrc); }

void PaperUecRtxTimerScanner::doNextEvent() {
    simtime_picosec now = eventlist().now();
    uecs_t::iterator i;
    for (i = _uecs.begin(); i != _uecs.end(); i++) {
        (*i)->rtx_timer_hook(now, _scanPeriod);
    }
    eventlist().sourceIsPendingRel(*this, _scanPeriod);
}
