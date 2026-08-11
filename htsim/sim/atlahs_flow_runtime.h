#ifndef ATLAHS_FLOW_RUNTIME_H
#define ATLAHS_FLOW_RUNTIME_H

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>

#include "atlahs_wqe.h"

using AtlahsFlowId = std::uint64_t;

// A GOAL node offset is allocated monotonically within one host schedule (see
// Graph::addNode in lgs/Parser.hpp).  Both values are uint32_t, so this packing
// is a collision-free simulation-wide flow identifier.
constexpr AtlahsFlowId makeAtlahsFlowId(std::uint32_t goal_host,
                                        std::uint32_t goal_offset) noexcept {
    return (static_cast<AtlahsFlowId>(goal_host) << 32) |
           static_cast<AtlahsFlowId>(goal_offset);
}

struct AtlahsFlowRequest {
    AtlahsFlowId flow_id = 0;
    std::uint32_t source = 0;
    std::uint32_t destination = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t start_time_ps = 0;
    std::uint32_t tag = 0;
    std::uint64_t policy_context_token = 0;
};

// The legacy mode reserves LogGOPS's per-NIC gap before handing a flow to
// HTSIM.  A flow runtime owns that timing itself and must not be pre-shaped by
// LogSimInterface.
enum class AtlahsNetworkTiming {
    LegacyLogSimGap,
    RuntimeOwned,
};

// Authority is selected once before setup. Existing HTSIM runtimes retain
// the timing-neutral legacy ledger. A composed SimLLM runtime owns the WQE
// lifecycle internally and exposes only an immutable completion projection.
enum class AtlahsWqeAuthorityMode {
    LegacyLedger,
    NativeRuntime,
};

enum class AtlahsRuntimeEventKind {
    PacketTxStarted,
    PacketTxFinished,
    PacketRxArrived,
    PacketDelivered,
    PacketDropped,
    EcnMarked,
    CnpReceived,
    EligibilityUpdated,
    RateUpdated,
    PfcFrameSubmitted,
    PfcPaused,
    PfcResumed,
    LinkStateChanged,
};

enum class AtlahsRuntimePacketKind {
    Data,
    Retransmission,
    Ack,
    Nak,
    Cnp,
    Pfc,
    OtherControl,
};

enum class AtlahsRuntimeDropLocation {
    None,
    TxPort,
    Fabric,
    RxPort,
};

enum class AtlahsRuntimeDropReason {
    None,
    Injected,
    QueueOverflow,
    LinkDown,
    PolicyRejected,
};

enum class AtlahsRuntimeDropEvidence {
    None,
    Controlled,
    Asserted,
    Observed,
    Inferred,
};

enum class AtlahsRuntimeLinkState {
    Unknown,
    Up,
    Down,
};

struct AtlahsRuntimeEventCapabilities {
    bool packet_attempt_events{false};
    bool ecn_cnp_events{false};
    bool policy_update_events{false};
    bool pfc_events{false};
    bool dynamic_link_events{false};
};

// Immutable observations from the runtime timing authority. Packet times are
// materialized serializer or receive boundaries, never wrapper admission.
// The event kind selects the applicable packet or control payload below. A
// packet terminal retains bounded correlation for late ECN or CNP until its
// parent flow reaches the logical terminal.
struct AtlahsRuntimeEvent {
    AtlahsRuntimeEventKind kind{AtlahsRuntimeEventKind::PacketTxStarted};
    AtlahsFlowId flow_id{0};
    std::uint64_t event_time_ps{0};
    std::uint32_t extent_index{0};
    std::uint64_t packet_index{0};
    std::uint32_t transmission_attempt{0};
    std::uint64_t payload_offset_bytes{0};
    std::uint64_t payload_bytes{0};
    std::uint64_t wire_bytes{0};
    AtlahsRuntimePacketKind packet_kind{AtlahsRuntimePacketKind::Data};
    bool ecn_marked{false};
    AtlahsRuntimeDropLocation drop_location{
        AtlahsRuntimeDropLocation::None};
    AtlahsRuntimeDropReason drop_reason{AtlahsRuntimeDropReason::None};
    std::uint64_t drop_resource_id{0};
    AtlahsRuntimeDropEvidence drop_evidence{
        AtlahsRuntimeDropEvidence::None};
    std::uint64_t policy_context_token{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    std::uint64_t link_id{0};
    std::uint8_t priority{0};
    std::uint32_t pause_quanta{0};
    bool has_pause_duration{false};
    std::uint64_t pause_duration_ps{0};
    std::uint64_t effective_at_ps{0};
    bool has_effective_rate{false};
    std::uint64_t effective_rate_bps{0};
    AtlahsRuntimeLinkState link_state{AtlahsRuntimeLinkState::Unknown};
};

struct AtlahsWqeCompletionProjection {
    AtlahsWqeId wqe_id{0};
    AtlahsWqeObjectId sq_id{0};
    AtlahsWqeObjectId rq_id{0};
    AtlahsWqeObjectId cq_id{0};
    std::uint64_t sq_post_sequence{0};
    std::uint64_t sq_dispatch_sequence{0};
    std::uint64_t cq_post_sequence{0};
    std::uint64_t cq_consume_sequence{0};
    AtlahsTransportKind transport_kind{AtlahsTransportKind::None};
    AtlahsWqeObjectId transport_object_id{0};
};

// Read-only lifecycle observations. Implementations report only work that
// actually committed; mode selection alone never advances these counters.
struct AtlahsWqeAuthorityCounters {
    std::uint64_t native_session_constructed{0};
    std::uint64_t legacy_ledger_constructed{0};
    std::uint64_t native_posts{0};
    std::uint64_t legacy_posts{0};
    std::uint64_t legacy_aborts{0};
    std::uint64_t legacy_mutations{0};
};

class AtlahsFlowRuntime {
public:
    using CompletionHandler = std::function<void(AtlahsFlowId)>;
    using EventHandler = std::function<void(const AtlahsRuntimeEvent&)>;

    virtual ~AtlahsFlowRuntime() = default;

    virtual void setup(std::uint32_t node_count,
                       CompletionHandler complete_flow) = 0;
    virtual void send(const AtlahsFlowRequest& request) = 0;

    virtual AtlahsRuntimeEventCapabilities
    eventCapabilities() const noexcept {
        return {};
    }

    // Install before setup. A runtime that advertises no event vocabulary
    // rejects a nonempty handler instead of fabricating observations.
    virtual void setEventHandler(EventHandler handler) {
        if (handler) {
            throw std::invalid_argument(
                "ATLAHS runtime does not expose event observations");
        }
    }

    // A runtime declares only its stable WQE-level transport object. Packet
    // details remain private to the runtime. Topology-free manifold models
    // intentionally retain the default None binding.
    virtual AtlahsTransportKind transportKind() const noexcept {
        return AtlahsTransportKind::None;
    }

    virtual AtlahsWqeAuthorityMode wqeAuthorityMode() const noexcept {
        return AtlahsWqeAuthorityMode::LegacyLedger;
    }

    virtual std::optional<AtlahsWqeCompletionProjection>
    completionProjection(AtlahsFlowId) const {
        return std::nullopt;
    }

    virtual AtlahsWqeAuthorityCounters
    observedWqeAuthorityCounters() const noexcept {
        return {};
    }

    // True until every physical effect owned by this runtime has drained.
    // This is deliberately stronger than "a completion callback is pending":
    // rnic-cn, for example, remains live through its in-band RETIRE tail.
    virtual bool hasPendingPhysicalWork() const noexcept = 0;
};

#endif  // ATLAHS_FLOW_RUNTIME_H
