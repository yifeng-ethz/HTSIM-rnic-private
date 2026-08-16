// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DCQCN_ATLAHS_CLI_H
#define DCQCN_ATLAHS_CLI_H

#include "dcqcn_atlahs_runtime.h"

#include <optional>
#include <string>

enum class DcqcnGoalRankMapping {
    Auto,
    GpuRank,
    UniqueNic,
};

struct DcqcnAtlahsCliOptions {
    std::string goal_file;
    std::optional<std::string> completion_csv;
    DcqcnGoalRankMapping goal_rank_mapping{DcqcnGoalRankMapping::Auto};
    // LogGOPS per-message host overhead "o" in nanoseconds, charged by the
    // shared LGS driver on sends and UQ-matched receives (legacy -lgs_o).
    std::uint32_t lgs_o_ns{0};
    DcqcnAtlahsRuntimeConfig runtime;
};

DcqcnAtlahsCliOptions parseDcqcnAtlahsCli(int argc, const char* const argv[]);
const char* dcqcnGoalRankMappingName(DcqcnGoalRankMapping mapping);
std::string dcqcnAtlahsCliUsage(const std::string& program_name);

#endif  // DCQCN_ATLAHS_CLI_H
