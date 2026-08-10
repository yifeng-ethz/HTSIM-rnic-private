// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 

#ifndef PAPER_NDP_H
#define PAPER_NDP_H

/*
 * A NDP source and sink
 */

#include <list>
#include <map>
#include "config.h"
#include "network.h"
#include "ndp.h" // fork keeps the RouteStrategy enum here
#include "paper_ndppacket.h"
#include "priopullqueue.h"
#include "trigger.h"
#include "eventlist.h"
#include "atlahs_event.h"
#include "atlahs_htsim_api.h"

#define timeInf 0
#define NDP_PACKET_SCATTER

//#define LOAD_BALANCED_SCATTER

//min RTO bound in us
// *** don't change this default - override it by calling PaperNdpSrc::setMinRTO()
#define DEFAULT_RTO_MIN 5000

#define RECORD_PATH_LENS // used for debugging which paths lengths packets were trimmed on - mostly useful for BCube

#define DEBUG_PATH_STATS
//enum RouteStrategy {NOT_SET, SINGLE_PATH, SCATTER_PERMUTE, SCATTER_RANDOM, PULL_BASED, SCATTER_ECMP, ECMP_FIB, ECMP_FIB_ECN, REACTIVE_ECN};

class PaperNdpSink;
class PaperNdpRTSPacer;
class Switch;
class ReorderBufferLogger;

class PaperReceiptEvent {
  public:
    PaperReceiptEvent()
        : _path_id(-1), _is_header(false) {};
    PaperReceiptEvent(uint32_t path_id, bool is_header)
        : _path_id(path_id), _is_header(is_header) {}
    inline int32_t path_id() const {return _path_id;}
    inline bool is_header() const {return _is_header;}
    int32_t _path_id;
    bool _is_header;
};

class PaperNdpSrc : public PacketSink, public EventSource, public TriggerTarget {
    friend class PaperNdpSink;
 public:
    PaperNdpSrc(NdpLogger* logger, TrafficLogger* pktlogger, EventList &eventlist, bool rts = false, PaperNdpRTSPacer* pacer = NULL);
    virtual void connect(Route* routeout, Route* routeback, PaperNdpSink& sink, simtime_picosec startTime);

    AtlahsHtsimApi *_atlahs_api = nullptr;
    uint64_t _flow_start_time;
    graph_node_properties *lgs_node; // used for logging purposes
    int simple_numb_paths = 0;


    void set_dst(uint32_t dst) {_dstaddr = dst;}
    void set_traffic_logger(TrafficLogger* pktlogger);
    void startflow();
    void setCwnd(uint32_t cwnd) {_cwnd = cwnd;}
    static void setMinRTO(uint32_t min_rto_in_us) {_min_rto = timeFromUs((uint32_t)min_rto_in_us);}
    static void setRouteStrategy(RouteStrategy strat) {_route_strategy = strat;}
    static void setPathEntropySize(uint32_t path_entropy_size) {_path_entropy_size = path_entropy_size;}
    void set_flowsize(uint64_t flow_size_in_bytes) {
            _flow_size = flow_size_in_bytes;
    }

    void set_stoptime(simtime_picosec stop_time) {
        _stop_time = stop_time;
        cout << "Setting stop time to " << timeAsSec(_stop_time) << endl;
    }

    // called from a trigger to start the flow.
    virtual void activate() {
        startflow();
    }

    void set_end_trigger(Trigger& trigger);

    virtual void doNextEvent();
    virtual void receivePacket(Packet& pkt);

    virtual void processRTS(PaperNdpPacket& pkt);
    virtual void processAck(const PaperNdpAck& ack);
    virtual void processNack(const PaperNdpNack& nack);

    void replace_route(Route* newroute);

    virtual void rtx_timer_hook(simtime_picosec now,simtime_picosec period);
    
    //used by all routing strategies except SINGLE and ECMP_FIB
    void set_paths(vector<const Route*>* rt);

    //used by ECMP_FIB strategy
    void set_paths(uint32_t path_count);


