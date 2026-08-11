// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "simllm_htsim_network_port.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "eventlist.h"

namespace htsim::simllm_rnic {
namespace {

using simllm::rnic::DropEvidenceProvenance;
using simllm::rnic::DropLocation;
using simllm::rnic::DropReason;
using simllm::rnic::NetworkEvent;
using simllm::rnic::NetworkEventKind;
using simllm::rnic::NetworkEventScope;
using simllm::rnic::NetworkLinkState;
using simllm::rnic::NetworkPacketKind;
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

NetworkPacketKind packetKind(AtlahsRuntimePacketKind kind) {
    switch (kind) {
    case AtlahsRuntimePacketKind::Data:
        return NetworkPacketKind::Data;
    case AtlahsRuntimePacketKind::Retransmission:
        return NetworkPacketKind::Retransmission;
    case AtlahsRuntimePacketKind::Ack:
        return NetworkPacketKind::Ack;
    case AtlahsRuntimePacketKind::Nak:
        return NetworkPacketKind::Nak;
    case AtlahsRuntimePacketKind::Cnp:
        return NetworkPacketKind::Cnp;
    case AtlahsRuntimePacketKind::Pfc:
        return NetworkPacketKind::Pfc;
    case AtlahsRuntimePacketKind::OtherControl:
        return NetworkPacketKind::OtherControl;
    }
    throw std::invalid_argument("unknown ATLAHS runtime packet kind");
}

DropLocation dropLocation(AtlahsRuntimeDropLocation location) {
    switch (location) {
    case AtlahsRuntimeDropLocation::None:
        return DropLocation::None;
    case AtlahsRuntimeDropLocation::TxPort:
        return DropLocation::TxPort;
    case AtlahsRuntimeDropLocation::Fabric:
        return DropLocation::Fabric;
    case AtlahsRuntimeDropLocation::RxPort:
        return DropLocation::RxPort;
    }
    throw std::invalid_argument("unknown ATLAHS runtime drop location");
}

DropReason dropReason(AtlahsRuntimeDropReason reason) {
    switch (reason) {
    case AtlahsRuntimeDropReason::None:
        return DropReason::None;
    case AtlahsRuntimeDropReason::Injected:
        return DropReason::Injected;
    case AtlahsRuntimeDropReason::QueueOverflow:
        return DropReason::QueueOverflow;
    case AtlahsRuntimeDropReason::LinkDown:
        return DropReason::LinkDown;
    case AtlahsRuntimeDropReason::PolicyRejected:
        return DropReason::PolicyRejected;
    }
    throw std::invalid_argument("unknown ATLAHS runtime drop reason");
}

DropEvidenceProvenance dropEvidence(AtlahsRuntimeDropEvidence evidence) {
    switch (evidence) {
    case AtlahsRuntimeDropEvidence::None:
        return DropEvidenceProvenance::None;
    case AtlahsRuntimeDropEvidence::Controlled:
        return DropEvidenceProvenance::Controlled;
    case AtlahsRuntimeDropEvidence::Asserted:
        return DropEvidenceProvenance::Asserted;
    case AtlahsRuntimeDropEvidence::Observed:
        return DropEvidenceProvenance::Observed;
    case AtlahsRuntimeDropEvidence::Inferred:
        return DropEvidenceProvenance::Inferred;
    }
    throw std::invalid_argument("unknown ATLAHS runtime drop evidence");
}

NetworkLinkState linkState(AtlahsRuntimeLinkState state) {
    switch (state) {
    case AtlahsRuntimeLinkState::Unknown:
        return NetworkLinkState::Unknown;
    case AtlahsRuntimeLinkState::Up:
        return NetworkLinkState::Up;
    case AtlahsRuntimeLinkState::Down:
        return NetworkLinkState::Down;
    }
    throw std::invalid_argument("unknown ATLAHS runtime link state");
}

NetworkEventKind eventKind(AtlahsRuntimeEventKind kind) {
    switch (kind) {
    case AtlahsRuntimeEventKind::PacketTxStarted:
        return NetworkEventKind::PacketTxStarted;
    case AtlahsRuntimeEventKind::PacketTxFinished:
        return NetworkEventKind::PacketTxFinished;
    case AtlahsRuntimeEventKind::PacketRxArrived:
        return NetworkEventKind::PacketRxArrived;
    case AtlahsRuntimeEventKind::PacketDelivered:
        return NetworkEventKind::Delivered;
    case AtlahsRuntimeEventKind::PacketDropped:
        return NetworkEventKind::Dropped;
    case AtlahsRuntimeEventKind::EcnMarked:
        return NetworkEventKind::EcnMarked;
    case AtlahsRuntimeEventKind::CnpReceived:
        return NetworkEventKind::CnpReceived;
    case AtlahsRuntimeEventKind::EligibilityUpdated:
        return NetworkEventKind::EligibilityUpdated;
    case AtlahsRuntimeEventKind::RateUpdated:
        return NetworkEventKind::RateUpdated;
    case AtlahsRuntimeEventKind::PfcFrameSubmitted:
        return NetworkEventKind::PfcFrameSubmitted;
    case AtlahsRuntimeEventKind::PfcPaused:
        return NetworkEventKind::PfcPaused;
    case AtlahsRuntimeEventKind::PfcResumed:
        return NetworkEventKind::PfcResumed;
    case AtlahsRuntimeEventKind::LinkStateChanged:
        return NetworkEventKind::LinkStateChanged;
    }
    throw std::invalid_argument("unknown ATLAHS runtime event kind");
}

bool isPacketEvent(AtlahsRuntimeEventKind kind) {
    return kind == AtlahsRuntimeEventKind::PacketTxStarted
           || kind == AtlahsRuntimeEventKind::PacketTxFinished
           || kind == AtlahsRuntimeEventKind::PacketRxArrived
           || kind == AtlahsRuntimeEventKind::PacketDelivered
           || kind == AtlahsRuntimeEventKind::PacketDropped;
}

bool isPacketTerminal(AtlahsRuntimeEventKind kind) {
    return kind == AtlahsRuntimeEventKind::PacketDelivered
           || kind == AtlahsRuntimeEventKind::PacketDropped;
}

}  // namespace

HtsimNetworkPort::HtsimNetworkPort(HtsimNetworkPortConfig config)
    : config_(std::move(config)) {
    validateConfig(config_);
}

simllm::rnic::NetworkPortCapabilities
HtsimNetworkPort::capabilities() const noexcept {
    simllm::rnic::NetworkPortCapabilities result;
    result.abi_version = config_.network_abi_version;
    if (config_.network_abi_version
        != simllm::rnic::kNetworkPortAbiVersionV2) {
        return result;
    }
    if (runtime_ == nullptr) {
        result.packet_attempt_events = true;
        result.ecn_cnp_events = config_.congestion;
        result.policy_update_events = config_.congestion;
        result.pfc_events = config_.control_frames;
        result.dynamic_link_events = config_.dynamic_link_events;
        return result;
    }
    const AtlahsRuntimeEventCapabilities native =
        runtime_->eventCapabilities();
    result.packet_attempt_events = native.packet_attempt_events;
    result.ecn_cnp_events = config_.congestion && native.ecn_cnp_events;
    result.policy_update_events =
        config_.congestion && native.policy_update_events;
    result.pfc_events = config_.control_frames && native.pfc_events;
    result.dynamic_link_events =
        config_.dynamic_link_events && native.dynamic_link_events;
    return result;
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
    const AtlahsRuntimeEventCapabilities native =
        runtime.eventCapabilities();
    if (config_.network_abi_version
            == simllm::rnic::kNetworkPortAbiVersionV2
        && (!native.packet_attempt_events
            || (config_.congestion
                && (!native.ecn_cnp_events
                    || !native.policy_update_events))
            || (config_.control_frames && !native.pfc_events)
            || (config_.dynamic_link_events
                && !native.dynamic_link_events))) {
        throw std::invalid_argument(
            "HTSIM runtime cannot satisfy the requested ABI-v2 vocabulary");
    }

    runtime_ = &runtime;
    runtime_capacity_ = runtime_capacity;
    terminal_ready_ = std::move(terminal_ready);
    try {
        if (config_.network_abi_version
            == simllm::rnic::kNetworkPortAbiVersionV2) {
            runtime_->setEventHandler(
                [this](const AtlahsRuntimeEvent& event) {
                    runtimeEvent(event);
                });
        }
        runtime_->setup(
            node_count,
            [this](AtlahsFlowId flow_id) { runtimeCompleted(flow_id); });
    } catch (...) {
        try {
            runtime_->setEventHandler({});
        } catch (...) {
        }
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
    if (config.network_abi_version
            != simllm::rnic::kNetworkPortAbiVersionV1
        && config.network_abi_version
            != simllm::rnic::kNetworkPortAbiVersionV2) {
        throw std::invalid_argument(
            "unsupported HTSIM NetworkPort ABI version");
    }
    if (config.capacity == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort capacity must be positive");
    }
    if (config.link_rate_bps == 0) {
        throw std::invalid_argument(
            "HTSIM NetworkPort link rate must be positive");
    }
    if (config.max_wire_packet_bytes == 0
        || config.data_header_bytes >= config.max_wire_packet_bytes) {
        throw std::invalid_argument(
            "HTSIM NetworkPort packet geometry is invalid");
    }
    if (config.network_abi_version
            == simllm::rnic::kNetworkPortAbiVersionV1
        && config.control_frames) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 cannot enable PFC or control frames");
    }
    if (config.network_abi_version
            == simllm::rnic::kNetworkPortAbiVersionV1
        && config.congestion) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 cannot enable ECN, CNP or rate feedback");
    }
    if (config.network_abi_version
            == simllm::rnic::kNetworkPortAbiVersionV1
        && config.dynamic_link_events) {
        throw std::invalid_argument(
            "NetworkPort ABI v1 cannot enable dynamic link events");
    }
}

