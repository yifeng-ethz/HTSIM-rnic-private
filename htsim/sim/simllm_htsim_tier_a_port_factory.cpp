// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "tier_a_port_factory.h"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "simllm_htsim_network_port.h"

namespace simllm::rnic::tier_a {
namespace {

htsim::simllm_rnic::HtsimNetworkPortConfig portConfig(
        const PortConfig& config) {
    if (config.link_rate_gbps
        > std::numeric_limits<std::uint64_t>::max() / 1000000000ULL) {
        throw std::overflow_error("Tier A link rate overflows bit/s");
    }
    htsim::simllm_rnic::HtsimNetworkPortConfig result;
    result.network_abi_version = config.network_abi_version;
    result.capacity = config.capacity;
    result.link_rate_bps = config.link_rate_gbps * 1000000000ULL;
    result.data_header_bytes = config.data_header_bytes;
    result.propagation_delay_ps = config.propagation_delay_ps;
    result.endpoint_count = 4;
    result.traffic_class = 3;
    result.control_frames = config.control_frames;
    result.congestion = config.congestion;
    result.drop_first = config.drop_first;
    return result;
}

class HtsimDrivenPort final : public DrivenPort {
public:
    explicit HtsimDrivenPort(const PortConfig& config)
        : port_(portConfig(config)) {}

    NetworkPortCapabilities capabilities() const noexcept override {
        return port_.capabilities();
    }

    NetworkSubmitResult trySubmit(
            const NetworkTxDescriptor& descriptor,
            Picoseconds now_ps) override {
        const NetworkSubmitResult result = port_.trySubmit(descriptor, now_ps);
        if (result.status == NetworkSubmitStatus::Accepted) {
            const auto& row = port_.issued().back();
            issued_.push_back(IssuedToken{
                row.token,
                row.wqe_id,
                row.accepted_at_ps,
                row.port_tx_at_ps,
                row.payload_bytes});
        }
        return result;
    }

    std::optional<Picoseconds> nextEventTime() const override {
        return port_.nextEventTime();
    }

    std::vector<NetworkEvent> takeDue(Picoseconds now_ps) override {
        std::vector<NetworkEvent> events = port_.takeDue(now_ps);
        for (const NetworkEvent& event : events) {
            if (event.scope == NetworkEventScope::FlowExtent) {
                terminals_.push_back(TerminalToken{
                    event.token,
                    event.wqe_id,
                    event.kind,
                    event.event_time_ps});
            } else if (event.scope == NetworkEventScope::PacketAttempt) {
                packet_events_.push_back(event);
            }
        }
        return events;
    }

    const std::vector<IssuedToken>& issued() const noexcept override {
        return issued_;
    }

    const std::vector<TerminalToken>& terminals() const noexcept override {
        return terminals_;
    }

    const std::vector<NetworkEvent>& packetEvents()
            const noexcept override {
        return packet_events_;
    }

    std::vector<NetworkToken> liveTokens() const override {
        return port_.liveTokens();
    }

private:
    htsim::simllm_rnic::HtsimNetworkPort port_;
    std::vector<IssuedToken> issued_;
    std::vector<TerminalToken> terminals_;
    std::vector<NetworkEvent> packet_events_;
};

class HtsimPortFactory final : public PortFactory {
public:
    const char* name() const noexcept override { return "htsim"; }

    std::unique_ptr<DrivenPort> create(const PortConfig& config) override {
        return std::make_unique<HtsimDrivenPort>(config);
    }
};

}  // namespace

std::unique_ptr<PortFactory> makePortFactory(const std::string& name) {
    if (name == "htsim") {
        return std::make_unique<HtsimPortFactory>();
    }
    throw std::invalid_argument(
        "htsim_rnic_tier_a requires --factory htsim");
}

}  // namespace simllm::rnic::tier_a
