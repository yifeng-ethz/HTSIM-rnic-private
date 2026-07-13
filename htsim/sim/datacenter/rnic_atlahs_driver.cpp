// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_atlahs_driver.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "fat_tree_topology.h"
#include "rnic_packetized_manifold.h"
#include "rnic_prbs_pacer.h"

namespace {

const char* resolvedGoalRankMappingName(
        AtlahsHtsimApi::GoalRankMapping mapping) {
    switch (mapping) {
    case AtlahsHtsimApi::GoalRankMapping::GpuRank:
        return "gpu-rank";
    case AtlahsHtsimApi::GoalRankMapping::UniqueNic:
        return "unique-nic";
    }
    throw std::invalid_argument("invalid resolved GOAL rank mapping");
}

std::unique_ptr<FatTreeTopologyCfg> makeCollectiveTopologyConfig(
        const RnicAtlahsCliOptions& options,
        std::uint32_t physical_node_count) {
    const auto buffer_bytes = static_cast<mem_b>(
        options.collective.tomahawk3_shared_buffer_bytes);
    std::unique_ptr<FatTreeTopologyCfg> topology_config;
    if (options.collective.topology_file.has_value()) {
        std::ifstream topology_probe(*options.collective.topology_file);
        if (!topology_probe.is_open()) {
            throw std::invalid_argument(
                "cannot open rnic-cn topology file '"
                + *options.collective.topology_file + "'");
        }
        topology_config = FatTreeTopologyCfg::load(
            *options.collective.topology_file,
            buffer_bytes,
            COMPOSITE,
            FAIR_PRIO);
        if (topology_config == nullptr) {
            throw std::runtime_error(
                "rnic-cn topology loader returned no configuration");
        }
        if (topology_config->no_of_nodes() != physical_node_count) {
            throw std::invalid_argument(
                "rnic-cn topology node count does not match resolved GOAL "
                "layout");
        }
    } else {
        if (!isRnicGeneratedTwoTierClosNodeCount(physical_node_count)) {
            throw std::invalid_argument(
                "generated rnic-cn topology requires GOAL nodes = K^2/2 "
                "for even K");
        }
        topology_config = std::make_unique<FatTreeTopologyCfg>(
            2,
            physical_node_count,
            options.link_capacity_bps,
            buffer_bytes,
            options.collective.hop_latency_ps,
            options.collective.switch_latency_ps,
            COMPOSITE,
            FAIR_PRIO);
    }
    topology_config->set_tomahawk3_shared_buffer_capacity(buffer_bytes);
    return topology_config;
}

RnicAtlahsRuntimeConfig collectiveRuntimeConfig(
        const RnicAtlahsCliOptions& options) {
    return RnicCollectiveNetworkConfig{
        options.link_capacity_bps,
        RnicDataPacketizationConfig(
            options.packet.max_wire_packet_bytes,
            options.packet.data_header_bytes),
        RnicRingCamConfig{
            options.collective.ring_delay_window_ps,
            options.collective.ring_release_tick_ps,
            options.collective.ring_wire_capacity_bytes},
        options.collective.global_prbs_seed,
        options.collective.control_deadline_ps,
        options.collective.margin_ppm,
        options.collective.control_wire_bytes,
        {},
    };
}

}  // namespace

std::unique_ptr<RnicAtlahsRuntimeAssembly> assembleRnicAtlahsProfile(
        EventList& event_list,
        const RnicAtlahsCliOptions& options,
        std::uint32_t physical_node_count,
        QueueLoggerFactory* logger_factory) {
    if (physical_node_count == 0) {
        throw std::invalid_argument(
            "RNIC ATLAHS profile requires a positive GOAL node count");
    }

    switch (options.profile) {
    case RnicProfile::CollectiveNetwork:
        return makeRnicAtlahsRuntime(
            event_list,
            options.profile,
            collectiveRuntimeConfig(options),
            makeCollectiveTopologyConfig(options, physical_node_count),
            logger_factory);
    case RnicProfile::PacketizedManifold:
        return makeRnicAtlahsRuntime(
            event_list,
            options.profile,
            RnicPacketizedManifoldRuntimeConfig{
                options.link_capacity_bps,
                RnicDataPacketizationConfig(
                    options.packet.max_wire_packet_bytes,
                    options.packet.data_header_bytes),
                options.manifold.fixed_propagation_delay_ps});
    case RnicProfile::FluidManifold:
        return makeRnicAtlahsRuntime(
            event_list,
            options.profile,
            RnicFluidManifoldRuntimeConfig{
                options.link_capacity_bps,
                options.manifold.fixed_propagation_delay_ps});
    }
    throw std::invalid_argument("invalid RNIC profile enum");
}

