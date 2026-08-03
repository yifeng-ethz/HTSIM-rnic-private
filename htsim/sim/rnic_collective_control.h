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

// Explicit wire-rate feedback carried by ACCEPT or by the ACK of one marked
// DATA packet. effective_time_ps is the join gate for ACCEPT and the receiver
// generation timestamp for UPDATE.
struct RnicCollectiveGrant {
    std::uint64_t flow_id;
    std::uint64_t membership_epoch;
    std::uint32_t n_hat;
    std::uint64_t wire_rate_bps;
    std::uint64_t effective_time_ps;
    RnicCollectiveGrantKind kind;
    std::uint64_t feedback_deadline_ps;
    std::uint64_t lease_expiry_ps;
    // True for the ACK of one marked DATA packet. False for ACCEPT and for
    // the fail-closed refresh reply to an idempotent re-DECLARE.
    bool marked_data_ack;
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
// newly admitted senders; incumbents obtain this epoch and rate independently
// through later marked-DATA ACKs.
struct RnicCollectiveMembershipUpdate {
    std::uint64_t membership_epoch;
    std::uint32_t n_hat;
    std::uint64_t wire_rate_bps;
    std::vector<std::uint64_t> accepted_flow_ids;
};

// Receiver-side membership and direct explicit-rate calculation for rnic-cn.
// N_hat is the sum of active DECLARE contributions in ppm of a flow; the
// grant is margin * C / N_hat, so fractional declarations release unused
// bottleneck share to the remaining members. The runtime carries every
// declaration and feedback packet in band through the simulated Clos.
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

    RnicCollectiveGrant acceptFor(
        std::uint64_t flow_id,
        std::uint64_t join_not_before_ps,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps) const;

    // Construct the explicit-rate ACK for one marked DATA packet after
    // receiver resequencing. This operation does not mutate membership.
    RnicCollectiveGrant markedFeedbackFor(
        std::uint64_t flow_id,
        std::uint64_t receiver_feedback_time_ps,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps) const;
    RnicCollectiveGrant refreshFeedbackFor(
        std::uint64_t flow_id,
        std::uint64_t receiver_feedback_time_ps,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps) const;

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
        std::uint64_t effective_time_ps,
        RnicCollectiveGrantKind kind,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps,
        bool marked_data_ack) const;

    std::uint64_t _bottleneck_wire_capacity_bps;
    std::uint32_t _margin_ppm;
    std::uint64_t _control_deadline_ps;
    std::uint64_t _membership_epoch{0};
    std::map<std::uint64_t, std::uint32_t> _active_nflow_by_flow;
};

// Sender-side gate for a single rnic-cn flow. A new sender remains closed
// until its ACCEPT reaches join_not_before. Once active, each marked-DATA ACK
// changes or renews its grant independently of every other sender.
class RnicSenderGrantGate {
public:
    enum class Phase {
        Idle,
        DeclarationInFlight,
        AcceptPendingEffectiveTime,
        Active,
        LeaseExpired,
        Retired,
    };

    explicit RnicSenderGrantGate(std::uint64_t flow_id) : _flow_id(flow_id) {}

    void declarationDispatched();
    bool receiveAccept(const RnicCollectiveGrant& grant,
                       std::uint64_t arrival_time_ps);
    bool applyRateFeedback(const RnicCollectiveGrant& grant,
                           std::uint64_t arrival_time_ps);
    bool activatePendingAccept(std::uint64_t now_ps);
    bool expireLease(std::uint64_t membership_epoch,
                     std::uint64_t lease_expiry_ps,
                     std::uint64_t now_ps);
    void receiverRetirementCommitted();

    std::uint64_t flowId() const noexcept { return _flow_id; }
    Phase phase() const noexcept { return _phase; }
    bool dataEligible() const noexcept { return _phase == Phase::Active; }
    std::uint64_t currentWireRateBps() const noexcept {
        return _current_wire_rate_bps;
    }
    std::uint64_t membershipEpoch() const noexcept { return _membership_epoch; }
    std::uint64_t leaseExpiryPs() const noexcept { return _lease_expiry_ps; }
    std::optional<std::uint64_t> pendingAcceptTimePs() const;

private:
    void validateGrantIdentity(const RnicCollectiveGrant& grant) const;
    static bool grantsEqual(const RnicCollectiveGrant& lhs,
                            const RnicCollectiveGrant& rhs) noexcept;
    void applyGrant(const RnicCollectiveGrant& grant);

    std::uint64_t _flow_id;
    Phase _phase{Phase::Idle};
    std::uint64_t _current_wire_rate_bps{0};
    std::uint64_t _membership_epoch{0};
    std::uint64_t _lease_expiry_ps{0};
    std::optional<RnicCollectiveGrant> _applied_grant;
    std::optional<RnicCollectiveGrant> _pending_accept;
};

#endif  // RNIC_COLLECTIVE_CONTROL_H
