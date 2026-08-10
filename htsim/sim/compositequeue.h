// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        
#ifndef COMPOSITE_QUEUE_H
#define COMPOSITE_QUEUE_H

/*
 * A composite queue that transforms packets into headers when there is no space and services headers with priority. 
 */

#define QUEUE_INVALID 0
#define QUEUE_LOW 1
#define QUEUE_HIGH 2


#include <list>
#include "queue.h"
#include "config.h"
#include "eventlist.h"
#include "network.h"
#include "loggertypes.h"

class CompositeQueue : public Queue {
 public:
    CompositeQueue(linkspeed_bps bitrate, mem_b maxsize, 
                   EventList &eventlist, QueueLogger* logger, 
                   uint16_t trim_size, bool disable_trim=false);
    virtual void receivePacket(Packet& pkt);
    virtual void doNextEvent();
    // should really be private, but loggers want to see
    mem_b _queuesize_low,_queuesize_high;
    int num_headers() const { return _num_headers;}
    int num_packets() const { return _num_packets;}
    int num_stripped() const { return _num_stripped;}
    int num_bounced() const { return _num_bounced;}
    int num_acks() const { return _num_acks;}
    int num_nacks() const { return _num_nacks;}
    int num_pulls() const { return _num_pulls;}
    mem_b queuesize_high_watermark() const { return _queuesize_high_watermark;}
    virtual mem_b queuesize() const;
    virtual void setName(const string& name) {
        Logged::setName(name); 
        _nodename += name;
    }

    void setRTS(bool return_to_sender){ _return_to_sender = return_to_sender;}

    virtual const string& nodename() { return _nodename; }
    void set_ecn_threshold(mem_b ecn_thresh) {
        _ecn_minthresh = ecn_thresh;
        _ecn_maxthresh = ecn_thresh;
    }
    void set_ecn_thresholds(mem_b min_thresh, mem_b max_thresh) {
        _ecn_minthresh = min_thresh;
        _ecn_maxthresh = max_thresh;
        if (_queue_id == 2)
            cout << "queue_id " << _queue_id << " ecn_low " << _ecn_minthresh << " ecn_high " << _ecn_maxthresh << endl;
    }

    void set_fail_rate(int fail_rate) {
        _is_failing = true;
        _fail_rate = fail_rate;
    }

    // Paper-algorithm compatibility knobs.  Defaults reproduce the fork's
    // current behavior exactly; only the paper main (htsim_uec_paper) changes
    // them, to the constants hardcoded in the ATLAHS artifact's queue:
    //   header bound at the trim site:      2  -> 20000  (x _maxsize)
    //   header bound at the arrival site:   2  -> 200000 (x _maxsize)
    //   ECN mark on header dequeue:      false -> true
    //   high/low service ratio:        100000  -> 10000
    static void set_header_bound_factors(mem_b trim_site, mem_b arrival_site) {
        _header_bound_factor_trim = trim_site;
        _header_bound_factor_arrival = arrival_site;
    }
    static void set_ecn_on_deque_headers(bool value) {
        _ecn_on_deque_headers = value;
    }
    static void set_high_prio_ratio(int value) { _ratio_high_default = value; }
    // The artifact stamps timestamp_sent on first queue arrival when the
    // sender left it zero (which happens for every packet sent at t=0); the
    // paper transport's RTT measurement depends on it.  Default off.
    static void set_stamp_ts_on_arrival(bool value) {
        _stamp_ts_on_arrival = value;
    }

    int _num_packets;
    int _num_headers; // only includes data packets stripped to headers, not acks or nacks
    int _num_acks;
    int _num_nacks;
    int _num_pulls;
    int _num_stripped; // count of packets we stripped
    int _num_bounced;  // count of packets we bounced
    mem_b _queuesize_high_watermark; // max occupancy of high priority queue

 protected:
    // Mechanism
    void beginService(); // start serving the item at the head of the queue
    void completeService(); // wrap up serving the item at the head of the queue
    bool decide_ECN();

    bool _disable_trim;

    int _serv;
    int _ratio_high, _ratio_low, _crt;
    // below minthresh, 0% marking, between minthresh and maxthresh
    // increasing random mark propbability, abve maxthresh, 100%
    // marking.
    mem_b _ecn_minthresh; 
    mem_b _ecn_maxthresh;

    uint16_t _trim_size;

    bool _return_to_sender;

    int _queue_id;
    CircularBuffer<Packet*> _enqueued_low;
    CircularBuffer<Packet*> _enqueued_high;

    bool _is_failing;
    int _fail_rate;
    int _packet_count;

    static mem_b _header_bound_factor_trim;
    static mem_b _header_bound_factor_arrival;
    static bool _ecn_on_deque_headers;
    static int _ratio_high_default;
    static bool _stamp_ts_on_arrival;
};

#endif
