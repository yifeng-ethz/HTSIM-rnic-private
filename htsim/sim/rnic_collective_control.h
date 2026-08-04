// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_CONTROL_H
#define RNIC_COLLECTIVE_CONTROL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

enum class RnicCollectiveGrantKind {
    Invalid,
    Accept,
    Update,
};

// Explicit wire-rate feedback carried by ACCEPT and by the ACK of every
// resequenced-and-released DATA packet. The receiver freezes one
// (membership_epoch, n_hat, wire_rate) snapshot per dwnd window; every
// feedback packet generated inside that window carries the same snapshot.
// effective_time_ps is the receiver window boundary the snapshot governs,
// i.e. (k + 2) * dwnd for a snapshot frozen at window k; the sender converts
// it to local time by subtracting one one-way control deadline.
// wire_rate_bps is the shared per-whole-flow rate margin * C * 1e6 /
// n_hat_ppm; each sender scales it by its own declared nflow fraction.
struct RnicCollectiveGrant {
    std::uint64_t flow_id;
    std::uint64_t membership_epoch;
    std::uint32_t n_hat;
    std::uint64_t wire_rate_bps;
    std::uint64_t effective_time_ps;
    RnicCollectiveGrantKind kind;
    // Vestigial, kept for wire-format stability; leases were removed and
    // both fields are always 0. Removal is tracked in the algorithm book
    // (section 3, D2).
    std::uint64_t feedback_deadline_ps;
    std::uint64_t lease_expiry_ps;
};

struct RnicCollectiveMembershipDeclaration {
    std::uint64_t flow_id;
    // Membership contribution in ppm of one flow, in [1, kFullFlowPpm].
    std::uint32_t nflow_ppm;
};

struct RnicCollectiveMembershipDelta {
    std::vector<RnicCollectiveMembershipDeclaration> declarations;
    std::vector<std::uint64_t> retired_flow_ids;
};

// Result of one receiver membership mutation. accepted_flow_ids contains only
// newly admitted senders; incumbents obtain the changed rate through the
// window-frozen feedback that rides every resequenced ACK.
struct RnicCollectiveMembershipUpdate {
    std::uint64_t membership_epoch;
    std::uint32_t n_hat;
    std::uint64_t wire_rate_bps;
    std::vector<std::uint64_t> accepted_flow_ids;
};

// The per-window frozen feedback triple. The receiver freezes it at each
// dwnd boundary before applying any same-boundary membership change, so all
// feedback generated within one window carries one consistent snapshot.
struct RnicCollectiveRateSnapshot {
    std::uint64_t membership_epoch;
    std::uint32_t n_hat_ppm;
    std::uint64_t wire_rate_bps;
};

// Receiver-side membership and direct explicit-rate calculation for rnic-cn.
// N_hat is the sum of active DECLARE contributions in ppm of a flow; the
// shared grant is margin * C * 1e6 / N_hat_ppm, so fractional declarations
// release unused bottleneck share to the remaining members. The runtime
// carries every declaration and feedback packet in band through the
// simulated Clos.
class RnicCollectiveController {
public:
    static constexpr std::uint32_t kPartsPerMillion = 1000000;
    static constexpr std::uint32_t kDefaultMarginPpm = 900000;
    // One whole flow's membership contribution. Declarations carry nflow in
    // parts-per-million of a flow so a short sender (payload below one
    // control round trip of granted transfer) can reserve proportionally
    // less of the bottleneck; N_hat sums these ppm contributions.
    static constexpr std::uint32_t kFullFlowPpm = 1000000;

    explicit RnicCollectiveController(
        std::uint64_t bottleneck_wire_capacity_bps,
        std::uint64_t worst_case_one_way_control_deadline_ps,
        std::uint32_t margin_ppm = kDefaultMarginPpm);
    RnicCollectiveController(const RnicCollectiveController&) = delete;
    RnicCollectiveController& operator=(const RnicCollectiveController&) = delete;
    RnicCollectiveController(RnicCollectiveController&&) = delete;
    RnicCollectiveController& operator=(RnicCollectiveController&&) = delete;

    // Validate and atomically apply one receiver membership change. A no-op
    // returns nullopt and leaves both membership and epoch unchanged.
    std::optional<RnicCollectiveMembershipUpdate> updateMembership(
        const RnicCollectiveMembershipDelta& delta);

    // NFLOW_UPDATE (book 1.4): mutate one active declaration's magnitude in
    // place and bump the membership epoch. Returns false for unknown
    // membership, which the caller counts and ignores, mirroring stale
    // DECLAREs. A same-value repeat is an idempotent no-op that leaves the
    // epoch unchanged. Membership identity, join and retire semantics are
    // untouched.
    bool updateDeclaration(std::uint64_t flow_id, std::uint32_t nflow_ppm);

    // The live triple the runtime freezes at each dwnd boundary.
    RnicCollectiveRateSnapshot rateSnapshot() const;

    // Feedback packets are built from a frozen window snapshot, never from
    // live state, so every ACK generated inside one window is identical.
    // governed_boundary_ps is the receiver-time boundary (k + 2) * dwnd.
    RnicCollectiveGrant acceptFor(
        std::uint64_t flow_id,
        const RnicCollectiveRateSnapshot& snapshot,
        std::uint64_t governed_boundary_ps) const;
    RnicCollectiveGrant feedbackFor(
        std::uint64_t flow_id,
        const RnicCollectiveRateSnapshot& snapshot,
        std::uint64_t governed_boundary_ps) const;