void HtsimNetworkPort::validateDescriptor(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        Picoseconds now_ps) const {
    if (descriptor.abi_version != config_.network_abi_version) {
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
            "HTSIM NetworkPort accepts one whole-flow extent");
    }
    if (descriptor.traffic_class != config_.traffic_class) {
        throw std::invalid_argument(
            "HTSIM NetworkPort received an unsupported traffic class");
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

NetworkToken HtsimNetworkPort::allocateToken() {
    if (next_token_ == 0
        || next_token_ == std::numeric_limits<NetworkToken>::max()) {
        throw std::overflow_error(
            "HTSIM NetworkPort token space is exhausted");
    }
    return next_token_++;
}

void HtsimNetworkPort::scheduleEvent(const NetworkEvent& event) {
    if (next_event_sequence_ == 0
        || next_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "HTSIM NetworkPort event sequence is exhausted");
    }
    const ScheduledKey key{event.event_time_ps, next_event_sequence_++};
    if (!scheduled_.emplace(key, event).second) {
        throw std::logic_error("HTSIM NetworkPort event key collided");
    }
    if (terminal_ready_ && !submission_event_sequence_floor_.has_value()) {
        terminal_ready_(event.event_time_ps);
    }
}

void HtsimNetworkPort::scheduleUnboundV2Events(
        NetworkToken extent_token,
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        Picoseconds now_ps) {
    RnicDataPacketizationConfig packetization(
        config_.max_wire_packet_bytes, config_.data_header_bytes);
    RnicWireSerializationClock serializer(config_.link_rate_bps);
    if (now_ps != 0) {
        serializer.rebaseIdle(now_ps);
    }
    const bool emit_drop = config_.drop_first && !drop_emitted_;
    std::uint64_t payload_offset = 0;
    std::uint64_t packet_index = 0;
    Picoseconds terminal_at_ps = checkedAdd(
        now_ps,
        config_.propagation_delay_ps,
        "HTSIM NetworkPort terminal timestamp overflows");
    while (payload_offset < descriptor.payload_bytes) {
        const RnicPacketExtent extent = packetization.packetize(
            descriptor.payload_bytes - payload_offset);
        const RnicWireSerializationInterval interval = serializer.serialize(
            now_ps, extent.wireBytes());
        const Picoseconds rx_at_ps = checkedAdd(
            interval.end_ps,
            config_.propagation_delay_ps,
            "HTSIM NetworkPort packet RX timestamp overflows");
        const NetworkToken attempt_token = allocateToken();
        NetworkEvent event;
        event.abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
        event.scope = NetworkEventScope::PacketAttempt;
        event.token = attempt_token;
        event.parent_token = extent_token;
        event.wqe_id = descriptor.wqe_id;
        event.extent_index = descriptor.extent_index;
        event.packet_index = packet_index;
        event.transmission_attempt = 0;
        event.payload_offset_bytes = payload_offset;
        event.payload_bytes = extent.payloadBytes();
        event.wire_bytes = extent.wireBytes();
        event.packet_kind = NetworkPacketKind::Data;
        event.kind = NetworkEventKind::PacketTxStarted;
        event.event_time_ps = interval.start_ps;
        scheduleEvent(event);
        event.kind = NetworkEventKind::PacketTxFinished;
        event.event_time_ps = interval.end_ps;
        scheduleEvent(event);

        const bool final_packet = extent.payloadBytes()
            == descriptor.payload_bytes - payload_offset;
        if (!emit_drop || !final_packet) {
            event.kind = NetworkEventKind::PacketRxArrived;
            event.event_time_ps = rx_at_ps;
            scheduleEvent(event);
            event.kind = NetworkEventKind::Delivered;
            scheduleEvent(event);
        } else {
            event.kind = NetworkEventKind::Dropped;
            event.event_time_ps = rx_at_ps;
            event.drop_location = DropLocation::Fabric;
            event.drop_reason = DropReason::Injected;
            event.drop_resource_id = 1;
            event.drop_evidence = DropEvidenceProvenance::Controlled;
            scheduleEvent(event);
        }
        terminal_at_ps = rx_at_ps;
        payload_offset = checkedAdd(
            payload_offset,
            extent.payloadBytes(),
            "HTSIM NetworkPort packet payload ledger overflows");
        ++packet_index;
    }

    NetworkEvent terminal;
    terminal.abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    terminal.scope = NetworkEventScope::FlowExtent;
    terminal.token = extent_token;
    terminal.wqe_id = descriptor.wqe_id;
    terminal.extent_index = descriptor.extent_index;
    terminal.event_time_ps = terminal_at_ps;
    if (emit_drop) {
        terminal.kind = NetworkEventKind::Dropped;
        terminal.drop_location = DropLocation::Fabric;
        terminal.drop_reason = DropReason::Injected;
        terminal.drop_resource_id = 1;
        terminal.drop_evidence = DropEvidenceProvenance::Controlled;
    } else {
        terminal.kind = NetworkEventKind::Delivered;
    }
    scheduleEvent(terminal);
    LiveToken& live = live_.at(extent_token);
    live.terminal_at_ps = terminal_at_ps;
    live.terminal_queued = true;
    if (emit_drop) {
        drop_emitted_ = true;
    }
}

