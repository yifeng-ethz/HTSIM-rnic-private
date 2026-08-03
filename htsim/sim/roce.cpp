// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include <math.h>
#include <iostream>
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include "roce.h"
#include "queue.h"
#include <stdio.h>
#include "switch.h"
#include "trigger.h"
using namespace std;

////////////////////////////////////////////////////////////////
//  ROCE SOURCE
////////////////////////////////////////////////////////////////

/* When you're debugging, sometimes it's useful to enable debugging on
   a single ROCE receiver, rather than on all of them.  Set this to the
   node ID and recompile if you need this; otherwise leave it
   alone. */
//#define LOGSINK 2332
#define LOGSINK   0 
bool RoceSink::ooo_enabled = false;

/* keep track of RTOs.  Generally, we shouldn't see RTOs if
   return-to-sender is enabled.  Otherwise we'll see them with very
   large incasts. */
uint32_t RoceSrc::_global_node_count = 0;
uint32_t RoceSrc::_global_rto_count = 0;

/* _min_rto can be tuned using SetMinRTO. Don't change it here.  */
simtime_picosec RoceSrc::_min_rto = timeFromUs((uint32_t)DEFAULT_RTO_MIN);

RoceSrc::RoceSrc(RoceLogger* logger, TrafficLogger* pktlogger, EventList &eventlist, linkspeed_bps rate)
    : BaseQueue(rate,eventlist,NULL), _flow(pktlogger), _logger(logger)
{
    _mss = Packet::data_packet_size();
    _end_trigger = NULL;

    _stop_time = 0;
    _flow_started = false;
    _base_rtt = timeInf;
    _acked_packets = 0;
    _packets_sent = 0;
    _new_packets_sent = 0;
    _rtx_packets_sent = 0;
    _acks_received = 0;
    _nacks_received = 0;

    _highest_sent = 0;
    _last_acked = 0;
    _dstaddr = UINT32_MAX;

    _sink = 0;
    _done = false;

    _rtt = 0;
    _rto = timeFromMs(20);
    _mdev = 0;
    _drops = 0;
    _flow_size = ((uint64_t)1)<<63;
  
    _node_num = _global_node_count++;
    _nodename = "rocesrc " + to_string(_node_num);

    //srand(time(NULL));
    _pathid = random()%256;

    //cout << _nodename << " path id is " << _pathid << endl;

    // debugging hack
    _log_me = false;
    //if (get_id() == 144212)
    //    _log_me = true;

    _state_send = READY;
    _time_last_sent = 0;
    _has_sent_packet = false;
    _highest_new_sequence_sent = 0;
    
    _pacing_rate = rate;
    
    update_spacing();
}

RoceSrc::~RoceSrc() {
    cancelPacing();
}

void RoceSrc::schedulePacingAt(simtime_picosec when) {
    if (when < eventlist().now()) {
        throw std::logic_error("RoCE pacing event is in the past");
    }
    if (_pacing_event.has_value()) {
        if ((*_pacing_event)->first <= when) {
            return;
        }
        eventlist().cancelPendingSourceByHandle(*this, *_pacing_event);
        _pacing_event.reset();
    }
    const EventList::Handle handle =
        eventlist().sourceIsPendingGetHandle(*this, when);
    if (handle != EventList::nullHandle()) {
        _pacing_event = handle;
    }
}

void RoceSrc::cancelPacing() {
    if (!_pacing_event.has_value()) {
        return;
    }
    eventlist().cancelPendingSourceByHandle(*this, *_pacing_event);
    _pacing_event.reset();
}

simtime_picosec RoceSrc::pacing_event_time() const {
    if (!_pacing_event.has_value()) {
        throw std::logic_error("RoCE source has no pending pacing event");
    }
    return (*_pacing_event)->first;
}

void RoceSrc::update_spacing() {
    if (_pacing_rate == 0) {
        throw std::logic_error("RoCE pacing rate must be positive");
    }
    constexpr unsigned __int128 kPicosecondsPerSecond =
        static_cast<unsigned __int128>(UINT64_C(1000000000000));
    const unsigned __int128 wire_bytes =
        static_cast<unsigned __int128>(Packet::data_packet_size())
        + static_cast<unsigned __int128>(RocePacket::ACKSIZE);
    const unsigned __int128 numerator =
        wire_bytes * 8 * kPicosecondsPerSecond;
    const unsigned __int128 interval =
        (numerator + _pacing_rate - 1) / _pacing_rate;
    if (interval == 0
        || interval > std::numeric_limits<simtime_picosec>::max()) {
        throw std::overflow_error("RoCE pacing interval is not representable");
    }
    _packet_spacing = static_cast<simtime_picosec>(interval);
}

