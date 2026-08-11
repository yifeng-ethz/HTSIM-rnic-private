// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "datacenter/dcqcn_atlahs_runtime.h"
#include "eventlist.h"
#include "simllm_atlahs_flow_runtime.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using htsim::simllm_rnic::HtsimIssuedToken;
using htsim::simllm_rnic::HtsimNetworkPort;
using htsim::simllm_rnic::HtsimTerminalToken;
using htsim::simllm_rnic::SimllmAtlahsFlowRuntime;
using htsim::simllm_rnic::SimllmAtlahsRuntimeConfig;
using htsim::simllm_rnic::defaultSimllmAtlahsDeviceConfig;
using htsim::simllm_rnic::makeComposedSimllmAtlahsFlowRuntime;
using simllm::rnic::DropEvidenceProvenance;
using simllm::rnic::DropLocation;
using simllm::rnic::DropReason;
using simllm::rnic::NetworkEvent;
using simllm::rnic::NetworkEventKind;
using simllm::rnic::NetworkEventScope;
using simllm::rnic::NetworkLinkState;
using simllm::rnic::NetworkPacketKind;

constexpr std::uint32_t kNodeCount = 64;
constexpr std::uint32_t kDestination = 63;
constexpr std::uint64_t kEndpointLinkBps = UINT64_C(400000000000);
constexpr std::uint64_t kPolicyContextToken = 9001;

struct Arguments {
    std::string topology;
    std::string condition;
    std::string variant;
    std::filesystem::path observations;
};

struct Cell {
    std::uint32_t flow_count{0};
    std::uint64_t payload_bytes{0};
    bool pfc_enabled{true};
    bool congestion{false};
    bool pfc{false};
    bool dynamic_link{false};
    simtime_picosec link_up_at_ps{0};
};

struct Completion {
    AtlahsFlowId flow_id{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    std::uint64_t payload_bytes{0};
    simtime_picosec completion_at_ps{0};
};

void usage() {
    throw std::invalid_argument(
        "usage: simllm_rnic_control_probe --topology PATH --condition NAME "
        "--variant enabled|disabled|no_transition --observations PATH");
}

Arguments parseArguments(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        if (index + 1 >= argc) {
            usage();
        }
        const std::string value = argv[++index];
        if (name == "--topology") {
            arguments.topology = value;
        } else if (name == "--condition") {
            arguments.condition = value;
        } else if (name == "--variant") {
            arguments.variant = value;
        } else if (name == "--observations") {
            arguments.observations = value;
        } else {
            usage();
        }
    }
    if (arguments.topology.empty() || arguments.condition.empty()
        || arguments.variant.empty() || arguments.observations.empty()) {
        usage();
    }
    if (arguments.variant != "enabled" && arguments.variant != "disabled"
        && arguments.variant != "no_transition") {
        usage();
    }
    return arguments;
}

Cell resolveCell(const Arguments& arguments) {
    Cell cell;
    if (arguments.condition == "four_flow_ecn") {
        cell = Cell{4, 64 * 1024, false, true, false, false, 0};
    } else if (arguments.condition == "eight_flow_ecn") {
        cell = Cell{8, 64 * 1024, false, true, false, false, 0};
    } else if (arguments.condition == "pfc_64k") {
        cell = Cell{8, 64 * 1024, true, false, true, false, 0};
    } else if (arguments.condition == "pfc_128k") {
        cell = Cell{8, 128 * 1024, true, false, true, false, 0};
    } else if (arguments.condition == "link_hold_short") {
        cell = Cell{1, 64 * 1024, true, false, false, true, 201000};
    } else if (arguments.condition == "link_hold_long") {
        cell = Cell{1, 64 * 1024, true, false, false, true, 401000};
    } else {
        throw std::invalid_argument("unknown control study condition");
    }
    if (arguments.variant == "no_transition" && !cell.dynamic_link) {
        throw std::invalid_argument(
            "no_transition is valid only for a dynamic-link condition");
    }
    return cell;
}

