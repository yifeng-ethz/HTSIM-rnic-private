// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "fat_tree_topology.h"
#include "rnic_atlahs_runtime_factory.h"
#include "rnic_fluid_manifold_runtime.h"
#include "rnic_packetized_manifold_runtime.h"

namespace {

constexpr std::uint64_t kAccessCapacityBps = 100000000000ULL;

static_assert(std::is_move_constructible_v<RnicAtlahsRuntimeAssembly>);
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
        [](std::uint32_t, std::uint32_t) {
            return timeFromNs(500);
        },
    };
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkOwnsForcedTomahawk3ClosAndRuntime) {
    EventList& event_list = testEventList();
    std::unique_ptr<FatTreeTopologyCfg> input_config = twoTierConfig();
    FatTreeTopologyCfg* const input_config_address = input_config.get();
    EXPECT_EQ(input_config->switch_model(), FatTreeSwitchModel::Default);

    RnicAtlahsRuntimeAssembly assembly = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::CollectiveNetwork,
        collectiveConfig(),
        std::move(input_config));

    EXPECT_EQ(assembly.profile_spec.profile,
              RnicProfile::CollectiveNetwork);
    EXPECT_EQ(assembly.profile_spec.fabric,
              RnicFabricModel::Tomahawk3Clos);
    ASSERT_NE(assembly.topology_config, nullptr);
    ASSERT_NE(assembly.topology, nullptr);
    ASSERT_NE(assembly.runtime, nullptr);
    EXPECT_EQ(assembly.topology_config.get(), input_config_address);
    EXPECT_EQ(&assembly.topology->cfg(), input_config_address);
    EXPECT_EQ(assembly.topology_config->switch_model(),
              FatTreeSwitchModel::Tomahawk3);
    EXPECT_NE(dynamic_cast<RnicCollectiveNetworkRuntime*>(
                  assembly.runtime.get()),
              nullptr);
}

TEST(RnicAtlahsRuntimeFactoryTest,
     CollectiveNetworkCalibratesEtaFromItsOwnedPhysicalClos) {
    EventList& event_list = testEventList();
    std::size_t caller_calibration_calls = 0;
    RnicCollectiveNetworkConfig config = collectiveConfig();
    config.calibrated_transit_ps =
        [&caller_calibration_calls](std::uint32_t, std::uint32_t) {
            ++caller_calibration_calls;
            return timeFromNs(1);
        };
    RnicAtlahsRuntimeAssembly assembly = makeRnicAtlahsRuntime(
        event_list,
        RnicProfile::CollectiveNetwork,
        std::move(config),
        twoTierConfig());
    auto* const runtime = dynamic_cast<RnicCollectiveNetworkRuntime*>(
        assembly.runtime.get());
    ASSERT_NE(runtime, nullptr);

    bool completed = false;
    runtime->setup(32, [&completed](AtlahsFlowId) { completed = true; });
    runtime->send({0x100000001ULL,
                   0,
                   31,
                   0,
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
    EXPECT_TRUE(completed);
    EXPECT_EQ(caller_calibration_calls, 0U);
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

    RnicAtlahsRuntimeAssembly assembly = makeRnicAtlahsRuntime(
        event_list, RnicProfile::PacketizedManifold, config);

    EXPECT_EQ(assembly.profile_spec.profile,
              RnicProfile::PacketizedManifold);
    EXPECT_EQ(assembly.profile_spec.fabric,
              RnicFabricModel::TopologyFreeManifold);
    EXPECT_EQ(assembly.topology_config, nullptr);
    EXPECT_EQ(assembly.topology, nullptr);
    auto* const runtime = dynamic_cast<RnicPacketizedManifoldRuntime*>(
        assembly.runtime.get());
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

    RnicAtlahsRuntimeAssembly assembly = makeRnicAtlahsRuntime(
        event_list, RnicProfile::FluidManifold, config);

    EXPECT_EQ(assembly.profile_spec.profile,
              RnicProfile::FluidManifold);
    EXPECT_EQ(assembly.profile_spec.fabric,
              RnicFabricModel::TopologyFreeManifold);
    EXPECT_EQ(assembly.topology_config, nullptr);
    EXPECT_EQ(assembly.topology, nullptr);
    auto* const runtime = dynamic_cast<RnicFluidManifoldRuntime*>(
        assembly.runtime.get());
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

}  // namespace
