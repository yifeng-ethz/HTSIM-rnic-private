// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "rnic_collective_packet.h"

namespace {

class RecordingObserver final : public RnicCollectivePacketLifecycleObserver {
public:
    void observe(const RnicCollectivePacketObservation& observation) noexcept override {
        observations.push_back(observation);
    }

    std::vector<RnicCollectivePacketObservation> observations;
};

class ConsumingEndpoint final : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        auto* collective = dynamic_cast<RnicCollectivePacket*>(&packet);
        if (collective == nullptr) {
            ADD_FAILURE() << "endpoint received a non-collective packet";
            return;
        }
        received_kinds.push_back(collective->kind());
        received_flow_ids.push_back(collective->atlahsFlowId());
        collective->consumeAtEndpoint();
    }

    const string& nodename() override { return _name; }

    std::vector<RnicCollectivePacketKind> received_kinds;
    std::vector<AtlahsFlowId> received_flow_ids;

private:
    string _name{"ConsumingEndpoint"};
};

class ForwardingSink final : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        auto* collective = dynamic_cast<RnicCollectivePacket*>(&packet);
        if (collective == nullptr) {
            ADD_FAILURE() << "forwarder received a non-collective packet";
            return;
        }
        try {
            collective->consumeAtEndpoint();
            ADD_FAILURE() << "intermediate route hop consumed the packet";
        } catch (const std::logic_error&) {
            early_consume_rejected = true;
        }
        packet.sendOn();
    }

    const string& nodename() override { return _name; }

    bool early_consume_rejected{false};

private:
    string _name{"ForwardingSink"};
};

Route routeTo(PacketSink& endpoint) {
    Route route;
    route.push_back(&endpoint);
    return route;
}

RnicCollectiveFinalLedger twoPacketLedger() {
    return {1500, 1628, 2};
}

RnicCollectiveDeclareMetadata declareMetadata(std::uint32_t nflow = 1) {
    return {nflow};
}

TEST(RnicCollectivePacketTest, CarriesFullAtlahsIdentityAndConsumesDataExplicitly) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();
    const AtlahsFlowId flow_id = makeAtlahsFlowId(0xfedcba98U, 0x76543210U);
    const RnicCollectiveDataMetadata metadata{0, 0, RnicPacketExtent(1000, 1064), 987654321,
                                              twoPacketLedger()};

    RnicCollectivePacket* packet =
        RnicCollectivePacket::newData(flow, route, 77, flow_id, 4, 9, metadata, observer);

    EXPECT_EQ(packet->kind(), RnicCollectivePacketKind::DATA);
    EXPECT_EQ(packet->atlahsFlowId(), flow_id);
    EXPECT_EQ(packet->source(), 4U);
    EXPECT_EQ(packet->destination(), 9U);
    EXPECT_EQ(packet->size(), 1064U);
    EXPECT_EQ(packet->priority(), Packet::PRIO_LO);
    EXPECT_FALSE(packet->header_only());
    EXPECT_EQ(packet->data().packet_index, 0U);
    EXPECT_EQ(packet->data().payload_byte_offset, 0U);
    EXPECT_EQ(packet->data().extent.payloadBytes(), 1000U);
    EXPECT_EQ(packet->data().eta_ps, 987654321U);
    EXPECT_EQ(packet->data().transmission_attempt, 0U);
    EXPECT_FALSE(packet->isFinalDataPacket());
    EXPECT_EQ(packet->finalLedger().total_wire_bytes, 1628U);
    ASSERT_EQ(observer->observations.size(), 1U);
    const std::uint64_t lifecycle_id = packet->lifecycleId();
    EXPECT_EQ(observer->observations[0].lifecycle, RnicCollectivePacketLifecycle::CREATED);
    EXPECT_EQ(observer->observations[0].lifecycle_id, lifecycle_id);

    packet->sendOn();

    ASSERT_EQ(endpoint.received_flow_ids.size(), 1U);
    EXPECT_EQ(endpoint.received_flow_ids[0], flow_id);
    ASSERT_EQ(observer->observations.size(), 2U);
    EXPECT_EQ(observer->observations[1].lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
    EXPECT_EQ(observer->observations[1].lifecycle_id, lifecycle_id);
    EXPECT_EQ(observer->observations[1].route_hops_completed, 1U);
    ASSERT_TRUE(observer->observations[1].data_packet_index.has_value());
    EXPECT_EQ(*observer->observations[1].data_packet_index, 0U);
}

