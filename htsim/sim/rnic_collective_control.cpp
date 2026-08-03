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

RnicCollectiveRateSnapshot RnicCollectiveController::rateSnapshot() const {
    return {_membership_epoch, effectiveFlowPpm(), currentWireRateBps()};
}

RnicCollectiveGrant RnicCollectiveController::acceptFor(
        std::uint64_t flow_id,
        const RnicCollectiveRateSnapshot& snapshot,
        std::uint64_t governed_boundary_ps) const {
    return grantFor(flow_id, snapshot, governed_boundary_ps,
                    RnicCollectiveGrantKind::Accept);
}

RnicCollectiveGrant RnicCollectiveController::feedbackFor(
        std::uint64_t flow_id,
        const RnicCollectiveRateSnapshot& snapshot,
        std::uint64_t governed_boundary_ps) const {
    return grantFor(flow_id, snapshot, governed_boundary_ps,
                    RnicCollectiveGrantKind::Update);
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
        const RnicCollectiveRateSnapshot& snapshot,
        std::uint64_t governed_boundary_ps,
        RnicCollectiveGrantKind kind) const {
    if (kind != RnicCollectiveGrantKind::Accept &&
        kind != RnicCollectiveGrantKind::Update) {
        throw std::invalid_argument("rnic-cn grant kind must be explicit");
    }
    if (!contains(flow_id)) {
        throw std::out_of_range(
            "grant requested for undeclared rnic-cn flow");
    }
    if (snapshot.membership_epoch == 0 || snapshot.n_hat_ppm == 0 ||
        snapshot.wire_rate_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn feedback requires a nonempty window snapshot");
    }
    if (governed_boundary_ps == 0) {
        throw std::invalid_argument(
            "rnic-cn feedback requires a governed dwnd boundary");
    }
    return {flow_id,
            snapshot.membership_epoch,
            snapshot.n_hat_ppm,
            snapshot.wire_rate_bps,
            governed_boundary_ps,
            kind,
            0,
            0};
}

std::uint64_t RnicSenderGrantGate::scaledByOwnNflow(
        std::uint64_t shared_wire_rate_bps,
        std::uint32_t own_nflow_ppm) {
    // own_nflow_ppm <= kFullFlowPpm, so the scaled rate never exceeds the
    // shared rate; the 128-bit product guards the n_hat_ppm = 1 extreme.
    const unsigned __int128 scaled =
        static_cast<unsigned __int128>(shared_wire_rate_bps) * own_nflow_ppm /
        RnicCollectiveController::kPartsPerMillion;
    if (scaled == 0) {
        throw std::overflow_error(
            "rnic-cn own-fraction scaling produced a zero wire rate");
    }
    return static_cast<std::uint64_t>(scaled);
}

void RnicSenderGrantGate::declarationDispatched(
        std::uint64_t shared_startup_rate_bps,
        std::uint32_t own_nflow_ppm,
        std::uint64_t one_way_control_deadline_ps) {
    if (_phase != Phase::Idle) {
        throw std::logic_error(
            "rnic-cn declaration dispatched in invalid sender phase");
    }
    if (shared_startup_rate_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn declaration requires a positive startup rate");
    }
    if (own_nflow_ppm == 0 ||
        own_nflow_ppm > RnicCollectiveController::kFullFlowPpm) {
        throw std::invalid_argument(
            "rnic-cn declaration nflow_ppm must be in [1, one flow]");
    }
    if (one_way_control_deadline_ps == 0) {
        throw std::invalid_argument(
            "rnic-cn declaration requires a positive one-way deadline");
    }
    _own_nflow_ppm = own_nflow_ppm;
    _one_way_ps = one_way_control_deadline_ps;
    // The startup reservation is this sender's fraction of the supplied
    // shared rate, i.e. the full ledger allocation; window snapshots then
    // re-time it at sender-local dwnd boundaries.
    _current_wire_rate_bps =
        scaledByOwnNflow(shared_startup_rate_bps, own_nflow_ppm);
    _phase = Phase::Active;
}

RnicSenderFeedbackOutcome RnicSenderGrantGate::receiveAccept(
        const RnicCollectiveGrant& grant,
        std::uint64_t arrival_time_ps) {
    if (grant.kind != RnicCollectiveGrantKind::Accept) {
        throw std::invalid_argument(
            "rnic-cn ACCEPT packet carries the wrong grant kind");
    }
    return applyRateFeedback(grant, arrival_time_ps);
}

