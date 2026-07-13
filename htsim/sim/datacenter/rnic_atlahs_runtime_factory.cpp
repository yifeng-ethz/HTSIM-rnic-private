// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_atlahs_runtime_factory.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "fat_tree_topology.h"
#include "rnic_fluid_manifold_runtime.h"
#include "rnic_packetized_manifold_runtime.h"

namespace {

template <typename Config>
Config& requireRuntimeConfig(RnicAtlahsRuntimeConfig& runtime_config,
                             RnicProfile profile) {
    Config* const config = std::get_if<Config>(&runtime_config);
    if (config == nullptr) {
        throw std::invalid_argument(
            std::string(rnicProfileName(profile))
            + " received a runtime configuration for a different profile");
    }
    return *config;
}

void rejectTopologyConfig(
        const std::unique_ptr<FatTreeTopologyCfg>& topology_config,
        RnicProfile profile) {
    if (topology_config != nullptr) {
        throw std::invalid_argument(
            std::string(rnicProfileName(profile))
            + " is topology-free and rejects a FatTree configuration");
    }
}

}  // namespace

RnicAtlahsRuntimeAssembly::RnicAtlahsRuntimeAssembly(
        std::unique_ptr<FatTreeTopologyCfg> topology_config_value,
        std::unique_ptr<FatTreeTopology> topology_value,
        std::unique_ptr<AtlahsFlowRuntime> runtime_value,
        RnicProfileSpec profile_spec_value)
    : topology_config(std::move(topology_config_value)),
      topology(std::move(topology_value)),
      runtime(std::move(runtime_value)),
      profile_spec(profile_spec_value) {
    if (runtime == nullptr) {
        throw std::invalid_argument(
            "RNIC ATLAHS session requires a runtime implementation");
    }
}

RnicAtlahsRuntimeAssembly::~RnicAtlahsRuntimeAssembly() = default;

void RnicAtlahsRuntimeAssembly::setup(
        std::uint32_t node_count, CompletionHandler complete_flow) {
    runtime->setup(node_count, std::move(complete_flow));
}

void RnicAtlahsRuntimeAssembly::send(
        const AtlahsFlowRequest& request) {
    runtime->send(request);
}

bool RnicAtlahsRuntimeAssembly::hasPendingPhysicalWork() const noexcept {
    return runtime->hasPendingPhysicalWork();
}

std::unique_ptr<RnicAtlahsRuntimeAssembly> makeRnicAtlahsRuntime(
        EventList& event_list,
        RnicProfile profile,
        RnicAtlahsRuntimeConfig runtime_config,
        std::unique_ptr<FatTreeTopologyCfg> topology_config,
        QueueLoggerFactory* logger_factory) {
    const RnicProfileSpec profile_spec = resolveRnicProfile(profile);

    switch (profile) {
    case RnicProfile::CollectiveNetwork: {
        RnicCollectiveNetworkConfig& config =
            requireRuntimeConfig<RnicCollectiveNetworkConfig>(
                runtime_config, profile);
        if (topology_config == nullptr) {
            throw std::invalid_argument(
                "rnic-cn requires a two-tier FatTree configuration");
        }
        if (topology_config->get_tiers() != 2) {
            throw std::invalid_argument(
                "rnic-cn requires a two-tier FatTree configuration");
        }
        if (topology_config->uses_pause_flow_control()) {
            throw std::invalid_argument(
                "rnic-cn rejects PAUSE/PFC and lossless queue modes");
        }
        if (topology_config->downlink_speed(TOR_TIER)
            != config.access_wire_capacity_bps) {
            throw std::invalid_argument(
                "rnic-cn access wire rate must equal the physical ToR "
                "downlink rate");
        }

        // Tomahawk3 is the physical traffic manager for every Clos profile.
        // Set it at the assembly boundary so a caller cannot accidentally
        // construct the legacy switch model and only discover that later.
        topology_config->set_switch_model(FatTreeSwitchModel::Tomahawk3);
        if (topology_config->switch_model()
            != FatTreeSwitchModel::Tomahawk3) {
            throw std::logic_error(
                "rnic-cn failed to select the Tomahawk3 switch model");
        }

        // ETA is a no-queue physical baseline, not a caller-selected model
        // parameter. The captured configuration has assembly lifetime and is
        // destroyed only after the runtime and topology.
        FatTreeTopologyCfg* const owned_topology_config =
            topology_config.get();
        config.calibrated_transit_ps =
            [owned_topology_config](std::uint32_t source,
                                    std::uint32_t destination) {
                return owned_topology_config
                    ->get_two_point_diameter_latency(
                        static_cast<int>(source),
                        static_cast<int>(destination));
            };

        auto topology = std::make_unique<FatTreeTopology>(
            topology_config.get(), logger_factory, &event_list, nullptr);
        auto runtime = std::make_unique<RnicCollectiveNetworkRuntime>(
            event_list, *topology, std::move(config));
        return std::make_unique<RnicAtlahsRuntimeAssembly>(
            std::move(topology_config),
            std::move(topology),
            std::move(runtime),
            profile_spec);
    }
    case RnicProfile::PacketizedManifold: {
        rejectTopologyConfig(topology_config, profile);
        RnicPacketizedManifoldRuntimeConfig& config =
            requireRuntimeConfig<RnicPacketizedManifoldRuntimeConfig>(
                runtime_config, profile);
        auto runtime = std::make_unique<RnicPacketizedManifoldRuntime>(
            event_list,
            config.node_link_capacity_bps,
            config.packetization,
            config.propagation_delay_ps);
        return std::make_unique<RnicAtlahsRuntimeAssembly>(
            nullptr, nullptr, std::move(runtime), profile_spec);
    }
    case RnicProfile::FluidManifold: {
        rejectTopologyConfig(topology_config, profile);
        RnicFluidManifoldRuntimeConfig& config =
            requireRuntimeConfig<RnicFluidManifoldRuntimeConfig>(
                runtime_config, profile);
        auto runtime = std::make_unique<RnicFluidManifoldRuntime>(
            event_list,
            config.node_link_capacity_bps,
            config.propagation_delay_ps);
        return std::make_unique<RnicAtlahsRuntimeAssembly>(
            nullptr, nullptr, std::move(runtime), profile_spec);
    }
    }

    throw std::invalid_argument("invalid RNIC profile enum");
}