    bool contains(std::uint64_t flow_id) const;
    std::size_t activeFlowCount() const noexcept {
        return _active_nflow_by_flow.size();
    }
    // Sum of active contributions, in ppm of a flow.
    std::uint32_t effectiveFlowPpm() const;
    std::uint64_t currentWireRateBps() const;
    std::uint64_t membershipEpoch() const noexcept { return _membership_epoch; }
    std::uint64_t bottleneckWireCapacityBps() const noexcept {
        return _bottleneck_wire_capacity_bps;
    }
    std::uint32_t marginPpm() const noexcept { return _margin_ppm; }
    std::uint64_t controlDeadlinePs() const noexcept {
        return _control_deadline_ps;
    }

private:
    RnicCollectiveGrant grantFor(
        std::uint64_t flow_id,
        const RnicCollectiveRateSnapshot& snapshot,
        std::uint64_t governed_boundary_ps,
        RnicCollectiveGrantKind kind) const;

    std::uint64_t _bottleneck_wire_capacity_bps;
    std::uint32_t _margin_ppm;
    std::uint64_t _control_deadline_ps;
    std::uint64_t _membership_epoch{0};
    std::map<std::uint64_t, std::uint32_t> _active_nflow_by_flow;
};

// Sender-visible result of one delivered feedback packet.
enum class RnicSenderFeedbackOutcome {
    // Duplicate window snapshot, stale window, or retired sender.
    Ignored,
    // The governed sender-local boundary had already begun at arrival; the
    // pacing rate changed immediately.
    AppliedNow,
    // The rate change is held until scheduledActivationTimePs(), the
    // sender-local dwnd boundary the snapshot governs.
    Scheduled,
};

// Sender-side gate for a single rnic-cn flow. Dispatching the DECLARE makes
// DATA eligible immediately: the gate opens at the sender's declared
// fraction of the caller-supplied startup capacity, and window-frozen
// feedback then re-times the rate at sender-local dwnd boundaries, never
// mid-window. Every shared rate is scaled by the gate's own declared nflow
// fraction, so concurrent fractional declarers aggregate to
// margin * C * (sum of fractions) / n_hat exactly. The gate reports the
// receiver's exact allocation; the runtime port-normalizes pacing across
// all of a sender's lanes (book 1.4 pacing correction), so an isolated
// small flow absorbs an undersubscribed receiver's whole surplus and the
// only pacing constraint below grant is the sender port.
class RnicSenderGrantGate {
public:
    enum class Phase {
        Idle,
        Active,
        Retired,
    };

    explicit RnicSenderGrantGate(std::uint64_t flow_id) : _flow_id(flow_id) {}

    // Declare-and-go: moves Idle -> Active and sets the startup rate
    // floor(shared_startup_rate_bps * own_nflow_ppm / 1e6) locally. The
    // caller supplies the shared per-whole-flow rate; under the maintainer's
    // full-deterministic cold-start ruling that is the reservation ledger's
    // margin * C * 1e6 / sum(registered nflow), so allocations are full from
    // the first reachable window, with no ramping.
    // one_way_control_deadline_ps is dwnd; the sender-local window clock
    // runs one one-way ahead of the receiver clock.
    void declarationDispatched(std::uint64_t shared_startup_rate_bps,
                               std::uint32_t own_nflow_ppm,
                               std::uint64_t one_way_control_deadline_ps);
    // RTT-rebalancer support (book 1.4): records a raised declaration. The
    // new fraction is adopted when a snapshot from a strictly newer
    // membership epoch is applied, so pacing still changes only at window
    // boundaries to ledger-consistent values.
    void updateOwnNflow(std::uint32_t nflow_ppm);
    RnicSenderFeedbackOutcome receiveAccept(const RnicCollectiveGrant& grant,
                                            std::uint64_t arrival_time_ps);
    RnicSenderFeedbackOutcome applyRateFeedback(
        const RnicCollectiveGrant& grant,
        std::uint64_t arrival_time_ps);
    // Applies the held snapshot at its sender-local boundary. Returns false
    // for a superseded schedule or a retired sender.
    bool activateScheduledRate(std::uint64_t now_ps);
    void receiverRetirementCommitted();

    std::uint64_t flowId() const noexcept { return _flow_id; }
    Phase phase() const noexcept { return _phase; }
    bool dataEligible() const noexcept { return _phase == Phase::Active; }
    // Scaled by this sender's own declared nflow fraction.
    std::uint64_t currentWireRateBps() const noexcept {
        return _current_wire_rate_bps;
    }
    std::uint32_t ownNflowPpm() const noexcept { return _own_nflow_ppm; }
    std::uint64_t membershipEpoch() const noexcept { return _membership_epoch; }
    std::optional<std::uint64_t> scheduledActivationTimePs() const;

private:
    void validateGrantIdentity(const RnicCollectiveGrant& grant) const;
    static std::uint64_t scaledByOwnNflow(std::uint64_t shared_wire_rate_bps,
                                          std::uint32_t own_nflow_ppm);
    void applyGrant(const RnicCollectiveGrant& grant);

    struct PendingOwnNflow {
        std::uint32_t nflow_ppm;
        std::uint64_t epoch_threshold;
    };

    std::uint64_t _flow_id;
    Phase _phase{Phase::Idle};
    std::uint32_t _own_nflow_ppm{0};
    std::uint64_t _one_way_ps{0};
    std::uint64_t _current_wire_rate_bps{0};
    std::uint64_t _membership_epoch{0};
    std::optional<PendingOwnNflow> _pending_own_nflow;
    std::optional<RnicCollectiveGrant> _pending_feedback;
    std::optional<RnicCollectiveGrant> _applied_feedback;
};

#endif  // RNIC_COLLECTIVE_CONTROL_H
