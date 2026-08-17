// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef DRAGONFLY_PROGRESSIVE_ROUTING_H
#define DRAGONFLY_PROGRESSIVE_ROUTING_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace htsim {

using DragonflyRouterId = std::uint32_t;
using DragonflyGroupId = std::uint16_t;
using DragonflyPortId = std::uint16_t;

enum class DragonflyLinkKind : std::uint8_t { Local, Global };
enum class DragonflyTableFamily : std::uint8_t {
    GlobalNonMinimal,
    GlobalMinimal,
    LocalNonMinimal,
    LocalMinimal,
};
enum class DragonflyRouteClass : std::uint8_t {
    Undecided,
    Minimal,
    NonMinimal,
};
enum class DragonflyRoutingControl : std::uint8_t {
    Adaptive0,
    Adaptive1,
    Adaptive2,
    Adaptive3,
    DeterministicMinimal,
    DeterministicNonMinimal,
};
enum class DragonflyRoutePhase : std::uint8_t {
    Injection,
    SourceAdaptiveUp,
    MinimalToTargetGroup,
    NonMinimalToIntermediate,
    IntermediateAdaptiveUp,
    IntermediateRoot,
    IntermediateToTargetGroup,
    TargetAdaptiveUp,
    TargetRoot,
    TargetLocal,
    Delivered,
    Dropped,
};

// These are conservative per-hop logical dependency phases for the collapsed
// one-local-dimension model, not a claim that the patent embodiment has eight
// physical VC buffers. Production integration may map them onto a proved
// dimension-aware VC scheme, but may not merge them without a new dependency
// proof. Their numeric order is their dependency order.
enum class DragonflyVirtualChannel : std::uint8_t {
    DependencyHop0 = 0,
    DependencyHop1 = 1,
    DependencyHop2 = 2,
    DependencyHop3 = 3,
    DependencyHop4 = 4,
    DependencyHop5 = 5,
    DependencyHop6 = 6,
    DependencyHop7 = 7,
};

enum class DragonflyDecisionOutcome : std::uint8_t {
    Forward,
    Delivered,
    Dropped,
};

// Link failures are physical state.  Routing-table knowledge about failures is
// separate and changes only when a control advertisement reaches a router.
enum class DragonflyRouteUpdateKind : std::uint8_t {
    DestinationAvailable,
    IntermediateGroupSafe,
};
enum class DragonflyRouteUpdateDisposition : std::uint8_t {
    Accepted,
    NotYetPhysicallyArrived,
    StaleOrDuplicate,
};

struct DragonflyRouteUpdateAdvertisement {
    DragonflyRouterId observer_router{0};
    DragonflyRouteUpdateKind kind{DragonflyRouteUpdateKind::DestinationAvailable};
    DragonflyGroupId group{0};
    bool usable{true};
    std::uint64_t sequence{0};
    std::uint64_t observation_time_ps{0};
    std::uint64_t physical_arrival_time_ps{0};
};
enum class DragonflyDropReason : std::uint8_t {
    None,
    NoLegalCandidate,
    FailedCommittedLink,
    InvalidRouteState,
    HopBudgetExceeded,
};

struct DragonflyPort {
    DragonflyPortId id;
    DragonflyRouterId next_router;
    DragonflyLinkKind kind;
};

// The concrete patent embodiment combines three remapped four-bit signals with
// unsigned saturating addition.  Quantizer thresholds remain caller-selected.
struct DragonflyCongestionComponents {
    std::uint8_t near_end{0};
    std::uint8_t far_end{0};
    std::uint8_t downstream{0};

    std::uint8_t compose() const;
};

struct DragonflyClassBias {
    std::uint8_t shift{0};     // 0, 1, or 2
    std::uint8_t additive{0};  // six-bit value
};

struct DragonflyAdaptiveBias {
    DragonflyClassBias minimal;
    DragonflyClassBias non_minimal;

    void validate() const;
};

struct DragonflyProgressiveConfig {
    std::uint16_t routers_per_group{4};
    std::uint16_t global_links_per_router{2};
    // In the canonical one-local-dimension topology, progressive routing can
    // use one source local hop, three intermediate local hops, and two target
    // local hops in addition to two global hops. The three intermediate hops
    // arise when GM first moves toward the target connector, a later input
    // selects LN, and post-root GM returns to that connector.
    std::uint8_t maximum_router_hops{8};
    std::uint64_t minimum_route_update_delay_ps{1};
    std::array<DragonflyAdaptiveBias, 4> adaptive_biases{};

    void validate() const;
};

// A canonical maximum-size Dragonfly: every group is a local full mesh and
// every pair of groups has exactly one bidirectional global connection.
class DragonflyCanonicalTopology {
public:
    DragonflyCanonicalTopology(std::uint16_t routers_per_group,
                               std::uint16_t global_links_per_router);

