// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_control.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

RnicSenderGrantGate& findGate(
        std::vector<RnicSenderGrantGate>& gates, uint64_t flow_id) {
    for (RnicSenderGrantGate& gate : gates) {
        if (gate.flowId() == flow_id) {
            return gate;
        }
    }
    throw std::logic_error("test sender gate not found");
}

void deliverGrant(RnicSenderGrantGate& gate,
                  const RnicCollectiveGrant& grant,
                  uint64_t arrival_time_ps) {
    if (grant.kind == RnicCollectiveGrantKind::Accept) {
        gate.receiveAccept(grant, arrival_time_ps);
    } else if (grant.kind == RnicCollectiveGrantKind::Update) {
        gate.receiveGrantUpdate(grant, arrival_time_ps);
    } else {
        throw std::logic_error("test attempted to deliver an invalid grant");
    }
}

void deliverWave(const RnicCollectiveGrantWave& wave,
                 std::vector<RnicSenderGrantGate>& gates,
                 uint64_t arrival_time_ps) {
    for (const RnicCollectiveGrant& grant : wave.grants) {
        deliverGrant(findGate(gates, grant.flow_id), grant, arrival_time_ps);
    }
}

std::vector<RnicSenderGrantGate*> gatesForWave(
        const RnicCollectiveGrantWave& wave,
        std::vector<RnicSenderGrantGate>& gates) {
    std::vector<RnicSenderGrantGate*> result;
    result.reserve(wave.grants.size());
    for (const RnicCollectiveGrant& grant : wave.grants) {
        result.push_back(&findGate(gates, grant.flow_id));
    }
    return result;
}

size_t activateWave(const RnicCollectiveGrantWave& wave,
                    std::vector<RnicSenderGrantGate>& gates,
                    RnicCollectiveController& controller,
                    uint64_t now_ps) {
    return RnicCollectiveGrantWaveBarrier::activate(
        wave, gatesForWave(wave, gates), controller, now_ps);
}

uint64_t aggregateWireRate(
        const std::vector<RnicSenderGrantGate>& gates) {
    uint64_t result = 0;
    for (const RnicSenderGrantGate& gate : gates) {
        result += gate.currentWireRateBps();
    }
    return result;
}

RnicCollectiveMembershipDelta unitDelta(
        std::initializer_list<uint64_t> declarations,
        std::initializer_list<uint64_t> retirements) {
    RnicCollectiveMembershipDelta delta;
    delta.declarations.reserve(declarations.size());
    for (const uint64_t flow_id : declarations) {
        delta.declarations.push_back({flow_id, 1});
    }
    delta.retired_flow_ids.assign(
        retirements.begin(), retirements.end());
    return delta;
}

RnicCollectiveMembershipDelta unitDelta(
        const std::vector<uint64_t>& declarations,
        std::initializer_list<uint64_t> retirements) {
    RnicCollectiveMembershipDelta delta;
    delta.declarations.reserve(declarations.size());
    for (const uint64_t flow_id : declarations) {
        delta.declarations.push_back({flow_id, 1});
    }
    delta.retired_flow_ids.assign(
        retirements.begin(), retirements.end());
    return delta;
}

