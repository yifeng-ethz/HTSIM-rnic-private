# `rnic-ss` / `ns-rosetta` behavioral contract

`rnic-ss` is an open Slingshot-like comparator.  It is not a product model and
does not claim HPE's proprietary classifier, thresholds, or rate arithmetic.
The switch model is named `ns-rosetta`; `ns-tm3` remains the switch model used
by the DCQCN and `rnic-cn` comparison fabric.

## Public mechanism boundary

The model implements only behavior described in public sources:

- Rosetta is an input-buffered, virtual-output-queued switch.  A request/grant
  exchange permits data to cross from an input tile to an output tile.
- Request-queue credits estimate output load for adaptive routing.
- A source switch considers up to four minimal or non-minimal paths, using
  congestion and path length and biasing toward minimal paths.
- Load information is carried between neighboring switches in reverse ACK
  metadata; the SC20 paper reports an average of four reverse bytes per forward
  packet for congestion/load information.
- End-to-end ACKs track outstanding packets between endpoint pairs.  Congestion
  management distinguishes contributing pairs from victims and applies stiff,
  pair-selective backpressure.
- Routing may be packet-by-packet for unordered traffic or flow-by-flow when
  ordering is required.

Sources are archived in the companion paper repository as
`slingshot-sc20-sensi-et-al.pdf` and
`hpe-slingshot-quickspecs-v13-2026.pdf`.

## Controlled first topology

The first comparison uses the same 64-node, non-oversubscribed, two-tier Clos
shape as the basic study.  Every node has one 400 Gbit/s RNIC directly attached
to a leaf.  Replacing `ns-tm3` with `ns-rosetta` isolates switch and congestion
management behavior; 400 Gbit/s is an experiment normalization, not a Rosetta
hardware-speed claim.  A native Dragonfly study follows only after this
controlled comparison.

One traffic class is used.  The normal `rnic-ss` baseline does not use ECN or
PFC.  Link errors are disabled, so normal runs should be congestion-lossless.
An overflow is a failed buffer-sizing invariant; an explicit loss stress uses
SACK/selective repeat rather than RoCE go-back-N.

## Physical information path

No sender reads a remote queue or global flow table.

1. A source leaf samples its own request depth and chooses four of the eight
   spine candidates deterministically from `(seed, pair, packet sequence)`.
2. It chooses the lowest-cost candidate from local depth plus the most recent
   physically returned remote-load estimate.  Stable hashing and hysteresis
   break ties and prevent route flapping.
3. DATA accumulates path-load metadata as it crosses `ns-rosetta` queues.
4. The receiver returns that sample in a physical ACK/SACK.  Only ACK arrival
   updates the source estimate; its age is recorded.
5. Backpressure enable/disable and packet credits are also physical, serialized
   control packets.  They receive strict non-preemptive priority but cannot
   interrupt a DATA packet already on a link.

In the two-tier Clos every spine path is minimal, so the path-length term is
zero.  Adaptive routing can avoid an intermediate hot path but cannot route
around the destination RNIC bottleneck.

## Pair-selective backpressure

`ns-rosetta` attributes destination-egress queued and in-flight bytes to
`(source, destination, traffic class)`.  When total backlog crosses `Q_hi`, it
captures the contributing pairs and physically sends `BP_ENABLE(epoch)` only
to those sources.  A sender in backpressure mode may transmit only against
returned packet credits.  The destination returns service-driven credits in
deficit-round-robin order across live contributing pairs; fair share therefore
emerges from bottleneck service rather than an `nflow` oracle.  `Q_lo < Q_hi`
disables backpressure with hysteresis after the queue drains.

Every endpoint pair also has an outstanding-packet window.  The default window
is the ceiling of one measured no-load control RTT of wire service plus one
maximum DATA envelope.  This bounds the unresponsive burst while retaining
single-flow line rate.

## SACK and retry

Every DATA packet carries a packet sequence number.  ACK/SACK contains a
cumulative next-expected sequence plus a 64-bit selective bitmap.  The sender
removes cumulatively or selectively acknowledged packets, immediately queues
reported holes for selective retransmission, and retains an RTO only for a
silent final loss.  Reordering is legal in unordered adaptive mode.  Duplicate
DATA and duplicate control epochs are idempotent.

## Network-calculus sizing

For aggregate arrival rate `R`, destination service `C`, physical reaction
delay `tau_bp`, maximum envelope `M`, and remaining outstanding windows `W_i`,
the implementation checks the conservative bound

```
Q_peak <= Q_detect + min(sum_i W_i,
                         max(R - C, 0) * tau_bp / 8 + sum_i M_i)
```

in bytes.  A run is invalid when the configured shared buffer is smaller than
this bound for its declared fan-in/window contract.  Queue plots use `q/C` in
microseconds so thresholds remain comparable across link rates.

The hysteresis gap is not public.  The default is selected from the physical
feedback/service time, then reported with a mandatory sweep:

- `Q_hi`: 1, 2, 4, and 8 MiB;
- `Q_lo / Q_hi`: 0.25, 0.50, and 0.75;
- shared buffer: 16, 32, and 64 MiB;
- candidate paths: 1, 2, 4, and 8 (four is the public-mechanism anchor);
- extra telemetry delay: 0, 1, 2, and 4 microseconds in addition to physical
  packet delay;
- returned credit quantum: 1, 4, and 16 packets;
- silent-loss RTO: 20, 50, and 100 milliseconds.  The 50 ms default is a
  conservative mlx5-style simulator fallback requested for a silently lost
  final packet; it is not attributed to Slingshot hardware and normal
  congestion-lossless runs must record zero RTOs.

The selected default must satisfy the queue bound, avoid threshold chatter,
keep a quiet single flow at line rate, and minimize p99 FCT/JCT across paired
seeds.  No sweep point is labeled as a hardware parameter.

## Required validation and hidden debug figures

- quiet single-flow latency and line rate;
- all-start fan-in from 2 through 64;
- eight-flow 5 ms join and finite-byte completion dynamics;
- intermediate-hot-path diversion with telemetry age;
- victim/non-contributor isolation;
- queue-bound and zero-overflow normal-run invariants;
- ordered versus unordered adaptive routing;
- SACK reordering, duplicate, isolated loss, and silent-last-loss cases;
- paired-seed flat FCT CDF and dependency-enforced all-reduce JCT.

Debug-only presentation slides align per-flow injection/goodput, total and
per-pair queue occupancy, backpressure epochs/credits, path shares, telemetry
age, SACK holes, retransmissions, and RTOs on one time axis.  These slides are
hidden in presentation mode but remain available for model review.
