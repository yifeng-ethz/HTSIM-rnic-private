// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_control.h"

#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

RnicCollectiveController::RnicCollectiveController(
        std::uint64_t bottleneck_wire_capacity_bps,
        std::uint64_t worst_case_one_way_control_deadline_ps,
        std::uint32_t margin_ppm)
    : _bottleneck_wire_capacity_bps(bottleneck_wire_capacity_bps),
      _margin_ppm(margin_ppm),
      _control_deadline_ps(worst_case_one_way_control_deadline_ps) {
    if (_bottleneck_wire_capacity_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn bottleneck wire capacity must be nonzero");
    }
    if (_margin_ppm == 0 || _margin_ppm > kPartsPerMillion) {
        throw std::invalid_argument("rnic-cn margin must be in (0, 1]");
    }
    if (_bottleneck_wire_capacity_bps >
        std::numeric_limits<std::uint64_t>::max() / _margin_ppm) {
        throw std::invalid_argument(
            "rnic-cn capacity is too large for fixed-point grant math");
    }
    if (_control_deadline_ps == 0) {
        throw std::invalid_argument(
            "rnic-cn one-way control deadline must be nonzero");
    }
}

std::optional<RnicCollectiveMembershipUpdate>
RnicCollectiveController::updateMembership(
        const RnicCollectiveMembershipDelta& delta) {
    std::map<std::uint64_t, std::uint32_t> declarations;
    for (const RnicCollectiveMembershipDeclaration& declaration :
         delta.declarations) {
        if (declaration.nflow_ppm == 0 ||
            declaration.nflow_ppm > kFullFlowPpm) {
            throw std::invalid_argument(
                "rnic-cn membership declaration nflow_ppm must be in "
                "[1, one flow]");
        }
        if (!declarations.emplace(
                 declaration.flow_id, declaration.nflow_ppm).second) {
            throw std::invalid_argument(
                "rnic-cn membership delta contains duplicate flow ids");
        }
    }

    const std::set<std::uint64_t> retirements(
        delta.retired_flow_ids.begin(), delta.retired_flow_ids.end());
    if (retirements.size() != delta.retired_flow_ids.size()) {
        throw std::invalid_argument(
            "rnic-cn membership delta contains duplicate flow ids");
    }
    for (const auto& declaration : declarations) {
        if (retirements.count(declaration.first) != 0) {
            throw std::invalid_argument(
                "rnic-cn membership delta both declares and retires a flow");
        }
    }

    std::map<std::uint64_t, std::uint32_t> next_active =
        _active_nflow_by_flow;
    std::vector<std::uint64_t> accepted_flow_ids;
    accepted_flow_ids.reserve(declarations.size());
    for (const std::uint64_t flow_id : retirements) {
        next_active.erase(flow_id);
    }
    for (const auto& declaration : declarations) {
        const auto existing = next_active.find(declaration.first);
        if (existing != next_active.end()) {
            if (existing->second != declaration.second) {
                throw std::invalid_argument(
                    "rnic-cn repeated DECLARE changed nflow");
            }
            continue;
        }
        next_active.emplace(declaration.first, declaration.second);
        accepted_flow_ids.push_back(declaration.first);
    }
    if (next_active == _active_nflow_by_flow) {
        return std::nullopt;
    }

    std::uint64_t next_n_hat = 0;
    for (const auto& active : next_active) {
        if (active.second >
            std::numeric_limits<std::uint32_t>::max() - next_n_hat) {
            throw std::overflow_error(
                "rnic-cn effective nflow exceeds feedback field");
        }
        next_n_hat += active.second;
    }

    std::uint64_t next_rate = 0;
    if (next_n_hat != 0) {
        const std::uint64_t numerator =
            _bottleneck_wire_capacity_bps * _margin_ppm;
        // next_n_hat is already in ppm of a flow, so no further scaling.
        next_rate = numerator / next_n_hat;
        if (next_rate == 0) {
            throw std::overflow_error(
                "rnic-cn active membership produces a zero wire-rate grant");
        }
    }
    if (_membership_epoch == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("rnic-cn membership epoch overflow");
    }

    _active_nflow_by_flow = std::move(next_active);
    ++_membership_epoch;
    return RnicCollectiveMembershipUpdate{
        _membership_epoch,
        static_cast<std::uint32_t>(next_n_hat),
        next_rate,
        std::move(accepted_flow_ids)};
}

