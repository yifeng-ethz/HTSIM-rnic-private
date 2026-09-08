// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_trace.h"

#include <stdexcept>

namespace {
constexpr const char* schema = "rnic-cn-trace-v1";
const std::array<const char*, 4> filenames{"flows.csv", "packets.csv", "queues.csv", "events.csv"};
const std::array<const char*, 4> headers{
    "schema,sequence,observed_at_ps,flow_id,packet_flow_id,source,destination,tag,start_time_ps,payload_bytes,total_wire_bytes,total_data_packets",
    "schema,sequence,observed_at_ps,packet_id,lifecycle_id,flow_id,packet_flow_id,kind,source,destination,path_id,wire_bytes,packet_index,attempt,payload_offset,payload_bytes,header_bytes,eta_ps,source_start_ps,source_end_ps,duplicate_test_copy,nflow_ppm,membership_epoch,n_hat,wire_rate_bps,effective_time_ps,feedback_deadline_ps,lease_expiry_ps,requested_attempt,acknowledged_attempt,final_gap_detection_ps,total_payload_bytes,total_wire_bytes,total_data_packets",
    "schema,sequence,observed_at_ps,transition,time_ps,switch_type,switch_id,ingress_id,egress_id,priority,packet_flow_id,packet_id,lifecycle_id,flow_id,kind,wire_bytes,buffered_bytes,in_service_bytes,backlog_bytes,shared_buffer_bytes",
    "schema,sequence,observed_at_ps,event,time_ps,flow_id,lifecycle_id,packet_id,packet_index,attempt,detail,logical_release_ps,service_start_ps,service_end_ps,trigger_lifecycle_id,predecessor_lifecycle_id,origin_attempt,deadline_ps"};

template <typename T> std::string number(T value) { return std::to_string(value); }
template <typename T> std::string number(const std::optional<T>& value) {
    return value.has_value() ? number(*value) : "";
}
const char* kindName(RnicCollectivePacketKind kind) {
    switch (kind) {
        case RnicCollectivePacketKind::DATA: return "DATA";
        case RnicCollectivePacketKind::DECLARE: return "DECLARE";
        case RnicCollectivePacketKind::ACCEPT: return "ACCEPT";
        case RnicCollectivePacketKind::GRANT_UPDATE: return "GRANT_UPDATE";
        case RnicCollectivePacketKind::GAP_NACK: return "GAP_NACK";
        case RnicCollectivePacketKind::GAP_RESOLVED: return "GAP_RESOLVED";
        case RnicCollectivePacketKind::RETIRE: return "RETIRE";
        case RnicCollectivePacketKind::NFLOW_UPDATE: return "NFLOW_UPDATE";
    }
    throw std::logic_error("unknown collective trace packet kind");
}
const char* transitionName(NsTm3QueueTransition transition) {
    switch (transition) {
        case NsTm3QueueTransition::Enqueued: return "enqueue";
        case NsTm3QueueTransition::Dequeued: return "service_start";
        case NsTm3QueueTransition::SerializationCompleted: return "service_end";
        case NsTm3QueueTransition::Dropped: return "drop";
    }
    throw std::logic_error("unknown collective trace queue transition");
}
}

RnicCollectiveTrace::RnicCollectiveTrace(const std::string& directory)
    : _directory(directory), _temporary(directory + ".tmp") {
    if (directory.empty() || EventList::now() != 0) {
        throw std::invalid_argument("rnic-cn trace requires a nonempty fresh path at time zero");
    }
    if (std::filesystem::exists(_directory) || std::filesystem::exists(_temporary) ||
        !std::filesystem::create_directory(_temporary)) {
        throw std::runtime_error("rnic-cn trace directory or temporary directory already exists");
    }
    for (std::size_t i = 0; i < _files.size(); ++i) {
        _files[i].exceptions(std::ios::failbit | std::ios::badbit);
        _files[i].open(_temporary / filenames[i], std::ios::binary);
        _files[i] << headers[i] << '\n';
    }
}

