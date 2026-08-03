#include <gtest/gtest.h>

#include "cnppacket.h"
#include "dcqcn.h"
#include "eventlist.h"
#include "eth_pause_packet.h"
#include "route.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

std::size_t pendingEventsFor(const EventSource& source) {
    std::size_t count = 0;
    for (const auto& pending : EventList::getPendingSources()) {
        if (pending.second == &source) {
            ++count;
        }
    }
    return count;
}

struct CnpObservation {
    std::size_t pacing_events;
    bool pacing_pending;
    bool cc_pending;
    simtime_picosec cc_deadline_ps;
};

class ScheduledCnp final : public EventSource {
public:
    ScheduledCnp(EventList& event_list,
                 DCQCNSrc& source,
                 simtime_picosec when,
                 std::vector<CnpObservation>& observations)
        : EventSource(event_list, "scheduled DCQCN CNP"),
          source_(source),
          flow_(nullptr),
          observations_(observations) {
        event_list.sourceIsPending(*this, when);
    }

    void doNextEvent() override {
        CNPPacket* cnp = CNPPacket::newpkt(flow_, empty_route_, 0);
        source_.processCNP(*cnp);
        cnp->free();
        observations_.push_back(CnpObservation{
            pendingEventsFor(source_),
            source_.pacing_event_pending(),
            source_.cc_timer_pending(),
            source_.cc_timer_time()});
    }

private:
    DCQCNSrc& source_;
    PacketFlow flow_;
    Route empty_route_;
    std::vector<CnpObservation>& observations_;
};

class ScheduledPause final : public EventSource {
public:
    ScheduledPause(EventList& event_list,
                   DCQCNSrc& source,
                   simtime_picosec when,
                   bool pause)
        : EventSource(event_list, "scheduled DCQCN PFC state"),
          source_(source),
          pause_(pause) {
        event_list.sourceIsPending(*this, when);
    }

    void doNextEvent() override {
        EthPausePacket* packet = EthPausePacket::newpkt(pause_ ? 1 : 0, 0);
        source_.processPause(*packet);
        packet->free();
    }

private:
    DCQCNSrc& source_;
    bool pause_;
};

TEST(DcqcnTimerTest,
     RepeatedCnpsKeepOnePacingChainAndOneResettableControlTimer) {
    EventList event_list;
    EventList::setEndtime(timeFromUs(2400u));

    const std::uint64_t original_byte_threshold = DCQCNSrc::_B;
    DCQCNSrc::_B =
        static_cast<std::uint64_t>(Packet::data_packet_size()) * 2;

    DCQCNSrc source(nullptr, nullptr, event_list, UINT64_C(400000000000));
    DCQCNSink sink(event_list);
    source.set_dst(1);
    sink.set_src(0);
    source.set_flowsize(UINT64_C(1) << 40);
    source.set_flowid(1);

    Route forward;
    forward.push_back(&sink);
    Route reverse;
    reverse.push_back(&source);
    source.connect(&forward, &reverse, sink, TRIGGER_START);
    source.startflow();

    std::vector<CnpObservation> observations;
    ScheduledCnp cnp_5us(
        event_list, source, timeFromUs(5u), observations);
    ScheduledCnp cnp_10us(
        event_list, source, timeFromUs(10u), observations);
    ScheduledCnp cnp_20us(
        event_list, source, timeFromUs(20u), observations);
    std::vector<std::unique_ptr<ScheduledCnp>> sustained_cnps;
    for (std::uint32_t time_us = 50; time_us <= 2200; time_us += 50) {
        sustained_cnps.push_back(std::make_unique<ScheduledCnp>(
            event_list, source, timeFromUs(time_us), observations));
    }
    ScheduledPause pause(event_list, source, timeFromUs(2160u), true);
    ScheduledPause resume(event_list, source, timeFromUs(2170u), false);

    PacketFlow immediate_flow(nullptr);
    Route empty_route;
    CNPPacket* immediate = CNPPacket::newpkt(
        immediate_flow, empty_route, 0);
    source.processCNP(*immediate);
    immediate->free();

    ASSERT_TRUE(source.pacing_event_pending());
    EXPECT_EQ(pendingEventsFor(source), 1U);
    ASSERT_TRUE(source.cc_timer_pending());
    EXPECT_EQ(source.cc_timer_time(), timeFromUs(55u));

    while (EventList::doNextEvent()) {
    }

    ASSERT_EQ(observations.size(), 3U + sustained_cnps.size());
    const simtime_picosec expected_deadlines[] = {
        timeFromUs(60u), timeFromUs(65u), timeFromUs(75u)};
    for (std::size_t index = 0; index < observations.size(); ++index) {
        EXPECT_EQ(observations[index].pacing_events, 1U);
        EXPECT_TRUE(observations[index].pacing_pending);
        EXPECT_TRUE(observations[index].cc_pending);
        if (index < 3) {
            EXPECT_EQ(observations[index].cc_deadline_ps,
                      expected_deadlines[index]);
        }
    }
    EXPECT_EQ(source.cc_timer_fire_count(), 3U);
    EXPECT_GT(source.byte_counter_rate_update_count(), 0U);
    EXPECT_GE(source.current_rate(), DCQCNSrc::minRate());
    EXPECT_EQ(CNPPacket::live_packet_count(), 0U);
    EXPECT_EQ(EthPausePacket::live_packet_count(), 0U);

    DCQCNSrc::_B = original_byte_threshold;
}

}  // namespace
