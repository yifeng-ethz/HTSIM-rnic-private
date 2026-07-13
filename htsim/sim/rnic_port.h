// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PORT_H
#define RNIC_PORT_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "rnic_prbs_pacer.h"
#include "rnic_ring_cam.h"

struct RnicTxPacket {
    uint64_t flow_id;
    uint64_t packet_index;
    uint64_t byte_offset;
    uint64_t payload_bytes;
    uint64_t charged_wire_bytes;
    uint64_t dispatch_start_ps;
    uint64_t dispatch_end_ps;
    uint64_t eta_ps;
};

struct RnicTxOpportunity {
    uint64_t start_ps;
    uint64_t end_ps;
    std::optional<RnicTxPacket> packet;
};

// One physical L2 transmit port shared by every flow at an RNIC node.
class RnicTxPort {
public:
    RnicTxPort(uint64_t node_id,
               uint64_t access_capacity_bps,
               uint64_t wire_quantum_bytes,
               uint64_t global_prbs_seed);

    void addFlow(uint64_t flow_id, uint64_t size_bytes, uint64_t calibrated_transit_ps);
    void setGrant(uint64_t flow_id, uint64_t grant_bps);
    void setDataEligible(uint64_t flow_id, bool eligible);

    bool contains(uint64_t flow_id) const;
    bool flowComplete(uint64_t flow_id) const;
    uint64_t flowBytesDispatched(uint64_t flow_id) const;
    uint64_t effectiveRateBps(uint64_t flow_id) const;
    size_t flowCount() const { return _flows.size(); }
    uint64_t nextWireOpportunityPs() const { return _next_wire_opportunity_ps; }
    uint64_t wireOpportunityDurationPs() const { return _wire_opportunity_duration_ps; }
    const RnicPrbsManifest& prbsManifest() const { return _pacer.manifest(); }

    // One call consumes exactly one fixed wire opportunity, including an idle
    // PRBS outcome. requested_start_ps may be later than the next free time but
    // never earlier. A short final packet is charged a full wire quantum.
    RnicTxOpportunity dispatchOpportunity(uint64_t requested_start_ps);

private:
    struct FlowState {
        uint64_t flow_id;
        uint64_t size_bytes;
        uint64_t calibrated_transit_ps;
        uint64_t grant_bps = 0;
        uint64_t effective_rate_bps = 0;
        uint64_t bytes_dispatched = 0;
        uint64_t packet_index = 0;
        bool data_eligible = false;
    };

    void recomputeEffectiveRates();
    FlowState& requireFlow(uint64_t flow_id);
    const FlowState& requireFlow(uint64_t flow_id) const;

    uint64_t _access_capacity_bps;
    uint64_t _wire_quantum_bytes;
    uint64_t _wire_opportunity_duration_ps;
    uint64_t _next_wire_opportunity_ps = 0;
    RnicPrbsPacer _pacer;
    std::map<uint64_t, FlowState> _flows;
};

struct RnicRxDelivery {
    RnicRingCamRelease release;
    uint64_t serializer_start_ps;
    uint64_t serializer_end_ps;
};

struct RnicRxArrivalResult {
    RnicRingCamAdmission admission;
    std::optional<uint64_t> logical_release_ps;
    std::vector<RnicRxDelivery> deliveries_released_before_admission;
};

// One physical L2 receive port: shared Ring-CAM first, then one serializer,
// then per-flow byte accounting.
class RnicRxPort {
public:
    RnicRxPort(uint64_t access_capacity_bps, RnicRingCamConfig ring_cam_config);

    RnicRxArrivalResult processArrival(const RnicRingCamPacket& packet);
    std::vector<RnicRxDelivery> advanceTo(uint64_t now_ps);

    uint64_t serializerAvailablePs() const { return _serializer_available_ps; }
    uint64_t deliveredBytes(uint64_t flow_id) const;
    const RnicRingCam& ringCam() const { return _ring_cam; }

private:
    std::vector<RnicRxDelivery> serialize(
        const std::vector<RnicRingCamRelease>& logical_releases);
    void accountDeliveriesThrough(uint64_t now_ps);

    uint64_t _access_capacity_bps;
    uint64_t _serializer_available_ps = 0;
    RnicRingCam _ring_cam;
    std::multimap<uint64_t, RnicRingCamPacket> _pending_deliveries;
    std::map<uint64_t, uint64_t> _delivered_bytes_by_flow;
};

class RnicNode {
public:
    RnicNode(uint64_t node_id,
             uint64_t access_capacity_bps,
             uint64_t wire_quantum_bytes,
             uint64_t global_prbs_seed,
             RnicRingCamConfig ring_cam_config)
        : _node_id(node_id),
          _tx_port(node_id,
                   access_capacity_bps,
                   wire_quantum_bytes,
                   global_prbs_seed),
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
