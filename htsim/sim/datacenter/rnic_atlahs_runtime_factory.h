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

// One API-owned runtime session.  Keeping the implementation, physical
// topology, and topology configuration in this same polymorphic object makes
// their lifetime relationship structural: moving the session into
// AtlahsHtsimApi cannot detach a CN runtime from the objects it references.
// Member destruction runs in reverse, so the implementation goes first,
// followed by the topology and then its configuration.
class RnicAtlahsRuntimeAssembly final : public AtlahsFlowRuntime {
public:
    RnicAtlahsRuntimeAssembly(
        std::unique_ptr<FatTreeTopologyCfg> topology_config_value,
        std::unique_ptr<FatTreeTopology> topology_value,
        std::unique_ptr<AtlahsFlowRuntime> runtime_value,
        RnicProfileSpec profile_spec_value);
    ~RnicAtlahsRuntimeAssembly() override;

    RnicAtlahsRuntimeAssembly(const RnicAtlahsRuntimeAssembly&) = delete;
    RnicAtlahsRuntimeAssembly& operator=(
        const RnicAtlahsRuntimeAssembly&) = delete;
    RnicAtlahsRuntimeAssembly(RnicAtlahsRuntimeAssembly&&) = delete;
    RnicAtlahsRuntimeAssembly& operator=(
        RnicAtlahsRuntimeAssembly&&) = delete;

    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;
    bool hasPendingPhysicalWork() const noexcept override;

    FatTreeTopologyCfg* topologyConfig() noexcept {
        return topology_config.get();
    }
    const FatTreeTopologyCfg* topologyConfig() const noexcept {
        return topology_config.get();
    }
    FatTreeTopology* physicalTopology() noexcept { return topology.get(); }
    const FatTreeTopology* physicalTopology() const noexcept {
        return topology.get();
    }
    AtlahsFlowRuntime& implementation() noexcept { return *runtime; }
    const AtlahsFlowRuntime& implementation() const noexcept {
        return *runtime;
    }
    const RnicProfileSpec& profileSpec() const noexcept {
        return profile_spec;
    }

private:
    std::unique_ptr<FatTreeTopologyCfg> topology_config;
    std::unique_ptr<FatTreeTopology> topology;
    std::unique_ptr<AtlahsFlowRuntime> runtime;
    RnicProfileSpec profile_spec;
};

std::unique_ptr<RnicAtlahsRuntimeAssembly> makeRnicAtlahsRuntime(
    EventList& event_list,
    RnicProfile profile,
    RnicAtlahsRuntimeConfig runtime_config,
    std::unique_ptr<FatTreeTopologyCfg> topology_config = nullptr,
    QueueLoggerFactory* logger_factory = nullptr);

#endif  // RNIC_ATLAHS_RUNTIME_FACTORY_H
