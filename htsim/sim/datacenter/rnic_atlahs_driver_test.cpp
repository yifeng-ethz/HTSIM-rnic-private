// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "rnic_atlahs_driver.h"
#include "rnic_collective_network_runtime.h"
#include "rnic_fluid_manifold_runtime.h"
#include "rnic_packetized_manifold_runtime.h"

namespace {

constexpr std::uint64_t kLinkCapacityBps = 400000000000ULL;

EventList& testEventList() {
    EventList& event_list = EventList::getTheEventList();
    while (EventList::doNextEvent()) {
    }
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    return event_list;
}

RnicAtlahsCliOptions optionsFor(RnicProfile profile) {
    RnicAtlahsCliOptions options{};
    options.goal_file = "rnic_one_send_32.bin";
    options.goal_rank_mapping = RnicAtlahsGoalRankMapping::GpuRank;
    options.node_count = 32;
    options.link_capacity_bps = kLinkCapacityBps;
    options.profile = profile;
    return options;
}

AtlahsHtsimApi::GoalLayout goalLayout() {
    return {32,
            1,
            1,
            AtlahsHtsimApi::GoalRankMapping::GpuRank,
            32};
}

TEST(RnicAtlahsDriverTest,
     RejectsGoalDerivedInvalidGeneratedClosShapeBeforeConstruction) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options =
        optionsFor(RnicProfile::CollectiveNetwork);

    EXPECT_THROW(
        assembleRnicAtlahsProfile(event_list, options, 31),
        std::invalid_argument);
}

TEST(RnicAtlahsDriverTest, RejectsMissingTopologyFileWithoutProcessExit) {
    EventList& event_list = testEventList();
    RnicAtlahsCliOptions options =
        optionsFor(RnicProfile::CollectiveNetwork);
    options.collective.topology_file =
        "/path/that/does/not/exist/rnic-topology";

    EXPECT_THROW(
        assembleRnicAtlahsProfile(event_list, options, 32),
        std::invalid_argument);
}

TEST(RnicAtlahsDriverTest, AssemblesGeneratedTomahawk3CollectiveProfile) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options =
        optionsFor(RnicProfile::CollectiveNetwork);

    auto session = assembleRnicAtlahsProfile(event_list, options, 32);

    ASSERT_NE(session, nullptr);
    ASSERT_NE(session->topologyConfig(), nullptr);
    ASSERT_NE(session->physicalTopology(), nullptr);
    EXPECT_EQ(session->topologyConfig()->get_tiers(), 2U);
    EXPECT_EQ(session->topologyConfig()->no_of_nodes(), 32U);
    EXPECT_EQ(session->topologyConfig()->switch_model(),
              FatTreeSwitchModel::Tomahawk3);
    EXPECT_EQ(session->topologyConfig()
                  ->tomahawk3_shared_buffer_capacity(TOR_TIER),
              static_cast<mem_b>(
                  options.collective.tomahawk3_shared_buffer_bytes));
    EXPECT_NE(dynamic_cast<RnicCollectiveNetworkRuntime*>(
                  &session->implementation()),
              nullptr);
}

TEST(RnicAtlahsDriverTest, AssemblesBothTopologyFreeProfilesWithoutAClos) {
    EventList& event_list = testEventList();
    {
        const RnicAtlahsCliOptions options =
            optionsFor(RnicProfile::PacketizedManifold);
        auto session = assembleRnicAtlahsProfile(event_list, options, 32);
        EXPECT_EQ(session->topologyConfig(), nullptr);
        EXPECT_EQ(session->physicalTopology(), nullptr);
        EXPECT_NE(dynamic_cast<RnicPacketizedManifoldRuntime*>(
                      &session->implementation()),
                  nullptr);
    }
    {
        const RnicAtlahsCliOptions options =
            optionsFor(RnicProfile::FluidManifold);
        auto session = assembleRnicAtlahsProfile(event_list, options, 32);
        EXPECT_EQ(session->topologyConfig(), nullptr);
        EXPECT_EQ(session->physicalTopology(), nullptr);
        EXPECT_NE(dynamic_cast<RnicFluidManifoldRuntime*>(
                      &session->implementation()),
                  nullptr);
    }
}

TEST(RnicAtlahsDriverTest, CollectiveManifestNamesPhysicalModelExactly) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options =
        optionsFor(RnicProfile::CollectiveNetwork);
    auto session = assembleRnicAtlahsProfile(event_list, options, 32);

    const std::string manifest =
        renderRnicAtlahsModelManifest(options, goalLayout(), *session);

    EXPECT_NE(manifest.find("profile=rnic-cn"), std::string::npos);
    EXPECT_NE(manifest.find("fabric=tomahawk3-clos"), std::string::npos);
    EXPECT_NE(manifest.find("clos_tiers=2 switch=Tomahawk3"),
              std::string::npos);
    EXPECT_NE(manifest.find(
                  "voq_key=physical-ingress-x-physical-egress"),
              std::string::npos);
    EXPECT_NE(manifest.find("pfc=off ecn=off"), std::string::npos);
    EXPECT_NE(manifest.find("declaration_gate=physical-accept"),
              std::string::npos);
    EXPECT_NE(manifest.find("eta=source-route-injection-plus-calibrated-transit"),
              std::string::npos);
}

TEST(RnicAtlahsDriverTest, ManifoldManifestsExcludePhysicalTopology) {
    EventList& event_list = testEventList();
    for (const RnicProfile profile : {RnicProfile::PacketizedManifold,
                                      RnicProfile::FluidManifold}) {
        const RnicAtlahsCliOptions options = optionsFor(profile);
        auto session = assembleRnicAtlahsProfile(event_list, options, 32);
        const std::string manifest =
            renderRnicAtlahsModelManifest(
                options, goalLayout(), *session);

        EXPECT_NE(manifest.find("topology=none"), std::string::npos);
        EXPECT_NE(manifest.find("manifold_queue=none"),
                  std::string::npos);
        EXPECT_EQ(manifest.find("switch=Tomahawk3"), std::string::npos);
    }
}

}  // namespace
