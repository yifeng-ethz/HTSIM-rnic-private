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

#ifdef HTSIM_ENABLE_SIMLLM_RNIC
#include "simllm_atlahs_flow_runtime.h"
#endif

namespace {

#ifdef HTSIM_ENABLE_SIMLLM_RNIC
htsim::simllm_rnic::SimllmAtlahsRuntimeConfig structuralRuntimeConfig(
        const RnicAtlahsCliOptions& options,
        const AtlahsHtsimApi::GoalLayout& goal_layout) {
    htsim::simllm_rnic::SimllmAtlahsRuntimeConfig config;
    config.session_id = "htsim-rnic-goal";
    config.transport_policy = rnicProfileName(options.profile);
    config.seed = options.profile == RnicProfile::CollectiveNetwork
                      ? options.collective.global_prbs_seed
                      : 0;
    config.topology_identity =
        std::string(rnicProfileName(options.profile)) + ":nodes="
        + std::to_string(goal_layout.physical_node_count);
    config.htsim_source_revision = HTSIM_COMPOSED_SOURCE_REVISION;
    config.simllm_source_revision = SIMLLM_COMPOSED_SOURCE_REVISION;
    config.device =
        htsim::simllm_rnic::defaultSimllmAtlahsDeviceConfig();
    config.port.endpoint_count = goal_layout.physical_node_count;
    config.port.link_rate_bps = options.link_capacity_bps;
    config.port.traffic_class = 3;
    return config;
}
#endif

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
              "start_time_ps,completion_time_ps,fct_ps,"
              "wqe_id,sq_id,rq_id,cq_id,sq_post_sequence,"
              "sq_dispatch_sequence,cq_post_sequence,cq_consume_sequence,"
              "transport_kind,transport_object_id\n";
    for (const AtlahsCompletedFlowRecord& flow : ordered) {
        output << rnicProfileName(profile) << ',' << flow.flow_id << ',' << flow.source << ','
               << flow.destination << ',' << flow.tag << ',' << flow.payload_bytes << ','
               << flow.start_time_ps << ',' << flow.completion_time_ps << ',' << flow.fct_ps()
               << ',' << flow.wqe_id << ',' << flow.sq_id << ',' << flow.rq_id << ','
               << flow.cq_id << ',' << flow.sq_post_sequence << ','
               << flow.sq_dispatch_sequence << ',' << flow.cq_post_sequence << ','
               << flow.cq_consume_sequence << ','
               << atlahsTransportKindName(flow.transport_kind) << ','
               << flow.transport_object_id
               << '\n';
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("failed while writing completion CSV '" + path + "'");
    }
}

void validateRuntimeQuiescence(AtlahsHtsimApi& api) {
    AtlahsFlowRuntime* runtime = api.getFlowRuntime();
#ifdef HTSIM_ENABLE_SIMLLM_RNIC
    auto* structural =
        dynamic_cast<htsim::simllm_rnic::SimllmAtlahsFlowRuntime*>(runtime);
    if (structural != nullptr) {
        structural->validateQuiescent();
        const auto& record = structural->runRecord();
        std::cout << "[RNIC manifest] hardware_mode=structural"
                  << " wqe_authority=simllm-native-rnic-session"
                  << " hardware_config_sha256="
                  << record.hardware_config_sha256
                  << " native_posts="
                  << record.authority_counters.native_posts << '\n';
        runtime = &structural->networkRuntime();
    } else {
        std::cout << "[RNIC manifest] hardware_mode=bypass"
                  << " wqe_authority=atlahs-wqe-ledger\n";
    }
#endif
    auto* assembly = dynamic_cast<RnicAtlahsRuntimeAssembly*>(runtime);
    if (assembly == nullptr) {
        throw std::logic_error("RNIC ATLAHS runtime is not an assembled profile");
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
                  << recovery.maximum_retry_attempt_observed
                  << " rnic_cn_stale_declarations_ignored="
                  << recovery.stale_declarations_ignored
                  << " rnic_cn_stale_nflow_updates_ignored="
                  << recovery.stale_nflow_updates_ignored << '\n';
        std::cout << renderRnicSevereLateDropManifest(recovery);
    }
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
#ifdef HTSIM_ENABLE_SIMLLM_RNIC
                if (options.profile != RnicProfile::FluidManifold) {
                    api.setFlowRuntime(
                        htsim::simllm_rnic::makeComposedSimllmAtlahsFlowRuntime(
                            event_list,
                            structuralRuntimeConfig(options, goal_layout),
                            std::move(session)));
                } else {
                    api.setFlowRuntime(std::move(session));
                }
#else
                api.setFlowRuntime(std::move(session));
#endif
            });
        if (result != 0) {
            throw std::runtime_error("ATLAHS GOAL execution returned " + std::to_string(result));
        }
        if (api.runtimeHasPendingPhysicalWork()) {
            throw std::logic_error("ATLAHS returned before RNIC physical quiescence");
        }
        validateRuntimeQuiescence(api);
        api.validateWqeQuiescent();
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
