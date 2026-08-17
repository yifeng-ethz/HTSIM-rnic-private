// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DRAGONFLY_ROUTE_STATE_STORE_H
#define DRAGONFLY_ROUTE_STATE_STORE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "dragonfly_progressive_routing.h"

class Packet;

namespace htsim {

using DragonflyOrderedBindingKey = std::uint64_t;

// Topology-owned sidecar state for packets traversing a progressively routed
// Dragonfly.  Packet objects remain owned by their existing PacketDB (or by a
// test); this class neither frees nor otherwise controls their lifetime.
//
// An ordered binding key is supplied for traffic whose route must remain bound
// to a particular ordering domain.  Unordered traffic uses std::nullopt.  The
// caller must present the same key on every subsequent operation.  A binding
// fixes the source router, target router, deterministic routing control, and
// route hash until releaseOrderedBinding() is called after its final live
// packet is removed.  Adaptive controls are rejected for ordered bindings.
class DragonflyRouteStateStore {
public:
    DragonflyRouteStateStore() = default;
    ~DragonflyRouteStateStore() = default;

    DragonflyRouteStateStore(const DragonflyRouteStateStore&) = delete;
    DragonflyRouteStateStore& operator=(const DragonflyRouteStateStore&) = delete;
    DragonflyRouteStateStore(DragonflyRouteStateStore&&) = delete;
    DragonflyRouteStateStore& operator=(DragonflyRouteStateStore&&) = delete;

    // Register exactly once after the Packet has its id/src/dst and immediately
    // before it is injected.  A live pointer may never be overwritten.
    void insert(Packet& packet,
                DragonflyRouteState state,
                std::optional<DragonflyOrderedBindingKey> ordered_binding_key = std::nullopt);

    // Lookup is checked against the packet identity captured by insert().
    // Returning a reference is safe until this entry is erased.
    const DragonflyRouteState& require(
        const Packet& packet,
        std::optional<DragonflyOrderedBindingKey> ordered_binding_key = std::nullopt) const;
    DragonflyRouteState& mutate(
        Packet& packet,
        std::optional<DragonflyOrderedBindingKey> ordered_binding_key = std::nullopt);

    // Terminal operations remove the sidecar before a pooled Packet can be
    // recycled.  The routing engine must already have put the state in the
    // matching terminal phase; removal never manufactures a terminal result.
    DragonflyRouteState eraseForDelivery(
        const Packet& packet,
        std::optional<DragonflyOrderedBindingKey> ordered_binding_key = std::nullopt);
    DragonflyRouteState eraseForDrop(
        const Packet& packet,
        std::optional<DragonflyOrderedBindingKey> ordered_binding_key = std::nullopt);

    // A completed ordered binding is retained at zero live packets so that a
    // later packet cannot silently change route identity.  The ordering-domain
    // owner releases it explicitly after all packets in that domain complete.
    void releaseOrderedBinding(DragonflyOrderedBindingKey ordered_binding_key);

    std::size_t size() const noexcept { return _entries.size(); }
    bool empty() const noexcept { return _entries.empty(); }
    std::size_t orderedBindingCount() const noexcept { return _ordered_bindings.size(); }

    // Call at topology teardown or experiment quiescence.  Silent packet-state
    // leakage is a correctness failure because PacketDB may reuse the tracked
    // addresses; an unreleased ordered binding is also a correctness failure.
    void validateQuiescent() const;

private:
    struct OrderedRoutingIdentity {
        DragonflyRouterId source_router;
        DragonflyRouterId target_router;
        DragonflyRoutingControl control;
        std::uint64_t route_hash;
    };

    struct OrderedBinding {
        OrderedRoutingIdentity identity;
        std::size_t live_packets;
    };

    struct PacketIdentity {
        std::uint32_t packet_id;
        std::uint32_t source;
        std::uint32_t destination;
        std::optional<DragonflyOrderedBindingKey> ordered_binding_key;
    };

    struct Entry {
        PacketIdentity identity;
        DragonflyRouteState state;
    };

    static PacketIdentity captureIdentity(
        const Packet& packet,
        std::optional<DragonflyOrderedBindingKey> ordered_binding_key);
    static void validateIdentity(const Packet& packet,
                                 const PacketIdentity& expected,
                                 std::optional<DragonflyOrderedBindingKey> ordered_binding_key);
    static OrderedRoutingIdentity captureOrderedRoutingIdentity(const DragonflyRouteState& state);
    static void validateOrderedRoutingIdentity(const DragonflyRouteState& state,
                                               const OrderedRoutingIdentity& expected);
    void validateOrderedBinding(const Entry& entry) const;

    const Entry& checkedEntry(const Packet& packet,
                              std::optional<DragonflyOrderedBindingKey> ordered_binding_key) const;
    Entry& checkedEntry(Packet& packet,
                        std::optional<DragonflyOrderedBindingKey> ordered_binding_key);
    DragonflyRouteState erase(const Packet& packet,
                              DragonflyRoutePhase terminal_phase,
                              std::optional<DragonflyOrderedBindingKey> ordered_binding_key);

    std::unordered_map<const Packet*, Entry> _entries;
    std::unordered_map<DragonflyOrderedBindingKey, OrderedBinding> _ordered_bindings;
};

}  // namespace htsim

#endif  // DRAGONFLY_ROUTE_STATE_STORE_H
