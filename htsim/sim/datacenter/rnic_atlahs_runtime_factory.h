// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_ATLAHS_RUNTIME_FACTORY_H
#define RNIC_ATLAHS_RUNTIME_FACTORY_H

#include <cstdint>
#include <memory>
#include <variant>

#include "atlahs_flow_runtime.h"
#include "rnic_collective_network_runtime.h"
#include "rnic_packet_extent.h"
#include "rnic_profile.h"

class FatTreeTopology;
class FatTreeTopologyCfg;
class QueueLoggerFactory;

struct RnicPacketizedManifoldRuntimeConfig {
    std::uint64_t node_link_capacity_bps;
    RnicDataPacketizationConfig packetization;
    std::uint64_t propagation_delay_ps;
};

struct RnicFluidManifoldRuntimeConfig {
    std::uint64_t node_link_capacity_bps;
    std::uint64_t propagation_delay_ps;
};

using RnicAtlahsRuntimeConfig =
    std::variant<RnicCollectiveNetworkConfig,
                 RnicPacketizedManifoldRuntimeConfig,
                 RnicFluidManifoldRuntimeConfig>;

// Member order is an ownership contract. Destruction runs in reverse, so the
// runtime releases all references before its physical topology, and the
// topology releases its references before the FatTree configuration.
struct RnicAtlahsRuntimeAssembly {
    RnicAtlahsRuntimeAssembly(
        std::unique_ptr<FatTreeTopologyCfg> topology_config_value,
        std::unique_ptr<FatTreeTopology> topology_value,
        std::unique_ptr<AtlahsFlowRuntime> runtime_value,
        RnicProfileSpec profile_spec_value);
    ~RnicAtlahsRuntimeAssembly();

    RnicAtlahsRuntimeAssembly(RnicAtlahsRuntimeAssembly&&) noexcept;
    RnicAtlahsRuntimeAssembly& operator=(
        RnicAtlahsRuntimeAssembly&&) noexcept = delete;
    RnicAtlahsRuntimeAssembly(const RnicAtlahsRuntimeAssembly&) = delete;
    RnicAtlahsRuntimeAssembly& operator=(
        const RnicAtlahsRuntimeAssembly&) = delete;

    std::unique_ptr<FatTreeTopologyCfg> topology_config;
    std::unique_ptr<FatTreeTopology> topology;
    std::unique_ptr<AtlahsFlowRuntime> runtime;
    RnicProfileSpec profile_spec;
};

RnicAtlahsRuntimeAssembly makeRnicAtlahsRuntime(
    EventList& event_list,
    RnicProfile profile,
    RnicAtlahsRuntimeConfig runtime_config,
    std::unique_ptr<FatTreeTopologyCfg> topology_config = nullptr,
    QueueLoggerFactory* logger_factory = nullptr);

#endif  // RNIC_ATLAHS_RUNTIME_FACTORY_H
