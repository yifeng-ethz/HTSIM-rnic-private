// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SS_DRAGONFLY_LOAD_H
#define SS_DRAGONFLY_LOAD_H

// Load sources for the ss-dragonfly sanity harness (HTSIM-29, HTSIM-30).
//
// Two source families beyond the open-loop line-rate source that shipped
// with the fabric:
//
//   - SsDragonflyPacedSource offers load at a declared rate below line
//     rate.  Packet k enters the host injection queue at
//     start + ceil(k * wire_bytes * 8 * 10^12 / offered_bps), the ceiling
//     taken over the total elapsed product so the schedule carries no
//     cumulative rounding drift.
//   - SsDragonflyClosedLoopSource injects one chunk greedily at line-rate
//     spacing, then waits for the chunk's last delivery plus a declared
//     endpoint think time before starting the next chunk.  The think time
//     is the seam where a measured per-chunk endpoint floor plugs in; it
//     is configuration, never a fitted constant.
//
// Declared rates and think times are configuration; all arithmetic is
// integer picoseconds with 128-bit intermediates where products can
// exceed 64 bits.  The semantics are registered in
// docs/ss-dragonfly-fabric/fixture-expectations.md (load-harness
// section) and docs/ss-dragonfly-fabric/load-harness.md.
//
// Everything here is default-off from the harness's point of view: the
// legacy open-loop driver path does not construct any of these types.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "eventlist.h"
#include "network.h"
#include "ss_dragonfly_fabric.h"

enum class SsDragonflySourceMode { OpenLoop, Paced, ClosedLoop };

// "open", "paced", or "closed"; anything else throws invalid_argument.
SsDragonflySourceMode parseSsDragonflySourceMode(const std::string& text);

// One declared flow of an explicit multi-receiver cell.  Parsed from the
// harness's repeatable -flow option, a comma-separated key=value list:
//   src=<host>,dst=<host>[,start_ps=<ps>][,offered_bps=<bps>][,think_ps=<ps>]
// src and dst are required; the optional fields override the invocation's
// global defaults for this flow only.
struct SsDragonflyLoadFlowSpec {
    std::uint32_t source{0};
    std::uint32_t destination{0};
    simtime_picosec start_ps{0};
    std::optional<std::uint64_t> offered_bps;
    std::optional<std::uint64_t> think_ps;
};

SsDragonflyLoadFlowSpec parseSsDragonflyFlowSpec(const std::string& text);

// ceil(chunk_payload_bytes / payload_per_packet); both must be positive.
std::uint64_t ssDragonflyChunkPackets(std::uint64_t chunk_payload_bytes,
                                      std::uint64_t payload_per_packet);

// The paced injection offset of packet `packet_index` relative to the
// flow start: ceil(packet_index * wire_bytes * 8 * 10^12 / offered_bps),
// exact over 128-bit intermediates.  Throws when the result does not fit
// simulated time.
simtime_picosec ssDragonflyPacedOffset(std::uint64_t wire_bytes,
                                       std::uint64_t offered_bps,
                                       std::uint64_t packet_index);

struct SsDragonflyChunkCompletion {
    std::uint64_t chunk_index{0};
    simtime_picosec first_injection_ps{0};
    simtime_picosec completion_ps{0};
};

// Common machinery of the load sources: packet construction identical to
// the open-loop source (same route hash rule, same packet identity), the
// per-chunk bookkeeping, and the delivery feedback entry point.  Chunk c
// covers packet sequences [c*N, (c+1)*N) and completes when all N are
// delivered, whatever the delivery order.
class SsDragonflyLoadSource : public EventSource {
public:
    SsDragonflyLoadSource(EventList& eventlist,
                          const std::string& name,
                          SsDragonflyTopology& fabric,
                          std::uint32_t flow_id,
                          std::uint32_t source,
                          std::uint32_t destination,
                          simtime_picosec start_abs_ps,
                          simtime_picosec stop_abs_ps,
                          htsim::DragonflyRoutingControl control,
                          bool per_packet_hash,
                          std::uint64_t wire_bytes,
                          std::uint64_t header_bytes,
                          std::uint64_t chunk_packets);
    // Cancels any pending self-event so a shared event list never fires
    // into a destroyed source (the fabric's control plane does the same).
    ~SsDragonflyLoadSource() override;

    std::uint32_t flowId() const noexcept { return _flow_id; }
    std::uint32_t source() const noexcept { return _source; }
    std::uint32_t destination() const noexcept { return _destination; }
    simtime_picosec startPs() const noexcept { return _start_ps; }
    std::uint64_t injectedPackets() const noexcept { return _sequence; }
    std::uint64_t chunkPackets() const noexcept { return _chunk_packets; }
    const std::vector<SsDragonflyChunkCompletion>& completedChunks() const noexcept {
        return _completions;
    }

