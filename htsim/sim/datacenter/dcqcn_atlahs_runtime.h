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
    mem_b ecn_kmin_bytes{65536};
    mem_b ecn_kmax_bytes{655360};
    std::uint32_t ecn_pmax_ppm{250000};
    std::uint64_t ecn_seed{1};
    mem_b pfc_low_threshold_bytes{520000};
    mem_b pfc_high_threshold_bytes{720000};
    simtime_picosec silent_loss_rto_ps{UINT64_C(50000000000)};
    linkspeed_bps dcqcn_min_rate_bps{UINT64_C(100000000)};
    std::uint64_t ecmp_seed{1};
    std::optional<std::string> state_trace_csv;
};

class DcqcnAtlahsRuntime final : public AtlahsFlowRuntime {
public:
    DcqcnAtlahsRuntime(EventList& event_list,
                       DcqcnAtlahsRuntimeConfig config,
                       std::uint32_t physical_node_count);
    ~DcqcnAtlahsRuntime() override;

    DcqcnAtlahsRuntime(const DcqcnAtlahsRuntime&) = delete;
    DcqcnAtlahsRuntime& operator=(const DcqcnAtlahsRuntime&) = delete;

    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;
    bool hasPendingPhysicalWork() const noexcept override;

    FatTreeTopology& topology() noexcept;
    const FatTreeTopology& topology() const noexcept;
    FatTreeTopologyCfg& topology_config() noexcept;
    const FatTreeTopologyCfg& topology_config() const noexcept;
    const DcqcnAtlahsRuntimeConfig& config() const noexcept;

    std::uint64_t completed_flow_count() const noexcept;
    std::uint64_t silent_rto_count() const noexcept;
    std::uint64_t ecn_marked_packet_count() const noexcept;
    std::uint64_t pfc_pause_count() const noexcept;
    std::uint64_t pfc_resume_count() const noexcept;
    std::uint64_t dropped_packet_count() const noexcept;
    std::size_t state_trace_row_count() const noexcept;
    void writeStateTraceCsv() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

std::string renderDcqcnAtlahsManifest(
    const DcqcnAtlahsRuntimeConfig& config,
    std::uint32_t physical_node_count,
    const std::string& goal_file,
    const std::string& completion_csv,
    const std::string& state_trace_csv,
    const char* resolved_rank_mapping);

#endif  // DCQCN_ATLAHS_RUNTIME_H
