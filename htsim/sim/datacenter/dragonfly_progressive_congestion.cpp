// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dragonfly_progressive_congestion.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace htsim {
namespace {

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right, const char* description) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error(description);
    }
    return left + right;
}

}  // namespace

DragonflyFourBitRemap::DragonflyFourBitRemap(Thresholds thresholds)
    : _thresholds(std::move(thresholds)) {
    if (_thresholds.front() == 0) {
        throw std::invalid_argument("Dragonfly four-bit remap must map zero input to zero");
    }
    if (!std::is_sorted(_thresholds.begin(), _thresholds.end()) ||
        std::adjacent_find(_thresholds.begin(), _thresholds.end()) != _thresholds.end()) {
        throw std::invalid_argument(
            "Dragonfly four-bit remap thresholds must be strictly increasing");
    }
}

DragonflyFourBitRemap DragonflyFourBitRemap::uniform(std::uint64_t units_per_level) {
    if (units_per_level == 0) {
        throw std::invalid_argument("Dragonfly four-bit remap level width must be nonzero");
    }
    Thresholds thresholds{};
    for (std::size_t index = 0; index < thresholds.size(); ++index) {
        const std::uint64_t level = static_cast<std::uint64_t>(index + 1);
        if (units_per_level > std::numeric_limits<std::uint64_t>::max() / level) {
            throw std::overflow_error("Dragonfly four-bit remap threshold overflow");
        }
        thresholds[index] = units_per_level * level;
    }
    return DragonflyFourBitRemap(thresholds);
}

std::uint8_t DragonflyFourBitRemap::map(std::uint64_t value) const noexcept {
    const auto upper = std::upper_bound(_thresholds.begin(), _thresholds.end(), value);
    return static_cast<std::uint8_t>(std::distance(_thresholds.begin(), upper));
}

DragonflyProgressiveCongestion::DragonflyProgressiveCongestion(
    DragonflyProgressiveCongestionConfig config)
    : _config(std::move(config)) {
    if (_config.minimum_downstream_delay_ps == 0) {
        throw std::invalid_argument("Dragonfly downstream advertisement delay must be positive");
    }
    if (_config.maximum_downstream_age_ps == 0) {
        throw std::invalid_argument(
            "Dragonfly downstream advertisement maximum age must be positive");
    }
    assertInvariant();
}

void DragonflyProgressiveCongestion::setNearEndWaitingBytes(std::uint64_t waiting_bytes) noexcept {
    _near_end_waiting_bytes = waiting_bytes;
}

void DragonflyProgressiveCongestion::addNearEndWaitingBytes(std::uint64_t bytes) {
    _near_end_waiting_bytes = checkedAdd(_near_end_waiting_bytes, bytes,
                                         "Dragonfly near-end waiting-byte counter overflow");
}

void DragonflyProgressiveCongestion::removeNearEndWaitingBytes(std::uint64_t bytes) {
    if (bytes > _near_end_waiting_bytes) {
        throw std::underflow_error("Dragonfly near-end waiting-byte counter underflow");
    }
    _near_end_waiting_bytes -= bytes;
}

void DragonflyProgressiveCongestion::messageSent(DragonflyCongestionMessageId message_id,
                                                 std::uint64_t bytes) {
    if (bytes == 0) {
        throw std::invalid_argument("Dragonfly in-flight message must contain bytes");
    }
    if (_in_flight_messages.find(message_id) != _in_flight_messages.end()) {
        throw std::invalid_argument("Dragonfly in-flight message ID is already active");
    }

    const std::uint64_t new_outstanding = checkedAdd(
        _outstanding_sent_bytes, bytes, "Dragonfly outstanding sent-byte counter overflow");
    const std::uint64_t new_in_flight =
        checkedAdd(_in_flight_bytes, bytes, "Dragonfly in-flight byte counter overflow");
    _in_flight_messages.emplace(message_id, bytes);
    _outstanding_sent_bytes = new_outstanding;
    _in_flight_bytes = new_in_flight;
    assertInvariant();
}

std::uint64_t DragonflyProgressiveCongestion::messageArrived(
    DragonflyCongestionMessageId message_id) {
    const auto message = _in_flight_messages.find(message_id);
    if (message == _in_flight_messages.end()) {
        throw std::invalid_argument("Dragonfly arrival names no in-flight message");
    }
    const std::uint64_t bytes = message->second;
    if (bytes > _in_flight_bytes) {
        throw std::logic_error("Dragonfly in-flight byte accounting is inconsistent");
    }
    _in_flight_bytes -= bytes;
    _in_flight_messages.erase(message);
    assertInvariant();
    return bytes;
}