const char* eventKindName(NetworkEventKind kind) {
    switch (kind) {
    case NetworkEventKind::Delivered:
        return "delivered";
    case NetworkEventKind::Dropped:
        return "dropped";
    case NetworkEventKind::PacketTxStarted:
        return "packet_tx_started";
    case NetworkEventKind::PacketTxFinished:
        return "packet_tx_finished";
    case NetworkEventKind::PacketRxArrived:
        return "packet_rx_arrived";
    case NetworkEventKind::EcnMarked:
        return "ecn_marked";
    case NetworkEventKind::CnpReceived:
        return "cnp_received";
    case NetworkEventKind::EligibilityUpdated:
        return "eligibility_updated";
    case NetworkEventKind::RateUpdated:
        return "rate_updated";
    case NetworkEventKind::PfcFrameSubmitted:
        return "pfc_frame_submitted";
    case NetworkEventKind::PfcPaused:
        return "pfc_paused";
    case NetworkEventKind::PfcResumed:
        return "pfc_resumed";
    case NetworkEventKind::LinkStateChanged:
        return "link_state_changed";
    }
    throw std::logic_error("unknown network event kind");
}

const char* scopeName(NetworkEventScope scope) {
    switch (scope) {
    case NetworkEventScope::FlowExtent:
        return "flow_extent";
    case NetworkEventScope::PacketAttempt:
        return "packet_attempt";
    case NetworkEventScope::TransportControl:
        return "transport_control";
    }
    throw std::logic_error("unknown network event scope");
}

const char* packetKindName(NetworkPacketKind kind) {
    switch (kind) {
    case NetworkPacketKind::Data:
        return "data";
    case NetworkPacketKind::Retransmission:
        return "retransmission";
    case NetworkPacketKind::Ack:
        return "ack";
    case NetworkPacketKind::Nak:
        return "nak";
    case NetworkPacketKind::Cnp:
        return "cnp";
    case NetworkPacketKind::Pfc:
        return "pfc";
    case NetworkPacketKind::OtherControl:
        return "other_control";
    }
    throw std::logic_error("unknown network packet kind");
}

const char* dropLocationName(DropLocation location) {
    switch (location) {
    case DropLocation::None:
        return "none";
    case DropLocation::TxPort:
        return "tx_port";
    case DropLocation::Fabric:
        return "fabric";
    case DropLocation::RxPort:
        return "rx_port";
    }
    throw std::logic_error("unknown drop location");
}

const char* dropReasonName(DropReason reason) {
    switch (reason) {
    case DropReason::None:
        return "none";
    case DropReason::Injected:
        return "injected";
    case DropReason::QueueOverflow:
        return "queue_overflow";
    case DropReason::LinkDown:
        return "link_down";
    case DropReason::PolicyRejected:
        return "policy_rejected";
    }
    throw std::logic_error("unknown drop reason");
}

const char* dropEvidenceName(DropEvidenceProvenance evidence) {
    switch (evidence) {
    case DropEvidenceProvenance::None:
        return "none";
    case DropEvidenceProvenance::Controlled:
        return "controlled";
    case DropEvidenceProvenance::Asserted:
        return "asserted";
    case DropEvidenceProvenance::Observed:
        return "observed";
    case DropEvidenceProvenance::Inferred:
        return "inferred";
    }
    throw std::logic_error("unknown drop evidence");
}

const char* linkStateName(NetworkLinkState state) {
    switch (state) {
    case NetworkLinkState::Unknown:
        return "unknown";
    case NetworkLinkState::Up:
        return "up";
    case NetworkLinkState::Down:
        return "down";
    }
    throw std::logic_error("unknown link state");
}

void writeEscaped(std::ostream& output, const std::string& value) {
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20) {
                throw std::invalid_argument(
                    "control probe JSON string contains a control byte");
            }
            output << character;
        }
    }
    output << '"';
}

void writeBoolean(std::ostream& output, bool value) {
    output << (value ? "true" : "false");
}

