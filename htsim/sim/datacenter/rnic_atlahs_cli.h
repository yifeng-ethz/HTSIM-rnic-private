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
// missing topology file means a generated two-tier ns-tm3 Clos.  A supplied
// topology file still has to resolve to that same checked physical model in
// the assembly layer.
struct RnicAtlahsCollectiveCliOptions {
    std::optional<std::string> topology_file;
    std::optional<std::string> queue_trace_csv;
    std::optional<std::string> state_trace_csv;
    std::uint64_t hop_latency_ps = 1000000;
    std::uint64_t switch_latency_ps = 0;
    std::uint64_t global_prbs_seed = UINT64_C(0x123456789abcdef0);
    std::uint64_t control_deadline_ps = 10000000;
    std::uint32_t margin_ppm = 900000;
    std::uint64_t control_wire_bytes = 64;
    std::uint64_t ring_delay_window_ps = 4096000;
    std::uint64_t ring_release_tick_ps = 16000;
    std::uint64_t ring_wire_capacity_bytes = UINT64_C(1) << 20;
    std::uint64_t ns_tm3_shared_buffer_bytes = UINT64_C(1) << 20;
    std::uint32_t maximum_repair_retries = 8;
};

// Public-mechanism parameters for the open rnic-ss comparator.  None of these
// defaults is presented as proprietary hardware arithmetic; every threshold
// remains explicit for the required sensitivity sweeps.
struct RnicAtlahsSlingshotCliOptions {
    std::optional<std::string> state_trace_csv;
    std::uint64_t control_wire_bytes = 64;
    std::uint64_t ns_rosetta_shared_buffer_bytes = UINT64_C(64) << 20;
    std::uint64_t q_hi_bytes = UINT64_C(4) << 20;
    std::uint64_t q_lo_bytes = UINT64_C(2) << 20;
    std::uint64_t telemetry_delay_ps = 0;
    std::uint64_t path_hysteresis_ps = 0;
    std::uint64_t maximum_sample_age_ps = 10000000;
    std::uint32_t credit_quantum_packets = 4;
    std::uint32_t outstanding_window_packets = 128;
    std::uint64_t rto_ps = UINT64_C(50000000000);
    std::uint32_t maximum_retransmissions = 8;
    std::uint64_t maximum_credit_ahead_bytes = UINT64_C(64) << 20;
    std::uint64_t routing_seed = UINT64_C(0x5a17b1c3d00df00d);
    bool unordered_packet_routing = false;
    bool allow_loss_stress = false;
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
    bool ns_tm3_shared_buffer_bytes = false;
    bool cn_queue_trace_csv = false;
    bool cn_state_trace_csv = false;
    bool cn_maximum_repair_retries = false;

    bool ss_control_wire_bytes = false;
    bool ss_state_trace_csv = false;
    bool ns_rosetta_shared_buffer_bytes = false;
    bool ss_q_hi_bytes = false;
    bool ss_q_lo_bytes = false;
    bool ss_telemetry_delay_ps = false;
    bool ss_path_hysteresis_ps = false;
    bool ss_maximum_sample_age_ps = false;
    bool ss_credit_quantum_packets = false;
    bool ss_outstanding_window_packets = false;
    bool ss_rto_ps = false;
    bool ss_maximum_retransmissions = false;
    bool ss_maximum_credit_ahead_bytes = false;
    bool ss_routing_seed = false;
    bool ss_routing = false;
    bool ss_loss_stress = false;
};

struct RnicAtlahsCliOptions {
    std::string goal_file;
    std::optional<std::string> completion_csv;
    RnicAtlahsGoalRankMapping goal_rank_mapping =
        RnicAtlahsGoalRankMapping::Auto;
    // Zero means the driver must derive the physical rank count from GOAL.
    std::uint32_t node_count;
    std::uint64_t link_capacity_bps;
    RnicProfile profile;

    RnicAtlahsPacketCliOptions packet;
    RnicAtlahsManifoldCliOptions manifold;
    RnicAtlahsCollectiveCliOptions collective;
    RnicAtlahsSlingshotCliOptions slingshot;
    RnicAtlahsExplicitCliOptions explicitly_supplied;
};

// Parse a conventional argc/argv array including argv[0].  Every accepted
// option has exactly one value and may occur exactly once.  Numeric values are
// unsigned base-10 integers in the units named by the flag; floating-point
// unit conversion and legacy aliases are intentionally unsupported.
RnicAtlahsCliOptions parseRnicAtlahsCli(
    int argc, const char* const argv[]);

// HTSIM's generated two-tier Clos has N = K^2/2 endpoints for an even
// ns-tm3 radix K. The driver also applies this predicate after deriving N
// from GOAL, when no explicit -nodes validation override was supplied.
bool isRnicGeneratedTwoTierClosNodeCount(
    std::uint32_t node_count) noexcept;

const char* rnicAtlahsGoalRankMappingName(
    RnicAtlahsGoalRankMapping mapping);
std::string rnicAtlahsCliUsage(const std::string& program_name);

#endif  // RNIC_ATLAHS_CLI_H
