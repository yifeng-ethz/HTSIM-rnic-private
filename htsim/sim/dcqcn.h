// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        


#ifndef DCQCN_H
#define DCQCN_H

/*
 * A DCQCN source and sink
 */

#include <cstdint>
#include <list>
#include <map>
#include <functional>
//#include "util.h"
#include "math.h"
#include "config.h"
#include "network.h"
#include "rocepacket.h"
#include "cnppacket.h"
#include "queue.h"
#include "roce.h"
#include "eventlist.h"
#include "eth_pause_packet.h"
#include "trigger.h"
#include "ecn.h"

#include <optional>
#include <stdexcept>
#define timeInf 0

class DCQCNSink;
class Switch;

class DCQCNSrc : public RoceSrc {
    friend class DCQCNSink;
public:
    using StateObserver = std::function<void(const char*)>;
    DCQCNSrc(RoceLogger* logger, TrafficLogger* pktlogger, EventList &eventlist, linkspeed_bps rate);
    ~DCQCNSrc() override = default;

    virtual void receivePacket(Packet& pkt);
    virtual void processCNP(const CNPPacket& cnp);    
    virtual void increaseRate();
    void processAck(const RoceAck& ack) override;
    void processNack(const RoceNack& nack) override;

    // Comparator-realism ruling: any loss recovery event (go-back-N
    // rewind, selective retransmission, or silent RTO) drives the same
    // alpha-based multiplicative cut the rate machine applies for a CNP.
    // The -loss_rate_cut flag isolates this coupling.
    void applyLossRateCut(const char* observer_event);
    static void setLossRateCut(bool enabled) {
        _loss_rate_cut_enabled = enabled;
    }
    static bool lossRateCutEnabled() noexcept {
        return _loss_rate_cut_enabled;
    }
    std::uint64_t loss_rate_cut_count() const noexcept {
        return _loss_rate_cuts;
    }

    // should really be private, but loggers want to see:
    uint32_t _cnps_received;    

    static simtime_picosec _cc_update_period;
    static double _g;
    static uint32_t _F;
    static linkspeed_bps _RAI, _RHAI;
    static uint64_t _B;
    static linkspeed_bps _minimum_rate_bps;
    static void setMinRate(linkspeed_bps rate) {
        if (rate == 0) {
            throw std::invalid_argument(
                "DCQCN minimum rate must be positive");
        }
        _minimum_rate_bps = rate;
    }
    static linkspeed_bps minRate() noexcept {
        return _minimum_rate_bps;
    }
    linkspeed_bps current_rate() const noexcept { return _RC; }
    double alpha() const noexcept { return _alpha; }
    bool cc_timer_pending() const noexcept;
    simtime_picosec cc_timer_time() const;
    std::uint64_t cc_timer_fire_count() const noexcept;
    std::uint64_t byte_counter_rate_update_count() const noexcept {
        return _byte_counter_rate_updates;
    }
    void setStateObserver(StateObserver observer) {
        _state_observer = std::move(observer);
    }

private:
    class CcTimer final : public EventSource {
    public:
        CcTimer(DCQCNSrc& owner, EventList& eventlist);
        ~CcTimer() override;

        void arm(simtime_picosec when);
        void cancel();
        void doNextEvent() override;
        bool pending() const noexcept { return _handle.has_value(); }
        simtime_picosec pendingTime() const;
        std::uint64_t fireCount() const noexcept { return _fire_count; }

    private:
        DCQCNSrc& _owner;
        std::optional<EventList::Handle> _handle;
        std::uint64_t _fire_count{0};
    };

    void ccTimerExpired();
    void send_packet() override;
    void rateCut(const char* observer_event);

    simtime_picosec _last_cc_update, _last_alpha_update;
    linkspeed_bps _RC, _RT, _link;
    double _alpha;
    bool _cc_active;
    
    enum increase_state {invalid = 0, fast_recovery=1,active_increase=2};
    //increase_state _ai_state;
    uint16_t _T,_BC;
    uint64_t _byte_counter;
    std::uint64_t _byte_counter_rate_updates{0};
    std::uint64_t _loss_rate_cuts{0};
    static bool _loss_rate_cut_enabled;
    CcTimer _cc_timer;
    StateObserver _state_observer;

};

class DCQCNSink : public RoceSink, public EventSource {
    friend class DCQCNSrc;
public:
    DCQCNSink(EventList &eventlist);
    ~DCQCNSink() override;
    virtual void doNextEvent();

    virtual void receivePacket(Packet& pkt);  
    static simtime_picosec _cnp_interval;
    bool cnp_timer_pending() const noexcept {
        return _cnp_timer.has_value();
    }

    inline id_t get_id() const {return RoceSink::get_id();}

private:
    simtime_picosec _last_cnp_sent_time;
 
    uint32_t _marked_packets_since_last_cnp;
    uint32_t _packets_since_last_cnp;

    // Mechanism
    void send_cnp();
    void armCnpTimer(simtime_picosec when);
    void cancelCnpTimer();
    std::optional<EventList::Handle> _cnp_timer;
};

#endif
