#include <gtest/gtest.h>

#include "ecn.h"
#include "eth_pause_packet.h"
#include "eventlist.h"
#include "fat_tree_switch_factory.h"
#include "ns_tm3_dcqcn_policy.h"
#include "ns_tm3_switch.h"
#include "pipe.h"
#include "queue.h"
#include "rocepacket.h"
#include "route.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr linkspeed_bps kLinkRate = UINT64_C(8000000000);
constexpr std::uint16_t kPacketBytes = 100;

class PolicyTestPacket final : public RocePacket {
public:
    PolicyTestPacket(PacketFlow& flow, const Route& route, packetid_t id, PktPriority priority)
        : _priority(priority) {
        set_route(flow, route, kPacketBytes, id);
        _type = ROCE;
        _seqno = static_cast<seq_t>(id) * kPacketBytes;
        _retransmitted = false;
        _last_packet = false;
    }

    PktPriority priority() const override { return _priority; }
    void free() override { dropped = true; }

    bool dropped{false};

private:
    PktPriority _priority;
};

class RecordingSink final : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        ids.push_back(packet.id());
        flags.push_back(packet.flags());
        times.push_back(EventList::now());
    }
    const string& nodename() override { return name; }

    string name{"policy-recording-sink"};
    std::vector<packetid_t> ids;
    std::vector<std::uint32_t> flags;
    std::vector<simtime_picosec> times;
};

class RecordingUpstream final : public BaseQueue {
public:
    explicit RecordingUpstream(EventList& event_list) : BaseQueue(kLinkRate, event_list, nullptr) {
        _nodename = "policy-recording-upstream";
    }

    void receivePacket(Packet& packet) override {
        if (packet.type() == ETH_PAUSE) {
            const auto& pause = static_cast<const EthPausePacket&>(packet);
            pauses.push_back(pause.sleepTime() > 0);
            pause_times.push_back(EventList::now());
            packet.free();
            return;
        }
        packet.sendOn();
    }
    void doNextEvent() override {}
    mem_b queuesize() const override { return 0; }
    mem_b maxsize() const override { return 0; }

    std::vector<bool> pauses;
    std::vector<simtime_picosec> pause_times;
};

struct SwitchHarness {
    explicit SwitchHarness(EventList& event_list)
        : event_list(event_list),
          owner(FatTreeSwitchFactory::create(FatTreeSwitchModel::NsTm3,
                                             event_list,
                                             "policy-test-switch",
                                             FatTreeSwitch::TOR,
                                             0,
                                             0,
                                             nullptr,
                                             4096)),
          traffic_manager(dynamic_cast<NsTm3Switch*>(owner.get())),
          egress(kLinkRate, event_list, nullptr) {
        if (traffic_manager == nullptr) {
            throw std::logic_error("factory did not return ns-tm3");
        }
        traffic_manager->addPort(&egress);
        ingress = dynamic_cast<NsTm3IngressPort*>(
            traffic_manager->create_physical_ingress("policy-ingress"));
        if (ingress == nullptr) {
            throw std::logic_error("factory did not return physical ingress");
        }
    }

    EventList& event_list;
    std::unique_ptr<FatTreeSwitch> owner;
    NsTm3Switch* traffic_manager;
    NsTm3EgressSerializer egress;
    NsTm3IngressPort* ingress{nullptr};
};

void drainEvents() {
    while (EventList::doNextEvent()) {
    }
}

std::vector<bool> redPattern(EventList& event_list,
                             std::uint64_t seed,
                             std::uint32_t egress_id,
                             mem_b queue_bytes,
                             std::size_t packet_count,
                             std::uint64_t ecn_domain_id = 0) {
    NsTm3DcqcnPolicy policy(event_list,
                            NsTm3DcqcnPolicyConfig{true, 100, 300, 500000, seed, false, 0, 0, 0},
                            ecn_domain_id);
    Route route;
    PacketFlow flow(nullptr);
    flow.set_flowid(37);
    std::vector<bool> marked;
    marked.reserve(packet_count);
    for (std::size_t index = 0; index < packet_count; ++index) {
        PolicyTestPacket packet(flow, route, static_cast<packetid_t>(index + 1), Packet::PRIO_LO);
        policy.packet_selected(packet, 11, egress_id, queue_bytes);
        marked.push_back((packet.flags() & ECN_CE) != 0);
    }
    return marked;
}

