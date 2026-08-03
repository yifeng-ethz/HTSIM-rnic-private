// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include <math.h>
#include <iostream>
#include <algorithm>
#include "dcqcn.h"
#include "queue.h"
#include <stdio.h>
#include "switch.h"
#include "trigger.h"
#include <limits>
#include <stdexcept>
using namespace std;

////////////////////////////////////////////////////////////////
//  DCQCN SOURCE
////////////////////////////////////////////////////////////////

/* keep track of RTOs.  Generally, we shouldn't see RTOs if
   return-to-sender is enabled.  Otherwise we'll see them with very
   large incasts. */
simtime_picosec DCQCNSink::_cnp_interval = timeFromUs(50.0);
simtime_picosec DCQCNSrc::_cc_update_period = timeFromUs(55.0);

double DCQCNSrc::_g = .00390625; // g = 1/256
uint64_t DCQCNSrc::_B = 10000000; // number of bytes to go until we fire the byte counter
linkspeed_bps DCQCNSrc::_minimum_rate_bps = UINT64_C(100000000);

uint32_t DCQCNSrc::_F = 5;
linkspeed_bps DCQCNSrc::_RAI = 0;
linkspeed_bps DCQCNSrc::_RHAI = 0;

DCQCNSrc::DCQCNSrc(RoceLogger* logger, TrafficLogger* pktlogger, EventList &eventlist, linkspeed_bps rate)
    : RoceSrc(logger,pktlogger,eventlist,rate),
      _cc_timer(*this, eventlist)
{
    _cnps_received = 0;

    //linkspeed
    _link = rate;
    //current transmit rate
    _RC = rate;
    //target transmit rate
    _RT = rate;
    _alpha = 1;
    _cc_active = false;

    _RAI = rate / 20;
    _RHAI = rate / 10;

    if (_minimum_rate_bps > rate) {
        throw std::invalid_argument(
            "DCQCN minimum rate exceeds the physical link rate");
    }

    _last_cc_update = 0;
    _last_alpha_update = 0;

    _T = 0;
    _BC = 0;
    _byte_counter = 0;
}

DCQCNSrc::CcTimer::CcTimer(DCQCNSrc& owner, EventList& eventlist)
    : EventSource(eventlist, "DCQCN CC timer"), _owner(owner) {}

DCQCNSrc::CcTimer::~CcTimer() {
    cancel();
}

void DCQCNSrc::CcTimer::arm(simtime_picosec when) {
    if (when < eventlist().now()) {
        throw std::logic_error("DCQCN CC timer is in the past");
    }
    if (_handle.has_value()) {
        if ((*_handle)->first == when) {
            return;
        }
        eventlist().cancelPendingSourceByHandle(*this, *_handle);
        _handle.reset();
    }
    const EventList::Handle handle =
        eventlist().sourceIsPendingGetHandle(*this, when);
    if (handle != EventList::nullHandle()) {
        _handle = handle;
    }
}

void DCQCNSrc::CcTimer::cancel() {
    if (!_handle.has_value()) {
        return;
    }
    eventlist().cancelPendingSourceByHandle(*this, *_handle);
    _handle.reset();
}

void DCQCNSrc::CcTimer::doNextEvent() {
    _handle.reset();
    ++_fire_count;
    _owner.ccTimerExpired();
}

simtime_picosec DCQCNSrc::CcTimer::pendingTime() const {
    if (!_handle.has_value()) {
        throw std::logic_error("DCQCN CC timer is not pending");
    }
    return (*_handle)->first;
}

bool DCQCNSrc::cc_timer_pending() const noexcept {
    return _cc_timer.pending();
}

simtime_picosec DCQCNSrc::cc_timer_time() const {
    return _cc_timer.pendingTime();
}

std::uint64_t DCQCNSrc::cc_timer_fire_count() const noexcept {
    return _cc_timer.fireCount();
}

