// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "simllm_atlahs_flow_runtime.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace htsim::simllm_rnic {
namespace {

using simllm::rnic::CompletionEntry;
using simllm::rnic::CompletionStatus;
using simllm::rnic::NetworkEvent;
using simllm::rnic::Picoseconds;
using simllm::rnic::PostStatus;
using simllm::rnic::RnicDevice;
using simllm::rnic::RnicDeviceAttachments;
using simllm::rnic::RnicDeviceConfig;
using simllm::rnic::RnicHardwareMode;
using simllm::rnic::WorkRequest;
using simllm::rnic::WqeId;

bool blank(const std::string& value) {
    return value.empty()
           || std::all_of(
               value.begin(), value.end(), [](unsigned char character) {
                   return character == ' ' || character == '\t'
                          || character == '\n' || character == '\r';
               });
}

void validateTransportPolicy(
        const std::string& policy,
        const AtlahsFlowRuntime& runtime) {
    const AtlahsTransportKind transport = runtime.transportKind();
    if ((policy == "rnic-nn" && transport != AtlahsTransportKind::None)
        || (policy == "rnic-cn"
            && transport != AtlahsTransportKind::RnicCnLinkPair)
        || (policy == "dcqcn"
            && transport != AtlahsTransportKind::DcqcnQueuePair)) {
        throw std::invalid_argument(
            "SimLLM ATLAHS policy disagrees with the htsim runtime");
    }
}

}  // namespace

RnicDeviceConfig defaultSimllmAtlahsDeviceConfig() {
    RnicDeviceConfig config;
    config.identity.qpn = 17;
    config.identity.policy_context_token = 9001;
    config.work_queue.sq_id = 1;
    config.work_queue.cq_id = 1;
    config.work_queue.source = 0;
    config.work_queue.qpn = config.identity.qpn;
    config.work_queue.policy_context_token =
        config.identity.policy_context_token;
    config.work_queue.sq_depth = 64;
    config.work_queue.cq_depth = 64;
    config.work_queue.doorbell_service_ps = 0;
    config.work_queue.wqe_fetch_service_ps = 0;
    config.work_queue.qpc_lookup_service_ps = 0;
    config.work_queue.scheduler_service_ps = 0;
    config.work_queue.cqe_write_service_ps = 0;
    config.qpc.enabled = false;
    config.dma.enabled = false;
    config.network.enabled = true;
    return config;
}

void SimllmAtlahsFlowRuntime::validateConfig(
        const SimllmAtlahsRuntimeConfig& config) {
    if (config.version != kSimllmAtlahsRuntimeConfigVersion) {
        throw std::invalid_argument(
            "unsupported SimLLM ATLAHS runtime configuration version");
    }
    if (blank(config.session_id) || blank(config.transport_policy)
        || blank(config.topology_identity)
        || blank(config.htsim_source_revision)
        || blank(config.simllm_source_revision)) {
        throw std::invalid_argument(
            "SimLLM ATLAHS run identity fields must be nonblank");
    }
    if (config.transport_policy != "rnic-nn"
        && config.transport_policy != "rnic-cn"
        && config.transport_policy != "dcqcn") {
        throw std::invalid_argument(
            "SimLLM ATLAHS transport policy must be rnic-nn, rnic-cn or dcqcn");
    }
    if (!config.device.network.enabled) {
        throw std::invalid_argument(
            "SimLLM ATLAHS structural mode requires the external network port");
    }
    if (config.device.dma.enabled) {
        throw std::invalid_argument(
            "SimLLM ATLAHS wrapper does not own PCIe or DMA state");
    }
    if (config.device.work_queue.qpn != config.device.identity.qpn
        || config.device.work_queue.policy_context_token
               != config.device.identity.policy_context_token) {
        throw std::invalid_argument(
            "SimLLM ATLAHS device identity projections disagree");
    }
    if (config.authority.native_session_enabled
        && config.authority.legacy_ledger_enabled) {
        throw std::invalid_argument(
            "structural and bypass authorities are mutually exclusive");
    }
    if (!config.authority.native_session_enabled
        && !config.authority.legacy_ledger_enabled) {
        throw std::invalid_argument(
            "one composition authority is required");
    }
    if (!config.authority.native_session_enabled
        || config.authority.legacy_ledger_enabled) {
        throw std::invalid_argument(
            "structural RNIC mode requires only the native authority");
    }
}

