// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "atlahs_wqe.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

AtlahsWqeObjectId checkedObjectId(std::uint64_t value) {
    if (value == 0) {
        throw std::overflow_error("ATLAHS WQE object-id space exhausted");
    }
    return value;
}

}  // namespace

const char* atlahsTransportKindName(AtlahsTransportKind kind) noexcept {
    switch (kind) {
        case AtlahsTransportKind::None:
            return "none";
        case AtlahsTransportKind::DcqcnQueuePair:
            return "dcqcn-qp";
        case AtlahsTransportKind::RnicCnLinkPair:
            return "rnic-cn-link-pair";
    }
    return "unknown";
}

AtlahsWqeLedger::AtlahsWqeLedger(
        std::uint32_t node_count,
        AtlahsTransportKind transport_kind)
    : _node_count(node_count), _transport_kind(transport_kind) {
    if (_node_count == 0) {
        throw std::invalid_argument("ATLAHS WQE ledger requires at least one node");
    }

    _send_queues.resize(_node_count);
    _receive_queues.resize(_node_count);
    _completion_queues.resize(_node_count);

    std::uint64_t next_object_id = 1;
    for (std::uint32_t node = 0; node < _node_count; ++node) {
        _send_queues[node].object_id = checkedObjectId(next_object_id++);
    }
    for (std::uint32_t node = 0; node < _node_count; ++node) {
        _receive_queues[node].object_id = checkedObjectId(next_object_id++);
    }
    for (std::uint32_t node = 0; node < _node_count; ++node) {
        _completion_queues[node].object_id = checkedObjectId(next_object_id++);
    }

    if (_transport_kind != AtlahsTransportKind::None) {
        const std::uint64_t node_count_u64 = _node_count;
        if (node_count_u64 > 1
            && node_count_u64 > std::numeric_limits<std::size_t>::max()
                                    / (node_count_u64 - 1)) {
            throw std::overflow_error(
                "ATLAHS directed transport table size overflow");
        }
        _transport_bindings.reserve(
            static_cast<std::size_t>(node_count_u64 * (node_count_u64 - 1)));
        for (std::uint32_t source = 0; source < _node_count; ++source) {
            for (std::uint32_t destination = 0;
                 destination < _node_count;
                 ++destination) {
                if (source == destination) {
                    continue;
                }
                const std::size_t index = _transport_bindings.size();
                _transport_binding_index.emplace(
                    std::make_pair(source, destination), index);
                _transport_bindings.push_back(
                    {checkedObjectId(next_object_id++),
                     _transport_kind,
                     source,
                     destination});
            }
        }
    }

    // WQEs occupy the same session-local object-id space as queues and
    // transport bindings. The first dynamic ID follows the immutable setup
    // table and is therefore deterministic for one (N, transport) session.
    _next_wqe_id = checkedObjectId(next_object_id);
}

std::uint64_t AtlahsWqeLedger::nextSequence(
        std::uint64_t value, const char* message) {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(message);
    }
    return value + 1;
}

void AtlahsWqeLedger::requireNode(std::uint32_t node) const {
    if (node >= _node_count) {
        throw std::out_of_range("ATLAHS WQE queue node is outside the configured range");
    }
}

