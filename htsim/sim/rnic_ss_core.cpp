// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_ss_core.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t kSackWordBits = 64;
constexpr std::uint32_t kSackWindowBits = 128;

bool sackBit(const std::array<std::uint64_t, 2>& bitmap,
             std::uint32_t offset) noexcept {
    return offset < kSackWindowBits
        && (bitmap[offset / kSackWordBits]
            & (UINT64_C(1) << (offset % kSackWordBits))) != 0;
}

void setSackBit(std::array<std::uint64_t, 2>& bitmap,
                std::uint32_t offset) noexcept {
    bitmap[offset / kSackWordBits]
        |= UINT64_C(1) << (offset % kSackWordBits);
}

void shiftSackBitmapRightOne(
        std::array<std::uint64_t, 2>& bitmap) noexcept {
    bitmap[0] = (bitmap[0] >> 1) | (bitmap[1] << 63);
    bitmap[1] >>= 1;
}

void validatePair(const RnicSsEndpointPair& pair) {
    if (pair.source == pair.destination) {
        throw std::invalid_argument(
            "Slingshot-like endpoint pair must be directed between two nodes");
    }
}

bool isReverseWireDirection(
        const RnicSsWirePacketMetadata& packet,
        const RnicSsEndpointPair& forward_pair) {
    return packet.wire_source == forward_pair.destination
        && packet.wire_destination == forward_pair.source;
}

bool isForwardWireDirection(
        const RnicSsWirePacketMetadata& packet,
        const RnicSsEndpointPair& forward_pair) {
    return packet.wire_source == forward_pair.source
        && packet.wire_destination == forward_pair.destination;
}

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool withinHysteresis(
        std::uint64_t current,
        std::uint64_t best,
        std::uint64_t hysteresis) noexcept {
    return current <= best || current - best <= hysteresis;
}

}  // namespace

void RnicSsPacketValidationConfig::validate() const {
    if (minimum_control_wire_bytes == 0) {
        throw std::invalid_argument(
            "Slingshot-like control packet minimum must be nonzero");
    }
    if (maximum_wire_bytes < minimum_control_wire_bytes) {
        throw std::invalid_argument(
            "Slingshot-like maximum wire extent is below control minimum");
    }
    if (path_count != 8) {
        throw std::invalid_argument(
            "Slingshot-like Clos core requires exactly eight candidate paths");
    }
    if (maximum_granted_wire_bytes_total == 0) {
        throw std::invalid_argument(
            "Slingshot-like maximum cumulative credit must be nonzero");
    }
}

