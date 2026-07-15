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
constexpr std::uint64_t kControlDeadline = 1000;

RnicCollectiveMembershipDelta declare(
        std::uint64_t flow_id,
        std::uint32_t nflow = 1) {
    return {{{flow_id, nflow}}, {}};
}

RnicSenderGrantGate admittedGate(
        const RnicCollectiveGrant& accept,
        std::uint64_t arrival_time_ps) {
    RnicSenderGrantGate gate(accept.flow_id);
    gate.declarationDispatched();
    EXPECT_TRUE(gate.receiveAccept(accept, arrival_time_ps));
    EXPECT_TRUE(gate.activatePendingAccept(accept.effective_time_ps));
    return gate;
}

TEST(RnicCollectiveControllerTest, DirectRateUsesDeclaredNflowAndMargin) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    const auto first = controller.updateMembership(declare(11, 3));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->membership_epoch, 1U);
    EXPECT_EQ(first->n_hat, 3U);
    EXPECT_EQ(first->wire_rate_bps, UINT64_C(30000000000));
    EXPECT_EQ(first->accepted_flow_ids, (std::vector<std::uint64_t>{11}));

    const auto second = controller.updateMembership(declare(12, 1));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->membership_epoch, 2U);
    EXPECT_EQ(second->n_hat, 4U);
    EXPECT_EQ(second->wire_rate_bps, UINT64_C(22500000000));
    EXPECT_EQ(controller.activeFlowCount(), 2U);
    EXPECT_EQ(controller.currentWireRateBps(), second->wire_rate_bps);
}

TEST(RnicCollectiveControllerTest, MembershipMutationIsTransactionalAndIdempotent) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(20, 2)).has_value());
    const std::uint64_t epoch = controller.membershipEpoch();

    EXPECT_FALSE(controller.updateMembership(declare(20, 2)).has_value());
    EXPECT_EQ(controller.membershipEpoch(), epoch);
    EXPECT_THROW(
        controller.updateMembership({{{20, 3}}, {}}),
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

TEST(RnicCollectiveControllerTest, AcceptAndMarkedAckCarryCurrentRate) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(30)).has_value());

    const RnicCollectiveGrant accept =
        controller.acceptFor(30, 10000, 2000, 20000);
    EXPECT_EQ(accept.kind, RnicCollectiveGrantKind::Accept);
    EXPECT_EQ(accept.membership_epoch, 1U);
    EXPECT_EQ(accept.n_hat, 1U);
    EXPECT_EQ(accept.wire_rate_bps, UINT64_C(90000000000));
    EXPECT_FALSE(accept.marked_data_ack);

    ASSERT_TRUE(controller.updateMembership(declare(31)).has_value());
    const RnicCollectiveGrant update =
        controller.markedFeedbackFor(30, 11000, 12000, 22000);
    EXPECT_EQ(update.kind, RnicCollectiveGrantKind::Update);
    EXPECT_EQ(update.membership_epoch, 2U);
    EXPECT_EQ(update.n_hat, 2U);
    EXPECT_EQ(update.wire_rate_bps, UINT64_C(45000000000));
    EXPECT_TRUE(update.marked_data_ack);
    const RnicCollectiveGrant refresh =
        controller.refreshFeedbackFor(30, 11500, 12500, 22500);
    EXPECT_EQ(refresh.membership_epoch, 2U);
    EXPECT_EQ(refresh.wire_rate_bps, UINT64_C(45000000000));
    EXPECT_FALSE(refresh.marked_data_ack);
}

TEST(RnicSenderGrantGateTest, JoinerWaitsForAcceptBoundary) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(40)).has_value());
    const RnicCollectiveGrant accept =
        controller.acceptFor(40, 10000, 2000, 20000);

    RnicSenderGrantGate gate(40);
    EXPECT_FALSE(gate.dataEligible());
    gate.declarationDispatched();
    EXPECT_TRUE(gate.receiveAccept(accept, 1500));
    EXPECT_EQ(gate.phase(),
              RnicSenderGrantGate::Phase::AcceptPendingEffectiveTime);
    EXPECT_EQ(gate.pendingAcceptTimePs(),
              std::optional<std::uint64_t>(10000));
    EXPECT_FALSE(gate.dataEligible());
    EXPECT_THROW(gate.activatePendingAccept(9999), std::invalid_argument);
    EXPECT_TRUE(gate.activatePendingAccept(10000));
    EXPECT_TRUE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), UINT64_C(90000000000));
}

TEST(RnicSenderGrantGateTest, IncumbentsApplyMarkedAcksIndependently) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership({{{50, 1}, {51, 1}}, {}}).has_value());
    RnicSenderGrantGate first = admittedGate(
        controller.acceptFor(50, 10000, 2000, 20000), 1500);
    RnicSenderGrantGate second = admittedGate(
        controller.acceptFor(51, 10000, 2000, 20000), 1600);

    ASSERT_TRUE(controller.updateMembership(declare(52)).has_value());
    const RnicCollectiveGrant first_update =
        controller.markedFeedbackFor(50, 11000, 12000, 22000);
    const RnicCollectiveGrant second_update =
        controller.markedFeedbackFor(51, 11500, 12500, 22500);
    EXPECT_TRUE(first.applyRateFeedback(first_update, 11900));
    EXPECT_EQ(first.membershipEpoch(), 2U);
    EXPECT_EQ(second.membershipEpoch(), 1U);
    EXPECT_TRUE(second.applyRateFeedback(second_update, 12400));
    EXPECT_EQ(second.membershipEpoch(), 2U);
}

