// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "rnic_collective_control.h"

#include <stdexcept>

#include <gtest/gtest.h>

namespace {

TEST(RnicCollectiveControllerTest, GrantIsDirectMarginCapacityDivision) {
    RnicCollectiveController controller(400000000000ULL);
    controller.declareFlow(1);
    EXPECT_EQ(controller.grantFor(1).wire_rate_bps, 360000000000ULL);
    EXPECT_EQ(controller.grantFor(1).n_hat, 1u);

    controller.declareFlow(2);
    EXPECT_EQ(controller.grantFor(1).wire_rate_bps, 180000000000ULL);
    EXPECT_EQ(controller.grantFor(2).wire_rate_bps, 180000000000ULL);
    EXPECT_EQ(controller.grantFor(1).n_hat, 2u);
}

TEST(RnicCollectiveControllerTest, MembershipChangeProducesOneVerticalStep) {
    RnicCollectiveController controller(1000, 900000);
    controller.declareFlow(10);
    const RnicCollectiveGrant before = controller.grantFor(10);

    controller.declareFlow(11);
    const RnicCollectiveGrant after = controller.grantFor(10);
    EXPECT_EQ(before.wire_rate_bps, 900u);
    EXPECT_EQ(after.wire_rate_bps, 450u);
    EXPECT_GT(after.membership_epoch, before.membership_epoch);
}

TEST(RnicCollectiveControllerTest, DeclarationAndRetirementAreIdempotent) {
    RnicCollectiveController controller(1000);
    EXPECT_TRUE(controller.declareFlow(10));
    const uint64_t declared_epoch = controller.membershipEpoch();
    EXPECT_FALSE(controller.declareFlow(10));
    EXPECT_EQ(controller.membershipEpoch(), declared_epoch);

    EXPECT_TRUE(controller.retireFlow(10));
    const uint64_t retired_epoch = controller.membershipEpoch();
    EXPECT_FALSE(controller.retireFlow(10));
    EXPECT_EQ(controller.membershipEpoch(), retired_epoch);
}

TEST(RnicCollectiveControllerTest, RetirementImmediatelyReallocatesReceiverGrant) {
    RnicCollectiveController controller(1000);
    controller.declareFlow(10);
    controller.declareFlow(11);
    EXPECT_EQ(controller.grantFor(10).wire_rate_bps, 450u);

    controller.retireFlow(11);
    EXPECT_EQ(controller.grantFor(10).wire_rate_bps, 900u);
    EXPECT_THROW(controller.grantFor(11), std::out_of_range);
}

TEST(RnicCollectiveControllerTest, GrantsForAllAreStableByFlowId) {
    RnicCollectiveController controller(1000);
    controller.declareFlow(20);
    controller.declareFlow(10);

    const auto grants = controller.grantsForAll();
    ASSERT_EQ(grants.size(), 2u);
    EXPECT_EQ(grants[0].flow_id, 10u);
    EXPECT_EQ(grants[1].flow_id, 20u);
    EXPECT_EQ(grants[0].membership_epoch, grants[1].membership_epoch);
}

TEST(RnicSenderGrantGateTest, DataIsHardGatedUntilAccept) {
    RnicSenderGrantGate gate(10);
    EXPECT_FALSE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), 0u);

    gate.declarationDispatched();
    EXPECT_FALSE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), 0u);

    gate.accept({10, 1, 4, 225});
    EXPECT_TRUE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), 225u);
}

TEST(RnicSenderGrantGateTest, ActiveSenderStepsDirectlyAndIgnoresStaleUpdates) {
    RnicSenderGrantGate gate(10);
    gate.declarationDispatched();
    gate.accept({10, 5, 1, 900});

    EXPECT_TRUE(gate.applyGrantUpdate({10, 6, 2, 450}));
    EXPECT_EQ(gate.currentWireRateBps(), 450u);
    EXPECT_FALSE(gate.applyGrantUpdate({10, 4, 1, 900}));
    EXPECT_EQ(gate.currentWireRateBps(), 450u);
}

TEST(RnicSenderGrantGateTest, RetirementClosesDataGate) {
    RnicSenderGrantGate gate(10);
    gate.declarationDispatched();
    gate.accept({10, 1, 1, 900});
    gate.retire();

    EXPECT_FALSE(gate.dataEligible());
    EXPECT_EQ(gate.currentWireRateBps(), 0u);
    gate.retire();
}

TEST(RnicCollectiveControlTest, RejectsInvalidControlStateAndParameters) {
    EXPECT_THROW(RnicCollectiveController(1000, 0), std::invalid_argument);
    EXPECT_THROW(RnicCollectiveController(1000, 1000001), std::invalid_argument);

    RnicSenderGrantGate gate(10);
    EXPECT_THROW(gate.accept({10, 1, 1, 900}), std::logic_error);
    gate.declarationDispatched();
    EXPECT_THROW(gate.declarationDispatched(), std::logic_error);
    EXPECT_THROW(gate.accept({11, 1, 1, 900}), std::invalid_argument);
}

}  // namespace
