#ifndef ATLAHS_FLOW_RUNTIME_H
#define ATLAHS_FLOW_RUNTIME_H

#include <cstdint>
#include <functional>

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

class AtlahsFlowRuntime {
public:
    using CompletionHandler = std::function<void(AtlahsFlowId)>;

    virtual ~AtlahsFlowRuntime() = default;

    virtual void setup(std::uint32_t node_count,
                       CompletionHandler complete_flow) = 0;
    virtual void send(const AtlahsFlowRequest& request) = 0;
};

#endif  // ATLAHS_FLOW_RUNTIME_H