void writeEvent(std::ostream& output, const NetworkEvent& event) {
    output << '{';
    output << "\"abi_version\":" << event.abi_version;
    output << ",\"kind\":";
    writeEscaped(output, eventKindName(event.kind));
    output << ",\"scope\":";
    writeEscaped(output, scopeName(event.scope));
    output << ",\"token\":" << event.token;
    output << ",\"parent_token\":" << event.parent_token;
    output << ",\"wqe_id\":" << event.wqe_id;
    output << ",\"event_time_ps\":" << event.event_time_ps;
    output << ",\"extent_index\":" << event.extent_index;
    output << ",\"packet_index\":" << event.packet_index;
    output << ",\"transmission_attempt\":" << event.transmission_attempt;
    output << ",\"payload_offset_bytes\":" << event.payload_offset_bytes;
    output << ",\"payload_bytes\":" << event.payload_bytes;
    output << ",\"wire_bytes\":" << event.wire_bytes;
    output << ",\"packet_kind\":";
    writeEscaped(output, packetKindName(event.packet_kind));
    output << ",\"ecn_marked\":";
    writeBoolean(output, event.ecn_marked);
    output << ",\"drop_location\":";
    writeEscaped(output, dropLocationName(event.drop_location));
    output << ",\"drop_reason\":";
    writeEscaped(output, dropReasonName(event.drop_reason));
    output << ",\"drop_resource_id\":" << event.drop_resource_id;
    output << ",\"drop_evidence\":";
    writeEscaped(output, dropEvidenceName(event.drop_evidence));
    output << ",\"policy_context_token\":" << event.policy_context_token;
    output << ",\"source\":" << event.source;
    output << ",\"destination\":" << event.destination;
    output << ",\"link_id\":" << event.link_id;
    output << ",\"priority\":" << static_cast<unsigned int>(event.priority);
    output << ",\"pause_quanta\":" << event.pause_quanta;
    output << ",\"has_pause_duration\":";
    writeBoolean(output, event.has_pause_duration);
    output << ",\"pause_duration_ps\":" << event.pause_duration_ps;
    output << ",\"effective_at_ps\":" << event.effective_at_ps;
    output << ",\"has_effective_rate\":";
    writeBoolean(output, event.has_effective_rate);
    output << ",\"effective_rate_bps\":" << event.effective_rate_bps;
    output << ",\"link_state\":";
    writeEscaped(output, linkStateName(event.link_state));
    output << '}';
}

void writeIssued(std::ostream& output, const HtsimIssuedToken& issued) {
    output << '{';
    output << "\"token\":" << issued.token;
    output << ",\"wqe_id\":" << issued.wqe_id;
    output << ",\"wr_id\":" << issued.wr_id;
    output << ",\"flow_id\":" << issued.flow_id;
    output << ",\"policy_context_token\":" << issued.policy_context_token;
    output << ",\"source\":" << issued.source;
    output << ",\"destination\":" << issued.destination;
    output << ",\"accepted_at_ps\":" << issued.accepted_at_ps;
    output << ",\"port_tx_at_ps\":" << issued.port_tx_at_ps;
    output << ",\"payload_bytes\":" << issued.payload_bytes;
    output << '}';
}

void writeTerminal(std::ostream& output, const HtsimTerminalToken& terminal) {
    output << '{';
    output << "\"token\":" << terminal.token;
    output << ",\"wqe_id\":" << terminal.wqe_id;
    output << ",\"flow_id\":" << terminal.flow_id;
    output << ",\"kind\":";
    writeEscaped(output, eventKindName(terminal.kind));
    output << ",\"at_ps\":" << terminal.at_ps;
    output << ",\"ecn_marked\":";
    writeBoolean(output, terminal.ecn_marked);
    output << ",\"drop_location\":";
    writeEscaped(output, dropLocationName(terminal.drop_location));
    output << ",\"drop_reason\":";
    writeEscaped(output, dropReasonName(terminal.drop_reason));
    output << '}';
}

template <typename Value, typename Writer>
void writeArray(std::ostream& output,
                const std::vector<Value>& values,
                Writer writer) {
    output << '[';
    bool first = true;
    for (const Value& value : values) {
        if (!first) {
            output << ',';
        }
        first = false;
        writer(output, value);
    }
    output << ']';
}

