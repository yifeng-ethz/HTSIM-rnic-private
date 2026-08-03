// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "fat_tree_topology.h"
#include "ns_rosetta_switch.h"
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
    return {32, 1, 1, AtlahsHtsimApi::GoalRankMapping::GpuRank, 32};
}

TEST(RnicAtlahsDriverTest, RejectsGoalDerivedInvalidGeneratedClosShapeBeforeConstruction) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options = optionsFor(RnicProfile::CollectiveNetwork);

    EXPECT_THROW(assembleRnicAtlahsProfile(event_list, options, 31), std::invalid_argument);
}

TEST(RnicAtlahsDriverTest, RejectsMissingTopologyFileWithoutProcessExit) {
    EventList& event_list = testEventList();
    RnicAtlahsCliOptions options = optionsFor(RnicProfile::CollectiveNetwork);
    options.collective.topology_file = "/path/that/does/not/exist/rnic-topology";

    EXPECT_THROW(assembleRnicAtlahsProfile(event_list, options, 32), std::invalid_argument);
}

TEST(RnicAtlahsDriverTest, AssemblesGeneratedNsTm3CollectiveProfile) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options = optionsFor(RnicProfile::CollectiveNetwork);

    auto session = assembleRnicAtlahsProfile(event_list, options, 32);

    ASSERT_NE(session, nullptr);
    ASSERT_NE(session->topologyConfig(), nullptr);
    ASSERT_NE(session->physicalTopology(), nullptr);
    EXPECT_EQ(session->topologyConfig()->get_tiers(), 2U);
    EXPECT_EQ(session->topologyConfig()->no_of_nodes(), 32U);
    EXPECT_EQ(session->topologyConfig()->switch_model(), FatTreeSwitchModel::NsTm3);
    EXPECT_EQ(session->topologyConfig()->ns_tm3_shared_buffer_capacity(TOR_TIER),
              static_cast<mem_b>(options.collective.ns_tm3_shared_buffer_bytes));
    EXPECT_NE(dynamic_cast<RnicCollectiveNetworkRuntime*>(&session->implementation()), nullptr);
}

TEST(RnicAtlahsDriverTest, RejectsSlingshotLikeProfileUntilItIsWired) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options = optionsFor(RnicProfile::SlingshotLike);

    EXPECT_THROW(assembleRnicAtlahsProfile(event_list, options, 32), std::invalid_argument);
}

TEST(RnicAtlahsDriverTest, AssemblesBothTopologyFreeProfilesWithoutAClos) {
    EventList& event_list = testEventList();
    {
        const RnicAtlahsCliOptions options = optionsFor(RnicProfile::PacketizedManifold);
        auto session = assembleRnicAtlahsProfile(event_list, options, 32);
        EXPECT_EQ(session->topologyConfig(), nullptr);
        EXPECT_EQ(session->physicalTopology(), nullptr);
        EXPECT_NE(dynamic_cast<RnicPacketizedManifoldRuntime*>(&session->implementation()),
                  nullptr);
    }
    {
        const RnicAtlahsCliOptions options = optionsFor(RnicProfile::FluidManifold);
        auto session = assembleRnicAtlahsProfile(event_list, options, 32);
        EXPECT_EQ(session->topologyConfig(), nullptr);
        EXPECT_EQ(session->physicalTopology(), nullptr);
        EXPECT_NE(dynamic_cast<RnicFluidManifoldRuntime*>(&session->implementation()), nullptr);
    }
}

