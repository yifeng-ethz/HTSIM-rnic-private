// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "dragonfly_progressive_congestion.h"
#include "dragonfly_progressive_routing.h"

namespace {

using htsim::DragonflyAdvertisementDisposition;
using htsim::DragonflyCandidate;
using htsim::DragonflyCongestionComponents;
using htsim::DragonflyDecisionOutcome;
using htsim::DragonflyDropReason;
using htsim::DragonflyGroupId;
using htsim::DragonflyLinkKind;
using htsim::DragonflyProgressiveConfig;
using htsim::DragonflyProgressiveCongestion;
using htsim::DragonflyProgressiveCongestionConfig;
using htsim::DragonflyProgressiveRouting;
using htsim::DragonflyRouteClass;
using htsim::DragonflyRoutePhase;
using htsim::DragonflyRouterId;
using htsim::DragonflyRouteUpdateAdvertisement;
using htsim::DragonflyRouteUpdateDisposition;
using htsim::DragonflyRouteUpdateKind;
using htsim::DragonflyRoutingControl;
using htsim::DragonflyTableFamily;
using htsim::DragonflyVirtualChannel;

struct RouteTrace {
    DragonflyDecisionOutcome outcome{DragonflyDecisionOutcome::Dropped};
    DragonflyDropReason drop_reason{DragonflyDropReason::None};
    std::vector<DragonflyRouterId> routers;
    std::vector<htsim::DragonflyPortId> ports;
    std::vector<DragonflyVirtualChannel> virtual_channels;
    std::optional<DragonflyGroupId> intermediate_group;
    std::optional<DragonflyRouterId> intermediate_root_router;
    std::optional<DragonflyRouterId> target_root_router;
    std::uint8_t router_hops{0};
    std::uint8_t global_hops{0};
    DragonflyRoutePhase phase{DragonflyRoutePhase::Injection};
};

RouteTrace runRoute(const DragonflyProgressiveRouting& routing,
                    DragonflyRouterId source,
                    DragonflyRouterId target,
                    DragonflyRoutingControl control,
                    std::uint64_t route_hash) {
    auto state = routing.initialize(source, target, control, route_hash);
    RouteTrace trace;
    trace.routers.push_back(source);
    DragonflyRouterId current = source;
    for (std::uint16_t step = 0;
         step < static_cast<std::uint16_t>(routing.config().maximum_router_hops) + 2; ++step) {
        const auto decision = routing.nextHop(current, state);
        if (decision.outcome != DragonflyDecisionOutcome::Forward) {
            trace.outcome = decision.outcome;
            trace.drop_reason = decision.drop_reason;
            trace.intermediate_group = state.intermediate_group;
            trace.intermediate_root_router = state.intermediate_root_router;
            trace.target_root_router = state.target_root_router;
            trace.router_hops = state.router_hops;
            trace.global_hops = state.global_hops;
            trace.phase = state.phase;
            return trace;
        }
        if (!decision.next_router.has_value() || !decision.output_port.has_value() ||
            !decision.virtual_channel.has_value()) {
            trace.outcome = DragonflyDecisionOutcome::Dropped;
            trace.drop_reason = DragonflyDropReason::InvalidRouteState;
            return trace;
        }
        trace.ports.push_back(*decision.output_port);
        trace.virtual_channels.push_back(*decision.virtual_channel);
        current = *decision.next_router;
        trace.routers.push_back(current);
    }
    trace.outcome = DragonflyDecisionOutcome::Dropped;
    trace.drop_reason = DragonflyDropReason::HopBudgetExceeded;
    return trace;
}

bool virtualChannelsAreMonotonic(const std::vector<DragonflyVirtualChannel>& virtual_channels) {
    return std::is_sorted(virtual_channels.begin(), virtual_channels.end(),
                          [](DragonflyVirtualChannel left, DragonflyVirtualChannel right) {
                              return static_cast<std::uint8_t>(left) <
                                     static_cast<std::uint8_t>(right);
                          });
}

using DragonflyDependencyChannel =
    std::tuple<DragonflyRouterId, htsim::DragonflyPortId, std::uint8_t>;

void addDependencies(
    const RouteTrace& trace,
    std::map<DragonflyDependencyChannel, std::set<DragonflyDependencyChannel>>& graph) {
    ASSERT_EQ(trace.ports.size(), trace.virtual_channels.size());
    ASSERT_EQ(trace.routers.size(), trace.ports.size() + 1);
    std::vector<DragonflyDependencyChannel> channels;
    for (std::size_t index = 0; index < trace.ports.size(); ++index) {
        channels.emplace_back(trace.routers[index], trace.ports[index],
                              static_cast<std::uint8_t>(trace.virtual_channels[index]));
        graph.try_emplace(channels.back());
    }
    for (std::size_t index = 1; index < channels.size(); ++index) {
        graph[channels[index - 1]].insert(channels[index]);
    }
}

bool dependencyGraphIsAcyclic(
    const std::map<DragonflyDependencyChannel, std::set<DragonflyDependencyChannel>>& graph) {
    std::map<DragonflyDependencyChannel, std::size_t> indegree;
    for (const auto& [channel, successors] : graph) {
        indegree.try_emplace(channel, 0);
        for (const auto& successor : successors) {
            ++indegree[successor];
        }
    }
    std::vector<DragonflyDependencyChannel> ready;
    for (const auto& [channel, count] : indegree) {
        if (count == 0) {
            ready.push_back(channel);
        }
    }
    std::size_t processed = 0;
    while (!ready.empty()) {
        const DragonflyDependencyChannel channel = ready.back();
        ready.pop_back();
        ++processed;
        const auto entry = graph.find(channel);
        if (entry == graph.end()) {
            continue;
        }
        for (const auto& successor : entry->second) {
            auto count = indegree.find(successor);
            if (count == indegree.end() || count->second == 0) {
                return false;
            }
            if (--count->second == 0) {
                ready.push_back(successor);
            }
        }
    }
    return processed == indegree.size();
}

void setAllCongestion(DragonflyProgressiveRouting& routing,
                      DragonflyCongestionComponents congestion) {
    const auto& topology = routing.topology();
    for (DragonflyRouterId router = 0; router < topology.routerCount(); ++router) {
        for (const auto& port : topology.ports(router)) {
            routing.setPortCongestion(router, port.id, congestion);
        }
    }
}

std::optional<RouteTrace> findEightHopProgressiveTrace(DragonflyProgressiveRouting& routing) {
    const DragonflyRouterId source = 0;
    const DragonflyRouterId target = 35;
    const DragonflyGroupId target_group = routing.topology().groupOf(target);
    for (std::uint64_t route_hash = 0; route_hash < 65'536; ++route_hash) {
        auto state =
            routing.initialize(source, target, DragonflyRoutingControl::Adaptive0, route_hash);
        DragonflyRouterId current = source;
        RouteTrace trace;
        trace.routers.push_back(source);
        bool source_gn_local = false;
        bool intermediate_gm_local = false;
        bool intermediate_ln_local = false;
        bool target_ln_local = false;

        for (std::uint8_t step = 0; step < 10; ++step) {
            setAllCongestion(routing, {0, 0, 0});
            const DragonflyRoutePhase phase_before = state.phase;
            const std::uint8_t local_hops_before = state.local_hops_in_phase;

            if (phase_before == DragonflyRoutePhase::Injection ||
                phase_before == DragonflyRoutePhase::SourceAdaptiveUp) {
                const auto gm = routing.globalMinimal(
                    current, target_group, phase_before == DragonflyRoutePhase::SourceAdaptiveUp);
                for (const auto& entry : gm) {
                    if (entry.output_port.has_value()) {
                        routing.setPortCongestion(current, *entry.output_port, {15, 0, 0});
                    }
                }
            } else if (phase_before == DragonflyRoutePhase::IntermediateAdaptiveUp) {
                const auto gm =
                    routing.globalMinimal(current, target_group, local_hops_before != 0);
                if (local_hops_before == 0) {
                    setAllCongestion(routing, {15, 0, 0});
                    for (const auto& entry : gm) {
                        if (entry.output_port.has_value()) {
                            routing.setPortCongestion(current, *entry.output_port, {0, 0, 0});
                        }
                    }
                } else {
                    for (const auto& entry : gm) {
                        if (entry.output_port.has_value()) {
                            routing.setPortCongestion(current, *entry.output_port, {15, 0, 0});
                        }
                    }
                }
            } else if (phase_before == DragonflyRoutePhase::TargetAdaptiveUp) {
                for (const auto& entry : routing.localMinimal(current, target)) {
                    if (entry.output_port.has_value()) {
                        routing.setPortCongestion(current, *entry.output_port, {15, 0, 0});
                    }
                }
            }

            const auto decision = routing.nextHop(current, state);
            if (decision.outcome != DragonflyDecisionOutcome::Forward) {
                trace.outcome = decision.outcome;
                trace.drop_reason = decision.drop_reason;
                trace.intermediate_group = state.intermediate_group;
                trace.intermediate_root_router = state.intermediate_root_router;
                trace.target_root_router = state.target_root_router;
                trace.router_hops = state.router_hops;
                trace.global_hops = state.global_hops;
                trace.phase = state.phase;
                break;
            }
            if (!decision.next_router.has_value() || !decision.output_port.has_value() ||
                !decision.virtual_channel.has_value()) {
                break;
            }

            const DragonflyLinkKind kind =
                routing.topology().port(current, *decision.output_port).kind;
            source_gn_local =
                source_gn_local || (phase_before == DragonflyRoutePhase::Injection &&
                                    state.phase == DragonflyRoutePhase::SourceAdaptiveUp &&
                                    kind == DragonflyLinkKind::Local);
            intermediate_gm_local = intermediate_gm_local ||
                                    (phase_before == DragonflyRoutePhase::IntermediateAdaptiveUp &&
                                     local_hops_before == 0 &&
                                     state.phase == DragonflyRoutePhase::IntermediateAdaptiveUp &&
                                     kind == DragonflyLinkKind::Local);
            intermediate_ln_local =
                intermediate_ln_local ||
                (phase_before == DragonflyRoutePhase::IntermediateAdaptiveUp &&
                 local_hops_before != 0 && state.phase == DragonflyRoutePhase::IntermediateRoot &&
                 kind == DragonflyLinkKind::Local);
            target_ln_local = target_ln_local ||
                              (phase_before == DragonflyRoutePhase::TargetAdaptiveUp &&
                               state.phase == DragonflyRoutePhase::TargetRoot &&
                               kind == DragonflyLinkKind::Local && *decision.next_router != target);

            trace.ports.push_back(*decision.output_port);
            trace.virtual_channels.push_back(*decision.virtual_channel);
            current = *decision.next_router;
            trace.routers.push_back(current);
        }

        if (trace.outcome == DragonflyDecisionOutcome::Delivered && trace.router_hops == 8 &&
            source_gn_local && intermediate_gm_local && intermediate_ln_local && target_ln_local) {
            return trace;
        }
    }
    return std::nullopt;
}

TEST(DragonflyProgressiveRoutingTest, BuildsCanonicalMaximumSizeReciprocalDragonfly) {
    const DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    EXPECT_EQ(topology.routersPerGroup(), 4U);
    EXPECT_EQ(topology.globalLinksPerRouter(), 2U);
    EXPECT_EQ(topology.groupCount(), 9U);
    EXPECT_EQ(topology.routerCount(), 36U);

    for (DragonflyRouterId router = 0; router < topology.routerCount(); ++router) {
        ASSERT_EQ(topology.ports(router).size(), 5U);
        EXPECT_EQ(
            std::count_if(topology.ports(router).begin(), topology.ports(router).end(),
                          [](const auto& port) { return port.kind == DragonflyLinkKind::Local; }),
            3);
        EXPECT_EQ(
            std::count_if(topology.ports(router).begin(), topology.ports(router).end(),
                          [](const auto& port) { return port.kind == DragonflyLinkKind::Global; }),
            2);
        for (const auto& port : topology.ports(router)) {
            const auto reverse = topology.reciprocalPort(router, port.id);
            const auto& peer_port = topology.port(port.next_router, reverse);
            EXPECT_EQ(peer_port.next_router, router);
            EXPECT_EQ(peer_port.kind, port.kind);
        }
    }

    for (DragonflyGroupId source = 0; source < topology.groupCount(); ++source) {
        for (DragonflyGroupId target = 0; target < topology.groupCount(); ++target) {
            if (source == target) {
                continue;
            }
            std::size_t directed_links = 0;
            const DragonflyRouterId base =
                static_cast<DragonflyRouterId>(source) * topology.routersPerGroup();
            for (std::uint16_t index = 0; index < topology.routersPerGroup(); ++index) {
                for (const auto& port : topology.ports(base + index)) {
                    if (port.kind == DragonflyLinkKind::Global &&
                        topology.groupOf(port.next_router) == target) {
                        ++directed_links;
                    }
                }
            }
            EXPECT_EQ(directed_links, 1U);
        }
    }
}

TEST(DragonflyProgressiveRoutingTest, SameRouterEndpointPairRequiresNoFabricHop) {
    const DragonflyProgressiveRouting routing;
    auto state = routing.initialize(7, 7, DragonflyRoutingControl::DeterministicMinimal, 99);
    const auto decision = routing.nextHop(7, state);

    EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Delivered);
    EXPECT_EQ(decision.drop_reason, DragonflyDropReason::None);
    EXPECT_FALSE(decision.next_router.has_value());
    EXPECT_EQ(state.router_hops, 0U);
    EXPECT_EQ(state.global_hops, 0U);
    EXPECT_EQ(state.phase, DragonflyRoutePhase::Delivered);
}

