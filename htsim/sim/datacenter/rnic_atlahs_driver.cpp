// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_atlahs_driver.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "fat_tree_topology.h"
#include "rnic_packetized_manifold.h"
#include "rnic_prbs_pacer.h"

namespace {

const char* resolvedGoalRankMappingName(AtlahsHtsimApi::GoalRankMapping mapping) {
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
    const auto buffer_bytes = static_cast<mem_b>(options.collective.ns_tm3_shared_buffer_bytes);
    std::unique_ptr<FatTreeTopologyCfg> topology_config;
    if (options.collective.topology_file.has_value()) {
        std::ifstream topology_probe(*options.collective.topology_file);
        if (!topology_probe.is_open()) {
            throw std::invalid_argument("cannot open rnic-cn topology file '" +
                                        *options.collective.topology_file + "'");
        }
        topology_config = FatTreeTopologyCfg::load(*options.collective.topology_file, buffer_bytes,
                                                   COMPOSITE, FAIR_PRIO);
        if (topology_config == nullptr) {
            throw std::runtime_error("rnic-cn topology loader returned no configuration");
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
            2, physical_node_count, options.link_capacity_bps, buffer_bytes,
            options.collective.hop_latency_ps, options.collective.switch_latency_ps, COMPOSITE,
            FAIR_PRIO);
    }
    topology_config->set_ns_tm3_shared_buffer_capacity(buffer_bytes);
    return topology_config;
}

// The resequencing window is the bandwidth jitter product of the known
// topology (algorithm book, section 1.2): upstream FIFO depths are bounded
// by (S_max - 1) packets at the bottleneck egress plus ceil(S_max / paths)
// packets per intermediate stage, so D = (Q_final + stages * Q_mid) * 8 / C.
// An explicitly supplied -rnic_cn_ring_window_ps overrides the derivation.
std::uint64_t derivedRingWindowPs(const RnicAtlahsCliOptions& options,
                                  const FatTreeTopologyCfg& topology_config,
                                  std::uint32_t physical_node_count) {
    if (options.explicitly_supplied.ring_delay_window_ps) {
        return options.collective.ring_delay_window_ps;
    }
    const std::uint64_t mtu_wire = options.packet.max_wire_packet_bytes;
    const std::uint64_t paths = std::max<std::uint32_t>(1, topology_config.radix_up(0));
    const std::uint64_t mid_stages =
        topology_config.get_tiers() >= 2
            ? 2ULL * (topology_config.get_tiers() - 1)
            : 0ULL;
    const std::uint64_t q_final = (physical_node_count - 1) * mtu_wire;
    const std::uint64_t q_mid =
        ((physical_node_count + paths - 1) / paths) * mtu_wire;
    const std::uint64_t upstream_bytes = q_final + mid_stages * q_mid;
    return upstream_bytes * 8ULL * 1000000000000ULL / options.link_capacity_bps;
}

RnicAtlahsRuntimeConfig collectiveRuntimeConfig(const RnicAtlahsCliOptions& options,
                                                const FatTreeTopologyCfg& topology_config,
                                                std::uint32_t physical_node_count) {
    return RnicCollectiveNetworkConfig{
        options.link_capacity_bps,
        RnicDataPacketizationConfig(options.packet.max_wire_packet_bytes,
                                    options.packet.data_header_bytes),
        RnicRingCamConfig{derivedRingWindowPs(options, topology_config,
                                              physical_node_count),
                          options.collective.ring_release_tick_ps,
                          options.collective.ring_wire_capacity_bytes},
        options.collective.global_prbs_seed,
        options.collective.control_deadline_ps,
        options.collective.margin_ppm,
        options.collective.control_wire_bytes,
        {},
        options.collective.maximum_retransmissions,
        options.collective.retransmission_rto_ps,
    };
}

}  // namespace