TEST(RnicCollectiveControllerTest, GrantIsDirectMarginCapacityDivision) {
    RnicCollectiveController controller(400000000000ULL, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(1);
    gates[0].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> one =
        controller.beginMembershipWave(unitDelta({1}, {}), 0);
    ASSERT_TRUE(one.has_value());
    ASSERT_EQ(one->grants.size(), 1u);
    EXPECT_EQ(one->wire_rate_bps, 360000000000ULL);
    EXPECT_EQ(one->n_hat, 1u);
    deliverWave(*one, gates, 50);
    activateWave(*one, gates, controller, 100);

    gates.emplace_back(2);
    gates[1].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> two =
        controller.beginMembershipWave(unitDelta({2}, {}), 100);
    ASSERT_TRUE(two.has_value());
    ASSERT_EQ(two->grants.size(), 2u);
    EXPECT_EQ(two->wire_rate_bps, 180000000000ULL);
    EXPECT_EQ(two->n_hat, 2u);
}

TEST(RnicCollectiveControllerTest, DeclaredNflowDefinesEffectiveCount) {
    RnicCollectiveController controller(1000, 100);
    const RnicCollectiveMembershipDelta declaration{
        {{10, 3}}, {}};

    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(declaration, 0);

    ASSERT_TRUE(wave.has_value());
    EXPECT_EQ(controller.activeFlowCount(), 1u);
    EXPECT_EQ(controller.effectiveFlowCount(), 3u);
    EXPECT_EQ(wave->n_hat, 3u);
    EXPECT_EQ(wave->wire_rate_bps, 300u);
    ASSERT_EQ(wave->grants.size(), 1u);
    EXPECT_EQ(wave->grants[0].n_hat, 3u);
}

TEST(RnicCollectiveControllerTest, MembershipChangeProducesOneVerticalStep) {
    RnicCollectiveController controller(1000, 100, 900000);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(10);
    gates[0].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> before =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(before.has_value());
    deliverWave(*before, gates, 50);
    activateWave(*before, gates, controller, 100);

    gates.emplace_back(11);
    gates[1].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> after =
        controller.beginMembershipWave(unitDelta({11}, {}), 100);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(before->wire_rate_bps, 900u);
    EXPECT_EQ(after->wire_rate_bps, 450u);
    EXPECT_GT(after->membership_epoch, before->membership_epoch);
}

TEST(RnicCollectiveControllerTest, DeclarationAndRetirementAreIdempotent) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> declaration =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(declaration.has_value());
    deliverWave(*declaration, gates, 50);
    activateWave(*declaration, gates, controller, 100);
    const uint64_t declared_epoch = controller.membershipEpoch();
    EXPECT_FALSE(
        controller.beginMembershipWave(unitDelta({10}, {}), 100)
            .has_value());
    EXPECT_EQ(controller.membershipEpoch(), declared_epoch);
    EXPECT_THROW(
        controller.beginMembershipWave({{{10, 2}}, {}}, 100),
        std::invalid_argument);
    EXPECT_EQ(controller.membershipEpoch(), declared_epoch);
    EXPECT_EQ(controller.effectiveFlowCount(), 1u);

    const std::optional<RnicCollectiveGrantWave> retirement =
        controller.beginMembershipWave(unitDelta({}, {10}), 100);
    ASSERT_TRUE(retirement.has_value());
    std::vector<RnicSenderGrantGate*> no_gates;
    RnicCollectiveGrantWaveBarrier::activate(
        *retirement, no_gates, controller, 200);
    gates[0].receiverRetirementCommitted();
    const uint64_t retired_epoch = controller.membershipEpoch();
    EXPECT_FALSE(
        controller.beginMembershipWave(unitDelta({}, {10}), 200)
            .has_value());
    EXPECT_EQ(controller.membershipEpoch(), retired_epoch);
}

TEST(RnicCollectiveControllerTest, RetirementChangesTheNextReceiverGrant) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(10);
    gates.emplace_back(11);
    gates[0].declarationDispatched();
    gates[1].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> initial =
        controller.beginMembershipWave(unitDelta({10, 11}, {}), 0);
    ASSERT_TRUE(initial.has_value());
    EXPECT_EQ(initial->wire_rate_bps, 450u);
    deliverWave(*initial, gates, 50);
    activateWave(*initial, gates, controller, 100);

    const std::optional<RnicCollectiveGrantWave> retirement =
        controller.beginMembershipWave(unitDelta({}, {11}), 100);
    ASSERT_TRUE(retirement.has_value());
    EXPECT_EQ(retirement->wire_rate_bps, 900u);
    ASSERT_EQ(retirement->grants.size(), 1u);
    EXPECT_EQ(retirement->grants[0].flow_id, 10u);
    deliverWave(*retirement, gates, 150);
    activateWave(*retirement, gates, controller, 200);
    gates[1].receiverRetirementCommitted();
    EXPECT_EQ(gates[0].currentWireRateBps(), 900u);
    EXPECT_FALSE(gates[1].dataEligible());
}