    // should really be private, but loggers want to see:
    uint64_t _highest_sent;  //seqno is in bytes
    uint64_t _packets_sent;
    uint64_t _last_acked;
    uint32_t _new_packets_sent;  // all the below reduced to 32 bits to save RAM
    uint32_t _rtx_packets_sent;
    uint32_t _acks_received;
    uint32_t _nacks_received;
    uint32_t _pulls_received;
    uint32_t _implicit_pulls;
    uint32_t _bounces_received;
    uint32_t _cwnd;
    uint32_t _flight_size;
    uint32_t _acked_packets;

    // the following are used with SCATTER_PERMUTE, SCATTER_RANDOM and
    // PULL_BASED route strategies
    uint16_t _crt_path;
    uint16_t _crt_direction;
    uint16_t _same_path_burst; // how many packets in a row to use same ECMP value (default is 1)
    void set_path_burst(uint16_t path_burst) {_same_path_burst = path_burst;}
    
    vector<int> _path_ids;

    uint32_t _dstaddr;
    vector<const Route*> _paths;
    vector<const Route*> _original_paths; //paths in original permutation order
#ifdef DEBUG_PATH_STATS
    vector<int> _path_counts_new; // only used for debugging, can remove later.
    vector<int> _path_counts_rtx; // only used for debugging, can remove later.
    vector<int> _path_counts_rto; // only used for debugging, can remove later.
#endif
    vector <int16_t> _path_acks; //keeps path scores
    vector <int16_t> _path_ecns; //keeps path scores
    vector <int16_t> _path_nacks; //keeps path scores
    vector <int16_t> _avoid_ratio; //keeps path scores
    vector <int16_t> _avoid_score; //keeps path scores
    vector <bool> _bad_path; //keeps path scores

    map<PaperNdpPacket::seq_t, simtime_picosec> _sent_times;
    map<PaperNdpPacket::seq_t, simtime_picosec> _first_sent_times;

    void print_stats();

    int _pull_window; // Used to keep track of expected pulls so we
                      // can handle return-to-sender cleanly.
                      // Increase by one for each Ack/Nack received.
                      // Decrease by one for each Pull received.
                      // Indicates how many pulls we expect to
                      // receive, if all currently sent but not yet
                      // acked/nacked packets are lost
                      // or are returned to sender.
    int _first_window_count;

    //round trip time estimate, needed for coupled congestion control
    simtime_picosec _rtt, _rto, _mdev,_base_rtt;

    uint16_t _mss;
 
    uint32_t _drops;

    PaperNdpSink* _sink;
 
    simtime_picosec _rtx_timeout;
    bool _rtx_timeout_pending;
    const Route* _route;

    int choose_route();
    int next_route();

    void pull_packets(PaperNdpPull::seq_t pull_no, PaperNdpPull::seq_t pacer_no);
    int send_packet(PaperNdpPull::seq_t pacer_no); // returns number of packets actually sent

    virtual const string& nodename() { return _nodename; }
    inline void set_flowid(flowid_t flow_id) { _flow.set_flowid(flow_id);}
    inline flowid_t flow_id() const { return _flow.flow_id();}
 
    //debugging hack
    void log_me();
    bool _log_me;

    static uint32_t _global_rto_count;  // keep track of the total number of timeouts across all srcs
    static simtime_picosec _min_rto;
    static RouteStrategy _route_strategy;
    static uint32_t _path_entropy_size; // now many paths do we include in our path set
    static int _global_node_count;
    static int _rtt_hist[10000000];
    int _node_num;

 private:
    // Housekeeping
    NdpLogger* _logger;
    TrafficLogger* _pktlogger;
    Trigger* _end_trigger;

    // Connectivity
    PacketFlow _flow;
    string _nodename;

    bool _rts;
    PaperNdpRTSPacer* _rts_pacer;

    enum  FeedbackType {ACK, ECN, NACK, BOUNCE, UNKNOWN};
    static const int HIST_LEN=12;
    FeedbackType _feedback_history[HIST_LEN];
    int _feedback_count;

