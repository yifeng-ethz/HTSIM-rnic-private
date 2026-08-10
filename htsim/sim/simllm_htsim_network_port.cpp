// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "simllm_htsim_network_port.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace htsim::simllm_rnic {
namespace {

using simllm::rnic::DropLocation;
using simllm::rnic::DropReason;
using simllm::rnic::NetworkEvent;
using simllm::rnic::NetworkEventKind;
using simllm::rnic::NetworkSubmitResult;
using simllm::rnic::NetworkToken;
using simllm::rnic::Picoseconds;

Picoseconds checkedAdd(
        Picoseconds lhs, Picoseconds rhs, const char* message) {
    if (rhs > std::numeric_limits<Picoseconds>::max() - lhs) {
        throw std::overflow_error(message);
    }
    return lhs + rhs;
}

}  // namespace

HtsimNetworkPort::HtsimNetworkPort(HtsimNetworkPortConfig config)
    : config_(std::move(config)) {
    validateConfig(config_);
}

void HtsimNetworkPort::validateConfig(
        const HtsimNetworkPortConfig& config) {
    if (config.version != kHtsimNetworkPortConfigVersion) {
        throw std::invalid_argument(
            "unsupported HTSIM NetworkPort configuration version");
    }
    if (config.capacity == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort capacity must be positive");
    }
    if (config.link_rate_bps == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort link rate must be positive");
    }
    if (config.control_frames) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 cannot enable PFC or control frames");
    }
    if (config.congestion) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 cannot enable ECN, CNP or rate feedback");
    }
    if (config.dynamic_link_events) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 cannot enable dynamic link events");
    }
}

void HtsimNetworkPort::validateDescriptor(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        Picoseconds now_ps) const {
    if (descriptor.abi_version != simllm::rnic::kNetworkPortAbiVersion) {
        throw std::invalid_argument(
            "HTSIM NetworkPort received an unsupported ABI version");
    }
    if (descriptor.wqe_id == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort requires a nonzero WQE correlation ID");
    }
    if (descriptor.policy_context_token == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort requires a nonzero policy-context token");
    }
    if (descriptor.extent_index != 0 || descriptor.extent_count != 1) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 accepts one whole-flow extent");
    }
    if (descriptor.traffic_class != config_.traffic_class) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 received an unsupported traffic class");
    }
    if (descriptor.source == descriptor.destination) {
        throw std::invalid_argument(
            "HTSIM NetworkPort requires distinct endpoints");
    }
    if (config_.endpoint_count != 0
        && (descriptor.source >= config_.endpoint_count
            || descriptor.destination >= config_.endpoint_count)) {
        throw std::out_of_range(
            "HTSIM NetworkPort endpoint is outside the configured range");
    }
    if (now_ps < descriptor.eligible_at_ps) {
        throw std::invalid_argument(
            "HTSIM NetworkPort submission precedes native eligibility");
    }
    if (descriptor.payload_bytes
        > std::numeric_limits<std::uint64_t>::max()
              - config_.data_header_bytes) {
        throw std::overflow_error(
            "HTSIM NetworkPort wire extent overflows");
    }
    if (seen_flows_.count(descriptor.flow_id) != 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort flow identity was reused in one session");
    }
}

Picoseconds HtsimNetworkPort::terminalTime(
        std::uint64_t payload_bytes, Picoseconds now_ps) const {
    const std::uint64_t wire_bytes = checkedAdd(
        payload_bytes,
        config_.data_header_bytes,
        "HTSIM NetworkPort wire extent overflows");
    Picoseconds serialization_end_ps = now_ps;
    if (wire_bytes != 0) {
        RnicWireSerializationClock serializer(config_.link_rate_bps);
        if (now_ps != 0) {
            serializer.rebaseIdle(now_ps);
        }
        serialization_end_ps = serializer.serialize(
            now_ps, wire_bytes).end_ps;
    }
    return checkedAdd(
        serialization_end_ps,
        config_.propagation_delay_ps,
        "HTSIM NetworkPort terminal timestamp overflows");
}