TEST(RnicCollectivePacketTest, DirectFreeIsAnObservableFabricDropAndCannotRepeat) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();

    RnicCollectivePacket* packet = RnicCollectivePacket::newDeclare(
        flow, route, 5, 0x100000001ULL, 2, 7, 64, declareMetadata(), observer);
    EXPECT_EQ(packet->priority(), Packet::PRIO_HI);
    EXPECT_FALSE(packet->header_only());
    EXPECT_THROW(packet->consumeAtEndpoint(), std::logic_error);

    packet->free();

    ASSERT_EQ(observer->observations.size(), 2U);
    EXPECT_EQ(observer->observations[1].lifecycle, RnicCollectivePacketLifecycle::FABRIC_DROP);
    EXPECT_EQ(observer->observations[1].route_hops_completed, 0U);
    EXPECT_FALSE(observer->observations[1].data_packet_index.has_value());
    EXPECT_THROW(packet->free(), std::logic_error);
    EXPECT_EQ(observer->observations.size(), 2U);
}

TEST(RnicCollectivePacketTest, DeclareCarriesOnlyNflow) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();

    RnicCollectivePacket* packet = RnicCollectivePacket::newDeclare(
        flow, route, 51, 0x100000010ULL, 2, 7, 64, declareMetadata(1), observer);

    EXPECT_EQ(packet->declaration().nflow, 1U);
    EXPECT_THROW(packet->data(), std::logic_error);
    EXPECT_THROW(packet->grant(), std::logic_error);
    packet->sendOn();
}

