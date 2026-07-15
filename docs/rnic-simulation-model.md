# RNIC simulation model

This document is the behavioral contract for the RNIC models integrated with
ATLAHS. It deliberately replaces the experimental RNIC implementation kept on
the `codex/archive/rnic-study-2026-07-08` branch. That branch is useful for
trend comparisons and provenance, but it is not a golden implementation.

## Source precedence

When two sources disagree, implement the first applicable source in this
order:

1. the design decisions recorded in the current RNIC migration discussion;
2. the time-regulated transport patent in the paper repository;
3. the SIGCOMM paper and its appendices;
4. the network-calculus model in the paper repository;
5. trends from archived simulators and presentation plots.

Archived results are regression evidence, not line-for-line specifications.
The new simulator should preserve a reported trend when the corresponding
experiment is reproducible and should explain a changed trend when a corrected
model invalidates an old result.

## Public profiles

`-rnic_profile` accepts exactly these profiles:

| Profile | Traffic | Fabric | Allocation and feedback | Sender scheduling |
| --- | --- | --- | --- | --- |
| `rnic-cn` | packets | `ns-tm3` two-tier Clos with VoQ | receiver-observed, in-band collective-network grants | node-wide PRBS packet pacer |
| `rnic-ss` | packets | `ns-rosetta` two-tier Clos with input-buffered request/grant switching | local load telemetry, endpoint-pair tracking, contributor-selective backpressure, and SACK/selective repeat | node-wide PRBS packet opportunities gated by every active physical credit domain |
| `rnic-nn` | packets | topology-free NN manifold with fixed propagation | instantaneous centralized packetized max-min | centrally feasible packet slots; PRBS may randomize ties |
| `rnic-nn-fluid` | fluid bytes | topology-free NN manifold with fixed propagation | instantaneous centralized fluid max-min | continuous service; no packet pacer |

CN always means **collective network**. Do not use `cc` as a profile or mode
name because it is conventionally read as congestion control. The canonical
simulator identifiers are `ns-tm3` for `rnic-cn` and `ns-rosetta` for
Slingshot-like `rnic-ss`; use them in manifests, experiment labels, and prose
about each simulator. Use Tomahawk 3 or Rosetta only when discussing the real
hardware family or source provenance.

The implementation keeps the following dimensions independent internally:

- `RnicFabricModel`: `NsTm3Clos`, `NsRosettaClos`, or `TopologyFreeManifold`;
- `RnicTrafficModel`: `Packetized` or `Fluid`;
- `RnicControlModel`: `InBandCollective`, `PairSelectiveBackpressure`, or
  `CentralOracle`;
- `RnicPacerModel`: `Prbs`, `CentralPacketSlots`, or `None`.

The profile resolver is the only place that combines them. Initially it rejects
unvalidated combinations rather than exposing a matrix of accidental modes.

## Standalone ATLAHS driver

The `htsim_rnic` executable is the single GOAL/ATLAHS entry point for all four
profiles. Build and invoke it directly with:

```sh
cmake -S htsim/sim -B build
cmake --build build --target htsim_rnic
build/datacenter/htsim_rnic \
  -goal workload.bin \
  -linkspeed_bps 400000000000 \
  -rnic_profile rnic-cn
```

`-goal` names the binary GOAL schedule consumed by LogGOPSim. The ATLAHS
launcher owns conversion from text `.goal` input. The driver reads the GOAL
header before constructing the RNIC session, derives rank/CPU/NIC/physical-node
layout once, and freezes that layout for the run. `-goal_rank_mapping` may be
`auto`, `gpu-rank`, or `unique-nic`; `auto` is the default. `-nodes` is an
optional assertion against the derived physical-node count, not a second source
of topology truth.

