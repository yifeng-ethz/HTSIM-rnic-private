// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#ifndef PAPER_NDPPACKET_H
#define PAPER_NDPPACKET_H

#include <list>
#include "network.h"
#include "ecn.h"

// PaperNdpPacket and PaperNdpAck are subclasses of Packet.
// They incorporate a packet database, to reuse packet objects that are no longer needed.
// Note: you never construct a new PaperNdpPacket or PaperNdpAck directly; 
// rather you use the static method newpkt() which knows to reuse old packets from the database.

//#define ACKSIZE 64
#define VALUE_NOT_SET -1
//#define PULL_MAXPATHS 256 // maximum path ID that can be pulled

class PaperNdpPacket : public Packet {
    using Packet::set_route;
 
public:
    typedef uint64_t seq_t;

    // pseudo-constructor for a routeless packet - routing information
    // must be filled in later
    inline static PaperNdpPacket* newpkt(PacketFlow &flow, 

                                    seq_t seqno, seq_t pacerno, int size, 
                                    bool retransmitted, 
                                    bool last_packet,
                                    uint32_t destination = UINT32_MAX) {
        PaperNdpPacket* p = _packetdb.allocPacket();


        p->set_attrs(flow, size+ACKSIZE, seqno+size-1); // The NDP sequence number is the first byte of the packet; I will ID the packet by its last byte.
        p->_type = NDP;
        p->_is_header = false;
        p->_bounced = false;
        p->_seqno = seqno;
        p->_pacerno = pacerno;
        p->_retransmitted = retransmitted;
        p->_last_packet = last_packet;
        p->_path_len = 0;
        p->_direction = NONE;
        p->set_dst(destination);
        p->_trim_hop = UINT32_MAX;
        p->_trim_direction = NONE;
        return p;
    }
  
    inline static PaperNdpPacket* newpkt(PacketFlow &flow, const Route &route, 
                                    seq_t seqno, seq_t pacerno, int size, 
                                    bool retransmitted, int32_t no_of_paths,
                                    bool last_packet,
                                    uint32_t destination = UINT32_MAX) {
        PaperNdpPacket* p = _packetdb.allocPacket();
 
        p->set_route(flow,route,size+ACKSIZE,seqno+size-1); // The NDP sequence number is the first byte of the packet; I will ID the packet by its last byte.
        p->_type = NDP;
        p->_is_header = false;
        p->_bounced = false;
        p->_seqno = seqno;
        p->_pacerno = pacerno;
        p->_direction = NONE;    
        p->_retransmitted = retransmitted;
        p->_no_of_paths = no_of_paths;
        p->_last_packet = last_packet;
        p->_path_len = route.size();
        p->_trim_hop = UINT32_MAX;
        p->_trim_direction = NONE;
        p->set_dst(destination);
        return p;
    }
  
    virtual inline void  strip_payload(uint16_t trim_size) {
        Packet::strip_payload(trim_size); _size = ACKSIZE;_trim_hop = _nexthop;
        _trim_direction = _direction;
    };

    virtual inline void set_route(const Route &route) {
        if (_trim_hop!=INT32_MAX)
            _trim_hop -= route.size();

        Packet::set_route(route);
    }

    void free() {_packetdb.freePacket(this);}
    virtual ~PaperNdpPacket(){}
    inline seq_t seqno() const {return _seqno;}
    inline seq_t pacerno() const {return _pacerno;}
    inline void set_pacerno(seq_t pacerno) {_pacerno = pacerno;}
    inline bool retransmitted() const {return _retransmitted;}
    inline bool last_packet() const {return _last_packet;}
    inline int32_t trim_hop() const {return _trim_hop;}
    inline packet_direction trim_direction() const {return _trim_direction;}