SimllmAtlahsFlowRuntime::SimllmAtlahsFlowRuntime(
        EventList& event_list,
        SimllmAtlahsRuntimeConfig config,
        std::unique_ptr<AtlahsFlowRuntime> network_runtime)
    : EventSource(event_list, "simllm-atlahs-flow-runtime"),
      config_(std::move(config)),
      network_runtime_(std::move(network_runtime)),
      port_(config_.port) {
    validateConfig(config_);
    if (network_runtime_ == nullptr) {
        throw std::invalid_argument(
            "SimLLM ATLAHS composition requires an htsim runtime");
    }
    validateTransportPolicy(config_.transport_policy, *network_runtime_);
    run_record_.session_id = config_.session_id;
    run_record_.hardware_mode = RnicHardwareMode::Structural;
    run_record_.authority =
        simllm::rnic::RnicWqeAuthority::SimllmNativeRnicSession;
    run_record_.transport_policy = config_.transport_policy;
    run_record_.seed = config_.seed;
    run_record_.topology_identity = config_.topology_identity;
    run_record_.htsim_source_revision = config_.htsim_source_revision;
    run_record_.simllm_source_revision = config_.simllm_source_revision;
    refreshRunRecord();
}

std::size_t SimllmAtlahsFlowRuntime::runtimePortCapacity(
        std::uint32_t node_count) const {
    const std::size_t sq_depth = config_.device.work_queue.sq_depth;
    if (sq_depth == 0
        || node_count > std::numeric_limits<std::size_t>::max() / sq_depth) {
        throw std::overflow_error(
            "SimLLM ATLAHS native session capacity overflows");
    }
    return static_cast<std::size_t>(node_count) * sq_depth;
}

SimllmAtlahsFlowRuntime::~SimllmAtlahsFlowRuntime() {
    if (event_handle_.has_value()) {
        EventList::cancelPendingSourceByHandle(*this, *event_handle_);
    }
    // The inner runtime owns EventList sources whose completion callback
    // targets the port. Destroy it while the port is still alive.
    network_runtime_.reset();
}

void SimllmAtlahsFlowRuntime::setup(
        std::uint32_t node_count, CompletionHandler complete_flow) {
    if (setup_) {
        throw std::logic_error("SimLLM ATLAHS runtime is already set up");
    }
    if (node_count == 0) {
        throw std::invalid_argument(
            "SimLLM ATLAHS runtime requires at least one endpoint");
    }
    if (!complete_flow) {
        throw std::invalid_argument(
            "SimLLM ATLAHS runtime requires a completion handler");
    }
    if (config_.port.endpoint_count != 0
        && config_.port.endpoint_count != node_count) {
        throw std::invalid_argument(
            "SimLLM ATLAHS endpoint count disagrees with the port");
    }
    const std::size_t port_capacity = runtimePortCapacity(node_count);

    std::vector<std::unique_ptr<RnicDevice>> devices;
    devices.reserve(node_count);
    std::optional<std::string> hardware_hash;
    std::optional<simllm::rnic::RnicSessionConfigRecord> session_config;
    for (std::uint32_t endpoint = 0; endpoint < node_count; ++endpoint) {
        RnicDeviceConfig device_config = config_.device;
        device_config.work_queue.source = endpoint;
        RnicDeviceAttachments attachments;
        attachments.network_port = &port_;
        auto device = std::make_unique<RnicDevice>(
            std::move(device_config), attachments);
        const auto record = simllm::rnic::makeStructuralSessionConfigRecord(
            config_.session_id,
            config_.transport_policy,
            *device);
        if (!hardware_hash.has_value()) {
            hardware_hash = record.hardware_config_sha256;
            session_config = record;
        } else if (record.hardware_config_sha256 != hardware_hash) {
            throw std::logic_error(
                "SimLLM ATLAHS endpoint hardware hashes disagree");
        }
        devices.push_back(std::move(device));
    }

    if (network_runtime_ != nullptr) {
        port_.bindRuntime(
            *network_runtime_,
            node_count,
            port_capacity,
            [this](Picoseconds terminal_at_ps) {
                if (terminal_at_ps < EventList::now()) {
                    throw std::logic_error(
                        "HTSIM terminal-ready callback moved backwards");
                }
                scheduleAt(terminal_at_ps);
            });
    }

    devices_ = std::move(devices);
    session_config_ = std::move(session_config);
    run_record_.hardware_config_sha256 = *hardware_hash;
    node_count_ = node_count;
    complete_flow_ = std::move(complete_flow);
    setup_ = true;
    authority_counters_.native_session_constructed = 1;
    refreshRunRecord();
}