With no `-topo` file, `rnic-cn` and `rnic-ss` generate a two-tier Clos only when
the resolved node count has the form `K^2/2` for an even `K`. A supplied
topology file must have exactly the derived physical-node count. The NN
profiles construct no physical fabric. Every successful run prints a
line-oriented model manifest and finishes only after application completion
and any required physical tail have drained; the last line records
`physical_quiescence=verified`.

Use `htsim_rnic --help` for profile-specific parameters. Cross-profile flags
are rejected so a command cannot silently describe a different model from the
one it executes.

## Physical endpoint model

An endpoint is one `RnicNode`, not a collection of isolated per-flow NICs:

```text
application flows
      |
      v
RnicTxPort: shared packet memory + per-flow head pointers
      |
      v
RnicPrbsPacer or centrally assigned packet slot
      |
      v
endpoint uplink -> selected fabric -> endpoint downlink
      |
      v
RnicRxPort: shared RingCamResequencer + one physical serializer
      |
      v
per-flow delivery state
```

There is one transmit access link and one receive access link per simulated
node. All flows at a node share those resources. The receiver resequencer is L2
and shared across flows; sequence tracking and reliable delivery are per-flow
L4 state after release. This scope is required for incast occupancy, collision,
and burst-reduction measurements to be meaningful.

### Packet and rate accounting

Every packetized profile carries an explicit `RnicPacketExtent` with two
independent ledgers:

- `payload_bytes` advances the application flow and is the numerator for
  payload goodput and completion;
- `wire_bytes` consumes source/destination serialization, packet-buffer, and
  Ring-CAM capacity and is the numerator for wire utilization.

The invariant is `0 <= payload_bytes <= wire_bytes` with `wire_bytes > 0`.
Control packets may therefore have zero payload but still consume the physical
wire. Packet payload is counted as delivered only at destination serializer
completion. Source dispatch completion is not flow completion.

DATA packetization is configured by maximum wire extent `M` and a per-DATA
header `H`, with `M > 0` and `0 <= H < M`. The maximum payload quantum is
`Q = M - H`. Every full DATA packet has extent `{Q, M}`; a final payload
remainder `0 < R <= Q` has exact extent `{R, R + H}` and is never padded to
`M`. A zero-size flow emits no DATA. Construction validates the inequalities
and every header/payload addition before producing the structurally valid
extent.

Both endpoint serializers retain the fractional remainder of the exact
rational wire boundary across back-to-back packets. Reported timestamps are
ceilings of those cumulative boundaries, not independently rounded packet
durations. The remainder resets only for an observably idle busy-period
restart. An arrival equal to the prior boundary's published ceiling still
rebases when the exact fractional boundary lies earlier; otherwise the next
packet would begin before its arrival. Intermediate byte/rate products use
128-bit arithmetic; an extent is rejected when its resulting simulator
timestamp cannot be represented. The fixed-envelope packet calendar also
rejects a configured full-`M` envelope
shorter than the simulator's one-picosecond tick because distinct slot
boundaries would otherwise collapse.

All packetized CN and NN grants are **wire-rate grants**. In particular,
`margin * C_b / N_hat` divides a physical wire capacity. Treating that value as
a payload rate would overbook the link whenever headers make
`wire_bytes > payload_bytes`. The fluid profile states its byte-service
convention separately and must not silently inherit packet header accounting.

## Network-calculus contract

In this section, `R`, `alpha`, `beta`, `S`, and `M` are measured in wire bytes.
Write physical link capacity as `C_b` bits per unit time and
`C_B = C_b / 8` bytes per unit time. For cumulative arrivals `R`, an arrival
curve `alpha` satisfies

```text
R(t) - R(s) <= alpha(t - s), for all s <= t.
```

A fixed-rate, fixed-latency endpoint edge offers the fluid service curve

```text
beta_{C_B,L}(t) = C_B [t - L]^+.
```

For packet size `M`, packetized service is

```text
beta^M_{C_B,L}(t) = M floor(C_B [t - L]^+ / M),
beta_{C_B,L+M/C_B}(t) <= beta^M_{C_B,L}(t) <= beta_{C_B,L}(t).
```

