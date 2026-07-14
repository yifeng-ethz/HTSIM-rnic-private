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
        options.collective.ns_tm3_shared_buffer_bytes);
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
    topology_config->set_ns_tm3_shared_buffer_capacity(buffer_bytes);
    return topology_config;
}

std::unique_ptr<FatTreeTopologyCfg> makeSlingshotTopologyConfig(
        const RnicAtlahsCliOptions& options,
        std::uint32_t physical_node_count) {
    const auto buffer_bytes = static_cast<mem_b>(
        options.slingshot.ns_rosetta_shared_buffer_bytes);
    std::unique_ptr<FatTreeTopologyCfg> topology_config;
    if (options.collective.topology_file.has_value()) {
        std::ifstream topology_probe(*options.collective.topology_file);
        if (!topology_probe.is_open()) {
            throw std::invalid_argument(
                "cannot open rnic-ss topology file '"
                + *options.collective.topology_file + "'");
        }
        topology_config = FatTreeTopologyCfg::load(
            *options.collective.topology_file,
            buffer_bytes,
            COMPOSITE,
            FAIR_PRIO);
        if (topology_config == nullptr) {
            throw std::runtime_error(
                "rnic-ss topology loader returned no configuration");
        }
        if (topology_config->no_of_nodes() != physical_node_count) {
            throw std::invalid_argument(
                "rnic-ss topology node count does not match GOAL layout");
        }
    } else {
        if (!isRnicGeneratedTwoTierClosNodeCount(physical_node_count)) {
            throw std::invalid_argument(
                "generated rnic-ss topology requires GOAL nodes = K^2/2 "
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
    topology_config->set_ns_rosetta_shared_buffer_capacity(buffer_bytes);
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
        options.collective.queue_trace_csv,
        options.collective.state_trace_csv,
        options.collective.maximum_repair_retries,
    };
}

RnicAtlahsRuntimeConfig slingshotRuntimeConfig(
        const RnicAtlahsCliOptions& options) {
    return RnicSsRuntimeConfig{
        options.link_capacity_bps,
        RnicDataPacketizationConfig(
            options.packet.max_wire_packet_bytes,
            options.packet.data_header_bytes),
        options.slingshot.control_wire_bytes,
        RnicSsSelectiveRepeatConfig{
            options.slingshot.outstanding_window_packets,
            options.slingshot.rto_ps,
            options.slingshot.maximum_retransmissions},
        RnicSsPathSelectionConfig{
            8,
            4,
            options.slingshot.path_hysteresis_ps,
            options.slingshot.maximum_sample_age_ps,
            0},
        RnicSsCreditConfig{
            options.slingshot.maximum_credit_ahead_bytes},
        options.slingshot.q_hi_bytes,
        options.slingshot.q_lo_bytes,
        options.slingshot.credit_quantum_packets,
        options.slingshot.telemetry_delay_ps,
        options.slingshot.routing_seed,
        options.slingshot.unordered_packet_routing,
        options.slingshot.allow_loss_stress,
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
    case RnicProfile::SlingshotLike:
        return makeRnicAtlahsRuntime(
            event_list,
            options.profile,
            slingshotRuntimeConfig(options),
            makeSlingshotTopologyConfig(options, physical_node_count),
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
                ? "ring-cam"
                : (spec.profile == RnicProfile::SlingshotLike
                       ? "voq-request-grant" : "none"))
        << '\n';
    manifest
        << "[RNIC manifest] goal=" << options.goal_file
        << " completion_csv="
        << (options.completion_csv.has_value()
                ? *options.completion_csv : "off")
        << " state_trace_csv="
        << (options.collective.state_trace_csv.has_value()
                ? *options.collective.state_trace_csv : "off")
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
            << " clos_tiers=2 switch=ns-tm3"
            << " link_failures=0"
            << " eta_calibration_config=construction-snapshot"
            << " voq_key=physical-ingress-x-physical-egress"
            << " arbitration=strict-priority-oldest-hol"
            << " routing=per-packet-path-round-robin"
            << " control_priority=strict-nonpreemptive"
            << " pfc=off ecn=off"
            << " shared_buffer_bytes="
            << options.collective.ns_tm3_shared_buffer_bytes;
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
            << " declare_cca_field=nflow"
            << " startup_nflow=1"
            << " declare_debug_fields=collective_id,expected_fan_in"
            << " declare_debug_affects_rx_cca=false"
            << " grant=margin*C/n_hat"
            << " n_hat=sum-active-declare-nflow"
            << " margin_ppm=" << options.collective.margin_ppm
            << " control_deadline_ps="
            << options.collective.control_deadline_ps
            << " control_wire_bytes="
            << options.collective.control_wire_bytes
            << " retirement=physical-retire-after-final-data"
            << '\n';
        manifest
            << "[RNIC manifest] prbs_algorithm="
            << RnicPrbsPacer::kAlgorithmName
            << " prbs_version=" << RnicPrbsPacer::kAlgorithmVersion
            << " polynomial=x^64+x^63+x^61+x^60+1"
            << " feedback_mask=0xd800000000000000"
            << " word_extraction=" << RnicPrbsPacer::kWordExtraction
            << " lfsr_steps_per_word="
            << RnicPrbsPacer::kLfsrStepsPerWord
            << " bounded_draw=" << RnicPrbsPacer::kBoundedDraw
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
            << " eta=source-route-injection-plus-packet-specific-no-load-transit"
            << " eta_transit=pipe-switch-latency-plus-remaining-ns-tm3-egress-serialization"
            << " same_time_order=release-before-admission"
            << '\n';
        manifest
            << "[RNIC manifest] recovery=selective-gap-nack"
            << " late_admission=repair"
            << " early_admission=hard-error"
            << " overflow_admission=hard-error"
            << " gap_decision=next-strict-ring-tick"
            << " gap_decision_epsilon_ps=0"
            << " gap_nack_priority=high"
            << " gap_nack_wire_bytes="
            << options.collective.control_wire_bytes
            << " repair_priority=low"
            << " repair_arbitration=per-flow-head-in-node-prbs-lottery"
            << " repair_prbs_draw=true"
            << " repair_rate_accounting=substitutes-fresh-granted-service"
            << " repair_eta=fresh-packet-specific-calibration"
            << " repair_scope=exact-logical-packet"
            << " maximum_repair_retries="
            << options.collective.maximum_repair_retries
            << " retirement_gate=exact-rx-ledger-and-no-gap"
            << '\n';
    } else if (spec.profile == RnicProfile::SlingshotLike) {
        manifest
            << "[RNIC manifest] topology="
            << (options.collective.topology_file.has_value()
                    ? *options.collective.topology_file
                    : "generated-two-tier")
            << " clos_tiers=2 switch=ns-rosetta"
            << " link_failures=0 pfc=off ecn=off"
            << " switch_queue=input-voq-request-grant"
            << " source_serializer=physical-host-queue"
            << " shared_buffer_bytes="
            << options.slingshot.ns_rosetta_shared_buffer_bytes;
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
            << "[RNIC manifest] model=open-slingshot-like-comparator"
            << " proprietary_threshold_claim=false"
            << " routing="
            << (options.slingshot.unordered_packet_routing
                    ? "unordered-packet" : "ordered-endpoint-pair")
            << " path_candidates=deterministic-four-of-eight"
            << " sender_information=local-request-depth-plus-physical-ack-telemetry"
            << " global_sender_state=false"
            << " telemetry_delay_ps="
            << options.slingshot.telemetry_delay_ps
            << " sample_age_ps="
            << options.slingshot.maximum_sample_age_ps
            << " path_hysteresis_ps="
            << options.slingshot.path_hysteresis_ps
            << " routing_seed=" << options.slingshot.routing_seed
            << '\n';
        manifest
            << "[RNIC manifest] pair_tracking=endpoint-pair-outstanding"
            << " ack=per-data-packet-sack128"
            << " recovery=selective-repeat"
            << " sack_hole_retry=immediate"
            << " silent_loss_rto_ps=" << options.slingshot.rto_ps
            << " normal_lossless_rto_expected=0"
            << " loss_stress="
            << (options.slingshot.allow_loss_stress ? "on" : "off")
            << " window_packets="
            << options.slingshot.outstanding_window_packets
            << " maximum_retransmissions="
            << options.slingshot.maximum_retransmissions
            << '\n';
        manifest
            << "[RNIC manifest] backpressure=pair-selective-physical"
            << " credit=service-driven-cumulative"
            << " control_priority=strict-nonpreemptive"
            << " q_hi_bytes=" << options.slingshot.q_hi_bytes
            << " q_lo_bytes=" << options.slingshot.q_lo_bytes
            << " credit_quantum_packets="
            << options.slingshot.credit_quantum_packets
            << " max_credit_ahead_bytes="
            << options.slingshot.maximum_credit_ahead_bytes
            << " control_wire_bytes="
            << options.slingshot.control_wire_bytes
            << '\n';
        manifest
            << "[RNIC manifest] queue_bound="
               "controlled-clos-analytical-envelope"
            << " bound_forward_data_serialization=included"
            << " bound_bp_enable_fan_in="
            << (goal_layout.physical_node_count == 0
                    ? 0 : goal_layout.physical_node_count - 1)
            << " bound_bp_enable_serialization="
               "destination-source-serializer"
            << " bound_reverse_blocking="
               "one-max-data-per-nonpreemptive-serializer"
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
