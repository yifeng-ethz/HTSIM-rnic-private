// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PROFILE_H
#define RNIC_PROFILE_H

#include <string>

enum class RnicProfile {
    CollectiveNetwork,
    SlingshotLike,
    PacketizedManifold,
    FluidManifold,
};

enum class RnicFabricModel {
    NsTm3Clos,
    NsRosettaClos,
    TopologyFreeManifold,
};

enum class RnicTrafficModel {
    Packetized,
    Fluid,
};

enum class RnicControlModel {
    InBandCollective,
    PairSelectiveBackpressure,
    CentralOracle,
};

enum class RnicPacerModel {
    Prbs,
    OutstandingWindowCredits,
    CentralPacketSlots,
    None,
};

struct RnicProfileSpec {
    RnicProfile profile;
    RnicFabricModel fabric;
    RnicTrafficModel traffic;
    RnicControlModel control;
    RnicPacerModel pacer;
};

RnicProfile parseRnicProfile(const std::string& name);
const char* rnicProfileName(RnicProfile profile);
RnicProfileSpec resolveRnicProfile(RnicProfile profile);

const char* rnicFabricModelName(RnicFabricModel model);
const char* rnicTrafficModelName(RnicTrafficModel model);
const char* rnicControlModelName(RnicControlModel model);
const char* rnicPacerModelName(RnicPacerModel model);

#endif