Every active allocation must satisfy both endpoint-edge constraints:

```text
sum(wire_bit_rate_f for f sourced at s) <= C_{b,up}(s)
sum(wire_bit_rate_f for f destined to d) <= C_{b,down}(d).
```

The central oracle uses progressive filling to produce the exact rational
max-min fair allocation. The executable rate for each flow is then floored
componentwise to whole bits per second; any fractional leftover remains idle
rather than being assigned by flow-ID priority. A symmetric `N:1` incast has
ideal share `C_{b,down}/N` (subject to any smaller source-edge or demand limit)
and executable share `floor(C_{b,down}/N)` bps. An ideal share below `1` bps
therefore becomes a zero-rate dormant flow until a later allocation change.
This is an explicit numerical policy, not packetization or a congestion queue.
Normalized exact-rational numerators and denominators must fit the allocator's
checked unsigned 128-bit domain; an unsupported case is rejected rather than
approximated with floating point.

## Packetized topology-free manifold (`rnic-nn`)

The topology-free NN manifold abstracts routing, switches, internal links,
congestion domains, and internal queues. It retains physical packets, endpoint
access-link serialization, and a configurable fixed propagation latency.

At every flow join or leave event, the central oracle recomputes the max-min
allocation at the same simulation time. A packet is admitted to a centrally
selected feasible source/destination slot and then enters a constant-delay
in-flight calendar. The manifold never builds a congestion queue, drops a
packet, applies backpressure, or adds load-dependent latency.

Uncoordinated sender-side lottery pacing at aggregate load exactly equal to a
downlink capacity would create a statistical `rho = 1` destination queue under
the intended renewal model. That would contradict the topology-free contract.
Consequently, `rnic-nn` uses a central collision-free packet-slot scheduler. It
may use deterministic PRBS state to choose among equally feasible matchings or
flow ties, but it must never schedule two packets that violate a source or
destination access-link slot. This is the packetized counterpart of max-min
service, not a best-effort fabric.

Scheduler version 1 uses a homogeneous full-`M` envelope grid. Within a
selected envelope, an exact wire extent `l <= M` is right-aligned on the source
edge, enters the manifold at the envelope's exact rational terminal boundary,
crosses fixed delay `L`, and is serialized immediately on the destination edge.
The unused source prefix and destination suffix stay idle; they are not charged
to the payload or wire-byte ledgers. Published timestamps are ceilings of the
exact rational boundaries. A join changes the next uncommitted envelope at the
same simulation timestamp, but it cannot retroactively preempt an envelope
whose scheduling event already ran at that timestamp. When the grant table has
no positive service (including an all-dormant sub-1-bps epoch), the next busy
period rebases its rational grid at the new arrival time instead of simulating
empty envelopes; dormant credit and flow identity remain intact.

For synchronized equal-size `N:1` flows, packetized round-robin has a useful
legacy reference ledger. With flow size `S`, full packet size `M`,
`P = ceil(S/M)`, final packet size `R = S - (P - 1)M`, and `U = S/C_B`, the
paper's named source-charged, non-overlapped convention gives ordered completion
times

```text
T_i = L + U + (N(P - 1)M + iR)/C_B.
```

The physical engine pipelines source serialization, fixed-delay transit, and
destination serialization whenever causality permits, so it must not be forced
to reproduce that non-overlapped ledger. There is also one intentional version-1
difference: the paper packs final tails at `R/C_B`, whereas the fixed-envelope
scheduler advances terminal packets at one `M/C_B` envelope per selected
destination. For `K` terminal tails it can therefore leave
`sum_j (M - l_j)` scheduled bytes idle. Full-size continuously backlogged
aggregate traffic at every grant-saturated endpoint retains the endpoint-link
packet service curve (the scheduler covers that endpoint in every envelope),
but an arbitrary short-only incast does not. This is not a per-flow
one-packet-discrepancy claim: matching constraints can move an individual
flow's service lead or lag across several packet envelopes while its long-run
grant is preserved. Tests retain the packed-tail formula as a named analytical
comparison and separately lock the runtime's exact wire accounting, fixed
delay, edge feasibility, saturated-endpoint coverage, and explicit
terminal-envelope tax. A future packed-tail runtime requires an asynchronous
variable-packet crossbar calendar, not a silent reinterpretation of this
scheduler version.