NetworkSubmitResult HtsimNetworkPort::trySubmit(
        const simllm::rnic::NetworkTxDescriptor& descriptor,
        Picoseconds now_ps) {
    validateDescriptor(descriptor, now_ps);
    if (submission_event_sequence_floor_.has_value()) {
        throw std::logic_error(
            "HTSIM NetworkPort does not support nested submissions");
    }
    if (runtime_ == nullptr
        && (config_.control_frames || config_.congestion
            || config_.dynamic_link_events)) {
        throw std::invalid_argument(
            "HTSIM control events require a bound runtime producer");
    }
    if (live_.size() >= effectiveCapacity()) {
        if (runtime_ != nullptr) {
            throw std::logic_error(
                "runtime-bound HTSIM port exceeded native session capacity");
        }
        std::optional<Picoseconds> retry_at_ps;
        for (const auto& item : live_) {
            if (item.second.terminal_queued
                && (!retry_at_ps.has_value()
                    || item.second.terminal_at_ps < *retry_at_ps)) {
                retry_at_ps = item.second.terminal_at_ps;
            }
        }
        if (!retry_at_ps.has_value()) {
            throw std::logic_error(
                "HTSIM NetworkPort has no future capacity event");
        }
        if (*retry_at_ps <= now_ps) {
            throw std::logic_error(
                "HTSIM NetworkPort due terminals must be drained before retry");
        }
        return NetworkSubmitResult::busy(*retry_at_ps);
    }

    const NetworkToken first_candidate_token = next_token_;
    const std::uint64_t first_candidate_sequence = next_event_sequence_;
    const bool drop_was_emitted = drop_emitted_;
    const NetworkToken token = allocateToken();
    const Picoseconds terminal_at_ps =
        runtime_ == nullptr
            && config_.network_abi_version
                == simllm::rnic::kNetworkPortAbiVersionV1
        ? terminalTime(descriptor.payload_bytes, now_ps)
        : 0;
    const LiveToken live{
        descriptor,
        now_ps,
        now_ps,
        terminal_at_ps,
        runtime_ == nullptr
            && config_.network_abi_version
                == simllm::rnic::kNetworkPortAbiVersionV1};

    const auto live_inserted = live_.emplace(token, live);
    if (!live_inserted.second) {
        throw std::logic_error("HTSIM NetworkPort recycled a live token");
    }
    submission_event_sequence_floor_ = first_candidate_sequence;
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
        if (runtime_ == nullptr
            && config_.network_abi_version
                == simllm::rnic::kNetworkPortAbiVersionV1) {
            NetworkEvent event;
            event.token = token;
            event.wqe_id = descriptor.wqe_id;
            event.event_time_ps = terminal_at_ps;
            scheduleEvent(event);
        } else if (runtime_ == nullptr) {
            scheduleUnboundV2Events(token, descriptor, now_ps);
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
        rollbackSubmission(
            token, descriptor.flow_id, first_candidate_sequence);
        next_token_ = first_candidate_token;
        next_event_sequence_ = first_candidate_sequence;
        drop_emitted_ = drop_was_emitted;
        submission_event_sequence_floor_.reset();
        throw;
    }

    submission_event_sequence_floor_.reset();
    if (terminal_ready_) {
        for (const auto& item : scheduled_) {
            if (item.first.second >= first_candidate_sequence) {
                terminal_ready_(item.second.event_time_ps);
            }
        }
    }

    return NetworkSubmitResult::accepted(token);
}