void RoceSrc::setRate(linkspeed_bps rate) {
    if (rate == 0) {
        throw std::invalid_argument("RoCE rate must be positive");
    }
    _bitrate = rate;
    _pacing_rate = rate;
    update_spacing();
    schedulePacingAt(eventlist().now());
}

/*mem_b RoceSrc::queuesize(){
  return 0;
  }

  mem_b RoceSrc::maxsize(){
  return 0;
  }*/

void RoceSrc::set_traffic_logger(TrafficLogger* pktlogger) {
    _flow.set_logger(pktlogger);
}

void RoceSrc::log_me() {
    // avoid looping
    if (_log_me == true)
        return;

    cout << "Enabling logging on RoceSrc " << _nodename << endl;
    _log_me = true;
    if (_sink)
        _sink->log_me();
}

void RoceSrc::startflow(){
    _flow_started = true;
    _highest_sent = 0;
    _last_acked = 0;
    
    _acked_packets = 0;
    _packets_sent = 0;
    _new_packets_sent = 0;
    _rtx_packets_sent = 0;
    _done = false;
    _time_last_sent = 0;
    _has_sent_packet = false;
    _highest_new_sequence_sent = 0;
    
    schedulePacingAt(eventlist().now());
}

void RoceSrc::set_end_trigger(Trigger& end_trigger) {
    _end_trigger = &end_trigger;
}

void RoceSrc::connect(Route* routeout, Route* routeback, RoceSink& sink, simtime_picosec starttime) {
    assert(routeout);
    _route = routeout;
    
    _sink = &sink;
    _flow.set_id(get_id()); // identify the packet flow with the ROCE source that generated it
    _flow._name = _name;
    _sink->connect(*this, routeback);

    if (starttime != TRIGGER_START) {
        //cout << "scheduling start at " << starttime << " now is " << timeAsUs(eventlist().now())<< endl;
        schedulePacingAt(timeFromUs((double)starttime));
        //startflow();
    }
    else if (_log_me) cout << "TRIGGER START " << _nodename << endl;
}

/* Process a NACK.  Generally this involves queuing the NACKed packet
   for retransmission, but then waiting for a PULL to actually resend
   it.  However, sometimes the NACK has the PULL bit set, and then we
   resend immediately */
void RoceSrc::processNack(const RoceNack& nack){
    _last_acked = nack.ackno();

    if (_log_me)
        cout << "Src " << get_id() << " go back n from " <<  _highest_sent << " to " << _last_acked << " at " << timeAsUs(eventlist().now()) << " us" << endl;

    _highest_sent = _last_acked;
    // A finite flow stops scheduling pacing events after its last packet.
    // GBN must explicitly restart the sender when a NACK reopens that tail.
    if (!_done) {
        schedulePacingAt(eventlist().now());
    }
}

/* Process an ACK.  Mostly just housekeeping*/
void RoceSrc::processAck(const RoceAck& ack) {
    RoceAck::seq_t ackno = ack.ackno();
    simtime_picosec ts = ack.ts();

    // Compute rtt.  This comes originally from TCP, and may not be optimal for ROCE */
    uint64_t m = eventlist().now()-ts;

    if (m!=0){
        if (_rtt>0){
            uint64_t abs;
            if (m>_rtt)
                abs = m - _rtt;
            else
                abs = _rtt - m;

            _mdev = 3 * _mdev / 4 + abs/4;
            _rtt = 7*_rtt/8 + m/8;

            _rto = _rtt + 4*_mdev;
        } else {
            _rtt = m;
            _mdev = m/2;
            _rto = _rtt + 4*_mdev;
        }
        if (_base_rtt==timeInf || _base_rtt > m)
            _base_rtt = m;
    }

    if (_rto < _min_rto)
        _rto = _min_rto * ((drand() * 0.5) + 0.75);

    if (ackno > _last_acked) { // a brand new ack    
        // we should probably cancel the rtx timer for any acked by
        // the cumulative ack, but we'll get an ACK or NACK anyway in
        // due course.
        _last_acked = ackno;
        _acked_packets = ackno;
    }
    if (_logger) _logger->logRoce(*this, RoceLogger::ROCE_RCV);

    if (_log_me)
        cout << "Src " << get_id() << " ackno " << ackno << endl;

    if (ackno * _mss >= _flow_size){
        _done = true;
        cancelPacing();
        if (_end_trigger) {
            _end_trigger->activate();
        }

        return;
    }
}

void RoceSrc::processPause(const EthPausePacket& p) {
    if (p.sleepTime()>0){
        //remote end is telling us to shut up.
        //cout << "Source " << str() << " PAUSE " << timeAsUs(eventlist().now()) << endl;
        //assert(_state_send != PAUSED);
        _state_send = PAUSED;
        cancelPacing();
    } else {
        //we are allowed to send!
        //assert(_state_send != READY);
        _state_send = READY;
        schedulePacingAt(eventlist().now());
    }
}