TEST(RnicCollectiveControllerTest, GrantsForAllAreStableByFlowIdAndKind) {
    RnicCollectiveController controller(1000, 1234);
    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(unitDelta({20, 10}, {}), 0);
    ASSERT_TRUE(wave.has_value());
    const std::vector<RnicCollectiveGrant>& grants = wave->grants;
    ASSERT_EQ(grants.size(), 2u);
    EXPECT_EQ(grants[0].flow_id, 10u);
    EXPECT_EQ(grants[1].flow_id, 20u);
    EXPECT_EQ(grants[0].kind, RnicCollectiveGrantKind::Accept);
    EXPECT_EQ(grants[1].kind, RnicCollectiveGrantKind::Accept);
    EXPECT_EQ(grants[0].membership_epoch, grants[1].membership_epoch);
    EXPECT_EQ(grants[0].effective_time_ps, 1234u);
    EXPECT_EQ(grants[1].effective_time_ps, 1234u);
}

TEST(RnicCollectiveControllerTest, MembershipBatchCreatesOneImmutableWave) {
    RnicCollectiveController controller(1000, 500);
    const std::optional<RnicCollectiveGrantWave> result =
        controller.beginMembershipWave(unitDelta({30, 10, 20}, {}), 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(controller.membershipEpoch(), 1u);

    const RnicCollectiveGrantWave& wave = *result;
    EXPECT_EQ(wave.membership_epoch, 1u);
    EXPECT_EQ(wave.n_hat, 3u);
    EXPECT_EQ(wave.wire_rate_bps, 300u);
    EXPECT_EQ(wave.effective_time_ps, 500u);
    ASSERT_EQ(wave.grants.size(), 3u);
    EXPECT_EQ(wave.grants[0].flow_id, 10u);
    EXPECT_EQ(wave.grants[1].flow_id, 20u);
    EXPECT_EQ(wave.grants[2].flow_id, 30u);
    for (const RnicCollectiveGrant& grant : wave.grants) {
        EXPECT_EQ(grant.membership_epoch, wave.membership_epoch);
        EXPECT_EQ(grant.n_hat, wave.n_hat);
        EXPECT_EQ(grant.wire_rate_bps, wave.wire_rate_bps);
        EXPECT_EQ(grant.effective_time_ps, wave.effective_time_ps);
        EXPECT_EQ(grant.kind, RnicCollectiveGrantKind::Accept);
    }
}

TEST(RnicCollectiveControllerTest, MembershipMutationIsTransactional) {
    RnicCollectiveController controller(1000, 100);
    EXPECT_THROW(
        controller.beginMembershipWave(unitDelta({10, 10}, {}), 0),
                 std::invalid_argument);
    EXPECT_THROW(
        controller.beginMembershipWave(unitDelta({}, {10, 10}), 0),
                 std::invalid_argument);
    EXPECT_THROW(
        controller.beginMembershipWave(unitDelta({10}, {10}), 0),
                 std::invalid_argument);
    EXPECT_THROW(controller.beginMembershipWave({{{10, 0}}, {}}, 0),
                 std::invalid_argument);
    EXPECT_EQ(controller.membershipEpoch(), 0u);
    EXPECT_EQ(controller.activeFlowCount(), 0u);

    RnicCollectiveController sub_bit_capacity(1, 100);
    EXPECT_THROW(
        sub_bit_capacity.beginMembershipWave(unitDelta({1}, {}), 0),
                 std::overflow_error);
    EXPECT_EQ(sub_bit_capacity.membershipEpoch(), 0u);
    EXPECT_EQ(sub_bit_capacity.activeFlowCount(), 0u);

    RnicCollectiveController overflowing_nflow(20000000000000ULL, 100);
    EXPECT_THROW(
        overflowing_nflow.beginMembershipWave(
            {{{1, std::numeric_limits<uint32_t>::max()}, {2, 1}}, {}},
            0),
        std::overflow_error);
    EXPECT_EQ(overflowing_nflow.membershipEpoch(), 0u);
    EXPECT_EQ(overflowing_nflow.activeFlowCount(), 0u);
}

TEST(RnicCollectiveControllerWaveTest,
     SerializesReceiverWavesAtDeadline) {
    RnicCollectiveController controller(1000, 80);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(3);
    gates.emplace_back(10);
    gates.emplace_back(20);
    gates[0].declarationDispatched();
    gates[1].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> first =
        controller.beginMembershipWave(unitDelta({10, 20}, {}), 100);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->membership_epoch, 1u);
    EXPECT_EQ(first->effective_time_ps, 180u);
    EXPECT_TRUE(controller.waveOutstanding());
    EXPECT_EQ(controller.outstandingEpoch(), 1u);
    EXPECT_EQ(controller.outstandingEffectiveTimePs(), 180u);
    EXPECT_THROW(controller.beginMembershipWave(
                     unitDelta({30}, {}), 120),
                 std::logic_error);

    deliverWave(*first, gates, 170);
    EXPECT_THROW(activateWave(*first, gates, controller, 179),
                 std::invalid_argument);
    EXPECT_FALSE(gates[0].dataEligible());
    EXPECT_FALSE(gates[1].dataEligible());
    EXPECT_EQ(activateWave(*first, gates, controller, 180), 2u);

    gates.emplace_back(30);
    gates.back().declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> second =
        controller.beginMembershipWave(unitDelta({30}, {}), 180);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->membership_epoch, 2u);
    EXPECT_EQ(second->effective_time_ps, 260u);
    deliverWave(*second, gates, 260);
    EXPECT_EQ(activateWave(*second, gates, controller, 260), 3u);
}

