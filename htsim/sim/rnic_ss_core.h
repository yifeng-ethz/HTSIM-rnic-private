// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_SS_CORE_H
#define RNIC_SS_CORE_H

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <variant>
#include <vector>

// Transport-independent primitives for the Slingshot-like RNIC model.  This
// layer has no route, queue, event-list, or global-fabric reference: every
// state transition below is driven by metadata that can be carried by a
// physical wire packet.

using RnicSsNodeId = std::uint32_t;
using RnicSsSequence = std::uint64_t;
using RnicSsTimePs = std::uint64_t;

struct RnicSsEndpointPair {
    RnicSsNodeId source;
    RnicSsNodeId destination;

    bool operator==(const RnicSsEndpointPair& other) const noexcept {
        return source == other.source && destination == other.destination;
    }
    bool operator!=(const RnicSsEndpointPair& other) const noexcept {
        return !(*this == other);
    }
    bool operator<(const RnicSsEndpointPair& other) const noexcept {
        return source < other.source
            || (source == other.source && destination < other.destination);
    }
};

// ACK_SACK is one physical reverse-path packet: next_expected_sequence is the
// cumulative ACK and sack_bitmap selectively acknowledges the following 64
// sequence positions.  A normalized receiver snapshot always has bit zero
// clear because receipt of that packet would advance the cumulative ACK.
enum class RnicSsPacketKind {
    DATA,
    ACK_SACK,
    TELEMETRY,
    BP_ENABLE,
    BP_DISABLE,
    CREDIT,
};

struct RnicSsDataMetadata {
    RnicSsEndpointPair pair;
    RnicSsSequence sequence;
    std::uint32_t payload_bytes;
    std::uint8_t path_id;
};

// queue_delay_ps is the serialization backlog observed at a real output port,
// not a live lookup.  observed_at_ps plus telemetry_sequence make delayed and
// reordered physical telemetry updates deterministic at the sender.
struct RnicSsDelayedLoadSample {
    std::uint8_t path_id;
    std::uint64_t queue_delay_ps;
    RnicSsTimePs observed_at_ps;
    std::uint64_t telemetry_sequence;
};

struct RnicSsAckSackMetadata {
    RnicSsEndpointPair forward_pair;
    RnicSsSequence next_expected_sequence;
    std::uint64_t sack_bitmap;
    // Load can be piggybacked on the physical ACK/SACK.  Standalone TELEMETRY
    // remains available when the delayed sample is produced independently.
    std::optional<RnicSsDelayedLoadSample> returned_load_sample{std::nullopt};
};

struct RnicSsTelemetryMetadata {
    RnicSsDelayedLoadSample sample;
};

// Each enable/disable transition uses a strictly increasing control epoch.
// An enable can include an initial cumulative byte grant.  A disable carries
// zero: it removes credit gating rather than granting an implicit burst.
struct RnicSsBackpressureMetadata {
    RnicSsEndpointPair forward_pair;
    std::uint64_t control_epoch;
    std::uint64_t granted_wire_bytes_total;
};

// Credits are cumulative within one control epoch.  This makes duplicate,
// delayed, and reordered physical CREDIT packets idempotent without an oracle.
struct RnicSsCreditMetadata {
    RnicSsEndpointPair forward_pair;
    std::uint64_t control_epoch;
    std::uint64_t granted_wire_bytes_total;
};

using RnicSsPacketPayload = std::variant<
    RnicSsDataMetadata,
    RnicSsAckSackMetadata,
    RnicSsTelemetryMetadata,
    RnicSsBackpressureMetadata,
    RnicSsCreditMetadata>;

struct RnicSsWirePacketMetadata {
    RnicSsPacketKind kind;
    RnicSsNodeId wire_source;
    RnicSsNodeId wire_destination;
    std::uint32_t wire_bytes;
    RnicSsPacketPayload payload;
};

struct RnicSsPacketValidationConfig {
    std::uint32_t minimum_control_wire_bytes{1};
    std::uint32_t maximum_wire_bytes{65535};
    std::uint8_t path_count{8};
    std::uint64_t maximum_granted_wire_bytes_total{
        UINT64_MAX};

    void validate() const;
};

