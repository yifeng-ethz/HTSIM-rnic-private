// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dragonfly_route_state_store.h"

#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "network.h"

namespace htsim {

DragonflyRouteStateStore::PacketIdentity DragonflyRouteStateStore::captureIdentity(
    const Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    return PacketIdentity{packet.id(), packet.src(), packet.dst(), ordered_binding_key};
}

void DragonflyRouteStateStore::validateIdentity(
    const Packet& packet,
    const PacketIdentity& expected,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    if (packet.id() != expected.packet_id || packet.src() != expected.source ||
        packet.dst() != expected.destination) {
        throw std::logic_error("Dragonfly route-state packet identity changed while live");
    }
    if (ordered_binding_key != expected.ordered_binding_key) {
        throw std::logic_error("Dragonfly route-state ordered binding key does not match");
    }
}

DragonflyRouteStateStore::OrderedRoutingIdentity
DragonflyRouteStateStore::captureOrderedRoutingIdentity(const DragonflyRouteState& state) {
    return OrderedRoutingIdentity{state.source_router, state.target_router, state.control,
                                  state.route_hash};
}

void DragonflyRouteStateStore::validateOrderedRoutingIdentity(
    const DragonflyRouteState& state,
    const OrderedRoutingIdentity& expected) {
    if (state.source_router != expected.source_router ||
        state.target_router != expected.target_router || state.control != expected.control ||
        state.route_hash != expected.route_hash) {
        throw std::logic_error("Dragonfly ordered binding route identity does not match");
    }
}

void DragonflyRouteStateStore::validateOrderedBinding(const Entry& entry) const {
    if (!entry.identity.ordered_binding_key.has_value()) {
        return;
    }

    const auto binding = _ordered_bindings.find(*entry.identity.ordered_binding_key);
    if (binding == _ordered_bindings.end() || binding->second.live_packets == 0) {
        throw std::logic_error("Dragonfly ordered binding registry is inconsistent");
    }
    validateOrderedRoutingIdentity(entry.state, binding->second.identity);
}

void DragonflyRouteStateStore::insert(
    Packet& packet,
    DragonflyRouteState state,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    const Packet* const address = &packet;
    if (_entries.find(address) != _entries.end()) {
        throw std::logic_error("Dragonfly route state already exists for this live packet");
    }

    auto binding = _ordered_bindings.end();
    bool binding_was_created = false;
    if (ordered_binding_key.has_value()) {
        if (state.control != DragonflyRoutingControl::DeterministicMinimal &&
            state.control != DragonflyRoutingControl::DeterministicNonMinimal) {
            throw std::invalid_argument(
                "Dragonfly ordered bindings require deterministic routing control");
        }
        const OrderedRoutingIdentity identity = captureOrderedRoutingIdentity(state);
        const auto result =
            _ordered_bindings.emplace(*ordered_binding_key, OrderedBinding{identity, 0});
        binding = result.first;
        binding_was_created = result.second;
        if (!binding_was_created) {
            validateOrderedRoutingIdentity(state, binding->second.identity);
        }
        if (binding->second.live_packets == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("Dragonfly ordered binding live-packet count overflow");
        }
        ++binding->second.live_packets;
    }

    try {
        Entry entry{captureIdentity(packet, ordered_binding_key), std::move(state)};
        const auto inserted = _entries.emplace(address, std::move(entry));
        if (!inserted.second) {
            throw std::logic_error("Dragonfly route-state insertion failed");
        }
    } catch (...) {
        if (ordered_binding_key.has_value()) {
            --binding->second.live_packets;
            if (binding_was_created) {
                _ordered_bindings.erase(binding);
            }
        }
        throw;
    }
}

const DragonflyRouteStateStore::Entry& DragonflyRouteStateStore::checkedEntry(
    const Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) const {
    const auto it = _entries.find(&packet);
    if (it == _entries.end()) {
        throw std::logic_error("Dragonfly route state is missing for this packet");
    }
    validateIdentity(packet, it->second.identity, ordered_binding_key);
    validateOrderedBinding(it->second);
    return it->second;
}

DragonflyRouteStateStore::Entry& DragonflyRouteStateStore::checkedEntry(
    Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    const auto it = _entries.find(&packet);
    if (it == _entries.end()) {
        throw std::logic_error("Dragonfly route state is missing for this packet");
    }
    validateIdentity(packet, it->second.identity, ordered_binding_key);
    validateOrderedBinding(it->second);
    return it->second;
}

const DragonflyRouteState& DragonflyRouteStateStore::require(
    const Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) const {
    return checkedEntry(packet, ordered_binding_key).state;
}

DragonflyRouteState& DragonflyRouteStateStore::mutate(
    Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    return checkedEntry(packet, ordered_binding_key).state;
}

DragonflyRouteState DragonflyRouteStateStore::erase(
    const Packet& packet,
    DragonflyRoutePhase terminal_phase,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    const auto it = _entries.find(&packet);
    if (it == _entries.end()) {
        throw std::logic_error("Dragonfly route state is missing for terminal removal");
    }
    validateIdentity(packet, it->second.identity, ordered_binding_key);
    validateOrderedBinding(it->second);
    if (it->second.state.phase != terminal_phase) {
        throw std::logic_error("Dragonfly route state is not in the requested terminal phase");
    }

    auto binding = _ordered_bindings.end();
    if (ordered_binding_key.has_value()) {
        binding = _ordered_bindings.find(*ordered_binding_key);
        if (binding == _ordered_bindings.end() || binding->second.live_packets == 0) {
            throw std::logic_error("Dragonfly ordered binding registry is inconsistent");
        }
    }

    DragonflyRouteState state = it->second.state;
    _entries.erase(it);
    if (ordered_binding_key.has_value()) {
        --binding->second.live_packets;
    }
    return state;
}

DragonflyRouteState DragonflyRouteStateStore::eraseForDelivery(
    const Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    return erase(packet, DragonflyRoutePhase::Delivered, ordered_binding_key);
}

DragonflyRouteState DragonflyRouteStateStore::eraseForDrop(
    const Packet& packet,
    std::optional<DragonflyOrderedBindingKey> ordered_binding_key) {
    return erase(packet, DragonflyRoutePhase::Dropped, ordered_binding_key);
}

void DragonflyRouteStateStore::releaseOrderedBinding(
    DragonflyOrderedBindingKey ordered_binding_key) {
    const auto binding = _ordered_bindings.find(ordered_binding_key);
    if (binding == _ordered_bindings.end()) {
        throw std::logic_error("Dragonfly ordered binding is missing");
    }
    if (binding->second.live_packets != 0) {
        throw std::logic_error("Dragonfly ordered binding still has live packets");
    }
    _ordered_bindings.erase(binding);
}

void DragonflyRouteStateStore::validateQuiescent() const {
    if (_entries.empty() && _ordered_bindings.empty()) {
        return;
    }

    std::ostringstream message;
    message << "Dragonfly route-state store is not quiescent: " << _entries.size()
            << " live packet entr" << (_entries.size() == 1 ? "y" : "ies") << " and "
            << _ordered_bindings.size() << " unreleased ordered binding"
            << (_ordered_bindings.size() == 1 ? "" : "s");
    throw std::logic_error(message.str());
}

}  // namespace htsim
