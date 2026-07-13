// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "fat_tree_topology.h"
#include "rnic_collective_route.h"
#include "tomahawk3_switch.h"

namespace {

class RecordingSink : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        packet_ids.push_back(packet.id());
    }

    const string& nodename() override { return _name; }

    std::vector<packetid_t> packet_ids;

private:
    string _name{"RnicCollectiveRouteTestSink"};
};

class TestPacket : public Packet {
public:
    TestPacket(PacketFlow& flow, const Route& route, packetid_t packet_id) {
        set_route(flow, route, 100, packet_id);
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