void validateRnicSsWirePacket(
    const RnicSsWirePacketMetadata& packet,
    const RnicSsPacketValidationConfig& config = {});

enum class RnicSsReceiveDisposition {
    NEW_IN_ORDER,
    NEW_OUT_OF_ORDER,
    DUPLICATE,
    OUTSIDE_SACK_WINDOW,
};

struct RnicSsReceiveResult {
    RnicSsReceiveDisposition disposition;
    std::uint32_t cumulative_advance;
};

// A receiver scoreboard covering [next_expected, next_expected + 63].
class RnicSsSackScoreboard {
public:
    explicit RnicSsSackScoreboard(
        RnicSsSequence initial_next_expected = 0);

    RnicSsReceiveResult observe(RnicSsSequence sequence);
    RnicSsSequence nextExpectedSequence() const noexcept {
        return _next_expected_sequence;
    }
    std::uint64_t bitmap() const noexcept { return _bitmap; }
    RnicSsAckSackMetadata snapshot(RnicSsEndpointPair pair) const noexcept;

private:
    RnicSsSequence _next_expected_sequence;
    std::uint64_t _bitmap{0};
};

struct RnicSsSelectiveRepeatConfig {
    std::uint32_t window_packets{64};
    // Conservative mlx5-style silent-final-loss fallback requested for the
    // simulator.  This configurable 50 ms default is not a claim about
    // Slingshot hardware; a normal lossless run should never invoke it.
    RnicSsTimePs retransmission_timeout_ps{50'000'000'000ULL};
    std::uint32_t maximum_retransmissions{8};

    void validate() const;
};

struct RnicSsOutstandingPacket {
    RnicSsSequence sequence;
    std::uint32_t wire_bytes;
    RnicSsTimePs first_sent_at_ps;
    RnicSsTimePs last_sent_at_ps;
    std::uint32_t transmission_count;
};

struct RnicSsAckResult {
    std::vector<RnicSsOutstandingPacket> newly_acked;
    // Outstanding sequences below the highest selective ACK are observable
    // holes and can be retried immediately.  A silent final loss has no such
    // evidence and remains protected by the configurable RTO.
    std::vector<RnicSsOutstandingPacket> reported_holes;
};

enum class RnicSsRetransmissionReason {
    SACK_HOLE,
    RTO,
};

// Sender-side selective-repeat ledger.  New DATA sequences are contiguous;
// ACK/SACK removes only the cumulatively or selectively acknowledged records.
class RnicSsSelectiveRepeatLedger {
public:
    RnicSsSelectiveRepeatLedger(
        RnicSsEndpointPair pair,
        RnicSsSelectiveRepeatConfig config = {},
        RnicSsSequence initial_sequence = 0);

    bool canSendNewPacket() const noexcept;
    RnicSsSequence recordNewTransmission(
        std::uint32_t wire_bytes,
        RnicSsTimePs now_ps);
    RnicSsAckResult applyAck(const RnicSsAckSackMetadata& ack);
    std::vector<RnicSsOutstandingPacket> retransmissionCandidates(
        RnicSsTimePs now_ps) const;
    void recordRetransmission(
        RnicSsSequence sequence,
        RnicSsTimePs now_ps,
        RnicSsRetransmissionReason reason = RnicSsRetransmissionReason::RTO);

    const RnicSsEndpointPair& pair() const noexcept { return _pair; }
    RnicSsSequence nextSequence() const noexcept { return _next_sequence; }
    std::size_t outstandingPacketCount() const noexcept {
        return _outstanding.size();
    }
    const std::map<RnicSsSequence, RnicSsOutstandingPacket>& outstanding()
        const noexcept {
        return _outstanding;
    }

private:
    RnicSsEndpointPair _pair;
    RnicSsSelectiveRepeatConfig _config;
    RnicSsSequence _initial_sequence;
    RnicSsSequence _next_sequence;
    std::map<RnicSsSequence, RnicSsOutstandingPacket> _outstanding;
};

struct RnicSsCreditConfig {
    // Bounds burst credit admitted by any one physical control update.
    std::uint64_t maximum_credit_ahead_wire_bytes{1ULL << 20};