void writeObservations(const Arguments& arguments,
                       const Cell& cell,
                       const SimllmAtlahsFlowRuntime& runtime,
                       const DcqcnAtlahsRuntime& physical,
                       const std::vector<Completion>& completions) {
    const std::filesystem::path temporary =
        arguments.observations.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open the control observation output");
    }
    const HtsimNetworkPort& port = runtime.networkPort();
    const auto capabilities = port.capabilities();
    const auto authority = runtime.authorityCounters();

    output << "{\n  \"schema\": \"simllm-rnic-control-probe-v1\",";
    output << "\n  \"condition\": ";
    writeEscaped(output, arguments.condition);
    output << ",\n  \"variant\": ";
    writeEscaped(output, arguments.variant);
    output << ",\n  \"flow_count\": " << cell.flow_count;
    output << ",\n  \"payload_bytes\": " << cell.payload_bytes;
    output << ",\n  \"capabilities\": {";
    output << "\"abi_version\":" << capabilities.abi_version;
    output << ",\"packet_attempt_events\":";
    writeBoolean(output, capabilities.packet_attempt_events);
    output << ",\"ecn_cnp_events\":";
    writeBoolean(output, capabilities.ecn_cnp_events);
    output << ",\"policy_update_events\":";
    writeBoolean(output, capabilities.policy_update_events);
    output << ",\"pfc_events\":";
    writeBoolean(output, capabilities.pfc_events);
    output << ",\"dynamic_link_events\":";
    writeBoolean(output, capabilities.dynamic_link_events);
    output << '}';

    output << ",\n  \"completions\": ";
    writeArray(output, completions, [](std::ostream& stream,
                                       const Completion& completion) {
        stream << '{';
        stream << "\"flow_id\":" << completion.flow_id;
        stream << ",\"source\":" << completion.source;
        stream << ",\"destination\":" << completion.destination;
        stream << ",\"payload_bytes\":" << completion.payload_bytes;
        stream << ",\"completion_at_ps\":" << completion.completion_at_ps;
        stream << '}';
    });
    output << ",\n  \"issued\": ";
    writeArray(output, port.issued(), writeIssued);
    output << ",\n  \"terminals\": ";
    writeArray(output, port.terminals(), writeTerminal);
    output << ",\n  \"packet_events\": ";
    writeArray(output, port.packetEvents(), writeEvent);
    output << ",\n  \"control_events\": ";
    writeArray(output, port.controlEvents(), writeEvent);

    output << ",\n  \"physical_counters\": {";
    output << "\"completed_flows\":" << physical.completed_flow_count();
    output << ",\"silent_rtos\":" << physical.silent_rto_count();
    output << ",\"loss_rate_cuts\":" << physical.loss_rate_cut_count();
    output << ",\"ecn_marked_packets\":" << physical.ecn_marked_packet_count();
    output << ",\"pfc_pauses\":" << physical.pfc_pause_count();
    output << ",\"pfc_resumes\":" << physical.pfc_resume_count();
    output << ",\"pfc_paused_wall_ps\":"
           << physical.pfc_paused_wall_ps_total();
    output << ",\"pfc_max_cascade_depth\":"
           << physical.pfc_max_cascade_depth();
    output << ",\"dropped_packets\":" << physical.dropped_packet_count();
    output << ",\"shared_pool_dropped_packets\":"
           << physical.shared_pool_dropped_packet_count();
    output << ",\"egress_domain_dropped_packets\":"
           << physical.egress_domain_dropped_packet_count();
    output << '}';

    output << ",\n  \"authority_counters\": {";
    output << "\"native_session_constructed\":"
           << authority.native_session_constructed;
    output << ",\"legacy_ledger_constructed\":"
           << authority.legacy_ledger_constructed;
    output << ",\"native_posts\":" << authority.native_posts;
    output << ",\"legacy_mutations\":" << authority.legacy_mutations;
    output << '}';
    output << ",\n  \"quiescent\": true\n}\n";
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write the control observation output");
    }
    std::filesystem::rename(temporary, arguments.observations);
}