void DCQCNSrc::processCNP(const CNPPacket& cnp){
    _RT = _RC;
    _RC = std::max<linkspeed_bps>(
        _minimum_rate_bps,
        static_cast<linkspeed_bps>(_RC * (1-_alpha/2)));
    _alpha = (1-_g)*_alpha + _g;

    //_ai_state = increase_state::fast_recovery;

    _T = 0;
    _BC = 0;
    _byte_counter = 0;
    _cc_active = true;

    _pacing_rate = _RC;
    update_spacing();
    if (_flow_started && !_done && _state_send == READY) {
        schedulePacingAt(eventlist().now());
    }
    _last_cc_update = eventlist().now();
    _last_alpha_update = eventlist().now();
    if (_state_observer) {
        _state_observer("cnp-rate-cut");
    }

    if (_cc_update_period == 0) {
        throw std::logic_error("DCQCN CC update period must be positive");
    }
    if (eventlist().now()
        <= std::numeric_limits<simtime_picosec>::max() - _cc_update_period) {
        _cc_timer.arm(eventlist().now() + _cc_update_period);
    }
}

void DCQCNSrc::increaseRate(){
    if (_RC>= _link) {
        //no need to increase, already at line rate!
        return;
    }

    if (max(_T,_BC) <= _F){
        //fast recovery
        _RC = (_RT + _RC) / 2;

    } else if (min(_T,_BC) > _F){
        //hyper increase
        _RT = _RT + (min(_T,_BC)-_F)* _RHAI;
        _RC = (_RT + _RC) / 2;

    } else {
        //active increase 
        _RT += _RAI;
        _RC = (_RT + _RC) / 2;

    }

    if (_RC > _link){
        _RC = _link;
    }
    if (_RC < _minimum_rate_bps) {
        _RC = _minimum_rate_bps;
    }

    _pacing_rate = _RC;
    update_spacing();

}

void DCQCNSrc::send_packet() {
    const std::uint64_t packets_before = _packets_sent;
    RoceSrc::send_packet();
    if (_packets_sent == packets_before || !_cc_active) {
        return;
    }
    if (_packets_sent != packets_before + 1) {
        throw std::logic_error(
            "DCQCN DATA send changed the packet counter by more than one");
    }
    if (_byte_counter > std::numeric_limits<std::uint64_t>::max() - _mss) {
        throw std::overflow_error("DCQCN byte counter overflow");
    }
    _byte_counter += _mss;
    if (_B == 0) {
        throw std::logic_error("DCQCN byte threshold must be positive");
    }
    while (_byte_counter >= _B) {
        _byte_counter -= _B;
        ++_BC;
        ++_byte_counter_rate_updates;
        increaseRate();
        if (_state_observer) {
            _state_observer("byte-recovery");
        }
    }
}

void DCQCNSrc::ccTimerExpired() {
    if (!_cc_active || _done || !_flow_started) {
        return;
    }
    const simtime_picosec now = eventlist().now();
    if (now - _last_alpha_update < _cc_update_period
        || now - _last_cc_update < _cc_update_period) {
        throw std::logic_error("DCQCN CC timer fired before its period");
    }

    _alpha = (1 - _g) * _alpha;
    _last_alpha_update = now;
    _last_cc_update = now;
    ++_T;
    increaseRate();
    if (_state_observer) {
        _state_observer("timer-recovery");
    }
    if (_state_send == READY) {
        schedulePacingAt(now);
    }

    if (now <= std::numeric_limits<simtime_picosec>::max()
                   - _cc_update_period) {
        _cc_timer.arm(now + _cc_update_period);
    }
}

void DCQCNSrc::processAck(const RoceAck& ack) {
    RoceSrc::processAck(ack);
    if (_done) {
        _cc_active = false;
        _cc_timer.cancel();
        if (auto* dcqcn_sink = dynamic_cast<DCQCNSink*>(_sink)) {
            dcqcn_sink->cancelCnpTimer();
        }
    }
}

