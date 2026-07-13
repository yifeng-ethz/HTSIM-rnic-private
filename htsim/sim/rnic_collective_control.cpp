// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_control.h"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

RnicCollectiveController::RnicCollectiveController(
        uint64_t bottleneck_wire_capacity_bps,
        uint64_t worst_case_one_way_control_deadline_ps,
        uint32_t margin_ppm)
    : _bottleneck_wire_capacity_bps(bottleneck_wire_capacity_bps),
      _margin_ppm(margin_ppm),
      _control_deadline_ps(worst_case_one_way_control_deadline_ps) {
    if (bottleneck_wire_capacity_bps == 0) {
        throw std::invalid_argument(
            "rnic-cn bottleneck wire capacity must be nonzero");
    }
    if (margin_ppm == 0 || margin_ppm > kPartsPerMillion) {
        throw std::invalid_argument("rnic-cn margin must be in (0, 1]");
    }
    if (bottleneck_wire_capacity_bps
        > std::numeric_limits<uint64_t>::max() / margin_ppm) {
        throw std::invalid_argument(
            "rnic-cn capacity is too large for fixed-point grant math");
    }
    if (_control_deadline_ps == 0) {
        throw std::invalid_argument(
            "rnic-cn homogeneous control deadline must be nonzero");
    }
}

bool RnicCollectiveController::applyMembershipDelta(
        const RnicCollectiveMembershipDelta& delta) {
    const std::set<uint64_t> declarations(
        delta.declared_flow_ids.begin(), delta.declared_flow_ids.end());
    const std::set<uint64_t> retirements(
        delta.retired_flow_ids.begin(), delta.retired_flow_ids.end());
    if (declarations.size() != delta.declared_flow_ids.size()
        || retirements.size() != delta.retired_flow_ids.size()) {
        throw std::invalid_argument(
            "rnic-cn membership delta contains duplicate flow ids");
    }
    for (const uint64_t flow_id : declarations) {
        if (retirements.count(flow_id) != 0) {
            throw std::invalid_argument(
                "rnic-cn membership delta both declares and retires a flow");
        }
    }

    std::set<uint64_t> next_active = _active_flow_ids;
    for (const uint64_t flow_id : retirements) {
        next_active.erase(flow_id);
    }
    next_active.insert(declarations.begin(), declarations.end());
    if (next_active == _active_flow_ids) {
        return false;
    }
    if (next_active.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error(
            "rnic-cn active-flow count exceeds feedback field");
    }
    if (!next_active.empty()) {
        const uint64_t numerator =
            _bottleneck_wire_capacity_bps * _margin_ppm;
        const uint64_t denominator =
            static_cast<uint64_t>(kPartsPerMillion) * next_active.size();
        if (numerator / denominator == 0) {
            throw std::overflow_error(
                "rnic-cn active membership produces a zero wire-rate grant");
        }
    }
    if (_membership_epoch == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("rnic-cn membership epoch overflow");
    }
    _active_flow_ids = std::move(next_active);
    ++_membership_epoch;
    return true;
}

bool RnicCollectiveController::contains(uint64_t flow_id) const {
    return _active_flow_ids.count(flow_id) != 0;
}

RnicCollectiveGrant RnicCollectiveController::grantFor(
        uint64_t flow_id,
        uint64_t effective_time_ps,
        RnicCollectiveGrantKind kind) const {
    if (kind != RnicCollectiveGrantKind::Accept
        && kind != RnicCollectiveGrantKind::Update) {
        throw std::invalid_argument("rnic-cn grant kind must be explicit");
    }
    if (!contains(flow_id)) {
        throw std::out_of_range("grant requested for undeclared rnic-cn flow");
    }
    const uint64_t wire_rate_bps = currentWireRateBps();
    if (wire_rate_bps == 0) {
        throw std::overflow_error(
            "rnic-cn active membership produces a zero wire-rate grant");
    }
    return {flow_id,
            _membership_epoch,
            static_cast<uint32_t>(_active_flow_ids.size()),
            wire_rate_bps,
            effective_time_ps,
            kind};
}

std::vector<RnicCollectiveGrant> RnicCollectiveController::grantsForAll(
        uint64_t effective_time_ps,
        const std::set<uint64_t>& accepted_flow_ids) const {
    for (const uint64_t flow_id : accepted_flow_ids) {
        if (!contains(flow_id)) {
            throw std::invalid_argument(
                "rnic-cn ACCEPT set contains an inactive flow");
        }
    }
    std::vector<RnicCollectiveGrant> grants;
    grants.reserve(_active_flow_ids.size());
    for (const uint64_t flow_id : _active_flow_ids) {
        const RnicCollectiveGrantKind kind =
            accepted_flow_ids.count(flow_id) == 0
                ? RnicCollectiveGrantKind::Update
                : RnicCollectiveGrantKind::Accept;
        grants.push_back(grantFor(flow_id, effective_time_ps, kind));
    }
    return grants;
}