TEST(DragonflyProgressiveRoutingTest, ExposesGnGmLnLmTablesAndFiltersUnsafeIntermediates) {
    DragonflyProgressiveRouting routing;
    const DragonflyRouterId current = 0;
    const DragonflyRouterId local_target = 3;
    const DragonflyGroupId remote_target = 8;

    const auto gm = routing.globalMinimal(current, remote_target, false);
    ASSERT_EQ(gm.size(), 1U);
    EXPECT_EQ(gm.front().family, DragonflyTableFamily::GlobalMinimal);

    const auto gn = routing.globalNonMinimal(current, remote_target);
    ASSERT_EQ(gn.size(), 7U);
    for (const auto& entry : gn) {
        EXPECT_EQ(entry.family, DragonflyTableFamily::GlobalNonMinimal);
        ASSERT_TRUE(entry.intermediate_group.has_value());
        EXPECT_NE(*entry.intermediate_group, 0U);
        EXPECT_NE(*entry.intermediate_group, remote_target);
        EXPECT_FALSE(entry.root_router.has_value());
    }

    const auto lm = routing.localMinimal(current, local_target);
    ASSERT_EQ(lm.size(), 1U);
    EXPECT_EQ(lm.front().family, DragonflyTableFamily::LocalMinimal);
    EXPECT_EQ(lm.front().next_router, local_target);
    EXPECT_FALSE(lm.front().diverged);

    const auto ln = routing.localNonMinimal(current, local_target);
    ASSERT_EQ(ln.size(), 4U);
    std::size_t root_detect_entries = 0;
    std::size_t target_root_entries = 0;
    for (const auto& entry : ln) {
        EXPECT_EQ(entry.family, DragonflyTableFamily::LocalNonMinimal);
        ASSERT_TRUE(entry.root_router.has_value());
        root_detect_entries += entry.root_detect ? 1U : 0U;
        target_root_entries += *entry.root_router == local_target ? 1U : 0U;
        if (entry.root_detect) {
            EXPECT_EQ(*entry.root_router, current);
            EXPECT_FALSE(entry.output_port.has_value());
        }
    }
    EXPECT_EQ(root_detect_entries, 1U);
    EXPECT_EQ(target_root_entries, 1U);
    const auto committed = routing.localNonMinimal(current, local_target, 1);
    ASSERT_EQ(committed.size(), 1U);
    EXPECT_EQ(committed.front().next_router, 1U);
    const auto root_detect = routing.localNonMinimal(1, 1, 1);
    ASSERT_EQ(root_detect.size(), 1U);
    EXPECT_TRUE(root_detect.front().root_detect);
    EXPECT_FALSE(root_detect.front().output_port.has_value());

    const DragonflyRouteUpdateAdvertisement unsafe_group{
        current, DragonflyRouteUpdateKind::IntermediateGroupSafe, 4, false, 1, 100, 120};
    EXPECT_EQ(routing.consumeRouteUpdate(unsafe_group, 119),
              DragonflyRouteUpdateDisposition::NotYetPhysicallyArrived);
    EXPECT_EQ(routing.globalNonMinimal(current, remote_target).size(), 7U);
    EXPECT_EQ(routing.consumeRouteUpdate(unsafe_group, 120),
              DragonflyRouteUpdateDisposition::Accepted);
    const auto filtered = routing.globalNonMinimal(current, remote_target);
    EXPECT_EQ(filtered.size(), 6U);
    EXPECT_EQ(std::find_if(filtered.begin(), filtered.end(),
                           [](const auto& entry) { return entry.intermediate_group == 4; }),
              filtered.end());
}

