// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_CONTROL_H
#define RNIC_COLLECTIVE_CONTROL_H

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

struct RnicCollectiveGrant {
    uint64_t flow_id;
    uint64_t membership_epoch;
    uint32_t n_hat;
    uint64_t wire_rate_bps;
};

// Receiver-side membership and direct explicit-rate calculation for RNIC-CN.
// Transport of declarations and grants is deliberately outside this class: the
// integration must carry them in band through the simulated Clos.
class RnicCollectiveController {
public:
    static constexpr uint32_t kPartsPerMillion = 1000000;
    static constexpr uint32_t kDefaultMarginPpm = 900000;

    explicit RnicCollectiveController(
        uint64_t bottleneck_wire_capacity_bps,
        uint32_t margin_ppm = kDefaultMarginPpm);

    bool declareFlow(uint64_t flow_id);
    bool retireFlow(uint64_t flow_id);
    bool contains(uint64_t flow_id) const;

    size_t activeFlowCount() const { return _active_flow_ids.size(); }
    uint64_t membershipEpoch() const { return _membership_epoch; }
    uint64_t bottleneckWireCapacityBps() const {
        return _bottleneck_wire_capacity_bps;
    }
    uint32_t marginPpm() const { return _margin_ppm; }

    RnicCollectiveGrant grantFor(uint64_t flow_id) const;
    std::vector<RnicCollectiveGrant> grantsForAll() const;

private:
    uint64_t currentWireRateBps() const;

    uint64_t _bottleneck_wire_capacity_bps;
    uint32_t _margin_ppm;
    uint64_t _membership_epoch = 0;
    std::set<uint64_t> _active_flow_ids;
};

class RnicSenderGrantGate {
public:
    enum class Phase {
        Idle,
        DeclarationInFlight,
        Active,
        Retired,
    };

    explicit RnicSenderGrantGate(uint64_t flow_id) : _flow_id(flow_id) {}

    void declarationDispatched();
    void accept(const RnicCollectiveGrant& grant);
    bool applyGrantUpdate(const RnicCollectiveGrant& grant);
    void retire();

    uint64_t flowId() const { return _flow_id; }
    Phase phase() const { return _phase; }
    bool dataEligible() const { return _phase == Phase::Active; }
    uint64_t currentWireRateBps() const { return _current_wire_rate_bps; }
    uint64_t membershipEpoch() const { return _membership_epoch; }

private:
    void validateGrantIdentity(const RnicCollectiveGrant& grant) const;

    uint64_t _flow_id;
    Phase _phase = Phase::Idle;
    uint64_t _current_wire_rate_bps = 0;
    uint64_t _membership_epoch = 0;
};

#endif