void validateRnicSsWirePacket(
        const RnicSsWirePacketMetadata& packet,
        const RnicSsPacketValidationConfig& config) {
    config.validate();
    if (packet.wire_source == packet.wire_destination) {
        throw std::invalid_argument(
            "Slingshot-like physical packet cannot loop to its source");
    }
    if (packet.wire_bytes == 0
        || packet.wire_bytes > config.maximum_wire_bytes) {
        throw std::invalid_argument(
            "Slingshot-like packet has an invalid physical wire extent");
    }

    const auto requireControlExtent = [&]() {
        if (packet.wire_bytes < config.minimum_control_wire_bytes) {
            throw std::invalid_argument(
                "Slingshot-like control packet is below its wire minimum");
        }
    };

    switch (packet.kind) {
    case RnicSsPacketKind::DATA: {
        const auto* data = std::get_if<RnicSsDataMetadata>(&packet.payload);
        if (data == nullptr) {
            throw std::invalid_argument(
                "Slingshot-like DATA kind has mismatched metadata");
        }
        validatePair(data->pair);
        if (!isForwardWireDirection(packet, data->pair)) {
            throw std::invalid_argument(
                "Slingshot-like DATA must travel in its endpoint-pair direction");
        }
        if (data->sequence == std::numeric_limits<RnicSsSequence>::max()) {
            throw std::invalid_argument(
                "Slingshot-like DATA reserves the maximum sequence value");
        }
        if (data->payload_bytes == 0
            || data->payload_bytes > packet.wire_bytes) {
            throw std::invalid_argument(
                "Slingshot-like DATA has an invalid payload extent");
        }
        if (data->path_id >= config.path_count) {
            throw std::invalid_argument(
                "Slingshot-like DATA path is outside the physical path set");
        }
        return;
    }
    case RnicSsPacketKind::ACK_SACK: {
        requireControlExtent();
        const auto* ack = std::get_if<RnicSsAckSackMetadata>(&packet.payload);
        if (ack == nullptr) {
            throw std::invalid_argument(
                "Slingshot-like ACK_SACK kind has mismatched metadata");
        }
        validatePair(ack->forward_pair);
        if (!isReverseWireDirection(packet, ack->forward_pair)) {
            throw std::invalid_argument(
                "Slingshot-like ACK_SACK must use the reverse physical path");
        }
        if (sackBit(ack->sack_bitmap, 0)) {
            throw std::invalid_argument(
                "Slingshot-like normalized SACK bitmap must have bit zero clear");
        }
        if (ack->returned_load_sample.has_value()
            && ack->returned_load_sample->path_id >= config.path_count) {
            throw std::invalid_argument(
                "Slingshot-like ACK/SACK telemetry path is unavailable");
        }
        return;
    }
    case RnicSsPacketKind::TELEMETRY: {
        requireControlExtent();
        const auto* telemetry =
            std::get_if<RnicSsTelemetryMetadata>(&packet.payload);
        if (telemetry == nullptr) {
            throw std::invalid_argument(
                "Slingshot-like TELEMETRY kind has mismatched metadata");
        }
        if (telemetry->sample.path_id >= config.path_count) {
            throw std::invalid_argument(
                "Slingshot-like TELEMETRY path is outside the physical path set");
        }
        return;
    }
    case RnicSsPacketKind::BP_ENABLE:
    case RnicSsPacketKind::BP_DISABLE: {
        requireControlExtent();
        const auto* backpressure =
            std::get_if<RnicSsBackpressureMetadata>(&packet.payload);
        if (backpressure == nullptr) {
            throw std::invalid_argument(
                "Slingshot-like backpressure kind has mismatched metadata");
        }
        validatePair(backpressure->forward_pair);
        if (!isReverseWireDirection(packet, backpressure->forward_pair)) {
            throw std::invalid_argument(
                "Slingshot-like backpressure must use the reverse physical path");
        }
        if (backpressure->control_epoch == 0) {
            throw std::invalid_argument(
                "Slingshot-like backpressure epoch must be nonzero");
        }
        if (backpressure->granted_wire_bytes_total
            > config.maximum_granted_wire_bytes_total) {
            throw std::invalid_argument(
                "Slingshot-like backpressure grant exceeds configured domain");
        }
        if (packet.kind == RnicSsPacketKind::BP_DISABLE
            && backpressure->granted_wire_bytes_total != 0) {
            throw std::invalid_argument(
                "Slingshot-like BP_DISABLE cannot carry implicit credit");
        }
        return;
    }
    case RnicSsPacketKind::CREDIT: {
        requireControlExtent();
        const auto* credit = std::get_if<RnicSsCreditMetadata>(&packet.payload);
        if (credit == nullptr) {
            throw std::invalid_argument(
                "Slingshot-like CREDIT kind has mismatched metadata");
        }
        validatePair(credit->forward_pair);
        if (!isReverseWireDirection(packet, credit->forward_pair)) {
            throw std::invalid_argument(
                "Slingshot-like CREDIT must use the reverse physical path");
        }
        if (credit->control_epoch == 0
            || credit->granted_wire_bytes_total == 0) {
            throw std::invalid_argument(
                "Slingshot-like CREDIT requires a nonzero epoch and grant");
        }
        if (credit->granted_wire_bytes_total
            > config.maximum_granted_wire_bytes_total) {
            throw std::invalid_argument(
                "Slingshot-like CREDIT grant exceeds configured domain");
        }
        return;
    }
    }
    throw std::logic_error("unknown Slingshot-like physical packet kind");
}