AtlahsWqeId AtlahsWqeLedger::postAndDispatch(
        std::uint64_t flow_id,
        std::uint32_t source,
        std::uint32_t destination,
        std::uint64_t payload_bytes,
        std::uint64_t now_ps) {
    requireNode(source);
    requireNode(destination);
    if (source == destination) {
        throw std::invalid_argument("ATLAHS WQE requires distinct endpoints");
    }
    if (_wqe_by_flow.count(flow_id) != 0) {
        throw std::logic_error("duplicate ATLAHS WQE flow ID");
    }
    if (_next_wqe_id == std::numeric_limits<AtlahsWqeId>::max()) {
        throw std::overflow_error("ATLAHS WQE id space exhausted");
    }

    QueueState& sq = _send_queues[source];
    const std::uint64_t sq_post_sequence = nextSequence(
        sq.post_count, "ATLAHS SQ post sequence overflow");
    const std::uint64_t sq_dispatch_sequence = nextSequence(
        sq.dispatch_or_consume_count,
        "ATLAHS SQ dispatch sequence overflow");

    AtlahsWqeObjectId transport_object_id = 0;
    if (_transport_kind != AtlahsTransportKind::None) {
        transport_object_id =
            transportBinding(source, destination).object_id;
    }

    const AtlahsWqeId wqe_id = _next_wqe_id++;
    AtlahsWqeRecord record;
    record.wqe_id = wqe_id;
    record.flow_id = flow_id;
    record.source = source;
    record.destination = destination;
    record.payload_bytes = payload_bytes;
    record.post_time_ps = now_ps;
    record.dispatch_time_ps = now_ps;
    record.sq_id = sq.object_id;
    record.rq_id = _receive_queues[destination].object_id;
    record.cq_id = _completion_queues[source].object_id;
    record.sq_post_sequence = sq_post_sequence;
    record.sq_dispatch_sequence = sq_dispatch_sequence;
    record.transport_kind = _transport_kind;
    record.transport_object_id = transport_object_id;

    _wqes.emplace(wqe_id, record);
    _wqe_by_flow.emplace(flow_id, wqe_id);
    sq.pending.push_back(wqe_id);
    sq.high_watermark = std::max(sq.high_watermark, sq.pending.size());
    sq.post_count = sq_post_sequence;

    // Dispatch is FIFO, but completion is not a dispatch gate. This permits
    // several WQEs from one source to remain outstanding simultaneously.
    if (sq.pending.front() != wqe_id) {
        throw std::logic_error("ATLAHS SQ FIFO dispatch order violated");
    }
    sq.pending.pop_front();
    sq.dispatch_or_consume_count = sq_dispatch_sequence;
    ++_outstanding_count;
    return wqe_id;
}

const AtlahsWqeRecord& AtlahsWqeLedger::complete(
        AtlahsWqeId wqe_id, std::uint64_t now_ps) {
    auto item = _wqes.find(wqe_id);
    if (item == _wqes.end()) {
        throw std::out_of_range("unknown ATLAHS WQE id");
    }
    AtlahsWqeRecord& record = item->second;
    if (record.completed()) {
        throw std::logic_error("ATLAHS WQE completed twice");
    }
    if (now_ps < record.dispatch_time_ps) {
        throw std::logic_error("ATLAHS WQE completed before SQ dispatch");
    }
    QueueState& cq = _completion_queues[record.source];
    const std::uint64_t post_sequence = nextSequence(
        cq.post_count, "ATLAHS CQ post sequence overflow");
    const std::uint64_t consume_sequence = nextSequence(
        cq.dispatch_or_consume_count,
        "ATLAHS CQ consume sequence overflow");

    cq.pending.push_back(wqe_id);
    cq.high_watermark = std::max(cq.high_watermark, cq.pending.size());
    cq.post_count = post_sequence;
    record.completion_time_ps = now_ps;
    record.cq_post_time_ps = now_ps;
    record.cq_post_sequence = post_sequence;

    if (cq.pending.front() != wqe_id) {
        throw std::logic_error("ATLAHS CQ FIFO consume order violated");
    }
    cq.pending.pop_front();
    cq.dispatch_or_consume_count = consume_sequence;
    record.cq_consume_time_ps = now_ps;
    record.cq_consume_sequence = consume_sequence;

    if (_outstanding_count == 0) {
        throw std::logic_error("ATLAHS WQE outstanding counter underflow");
    }
    --_outstanding_count;
    ++_completed_count;
    return record;
}

void AtlahsWqeLedger::abort(AtlahsWqeId wqe_id) {
    auto item = _wqes.find(wqe_id);
    if (item == _wqes.end()) {
        throw std::out_of_range("unknown ATLAHS WQE id");
    }
    if (item->second.completed()) {
        throw std::logic_error("cannot abort a completed ATLAHS WQE");
    }
    if (_outstanding_count == 0) {
        throw std::logic_error("ATLAHS WQE outstanding counter underflow");
    }
    _wqe_by_flow.erase(item->second.flow_id);
    _wqes.erase(item);
    --_outstanding_count;
}

