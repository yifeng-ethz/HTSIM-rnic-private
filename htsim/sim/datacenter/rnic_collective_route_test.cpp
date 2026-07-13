// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "fat_tree_topology.h"
#include "rnic_collective_route.h"
#include "rnic_packet_extent.h"
#include "tomahawk3_switch.h"

namespace {

class RecordingSink : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        packet_ids.push_back(packet.id());
        arrival_times_ps.push_back(EventList::now());
    }

    const string& nodename() override { return _name; }

    std::vector<packetid_t> packet_ids;
    std::vector<std::uint64_t> arrival_times_ps;

private:
    string _name{"RnicCollectiveRouteTestSink"};
};

class TestPacket : public Packet {
public:
    TestPacket(PacketFlow& flow,
               const Route& route,
               packetid_t packet_id,
               std::uint32_t wire_bytes = 100) {
        set_route(
            flow, route, static_cast<int>(wire_bytes), packet_id);
    }

    PktPriority priority() const override { return PRIO_LO; }
    void free() override { dropped = true; }

    bool dropped = false;
};

class LegacyRouteOwner {
public:
    explicit LegacyRouteOwner(std::vector<const Route*>* routes)
        : _routes(routes) {}

    ~LegacyRouteOwner() {
        if (_routes == nullptr) {
            return;
        }
        for (const Route* route : *_routes) {
            delete route;
        }
        delete _routes;
    }

    const std::vector<const Route*>& routes() const { return *_routes; }

private:
    std::vector<const Route*>* _routes;
};

class TwoTierTomahawk3Topology {
public:
    TwoTierTomahawk3Topology()
        : event_list(EventList::getTheEventList()),
          config(2,
                 32,
                 speedFromGbps(100),
                 32768,
                 timeFromNs(1000),
                 0,
                 COMPOSITE,
                 FAIR_PRIO) {
        config.set_switch_model(FatTreeSwitchModel::Tomahawk3);
        config.set_tomahawk3_shared_buffer_capacity(65536);
        topology = std::make_unique<FatTreeTopology>(
            &config, nullptr, &event_list, nullptr);
    }

    EventList& event_list;
    FatTreeTopologyCfg config;
    std::unique_ptr<FatTreeTopology> topology;
};

void drainAllEvents() {
    while (EventList::doNextEvent()) {
    }
}

void expectNoLoadTransit(
        TwoTierTomahawk3Topology& fixture,
        RnicCollectiveRouteProvider& provider,
        RecordingSink& endpoint,
        std::uint32_t source,
        std::uint32_t destination,
        const RnicPacketExtent& extent,
        packetid_t packet_id) {
    const auto& routes = provider.routes(source, destination, endpoint);
    ASSERT_FALSE(routes.empty());

    PacketFlow flow(nullptr);
    TestPacket packet(
        flow,
        *routes.front(),
        packet_id,
        static_cast<std::uint32_t>(extent.wireBytes()));
    const std::uint64_t injection_time_ps = EventList::now();
    const std::uint64_t calibrated_transit_ps =
        rnicCollectiveNoQueueTransitPs(
            fixture.config, source, destination, extent);
    packet.sendOn();
    drainAllEvents();

    ASSERT_FALSE(endpoint.arrival_times_ps.empty());
    EXPECT_FALSE(packet.dropped);
    EXPECT_EQ(endpoint.arrival_times_ps.back() - injection_time_ps,
              calibrated_transit_ps);
}

TEST(RnicCollectiveRouteProviderTest,
     OmitsOnlySourceQueueAndPreservesDeterministicPhysicalSuffixes) {
    TwoTierTomahawk3Topology fixture;
    constexpr std::uint32_t source = 0;
    constexpr std::uint32_t destination = 31;
    ASSERT_NE(fixture.config.HOST_POD_SWITCH(source),
              fixture.config.HOST_POD_SWITCH(destination));

    LegacyRouteOwner legacy(fixture.topology->get_bidir_paths(
        source, destination, false));
    RecordingSink endpoint;
    RnicCollectiveRouteProvider provider(*fixture.topology);
    const auto& routes = provider.routes(source, destination, endpoint);

    ASSERT_EQ(routes.size(), legacy.routes().size());
    ASSERT_FALSE(routes.empty());
    for (std::size_t path_index = 0;
         path_index < routes.size();
         ++path_index) {
        const Route& original = *legacy.routes().at(path_index);
        const Route& suffix = *routes.at(path_index);
        ASSERT_GE(original.size(), 2U);
        ASSERT_EQ(suffix.size(), original.size());
        EXPECT_EQ(suffix.at(0), original.at(1));
        EXPECT_NE(suffix.at(0), original.at(0));
        for (std::size_t sink_index = 1;
             sink_index + 1 < suffix.size();
             ++sink_index) {
            EXPECT_EQ(suffix.at(sink_index), original.at(sink_index + 1));
        }
        EXPECT_EQ(suffix.at(suffix.size() - 1), &endpoint);
        EXPECT_EQ(suffix.path_id(), static_cast<int>(path_index));
        EXPECT_EQ(suffix.no_of_paths(), static_cast<int>(routes.size()));
        EXPECT_NE(dynamic_cast<Pipe*>(suffix.at(0)), nullptr);
    }

    const auto& cached = provider.routes(source, destination, endpoint);
    EXPECT_EQ(&cached, &routes);
    EXPECT_EQ(cached, routes);
    EXPECT_EQ(provider.cachedPairCount(), 1U);
}