RnicSsSackScoreboard::RnicSsSackScoreboard(
        RnicSsSequence initial_next_expected)
    : _next_expected_sequence(initial_next_expected) {
    if (initial_next_expected == std::numeric_limits<RnicSsSequence>::max()) {
        throw std::invalid_argument(
            "Slingshot-like SACK base reserves the maximum sequence value");
    }
}

RnicSsReceiveResult RnicSsSackScoreboard::observe(
        RnicSsSequence sequence) {
    if (sequence == std::numeric_limits<RnicSsSequence>::max()) {
        throw std::invalid_argument(
            "Slingshot-like DATA reserves the maximum sequence value");
    }
    if (sequence < _next_expected_sequence) {
        return {RnicSsReceiveDisposition::DUPLICATE, 0};
    }
    const std::uint64_t offset = sequence - _next_expected_sequence;
    if (offset >= kSackWindowBits) {
        return {RnicSsReceiveDisposition::OUTSIDE_SACK_WINDOW, 0};
    }
    const std::uint32_t bitmap_offset = static_cast<std::uint32_t>(offset);
    if (sackBit(_bitmap, bitmap_offset)) {
        return {RnicSsReceiveDisposition::DUPLICATE, 0};
    }

    const bool in_order = offset == 0;
    setSackBit(_bitmap, bitmap_offset);
    std::uint32_t advance = 0;
    while (sackBit(_bitmap, 0)) {
        shiftSackBitmapRightOne(_bitmap);
        ++_next_expected_sequence;
        ++advance;
    }
    return {in_order ? RnicSsReceiveDisposition::NEW_IN_ORDER
                     : RnicSsReceiveDisposition::NEW_OUT_OF_ORDER,
            advance};
}

RnicSsAckSackMetadata RnicSsSackScoreboard::snapshot(
        RnicSsEndpointPair pair) const noexcept {
    return {pair, _next_expected_sequence, _bitmap};
}

void RnicSsSelectiveRepeatConfig::validate() const {
    if (window_packets == 0 || window_packets > kSackWindowBits) {
        throw std::invalid_argument(
            "Slingshot-like selective-repeat window must be in [1,128]");
    }
    if (retransmission_timeout_ps == 0) {
        throw std::invalid_argument(
            "Slingshot-like selective-repeat RTO must be nonzero");
    }
    if (maximum_retransmissions == 0) {
        throw std::invalid_argument(
            "Slingshot-like selective-repeat requires a retry budget");
    }
}

RnicSsSelectiveRepeatLedger::RnicSsSelectiveRepeatLedger(
        RnicSsEndpointPair pair,
        RnicSsSelectiveRepeatConfig config,
        RnicSsSequence initial_sequence)
    : _pair(pair),
      _config(config),
      _initial_sequence(initial_sequence),
      _next_sequence(initial_sequence) {
    validatePair(pair);
    _config.validate();
    if (initial_sequence == std::numeric_limits<RnicSsSequence>::max()) {
        throw std::invalid_argument(
            "Slingshot-like sender reserves the maximum sequence value");
    }
}

bool RnicSsSelectiveRepeatLedger::canSendNewPacket() const noexcept {
    return _outstanding.size() < _config.window_packets
        && _next_sequence != std::numeric_limits<RnicSsSequence>::max();
}

RnicSsSequence RnicSsSelectiveRepeatLedger::recordNewTransmission(
        std::uint32_t wire_bytes,
        RnicSsTimePs now_ps) {
    if (wire_bytes == 0) {
        throw std::invalid_argument(
            "Slingshot-like sender cannot record a zero-byte DATA packet");
    }
    if (!canSendNewPacket()) {
        throw std::logic_error(
            "Slingshot-like selective-repeat send window is full");
    }
    const RnicSsSequence sequence = _next_sequence++;
    const bool inserted = _outstanding.emplace(
        sequence,
        RnicSsOutstandingPacket{sequence, wire_bytes, now_ps, now_ps, 1})
                              .second;
    if (!inserted) {
        throw std::logic_error(
            "Slingshot-like sender sequence already exists in ledger");
    }
    return sequence;
}