    inline simtime_picosec ts() const {return _ts;}
    inline void set_ts(simtime_picosec ts) {_ts = ts;}
    inline int32_t path_id() const {if (_pathid!=UINT32_MAX) return _pathid; else return _route->path_id();}
    inline int32_t no_of_paths() const {return _no_of_paths;}
    virtual PktPriority priority() const {
        if (_is_header) {
            return Packet::PRIO_HI;
        } else {
            return Packet::PRIO_LO;
        }
    }
    const static int ACKSIZE=64; 
protected:
    seq_t _seqno;
    seq_t _pacerno;  // the pacer sequence number from the pull, seq space is common to all flows on that pacer
    simtime_picosec _ts;

    bool _retransmitted;
    int32_t _no_of_paths;  // how many paths are in the sender's
    // list.  A real implementation would not
    // send this in every packet, but this is
    // simulation, and this is easiest to
    // implement
    bool _last_packet;  // set to true in the last packet in a flow.
    int32_t _trim_hop;
    packet_direction _trim_direction;
    static PacketDB<PaperNdpPacket> _packetdb;
};

class PaperNdpAck : public Packet {
public:
    typedef PaperNdpPacket::seq_t seq_t;
  
    inline static PaperNdpAck* newpkt(PacketFlow &flow, const Route &route, 
                                 seq_t pacerno, seq_t ackno, seq_t cumulative_ack,
                                 seq_t pullno, int32_t path_id, uint32_t destination = UINT32_MAX) {
        PaperNdpAck* p = _packetdb.allocPacket();
        p->set_route(flow,route,PaperNdpPacket::ACKSIZE,ackno);
        p->_type = NDPACK;
        p->_is_header = true;
        p->_bounced = false;
        p->_pacerno = pacerno;
        p->_ackno = ackno;
        p->_cumulative_ack = cumulative_ack;
        p->_pull = true;
        p->_pullno = pullno;
        p->_path_id = path_id;
        p->_path_len = 0;
        p->_direction = NONE;
        p->_ecn_echo = false;
        p->set_dst(destination);
        return p;
    }
  
    void free() {_packetdb.freePacket(this);}
    inline seq_t pacerno() const {return _pacerno;}
    inline void set_pacerno(seq_t pacerno) {_pacerno = pacerno;}
    inline seq_t ackno() const {return _ackno;}
    inline seq_t cumulative_ack() const {return _cumulative_ack;}
    inline simtime_picosec ts() const {return _ts;}
    inline void set_ts(simtime_picosec ts) {_ts = ts;}
    inline bool pull() const {return _pull;}
    inline seq_t pullno() const {return _pullno;}
    int32_t  path_id() const {return _path_id;}
    inline void dont_pull() {_pull = false; _pullno = 0;}
    inline void set_ecn_echo(bool ecn_echo) {_ecn_echo = ecn_echo;}
    inline bool ecn_echo() const {return _ecn_echo;}
    virtual PktPriority priority() const {return Packet::PRIO_HI;}
  
    virtual ~PaperNdpAck(){}

protected:
    seq_t _pacerno;
    seq_t _ackno;
    seq_t _cumulative_ack;
    simtime_picosec _ts;
    seq_t _pullno;

    int32_t _path_id; //see comment in PaperNdpPull
    bool _pull;
    bool _ecn_echo;
    static PacketDB<PaperNdpAck> _packetdb;
};


class PaperNdpNack : public Packet {
public:
    typedef PaperNdpPacket::seq_t seq_t;
  
    inline static PaperNdpNack* newpkt(PacketFlow &flow, const Route &route, 
                                  seq_t pacerno, seq_t ackno, seq_t cumulative_ack,
                                  seq_t pullno, int32_t path_id,uint32_t destination = UINT32_MAX) {
        PaperNdpNack* p = _packetdb.allocPacket();
        p->set_route(flow,route,PaperNdpPacket::ACKSIZE,ackno);
        p->_type = NDPNACK;
        p->_is_header = true;
        p->_bounced = false;
        p->_pacerno = pacerno;
        p->_ackno = ackno;
        p->_cumulative_ack = cumulative_ack;
        p->_pull = true;
        p->_direction = NONE;
        p->_pullno = pullno;
        p->_path_id = path_id; // used to indicate which path the data
        // packet was trimmed on
        p->_path_len = 0;
        p->_ecn_echo = false;
        p->set_dst(destination);
        return p;
    }
  
