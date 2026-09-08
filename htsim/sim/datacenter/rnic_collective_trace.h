// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_COLLECTIVE_TRACE_H
#define RNIC_COLLECTIVE_TRACE_H

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ns_tm3_switch.h"
#include "rnic_collective_packet.h"

struct RnicCnTraceEvent {
    std::string event;
    std::uint64_t time_ps;
    AtlahsFlowId flow_id;
    std::uint64_t lifecycle_id{0};
    std::string detail;
    std::optional<std::uint64_t> logical_release_ps;
    std::optional<std::uint64_t> service_start_ps;
    std::optional<std::uint64_t> service_end_ps;
    std::uint64_t trigger_lifecycle_id{0};
    std::uint64_t predecessor_lifecycle_id{0};
    std::optional<std::uint64_t> packet_index;
    std::optional<std::uint32_t> attempt;
    std::optional<std::uint32_t> origin_attempt;
    std::optional<std::uint64_t> deadline_ps;
};

// Observe existing authorities without scheduling work or allocating packet
// identities. Row observation time is separate from projected service bounds.
class RnicCollectiveTrace final : public NsTm3QueueObserver {
public:
    explicit RnicCollectiveTrace(const std::string& directory);
    void flow(const AtlahsFlowRequest& request, std::uint32_t packet_flow_id,
              const RnicCollectiveFinalLedger& ledger) noexcept;
    void packet(const RnicCollectivePacket& packet, std::uint32_t path_id,
                std::uint64_t source_start_ps, std::uint64_t source_end_ps,
                bool duplicate_test_copy) noexcept;
    void lifecycle(const RnicCollectivePacketObservation& observation) noexcept;
    void event(const RnicCnTraceEvent& event) noexcept;
    void observe(const NsTm3QueueObservation& observation) noexcept override;
    void finish();

private:
    struct Identity {
        std::uint64_t lifecycle;
        AtlahsFlowId flow;
        std::uint32_t packet_flow;
        std::string kind;
        std::optional<std::uint64_t> index;
        std::optional<std::uint32_t> attempt;
    };
    void row(std::size_t table, const std::vector<std::string>& cells);
    void fail() noexcept;
    std::filesystem::path _directory;
    std::filesystem::path _temporary;
    std::array<std::ofstream, 4> _files;
    std::map<packetid_t, Identity> _packets;
    std::map<std::uint64_t, packetid_t> _lifecycle_packets;
    std::uint64_t _sequence{0};
    std::array<std::uint64_t, 4> _row_counts{};
    std::uint64_t _data_packets{0};
    std::uint64_t _control_packets{0};
    std::exception_ptr _failure;
    bool _finished{false};
};

#endif
