#include "rocepacket.h"

PacketDB<RocePacket> RocePacket::_packetdb;
PacketDB<RoceAck> RoceAck::_packetdb;
PacketDB<RoceNack> RoceNack::_packetdb;
std::uint64_t RocePacket::_live_packets = 0;
std::uint64_t RoceAck::_live_packets = 0;
std::uint64_t RoceNack::_live_packets = 0;