TEST(DragonflyProgressiveRoutingTest,
     LmDivergedBitBlocksAdaptiveUproutingButRemainsLegalForDownrouting) {
    DragonflyProgressiveRouting routing;
    const DragonflyRouterId source = 0;
    const DragonflyRouterId target = 3;
    routing.setLocalMinimalDiverged(source, target, true);
    const auto lm = routing.localMinimal(source, target);
    ASSERT_EQ(lm.size(), 1U);
    EXPECT_TRUE(lm.front().diverged);
    EXPECT_THROW(routing.setLocalMinimalDiverged(source, 4, true), std::invalid_argument);

    std::optional<htsim::DragonflyRouteState> selected_state;
    std::optional<htsim::DragonflyRoutingDecision> selected_decision;
    for (std::uint64_t route_hash = 0; route_hash < 4'096; ++route_hash) {
        auto state =
            routing.initialize(source, target, DragonflyRoutingControl::Adaptive0, route_hash);
        const auto decision = routing.nextHop(source, state);
        if (decision.outcome == DragonflyDecisionOutcome::Forward &&
            state.phase == DragonflyRoutePhase::TargetRoot && decision.next_router.has_value() &&
            *decision.next_router != target) {
            selected_state = state;
            selected_decision = decision;
            break;
        }
    }
    ASSERT_TRUE(selected_state.has_value());
    ASSERT_TRUE(selected_decision.has_value());
    ASSERT_FALSE(selected_decision->candidates.empty());
    EXPECT_TRUE(std::all_of(selected_decision->candidates.begin(),
                            selected_decision->candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.route_class == DragonflyRouteClass::NonMinimal;
                            }));

    DragonflyRouterId current = *selected_decision->next_router;
    const auto downroute = routing.nextHop(current, *selected_state);
    ASSERT_EQ(downroute.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(selected_state->phase, DragonflyRoutePhase::TargetLocal);
    current = *downroute.next_router;
    EXPECT_EQ(routing.nextHop(current, *selected_state).outcome,
              DragonflyDecisionOutcome::Delivered);

    const RouteTrace deterministic =
        runRoute(routing, source, target, DragonflyRoutingControl::DeterministicMinimal, 17);
    EXPECT_EQ(deterministic.outcome, DragonflyDecisionOutcome::Delivered);
    EXPECT_EQ(deterministic.router_hops, 1U);
}

TEST(DragonflyProgressiveRoutingTest, ComposesFourBitSignalsAndAppliesSaturatingSixBitBiases) {
    EXPECT_EQ((DragonflyCongestionComponents{1, 2, 3}.compose()), 6U);
    EXPECT_EQ((DragonflyCongestionComponents{15, 15, 15}.compose()), 15U);
    EXPECT_THROW((DragonflyCongestionComponents{16, 0, 0}.compose()), std::invalid_argument);

    DragonflyProgressiveConfig invalid;
    invalid.adaptive_biases[0].minimal.shift = 3;
    EXPECT_THROW((void)DragonflyProgressiveRouting{invalid}, std::invalid_argument);
    invalid.adaptive_biases[0].minimal.shift = 0;
    invalid.adaptive_biases[0].minimal.additive = 64;
    EXPECT_THROW((void)DragonflyProgressiveRouting{invalid}, std::invalid_argument);
    invalid.adaptive_biases[0].minimal.additive = 0;
    invalid.maximum_router_hops = 7;
    EXPECT_THROW((void)DragonflyProgressiveRouting{invalid}, std::invalid_argument);
    invalid.maximum_router_hops = 9;
    EXPECT_THROW((void)DragonflyProgressiveRouting{invalid}, std::invalid_argument);
    invalid.maximum_router_hops = 8;
    invalid.minimum_route_update_delay_ps = 0;
    EXPECT_THROW((void)DragonflyProgressiveRouting{invalid}, std::invalid_argument);

    DragonflyProgressiveConfig config;
    config.adaptive_biases[0].minimal = {2, 63};
    config.adaptive_biases[0].non_minimal = {2, 63};
    DragonflyProgressiveRouting saturated(config);
    setAllCongestion(saturated, {15, 15, 15});
    auto state = saturated.initialize(0, 35, DragonflyRoutingControl::Adaptive0, 17);
    const auto decision = saturated.nextHop(0, state);
    ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_EQ(decision.candidates.size(), 4U);
    for (const DragonflyCandidate& candidate : decision.candidates) {
        EXPECT_EQ(candidate.congestion, 15U);
        EXPECT_EQ(candidate.adjusted_score, 63U);
    }
}

