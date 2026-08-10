// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SIMLLM_HTSIM_NETWORK_PORT_H
#define SIMLLM_HTSIM_NETWORK_PORT_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "rnic_wire_serialization.h"
#include "simllm/rnic/network_port.h"

namespace htsim::simllm_rnic {

inline constexpr std::uint32_t kHtsimNetworkPortConfigVersion = 1;

struct HtsimNetworkPortConfig {
    std::uint32_t version{kHtsimNetworkPortConfigVersion};
    std::size_t capacity{1};
    std::uint64_t link_rate_bps{400000000000ULL};
    std::uint64_t data_header_bytes{0};
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

// ABI-v1 HTSIM service for one whole-flow extent. The port retains only
// immutable descriptor correlation and network-owned token state. Hardware
// queues, QPs, PCIe, DMA and scheduling remain in the SimLLM device.
class HtsimNetworkPort final : public simllm::rnic::NetworkPort {
public:
    explicit HtsimNetworkPort(HtsimNetworkPortConfig config);

    simllm::rnic::NetworkSubmitResult trySubmit(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        simllm::rnic::Picoseconds now_ps) override;

    std::optional<simllm::rnic::Picoseconds> nextEventTime() const;
    std::vector<simllm::rnic::NetworkEvent> takeDue(
        simllm::rnic::Picoseconds now_ps);
    bool hasPendingPhysicalWork() const noexcept;

    const HtsimNetworkPortConfig& config() const noexcept { return config_; }
    const std::vector<HtsimIssuedToken>& issued() const noexcept {
        return issued_;
    }
    const std::vector<HtsimTerminalToken>& terminals() const noexcept {
        return terminals_;
    }
    std::vector<simllm::rnic::NetworkToken> liveTokens() const;
    std::uint32_t ownerSource(simllm::rnic::NetworkToken token) const;

private:
    struct LiveToken {
        simllm::rnic::NetworkTxDescriptor descriptor;
        simllm::rnic::Picoseconds accepted_at_ps{0};
        simllm::rnic::Picoseconds port_tx_at_ps{0};
        simllm::rnic::Picoseconds terminal_at_ps{0};
    };

    static void validateConfig(const HtsimNetworkPortConfig& config);
    void validateDescriptor(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        simllm::rnic::Picoseconds now_ps) const;
    simllm::rnic::Picoseconds terminalTime(
        std::uint64_t payload_bytes,
        simllm::rnic::Picoseconds now_ps) const;

    HtsimNetworkPortConfig config_;
    simllm::rnic::NetworkToken next_token_{1};
    bool drop_emitted_{false};
    std::map<simllm::rnic::NetworkToken, LiveToken> live_;
    std::multimap<simllm::rnic::Picoseconds,
                  simllm::rnic::NetworkToken> scheduled_;
    std::set<simllm::rnic::FlowId> seen_flows_;
    std::map<simllm::rnic::NetworkToken, std::uint32_t> token_sources_;
    std::vector<HtsimIssuedToken> issued_;
    std::vector<HtsimTerminalToken> terminals_;
};

}  // namespace htsim::simllm_rnic

#endif  // SIMLLM_HTSIM_NETWORK_PORT_H