void HtsimNetworkPort::runtimeEvent(const AtlahsRuntimeEvent& native) {
    if (runtime_ == nullptr
        || config_.network_abi_version
            != simllm::rnic::kNetworkPortAbiVersionV2) {
        throw std::logic_error(
            "HTSIM NetworkPort received an event outside ABI v2");
    }
    const simllm::rnic::NetworkPortCapabilities advertised = capabilities();
    const bool packet_event = isPacketEvent(native.kind);
    const bool correlated_control =
        native.kind == AtlahsRuntimeEventKind::EcnMarked
        || native.kind == AtlahsRuntimeEventKind::CnpReceived
        || native.kind == AtlahsRuntimeEventKind::EligibilityUpdated
        || native.kind == AtlahsRuntimeEventKind::RateUpdated;
    const auto extent = token_by_flow_.find(native.flow_id);
    if ((packet_event || correlated_control)
        && extent == token_by_flow_.end()) {
        throw std::invalid_argument(
            "HTSIM runtime event references an unknown flow");
    }

    NetworkEvent event;
    event.abi_version = simllm::rnic::kNetworkPortAbiVersionV2;
    event.kind = eventKind(native.kind);
    event.event_time_ps = native.event_time_ps;
    event.extent_index = native.extent_index;
    event.packet_index = native.packet_index;
    event.transmission_attempt = native.transmission_attempt;
    event.payload_offset_bytes = native.payload_offset_bytes;
    event.payload_bytes = native.payload_bytes;
    event.wire_bytes = native.wire_bytes;
    event.packet_kind = packetKind(native.packet_kind);
    event.ecn_marked = native.ecn_marked;
    event.drop_location = dropLocation(native.drop_location);
    event.drop_reason = dropReason(native.drop_reason);
    event.drop_resource_id = native.drop_resource_id;
    event.drop_evidence = dropEvidence(native.drop_evidence);
    event.policy_context_token = native.policy_context_token;
    event.source = native.source;
    event.destination = native.destination;
    event.link_id = native.link_id;
    event.priority = native.priority;
    event.pause_quanta = native.pause_quanta;
    event.has_pause_duration = native.has_pause_duration;
    event.pause_duration_ps = native.pause_duration_ps;
    event.effective_at_ps = native.effective_at_ps;
    event.has_effective_rate = native.has_effective_rate;
    event.effective_rate_bps = native.effective_rate_bps;
    event.link_state = linkState(native.link_state);

    const RuntimePacketKey packet_key{
        native.flow_id, native.packet_index, native.transmission_attempt};
    if (packet_event) {
        if (!advertised.packet_attempt_events) {
            throw std::logic_error(
                "HTSIM runtime emitted an unadvertised packet event");
        }
        event.scope = NetworkEventScope::PacketAttempt;
        event.parent_token = extent->second;
        event.wqe_id = live_.at(extent->second).descriptor.wqe_id;
        if (native.kind == AtlahsRuntimeEventKind::PacketTxStarted) {
            if (completed_runtime_packet_tokens_.count(packet_key) != 0) {
                throw std::logic_error(
                    "HTSIM runtime reused a completed packet identity");
            }
            const NetworkToken attempt_token = allocateToken();
            if (!runtime_packet_tokens_.emplace(
                    packet_key, attempt_token).second) {
                throw std::logic_error(
                    "HTSIM runtime reused a packet-attempt identity");
            }
            event.token = attempt_token;
        } else {
            const auto attempt = runtime_packet_tokens_.find(packet_key);
            if (attempt == runtime_packet_tokens_.end()) {
                throw std::logic_error(
                    "HTSIM runtime packet event predates TX issue");
            }
            event.token = attempt->second;
        }
    } else {
        event.scope = NetworkEventScope::TransportControl;
        if (correlated_control) {
            event.parent_token = extent->second;
            event.wqe_id = live_.at(extent->second).descriptor.wqe_id;
        }
        switch (native.kind) {
        case AtlahsRuntimeEventKind::EcnMarked:
        case AtlahsRuntimeEventKind::CnpReceived: {
            if (!advertised.ecn_cnp_events) {
                throw std::logic_error(
                    "HTSIM runtime emitted unadvertised ECN or CNP");
            }
            const auto attempt = runtime_packet_tokens_.find(packet_key);
            const auto completed =
                completed_runtime_packet_tokens_.find(packet_key);
            if (attempt == runtime_packet_tokens_.end()
                && completed == completed_runtime_packet_tokens_.end()) {
                throw std::logic_error(
                    "HTSIM ECN or CNP event lacks a packet attempt");
            }
            event.token = attempt != runtime_packet_tokens_.end()
                ? attempt->second
                : completed->second;
            break;
        }
        case AtlahsRuntimeEventKind::EligibilityUpdated:
        case AtlahsRuntimeEventKind::RateUpdated:
            if (!advertised.policy_update_events) {
                throw std::logic_error(
                    "HTSIM runtime emitted an unadvertised policy update");
            }
            break;
        case AtlahsRuntimeEventKind::PfcFrameSubmitted:
        case AtlahsRuntimeEventKind::PfcPaused:
        case AtlahsRuntimeEventKind::PfcResumed:
            if (!advertised.pfc_events) {
                throw std::logic_error(
                    "HTSIM runtime emitted an unadvertised PFC event");
            }
            break;
        case AtlahsRuntimeEventKind::LinkStateChanged:
            if (!advertised.dynamic_link_events) {
                throw std::logic_error(
                    "HTSIM runtime emitted an unadvertised link event");
            }
            break;
        default:
            throw std::logic_error(
                "HTSIM runtime event scope classification failed");
        }
    }
    scheduleEvent(event);
    if (isPacketTerminal(native.kind)) {
        auto completed = runtime_packet_tokens_.extract(packet_key);
        if (completed.empty()) {
            throw std::logic_error(
                "HTSIM runtime packet terminal lost its live correlation");
        }
        const auto inserted = completed_runtime_packet_tokens_.insert(
            std::move(completed));
        if (!inserted.inserted) {
            throw std::logic_error(
                "HTSIM runtime repeated a completed packet identity");
        }
    }
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
    for (const auto& item : runtime_packet_tokens_) {
        if (std::get<0>(item.first) == flow_id) {
            throw std::logic_error(
                "HTSIM runtime completed with a live packet attempt");
        }
    }

    NetworkEvent event;
    event.abi_version = config_.network_abi_version;
    event.kind = NetworkEventKind::Delivered;
    event.scope = NetworkEventScope::FlowExtent;
    event.token = token->second;
    event.wqe_id = live->second.descriptor.wqe_id;
    event.extent_index = live->second.descriptor.extent_index;
    event.event_time_ps = now_ps;
    scheduleEvent(event);
    live->second.terminal_at_ps = now_ps;
    live->second.terminal_queued = true;
}

