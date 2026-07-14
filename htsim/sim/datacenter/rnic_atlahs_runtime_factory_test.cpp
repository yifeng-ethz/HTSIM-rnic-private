// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "atlahs_htsim_api.h"
#include "fat_tree_topology.h"
#include "rnic_atlahs_runtime_factory.h"
#include "rnic_fluid_manifold_runtime.h"
#include "rnic_packetized_manifold_runtime.h"
#include "ns_tm3_switch.h"

namespace {

constexpr std::uint64_t kAccessCapacityBps = 100000000000ULL;

static_assert(!std::is_move_constructible_v<RnicAtlahsRuntimeAssembly>);
static_assert(!std::is_move_assignable_v<RnicAtlahsRuntimeAssembly>);

EventList& testEventList() {
    EventList& event_list = EventList::getTheEventList();
    while (EventList::doNextEvent()) {
    }
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
    return event_list;
}

std::unique_ptr<FatTreeTopologyCfg> twoTierConfig(
        queue_type queue_mode = COMPOSITE,
        std::uint64_t access_capacity_bps = kAccessCapacityBps) {
    return std::make_unique<FatTreeTopologyCfg>(
        2,
        32,
        access_capacity_bps,
        1 << 20,
        timeFromNs(100),
        0,
        queue_mode,
        FAIR_PRIO);
}

std::unique_ptr<FatTreeTopologyCfg> threeTierConfig() {
    return std::make_unique<FatTreeTopologyCfg>(
        3,
        16,
        kAccessCapacityBps,
        1 << 20,
        timeFromNs(100),
        0,
        COMPOSITE,
        FAIR_PRIO);
}

RnicCollectiveNetworkConfig collectiveConfig(
        std::uint64_t access_capacity_bps = kAccessCapacityBps) {
    return {
        access_capacity_bps,
        RnicDataPacketizationConfig(1000, 64),
        RnicRingCamConfig{timeFromUs(4.096),
                          timeFromNs(16),
                          1 << 20},
        0x123456789abcdef0ULL,
        timeFromUs(10.0),
        RnicCollectiveController::kDefaultMarginPpm,
        64,
        [](std::uint32_t,
           std::uint32_t,
           const RnicPacketExtent&) {
            return timeFromNs(500);
        },
    };
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkOwnsForcedNsTm3ClosAndRuntime) {
    EventList& event_list = testEventList();
    std::unique_ptr<FatTreeTopologyCfg> input_config = twoTierConfig();
    FatTreeTopologyCfg* const input_config_address = input_config.get();
    EXPECT_EQ(input_config->switch_model(), FatTreeSwitchModel::Default);

    auto assembly = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::CollectiveNetwork,
        collectiveConfig(),
        std::move(input_config));

