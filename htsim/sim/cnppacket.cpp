// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#include "cnppacket.h"

PacketDB<CNPPacket> CNPPacket::_packetdb;
std::uint64_t CNPPacket::_live_packets = 0;