void HtsimNetworkPort::validateTerminal(
        const NetworkEvent& event,
        const LiveToken& live) const {
    if (event.abi_version != config_.network_abi_version
        || event.scope != NetworkEventScope::FlowExtent) {
        throw std::invalid_argument(
            "HTSIM terminal carries an unsupported ABI or scope");
    }
    if (event.token == 0 || event.wqe_id != live.descriptor.wqe_id
        || event.extent_index != live.descriptor.extent_index) {
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
            || event.drop_reason != DropReason::None
            || event.drop_evidence != DropEvidenceProvenance::None) {
            throw std::invalid_argument(
                "HTSIM delivery carries contradictory drop evidence");
        }
        return;
    }
    if (event.kind != NetworkEventKind::Dropped
        || event.drop_location == DropLocation::None
        || event.drop_reason == DropReason::None
        || (event.abi_version == simllm::rnic::kNetworkPortAbiVersionV2
            && event.drop_evidence == DropEvidenceProvenance::None)) {
        throw std::invalid_argument(
            "HTSIM drop omitted typed terminal evidence");
    }
}

void HtsimNetworkPort::rollbackSubmission(
        NetworkToken token,
        simllm::rnic::FlowId flow_id,
        std::uint64_t first_event_sequence) noexcept {
    for (auto scheduled = scheduled_.begin();
         scheduled != scheduled_.end();) {
        if (scheduled->first.second >= first_event_sequence) {
            scheduled = scheduled_.erase(scheduled);
        } else {
            ++scheduled;
        }
    }
    for (auto attempt = runtime_packet_tokens_.begin();
         attempt != runtime_packet_tokens_.end();) {
        if (std::get<0>(attempt->first) == flow_id) {
            attempt = runtime_packet_tokens_.erase(attempt);
        } else {
            ++attempt;
        }
    }
    purgeCompletedPacketTokens(flow_id);
    if (!issued_.empty() && issued_.back().token == token) {
        issued_.pop_back();
    }
    token_sources_.erase(token);
    token_by_flow_.erase(flow_id);
    seen_flows_.erase(flow_id);
    live_.erase(token);
}