TEST(RnicSenderGrantGateTest, ReorderedSameEpochAckCannotShortenLease) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(60)).has_value());
    RnicSenderGrantGate gate = admittedGate(
        controller.acceptFor(60, 100, 50, 500), 40);

    const RnicCollectiveGrant older =
        controller.markedFeedbackFor(60, 200, 400, 600);
    const RnicCollectiveGrant newer =
        controller.markedFeedbackFor(60, 300, 500, 700);
    EXPECT_TRUE(gate.applyRateFeedback(newer, 350));
    EXPECT_EQ(gate.leaseExpiryPs(), 700U);
    EXPECT_FALSE(gate.applyRateFeedback(older, 360));
    EXPECT_EQ(gate.leaseExpiryPs(), 700U);
    EXPECT_EQ(gate.currentWireRateBps(), newer.wire_rate_bps);
}

TEST(RnicSenderGrantGateTest, DuplicateAckIsIgnoredAndLaterAckRenewsLease) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(70)).has_value());
    RnicSenderGrantGate gate = admittedGate(
        controller.acceptFor(70, 100, 50, 500), 40);

    const RnicCollectiveGrant first =
        controller.markedFeedbackFor(70, 200, 300, 600);
    EXPECT_TRUE(gate.applyRateFeedback(first, 250));
    EXPECT_FALSE(gate.applyRateFeedback(first, 260));
    const RnicCollectiveGrant later =
        controller.markedFeedbackFor(70, 250, 350, 650);
    EXPECT_TRUE(gate.applyRateFeedback(later, 300));
    EXPECT_EQ(gate.leaseExpiryPs(), 650U);
}

TEST(RnicSenderGrantGateTest, SameTimeRenewalMakesOldExpiryRecordStale) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(80)).has_value());
    RnicSenderGrantGate gate = admittedGate(
        controller.acceptFor(80, 100, 50, 500), 40);
    const RnicCollectiveGrant first =
        controller.markedFeedbackFor(80, 200, 300, 600);
    ASSERT_TRUE(gate.applyRateFeedback(first, 250));

    const RnicCollectiveGrant renewal =
        controller.markedFeedbackFor(80, 500, 600, 900);
    ASSERT_TRUE(gate.applyRateFeedback(renewal, 600));
    EXPECT_FALSE(gate.expireLease(1, 600, 600));
    EXPECT_TRUE(gate.dataEligible());
    EXPECT_EQ(gate.leaseExpiryPs(), 900U);
}

TEST(RnicSenderGrantGateTest, RejectsMalformedAndNoncausalFeedbackIntervals) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(90)).has_value());
    const RnicCollectiveGrant valid_accept =
        controller.acceptFor(90, 1000, 500, 2000);

    RnicSenderGrantGate malformed_accept(90);
    malformed_accept.declarationDispatched();
    RnicCollectiveGrant bad = valid_accept;
    bad.lease_expiry_ps = bad.feedback_deadline_ps;
    EXPECT_THROW(
        malformed_accept.receiveAccept(bad, 400),
        std::invalid_argument);
    EXPECT_EQ(malformed_accept.phase(),
              RnicSenderGrantGate::Phase::DeclarationInFlight);

    RnicSenderGrantGate gate = admittedGate(valid_accept, 400);
    const RnicCollectiveGrant update =
        controller.markedFeedbackFor(90, 1100, 1200, 2000);
    EXPECT_THROW(
        gate.applyRateFeedback(update, 1099),
        std::runtime_error);
    EXPECT_THROW(
        gate.applyRateFeedback(update, 1201),
        std::runtime_error);
    EXPECT_EQ(gate.membershipEpoch(), 1U);
    EXPECT_EQ(gate.leaseExpiryPs(), valid_accept.lease_expiry_ps);
}

TEST(RnicSenderGrantGateTest, LeaseExpiryFailsClosedAndRetirementIsTerminal) {
    RnicCollectiveController controller(kCapacity, kControlDeadline);
    ASSERT_TRUE(controller.updateMembership(declare(100)).has_value());
    RnicSenderGrantGate gate = admittedGate(
        controller.acceptFor(100, 100, 50, 500), 40);
    EXPECT_TRUE(gate.expireLease(1, 500, 500));
    EXPECT_EQ(gate.phase(), RnicSenderGrantGate::Phase::LeaseExpired);
    EXPECT_FALSE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), 0U);

    gate.receiverRetirementCommitted();
    EXPECT_EQ(gate.phase(), RnicSenderGrantGate::Phase::Retired);
    EXPECT_FALSE(gate.applyRateFeedback(
        controller.markedFeedbackFor(100, 600, 700, 900), 650));
}

TEST(RnicCollectiveControlTest, RejectsInvalidParametersAndOverflow) {
    EXPECT_THROW(
        RnicCollectiveController(0, kControlDeadline),
        std::invalid_argument);
    EXPECT_THROW(
        RnicCollectiveController(kCapacity, 0),
        std::invalid_argument);
    EXPECT_THROW(
        RnicCollectiveController(kCapacity, kControlDeadline, 0),
        std::invalid_argument);
    EXPECT_THROW(
        RnicCollectiveController(
            std::numeric_limits<std::uint64_t>::max(),
            kControlDeadline),
        std::invalid_argument);

    RnicCollectiveController controller(kCapacity, kControlDeadline);
    EXPECT_THROW(
        controller.updateMembership(declare(1, 0)),
        std::invalid_argument);
    ASSERT_TRUE(controller.updateMembership(declare(
        1, std::numeric_limits<std::uint32_t>::max())).has_value());
    EXPECT_THROW(
        controller.updateMembership(declare(2, 1)),
        std::overflow_error);
}

}  // namespace