    // Mechanism
    void clear_timer(uint64_t start,uint64_t end);
    void retransmit_packet();
    void permute_paths();
    void update_rtx_time();
    void process_cumulative_ack(PaperNdpPacket::seq_t cum_ackno);
    inline void count_ack(int32_t path_id) {count_feedback(path_id, ACK);}
    inline void count_nack(int32_t path_id) {count_feedback(path_id, NACK);}
    inline void count_bounce(int32_t path_id) {count_feedback(path_id, BOUNCE);}
    void count_ecn(int32_t path_id);
    void count_feedback(int32_t path_id, FeedbackType fb);
    bool is_bad_path();
    void log_rtt(simtime_picosec sent_time);
    PaperNdpPull::seq_t _last_pull;
    PaperNdpPull::seq_t _max_pull;
    uint64_t _flow_size;  //The flow size in bytes.  Stop sending after this amount.
    simtime_picosec _stop_time;
    map <PaperNdpPacket::seq_t, PaperNdpPacket*> _rtx_queue; //Packets queued for (hopefuly) imminent retransmission
};

class PaperNdpPullPacer;
class PaperNdpRTSPacer;

class PaperNdpSink : public PacketSink, public DataReceiver {
    friend class PaperNdpSrc;
 public:
    PaperNdpSink(EventList& ev, linkspeed_bps linkspeed, double pull_rate_modifier);
    PaperNdpSink(PaperNdpPullPacer* pacer);

    void add_buffer_logger(ReorderBufferLogger *logger) {
            _buffer_logger = logger;
    } 

    int simple_numb_paths = 0;
    int32_t from_sink = -1;
    int32_t to_sink = -1;
    int32_t tag_sink = 0;
 
    void receivePacket(Packet& pkt);
    void process_request_to_send(PaperNdpRTS* rts);

    void receiver_core_trim(PaperNdpPacket* p);
    void receiver_ecn_accounting(PaperNdpPacket* p);
    void receiver_increase(PaperNdpPacket* p);

    PaperNdpAck::seq_t _cumulative_ack; // the packet we have cumulatively acked
    uint32_t _drops;
    uint64_t cumulative_ack() { return _cumulative_ack + _received.size()*9000;}
    uint64_t total_received() const { return _total_received;}
    uint32_t drops(){ return _src->_drops;}
    virtual const string& nodename() { return _nodename; }
    void increase_window() {_pull_no++;} 
    static void setRouteStrategy(RouteStrategy strat) {_route_strategy = strat;}

    void set_src(uint32_t s) {_srcaddr = s;}
    void set_end_trigger(Trigger& trigger);

    list<PaperNdpAck::seq_t> _received; // list of packets above a hole, that we've received
 
    PaperNdpSrc* _src;

    //debugging hack
    void log_me();
    bool _log_me;

    uint32_t _srcaddr;
    
    //needed by all strategies except SINGLE and ECMP_FIB
    void set_paths(vector<const Route*>* rt);
    void set_paths(uint32_t no_of_paths);


    

#ifdef RECORD_PATH_LENS
#define MAX_PATH_LEN 20u
    vector<uint32_t> _path_lens;
    vector<uint32_t> _trimmed_path_lens;
#endif
    static RouteStrategy _route_strategy;

    uint64_t reorder_buffer_size() {return _received.size();};
    uint64_t reorder_buffer_max() {return _ooo;};

    void set_priority(int priority) {_priority = priority;}
    inline int priority() const {return _priority;}
    static bool _oversubscribed_congestion_control;
    static double _g;
 private:
 
    // Connectivity
    void connect(PaperNdpSrc& src, Route* route);

    inline uint32_t flow_id() const {
            return _src->flow_id();
    };

    // the following are used with SCATTER_PERMUTE, SCATTER_RANDOM,
    // and PULL_BASED route strategies
    uint16_t _crt_path; // index into paths
    uint16_t _crt_direction;
    vector<int> _path_ids; // path IDs to be used for ECMP FIB. 
    vector<const Route*> _paths; //paths in current permutation order
    vector<const Route*> _original_paths; //paths in original permutation order
    const Route* _route;
    Trigger* _end_trigger;

    string _nodename;
    ReorderBufferLogger* _buffer_logger;
 
    PaperNdpPullPacer* _pacer;
    PaperNdpPull::seq_t _pull_no; // pull sequence number (local to connection)
    PaperNdpPacket::seq_t _last_packet_seqno; //sequence number of the last
                                         //packet in the connection (or 0 if not known)
    uint64_t _total_received;
    PaperNdpPacket::seq_t _highest_seqno;
    int _priority; // this receiver's priority relative to others on same pacer - low is best

    uint32_t _parked_cwnd;
    uint32_t _parked_increase;

    //used for DCTCP implementation 
    uint32_t _ecn_decrease;
    uint64_t _marked_bytes, _acked_bytes;
    double _alpha;