TEST(NsTm3DcqcnPolicyTest, IsStrictlyOptInByDefault) {
    EventList& event_list = EventList::getTheEventList();
    SwitchHarness harness(event_list);
    EXPECT_EQ(harness.traffic_manager->dcqcn_policy(), nullptr);

    EthPausePacket* pause = EthPausePacket::newpkt(1, 0);
    EXPECT_THROW(harness.ingress->receivePacket(*pause), std::invalid_argument);
    pause->free();

    pause = EthPausePacket::newpkt(1, 0);
    EXPECT_THROW(harness.egress.receivePacket(*pause), std::invalid_argument);
    pause->free();
}

TEST(NsTm3DcqcnPolicyTest, RejectsInitialConfigurationWithPacketInFlight) {
    EventList& event_list = EventList::getTheEventList();
    SwitchHarness harness(event_list);
    RecordingSink sink;
    Route route;
    route.push_back(&harness.egress);
    route.push_back(&sink);
    PacketFlow flow(nullptr);
    PolicyTestPacket packet(flow, route, 1, Packet::PRIO_LO);
    const NsTm3DcqcnPolicyConfig policy{true, 50, 100, 1000000, 1, true, 25, 50, kLinkRate};

    harness.ingress->receivePacket(packet);

    EXPECT_THROW(harness.traffic_manager->configure_dcqcn_policy(policy), std::logic_error);
    drainEvents();
    EXPECT_NO_THROW(harness.traffic_manager->configure_dcqcn_policy(policy));
}

TEST(NsTm3DcqcnPolicyTest, RedIsSeededPerPacketAndEgressAndLinearBetweenThresholds) {
    EventList& event_list = EventList::getTheEventList();
    const std::vector<bool> first = redPattern(event_list, 19, 3, 200, 20000);
    const std::vector<bool> replay = redPattern(event_list, 19, 3, 200, 20000);
    const std::vector<bool> other_seed = redPattern(event_list, 20, 3, 200, 20000);
    const std::vector<bool> other_egress = redPattern(event_list, 19, 4, 200, 20000);
    const std::vector<bool> other_switch = redPattern(event_list, 19, 3, 200, 20000, 1);

    EXPECT_EQ(first, replay);
    EXPECT_NE(first, other_seed);
    EXPECT_NE(first, other_egress);
    EXPECT_NE(first, other_switch);
    const std::size_t marked =
        static_cast<std::size_t>(std::count(first.begin(), first.end(), true));
    // At the midpoint the linear ramp is 250,000 ppm.
    EXPECT_GT(marked, 4700U);
    EXPECT_LT(marked, 5300U);
}

TEST(NsTm3DcqcnPolicyTest, RedHonorsZeroAndPmaxEndpoints) {
    EventList& event_list = EventList::getTheEventList();
    const std::vector<bool> at_kmin = redPattern(event_list, 19, 3, 100, 512);
    EXPECT_EQ(std::count(at_kmin.begin(), at_kmin.end(), true), 0);

    NsTm3DcqcnPolicy policy(event_list,
                            NsTm3DcqcnPolicyConfig{true, 100, 300, 1000000, 19, false, 0, 0, 0});
    Route route;
    PacketFlow flow(nullptr);
    flow.set_flowid(37);
    for (packetid_t id = 1; id <= 512; ++id) {
        PolicyTestPacket packet(flow, route, id, Packet::PRIO_LO);
        policy.packet_selected(packet, 11, 3, 300);
        EXPECT_NE(packet.flags() & ECN_CE, 0U);
    }
}

