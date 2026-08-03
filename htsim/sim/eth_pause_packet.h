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
 protected:
    uint32_t _sleepTime;
    uint32_t _senderID;
    static PacketDB<EthPausePacket> _packetdb;
    static std::uint64_t _live_packets;
};

#endif
