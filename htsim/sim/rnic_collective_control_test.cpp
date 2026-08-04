// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_control.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr std::uint64_t kCapacity = UINT64_C(100000000000);
// dwnd: the one-way control deadline anchors the receiver window grid.
constexpr std::uint64_t kDwnd = 1000;
constexpr std::uint64_t kMarginDeratedCapacity = UINT64_C(90000000000);

RnicCollectiveMembershipDelta declare(
        std::uint64_t flow_id,
        std::uint32_t nflow_ppm = RnicCollectiveController::kFullFlowPpm) {
    return {{{flow_id, nflow_ppm}}, {}};
}

RnicSenderGrantGate activeGate(
        std::uint64_t flow_id,
        std::uint32_t own_nflow_ppm = RnicCollectiveController::kFullFlowPpm,
        std::uint64_t startup_capacity_bps = kMarginDeratedCapacity) {
    RnicSenderGrantGate gate(flow_id);
    gate.declarationDispatched(startup_capacity_bps, own_nflow_ppm, kDwnd);
    return gate;
}

TEST(RnicCollectiveControllerTest, DirectRateUsesDeclaredNflowAndMargin) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    const auto first = controller.updateMembership(
        {{{11, RnicCollectiveController::kFullFlowPpm},
          {12, RnicCollectiveController::kFullFlowPpm},
          {13, RnicCollectiveController::kFullFlowPpm}},
         {}});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->membership_epoch, 1U);
    EXPECT_EQ(first->n_hat, 3000000U);
    EXPECT_EQ(first->wire_rate_bps, UINT64_C(30000000000));
    EXPECT_EQ(first->accepted_flow_ids, (std::vector<std::uint64_t>{11, 12, 13}));

    const auto second = controller.updateMembership(declare(14));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->membership_epoch, 2U);
    EXPECT_EQ(second->n_hat, 4000000U);
    EXPECT_EQ(second->wire_rate_bps, UINT64_C(22500000000));
    EXPECT_EQ(controller.activeFlowCount(), 4U);
    EXPECT_EQ(controller.currentWireRateBps(), second->wire_rate_bps);
}

TEST(RnicCollectiveControllerTest, MembershipMutationIsTransactionalAndIdempotent) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(20, 500000)).has_value());
    const std::uint64_t epoch = controller.membershipEpoch();

    EXPECT_FALSE(controller.updateMembership(declare(20, 500000)).has_value());
    EXPECT_EQ(controller.membershipEpoch(), epoch);
    EXPECT_THROW(
        controller.updateMembership({{{20, 600000}}, {}}),
        std::invalid_argument);
    EXPECT_THROW(
        controller.updateMembership({{{21, 1}, {21, 1}}, {}}),
        std::invalid_argument);
    EXPECT_THROW(
        controller.updateMembership({{{21, 1}}, {21}}),
        std::invalid_argument);
    EXPECT_EQ(controller.membershipEpoch(), epoch);
    EXPECT_TRUE(controller.contains(20));
    EXPECT_FALSE(controller.contains(21));

    const auto retired =
        controller.updateMembership({{}, {20}});
    ASSERT_TRUE(retired.has_value());
    EXPECT_EQ(retired->n_hat, 0U);
    EXPECT_EQ(retired->wire_rate_bps, 0U);
    EXPECT_FALSE(controller.contains(20));
}

TEST(RnicCollectiveControllerTest, FeedbackGrantsCarryTheFrozenWindowSnapshot) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(30)).has_value());
    const RnicCollectiveRateSnapshot frozen = controller.rateSnapshot();
    EXPECT_EQ(frozen.membership_epoch, 1U);
    EXPECT_EQ(frozen.n_hat_ppm, 1000000U);
    EXPECT_EQ(frozen.wire_rate_bps, UINT64_C(90000000000));

    const RnicCollectiveGrant accept = controller.acceptFor(30, frozen, 2 * kDwnd);
    EXPECT_EQ(accept.kind, RnicCollectiveGrantKind::Accept);
    EXPECT_EQ(accept.membership_epoch, 1U);
    EXPECT_EQ(accept.n_hat, 1000000U);
    EXPECT_EQ(accept.wire_rate_bps, UINT64_C(90000000000));
    EXPECT_EQ(accept.effective_time_ps, 2 * kDwnd);
    EXPECT_EQ(accept.feedback_deadline_ps, 0U);
    EXPECT_EQ(accept.lease_expiry_ps, 0U);

    // The frozen snapshot outlives a later mutation: same-window feedback
    // keeps the boundary numbers while the live snapshot moves on.
    ASSERT_TRUE(controller.updateMembership(declare(31)).has_value());
    const RnicCollectiveGrant same_window =
        controller.feedbackFor(30, frozen, 2 * kDwnd);
    EXPECT_EQ(same_window.kind, RnicCollectiveGrantKind::Update);
    EXPECT_EQ(same_window.membership_epoch, 1U);
    EXPECT_EQ(same_window.n_hat, 1000000U);
    EXPECT_EQ(same_window.wire_rate_bps, UINT64_C(90000000000));
    const RnicCollectiveRateSnapshot live = controller.rateSnapshot();
    EXPECT_EQ(live.membership_epoch, 2U);
    EXPECT_EQ(live.n_hat_ppm, 2000000U);
    EXPECT_EQ(live.wire_rate_bps, UINT64_C(45000000000));

    EXPECT_THROW(controller.feedbackFor(99, live, 2 * kDwnd), std::out_of_range);
    EXPECT_THROW(controller.acceptFor(30, {0, 0, 0}, 2 * kDwnd),
                 std::invalid_argument);
    EXPECT_THROW(controller.feedbackFor(30, live, 0), std::invalid_argument);
}