void SimllmAtlahsFlowRuntime::send(const AtlahsFlowRequest& request) {
    if (!setup_) {
        throw std::logic_error("SimLLM ATLAHS send precedes setup");
    }
    const Picoseconds now_ps = EventList::now();
    if (request.start_time_ps != now_ps) {
        throw std::invalid_argument(
            "SimLLM ATLAHS flow start must equal event-list time");
    }
    if (request.source >= node_count_
        || request.destination >= node_count_) {
        throw std::out_of_range(
            "SimLLM ATLAHS flow endpoint is outside the session");
    }
    if (request.source == request.destination) {
        throw std::invalid_argument(
            "SimLLM ATLAHS flow requires distinct endpoints");
    }
    if (pending_flows_.count(request.flow_id) != 0
        || completion_projections_.count(request.flow_id) != 0) {
        throw std::invalid_argument(
            "SimLLM ATLAHS flow identity was reused");
    }
    if (next_wr_id_ == 0
        || next_wr_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "SimLLM ATLAHS WR identity space is exhausted");
    }

    WorkRequest work_request;
    work_request.wr_id = next_wr_id_;
    work_request.flow_id = request.flow_id;
    work_request.flow_tag = request.tag;
    work_request.destination = request.destination;
    work_request.payload_bytes = request.payload_bytes;
    work_request.traffic_class = config_.port.traffic_class;
    work_request.signaled = true;

    RnicDevice& source = *devices_.at(request.source);
    if (authority_counters_.native_posts
        == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "SimLLM ATLAHS native-post counter overflows");
    }
    const auto post = source.postSend(work_request, now_ps);
    if (post.status != PostStatus::Accepted || post.wqe_id == 0) {
        throw std::runtime_error(
            "SimLLM ATLAHS native SQ rejected a GOAL flow");
    }
    ++authority_counters_.native_posts;
    const auto inserted = pending_flows_.emplace(
        request.flow_id,
        PendingFlow{request, request.source, post.wqe_id});
    const auto owner = flow_by_wqe_.emplace(
        std::make_pair(request.source, post.wqe_id),
        request.flow_id);
    if (!inserted.second || !owner.second) {
        throw std::logic_error(
            "SimLLM ATLAHS native WQE correlation is not unique");
    }
    source.ringDoorbell(now_ps);
    source.progress(now_ps);

    ++next_wr_id_;
    ++run_record_.submitted_flows;
    refreshRunRecord();
    reschedule();
}

void SimllmAtlahsFlowRuntime::doNextEvent() {
    event_handle_.reset();
    scheduled_at_ps_.reset();
    driveAt(EventList::now());
    refreshRunRecord();
    reschedule();
}

void SimllmAtlahsFlowRuntime::driveAt(Picoseconds now_ps) {
    for (const NetworkEvent& event : port_.takeDue(now_ps)) {
        const simllm::rnic::NetworkToken owner_token =
            event.scope == simllm::rnic::NetworkEventScope::FlowExtent
            ? event.token
            : event.parent_token;
        const std::uint32_t endpoint = owner_token == 0
            ? event.source
            : port_.ownerSource(owner_token);
        if (endpoint >= devices_.size()) {
            throw std::out_of_range(
                "HTSIM event source is outside the native device session");
        }
        devices_.at(endpoint)->onNetworkEvent(event);
    }

    for (std::uint32_t endpoint = 0; endpoint < devices_.size(); ++endpoint) {
        RnicDevice& device = *devices_[endpoint];
        if (device.fatal()) {
            throw std::logic_error(
                "SimLLM ATLAHS native device became fatal");
        }
        device.progress(now_ps);
        std::vector<CompletionEntry> completions =
            device.pollCompletionQueue(
                std::numeric_limits<std::size_t>::max(), now_ps);
        for (const CompletionEntry& completion : completions) {
            retireCompletion(endpoint, completion);
        }
    }
}