std::unique_ptr<RnicAtlahsRuntimeAssembly> assembleRnicAtlahsProfile(
    EventList& event_list,
    const RnicAtlahsCliOptions& options,
    std::uint32_t physical_node_count,
    QueueLoggerFactory* logger_factory) {
    if (physical_node_count == 0) {
        throw std::invalid_argument("RNIC ATLAHS profile requires a positive GOAL node count");
    }

    switch (options.profile) {
        case RnicProfile::CollectiveNetwork: {
            auto topology_config =
                makeCollectiveTopologyConfig(options, physical_node_count);
            auto runtime_config = collectiveRuntimeConfig(
                options, *topology_config, physical_node_count);
            return makeRnicAtlahsRuntime(
                event_list, options.profile, std::move(runtime_config),
                std::move(topology_config), logger_factory);
        }
        case RnicProfile::SlingshotLike:
            throw std::invalid_argument(
                "rnic-ss profile is not wired yet; it lands with the "
                "slingshot-like runtime in a follow-up change");
        case RnicProfile::PacketizedManifold:
            return makeRnicAtlahsRuntime(
                event_list, options.profile,
                RnicPacketizedManifoldRuntimeConfig{
                    options.link_capacity_bps,
                    RnicDataPacketizationConfig(options.packet.max_wire_packet_bytes,
                                                options.packet.data_header_bytes),
                    options.manifold.fixed_propagation_delay_ps});
        case RnicProfile::FluidManifold:
            return makeRnicAtlahsRuntime(
                event_list, options.profile,
                RnicFluidManifoldRuntimeConfig{options.link_capacity_bps,
                                               options.manifold.fixed_propagation_delay_ps});
    }
    throw std::invalid_argument("invalid RNIC profile enum");
}

std::string renderRnicSevereLateDropManifest(
    const RnicCollectiveRecoveryStatistics& recovery) {
    if (recovery.late_data_packets == 0) {
        return std::string();
    }
    std::ostringstream manifest;
    manifest << "[RNIC manifest] rnic_cn_severe_late_drop=flagged" << '\n';
    manifest << "[RNIC warning] " << recovery.late_data_packets
             << " Ring-CAM late admission(s) were recovered deterministically"
                " through the GAP NACK path with rates untouched, but a late"
                " drop means the derived jitter bound (bandwidth jitter"
                " product) was violated and should in principle be fixed."
             << '\n';
    return manifest.str();
}