TEST(RnicCollectiveControllerWaveTest, NoOpAndFailureAreTransactional) {
    RnicCollectiveController controller(1000, 80);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> initial =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(initial.has_value());
    deliverWave(*initial, gates, 80);
    activateWave(*initial, gates, controller, 80);

    EXPECT_FALSE(
        controller.beginMembershipWave(unitDelta({10}, {}), 80)
            .has_value());
    EXPECT_FALSE(controller.waveOutstanding());
    EXPECT_EQ(controller.membershipEpoch(), 1u);

    EXPECT_THROW(
        controller.beginMembershipWave(
            unitDelta({20}, {}),
            std::numeric_limits<uint64_t>::max() - 79),
        std::overflow_error);
    EXPECT_FALSE(controller.contains(20));
    EXPECT_FALSE(controller.waveOutstanding());
}

TEST(RnicCollectiveControllerWaveTest,
     RedundantDeclarationDoesNotTurnUpdateIntoAccept) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(10);
    gates[0].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> initial =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(initial.has_value());
    deliverWave(*initial, gates, 90);
    activateWave(*initial, gates, controller, 100);

    gates.emplace_back(20);
    gates[1].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> mixed =
        controller.beginMembershipWave(unitDelta({10, 20}, {}), 100);
    ASSERT_TRUE(mixed.has_value());
    ASSERT_EQ(mixed->grants.size(), 2u);
    EXPECT_EQ(mixed->grants[0].flow_id, 10u);
    EXPECT_EQ(mixed->grants[0].kind, RnicCollectiveGrantKind::Update);
    EXPECT_EQ(mixed->grants[1].flow_id, 20u);
    EXPECT_EQ(mixed->grants[1].kind, RnicCollectiveGrantKind::Accept);

    deliverWave(*mixed, gates, 190);
    activateWave(*mixed, gates, controller, 200);
    EXPECT_TRUE(gates[0].dataEligible());
    EXPECT_TRUE(gates[1].dataEligible());
}

TEST(RnicCollectiveControllerWaveTest, EmptyRetirementWaveCompletes) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> initial =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(initial.has_value());
    deliverWave(*initial, gates, 50);
    activateWave(*initial, gates, controller, 100);
    const std::optional<RnicCollectiveGrantWave> retirement =
        controller.beginMembershipWave(unitDelta({}, {10}), 100);
    ASSERT_TRUE(retirement.has_value());
    EXPECT_EQ(retirement->n_hat, 0u);
    EXPECT_TRUE(retirement->grants.empty());
    std::vector<RnicSenderGrantGate*> no_gates;
    EXPECT_EQ(RnicCollectiveGrantWaveBarrier::activate(
                  *retirement, no_gates, controller, 200),
              0u);
    gates[0].receiverRetirementCommitted();
    EXPECT_FALSE(controller.waveOutstanding());
    EXPECT_EQ(controller.activeFlowCount(), 0u);
}