void SimllmAtlahsFlowRuntime::retireCompletion(
        std::uint32_t endpoint, const CompletionEntry& completion) {
    const auto owner = flow_by_wqe_.find(
        std::make_pair(endpoint, completion.wqe_id));
    if (owner == flow_by_wqe_.end()) {
        throw std::logic_error(
            "SimLLM ATLAHS completion has no GOAL flow owner");
    }
    const AtlahsFlowId flow_id = owner->second;
    const auto pending = pending_flows_.find(flow_id);
    if (pending == pending_flows_.end()
        || pending->second.endpoint != endpoint
        || pending->second.wqe_id != completion.wqe_id) {
        throw std::logic_error(
            "SimLLM ATLAHS completion changed WQE ownership");
    }
    if (completion.status != CompletionStatus::Success
        && completion.status != CompletionStatus::TransportError) {
        throw std::logic_error(
            "SimLLM ATLAHS flow reached an unsupported completion status");
    }

    const auto& record = devices_[endpoint]->wqe(completion.wqe_id);
    if (!record.network_token.has_value()) {
        throw std::logic_error(
            "SimLLM ATLAHS completed WQE has no network token");
    }
    const AtlahsWqeCompletionProjection projection{
        completion.wqe_id,
        config_.device.work_queue.sq_id,
        0,
        config_.device.work_queue.cq_id,
        record.sq_sequence,
        record.sq_sequence,
        completion.cqe_sequence,
        completion.cqe_sequence,
        transportKind(),
        atlahsTransportObjectId(
            node_count_,
            transportKind(),
            pending->second.request.source,
            pending->second.request.destination)};
    if (!completion_projections_.emplace(flow_id, projection).second) {
        throw std::logic_error(
            "SimLLM ATLAHS flow completed twice");
    }

    pending_flows_.erase(pending);
    flow_by_wqe_.erase(owner);
    polled_completions_.push_back(completion);
    ++run_record_.completed_flows;
    complete_flow_(flow_id);
}

std::optional<Picoseconds> SimllmAtlahsFlowRuntime::nextEventTime() const {
    std::optional<Picoseconds> next = port_.nextEventTime();
    for (const auto& device : devices_) {
        const std::optional<Picoseconds> device_time =
            device->nextEventTime();
        if (device_time.has_value()
            && (!next.has_value() || *device_time < *next)) {
            next = device_time;
        }
    }
    return next;
}

void SimllmAtlahsFlowRuntime::reschedule() {
    const std::optional<Picoseconds> next = nextEventTime();
    if (!next.has_value()) {
        if (event_handle_.has_value()) {
            EventList::cancelPendingSourceByHandle(*this, *event_handle_);
            event_handle_.reset();
            scheduled_at_ps_.reset();
        }
        return;
    }
    if (*next < EventList::now()) {
        throw std::logic_error(
            "SimLLM ATLAHS event schedule moved backwards");
    }
    scheduleAt(*next);
}

void SimllmAtlahsFlowRuntime::scheduleAt(Picoseconds at_ps) {
    if (at_ps < EventList::now()) {
        throw std::logic_error(
            "SimLLM ATLAHS event schedule moved backwards");
    }
    if (scheduled_at_ps_.has_value()
        && *scheduled_at_ps_ <= at_ps) {
        return;
    }
    if (event_handle_.has_value()) {
        EventList::cancelPendingSourceByHandle(*this, *event_handle_);
    }
    const EventList::Handle handle =
        EventList::sourceIsPendingGetHandle(*this, at_ps);
    if (handle == EventList::nullHandle()) {
        event_handle_.reset();
        scheduled_at_ps_.reset();
        return;
    }
    event_handle_ = handle;
    scheduled_at_ps_ = at_ps;
}

