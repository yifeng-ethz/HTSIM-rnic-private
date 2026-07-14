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
6. Each source RNIC admits at most one DATA head per physical access-link
   opportunity.  A node-scoped `galois-lfsr64-block64` PRBS lottery selects
   among eligible endpoint pairs and advances by the exact selected wire
   extent.  The source checks the real serializer before admission, so a SACK
   window is a recovery ledger rather than an instantaneous burst placed in
   front of the wire.

In the two-tier Clos every spine path is minimal, so the path-length term is
zero.  Adaptive routing can avoid an intermediate hot path but cannot route
around the destination RNIC bottleneck.

## Pair-selective backpressure

`ns-rosetta` attributes every leaf and spine egress's queued and in-flight
bytes to `(source, destination, traffic class)`.  When an egress crosses
`Q_hi`, that physical egress captures only its switch-local resident pairs and
sends `BP_ENABLE(domain, epoch)` to those sources.  Each switch also has an
independent shared-buffer pressure domain.  Its high threshold is placed below
capacity by the checked aggregate reaction envelope and its enables start with
zero credit.  This guard covers the case where several individually acceptable
egresses jointly consume the switch-owned buffer.

A sender keeps a separate cumulative-credit gate for every physical domain it
has actually heard from.  DATA must have credit in all active gates and
consumes all of them; a delayed disable from one leaf or spine can therefore
never release another bottleneck.  Service completion at the exact egress
returns cumulative credits.  A pair is disabled independently when it leaves
that congestion point, including a flush of a partial credit quantum, so a
one-to-three-packet tail cannot deadlock behind a four-packet quantum.  Fair
share emerges from switch arbitration and observed service rather than an
`nflow` oracle.  No observer scans the runtime's global pair table.

Every endpoint pair also has an outstanding-packet window.  The implementation
computes an analytical controlled-Clos loop envelope, adds one maximum DATA
envelope, and rounds the required packet count up to an RTL-friendly power of
two.  The canonical 64-node configuration fits the 128-packet SACK window.
This is a topology/model calculation, not a measured RTT or a hardware claim.

## SACK and retry

Every DATA packet carries a packet sequence number.  ACK/SACK contains a
cumulative next-expected sequence plus a two-word 128-bit selective bitmap.  The sender
removes cumulatively or selectively acknowledged packets, immediately queues
reported holes for selective retransmission, and retains an RTO only for a
silent final loss.  Reordering is legal in unordered adaptive mode.  Duplicate
DATA and duplicate control epochs are idempotent.  Expiry after the configured
final retry is a terminal runtime error naming the endpoint pair and sequence;
it cannot remain scheduled forever at one simulation timestamp.  Earliest-RTO
discovery uses a min-heap keyed by `(deadline, pair, sequence, generation)`;
ACKed records and superseded retransmission generations are discarded lazily
after an exact ledger check, so ACK processing does not scan every outstanding
packet.

## Network-calculus sizing

The checked envelope is specific to the controlled two-tier Clos implemented
here.  Let `D` be the maximum one-way propagation/switch latency, `H=4` the
cross-leaf serializer count (one endpoint serializer plus three `ns-rosetta`
egresses), `F_e` the maximum directed endpoint-pair contributors to one
physical egress, and `s_D` and `s_C` the ceil-rounded serialization times of
one maximum DATA and one control envelope.  In the canonical 8-by-8 Clos,
`F_e=8*56=448`; this is deliberately larger than the 63 sources of one simple
incast.  The implementation calculates

```
tau_forward = D + H * s_D
tau_last_bp = D + H * s_D + (F_e + H - 1) * s_C
tau_bound   = tau_forward + tau_last_bp
```

`tau_forward` deliberately includes the complete DATA path.  `tau_last_bp`
allows one non-preemptive maximum-DATA blocker at each reverse serializer and
the largest control fan-in on any physical serializer.  Controls conservatively
traverse the complete reverse endpoint route even when the detecting point is
an intermediate spine.

For a switch with `I` physical ingresses, maximum DATA envelope `M`, initial
credit `G`, and `F_e` possible egress contributors, the per-egress reaction and
peak bounds are

```
B_excess = ceil((I - 1) * C * tau_bound / 8)
B_egress = B_excess + F_e * (M + G) + 2 * F_e * control_bytes
Q_egress_peak <= Q_hi + B_egress
```

For switch-wide pressure, let `P_s` be the number of directed endpoint pairs
that can physically use the switch.  The aggregate reaction headroom is

```
B_shared = max(B_egress,
               B_excess + P_s * M + 2 * P_s * control_bytes)
Q_shared_hi = min(E * Q_hi, shared_capacity - B_shared)
```

where `E` is the physical egress count.  The one-packet term per possible pair
covers phase-quantized packets already distributed across upstream paths when
the switch-wide observation occurs.  A run is invalid if the per-egress peak
exceeds the shared buffer, if the aggregate headroom cannot fit, or if the
derived shared hysteresis collapses.  With the canonical four-packet credit
quantum, 16 MiB is rejected and 32 MiB is accepted.  The manifest emits the
port/pair counts, both reaction bounds, derived shared thresholds, observed
high watermark, leaf/spine pressure events, and maximum simultaneous sender
domains.  This sizing rule is not asserted as a proof for arbitrary topologies,
link rates, priority schedulers, or proprietary Slingshot hardware.  Queue
plots use `q/C` in microseconds so thresholds remain comparable across link
rates.

The hysteresis gap is not public.  The default is selected from the physical
feedback/service time, then reported with a mandatory sweep:

- `Q_hi`: 1, 2, 4, and 8 MiB;
- `Q_lo / Q_hi`: 0.25, 0.50, and 0.75;
- shared buffer: 16, 32, and 64 MiB.  On the canonical 64-node parameters,
  16 MiB is retained in the scan manifest as `expected-invalid` rather than
  executed as though it were a valid lossless point;
- extra telemetry delay: 0, 1, 2, and 4 microseconds in addition to physical
  packet delay;
- path-selection hysteresis: 0, 0.5, 1, and 2 microseconds of reported
  path-load cost;
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

The join/exit experiment can request the common sparse ATLAHS state schema
with `-rnic_ss_state_trace_csv FILE`.  The trace is buffered and atomically
installed only after physical quiescence.  Its effective sender rate is a
local observation: access-link rate outside backpressure, zero before the
first returned CREDIT in a new epoch, then the minimum cumulative service-rate
observation across active physical domains.  This state is write-only
instrumentation; it is never consumed by routing, credit, SACK, or forwarding
logic and introduces no oracle or global state.