RnicCollectiveGrant RnicCollectiveController::acceptFor(
        std::uint64_t flow_id,
        std::uint64_t join_not_before_ps,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps) const {
    if (feedback_deadline_ps > join_not_before_ps) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT deadline exceeds its join gate");
    }
    if (lease_expiry_ps <= join_not_before_ps ||
        lease_expiry_ps <= feedback_deadline_ps) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT lease must outlive its deadline and join gate");
    }
    return grantFor(flow_id,
                    join_not_before_ps,
                    RnicCollectiveGrantKind::Accept,
                    feedback_deadline_ps,
                    lease_expiry_ps,
                    false);
}

RnicCollectiveGrant RnicCollectiveController::markedFeedbackFor(
        std::uint64_t flow_id,
        std::uint64_t receiver_feedback_time_ps,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps) const {
    if (feedback_deadline_ps < receiver_feedback_time_ps) {
        throw std::invalid_argument(
            "rnic-cn marked feedback deadline precedes receiver generation");
    }
    if (lease_expiry_ps <= feedback_deadline_ps) {
        throw std::invalid_argument(
            "rnic-cn marked feedback lease must outlive its delivery deadline");
    }
    return grantFor(flow_id,
                    receiver_feedback_time_ps,
                    RnicCollectiveGrantKind::Update,
                    feedback_deadline_ps,
                    lease_expiry_ps,
                    true);
}

RnicCollectiveGrant RnicCollectiveController::refreshFeedbackFor(
        std::uint64_t flow_id,
        std::uint64_t receiver_feedback_time_ps,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps) const {
    if (feedback_deadline_ps < receiver_feedback_time_ps) {
        throw std::invalid_argument(
            "rnic-cn refresh feedback deadline precedes receiver generation");
    }
    if (lease_expiry_ps <= feedback_deadline_ps) {
        throw std::invalid_argument(
            "rnic-cn refresh feedback lease must outlive its delivery deadline");
    }
    return grantFor(flow_id,
                    receiver_feedback_time_ps,
                    RnicCollectiveGrantKind::Update,
                    feedback_deadline_ps,
                    lease_expiry_ps,
                    false);
}

bool RnicCollectiveController::contains(std::uint64_t flow_id) const {
    return _active_nflow_by_flow.count(flow_id) != 0;
}

std::uint32_t RnicCollectiveController::effectiveFlowPpm() const {
    std::uint64_t result = 0;
    for (const auto& active : _active_nflow_by_flow) {
        result += active.second;
    }
    if (result > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "rnic-cn effective nflow exceeds feedback field");
    }
    return static_cast<std::uint32_t>(result);
}

std::uint64_t RnicCollectiveController::currentWireRateBps() const {
    const std::uint32_t n_hat_ppm = effectiveFlowPpm();
    if (n_hat_ppm == 0) {
        return 0;
    }
    const std::uint64_t numerator =
        _bottleneck_wire_capacity_bps * _margin_ppm;
    return numerator / n_hat_ppm;
}

RnicCollectiveGrant RnicCollectiveController::grantFor(
        std::uint64_t flow_id,
        std::uint64_t effective_time_ps,
        RnicCollectiveGrantKind kind,
        std::uint64_t feedback_deadline_ps,
        std::uint64_t lease_expiry_ps,
        bool marked_data_ack) const {
    if (kind != RnicCollectiveGrantKind::Accept &&
        kind != RnicCollectiveGrantKind::Update) {
        throw std::invalid_argument("rnic-cn grant kind must be explicit");
    }
    if (!contains(flow_id)) {
        throw std::out_of_range(
            "grant requested for undeclared rnic-cn flow");
    }
    const std::uint64_t wire_rate_bps = currentWireRateBps();
    if (wire_rate_bps == 0) {
        throw std::overflow_error(
            "rnic-cn active membership produces a zero wire-rate grant");
    }
    return {flow_id,
            _membership_epoch,
            effectiveFlowPpm(),
            wire_rate_bps,
            effective_time_ps,
            kind,
            feedback_deadline_ps,
            lease_expiry_ps,
            marked_data_ack};
}