bool SimllmAtlahsFlowRuntime::hasPendingPhysicalWork() const noexcept {
    if (!pending_flows_.empty() || port_.hasPendingPhysicalWork()) {
        return true;
    }
    return std::any_of(
        devices_.begin(), devices_.end(), [](const auto& device) {
            return device->hasPendingPhysicalWork();
        });
}

AtlahsTransportKind
SimllmAtlahsFlowRuntime::transportKind() const noexcept {
    if (network_runtime_ != nullptr) {
        return network_runtime_->transportKind();
    }
    if (config_.transport_policy == "rnic-cn") {
        return AtlahsTransportKind::RnicCnLinkPair;
    }
    if (config_.transport_policy == "dcqcn") {
        return AtlahsTransportKind::DcqcnQueuePair;
    }
    return AtlahsTransportKind::None;
}

std::optional<AtlahsWqeCompletionProjection>
SimllmAtlahsFlowRuntime::completionProjection(
        AtlahsFlowId flow_id) const {
    const auto projection = completion_projections_.find(flow_id);
    if (projection == completion_projections_.end()) {
        return std::nullopt;
    }
    return projection->second;
}

AtlahsWqeAuthorityCounters
SimllmAtlahsFlowRuntime::observedWqeAuthorityCounters() const noexcept {
    AtlahsWqeAuthorityCounters observed;
    observed.native_session_constructed =
        authority_counters_.native_session_constructed;
    observed.legacy_ledger_constructed =
        authority_counters_.legacy_ledger_constructed;
    observed.native_posts = authority_counters_.native_posts;
    observed.legacy_mutations = authority_counters_.legacy_mutations;
    return observed;
}

const simllm::rnic::RnicSessionConfigRecord&
SimllmAtlahsFlowRuntime::sessionConfigRecord() const {
    if (!session_config_.has_value()) {
        throw std::logic_error(
            "SimLLM ATLAHS session record precedes setup");
    }
    return *session_config_;
}

const RnicDevice& SimllmAtlahsFlowRuntime::device(
        std::uint32_t endpoint) const {
    if (!setup_ || endpoint >= devices_.size()) {
        throw std::out_of_range(
            "SimLLM ATLAHS device endpoint is outside the session");
    }
    return *devices_[endpoint];
}

void SimllmAtlahsFlowRuntime::validateQuiescent() const {
    if (!setup_) {
        throw std::logic_error(
            "SimLLM ATLAHS quiescence validation precedes setup");
    }
    if (hasPendingPhysicalWork() || event_handle_.has_value()
        || !port_.liveTokens().empty()
        || port_.issued().size() != port_.terminals().size()
        || run_record_.submitted_flows != run_record_.completed_flows) {
        throw std::logic_error(
            "SimLLM ATLAHS session is not physically quiescent");
    }
    if (authority_counters_.native_session_constructed != 1
        || authority_counters_.native_posts
            != run_record_.submitted_flows
        || authority_counters_.legacy_ledger_constructed != 0
        || authority_counters_.legacy_mutations != 0) {
        throw std::logic_error(
            "SimLLM ATLAHS authority counters are not exclusive");
    }
    for (const auto& device : devices_) {
        device->validateInvariants();
        if (device->fatal() || device->hasPendingPhysicalWork()
            || device->occupiedSqEntries() != 0
            || device->completionQueueDepth() != 0
            || device->unpublishedWqeCount() != 0) {
            throw std::logic_error(
                "SimLLM ATLAHS native device is not quiescent");
        }
    }
}

void SimllmAtlahsFlowRuntime::refreshRunRecord() {
    run_record_.authority_counters = authority_counters_;
    run_record_.quiescent = setup_ && !hasPendingPhysicalWork()
                            && run_record_.submitted_flows
                                   == run_record_.completed_flows;
}

std::unique_ptr<SimllmAtlahsFlowRuntime>
makeComposedSimllmAtlahsFlowRuntime(
        EventList& event_list,
        SimllmAtlahsRuntimeConfig config,
        std::unique_ptr<AtlahsFlowRuntime> network_runtime) {
    return std::make_unique<SimllmAtlahsFlowRuntime>(
        event_list,
        std::move(config),
        std::move(network_runtime));
}

}  // namespace htsim::simllm_rnic