## Fluid topology-free manifold (`rnic-nn-fluid`)

`rnic-nn-fluid` applies the same instantaneous max-min allocation and endpoint
constraints to continuous bytes. A rate change splits an in-progress transfer
at the change time, accounts service under the old rate, and reschedules its
completion under the new rate. Propagation remains fixed. There are no packets,
PRBS slots, acknowledgements, resequencing, loss recovery, packetization, or
packet-size quantization.

Payload service debt is exact at the allocator's whole-bps output: internally
it is represented as `payload_bits * 10^12`, and an interval subtracts
`rate_bps * elapsed_ps`. The ideal max-min rates are exact rationals before the
componentwise whole-bps floor described above. A last-bit boundary is scheduled
at the ceiling of its exact rational duration to the next representable
picosecond, after which fixed propagation is added. Thus this profile removes
packet quantization, but it does not claim an infinitely fine rate or time
axis. In particular, a positive rational share below `1` bps floors to zero.

This profile is the `M -> 0` idealization used to separate packetization effects
from topology and control-loop effects.

## Collective-network control

`rnic-cn` has no central oracle and does not read global simulator flow state.
Its startup sequence is:

1. a flow sends a declaration with `nflow = 1` before any DATA;
2. the receiver-side controller adds that `nflow` contribution to the active
   receiver count;
3. DATA remains hard-gated until an ACCEPT grant returns in band;
4. the sender moves directly from zero to the wire-rate grant
   `margin * C_b / N_hat`;
5. subsequent grants use the same controller and travel in band.

`nflow` is the only DECLARE field available to the RX CCA. The packet may also
carry `collective_id` and `expected_fan_in`, but those names are explicitly
debug-only: the endpoint discards them before constructing the receiver
membership batch. In particular, expected fan-in cannot pre-install future
senders, change `N_hat`, or affect a grant. The transport flow identity remains
necessary to route the matching ACCEPT and RETIRE, but it is not a collective
size oracle. The current hard startup declaration admits one L4 flow and
therefore requires `nflow = 1`; the patent's separate soft-window weighted
token design is not silently folded into this path.

A join wave cannot safely open the new sender before incumbent reductions take
effect: with one incumbent, the transient would otherwise be
`0.9C + 0.45C = 1.35C`. For the paper-scoped homogeneous Clos model, every
physical ACCEPT/UPDATE in one immutable membership wave therefore carries a
common `effective_time`. Senders retain the old grant (or remain gated) until
that boundary. The sender gate has no per-flow activation operation: one
receiver-scoped barrier authenticates the exact immutable wave, preflights its
complete flow set, and commits every grant synchronously. Missing feedback
therefore leaves all incumbents at the old rate and all joiners gated. At an
effective timestamp, the runtime must process all same-time feedback arrivals,
then the barrier, then DATA scheduling, so an arrival exactly at the deadline
is timely without exposing a partial wave. Every feedback packet must
physically arrive by a configured worst-case one-way control deadline that
includes receiver fanout serialization and non-preemptive switch blocking; a
miss invalidates the run. Receiver membership waves are serialized, and
declarations observed at the same modeled timestamp are microbatched before a
wave is emitted.