void DCQCNSrc::receivePacket(Packet& pkt) 
{
    if (!_flow_started){
        assert(pkt.type()==ETH_PAUSE);
        pkt.free();
        return; 
    }

    if (_stop_time && eventlist().now() >= _stop_time) {
        // stop sending new data, but allow us to finish any retransmissions
        _flow_size = _highest_sent+_mss;
        _stop_time = 0;
    }

    if (_done) {
        pkt.free();
        return;
    }

    switch (pkt.type()) {
    case ETH_PAUSE:    
        processPause((const EthPausePacket&)pkt);
        pkt.free();
        return;
    case ROCENACK: 
        _nacks_received++;
        processNack((const RoceNack&)pkt);
        pkt.free();
        return;
    case ROCEACK:
        _acks_received++;
        processAck((const RoceAck&)pkt);
        pkt.free();
        return;
    case CNP:
        _cnps_received++;
        processCNP((const CNPPacket&)pkt);
        pkt.free();
        return;

    default:
        abort();
    }
}

////////////////////////////////////////////////////////////////
//  DCQCN SINK
////////////////////////////////////////////////////////////////

/* Only use this constructor when there is only one for to this receiver */
DCQCNSink::DCQCNSink(EventList &eventlist)
    : RoceSink(), EventSource(eventlist, "DCQCN Sink")
{
    _last_cnp_sent_time = UINT64_MAX;
    _marked_packets_since_last_cnp = 0;
    _packets_since_last_cnp = 0;
}

DCQCNSink::~DCQCNSink() {
    cancelCnpTimer();
}

void DCQCNSink::armCnpTimer(simtime_picosec when) {
    if (when < eventlist().now()) {
        throw std::logic_error("DCQCN CNP timer is in the past");
    }
    if (_cnp_timer.has_value()) {
        if ((*_cnp_timer)->first == when) {
            return;
        }
        eventlist().cancelPendingSourceByHandle(*this, *_cnp_timer);
        _cnp_timer.reset();
    }
    const EventList::Handle handle =
        eventlist().sourceIsPendingGetHandle(*this, when);
    if (handle != EventList::nullHandle()) {
        _cnp_timer = handle;
    }
}

void DCQCNSink::cancelCnpTimer() {
    if (!_cnp_timer.has_value()) {
        return;
    }
    eventlist().cancelPendingSourceByHandle(*this, *_cnp_timer);
    _cnp_timer.reset();
}

// Receive a packet.
// Note: _cumulative_ack is the last byte we've ACKed.
// seqno is the first byte of the new packet.
void DCQCNSink::receivePacket(Packet& pkt) {
    bool ecn_marked = ((pkt.flags() & ECN_CE) != 0);
    RoceSink::receivePacket(pkt);

    if (ecn_marked){
        //generate CNPs here.
        if (_last_cnp_sent_time == UINT64_MAX || eventlist().now() - _last_cnp_sent_time >= _cnp_interval){
            send_cnp();
            armCnpTimer(eventlist().now() + _cnp_interval);
        }               
        else {
            _marked_packets_since_last_cnp++;
        }
    }
    _packets_since_last_cnp++;
}

void DCQCNSink::doNextEvent(){
    _cnp_timer.reset();
    if (eventlist().now() - _last_cnp_sent_time >= _cnp_interval && _marked_packets_since_last_cnp >0){
        send_cnp();
        armCnpTimer(eventlist().now() + _cnp_interval);
    }
}

void DCQCNSink::send_cnp() {
    CNPPacket *cnp = 0;
    cnp = CNPPacket::newpkt(_src->_flow, *_route, _cumulative_ack,_srcaddr);
    cnp->set_pathid(0);

    cnp->sendOn();

    _last_cnp_sent_time = eventlist().now();
    _packets_since_last_cnp = 0;
    _marked_packets_since_last_cnp = 0;
}
