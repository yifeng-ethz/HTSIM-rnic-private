// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PORT_H
#define RNIC_PORT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "rnic_packet_extent.h"
#include "rnic_prbs_pacer.h"
#include "rnic_ring_cam.h"
#include "rnic_wire_serialization.h"

enum class RnicTxPacketKind {
    FreshData,
    Retransmission,
};

struct RnicTxRetransmissionCandidate {
    uint64_t flow_id;
    uint64_t packet_index;
    uint64_t payload_byte_offset;
    RnicPacketExtent extent;
};

struct RnicTxPacket {
    uint64_t flow_id;
    uint64_t packet_index;
    uint64_t payload_byte_offset;
    RnicPacketExtent extent;
    uint64_t dispatch_start_ps;
    uint64_t dispatch_end_ps;
    uint64_t eta_ps;
    RnicTxPacketKind kind{RnicTxPacketKind::FreshData};
};

struct RnicTxOpportunity {
    uint64_t start_ps;
    uint64_t end_ps;
    std::optional<RnicTxPacket> packet;
};

// One physical L2 transmit port shared by every flow at an RNIC node.
class RnicTxPort {
public:
    // No-queue transit starts after this port's source serializer.  The
    // exact DATA extent is supplied at dispatch so a short final packet is
    // never calibrated as a maximum-size packet.
    using TransitCalibration = std::function<uint64_t(const RnicPacketExtent&)>;

    RnicTxPort(uint64_t node_id,
               uint64_t access_capacity_bps,
               uint64_t max_wire_packet_bytes,
               uint64_t global_prbs_seed,
               uint64_t eligibility_tick_ps = 0);
    RnicTxPort(uint64_t node_id,
               uint64_t access_capacity_bps,
               RnicDataPacketizationConfig packetization,
               uint64_t global_prbs_seed,
               uint64_t eligibility_tick_ps = 0);

    void addFlow(uint64_t flow_id,
                 uint64_t payload_size_bytes,
                 TransitCalibration calibrated_transit_ps);

    // Convenience for tests and topology-free callers whose transit is
    // independent of packet extent.
    void addFlow(uint64_t flow_id, uint64_t payload_size_bytes, uint64_t calibrated_transit_ps);
    void setWireRateGrant(uint64_t flow_id, uint64_t wire_rate_grant_bps);
    void setDataEligible(uint64_t flow_id, bool eligible);

    // Makes one per-flow deterministic-retransmission head participate in the
    // same PRBS lottery and source-edge progressive filling as fresh DATA. The
    // runtime owns the retransmission metadata and supplies the exact head to
    // dispatchOpportunity(); this bit only records whether that head exists.
    void setRetransmissionPending(uint64_t flow_id, bool pending);

    // Runtime-only terminal cleanup after receiver retirement has committed.
    // The source payload must already be fully dispatched, and the runtime
    // must first close DATA eligibility and clear the receiver grant.
    void removeRetiredFlow(uint64_t flow_id);

    bool contains(uint64_t flow_id) const;
    bool sourcePayloadDispatched(uint64_t flow_id) const;
    uint64_t flowPayloadBytesDispatched(uint64_t flow_id) const;
    uint64_t effectiveWireRateBps(uint64_t flow_id) const;
    bool hasRetransmissionPending(uint64_t flow_id) const;
    bool hasDispatchableData() const;
    size_t flowCount() const { return _flows.size(); }
    uint64_t nextDataOpportunityPs() const { return _data_opportunity_serializer.availablePs(); }
    uint64_t physicalSerializerAvailablePs() const { return _wire_serializer.availablePs(); }
    uint64_t nextWireOpportunityPs() const;
    uint64_t maxWirePacketBytes() const { return _packetization.maxWirePacketBytes(); }
    uint64_t dataHeaderBytes() const { return _packetization.dataHeaderBytes(); }
    uint64_t maxDataPayloadBytes() const { return _packetization.maxPayloadBytes(); }
    const RnicPrbsManifest& prbsManifest() const { return _pacer.manifest(); }

