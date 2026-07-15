// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "dragonfly_route_state_store.h"

#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "network.h"

namespace htsim {
namespace {

class TestPacket final : public Packet {
public:
    void initialize(PacketFlow& flow,
                    packetid_t packet_id,
                    std::uint32_t source,
                    std::uint32_t destination) {
        set_attrs(flow, 256, packet_id);
        set_src(source);
        set_dst(destination);
    }

    PktPriority priority() const override { return PRIO_LO; }
    void free() override {}
};

DragonflyRouteState makeState(DragonflyRouterId source = 1,
                              DragonflyRouterId target = 19,
                              std::uint64_t route_hash = 0x1234) {
    DragonflyRouteState state;
    state.source_router = source;
    state.target_router = target;
    state.control = DragonflyRoutingControl::DeterministicNonMinimal;
    state.route_hash = route_hash;
    return state;
}

TEST(DragonflyRouteStateStoreTest, InsertsRequiresAndMutatesCheckedState) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 17, 3, 11);
    DragonflyRouteStateStore store;

    store.insert(packet, makeState(), 0xabc);

    EXPECT_EQ(store.size(), 1U);
    EXPECT_FALSE(store.empty());
    EXPECT_EQ(store.require(packet, 0xabc).target_router, 19U);

    store.mutate(packet, 0xabc).decision_count = 4;
    const DragonflyRouteStateStore& const_store = store;
    EXPECT_EQ(const_store.require(packet, 0xabc).decision_count, 4U);

    store.mutate(packet, 0xabc).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(packet, 0xabc);
    store.releaseOrderedBinding(0xabc);
}

TEST(DragonflyRouteStateStoreTest, RejectsChangedPacketIdentityAndWrongBinding) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 18, 4, 12);
    DragonflyRouteStateStore store;
    store.insert(packet, makeState(), 91);

    EXPECT_THROW(store.require(packet), std::logic_error);
    EXPECT_THROW(store.mutate(packet, 92), std::logic_error);

    packet.set_src(5);
    EXPECT_THROW(store.require(packet, 91), std::logic_error);
    packet.set_src(4);

    packet.set_dst(13);
    EXPECT_THROW(store.require(packet, 91), std::logic_error);
    packet.set_dst(12);

    packet.initialize(flow, 118, 4, 12);
    EXPECT_THROW(store.require(packet, 91), std::logic_error);
    packet.initialize(flow, 18, 4, 12);

    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.require(packet, 91).route_hash, 0x1234U);

    store.mutate(packet, 91).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(packet, 91);
    store.releaseOrderedBinding(91);
}

TEST(DragonflyRouteStateStoreTest, RejectsDuplicateLivePointer) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 19, 5, 13);
    DragonflyRouteStateStore store;
    store.insert(packet, makeState());

    EXPECT_THROW(store.insert(packet, makeState(2, 20)), std::logic_error);
    packet.initialize(flow, 119, 15, 23);
    EXPECT_THROW(store.insert(packet, makeState(2, 20)), std::logic_error);
    packet.initialize(flow, 19, 5, 13);
    EXPECT_EQ(store.require(packet).source_router, 1U);
    EXPECT_EQ(store.size(), 1U);

    store.mutate(packet).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(packet);
}