RnicSsAckResult RnicSsSelectiveRepeatLedger::applyAck(
        const RnicSsAckSackMetadata& ack) {
    if (ack.forward_pair != _pair) {
        throw std::invalid_argument(
            "Slingshot-like ACK/SACK belongs to another endpoint pair");
    }
    if (sackBit(ack.sack_bitmap, 0)) {
        throw std::invalid_argument(
            "Slingshot-like ACK/SACK bitmap is not normalized");
    }
    if (ack.next_expected_sequence < _initial_sequence) {
        throw std::invalid_argument(
            "Slingshot-like ACK/SACK precedes this sender sequence space");
    }
    if (ack.next_expected_sequence > _next_sequence) {
        throw std::invalid_argument(
            "Slingshot-like cumulative ACK exceeds sent sequence space");
    }

    // Set bits beyond the sender's allocated sequence space indicate corrupt
    // or misrouted control metadata; accepting them could hide integration bugs.
    for (std::uint32_t bit = 0; bit < kSackWindowBits; ++bit) {
        if (!sackBit(ack.sack_bitmap, bit)) {
            continue;
        }
        if (ack.next_expected_sequence
            > std::numeric_limits<RnicSsSequence>::max() - bit) {
            throw std::invalid_argument(
                "Slingshot-like SACK sequence arithmetic overflows");
        }
        if (ack.next_expected_sequence + bit >= _next_sequence) {
            throw std::invalid_argument(
                "Slingshot-like SACK acknowledges unsent DATA");
        }
    }

    RnicSsAckResult result;
    std::optional<std::uint32_t> highest_selective_ack;
    for (std::uint32_t bit = kSackWindowBits; bit > 0; --bit) {
        const std::uint32_t candidate = bit - 1;
        if (sackBit(ack.sack_bitmap, candidate)) {
            highest_selective_ack = candidate;
            break;
        }
    }

    for (auto it = _outstanding.begin(); it != _outstanding.end();) {
        bool acknowledged = it->first < ack.next_expected_sequence;
        std::optional<std::uint64_t> selective_offset;
        if (!acknowledged && it->first >= ack.next_expected_sequence) {
            const std::uint64_t offset =
                it->first - ack.next_expected_sequence;
            selective_offset = offset;
            acknowledged = offset < kSackWindowBits
                && sackBit(ack.sack_bitmap,
                           static_cast<std::uint32_t>(offset));
        }
        if (acknowledged) {
            result.newly_acked.push_back(it->second);
            it = _outstanding.erase(it);
        } else {
            if (highest_selective_ack.has_value()
                && selective_offset.has_value()
                && *selective_offset < *highest_selective_ack) {
                result.reported_holes.push_back(it->second);
            }
            ++it;
        }
    }
    return result;
}

std::vector<RnicSsOutstandingPacket>
RnicSsSelectiveRepeatLedger::retransmissionCandidates(
        RnicSsTimePs now_ps) const {
    std::vector<RnicSsOutstandingPacket> result;
    for (const auto& [sequence, record] : _outstanding) {
        (void)record;
        const std::optional<RnicSsOutstandingPacket> candidate =
            retransmissionCandidate(sequence, now_ps);
        if (candidate.has_value()) {
            result.push_back(*candidate);
        }
    }
    return result;
}