    // Called by the delivery dispatch with the delivered packet's
    // sequence (its packet id) at its delivery instant.
    void noteDelivered(std::uint64_t sequence, simtime_picosec at_ps);

protected:
    // Injects the packet with the current sequence, exactly the open-loop
    // construction, and advances the sequence.
    void injectOnePacket();
    bool stopReached() const { return EventList::now() >= _stop_ps; }
    virtual void onChunkCompleted(const SsDragonflyChunkCompletion& completion) = 0;

    SsDragonflyTopology& _fabric;
    const std::uint32_t _flow_id;
    const std::uint32_t _source;
    const std::uint32_t _destination;
    const simtime_picosec _start_ps;
    const simtime_picosec _stop_ps;
    const htsim::DragonflyRoutingControl _control;
    const bool _per_packet_hash;
    const std::uint64_t _wire_bytes;
    const std::uint64_t _header_bytes;
    const std::uint64_t _chunk_packets;  // 0 disables chunk accounting

private:
    PacketFlow _packet_flow;
    std::uint64_t _sequence{0};
    std::vector<simtime_picosec> _chunk_first_injection;
    std::vector<std::uint64_t> _chunk_delivered;
    std::vector<SsDragonflyChunkCompletion> _completions;
};

// HTSIM-29, rate-controlled arm.  offered_bps must be positive and no
// greater than the host link rate, so the host queue never accumulates.
class SsDragonflyPacedSource final : public SsDragonflyLoadSource {
public:
    SsDragonflyPacedSource(EventList& eventlist,
                           SsDragonflyTopology& fabric,
                           std::uint32_t flow_id,
                           std::uint32_t source,
                           std::uint32_t destination,
                           simtime_picosec start_abs_ps,
                           simtime_picosec stop_abs_ps,
                           htsim::DragonflyRoutingControl control,
                           bool per_packet_hash,
                           std::uint64_t wire_bytes,
                           std::uint64_t header_bytes,
                           std::uint64_t chunk_packets,
                           std::uint64_t offered_bps);

    void doNextEvent() override;
    std::uint64_t offeredBps() const noexcept { return _offered_bps; }

private:
    void onChunkCompleted(const SsDragonflyChunkCompletion&) override {}

    const std::uint64_t _offered_bps;
};

// HTSIM-29, closed-loop greedy arm.  A physical drop would stall the
// loop silently (no retransmission is modeled), so the harness treats
// any drop in closed-loop mode as fatal; this class only does the
// pacing.
class SsDragonflyClosedLoopSource final : public SsDragonflyLoadSource {
public:
    SsDragonflyClosedLoopSource(EventList& eventlist,
                                SsDragonflyTopology& fabric,
                                std::uint32_t flow_id,
                                std::uint32_t source,
                                std::uint32_t destination,
                                simtime_picosec start_abs_ps,
                                simtime_picosec stop_abs_ps,
                                htsim::DragonflyRoutingControl control,
                                bool per_packet_hash,
                                std::uint64_t wire_bytes,
                                std::uint64_t header_bytes,
                                std::uint64_t chunk_packets,
                                std::uint64_t think_ps);

    void doNextEvent() override;
    std::uint64_t thinkPs() const noexcept { return _think_ps; }

private:
    void onChunkCompleted(const SsDragonflyChunkCompletion& completion) override;

    const std::uint64_t _think_ps;
    const simtime_picosec _serialization_ps;
    std::uint64_t _remaining_in_chunk{0};
    bool _halted{false};
};

// Routes fabric observations to the owning sources: deliveries by the
// packet's (source, destination) pair, which explicit cells keep
// pairwise distinct, plus the first physical drop instant for the load
// manifest.
class SsDragonflyLoadDispatch {
public:
    // Rejects a second source with the same (source, destination) pair.
    void addSource(SsDragonflyLoadSource& source);

    // For the fabric delivery observer; throws when the packet matches
    // no registered flow.
    void noteDelivery(const SsDragonflyPacket& packet, simtime_picosec at_ps);
    // For the fabric drop observer.
    void noteDrop(simtime_picosec at_ps);

    std::optional<simtime_picosec> firstDropPs() const noexcept { return _first_drop_ps; }
    std::uint64_t droppedPackets() const noexcept { return _dropped_packets; }

private:
    std::vector<SsDragonflyLoadSource*> _sources;
    std::optional<simtime_picosec> _first_drop_ps;
    std::uint64_t _dropped_packets{0};
};

#endif  // SS_DRAGONFLY_LOAD_H