    // Mechanism
    void send_ack(simtime_picosec ts, PaperNdpPacket::seq_t ackno, PaperNdpPacket::seq_t pacer_no,
                  bool ecn_marked, bool enqueue_pull);
    void send_nack(simtime_picosec ts, PaperNdpPacket::seq_t ackno, PaperNdpPacket::seq_t pacer_no,
                   bool enqueue_pull, bool ecn_marked);
    void permute_paths();
   
    //Path History
    void update_path_history(const PaperNdpPacket& p);
#define HISTORY_PER_PATH 1 //how much history to hold - we hold an
                           //average of HISTORY_PER_PATH entries for
                           //each possible path
    vector<PaperReceiptEvent> _path_history;  //this is a bit heavyweight,
                                         //but it will let us
                                         //experiment with different
                                         //algorithms
    int _path_hist_index; //index of last entry to be added to _path_history
    int _path_hist_first; //index of oldest entry added to _path_history
    int _no_of_paths;
    uint64_t _ooo;
};

class PaperNdpPullPacer : public EventSource {
 public:
    PaperNdpPullPacer(EventList& ev, linkspeed_bps linkspeed, double pull_rate_modifier);  
    PaperNdpPullPacer(EventList& ev, char* fn);  
    // pull_rate_modifier is the multiplier of link speed used when
    // determining pull rate.  Generally 1 for FatTree, probable 2 for BCube
    // as there are two distinct paths between each node pair.

    void sendPacket(Packet* p, PaperNdpPacket::seq_t pacerno, PaperNdpSink *receiver);
    virtual void doNextEvent();
    void release_pulls(uint32_t flow_id, PaperNdpSink *receiver);
    void enqueue_pull(PaperNdpPull* pkt, PaperNdpSink *receiver);

    //debugging hack
    void log_me();
    bool _log_me;

    //void set_preferred_flow(int id) { _preferred_flow = id;cout << "Preferring flow "<< id << endl;};
    PaperNdpPull::seq_t pacer_no() {return _pacer_no;}

 private:
    void set_pacerno(Packet *pkt, PaperNdpPull::seq_t pacer_no);

    //#define FIFO_PULL_QUEUE
#define FAIR_PULL_QUEUE
#ifdef FIFO_PULL_QUEUE
    FifoPullQueue<PaperNdpPull> _pull_queue;
#elifdef FAIR_PULL_QUEUE
    FairPullQueue<PaperNdpPull> _pull_queue;
#else
    PrioPullQueue<PaperNdpPull> _pull_queue;
#endif
    simtime_picosec _last_pull;
    simtime_picosec _packet_drain_time;
    PaperNdpPull::seq_t _pacer_no; // pull sequence number, shared by all connections on this pacer

    //pull distribution from real life
    static int _pull_spacing_cdf_count;
    static double* _pull_spacing_cdf;

    //debugging
    double _total_excess;
    int _excess_count;
    //int _preferred_flow;
};


class PaperNdpRTSPacer : public EventSource {
 public:
    PaperNdpRTSPacer(EventList& ev, linkspeed_bps linkspeed, double pull_rate_modifier);  

    // pull_rate_modifier is the multiplier of link speed used when
    // determining pull rate.  Generally 1 for FatTree, probable 2 for BCube
    // as there are two distinct paths between each node pair.

    virtual void doNextEvent();

    void enqueue_rts(PaperNdpRTS* pkt);

 private:
    //#define RTS_FIFO_PULL_QUEUE

#ifdef RTS_FIFO_PULL_QUEUE
    FifoPullQueue<PaperNdpRTS> _rts_queue;
#else
    FairPullQueue<PaperNdpRTS> _rts_queue;
#endif
    
    simtime_picosec _last_rts;
    bool _first;
    simtime_picosec _packet_drain_time;
};


class PaperNdpRtxTimerScanner : public EventSource {
 public:
    PaperNdpRtxTimerScanner(simtime_picosec scanPeriod, EventList& eventlist);
    void doNextEvent();
    void registerNdp(PaperNdpSrc &tcpsrc);
 private:
    simtime_picosec _scanPeriod;
    typedef list<PaperNdpSrc*> tcps_t;
    tcps_t _tcps;
};

#endif

