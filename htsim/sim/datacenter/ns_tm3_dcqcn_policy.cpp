// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "ns_tm3_dcqcn_policy.h"

#include "ecn.h"
#include "eth_pause_packet.h"
#include "eventlist.h"
#include "pipe.h"
#include "queue.h"
#include "rocepacket.h"
#include "route.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t kPartsPerMillion = 1000000;

std::uint64_t splitmix64(std::uint64_t value) {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

simtime_picosec pfcSerializationTime(linkspeed_bps link_rate_bps) {
    constexpr std::uint64_t kPauseWireBits = UINT64_C(64) * 8;
    constexpr std::uint64_t kPicosecondsPerSecond = UINT64_C(1000000000000);
    const std::uint64_t numerator =
        kPauseWireBits * kPicosecondsPerSecond;
    return static_cast<simtime_picosec>(
        (numerator + link_rate_bps - 1) / link_rate_bps);
}

}  // namespace

// A PFC frame travels against the observed DATA ingress.  HTSIM's explicit
// forward route does not provide a reverse queue for that hop, so this models
// one dedicated, link-local control serializer per physical ingress followed
// by the real cable propagation delay.  It intentionally does not share its
// serializer with reverse DATA/control traffic; the manifest records that
// approximation.
class NsTm3PfcReverseLink final : public EventSource, public PacketSink {
public:
    NsTm3PfcReverseLink(EventList& event_list,
                        simtime_picosec propagation_delay_ps,
                        linkspeed_bps link_rate_bps,
                        PacketSink& upstream)
        : EventSource(event_list, "ns-tm3 PFC reverse serializer"),
          _propagation_delay_ps(propagation_delay_ps),
          _serialization_ps(pfcSerializationTime(link_rate_bps)),
          _upstream(upstream) {}

    ~NsTm3PfcReverseLink() override {
        if (_event_handle.has_value()) {
            eventlist().cancelPendingSourceByHandle(*this, *_event_handle);
            _event_handle.reset();
        }
        for (const InFlightFrame& frame : _in_flight) {
            frame.packet->free();
        }
    }

    void receivePacket(Packet& packet) override {
        if (packet.type() != ETH_PAUSE || packet.size() != PAUSESIZE) {
            throw std::invalid_argument(
                "ns-tm3 reverse PFC serializer accepts only 64-byte PFC frames");
        }
        const simtime_picosec now = eventlist().now();
        const simtime_picosec start =
            std::max(now, _serializer_available_ps);
        if (start > std::numeric_limits<simtime_picosec>::max()
                        - _serialization_ps) {
            throw std::overflow_error("ns-tm3 PFC serialization overflow");
        }
        const simtime_picosec serializer_end = start + _serialization_ps;
        if (serializer_end > std::numeric_limits<simtime_picosec>::max()
                                 - _propagation_delay_ps) {
            throw std::overflow_error("ns-tm3 PFC propagation overflow");
        }
        const simtime_picosec arrival =
            serializer_end + _propagation_delay_ps;
        _serializer_available_ps = serializer_end;
        _in_flight.push_back(InFlightFrame{arrival, &packet});
        if (_in_flight.size() == 1) {
            arm(arrival);
        }
    }

    void doNextEvent() override {
        _event_handle.reset();
        if (_in_flight.empty()
            || _in_flight.front().arrival_ps != eventlist().now()) {
            throw std::logic_error(
                "ns-tm3 reverse PFC serializer fired off boundary");
        }
        Packet* packet = _in_flight.front().packet;
        _in_flight.pop_front();
        _upstream.receivePacket(*packet);
        if (!_in_flight.empty()) {
            arm(_in_flight.front().arrival_ps);
        }
    }

    const std::string& nodename() override { return _name; }

private:
    struct InFlightFrame {
        simtime_picosec arrival_ps;
        Packet* packet;
    };

    void arm(simtime_picosec when) {
        if (_event_handle.has_value()) {
            throw std::logic_error(
                "ns-tm3 reverse PFC serializer armed twice");
        }
        const EventList::Handle handle =
            eventlist().sourceIsPendingGetHandle(*this, when);
        if (handle != EventList::nullHandle()) {
            _event_handle = handle;
        }
    }

    simtime_picosec _propagation_delay_ps;
    simtime_picosec _serialization_ps;
    PacketSink& _upstream;
    simtime_picosec _serializer_available_ps{0};
    std::deque<InFlightFrame> _in_flight;
    std::optional<EventList::Handle> _event_handle;
    std::string _name{"ns-tm3-pfc-reverse-serializer"};
};