void RnicCollectiveTrace::row(std::size_t table, const std::vector<std::string>& cells) {
    if (_finished) throw std::logic_error("collective trace observed work after publication");
    const std::array<std::size_t, 4> widths{9, 31, 17, 15};
    if (cells.size() != widths.at(table)) throw std::logic_error("trace row width mismatch");
    auto& out = _files.at(table);
    out << schema << ',' << ++_sequence << ',' << EventList::now();
    for (const auto& cell : cells) {
        if (cell.find_first_of(",\r\n\"") != std::string::npos) {
            throw std::logic_error("invalid collective trace field");
        }
        out << ',' << cell;
    }
    out << '\n';
    ++_row_counts.at(table);
}

void RnicCollectiveTrace::fail() noexcept {
    if (!_failure) _failure = std::current_exception();
}

void RnicCollectiveTrace::flow(const AtlahsFlowRequest& request, std::uint32_t packet_flow_id,
                               const RnicCollectiveFinalLedger& ledger) noexcept {
    if (_failure) return;
    try {
        row(0, {number(request.flow_id), number(packet_flow_id), number(request.source),
                number(request.destination), number(request.tag), number(request.start_time_ps),
                number(request.payload_bytes), number(ledger.total_wire_bytes),
                number(ledger.total_data_packets)});
    } catch (...) { fail(); }
}

void RnicCollectiveTrace::packet(const RnicCollectivePacket& packet, std::uint32_t path_id,
                                 std::uint64_t source_start_ps, std::uint64_t source_end_ps,
                                 bool duplicate_test_copy) noexcept {
    if (_failure) return;
    try {
        Identity identity{packet.lifecycleId(), packet.atlahsFlowId(), packet.flow_id(),
                          kindName(packet.kind()), std::nullopt, std::nullopt};
        std::vector<std::string> cells{
            number(packet.id()), number(identity.lifecycle), number(identity.flow),
            number(identity.packet_flow), identity.kind, number(packet.source()),
            number(packet.destination()), number(path_id), number(packet.size())};
        cells.resize(31);
        cells[15] = number(source_start_ps);
        cells[16] = number(source_end_ps);
        cells[17] = duplicate_test_copy ? "1" : "0";
        auto extent = [&cells](std::uint64_t index, std::uint64_t offset,
                                const RnicPacketExtent& value) {
            cells[9] = number(index); cells[11] = number(offset);
            cells[12] = number(value.payloadBytes());
            cells[13] = number(value.wireBytes() - value.payloadBytes());
        };
        if (packet.kind() == RnicCollectivePacketKind::DATA) {
            const auto& data = packet.data();
            identity.index = data.packet_index; identity.attempt = data.transmission_attempt;
            extent(data.packet_index, data.payload_byte_offset, data.extent);
            cells[10] = number(data.transmission_attempt); cells[14] = number(data.eta_ps);
        } else if (packet.kind() == RnicCollectivePacketKind::GAP_NACK) {
            const auto& nack = packet.gapNack();
            extent(nack.packet_index, nack.payload_byte_offset, nack.extent);
            cells[25] = number(nack.requested_transmission_attempt);
        } else if (packet.kind() == RnicCollectivePacketKind::GAP_RESOLVED) {
            const auto& resolved = packet.gapResolved();
            extent(resolved.packet_index, resolved.payload_byte_offset, resolved.extent);
            cells[26] = number(resolved.acknowledged_transmission_attempt);
        } else if (packet.kind() == RnicCollectivePacketKind::DECLARE ||
                   packet.kind() == RnicCollectivePacketKind::NFLOW_UPDATE) {
            cells[18] = number(packet.declaration().nflow_ppm);
        } else if (packet.kind() == RnicCollectivePacketKind::ACCEPT ||
                   packet.kind() == RnicCollectivePacketKind::GRANT_UPDATE) {
            const auto& grant = packet.grant();
            cells[19] = number(grant.membership_epoch); cells[20] = number(grant.n_hat);
            cells[21] = number(grant.wire_rate_bps); cells[22] = number(grant.effective_time_ps);
            cells[23] = number(grant.feedback_deadline_ps); cells[24] = number(grant.lease_expiry_ps);
        }
        if (packet.kind() == RnicCollectivePacketKind::DATA ||
            packet.kind() == RnicCollectivePacketKind::RETIRE) {
            const auto& ledger = packet.finalLedger();
            cells[28] = number(ledger.total_payload_bytes); cells[29] = number(ledger.total_wire_bytes);
            cells[30] = number(ledger.total_data_packets);
            if (packet.kind() == RnicCollectivePacketKind::RETIRE)
                cells[27] = number(packet.retire().final_gap_detection_ps);
        }
        if (!_packets.emplace(packet.id(), identity).second ||
            !_lifecycle_packets.emplace(identity.lifecycle, packet.id()).second) {
            throw std::logic_error("duplicate collective trace physical identity");
        }
        row(1, cells);
        if (packet.kind() == RnicCollectivePacketKind::DATA) ++_data_packets;
        else ++_control_packets;
    } catch (...) { fail(); }
}

