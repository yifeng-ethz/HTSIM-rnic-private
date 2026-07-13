// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "eventlist.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

class ProbeEventSource final : public EventSource {
public:
    explicit ProbeEventSource(EventList& event_list)
        : EventSource(event_list, "eventlist-microphase-probe") {}

    void doNextEvent() override { ++dispatch_count; }

    std::uint64_t dispatch_count = 0;
};

void drainEvents() {
    while (EventList::doNextEvent()) {
    }
}

TEST(EventListMicrophaseTest, ReportsOnlyTheExactPendingTimestamp) {
    EventList& event_list = EventList::getTheEventList();
    drainEvents();
    ProbeEventSource at_now(event_list);
    ProbeEventSource later(event_list);
    const simtime_picosec now_ps = EventList::now();

    EventList::sourceIsPending(at_now, now_ps);
    EventList::sourceIsPending(later, now_ps + 1);

    EXPECT_TRUE(EventList::hasPendingSourceAt(now_ps));
    EXPECT_TRUE(EventList::hasPendingSourceAt(now_ps + 1));
    EXPECT_FALSE(EventList::hasPendingSourceAt(now_ps + 2));
    ASSERT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(at_now.dispatch_count, 1u);
    EXPECT_FALSE(EventList::hasPendingSourceAt(now_ps));
    EXPECT_TRUE(EventList::hasPendingSourceAt(now_ps + 1));
    drainEvents();
}

class YieldingEventSource final : public EventSource {
public:
    explicit YieldingEventSource(EventList& event_list)
        : EventSource(event_list, "eventlist-microphase-yield") {}

    void doNextEvent() override {
        ++dispatch_count;
        if (EventList::hasPendingSourceAt(EventList::now())) {
            EventList::sourceIsPending(*this, EventList::now());
            return;
        }
        committed = true;
    }

    std::uint64_t dispatch_count = 0;
    bool committed = false;
};

TEST(EventListMicrophaseTest, SourceCanYieldUntilSameTimeEventsQuiesce) {
    EventList& event_list = EventList::getTheEventList();
    drainEvents();
    YieldingEventSource barrier(event_list);
    ProbeEventSource feedback_one(event_list);
    ProbeEventSource feedback_two(event_list);
    const simtime_picosec boundary_ps = EventList::now();

    // Insert the would-be barrier first. It must still run last.
    EventList::sourceIsPending(barrier, boundary_ps);
    EventList::sourceIsPending(feedback_one, boundary_ps);
    EventList::sourceIsPending(feedback_two, boundary_ps);
    drainEvents();

    EXPECT_EQ(feedback_one.dispatch_count, 1u);
    EXPECT_EQ(feedback_two.dispatch_count, 1u);
    EXPECT_EQ(barrier.dispatch_count, 2u);
    EXPECT_TRUE(barrier.committed);
}

}  // namespace