NsTm3DcqcnPolicy::NsTm3DcqcnPolicy(
        EventList& event_list,
        NsTm3DcqcnPolicyConfig config,
        std::uint64_t ecn_domain_id)
    : _event_list(event_list),
      _config(config),
      _ecn_domain_id(ecn_domain_id) {
    if (_config.ecn_enabled
        && (_config.ecn_kmin_bytes < 0
            || _config.ecn_kmax_bytes <= _config.ecn_kmin_bytes
            || _config.ecn_pmax_ppm == 0
            || _config.ecn_pmax_ppm > kPartsPerMillion)) {
        throw std::invalid_argument(
            "ns-tm3 DCQCN RED requires 0 <= kmin < kmax and "
            "0 < pmax <= 1000000 ppm");
    }
    if (_config.pfc_enabled
        && (_config.pfc_low_threshold_bytes <= 0
            || _config.pfc_high_threshold_bytes <= 0
            || _config.pfc_low_threshold_bytes
                   >= _config.pfc_high_threshold_bytes
            || _config.pfc_link_rate_bps == 0)) {
        throw std::invalid_argument(
            "ns-tm3 DCQCN PFC requires 0 < low < high and a link rate");
    }
}

NsTm3DcqcnPolicy::~NsTm3DcqcnPolicy() = default;

void NsTm3DcqcnPolicy::observe_physical_ingress(
        Packet& packet, std::uint32_t ingress_id) {
    if (!_config.pfc_enabled) {
        return;
    }
    const Route* route = packet.route();
    const std::uint32_t next_hop = packet.nexthop();
    if (route == nullptr || next_hop < 3 || next_hop > route->size()) {
        throw std::logic_error(
            "ns-tm3 DCQCN PFC requires explicit physical routes");
    }

    auto* propagation_pipe =
        dynamic_cast<Pipe*>(route->at(next_hop - 2));
    auto* upstream_egress =
        dynamic_cast<BaseQueue*>(route->at(next_hop - 3));
    if (propagation_pipe == nullptr || upstream_egress == nullptr) {
        throw std::logic_error(
            "ns-tm3 DCQCN ingress lacks an upstream egress and wire");
    }

    IngressState& ingress = ingress_state(ingress_id);
    if (ingress.upstream_egress == nullptr) {
        ingress.upstream_egress = upstream_egress;
        ingress.reverse_wire = std::make_unique<NsTm3PfcReverseLink>(
            _event_list,
            propagation_pipe->delay(),
            _config.pfc_link_rate_bps,
            *upstream_egress);
        return;
    }
    if (ingress.upstream_egress != upstream_egress) {
        throw std::logic_error(
            "ns-tm3 physical ingress changed its upstream egress");
    }
}

void NsTm3DcqcnPolicy::packet_enqueued(
        Packet& packet, std::uint32_t ingress_id) {
    if (!_config.pfc_enabled || packet.priority() != Packet::PRIO_LO) {
        return;
    }
    IngressState& ingress = ingress_state(ingress_id);
    if (ingress.upstream_egress == nullptr || ingress.reverse_wire == nullptr) {
        throw std::logic_error(
            "ns-tm3 DCQCN admitted DATA before learning its physical link");
    }
    if (packet.size()
        > static_cast<std::uint64_t>(
            std::numeric_limits<mem_b>::max() - ingress.data_buffered_bytes)) {
        throw std::overflow_error("ns-tm3 DCQCN ingress meter overflow");
    }
    ingress.data_buffered_bytes += packet.size();
    _counters.max_ingress_buffered_bytes = std::max(
        _counters.max_ingress_buffered_bytes,
        ingress.data_buffered_bytes);
    if (!ingress.data_paused
        && ingress.data_buffered_bytes
               >= _config.pfc_high_threshold_bytes) {
        ingress.data_paused = true;
        send_pfc(ingress, true);
    }
}

void NsTm3DcqcnPolicy::packet_selected(
        Packet& packet,
        std::uint32_t ingress_id,
        std::uint32_t egress_id,
        mem_b egress_buffered_bytes) {
    if (_config.ecn_enabled
        && packet.type() == ROCE
        && should_mark_ecn(
            packet, ingress_id, egress_id, egress_buffered_bytes)) {
        packet.set_flags(packet.flags() | ECN_CE);
        ++_counters.ecn_marked_packets;
    }

    if (!_config.pfc_enabled || packet.priority() != Packet::PRIO_LO) {
        return;
    }
    IngressState& ingress = ingress_state(ingress_id);
    if (ingress.data_buffered_bytes < packet.size()) {
        throw std::logic_error("ns-tm3 DCQCN ingress meter underflow");
    }
    ingress.data_buffered_bytes -= packet.size();
    if (ingress.data_paused
        && ingress.data_buffered_bytes
               <= _config.pfc_low_threshold_bytes) {
        ingress.data_paused = false;
        send_pfc(ingress, false);
    }
}