void RnicSenderGrantGate::declarationDispatched() {
    if (_phase != Phase::Idle) {
        throw std::logic_error(
            "rnic-cn declaration dispatched in invalid sender phase");
    }
    _phase = Phase::DeclarationInFlight;
}

bool RnicSenderGrantGate::receiveAccept(
        const RnicCollectiveGrant& grant,
        std::uint64_t arrival_time_ps) {
    validateGrantIdentity(grant);
    if (_phase == Phase::Retired) {
        return false;
    }
    if (grant.kind != RnicCollectiveGrantKind::Accept) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT packet carries the wrong grant kind");
    }
    if (grant.marked_data_ack) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT cannot identify a marked DATA ACK");
    }
    if (grant.membership_epoch == 0 || grant.n_hat == 0 ||
        grant.wire_rate_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT has empty explicit-rate metadata");
    }
    if (grant.feedback_deadline_ps == 0 ||
        grant.feedback_deadline_ps > grant.effective_time_ps ||
        grant.lease_expiry_ps <= grant.feedback_deadline_ps ||
        grant.lease_expiry_ps <= grant.effective_time_ps) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT has an invalid deadline, join gate, or lease");
    }
    if (arrival_time_ps > grant.feedback_deadline_ps) {
        throw std::runtime_error(
            "rnic-cn ACCEPT missed its physical delivery deadline");
    }
    if (_phase == Phase::Idle) {
        throw std::logic_error(
            "rnic-cn ACCEPT arrived before declaration dispatch");
    }
    if (_phase == Phase::Active || _phase == Phase::LeaseExpired) {
        throw std::logic_error(
            "rnic-cn admitted sender received a second ACCEPT");
    }
    if (_pending_accept.has_value()) {
        if (!grantsEqual(*_pending_accept, grant)) {
            throw std::invalid_argument(
                "rnic-cn conflicting ACCEPT for one sender");
        }
        return false;
    }

    _pending_accept = grant;
    _phase = Phase::AcceptPendingEffectiveTime;
    return true;
}

bool RnicSenderGrantGate::applyRateFeedback(
        const RnicCollectiveGrant& grant,
        std::uint64_t arrival_time_ps) {
    validateGrantIdentity(grant);
    if (grant.kind != RnicCollectiveGrantKind::Update) {
        throw std::invalid_argument(
            "rnic-cn rate feedback must carry a grant UPDATE");
    }
    if (_phase == Phase::Idle || _phase == Phase::DeclarationInFlight ||
        _phase == Phase::AcceptPendingEffectiveTime) {
        throw std::logic_error(
            "rnic-cn rate feedback arrived before sender admission");
    }
    if (_phase == Phase::Retired) {
        return false;
    }
    if (grant.membership_epoch == 0 || grant.n_hat == 0 ||
        grant.wire_rate_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn rate feedback has empty explicit-rate metadata");
    }
    if (grant.feedback_deadline_ps < grant.effective_time_ps ||
        grant.lease_expiry_ps <= grant.feedback_deadline_ps) {
        throw std::invalid_argument(
            "rnic-cn rate feedback has an invalid deadline or lease");
    }
    if (arrival_time_ps < grant.effective_time_ps) {
        throw std::runtime_error(
            "rnic-cn rate feedback arrived before receiver generation");
    }
    if (arrival_time_ps > grant.feedback_deadline_ps) {
        throw std::runtime_error(
            "rnic-cn rate feedback missed its physical delivery deadline");
    }
    if (arrival_time_ps >= grant.lease_expiry_ps) {
        throw std::runtime_error(
            "rnic-cn rate feedback arrived without a live grant lease");
    }

    if (_applied_grant.has_value()) {
        const RnicCollectiveGrant& applied = *_applied_grant;
        if (grant.membership_epoch < applied.membership_epoch) {
            return false;
        }
        if (grant.membership_epoch == applied.membership_epoch) {
            if (grant.n_hat != applied.n_hat ||
                grant.wire_rate_bps != applied.wire_rate_bps) {
                throw std::invalid_argument(
                    "rnic-cn rate feedback conflicts within one membership epoch");
            }
            if (grant.effective_time_ps < applied.effective_time_ps) {
                return false;
            }
            if (grant.effective_time_ps == applied.effective_time_ps) {
                if (grant.feedback_deadline_ps != applied.feedback_deadline_ps ||
                    grant.lease_expiry_ps != applied.lease_expiry_ps) {
                    throw std::invalid_argument(
                        "rnic-cn rate feedback conflicts at one generation time");
                }
                return false;
            }
            if (grant.lease_expiry_ps <= applied.lease_expiry_ps) {
                return false;
            }
        } else if (grant.effective_time_ps <= applied.effective_time_ps) {
            throw std::invalid_argument(
                "rnic-cn newer membership epoch has noncausal feedback time");
        }
    }

    applyGrant(grant);
    return true;
}