TEST(DragonflyRouteStateStoreTest, DeliveryErasesWithoutLeavingStaleState) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 20, 6, 14);
    DragonflyRouteStateStore store;
    store.insert(packet, makeState());
    store.mutate(packet).router_hops = 3;
    store.mutate(packet).phase = DragonflyRoutePhase::Delivered;

    const auto terminal = store.eraseForDelivery(packet);

    EXPECT_EQ(terminal.phase, DragonflyRoutePhase::Delivered);
    EXPECT_EQ(terminal.router_hops, 3U);
    EXPECT_TRUE(store.empty());
    EXPECT_THROW(store.require(packet), std::logic_error);
    EXPECT_THROW(store.eraseForDelivery(packet), std::logic_error);
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, DropErasesAndMarksTerminalReason) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 21, 7, 15);
    DragonflyRouteStateStore store;
    store.insert(packet, makeState(), 77);
    store.mutate(packet, 77).phase = DragonflyRoutePhase::Dropped;

    const auto terminal = store.eraseForDrop(packet, 77);

    EXPECT_EQ(terminal.phase, DragonflyRoutePhase::Dropped);
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.orderedBindingCount(), 1U);
    EXPECT_THROW(store.validateQuiescent(), std::logic_error);
    store.releaseOrderedBinding(77);
    EXPECT_EQ(store.orderedBindingCount(), 0U);
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, AllowsCleanPointerReuseOnlyAfterErase) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    DragonflyRouteStateStore store;

    packet.initialize(flow, 22, 8, 16);
    store.insert(packet, makeState(2, 20, 100));
    EXPECT_THROW(store.insert(packet, makeState(3, 21, 200)), std::logic_error);
    store.mutate(packet).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(packet);

    // Model PacketDB recycling the same object for a different packet.
    packet.initialize(flow, 23, 9, 17);
    store.insert(packet, makeState(3, 21, 200), 44);

    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.require(packet, 44).route_hash, 200U);
    EXPECT_EQ(store.require(packet, 44).source_router, 3U);
    EXPECT_THROW(store.require(packet), std::logic_error);

    store.mutate(packet, 44).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(packet, 44);
    store.releaseOrderedBinding(44);
}

TEST(DragonflyRouteStateStoreTest, QuiescenceValidationFindsEveryLeak) {
    PacketFlow flow(nullptr);
    TestPacket first;
    TestPacket second;
    first.initialize(flow, 24, 10, 18);
    second.initialize(flow, 25, 11, 19);
    DragonflyRouteStateStore store;
    store.insert(first, makeState());
    store.insert(second, makeState(4, 22));

    EXPECT_THROW(store.validateQuiescent(), std::logic_error);
    store.mutate(first).phase = DragonflyRoutePhase::Delivered;
    store.eraseForDelivery(first);
    EXPECT_THROW(store.validateQuiescent(), std::logic_error);
    store.mutate(second).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(second);
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, OrderedBindingRejectsDifferentHashAcrossConcurrentPackets) {
    PacketFlow flow(nullptr);
    TestPacket first;
    TestPacket second;
    first.initialize(flow, 27, 0, 71);
    second.initialize(flow, 28, 0, 71);
    DragonflyRouteStateStore store;
    constexpr DragonflyOrderedBindingKey binding_key = 0x8800;

    store.insert(first, makeState(1, 19, 0x1111), binding_key);
    EXPECT_THROW(store.insert(second, makeState(1, 19, 0x2222), binding_key), std::logic_error);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.orderedBindingCount(), 1U);

    store.mutate(first, binding_key).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(first, binding_key);
    store.releaseOrderedBinding(binding_key);
}

TEST(DragonflyRouteStateStoreTest, OrderedBindingRejectsDifferentEndpointsAndRoutingControl) {
    PacketFlow flow(nullptr);
    TestPacket first;
    TestPacket contender;
    first.initialize(flow, 29, 0, 71);
    contender.initialize(flow, 30, 0, 71);
    DragonflyRouteStateStore store;
    constexpr DragonflyOrderedBindingKey binding_key = 0x8801;
    const DragonflyRouteState bound = makeState(1, 19, 0x3333);
    store.insert(first, bound, binding_key);

    EXPECT_THROW(store.insert(contender, makeState(2, 19, 0x3333), binding_key), std::logic_error);
    EXPECT_THROW(store.insert(contender, makeState(1, 20, 0x3333), binding_key), std::logic_error);
    DragonflyRouteState different_control = bound;
    different_control.control = DragonflyRoutingControl::DeterministicMinimal;
    EXPECT_THROW(store.insert(contender, different_control, binding_key), std::logic_error);
    EXPECT_EQ(store.size(), 1U);

    store.mutate(first, binding_key).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(first, binding_key);
    store.releaseOrderedBinding(binding_key);
}