The receiver controller owns membership, deadline sequencing, and the retained
wave as one non-copyable object. `beginMembershipWave` is its only membership
mutation path, and a new wave is rejected until the barrier clears the current
one. A source-drained sender remains able to consume intervening updates; its
grant gate reaches `Retired` only after the receiver transition excluding that
flow has succeeded. This keeps a RETIRE that races another membership event
from silently desynchronizing receiver membership and sender gates.

This effective-time field is an explicit homogeneous-RTT implementation
idealization, not a general delay-independent protocol claim, and its deadline
tail may extend the nominal one-RTT start. Without a proven deadline, a safe
implementation requires an in-band incumbent-ACK barrier and another control
round trip; that alternative is outside the current profile.

The default paper parameters are `margin = 0.9`, admission window
`Delta = 4.096 us`, and release tick `delta = 16 ns`. There is no slow start,
startup probe, additive ramp, or out-of-band fanout. Flow retirement is an
explicit control event so the receiver's active count cannot leak after short
flows.

The present paper scope assumes one calibrated, bounded control-loop deadline
for flows in a two-tier datacenter Clos and synchronized activation time.
Heterogeneous control RTT stability is deliberately out of scope and must be
reported as such rather than hidden behind per-flow tuning.
The physical profile therefore rejects preconfigured link failures and freezes
its ETA calibration inputs when it constructs the homogeneous Clos. A later
mutation of the legacy topology-config object cannot rewrite timestamps for
already-constructed pipes and serializers.

## Node-wide PRBS pacer

The `rnic-cn` sender schedules one wire event at a time across all eligible
flows on an `RnicTxPort`. Let flow `i` have a wire-rate grant `r_i`, head wire
extent `l_i`, access capacity `C`, and maximum/idle wire extent `M`. Equal
`l_i = M` heads use the selected generator version's `r_i/C` lottery path
verbatim, including flow-id ordering, one bounded draw per opportunity, and
the resulting idle sequence.

For unequal heads, selection uses size-normalized hazards

```text
b_i = r_i M / l_i,       b_idle = C - sum_i r_i,
B = b_idle + sum_i b_i,  p_i = b_i/B.
```

The selected DATA extent starts immediately at the later of the virtual PRBS
opportunity boundary and the physical serializer boundary; no positive random
gap is added. An `M`-byte idle outcome advances only the virtual opportunity
clock: it leaves the physical wire free, so a high-priority CN control frame
that arrives during that interval can serialize there. Control consumes the
same physical node serializer as DATA but does not consume a DATA lottery draw.
If a control frame extends past the next virtual DATA boundary, that DATA
opportunity waits for the control frame to finish.

In a control-free interval this preserves exact wire rates before
finite-precision quantization. Indeed, `sum_j b_j l_j = C M`, where the virtual
idle event has `l_idle = M`, so

```text
E[event duration] = (8/C) sum_j p_j l_j = 8M/B,
E[flow-i wire bits/event] = 8 p_i l_i = 8 r_i M/B.
```

Their ratio is exactly `r_i`. The implementation represents each unequal-head
hazard as `a_i = floor(2^q b_i)` and the idle hazard exactly as
`2^q b_idle`, choosing the largest `q <= 32` whose weights and sum fit unsigned
128-bit arithmetic. Thus each represented flow hazard has absolute error less
than `2^-q`; normal configurations use Q32, while `q = 0` is only the explicit
extreme-`uint64` fallback. A 64-step binary rational comparison maps one LFSR
word against the exact cumulative/total 128-bit weights without a narrowing
ticket range or 192-bit product; its finite-grid probability error is at most
`2^-64`.