std::string renderRnicAtlahsModelManifest(
        const RnicAtlahsCliOptions& options,
        const AtlahsHtsimApi::GoalLayout& goal_layout,
        const RnicAtlahsRuntimeAssembly& assembly) {
    const RnicProfileSpec& spec = assembly.profileSpec();
    if (spec.profile != options.profile) {
        throw std::logic_error(
            "RNIC manifest profile differs from assembled runtime");
    }

    std::ostringstream manifest;
    manifest
        << "[RNIC manifest] schema=rnic-atlahs-model-v1"
        << " profile=" << rnicProfileName(spec.profile)
        << " fabric=" << rnicFabricModelName(spec.fabric)
        << " traffic=" << rnicTrafficModelName(spec.traffic)
        << " control=" << rnicControlModelName(spec.control)
        << " pacer=" << rnicPacerModelName(spec.pacer)
        << " rb="
        << (spec.profile == RnicProfile::CollectiveNetwork
                ? "ring-cam" : "none")
        << '\n';
    manifest
        << "[RNIC manifest] goal=" << options.goal_file
        << " requested_rank_mapping="
        << rnicAtlahsGoalRankMappingName(options.goal_rank_mapping)
        << " resolved_rank_mapping="
        << resolvedGoalRankMappingName(goal_layout.rank_mapping)
        << " ranks=" << goal_layout.rank_count
        << " cpus=" << goal_layout.cpu_count
        << " nics=" << goal_layout.nic_count
        << " physical_nodes=" << goal_layout.physical_node_count
        << " endpoint_link_bps=" << options.link_capacity_bps
        << '\n';

    if (spec.traffic == RnicTrafficModel::Packetized) {
        manifest
            << "[RNIC manifest] max_wire_packet_bytes="
            << options.packet.max_wire_packet_bytes
            << " data_header_bytes=" << options.packet.data_header_bytes
            << " final_packet=exact-wire-extent"
            << " accounting=payload-and-wire"
            << '\n';
    }

    if (spec.profile == RnicProfile::CollectiveNetwork) {
        manifest
            << "[RNIC manifest] topology="
            << (options.collective.topology_file.has_value()
                    ? *options.collective.topology_file
                    : "generated-two-tier")
            << " clos_tiers=2 switch=Tomahawk3"
            << " voq_key=physical-ingress-x-physical-egress"
            << " arbitration=deterministic-round-robin"
            << " routing=per-packet-path-round-robin"
            << " control_priority=strict-nonpreemptive"
            << " pfc=off ecn=off"
            << " shared_buffer_bytes="
            << options.collective.tomahawk3_shared_buffer_bytes;
        if (!options.collective.topology_file.has_value()) {
            manifest
                << " hop_latency_ps="
                << options.collective.hop_latency_ps
                << " switch_latency_ps="
                << options.collective.switch_latency_ps;
        } else {
            manifest << " latency_source=topology-file";
        }
        manifest << '\n';
        manifest
            << "[RNIC manifest] declaration_gate=physical-accept"
            << " grant=margin*C/active-receiver-flows"
            << " margin_ppm=" << options.collective.margin_ppm
            << " control_deadline_ps="
            << options.collective.control_deadline_ps
            << " control_wire_bytes="
            << options.collective.control_wire_bytes
            << " retirement=physical-retire-after-final-data"
            << '\n';
        manifest
            << "[RNIC manifest] prbs_algorithm=galois-lfsr64"
            << " prbs_version=" << RnicPrbsPacer::kAlgorithmVersion
            << " polynomial=x^64+x^63+x^61+x^60+1"
            << " feedback_mask=0xd800000000000000"
            << " seed_derivation=splitmix64-pair-v1"
            << " global_seed=" << options.collective.global_prbs_seed
            << " mixed_extent_hazard=adaptive-fixed-point-q32"
            << " tie_break=flow-id"
            << '\n';
        manifest
            << "[RNIC manifest] ring_window_ps="
            << options.collective.ring_delay_window_ps
            << " ring_tick_ps="
            << options.collective.ring_release_tick_ps
            << " ring_capacity_wire_bytes="
            << options.collective.ring_wire_capacity_bytes
            << " scope=node-shared"
            << " eta=source-route-injection-plus-calibrated-transit"
            << " same_time_order=release-before-admission"
            << '\n';
    } else {
        manifest
            << "[RNIC manifest] topology=none"
            << " fixed_propagation_ps="
            << options.manifold.fixed_propagation_delay_ps
            << " allocation=instant-central-max-min"
            << " edge_constraints=source-and-destination"
            << " manifold_queue=none"
            << " loss=none backpressure=none"
            << '\n';
        if (spec.profile == RnicProfile::PacketizedManifold) {
            manifest
                << "[RNIC manifest] packet_scheduler_version="
                << RnicPacketizedSlotCalendar::kSchedulerVersion
                << " matching=collision-free-saturated-port-first"
                << " final_tail_serialization=exact"
                << " calendar_reservation=full-envelope"
                << '\n';
        } else {
            manifest
                << "[RNIC manifest] service=continuous-fluid"
                << " allocation_arithmetic=exact-rational"
                << " bps_policy=componentwise-floor"
                << " completion_time=picosecond-ceiling"
                << " propagation=after-last-serviced-bit"
                << '\n';
        }
    }
    return manifest.str();
}