TEST(DragonflyProgressiveRoutingTest, AdaptiveSelectionSamplesTwoPerClassAndMinimalWinsTies) {
    DragonflyProgressiveRouting routing;
    auto state = routing.initialize(0, 35, DragonflyRoutingControl::Adaptive0, 91);
    const auto decision = routing.nextHop(0, state);
    ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_EQ(decision.candidates.size(), 4U);
    EXPECT_EQ(std::count_if(decision.candidates.begin(), decision.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.route_class == DragonflyRouteClass::Minimal;
                            }),
              2);
    EXPECT_EQ(std::count_if(decision.candidates.begin(), decision.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.route_class == DragonflyRouteClass::NonMinimal;
                            }),
              2);
    // The winning GM output is a local hop to the connector.  Adaptive state
    // therefore remains undecided until the restricted connector decision.
    EXPECT_EQ(state.route_class, DragonflyRouteClass::Undecided);
    EXPECT_FALSE(state.intermediate_group.has_value());

    std::vector<DragonflyGroupId> sampled_intermediates;
    std::vector<htsim::DragonflyPortId> sampled_non_minimal_ports;
    for (const DragonflyCandidate& candidate : decision.candidates) {
        if (candidate.route_class == DragonflyRouteClass::NonMinimal) {
            ASSERT_TRUE(candidate.entry.intermediate_group.has_value());
            ASSERT_TRUE(candidate.entry.output_port.has_value());
            sampled_intermediates.push_back(*candidate.entry.intermediate_group);
            sampled_non_minimal_ports.push_back(*candidate.entry.output_port);
        }
    }
    ASSERT_EQ(sampled_intermediates.size(), 2U);
    EXPECT_NE(sampled_intermediates[0], sampled_intermediates[1]);
    ASSERT_EQ(sampled_non_minimal_ports.size(), 2U);
    EXPECT_NE(sampled_non_minimal_ports[0], sampled_non_minimal_ports[1]);
}

TEST(DragonflyProgressiveRoutingTest,
     AdaptiveRoutingReconsidersAtARestrictedConnectorBeforeDivergence) {
    DragonflyProgressiveRouting routing;
    constexpr DragonflyRouterId source = 1;
    constexpr DragonflyRouterId target = 4;
    auto state = routing.initialize(source, target, DragonflyRoutingControl::Adaptive0, 91);

    const auto first = routing.nextHop(source, state);
    ASSERT_EQ(first.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(first.next_router.has_value());
    const DragonflyRouterId connector = *first.next_router;
    EXPECT_EQ(connector, routing.topology().groupConnector(0, 1));
    EXPECT_EQ(state.route_class, DragonflyRouteClass::Undecided);
    EXPECT_FALSE(state.diverged);
    EXPECT_EQ(state.phase, DragonflyRoutePhase::SourceAdaptiveUp);

    const auto minimal = routing.globalMinimal(connector, 1, true);
    ASSERT_EQ(minimal.size(), 1U);
    ASSERT_TRUE(minimal.front().output_port.has_value());
    routing.setPortCongestion(connector, *minimal.front().output_port, {15, 0, 0});

    const auto second = routing.nextHop(connector, state);
    ASSERT_EQ(second.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(state.route_class, DragonflyRouteClass::NonMinimal);
    // GN won on congestion; DVG remains false because a legal restricted GM
    // candidate still existed at this input queue.
    EXPECT_FALSE(state.diverged);
    ASSERT_TRUE(state.intermediate_group.has_value());
    EXPECT_NE(*state.intermediate_group, 0U);
    EXPECT_NE(*state.intermediate_group, 1U);
    ASSERT_TRUE(second.output_port.has_value());
    EXPECT_EQ(routing.topology().port(connector, *second.output_port).kind,
              DragonflyLinkKind::Global);
    EXPECT_EQ(std::count_if(second.candidates.begin(), second.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.route_class == DragonflyRouteClass::Minimal;
                            }),
              2);
    EXPECT_EQ(std::count_if(second.candidates.begin(), second.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.route_class == DragonflyRouteClass::NonMinimal;
                            }),
              2);
}

TEST(DragonflyProgressiveRoutingTest,
     InvalidRestrictedGmForcesGnAndDivergenceResetsInTheIntermediateGroup) {
    DragonflyProgressiveRouting routing;
    constexpr DragonflyRouterId source = 1;
    constexpr DragonflyRouterId target = 4;
    auto state = routing.initialize(source, target, DragonflyRoutingControl::Adaptive0, 91);
    const auto first = routing.nextHop(source, state);
    ASSERT_EQ(first.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(first.next_router.has_value());
    const DragonflyRouterId connector = *first.next_router;
    ASSERT_EQ(state.phase, DragonflyRoutePhase::SourceAdaptiveUp);

    const auto gm = routing.globalMinimal(connector, 1, true);
    ASSERT_EQ(gm.size(), 1U);
    ASSERT_TRUE(gm.front().output_port.has_value());
    routing.setPhysicalLinkAlive(connector, *gm.front().output_port, false);

    const auto forced = routing.nextHop(connector, state);
    ASSERT_EQ(forced.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(std::count_if(forced.candidates.begin(), forced.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.route_class == DragonflyRouteClass::Minimal;
                            }),
              0);
    EXPECT_EQ(state.route_class, DragonflyRouteClass::NonMinimal);
    EXPECT_EQ(state.phase, DragonflyRoutePhase::IntermediateAdaptiveUp);
    EXPECT_TRUE(state.intermediate_group.has_value());
    // DVG describes the source lookup. Entry into a new group starts a new UP
    // episode, so it cannot leak across the physical global boundary.
    EXPECT_FALSE(state.diverged);
}

TEST(DragonflyProgressiveRoutingTest, ALocalGnChoiceDoesNotCommitAndMayReturnToGmAtTheNextInput) {
    DragonflyProgressiveConfig config;
    config.adaptive_biases[0].minimal.additive = 1;
    DragonflyProgressiveRouting routing(config);
    constexpr DragonflyRouterId source = 0;
    constexpr DragonflyRouterId target = 35;
    const DragonflyGroupId target_group = routing.topology().groupOf(target);
    const DragonflyRouterId target_connector = routing.topology().groupConnector(0, target_group);
    const auto gm = routing.globalMinimal(source, target_group, false);
    ASSERT_EQ(gm.size(), 1U);
    ASSERT_TRUE(gm.front().output_port.has_value());

    std::optional<htsim::DragonflyRouteState> selected_state;
    std::optional<htsim::DragonflyRoutingDecision> selected_decision;
    for (std::uint64_t route_hash = 0; route_hash < 4'096; ++route_hash) {
        auto state =
            routing.initialize(source, target, DragonflyRoutingControl::Adaptive0, route_hash);
        auto decision = routing.nextHop(source, state);
        if (decision.outcome == DragonflyDecisionOutcome::Forward &&
            decision.next_router == target_connector &&
            state.phase == DragonflyRoutePhase::SourceAdaptiveUp) {
            selected_state = state;
            selected_decision = decision;
            break;
        }
    }
    ASSERT_TRUE(selected_state.has_value());
    ASSERT_TRUE(selected_decision.has_value());
    EXPECT_EQ(selected_state->phase, DragonflyRoutePhase::SourceAdaptiveUp);
    EXPECT_EQ(selected_state->route_class, DragonflyRouteClass::Undecided);
    EXPECT_FALSE(selected_state->intermediate_group.has_value());

    for (const auto& port : routing.topology().ports(target_connector)) {
        routing.setPortCongestion(target_connector, port.id, {15, 0, 0});
    }
    const auto restricted_gm = routing.globalMinimal(target_connector, target_group, true);
    ASSERT_EQ(restricted_gm.size(), 1U);
    ASSERT_TRUE(restricted_gm.front().output_port.has_value());
    routing.setPortCongestion(target_connector, *restricted_gm.front().output_port, {0, 0, 0});

    const auto next = routing.nextHop(target_connector, *selected_state);
    ASSERT_EQ(next.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(next.output_port.has_value());
    EXPECT_EQ(selected_state->route_class, DragonflyRouteClass::Minimal);
    EXPECT_FALSE(selected_state->intermediate_group.has_value());
    EXPECT_EQ(routing.topology().port(target_connector, *next.output_port).kind,
              DragonflyLinkKind::Global);
}

TEST(DragonflyProgressiveRoutingTest,
     FirstGnGlobalArrivalArbitratesGmFullAgainstLnBeforeRootDetect) {
    DragonflyProgressiveConfig config;
    config.adaptive_biases[0].minimal.additive = 63;
    DragonflyProgressiveRouting routing(config);
    auto state = routing.initialize(0, 35, DragonflyRoutingControl::Adaptive0, 113);
    DragonflyRouterId current = 0;
    for (std::uint8_t step = 0; step < 3 && state.global_hops == 0; ++step) {
        const auto decision = routing.nextHop(current, state);
        ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
        ASSERT_TRUE(decision.next_router.has_value());
        current = *decision.next_router;
    }
    ASSERT_EQ(state.global_hops, 1U);
    ASSERT_EQ(state.phase, DragonflyRoutePhase::IntermediateAdaptiveUp);
    EXPECT_FALSE(state.intermediate_root_router.has_value());

    const auto decision = routing.nextHop(current, state);
    ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(std::count_if(decision.candidates.begin(), decision.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.entry.family ==
                                       DragonflyTableFamily::GlobalMinimal;
                            }),
              2);
    EXPECT_EQ(std::count_if(decision.candidates.begin(), decision.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.entry.family ==
                                       DragonflyTableFamily::LocalNonMinimal;
                            }),
              2);
    EXPECT_EQ(state.phase, DragonflyRoutePhase::IntermediateRoot);
    EXPECT_TRUE(state.intermediate_root_router.has_value());
}