TEST(RnicSenderGrantGateTest, DeclareOpensDataImmediatelyAtDeclaredFraction) {
    RnicSenderGrantGate full(40);
    EXPECT_EQ(full.phase(), RnicSenderGrantGate::Phase::Idle);
    EXPECT_FALSE(full.dataEligible());
    full.declarationDispatched(kMarginDeratedCapacity,
                               RnicCollectiveController::kFullFlowPpm, kDwnd);
    EXPECT_EQ(full.phase(), RnicSenderGrantGate::Phase::Active);
    EXPECT_TRUE(full.dataEligible());
    EXPECT_EQ(full.currentWireRateBps(), kMarginDeratedCapacity);

    RnicSenderGrantGate quarter(41);
    quarter.declarationDispatched(kMarginDeratedCapacity, 250000, kDwnd);
    EXPECT_TRUE(quarter.dataEligible());
    EXPECT_EQ(quarter.currentWireRateBps(), UINT64_C(22500000000));
    EXPECT_EQ(quarter.ownNflowPpm(), 250000U);

    // Book 1.4 pacing correction: the gate reports the receiver's exact
    // allocation, so a sole fractional declarer absorbs the undersubscribed
    // receiver's whole surplus; only the sender port (normalized by the
    // runtime) constrains pacing below grant.
    RnicSenderGrantGate surplus(43);
    surplus.declarationDispatched(UINT64_C(162000000000), 555556, kDwnd);
    EXPECT_EQ(surplus.currentWireRateBps(), UINT64_C(90000072000));

    RnicSenderGrantGate invalid(42);
    EXPECT_THROW(invalid.declarationDispatched(0, 1, kDwnd),
                 std::invalid_argument);
    EXPECT_THROW(invalid.declarationDispatched(kMarginDeratedCapacity, 0, kDwnd),
                 std::invalid_argument);
    EXPECT_THROW(
        invalid.declarationDispatched(
            kMarginDeratedCapacity,
            RnicCollectiveController::kFullFlowPpm + 1, kDwnd),
        std::invalid_argument);
    EXPECT_THROW(
        invalid.declarationDispatched(kMarginDeratedCapacity, 1, 0),
        std::invalid_argument);
    invalid.declarationDispatched(kMarginDeratedCapacity, 1, kDwnd);
    EXPECT_THROW(
        invalid.declarationDispatched(kMarginDeratedCapacity, 1, kDwnd),
        std::logic_error);
}

TEST(RnicCollectiveControllerTest, UpdateDeclarationRescalesActiveMembership) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(110, 500000)).has_value());
    ASSERT_TRUE(controller.updateMembership(declare(111, 500000)).has_value());
    ASSERT_EQ(controller.membershipEpoch(), 2U);

    // Magnitude change: contribution and epoch both move.
    EXPECT_TRUE(controller.updateDeclaration(110, 250000));
    EXPECT_EQ(controller.membershipEpoch(), 3U);
    EXPECT_EQ(controller.effectiveFlowPpm(), 750000U);
    EXPECT_EQ(controller.currentWireRateBps(), UINT64_C(120000000000));

    // Same-value repeat: idempotent no-op, epoch unchanged.
    EXPECT_TRUE(controller.updateDeclaration(110, 250000));
    EXPECT_EQ(controller.membershipEpoch(), 3U);
    EXPECT_EQ(controller.effectiveFlowPpm(), 750000U);

    // Unknown membership: counted-and-ignored by the caller, never a throw.
    EXPECT_FALSE(controller.updateDeclaration(999, 250000));
    EXPECT_EQ(controller.membershipEpoch(), 3U);

    EXPECT_THROW(controller.updateDeclaration(110, 0), std::invalid_argument);
    EXPECT_THROW(
        controller.updateDeclaration(
            110, RnicCollectiveController::kFullFlowPpm + 1),
        std::invalid_argument);
}

