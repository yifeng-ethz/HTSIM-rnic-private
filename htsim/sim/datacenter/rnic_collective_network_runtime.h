// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_NETWORK_RUNTIME_H
#define RNIC_COLLECTIVE_NETWORK_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "atlahs_flow_runtime.h"
#include "eventlist.h"
#include "rnic_collective_control.h"
#include "rnic_collective_packet.h"
#include "rnic_packet_extent.h"
#include "rnic_ring_cam.h"

class FatTreeTopology;
class RnicNode;

struct RnicCollectiveNetworkConfig {
    using TransitCalibration =
        std::function<std::uint64_t(
            std::uint32_t,
            std::uint32_t,
            const RnicPacketExtent&)>;

    std::uint64_t access_wire_capacity_bps;
    RnicDataPacketizationConfig packetization;
    RnicRingCamConfig ring_cam;
    std::uint64_t global_prbs_seed;
    std::uint64_t control_deadline_ps;
    std::uint32_t margin_ppm;
    std::uint64_t control_wire_bytes;
    TransitCalibration calibrated_transit_ps;
};

struct RnicCollectiveFlowSnapshot {
    AtlahsFlowRequest request;
    RnicCollectiveFinalLedger final_ledger;
    RnicSenderGrantGate::Phase sender_phase;
    std::uint64_t current_wire_rate_bps;
    std::uint64_t source_payload_bytes_dispatched;
    std::uint64_t source_wire_bytes_dispatched;
    std::uint64_t source_data_packets_dispatched;
    std::uint64_t delivered_payload_bytes;
    std::uint64_t delivered_wire_bytes;
    std::uint64_t delivered_data_packets;
    bool declaration_dispatched;
    bool retire_dispatched;
    bool retire_received;
    bool retirement_queued;
    bool receiver_retired;
    bool completion_notified;
    std::optional<std::uint64_t> delivery_completion_time_ps;
    std::optional<std::uint64_t> retirement_completion_time_ps;
};

// Physical packet runtime for the `rnic-cn` profile.  Every flow uses one
// node-wide PRBS TX port, traverses explicit routes through a two-tier
// ns-tm3 Clos, and enters one shared Ring-CAM/RX serializer. Receiver
// membership is changed only by in-band DECLARE/ACCEPT/UPDATE/RETIRE waves.
class RnicCollectiveNetworkRuntime final : public AtlahsFlowRuntime,
                                           private EventSource {
public:
    RnicCollectiveNetworkRuntime(
        EventList& event_list,
        FatTreeTopology& topology,
        RnicCollectiveNetworkConfig config);
    ~RnicCollectiveNetworkRuntime() override;

    RnicCollectiveNetworkRuntime(
        const RnicCollectiveNetworkRuntime&) = delete;
    RnicCollectiveNetworkRuntime& operator=(
        const RnicCollectiveNetworkRuntime&) = delete;

    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;

    bool isSetup() const noexcept;
    std::uint32_t nodeCount() const noexcept;
    std::size_t flowCount() const noexcept;
    bool contains(AtlahsFlowId flow_id) const noexcept;
    RnicCollectiveFlowSnapshot flow(AtlahsFlowId flow_id) const;
    std::size_t receiverActiveFlowCount(std::uint32_t node_id) const;
    std::size_t pendingFabricPacketCount() const noexcept;
    std::size_t pendingDestinationDataCount() const noexcept;
    bool hasPendingPhysicalWork() const noexcept override;
    const RnicNode& node(std::uint32_t node_id) const;

    // Strong end-of-run assertion used by tests and manifest-producing
    // drivers. Completed flow history may remain, but no physical/control
    // work, receiver membership, or routed packet may remain live.
    void validateQuiescent() const;

private:
    struct Impl;

    void doNextEvent() override;
    bool isTraffic() override { return true; }

    std::unique_ptr<Impl> _impl;
};

#endif  // RNIC_COLLECTIVE_NETWORK_RUNTIME_H
