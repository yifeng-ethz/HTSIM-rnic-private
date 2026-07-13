// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
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
    return argc == 2 && argv != nullptr && argv[1] != nullptr
           && (std::string(argv[1]) == "-h"
               || std::string(argv[1]) == "--help");
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string program_name =
        argc > 0 && argv != nullptr && argv[0] != nullptr
            ? argv[0] : "htsim_rnic";
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
        const RnicAtlahsCliOptions options =
            parseRnicAtlahsCli(argc, parse_arguments.data());

        EventList event_list;
        EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());
        AtlahsHtsimApi api;
        LogSimInterface logsim(
            nullptr,
            static_cast<TrafficLoggerSimple*>(nullptr),
            event_list,
            nullptr,
            nullptr);

        api.setEventList(&event_list);
        api.setLogSimInterface(&logsim);
        api.setGoalRankMappingOverride(
            goalRankMappingOverride(options.goal_rank_mapping));
        api.linkspeed = options.link_capacity_bps;
        if (options.node_count != 0) {
            api.total_nodes = static_cast<int>(options.node_count);
        }
        logsim.htsim_api = &api;
        logsim.set_protocol(RNIC_PROTOCOL);

        const int result = start_lgs(
            options.goal_file,
            logsim,
            [&](const AtlahsHtsimApi::GoalLayout& goal_layout) {
                auto session = assembleRnicAtlahsProfile(
                    event_list,
                    options,
                    goal_layout.physical_node_count);
                api.setTopologyCfg(session->topologyConfig());
                api.setTopology(session->physicalTopology());
                std::cout << renderRnicAtlahsModelManifest(
                    options, goal_layout, *session);
                api.setFlowRuntime(std::move(session));
            });
        if (result != 0) {
            throw std::runtime_error(
                "ATLAHS GOAL execution returned "
                + std::to_string(result));
        }
        if (api.runtimeHasPendingPhysicalWork()) {
            throw std::logic_error(
                "ATLAHS returned before RNIC physical quiescence");
        }
        std::cout << "[RNIC manifest] physical_quiescence=verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "htsim_rnic: " << error.what() << '\n'
                  << rnicAtlahsCliUsage(program_name) << '\n';
        return 2;
    }
}