TEST(RnicSenderGrantGateTest, RaisedOwnNflowIsAdoptedAtANewerEpochBoundary) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(
                    {{{120, 250000}, {121, 750000}}, {}}).has_value());
    RnicSenderGrantGate gate = activeGate(120, 250000);
    const RnicCollectiveGrant first =
        controller.feedbackFor(120, controller.rateSnapshot(), 2 * kDwnd);
    EXPECT_EQ(gate.applyRateFeedback(first, 2 * kDwnd),
              RnicSenderFeedbackOutcome::AppliedNow);
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(22500000000));

    // The raise waits for a snapshot from a strictly newer epoch, i.e. one
    // that includes the receiver-side NFLOW_UPDATE (book 1.4).
    gate.updateOwnNflow(500000);
    EXPECT_EQ(gate.ownNflowPpm(), 250000U);
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(22500000000));

    ASSERT_TRUE(controller.updateDeclaration(120, 500000));
    const RnicCollectiveGrant updated =
        controller.feedbackFor(120, controller.rateSnapshot(), 4 * kDwnd);
    EXPECT_EQ(gate.applyRateFeedback(updated, 4 * kDwnd),
              RnicSenderFeedbackOutcome::AppliedNow);
    EXPECT_EQ(gate.ownNflowPpm(), 500000U);
    // n_hat = 1.25 flows, shared floor(9e16 / 1250000) = 7.2e10, scaled by
    // the adopted half flow.
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(36000000000));

    // A same-value update is an idempotent no-op.
    gate.updateOwnNflow(500000);
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(36000000000));
    EXPECT_THROW(gate.updateOwnNflow(0), std::invalid_argument);
}

TEST(RnicSenderGrantGateTest, OwnFractionScalesEverySharedRate) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(
        controller.updateMembership({{{50, RnicCollectiveController::kFullFlowPpm},
                                      {51, 250000}},
                                     {}})
            .has_value());
    const RnicCollectiveRateSnapshot snapshot = controller.rateSnapshot();
    EXPECT_EQ(snapshot.n_hat_ppm, 1250000U);
    EXPECT_EQ(snapshot.wire_rate_bps, UINT64_C(72000000000));

    RnicSenderGrantGate full = activeGate(50);
    RnicSenderGrantGate quarter = activeGate(51, 250000);
    // Arrival past the governed boundary applies the shared snapshot now.
    EXPECT_EQ(full.applyRateFeedback(
                  controller.feedbackFor(50, snapshot, 2 * kDwnd), 2 * kDwnd),
              RnicSenderFeedbackOutcome::AppliedNow);
    EXPECT_EQ(quarter.applyRateFeedback(
                  controller.feedbackFor(51, snapshot, 2 * kDwnd), 2 * kDwnd),
              RnicSenderFeedbackOutcome::AppliedNow);
    EXPECT_EQ(full.currentWireRateBps(), UINT64_C(72000000000));
    EXPECT_EQ(quarter.currentWireRateBps(), UINT64_C(18000000000));
    EXPECT_EQ(full.membershipEpoch(), 1U);
    EXPECT_EQ(quarter.membershipEpoch(), 1U);
}

TEST(RnicSenderGrantGateTest, FeedbackActivatesExactlyAtTheGovernedBoundary) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(60)).has_value());
    RnicSenderGrantGate gate = activeGate(60, 250000);
    const std::uint64_t startup_rate = gate.currentWireRateBps();

    // Snapshot of window k = 1 governs receiver boundary 3 * dwnd; the
    // sender-local boundary is one one-way earlier, at 2 * dwnd.
    const RnicCollectiveGrant accept =
        controller.acceptFor(60, controller.rateSnapshot(), 3 * kDwnd);
    EXPECT_EQ(gate.receiveAccept(accept, 1500),
              RnicSenderFeedbackOutcome::Scheduled);
    EXPECT_EQ(gate.currentWireRateBps(), startup_rate);
    EXPECT_EQ(gate.scheduledActivationTimePs(),
              std::optional<std::uint64_t>(2 * kDwnd));
    EXPECT_FALSE(gate.activateScheduledRate(2 * kDwnd - 1));
    EXPECT_EQ(gate.currentWireRateBps(), startup_rate);
    EXPECT_TRUE(gate.activateScheduledRate(2 * kDwnd));
    // margin * C shared by one whole flow, scaled by the own quarter.
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(22500000000));
    EXPECT_EQ(gate.membershipEpoch(), 1U);
    EXPECT_FALSE(gate.scheduledActivationTimePs().has_value());
    EXPECT_FALSE(gate.activateScheduledRate(2 * kDwnd));
}