RnicCollectiveGrantWave RnicCollectiveController::grantWave(
        uint64_t effective_time_ps,
        const std::set<uint64_t>& accepted_flow_ids) const {
    if (_active_flow_ids.empty()) {
        if (!accepted_flow_ids.empty()) {
            throw std::invalid_argument(
                "rnic-cn empty wave cannot ACCEPT a sender");
        }
        return {_membership_epoch, 0, 0, effective_time_ps, {}};
    }
    const std::vector<RnicCollectiveGrant> grants =
        grantsForAll(effective_time_ps, accepted_flow_ids);
    return {_membership_epoch,
            grants.front().n_hat,
            grants.front().wire_rate_bps,
            effective_time_ps,
            grants};
}

uint64_t RnicCollectiveController::currentWireRateBps() const {
    if (_active_flow_ids.empty()) {
        return 0;
    }
    if (_active_flow_ids.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error(
            "rnic-cn active-flow count exceeds feedback field");
    }
    const uint64_t numerator = _bottleneck_wire_capacity_bps * _margin_ppm;
    const uint64_t denominator = static_cast<uint64_t>(kPartsPerMillion)
                                 * _active_flow_ids.size();
    return numerator / denominator;
}

std::optional<RnicCollectiveGrantWave>
RnicCollectiveController::beginMembershipWave(
        const RnicCollectiveMembershipDelta& delta,
        uint64_t receiver_observation_time_ps) {
    if (waveOutstanding()) {
        throw std::logic_error(
            "rnic-cn membership waves must be serialized per receiver");
    }

    RnicCollectiveController next_controller(
        _bottleneck_wire_capacity_bps, _control_deadline_ps, _margin_ppm);
    next_controller._active_flow_ids = _active_flow_ids;
    next_controller._membership_epoch = _membership_epoch;
    std::set<uint64_t> accepted_flow_ids;
    for (const uint64_t flow_id : delta.declared_flow_ids) {
        if (!contains(flow_id)) {
            accepted_flow_ids.insert(flow_id);
        }
    }
    if (!next_controller.applyMembershipDelta(delta)) {
        return std::nullopt;
    }
    if (_control_deadline_ps
        > std::numeric_limits<uint64_t>::max()
              - receiver_observation_time_ps) {
        throw std::overflow_error(
            "rnic-cn grant-wave effective timestamp overflow");
    }
    const uint64_t effective_time_ps =
        receiver_observation_time_ps + _control_deadline_ps;
    RnicCollectiveGrantWave wave =
        next_controller.grantWave(effective_time_ps, accepted_flow_ids);

    // Copy the immutable expected wave before committing receiver membership.
    // The barrier later compares the caller's wave against this retained copy,
    // so it cannot complete an epoch with fabricated or partial metadata.
    RnicCollectiveGrantWave retained_wave = wave;
    _active_flow_ids = std::move(next_controller._active_flow_ids);
    _membership_epoch = next_controller._membership_epoch;
    _outstanding_wave = std::move(retained_wave);
    return wave;
}

void RnicCollectiveController::completeWave(
        uint64_t membership_epoch, uint64_t now_ps) {
    if (!waveOutstanding()) {
        throw std::logic_error("rnic-cn has no outstanding grant wave");
    }
    if (membership_epoch != _outstanding_wave->membership_epoch) {
        throw std::invalid_argument(
            "rnic-cn completed the wrong membership epoch");
    }
    if (now_ps != _outstanding_wave->effective_time_ps) {
        throw std::invalid_argument(
            "rnic-cn grant wave must complete at its effective boundary");
    }
    _outstanding_wave.reset();
}

void RnicSenderGrantGate::declarationDispatched() {
    if (_phase != Phase::Idle) {
        throw std::logic_error(
            "rnic-cn declaration dispatched in invalid sender phase");
    }
    _phase = Phase::DeclarationInFlight;
}

bool RnicSenderGrantGate::receiveAccept(
        const RnicCollectiveGrant& grant, uint64_t arrival_time_ps) {
    return receiveGrant(grant, arrival_time_ps, true);
}

bool RnicSenderGrantGate::receiveGrantUpdate(
        const RnicCollectiveGrant& grant, uint64_t arrival_time_ps) {
    return receiveGrant(grant, arrival_time_ps, false);
}

