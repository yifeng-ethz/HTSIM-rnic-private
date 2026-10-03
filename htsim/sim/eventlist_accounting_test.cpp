#include "eventlist.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "network.h"
#include "pipe.h"
#include "queue.h"
#include "trigger.h"

// Counter-limit injection does not require billions of allocated records.
// Empty-list resets isolate tests; they never erase pending callback ownership.
class EventListAccountingTestPeer {
public:
    static void resetEmpty(simtime_picosec now = 0) {
        if (!EventList::_pendingsources.empty() || !EventList::_pending_triggers.empty()) {
            throw std::logic_error("test reset requires a drained EventList");
        }
        EventList::_lasteventtime = now;
        EventList::_endtime = 0;
        EventList::_trafficeventcount = 0;
    }
    static void seedCount(int count) { EventList::_trafficeventcount = count; }
    static size_t triggerCount() { return EventList::_pending_triggers.size(); }
};

namespace {

using Entry = std::pair<simtime_picosec, EventSource*>;

std::vector<Entry> pending() {
    const auto raw = EventList::getPendingSources();
    return {raw.begin(), raw.end()};
}

void census() {
    int expected = 0;
    for (const auto& entry : EventList::getPendingSources()) {
        expected += entry.second->isTraffic() ? 1 : 0;
    }
    EXPECT_EQ(EventList::trafficEventCount(), expected);
}

void expectPending(std::vector<Entry> expected) {
    auto actual = pending();
    const auto order = [](const Entry& a, const Entry& b) {
        return a.first != b.first ? a.first < b.first
                                 : std::less<EventSource*>{}(a.second, b.second);
    };
    std::sort(expected.begin(), expected.end(), order);
    std::sort(actual.begin(), actual.end(), order);
    EXPECT_EQ(actual, expected);
    census();
}

struct Snapshot {
    std::vector<Entry> entries{pending()};
    simtime_picosec now{EventList::now()};
    int count{EventList::trafficEventCount()};
    size_t triggers{EventListAccountingTestPeer::triggerCount()};
};

void unchanged(const Snapshot& before) {
    EXPECT_EQ(pending(), before.entries);
    EXPECT_EQ(EventList::now(), before.now);
    EXPECT_EQ(EventList::trafficEventCount(), before.count);
    EXPECT_EQ(EventListAccountingTestPeer::triggerCount(), before.triggers);
}

class Probe final : public EventSource {
public:
    explicit Probe(bool traffic = true)
        : EventSource(EventList::getTheEventList(), "accounting-probe"), traffic_(traffic) {}
    bool isTraffic() override { ++queries; return traffic_; }
    void doNextEvent() override {
        ++calls;
        if (on_event) on_event();
    }
    uint64_t calls{0};
    uint64_t queries{0};
    std::function<void()> on_event;
private:
    const bool traffic_;
};

class TriggerProbe final : public TriggerTarget {
public:
    void activate() override { ++calls; if (on_event) on_event(); }
    uint64_t calls{0};
    std::function<void()> on_event;
};

void drain(size_t max_events = 100000) {
    size_t events = 0;
    while (EventList::doNextEvent()) {
        census();
        if (++events > max_events) {
            ADD_FAILURE() << "unexpected unbounded event chain";
            return;
        }
    }
    EXPECT_TRUE(pending().empty());
    EXPECT_EQ(EventListAccountingTestPeer::triggerCount(), 0U);
    EXPECT_EQ(EventList::trafficEventCount(), 0);
}

class EventListAccountingTest : public ::testing::Test {
protected:
    void SetUp() override {
        EventList::getTheEventList();
        EventListAccountingTestPeer::resetEmpty();
    }
    void TearDown() override {
        EXPECT_TRUE(pending().empty());
        EXPECT_EQ(EventListAccountingTestPeer::triggerCount(), 0U);
        EXPECT_EQ(EventList::trafficEventCount(), 0);
    }
};

TEST_F(EventListAccountingTest, StartupNontrafficHandle) {
    Probe timer(false);
    const auto handle = EventList::sourceIsPendingGetHandle(timer, 1);
    EXPECT_NE(handle, EventList::nullHandle());
    expectPending({{1, &timer}});
    drain();
    EXPECT_EQ(timer.calls, 1U);
    EXPECT_EQ(EventList::now(), 1U);
}

TEST_F(EventListAccountingTest, MixedInsertionMultiplicity) {
    Probe traffic, timer(false);
    auto before_queries = traffic.queries;
    EventList::sourceIsPending(traffic, 2);
    EXPECT_EQ(traffic.queries - before_queries, 1U);
    expectPending({{2, &traffic}});
    EventList::sourceIsPendingRel(traffic, 2);
    before_queries = traffic.queries;
    const auto handle = EventList::sourceIsPendingGetHandle(traffic, 2);
    EXPECT_EQ(traffic.queries - before_queries, 1U);
    EXPECT_NE(handle, EventList::nullHandle());
    EventList::sourceIsPending(timer, 1);
    expectPending({{1, &timer}, {2, &traffic}, {2, &traffic}, {2, &traffic}});
    drain();
    EXPECT_EQ(traffic.calls, 3U);
    EXPECT_EQ(timer.calls, 1U);
}

TEST_F(EventListAccountingTest, SourceCancellationRemovesOne) {
    Probe source, other, absent;
    EventList::sourceIsPending(source, 3);
    EventList::sourceIsPending(source, 1);
    EventList::sourceIsPending(source, 1);
    EventList::sourceIsPending(other, 1);
    EventList::cancelPendingSource(source);
    expectPending({{1, &source}, {1, &other}, {3, &source}});
    const Snapshot before;
    EventList::cancelPendingSource(absent);
    unchanged(before);
    drain();
    EXPECT_EQ(source.calls, 2U);
    EXPECT_EQ(other.calls, 1U);
}

TEST_F(EventListAccountingTest, TimeCancellationRemovesOne) {
    Probe source, other;
    EventList::sourceIsPending(source, 2);
    EventList::sourceIsPending(source, 2);
    EventList::sourceIsPending(source, 4);
    EventList::sourceIsPending(other, 2);
    EventList::cancelPendingSourceByTime(source, 2);
    expectPending({{2, &source}, {2, &other}, {4, &source}});
    drain();
    EXPECT_EQ(source.calls, 2U);
    EXPECT_EQ(other.calls, 1U);
}

TEST_F(EventListAccountingTest, HandleCancellationRemovesExactDuplicate) {
    Probe source, other;
    const auto first = EventList::sourceIsPendingGetHandle(source, 2);
    const auto second = EventList::sourceIsPendingGetHandle(source, 2);
    const auto unrelated = EventList::sourceIsPendingGetHandle(other, 2);
    EventList::cancelPendingSourceByHandle(source, second);
    EXPECT_EQ(first->second, &source);
    EXPECT_EQ(unrelated->second, &other);
    expectPending({{2, &source}, {2, &other}});
    EventList::cancelPendingSourceByHandle(source, first);
    expectPending({{2, &other}});
    EventList::cancelPendingSourceByHandle(other, unrelated);
    expectPending({});
}

TEST_F(EventListAccountingTest, ReschedulePreservesCancelOnePolicy) {
    Probe source;
    EventList::sourceIsPending(source, 1);
    EventList::sourceIsPending(source, 3);
    EventList::reschedulePendingSource(source, 2);
    expectPending({{2, &source}, {3, &source}});
    EventList::setEndtime(4);
    EventList::reschedulePendingSource(source, 4);
    expectPending({{3, &source}});
    EXPECT_EQ(EventList::now(), 0U);
    drain();
    EXPECT_EQ(source.calls, 1U);
}

TEST_F(EventListAccountingTest, ExclusiveEndTimeDoesNotPruneExistingEvents) {
    Probe source;
    EventList::setEndtime(10);
    EventList::sourceIsPending(source, 9);
    EventList::sourceIsPendingRel(source, 9);
    EXPECT_NE(EventList::sourceIsPendingGetHandle(source, 9), EventList::nullHandle());
    expectPending({{9, &source}, {9, &source}, {9, &source}});
    const Snapshot before;
    EventList::sourceIsPending(source, 10);
    EventList::sourceIsPending(source, 11);
    EventList::sourceIsPendingRel(source, 10);
    EventList::sourceIsPendingRel(source, 11);
    EXPECT_EQ(EventList::sourceIsPendingGetHandle(source, 10), EventList::nullHandle());
    EXPECT_EQ(EventList::sourceIsPendingGetHandle(source, 11), EventList::nullHandle());
    unchanged(before);
    EventList::setEndtime(8);
    expectPending(before.entries);
    drain();
    EXPECT_EQ(source.calls, 3U);
    EXPECT_EQ(EventList::now(), 9U);
}

TEST_F(EventListAccountingTest, NontrafficAdmissionPoliciesRemainDistinct) {
    Probe tick, traffic, timer(false);
    EventList::sourceIsPending(tick, 1);
    drain();
    EXPECT_EQ(EventList::sourceIsPendingGetHandle(timer, 2), EventList::nullHandle());
    EventList::sourceIsPending(timer, 2);
    expectPending({{2, &timer}});
    EventList::sourceIsPending(traffic, 3);
    EXPECT_NE(EventList::sourceIsPendingGetHandle(timer, 2), EventList::nullHandle());
    expectPending({{2, &timer}, {2, &timer}, {3, &traffic}});
    EventList::cancelPendingSource(traffic);
    const Snapshot before;
    EXPECT_EQ(EventList::sourceIsPendingGetHandle(timer, 2), EventList::nullHandle());
    unchanged(before);
    drain();
    EXPECT_EQ(timer.calls, 2U);
    EXPECT_EQ(traffic.calls, 0U);
}

TEST_F(EventListAccountingTest, RecursiveSameTimeTrafficOwnsNewRecords) {
    Probe parent, first, second, grandchild, timer(false);
    parent.on_event = [&] {
        expectPending({});
        EXPECT_EQ(EventList::now(), 4U);
        EventList::sourceIsPending(first, EventList::now());
        EXPECT_NE(EventList::sourceIsPendingGetHandle(second, EventList::now()), EventList::nullHandle());
        EventList::sourceIsPending(timer, EventList::now());
        expectPending({{4, &first}, {4, &second}, {4, &timer}});
    };
    first.on_event = [&] {
        expectPending({{4, &second}, {4, &timer}});
        EventList::sourceIsPending(grandchild, EventList::now());
        expectPending({{4, &second}, {4, &timer}, {4, &grandchild}});
    };
    EventList::sourceIsPending(parent, 4);
    drain();
    EXPECT_EQ(parent.calls, 1U);
    EXPECT_EQ(first.calls, 1U);
    EXPECT_EQ(second.calls, 1U);
    EXPECT_EQ(grandchild.calls, 1U);
    EXPECT_EQ(timer.calls, 1U);
    EXPECT_EQ(EventList::now(), 4U);
}

TEST_F(EventListAccountingTest, ImmediateTriggersDoNotChargeTraffic) {
    TriggerProbe repeated, descendant;
    Probe traffic;
    repeated.on_event = [&] {
        EXPECT_EQ(EventList::now(), 0U);
        census();
        if (repeated.calls == 1) {
            EventList::sourceIsPending(traffic, EventList::now());
            EventList::triggerIsPending(descendant);
        }
    };
    EventList::triggerIsPending(repeated);
    EventList::triggerIsPending(repeated);
    EXPECT_EQ(EventList::trafficEventCount(), 0);
    EXPECT_TRUE(pending().empty());
    EXPECT_EQ(EventList::nextEventTime(), 0U);
    drain();
    EXPECT_EQ(repeated.calls, 2U);
    EXPECT_EQ(descendant.calls, 1U);
    EXPECT_EQ(traffic.calls, 1U);
    EXPECT_EQ(EventList::now(), 0U);
}

class RealPacket final : public Packet {
public:
    PktPriority priority() const override { return PRIO_LO; }
};

class ArrivalSink final : public PacketSink {
public:
    void receivePacket(Packet& packet) override {
        arrivals.emplace_back(packet.id(), EventList::now());
        census();
    }
    const string& nodename() override { return name; }
    std::vector<std::pair<packetid_t, simtime_picosec>> arrivals;
    string name{"accounting-arrival-sink"};
};

TEST_F(EventListAccountingTest, RealQueuePipeAndFiniteSampler) {
    EventListAccountingTestPeer::resetEmpty(timeFromUs(40u));
    const auto start = EventList::now();
    auto& events = EventList::getTheEventList();
    Queue queue(8000000000ULL, 16000, events, nullptr);
    Pipe pipe(timeFromUs(2u), events);
    ArrivalSink sink;
    Route route;
    route.push_back(&queue);
    route.push_back(&pipe);
    route.push_back(&sink);
    PacketFlow flow(nullptr);
    std::array<RealPacket, 8> packets;
    for (size_t i = 0; i < packets.size(); ++i) {
        packets[i].set_route(flow, route, 1000, static_cast<packetid_t>(i));
        packets[i].sendOn();
        expectPending({{start + timeFromUs(1u), &queue}});
    }
    Probe sampler(false);
    sampler.on_event = [&] {
        census();
        EXPECT_LE(EventList::trafficEventCount(), 2);
        EventList::sourceIsPendingRel(sampler, timeFromUs(1u));
    };
    EventList::setEndtime(start + timeFromUs(12u));
    EventList::sourceIsPendingRel(sampler, timeFromUs(1u));
    drain();
    ASSERT_EQ(sink.arrivals.size(), packets.size());
    for (size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(sink.arrivals[i].first, i);
        EXPECT_EQ(sink.arrivals[i].second, start + timeFromUs(static_cast<uint32_t>(i + 3)));
    }
    EXPECT_EQ(sampler.calls, 11U);
    EXPECT_EQ(queue.queuesize(), 0U);
    EXPECT_EQ(EventList::now(), start + timeFromUs(11u));
}

TEST_F(EventListAccountingTest, LongLifetimeKeepsOneLiveTrafficRecord) {
    constexpr uint64_t callbacks = 65536;
    constexpr simtime_picosec interval = 100000000000ULL;
    Probe source;
    source.on_event = [&] {
        expectPending({});
        EXPECT_EQ(EventList::now(), source.calls * interval);
        if (source.calls < callbacks) {
            EventList::sourceIsPendingRel(source, interval);
            expectPending({{(source.calls + 1) * interval, &source}});
        }
    };
    EventList::sourceIsPending(source, interval);
    drain(callbacks);
    EXPECT_EQ(source.calls, callbacks);
    EXPECT_EQ(EventList::now(), callbacks * interval);
    EXPECT_GT(EventList::now(), timeFromSec(1));
}

TEST_F(EventListAccountingTest, ThrowingCallbackRemainsRemoved) {
    Probe failed, later;
    failed.on_event = [] { throw std::runtime_error("deliberate callback failure"); };
    EventList::sourceIsPending(failed, 1);
    EventList::sourceIsPending(later, 2);
    EXPECT_THROW(EventList::doNextEvent(), std::runtime_error);
    expectPending({{2, &later}});
    EXPECT_EQ(EventList::now(), 1U);
    EXPECT_EQ(failed.calls, 1U);
    EXPECT_EQ(later.calls, 0U);
    drain();
    EXPECT_EQ(later.calls, 1U);
}

TEST_F(EventListAccountingTest, PlainOverflowPreservesAllOwnership) {
    Probe source;
    TriggerProbe trigger;
    EventList::triggerIsPending(trigger);
    EventListAccountingTestPeer::seedCount(std::numeric_limits<int>::max());
    const Snapshot before;
    EXPECT_THROW(EventList::sourceIsPending(source, 1), std::overflow_error);
    unchanged(before);
    EXPECT_EQ(source.calls, 0U);
    EventListAccountingTestPeer::seedCount(0);
    drain();
    EXPECT_EQ(trigger.calls, 1U);
}

TEST_F(EventListAccountingTest, HandleOverflowExposesNoRecord) {
    Probe source;
    TriggerProbe trigger;
    EventList::triggerIsPending(trigger);
    EventListAccountingTestPeer::seedCount(std::numeric_limits<int>::max());
    const Snapshot before;
    std::optional<EventList::Handle> result;
    EXPECT_THROW(result = EventList::sourceIsPendingGetHandle(source, 1), std::overflow_error);
    EXPECT_FALSE(result.has_value());
    unchanged(before);
    EXPECT_EQ(source.calls, 0U);
    EventListAccountingTestPeer::seedCount(0);
    drain();
    EXPECT_EQ(trigger.calls, 1U);
}

TEST_F(EventListAccountingTest, RemovalUnderflowPreservesRecordHandleAndClock) {
    Probe source;
    for (const int seed : {0, std::numeric_limits<int>::min()}) {
        for (int operation = 0; operation < 4; ++operation) {
            const auto handle = EventList::sourceIsPendingGetHandle(source, 5);
            EventListAccountingTestPeer::seedCount(seed);
            const Snapshot before;
            if (operation == 0) { EXPECT_THROW(EventList::doNextEvent(), std::logic_error); }
            if (operation == 1) { EXPECT_THROW(EventList::cancelPendingSource(source), std::logic_error); }
            if (operation == 2) { EXPECT_THROW(EventList::cancelPendingSourceByTime(source, 5), std::logic_error); }
            if (operation == 3) { EXPECT_THROW(EventList::cancelPendingSourceByHandle(source, handle), std::logic_error); }
            unchanged(before);
            EXPECT_EQ(handle->first, 5U);
            EXPECT_EQ(handle->second, &source);
            EXPECT_EQ(source.calls, 0U);
            EventListAccountingTestPeer::seedCount(1);
            EventList::cancelPendingSourceByHandle(source, handle);
            expectPending({});
        }
    }
}

TEST_F(EventListAccountingTest, AdjacentCounterLimitsAndNontraffic) {
    Probe traffic, timer(false);
    const auto maximum = std::numeric_limits<int>::max();
    EventListAccountingTestPeer::seedCount(maximum - 1);
    const auto handle = EventList::sourceIsPendingGetHandle(traffic, 1);
    EXPECT_EQ(EventList::trafficEventCount(), maximum);
    EXPECT_EQ(pending().size(), 1U);
    EventList::cancelPendingSourceByHandle(traffic, handle);
    EXPECT_EQ(EventList::trafficEventCount(), maximum - 1);
    EXPECT_TRUE(pending().empty());
    EventListAccountingTestPeer::seedCount(maximum);
    EventList::sourceIsPending(timer, 1);
    const auto timer_handle = EventList::sourceIsPendingGetHandle(timer, 2);
    EXPECT_EQ(EventList::trafficEventCount(), maximum);
    EXPECT_TRUE(EventList::doNextEvent());
    EXPECT_EQ(timer.calls, 1U);
    EXPECT_EQ(EventList::trafficEventCount(), maximum);
    EventList::cancelPendingSourceByHandle(timer, timer_handle);
    EXPECT_TRUE(pending().empty());
    EventListAccountingTestPeer::seedCount(std::numeric_limits<int>::min());
    const Snapshot before;
    EXPECT_THROW(EventList::sourceIsPending(traffic, 2), std::logic_error);
    EXPECT_THROW(EventList::sourceIsPendingGetHandle(traffic, 2), std::logic_error);
    unchanged(before);
    EventListAccountingTestPeer::seedCount(0);
    census();
}

TEST_F(EventListAccountingTest, DeterministicMixedOperationLedger) {
    std::array<std::unique_ptr<Probe>, 8> sources;
    for (size_t i = 0; i < sources.size(); ++i) sources[i] = std::make_unique<Probe>(i % 2 == 0);
    struct Record {
        simtime_picosec time;
        size_t source;
        uint64_t sequence;
        std::optional<EventList::Handle> handle;
    };
    std::vector<Record> ledger;
    uint64_t sequence = 0;
    uint32_t random = 0x1935827U;
    const auto draw = [&] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    const auto earlier = [](const Record& a, const Record& b) {
        return a.time != b.time ? a.time < b.time : a.sequence < b.sequence;
    };
    const auto verify = [&] {
        std::vector<Entry> expected;
        for (const auto& record : ledger) expected.emplace_back(record.time, sources[record.source].get());
        expectPending(std::move(expected));
    };
    for (size_t step = 0; step < 512; ++step) {
        SCOPED_TRACE(step);
        const size_t source = draw() % sources.size();
        const int operation = draw() % 7;
        const auto when = EventList::now() + 1 + draw() % 9;
        auto& probe = *sources[source];
        if (operation < 3) {
            std::optional<EventList::Handle> handle;
            bool accepted = true;
            if (operation == 0) EventList::sourceIsPending(probe, when);
            if (operation == 1) EventList::sourceIsPendingRel(probe, when - EventList::now());
            if (operation == 2) {
                const bool eligible = source % 2 == 0 || EventList::now() == 0 ||
                    std::any_of(ledger.begin(), ledger.end(), [](const Record& r) { return r.source % 2 == 0; });
                const auto actual = EventList::sourceIsPendingGetHandle(probe, when);
                accepted = actual != EventList::nullHandle();
                EXPECT_EQ(accepted, eligible);
                if (accepted) handle = actual;
            }
            if (accepted) ledger.push_back({when, source, sequence++, handle});
        } else if (operation == 3 || operation == 6) {
            auto first = ledger.end();
            for (auto i = ledger.begin(); i != ledger.end(); ++i) {
                if (i->source == source && (first == ledger.end() || earlier(*i, *first))) first = i;
            }
            if (operation == 3) EventList::cancelPendingSource(probe);
            else EventList::reschedulePendingSource(probe, when);
            if (first != ledger.end()) ledger.erase(first);
            if (operation == 6) ledger.push_back({when, source, sequence++, std::nullopt});
        } else if (operation == 4 && !ledger.empty()) {
            auto selected = ledger.begin() + draw() % ledger.size();
            if (selected->handle) {
                EventList::cancelPendingSourceByHandle(*sources[selected->source], *selected->handle);
                ledger.erase(selected);
            } else {
                const auto chosen = *selected;
                auto first = ledger.end();
                for (auto i = ledger.begin(); i != ledger.end(); ++i) {
                    if (i->source == chosen.source && i->time == chosen.time &&
                        (first == ledger.end() || i->sequence < first->sequence)) first = i;
                }
                EventList::cancelPendingSourceByTime(*sources[chosen.source], chosen.time);
                ledger.erase(first);
            }
        } else if (operation == 5) {
            const auto before = EventList::now();
            if (ledger.empty()) {
                EXPECT_FALSE(EventList::doNextEvent());
                EXPECT_EQ(EventList::now(), before);
            } else {
                const auto first = std::min_element(ledger.begin(), ledger.end(), earlier);
                const auto expected = *first;
                const auto calls = sources[expected.source]->calls;
                ledger.erase(first);
                EXPECT_TRUE(EventList::doNextEvent());
                EXPECT_EQ(EventList::now(), expected.time);
                EXPECT_EQ(sources[expected.source]->calls, calls + 1);
            }
        }
        verify();
    }
    while (!ledger.empty()) {
        const auto first = std::min_element(ledger.begin(), ledger.end(), earlier);
        const auto expected = *first;
        const auto calls = sources[expected.source]->calls;
        ledger.erase(first);
        EXPECT_TRUE(EventList::doNextEvent());
        EXPECT_EQ(EventList::now(), expected.time);
        EXPECT_EQ(sources[expected.source]->calls, calls + 1);
        verify();
    }
    EXPECT_FALSE(EventList::doNextEvent());
}

}  // namespace
