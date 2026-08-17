// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_SS_PACKET_H
#define RNIC_SS_PACKET_H

#include <cstdint>
#include <memory>
#include <optional>

#include "atlahs_flow_runtime.h"
#include "network.h"
#include "rnic_ss_core.h"

struct RnicSsPacketTermination {
    packetid_t packet_id;
    bool fabric_drop;
};

class RnicSsPacketLifecycleObserver {
public:
    virtual ~RnicSsPacketLifecycleObserver() = default;
    virtual void observe(
        const RnicSsPacketTermination& termination) noexcept = 0;
};

// One pooled physical packet for the open rnic-ss comparator.  DATA is low
// priority; ACK/SACK, telemetry, pair-selective backpressure, and credits are
// strict non-preemptive high priority at the real serializers in the route.
class RnicSsPacket final : public Packet {
public:
    static RnicSsPacket* newPacket(
        PacketFlow& flow,
        const Route& route,
        packetid_t packet_id,
        RnicSsWirePacketMetadata metadata,
        AtlahsFlowId atlahs_flow_id,
        std::uint64_t payload_byte_offset,
        bool retransmission,
        std::shared_ptr<RnicSsPacketLifecycleObserver> observer);

    ~RnicSsPacket() override = default;
    RnicSsPacket(const RnicSsPacket&) = delete;
    RnicSsPacket& operator=(const RnicSsPacket&) = delete;

    const RnicSsWirePacketMetadata& metadata() const noexcept {
        return *_metadata;
    }
    RnicSsPacketKind kind() const noexcept { return _metadata->kind; }
    AtlahsFlowId atlahsFlowId() const noexcept { return _atlahs_flow_id; }
    std::uint64_t payloadByteOffset() const noexcept {
        return _payload_byte_offset;
    }
    bool isRetransmission() const noexcept { return _retransmission; }

    // Called only from an ns-rosetta grant observation.  This is historical
    // packet state that can be returned later in a physical ACK; it is not a
    // sender-side remote queue read.
    void addQueueResidence(
        std::uint64_t queue_delay_ps,
        std::uint64_t observed_at_ps) noexcept;
    std::uint64_t accumulatedQueueDelayPs() const noexcept {
        return _accumulated_queue_delay_ps;
    }
    std::uint64_t loadObservedAtPs() const noexcept {
        return _load_observed_at_ps;
    }
    bool hasLoadObservation() const noexcept { return _has_load_observation; }

    PktPriority priority() const override;
    void consumeAtEndpoint();
    void free() override;

    void strip_payload(std::uint16_t) override;
    void bounce() override;
    void unbounce(std::uint16_t) override;

private:
    friend class PacketDB<RnicSsPacket>;

    enum class PoolState { Recycled, InFlight };

    RnicSsPacket() = default;
    void initialize(
        PacketFlow& flow,
        const Route& route,
        packetid_t packet_id,
        RnicSsWirePacketMetadata metadata,
        AtlahsFlowId atlahs_flow_id,
        std::uint64_t payload_byte_offset,
        bool retransmission,
        std::shared_ptr<RnicSsPacketLifecycleObserver> observer);
    void terminate(bool fabric_drop);

    std::optional<RnicSsWirePacketMetadata> _metadata;
    AtlahsFlowId _atlahs_flow_id{0};
    std::uint64_t _payload_byte_offset{0};
    bool _retransmission{false};
    std::uint64_t _accumulated_queue_delay_ps{0};
    std::uint64_t _load_observed_at_ps{0};
    bool _has_load_observation{false};
    std::shared_ptr<RnicSsPacketLifecycleObserver> _observer;
    PoolState _pool_state{PoolState::Recycled};

    static PacketDB<RnicSsPacket> _packet_db;
};

#endif  // RNIC_SS_PACKET_H