    void free() {_packetdb.freePacket(this);}
    inline seq_t pacerno() const {return _pacerno;}
    inline void set_pacerno(seq_t pacerno) {_pacerno = pacerno;}
    inline seq_t ackno() const {return _ackno;}
    inline seq_t cumulative_ack() const {return _cumulative_ack;}
    inline simtime_picosec ts() const {return _ts;}
    inline void set_ts(simtime_picosec ts) {_ts = ts;}
    inline bool pull() const {return _pull;}
    inline seq_t pullno() const {return _pullno;}
    int32_t path_id() const {return _path_id;}
    inline void dont_pull() {_pull = false; _pullno = 0;}
    inline void set_ecn_echo(bool ecn_echo) {_ecn_echo = ecn_echo;}
    inline bool ecn_echo() const {return _ecn_echo;}
    virtual PktPriority priority() const {return Packet::PRIO_LO;}
  
    virtual ~PaperNdpNack(){}

protected:
    seq_t _pacerno;
    seq_t _ackno;
    seq_t _cumulative_ack;
    simtime_picosec _ts;
    seq_t _pullno;
    int32_t _path_id;
    bool _pull;
    bool _ecn_echo;
    static PacketDB<PaperNdpNack> _packetdb;
};

class PaperNdpRTS : public Packet {
public:
    typedef PaperNdpPacket::seq_t seq_t;
    
    inline static PaperNdpRTS* newpkt(PacketFlow& flow, int grants,uint32_t destination = UINT32_MAX) {
        PaperNdpRTS* p = _packetdb.allocPacket();

        p->set_attrs(flow, PaperNdpPacket::ACKSIZE, 0);
        p->_type = NDPRTS;
        p->_is_header = true;
        p->_bounced = false;
        p->_grants = grants;
        p->_path_id = 0;
        p->_direction = NONE;    
        p->set_dst(destination);
        return p;
    }

    inline static PaperNdpRTS* newpkt(PacketFlow& flow, const route_t& route, int grants,uint32_t destination = UINT32_MAX) {
        PaperNdpRTS* p = _packetdb.allocPacket();
        p->set_route(flow, route, PaperNdpPacket::ACKSIZE, 0);
        assert(p->route());

        p->_type = NDPRTS;
        p->_is_header = true;
        p->_bounced = false;
        p->_grants = grants;
        p->_path_id = 0;
        p->_direction = NONE;    
        p->set_dst(destination);

        return p;
    }
    
    void free() {_packetdb.freePacket(this);}
    inline seq_t grants() const {return _grants;}
    inline void set_grants(seq_t grants) {_grants = grants;}
    int32_t path_id() const {return _path_id;}

    inline void set_ts(simtime_picosec ts) {_ts = ts;}
    virtual PktPriority priority() const {return Packet::PRIO_HI;}
    
    virtual ~PaperNdpRTS(){}

protected:
    simtime_picosec _ts;
    seq_t _grants;
    int32_t _path_id; // indicates ??
    static PacketDB<PaperNdpRTS> _packetdb;
};



class PaperNdpPull : public Packet {
public:
    typedef PaperNdpPacket::seq_t seq_t;

    inline static PaperNdpPull* newpkt(PaperNdpAck* ack) {
        PaperNdpPull* p = _packetdb.allocPacket();
        assert(ack->route());
        p->set_route(ack->flow(), *(ack->route()), PaperNdpPacket::ACKSIZE, ack->ackno());

        assert(p->route());
        p->_type = NDPPULL;
        p->_is_header = true;
        p->_bounced = false;
        p->_ackno = ack->ackno();
        p->_cumulative_ack = ack->cumulative_ack();
        p->_pullno = ack->pullno();
        p->_path_len = 0;
        p->_direction = NONE;
        p->set_dst(ack->dst());
        return p;
    }
  