void RoceSrc::receivePacket(Packet& pkt) 
{
    if (!_flow_started){
        assert(pkt.type()==ETH_PAUSE);
        pkt.free();
        return; 
    }

    if (_stop_time && eventlist().now() >= _stop_time) {
        // stop sending new data, but allow us to finish any retransmissions
        _flow_size = _highest_sent * _mss + _mss;
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
    default:
        abort();
    }
}

// Note: the data sequence number is the number of Byte1 of the packet, not the last byte.
void RoceSrc::send_packet() {
    RocePacket* p = NULL;
    bool last_packet = false;
    if (_log_me)
        cout << "Src " << get_id() << " send_packet\n";
    assert(_flow_started);

    if (_flow_size && (_last_acked * _mss >= _flow_size || _highest_sent * _mss  > _flow_size)) {
        //flow is finished or I've already sent all packets, waiting for ACK.
        if (_log_me)
            cout << "Src " << get_id() << " flow is finished, not sending\n";
        return;
    }

    if (_flow_size && (_highest_sent + 1) * _mss >= _flow_size) {
        last_packet = true;
        if (_log_me) {
            cout << _name << " " << get_id() << " sending last packet with SEQNO " << _highest_sent+1 << " at " << timeAsUs(eventlist().now()) << endl;
        }
    }

    uint16_t payload_bytes = _mss;
    if (_flow_size) {
        const uint64_t payload_sent = _highest_sent * _mss;
        const uint64_t remaining = _flow_size - payload_sent;
        payload_bytes = static_cast<uint16_t>(std::min<uint64_t>(
            remaining, static_cast<uint64_t>(_mss)));
    }
    const uint64_t sequence = _highest_sent + 1;
    const bool retransmission = sequence <= _highest_new_sequence_sent;
    p = RocePacket::newpkt(_flow, *_route, sequence,
                           payload_bytes, false, last_packet, _dstaddr);
    
    assert(p);
    p->set_pathid(_pathid);

    p->flow().logTraffic(*p,*this,TrafficLogger::PKT_CREATESEND);
    p->set_ts(eventlist().now());
    
    if (_log_me) {
        cout << "Src " << get_id() << " sent " << _highest_sent+1 << " Flow Size: " << _flow_size << endl;
    }
    _highest_sent ++;
    _packets_sent++;
    if (retransmission) {
        ++_rtx_packets_sent;
    } else {
        ++_new_packets_sent;
        _highest_new_sequence_sent = sequence;
    }

    //cout << "Sent " << _highest_sent+1 << " Flow Size: " << _flow_size << " Flow " << _name << " time " << timeAsUs(eventlist().now()) << endl;

    p->sendOn();
}

void RoceSrc::doNextEvent() {
    // EventList removed this exact handle before invoking the source.
    _pacing_event.reset();
    if (!_flow_started){
      startflow();
      return;
    }

    assert(_flow_started);
    if (_done) {
        return;
    }
    if (_log_me) 
        cout << "Src " << get_id() << " do next event\n";
        

    if (_state_send==PAUSED) {
        if (_log_me) 
            cout << "Src " << get_id() << " paused\n";

        return;
    }

    if (_flow_size && _highest_sent * _mss >= _flow_size) {
        if (_log_me) 
            cout << "Src " << get_id()  << " stopping send coz highest_sent is " << _highest_sent << endl;
        return;
    }

    const simtime_picosec now = eventlist().now();
    auto next_eligible_time = [this]() {
        if (!_has_sent_packet) {
            return eventlist().now();
        }
        if (_time_last_sent
            > std::numeric_limits<simtime_picosec>::max()
                  - _packet_spacing) {
            throw std::overflow_error("RoCE pacing deadline overflow");
        }
        return _time_last_sent + _packet_spacing;
    };

    if (!_has_sent_packet || now >= next_eligible_time()) {
        const std::uint64_t packets_before = _packets_sent;
        send_packet();
        if (_packets_sent == packets_before) {
            // No packet was eligible (normally the finite tail is waiting for
            // its cumulative ACK).  A wakeup loop cannot make progress here.
            return;
        }
        _time_last_sent = now;
        _has_sent_packet = true;
    }

    if (_done || _state_send == PAUSED
        || (_flow_size && _highest_sent * _mss >= _flow_size)) {
        return;
    }

    const simtime_picosec next_send = next_eligible_time();
    if (next_send <= now) {
        std::ostringstream error;
        error << "RoCE pacing failed to advance: now=" << now
              << " last_send=" << _time_last_sent
              << " spacing=" << _packet_spacing
              << " rate=" << _pacing_rate;
        throw std::logic_error(error.str());
    }

    schedulePacingAt(next_send);
}