Each node receives a distinct deterministic phase of the LFSR stream derived
from `(global_seed, node_id)`; these reproducible pseudorandom phases are
intended to approximate independent renewal streams. Version 2 is named
`galois-lfsr64-block64`. For each lottery word, bit `j` is the register LSB
before Galois transition `j`, for `j = 0..63`; consecutive calls therefore
consume disjoint 64-bit output blocks. The bounded draw is
`nonzero-rejection-modulo-v1`. Version 1 advanced the LFSR by only one bit and
exposed the whole shifted register, so adjacent draws were separated by one
transition and strongly linearly related. That implementation is retired: it
created excess short-range correlation and incast burstiness rather than the
renewal-like approximation required by the patent. The rendered run manifest
records the algorithm and extraction versions, polynomial, bounded-draw rule,
and global seed. Each in-memory per-node pacer manifest additionally records
the node ID and derived node seed.

PRBS is used as a statistical pacing approximation, not as a deterministic
arrival regulator. The aggregate grant ceiling bounds sustainable rate `rho`;
it does not configure or enforce a finite burst term `sigma`. The intended
independent-renewal model therefore has an unbounded queue-delay support. The
implemented block64 trace is deterministic and finite-period, but the
simulator neither computes nor enforces a useful per-period `(sigma,rho)` or
`Delta` guarantee. A sufficiently long replay can consequently exceed
`Delta = 4.096 us` even with correct `N_hat` and aggregate grant `0.9C`. A hard
all-time delay bound would require a separately named bounded-discrepancy or
token-bucket regulator. The current profile does not claim that PRBS alone
proves such a bound.

Adding a random fraction of a deterministic inter-packet gap is not PRBS
pacing: it changes the mean rate and is prohibited.

### Deterministic retransmission uses granted PRBS service

The Ring-CAM admits a packet whose observed age is exactly `Delta`; only an age
strictly greater than `Delta` is late. An observed late packet is conclusive,
and the receiver reports that exact logical packet with an in-band gap NACK. A
real fabric drop has no receiver-visible lifecycle or rejected ETA. A middle
drop is therefore exposed when a later packet crosses the post-resequencing
interface. RETIRE carries the maximum original release deadline over the whole
original PSN sequence, so a dropped tail is exposed at that published deadline
without waiting through another `Delta`. At either a successor release or the
RETIRE audit, all receiver admissions and releases due at the timestamp are
processed before the gap decision. Thus the decision uses `epsilon = 0`: it
adds neither a second `Delta` nor a residual hardware tick. The NACK carries
only the exact logical range and requested transmission attempt.

At the sender, a queued retransmission becomes the affected flow's head in the
same node-wide PRBS lottery used by fresh DATA.  There is one candidate per
flow at its current effective wire-rate grant.  A selected retransmission
therefore consumes one ordinary PRBS opportunity and substitutes for fresh
granted service; it is never a second, unpaced DATA class.  Other flows remain
eligible in the lottery while that retransmission is pending.  Control frames,
including the gap NACK, retain strict non-preemptive priority on the shared
physical serializer and do not consume a DATA draw.

Each retransmission is stamped with a new `eta` at its actual source-serializer
completion plus packet-specific calibrated transit.  It does not advance the
original source payload, packet-index, or final-flow ledger.  The receiver
grant and TX flow state remain live after the final original packet until the
exact RX ledger closes and retirement commits, so a final-tail retransmission
still has granted service.  Duplicate NACKs and DATA are idempotent; the
retransmission limit is a terminal diagnostic guard, not extra rate or
permission to change the fixed `Delta = 4.096 us` and `margin = 0.9` operating
point.

When a NACKed range later crosses the post-resequencing boundary, the receiver
sends a physical `GAP_RESOLVED` control carrying that range and the highest
successful attempt. Sender retry state is closed only when this routed control
arrives; receiver state never directly erases a sender queue or timer. A stale
NACK that arrives after the closure is absorbed by a retained sender tombstone.

Each physically serialized retry also arms a bounded sender watchdog, by
default `50,000,000,000 ps`, starting at the retry's serializer end boundary.
If neither a later physical NACK nor `GAP_RESOLVED` arrives before expiry, the
watchdog authorizes the next deterministic attempt, subject to the configured
attempt limit. This is a fallback for dropped DATA or absent receiver closure,
not normal-path loss inference. The `ns-tm3` fabric can physically drop
high-priority controls, but this runtime treats every such drop—including
DECLARE, ACCEPT/UPDATE, GAP_NACK, GAP_RESOLVED, and RETIRE—as fatal. The DATA
watchdog therefore does not claim control reliability.