    std::uint16_t routersPerGroup() const noexcept { return _a; }
    std::uint16_t globalLinksPerRouter() const noexcept { return _h; }
    DragonflyGroupId groupCount() const noexcept { return _groups; }
    DragonflyRouterId routerCount() const noexcept {
        return static_cast<DragonflyRouterId>(_ports.size());
    }

    DragonflyGroupId groupOf(DragonflyRouterId router) const;
    const std::vector<DragonflyPort>& ports(DragonflyRouterId router) const;
    const DragonflyPort& port(DragonflyRouterId router, DragonflyPortId port_id) const;
    std::optional<DragonflyPortId> portTo(DragonflyRouterId router,
                                          DragonflyRouterId next_router) const;
    std::optional<DragonflyPortId> globalPortToGroup(DragonflyRouterId router,
                                                     DragonflyGroupId group) const;
    DragonflyRouterId groupConnector(DragonflyGroupId source_group,
                                     DragonflyGroupId target_group) const;
    DragonflyPortId reciprocalPort(DragonflyRouterId router, DragonflyPortId port_id) const;

private:
    void validateRouter(DragonflyRouterId router) const;
    void validateGroup(DragonflyGroupId group) const;

    std::uint16_t _a;
    std::uint16_t _h;
    DragonflyGroupId _groups;
    std::vector<std::vector<DragonflyPort>> _ports;
};

struct DragonflyTableEntry {
    DragonflyTableFamily family;
    std::optional<DragonflyPortId> output_port;
    DragonflyRouterId next_router;
    std::optional<DragonflyGroupId> intermediate_group;
    std::optional<DragonflyRouterId> root_router;
    // True for a GM restricted entry or for the collapsed GN hierarchy after
    // its single local dimension has already been traversed.
    bool current_connector_only{false};
    bool root_detect{false};
    // Patent LM-table bit: this entry remains legal for downrouting but is
    // ineligible as the minimal choice during adaptive target-group uprouting.
    bool diverged{false};
};

struct DragonflyCandidate {
    DragonflyTableEntry entry;
    DragonflyRouteClass route_class;
    std::uint8_t congestion;
    std::uint8_t adjusted_score;
};

// This state is carried by a packet. A local adaptive GN/LN output does not
// commit an end-to-end path. The intermediate group becomes immutable only
// when the first global hop physically enters it. An LN choice records the
// pending one-hop root in the state returned for the next input; that input
// authenticates physical arrival through expected_current_router and
// previous_output_port before performing root-detect.
struct DragonflyRouteState {
    DragonflyRouterId source_router{0};
    DragonflyRouterId target_router{0};
    DragonflyRoutingControl control{DragonflyRoutingControl::Adaptive0};
    DragonflyRouteClass route_class{DragonflyRouteClass::Undecided};
    DragonflyRoutePhase phase{DragonflyRoutePhase::Injection};
    std::optional<DragonflyGroupId> intermediate_group;
    std::optional<DragonflyRouterId> intermediate_root_router;
    std::optional<DragonflyRouterId> target_root_router;
    std::optional<DragonflyRouterId> previous_router;
    std::optional<DragonflyPortId> previous_output_port;
    // The switch adapter must present the packet at exactly this router.  This
    // prevents an integration error from teleporting a packet along its route.
    DragonflyRouterId expected_current_router{0};
    std::optional<DragonflyVirtualChannel> last_virtual_channel;
    std::uint64_t route_hash{0};
    std::uint32_t decision_count{0};
    std::uint8_t router_hops{0};
    std::uint8_t global_hops{0};
    std::uint8_t local_hops_in_phase{0};
    // Current UP episode has no legal minimal candidate. This resets on entry
    // to a new group and after root-detect; it is not a sticky route class.
    bool diverged{false};
};

struct DragonflyRoutingDecision {
    DragonflyDecisionOutcome outcome{DragonflyDecisionOutcome::Dropped};
    DragonflyDropReason drop_reason{DragonflyDropReason::None};
    std::optional<DragonflyRouterId> next_router;
    std::optional<DragonflyPortId> output_port;
    std::optional<DragonflyVirtualChannel> virtual_channel;
    std::vector<DragonflyCandidate> candidates;
};

class DragonflyProgressiveRouting {
public:
    explicit DragonflyProgressiveRouting(DragonflyProgressiveConfig config = {});

    const DragonflyCanonicalTopology& topology() const noexcept { return _topology; }
    const DragonflyProgressiveConfig& config() const noexcept { return _config; }

    DragonflyRouteState initialize(DragonflyRouterId source,
                                   DragonflyRouterId target,
                                   DragonflyRoutingControl control,
                                   std::uint64_t route_hash) const;

