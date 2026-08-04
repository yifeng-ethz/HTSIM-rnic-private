// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NS_TM3_DCQCN_POLICY_H
#define NS_TM3_DCQCN_POLICY_H

#include "config.h"
#include "network.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class BaseQueue;
class EventList;
class NsTm3PfcReverseLink;

// Optional DCQCN behavior layered on the ns-tm3 traffic manager.  The
// default ns-tm3 model never constructs this policy, so its ECN/PFC/drop
// behavior remains unchanged.  PFC is deliberately one-priority: it pauses
// RoCE DATA (PRIO_LO) while ACK/NACK/CNP control traffic (PRIO_HI) remains
// eligible at each packet boundary.
struct NsTm3DcqcnPolicyConfig {
    bool ecn_enabled{true};
    mem_b ecn_kmin_bytes{0};
    mem_b ecn_kmax_bytes{0};
    std::uint32_t ecn_pmax_ppm{0};
    std::uint64_t ecn_seed{1};
    bool pfc_enabled{true};
    mem_b pfc_low_threshold_bytes{0};
    mem_b pfc_high_threshold_bytes{0};
    linkspeed_bps pfc_link_rate_bps{0};
};

struct NsTm3DcqcnPolicyCounters {
    std::uint64_t ecn_marked_packets{0};
    std::uint64_t pause_frames{0};
    std::uint64_t resume_frames{0};
    mem_b max_ingress_buffered_bytes{0};
    // PFC storm observability (comparator-realism ruling): cumulative wall
    // time any of this switch's ingress meters held its upstream paused,
    // and the deepest pause cascade this policy participated in. A pause
    // emitted while one of the owning switch's egresses is itself paused
    // counts as that egress depth plus one.
    simtime_picosec paused_wall_ps{0};
    std::uint32_t max_pause_cascade_depth{0};
};

// Per-ingress-port PFC measurement, reported in the run manifest. Ports
// that never paused are omitted.
struct NsTm3DcqcnPfcPortMetrics {
    std::uint32_t ingress_id;
    std::uint64_t pause_frames;
    std::uint64_t resume_frames;
    simtime_picosec paused_wall_ps;
    std::uint32_t max_pause_cascade_depth;
};

class NsTm3DcqcnPolicy {
public:
    NsTm3DcqcnPolicy(EventList& event_list,
                     NsTm3DcqcnPolicyConfig config,
                     std::uint64_t ecn_domain_id = 0);
    ~NsTm3DcqcnPolicy();

    NsTm3DcqcnPolicy(const NsTm3DcqcnPolicy&) = delete;
    NsTm3DcqcnPolicy& operator=(const NsTm3DcqcnPolicy&) = delete;

    // Learns the physical reverse-link target from the packet's explicit
    // route.  The route segment is always upstream egress, propagation Pipe,
    // then this physical ingress.
    void observe_physical_ingress(Packet& packet, std::uint32_t ingress_id);

    // Admission and selection are the exact ns-tm3 shared-memory boundaries.
    // Only admitted low-priority bytes contribute to the PFC ingress meter.
    void packet_enqueued(Packet& packet, std::uint32_t ingress_id);
    void packet_selected(Packet& packet,
                         std::uint32_t ingress_id,
                         std::uint32_t egress_id,
                         mem_b egress_buffered_bytes);

    const NsTm3DcqcnPolicyConfig& config() const noexcept { return _config; }
    const NsTm3DcqcnPolicyCounters& counters() const noexcept {
        return _counters;
    }
    mem_b ingress_buffered_bytes(std::uint32_t ingress_id) const;

    // The owning switch supplies the depth of its deepest currently paused
    // egress so an emitted pause can be attributed to the chain that caused
    // it. Absent (standalone policies, host-edge tests) every pause is a
    // root pause of depth one.
    using ActiveEgressPauseDepthProvider = std::function<std::uint32_t()>;
    void set_active_egress_pause_depth_provider(
        ActiveEgressPauseDepthProvider provider) {
        _active_egress_pause_depth_provider = std::move(provider);
    }

    // Measurement only; no behavior depends on these values.
    std::vector<NsTm3DcqcnPfcPortMetrics> pfc_port_metrics() const;

private:
    struct IngressState {
        mem_b data_buffered_bytes{0};
        bool data_paused{false};
        BaseQueue* upstream_egress{nullptr};
        std::unique_ptr<NsTm3PfcReverseLink> reverse_wire;
        std::uint64_t pause_frames{0};
        std::uint64_t resume_frames{0};
        simtime_picosec paused_since_ps{0};
        simtime_picosec paused_wall_ps{0};
        std::uint32_t max_pause_cascade_depth{0};
    };

    IngressState& ingress_state(std::uint32_t ingress_id);
    const IngressState& ingress_state(std::uint32_t ingress_id) const;
    bool should_mark_ecn(const Packet& packet,
                         std::uint32_t ingress_id,
                         std::uint32_t egress_id,
                         mem_b egress_buffered_bytes) const;
    void send_pfc(IngressState& ingress, bool pause);

    EventList& _event_list;
    NsTm3DcqcnPolicyConfig _config;
    std::uint64_t _ecn_domain_id;
    NsTm3DcqcnPolicyCounters _counters;
    ActiveEgressPauseDepthProvider _active_egress_pause_depth_provider;
    std::vector<IngressState> _ingresses;
};

#endif  // NS_TM3_DCQCN_POLICY_H