TEST(NsTm3DcqcnPolicyTest, RejectsInvalidRedRangesAndProbability) {
    EventList& event_list = EventList::getTheEventList();
    EXPECT_THROW(NsTm3DcqcnPolicy(
                     event_list, NsTm3DcqcnPolicyConfig{true, 100, 100, 250000, 1, false, 0, 0, 0}),
                 std::invalid_argument);
    EXPECT_THROW(NsTm3DcqcnPolicy(event_list, NsTm3DcqcnPolicyConfig{true, 100, 300, 1000001, 1,
                                                                     false, 0, 0, 0}),
                 std::invalid_argument);
}

TEST(NsTm3DcqcnPolicyTest, MarksEcnAndDeliversSerializedPhysicalPauseAndResume) {
    EventList& event_list = EventList::getTheEventList();
    SwitchHarness harness(event_list);
    harness.traffic_manager->configure_dcqcn_policy(
        NsTm3DcqcnPolicyConfig{true, 50, 100, 1000000, 1, true, 25, 50, kLinkRate});

    RecordingUpstream upstream(event_list);
    Pipe forward_wire(timeFromNs(100u), event_list);
    RecordingSink sink;
    Route route;
    route.push_back(&upstream);
    route.push_back(&forward_wire);
    route.push_back(harness.ingress);
    route.push_back(&harness.egress);
    route.push_back(&sink);
    PacketFlow flow(nullptr);
    PolicyTestPacket packet(flow, route, 1, Packet::PRIO_LO);

    const simtime_picosec start = EventList::now();
    packet.sendOn();
    drainEvents();

    ASSERT_EQ(sink.ids, (std::vector<packetid_t>{1}));
    ASSERT_EQ(sink.flags.size(), 1U);
    EXPECT_NE(sink.flags.front() & ECN_CE, 0U);
    ASSERT_EQ(upstream.pauses, (std::vector<bool>{true, false}));
    ASSERT_EQ(upstream.pause_times.size(), 2U);
    // 100 ns forward propagation before admission, then a dedicated reverse
    // serializer (64 ns/frame at 8 Gbit/s) followed by 100 ns propagation.
    // PAUSE and RESUME occupy consecutive wire slots and cannot co-arrive.
    EXPECT_EQ(upstream.pause_times[0], start + timeFromNs(264u));
    EXPECT_EQ(upstream.pause_times[1], start + timeFromNs(328u));
    const NsTm3DcqcnPolicyCounters& counters = harness.traffic_manager->dcqcn_policy()->counters();
    EXPECT_EQ(counters.ecn_marked_packets, 1U);
    EXPECT_EQ(counters.pause_frames, 1U);
    EXPECT_EQ(counters.resume_frames, 1U);
}