bool RnicSenderGrantGate::activatePendingAccept(std::uint64_t now_ps) {
    if (_phase != Phase::AcceptPendingEffectiveTime ||
        !_pending_accept.has_value()) {
        return false;
    }
    if (_pending_accept->effective_time_ps != now_ps) {
        throw std::invalid_argument(
            "rnic-cn join gate opened outside its two-window boundary");
    }
    if (_pending_accept->lease_expiry_ps <= now_ps) {
        throw std::runtime_error(
            "rnic-cn pending ACCEPT has no live grant lease");
    }
    const RnicCollectiveGrant grant = *_pending_accept;
    _pending_accept.reset();
    applyGrant(grant);
    return true;
}

bool RnicSenderGrantGate::expireLease(
        std::uint64_t membership_epoch,
        std::uint64_t lease_expiry_ps,
        std::uint64_t now_ps) {
    if (now_ps != lease_expiry_ps) {
        throw std::invalid_argument(
            "rnic-cn grant lease expired outside its published boundary");
    }
    if (_phase != Phase::Active ||
        _membership_epoch != membership_epoch ||
        _lease_expiry_ps != lease_expiry_ps) {
        return false;
    }
    _phase = Phase::LeaseExpired;
    _current_wire_rate_bps = 0;
    return true;
}

void RnicSenderGrantGate::receiverRetirementCommitted() {
    if (_phase == Phase::Retired) {
        return;
    }
    if (_phase != Phase::Active && _phase != Phase::LeaseExpired) {
        throw std::logic_error(
            "rnic-cn sender retirement requires an admitted grant");
    }
    if (_pending_accept.has_value()) {
        throw std::logic_error(
            "rnic-cn sender retired with a pending ACCEPT");
    }
    _phase = Phase::Retired;
    _current_wire_rate_bps = 0;
    _lease_expiry_ps = 0;
}

std::optional<std::uint64_t>
RnicSenderGrantGate::pendingAcceptTimePs() const {
    if (!_pending_accept.has_value()) {
        return std::nullopt;
    }
    return _pending_accept->effective_time_ps;
}

void RnicSenderGrantGate::validateGrantIdentity(
        const RnicCollectiveGrant& grant) const {
    if (grant.flow_id != _flow_id) {
        throw std::invalid_argument(
            "rnic-cn grant delivered to the wrong sender flow");
    }
}

bool RnicSenderGrantGate::grantsEqual(
        const RnicCollectiveGrant& lhs,
        const RnicCollectiveGrant& rhs) noexcept {
    return lhs.flow_id == rhs.flow_id &&
           lhs.membership_epoch == rhs.membership_epoch &&
           lhs.n_hat == rhs.n_hat &&
           lhs.wire_rate_bps == rhs.wire_rate_bps &&
           lhs.effective_time_ps == rhs.effective_time_ps &&
           lhs.kind == rhs.kind &&
           lhs.feedback_deadline_ps == rhs.feedback_deadline_ps &&
           lhs.lease_expiry_ps == rhs.lease_expiry_ps &&
           lhs.marked_data_ack == rhs.marked_data_ack;
}

void RnicSenderGrantGate::applyGrant(
        const RnicCollectiveGrant& grant) {
    _membership_epoch = grant.membership_epoch;
    _current_wire_rate_bps = grant.wire_rate_bps;
    _lease_expiry_ps = grant.lease_expiry_ps;
    _applied_grant = grant;
    _phase = Phase::Active;
}