TEST(RnicCollectivePacketTest, EndpointConsumptionRequiresTheExactExplicitRouteEnd) {
    PacketFlow flow(nullptr);
    ForwardingSink forwarding_sink;
    ConsumingEndpoint endpoint;
    Route route;
    route.push_back(&forwarding_sink);
    route.push_back(&endpoint);
    auto observer = std::make_shared<RecordingObserver>();

    RnicCollectivePacket* packet = RnicCollectivePacket::newDeclare(
        flow, route, 6, 0x100000002ULL, 2, 7, 64, declareMetadata(), observer);
    packet->sendOn();

    EXPECT_TRUE(forwarding_sink.early_consume_rejected);
    ASSERT_EQ(endpoint.received_flow_ids.size(), 1U);
    ASSERT_EQ(observer->observations.size(), 2U);
    EXPECT_EQ(observer->observations[1].lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
    EXPECT_EQ(observer->observations[1].route_hops_completed, 2U);
}

TEST(RnicCollectivePacketTest, PoolReuseResetsInheritedPacketState) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();

    RnicCollectivePacket* first = RnicCollectivePacket::newDeclare(
        flow, route, 7, 0x100000003ULL, 2, 7, 64, declareMetadata(), observer);
    const std::uint64_t first_lifecycle = first->lifecycleId();
    first->set_flags(0xfeedU);
    first->set_hop_count(99);
    first->set_direction(UP);
    first->set_src(100);
    first->set_dst(101);
    first->set_next_hop(&endpoint);
    first->free();

    RnicCollectivePacket* reused = RnicCollectivePacket::newDeclare(
        flow, route, 8, 0x100000004ULL, 3, 8, 72, declareMetadata(), observer);
    EXPECT_EQ(reused, first);
    EXPECT_NE(reused->lifecycleId(), first_lifecycle);
    EXPECT_EQ(reused->id(), 8U);
    EXPECT_EQ(reused->size(), 72U);
    EXPECT_EQ(reused->flags(), 0U);
    EXPECT_EQ(reused->hop_count(), 0U);
    EXPECT_EQ(reused->get_direction(), NONE);
    EXPECT_EQ(reused->nexthop(), 0U);
    EXPECT_FALSE(reused->bounced());
    EXPECT_FALSE(reused->header_only());
    EXPECT_EQ(reused->src(), 3U);
    EXPECT_EQ(reused->dst(), 8U);
    EXPECT_EQ(reused->route(), &route);
    EXPECT_EQ(reused->ref_count(), 1);

    reused->sendOn();
    ASSERT_EQ(observer->observations.size(), 4U);
    EXPECT_EQ(observer->observations[3].lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
}

TEST(RnicCollectivePacketTest, CarriesTypedGrantMetadataAtHighPriority) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();
    const RnicCollectiveGrant grant{
        0xabcdef0123456789ULL, 11, 37, 8000000000ULL, 123456,
        RnicCollectiveGrantKind::Update, 124000, 130000, true,
    };

    RnicCollectivePacket* packet =
        RnicCollectivePacket::newGrantUpdate(flow, route, 91, 9, 4, 80, grant, observer);

    EXPECT_EQ(packet->kind(), RnicCollectivePacketKind::GRANT_UPDATE);
    EXPECT_EQ(packet->priority(), Packet::PRIO_HI);
    EXPECT_EQ(packet->atlahsFlowId(), grant.flow_id);
    EXPECT_EQ(packet->grant().membership_epoch, 11U);
    EXPECT_EQ(packet->grant().n_hat, 37U);
    EXPECT_EQ(packet->grant().wire_rate_bps, 8000000000ULL);
    EXPECT_EQ(packet->grant().effective_time_ps, 123456U);
    EXPECT_EQ(packet->grant().kind, RnicCollectiveGrantKind::Update);
    EXPECT_TRUE(packet->grant().marked_data_ack);

    packet->sendOn();
    ASSERT_EQ(observer->observations.size(), 2U);
    EXPECT_EQ(observer->observations[1].lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);

    const RnicCollectiveGrant accept{
        grant.flow_id, 12, 38, 7000000000ULL, 223456,
        RnicCollectiveGrantKind::Accept, 200000, 300000, false,
    };
    RnicCollectivePacket* accept_packet =
        RnicCollectivePacket::newAccept(flow, route, 93, 9, 4, 80, accept, observer);
    EXPECT_EQ(accept_packet->kind(), RnicCollectivePacketKind::ACCEPT);
    EXPECT_EQ(accept_packet->grant().kind, RnicCollectiveGrantKind::Accept);
    EXPECT_FALSE(accept_packet->grant().marked_data_ack);
    accept_packet->sendOn();
    ASSERT_EQ(observer->observations.size(), 4U);
    EXPECT_EQ(observer->observations[3].lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
}

TEST(RnicCollectivePacketTest, RetireCarriesTheFinalLedgerAndGapDetectionDeadline) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();
    const RnicCollectiveFinalLedger ledger = twoPacketLedger();
    const RnicCollectiveRetireMetadata retire{ledger, 424242};

    RnicCollectivePacket* packet = RnicCollectivePacket::newRetire(flow, route, 92, 0x200000003ULL,
                                                                   4, 9, 64, retire, observer);

    EXPECT_EQ(packet->kind(), RnicCollectivePacketKind::RETIRE);
    EXPECT_EQ(packet->finalLedger().total_payload_bytes, 1500U);
    EXPECT_EQ(packet->finalLedger().total_wire_bytes, 1628U);
    EXPECT_EQ(packet->finalLedger().total_data_packets, 2U);
    EXPECT_EQ(packet->retire().final_gap_detection_ps, 424242U);
    EXPECT_THROW(packet->data(), std::logic_error);
    packet->sendOn();
    EXPECT_EQ(observer->observations.back().lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
}

TEST(RnicCollectivePacketTest, GapNackCarriesExactLogicalRangeAtHighPriority) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();
    const RnicCollectiveGapNackMetadata gap{7, 7000, RnicPacketExtent(936, 1000), 2};

    RnicCollectivePacket* packet =
        RnicCollectivePacket::newGapNack(flow, route, 94, 0x200000004ULL, 9, 4, 64, gap, observer);

    EXPECT_EQ(packet->kind(), RnicCollectivePacketKind::GAP_NACK);
    EXPECT_EQ(packet->priority(), Packet::PRIO_HI);
    EXPECT_EQ(packet->gapNack().packet_index, 7U);
    EXPECT_EQ(packet->gapNack().payload_byte_offset, 7000U);
    EXPECT_EQ(packet->gapNack().extent.payloadBytes(), 936U);
    EXPECT_EQ(packet->gapNack().extent.wireBytes(), 1000U);
    EXPECT_EQ(packet->gapNack().requested_transmission_attempt, 2U);
    EXPECT_THROW(packet->data(), std::logic_error);
    packet->sendOn();
    EXPECT_EQ(observer->observations.back().lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
}

TEST(RnicCollectivePacketTest, GapResolvedPhysicallyClosesTheExactRetryRangeAtHighPriority) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();
    const RnicCollectiveGapResolvedMetadata resolved{7, 7000, RnicPacketExtent(936, 1000), 2};

    RnicCollectivePacket* packet = RnicCollectivePacket::newGapResolved(
        flow, route, 95, 0x200000005ULL, 9, 4, 64, resolved, observer);

    EXPECT_EQ(packet->kind(), RnicCollectivePacketKind::GAP_RESOLVED);
    EXPECT_EQ(packet->priority(), Packet::PRIO_HI);
    EXPECT_EQ(packet->gapResolved().packet_index, 7U);
    EXPECT_EQ(packet->gapResolved().payload_byte_offset, 7000U);
    EXPECT_EQ(packet->gapResolved().extent.payloadBytes(), 936U);
    EXPECT_EQ(packet->gapResolved().extent.wireBytes(), 1000U);
    EXPECT_EQ(packet->gapResolved().acknowledged_transmission_attempt, 2U);
    EXPECT_THROW(packet->gapNack(), std::logic_error);
    packet->sendOn();
    EXPECT_EQ(observer->observations.back().lifecycle,
              RnicCollectivePacketLifecycle::ENDPOINT_CONSUMED);
}