TEST(NsTm3DcqcnPolicyTest, PfcMetersIngressesIndependentlyWhileEcnSeesAggregateEgressBacklog) {
    EventList& event_list = EventList::getTheEventList();
    SwitchHarness harness(event_list);
    auto* second_ingress = dynamic_cast<NsTm3IngressPort*>(
        harness.traffic_manager->create_physical_ingress("policy-ingress-1"));
    ASSERT_NE(second_ingress, nullptr);
    harness.traffic_manager->configure_dcqcn_policy(
        NsTm3DcqcnPolicyConfig{true, 250, 300, 1000000, 1, true, 100, 250, kLinkRate});

    RecordingUpstream upstream_0(event_list);
    RecordingUpstream upstream_1(event_list);
    Pipe wire_0(timeFromNs(1u), event_list);
    Pipe wire_1(timeFromNs(1u), event_list);
    RecordingSink sink;
    Route route_0;
    route_0.push_back(&upstream_0);
    route_0.push_back(&wire_0);
    route_0.push_back(harness.ingress);
    route_0.push_back(&harness.egress);
    route_0.push_back(&sink);
    Route route_1;
    route_1.push_back(&upstream_1);
    route_1.push_back(&wire_1);
    route_1.push_back(second_ingress);
    route_1.push_back(&harness.egress);
    route_1.push_back(&sink);
    PacketFlow flow_0(nullptr);
    PacketFlow flow_1(nullptr);
    flow_0.set_flowid(40);
    flow_1.set_flowid(41);
    PolicyTestPacket first_0(flow_0, route_0, 1, Packet::PRIO_LO);
    PolicyTestPacket second_0(flow_0, route_0, 2, Packet::PRIO_LO);
    PolicyTestPacket first_1(flow_1, route_1, 3, Packet::PRIO_LO);
    PolicyTestPacket second_1(flow_1, route_1, 4, Packet::PRIO_LO);

    // Hold DATA at the shared egress so each ingress contributes 200 bytes.
    // Neither ingress reaches the 250-byte PFC threshold, while the aggregate
    // 400-byte egress backlog is above ECN Kmax.
    harness.egress.receivePacket(*EthPausePacket::newpkt(1, 0));
    first_0.sendOn();
    second_0.sendOn();
    first_1.sendOn();
    second_1.sendOn();
    drainEvents();

    const NsTm3DcqcnPolicy* policy = harness.traffic_manager->dcqcn_policy();
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->ingress_buffered_bytes(0), 200);
    EXPECT_EQ(policy->ingress_buffered_bytes(1), 200);
    EXPECT_EQ(harness.traffic_manager->egress_buffered_bytes(0), 400);
    EXPECT_EQ(harness.traffic_manager->shared_buffer_occupancy(), 400);
    EXPECT_EQ(policy->counters().pause_frames, 0U);
    EXPECT_TRUE(upstream_0.pauses.empty());
    EXPECT_TRUE(upstream_1.pauses.empty());

    harness.egress.receivePacket(*EthPausePacket::newpkt(0, 0));
    drainEvents();
    ASSERT_EQ(sink.flags.size(), 4U);
    EXPECT_NE(sink.flags.front() & ECN_CE, 0U);
    EXPECT_EQ(policy->ingress_buffered_bytes(0), 0);
    EXPECT_EQ(policy->ingress_buffered_bytes(1), 0);
}

TEST(NsTm3DcqcnPolicyTest, DataPauseWaitsForPacketBoundaryAndDoesNotBlockControl) {
    EventList& event_list = EventList::getTheEventList();
    SwitchHarness harness(event_list);
    harness.traffic_manager->configure_dcqcn_policy(
        NsTm3DcqcnPolicyConfig{true, 4096, 8192, 1000000, 1, true, 500, 1000, kLinkRate});
    RecordingUpstream upstream(event_list);
    Pipe forward_wire(timeFromNs(1u), event_list);
    RecordingSink sink;
    Route route;
    route.push_back(&upstream);
    route.push_back(&forward_wire);
    route.push_back(harness.ingress);
    route.push_back(&harness.egress);
    route.push_back(&sink);
    PacketFlow flow(nullptr);
    PolicyTestPacket first(flow, route, 1, Packet::PRIO_LO);
    PolicyTestPacket second(flow, route, 2, Packet::PRIO_LO);
    PolicyTestPacket control(flow, route, 3, Packet::PRIO_HI);

    first.sendOn();
    second.sendOn();
    control.sendOn();
    while (harness.traffic_manager->buffer_counters().admitted_packets < 3) {
        ASSERT_TRUE(EventList::doNextEvent());
    }

    harness.egress.receivePacket(*EthPausePacket::newpkt(1, 0));
    ASSERT_TRUE(EventList::doNextEvent());
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(sink.ids, (std::vector<packetid_t>{1, 3}));
    EXPECT_TRUE(harness.egress.data_is_paused());

    harness.egress.receivePacket(*EthPausePacket::newpkt(0, 0));
    drainEvents();
    EXPECT_EQ(sink.ids, (std::vector<packetid_t>{1, 3, 2}));
}

}  // namespace
