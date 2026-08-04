// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_ATLAHS_DRIVER_H
#define RNIC_ATLAHS_DRIVER_H

#include <cstdint>
#include <memory>
#include <string>

#include "atlahs_htsim_api.h"
#include "rnic_atlahs_cli.h"
#include "rnic_atlahs_runtime_factory.h"
#include "rnic_collective_network_runtime.h"

class EventList;
class QueueLoggerFactory;

// Resolve CLI model parameters only after the immutable GOAL header has
// supplied the physical endpoint count.  In particular, this function checks
// a generated two-tier shape before entering legacy FatTree construction,
// whose invalid-shape path terminates the process rather than throwing.
std::unique_ptr<RnicAtlahsRuntimeAssembly> assembleRnicAtlahsProfile(
    EventList& event_list,
    const RnicAtlahsCliOptions& options,
    std::uint32_t physical_node_count,
    QueueLoggerFactory* logger_factory = nullptr);

// Stable, line-oriented model manifest emitted before GOAL execution.  Build,
// source-control, workload-hash, and experiment-metric provenance belong to
// the outer experiment harness; this manifest records the resolved simulator
// semantics needed to interpret those results.
// Severe-flag semantics (maintainer ruling, comparator-realism round):
// any Ring-CAM late admission means the derived jitter bound was violated.
// The run still recovers deterministically through the GAP NACK path with
// rates untouched, but the manifest flags it and the driver prints one
// warning. Returns the lines to print, or an empty string for a
// zero-recovery run.
std::string renderRnicSevereLateDropManifest(
    const RnicCollectiveRecoveryStatistics& recovery);

std::string renderRnicAtlahsModelManifest(
    const RnicAtlahsCliOptions& options,
    const AtlahsHtsimApi::GoalLayout& goal_layout,
    const RnicAtlahsRuntimeAssembly& assembly);

#endif  // RNIC_ATLAHS_DRIVER_H
