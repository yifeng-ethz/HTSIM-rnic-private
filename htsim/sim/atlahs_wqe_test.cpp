#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <tuple>
#include <vector>

#include "atlahs_wqe.h"

namespace {

TEST(AtlahsWqeLedgerTest, DirectedBindingTablesAreStableForTwoAndFourNodes) {
    for (const std::uint32_t node_count : {2U, 4U}) {
        for (const AtlahsTransportKind kind : {
                 AtlahsTransportKind::DcqcnQueuePair,
                 AtlahsTransportKind::RnicCnLinkPair}) {
            AtlahsWqeLedger first(node_count, kind);
            AtlahsWqeLedger second(node_count, kind);

            ASSERT_EQ(first.transportBindings().size(),
                      static_cast<std::size_t>(node_count * (node_count - 1)));
            ASSERT_EQ(first.transportBindings().size(),
                      second.transportBindings().size());

            std::set<AtlahsWqeObjectId> object_ids;
            std::set<std::tuple<std::uint32_t, std::uint32_t>> pairs;
            for (std::size_t index = 0;
                 index < first.transportBindings().size();
                 ++index) {
                const AtlahsTransportBinding& left =
                    first.transportBindings()[index];
                const AtlahsTransportBinding& right =
                    second.transportBindings()[index];
                EXPECT_EQ(left.object_id, right.object_id);
                EXPECT_EQ(left.kind, kind);
                EXPECT_EQ(left.source, right.source);
                EXPECT_EQ(left.destination, right.destination);
                EXPECT_NE(left.source, left.destination);
                EXPECT_TRUE(object_ids.insert(left.object_id).second);
                EXPECT_TRUE(pairs.emplace(left.source, left.destination).second);
                EXPECT_EQ(&first.transportBinding(left.source, left.destination),
                          &left);
            }
            EXPECT_EQ(pairs.size(),
                      static_cast<std::size_t>(node_count * (node_count - 1)));
        }
    }
}

TEST(AtlahsWqeLedgerTest, FrozenLifecycleMatrixMatchesExactQueueAccounting) {
    for (const std::size_t wqe_count : {2U, 4U}) {
        for (const AtlahsTransportKind kind : {
                 AtlahsTransportKind::DcqcnQueuePair,
                 AtlahsTransportKind::RnicCnLinkPair}) {
            AtlahsWqeLedger ledger(2, kind);
            const AtlahsWqeObjectId forward_transport =
                ledger.transportBinding(0, 1).object_id;
            const AtlahsWqeObjectId reverse_transport =
                ledger.transportBinding(1, 0).object_id;
            EXPECT_NE(forward_transport, reverse_transport);

            std::vector<AtlahsWqeId> wqe_ids;
            for (std::size_t index = 0; index < wqe_count; ++index) {
                const AtlahsWqeId wqe_id = ledger.postAndDispatch(
                    1000 + index, 0, 1, 4096, 7000 + index);
                wqe_ids.push_back(wqe_id);
                const AtlahsWqeRecord& record = ledger.wqe(wqe_id);
                EXPECT_EQ(record.transport_kind, kind);
                EXPECT_EQ(record.transport_object_id, forward_transport);
                EXPECT_EQ(record.sq_post_sequence, index + 1);
                EXPECT_EQ(record.sq_dispatch_sequence, index + 1);
                EXPECT_EQ(record.post_time_ps, record.dispatch_time_ps);
            }

            const AtlahsWqeQueueSnapshot sq = ledger.sendQueue(0);
            EXPECT_EQ(sq.post_count, wqe_count);
            EXPECT_EQ(sq.dispatch_or_consume_count, wqe_count);
            EXPECT_EQ(sq.pending_depth, 0U);
            EXPECT_EQ(sq.high_watermark, 1U);

            for (std::size_t index = 0; index < wqe_ids.size(); ++index) {
                const AtlahsWqeRecord& record =
                    ledger.complete(wqe_ids[index], 9000 + index);
                ASSERT_TRUE(record.cq_post_sequence.has_value());
                ASSERT_TRUE(record.cq_consume_sequence.has_value());
                EXPECT_EQ(*record.cq_post_sequence, index + 1);
                EXPECT_EQ(*record.cq_consume_sequence, index + 1);
                EXPECT_EQ(record.cq_post_time_ps, record.cq_consume_time_ps);
            }

            const AtlahsWqeQueueSnapshot cq = ledger.completionQueue(0);
            EXPECT_EQ(cq.post_count, wqe_count);
            EXPECT_EQ(cq.dispatch_or_consume_count, wqe_count);
            EXPECT_EQ(cq.pending_depth, 0U);
            EXPECT_EQ(cq.high_watermark, 1U);
            EXPECT_EQ(ledger.outstandingCount(), 0U);
            EXPECT_EQ(ledger.completedCount(), wqe_count);

            for (std::uint32_t node = 0; node < ledger.nodeCount(); ++node) {
                const AtlahsWqeQueueSnapshot rq = ledger.receiveQueue(node);
                EXPECT_EQ(rq.post_count, 0U);
                EXPECT_EQ(rq.dispatch_or_consume_count, 0U);
                EXPECT_EQ(rq.pending_depth, 0U);
                EXPECT_EQ(rq.high_watermark, 0U);
            }
            EXPECT_NO_THROW(ledger.validateQuiescent());
        }
    }
}

TEST(AtlahsWqeLedgerTest, SqAllowsMultipleOutstandingAndCqIsConsumedImmediately) {
    AtlahsWqeLedger ledger(4, AtlahsTransportKind::DcqcnQueuePair);
    const AtlahsWqeId first_id =
        ledger.postAndDispatch(101, 1, 2, 4096, 7000);
    const AtlahsWqeId second_id =
        ledger.postAndDispatch(102, 1, 3, 8192, 7000);

    const AtlahsWqeRecord& first = ledger.wqe(first_id);
    const AtlahsWqeRecord& second = ledger.wqe(second_id);
    EXPECT_EQ(first.sq_id, second.sq_id);
    EXPECT_EQ(first.cq_id, second.cq_id);
    EXPECT_NE(first.rq_id, second.rq_id);
    EXPECT_EQ(first.sq_post_sequence, 1U);
    EXPECT_EQ(first.sq_dispatch_sequence, 1U);
    EXPECT_EQ(second.sq_post_sequence, 2U);
    EXPECT_EQ(second.sq_dispatch_sequence, 2U);
    EXPECT_EQ(first.post_time_ps, first.dispatch_time_ps);
    EXPECT_EQ(second.post_time_ps, second.dispatch_time_ps);
    EXPECT_EQ(ledger.outstandingCount(), 2U);

    const AtlahsWqeQueueSnapshot sq = ledger.sendQueue(1);
    EXPECT_EQ(sq.post_count, 2U);
    EXPECT_EQ(sq.dispatch_or_consume_count, 2U);
    EXPECT_EQ(sq.pending_depth, 0U);
    EXPECT_EQ(sq.high_watermark, 1U);
    EXPECT_THROW(ledger.validateQuiescent(), std::logic_error);

    // Completion order need not equal SQ dispatch order. It defines the CQ
    // sequence, and each CQE is consumed at the same timestamp it is posted.
    const AtlahsWqeRecord& completed_second = ledger.complete(second_id, 9000);
    ASSERT_TRUE(completed_second.completion_time_ps.has_value());
    ASSERT_TRUE(completed_second.cq_post_time_ps.has_value());
    ASSERT_TRUE(completed_second.cq_consume_time_ps.has_value());
    ASSERT_TRUE(completed_second.cq_post_sequence.has_value());
    ASSERT_TRUE(completed_second.cq_consume_sequence.has_value());
    EXPECT_EQ(*completed_second.completion_time_ps, 9000U);
    EXPECT_EQ(*completed_second.cq_post_time_ps, 9000U);
    EXPECT_EQ(*completed_second.cq_consume_time_ps, 9000U);
    EXPECT_EQ(*completed_second.cq_post_sequence, 1U);
    EXPECT_EQ(*completed_second.cq_consume_sequence, 1U);

    const AtlahsWqeRecord& completed_first = ledger.complete(first_id, 9100);
    EXPECT_EQ(*completed_first.cq_post_sequence, 2U);
    EXPECT_EQ(*completed_first.cq_consume_sequence, 2U);
    EXPECT_EQ(ledger.outstandingCount(), 0U);
    EXPECT_EQ(ledger.completedCount(), 2U);

    const AtlahsWqeQueueSnapshot cq = ledger.completionQueue(1);
    EXPECT_EQ(cq.post_count, 2U);
    EXPECT_EQ(cq.dispatch_or_consume_count, 2U);
    EXPECT_EQ(cq.pending_depth, 0U);
    EXPECT_EQ(cq.high_watermark, 1U);

    // RQs are reserved identity placeholders in this model. The receive side
    // is accounted by WQE completion, not by an independently polled RQ.
    for (std::uint32_t node = 0; node < ledger.nodeCount(); ++node) {
        const AtlahsWqeQueueSnapshot rq = ledger.receiveQueue(node);
        EXPECT_EQ(rq.post_count, 0U);
        EXPECT_EQ(rq.dispatch_or_consume_count, 0U);
        EXPECT_EQ(rq.pending_depth, 0U);
        EXPECT_EQ(rq.high_watermark, 0U);
    }
    EXPECT_NO_THROW(ledger.validateQuiescent());
}

TEST(AtlahsWqeLedgerTest, BindingIdentityMatchesTransportKind) {
    for (const AtlahsTransportKind kind : {
             AtlahsTransportKind::DcqcnQueuePair,
             AtlahsTransportKind::RnicCnLinkPair}) {
        AtlahsWqeLedger ledger(4, kind);
        const AtlahsWqeId wqe_id =
            ledger.postAndDispatch(200 + static_cast<std::uint64_t>(kind),
                                   2, 0, 1, 11);
        const AtlahsWqeRecord& record = ledger.wqe(wqe_id);
        EXPECT_EQ(record.transport_kind, kind);
        EXPECT_EQ(record.transport_object_id,
                  ledger.transportBinding(2, 0).object_id);
        EXPECT_STREQ(atlahsTransportKindName(kind),
                     kind == AtlahsTransportKind::DcqcnQueuePair
                         ? "dcqcn-qp"
                         : "rnic-cn-link-pair");
    }

    AtlahsWqeLedger manifold(4, AtlahsTransportKind::None);
    EXPECT_TRUE(manifold.transportBindings().empty());
    const AtlahsWqeRecord& record = manifold.wqe(
        manifold.postAndDispatch(300, 0, 1, 32, 17));
    EXPECT_EQ(record.transport_kind, AtlahsTransportKind::None);
    EXPECT_EQ(record.transport_object_id, 0U);
    EXPECT_STREQ(atlahsTransportKindName(record.transport_kind), "none");
    EXPECT_THROW(manifold.transportBinding(0, 1), std::logic_error);
}

TEST(AtlahsWqeLedgerTest, RejectsInvalidLifecycleTransitions) {
    EXPECT_THROW(AtlahsWqeLedger(0, AtlahsTransportKind::None),
                 std::invalid_argument);

    AtlahsWqeLedger ledger(2, AtlahsTransportKind::DcqcnQueuePair);
    EXPECT_THROW(ledger.postAndDispatch(1, 2, 0, 1, 0),
                 std::out_of_range);
    EXPECT_THROW(ledger.postAndDispatch(1, 0, 0, 1, 0),
                 std::invalid_argument);

    const AtlahsWqeId wqe_id = ledger.postAndDispatch(1, 0, 1, 1, 10);
    EXPECT_THROW(ledger.postAndDispatch(1, 0, 1, 1, 10),
                 std::logic_error);
    EXPECT_THROW(ledger.complete(wqe_id, 9), std::logic_error);
    EXPECT_NO_THROW(ledger.complete(wqe_id, 10));
    EXPECT_THROW(ledger.complete(wqe_id, 10), std::logic_error);
    EXPECT_THROW(ledger.abort(wqe_id), std::logic_error);

    const AtlahsWqeId aborted = ledger.postAndDispatch(2, 1, 0, 1, 20);
    ledger.abort(aborted);
    EXPECT_FALSE(ledger.contains(aborted));
    EXPECT_FALSE(ledger.containsFlow(2));
    EXPECT_EQ(ledger.outstandingCount(), 0U);
}

}  // namespace