////////////////////////////////////////////////////////////////
//  ROCE SINK
////////////////////////////////////////////////////////////////

/* Only use this constructor when there is only one for to this receiver */
RoceSink::RoceSink()
    : DataReceiver("roce_sink"), _cumulative_ack(0), _epsn_rx_bitmap(0), _total_received(0)
{
    _src = 0;
    
    _nodename = "rocesink";
    _highest_seqno = 0;
    _log_me = false;
    //if (get_id() == 144214)
    //    _log_me = true;
    _nack_sent = false;
    _out_of_order_count = 0;
}

void RoceSink::log_me() {
    // avoid looping
    if (_log_me == true)
        return;

    _log_me = true;

    if (_src)
        _src->log_me();  
}

/* Connect a src to this sink. */ 
void RoceSink::connect(RoceSrc& src, Route* route)
{
    _src = &src;
    _route = route;
    _cumulative_ack = 0;
    _drops = 0;
}


// Receive a packet.
// Note: _cumulative_ack is the last byte we've ACKed.
// seqno is the first byte of the new packet.
void RoceSink::receivePacket(Packet& pkt) {
    /*
      if (random()%10==0){
      pkt.free();
      return;
      }*/

    assert(pkt.dst () == _src->_dstaddr);

    switch (pkt.type()) {
    case ROCE:
        break;
    default:
        abort();
    }

    RocePacket *p = (RocePacket*)(&pkt);
    RocePacket::seq_t seqno = p->seqno();
    if (_log_me) {
        cout << "Sink " << get_id() << " recv'd " << seqno << endl;
    }
    simtime_picosec ts = p->ts();
    //bool last_packet = ((RocePacket*)&pkt)->last_packet();

    if (seqno > _cumulative_ack+1){
        if (ooo_enabled && seqno - _cumulative_ack <= roceMaxReorder){
            //store packet in OOO buffer.
            _epsn_rx_bitmap[seqno] = 1;
            _out_of_order_count++;

            cout << this << " ooo count+ " << _out_of_order_count << endl;

            pkt.flow().logTraffic(pkt,*this,TrafficLogger::PKT_RCVDESTROY);
            p->free();
            return;
        }

        if (!_nack_sent){
            send_nack(ts,_cumulative_ack);  
            _nack_sent = true;
            if (_log_me) {
                cout << "Wrong seqno received at Roce SINK " << seqno
                     << " expecting " << _cumulative_ack << endl;
            }
        }
        pkt.flow().logTraffic(pkt,*this,TrafficLogger::PKT_RCVDESTROY);

        p->free();
        return;
    }

    if (seqno == _cumulative_ack+1) { // it's the next expected seq no
        _cumulative_ack = seqno;

        if (ooo_enabled){
            assert(_epsn_rx_bitmap[seqno] == 0);

            while (_epsn_rx_bitmap[_cumulative_ack + 1]) {
                // clean OOO state, this will wrap at some point.
                _cumulative_ack ++;
                _epsn_rx_bitmap[_cumulative_ack] = 0;
                _out_of_order_count--;
                cout << this << " ooo count- " << _out_of_order_count << endl;
            }
        }
        if (_nack_sent) 
            _nack_sent = false;

        send_ack(ts);
    } else if (seqno < _cumulative_ack+1) {
        // A duplicate may be the sender's silent-tail RTO recovery after the
        // final ACK was lost.  Re-ACK the current cumulative edge.
        send_ack(ts);
    }
    // have we seen everything yet?
    pkt.flow().logTraffic(pkt,*this,TrafficLogger::PKT_RCVDESTROY);
    pkt.free();
}

void RoceSink::send_ack(simtime_picosec ts) {
    RoceAck *ack = 0;
    ack = RoceAck::newpkt(_src->_flow, *_route, _cumulative_ack,_srcaddr);
    if (_log_me)
        cout << "Sink " << get_id() << " sending ack " << _cumulative_ack << endl;
    ack->set_pathid(0);
    ack->set_ts(ts);
    ack->sendOn();
}

void RoceSink::send_nack(simtime_picosec ts, RocePacket::seq_t ackno) {
    RoceNack *nack = NULL;
    nack = RoceNack::newpkt(_src->_flow, *_route, ackno,_srcaddr);
    if (_log_me)
        cout << "Sink " << get_id() << " sending nack " << ackno << endl;

    nack->set_pathid(0);
    assert(nack);
    nack->flow().logTraffic(*nack,*this,TrafficLogger::PKT_CREATE);
    nack->set_ts(ts);
    nack->sendOn();
}
