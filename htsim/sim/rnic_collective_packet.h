// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_PACKET_H
#define RNIC_COLLECTIVE_PACKET_H

#include <cstdint>
#include <memory>
#include <optional>

#include "atlahs_flow_runtime.h"
#include "network.h"
#include "rnic_collective_control.h"
#include "rnic_packet_extent.h"

// The seven in-band wire objects used by the collective-network profile.
// Packet::type() intentionally remains the neutral HTSIM IP value: existing
// packet_type values cannot represent these kinds without changing network.h,
// and collective endpoints must dispatch on this explicit kind instead.
enum class RnicCollectivePacketKind {
    DATA,
    DECLARE,
    ACCEPT,
    GRANT_UPDATE,
    GAP_NACK,
    GAP_RESOLVED,
    RETIRE,
};

// The immutable end-of-flow ledger carried by DATA and RETIRE.  A receiver
// may remove membership only after RETIRE has arrived and its accumulated DATA
// ledger equals all three totals.
struct RnicCollectiveFinalLedger {
    std::uint64_t total_payload_bytes;
    std::uint64_t total_wire_bytes;
    std::uint64_t total_data_packets;
};

struct RnicCollectiveDataMetadata {
    std::uint64_t packet_index;
    std::uint64_t payload_byte_offset;
    RnicPacketExtent extent;
    std::uint64_t eta_ps;
    RnicCollectiveFinalLedger final_ledger;
    // Zero is the original transmission.  Deterministic retransmission of the
    // same logical packet increments this attempt; receiver state is keyed by
    // flow plus packet_index rather than by a simulator lifecycle identifier.
    std::uint32_t transmission_attempt{0};
    // Exactly one backlogged DATA packet is selected at a deterministic
    // pseudo-random position in each delay window. After post-resequence
    // release, this bit causes the receiver to return an explicit-rate ACK.
    bool rate_feedback_mark{false};

    bool isFinalPacket() const noexcept {
        return packet_index + 1 == final_ledger.total_data_packets;
    }
};

// Exact single-packet range requested by deterministic gap recovery.  This is
// protocol-visible information only: a truly dropped packet has no
// receiver-observed ETA or simulator lifecycle identifier.
struct RnicCollectiveGapNackMetadata {
    std::uint64_t packet_index;
    std::uint64_t payload_byte_offset;
    RnicPacketExtent extent;
    std::uint32_t requested_transmission_attempt;
    bool rate_feedback_mark{false};
};

// Physical receiver-to-sender closure for one previously NACKed logical
// packet.  It carries no ETA or simulator lifecycle identity: the exact L4
// range and highest retry attempt made unnecessary are protocol-visible.
struct RnicCollectiveGapResolvedMetadata {
    std::uint64_t packet_index;
    std::uint64_t payload_byte_offset;
    RnicPacketExtent extent;
    std::uint32_t acknowledged_transmission_attempt;
};

// RETIRE closes the original DATA sequence. The sender publishes the maximum
// Ring-CAM release deadline over every original PSN. A RETIRE that overtakes
// DATA can therefore audit a tail gap at that exact deadline, after all due
// same-timestamp releases, without another Delta or residual tick.
struct RnicCollectiveRetireMetadata {
    RnicCollectiveFinalLedger final_ledger;
    std::uint64_t final_gap_detection_ps;
};

// DECLARE exposes only the receiver's explicit-rate input.
struct RnicCollectiveDeclareMetadata {
    std::uint32_t nflow;
};

// CREATED plus exactly one terminal observation gives the integration a
// complete outstanding-packet ledger.  A direct Packet::free() is always a
// fabric drop; successful delivery requires consumeAtEndpoint().
enum class RnicCollectivePacketLifecycle {
    CREATED,
    ENDPOINT_CONSUMED,
    FABRIC_DROP,
};

struct RnicCollectivePacketObservation {
    std::uint64_t lifecycle_id;
    RnicCollectivePacketLifecycle lifecycle;
    RnicCollectivePacketKind kind;
    AtlahsFlowId flow_id;
    std::uint32_t source;
    std::uint32_t destination;
    packetid_t htsim_packet_id;
    std::uint16_t wire_bytes;
    std::uint32_t route_hops_completed;
    std::optional<std::uint64_t> data_packet_index;
};

class RnicCollectivePacketLifecycleObserver {
public:
    virtual ~RnicCollectivePacketLifecycleObserver() = default;
    virtual void observe(const RnicCollectivePacketObservation& observation) noexcept = 0;
};

