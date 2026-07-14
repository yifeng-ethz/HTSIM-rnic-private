// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "atlahs_htsim_api.h"
#include "eventlist.h"
#include "logsim-interface.h"
#include "rnic_atlahs_cli.h"
#include "rnic_atlahs_driver.h"
#include "rnic_prbs_pacer.h"

namespace {

std::optional<AtlahsHtsimApi::GoalRankMapping> goalRankMappingOverride(
    RnicAtlahsGoalRankMapping mapping) {
    switch (mapping) {
        case RnicAtlahsGoalRankMapping::Auto:
            return std::nullopt;
        case RnicAtlahsGoalRankMapping::GpuRank:
            return AtlahsHtsimApi::GoalRankMapping::GpuRank;
        case RnicAtlahsGoalRankMapping::UniqueNic:
            return AtlahsHtsimApi::GoalRankMapping::UniqueNic;
    }
    throw std::invalid_argument("invalid GOAL rank-mapping enum");
}

bool requestedHelp(int argc, char* argv[]) {
    return argc == 2 && argv != nullptr && argv[1] != nullptr &&
           (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help");
}

void writeCompletionCsv(const std::string& path,
                        RnicProfile profile,
                        const std::vector<AtlahsCompletedFlowRecord>& completed_flows) {
    std::vector<AtlahsCompletedFlowRecord> ordered = completed_flows;
    std::sort(ordered.begin(), ordered.end(),
              [](const AtlahsCompletedFlowRecord& left, const AtlahsCompletedFlowRecord& right) {
                  return left.flow_id < right.flow_id;
              });

    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("cannot open completion CSV '" + path + "'");
    }
    output << "profile,flow_id,source,destination,tag,payload_bytes,"
              "start_time_ps,completion_time_ps,fct_ps\n";
    for (const AtlahsCompletedFlowRecord& flow : ordered) {
        output << rnicProfileName(profile) << ',' << flow.flow_id << ',' << flow.source << ','
               << flow.destination << ',' << flow.tag << ',' << flow.payload_bytes << ','
               << flow.start_time_ps << ',' << flow.completion_time_ps << ',' << flow.fct_ps()
               << '\n';
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("failed while writing completion CSV '" + path + "'");
    }
}

void validateRuntimeQuiescence(AtlahsHtsimApi& api) {
    auto* assembly = dynamic_cast<RnicAtlahsRuntimeAssembly*>(api.getFlowRuntime());
    if (assembly == nullptr) {
        throw std::logic_error("RNIC ATLAHS runtime is not an assembled profile");
    }

    if (auto* runtime = dynamic_cast<RnicSsRuntime*>(&assembly->implementation())) {
        runtime->validateQuiescent();
        const RnicSsRuntimeStatistics& statistics = runtime->statistics();
        std::cout << "[RNIC manifest] rnic_ss_bound_kind="
                     "controlled-clos-per-domain-aggregate-envelope"
                  << " rnic_ss_forward_data_observation_ps="
                  << statistics.physical_forward_data_observation_ps
                  << " rnic_ss_last_bp_enable_feedback_ps="
                  << statistics.physical_last_bp_enable_feedback_ps
                  << " rnic_ss_bound_control_loop_ps=" << statistics.physical_bound_control_loop_ps
                  << " rnic_ss_maximum_bp_enable_fan_in=" << statistics.maximum_bp_enable_fan_in
                  << " rnic_ss_maximum_switch_endpoint_pairs="
                  << statistics.maximum_switch_endpoint_pairs
                  << " rnic_ss_maximum_switch_physical_ingresses="
                  << statistics.maximum_switch_physical_ingresses
                  << " rnic_ss_maximum_switch_physical_egresses="
                  << statistics.maximum_switch_physical_egresses
                  << " rnic_ss_control_loop_window_packets="
                  << statistics.control_loop_window_packets
                  << " rnic_ss_configured_window_below_control_loop="
                  << (statistics.configured_window_below_control_loop ? "true" : "false")
                  << " rnic_ss_analytical_queue_bound_bytes="
                  << statistics.analytical_queue_bound_bytes
                  << " rnic_ss_analytical_shared_reaction_bytes="
                  << statistics.analytical_shared_reaction_bytes
                  << " rnic_ss_minimum_shared_pressure_high_bytes="
                  << statistics.minimum_shared_pressure_high_bytes
                  << " rnic_ss_maximum_shared_pressure_high_bytes="
                  << statistics.maximum_shared_pressure_high_bytes
                  << " rnic_ss_source_pacer=" << RnicPrbsPacer::kAlgorithmName
                  << " rnic_ss_source_pacer_version=" << RnicPrbsPacer::kAlgorithmVersion << '\n';
        std::cout
            << "[RNIC manifest] rnic_ss_new_data_packets=" << statistics.new_data_packets
            << " rnic_ss_sack_retransmissions=" << statistics.sack_retransmissions
            << " rnic_ss_rto_retransmissions=" << statistics.rto_retransmissions
            << " rnic_ss_rto_deadline_pushes=" << statistics.rto_deadline_pushes
            << " rnic_ss_rto_deadline_stale_pops=" << statistics.rto_deadline_stale_pops
            << " rnic_ss_rto_deadline_due_pops=" << statistics.rto_deadline_due_pops
            << " rnic_ss_rto_deadline_heap_high_watermark="
            << statistics.rto_deadline_heap_high_watermark
            << " rnic_ss_fabric_drops=" << statistics.fabric_drops
            << " rnic_ss_maximum_observed_shared_buffer_bytes="
            << statistics.maximum_observed_shared_buffer_bytes
            << " rnic_ss_leaf_egress_bp_enable_events=" << statistics.leaf_egress_bp_enable_events
            << " rnic_ss_spine_egress_bp_enable_events=" << statistics.spine_egress_bp_enable_events
            << " rnic_ss_leaf_shared_bp_enable_events=" << statistics.leaf_shared_bp_enable_events
            << " rnic_ss_spine_shared_bp_enable_events=" << statistics.spine_shared_bp_enable_events
            << " rnic_ss_maximum_active_credit_domains=" << statistics.maximum_active_credit_domains
            << " rnic_ss_source_prbs_data_opportunities="
            << statistics.source_prbs_data_opportunities
            << " rnic_ss_source_prbs_busy_deferrals=" << statistics.source_prbs_busy_deferrals
            << " rnic_ss_ordered_path_bindings=" << statistics.ordered_path_bindings
            << " rnic_ss_ordered_path_binding_max_leaf_skew="
            << statistics.ordered_path_binding_max_leaf_skew
            << " rnic_ss_ordered_path_bindings_by_path="
            << statistics.ordered_path_bindings_by_path[0] << ':'
            << statistics.ordered_path_bindings_by_path[1] << ':'
            << statistics.ordered_path_bindings_by_path[2] << ':'
            << statistics.ordered_path_bindings_by_path[3] << ':'
            << statistics.ordered_path_bindings_by_path[4] << ':'
            << statistics.ordered_path_bindings_by_path[5] << ':'
            << statistics.ordered_path_bindings_by_path[6] << ':'
            << statistics.ordered_path_bindings_by_path[7]
            << " rnic_ss_source_priority_violations=" << statistics.source_priority_violations
            << '\n';
        return;
    }

    if (auto* runtime = dynamic_cast<RnicCollectiveNetworkRuntime*>(&assembly->implementation())) {
        runtime->validateQuiescent();
        const RnicCollectiveRecoveryStatistics& recovery = runtime->recoveryStatistics();
        std::cout << "[RNIC manifest] rnic_cn_late_data_packets=" << recovery.late_data_packets
                  << " rnic_cn_gap_nacks_dispatched=" << recovery.gap_nacks_dispatched
                  << " rnic_cn_gap_nacks_received=" << recovery.gap_nacks_received
                  << " rnic_cn_deterministic_retransmissions="
                  << recovery.deterministic_retransmissions
                  << " rnic_cn_deterministic_retransmission_wire_bytes="
                  << recovery.deterministic_retransmission_wire_bytes
                  << " rnic_cn_duplicate_gap_nacks_ignored=" << recovery.duplicate_gap_nacks_ignored
                  << " rnic_cn_duplicate_data_packets_ignored="
                  << recovery.duplicate_data_packets_ignored
                  << " rnic_cn_maximum_retry_attempt_observed="
                  << recovery.maximum_retry_attempt_observed << '\n';
    }
}

void writeRequestedStateTrace(AtlahsHtsimApi& api, const RnicAtlahsCliOptions& options) {
    const bool collective_requested = options.collective.state_trace_csv.has_value();
    const bool slingshot_requested = options.slingshot.state_trace_csv.has_value();
    if (!collective_requested && !slingshot_requested) {
        return;
    }
    auto* assembly = dynamic_cast<RnicAtlahsRuntimeAssembly*>(api.getFlowRuntime());
    if (assembly == nullptr) {
        throw std::logic_error("RNIC state trace runtime is not an assembled profile");
    }
    if (collective_requested) {
        auto* runtime = dynamic_cast<RnicCollectiveNetworkRuntime*>(&assembly->implementation());
        if (runtime == nullptr) {
            throw std::logic_error("rnic-cn state trace requested for another profile");
        }
        runtime->writeStateTraceCsv();
        return;
    }
    auto* runtime = dynamic_cast<RnicSsRuntime*>(&assembly->implementation());
    if (runtime == nullptr) {
        throw std::logic_error("rnic-ss state trace requested for another profile");
    }
    runtime->writeStateTraceCsv();
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string program_name =
        argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "htsim_rnic";
    if (requestedHelp(argc, argv)) {
        std::cout << rnicAtlahsCliUsage(program_name) << '\n';
        return 0;
    }

    try {
        std::vector<const char*> parse_arguments;
        parse_arguments.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            parse_arguments.push_back(argv[index]);
        }
        const RnicAtlahsCliOptions options = parseRnicAtlahsCli(argc, parse_arguments.data());

        EventList event_list;
        EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
        AtlahsHtsimApi api;
        LogSimInterface logsim(nullptr, static_cast<TrafficLoggerSimple*>(nullptr), event_list,
                               nullptr, nullptr);

        api.setEventList(&event_list);
        api.setLogSimInterface(&logsim);
        api.setGoalRankMappingOverride(goalRankMappingOverride(options.goal_rank_mapping));
        api.linkspeed = options.link_capacity_bps;
        if (options.node_count != 0) {
            api.total_nodes = static_cast<int>(options.node_count);
        }
        logsim.htsim_api = &api;
        logsim.set_protocol(RNIC_PROTOCOL);

        const int result = start_lgs(
            options.goal_file, logsim, [&](const AtlahsHtsimApi::GoalLayout& goal_layout) {
                auto session =
                    assembleRnicAtlahsProfile(event_list, options, goal_layout.physical_node_count);
                api.setTopologyCfg(session->topologyConfig());
                api.setTopology(session->physicalTopology());
                std::cout << renderRnicAtlahsModelManifest(options, goal_layout, *session);
                api.setFlowRuntime(std::move(session));
            });
        if (result != 0) {
            throw std::runtime_error("ATLAHS GOAL execution returned " + std::to_string(result));
        }
        if (api.runtimeHasPendingPhysicalWork()) {
            throw std::logic_error("ATLAHS returned before RNIC physical quiescence");
        }
        validateRuntimeQuiescence(api);
        writeRequestedStateTrace(api, options);
        if (options.completion_csv.has_value()) {
            writeCompletionCsv(*options.completion_csv, options.profile, api.completedFlows());
        }
        std::cout << "[RNIC manifest] physical_quiescence=verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "htsim_rnic: " << error.what() << '\n'
                  << rnicAtlahsCliUsage(program_name) << '\n';
        return 2;
    }
}
