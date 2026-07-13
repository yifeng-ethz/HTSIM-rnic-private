// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_PACKETIZED_MANIFOLD_RUNTIME_H
#define RNIC_PACKETIZED_MANIFOLD_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <vector>

#include "atlahs_flow_runtime.h"
#include "eventlist.h"
#include "rnic_packet_extent.h"
#include "rnic_packetized_manifold.h"

struct RnicPacketizedFlowSnapshot {
    AtlahsFlowRequest request;
    std::uint64_t total_packet_count;
    std::uint64_t total_wire_bytes;
    std::uint64_t wire_rate_grant_bps;
    std::uint64_t packets_reserved;
    std::uint64_t payload_bytes_reserved;
    std::uint64_t wire_bytes_reserved;
    std::uint64_t packets_source_serialized;
    std::uint64_t payload_bytes_source_serialized;
    std::uint64_t wire_bytes_source_serialized;
    std::uint64_t packets_delivered;
    std::uint64_t payload_bytes_delivered;
    std::uint64_t wire_bytes_delivered;
    std::optional<std::uint64_t> source_completion_time_ps;
    std::optional<std::uint64_t> delivery_completion_time_ps;
    bool completion_notified;

    bool sourceBacklogged() const noexcept {
        return payload_bytes_reserved < request.payload_bytes;
    }
    bool draining() const noexcept {
        return !sourceBacklogged() && !delivery_completion_time_ps.has_value();
    }
    bool completed() const noexcept {
        return delivery_completion_time_ps.has_value();
    }
};

// ATLAHS adapter for packetized `rnic-nn`. A central max-min table feeds a
// collision-free full-M packet calendar. The topology-free manifold itself has
// no route, queue, loss, backpressure, acknowledgement, PRBS pacer, or Ring-CAM.
class RnicPacketizedManifoldRuntime final : public AtlahsFlowRuntime,
                                            private EventSource {
public:
    using TimePs = RnicPacketizedReservation::TimePs;
    using RateBps = RnicPacketizedGrant::RateBps;

    RnicPacketizedManifoldRuntime(
        EventList& event_list,
        RateBps node_link_capacity_bps,
        RnicDataPacketizationConfig packetization,
        TimePs propagation_delay_ps);
    RnicPacketizedManifoldRuntime(
        EventList& event_list,
        RateBps node_link_capacity_bps,
        std::uint64_t max_wire_packet_bytes,
        TimePs propagation_delay_ps);
    ~RnicPacketizedManifoldRuntime() override;

    RnicPacketizedManifoldRuntime(
        const RnicPacketizedManifoldRuntime&) = delete;
    RnicPacketizedManifoldRuntime& operator=(
        const RnicPacketizedManifoldRuntime&) = delete;

    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;

    bool isSetup() const noexcept { return _is_setup; }
    std::uint32_t nodeCount() const noexcept { return _node_count; }
    RateBps nodeLinkCapacity() const noexcept {
        return _node_link_capacity_bps;
    }
    const RnicDataPacketizationConfig& packetization() const noexcept {
        return _packetization;
    }
    TimePs propagationDelay() const noexcept { return _propagation_delay_ps; }
    std::size_t flowCount() const noexcept { return _flows.size(); }
    std::size_t backloggedFlowCount() const noexcept;
    std::size_t pendingSourcePacketCount() const noexcept {
        return _pending_source_completions.size();
    }
    // Includes every reserved packet awaiting destination completion: it may
    // still be waiting for its right-aligned source interval, physically in
    // transit, or serializing on the destination edge.
    std::size_t pendingDeliveryPacketCount() const noexcept {
        return _pending_deliveries.size();
    }
    bool contains(AtlahsFlowId flow_id) const noexcept;
    RnicPacketizedFlowSnapshot flow(AtlahsFlowId flow_id) const;
    std::optional<TimePs> nextSlotStartPs() const noexcept;

private:
    struct FlowState {
        AtlahsFlowRequest request;
        std::uint64_t total_packet_count = 0;
        std::uint64_t total_wire_bytes = 0;
        std::uint64_t wire_rate_grant_bps = 0;
        std::uint64_t packets_reserved = 0;
        std::uint64_t payload_bytes_reserved = 0;
        std::uint64_t wire_bytes_reserved = 0;
        std::uint64_t packets_source_serialized = 0;
        std::uint64_t payload_bytes_source_serialized = 0;
        std::uint64_t wire_bytes_source_serialized = 0;
        std::uint64_t packets_delivered = 0;
        std::uint64_t payload_bytes_delivered = 0;
        std::uint64_t wire_bytes_delivered = 0;
        std::optional<TimePs> source_completion_time_ps;
        std::optional<TimePs> delivery_completion_time_ps;
        bool completion_notified = false;

        bool sourceBacklogged() const noexcept {
            return payload_bytes_reserved < request.payload_bytes;
        }
    };

    struct PacketEvent {
        AtlahsFlowId flow_id;
        std::uint64_t packet_index;
        RnicPacketExtent extent;
    };

    using FlowMap = std::map<AtlahsFlowId, FlowState>;
    using PacketEventMap = std::multimap<TimePs, PacketEvent>;
    using ZeroDeliveryMap = std::multimap<TimePs, AtlahsFlowId>;

    void doNextEvent() override;
    bool isTraffic() override { return true; }

    void requireSetup() const;
    static std::pair<std::uint64_t, std::uint64_t> packetLedger(
        std::uint64_t payload_bytes,
        const RnicDataPacketizationConfig& packetization);
    static std::size_t backloggedFlowCount(const FlowMap& flows) noexcept;
    std::vector<RnicMaxMinFlow> activeFlows(const FlowMap& flows) const;
    bool recomputeAllocation(RnicPacketizedSlotCalendar& calendar,
                             FlowMap& flows) const;
    void validateNextSlot(const RnicPacketizedSlotCalendar& calendar,
                          const FlowMap& flows,
                          bool has_positive_grant) const;
    void reserveSlotAt(TimePs now_ps);
    std::vector<AtlahsFlowId> settleDueEvents(TimePs now_ps);
    std::exception_ptr notifyCompletions(
        const std::vector<AtlahsFlowId>& completed_flows);
    std::optional<TimePs> earliestEventTime(TimePs now_ps) const;
    std::optional<TimePs> earliestEventTime(
        const std::optional<RnicPacketizedSlotCalendar>& calendar,
        bool has_positive_grant,
        const FlowMap& flows,
        const PacketEventMap& pending_source_completions,
        const PacketEventMap& pending_deliveries,
        const ZeroDeliveryMap& pending_zero_deliveries,
        TimePs now_ps) const;
    void reschedule(std::optional<TimePs> next_event_time);

    RateBps _node_link_capacity_bps;
    RnicDataPacketizationConfig _packetization;
    TimePs _propagation_delay_ps;
    bool _is_setup = false;
    std::uint32_t _node_count = 0;
    CompletionHandler _complete_flow;
    FlowMap _flows;
    std::optional<RnicPacketizedSlotCalendar> _calendar;
    bool _has_positive_grant = false;
    PacketEventMap _pending_source_completions;
    PacketEventMap _pending_deliveries;
    ZeroDeliveryMap _pending_zero_deliveries;
    std::optional<EventList::Handle> _event_handle;
};

#endif  // RNIC_PACKETIZED_MANIFOLD_RUNTIME_H
