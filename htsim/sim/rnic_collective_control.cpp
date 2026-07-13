// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_control.h"

#include <limits>
#include <stdexcept>

RnicCollectiveController::RnicCollectiveController(
        uint64_t bottleneck_capacity_bps,
        uint32_t margin_ppm)
    : _bottleneck_capacity_bps(bottleneck_capacity_bps),
      _margin_ppm(margin_ppm) {
    if (margin_ppm == 0 || margin_ppm > kPartsPerMillion) {
        throw std::invalid_argument("RNIC-CN margin must be in (0, 1]");
    }
    if (bottleneck_capacity_bps
        > std::numeric_limits<uint64_t>::max() / margin_ppm) {
        throw std::invalid_argument("RNIC-CN capacity is too large for fixed-point grant math");
    }
}

bool RnicCollectiveController::declareFlow(uint64_t flow_id) {
    const bool inserted = _active_flow_ids.insert(flow_id).second;
    if (inserted) {
        ++_membership_epoch;
    }
    return inserted;
}

bool RnicCollectiveController::retireFlow(uint64_t flow_id) {
    const bool removed = _active_flow_ids.erase(flow_id) != 0;
    if (removed) {
        ++_membership_epoch;
    }
    return removed;
}

bool RnicCollectiveController::contains(uint64_t flow_id) const {
    return _active_flow_ids.count(flow_id) != 0;
}

RnicCollectiveGrant RnicCollectiveController::grantFor(uint64_t flow_id) const {
    if (!contains(flow_id)) {
        throw std::out_of_range("grant requested for undeclared RNIC-CN flow");
    }
    return {flow_id,
            _membership_epoch,
            static_cast<uint32_t>(_active_flow_ids.size()),
            currentRateBps()};
}

std::vector<RnicCollectiveGrant> RnicCollectiveController::grantsForAll() const {
    std::vector<RnicCollectiveGrant> grants;
    grants.reserve(_active_flow_ids.size());
    for (const uint64_t flow_id : _active_flow_ids) {
        grants.push_back(grantFor(flow_id));
    }
    return grants;
}

uint64_t RnicCollectiveController::currentRateBps() const {
    if (_active_flow_ids.empty()) {
        return 0;
    }
    if (_active_flow_ids.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("RNIC-CN active-flow count exceeds feedback field");
    }
    const uint64_t numerator = _bottleneck_capacity_bps * _margin_ppm;
    const uint64_t denominator = static_cast<uint64_t>(kPartsPerMillion)
                                 * _active_flow_ids.size();
    return numerator / denominator;
}

void RnicSenderGrantGate::declarationDispatched() {
    if (_phase != Phase::Idle) {
        throw std::logic_error("RNIC-CN declaration dispatched in invalid sender phase");
    }
    _phase = Phase::DeclarationInFlight;
}

void RnicSenderGrantGate::accept(const RnicCollectiveGrant& grant) {
    if (_phase != Phase::DeclarationInFlight) {
        throw std::logic_error("RNIC-CN ACCEPT received in invalid sender phase");
    }
    validateGrantIdentity(grant);
    if (grant.n_hat == 0) {
        throw std::invalid_argument("RNIC-CN ACCEPT has an empty membership count");
    }
    _membership_epoch = grant.membership_epoch;
    _current_rate_bps = grant.rate_bps;
    _phase = Phase::Active;
}

bool RnicSenderGrantGate::applyGrantUpdate(const RnicCollectiveGrant& grant) {
    if (_phase != Phase::Active) {
        throw std::logic_error("RNIC-CN grant update received before ACCEPT");
    }
    validateGrantIdentity(grant);
    if (grant.n_hat == 0) {
        throw std::invalid_argument("RNIC-CN grant update has an empty membership count");
    }
    if (grant.membership_epoch < _membership_epoch) {
        return false;
    }
    _membership_epoch = grant.membership_epoch;
    _current_rate_bps = grant.rate_bps;
    return true;
}

void RnicSenderGrantGate::retire() {
    if (_phase == Phase::Retired) {
        return;
    }
    if (_phase == Phase::Idle) {
        throw std::logic_error("RNIC-CN sender retired before declaration");
    }
    _phase = Phase::Retired;
    _current_rate_bps = 0;
}

void RnicSenderGrantGate::validateGrantIdentity(const RnicCollectiveGrant& grant) const {
    if (grant.flow_id != _flow_id) {
        throw std::invalid_argument("RNIC-CN grant delivered to the wrong sender flow");
    }
}
