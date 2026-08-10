#ifndef ATLAHS_FLOW_RUNTIME_H
#define ATLAHS_FLOW_RUNTIME_H

#include <cstdint>
#include <functional>
#include <optional>

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

class AtlahsFlowRuntime {
public:
    using CompletionHandler = std::function<void(AtlahsFlowId)>;

    virtual ~AtlahsFlowRuntime() = default;

    virtual void setup(std::uint32_t node_count,
                       CompletionHandler complete_flow) = 0;
    virtual void send(const AtlahsFlowRequest& request) = 0;

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

    // True until every physical effect owned by this runtime has drained.
    // This is deliberately stronger than "a completion callback is pending":
    // rnic-cn, for example, remains live through its in-band RETIRE tail.
    virtual bool hasPendingPhysicalWork() const noexcept = 0;
};

#endif  // ATLAHS_FLOW_RUNTIME_H