    void validate() const;
};

enum class RnicSsControlApplyResult {
    APPLIED,
    DUPLICATE_OR_STALE,
    WRONG_EPOCH,
};

struct RnicSsPairCreditSnapshot {
    bool backpressure_enabled;
    std::uint64_t control_epoch;
    std::uint64_t granted_wire_bytes_total;
    std::uint64_t consumed_wire_bytes_total;
    std::uint64_t availableWireBytes() const noexcept {
        return granted_wire_bytes_total - consumed_wire_bytes_total;
    }
};

class RnicSsPairCreditState {
public:
    RnicSsPairCreditState(
        RnicSsEndpointPair pair,
        RnicSsCreditConfig config = {});

    RnicSsControlApplyResult applyEnable(
        const RnicSsBackpressureMetadata& enable);
    RnicSsControlApplyResult applyDisable(
        const RnicSsBackpressureMetadata& disable);
    RnicSsControlApplyResult applyCredit(
        const RnicSsCreditMetadata& credit);

    bool canSend(std::uint32_t wire_bytes) const noexcept;
    void consumeForData(std::uint32_t wire_bytes);
    RnicSsPairCreditSnapshot snapshot() const noexcept;

private:
    void validatePair(const RnicSsEndpointPair& pair) const;
    void validateCreditAhead(std::uint64_t granted_total) const;

    RnicSsEndpointPair _pair;
    RnicSsCreditConfig _config;
    bool _enabled{false};
    std::uint64_t _control_epoch{0};
    std::uint64_t _granted_total{0};
    std::uint64_t _consumed_total{0};
};

// Owns independent contributor state for each directed endpoint pair.
class RnicSsPairCreditTable {
public:
    explicit RnicSsPairCreditTable(RnicSsCreditConfig config = {});

    RnicSsPairCreditState& stateFor(RnicSsEndpointPair pair);
    const RnicSsPairCreditState* find(RnicSsEndpointPair pair) const noexcept;
    std::size_t size() const noexcept { return _states.size(); }

private:
    RnicSsCreditConfig _config;
    std::map<RnicSsEndpointPair, RnicSsPairCreditState> _states;
};

struct RnicSsPathSelectionConfig {
    std::uint8_t path_count{8};
    std::uint8_t candidate_count{4};
    std::uint64_t hysteresis_queue_delay_ps{0};
    std::uint64_t maximum_sample_age_ps{10'000'000ULL};
    std::uint64_t unknown_path_queue_delay_ps{UINT64_MAX};

    void validate() const;
};

struct RnicSsPathDecision {
    std::uint8_t selected_path;
    std::array<std::uint8_t, 4> candidates;
    std::array<std::uint64_t, 4> candidate_queue_delay_ps;
    std::array<bool, 4> candidate_had_fresh_sample;
    bool retained_by_hysteresis;
};

// Deterministic power-of-four selection over eight equal-cost candidates.
// selection_key can remain fixed for ordered flow-level routing or vary per
// packet for explicitly unordered experiments.
class RnicSsHystereticPathSelector {
public:
    explicit RnicSsHystereticPathSelector(
        RnicSsPathSelectionConfig config = {});

    static std::array<std::uint8_t, 4> sampleFourOfEight(
        RnicSsEndpointPair pair,
        std::uint64_t selection_key) noexcept;

    // The receiving integration supplies both the delayed observation and the
    // physical packet arrival time.  Older/reordered observations are ignored.
    bool ingestLoadSample(
        const RnicSsDelayedLoadSample& sample,
        RnicSsTimePs received_at_ps);

    RnicSsPathDecision select(
        RnicSsEndpointPair pair,
        std::uint64_t selection_key,
        RnicSsTimePs now_ps,
        std::optional<std::uint8_t> current_path = std::nullopt) const;

private:
    struct StoredSample {
        RnicSsDelayedLoadSample sample;
        RnicSsTimePs received_at_ps;
    };

    std::pair<std::uint64_t, bool> loadFor(
        std::uint8_t path_id,
        RnicSsTimePs now_ps) const;

    RnicSsPathSelectionConfig _config;
    std::array<std::optional<StoredSample>, 8> _samples;
};

#endif
