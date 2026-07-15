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
class RnicCollectiveNetworkRuntimeTestPeer;

struct RnicCollectiveNetworkConfig {
    using TransitCalibration =
        std::function<std::uint64_t(std::uint32_t, std::uint32_t, const RnicPacketExtent&)>;

    std::uint64_t access_wire_capacity_bps;
    RnicDataPacketizationConfig packetization;
    RnicRingCamConfig ring_cam;
    std::uint64_t global_prbs_seed;
    std::uint64_t control_deadline_ps;
    std::uint32_t margin_ppm;
    std::uint64_t control_wire_bytes;
    TransitCalibration calibrated_transit_ps;
    // Maximum number of deterministic retransmissions after the original.
    // A retransmission that is still late at this attempt fails explicitly.
    std::uint32_t maximum_retransmissions{8};
    // Sender-visible overtime for a retry that receives neither a subsequent
    // exact-gap NACK nor a physical GAP_RESOLVED closure.
    std::uint64_t retransmission_rto_ps{50000000000ULL};
};

struct RnicCollectiveRecoveryStatistics {
    std::uint64_t late_data_packets{0};
    std::uint64_t gap_nacks_dispatched{0};
    std::uint64_t gap_nacks_received{0};
    std::uint64_t deterministic_retransmissions{0};
    std::uint64_t deterministic_retransmission_wire_bytes{0};
    std::uint64_t duplicate_gap_nacks_ignored{0};
    std::uint64_t duplicate_data_packets_ignored{0};
    std::uint32_t maximum_retry_attempt_observed{0};
};

struct RnicCollectiveFlowSnapshot {
    AtlahsFlowRequest request;
    RnicCollectiveFinalLedger final_ledger;
    RnicSenderGrantGate::Phase sender_phase;
    std::uint64_t current_wire_rate_bps;
    std::uint64_t membership_epoch;
    std::uint64_t grant_lease_expiry_ps;
    std::uint64_t marked_data_packets_dispatched;
    std::uint64_t marked_rate_acks_generated;
    std::uint64_t marked_rate_acks_received;
    std::uint64_t rate_refresh_declarations_dispatched;
    std::uint64_t rate_refresh_acks_generated;
    std::uint64_t rate_refresh_acks_received;
    std::uint64_t source_payload_bytes_dispatched;
    std::uint64_t source_wire_bytes_dispatched;
    std::uint64_t source_data_packets_dispatched;
    std::uint64_t delivered_payload_bytes;
    std::uint64_t delivered_wire_bytes;
    std::uint64_t delivered_data_packets;
    std::uint64_t late_data_packets;
    std::uint64_t gap_nacks_dispatched;
    std::uint64_t gap_nacks_received;
    std::uint64_t deterministic_retransmissions;
    std::uint64_t deterministic_retransmission_wire_bytes;
    std::uint64_t duplicate_gap_nacks_ignored;
    std::uint64_t duplicate_data_packets_ignored;
    std::uint32_t maximum_retry_attempt_observed;
    std::size_t missing_data_packets;
    std::size_t ready_out_of_order_packets;
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
// membership is changed only by in-band DECLARE/ACCEPT/RETIRE controls.
// GRANT_UPDATE is normally the reverse-path ACK for one post-resequence
// marked DATA packet; incumbents apply it independently, while new senders
// wait K+2*dwnd. A sender whose lease expires closes its DATA gate,
// re-DECLAREs idempotently, and resumes only after the receiver returns the
// current explicit rate. The same fail-closed rule applies to bounded
// retransmissions after fresh payload has been dispatched.
class RnicCollectiveNetworkRuntime final : public AtlahsFlowRuntime, private EventSource {
public:
    RnicCollectiveNetworkRuntime(EventList& event_list,
                                 FatTreeTopology& topology,
                                 RnicCollectiveNetworkConfig config);
    ~RnicCollectiveNetworkRuntime() override;

    RnicCollectiveNetworkRuntime(const RnicCollectiveNetworkRuntime&) = delete;
    RnicCollectiveNetworkRuntime& operator=(const RnicCollectiveNetworkRuntime&) = delete;

    void setup(std::uint32_t node_count, CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;

    bool isSetup() const noexcept;
    std::uint32_t nodeCount() const noexcept;
    std::size_t flowCount() const noexcept;
    bool contains(AtlahsFlowId flow_id) const noexcept;
    RnicCollectiveFlowSnapshot flow(AtlahsFlowId flow_id) const;
    std::size_t receiverActiveFlowCount(std::uint32_t node_id) const;
    std::size_t pendingFabricPacketCount() const noexcept;
    std::size_t pendingDestinationDataCount() const noexcept;
    const RnicCollectiveRecoveryStatistics& recoveryStatistics() const noexcept;
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

    // Narrow, private fault-injection seam used to prove replay
    // idempotence with real routed packets.  It is deliberately inaccessible
    // to drivers and profiles, so production simulations cannot enable it.
    void duplicateCurrentGapNackForTesting(AtlahsFlowId flow_id);
    void duplicateCurrentRetransmissionForTesting(AtlahsFlowId flow_id);
    void dropOriginalDataForTesting(AtlahsFlowId flow_id, std::uint64_t packet_index);
    void dropDataAttemptForTesting(AtlahsFlowId flow_id,
                                   std::uint64_t packet_index,
                                   std::uint32_t transmission_attempt);
    void duplicateOriginalDataForTesting(AtlahsFlowId flow_id, std::uint64_t packet_index);
    void replayResolvedGapNackForTesting(AtlahsFlowId flow_id, std::uint64_t packet_index);
    std::uint64_t maxOriginalReleaseForTesting(AtlahsFlowId flow_id) const;
    std::uint64_t finalOriginalReleaseForTesting(AtlahsFlowId flow_id) const;
    std::optional<std::uint64_t> publishedRetireDeadlineForTesting(AtlahsFlowId flow_id) const;
    std::optional<std::uint64_t> firstGapObservationForTesting(AtlahsFlowId flow_id) const;
    std::optional<std::uint64_t> firstGapDecisionForTesting(AtlahsFlowId flow_id) const;
    std::optional<std::uint64_t> retryDispatchForTesting(AtlahsFlowId flow_id,
                                                         std::uint32_t transmission_attempt) const;
    bool markFreshDataForTesting(AtlahsFlowId flow_id, std::uint64_t eta_ps);
    friend class RnicCollectiveNetworkRuntimeTestPeer;

    std::unique_ptr<Impl> _impl;
};

#endif  // RNIC_COLLECTIVE_NETWORK_RUNTIME_H