int run(const Arguments& arguments) {
    const Cell cell = resolveCell(arguments);
    EventList event_list;
    EventList::setEndtime(std::numeric_limits<simtime_picosec>::max());

    DcqcnAtlahsRuntimeConfig network_config;
    network_config.topology_file = arguments.topology;
    network_config.endpoint_link_bps = kEndpointLinkBps;
    network_config.max_wire_packet_bytes = 4096;
    network_config.data_header_bytes = 64;
    network_config.ns_tm3_shared_buffer_bytes = 1024 * 1024;
    network_config.ns_tm3_egress_buffer_bytes = 1024 * 1024;
    network_config.ecn_kmin_bytes = 0;
    network_config.ecn_kmax_bytes = 4096;
    network_config.ecn_pmax_ppm = 1000000;
    network_config.ecn_seed = 9;
    network_config.pfc_enabled = cell.pfc_enabled;
    network_config.pfc_low_threshold_bytes = 4096;
    network_config.pfc_high_threshold_bytes = 8192;
    network_config.packet_event_observations = true;

    const bool observations_enabled = arguments.variant == "enabled";
    network_config.congestion_event_observations =
        observations_enabled && (cell.congestion || cell.pfc);
    network_config.pfc_event_observations = observations_enabled && cell.pfc;
    network_config.dynamic_link_event_observations =
        observations_enabled && cell.dynamic_link;
    if (cell.dynamic_link && arguments.variant != "no_transition") {
        network_config.dynamic_link_transitions = {
            DcqcnDynamicLinkTransition{1, 0, 1000, false},
            DcqcnDynamicLinkTransition{
                1, 0, cell.link_up_at_ps, true},
        };
    }

    auto network = std::make_unique<DcqcnAtlahsRuntime>(
        event_list, std::move(network_config), kNodeCount);
    DcqcnAtlahsRuntime* physical = network.get();

    SimllmAtlahsRuntimeConfig runtime_config;
    runtime_config.session_id =
        "control-v2-" + arguments.condition + "-" + arguments.variant;
    runtime_config.transport_policy = "dcqcn";
    runtime_config.seed = 9;
    runtime_config.topology_identity = arguments.topology;
    runtime_config.htsim_source_revision = "study-worktree";
    runtime_config.simllm_source_revision = "study-worktree";
    runtime_config.device = defaultSimllmAtlahsDeviceConfig();
    runtime_config.device.identity.policy_context_token = kPolicyContextToken;
    runtime_config.device.work_queue.policy_context_token =
        kPolicyContextToken;
    runtime_config.port.endpoint_count = kNodeCount;
    runtime_config.port.network_abi_version =
        simllm::rnic::kNetworkPortAbiVersionV2;
    runtime_config.port.link_rate_bps = kEndpointLinkBps;
    runtime_config.port.data_header_bytes = 64;
    runtime_config.port.max_wire_packet_bytes = 4096;
    runtime_config.port.congestion =
        observations_enabled && (cell.congestion || cell.pfc);
    runtime_config.port.control_frames = observations_enabled && cell.pfc;
    runtime_config.port.dynamic_link_events =
        observations_enabled && cell.dynamic_link;

    auto runtime = makeComposedSimllmAtlahsFlowRuntime(
        event_list, std::move(runtime_config), std::move(network));
    std::vector<Completion> completions;
    std::map<AtlahsFlowId, Completion> completion_by_flow;
    for (std::uint32_t source = 0; source < cell.flow_count; ++source) {
        const AtlahsFlowId flow_id = 1000 + source;
        completion_by_flow.emplace(
            flow_id,
            Completion{flow_id, source, kDestination, cell.payload_bytes, 0});
    }
    runtime->setup(kNodeCount, [&](AtlahsFlowId flow_id) {
        Completion completion = completion_by_flow.at(flow_id);
        completion.completion_at_ps = EventList::now();
        completions.push_back(completion);
    });
    for (std::uint32_t source = 0; source < cell.flow_count; ++source) {
        runtime->send(AtlahsFlowRequest{
            1000 + source,
            source,
            kDestination,
            cell.payload_bytes,
            EventList::now(),
            9,
            kPolicyContextToken});
    }

    std::size_t iterations = 0;
    while (runtime->hasPendingPhysicalWork()) {
        if (++iterations > 1000000 || !EventList::doNextEvent()) {
            throw std::runtime_error(
                "control probe did not reach physical quiescence");
        }
    }
    runtime->validateQuiescent();
    if (completions.size() != cell.flow_count) {
        throw std::logic_error("control probe completion ledger did not close");
    }
    writeObservations(arguments, cell, *runtime, *physical, completions);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parseArguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "simllm_rnic_control_probe: " << error.what() << '\n';
        return 1;
    }
}