bool AtlahsWqeLedger::contains(AtlahsWqeId wqe_id) const noexcept {
    return _wqes.count(wqe_id) != 0;
}

bool AtlahsWqeLedger::containsFlow(std::uint64_t flow_id) const noexcept {
    return _wqe_by_flow.count(flow_id) != 0;
}

const AtlahsWqeRecord& AtlahsWqeLedger::wqe(AtlahsWqeId wqe_id) const {
    const auto item = _wqes.find(wqe_id);
    if (item == _wqes.end()) {
        throw std::out_of_range("unknown ATLAHS WQE id");
    }
    return item->second;
}

const AtlahsWqeRecord& AtlahsWqeLedger::wqeForFlow(
        std::uint64_t flow_id) const {
    const auto item = _wqe_by_flow.find(flow_id);
    if (item == _wqe_by_flow.end()) {
        throw std::out_of_range("unknown ATLAHS WQE flow ID");
    }
    return wqe(item->second);
}

AtlahsWqeQueueSnapshot AtlahsWqeLedger::queueSnapshot(
        const QueueState& queue,
        AtlahsWqeQueueKind kind,
        std::uint32_t node) const {
    return {queue.object_id,
            kind,
            node,
            queue.post_count,
            queue.dispatch_or_consume_count,
            queue.pending.size(),
            queue.high_watermark};
}

AtlahsWqeQueueSnapshot AtlahsWqeLedger::sendQueue(
        std::uint32_t node) const {
    requireNode(node);
    return queueSnapshot(_send_queues[node], AtlahsWqeQueueKind::Send, node);
}

AtlahsWqeQueueSnapshot AtlahsWqeLedger::receiveQueue(
        std::uint32_t node) const {
    requireNode(node);
    return queueSnapshot(
        _receive_queues[node], AtlahsWqeQueueKind::Receive, node);
}

AtlahsWqeQueueSnapshot AtlahsWqeLedger::completionQueue(
        std::uint32_t node) const {
    requireNode(node);
    return queueSnapshot(
        _completion_queues[node], AtlahsWqeQueueKind::Completion, node);
}

const AtlahsTransportBinding& AtlahsWqeLedger::transportBinding(
        std::uint32_t source, std::uint32_t destination) const {
    requireNode(source);
    requireNode(destination);
    if (_transport_kind == AtlahsTransportKind::None) {
        throw std::logic_error(
            "topology-free ATLAHS runtime has no transport object table");
    }
    const auto item = _transport_binding_index.find(
        std::make_pair(source, destination));
    if (item == _transport_binding_index.end()) {
        throw std::out_of_range("invalid ATLAHS directed transport pair");
    }
    return _transport_bindings.at(item->second);
}

void AtlahsWqeLedger::validateQuiescent() const {
    if (_outstanding_count != 0) {
        throw std::logic_error(
            "ATLAHS WQE ledger still has outstanding WQEs");
    }
    for (const QueueState& sq : _send_queues) {
        if (!sq.pending.empty()
            || sq.post_count != sq.dispatch_or_consume_count) {
            throw std::logic_error(
                "ATLAHS WQE ledger has a nonquiescent SQ");
        }
    }
    for (const QueueState& rq : _receive_queues) {
        if (!rq.pending.empty() || rq.post_count != 0
            || rq.dispatch_or_consume_count != 0) {
            throw std::logic_error(
                "ATLAHS WQE ledger has a nonquiescent RQ placeholder");
        }
    }
    for (const QueueState& cq : _completion_queues) {
        if (!cq.pending.empty()
            || cq.post_count != cq.dispatch_or_consume_count) {
            throw std::logic_error(
                "ATLAHS WQE ledger has a nonquiescent CQ");
        }
    }

    std::size_t completed_records = 0;
    for (const auto& item : _wqes) {
        if (!item.second.completed()) {
            throw std::logic_error(
                "ATLAHS WQE ledger retained an incomplete WQE");
        }
        ++completed_records;
    }
    if (completed_records != _completed_count) {
        throw std::logic_error(
            "ATLAHS WQE completed counter disagrees with its records");
    }
}