TEST(RnicAtlahsDriverTest, CollectiveManifestNamesPhysicalModelExactly) {
    EventList& event_list = testEventList();
    const RnicAtlahsCliOptions options = optionsFor(RnicProfile::CollectiveNetwork);
    auto session = assembleRnicAtlahsProfile(event_list, options, 32);

    const std::string manifest = renderRnicAtlahsModelManifest(options, goalLayout(), *session);

    EXPECT_NE(manifest.find("profile=rnic-cn"), std::string::npos);
    EXPECT_NE(manifest.find("fabric=ns-tm3-clos"), std::string::npos);
    EXPECT_NE(manifest.find("clos_tiers=2 switch=ns-tm3"), std::string::npos);
    EXPECT_NE(manifest.find("link_failures=0 eta_calibration_config=construction-snapshot"),
              std::string::npos);
    EXPECT_NE(manifest.find("voq_key=physical-ingress-x-physical-egress"), std::string::npos);
    EXPECT_NE(manifest.find("pfc=off ecn=off"), std::string::npos);
    EXPECT_NE(manifest.find("declaration_gate=declare-and-go"), std::string::npos);
    EXPECT_NE(manifest.find("declare_cca_field=nflow_ppm fractional_nflow=always"),
              std::string::npos);
    EXPECT_NE(manifest.find("declare_debug_fields=collective_id,expected_fan_in "
                            "declare_debug_affects_rx_cca=false"),
              std::string::npos);
    EXPECT_NE(
        manifest.find("grant=margin*C/n_hat-scaled-by-own-nflow n_hat=sum-active-declare-nflow"),
        std::string::npos);
    EXPECT_NE(manifest.find("rate_feedback=every-resequenced-ack-window-frozen-snapshot"),
              std::string::npos);
    EXPECT_NE(manifest.find("rate_effect=receiver-boundary-k-plus-2"), std::string::npos);
    EXPECT_NE(manifest.find("eta=source-route-injection-plus-packet-specific-no-load-transit"),
              std::string::npos);
    EXPECT_NE(
        manifest.find("eta_transit=pipe-switch-latency-plus-remaining-ns-tm3-egress-serialization"),
        std::string::npos);
    EXPECT_NE(manifest.find("prbs_algorithm=galois-lfsr64-block64 prbs_version=2"),
              std::string::npos);
    EXPECT_NE(manifest.find("word_extraction=prestep-lsb-block64-lsb-first"), std::string::npos);
    EXPECT_NE(manifest.find("lfsr_steps_per_word=64"), std::string::npos);
    EXPECT_NE(manifest.find("bounded_draw=nonzero-rejection-modulo-v1"), std::string::npos);
    EXPECT_NE(manifest.find("recovery=deterministic-gap-nack-retransmission"), std::string::npos);
    EXPECT_NE(manifest.find("gap_decision=post-resequence-same-timestamp"), std::string::npos);
    EXPECT_NE(manifest.find("early_admission=hard-error"), std::string::npos);
    EXPECT_NE(manifest.find("overflow_admission=hard-error"), std::string::npos);
    EXPECT_NE(manifest.find("gap_nack_priority=high"), std::string::npos);
    EXPECT_NE(manifest.find("retransmission_priority=low"), std::string::npos);
    EXPECT_NE(manifest.find("retransmission_arbitration=per-flow-head-in-node-prbs-lottery"),
              std::string::npos);
    EXPECT_NE(manifest.find("retransmission_prbs_draw=true"), std::string::npos);
    EXPECT_NE(manifest.find("retransmission_rate_accounting=substitutes-fresh-granted-service"),
              std::string::npos);
    EXPECT_NE(manifest.find("maximum_retransmissions=8"), std::string::npos);
    EXPECT_NE(manifest.find("retransmission_rto_ps=50000000000"), std::string::npos);
    EXPECT_NE(manifest.find("retransmission_rto_epoch=physical-retry-serialization-end"),
              std::string::npos);
    EXPECT_NE(manifest.find("control_loss=fatal-no-control-recovery"), std::string::npos);
    EXPECT_NE(manifest.find("retire_deadline=max-original-release"), std::string::npos);
    EXPECT_NE(manifest.find("retirement_gate=exact-rx-ledger-and-no-gap"), std::string::npos);
}

TEST(RnicAtlahsDriverTest, ManifoldManifestsExcludePhysicalTopology) {
    EventList& event_list = testEventList();
    for (const RnicProfile profile :
         {RnicProfile::PacketizedManifold, RnicProfile::FluidManifold}) {
        const RnicAtlahsCliOptions options = optionsFor(profile);
        auto session = assembleRnicAtlahsProfile(event_list, options, 32);
        const std::string manifest = renderRnicAtlahsModelManifest(options, goalLayout(), *session);

        EXPECT_NE(manifest.find("topology=none"), std::string::npos);
        EXPECT_NE(manifest.find("manifold_queue=none"), std::string::npos);
        EXPECT_EQ(manifest.find("switch=ns-tm3"), std::string::npos);
    }
}

}  // namespace