    std::vector<DragonflyTableEntry> globalMinimal(DragonflyRouterId current,
                                                   DragonflyGroupId target_group,
                                                   bool restricted) const;
    std::vector<DragonflyTableEntry> globalNonMinimal(
        DragonflyRouterId current,
        DragonflyGroupId final_target_group,
        bool require_current_global_output = false) const;
    std::vector<DragonflyTableEntry> localMinimal(DragonflyRouterId current,
                                                  DragonflyRouterId target) const;
    std::vector<DragonflyTableEntry> localNonMinimal(
        DragonflyRouterId current,
        DragonflyRouterId target,
        std::optional<DragonflyRouterId> committed_root = std::nullopt) const;

    void setPortCongestion(DragonflyRouterId router,
                           DragonflyPortId port,
                           DragonflyCongestionComponents congestion);
    DragonflyCongestionComponents portCongestion(DragonflyRouterId router,
                                                 DragonflyPortId port) const;
    void setPortAlive(DragonflyRouterId router, DragonflyPortId port, bool alive);
    void setPhysicalLinkAlive(DragonflyRouterId router, DragonflyPortId port, bool alive);
    void setLocalMinimalDiverged(DragonflyRouterId current,
                                 DragonflyRouterId target,
                                 bool diverged);
    bool portAlive(DragonflyRouterId router, DragonflyPortId port) const;
    DragonflyRouteUpdateDisposition consumeRouteUpdate(
        const DragonflyRouteUpdateAdvertisement& advertisement,
        std::uint64_t current_time_ps);
    bool destinationAvailable(DragonflyRouterId observer_router,
                              DragonflyGroupId destination_group) const;
    bool groupSafe(DragonflyRouterId observer_router, DragonflyGroupId group) const;

    DragonflyRoutingDecision nextHop(DragonflyRouterId current, DragonflyRouteState& state) const;

private:
    struct SelectedEntry {
        DragonflyTableEntry entry;
        std::vector<DragonflyCandidate> candidates;
    };

    std::vector<DragonflyTableEntry> pathToGroup(
        DragonflyTableFamily family,
        DragonflyRouterId current,
        DragonflyGroupId target_group,
        bool require_current_connector,
        std::optional<DragonflyGroupId> intermediate_group) const;
    std::vector<DragonflyTableEntry> legalEntries(
        DragonflyRouterId current,
        const std::vector<DragonflyTableEntry>& entries) const;
    std::vector<DragonflyTableEntry> adaptiveLocalMinimal(DragonflyRouterId current,
                                                          DragonflyRouterId target) const;
    bool physicalLinkAlive(DragonflyRouterId router, DragonflyPortId port) const;
    std::vector<DragonflyTableEntry> sampleTwo(const std::vector<DragonflyTableEntry>& entries,
                                               std::uint64_t random_value) const;
    SelectedEntry selectInitial(DragonflyRouterId current,
                                DragonflyRouteState& state,
                                const std::vector<DragonflyTableEntry>& minimal,
                                const std::vector<DragonflyTableEntry>& non_minimal) const;
    SelectedEntry selectOneClass(DragonflyRouterId current,
                                 DragonflyRouteState& state,
                                 DragonflyRouteClass route_class,
                                 const std::vector<DragonflyTableEntry>& entries,
                                 bool adaptive_pair) const;
    DragonflyCandidate scoreCandidate(DragonflyRouterId current,
                                      DragonflyRouteClass route_class,
                                      const DragonflyTableEntry& entry,
                                      const DragonflyRouteState& state) const;
    DragonflyRoutingDecision forward(DragonflyRouterId current,
                                     DragonflyRouteState& state,
                                     SelectedEntry selected) const;
    DragonflyRoutingDecision drop(DragonflyRouteState& state,
                                  DragonflyDropReason reason,
                                  std::vector<DragonflyCandidate> candidates = {}) const;
    DragonflyRoutingDecision delivered(DragonflyRouteState& state) const;
    DragonflyAdaptiveBias biasFor(DragonflyRoutingControl control) const;
    std::uint8_t adjustedScore(std::uint8_t congestion, DragonflyClassBias bias) const;
    void validateState(DragonflyRouterId current, const DragonflyRouteState& state) const;

    DragonflyProgressiveConfig _config;
    DragonflyCanonicalTopology _topology;
    std::vector<std::vector<DragonflyCongestionComponents>> _congestion;
    std::vector<std::vector<bool>> _link_alive;
    std::vector<std::vector<bool>> _local_minimal_diverged;
    std::vector<std::vector<bool>> _destination_available;
    std::vector<std::vector<bool>> _safe_groups;
    std::vector<std::vector<std::optional<std::uint64_t>>> _destination_update_sequences;
    std::vector<std::vector<std::optional<std::uint64_t>>> _safe_group_update_sequences;
    std::vector<std::optional<std::uint64_t>> _latest_route_update_time_ps;
};

}  // namespace htsim

#endif  // DRAGONFLY_PROGRESSIVE_ROUTING_H
