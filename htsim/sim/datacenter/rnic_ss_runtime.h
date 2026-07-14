// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_SS_RUNTIME_H
#define RNIC_SS_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "atlahs_state_trace.h"
#include "atlahs_flow_runtime.h"
#include "eventlist.h"
#include "rnic_packet_extent.h"
#include "rnic_ss_core.h"

class FatTreeTopology;

struct RnicSsRuntimeConfig {
    std::uint64_t access_wire_capacity_bps;
    RnicDataPacketizationConfig packetization;
    std::uint64_t control_wire_bytes{64};
    RnicSsSelectiveRepeatConfig selective_repeat{};
    RnicSsPathSelectionConfig path_selection{
        8, 4, 0, 10'000'000ULL, 0};
    RnicSsCreditConfig credit{};
    std::uint64_t q_hi_bytes{4ULL << 20};
    std::uint64_t q_lo_bytes{2ULL << 20};
    std::uint32_t credit_quantum_packets{4};
    std::uint64_t extra_telemetry_delay_ps{0};
    std::uint64_t routing_seed{UINT64_C(0x5a17b1c3d00df00d)};
    // Ordered endpoint-pair routing is the controlled-Clos default.  Explicit
    // unordered packet routing remains available as a sensitivity study.
    bool unordered_packet_routing{false};
    // Normal experiments are lossless invariants.  Only an explicitly named
    // recovery stress may tolerate a fabric drop or RTO at quiescence.
    bool allow_loss_stress{false};
    // Optional sparse, event-driven sender state trace.  The runtime buffers
    // rows and installs the CSV only after physical quiescence.
    std::optional<std::string> state_trace_csv;

    void validate() const;
};

struct RnicSsRuntimeStatistics {
    std::uint64_t new_data_packets{0};
    std::uint64_t sack_retransmissions{0};
    std::uint64_t rto_retransmissions{0};
    std::uint64_t rto_deadline_pushes{0};
    std::uint64_t rto_deadline_stale_pops{0};
    std::uint64_t rto_deadline_due_pops{0};
    std::uint64_t rto_deadline_heap_high_watermark{0};
    std::uint64_t ack_sack_packets{0};
    std::uint64_t bp_enable_packets{0};
    std::uint64_t bp_disable_packets{0};
    std::uint64_t credit_packets{0};
    std::uint64_t fabric_drops{0};
    std::uint64_t source_data_serializer_packets{0};
    std::uint64_t source_ack_serializer_packets{0};
    std::uint64_t source_bp_serializer_packets{0};
    std::uint64_t source_credit_serializer_packets{0};
    std::uint64_t source_priority_violations{0};
    // Analytical components of the controlled-Clos queue envelope.  The
    // bound_control_loop interval is forward_data_observation plus the time
    // until the last serialized BP_ENABLE can reach a contributor.
    std::uint64_t physical_forward_data_observation_ps{0};
    std::uint64_t physical_last_bp_enable_feedback_ps{0};
    std::uint64_t physical_bound_control_loop_ps{0};
    std::uint64_t maximum_bp_enable_fan_in{0};
    std::uint64_t control_loop_window_packets{0};
    std::uint64_t analytical_queue_bound_bytes{0};
    bool configured_window_below_control_loop{false};
};

struct RnicSsFlowSnapshot {
    AtlahsFlowRequest request;
    std::uint64_t source_payload_bytes_packetized;
    std::uint64_t delivered_payload_bytes;
    std::uint64_t delivered_data_packets;
    bool completion_notified;
};

// Physical ATLAHS runtime for the open rnic-ss comparator.  It is limited to
// the controlled two-tier ns-rosetta Clos.  Every sender decision uses its
// local request depth plus telemetry returned in a physical ACK/SACK.
class RnicSsRuntime final : public AtlahsFlowRuntime,
                            private EventSource {
public:
    RnicSsRuntime(EventList& event_list,
                  FatTreeTopology& topology,
                  RnicSsRuntimeConfig config);
    ~RnicSsRuntime() override;

    RnicSsRuntime(const RnicSsRuntime&) = delete;
    RnicSsRuntime& operator=(const RnicSsRuntime&) = delete;

    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;
    bool hasPendingPhysicalWork() const noexcept override;

    bool isSetup() const noexcept;
    std::size_t flowCount() const noexcept;
    RnicSsFlowSnapshot flow(AtlahsFlowId flow_id) const;
    const RnicSsRuntimeStatistics& statistics() const noexcept;
    void validateQuiescent() const;
    std::size_t stateTraceRowCount() const noexcept;
    void writeStateTraceCsv() const;

private:
    struct Impl;
    void doNextEvent() override;
    bool isTraffic() override { return true; }

    std::unique_ptr<Impl> _impl;
};

#endif  // RNIC_SS_RUNTIME_H