Consequently, fresh plus retransmitted DATA remains inside the same sustainable
aggregate grant ceiling `sum r_i <= margin * C`; recovery cannot add a second
line-rate load on top of that ceiling.  As above, PRBS supplies no hard finite
`sigma`, so this is a stable rate-accounting invariant rather than a proof that
every finite replay has zero late packets.

## Ring-CAM resequencer

The sender stamps packet eligibility `eta` at the physical route-injection
boundary: source serializer completion plus packet-specific no-load physical
transit. That transit includes the route's pipe and switch-pipeline latency and
the remaining `ns-tm3` egress serialization at the packet's exact wire
extent. A same-leaf path has one leaf-to-RNIC serialization; a cross-leaf
two-tier path has two leaf-spine serializations at the inter-switch link
rate plus one leaf-to-RNIC serialization. The physical HTSIM route starts at
the source-serializer boundary, so adding transit at source dispatch start
would incorrectly omit source serialization. Calibrating every packet as a
maximum-size packet would instead make short tails appear early.

Fresh per-flow eligibility ticks are nondecreasing by PSN. If an exact short
tail would regress below its predecessor's quantized tick, the sender consumes
virtual PRBS opportunities while the physical wire remains idle until the tail
can preserve that ordering. Its ETA is still actual serializer completion plus
its exact packet-specific calibrated transit; it is never clamped or restamped
to a synthetic future time. Transit calibration is cached exactly once for
each fresh head, including the short tail.

At a receiver, a packet is admitted only while its timestamp is in the active
window and is released independently when the lower window edge reaches it.
Conceptually:

```text
q = t_arrival - eta
admit when 0 <= q <= Delta
release at delta * ceil((eta + Delta) / delta)
```

Thus an arrival at exactly `eta` is valid, while one at exactly `eta + Delta`
is also in-window; only `q > Delta` is late. When release and admission share a
simulation timestamp, already-admitted releases are processed first, then the
new equality admission is released in the same-timestamp fixed point. All due
releases precede any successor or RETIRE gap decision, so the recovery
decision has `epsilon = 0`. Finite-width timestamps use modular age
comparisons; ordinary unsigned ordering is invalid across counter wraparound.

The implementation uses one node-wide timestamp-ordered store. It does not sort
or barrier by flow or timestamp bucket, and a missing or late packet cannot
block an unrelated eligible packet. Ring-CAM occupancy is charged in stored
wire bytes while preserving each packet's payload extent. After release, the
one physical receiver serializer consumes wire bytes; per-flow delivery counts
payload and wire bytes independently at serializer completion.

Release removes bytes from the finite Ring-CAM immediately. It may create a
post-release serializer backlog, tracked separately as current and high-water
pending wire bytes. `RnicRxScheduledSerialization` reports that future physical
service interval; it is not a delivery notification. No finite bound for this
post-release backlog is claimed by the Ring-CAM capacity formula below.

For an aggregate arrival envelope `alpha`, release satisfies the conservative
bound

```text
R_rb(t) - R_rb(s) <= alpha(t - s + delta) + M.
```

Let `R_in_peak` be the peak admitted wire-byte rate (bytes per unit time). The
configured buffer must cover at least those bytes over one window plus a packet
edge:

```text
W_RB >= R_in_peak * Delta + M.
```

Drops are classified as early-window, late-window, or capacity violations;
tests never merge these causes.

## `ns-tm3` Clos model

