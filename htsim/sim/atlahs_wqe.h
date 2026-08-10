// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef ATLAHS_WQE_H
#define ATLAHS_WQE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using AtlahsWqeId = std::uint64_t;
using AtlahsWqeObjectId = std::uint64_t;

// The transport object is deliberately WQE-level metadata. Packet engines
// remain free to create any number of packets without exposing them through
// this ledger.
enum class AtlahsTransportKind {
    None,
    DcqcnQueuePair,
    RnicCnLinkPair,
};

const char* atlahsTransportKindName(AtlahsTransportKind kind) noexcept;

// Stable session-local identity for a directed transport pair. Queue objects
// occupy the first 3 * node_count IDs, matching AtlahsWqeLedger. Packet and
// flow correlation tokens are intentionally excluded from this namespace.
AtlahsWqeObjectId atlahsTransportObjectId(
    std::uint32_t node_count,
    AtlahsTransportKind kind,
    std::uint32_t source,
    std::uint32_t destination);

enum class AtlahsWqeQueueKind {
    Send,
    Receive,
    Completion,
};

struct AtlahsWqeQueueSnapshot {
    AtlahsWqeObjectId object_id{0};
    AtlahsWqeQueueKind kind{AtlahsWqeQueueKind::Send};
    std::uint32_t node{0};
    std::uint64_t post_count{0};
    std::uint64_t dispatch_or_consume_count{0};
    std::size_t pending_depth{0};
    std::size_t high_watermark{0};
};

struct AtlahsTransportBinding {
    AtlahsWqeObjectId object_id{0};
    AtlahsTransportKind kind{AtlahsTransportKind::None};
    std::uint32_t source{0};
    std::uint32_t destination{0};
};

struct AtlahsWqeRecord {
    AtlahsWqeId wqe_id{0};
    std::uint64_t flow_id{0};
    std::uint32_t source{0};
    std::uint32_t destination{0};
    std::uint64_t payload_bytes{0};
    std::uint64_t post_time_ps{0};
    std::uint64_t dispatch_time_ps{0};
    AtlahsWqeObjectId sq_id{0};
    AtlahsWqeObjectId rq_id{0};
    AtlahsWqeObjectId cq_id{0};
    std::uint64_t sq_post_sequence{0};
    std::uint64_t sq_dispatch_sequence{0};
    std::optional<std::uint64_t> completion_time_ps;
    std::optional<std::uint64_t> cq_post_time_ps;
    std::optional<std::uint64_t> cq_consume_time_ps;
    std::optional<std::uint64_t> cq_post_sequence;
    std::optional<std::uint64_t> cq_consume_sequence;
    AtlahsTransportKind transport_kind{AtlahsTransportKind::None};
    AtlahsWqeObjectId transport_object_id{0};

    bool completed() const noexcept {
        return completion_time_ps.has_value();
    }
};

// One timing-neutral WQE ledger for an ATLAHS runtime session. There is one
// SQ and CQ per source node, plus one reserved RQ placeholder per destination
// node. Posting and FIFO dispatch happen at the Send timestamp. Completion
// posts to and consumes from the CQ at one timestamp, modeling an immediately
// polling virtual CPU without adding an event or delay.
class AtlahsWqeLedger {
public:
    AtlahsWqeLedger(std::uint32_t node_count,
                    AtlahsTransportKind transport_kind);

    AtlahsWqeLedger(const AtlahsWqeLedger&) = delete;
    AtlahsWqeLedger& operator=(const AtlahsWqeLedger&) = delete;

    std::uint32_t nodeCount() const noexcept { return _node_count; }
    AtlahsTransportKind transportKind() const noexcept {
        return _transport_kind;
    }

    AtlahsWqeId postAndDispatch(std::uint64_t flow_id,
                                std::uint32_t source,
                                std::uint32_t destination,
                                std::uint64_t payload_bytes,
                                std::uint64_t now_ps);
    const AtlahsWqeRecord& complete(AtlahsWqeId wqe_id,
                                   std::uint64_t now_ps);
    void abort(AtlahsWqeId wqe_id);

    bool contains(AtlahsWqeId wqe_id) const noexcept;
    bool containsFlow(std::uint64_t flow_id) const noexcept;
    const AtlahsWqeRecord& wqe(AtlahsWqeId wqe_id) const;
    const AtlahsWqeRecord& wqeForFlow(std::uint64_t flow_id) const;

    AtlahsWqeQueueSnapshot sendQueue(std::uint32_t node) const;
    AtlahsWqeQueueSnapshot receiveQueue(std::uint32_t node) const;
    AtlahsWqeQueueSnapshot completionQueue(std::uint32_t node) const;
    const std::vector<AtlahsTransportBinding>& transportBindings() const noexcept {
        return _transport_bindings;
    }
    const AtlahsTransportBinding& transportBinding(
        std::uint32_t source, std::uint32_t destination) const;

    std::size_t outstandingCount() const noexcept { return _outstanding_count; }
    std::size_t completedCount() const noexcept { return _completed_count; }
    void validateQuiescent() const;

private:
    struct QueueState {
        AtlahsWqeObjectId object_id{0};
        std::uint64_t post_count{0};
        std::uint64_t dispatch_or_consume_count{0};
        std::deque<AtlahsWqeId> pending;
        std::size_t high_watermark{0};
    };

    static std::uint64_t nextSequence(std::uint64_t value,
                                      const char* message);
    void requireNode(std::uint32_t node) const;
    AtlahsWqeQueueSnapshot queueSnapshot(const QueueState& queue,
                                         AtlahsWqeQueueKind kind,
                                         std::uint32_t node) const;

    std::uint32_t _node_count;
    AtlahsTransportKind _transport_kind;
    std::vector<QueueState> _send_queues;
    std::vector<QueueState> _receive_queues;
    std::vector<QueueState> _completion_queues;
    std::vector<AtlahsTransportBinding> _transport_bindings;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t>
        _transport_binding_index;
    std::map<AtlahsWqeId, AtlahsWqeRecord> _wqes;
    std::map<std::uint64_t, AtlahsWqeId> _wqe_by_flow;
    AtlahsWqeId _next_wqe_id{1};
    std::size_t _outstanding_count{0};
    std::size_t _completed_count{0};
};

#endif  // ATLAHS_WQE_H
