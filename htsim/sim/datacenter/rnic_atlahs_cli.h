// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_ATLAHS_CLI_H
#define RNIC_ATLAHS_CLI_H

#include <cstdint>
#include <optional>
#include <string>

#include "rnic_profile.h"

enum class RnicAtlahsGoalRankMapping {
    Auto,
    GpuRank,
    UniqueNic,
};

// Packet fields are shared by the two physical-packet profiles.  The maximum
// is the complete wire extent, not application payload or a legacy HTSIM MTU.
struct RnicAtlahsPacketCliOptions {
    std::uint64_t max_wire_packet_bytes = 4160;
    std::uint64_t data_header_bytes = 64;
};

// The NN profiles have no topology.  This is the fixed propagation component
// between their finite-rate endpoint links.
struct RnicAtlahsManifoldCliOptions {
    std::uint64_t fixed_propagation_delay_ps = 2000000;
};

// Options needed to assemble the physical collective-network profile.  A
// missing topology file means a generated two-tier Tomahawk3 Clos.  A supplied
// topology file still has to resolve to that same checked physical model in
// the assembly layer.
struct RnicAtlahsCollectiveCliOptions {
    std::optional<std::string> topology_file;
    std::uint64_t hop_latency_ps = 1000000;
    std::uint64_t switch_latency_ps = 0;
    std::uint64_t global_prbs_seed = UINT64_C(0x123456789abcdef0);
    std::uint64_t control_deadline_ps = 10000000;
    std::uint32_t margin_ppm = 900000;
    std::uint64_t control_wire_bytes = 64;
    std::uint64_t ring_delay_window_ps = 4096000;
    std::uint64_t ring_release_tick_ps = 16000;
    std::uint64_t ring_wire_capacity_bytes = UINT64_C(1) << 20;
    std::uint64_t tomahawk3_shared_buffer_bytes = UINT64_C(1) << 20;
};

// Defaults remain visible in the resolved options, while this side table lets
// a run manifest distinguish them from deliberate experiment overrides.
struct RnicAtlahsExplicitCliOptions {
    bool node_count = false;
    bool goal_rank_mapping = false;
    bool max_wire_packet_bytes = false;
    bool data_header_bytes = false;
    bool fixed_propagation_delay_ps = false;

    bool topology_file = false;
    bool hop_latency_ps = false;
    bool switch_latency_ps = false;
    bool global_prbs_seed = false;
    bool control_deadline_ps = false;
    bool margin_ppm = false;
    bool control_wire_bytes = false;
    bool ring_delay_window_ps = false;
    bool ring_release_tick_ps = false;
    bool ring_wire_capacity_bytes = false;
    bool tomahawk3_shared_buffer_bytes = false;
};

struct RnicAtlahsCliOptions {
    std::string goal_file;
    RnicAtlahsGoalRankMapping goal_rank_mapping =
        RnicAtlahsGoalRankMapping::Auto;
    // Zero means the driver must derive the physical rank count from GOAL.
    std::uint32_t node_count;
    std::uint64_t link_capacity_bps;
    RnicProfile profile;

    RnicAtlahsPacketCliOptions packet;
    RnicAtlahsManifoldCliOptions manifold;
    RnicAtlahsCollectiveCliOptions collective;
    RnicAtlahsExplicitCliOptions explicitly_supplied;
};

// Parse a conventional argc/argv array including argv[0].  Every accepted
// option has exactly one value and may occur exactly once.  Numeric values are
// unsigned base-10 integers in the units named by the flag; floating-point
// unit conversion and legacy aliases are intentionally unsupported.
RnicAtlahsCliOptions parseRnicAtlahsCli(
    int argc, const char* const argv[]);

// HTSIM's generated two-tier Clos has N = K^2/2 endpoints for an even
// Tomahawk3 radix K. The driver also applies this predicate after deriving N
// from GOAL, when no explicit -nodes validation override was supplied.
bool isRnicGeneratedTwoTierClosNodeCount(
    std::uint32_t node_count) noexcept;

const char* rnicAtlahsGoalRankMappingName(
    RnicAtlahsGoalRankMapping mapping);
std::string rnicAtlahsCliUsage(const std::string& program_name);

#endif  // RNIC_ATLAHS_CLI_H