TEST(RnicSenderGrantGateTest, DataIsHardGatedUntilAtomicBoundary) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(wave.has_value());
    deliverWave(*wave, gates, 50);
    EXPECT_EQ(gates[0].phase(),
              RnicSenderGrantGate::Phase::AcceptPendingEffectiveTime);
    EXPECT_FALSE(gates[0].dataEligible());
    EXPECT_EQ(gates[0].currentWireRateBps(), 0u);

    EXPECT_THROW(activateWave(*wave, gates, controller, 99),
                 std::invalid_argument);
    EXPECT_FALSE(gates[0].dataEligible());
    EXPECT_EQ(activateWave(*wave, gates, controller, 100), 1u);
    EXPECT_TRUE(gates[0].dataEligible());
    EXPECT_EQ(gates[0].currentWireRateBps(), 900u);
}

TEST(RnicSenderGrantGateTest, ActiveSenderStepsDirectlyAtWaveBoundary) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(10);
    gates[0].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> initial =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(initial.has_value());
    deliverWave(*initial, gates, 50);
    activateWave(*initial, gates, controller, 100);

    gates.emplace_back(20);
    gates[1].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> joined =
        controller.beginMembershipWave(unitDelta({20}, {}), 100);
    ASSERT_TRUE(joined.has_value());
    deliverWave(*joined, gates, 150);
    EXPECT_EQ(gates[0].currentWireRateBps(), 900u);
    EXPECT_EQ(gates[1].currentWireRateBps(), 0u);
    activateWave(*joined, gates, controller, 200);
    EXPECT_EQ(gates[0].currentWireRateBps(), 450u);
    EXPECT_EQ(gates[1].currentWireRateBps(), 450u);

    EXPECT_FALSE(gates[0].receiveAccept(initial->grants[0], 1000));
    EXPECT_EQ(gates[0].currentWireRateBps(), 450u);
}

TEST(RnicSenderGrantGateTest, JoinSweepNeverExceedsMarginCapacity) {
    const uint64_t capacity = 1000;
    for (uint64_t incumbent_count = 1; incumbent_count <= 16;
         ++incumbent_count) {
        RnicCollectiveController controller(capacity, 100);
        std::vector<RnicSenderGrantGate> gates;
        std::vector<uint64_t> initial_ids;
        gates.reserve(incumbent_count + 1);
        initial_ids.reserve(incumbent_count);
        for (uint64_t i = 0; i < incumbent_count; ++i) {
            initial_ids.push_back(i + 1);
            gates.emplace_back(i + 1);
            gates.back().declarationDispatched();
        }

        const std::optional<RnicCollectiveGrantWave> initial =
            controller.beginMembershipWave(unitDelta(initial_ids, {}), 0);
        ASSERT_TRUE(initial.has_value());
        deliverWave(*initial, gates, 99);
        activateWave(*initial, gates, controller, 100);
        EXPECT_LE(aggregateWireRate(gates), 900u);

        const uint64_t joiner_id = incumbent_count + 1;
        gates.emplace_back(joiner_id);
        gates.back().declarationDispatched();
        const std::optional<RnicCollectiveGrantWave> joined =
            controller.beginMembershipWave(
                unitDelta({joiner_id}, {}), 100);
        ASSERT_TRUE(joined.has_value());

        // The ACCEPT may physically arrive first. It remains gated while the
        // incumbent UPDATE packets arrive in arbitrary order before T.
        for (std::vector<RnicCollectiveGrant>::const_reverse_iterator it =
                 joined->grants.rbegin();
             it != joined->grants.rend(); ++it) {
            deliverGrant(findGate(gates, it->flow_id), *it, 199);
        }
        EXPECT_LE(aggregateWireRate(gates), 900u);
        EXPECT_FALSE(gates.back().dataEligible());

        activateWave(*joined, gates, controller, 200);
        EXPECT_LE(aggregateWireRate(gates), 900u);
        EXPECT_TRUE(gates.back().dataEligible());
    }
}