    inline static PaperNdpPull* newpkt(PaperNdpNack* nack) {
        PaperNdpPull* p = _packetdb.allocPacket();
        assert(nack->route());
        p->set_route(nack->flow(), *(nack->route()), PaperNdpPacket::ACKSIZE, nack->ackno());

        assert(p->route());

        p->_type = NDPPULL;
        p->_is_header = true;
        p->_bounced = false;
        p->_ackno = nack->ackno();
        p->_cumulative_ack = nack->cumulative_ack();
        p->_pullno = nack->pullno();
        p->_path_len = 0;
        p->_direction = NONE;
        p->set_dst(nack->dst());
        return p;
    }

    inline static PaperNdpPull* newpkt(PaperNdpRTS* rts, seq_t cumack,seq_t pullno) {
        PaperNdpPull* p = _packetdb.allocPacket();
        p->set_attrs(rts->flow(), PaperNdpPacket::ACKSIZE, 0);

        p->_type = NDPPULL;
        p->_is_header = true;
        p->_bounced = false;
        p->_ackno = cumack;
        p->_cumulative_ack = cumack;
        p->_pullno = pullno;

        //cout << "Creating PULL packet with pull no " << p->_pullno << " received pull no " << pullno << endl;
        p->_path_len = 0;
        p->_direction = NONE;
        p->set_dst(rts->dst());
        return p;
    }

    inline static PaperNdpPull* newpkt(PaperNdpRTS* rts, const route_t& route,seq_t cumack,seq_t pullno,uint32_t destination = UINT32_MAX) {
        PaperNdpPull* p = _packetdb.allocPacket();
        p->set_route(rts->flow(), route, PaperNdpPacket::ACKSIZE, 0);

        assert(p->route());

        p->_type = NDPPULL;
        p->_is_header = true;
        p->_bounced = false;
        p->_ackno = cumack;
        p->_cumulative_ack = cumack;
        p->_pullno = pullno;

        //cout << "Creating PULL2 packet with pull no " << p->_pullno << " received pull no " << pullno << endl;

        p->_path_len = 0;
        p->set_dst(destination);
        p->_direction = NONE;
        return p;
    }    

    inline static PaperNdpPull* newpkt(PacketFlow& flow, const route_t& route,seq_t cumack,seq_t pullno,uint32_t destination = UINT32_MAX) {
        PaperNdpPull* p = _packetdb.allocPacket();
        p->set_route(flow, route, PaperNdpPacket::ACKSIZE, 0);

        assert(p->route());

        p->_type = NDPPULL;
        p->_is_header = true;
        p->_bounced = false;
        p->_ackno = cumack;
        p->_cumulative_ack = cumack;
        p->_pullno = pullno;
        p->_path_len = 0;
        p->set_dst(destination);
        p->_direction = NONE;
        return p;
    }    


    void free() {_packetdb.freePacket(this);}
    inline seq_t pacerno() const {return _pacerno;}
    inline void set_pacerno(seq_t pacerno) {_pacerno = pacerno;}
    inline seq_t ackno() const {return _ackno;}
    inline seq_t cumulative_ack() const {return _cumulative_ack;}
    inline seq_t pullno() const {return _pullno;}
    int32_t path_id() const {return _path_id;}
    virtual PktPriority priority() const {return Packet::PRIO_HI;}
  
    virtual ~PaperNdpPull(){}

protected:
    seq_t _pacerno;
    seq_t _ackno;
    seq_t _cumulative_ack;
    seq_t _pullno;
    int32_t _path_id; // indicates ??
    static PacketDB<PaperNdpPull> _packetdb;
};

#endif