bool RnicSenderGrantGate::receiveGrant(
        const RnicCollectiveGrant& grant,
        uint64_t arrival_time_ps,
        bool is_accept) {
    validateGrantIdentity(grant);
    if (_phase == Phase::Retired) {
        return false;
    }
    if (_phase == Phase::Idle) {
        throw std::logic_error(
            "rnic-cn grant received before declaration dispatch");
    }
    if (grant.membership_epoch == 0 || grant.n_hat == 0) {
        throw std::invalid_argument(
            "rnic-cn grant has an empty epoch or membership count");
    }
    if (grant.wire_rate_bps == 0) {
        throw std::invalid_argument("rnic-cn grant wire rate must be nonzero");
    }
    const bool grant_is_accept =
        grant.kind == RnicCollectiveGrantKind::Accept;
    if (grant.kind != RnicCollectiveGrantKind::Accept
        && grant.kind != RnicCollectiveGrantKind::Update) {
        throw std::invalid_argument("rnic-cn grant kind must be explicit");
    }
    if (grant_is_accept != is_accept) {
        throw std::invalid_argument(
            "rnic-cn grant kind does not match its control packet");
    }
    if (_applied_grant.has_value()
        && grant.membership_epoch <= _applied_grant->membership_epoch) {
        if (grant.membership_epoch < _applied_grant->membership_epoch) {
            return false;
        }
        if (!grantsEqual(grant, *_applied_grant)) {
            throw std::invalid_argument(
                "rnic-cn conflicting grant for an applied membership epoch");
        }
        return false;
    }

    auto same_epoch = _pending_grants.find(grant.membership_epoch);
    if (same_epoch != _pending_grants.end()) {
        if (!grantsEqual(grant, same_epoch->second.grant)) {
            throw std::invalid_argument(
                "rnic-cn conflicting grant for a pending membership epoch");
        }
        return false;
    }

    if (arrival_time_ps > grant.effective_time_ps) {
        throw std::runtime_error(
            "rnic-cn grant missed its homogeneous control deadline");
    }

    const auto next = _pending_grants.lower_bound(grant.membership_epoch);
    if (next != _pending_grants.end()
        && grant.effective_time_ps >= next->second.grant.effective_time_ps) {
        throw std::invalid_argument(
            "rnic-cn grant epochs require increasing effective times");
    }
    if (next != _pending_grants.begin()) {
        const auto previous = std::prev(next);
        if (grant.effective_time_ps
            <= previous->second.grant.effective_time_ps) {
            throw std::invalid_argument(
                "rnic-cn grant epochs require increasing effective times");
        }
    } else if (_applied_grant.has_value()
               && grant.effective_time_ps
                      <= _applied_grant->effective_time_ps) {
        throw std::invalid_argument(
            "rnic-cn grant epochs require increasing effective times");
    }

    if (is_accept) {
        if (_phase == Phase::Active) {
            throw std::logic_error(
                "rnic-cn active sender received a new ACCEPT");
        }
        if (_accept_epoch.has_value()
            && *_accept_epoch != grant.membership_epoch) {
            throw std::invalid_argument(
                "rnic-cn sender received ACCEPT for multiple epochs");
        }
    }
    _pending_grants.emplace(
        grant.membership_epoch, PendingGrant{grant, is_accept});
    if (is_accept) {
        _accept_epoch = grant.membership_epoch;
        _phase = Phase::AcceptPendingEffectiveTime;
    }
    return true;
}

void RnicSenderGrantGate::validateWaveActivation(
        const RnicCollectiveGrant& grant, uint64_t now_ps) const {
    validateGrantIdentity(grant);
    if (grant.effective_time_ps != now_ps) {
        throw std::invalid_argument(
            "rnic-cn grant activated outside its effective boundary");
    }
    const auto pending = _pending_grants.find(grant.membership_epoch);
    if (pending == _pending_grants.end()
        || !grantsEqual(pending->second.grant, grant)) {
        throw std::logic_error(
            "rnic-cn grant wave reached activation with missing feedback");
    }
    if (pending != _pending_grants.begin()) {
        throw std::logic_error(
            "rnic-cn grant waves must activate in membership-epoch order");
    }
    if (pending->second.is_accept
        != (grant.kind == RnicCollectiveGrantKind::Accept)) {
        throw std::logic_error(
            "rnic-cn pending grant kind does not match its wave");
    }
    if (_phase != Phase::Active && !pending->second.is_accept) {
        throw std::logic_error(
            "rnic-cn sender cannot activate before ACCEPT");
    }
}

void RnicSenderGrantGate::applyWaveGrant(
        const RnicCollectiveGrant& grant) {
    auto pending = _pending_grants.find(grant.membership_epoch);
    _membership_epoch = grant.membership_epoch;
    _current_wire_rate_bps = grant.wire_rate_bps;
    _applied_grant = grant;
    if (pending->second.is_accept) {
        _phase = Phase::Active;
    }
    _pending_grants.erase(pending);
}