std::optional<RnicSsOutstandingPacket>
RnicSsSelectiveRepeatLedger::retransmissionCandidate(
        RnicSsSequence sequence,
        RnicSsTimePs now_ps) const {
    const auto it = _outstanding.find(sequence);
    if (it == _outstanding.end()) {
        return std::nullopt;
    }
    const RnicSsOutstandingPacket& record = it->second;
    if (now_ps < record.last_sent_at_ps) {
        throw std::invalid_argument(
            "Slingshot-like retransmission time precedes DATA send");
    }
    if (now_ps - record.last_sent_at_ps
        < _config.retransmission_timeout_ps) {
        return std::nullopt;
    }
    const std::uint32_t retransmissions = record.transmission_count - 1;
    if (retransmissions >= _config.maximum_retransmissions) {
        throw std::runtime_error(
            "Slingshot-like selective-repeat retry budget exhausted: "
            "source=" + std::to_string(_pair.source)
            + " destination=" + std::to_string(_pair.destination)
            + " sequence=" + std::to_string(sequence)
            + " retransmissions=" + std::to_string(retransmissions));
    }
    return record;
}

void RnicSsSelectiveRepeatLedger::recordRetransmission(
        RnicSsSequence sequence,
        RnicSsTimePs now_ps,
        RnicSsRetransmissionReason reason) {
    auto it = _outstanding.find(sequence);
    if (it == _outstanding.end()) {
        throw std::out_of_range(
            "Slingshot-like retransmission sequence is not outstanding");
    }
    RnicSsOutstandingPacket& record = it->second;
    if (now_ps < record.last_sent_at_ps) {
        throw std::logic_error(
            "Slingshot-like retransmission precedes its previous send");
    }
    if (reason == RnicSsRetransmissionReason::RTO
        && now_ps - record.last_sent_at_ps
               < _config.retransmission_timeout_ps) {
        throw std::logic_error(
            "Slingshot-like retransmission attempted before its RTO");
    }
    if (record.transmission_count - 1
        >= _config.maximum_retransmissions) {
        throw std::logic_error(
            "Slingshot-like retransmission budget exhausted");
    }
    record.last_sent_at_ps = now_ps;
    ++record.transmission_count;
}

void RnicSsCreditConfig::validate() const {
    if (maximum_credit_ahead_wire_bytes == 0) {
        throw std::invalid_argument(
            "Slingshot-like credit-ahead bound must be nonzero");
    }
}

RnicSsPairCreditState::RnicSsPairCreditState(
        RnicSsEndpointPair pair,
        RnicSsCreditConfig config)
    : _pair(pair), _config(config) {
    ::validatePair(pair);
    _config.validate();
}

void RnicSsPairCreditState::validatePair(
        const RnicSsEndpointPair& pair) const {
    if (pair != _pair) {
        throw std::invalid_argument(
            "Slingshot-like control packet belongs to another endpoint pair");
    }
}

void RnicSsPairCreditState::validateCreditAhead(
        std::uint64_t granted_total) const {
    if (granted_total < _consumed_total
        || granted_total - _consumed_total
               > _config.maximum_credit_ahead_wire_bytes) {
        throw std::invalid_argument(
            "Slingshot-like cumulative credit exceeds its physical burst bound");
    }
}

RnicSsControlApplyResult RnicSsPairCreditState::applyEnable(
        const RnicSsBackpressureMetadata& enable) {
    validatePair(enable.forward_pair);
    if (enable.control_epoch == 0) {
        throw std::invalid_argument(
            "Slingshot-like BP_ENABLE epoch must be nonzero");
    }
    if (enable.control_epoch <= _control_epoch) {
        return RnicSsControlApplyResult::DUPLICATE_OR_STALE;
    }
    if (enable.granted_wire_bytes_total
        > _config.maximum_credit_ahead_wire_bytes) {
        throw std::invalid_argument(
            "Slingshot-like BP_ENABLE initial credit exceeds burst bound");
    }
    _enabled = true;
    _control_epoch = enable.control_epoch;
    _granted_total = enable.granted_wire_bytes_total;
    _consumed_total = 0;
    return RnicSsControlApplyResult::APPLIED;
}