`rnic-cn` uses the `ns-tm3` switch model at leaf and spine tiers. This is a
behavioral traffic-manager contract motivated by the Tomahawk 3 hardware
family, not a cycle-accurate claim about a particular ASIC revision.
`rnic-ss` instead uses the separate `ns-rosetta` contract documented in
`rnic-slingshot-like-model.md`; topology-free profiles instantiate neither
switch model.

The switch reuses the existing Clos FIB and path selection, then enqueues by
`(physical ingress port, physical egress port)` into VoQs. Within each strict
priority class, an egress selects the oldest head packet across non-empty
ingress VoQs (with ingress ID as the deterministic same-time tie break) and
serves it at the configured physical egress rate. This preserves the aggregate
output-FIFO service model used by the CN network-calculus argument without
reintroducing ingress head-of-line blocking. Whole-packet ingress round-robin is
retained only as an explicit sensitivity because it gives each physical ingress
an equal turn regardless of offered rate; under local plus cross-leaf fan-in it
can make one packet's residence far exceed instantaneous aggregate `q/C`.
Control packets use an explicit priority class without bypassing serialization.
Buffer limits, drop accounting, and any pause behavior are properties of this
switch model and must not be confused with endpoint resequencing capacity.

### Receive-goodput observability

The physical profiles expose an optional, read-only delivered-payload trace.
It observes the existing receiver ledger and never supplies state to routing,
pacing, CCA, recovery, or switch arbitration. A logical payload extent is
counted once: on DCQCN's first in-order acceptance, on `rnic-ss`'s first SACK
scoreboard acceptance, or on `rnic-cn`'s post-Ring-CAM in-order delivery.
Retries rejected by the receiver and duplicate copies add no bytes.

Bins are aligned to the simulation epoch, not to individual flow starts, and
empty per-flow bins are omitted. The CSV records both exact payload bytes and
the derived integer rate `floor(bytes * 8 * 10^12 / bin_width_ps)`. The path
and positive bin width must be configured together; otherwise the observer is
disabled. A requested trace remains memory-resident until physical quiescence
and is then installed by an atomic rename.

## Reproducibility and acceptance gates

Every reported run records the resolved profile dimensions, link rates, fixed
latencies, MTU, `Delta`, `delta`, control margin/RTT, PRBS identity and global seed,
`ns-tm3` scheduler policy, topology, workload hash, simulator commit, and
ATLAHS commit.

Tests are layered:

1. exact invariants: exact-rational ideal max-min fairness followed by the
   declared componentwise whole-bps floor, no manifold-internal queue or
   load-dependent latency, aggregate endpoint packet service curves,
   declaration gate,
   resequencer window/tick rules, node-wide serialization, and deterministic
   seeded replay;
2. trend regressions: packetized NN approaches fluid NN as `M` decreases,
   node-distinct PRBS phases suppress synchronized impulses relative to aligned
   fixed phases, RB release suppresses incast burst peaks, CN stays
   near NN for the validated paper workload, and conventional reactive
   transports degrade more strongly under large incast;
3. historical comparisons: slide and paper values with tolerances and written
   provenance. A mismatch here is an investigation, not permission to violate
   an exact invariant.

The slide-4 standalone simulator is retained only as a historical trend test:
it used deterministic slot staggering and did not model the shared physical
RNIC described above.

Likewise, a receiver-bound CN flow granted with the default `margin = 0.9`
cannot universally match full-capacity NN service: its asymptotic rate tax is
approximately `1 / 0.9`. A historical mean ratio closer to one may still arise
for a latency- or compute-dominated workload, but it is never allowed to
override the explicit grant equation.

## Explicit non-goals for this migration

- heterogeneous-RTT stability for the collective-network control loop;
- cycle-accurate Tomahawk 3 microarchitecture or undocumented ASIC behavior;
- preserving old startup probes, fallback grants, per-flow receiver buffers, or
  random-gap pacing;
- treating an archived plot's final digits as a golden result;
- exposing arbitrary combinations of profile dimensions before each has a
  physical interpretation and invariant tests.