TEST(RnicSenderGrantGateTest, MissingFeedbackCannotPartiallyActivateWave) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(10);
    gates[0].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> initial =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(initial.has_value());
    deliverWave(*initial, gates, 50);
    activateWave(*initial, gates, controller, 100);

    gates.emplace_back(20);
    gates[1].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> joined =
        controller.beginMembershipWave(unitDelta({20}, {}), 100);
    ASSERT_TRUE(joined.has_value());
    ASSERT_EQ(joined->grants.size(), 2u);
    deliverGrant(gates[1], joined->grants[1], 150);

    EXPECT_THROW(activateWave(*joined, gates, controller, 200),
                 std::logic_error);
    EXPECT_TRUE(controller.waveOutstanding());
    EXPECT_EQ(gates[0].currentWireRateBps(), 900u);
    EXPECT_EQ(gates[1].currentWireRateBps(), 0u);
    EXPECT_FALSE(gates[1].dataEligible());

    // Arrival at T is timely. The runtime's same-time microphase delivers all
    // feedback first, then invokes the barrier, then permits DATA dispatch.
    deliverGrant(gates[0], joined->grants[0], 200);
    activateWave(*joined, gates, controller, 200);
    EXPECT_EQ(gates[0].currentWireRateBps(), 450u);
    EXPECT_EQ(gates[1].currentWireRateBps(), 450u);
}

TEST(RnicSenderGrantGateTest, ControllerAuthenticatesImmutableWave) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.reserve(2);
    gates.emplace_back(10);
    gates.emplace_back(20);
    gates[0].declarationDispatched();
    gates[1].declarationDispatched();

    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(unitDelta({10, 20}, {}), 0);
    ASSERT_TRUE(wave.has_value());
    deliverWave(*wave, gates, 50);

    RnicCollectiveGrantWave changed = *wave;
    changed.wire_rate_bps = 899;
    EXPECT_THROW(activateWave(changed, gates, controller, 100),
                 std::invalid_argument);
    EXPECT_FALSE(gates[0].dataEligible());
    EXPECT_FALSE(gates[1].dataEligible());
    EXPECT_TRUE(controller.waveOutstanding());

    activateWave(*wave, gates, controller, 100);
    EXPECT_TRUE(gates[0].dataEligible());
    EXPECT_TRUE(gates[1].dataEligible());
}