TEST(RnicSenderGrantGateTest, SameWindowFeedbackIsIdempotent) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(70)).has_value());
    RnicSenderGrantGate gate = activeGate(70);
    const RnicCollectiveRateSnapshot snapshot = controller.rateSnapshot();

    const RnicCollectiveGrant accept =
        controller.acceptFor(70, snapshot, 2 * kDwnd);
    EXPECT_EQ(gate.receiveAccept(accept, 2 * kDwnd),
              RnicSenderFeedbackOutcome::AppliedNow);
    // Every ACK generated in one receiver window carries the identical
    // frozen snapshot; repeats change nothing.
    const RnicCollectiveGrant same_window =
        controller.feedbackFor(70, snapshot, 2 * kDwnd);
    EXPECT_EQ(gate.applyRateFeedback(same_window, 2 * kDwnd + 100),
              RnicSenderFeedbackOutcome::Ignored);
    EXPECT_EQ(gate.applyRateFeedback(same_window, 2 * kDwnd + 200),
              RnicSenderFeedbackOutcome::Ignored);
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(90000000000));

    RnicCollectiveGrant conflicting = same_window;
    conflicting.wire_rate_bps = UINT64_C(45000000000);
    EXPECT_THROW(gate.applyRateFeedback(conflicting, 2 * kDwnd + 300),
                 std::invalid_argument);
}

TEST(RnicSenderGrantGateTest, StaleWindowIsIgnoredAndLateSnapshotAppliesOnArrival) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(80)).has_value());
    RnicSenderGrantGate gate = activeGate(80);
    const RnicCollectiveRateSnapshot first = controller.rateSnapshot();

    ASSERT_TRUE(controller.updateMembership(declare(81)).has_value());
    const RnicCollectiveRateSnapshot second = controller.rateSnapshot();
    const RnicCollectiveGrant newer =
        controller.feedbackFor(80, second, 4 * kDwnd);
    // Arrival inside the governed sender window applies immediately rather
    // than pacing another window on stale state.
    EXPECT_EQ(gate.applyRateFeedback(newer, 3 * kDwnd + 500),
              RnicSenderFeedbackOutcome::AppliedNow);
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(45000000000));

    const RnicCollectiveGrant reordered_older =
        controller.feedbackFor(80, first, 3 * kDwnd);
    EXPECT_EQ(gate.applyRateFeedback(reordered_older, 3 * kDwnd + 600),
              RnicSenderFeedbackOutcome::Ignored);
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(45000000000));
    EXPECT_EQ(gate.membershipEpoch(), 2U);
}

TEST(RnicSenderGrantGateTest, RejectsMalformedAndNoncausalFeedback) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(90)).has_value());
    const RnicCollectiveGrant valid =
        controller.feedbackFor(90, controller.rateSnapshot(), 2 * kDwnd);

    RnicSenderGrantGate idle(90);
    EXPECT_THROW(idle.applyRateFeedback(valid, 2 * kDwnd), std::logic_error);

    RnicSenderGrantGate gate = activeGate(90);
    RnicCollectiveGrant wrong_flow = valid;
    wrong_flow.flow_id = 91;
    EXPECT_THROW(gate.applyRateFeedback(wrong_flow, 2 * kDwnd),
                 std::invalid_argument);
    RnicCollectiveGrant vestigial = valid;
    vestigial.lease_expiry_ps = 1;
    EXPECT_THROW(gate.applyRateFeedback(vestigial, 2 * kDwnd),
                 std::invalid_argument);
    RnicCollectiveGrant misaligned = valid;
    misaligned.effective_time_ps = 2 * kDwnd + 1;
    EXPECT_THROW(gate.applyRateFeedback(misaligned, 2 * kDwnd),
                 std::invalid_argument);
    RnicCollectiveGrant wrong_kind = valid;
    wrong_kind.kind = RnicCollectiveGrantKind::Accept;
    EXPECT_THROW(gate.receiveAccept(valid, 2 * kDwnd), std::invalid_argument);
    // A snapshot cannot govern a boundary more than two windows past its
    // physical arrival.
    RnicCollectiveGrant noncausal = valid;
    noncausal.effective_time_ps = 5 * kDwnd;
    EXPECT_THROW(gate.applyRateFeedback(noncausal, 2 * kDwnd),
                 std::runtime_error);
    EXPECT_EQ(gate.currentWireRateBps(), kMarginDeratedCapacity);
}