RnicSenderFeedbackOutcome RnicSenderGrantGate::applyRateFeedback(
        const RnicCollectiveGrant& grant,
        std::uint64_t arrival_time_ps) {
    validateGrantIdentity(grant);
    if (grant.kind != RnicCollectiveGrantKind::Accept &&
        grant.kind != RnicCollectiveGrantKind::Update) {
        throw std::invalid_argument(
            "rnic-cn rate feedback requires an explicit grant kind");
    }
    if (_phase == Phase::Idle) {
        throw std::logic_error(
            "rnic-cn rate feedback arrived before declaration dispatch");
    }
    if (_phase == Phase::Retired) {
        return RnicSenderFeedbackOutcome::Ignored;
    }
    if (grant.membership_epoch == 0 || grant.n_hat == 0 ||
        grant.wire_rate_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn rate feedback has empty explicit-rate metadata");
    }
    if (grant.feedback_deadline_ps != 0 || grant.lease_expiry_ps != 0) {
        throw std::invalid_argument(
            "rnic-cn rate feedback carries nonzero vestigial lease fields");
    }
    if (grant.effective_time_ps < 2 * _one_way_ps ||
        grant.effective_time_ps % _one_way_ps != 0) {
        throw std::invalid_argument(
            "rnic-cn feedback boundary is not dwnd aligned");
    }
    // A snapshot frozen at window k governs boundary (k + 2) * dwnd, so a
    // physically delivered snapshot can never govern a boundary more than
    // two windows past its arrival.
    if (grant.effective_time_ps > arrival_time_ps + 2 * _one_way_ps) {
        throw std::runtime_error(
            "rnic-cn rate feedback carries a noncausal window snapshot");
    }

    const RnicCollectiveGrant* newest =
        _pending_feedback.has_value()
            ? &*_pending_feedback
            : (_applied_feedback.has_value() ? &*_applied_feedback : nullptr);
    if (newest != nullptr) {
        if (grant.effective_time_ps < newest->effective_time_ps) {
            if (grant.membership_epoch > newest->membership_epoch) {
                throw std::invalid_argument(
                    "rnic-cn older window snapshot carries a newer epoch");
            }
            return RnicSenderFeedbackOutcome::Ignored;
        }
        if (grant.effective_time_ps == newest->effective_time_ps) {
            // Same-window dedup: every ACK generated in one receiver window
            // carries the identical frozen snapshot.
            if (grant.membership_epoch != newest->membership_epoch ||
                grant.n_hat != newest->n_hat ||
                grant.wire_rate_bps != newest->wire_rate_bps) {
                throw std::invalid_argument(
                    "rnic-cn one receiver window froze two snapshots");
            }
            return RnicSenderFeedbackOutcome::Ignored;
        }
        if (grant.membership_epoch < newest->membership_epoch) {
            throw std::invalid_argument(
                "rnic-cn newer window snapshot carries an older epoch");
        }
    }

    const std::uint64_t activation_ps =
        grant.effective_time_ps - _one_way_ps;
    if (activation_ps <= arrival_time_ps) {
        // The governed sender-local window already began; a late snapshot
        // applies from delivery rather than pacing on stale state.
        _pending_feedback.reset();
        applyGrant(grant);
        return RnicSenderFeedbackOutcome::AppliedNow;
    }
    _pending_feedback = grant;
    return RnicSenderFeedbackOutcome::Scheduled;
}

bool RnicSenderGrantGate::activateScheduledRate(std::uint64_t now_ps) {
    if (_phase != Phase::Active || !_pending_feedback.has_value()) {
        return false;
    }
    if (_pending_feedback->effective_time_ps - _one_way_ps != now_ps) {
        return false;
    }
    const RnicCollectiveGrant grant = *_pending_feedback;
    _pending_feedback.reset();
    applyGrant(grant);
    return true;
}

void RnicSenderGrantGate::receiverRetirementCommitted() {
    if (_phase == Phase::Retired) {
        return;
    }
    if (_phase != Phase::Active) {
        throw std::logic_error(
            "rnic-cn sender retirement requires a dispatched declaration");
    }
    _phase = Phase::Retired;
    _current_wire_rate_bps = 0;
    _pending_feedback.reset();
}

std::optional<std::uint64_t>
RnicSenderGrantGate::scheduledActivationTimePs() const {
    if (!_pending_feedback.has_value()) {
        return std::nullopt;
    }
    return _pending_feedback->effective_time_ps - _one_way_ps;
}

void RnicSenderGrantGate::validateGrantIdentity(
        const RnicCollectiveGrant& grant) const {
    if (grant.flow_id != _flow_id) {
        throw std::invalid_argument(
            "rnic-cn grant delivered to the wrong sender flow");
    }
}

void RnicSenderGrantGate::applyGrant(
        const RnicCollectiveGrant& grant) {
    _membership_epoch = grant.membership_epoch;
    _current_wire_rate_bps =
        scaledByOwnNflow(grant.wire_rate_bps, _own_nflow_ppm);
    _applied_feedback = grant;
}
