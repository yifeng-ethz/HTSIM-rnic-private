// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_CONTROL_H
#define RNIC_COLLECTIVE_CONTROL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

enum class RnicCollectiveGrantKind {
    Invalid,
    Accept,
    Update,
};

struct RnicCollectiveGrant {
    uint64_t flow_id;
    uint64_t membership_epoch;
    uint32_t n_hat;
    uint64_t wire_rate_bps;
    uint64_t effective_time_ps;
    RnicCollectiveGrantKind kind;
};

struct RnicCollectiveMembershipDeclaration {
    uint64_t flow_id;
    uint32_t nflow;
};

struct RnicCollectiveMembershipDelta {
    std::vector<RnicCollectiveMembershipDeclaration> declarations;
    std::vector<uint64_t> retired_flow_ids;
};

// One immutable receiver grant snapshot. Every in-band ACCEPT/UPDATE in the
// wave carries the same effective time. Under the explicitly homogeneous CN
// control-deadline model, senders buffer the wave and change rates together at
// that boundary; a late packet invalidates the run.
struct RnicCollectiveGrantWave {
    uint64_t membership_epoch;
    uint32_t n_hat;
    uint64_t wire_rate_bps;
    uint64_t effective_time_ps;
    std::vector<RnicCollectiveGrant> grants;
};

class RnicCollectiveGrantWaveBarrier;

// Receiver-side membership and direct explicit-rate calculation for the
// collective-network (`rnic-cn`) profile. N_hat is the sum of active DECLARE
// nflow contributions; no collective identity or expected fan-in enters this
// object.
// Transport of declarations and grants is deliberately outside this class: the
// integration must carry them in band through the simulated Clos.
class RnicCollectiveController {
public:
    static constexpr uint32_t kPartsPerMillion = 1000000;
    static constexpr uint32_t kDefaultMarginPpm = 900000;

    explicit RnicCollectiveController(
        uint64_t bottleneck_wire_capacity_bps,
        uint64_t worst_case_one_way_control_deadline_ps,
        uint32_t margin_ppm = kDefaultMarginPpm);
    RnicCollectiveController(const RnicCollectiveController&) = delete;
    RnicCollectiveController& operator=(
        const RnicCollectiveController&) = delete;
    RnicCollectiveController(RnicCollectiveController&&) = delete;
    RnicCollectiveController& operator=(RnicCollectiveController&&) = delete;

    std::optional<RnicCollectiveGrantWave> beginMembershipWave(
        const RnicCollectiveMembershipDelta& delta,
        uint64_t receiver_observation_time_ps);

    bool contains(uint64_t flow_id) const;

    size_t activeFlowCount() const {
        return _active_nflow_by_flow.size();
    }
    uint32_t effectiveFlowCount() const;
    uint64_t membershipEpoch() const { return _membership_epoch; }
    uint64_t bottleneckWireCapacityBps() const {
        return _bottleneck_wire_capacity_bps;
    }
    uint32_t marginPpm() const { return _margin_ppm; }
    uint64_t controlDeadlinePs() const {
        return _control_deadline_ps;
    }
    bool waveOutstanding() const {
        return _outstanding_wave.has_value();
    }
    std::optional<uint64_t> outstandingEpoch() const {
        if (!_outstanding_wave.has_value()) {
            return std::nullopt;
        }
        return _outstanding_wave->membership_epoch;
    }
    std::optional<uint64_t> outstandingEffectiveTimePs() const {
        if (!_outstanding_wave.has_value()) {
            return std::nullopt;
        }
        return _outstanding_wave->effective_time_ps;
    }

private:
    friend class RnicCollectiveGrantWaveBarrier;

    bool applyMembershipDelta(const RnicCollectiveMembershipDelta& delta);
    RnicCollectiveGrant grantFor(
        uint64_t flow_id,
        uint64_t effective_time_ps,
        RnicCollectiveGrantKind kind) const;
    std::vector<RnicCollectiveGrant> grantsForAll(
        uint64_t effective_time_ps,
        const std::set<uint64_t>& accepted_flow_ids) const;
    RnicCollectiveGrantWave grantWave(
        uint64_t effective_time_ps,
        const std::set<uint64_t>& accepted_flow_ids) const;
    uint64_t currentWireRateBps() const;
    void completeWave(uint64_t membership_epoch, uint64_t now_ps);

    uint64_t _bottleneck_wire_capacity_bps;
    uint32_t _margin_ppm;
    uint64_t _control_deadline_ps;
    uint64_t _membership_epoch = 0;
    std::map<uint64_t, uint32_t> _active_nflow_by_flow;
    std::optional<RnicCollectiveGrantWave> _outstanding_wave;
};

class RnicSenderGrantGate {
public:
    enum class Phase {
        Idle,
        DeclarationInFlight,
        AcceptPendingEffectiveTime,
        Active,
        Retired,
    };

    explicit RnicSenderGrantGate(uint64_t flow_id) : _flow_id(flow_id) {}

    void declarationDispatched();
    bool receiveAccept(
        const RnicCollectiveGrant& grant, uint64_t arrival_time_ps);
    bool receiveGrantUpdate(
        const RnicCollectiveGrant& grant, uint64_t arrival_time_ps);
    // Finalize the sender only after the receiver wave that excludes this
    // flow has committed. The runtime keeps a source-drained flow able to
    // receive intervening UPDATEs until that boundary.
    void receiverRetirementCommitted();

    uint64_t flowId() const { return _flow_id; }
    Phase phase() const { return _phase; }
    bool dataEligible() const { return _phase == Phase::Active; }
    uint64_t currentWireRateBps() const { return _current_wire_rate_bps; }
    uint64_t membershipEpoch() const { return _membership_epoch; }
    size_t pendingGrantCount() const { return _pending_grants.size(); }
    std::optional<uint64_t> nextGrantEffectiveTimePs() const;

private:
    friend class RnicCollectiveGrantWaveBarrier;

    struct PendingGrant {
        RnicCollectiveGrant grant;
        bool is_accept;
    };

    bool receiveGrant(const RnicCollectiveGrant& grant,
                      uint64_t arrival_time_ps,
                      bool is_accept);
    void validateWaveActivation(
        const RnicCollectiveGrant& grant, uint64_t now_ps) const;
    void applyWaveGrant(const RnicCollectiveGrant& grant);
    void validateGrantIdentity(const RnicCollectiveGrant& grant) const;
    static bool grantsEqual(const RnicCollectiveGrant& lhs,
                            const RnicCollectiveGrant& rhs);

    uint64_t _flow_id;
    Phase _phase = Phase::Idle;
    uint64_t _current_wire_rate_bps = 0;
    uint64_t _membership_epoch = 0;
    std::optional<RnicCollectiveGrant> _applied_grant;
    std::optional<uint64_t> _accept_epoch;
    std::map<uint64_t, PendingGrant> _pending_grants;
};

// The only operation that can expose a new sender rate. It preflights every
// expected grant, then applies the whole receiver wave synchronously before
// the controller can admit another membership change. In HTSIM's single-thread
// event loop, no DATA dispatch can observe a partially activated wave.
class RnicCollectiveGrantWaveBarrier {
public:
    static size_t activate(
        const RnicCollectiveGrantWave& wave,
        const std::vector<RnicSenderGrantGate*>& sender_gates,
        RnicCollectiveController& controller,
        uint64_t now_ps);
};

#endif