NetworkSubmitResult HtsimNetworkPort::trySubmit(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        Picoseconds now_ps) {
    validateDescriptor(descriptor, now_ps);
    if (live_.size() >= config_.capacity) {
        if (scheduled_.empty() || scheduled_.begin()->first <= now_ps) {
            throw std::logic_error(
                "HTSIM NetworkPort due terminals must be drained before retry");
        }
        return NetworkSubmitResult::busy(scheduled_.begin()->first);
    }
    if (next_token_ == 0
        || next_token_ == std::numeric_limits<NetworkToken>::max()) {
        throw std::overflow_error(
            "HTSIM NetworkPort token space is exhausted");
    }

    const NetworkToken token = next_token_;
    const Picoseconds terminal_at_ps = terminalTime(
        descriptor.payload_bytes, now_ps);
    const LiveToken live{
        descriptor,
        now_ps,
        now_ps,
        terminal_at_ps};

    const auto live_inserted = live_.emplace(token, live);
    if (!live_inserted.second) {
        throw std::logic_error("HTSIM NetworkPort recycled a live token");
    }
    auto scheduled = scheduled_.end();
    bool flow_inserted = false;
    bool source_inserted = false;
    try {
        scheduled = scheduled_.emplace(terminal_at_ps, token);
        flow_inserted = seen_flows_.insert(descriptor.flow_id).second;
        source_inserted = token_sources_.emplace(
            token, descriptor.source).second;
        if (!flow_inserted || !source_inserted) {
            throw std::logic_error(
                "HTSIM NetworkPort correlation insertion failed");
        }
        issued_.push_back(HtsimIssuedToken{
            token,
            descriptor.wqe_id,
            descriptor.wr_id,
            descriptor.flow_id,
            descriptor.policy_context_token,
            descriptor.source,
            descriptor.destination,
            now_ps,
            now_ps,
            descriptor.payload_bytes});
    } catch (...) {
        if (scheduled != scheduled_.end()) {
            scheduled_.erase(scheduled);
        }
        if (flow_inserted) {
            seen_flows_.erase(descriptor.flow_id);
        }
        if (source_inserted) {
            token_sources_.erase(token);
        }
        live_.erase(token);
        throw;
    }

    ++next_token_;
    return NetworkSubmitResult::accepted(token);
}

std::optional<Picoseconds> HtsimNetworkPort::nextEventTime() const {
    if (scheduled_.empty()) {
        return std::nullopt;
    }
    return scheduled_.begin()->first;
}

std::vector<NetworkEvent> HtsimNetworkPort::takeDue(Picoseconds now_ps) {
    std::vector<NetworkEvent> events;
    while (!scheduled_.empty() && scheduled_.begin()->first <= now_ps) {
        const Picoseconds terminal_at_ps = scheduled_.begin()->first;
        const NetworkToken token = scheduled_.begin()->second;
        scheduled_.erase(scheduled_.begin());

        const auto live = live_.find(token);
        if (live == live_.end()) {
            throw std::logic_error(
                "HTSIM NetworkPort scheduled a token that is not live");
        }
        NetworkEvent event;
        event.token = token;
        event.wqe_id = live->second.descriptor.wqe_id;
        event.event_time_ps = terminal_at_ps;
        if (config_.drop_first && !drop_emitted_) {
            event.kind = NetworkEventKind::Dropped;
            event.drop_location = DropLocation::Fabric;
            event.drop_reason = DropReason::Injected;
            drop_emitted_ = true;
        } else {
            event.kind = NetworkEventKind::Delivered;
            event.drop_location = DropLocation::None;
            event.drop_reason = DropReason::None;
        }
        terminals_.push_back(HtsimTerminalToken{
            token,
            event.wqe_id,
            live->second.descriptor.flow_id,
            event.kind,
            terminal_at_ps,
            event.ecn_marked,
            event.drop_location,
            event.drop_reason});
        live_.erase(live);
        events.push_back(event);
    }
    return events;
}

bool HtsimNetworkPort::hasPendingPhysicalWork() const noexcept {
    return !live_.empty() || !scheduled_.empty();
}

std::vector<NetworkToken> HtsimNetworkPort::liveTokens() const {
    std::vector<NetworkToken> tokens;
    tokens.reserve(live_.size());
    for (const auto& item : live_) {
        tokens.push_back(item.first);
    }
    return tokens;
}

std::uint32_t HtsimNetworkPort::ownerSource(NetworkToken token) const {
    const auto source = token_sources_.find(token);
    if (source == token_sources_.end()) {
        throw std::out_of_range(
            "HTSIM NetworkPort token has no source owner");
    }
    return source->second;
}

}  // namespace htsim::simllm_rnic