TEST(DragonflyProgressiveRoutingTest, TargetGroupArrivalArbitratesLmAgainstLnThenMinimalWinsATie) {
    DragonflyProgressiveRouting routing;
    auto state = routing.initialize(0, 35, DragonflyRoutingControl::Adaptive0, 91);
    DragonflyRouterId current = 0;
    for (std::uint8_t step = 0; step < 3 && state.global_hops == 0; ++step) {
        const auto decision = routing.nextHop(current, state);
        ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
        ASSERT_TRUE(decision.next_router.has_value());
        current = *decision.next_router;
    }
    ASSERT_EQ(state.global_hops, 1U);
    ASSERT_EQ(state.phase, DragonflyRoutePhase::TargetAdaptiveUp);
    ASSERT_NE(current, state.target_router);

    const auto decision = routing.nextHop(current, state);
    ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(std::count_if(decision.candidates.begin(), decision.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.entry.family == DragonflyTableFamily::LocalMinimal;
                            }),
              2);
    EXPECT_EQ(std::count_if(decision.candidates.begin(), decision.candidates.end(),
                            [](const DragonflyCandidate& candidate) {
                                return candidate.entry.family ==
                                       DragonflyTableFamily::LocalNonMinimal;
                            }),
              2);
    EXPECT_EQ(state.phase, DragonflyRoutePhase::TargetLocal);
    EXPECT_EQ(decision.next_router, state.target_router);
}

TEST(DragonflyProgressiveRoutingTest,
     RouteStateRejectsTeleportationAndCannotResurrectTerminalPackets) {
    const DragonflyProgressiveRouting routing;
    auto state = routing.initialize(1, 4, DragonflyRoutingControl::DeterministicMinimal, 7);
    const auto first = routing.nextHop(1, state);
    ASSERT_EQ(first.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(first.next_router.has_value());
    EXPECT_EQ(state.expected_current_router, *first.next_router);

    const auto teleported = routing.nextHop(4, state);
    EXPECT_EQ(teleported.outcome, DragonflyDecisionOutcome::Dropped);
    EXPECT_EQ(teleported.drop_reason, DragonflyDropReason::InvalidRouteState);
    EXPECT_EQ(state.phase, DragonflyRoutePhase::Dropped);
    const auto dropped_at_target = routing.nextHop(4, state);
    EXPECT_EQ(dropped_at_target.outcome, DragonflyDecisionOutcome::Dropped);

    auto delivered = routing.initialize(7, 7, DragonflyRoutingControl::DeterministicMinimal, 1);
    ASSERT_EQ(routing.nextHop(7, delivered).outcome, DragonflyDecisionOutcome::Delivered);
    const auto delivered_at_wrong_router = routing.nextHop(6, delivered);
    EXPECT_EQ(delivered_at_wrong_router.outcome, DragonflyDecisionOutcome::Dropped);
    EXPECT_EQ(delivered_at_wrong_router.drop_reason, DragonflyDropReason::InvalidRouteState);

    auto already_dropped =
        routing.initialize(7, 7, DragonflyRoutingControl::DeterministicMinimal, 1);
    already_dropped.phase = DragonflyRoutePhase::Dropped;
    EXPECT_EQ(routing.nextHop(7, already_dropped).outcome, DragonflyDecisionOutcome::Dropped);
}

TEST(DragonflyProgressiveRoutingTest, RouteStateRejectsCorruptedHopPhaseAndRouteClassMetadata) {
    const DragonflyProgressiveRouting routing;
    auto state = routing.initialize(1, 4, DragonflyRoutingControl::DeterministicMinimal, 7);
    const auto first = routing.nextHop(1, state);
    ASSERT_EQ(first.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(first.next_router.has_value());
    ASSERT_TRUE(state.previous_router.has_value());
    ASSERT_TRUE(state.previous_output_port.has_value());
    const DragonflyRouterId current = *first.next_router;

    const auto expect_invalid = [&](auto corrupted) {
        const auto decision = routing.nextHop(current, corrupted);
        EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Dropped);
        EXPECT_EQ(decision.drop_reason, DragonflyDropReason::InvalidRouteState);
    };

    auto wrong_vc = state;
    wrong_vc.last_virtual_channel = DragonflyVirtualChannel::DependencyHop7;
    expect_invalid(wrong_vc);

    auto wrong_port = state;
    for (const auto& port : routing.topology().ports(*state.previous_router)) {
        if (port.next_router != current) {
            wrong_port.previous_output_port = port.id;
            break;
        }
    }
    ASSERT_NE(wrong_port.previous_output_port, state.previous_output_port);
    expect_invalid(wrong_port);

    auto excessive_hops = state;
    excessive_hops.router_hops = 9;
    expect_invalid(excessive_hops);

    auto wrong_class = state;
    wrong_class.route_class = DragonflyRouteClass::NonMinimal;
    expect_invalid(wrong_class);
}

TEST(DragonflyProgressiveRoutingTest, EveryNonInjectionPhaseRejectsMissingRequiredMetadata) {
    const DragonflyProgressiveRouting routing;
    const std::vector<DragonflyRoutePhase> phases{
        DragonflyRoutePhase::SourceAdaptiveUp,
        DragonflyRoutePhase::MinimalToTargetGroup,
        DragonflyRoutePhase::NonMinimalToIntermediate,
        DragonflyRoutePhase::IntermediateAdaptiveUp,
        DragonflyRoutePhase::IntermediateRoot,
        DragonflyRoutePhase::IntermediateToTargetGroup,
        DragonflyRoutePhase::TargetAdaptiveUp,
        DragonflyRoutePhase::TargetRoot,
        DragonflyRoutePhase::TargetLocal,
        DragonflyRoutePhase::Delivered,
    };
    for (const DragonflyRoutePhase phase : phases) {
        auto state = routing.initialize(0, 35, DragonflyRoutingControl::Adaptive0, 1);
        state.phase = phase;
        const auto decision = routing.nextHop(0, state);
        EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Dropped)
            << "phase=" << static_cast<unsigned int>(phase);
        EXPECT_EQ(decision.drop_reason, DragonflyDropReason::InvalidRouteState)
            << "phase=" << static_cast<unsigned int>(phase);
    }
}