void DragonflyProgressiveCongestion::creditReturned(std::uint64_t bytes) {
    if (bytes == 0) {
        throw std::invalid_argument("Dragonfly returned credit must cover at least one byte");
    }
    if (bytes > farEndResidentBytes()) {
        throw std::underflow_error("Dragonfly credit cannot acknowledge bytes still in flight");
    }
    _outstanding_sent_bytes -= bytes;
    assertInvariant();
}

std::uint64_t DragonflyProgressiveCongestion::farEndResidentBytes() const {
    assertInvariant();
    return _outstanding_sent_bytes - _in_flight_bytes;
}

DragonflyDownstreamAdvertisement DragonflyProgressiveCongestion::makeDownstreamAdvertisement(
    std::uint64_t sequence,
    std::uint64_t observation_time_ps,
    std::uint64_t waiting_bytes) const {
    return {sequence, observation_time_ps, minimumArrivalTime(observation_time_ps), waiting_bytes};
}

DragonflyAdvertisementDisposition DragonflyProgressiveCongestion::consumeDownstreamAdvertisement(
    const DragonflyDownstreamAdvertisement& advertisement,
    std::uint64_t current_time_ps) {
    validateAdvertisement(advertisement);
    if (current_time_ps < advertisement.observation_time_ps) {
        throw std::invalid_argument(
            "Dragonfly downstream advertisement cannot exist before its observation");
    }
    advanceTime(current_time_ps);
    if (current_time_ps < advertisement.earliest_arrival_time_ps) {
        return DragonflyAdvertisementDisposition::NotYetPhysicallyArrived;
    }
    if (_downstream_advertisement &&
        advertisement.sequence <= _downstream_advertisement->sequence) {
        return DragonflyAdvertisementDisposition::StaleSequence;
    }
    if (_downstream_advertisement &&
        advertisement.observation_time_ps < _downstream_advertisement->observation_time_ps) {
        return DragonflyAdvertisementDisposition::StaleObservation;
    }

    _downstream_advertisement = advertisement;
    _downstream_consumption_time_ps = current_time_ps;
    assertInvariant();
    return DragonflyAdvertisementDisposition::Accepted;
}

DragonflyCongestionComponents DragonflyProgressiveCongestion::components(
    std::uint64_t current_time_ps) const {
    advanceTime(current_time_ps);
    assertInvariant();
    return {
        _config.near_end_remap.map(_near_end_waiting_bytes),
        _config.far_end_remap.map(_outstanding_sent_bytes - _in_flight_bytes),
        _config.downstream_remap.map(downstreamIsFresh(current_time_ps) ? downstreamWaitingBytes()
                                                                        : 0),
    };
}

std::uint64_t DragonflyProgressiveCongestion::minimumArrivalTime(
    std::uint64_t observation_time_ps) const {
    return checkedAdd(observation_time_ps, _config.minimum_downstream_delay_ps,
                      "Dragonfly downstream advertisement arrival-time overflow");
}

void DragonflyProgressiveCongestion::validateAdvertisement(
    const DragonflyDownstreamAdvertisement& advertisement) const {
    const std::uint64_t minimum_arrival = minimumArrivalTime(advertisement.observation_time_ps);
    if (advertisement.earliest_arrival_time_ps < minimum_arrival) {
        throw std::invalid_argument("Dragonfly downstream advertisement violates physical delay");
    }
}

void DragonflyProgressiveCongestion::advanceTime(std::uint64_t current_time_ps) const {
    if (_latest_time_ps && current_time_ps < *_latest_time_ps) {
        throw std::invalid_argument("Dragonfly congestion model time cannot move backwards");
    }
    _latest_time_ps = current_time_ps;
}

bool DragonflyProgressiveCongestion::downstreamIsFresh(std::uint64_t current_time_ps) const {
    if (!_downstream_advertisement) {
        return false;
    }
    if (current_time_ps < _downstream_advertisement->observation_time_ps) {
        throw std::logic_error("Dragonfly downstream observation lies in the future");
    }
    return current_time_ps - _downstream_advertisement->observation_time_ps <=
           _config.maximum_downstream_age_ps;
}

void DragonflyProgressiveCongestion::assertInvariant() const {
    if (_in_flight_bytes > _outstanding_sent_bytes) {
        throw std::logic_error("Dragonfly in-flight bytes exceed outstanding sent bytes");
    }
    if (_downstream_advertisement.has_value() != _downstream_consumption_time_ps.has_value()) {
        throw std::logic_error("Dragonfly downstream advertisement state is incomplete");
    }
    if (_downstream_advertisement &&
        *_downstream_consumption_time_ps < _downstream_advertisement->earliest_arrival_time_ps) {
        throw std::logic_error("Dragonfly downstream advertisement was consumed before arrival");
    }
}

}  // namespace htsim