void RnicSenderGrantGate::receiverRetirementCommitted() {
    if (_phase == Phase::Retired) {
        return;
    }
    if (_phase != Phase::Active) {
        throw std::logic_error(
            "rnic-cn sender retirement requires an active committed grant");
    }
    if (!_pending_grants.empty()) {
        throw std::logic_error(
            "rnic-cn sender retired before its grant wave committed");
    }
    _phase = Phase::Retired;
    _current_wire_rate_bps = 0;
    _pending_grants.clear();
}

std::optional<uint64_t> RnicSenderGrantGate::nextGrantEffectiveTimePs() const {
    if (_pending_grants.empty()) {
        return std::nullopt;
    }
    return _pending_grants.begin()->second.grant.effective_time_ps;
}

void RnicSenderGrantGate::validateGrantIdentity(const RnicCollectiveGrant& grant) const {
    if (grant.flow_id != _flow_id) {
        throw std::invalid_argument(
            "rnic-cn grant delivered to the wrong sender flow");
    }
}

bool RnicSenderGrantGate::grantsEqual(
        const RnicCollectiveGrant& lhs,
        const RnicCollectiveGrant& rhs) {
    return lhs.flow_id == rhs.flow_id
           && lhs.membership_epoch == rhs.membership_epoch
           && lhs.n_hat == rhs.n_hat
           && lhs.wire_rate_bps == rhs.wire_rate_bps
           && lhs.effective_time_ps == rhs.effective_time_ps
           && lhs.kind == rhs.kind;
}

size_t RnicCollectiveGrantWaveBarrier::activate(
        const RnicCollectiveGrantWave& wave,
        const std::vector<RnicSenderGrantGate*>& sender_gates,
        RnicCollectiveController& controller,
        uint64_t now_ps) {
    if (!controller.waveOutstanding()) {
        throw std::logic_error(
            "rnic-cn activation does not match the outstanding receiver wave");
    }
    const RnicCollectiveGrantWave& expected = *controller._outstanding_wave;
    bool wave_matches = wave.membership_epoch == expected.membership_epoch
                        && wave.n_hat == expected.n_hat
                        && wave.wire_rate_bps == expected.wire_rate_bps
                        && wave.effective_time_ps == expected.effective_time_ps
                        && wave.grants.size() == expected.grants.size();
    for (size_t i = 0; wave_matches && i < wave.grants.size(); ++i) {
        wave_matches = RnicSenderGrantGate::grantsEqual(
            wave.grants[i], expected.grants[i]);
    }
    if (!wave_matches) {
        throw std::invalid_argument(
            "rnic-cn activation changed the immutable receiver wave");
    }
    if (now_ps != wave.effective_time_ps) {
        throw std::invalid_argument(
            "rnic-cn grant wave activated outside its effective boundary");
    }
    if (wave.grants.size() != sender_gates.size()
        || wave.grants.size() != wave.n_hat) {
        throw std::invalid_argument(
            "rnic-cn grant wave does not cover its complete membership");
    }
    if (wave.grants.empty()) {
        if (wave.n_hat != 0 || wave.wire_rate_bps != 0) {
            throw std::invalid_argument(
                "rnic-cn empty grant wave has nonempty rate metadata");
        }
    }

    std::set<uint64_t> covered_flow_ids;
    for (size_t i = 0; i < wave.grants.size(); ++i) {
        const RnicCollectiveGrant& grant = wave.grants[i];
        RnicSenderGrantGate* gate = sender_gates[i];
        if (gate == nullptr || gate->flowId() != grant.flow_id
            || grant.membership_epoch != wave.membership_epoch
            || grant.n_hat != wave.n_hat
            || grant.wire_rate_bps != wave.wire_rate_bps
            || grant.effective_time_ps != wave.effective_time_ps
            || (grant.kind == RnicCollectiveGrantKind::Accept)
                   != (gate->phase()
                       == RnicSenderGrantGate::Phase::AcceptPendingEffectiveTime)
            || !covered_flow_ids.insert(grant.flow_id).second) {
            throw std::invalid_argument(
                "rnic-cn grant wave metadata or sender coverage mismatch");
        }
        gate->validateWaveActivation(grant, now_ps);
    }

    // Every operation that can fail has completed. Grant assignment and map
    // erasure below are non-allocating, so DATA eligibility changes as one
    // synchronous commit with no externally observable partial wave.
    for (size_t i = 0; i < wave.grants.size(); ++i) {
        sender_gates[i]->applyWaveGrant(wave.grants[i]);
    }
    controller.completeWave(wave.membership_epoch, now_ps);
    return wave.grants.size();
}