TEST(DragonflyProgressiveRoutingTest, RouteUpdatesEnforcePhysicalDelayOrderingAndSequence) {
    DragonflyProgressiveRouting routing;
    constexpr DragonflyRouterId observer = 5;
    constexpr DragonflyGroupId destination = 8;
    const DragonflyRouteUpdateAdvertisement invalid_delay{
        observer, DragonflyRouteUpdateKind::DestinationAvailable, destination, false, 1, 100, 100};
    EXPECT_THROW(routing.consumeRouteUpdate(invalid_delay, 100), std::invalid_argument);

    const DragonflyRouteUpdateAdvertisement unavailable{
        observer, DragonflyRouteUpdateKind::DestinationAvailable, destination, false, 3, 100, 110};
    EXPECT_EQ(routing.consumeRouteUpdate(unavailable, 109),
              DragonflyRouteUpdateDisposition::NotYetPhysicallyArrived);
    EXPECT_TRUE(routing.destinationAvailable(observer, destination));
    EXPECT_EQ(routing.consumeRouteUpdate(unavailable, 110),
              DragonflyRouteUpdateDisposition::Accepted);
    EXPECT_FALSE(routing.destinationAvailable(observer, destination));

    auto stale = unavailable;
    stale.sequence = 2;
    stale.usable = true;
    stale.observation_time_ps = 111;
    stale.physical_arrival_time_ps = 120;
    EXPECT_EQ(routing.consumeRouteUpdate(stale, 120),
              DragonflyRouteUpdateDisposition::StaleOrDuplicate);
    EXPECT_FALSE(routing.destinationAvailable(observer, destination));

    auto regression = stale;
    regression.sequence = 4;
    regression.observation_time_ps = 105;
    regression.physical_arrival_time_ps = 115;
    EXPECT_THROW(routing.consumeRouteUpdate(regression, 115), std::invalid_argument);
}