RnicSsControlApplyResult RnicSsPairCreditState::applyDisable(
        const RnicSsBackpressureMetadata& disable) {
    validatePair(disable.forward_pair);
    if (disable.control_epoch == 0) {
        throw std::invalid_argument(
            "Slingshot-like BP_DISABLE epoch must be nonzero");
    }
    if (disable.granted_wire_bytes_total != 0) {
        throw std::invalid_argument(
            "Slingshot-like BP_DISABLE cannot carry implicit credit");
    }
    if (disable.control_epoch <= _control_epoch) {
        return RnicSsControlApplyResult::DUPLICATE_OR_STALE;
    }
    _enabled = false;
    _control_epoch = disable.control_epoch;
    _granted_total = 0;
    _consumed_total = 0;
    return RnicSsControlApplyResult::APPLIED;
}

RnicSsControlApplyResult RnicSsPairCreditState::applyCredit(
        const RnicSsCreditMetadata& credit) {
    validatePair(credit.forward_pair);
    if (credit.control_epoch == 0
        || credit.granted_wire_bytes_total == 0) {
        throw std::invalid_argument(
            "Slingshot-like CREDIT requires a nonzero epoch and grant");
    }
    if (credit.control_epoch != _control_epoch || !_enabled) {
        return RnicSsControlApplyResult::WRONG_EPOCH;
    }
    if (credit.granted_wire_bytes_total <= _granted_total) {
        return RnicSsControlApplyResult::DUPLICATE_OR_STALE;
    }
    validateCreditAhead(credit.granted_wire_bytes_total);
    _granted_total = credit.granted_wire_bytes_total;
    return RnicSsControlApplyResult::APPLIED;
}

bool RnicSsPairCreditState::canSend(std::uint32_t wire_bytes) const noexcept {
    if (wire_bytes == 0) {
        return false;
    }
    if (!_enabled) {
        return true;
    }
    return _granted_total >= _consumed_total
        && wire_bytes <= _granted_total - _consumed_total;
}

void RnicSsPairCreditState::consumeForData(std::uint32_t wire_bytes) {
    if (wire_bytes == 0) {
        throw std::invalid_argument(
            "Slingshot-like DATA cannot consume zero wire credit");
    }
    if (!_enabled) {
        return;
    }
    if (!canSend(wire_bytes)) {
        throw std::logic_error(
            "Slingshot-like DATA exceeds available contributor credit");
    }
    _consumed_total += wire_bytes;
}

RnicSsPairCreditSnapshot RnicSsPairCreditState::snapshot() const noexcept {
    return {_enabled, _control_epoch, _granted_total, _consumed_total};
}

RnicSsPairCreditTable::RnicSsPairCreditTable(RnicSsCreditConfig config)
    : _config(config) {
    _config.validate();
}

RnicSsPairCreditState& RnicSsPairCreditTable::stateFor(
        RnicSsEndpointPair pair) {
    ::validatePair(pair);
    auto [it, inserted] = _states.try_emplace(pair, pair, _config);
    (void)inserted;
    return it->second;
}

const RnicSsPairCreditState* RnicSsPairCreditTable::find(
        RnicSsEndpointPair pair) const noexcept {
    auto it = _states.find(pair);
    return it == _states.end() ? nullptr : &it->second;
}

void RnicSsPathSelectionConfig::validate() const {
    if (path_count != 8 || candidate_count != 4) {
        throw std::invalid_argument(
            "Slingshot-like path selection requires four of eight paths");
    }
    if (maximum_sample_age_ps == 0) {
        throw std::invalid_argument(
            "Slingshot-like load-sample lifetime must be nonzero");
    }
}

RnicSsHystereticPathSelector::RnicSsHystereticPathSelector(
        RnicSsPathSelectionConfig config)
    : _config(config) {
    _config.validate();
}

std::array<std::uint8_t, 4>
RnicSsHystereticPathSelector::sampleFourOfEight(
        RnicSsEndpointPair pair,
        std::uint64_t selection_key) noexcept {
    std::array<std::uint8_t, 8> permutation{{0, 1, 2, 3, 4, 5, 6, 7}};
    std::uint64_t state = splitmix64(
        (static_cast<std::uint64_t>(pair.source) << 32)
        | static_cast<std::uint64_t>(pair.destination));
    state = splitmix64(state ^ selection_key);
    for (std::size_t i = 0; i < 4; ++i) {
        state = splitmix64(state + i);
        const std::size_t selected =
            i + static_cast<std::size_t>(state % (8 - i));
        std::swap(permutation[i], permutation[selected]);
    }
    return {{permutation[0], permutation[1],
             permutation[2], permutation[3]}};
}