bool NsTm3DcqcnPolicy::should_mark_ecn(
        const Packet& packet,
        std::uint32_t ingress_id,
        std::uint32_t egress_id,
        mem_b egress_buffered_bytes) const {
    if (egress_buffered_bytes <= _config.ecn_kmin_bytes) {
        return false;
    }

    std::uint32_t probability_ppm = _config.ecn_pmax_ppm;
    if (egress_buffered_bytes < _config.ecn_kmax_bytes) {
        const std::uint64_t offset = static_cast<std::uint64_t>(
            egress_buffered_bytes - _config.ecn_kmin_bytes);
        const std::uint64_t span = static_cast<std::uint64_t>(
            _config.ecn_kmax_bytes - _config.ecn_kmin_bytes);
        probability_ppm = static_cast<std::uint32_t>(
            (static_cast<__uint128_t>(_config.ecn_pmax_ppm) * offset)
            / span);
    }
    if (probability_ppm == 0) {
        return false;
    }

    // Flow plus wire sequence is stable across allocator reuse and unrelated
    // event ordering, unlike Packet::id().
    const auto& roce = static_cast<const RocePacket&>(packet);
    std::uint64_t sample = splitmix64(
        _config.ecn_seed ^ UINT64_C(0x6a09e667f3bcc909));
    sample = splitmix64(sample ^ _ecn_domain_id);
    sample = splitmix64(sample ^ packet.flow_id());
    sample = splitmix64(sample ^ roce.seqno());
    sample = splitmix64(
        sample ^ (static_cast<std::uint64_t>(egress_id) << 32)
        ^ ingress_id);
    return sample % kPartsPerMillion < probability_ppm;
}

mem_b NsTm3DcqcnPolicy::ingress_buffered_bytes(
        std::uint32_t ingress_id) const {
    return ingress_state(ingress_id).data_buffered_bytes;
}

NsTm3DcqcnPolicy::IngressState& NsTm3DcqcnPolicy::ingress_state(
        std::uint32_t ingress_id) {
    if (ingress_id >= _ingresses.size()) {
        _ingresses.resize(static_cast<std::size_t>(ingress_id) + 1);
    }
    return _ingresses[ingress_id];
}

const NsTm3DcqcnPolicy::IngressState& NsTm3DcqcnPolicy::ingress_state(
        std::uint32_t ingress_id) const {
    if (ingress_id >= _ingresses.size()) {
        throw std::out_of_range("unknown ns-tm3 DCQCN ingress");
    }
    return _ingresses[ingress_id];
}

void NsTm3DcqcnPolicy::send_pfc(
        IngressState& ingress, bool pause) {
    if (ingress.reverse_wire == nullptr) {
        throw std::logic_error(
            "ns-tm3 DCQCN cannot send PFC before link discovery");
    }
    // The legacy HTSIM PFC model uses a nonzero quanta value as a pause
    // marker; duration is controlled by an explicit RESUME frame.
    EthPausePacket* packet = EthPausePacket::newpkt(pause ? 1 : 0, 0);
    if (pause) {
        // Cascade attribution (comparator-realism ruling): a pause emitted
        // while an egress of the owning switch is itself paused extends
        // that chain by one; otherwise this is a root pause of depth one.
        const std::uint32_t upstream_depth =
            _active_egress_pause_depth_provider
                ? _active_egress_pause_depth_provider()
                : 0;
        if (upstream_depth
            >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error(
                "ns-tm3 PFC cascade depth overflow");
        }
        const std::uint32_t depth = upstream_depth + 1;
        packet->setCascadeDepth(depth);
        ++ingress.pause_frames;
        ingress.paused_since_ps = _event_list.now();
        ingress.max_pause_cascade_depth =
            std::max(ingress.max_pause_cascade_depth, depth);
        ++_counters.pause_frames;
        _counters.max_pause_cascade_depth =
            std::max(_counters.max_pause_cascade_depth, depth);
    } else {
        const simtime_picosec now = _event_list.now();
        if (now < ingress.paused_since_ps) {
            throw std::logic_error(
                "ns-tm3 PFC resume precedes its pause");
        }
        const simtime_picosec held = now - ingress.paused_since_ps;
        ingress.paused_wall_ps += held;
        _counters.paused_wall_ps += held;
        ++ingress.resume_frames;
        ++_counters.resume_frames;
    }
    ingress.reverse_wire->receivePacket(*packet);
}

std::vector<NsTm3DcqcnPfcPortMetrics>
NsTm3DcqcnPolicy::pfc_port_metrics() const {
    std::vector<NsTm3DcqcnPfcPortMetrics> metrics;
    for (std::uint32_t ingress_id = 0; ingress_id < _ingresses.size();
         ++ingress_id) {
        const IngressState& ingress = _ingresses[ingress_id];
        if (ingress.pause_frames == 0) {
            continue;
        }
        metrics.push_back({ingress_id, ingress.pause_frames,
                           ingress.resume_frames, ingress.paused_wall_ps,
                           ingress.max_pause_cascade_depth});
    }
    return metrics;
}