// A pooled, explicit-route HTSIM packet.  Metadata is immutable after factory
// construction, DATA is low priority, and all six controls are high priority.
// GRANT_UPDATE normally represents a marked-DATA explicit-rate ACK. A sender
// whose lease expires re-DECLAREs before resuming, and receives the same
// current-rate packet with marked_data_ack=false.
// Control identity comes only from kind()/priority(): header_only remains false
// because HTSIM also uses that bit to mean a DATA packet was trimmed.  There is
// deliberately no trim, bounce, route replacement, or retransmission path.
class RnicCollectivePacket final : public Packet {
public:
    static RnicCollectivePacket* newData(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        AtlahsFlowId flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        const RnicCollectiveDataMetadata& metadata,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static RnicCollectivePacket* newDeclare(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        AtlahsFlowId flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t wire_bytes,
        const RnicCollectiveDeclareMetadata& metadata,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static RnicCollectivePacket* newAccept(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t wire_bytes,
        const RnicCollectiveGrant& grant,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static RnicCollectivePacket* newGrantUpdate(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t wire_bytes,
        const RnicCollectiveGrant& grant,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static RnicCollectivePacket* newGapNack(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        AtlahsFlowId flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t wire_bytes,
        const RnicCollectiveGapNackMetadata& gap_nack,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static RnicCollectivePacket* newGapResolved(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        AtlahsFlowId flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t wire_bytes,
        const RnicCollectiveGapResolvedMetadata& gap_resolved,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static RnicCollectivePacket* newRetire(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        AtlahsFlowId flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t wire_bytes,
        const RnicCollectiveRetireMetadata& retire,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    ~RnicCollectivePacket() override = default;
    RnicCollectivePacket(const RnicCollectivePacket&) = delete;
    RnicCollectivePacket& operator=(const RnicCollectivePacket&) = delete;

    RnicCollectivePacketKind kind() const noexcept { return _kind; }
    AtlahsFlowId atlahsFlowId() const noexcept { return _flow_id; }
    std::uint32_t source() const noexcept { return _source; }
    std::uint32_t destination() const noexcept { return _destination; }
    std::uint64_t lifecycleId() const noexcept { return _lifecycle_id; }

    const RnicCollectiveDataMetadata& data() const;
    const RnicCollectiveDeclareMetadata& declaration() const;
    const RnicCollectiveGrant& grant() const;
    const RnicCollectiveGapNackMetadata& gapNack() const;
    const RnicCollectiveGapResolvedMetadata& gapResolved() const;
    const RnicCollectiveRetireMetadata& retire() const;
    const RnicCollectiveFinalLedger& finalLedger() const;
    bool isFinalDataPacket() const;

    PktPriority priority() const override;

    // The sole successful terminal operation.  Every inherited/free-based
    // disappearance is classified as FABRIC_DROP instead.
    void consumeAtEndpoint();
    void free() override;

    void strip_payload(std::uint16_t trim_size) override;
    void bounce() override;
    void unbounce(std::uint16_t packet_size) override;

private:
    friend class PacketDB<RnicCollectivePacket>;

    enum class PoolState {
        RECYCLED,
        IN_FLIGHT,
    };

    RnicCollectivePacket() = default;

    static RnicCollectivePacket* newPacket(
        PacketFlow& flow,
        const Route& route,
        packetid_t htsim_packet_id,
        RnicCollectivePacketKind kind,
        AtlahsFlowId flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint16_t wire_bytes,
        std::optional<RnicCollectiveDataMetadata> data,
        std::optional<RnicCollectiveDeclareMetadata> declaration,
        std::optional<RnicCollectiveGrant> grant,
        std::optional<RnicCollectiveGapNackMetadata> gap_nack,
        std::optional<RnicCollectiveRetireMetadata> retire,
        std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved,
        std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer);

    static std::uint16_t checkedWireBytes(std::uint64_t wire_bytes);
    static void validateRoute(const Route& route);
    static void validateFinalLedger(const RnicCollectiveFinalLedger& final_ledger);
    static void validateData(const RnicCollectiveDataMetadata& metadata);
    static void validateDeclaration(const RnicCollectiveDeclareMetadata& metadata);
    static void validateGrant(const RnicCollectiveGrant& grant,
                              RnicCollectivePacketKind packet_kind);
    static void validateGapNack(const RnicCollectiveGapNackMetadata& gap_nack);
    static void validateGapResolved(const RnicCollectiveGapResolvedMetadata& gap_resolved);
    static std::uint64_t takeLifecycleId();

    void initialize(PacketFlow& flow,
                    const Route& route,
                    packetid_t htsim_packet_id,
                    RnicCollectivePacketKind kind,
                    AtlahsFlowId flow_id,
                    std::uint32_t source,
                    std::uint32_t destination,
                    std::uint16_t wire_bytes,
                    std::optional<RnicCollectiveDataMetadata> data,
                    std::optional<RnicCollectiveDeclareMetadata> declaration,
                    std::optional<RnicCollectiveGrant> grant,
                    std::optional<RnicCollectiveGapNackMetadata> gap_nack,
                    std::optional<RnicCollectiveRetireMetadata> retire,
                    std::optional<RnicCollectiveGapResolvedMetadata> gap_resolved,
                    std::shared_ptr<RnicCollectivePacketLifecycleObserver> observer,
                    std::uint64_t lifecycle_id);
    RnicCollectivePacketObservation observation(RnicCollectivePacketLifecycle lifecycle) const;
    void terminate(RnicCollectivePacketLifecycle lifecycle);

    // Route mutation is intentionally unavailable after factory construction.
    void set_route(const Route& route) override;
    void set_route(const Route* route) override;
    void set_route(PacketFlow& flow,
                   const Route& route,
                   int packet_size,
                   packetid_t packet_id) override;

    RnicCollectivePacketKind _kind{RnicCollectivePacketKind::DATA};
    AtlahsFlowId _flow_id{0};
    std::uint32_t _source{0};
    std::uint32_t _destination{0};
    std::uint16_t _wire_bytes{0};
    std::uint64_t _lifecycle_id{0};
    std::optional<RnicCollectiveDataMetadata> _data;
    std::optional<RnicCollectiveDeclareMetadata> _declaration;
    std::optional<RnicCollectiveGrant> _grant;
    std::optional<RnicCollectiveGapNackMetadata> _gap_nack;
    std::optional<RnicCollectiveRetireMetadata> _retire;
    std::optional<RnicCollectiveGapResolvedMetadata> _gap_resolved;
    std::shared_ptr<RnicCollectivePacketLifecycleObserver> _observer;
    PoolState _pool_state{PoolState::RECYCLED};

    static PacketDB<RnicCollectivePacket> _packetdb;
    static std::uint64_t _next_lifecycle_id;
};

#endif  // RNIC_COLLECTIVE_PACKET_H
