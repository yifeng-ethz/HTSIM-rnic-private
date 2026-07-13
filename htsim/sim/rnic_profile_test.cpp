// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_profile.h"

#include <stdexcept>

#include <gtest/gtest.h>

TEST(RnicProfileTest, ParsesOnlyPublicProfileNames) {
    EXPECT_EQ(parseRnicProfile("rnic-cn"), RnicProfile::CollectiveNetwork);
    EXPECT_EQ(parseRnicProfile("rnic-nn"), RnicProfile::PacketizedManifold);
    EXPECT_EQ(parseRnicProfile("rnic-nn-fluid"), RnicProfile::FluidManifold);

    EXPECT_THROW(parseRnicProfile("rnic-cc"), std::invalid_argument);
    EXPECT_THROW(parseRnicProfile("tm3"), std::invalid_argument);
    EXPECT_THROW(parseRnicProfile("rnic-null"), std::invalid_argument);
}

TEST(RnicProfileTest, CollectiveNetworkUsesTomahawk3AndPrbs) {
    const RnicProfileSpec spec = resolveRnicProfile(RnicProfile::CollectiveNetwork);

    EXPECT_EQ(spec.fabric, RnicFabricModel::Tomahawk3Clos);
    EXPECT_EQ(spec.traffic, RnicTrafficModel::Packetized);
    EXPECT_EQ(spec.control, RnicControlModel::InBandCollective);
    EXPECT_EQ(spec.pacer, RnicPacerModel::Prbs);
}

TEST(RnicProfileTest, PacketizedManifoldUsesCentralFeasibleSlots) {
    const RnicProfileSpec spec = resolveRnicProfile(RnicProfile::PacketizedManifold);

    EXPECT_EQ(spec.fabric, RnicFabricModel::TopologyFreeManifold);
    EXPECT_EQ(spec.traffic, RnicTrafficModel::Packetized);
    EXPECT_EQ(spec.control, RnicControlModel::CentralOracle);
    EXPECT_EQ(spec.pacer, RnicPacerModel::CentralPacketSlots);
}

TEST(RnicProfileTest, FluidManifoldHasNoPacketPacer) {
    const RnicProfileSpec spec = resolveRnicProfile(RnicProfile::FluidManifold);

    EXPECT_EQ(spec.fabric, RnicFabricModel::TopologyFreeManifold);
    EXPECT_EQ(spec.traffic, RnicTrafficModel::Fluid);
    EXPECT_EQ(spec.control, RnicControlModel::CentralOracle);
    EXPECT_EQ(spec.pacer, RnicPacerModel::None);
}

TEST(RnicProfileTest, NamesAreStableManifestValues) {
    for (const RnicProfile profile : {RnicProfile::CollectiveNetwork,
                                      RnicProfile::PacketizedManifold,
                                      RnicProfile::FluidManifold}) {
        EXPECT_EQ(parseRnicProfile(rnicProfileName(profile)), profile);
    }

    EXPECT_STREQ(rnicFabricModelName(RnicFabricModel::Tomahawk3Clos), "tomahawk3-clos");
    EXPECT_STREQ(rnicFabricModelName(RnicFabricModel::TopologyFreeManifold),
                 "manifold-nn");
    EXPECT_STREQ(rnicTrafficModelName(RnicTrafficModel::Packetized), "packetized");
    EXPECT_STREQ(rnicControlModelName(RnicControlModel::CentralOracle), "central-oracle");
    EXPECT_STREQ(rnicPacerModelName(RnicPacerModel::CentralPacketSlots),
                 "central-packet-slots");
}