TEST(DragonflyProgressiveRoutingTest,
     PhysicallyArrivedDownstreamPressureChangesWinnerWithEqualNearQueues) {
    constexpr DragonflyRouterId source = 0;
    constexpr DragonflyRouterId target = 35;
    constexpr std::uint64_t route_hash = 91;

    DragonflyProgressiveRouting baseline;
    auto baseline_state =
        baseline.initialize(source, target, DragonflyRoutingControl::Adaptive0, route_hash);
    const auto baseline_decision = baseline.nextHop(source, baseline_state);
    ASSERT_EQ(baseline_decision.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_EQ(baseline_state.route_class, DragonflyRouteClass::Undecided);

    std::optional<htsim::DragonflyPortId> minimal_port;
    for (const DragonflyCandidate& candidate : baseline_decision.candidates) {
        if (candidate.route_class != DragonflyRouteClass::Minimal) {
            continue;
        }
        ASSERT_TRUE(candidate.entry.output_port.has_value());
        if (!minimal_port.has_value()) {
            minimal_port = candidate.entry.output_port;
        }
        EXPECT_EQ(candidate.entry.output_port, minimal_port);
    }
    ASSERT_TRUE(minimal_port.has_value());

    DragonflyProgressiveCongestionConfig signal_config;
    signal_config.minimum_downstream_delay_ps = 20;
    DragonflyProgressiveCongestion signal(signal_config);
    const auto advertisement = signal.makeDownstreamAdvertisement(1, 100, 15);

    // Before the modeled neighbor advertisement arrives, all local output
    // queues and visible remote components are equal, so the tie remains
    // minimal.
    ASSERT_EQ(signal.consumeDownstreamAdvertisement(advertisement, 119),
              DragonflyAdvertisementDisposition::NotYetPhysicallyArrived);
    DragonflyProgressiveRouting before_arrival;
    before_arrival.setPortCongestion(source, *minimal_port, signal.components(119));
    auto before_state =
        before_arrival.initialize(source, target, DragonflyRoutingControl::Adaptive0, route_hash);
    const auto before_decision = before_arrival.nextHop(source, before_state);
    ASSERT_EQ(before_decision.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(before_state.route_class, DragonflyRouteClass::Undecided);

    // At physical arrival the only changed term is downstream pressure for
    // the minimal output.  Near-end queues remain equal and the non-minimal
    // candidates now win on genuinely remote information.
    ASSERT_EQ(signal.consumeDownstreamAdvertisement(advertisement, 120),
              DragonflyAdvertisementDisposition::Accepted);
    DragonflyProgressiveRouting after_arrival;
    after_arrival.setPortCongestion(source, *minimal_port, signal.components(120));
    auto after_state =
        after_arrival.initialize(source, target, DragonflyRoutingControl::Adaptive0, route_hash);
    const auto after_decision = after_arrival.nextHop(source, after_state);
    ASSERT_EQ(after_decision.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(after_decision.output_port.has_value());
    EXPECT_NE(after_decision.output_port, minimal_port);
    EXPECT_TRUE(after_state.route_class == DragonflyRouteClass::Undecided ||
                after_state.route_class == DragonflyRouteClass::NonMinimal);
}

TEST(DragonflyProgressiveRoutingTest,
     PhysicalIntermediateAndLnRootsPersistAfterIrreversibleBoundaries) {
    DragonflyProgressiveConfig config;
    config.adaptive_biases[0].minimal.additive = 63;
    DragonflyProgressiveRouting routing(config);
    auto state = routing.initialize(0, 35, DragonflyRoutingControl::Adaptive0, 113);
    DragonflyRouterId current = 0;
    std::vector<DragonflyVirtualChannel> virtual_channels;

    htsim::DragonflyRoutingDecision decision;
    for (std::uint8_t step = 0; step <= routing.config().maximum_router_hops; ++step) {
        decision = routing.nextHop(current, state);
        ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
        ASSERT_TRUE(decision.virtual_channel.has_value());
        ASSERT_TRUE(decision.next_router.has_value());
        virtual_channels.push_back(*decision.virtual_channel);
        current = *decision.next_router;
        if (state.global_hops == 1) {
            break;
        }
    }
    ASSERT_EQ(state.route_class, DragonflyRouteClass::NonMinimal);
    ASSERT_TRUE(state.intermediate_group.has_value());
    const DragonflyGroupId committed_intermediate = *state.intermediate_group;

    setAllCongestion(routing, {15, 15, 15});
    for (std::uint8_t step = state.router_hops; step <= routing.config().maximum_router_hops;
         ++step) {
        decision = routing.nextHop(current, state);
        if (decision.outcome != DragonflyDecisionOutcome::Forward) {
            break;
        }
        ASSERT_TRUE(decision.virtual_channel.has_value());
        virtual_channels.push_back(*decision.virtual_channel);
        current = *decision.next_router;
        EXPECT_EQ(state.intermediate_group, committed_intermediate);
    }
    EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Delivered);
    EXPECT_EQ(current, 35U);
    EXPECT_EQ(state.intermediate_group, committed_intermediate);
    EXPECT_TRUE(state.intermediate_root_router.has_value());
    EXPECT_TRUE(state.target_root_router.has_value());
    EXPECT_EQ(state.global_hops, 2U);
    EXPECT_LE(state.router_hops, 8U);
    EXPECT_TRUE(virtualChannelsAreMonotonic(virtual_channels));
}

TEST(DragonflyProgressiveRoutingTest,
     DeterministicModesAreOrderedReachableAndRespectHopAndVcBounds) {
    const DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    for (DragonflyRouterId source = 0; source < topology.routerCount(); ++source) {
        for (DragonflyRouterId target = 0; target < topology.routerCount(); ++target) {
            if (source == target) {
                continue;
            }
            const std::uint64_t route_hash = (static_cast<std::uint64_t>(source) << 32) | target;
            for (DragonflyRoutingControl control :
                 {DragonflyRoutingControl::DeterministicMinimal,
                  DragonflyRoutingControl::DeterministicNonMinimal}) {
                const RouteTrace first = runRoute(routing, source, target, control, route_hash);
                const RouteTrace repeated = runRoute(routing, source, target, control, route_hash);
                ASSERT_EQ(first.outcome, DragonflyDecisionOutcome::Delivered)
                    << "source=" << source << " target=" << target
                    << " control=" << static_cast<unsigned int>(control)
                    << " reason=" << static_cast<unsigned int>(first.drop_reason);
                EXPECT_EQ(first.routers, repeated.routers);
                EXPECT_EQ(first.ports, repeated.ports);
                EXPECT_EQ(first.virtual_channels, repeated.virtual_channels);
                EXPECT_TRUE(virtualChannelsAreMonotonic(first.virtual_channels));
                EXPECT_LE(first.router_hops, routing.config().maximum_router_hops);

                const bool same_group = topology.groupOf(source) == topology.groupOf(target);
                if (control == DragonflyRoutingControl::DeterministicMinimal) {
                    EXPECT_EQ(first.global_hops, same_group ? 0U : 1U);
                    EXPECT_LE(first.router_hops, same_group ? 1U : 3U);
                } else {
                    EXPECT_EQ(first.global_hops, same_group ? 0U : 2U);
                    EXPECT_LE(first.router_hops, same_group ? 2U : 7U);
                    if (same_group) {
                        EXPECT_TRUE(first.target_root_router.has_value());
                    }
                    EXPECT_EQ(first.intermediate_root_router.has_value(), !same_group);
                    EXPECT_EQ(first.intermediate_group.has_value(), !same_group);
                }
            }
        }
    }
}

TEST(DragonflyProgressiveRoutingTest,
     EveryAdaptiveClassReachesEveryPairUnderBothForcedRouteClasses) {
    for (const bool force_non_minimal : {false, true}) {
        DragonflyProgressiveConfig config;
        for (auto& bias : config.adaptive_biases) {
            bias.minimal.additive = force_non_minimal ? 63 : 0;
            bias.non_minimal.additive = force_non_minimal ? 0 : 63;
        }
        const DragonflyProgressiveRouting routing(config);
        const auto& topology = routing.topology();
        for (DragonflyRouterId source = 0; source < topology.routerCount(); ++source) {
            for (DragonflyRouterId target = 0; target < topology.routerCount(); ++target) {
                if (source == target) {
                    continue;
                }
                for (const auto control :
                     {DragonflyRoutingControl::Adaptive0, DragonflyRoutingControl::Adaptive1,
                      DragonflyRoutingControl::Adaptive2, DragonflyRoutingControl::Adaptive3}) {
                    for (std::uint64_t route_hash = 0; route_hash < 4; ++route_hash) {
                        const RouteTrace trace =
                            runRoute(routing, source, target, control, route_hash);
                        ASSERT_EQ(trace.outcome, DragonflyDecisionOutcome::Delivered)
                            << "source=" << source << " target=" << target
                            << " control=" << static_cast<unsigned int>(control)
                            << " hash=" << route_hash << " force_non_minimal=" << force_non_minimal;
                        EXPECT_LE(trace.router_hops, 8U);
                        EXPECT_TRUE(virtualChannelsAreMonotonic(trace.virtual_channels));
                        if (topology.groupOf(source) != topology.groupOf(target)) {
                            EXPECT_EQ(trace.global_hops, force_non_minimal ? 2U : 1U);
                        }
                    }
                }
            }
        }
    }
}

TEST(DragonflyProgressiveRoutingTest,
     ProgressiveGmThenLnReconsiderationUsesTheEighthDependencyPhase) {
    DragonflyProgressiveRouting routing;
    const auto trace = findEightHopProgressiveTrace(routing);
    ASSERT_TRUE(trace.has_value());
    ASSERT_EQ(trace->outcome, DragonflyDecisionOutcome::Delivered);
    ASSERT_EQ(trace->router_hops, 8U);
    ASSERT_EQ(trace->virtual_channels.size(), 8U);
    EXPECT_EQ(trace->virtual_channels.back(), DragonflyVirtualChannel::DependencyHop7);
    EXPECT_TRUE(virtualChannelsAreMonotonic(trace->virtual_channels));
}

TEST(DragonflyProgressiveRoutingTest, EnumeratedDirectedLinkAndVcDependencyGraphIsAcyclic) {
    const DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    std::map<DragonflyDependencyChannel, std::set<DragonflyDependencyChannel>> dependencies;

    for (DragonflyRouterId source = 0; source < topology.routerCount(); ++source) {
        for (DragonflyRouterId target = 0; target < topology.routerCount(); ++target) {
            if (source == target) {
                continue;
            }
            const RouteTrace minimal =
                runRoute(routing, source, target, DragonflyRoutingControl::DeterministicMinimal,
                         (static_cast<std::uint64_t>(source) << 32) | target);
            ASSERT_EQ(minimal.outcome, DragonflyDecisionOutcome::Delivered);
            addDependencies(minimal, dependencies);

            std::set<DragonflyGroupId> intermediate_groups;
            std::set<DragonflyRouterId> local_roots;
            for (std::uint64_t route_hash = 0; route_hash < 64; ++route_hash) {
                const RouteTrace non_minimal =
                    runRoute(routing, source, target,
                             DragonflyRoutingControl::DeterministicNonMinimal, route_hash);
                ASSERT_EQ(non_minimal.outcome, DragonflyDecisionOutcome::Delivered);
                addDependencies(non_minimal, dependencies);
                if (non_minimal.intermediate_group.has_value()) {
                    intermediate_groups.insert(*non_minimal.intermediate_group);
                }
                if (non_minimal.target_root_router.has_value()) {
                    local_roots.insert(*non_minimal.target_root_router);
                }
            }
            if (topology.groupOf(source) != topology.groupOf(target)) {
                EXPECT_EQ(intermediate_groups.size(), topology.groupCount() - 2);
            } else {
                EXPECT_EQ(local_roots.size(), topology.routersPerGroup());
            }
        }
    }

    DragonflyProgressiveRouting adaptive;
    const auto eight_hop = findEightHopProgressiveTrace(adaptive);
    ASSERT_TRUE(eight_hop.has_value());
    addDependencies(*eight_hop, dependencies);

    EXPECT_FALSE(dependencies.empty());
    EXPECT_TRUE(dependencyGraphIsAcyclic(dependencies));
}

TEST(DragonflyProgressiveRoutingTest, DeterministicChoiceDoesNotDependOnCongestionFeedback) {
    DragonflyProgressiveRouting clear;
    DragonflyProgressiveRouting congested;
    setAllCongestion(congested, {15, 15, 15});
    for (DragonflyRoutingControl control : {DragonflyRoutingControl::DeterministicMinimal,
                                            DragonflyRoutingControl::DeterministicNonMinimal}) {
        const RouteTrace clear_trace = runRoute(clear, 0, 35, control, 829);
        const RouteTrace congested_trace = runRoute(congested, 0, 35, control, 829);
        ASSERT_EQ(clear_trace.outcome, DragonflyDecisionOutcome::Delivered);
        ASSERT_EQ(congested_trace.outcome, DragonflyDecisionOutcome::Delivered);
        EXPECT_EQ(clear_trace.routers, congested_trace.routers);
        EXPECT_EQ(clear_trace.ports, congested_trace.ports);
    }
}

TEST(DragonflyProgressiveRoutingTest,
     LocalFailuresApplyImmediatelyButSafeGroupKnowledgeArrivesCausally) {
    DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    const DragonflyGroupId group_zero = 0;
    const DragonflyGroupId group_one = 1;
    const DragonflyRouterId connector = topology.groupConnector(group_zero, group_one);
    const auto output = topology.globalPortToGroup(connector, group_one);
    ASSERT_TRUE(output.has_value());
    const auto& link = topology.port(connector, *output);
    const auto reverse = topology.reciprocalPort(connector, *output);
    routing.setPhysicalLinkAlive(connector, *output, false);
    EXPECT_FALSE(routing.portAlive(connector, *output));
    EXPECT_FALSE(routing.portAlive(link.next_router, reverse));

    auto state = routing.initialize(connector, 4, DragonflyRoutingControl::DeterministicMinimal, 1);
    const auto failed_initial = routing.nextHop(connector, state);
    EXPECT_EQ(failed_initial.outcome, DragonflyDecisionOutcome::Dropped);
    EXPECT_EQ(failed_initial.drop_reason, DragonflyDropReason::NoLegalCandidate);

    constexpr DragonflyRouterId observer = 8;
    const auto before = routing.globalNonMinimal(observer, 0);
    ASSERT_NE(std::find_if(
                  before.begin(), before.end(),
                  [group_one](const auto& entry) { return entry.intermediate_group == group_one; }),
              before.end());
    const DragonflyRouteUpdateAdvertisement unsafe_group{
        observer, DragonflyRouteUpdateKind::IntermediateGroupSafe, group_one, false, 9, 1'000,
        1'020};
    EXPECT_EQ(routing.consumeRouteUpdate(unsafe_group, 1'019),
              DragonflyRouteUpdateDisposition::NotYetPhysicallyArrived);
    EXPECT_TRUE(routing.groupSafe(observer, group_one));
    EXPECT_EQ(routing.consumeRouteUpdate(unsafe_group, 1'020),
              DragonflyRouteUpdateDisposition::Accepted);
    EXPECT_FALSE(routing.groupSafe(observer, group_one));
    const auto after = routing.globalNonMinimal(observer, 0);
    EXPECT_EQ(std::find_if(
                  after.begin(), after.end(),
                  [group_one](const auto& entry) { return entry.intermediate_group == group_one; }),
              after.end());
}

TEST(DragonflyProgressiveRoutingTest, ACommittedRouteReportsADeadDownstreamLink) {
    DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    const DragonflyRouterId source = 1;
    const DragonflyRouterId target = 4;
    auto state =
        routing.initialize(source, target, DragonflyRoutingControl::DeterministicMinimal, 3);
    const auto first = routing.nextHop(source, state);
    ASSERT_EQ(first.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_TRUE(first.next_router.has_value());
    const DragonflyRouterId connector = *first.next_router;
    ASSERT_EQ(connector, topology.groupConnector(0, 1));
    const auto global = topology.globalPortToGroup(connector, 1);
    ASSERT_TRUE(global.has_value());
    routing.setPhysicalLinkAlive(connector, *global, false);

    const auto second = routing.nextHop(connector, state);
    EXPECT_EQ(second.outcome, DragonflyDecisionOutcome::Dropped);
    EXPECT_EQ(second.drop_reason, DragonflyDropReason::FailedCommittedLink);
}

TEST(DragonflyProgressiveRoutingTest,
     RemoteFailureIsNotAnOracleAndOnlyAnArrivedRouteUpdateChangesInjection) {
    DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    const DragonflyRouterId source = 1;
    const DragonflyRouterId target = 4;
    const DragonflyRouterId connector = topology.groupConnector(0, 1);
    const auto global = topology.globalPortToGroup(connector, 1);
    ASSERT_TRUE(global.has_value());
    // A source with only a local output cannot read the connector's live port.
    // It forwards to the connector, which then observes its own failed output.
    routing.setPhysicalLinkAlive(connector, *global, false);
    auto state =
        routing.initialize(source, target, DragonflyRoutingControl::DeterministicMinimal, 7);
    auto decision = routing.nextHop(source, state);
    ASSERT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
    ASSERT_EQ(decision.next_router, connector);
    decision = routing.nextHop(connector, state);
    EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Dropped);
    EXPECT_EQ(decision.drop_reason, DragonflyDropReason::FailedCommittedLink);

    // The route-table update has an explicit physical arrival boundary.  The
    // same source still forwards one picosecond before it, then rejects a new
    // injection at the exact arrival time.
    const DragonflyRouteUpdateAdvertisement unavailable{
        source, DragonflyRouteUpdateKind::DestinationAvailable, 1, false, 4, 200, 220};
    EXPECT_EQ(routing.consumeRouteUpdate(unavailable, 219),
              DragonflyRouteUpdateDisposition::NotYetPhysicallyArrived);
    state = routing.initialize(source, target, DragonflyRoutingControl::DeterministicMinimal, 7);
    decision = routing.nextHop(source, state);
    EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Forward);
    EXPECT_EQ(routing.consumeRouteUpdate(unavailable, 220),
              DragonflyRouteUpdateDisposition::Accepted);
    state = routing.initialize(source, target, DragonflyRoutingControl::DeterministicMinimal, 7);
    decision = routing.nextHop(source, state);
    EXPECT_EQ(decision.outcome, DragonflyDecisionOutcome::Dropped);
    EXPECT_EQ(decision.drop_reason, DragonflyDropReason::NoLegalCandidate);
}

TEST(DragonflyProgressiveRoutingTest, GnExcludesAnIntermediateOnlyAfterItsSafetyUpdateArrives) {
    DragonflyProgressiveRouting routing;
    const auto& topology = routing.topology();
    constexpr DragonflyRouterId source = 1;
    constexpr DragonflyGroupId intermediate = 1;
    constexpr DragonflyGroupId final_target = 8;

    const auto before = routing.globalNonMinimal(source, final_target);
    ASSERT_NE(std::find_if(before.begin(), before.end(),
                           [intermediate](const auto& entry) {
                               return entry.intermediate_group == intermediate;
                           }),
              before.end());

    const DragonflyRouterId connector =
        topology.groupConnector(topology.groupOf(source), intermediate);
    const auto global = topology.globalPortToGroup(connector, intermediate);
    ASSERT_TRUE(global.has_value());
    routing.setPhysicalLinkAlive(connector, *global, false);

    // The physical mutation is remote from source, so it cannot change the
    // source's GN table synchronously.
    const auto before_advertisement = routing.globalNonMinimal(source, final_target);
    EXPECT_NE(std::find_if(before_advertisement.begin(), before_advertisement.end(),
                           [intermediate](const auto& entry) {
                               return entry.intermediate_group == intermediate;
                           }),
              before_advertisement.end());

    const DragonflyRouteUpdateAdvertisement unsafe_group{
        source, DragonflyRouteUpdateKind::IntermediateGroupSafe, intermediate, false, 2, 500, 525};
    EXPECT_EQ(routing.consumeRouteUpdate(unsafe_group, 524),
              DragonflyRouteUpdateDisposition::NotYetPhysicallyArrived);
    EXPECT_EQ(routing.consumeRouteUpdate(unsafe_group, 525),
              DragonflyRouteUpdateDisposition::Accepted);
    const auto after = routing.globalNonMinimal(source, final_target);
    EXPECT_EQ(std::find_if(after.begin(), after.end(),
                           [intermediate](const auto& entry) {
                               return entry.intermediate_group == intermediate;
                           }),
              after.end());
    EXPECT_EQ(after.size() + 1, before.size());
}

}  // namespace