TEST(RnicSenderGrantGateTest, EveryGrantArrivalPermutationCommitsOnce) {
    std::vector<size_t> order{0, 1, 2};
    do {
        RnicCollectiveController controller(1000, 100);
        std::vector<RnicSenderGrantGate> gates;
        gates.reserve(3);
        for (uint64_t flow_id = 1; flow_id <= 3; ++flow_id) {
            gates.emplace_back(flow_id);
            gates.back().declarationDispatched();
        }
        const std::optional<RnicCollectiveGrantWave> wave =
            controller.beginMembershipWave(
                unitDelta({1, 2, 3}, {}), 0);
        ASSERT_TRUE(wave.has_value());
        for (const size_t index : order) {
            deliverGrant(findGate(gates, wave->grants[index].flow_id),
                         wave->grants[index], 50 + index);
        }
        EXPECT_EQ(activateWave(*wave, gates, controller, 100), 3u);
        EXPECT_EQ(aggregateWireRate(gates), 900u);
        for (const RnicSenderGrantGate& gate : gates) {
            EXPECT_TRUE(gate.dataEligible());
            EXPECT_EQ(gate.membershipEpoch(), 1u);
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(RnicSenderGrantGateTest,
     DuplicateIsIdempotentAndConflictOrWrongKindIsRejected) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(wave.has_value());
    const RnicCollectiveGrant accept = wave->grants[0];

    EXPECT_TRUE(gates[0].receiveAccept(accept, 50));
    EXPECT_FALSE(gates[0].receiveAccept(accept, 60));
    RnicCollectiveGrant conflict = accept;
    conflict.wire_rate_bps = 800;
    EXPECT_THROW(gates[0].receiveAccept(conflict, 60),
                 std::invalid_argument);
    EXPECT_THROW(gates[0].receiveGrantUpdate(accept, 60),
                 std::invalid_argument);

    activateWave(*wave, gates, controller, 100);
    EXPECT_FALSE(gates[0].receiveAccept(accept, 1000));
    RnicCollectiveGrant wrong_equal_epoch_kind = accept;
    wrong_equal_epoch_kind.kind = RnicCollectiveGrantKind::Update;
    EXPECT_THROW(gates[0].receiveGrantUpdate(wrong_equal_epoch_kind, 100),
                 std::invalid_argument);
}

TEST(RnicSenderGrantGateTest, MissedEffectiveDeadlineFailsClosed) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(wave.has_value());

    EXPECT_THROW(gates[0].receiveAccept(wave->grants[0], 101),
                 std::runtime_error);
    EXPECT_FALSE(gates[0].dataEligible());
    EXPECT_EQ(gates[0].pendingGrantCount(), 0u);
}

TEST(RnicSenderGrantGateTest, RetirementClosesDataGate) {
    RnicCollectiveController controller(1000, 100);
    std::vector<RnicSenderGrantGate> gates;
    gates.emplace_back(10);
    gates[0].declarationDispatched();
    const std::optional<RnicCollectiveGrantWave> wave =
        controller.beginMembershipWave(unitDelta({10}, {}), 0);
    ASSERT_TRUE(wave.has_value());
    deliverWave(*wave, gates, 50);
    activateWave(*wave, gates, controller, 100);
    const std::optional<RnicCollectiveGrantWave> retirement =
        controller.beginMembershipWave(unitDelta({}, {10}), 100);
    ASSERT_TRUE(retirement.has_value());
    std::vector<RnicSenderGrantGate*> no_gates;
    RnicCollectiveGrantWaveBarrier::activate(
        *retirement, no_gates, controller, 200);
    gates[0].receiverRetirementCommitted();

    EXPECT_FALSE(gates[0].dataEligible());
    EXPECT_EQ(gates[0].currentWireRateBps(), 0u);
    EXPECT_FALSE(gates[0].receiveGrantUpdate(
        {10, 2, 1, 900, 200, RnicCollectiveGrantKind::Update}, 150));
    gates[0].receiverRetirementCommitted();
}

TEST(RnicCollectiveControlTest, RejectsInvalidControlStateAndParameters) {
    EXPECT_THROW(RnicCollectiveController(0, 100), std::invalid_argument);
    EXPECT_THROW(RnicCollectiveController(1000, 0), std::invalid_argument);
    EXPECT_THROW(RnicCollectiveController(1000, 100, 1000001),
                 std::invalid_argument);
    const uint64_t overflowing_capacity =
        std::numeric_limits<uint64_t>::max();
    EXPECT_THROW((void)RnicCollectiveController(overflowing_capacity, 100),
                 std::invalid_argument);

    RnicSenderGrantGate gate(10);
    EXPECT_THROW(gate.receiverRetirementCommitted(), std::logic_error);
    EXPECT_THROW(gate.receiveAccept(
                     {10, 1, 1, 900, 100,
                      RnicCollectiveGrantKind::Accept},
                     50),
                 std::logic_error);
    gate.declarationDispatched();
    EXPECT_THROW(gate.declarationDispatched(), std::logic_error);
    EXPECT_THROW(gate.receiverRetirementCommitted(), std::logic_error);
    EXPECT_THROW(gate.receiveAccept(
                     {11, 1, 1, 900, 100,
                      RnicCollectiveGrantKind::Accept},
                     50),
                 std::invalid_argument);
    EXPECT_THROW(gate.receiveAccept(
                     {10, 1, 0, 900, 100,
                      RnicCollectiveGrantKind::Accept},
                     50),
                 std::invalid_argument);
    EXPECT_THROW(gate.receiveAccept(
                     {10, 1, 1, 0, 100,
                      RnicCollectiveGrantKind::Accept},
                     50),
                 std::invalid_argument);
    EXPECT_THROW(gate.receiveGrantUpdate(
                     {10, 1, 1, 900, 100,
                      RnicCollectiveGrantKind::Invalid},
                     50),
                 std::invalid_argument);
}

}  // namespace
