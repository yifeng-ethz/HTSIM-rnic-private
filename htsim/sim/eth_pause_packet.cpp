#include "eth_pause_packet.h"

PacketDB<EthPausePacket> EthPausePacket::_packetdb;
std::uint64_t EthPausePacket::_live_packets = 0;

