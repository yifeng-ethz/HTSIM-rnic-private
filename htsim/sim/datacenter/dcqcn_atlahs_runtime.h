// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DCQCN_ATLAHS_RUNTIME_H
#define DCQCN_ATLAHS_RUNTIME_H

#include "atlahs_flow_runtime.h"
#include "config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class EventList;
class FatTreeTopology;
class FatTreeTopologyCfg;

struct DcqcnAtlahsRuntimeConfig {
    std::string topology_file;
    linkspeed_bps endpoint_link_bps{UINT64_C(400000000000)};
    std::uint16_t max_wire_packet_bytes{4096};
    std::uint16_t data_header_bytes{64};
    mem_b ns_tm3_shared_buffer_bytes{32 * 1024 * 1024};
    mem_b ns_tm3_egress_buffer_bytes{32 * 1024 * 1024};
    mem_b ecn_kmin_bytes{65536};
    mem_b ecn_kmax_bytes{655360};
    std::uint32_t ecn_pmax_ppm{250000};
    std::uint64_t ecn_seed{1};
    mem_b pfc_low_threshold_bytes{520000};
    mem_b pfc_high_threshold_bytes{720000};
    simtime_picosec silent_loss_rto_ps{UINT64_C(50000000000)};
    linkspeed_bps dcqcn_min_rate_bps{UINT64_C(100000000)};
    // Comparator-realism ruling (2026-08-04): pfc off is the ECN-only mode
    // in which buffer overflow drops packets and recovery is the
    // transport's job; selective_repeat models the mlx5 ConnectX-6 Dx
    // limited selective repeat with go-back-N fallback beyond the window;
    // loss_rate_cut couples every loss recovery event to the CNP-style
    // multiplicative rate cut.
    bool pfc_enabled{true};
    bool selective_repeat{false};
    std::uint32_t sr_window_packets{64};
    bool loss_rate_cut{true};
    std::uint64_t ecmp_seed{1};
    std::optional<std::string> state_trace_csv;
    std::optional<std::string> goodput_trace_csv;
    simtime_picosec goodput_trace_bin_ps{0};
};

class DcqcnAtlahsRuntime final : public AtlahsFlowRuntime {
public:
    DcqcnAtlahsRuntime(EventList& event_list,
                       DcqcnAtlahsRuntimeConfig config,
                       std::uint32_t physical_node_count);
    ~DcqcnAtlahsRuntime() override;

    DcqcnAtlahsRuntime(const DcqcnAtlahsRuntime&) = delete;
    DcqcnAtlahsRuntime& operator=(const DcqcnAtlahsRuntime&) = delete;

    void setup(std::uint32_t node_count, CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;
    bool hasPendingPhysicalWork() const noexcept override;
    AtlahsTransportKind transportKind() const noexcept override {
        return AtlahsTransportKind::DcqcnQueuePair;
    }

    FatTreeTopology& topology() noexcept;
    const FatTreeTopology& topology() const noexcept;
    FatTreeTopologyCfg& topology_config() noexcept;
    const FatTreeTopologyCfg& topology_config() const noexcept;
    const DcqcnAtlahsRuntimeConfig& config() const noexcept;

    std::uint64_t completed_flow_count() const noexcept;
    std::uint64_t silent_rto_count() const noexcept;
    std::uint64_t loss_rate_cut_count() const noexcept;
    std::uint64_t ecn_marked_packet_count() const noexcept;
    std::uint64_t pfc_pause_count() const noexcept;
    std::uint64_t pfc_resume_count() const noexcept;
    std::uint64_t pfc_paused_wall_ps_total() const noexcept;
    std::uint32_t pfc_max_cascade_depth() const noexcept;
    // Per-switch and per-port dcqcn_pfc_* manifest lines (measurement
    // only); empty when no pause was ever sent.
    std::string renderPfcPortMetricsManifest() const;
    std::uint64_t dropped_packet_count() const noexcept;
    std::uint64_t shared_pool_dropped_packet_count() const noexcept;
    std::uint64_t egress_domain_dropped_packet_count() const noexcept;
    std::size_t state_trace_row_count() const noexcept;
    void writeStateTraceCsv() const;
    std::size_t goodput_trace_row_count() const noexcept;
    void writeGoodputTraceCsv() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

std::string renderDcqcnAtlahsManifest(const DcqcnAtlahsRuntimeConfig& config,
                                      std::uint32_t physical_node_count,
                                      const std::string& goal_file,
                                      const std::string& completion_csv,
                                      const std::string& state_trace_csv,
                                      const char* resolved_rank_mapping);

#endif  // DCQCN_ATLAHS_RUNTIME_H
