// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SIMLLM_HTSIM_NETWORK_PORT_H
#define SIMLLM_HTSIM_NETWORK_PORT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "atlahs_flow_runtime.h"
#include "rnic_packet_extent.h"
#include "rnic_wire_serialization.h"
#include "simllm/rnic/network_port.h"

namespace htsim::simllm_rnic {

inline constexpr std::uint32_t kHtsimNetworkPortConfigVersion = 1;

struct HtsimNetworkPortConfig {
    std::uint32_t version{kHtsimNetworkPortConfigVersion};
    std::uint32_t network_abi_version{
        simllm::rnic::kNetworkPortAbiVersionV1};
    // Capacity belongs to the unbound Tier A serializer. A runtime-bound
    // composition replaces it with the complete native-session capacity so
    // HTSIM remains the only admission and drop authority.
    std::size_t capacity{1};
    std::uint64_t link_rate_bps{400000000000ULL};
    std::uint64_t data_header_bytes{0};
    std::uint64_t max_wire_packet_bytes{4096};
    simllm::rnic::Picoseconds propagation_delay_ps{0};
    std::uint32_t endpoint_count{0};
    std::uint8_t traffic_class{3};
    bool control_frames{false};
    bool congestion{false};
    bool dynamic_link_events{false};
    bool drop_first{false};
};

struct HtsimIssuedToken {
    simllm::rnic::NetworkToken token{0};
    simllm::rnic::WqeId wqe_id{0};
    std::uint64_t wr_id{0};
    simllm::rnic::FlowId flow_id{0};
    simllm::rnic::PolicyContextToken policy_context_token{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    simllm::rnic::Picoseconds accepted_at_ps{0};
    simllm::rnic::Picoseconds port_tx_at_ps{0};
    std::uint64_t payload_bytes{0};
};

struct HtsimTerminalToken {
    simllm::rnic::NetworkToken token{0};
    simllm::rnic::WqeId wqe_id{0};
    simllm::rnic::FlowId flow_id{0};
    simllm::rnic::NetworkEventKind kind{
        simllm::rnic::NetworkEventKind::Delivered};
    simllm::rnic::Picoseconds at_ps{0};
    bool ecn_marked{false};
    simllm::rnic::DropLocation drop_location{
        simllm::rnic::DropLocation::None};
    simllm::rnic::DropReason drop_reason{
        simllm::rnic::DropReason::None};
};

// Versioned HTSIM service for one whole-flow extent. The port retains only
// immutable descriptor correlation and network-owned token state. Hardware
// queues, QPs, PCIe, DMA and scheduling remain in the SimLLM device.
class HtsimNetworkPort final : public simllm::rnic::NetworkPort {
public:
    using TerminalReadyHandler =
        std::function<void(simllm::rnic::Picoseconds)>;

    explicit HtsimNetworkPort(HtsimNetworkPortConfig config);

    // Bind the compatibility port to the htsim transport and fabric runtime.
    // The runtime remains externally owned and must outlive this port. Its
    // completion callback is reduced to one flow terminal event without
    // exposing any native queue or QP object.
    void bindRuntime(
        AtlahsFlowRuntime& runtime,
        std::uint32_t node_count,
        std::size_t runtime_capacity,
        TerminalReadyHandler terminal_ready);

    simllm::rnic::NetworkPortCapabilities
    capabilities() const noexcept override;

    simllm::rnic::NetworkSubmitResult trySubmit(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        simllm::rnic::Picoseconds now_ps) override;

    std::optional<simllm::rnic::Picoseconds> nextEventTime() const;
    std::vector<simllm::rnic::NetworkEvent> takeDue(
        simllm::rnic::Picoseconds now_ps);
    bool hasPendingPhysicalWork() const noexcept;
    bool hasBoundRuntime() const noexcept { return runtime_ != nullptr; }
    std::size_t effectiveCapacity() const noexcept {
        return runtime_capacity_.value_or(config_.capacity);
    }