void HtsimNetworkPort::purgeCompletedPacketTokens(
        simllm::rnic::FlowId flow_id) noexcept {
    for (auto attempt = completed_runtime_packet_tokens_.begin();
         attempt != completed_runtime_packet_tokens_.end();) {
        if (std::get<0>(attempt->first) == flow_id) {
            attempt = completed_runtime_packet_tokens_.erase(attempt);
        } else {
            ++attempt;
        }
    }
}

std::optional<Picoseconds> HtsimNetworkPort::nextEventTime() const {
    if (scheduled_.empty()) {
        return std::nullopt;
    }
    return scheduled_.begin()->first.first;
}

std::vector<NetworkEvent> HtsimNetworkPort::takeDue(Picoseconds now_ps) {
    std::vector<NetworkEvent> events;
    while (!scheduled_.empty()
           && scheduled_.begin()->first.first <= now_ps) {
        const auto scheduled = scheduled_.begin();
        NetworkEvent event = scheduled->second;
        if (event.scope == NetworkEventScope::FlowExtent) {
            const auto live = live_.find(event.token);
            if (live == live_.end()) {
                throw std::logic_error(
                    "HTSIM NetworkPort scheduled a token that is not live");
            }
            const bool consume_v1_drop = runtime_ == nullptr
                && config_.network_abi_version
                    == simllm::rnic::kNetworkPortAbiVersionV1
                && config_.drop_first && !drop_emitted_;
            if (consume_v1_drop) {
                event.kind = NetworkEventKind::Dropped;
                event.drop_location = DropLocation::Fabric;
                event.drop_reason = DropReason::Injected;
            }
            validateTerminal(event, live->second);
            terminals_.push_back(HtsimTerminalToken{
                event.token,
                event.wqe_id,
                live->second.descriptor.flow_id,
                event.kind,
                event.event_time_ps,
                event.ecn_marked,
                event.drop_location,
                event.drop_reason});
            if (consume_v1_drop) {
                drop_emitted_ = true;
            }
            purgeCompletedPacketTokens(
                live->second.descriptor.flow_id);
            live_.erase(live);
        } else if (event.scope == NetworkEventScope::PacketAttempt) {
            packet_events_.push_back(event);
        } else {
            control_events_.push_back(event);
        }
        scheduled_.erase(scheduled);
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