void RnicCollectiveTrace::lifecycle(const RnicCollectivePacketObservation& observation) noexcept {
    if (_failure) return;
    try {
        const char* name = observation.lifecycle == RnicCollectivePacketLifecycle::CREATED
            ? "packet_created" : observation.lifecycle == RnicCollectivePacketLifecycle::FABRIC_DROP
            ? "fabric_drop" : "endpoint_consumed";
        row(3, {name, number(EventList::now()), number(observation.flow_id),
                number(observation.lifecycle_id), number(observation.htsim_packet_id),
                number(observation.data_packet_index), "", number(observation.route_hops_completed),
                "", "", "", "0", "0", "", ""});
    } catch (...) { fail(); }
}

void RnicCollectiveTrace::event(const RnicCnTraceEvent& value) noexcept {
    if (_failure) return;
    try {
        std::string packet_id;
        auto index = value.packet_index;
        auto attempt = value.attempt;
        if (value.lifecycle_id != 0) {
            const auto id = _lifecycle_packets.at(value.lifecycle_id);
            const auto& identity = _packets.at(id);
            if (identity.flow != value.flow_id) throw std::logic_error("trace event flow mismatch");
            packet_id = number(id);
            if (!index) index = identity.index;
            if (!attempt) attempt = identity.attempt;
        }
        row(3, {value.event, number(value.time_ps), number(value.flow_id),
                number(value.lifecycle_id), packet_id, number(index), number(attempt), value.detail,
                number(value.logical_release_ps), number(value.service_start_ps),
                number(value.service_end_ps), number(value.trigger_lifecycle_id),
                number(value.predecessor_lifecycle_id), number(value.origin_attempt),
                number(value.deadline_ps)});
    } catch (...) { fail(); }
}

void RnicCollectiveTrace::observe(const NsTm3QueueObservation& observation) noexcept {
    if (_failure) return;
    try {
        const auto& identity = _packets.at(observation.packet_id);
        if (identity.packet_flow != observation.flow_id)
            throw std::logic_error("trace queue PacketFlow mapping mismatch");
        row(2, {transitionName(observation.transition), number(observation.time_ps),
                number(observation.switch_type), number(observation.switch_id),
                number(observation.ingress_id), number(observation.egress_id),
                number(observation.priority), number(observation.flow_id),
                number(observation.packet_id), number(identity.lifecycle), number(identity.flow),
                identity.kind, number(observation.packet_bytes),
                number(observation.egress_buffered_bytes), number(observation.egress_in_service_bytes),
                number(observation.egress_backlog_bytes), number(observation.shared_buffer_occupancy_bytes)});
    } catch (...) { fail(); }
}

void RnicCollectiveTrace::finish() {
    if (_failure) std::rethrow_exception(_failure);
    if (_finished) throw std::logic_error("collective trace was already published");
    for (auto& file : _files) { file.flush(); file.close(); }
    std::ofstream manifest;
    manifest.exceptions(std::ios::failbit | std::ios::badbit);
    manifest.open(_temporary / "manifest.csv", std::ios::binary);
    manifest << "schema,status,finalized,physical_quiescence,sequence_first,sequence_last,"
                "observation_count,flows_rows,packets_rows,queues_rows,events_rows,flow_count,"
                "data_packet_count,control_packet_count,physical_quiescence_time_ps\n"
             << schema << ",complete,true,verified," << (_sequence == 0 ? 0 : 1) << ','
             << _sequence << ',' << _sequence;
    for (const auto count : _row_counts) manifest << ',' << count;
    manifest << ',' << _row_counts[0] << ',' << _data_packets << ',' << _control_packets
             << ',' << EventList::now() << '\n';
    manifest.close();
    if (std::filesystem::exists(_directory)) throw std::runtime_error("trace destination appeared");
    std::filesystem::rename(_temporary, _directory);
    _finished = true;
}