    const HtsimNetworkPortConfig& config() const noexcept { return config_; }
    const std::vector<HtsimIssuedToken>& issued() const noexcept {
        return issued_;
    }
    const std::vector<HtsimTerminalToken>& terminals() const noexcept {
        return terminals_;
    }
    const std::vector<simllm::rnic::NetworkEvent>& packetEvents()
            const noexcept {
        return packet_events_;
    }
    const std::vector<simllm::rnic::NetworkEvent>& controlEvents()
            const noexcept {
        return control_events_;
    }
    std::vector<simllm::rnic::NetworkToken> liveTokens() const;
    std::uint32_t ownerSource(simllm::rnic::NetworkToken token) const;

private:
    struct LiveToken {
        simllm::rnic::NetworkTxDescriptor descriptor;
        simllm::rnic::Picoseconds accepted_at_ps{0};
        simllm::rnic::Picoseconds port_tx_at_ps{0};
        simllm::rnic::Picoseconds terminal_at_ps{0};
        bool terminal_queued{false};
    };

    using ScheduledKey =
        std::pair<simllm::rnic::Picoseconds, std::uint64_t>;
    using RuntimePacketKey =
        std::tuple<simllm::rnic::FlowId, std::uint64_t, std::uint32_t>;

    static void validateConfig(const HtsimNetworkPortConfig& config);
    void validateDescriptor(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        simllm::rnic::Picoseconds now_ps) const;
    simllm::rnic::Picoseconds terminalTime(
        std::uint64_t payload_bytes,
        simllm::rnic::Picoseconds now_ps) const;
    void scheduleEvent(const simllm::rnic::NetworkEvent& event);
    simllm::rnic::NetworkToken allocateToken();
    void scheduleUnboundV2Events(
        simllm::rnic::NetworkToken extent_token,
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        simllm::rnic::Picoseconds now_ps);
    void runtimeEvent(const AtlahsRuntimeEvent& event);
    void runtimeCompleted(AtlahsFlowId flow_id);
    void validateTerminal(
        const simllm::rnic::NetworkEvent& event,
        const LiveToken& live) const;
    void rollbackSubmission(
        simllm::rnic::NetworkToken token,
        simllm::rnic::FlowId flow_id,
        std::uint64_t first_event_sequence) noexcept;
    void purgeCompletedPacketTokens(
        simllm::rnic::FlowId flow_id) noexcept;

    HtsimNetworkPortConfig config_;
    AtlahsFlowRuntime* runtime_{nullptr};
    std::optional<std::size_t> runtime_capacity_;
    TerminalReadyHandler terminal_ready_;
    simllm::rnic::NetworkToken next_token_{1};
    std::uint64_t next_event_sequence_{1};
    bool drop_emitted_{false};
    std::map<simllm::rnic::NetworkToken, LiveToken> live_;
    std::map<ScheduledKey, simllm::rnic::NetworkEvent> scheduled_;
    std::set<simllm::rnic::FlowId> seen_flows_;
    std::map<simllm::rnic::FlowId, simllm::rnic::NetworkToken>
        token_by_flow_;
    std::map<simllm::rnic::NetworkToken, std::uint32_t> token_sources_;
    std::map<RuntimePacketKey, simllm::rnic::NetworkToken>
        runtime_packet_tokens_;
    std::map<RuntimePacketKey, simllm::rnic::NetworkToken>
        completed_runtime_packet_tokens_;
    std::optional<std::uint64_t> submission_event_sequence_floor_;
    std::vector<HtsimIssuedToken> issued_;
    std::vector<HtsimTerminalToken> terminals_;
    std::vector<simllm::rnic::NetworkEvent> packet_events_;
    std::vector<simllm::rnic::NetworkEvent> control_events_;
};

}  // namespace htsim::simllm_rnic

#endif  // SIMLLM_HTSIM_NETWORK_PORT_H
