# RNIC physical endpoint foundations

This directory documents the shared endpoint machinery used by the future
physical `rnic-cn` and `rnic-ss` profiles. It is incremental PR 2 of 5.

These files model what happens at one RNIC's access links: choosing a DATA
packet, serializing DATA and control on one transmit wire, accepting packets at
the receiver, restoring their timestamp order, and serializing them onto one
receive wire. They do **not** implement a complete congestion-control protocol
or a network fabric.

The end-to-end shape is:

```text
receiver grant + flow eligibility
               |
       local source arbitration
               |
     deterministic PRBS selector
               |
       one shared TX serializer  <--- control frames use this wire too
               |
       future physical fabric
               |
        Ring-CAM admission
               |
       one shared RX serializer
               |
       delivered byte counters
```

## The four building blocks

| Component | Role |
| --- | --- |
| `rnic_wire_serialization.h` | Keeps exact time for one physical wire |
| `rnic_prbs_pacer.{h,cpp}` | Deterministically chooses the next DATA opportunity |
| `rnic_ring_cam.{h,cpp}` | Classifies arrivals and releases admitted packets in timestamp order |
| `rnic_port.{h,cpp}` | Combines those pieces into one TX and one RX port per node |

The payload-versus-wire byte representation is provided by
`rnic_packet_extent.h`, which was introduced in PR 1.

## Payload bytes and wire bytes are different

Payload bytes advance the application flow. Wire bytes consume serializer and
receiver-buffer capacity. Every DATA packet header therefore consumes physical
capacity even though it is not application payload.

For example, with a 1000-byte maximum wire packet and a 40-byte DATA header,
2000 payload bytes become:

```text
packet 0: 960 payload + 40 header = 1000 wire bytes
packet 1: 960 payload + 40 header = 1000 wire bytes
packet 2:  80 payload + 40 header =  120 wire bytes
```

All rates in this layer are wire bits per second. All times are picoseconds.
Serialization time is:

```text
wire_bytes * 8 * 10^12 / wire_capacity_bps
```

## Exact wire timing without accumulated rounding

HTSIM publishes events at integer picoseconds, but a packet's exact
serialization boundary may be fractional. `RnicWireSerializationClock` keeps
that fraction internally and rounds only the published timestamp upward.

For example, one byte on a 3-Tbit/s link takes exactly 8/3 ps. Three
back-to-back bytes finish at exact times 8/3, 16/3, and 8 ps, so their published
end times are:

```text
3 ps, 6 ps, 8 ps
```

They do not become `3, 6, 9`. Rounding each packet independently would invent
one extra picosecond by the third packet.

An explicit idle rebase discards the saved fraction. A runtime should do that
only after it has established that the physical wire is genuinely idle.
Equality with the published availability time may still be back-to-back
service with a hidden fractional boundary.

## What the PRBS pacer means

The PRBS pacer is a deterministic packet-opportunity selector. It is not a
generator of random delays between packets.

Each node receives a repeatable stream derived from a global seed and the node
ID. For equal-sized packet heads, a flow's long-run selection probability is
its wire-rate share of the access link. Any unused rate becomes an idle
outcome.

On a 100-Gbit/s access link, for example:

```text
flow A grant: 40 Gbit/s  -> about 40% of DATA opportunities
flow B grant: 30 Gbit/s  -> about 30% of DATA opportunities
unused rate:  30 Gbit/s  -> about 30% idle opportunities
```

These are long-run shares, not a hard guarantee for every short time window.
The same seed, node ID, call sequence, candidate set, and packet sizes reproduce
the same choices exactly. Candidate order does not change the replay because
the pacer sorts candidates by flow ID.

Short final packets need more selection events than full-sized packets to
preserve the same wire-byte rate. The variable-size selector accounts for this
by normalizing each event hazard by its current wire length. It does not pretend
that a short tail is a padded full packet.

## Why the TX port has two clocks

`RnicTxPort` owns two exact clocks:

- the physical wire clock used by every real DATA or control frame;
- a virtual DATA-opportunity clock advanced by the PRBS sequence.

This separation allows control traffic to use a DATA opportunity that PRBS
left idle. An idle lottery result advances virtual DATA time but does not
reserve the physical wire.

For example, on an 8-Tbit/s link with a 1000-byte DATA quantum:

```text
t = 0 ps:    PRBS chooses idle
             virtual DATA time advances to 1000 ps
             the physical wire remains free

t = 100 ps:  a 64-byte control frame uses the physical wire
             it finishes at 164 ps

t = 1000 ps: the next selected DATA packet may start
```

If a long control frame extends beyond the next DATA boundary, DATA waits for
the real serializer. Control does not consume or redraw the DATA lottery.
Service is non-preemptive: the endpoint runtime gives queued control priority
by dispatching it at a packet boundary before asking for another DATA
opportunity.

## Grants, eligibility, and one shared source link

A flow participates in DATA selection only when:

- its DATA gate is open;
- its receiver wire-rate grant is nonzero; and
- it has fresh payload or one pending retransmission head.

Different receivers may issue grants that add up to more than one sender can
transmit. The TX port treats those grants as demand caps and performs local
unweighted progressive filling across the one physical source link.