TEST(RnicCollectivePacketTest, RejectsLossyWireAndLedgerConversions) {
    PacketFlow flow(nullptr);
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();

    EXPECT_THROW(
        RnicCollectivePacket::newDeclare(flow, route, 1, 1, 0, 1, 0, declareMetadata(), observer),
        std::invalid_argument);
    EXPECT_THROW(RnicCollectivePacket::newDeclare(
                     flow, route, 1, 1, 0, 1,
                     static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max()) + 1,
                     declareMetadata(), observer),
                 std::invalid_argument);
    EXPECT_THROW(
        RnicCollectivePacket::newDeclare(flow, route, 1, 1, 0, 1, 64, declareMetadata(0), observer),
        std::invalid_argument);

    const RnicCollectiveDataMetadata oversized{
        0,
        0,
        RnicPacketExtent(1,
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max()) + 1),
        1,
        {1, static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max()) + 1, 1},
    };
    EXPECT_THROW(RnicCollectivePacket::newData(flow, route, 1, 1, 0, 1, oversized, observer),
                 std::invalid_argument);

    const RnicCollectiveDataMetadata inconsistent_final{0, 0, RnicPacketExtent(1500, 1564), 1,
                                                        twoPacketLedger()};
    EXPECT_THROW(
        RnicCollectivePacket::newData(flow, route, 1, 1, 0, 1, inconsistent_final, observer),
        std::invalid_argument);

    RnicCollectiveGrant wrong_kind{
        1, 1, 1, 1, 10, RnicCollectiveGrantKind::Accept, 5, 20, false};
    EXPECT_THROW(
        RnicCollectivePacket::newGrantUpdate(flow, route, 1, 1, 0, 64, wrong_kind, observer),
        std::invalid_argument);
    EXPECT_TRUE(observer->observations.empty());
}

TEST(RnicCollectivePacketTest, RequiresObserverAndNonemptyExplicitRoute) {
    PacketFlow flow(nullptr);
    Route empty_route;
    ConsumingEndpoint endpoint;
    Route route = routeTo(endpoint);
    auto observer = std::make_shared<RecordingObserver>();

    EXPECT_THROW(RnicCollectivePacket::newDeclare(flow, empty_route, 1, 1, 0, 1, 64,
                                                  declareMetadata(), observer),
                 std::invalid_argument);
    EXPECT_THROW(
        RnicCollectivePacket::newDeclare(flow, route, 1, 1, 0, 1, 64, declareMetadata(), nullptr),
        std::invalid_argument);
    EXPECT_TRUE(observer->observations.empty());
}

}  // namespace
