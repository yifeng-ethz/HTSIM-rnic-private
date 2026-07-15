// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef RNIC_FLUID_MANIFOLD_RUNTIME_H
#define RNIC_FLUID_MANIFOLD_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "atlahs_flow_runtime.h"
#include "eventlist.h"
#include "rnic_fluid_manifold.h"

// ATLAHS adapter for the rnic-nn-fluid profile.  It deliberately contains no
// packet transport machinery: active byte streams receive an instantaneous
// central max-min allocation, and a flow is reported complete after its last
// serviced bit crosses one fixed propagation delay.
class RnicFluidManifoldRuntime final : public AtlahsFlowRuntime,
                                       private EventSource {
public:
    using TimePs = RnicFluidManifold::TimePs;
    using RateBps = RnicFluidFlowSpec::RateBps;

    RnicFluidManifoldRuntime(EventList& event_list,
                             RateBps node_link_capacity_bps,
                             TimePs propagation_delay_ps);
    ~RnicFluidManifoldRuntime() override;

    RnicFluidManifoldRuntime(const RnicFluidManifoldRuntime&) = delete;
    RnicFluidManifoldRuntime& operator=(const RnicFluidManifoldRuntime&) = delete;

    void setup(std::uint32_t node_count,
               CompletionHandler complete_flow) override;
    void send(const AtlahsFlowRequest& request) override;
    bool hasPendingPhysicalWork() const noexcept override;

    bool isSetup() const noexcept { return _manifold != nullptr; }
    std::uint32_t nodeCount() const noexcept { return _node_count; }
    RateBps nodeLinkCapacity() const noexcept { return _node_link_capacity_bps; }
    TimePs propagationDelay() const noexcept { return _propagation_delay_ps; }
    std::size_t activeFlowCount() const;
    bool contains(AtlahsFlowId flow_id) const;
    RnicFluidFlowSnapshot flow(AtlahsFlowId flow_id) const;

private:
    struct DeliveryState {
        bool completion_notified = false;
    };

    void doNextEvent() override;
    bool isTraffic() override { return true; }

    const RnicFluidManifold& requireManifold() const;
    RnicFluidManifold& requireManifold();

    void validateSchedulable(const RnicFluidManifold& manifold,
                             const std::map<AtlahsFlowId, DeliveryState>& deliveries,
                             TimePs now_ps) const;
    std::optional<TimePs> earliestEventTime(
        const RnicFluidManifold& manifold,
        const std::map<AtlahsFlowId, DeliveryState>& deliveries,
        TimePs now_ps) const;
    void reschedule(std::optional<TimePs> next_event_time);
    std::exception_ptr notifyDueCompletions(TimePs now_ps);

    RateBps _node_link_capacity_bps;
    TimePs _propagation_delay_ps;
    std::uint32_t _node_count = 0;
    CompletionHandler _complete_flow;
    std::unique_ptr<RnicFluidManifold> _manifold;
    std::map<AtlahsFlowId, DeliveryState> _deliveries;
    std::optional<EventList::Handle> _event_handle;
};

#endif  // RNIC_FLUID_MANIFOLD_RUNTIME_H
