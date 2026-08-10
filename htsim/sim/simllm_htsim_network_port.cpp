// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "simllm_htsim_network_port.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include "eventlist.h"

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

void HtsimNetworkPort::bindRuntime(
        AtlahsFlowRuntime& runtime,
        std::uint32_t node_count,
        std::size_t runtime_capacity,
        TerminalReadyHandler terminal_ready) {
    if (runtime_ != nullptr || !live_.empty() || !issued_.empty()) {
        throw std::logic_error(
            "HTSIM NetworkPort runtime binding is immutable");
    }
    if (node_count == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort runtime requires at least one endpoint");
    }
    if (runtime_capacity == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort runtime capacity must be positive");
    }
    if (config_.drop_first) {
        throw std::invalid_argument(
            "runtime-bound HTSIM drops must originate in the runtime");
    }
    if (!terminal_ready) {
        throw std::invalid_argument(
            "HTSIM NetworkPort runtime requires a terminal-ready handler");
    }

    runtime_ = &runtime;
    runtime_capacity_ = runtime_capacity;
    terminal_ready_ = std::move(terminal_ready);
    try {
        runtime_->setup(
            node_count,
            [this](AtlahsFlowId flow_id) { runtimeCompleted(flow_id); });
    } catch (...) {
        runtime_ = nullptr;
        runtime_capacity_.reset();
        terminal_ready_ = nullptr;
        throw;
    }
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
    if (live_.size() >= effectiveCapacity()) {
        if (runtime_ != nullptr) {
            throw std::logic_error(
                "runtime-bound HTSIM port exceeded native session capacity");
        }
        if (scheduled_.empty()) {
            throw std::logic_error(
                "HTSIM NetworkPort has no future capacity event");
        }
        if (scheduled_.begin()->first <= now_ps) {
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
    const Picoseconds terminal_at_ps = runtime_ == nullptr
        ? terminalTime(descriptor.payload_bytes, now_ps)
        : 0;
    const LiveToken live{
        descriptor,
        now_ps,
        now_ps,
        terminal_at_ps,
        runtime_ == nullptr};

    const auto live_inserted = live_.emplace(token, live);
    if (!live_inserted.second) {
        throw std::logic_error("HTSIM NetworkPort recycled a live token");
    }
    try {
        if (!seen_flows_.insert(descriptor.flow_id).second
            || !token_by_flow_.emplace(descriptor.flow_id, token).second
            || !token_sources_.emplace(token, descriptor.source).second) {
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
        if (runtime_ == nullptr) {
            scheduled_.emplace(terminal_at_ps, token);
        } else {
            AtlahsFlowRequest request;
            request.flow_id = descriptor.flow_id;
            request.source = descriptor.source;
            request.destination = descriptor.destination;
            request.payload_bytes = descriptor.payload_bytes;
            request.start_time_ps = now_ps;
            request.tag = descriptor.flow_tag;
            runtime_->send(request);
        }
    } catch (...) {
        rollbackSubmission(token, descriptor.flow_id);
        throw;
    }

    ++next_token_;
    return NetworkSubmitResult::accepted(token);
}

void HtsimNetworkPort::runtimeCompleted(AtlahsFlowId flow_id) {
    if (runtime_ == nullptr) {
        throw std::logic_error(
            "HTSIM NetworkPort received a completion without a runtime");
    }
    const auto token = token_by_flow_.find(flow_id);
    if (token == token_by_flow_.end()) {
        throw std::invalid_argument(
            "HTSIM runtime completed an unknown flow token");
    }
    const auto live = live_.find(token->second);
    if (live == live_.end() || live->second.terminal_queued) {
        throw std::invalid_argument(
            "HTSIM runtime completed a flow token more than once");
    }
    const Picoseconds now_ps = EventList::now();
    if (now_ps < live->second.accepted_at_ps) {
        throw std::logic_error(
            "HTSIM runtime completed a flow before admission");
    }

    scheduled_.emplace(now_ps, token->second);
    live->second.terminal_at_ps = now_ps;
    live->second.terminal_queued = true;
    terminal_ready_(now_ps);
}

void HtsimNetworkPort::validateTerminal(
        const NetworkEvent& event,
        const LiveToken& live) const {
    if (event.abi_version != simllm::rnic::kNetworkPortAbiVersion) {
        throw std::invalid_argument(
            "HTSIM terminal carries an unsupported ABI version");
    }
    if (event.token == 0 || event.wqe_id != live.descriptor.wqe_id) {
        throw std::invalid_argument(
            "HTSIM terminal changed native WQE correlation");
    }
    if (!live.terminal_queued
        || event.event_time_ps != live.terminal_at_ps
        || event.event_time_ps < live.accepted_at_ps) {
        throw std::invalid_argument(
            "HTSIM terminal changed its committed time or lifecycle");
    }
    if (event.kind == NetworkEventKind::Delivered) {
        if (event.drop_location != DropLocation::None
            || event.drop_reason != DropReason::None) {
            throw std::invalid_argument(
                "HTSIM delivery carries contradictory drop evidence");
        }
        return;
    }
    if (event.kind != NetworkEventKind::Dropped
        || event.drop_location == DropLocation::None
        || event.drop_reason == DropReason::None) {
        throw std::invalid_argument(
            "HTSIM drop omitted typed terminal evidence");
    }
}

void HtsimNetworkPort::rollbackSubmission(
        NetworkToken token, simllm::rnic::FlowId flow_id) noexcept {
    for (auto scheduled = scheduled_.begin();
         scheduled != scheduled_.end();) {
        if (scheduled->second == token) {
            scheduled = scheduled_.erase(scheduled);
        } else {
            ++scheduled;
        }
    }
    if (!issued_.empty() && issued_.back().token == token) {
        issued_.pop_back();
    }
    token_sources_.erase(token);
    token_by_flow_.erase(flow_id);
    seen_flows_.erase(flow_id);
    live_.erase(token);
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
        const auto scheduled = scheduled_.begin();
        const Picoseconds terminal_at_ps = scheduled->first;
        const NetworkToken token = scheduled->second;

        const auto live = live_.find(token);
        if (live == live_.end()) {
            throw std::logic_error(
                "HTSIM NetworkPort scheduled a token that is not live");
        }
        NetworkEvent event;
        event.token = token;
        event.wqe_id = live->second.descriptor.wqe_id;
        event.event_time_ps = terminal_at_ps;
        const bool emit_drop = runtime_ == nullptr
                               && config_.drop_first
                               && !drop_emitted_;
        if (emit_drop) {
            event.kind = NetworkEventKind::Dropped;
            event.drop_location = DropLocation::Fabric;
            event.drop_reason = DropReason::Injected;
        } else {
            event.kind = NetworkEventKind::Delivered;
            event.drop_location = DropLocation::None;
            event.drop_reason = DropReason::None;
        }
        validateTerminal(event, live->second);
        terminals_.push_back(HtsimTerminalToken{
            token,
            event.wqe_id,
            live->second.descriptor.flow_id,
            event.kind,
            terminal_at_ps,
            event.ecn_marked,
            event.drop_location,
            event.drop_reason});
        if (emit_drop) {
            drop_emitted_ = true;
        }
        scheduled_.erase(scheduled);
        live_.erase(live);
        events.push_back(event);
    }
    return events;
}

bool HtsimNetworkPort::hasPendingPhysicalWork() const noexcept {
    return !live_.empty() || !scheduled_.empty()
           || (runtime_ != nullptr && runtime_->hasPendingPhysicalWork());
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
