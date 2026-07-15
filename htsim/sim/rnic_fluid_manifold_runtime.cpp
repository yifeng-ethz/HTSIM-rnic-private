// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_fluid_manifold_runtime.h"

#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

RnicFluidManifoldRuntime::RnicFluidManifoldRuntime(
        EventList& event_list,
        RateBps node_link_capacity_bps,
        TimePs propagation_delay_ps)
    : EventSource(event_list, "rnic-fluid-manifold-runtime"),
      _node_link_capacity_bps(node_link_capacity_bps),
      _propagation_delay_ps(propagation_delay_ps) {
    if (_node_link_capacity_bps == 0) {
        throw std::invalid_argument("fluid manifold node-link capacity must be positive");
    }
}

RnicFluidManifoldRuntime::~RnicFluidManifoldRuntime() {
    if (_event_handle.has_value()) {
        EventList::cancelPendingSourceByHandle(*this, *_event_handle);
    }
}

void RnicFluidManifoldRuntime::setup(
        std::uint32_t node_count,
        CompletionHandler complete_flow) {
    if (isSetup()) {
        throw std::logic_error("fluid manifold runtime is already set up");
    }
    if (node_count == 0) {
        throw std::invalid_argument("fluid manifold runtime requires at least one node");
    }
    if (!complete_flow) {
        throw std::invalid_argument("fluid manifold runtime requires a completion handler");
    }

    RnicFluidManifold::CapacityMap source_uplinks;
    RnicFluidManifold::CapacityMap destination_downlinks;
    for (std::uint64_t node = 0; node < node_count; ++node) {
        const auto node_id = static_cast<RnicFluidFlowSpec::NodeId>(node);
        source_uplinks.emplace(node_id, _node_link_capacity_bps);
        destination_downlinks.emplace(node_id, _node_link_capacity_bps);
    }

    auto manifold = std::make_unique<RnicFluidManifold>(
        std::move(source_uplinks),
        std::move(destination_downlinks),
        _propagation_delay_ps);

    _complete_flow = std::move(complete_flow);
    _node_count = node_count;
    _manifold = std::move(manifold);
}

void RnicFluidManifoldRuntime::send(const AtlahsFlowRequest& request) {
    RnicFluidManifold& manifold = requireManifold();
    const TimePs now_ps = EventList::now();

    if (request.start_time_ps != now_ps) {
        throw std::invalid_argument(
            "fluid manifold flow start time must equal the current event-list time");
    }
    if (request.source >= _node_count || request.destination >= _node_count) {
        throw std::out_of_range("fluid manifold flow node is outside the configured range");
    }
    if (_deliveries.count(request.flow_id) != 0 || manifold.contains(request.flow_id)) {
        throw std::invalid_argument("duplicate fluid manifold runtime flow id");
    }

    // Build the new allocation off to the side.  In particular, a duration or
    // delivery-time overflow must not leave a partially admitted flow behind.
    RnicFluidManifold candidate = manifold;
    candidate.addFlow({request.flow_id,
                       request.source,
                       request.destination,
                       request.payload_bytes},
                      now_ps);
    std::map<AtlahsFlowId, DeliveryState> candidate_deliveries = _deliveries;
    candidate_deliveries.emplace(request.flow_id, DeliveryState{});

    validateSchedulable(candidate, candidate_deliveries, now_ps);
    const std::optional<TimePs> next_event =
        earliestEventTime(candidate, candidate_deliveries, now_ps);

    manifold = std::move(candidate);
    _deliveries = std::move(candidate_deliveries);
    reschedule(next_event);
}

bool RnicFluidManifoldRuntime::hasPendingPhysicalWork() const noexcept {
    for (const auto& entry : _deliveries) {
        if (!entry.second.completion_notified) {
            return true;
        }
    }
    return false;
}

std::size_t RnicFluidManifoldRuntime::activeFlowCount() const {
    return requireManifold().activeFlowCount();
}

bool RnicFluidManifoldRuntime::contains(AtlahsFlowId flow_id) const {
    return requireManifold().contains(flow_id);
}

RnicFluidFlowSnapshot RnicFluidManifoldRuntime::flow(AtlahsFlowId flow_id) const {
    return requireManifold().flow(flow_id);
}

void RnicFluidManifoldRuntime::doNextEvent() {
    // EventList erased this event's handle immediately before dispatch.
    _event_handle.reset();

    RnicFluidManifold& manifold = requireManifold();
    const TimePs now_ps = EventList::now();
    manifold.advanceTo(now_ps);

    const std::exception_ptr completion_error = notifyDueCompletions(now_ps);
    validateSchedulable(manifold, _deliveries, now_ps);
    reschedule(earliestEventTime(manifold, _deliveries, now_ps));

    if (completion_error) {
        std::rethrow_exception(completion_error);
    }
}