TEST(RnicSenderGrantGateTest, RetirementIsTerminal) {
    RnicCollectiveController controller(kCapacity, kDwnd);
    ASSERT_TRUE(controller.updateMembership(declare(100)).has_value());
    RnicSenderGrantGate gate = activeGate(100);
    const RnicCollectiveGrant pending =
        controller.feedbackFor(100, controller.rateSnapshot(), 3 * kDwnd);
    EXPECT_EQ(gate.applyRateFeedback(pending, kDwnd + 500),
              RnicSenderFeedbackOutcome::Scheduled);

    gate.receiverRetirementCommitted();
    EXPECT_EQ(gate.phase(), RnicSenderGrantGate::Phase::Retired);
    EXPECT_FALSE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), 0U);
    EXPECT_FALSE(gate.scheduledActivationTimePs().has_value());
    EXPECT_FALSE(gate.activateScheduledRate(2 * kDwnd));
    EXPECT_EQ(gate.applyRateFeedback(pending, 2 * kDwnd),
              RnicSenderFeedbackOutcome::Ignored);
    gate.receiverRetirementCommitted();
    EXPECT_EQ(gate.phase(), RnicSenderGrantGate::Phase::Retired);
}

TEST(RnicCollectiveControlTest, RejectsInvalidParametersAndOverflow) {
    EXPECT_THROW(
        RnicCollectiveController(0, kDwnd),
        std::invalid_argument);
    EXPECT_THROW(
        RnicCollectiveController(kCapacity, 0),
        std::invalid_argument);
    EXPECT_THROW(
        RnicCollectiveController(kCapacity, kDwnd, 0),
        std::invalid_argument);
    EXPECT_THROW(
        RnicCollectiveController(
            std::numeric_limits<std::uint64_t>::max(),
            kDwnd),
        std::invalid_argument);

    RnicCollectiveController controller(kCapacity, kDwnd);
    EXPECT_THROW(
        controller.updateMembership(declare(1, 0)),
        std::invalid_argument);
    EXPECT_THROW(
        controller.updateMembership(
            declare(1, RnicCollectiveController::kFullFlowPpm + 1)),
        std::invalid_argument);
    // Fill membership to just under the uint32 feedback field, then push the
    // accumulated ppm total over it with one more whole flow.
    RnicCollectiveMembershipDelta bulk;
    for (std::uint64_t id = 1; id <= 4294; ++id) {
        bulk.declarations.push_back(
            {id, RnicCollectiveController::kFullFlowPpm});
    }
    ASSERT_TRUE(controller.updateMembership(bulk).has_value());
    EXPECT_THROW(
        controller.updateMembership(
            declare(5000, RnicCollectiveController::kFullFlowPpm)),
        std::overflow_error);
}

}  // namespace

TEST(RnicCollectiveControllerTest, FractionalDeclarationsReleaseUnusedShare) {
    RnicCollectiveController controller(UINT64_C(100000000000), 10000000);

    // One full flow plus one quarter flow: n_hat = 1.25 flows in ppm, so the
    // grant is margin * C / 1.25 instead of margin * C / 2.
    RnicCollectiveMembershipDelta delta;
    delta.declarations = {{1, RnicCollectiveController::kFullFlowPpm},
                          {2, 250000}};
    const auto update = controller.updateMembership(delta);
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->n_hat, 1250000U);
    EXPECT_EQ(update->wire_rate_bps, UINT64_C(72000000000));

    // Retiring the fractional member restores the full-flow grant exactly.
    RnicCollectiveMembershipDelta retire;
    retire.retired_flow_ids = {2};
    const auto after = controller.updateMembership(retire);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->n_hat, 1000000U);
    EXPECT_EQ(after->wire_rate_bps, UINT64_C(90000000000));
}

TEST(RnicCollectiveControllerTest, RejectsDeclarationsOutsidePpmDomain) {
    RnicCollectiveController controller(UINT64_C(100000000000), 10000000);
    RnicCollectiveMembershipDelta zero;
    zero.declarations = {{1, 0}};
    EXPECT_THROW(controller.updateMembership(zero), std::invalid_argument);
    RnicCollectiveMembershipDelta beyond;
    beyond.declarations = {{1, RnicCollectiveController::kFullFlowPpm + 1}};
    EXPECT_THROW(controller.updateMembership(beyond), std::invalid_argument);
}