TEST(RnicCollectiveRouteProviderTest,
     SourceSerializedRouteTraversesTheClosAndReachesSharedEndpoint) {
    TwoTierTomahawk3Topology fixture;
    RecordingSink endpoint;
    RnicCollectiveRouteProvider provider(*fixture.topology);
    const auto& routes = provider.routes(0, 31, endpoint);
    ASSERT_FALSE(routes.empty());

    PacketFlow flow(nullptr);
    TestPacket packet(flow, *routes.front(), 7);
    packet.sendOn();
    drainAllEvents();

    EXPECT_FALSE(packet.dropped);
    EXPECT_EQ(endpoint.packet_ids, (std::vector<packetid_t>{7}));
}

TEST(RnicCollectiveRouteProviderTest,
     SameTorNoLoadArrivalAgeMatchesFullAndShortTailCalibration) {
    TwoTierTomahawk3Topology fixture;
    RecordingSink endpoint;
    RnicCollectiveRouteProvider provider(*fixture.topology);
    ASSERT_EQ(fixture.config.HOST_POD_SWITCH(0),
              fixture.config.HOST_POD_SWITCH(1));

    expectNoLoadTransit(
        fixture, provider, endpoint, 0, 1, RnicPacketExtent(936, 1000), 8);
    expectNoLoadTransit(
        fixture, provider, endpoint, 0, 1, RnicPacketExtent(13, 77), 9);

    EXPECT_EQ(endpoint.packet_ids,
              (std::vector<packetid_t>{8, 9}));
    EXPECT_EQ(endpoint.arrival_times_ps[1]
                  - endpoint.arrival_times_ps[0],
              timeFromNs(1000) * 2 + 77 * 80);
}

TEST(RnicCollectiveRouteProviderTest,
     CrossTorNoLoadArrivalAgeMatchesFullAndShortTailCalibration) {
    TwoTierTomahawk3Topology fixture;
    RecordingSink endpoint;
    RnicCollectiveRouteProvider provider(*fixture.topology);
    ASSERT_NE(fixture.config.HOST_POD_SWITCH(0),
              fixture.config.HOST_POD_SWITCH(31));

    expectNoLoadTransit(
        fixture, provider, endpoint, 0, 31, RnicPacketExtent(936, 1000), 10);
    expectNoLoadTransit(
        fixture, provider, endpoint, 0, 31, RnicPacketExtent(13, 77), 11);

    EXPECT_EQ(endpoint.packet_ids,
              (std::vector<packetid_t>{10, 11}));
    EXPECT_EQ(endpoint.arrival_times_ps[1]
                  - endpoint.arrival_times_ps[0],
              timeFromNs(1000) * 4 + 3 * 77 * 80);
}

TEST(RnicCollectiveRouteProviderTest, RejectsInvalidPairsAndEndpointRebinding) {
    TwoTierTomahawk3Topology fixture;
    RecordingSink first_endpoint;
    RecordingSink second_endpoint;
    RnicCollectiveRouteProvider provider(*fixture.topology);

    EXPECT_THROW(provider.routes(0, 0, first_endpoint), std::invalid_argument);
    EXPECT_THROW(provider.routes(32, 1, first_endpoint), std::out_of_range);
    EXPECT_THROW(provider.routes(1, 32, first_endpoint), std::out_of_range);

    static_cast<void>(provider.routes(0, 31, first_endpoint));
    EXPECT_THROW(provider.routes(0, 31, second_endpoint),
                 std::invalid_argument);
    EXPECT_THROW(provider.routes(1, 31, second_endpoint),
                 std::invalid_argument);
}

TEST(RnicCollectiveRouteProviderTest, RejectsNonTomahawkAndNonTwoTierFabrics) {
    EventList& event_list = EventList::getTheEventList();

    FatTreeTopologyCfg default_config(
        2, 32, speedFromGbps(100), 32768, timeFromNs(1000), 0,
        COMPOSITE, FAIR_PRIO);
    FatTreeTopology default_topology(
        &default_config, nullptr, &event_list, nullptr);
    EXPECT_THROW(
        static_cast<void>(RnicCollectiveRouteProvider{default_topology}),
        std::invalid_argument);

    FatTreeTopologyCfg three_tier_config(
        3, 128, speedFromGbps(100), 32768, timeFromNs(1000), 0,
        COMPOSITE, FAIR_PRIO);
    three_tier_config.set_switch_model(FatTreeSwitchModel::Tomahawk3);
    three_tier_config.set_tomahawk3_shared_buffer_capacity(65536);
    FatTreeTopology three_tier_topology(
        &three_tier_config, nullptr, &event_list, nullptr);
    EXPECT_THROW(
        static_cast<void>(RnicCollectiveRouteProvider{three_tier_topology}),
        std::invalid_argument);
}

}  // namespace