const RnicFluidManifold& RnicFluidManifoldRuntime::requireManifold() const {
    if (!_manifold) {
        throw std::logic_error("fluid manifold runtime has not been set up");
    }
    return *_manifold;
}

RnicFluidManifold& RnicFluidManifoldRuntime::requireManifold() {
    if (!_manifold) {
        throw std::logic_error("fluid manifold runtime has not been set up");
    }
    return *_manifold;
}

void RnicFluidManifoldRuntime::validateSchedulable(
        const RnicFluidManifold& manifold,
        const std::map<AtlahsFlowId, DeliveryState>& deliveries,
        TimePs now_ps) const {
    const TimePs maximum_time = std::numeric_limits<TimePs>::max();
    for (const auto& entry : deliveries) {
        const RnicFluidFlowSnapshot snapshot = manifold.flow(entry.first);
        if (!snapshot.active()) {
            if (!snapshot.delivery_completion_time_ps.has_value()
                || *snapshot.delivery_completion_time_ps < now_ps) {
                if (!entry.second.completion_notified) {
                    throw std::logic_error("fluid manifold has an invalid delivery time");
                }
            }
            continue;
        }
        const std::optional<TimePs> service_completion =
            manifold.projectedServiceCompletionTime(entry.first);
        if (!service_completion.has_value()) {
            // Integer-bps allocation can intentionally leave an extremely
            // oversubscribed flow dormant.  A later completion or join can
            // make it schedulable again; no completion time is invented here.
            continue;
        }
        if (_propagation_delay_ps > maximum_time - *service_completion) {
            throw std::overflow_error("fluid manifold delivery time overflow");
        }
    }
}

std::optional<RnicFluidManifoldRuntime::TimePs>
RnicFluidManifoldRuntime::earliestEventTime(
        const RnicFluidManifold& manifold,
        const std::map<AtlahsFlowId, DeliveryState>& deliveries,
        TimePs now_ps) const {
    std::optional<TimePs> earliest = manifold.nextServiceCompletionTime();
    for (const auto& entry : deliveries) {
        if (entry.second.completion_notified) {
            continue;
        }
        const RnicFluidFlowSnapshot snapshot = manifold.flow(entry.first);
        if (!snapshot.delivery_completion_time_ps.has_value()) {
            continue;
        }
        const TimePs delivery_time = *snapshot.delivery_completion_time_ps;
        if (delivery_time < now_ps) {
            throw std::logic_error("fluid manifold delivery event is in the past");
        }
        if (!earliest.has_value() || delivery_time < *earliest) {
            earliest = delivery_time;
        }
    }
    if (earliest.has_value() && *earliest < now_ps) {
        throw std::logic_error("fluid manifold service event is in the past");
    }
    return earliest;
}

void RnicFluidManifoldRuntime::reschedule(
        std::optional<TimePs> next_event_time) {
    if (_event_handle.has_value()) {
        EventList::cancelPendingSourceByHandle(*this, *_event_handle);
        _event_handle.reset();
    }
    if (!next_event_time.has_value()) {
        return;
    }

    EventList::Handle handle =
        EventList::sourceIsPendingGetHandle(*this, *next_event_time);
    if (handle != EventList::nullHandle()) {
        _event_handle = handle;
    }
}

std::exception_ptr RnicFluidManifoldRuntime::notifyDueCompletions(TimePs now_ps) {
    std::vector<AtlahsFlowId> due;
    for (const auto& entry : _deliveries) {
        if (entry.second.completion_notified) {
            continue;
        }
        const RnicFluidFlowSnapshot snapshot = requireManifold().flow(entry.first);
        if (snapshot.delivery_completion_time_ps.has_value()
            && *snapshot.delivery_completion_time_ps <= now_ps) {
            due.push_back(entry.first);
        }
    }

    std::exception_ptr first_error;
    for (AtlahsFlowId flow_id : due) {
        DeliveryState& state = _deliveries.at(flow_id);
        if (state.completion_notified) {
            continue;
        }
        // Mark first so a re-entrant or throwing completion handler cannot
        // notify the same ATLAHS flow twice.
        state.completion_notified = true;
        try {
            _complete_flow(flow_id);
        } catch (...) {
            if (!first_error) {
                first_error = std::current_exception();
            }
        }
    }
    return first_error;
}
