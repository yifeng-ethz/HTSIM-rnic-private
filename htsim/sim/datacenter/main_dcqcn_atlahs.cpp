// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dcqcn_atlahs_cli.h"

#include "atlahs_htsim_api.h"
#include "eventlist.h"
#include "logsim-interface.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool requestedHelp(int argc, char* argv[]) {
    return argc == 2 && argv != nullptr && argv[1] != nullptr &&
           (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help");
}

std::optional<AtlahsHtsimApi::GoalRankMapping> rankMappingOverride(DcqcnGoalRankMapping mapping) {
    switch (mapping) {
        case DcqcnGoalRankMapping::Auto:
            return std::nullopt;
        case DcqcnGoalRankMapping::GpuRank:
            return AtlahsHtsimApi::GoalRankMapping::GpuRank;
        case DcqcnGoalRankMapping::UniqueNic:
            return AtlahsHtsimApi::GoalRankMapping::UniqueNic;
    }
    throw std::invalid_argument("invalid DCQCN GOAL rank mapping");
}

const char* resolvedRankMappingName(AtlahsHtsimApi::GoalRankMapping mapping) {
    switch (mapping) {
        case AtlahsHtsimApi::GoalRankMapping::GpuRank:
            return "gpu-rank";
        case AtlahsHtsimApi::GoalRankMapping::UniqueNic:
            return "unique-nic";
    }
    throw std::invalid_argument("invalid resolved GOAL rank mapping");
}

void writeCompletionCsv(const std::string& path,
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
        output << "dcqcn," << flow.flow_id << ',' << flow.source << ',' << flow.destination << ','
               << flow.tag << ',' << flow.payload_bytes << ',' << flow.start_time_ps << ','
               << flow.completion_time_ps << ',' << flow.fct_ps() << ','
               << flow.wqe_id << ',' << flow.sq_id << ',' << flow.rq_id << ','
               << flow.cq_id << ',' << flow.sq_post_sequence << ','
               << flow.sq_dispatch_sequence << ',' << flow.cq_post_sequence << ','
               << flow.cq_consume_sequence << ','
               << atlahsTransportKindName(flow.transport_kind) << ','
               << flow.transport_object_id << '\n';
    }
    output.flush();
    if (!output) {
        throw std::runtime_error("failed while writing completion CSV '" + path + "'");
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string program_name =
        argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "htsim_dcqcn_atlahs";
    if (requestedHelp(argc, argv)) {
        std::cout << dcqcnAtlahsCliUsage(program_name) << '\n';
        return 0;
    }

    try {
        std::vector<const char*> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            arguments.push_back(argv[index]);
        }
        const DcqcnAtlahsCliOptions options = parseDcqcnAtlahsCli(argc, arguments.data());

        EventList event_list;
        EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
        AtlahsHtsimApi api;
        LogSimInterface logsim(nullptr, static_cast<TrafficLoggerSimple*>(nullptr), event_list,
                               nullptr, nullptr);
        api.setEventList(&event_list);
        api.setLogSimInterface(&logsim);
        api.setGoalRankMappingOverride(rankMappingOverride(options.goal_rank_mapping));
        api.linkspeed = options.runtime.endpoint_link_bps;
        logsim.htsim_api = &api;
        logsim.set_protocol(RNIC_PROTOCOL);
        logsim.lgs_o = static_cast<int>(options.lgs_o_ns);

        DcqcnAtlahsRuntime* installed_runtime = nullptr;
        const int result =
            start_lgs(options.goal_file, logsim, [&](const AtlahsHtsimApi::GoalLayout& layout) {
                auto runtime = std::make_unique<DcqcnAtlahsRuntime>(event_list, options.runtime,
                                                                    layout.physical_node_count);
                api.setTopologyCfg(&runtime->topology_config());
                api.setTopology(&runtime->topology());
                std::cout << renderDcqcnAtlahsManifest(
                    options.runtime, layout.physical_node_count, options.goal_file,
                    options.completion_csv.value_or(""),
                    options.runtime.state_trace_csv.value_or(""),
                    resolvedRankMappingName(layout.rank_mapping));
                installed_runtime = runtime.get();
                api.setFlowRuntime(std::move(runtime));
            });
        if (result != 0) {
            throw std::runtime_error("ATLAHS GOAL execution returned " + std::to_string(result));
        }
        if (api.runtimeHasPendingPhysicalWork()) {
            throw std::logic_error("ATLAHS returned before DCQCN physical quiescence");
        }
        api.validateWqeQuiescent();
        if (options.completion_csv.has_value()) {
            writeCompletionCsv(*options.completion_csv, api.completedFlows());
        }
        if (installed_runtime == nullptr) {
            throw std::logic_error("DCQCN runtime was not installed");
        }
        if (options.runtime.state_trace_csv.has_value()) {
            installed_runtime->writeStateTraceCsv();
        }
        if (options.runtime.goodput_trace_csv.has_value()) {
            installed_runtime->writeGoodputTraceCsv();
        }
        std::cout << installed_runtime->renderPfcPortMetricsManifest();
        std::cout << "[DCQCN manifest] completed_flows="
                  << installed_runtime->completed_flow_count()
                  << " silent_rtos=" << installed_runtime->silent_rto_count()
                  << " loss_rate_cuts=" << installed_runtime->loss_rate_cut_count()
                  << " ecn_marked_packets=" << installed_runtime->ecn_marked_packet_count()
                  << " dcqcn_pfc_pause_frames=" << installed_runtime->pfc_pause_count()
                  << " dcqcn_pfc_resume_frames=" << installed_runtime->pfc_resume_count()
                  << " dcqcn_pfc_paused_wall_ps="
                  << installed_runtime->pfc_paused_wall_ps_total()
                  << " dcqcn_pfc_max_cascade_depth="
                  << installed_runtime->pfc_max_cascade_depth()
                  << " ns_tm3_dropped_packets=" << installed_runtime->dropped_packet_count()
                  << " ns_tm3_shared_pool_dropped_packets="
                  << installed_runtime->shared_pool_dropped_packet_count()
                  << " ns_tm3_egress_domain_dropped_packets="
                  << installed_runtime->egress_domain_dropped_packet_count()
                  << " physical_quiescence=verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "htsim_dcqcn_atlahs: " << error.what() << '\n'
                  << dcqcnAtlahsCliUsage(program_name) << '\n';
        return 2;
    }
}