    ASSERT_NE(assembly, nullptr);
    EXPECT_EQ(assembly->profileSpec().profile,
              RnicProfile::CollectiveNetwork);
    EXPECT_EQ(assembly->profileSpec().fabric,
              RnicFabricModel::NsTm3Clos);
    ASSERT_NE(assembly->topologyConfig(), nullptr);
    ASSERT_NE(assembly->physicalTopology(), nullptr);
    EXPECT_EQ(assembly->topologyConfig(), input_config_address);
    EXPECT_EQ(&assembly->physicalTopology()->cfg(), input_config_address);
    EXPECT_EQ(assembly->topologyConfig()->switch_model(),
              FatTreeSwitchModel::NsTm3);
    EXPECT_NE(dynamic_cast<RnicCollectiveNetworkRuntime*>(
                  &assembly->implementation()),
              nullptr);
    ASSERT_FALSE(assembly->physicalTopology()->switches_lp.empty());
    ASSERT_FALSE(assembly->physicalTopology()->switches_up.empty());
    EXPECT_TRUE(assembly->physicalTopology()->switches_c.empty());
    for (Switch* const physical_switch :
         assembly->physicalTopology()->switches_lp) {
        EXPECT_NE(dynamic_cast<NsTm3Switch*>(physical_switch), nullptr);
    }
    for (Switch* const physical_switch :
         assembly->physicalTopology()->switches_up) {
        EXPECT_NE(dynamic_cast<NsTm3Switch*>(physical_switch), nullptr);
    }
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkCalibratesDataEtaFromItsOwnedPhysicalClos) {
    EventList& event_list = testEventList();
    std::size_t caller_calibration_calls = 0;
    RnicCollectiveNetworkConfig config = collectiveConfig();
    config.calibrated_transit_ps =
        [&caller_calibration_calls](std::uint32_t,
                                    std::uint32_t,
                                    const RnicPacketExtent&) {
            ++caller_calibration_calls;
            return timeFromNs(1);
        };
    auto assembly = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::CollectiveNetwork,
        std::move(config),
        twoTierConfig());
    auto* const runtime = dynamic_cast<RnicCollectiveNetworkRuntime*>(
        &assembly->implementation());
    ASSERT_NE(runtime, nullptr);

    std::size_t completions = 0;
    runtime->setup(32, [&completions](AtlahsFlowId) { ++completions; });
    // Full-wire and short-tail DATA on same- and cross-ToR paths exercise all
    // packet-extent-aware physical no-queue calibrations.
    runtime->send({0x100000001ULL,
                   0,
                   1,
                   936,
                   EventList::now(),
                   0});
    runtime->send({0x100000002ULL,
                   2,
                   31,
                   936,
                   EventList::now(),
                   0});
    runtime->send({0x100000003ULL,
                   4,
                   5,
                   13,
                   EventList::now(),
                   0});
    runtime->send({0x100000004ULL,
                   6,
                   30,
                   13,
                   EventList::now(),
                   0});
    constexpr std::size_t maximum_events = 100000;
    std::size_t event_count = 0;
    while (runtime->hasPendingPhysicalWork()
           && event_count < maximum_events) {
        ASSERT_TRUE(EventList::doNextEvent());
        ++event_count;
    }

    EXPECT_FALSE(runtime->hasPendingPhysicalWork());
    runtime->validateQuiescent();
    EXPECT_EQ(completions, 4U);
    EXPECT_EQ(runtime->flow(0x100000001ULL).delivered_data_packets, 1U);
    EXPECT_EQ(runtime->flow(0x100000002ULL).delivered_data_packets, 1U);
    EXPECT_EQ(runtime->flow(0x100000003ULL).delivered_data_packets, 1U);
    EXPECT_EQ(runtime->flow(0x100000004ULL).delivered_data_packets, 1U);
    EXPECT_EQ(caller_calibration_calls, 0U);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     ApiOwnsCollectiveRuntimeTopologyAndConfigurationAsOneSession) {
    EventList& event_list = testEventList();
    AtlahsHtsimApi api;
    api.setEventList(&event_list);
    api.total_nodes = 32;

    auto session = makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig(),
                              twoTierConfig());
    RnicAtlahsRuntimeAssembly* const session_address = session.get();
    api.setTopologyCfg(session->topologyConfig());
    api.setTopology(session->physicalTopology());
    api.setFlowRuntime(std::move(session));

    EXPECT_EQ(api.getFlowRuntime(), session_address);
    EXPECT_EQ(api.getTopologyCfg(), session_address->topologyConfig());
    EXPECT_EQ(api.getTopology(), session_address->physicalTopology());
    api.Setup();

    auto* const runtime = dynamic_cast<RnicCollectiveNetworkRuntime*>(
        &session_address->implementation());
    ASSERT_NE(runtime, nullptr);
    runtime->send({0x100000003ULL,
                   0,
                   31,
                   936,
                   EventList::now(),
                   0});
    constexpr std::size_t maximum_events = 100000;
    std::size_t event_count = 0;
    while (api.runtimeHasPendingPhysicalWork()
           && event_count < maximum_events) {
        ASSERT_TRUE(EventList::doNextEvent());
        ++event_count;
    }

    EXPECT_FALSE(api.runtimeHasPendingPhysicalWork());
    runtime->validateQuiescent();
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkRequiresExactlyTwoPhysicalTiers) {
    EventList& event_list = testEventList();

    EXPECT_THROW(
        makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig()),
        std::invalid_argument);
    EXPECT_THROW(
        makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig(),
                              threeTierConfig()),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkRejectsPfcAndLosslessQueueModes) {
    EventList& event_list = testEventList();

    EXPECT_THROW(
        makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig(),
                              twoTierConfig(LOSSLESS)),
        std::invalid_argument);
    EXPECT_THROW(
        makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig(),
                              twoTierConfig(LOSSLESS_INPUT)),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkRejectsPreconfiguredLinkFailures) {
    EventList& event_list = testEventList();
    std::unique_ptr<FatTreeTopologyCfg> failed = twoTierConfig();
    failed->set_failed_links(1);

    EXPECT_THROW(
        makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig(),
                              std::move(failed)),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkFreezesCalibrationAtAssemblyConstruction) {
    EventList& event_list = testEventList();
    auto assembly = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::CollectiveNetwork,
        collectiveConfig(),
        twoTierConfig());
    auto* const runtime = dynamic_cast<RnicCollectiveNetworkRuntime*>(
        &assembly->implementation());
    ASSERT_NE(runtime, nullptr);

    // Physical serializers already own the original 100-Gbit/s rate. The
    // legacy API still exposes its configuration mutably; changing that
    // object must not change the construction-time ETA baseline.
    FatTreeTopologyCfg* const exposed_config = assembly->topologyConfig();
    exposed_config->set_tier_parameters(
        AGG_TIER,
        exposed_config->radix_up(AGG_TIER),
        exposed_config->radix_down(AGG_TIER),
        exposed_config->queue_up(AGG_TIER),
        exposed_config->queue_down(AGG_TIER),
        exposed_config->bundlesize(AGG_TIER),
        exposed_config->downlink_speed(AGG_TIER) / 4,
        1);

    std::size_t completions = 0;
    runtime->setup(32, [&completions](AtlahsFlowId) { ++completions; });
    runtime->send({0x100000005ULL,
                   0,
                   31,
                   936,
                   EventList::now(),
                   0});
    constexpr std::size_t maximum_events = 100000;
    std::size_t event_count = 0;
    while (runtime->hasPendingPhysicalWork()
           && event_count < maximum_events) {
        ASSERT_TRUE(EventList::doNextEvent());
        ++event_count;
    }

    EXPECT_FALSE(runtime->hasPendingPhysicalWork());
    runtime->validateQuiescent();
    EXPECT_EQ(completions, 1U);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkRequiresRuntimeAndPhysicalAccessRatesToMatch) {
    EventList& event_list = testEventList();

    EXPECT_THROW(
        makeRnicAtlahsRuntime(event_list,
                              RnicProfile::CollectiveNetwork,
                              collectiveConfig(kAccessCapacityBps / 2),
                              twoTierConfig()),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     PacketizedManifoldOwnsNoTopologyAndPreservesPhysicalParameters) {
    EventList& event_list = testEventList();
    const RnicPacketizedManifoldRuntimeConfig config{
        400000000000ULL,
        RnicDataPacketizationConfig(2048, 64),
        timeFromNs(350)};

    auto assembly = makeRnicAtlahsRuntime(
        event_list, RnicProfile::PacketizedManifold, config);

    ASSERT_NE(assembly, nullptr);
    EXPECT_EQ(assembly->profileSpec().profile,
              RnicProfile::PacketizedManifold);
    EXPECT_EQ(assembly->profileSpec().fabric,
              RnicFabricModel::TopologyFreeManifold);
    EXPECT_EQ(assembly->topologyConfig(), nullptr);
    EXPECT_EQ(assembly->physicalTopology(), nullptr);
    auto* const runtime = dynamic_cast<RnicPacketizedManifoldRuntime*>(
        &assembly->implementation());
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->nodeLinkCapacity(), config.node_link_capacity_bps);
    EXPECT_EQ(runtime->packetization().maxWirePacketBytes(), 2048U);
    EXPECT_EQ(runtime->packetization().dataHeaderBytes(), 64U);
    EXPECT_EQ(runtime->propagationDelay(), config.propagation_delay_ps);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     FluidManifoldOwnsNoTopologyAndPreservesPhysicalParameters) {
    EventList& event_list = testEventList();
    const RnicFluidManifoldRuntimeConfig config{
        200000000000ULL, timeFromNs(725)};

    auto assembly = makeRnicAtlahsRuntime(
        event_list, RnicProfile::FluidManifold, config);

    ASSERT_NE(assembly, nullptr);
    EXPECT_EQ(assembly->profileSpec().profile,
              RnicProfile::FluidManifold);
    EXPECT_EQ(assembly->profileSpec().fabric,
              RnicFabricModel::TopologyFreeManifold);
    EXPECT_EQ(assembly->topologyConfig(), nullptr);
    EXPECT_EQ(assembly->physicalTopology(), nullptr);
    auto* const runtime = dynamic_cast<RnicFluidManifoldRuntime*>(
        &assembly->implementation());
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->nodeLinkCapacity(), config.node_link_capacity_bps);
    EXPECT_EQ(runtime->propagationDelay(), config.propagation_delay_ps);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     BothTopologyFreeProfilesRejectAClosConfiguration) {
    EventList& event_list = testEventList();

    EXPECT_THROW(
        makeRnicAtlahsRuntime(
            event_list,
            RnicProfile::PacketizedManifold,
            RnicPacketizedManifoldRuntimeConfig{
                kAccessCapacityBps,
                RnicDataPacketizationConfig(1000, 64),
                timeFromNs(100)},
            twoTierConfig()),
        std::invalid_argument);
    EXPECT_THROW(
        makeRnicAtlahsRuntime(
            event_list,
            RnicProfile::FluidManifold,
            RnicFluidManifoldRuntimeConfig{
                kAccessCapacityBps, timeFromNs(100)},
            twoTierConfig()),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     RejectsProfileAndTypedRuntimeConfigurationMismatch) {
    EventList& event_list = testEventList();

    EXPECT_THROW(
        makeRnicAtlahsRuntime(
            event_list,
            RnicProfile::PacketizedManifold,
            RnicFluidManifoldRuntimeConfig{
                kAccessCapacityBps, timeFromNs(100)}),
        std::invalid_argument);
    EXPECT_THROW(
        makeRnicAtlahsRuntime(
            event_list,
            RnicProfile::FluidManifold,
            RnicPacketizedManifoldRuntimeConfig{
                kAccessCapacityBps,
                RnicDataPacketizationConfig(1000, 64),
                timeFromNs(100)}),
        std::invalid_argument);
    EXPECT_THROW(
        makeRnicAtlahsRuntime(
            event_list,
            RnicProfile::CollectiveNetwork,
            RnicFluidManifoldRuntimeConfig{
                kAccessCapacityBps, timeFromNs(100)},
            twoTierConfig()),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest, RejectsAnInvalidProfileEnum) {
    EventList& event_list = testEventList();

    EXPECT_THROW(
        makeRnicAtlahsRuntime(
            event_list,
            static_cast<RnicProfile>(99),
            RnicFluidManifoldRuntimeConfig{
                kAccessCapacityBps, timeFromNs(100)}),
        std::invalid_argument);
}

TEST(RnicAtlahsRuntimeFactoryTest, SessionRejectsMissingImplementation) {
    EXPECT_THROW(
        std::make_unique<RnicAtlahsRuntimeAssembly>(
            nullptr,
            nullptr,
            nullptr,
            resolveRnicProfile(RnicProfile::PacketizedManifold)),
        std::invalid_argument);
}

}  // namespace