bool RnicSsHystereticPathSelector::ingestLoadSample(
        const RnicSsDelayedLoadSample& sample,
        RnicSsTimePs received_at_ps) {
    if (sample.path_id >= _config.path_count) {
        throw std::invalid_argument(
            "Slingshot-like telemetry names an unavailable path");
    }
    if (received_at_ps < sample.observed_at_ps) {
        throw std::invalid_argument(
            "Slingshot-like telemetry arrived before it was observed");
    }
    std::optional<StoredSample>& stored = _samples[sample.path_id];
    if (stored.has_value()) {
        const RnicSsDelayedLoadSample& previous = stored->sample;
        if (sample.observed_at_ps < previous.observed_at_ps
            || (sample.observed_at_ps == previous.observed_at_ps
                && sample.telemetry_sequence <= previous.telemetry_sequence)) {
            return false;
        }
    }
    stored = StoredSample{sample, received_at_ps};
    return true;
}

std::pair<std::uint64_t, bool> RnicSsHystereticPathSelector::loadFor(
        std::uint8_t path_id,
        RnicSsTimePs now_ps) const {
    const std::optional<StoredSample>& stored = _samples[path_id];
    if (!stored.has_value()) {
        return {_config.unknown_path_queue_delay_ps, false};
    }
    const RnicSsDelayedLoadSample& sample = stored->sample;
    if (now_ps < stored->received_at_ps) {
        throw std::invalid_argument(
            "Slingshot-like selection time precedes physical telemetry arrival");
    }
    if (now_ps - sample.observed_at_ps > _config.maximum_sample_age_ps) {
        return {_config.unknown_path_queue_delay_ps, false};
    }
    return {sample.queue_delay_ps, true};
}

RnicSsPathDecision RnicSsHystereticPathSelector::select(
        RnicSsEndpointPair pair,
        std::uint64_t selection_key,
        RnicSsTimePs now_ps,
        std::optional<std::uint8_t> current_path) const {
    ::validatePair(pair);
    if (current_path.has_value() && *current_path >= _config.path_count) {
        throw std::invalid_argument(
            "Slingshot-like current path is outside the physical path set");
    }

    RnicSsPathDecision decision{};
    decision.candidates = sampleFourOfEight(pair, selection_key);
    decision.retained_by_hysteresis = false;

    std::size_t best_index = 0;
    for (std::size_t i = 0; i < decision.candidates.size(); ++i) {
        const auto [load, fresh] = loadFor(decision.candidates[i], now_ps);
        decision.candidate_queue_delay_ps[i] = load;
        decision.candidate_had_fresh_sample[i] = fresh;
        if (i != 0
            && (load < decision.candidate_queue_delay_ps[best_index]
                || (load == decision.candidate_queue_delay_ps[best_index]
                    && decision.candidates[i]
                        < decision.candidates[best_index]))) {
            best_index = i;
        }
    }

    std::size_t selected_index = best_index;
    if (current_path.has_value()) {
        auto current = std::find(
            decision.candidates.begin(),
            decision.candidates.end(),
            *current_path);
        if (current != decision.candidates.end()) {
            const std::size_t current_index = static_cast<std::size_t>(
                std::distance(decision.candidates.begin(), current));
            if (withinHysteresis(
                    decision.candidate_queue_delay_ps[current_index],
                    decision.candidate_queue_delay_ps[best_index],
                    _config.hysteresis_queue_delay_ps)) {
                selected_index = current_index;
                decision.retained_by_hysteresis =
                    current_index != best_index;
            }
        }
    }
    decision.selected_path = decision.candidates[selected_index];
    return decision;
}