For a source capacity of 100 and receiver grants of 20 and 100:

```text
flow A effective source rate = 20
flow B effective source rate = 80
```

For two grants of 80, both effective rates are 50. This local arbitration
prevents independent receiver decisions from creating more source service than
the access link can carry.

Fresh DATA and a pending retransmission use the same flow's normal PRBS
opportunity. A retransmission is not a second high-priority DATA class and does
not advance the fresh-payload byte counter.

## What ETA means at the source

The TX port stamps a packet with:

```text
ETA = source-serialization end + calibrated no-queue transit
```

The calibration begins after the source serializer and may depend on the
packet's exact wire extent. It must not include the source serialization again,
and it is not a prediction of queueing delay inside the future switch fabric.

For a 1000-byte packet on a 100-Gbit/s access link, source serialization takes
80,000 ps. With 500 ps of calibrated post-source transit:

```text
dispatch start =      0 ps
dispatch end   = 80,000 ps
ETA            = 80,500 ps
```

Finishing source dispatch is not receiver delivery. The packet still has to
cross the fabric, pass receiver admission, and finish the destination
serializer.

## Ring-CAM admission

Each packet carries an ETA from the sender and has an observed arrival time at
the receiver. With delay window `D`:

```text
arrival < ETA           -> Early
ETA <= arrival <= ETA+D -> Admitted
arrival > ETA+D         -> Late
```

Both ends of the admission window are inclusive. An admitted packet's logical
release time is `ETA + D`, rounded upward to the configured release tick.

With `ETA = 100 ps`, `D = 100 ps`, and a 10-ps release tick:

```text
arrival  90 ps -> Early
arrival 100 ps -> Admitted; logical release at 200 ps
arrival 190 ps -> Admitted; logical release at 200 ps
arrival 200 ps -> Admitted; logical release at 200 ps
arrival 201 ps -> Late
```

Ring-CAM storage is shared across all flows on the receive port and is charged
in complete wire bytes. A packet that fits the time window but not the
remaining storage is classified as `Overflow`.

At a timestamp, the Ring-CAM first releases all previously stored packets due
at that time, then considers the new arrival. This lets a release free capacity
for a same-time admission. A packet newly admitted exactly at its own release
timestamp becomes immediately eligible, but a subsequent same-time advance is
needed to return it to the runtime.

Release order is by logical release time, then ETA, then stable admission
order. Packet ID is not a sequencing key, so a missing packet number does not
block later packets. This is timestamp-window resequencing, not PSN-gap repair.

## Logical release is not delivery

A Ring-CAM release only moves a packet to the one shared destination
serializer. Delivered payload and wire counters advance at the end of that
serialization.

For example, a 128-wire-byte packet released at 100 ps on an 8-Tbit/s receive
link finishes at 228 ps:

```text
at 227 ps: delivered counters are unchanged
at 228 ps: payload and wire bytes become delivered
```

Ring-CAM occupancy and pending destination-serializer occupancy are separate.
Logical release frees Ring-CAM storage even if the destination serializer is
still backlogged.

## Assumptions and non-goals

This layer assumes monotonic simulator time, deterministic transit calibration,
one shared TX and one shared RX access link per node, and nonempty physical
frames. The endpoint runtime remains responsible for packet ownership, control
queuing, loss handling, and recovery policy.

This PR deliberately does not add:

- `rnic-cn` membership, explicit-rate feedback, leases, or join gating;
- `rnic-ss` path selection, credits, or selective backpressure;
- TM3 or Rosetta switch pipelines, queues, routing, or topology hooks;
- runtime factories, command-line drivers, or GOAL integration;
- traces, campaign scripts, plots, presentation assets, or experiments.

Those pieces belong to later incremental PRs. This PR is only the shared
physical endpoint layer they will reuse.

## Suggested review order

1. `rnic_packet_extent.h` for the existing payload/wire distinction.
2. `rnic_wire_serialization.h` and its tests near the bottom of
   `rnic_port_test.cpp`.
3. `rnic_prbs_pacer.{h,cpp}` and `rnic_prbs_pacer_test.cpp`.
4. `rnic_ring_cam.{h,cpp}` and `rnic_ring_cam_test.cpp`.
5. The TX half of `rnic_port.{h,cpp}`.
6. The RX half of `rnic_port.{h,cpp}`.
7. `rnic_port_test.cpp` for the combined boundary cases.

High-value tests for a first pass include:

- `CumulativeWireClockPreventsPerPacketCeilDrift`;
- `ControlCanUseAReservedPrbsIdleInterval`;
- `LocalMaxMinCapsOversubscribedReceiverGrants`;
- `VariableWireEventsPreserveFlowAndIdleWireRates`;
- `ReleasesBeforeClassifyingAnArrivalAtTheSameTime`;
- `NextEventMovesFromLogicalReleaseToExactCompletion`.

## Build and test

From the repository root:

```sh
cmake -S htsim/sim -B build -DENABLE_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The focused tests cover deterministic replay, rate and idle shares, exact
serialization, short tails, control/DATA interaction, source oversubscription,
retransmission heads, Ring-CAM admission boundaries, shared buffer accounting,
same-time ordering, destination completion, invalid input, and arithmetic
overflow.
