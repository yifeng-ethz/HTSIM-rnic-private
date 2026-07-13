// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_profile.h"

#include <stdexcept>

RnicProfile parseRnicProfile(const std::string& name) {
    if (name == "rnic-cn") {
        return RnicProfile::CollectiveNetwork;
    }
    if (name == "rnic-nn") {
        return RnicProfile::NullNetworkPacketized;
    }
    if (name == "rnic-nn-fluid") {
        return RnicProfile::NullNetworkFluid;
    }
    throw std::invalid_argument(
        "unknown RNIC profile '" + name +
        "' (expected rnic-cn, rnic-nn, or rnic-nn-fluid)");
}

const char* rnicProfileName(RnicProfile profile) {
    switch (profile) {
    case RnicProfile::CollectiveNetwork:
        return "rnic-cn";
    case RnicProfile::NullNetworkPacketized:
        return "rnic-nn";
    case RnicProfile::NullNetworkFluid:
        return "rnic-nn-fluid";
    }
    throw std::invalid_argument("invalid RNIC profile enum");
}

RnicProfileSpec resolveRnicProfile(RnicProfile profile) {
    switch (profile) {
    case RnicProfile::CollectiveNetwork:
        return {profile,
                RnicFabricModel::Tomahawk3Clos,
                RnicTrafficModel::Packetized,
                RnicControlModel::InBandCollective,
                RnicPacerModel::Prbs};
    case RnicProfile::NullNetworkPacketized:
        return {profile,
                RnicFabricModel::NullNetworkManifold,
                RnicTrafficModel::Packetized,
                RnicControlModel::CentralOracle,
                RnicPacerModel::CentralPacketSlots};
    case RnicProfile::NullNetworkFluid:
        return {profile,
                RnicFabricModel::NullNetworkManifold,
                RnicTrafficModel::Fluid,
                RnicControlModel::CentralOracle,
                RnicPacerModel::None};
    }
    throw std::invalid_argument("invalid RNIC profile enum");
}

const char* rnicFabricModelName(RnicFabricModel model) {
    switch (model) {
    case RnicFabricModel::Tomahawk3Clos:
        return "tomahawk3-clos";
    case RnicFabricModel::NullNetworkManifold:
        return "null-network-manifold";
    }
    throw std::invalid_argument("invalid RNIC fabric model enum");
}

const char* rnicTrafficModelName(RnicTrafficModel model) {
    switch (model) {
    case RnicTrafficModel::Packetized:
        return "packetized";
    case RnicTrafficModel::Fluid:
        return "fluid";
    }
    throw std::invalid_argument("invalid RNIC traffic model enum");
}

const char* rnicControlModelName(RnicControlModel model) {
    switch (model) {
    case RnicControlModel::InBandCollective:
        return "in-band-collective";
    case RnicControlModel::CentralOracle:
        return "central-oracle";
    }
    throw std::invalid_argument("invalid RNIC control model enum");
}

const char* rnicPacerModelName(RnicPacerModel model) {
    switch (model) {
    case RnicPacerModel::Prbs:
        return "prbs";
    case RnicPacerModel::CentralPacketSlots:
        return "central-packet-slots";
    case RnicPacerModel::None:
        return "none";
    }
    throw std::invalid_argument("invalid RNIC pacer model enum");
}
