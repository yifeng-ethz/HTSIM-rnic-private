// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef ETHPACKET_H
#define ETHPACKET_H

#include <cstdint>
#include <list>
#include <bitset>
#include "network.h"

// ETHPAUSE is a subclass of Packet
// They incorporate a packet database, to reuse packet objects that are no longer needed.
// Note: you never construct a new EthPause packet directly; 
// rather you use the static method newpkt() which knows to reuse old packets from the database.

#define PAUSESIZE 64

class EthPausePacket : public Packet {
 public:
    //do not implement PFC; one priority class alone

    inline static EthPausePacket* newpkt(uint32_t sleep, uint32_t senderid){
        EthPausePacket* p = _packetdb.allocPacket();
        p->_type = ETH_PAUSE;
        p->_sleepTime = sleep;
        p->_senderID = senderid;
        p->_size = PAUSESIZE;
        p->_flow = &(Packet::_defaultFlow);
        p->_cascade_depth = 0;
        ++_live_packets;
        return p;
    }
  
    virtual PktPriority priority() const {return Packet::PRIO_NONE;} // This shouldn't encounter a priority queue
    void free() {
        assert(_live_packets > 0);
        --_live_packets;
        _packetdb.freePacket(this);
    }
    static std::uint64_t live_packet_count() { return _live_packets; }
    virtual ~EthPausePacket(){}

    inline uint32_t sleepTime() const {return _sleepTime;}
    inline uint32_t senderID() const {return _senderID;}
    // Comparator-realism ruling: pause frames carry their cascade depth so
    // the receiving egress can attribute a later upstream pause to the
    // chain that caused it. Depth 1 is a root pause; a pause emitted while
    // the emitting switch has a paused egress is that egress depth plus one.
    inline uint32_t cascadeDepth() const {return _cascade_depth;}
    inline void setCascadeDepth(uint32_t depth) {_cascade_depth = depth;}
 protected:
    uint32_t _sleepTime;
    uint32_t _senderID;
    uint32_t _cascade_depth{0};
    static PacketDB<EthPausePacket> _packetdb;
    static std::uint64_t _live_packets;
};

#endif