    // One call consumes one selected DATA extent or advances one virtual,
    // max-wire-sized idle opportunity.  Idle PRBS outcomes do not reserve the
    // physical serializer, so a control frame may use that otherwise empty
    // interval.  A selected short final is serialized at its exact wire
    // extent; the size-aware lottery preserves wire-byte rather than packet
    // shares.
    RnicTxOpportunity dispatchOpportunity(uint64_t requested_start_ps);
    RnicTxOpportunity dispatchOpportunity(
        uint64_t requested_start_ps,
        const std::vector<RnicTxRetransmissionCandidate>& retransmission_heads);

    // Serialize one nonempty control frame on the same physical node wire as
    // DATA.  The CN runtime owns the strict-priority queue and calls this at a
    // packet boundary before dispatchOpportunity().  Control does not consume
    // a PRBS DATA opportunity.
    RnicWireSerializationInterval dispatchControl(uint64_t requested_start_ps, uint64_t wire_bytes);

    // The runtime calls this only after observing that no real frame is queued
    // at a published completion boundary.
    void rebasePhysicalIdle(uint64_t now_ps);

    // Publish a newly eligible DATA-class head at an integral event boundary.
    // This never dispatches the head; it only prevents a same-ceil fractional
    // opportunity from beginning before the eligibility event.
    void rebaseDataClassIdle(uint64_t now_ps);

private:
    struct FlowState {
        uint64_t flow_id;
        uint64_t payload_size_bytes;
        TransitCalibration calibrated_transit_ps;
        uint64_t wire_rate_grant_bps = 0;
        uint64_t effective_wire_rate_bps = 0;
        uint64_t payload_bytes_dispatched = 0;
        uint64_t packet_index = 0;
        std::optional<RnicPacketExtent> cached_fresh_extent;
        std::optional<uint64_t> cached_fresh_transit_ps;
        std::optional<uint64_t> last_fresh_eta_tick_ps;
        bool data_eligible = false;
        bool retransmission_pending = false;
    };

    RnicPacketExtent headExtent(const FlowState& state) const;
    bool freshHeadPreservesEtaOrder(const FlowState& state,
                                    const RnicPacketExtent& extent,
                                    uint64_t calibrated_transit_ps,
                                    uint64_t requested_start_ps) const;
    void prepareFreshHead(FlowState& state);
    uint64_t quantizedEtaPs(uint64_t eta_ps) const;
    void recomputeEffectiveRates();
    FlowState& requireFlow(uint64_t flow_id);
    const FlowState& requireFlow(uint64_t flow_id) const;

    uint64_t _access_capacity_bps;
    RnicDataPacketizationConfig _packetization;
    RnicWireSerializationClock _wire_serializer;
    RnicWireSerializationClock _data_opportunity_serializer;
    RnicPrbsPacer _pacer;
    uint64_t _eligibility_tick_ps;
    std::map<uint64_t, FlowState> _flows;
};

struct RnicRxScheduledSerialization {
    RnicRingCamRelease release;
    uint64_t serializer_start_ps;
    uint64_t serializer_end_ps;
};

// One exact packet whose destination-link serializer has reached its terminal
// wire boundary.  packet_id is the runtime-visible identity; the retained
// packet also carries flow_id, timestamps, and the exact payload/wire extent.
struct RnicRxPacketCompletion {
    RnicRingCamPacket packet;
    uint64_t serializer_end_ps;
};

struct RnicRxArrivalResult {
    RnicRingCamAdmission admission;
    std::optional<uint64_t> logical_release_ps;
    std::vector<RnicRxScheduledSerialization> serializations_scheduled_before_admission;
    std::vector<RnicRxPacketCompletion> packets_completed_through_arrival;
};

struct RnicRxAdvanceResult {
    std::vector<RnicRxScheduledSerialization> serializations_scheduled;
    std::vector<RnicRxPacketCompletion> packets_completed;
};