std::string renderRnicAtlahsModelManifest(const RnicAtlahsCliOptions& options,
                                          const AtlahsHtsimApi::GoalLayout& goal_layout,
                                          const RnicAtlahsRuntimeAssembly& assembly) {
    const RnicProfileSpec& spec = assembly.profileSpec();
    if (spec.profile != options.profile) {
        throw std::logic_error("RNIC manifest profile differs from assembled runtime");
    }
    std::ostringstream manifest;
    manifest << "[RNIC manifest] schema=rnic-atlahs-model-v1"
             << " profile=" << rnicProfileName(spec.profile)
             << " fabric=" << rnicFabricModelName(spec.fabric)
             << " traffic=" << rnicTrafficModelName(spec.traffic)
             << " control=" << rnicControlModelName(spec.control)
             << " pacer=" << rnicPacerModelName(spec.pacer) << " rb="
             << (spec.profile == RnicProfile::CollectiveNetwork
                     ? "ring-cam"
                     : (spec.profile == RnicProfile::SlingshotLike ? "voq-request-grant" : "none"))
             << '\n';
    manifest << "[RNIC manifest] goal=" << options.goal_file << " completion_csv="
             << (options.completion_csv.has_value() ? *options.completion_csv : "off")
             << " requested_rank_mapping="
             << rnicAtlahsGoalRankMappingName(options.goal_rank_mapping)
             << " resolved_rank_mapping=" << resolvedGoalRankMappingName(goal_layout.rank_mapping)
             << " ranks=" << goal_layout.rank_count << " cpus=" << goal_layout.cpu_count
             << " nics=" << goal_layout.nic_count
             << " physical_nodes=" << goal_layout.physical_node_count
             << " endpoint_link_bps=" << options.link_capacity_bps << '\n';

    if (spec.traffic == RnicTrafficModel::Packetized) {
        manifest << "[RNIC manifest] max_wire_packet_bytes=" << options.packet.max_wire_packet_bytes
                 << " data_header_bytes=" << options.packet.data_header_bytes
                 << " final_packet=exact-wire-extent"
                 << " accounting=payload-and-wire" << '\n';
    }

    if (spec.profile == RnicProfile::CollectiveNetwork) {
        manifest << "[RNIC manifest] topology="
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
                 << " shared_buffer_bytes=" << options.collective.ns_tm3_shared_buffer_bytes;
        if (!options.collective.topology_file.has_value()) {
            manifest << " hop_latency_ps=" << options.collective.hop_latency_ps
                     << " switch_latency_ps=" << options.collective.switch_latency_ps;
        } else {
            manifest << " latency_source=topology-file";
        }
        manifest << '\n';
        manifest << "[RNIC manifest] declaration_gate=declare-and-go"
                 << " declare_cca_field=nflow_ppm"
                 << " fractional_nflow=always"
                 << " declare_debug_fields=collective_id,expected_fan_in"
                 << " declare_debug_affects_rx_cca=false"
                 << " grant=margin*C/n_hat-scaled-by-own-nflow"
                 << " n_hat=sum-active-declare-nflow"
                 << " rate_feedback=every-resequenced-ack-window-frozen-snapshot"
                 << " rate_effect=receiver-boundary-k-plus-2"
                 << " startup_rate=full-reservation-ledger-allocation"
                 << " egress_composition=cegress-fair-share-min-small-wqe"
                 << " rebalancer=rtt-slack-redistribution-nflow-update"
                 << " margin_ppm=" << options.collective.margin_ppm
                 << " control_deadline_ps=" << options.collective.control_deadline_ps
                 << " control_wire_bytes=" << options.collective.control_wire_bytes
                 << " retirement=physical-retire-after-final-data" << '\n';
        manifest << "[RNIC manifest] prbs_algorithm=" << RnicPrbsPacer::kAlgorithmName
                 << " prbs_version=" << RnicPrbsPacer::kAlgorithmVersion
                 << " polynomial=x^64+x^63+x^61+x^60+1"
                 << " feedback_mask=0xd800000000000000"
                 << " word_extraction=" << RnicPrbsPacer::kWordExtraction
                 << " lfsr_steps_per_word=" << RnicPrbsPacer::kLfsrStepsPerWord
                 << " bounded_draw=" << RnicPrbsPacer::kBoundedDraw
                 << " seed_derivation=splitmix64-pair-v1"
                 << " global_seed=" << options.collective.global_prbs_seed
                 << " mixed_extent_hazard=adaptive-fixed-point-q32"
                 << " tie_break=flow-id" << '\n';
        manifest << "[RNIC manifest] ring_window_ps="
                 << derivedRingWindowPs(options, *assembly.topologyConfig(),
                                        goal_layout.physical_node_count)
                 << " ring_tick_ps=" << options.collective.ring_release_tick_ps
                 << " ring_capacity_wire_bytes=" << options.collective.ring_wire_capacity_bytes
                 << " scope=node-shared"
                 << " eta=source-route-injection-plus-packet-specific-no-load-transit"
                 << " eta_transit=pipe-switch-latency-plus-remaining-ns-tm3-egress-serialization"
                 << " same_time_order=release-before-admission" << '\n';
        manifest << "[RNIC manifest] recovery=deterministic-gap-nack-retransmission"
                 << " late_admission=rejected-as-gap"
                 << " early_admission=hard-error"
                 << " overflow_admission=hard-error"
                 << " gap_decision=post-resequence-same-timestamp"
                 << " gap_decision_epsilon_ps=0"
                 << " gap_nack_priority=high"
                 << " gap_nack_wire_bytes=" << options.collective.control_wire_bytes
                 << " retransmission_priority=low"
                 << " retransmission_arbitration=per-flow-head-in-node-prbs-lottery"
                 << " retransmission_prbs_draw=true"
                 << " retransmission_rate_accounting=substitutes-fresh-granted-service"
                 << " retransmission_eta=fresh-packet-specific-calibration"
                 << " retransmission_scope=exact-logical-packet"
                 << " maximum_retransmissions=" << options.collective.maximum_retransmissions
                 << " retransmission_rto_ps=" << options.collective.retransmission_rto_ps
                 << " retransmission_rto_epoch=physical-retry-serialization-end"
                 << " control_loss=fatal-no-control-recovery"
                 << " retire_deadline=max-original-release"
                 << " retirement_gate=exact-rx-ledger-and-no-gap" << '\n';
    } else if (spec.profile == RnicProfile::SlingshotLike) {
        manifest << "[RNIC manifest] topology="
                 << (options.collective.topology_file.has_value()
                         ? *options.collective.topology_file
                         : "generated-two-tier")
                 << " clos_tiers=2 switch=ns-rosetta"
                 << " link_failures=0 pfc=off ecn=off"
                 << " switch_queue=input-voq-request-grant"
                 << " source_serializer=physical-host-queue"
                 << " shared_buffer_bytes=" << options.slingshot.ns_rosetta_shared_buffer_bytes;
        if (!options.collective.topology_file.has_value()) {
            manifest << " hop_latency_ps=" << options.collective.hop_latency_ps
                     << " switch_latency_ps=" << options.collective.switch_latency_ps;
        } else {
            manifest << " latency_source=topology-file";
        }
        manifest << '\n';
        manifest << "[RNIC manifest] model=open-slingshot-like-comparator"
                 << " proprietary_threshold_claim=false"
                 << " routing="
                 << (options.slingshot.unordered_packet_routing ? "unordered-packet"
                                                                : "ordered-endpoint-pair")
                 << " path_candidates=deterministic-four-of-eight"
                 << " sender_information=source-local-switch-state-plus-physical-ack-telemetry"
                 << " local_path_score=source-leaf-backlog-delay"
                 << " equal_cost_tie=hashed-candidate-order"
                 << " ordered_startup_reservation=one-data-envelope-until-first-data-arrival"
                 << " reservation_buffer_residency=false"
                 << " global_sender_state=false"
                 << " telemetry_delay_ps=" << options.slingshot.telemetry_delay_ps
                 << " sample_age_ps=" << options.slingshot.maximum_sample_age_ps
                 << " path_hysteresis_ps=" << options.slingshot.path_hysteresis_ps
                 << " routing_seed=" << options.slingshot.routing_seed << '\n';
        manifest << "[RNIC manifest] state_trace_rate="
                    "sender-local-credit-feedback-service"
                 << " state_trace_rate_oracle=false"
                 << " state_trace_rate_before_bp=access-link"
                 << " state_trace_rate_during_bp="
                    "min-domain-delta-cumulative-credit-over-physical-arrival-interval"
                 << '\n';
        manifest << "[RNIC manifest] source_prbs_algorithm=" << RnicPrbsPacer::kAlgorithmName
                 << " source_prbs_version=" << RnicPrbsPacer::kAlgorithmVersion
                 << " source_prbs_seed=routing-seed-domain-separated"
                 << " source_data_admission="
                    "one-future-head-per-physical-access-serializer"
                 << " sack_window_is_injection_burst=false" << '\n';
        manifest << "[RNIC manifest] pair_tracking=endpoint-pair-outstanding"
                 << " ack=per-data-packet-sack128"
                 << " recovery=selective-repeat"
                 << " sack_hole_retry=immediate"
                 << " silent_loss_rto_ps=" << options.slingshot.rto_ps
                 << " normal_lossless_rto_expected=0"
                 << " loss_stress=" << (options.slingshot.allow_loss_stress ? "on" : "off")
                 << " window_packets=" << options.slingshot.outstanding_window_packets
                 << " maximum_retransmissions=" << options.slingshot.maximum_retransmissions
                 << '\n';
        manifest << "[RNIC manifest] backpressure=pair-selective-physical"
                 << " pressure_domains="
                    "every-leaf-spine-egress-plus-switch-shared"
                 << " sender_credit_gate=intersection-of-active-domains"
                 << " shared_domain_initial_credit=0"
                 << " credit=service-driven-cumulative"
                 << " control_priority=strict-nonpreemptive"
                 << " q_hi_bytes=" << options.slingshot.q_hi_bytes
                 << " q_lo_bytes=" << options.slingshot.q_lo_bytes
                 << " credit_quantum_packets=" << options.slingshot.credit_quantum_packets
                 << " max_credit_ahead_bytes=" << options.slingshot.maximum_credit_ahead_bytes
                 << " control_wire_bytes=" << options.slingshot.control_wire_bytes << '\n';
        manifest << "[RNIC manifest] queue_bound="
                    "controlled-clos-per-domain-aggregate-envelope"
                 << " bound_forward_data_serialization=included"
                 << " bound_bp_enable_fan_in="
                    "maximum-physical-egress-endpoint-pairs"
                 << " bound_bp_enable_serialization="
                    "maximum-physical-control-serializer"
                 << " bound_reverse_blocking="
                    "one-max-data-per-nonpreemptive-serializer"
                 << " bound_shared_pair_phase="
                    "one-max-data-per-switch-local-directed-pair"
                 << '\n';
    } else {
        manifest << "[RNIC manifest] topology=none"
                 << " fixed_propagation_ps=" << options.manifold.fixed_propagation_delay_ps
                 << " allocation=instant-central-max-min"
                 << " edge_constraints=source-and-destination"
                 << " manifold_queue=none"
                 << " loss=none backpressure=none" << '\n';
        if (spec.profile == RnicProfile::PacketizedManifold) {
            manifest << "[RNIC manifest] packet_scheduler_version="
                     << RnicPacketizedSlotCalendar::kSchedulerVersion
                     << " matching=collision-free-saturated-port-first"
                     << " final_tail_serialization=exact"
                     << " calendar_reservation=full-envelope" << '\n';
        } else {
            manifest << "[RNIC manifest] service=continuous-fluid"
                     << " allocation_arithmetic=exact-rational"
                     << " bps_policy=componentwise-floor"
                     << " completion_time=picosecond-ceiling"
                     << " propagation=after-last-serviced-bit" << '\n';
        }
    }
    return manifest.str();
}