TEST(DragonflyRouteStateStoreTest, OrderedBindingRejectsAdaptiveRoutingControl) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 36, 0, 71);
    DragonflyRouteStateStore store;
    DragonflyRouteState adaptive = makeState();
    adaptive.control = DragonflyRoutingControl::Adaptive2;

    EXPECT_THROW(store.insert(packet, adaptive, 0x8804), std::invalid_argument);
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.orderedBindingCount(), 0U);
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, OrderedBindingCannotBeReleasedWhilePacketsAreLive) {
    PacketFlow flow(nullptr);
    TestPacket first;
    TestPacket second;
    first.initialize(flow, 31, 0, 71);
    second.initialize(flow, 32, 0, 71);
    DragonflyRouteStateStore store;
    constexpr DragonflyOrderedBindingKey binding_key = 0x8802;

    store.insert(first, makeState(), binding_key);
    store.insert(second, makeState(), binding_key);
    EXPECT_THROW(store.releaseOrderedBinding(binding_key), std::logic_error);

    store.mutate(first, binding_key).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(first, binding_key);
    EXPECT_THROW(store.releaseOrderedBinding(binding_key), std::logic_error);

    store.mutate(second, binding_key).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(second, binding_key);
    EXPECT_NO_THROW(store.releaseOrderedBinding(binding_key));
    EXPECT_THROW(store.releaseOrderedBinding(binding_key), std::logic_error);
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, OrderedBindingAllowsNewIdentityOnlyAfterExplicitRelease) {
    PacketFlow flow(nullptr);
    TestPacket first;
    TestPacket second;
    first.initialize(flow, 33, 0, 71);
    second.initialize(flow, 34, 1, 70);
    DragonflyRouteStateStore store;
    constexpr DragonflyOrderedBindingKey binding_key = 0x8803;

    store.insert(first, makeState(1, 19, 0x4444), binding_key);
    store.mutate(first, binding_key).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(first, binding_key);

    EXPECT_THROW(store.insert(second, makeState(2, 20, 0x5555), binding_key), std::logic_error);
    store.releaseOrderedBinding(binding_key);
    EXPECT_NO_THROW(store.insert(second, makeState(2, 20, 0x5555), binding_key));

    store.mutate(second, binding_key).phase = DragonflyRoutePhase::Dropped;
    store.eraseForDrop(second, binding_key);
    store.releaseOrderedBinding(binding_key);
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, DeliveryRemovalRejectsNonDeliveredState) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 35, 0, 71);
    DragonflyRouteStateStore store;
    store.insert(packet, makeState());

    EXPECT_EQ(store.require(packet).phase, DragonflyRoutePhase::Injection);
    EXPECT_THROW(store.eraseForDelivery(packet), std::logic_error);
    EXPECT_THROW(store.eraseForDrop(packet), std::logic_error);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.require(packet).phase, DragonflyRoutePhase::Injection);

    store.mutate(packet).phase = DragonflyRoutePhase::Delivered;
    EXPECT_NO_THROW(store.eraseForDelivery(packet));
    EXPECT_NO_THROW(store.validateQuiescent());
}

TEST(DragonflyRouteStateStoreTest, CarriesOneCommittedRouteAcrossEveryProgressiveSwitch) {
    PacketFlow flow(nullptr);
    TestPacket packet;
    packet.initialize(flow, 26, 0, 71);
    DragonflyProgressiveRouting routing;
    DragonflyRouteStateStore store;
    store.insert(
        packet, routing.initialize(0, 35, DragonflyRoutingControl::DeterministicNonMinimal, 0xfeed),
        0x5500);

    DragonflyRouterId current = 0;
    DragonflyRoutingDecision decision;
    for (std::uint8_t step = 0; step <= routing.config().maximum_router_hops; ++step) {
        decision = routing.nextHop(current, store.mutate(packet, 0x5500));
        if (decision.outcome != DragonflyDecisionOutcome::Forward) {
            break;
        }
        ASSERT_TRUE(decision.next_router.has_value());
        current = *decision.next_router;
    }

    ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Delivered);
    EXPECT_EQ(current, 35U);
    const DragonflyRouteState terminal = store.eraseForDelivery(packet, 0x5500);
    EXPECT_EQ(terminal.phase, DragonflyRoutePhase::Delivered);
    EXPECT_EQ(terminal.global_hops, 2U);
    EXPECT_LE(terminal.router_hops, routing.config().maximum_router_hops);
    EXPECT_TRUE(store.empty());
    EXPECT_THROW(store.validateQuiescent(), std::logic_error);
    store.releaseOrderedBinding(0x5500);
    EXPECT_NO_THROW(store.validateQuiescent());
}

}  // namespace
}  // namespace htsim
