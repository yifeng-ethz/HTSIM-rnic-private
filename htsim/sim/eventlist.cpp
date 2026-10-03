// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        

#include "eventlist.h"
#include "trigger.h"

#include <limits>
#include <stdexcept>

simtime_picosec EventList::_endtime = 0;
simtime_picosec EventList::_lasteventtime = 0;
int EventList::_trafficeventcount = 0;
EventList::pendingsources_t EventList::_pendingsources;
vector <TriggerTarget*> EventList::_pending_triggers;
int EventList::_instanceCount = 0;
EventList* EventList::_theEventList = nullptr;

EventList::EventList()
{
    if (EventList::_instanceCount != 0) 
    {
        std::cerr << "There should be only one instance of EventList. Abort." << std::endl;
        abort();
    }

    EventList::_theEventList = this;
    EventList::_instanceCount += 1;
}

EventList& 
EventList::getTheEventList()
{
    if (EventList::_theEventList == nullptr) 
    {
        EventList::_theEventList = new EventList();
    }
    return *EventList::_theEventList;
}

bool
EventList::hasPendingSourceAt(simtime_picosec when)
{
    const auto range = _pendingsources.equal_range(when);
    return range.first != range.second;
}

std::optional<simtime_picosec>
EventList::nextEventTime()
{
    if (!_pending_triggers.empty()) {
        return now();
    }
    if (_pendingsources.empty()) {
        return std::nullopt;
    }
    return _pendingsources.begin()->first;
}

void
EventList::setEndtime(simtime_picosec endtime)
{
    EventList::_endtime = endtime;
}

bool
EventList::doNextEvent() 
{
    // triggers happen immediately - no time passes; no guarantee that
    // they happen in any particular order (don't assume FIFO or LIFO).
    if (!_pending_triggers.empty()) {
        TriggerTarget *target = _pending_triggers.back();
        _pending_triggers.pop_back();
        target->activate();
        return true;
    }
    
    if (_pendingsources.empty())
        return false;
    
    pendingsources_t::iterator i = _pendingsources.begin();
    simtime_picosec nexteventtime = i->first;
    EventSource* nextsource = i->second;
    removePending(i);
    assert(nexteventtime >= _lasteventtime);
    _lasteventtime = nexteventtime; // set this before calling doNextEvent, so that this::now() is accurate
    nextsource->doNextEvent();
    return true;
}


EventList::Handle
EventList::insertPending(EventSource& src, simtime_picosec when, bool traffic)
{
    int next_count = _trafficeventcount;
    if (traffic) {
        if (next_count < 0) {
            throw std::logic_error("negative EventList traffic count at insertion");
        }
        if (next_count == std::numeric_limits<int>::max()) {
            throw std::overflow_error("EventList pending traffic count overflow");
        }
        ++next_count;
    }
    // Commit the count only after map allocation/insertion succeeds.
    Handle handle = _pendingsources.insert(make_pair(when, &src));
    _trafficeventcount = next_count;
    return handle;
}

void
EventList::removePending(Handle handle)
{
    const bool traffic = handle->second->isTraffic();
    if (traffic) {
        if (_trafficeventcount <= 0) {
            throw std::logic_error("EventList pending traffic count underflow");
        }
        --_trafficeventcount;
    }
    _pendingsources.erase(handle);
}

void
EventList::sourceIsPending(EventSource& src, simtime_picosec when)
{
    assert(when >= now());
    const bool traffic = src.isTraffic();
    if (_endtime == 0 || when < _endtime) {
        insertPending(src, when, traffic);
    }
}

EventList::Handle
EventList::sourceIsPendingGetHandle(EventSource& src, simtime_picosec when)
{
    assert(when >= now());
    const bool traffic = src.isTraffic();
    if ((_endtime == 0 || when < _endtime) &&
        (traffic || _trafficeventcount > 0 || _lasteventtime == 0)) {
        return insertPending(src, when, traffic);
    }
    return _pendingsources.end();
}

void
EventList::triggerIsPending(TriggerTarget &target) {
    _pending_triggers.push_back(&target);
}

void 
EventList::cancelPendingSource(EventSource &src) {
    pendingsources_t::iterator i = _pendingsources.begin();
    while (i != _pendingsources.end()) {
        if (i->second == &src) {
            removePending(i);
            return;
        }
        i++;
    }
}

void 
EventList::cancelPendingSourceByTime(EventSource &src, simtime_picosec when) {
    // fast cancellation of a timer - the timer MUST exist
    // this should normally be fast, except if we have a lot of events with exactly the same time value

    auto range = _pendingsources.equal_range(when);

    for (auto i = range.first; i != range.second; ++i) {
        if (i->second == &src) {
            removePending(i);
            return;
        }
    }
    abort();
}


void EventList::cancelPendingSourceByHandle(EventSource &src, EventList::Handle handle) {
    // If we're cancelling timers often, cancel them by handle.  But
    // be careful - cancelling a handle that has already been
    // cancelled or has already expired is undefined behaviour
    assert(handle->second == &src);
    assert(handle != _pendingsources.end());
    assert(handle->first >= now());
    
    removePending(handle);
}

void 
EventList::reschedulePendingSource(EventSource &src, simtime_picosec when) {
    cancelPendingSource(src);
    sourceIsPending(src, when);
}

EventSource::EventSource(const string& name) : EventSource(EventList::getTheEventList(), name) 
{
}