// One physical L2 receive port: shared Ring-CAM first, then one serializer,
// then per-flow byte accounting.
class RnicRxPort {
public:
    RnicRxPort(uint64_t access_capacity_bps, RnicRingCamConfig ring_cam_config);

    RnicRxArrivalResult processArrival(const RnicRingCamPacket& packet);

    // The completion-aware entry point for an event-driven runtime.  It
    // releases every Ring-CAM entry due through now_ps, schedules those exact
    // extents on the destination serializer, and returns every packet whose
    // serialization ends no later than now_ps.
    RnicRxAdvanceResult advanceToWithCompletions(uint64_t now_ps);

    // Compatibility wrapper for existing callers that only need newly
    // scheduled serializations; byte accounting still advances as before.
    std::vector<RnicRxScheduledSerialization> advanceTo(uint64_t now_ps);

    // Earliest internal event still requiring a runtime callback: either a
    // Ring-CAM logical release or a destination serializer completion.
    std::optional<uint64_t> nextEventTimePs() const;

    uint64_t serializerAvailablePs() const { return _wire_serializer.availablePs(); }
    uint64_t deliveredPayloadBytes(uint64_t flow_id) const;
    uint64_t deliveredWireBytes(uint64_t flow_id) const;
    uint64_t pendingSerializerWireBytes() const { return _pending_serializer_wire_bytes; }
    uint64_t pendingSerializerHighWatermarkWireBytes() const {
        return _pending_serializer_high_watermark_wire_bytes;
    }
    const RnicRingCam& ringCam() const { return _ring_cam; }

private:
    std::vector<RnicRxScheduledSerialization> scheduleSerializations(
        const std::vector<RnicRingCamRelease>& logical_releases);
    std::vector<RnicRxPacketCompletion> accountDeliveriesThrough(uint64_t now_ps);
    void updateLogicalReleaseTracking(const std::vector<RnicRingCamRelease>& released,
                                      std::optional<uint64_t> admitted_release_ps);

    RnicWireSerializationClock _wire_serializer;
    RnicRingCam _ring_cam;
    std::map<uint64_t, uint64_t> _pending_logical_release_counts;
    std::multimap<uint64_t, RnicRingCamPacket> _pending_serializations;
    uint64_t _pending_serializer_wire_bytes = 0;
    uint64_t _pending_serializer_high_watermark_wire_bytes = 0;
    std::map<uint64_t, uint64_t> _delivered_payload_bytes_by_flow;
    std::map<uint64_t, uint64_t> _delivered_wire_bytes_by_flow;
};

class RnicNode {
public:
    RnicNode(uint64_t node_id,
             uint64_t access_capacity_bps,
             uint64_t wire_quantum_bytes,
             uint64_t global_prbs_seed,
             RnicRingCamConfig ring_cam_config)
        : RnicNode(node_id,
                   access_capacity_bps,
                   RnicDataPacketizationConfig(wire_quantum_bytes),
                   global_prbs_seed,
                   ring_cam_config) {}

    RnicNode(uint64_t node_id,
             uint64_t access_capacity_bps,
             RnicDataPacketizationConfig packetization,
             uint64_t global_prbs_seed,
             RnicRingCamConfig ring_cam_config)
        : _node_id(node_id),
          _tx_port(node_id,
                   access_capacity_bps,
                   packetization,
                   global_prbs_seed,
                   ring_cam_config.release_tick_ps),
          _rx_port(access_capacity_bps, ring_cam_config) {}

    uint64_t nodeId() const { return _node_id; }
    RnicTxPort& txPort() { return _tx_port; }
    RnicRxPort& rxPort() { return _rx_port; }
    const RnicTxPort& txPort() const { return _tx_port; }
    const RnicRxPort& rxPort() const { return _rx_port; }

private:
    uint64_t _node_id;
    RnicTxPort _tx_port;
    RnicRxPort _rx_port;
};

#endif
