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
| `rnic-cn` | packets | Tomahawk 3 two-tier Clos with VoQ | receiver-observed, in-band collective-network grants | node-wide PRBS packet pacer |
| `rnic-nn` | packets | topology-free NN manifold with fixed propagation | instantaneous centralized packetized max-min | centrally feasible packet slots; PRBS may randomize ties |
| `rnic-nn-fluid` | fluid bytes | topology-free NN manifold with fixed propagation | instantaneous centralized fluid max-min | continuous service; no packet pacer |

CN always means **collective network**. Do not use `cc` as a profile or mode
name because it is conventionally read as congestion control. Spell Tomahawk 3
out in class, file, and manifest names; `TM3` is acceptable only as explanatory
shorthand in prose.

The implementation keeps the following dimensions independent internally:

- `RnicFabricModel`: `Tomahawk3Clos` or `TopologyFreeManifold`;
- `RnicTrafficModel`: `Packetized` or `Fluid`;
- `RnicControlModel`: `InBandCollective` or `CentralOracle`;
- `RnicPacerModel`: `Prbs`, `CentralPacketSlots`, or `None`.

The profile resolver is the only place that combines them. Initially it rejects
unvalidated combinations rather than exposing a matrix of accidental modes.

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

Independent sender PRBS streams at aggregate load exactly equal to a downlink
capacity would create a stochastic `rho = 1` destination queue. That would
contradict the topology-free contract. Consequently, `rnic-nn` uses a central
collision-free packet-slot scheduler. It may use deterministic PRBS state to
choose among equally feasible matchings or flow ties, but it must never schedule
two packets that violate a source or destination access-link slot. This is the
packetized counterpart of max-min service, not a best-effort fabric.

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

1. a flow sends a declaration before any DATA;
2. the receiver-side controller counts the declared active flow;
3. DATA remains hard-gated until an ACCEPT grant returns in band;
4. the sender moves directly from zero to the wire-rate grant
   `margin * C_b / N_hat`;
5. subsequent grants use the same controller and travel in band.

The default paper parameters are `margin = 0.9`, admission window
`Delta = 4.096 us`, and release tick `delta = 16 ns`. There is no slow start,
startup probe, additive ramp, or out-of-band fanout. Flow retirement is an
explicit control event so the receiver's active count cannot leak after short
flows.

The present paper scope assumes one calibrated control-loop RTT for flows in a
two-tier datacenter Clos. Heterogeneous control RTT stability is deliberately
out of scope and must be reported as such rather than hidden behind per-flow
tuning.

## Node-wide PRBS pacer

The `rnic-cn` sender schedules one wire event at a time across all eligible
flows on an `RnicTxPort`. Let flow `i` have a wire-rate grant `r_i`, head wire
extent `l_i`, access capacity `C`, and maximum/idle wire extent `M`. Equal
`l_i = M` heads use the original `r_i/C` lottery verbatim, including flow-id
ordering, bounded draw, LFSR-word consumption, and idle sequence.

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

The generator is an independent per-node LFSR stream seeded from
`(global_seed, node_id)`. The run manifest records the algorithm name, version,
polynomial, global seed, and derived node seed. A deterministic pacer remains
available only as a named diagnostic (`-rnic_pacer deterministic`); the profile
default is `-rnic_pacer prbs`.

Adding a random fraction of a deterministic inter-packet gap is not PRBS
pacing: it changes the mean rate and is prohibited.

## Ring-CAM resequencer

The sender stamps packet eligibility `eta` at physical dispatch using calibrated
constant transit. At a receiver, a packet is admitted only while its timestamp
is in the active window and is released independently when the lower window
edge reaches it. Conceptually:

```text
q = t_arrival - eta
admit when 0 <= q < Delta
release at delta * ceil((eta + Delta) / delta)
```

Thus an arrival at exactly `eta` is valid, while one at exactly `eta + Delta`
is late. When release and admission share a simulation timestamp, releases are
processed first. Finite-width timestamps use modular age comparisons; ordinary
unsigned ordering is invalid across counter wraparound.

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

## Tomahawk 3 Clos model

Every Clos profile uses the Tomahawk 3 switch model at ToR, aggregation, and
core tiers until explicitly overridden by a future model. This is a behavioral
traffic-manager model, not a cycle-accurate claim about a particular ASIC
revision.

The switch reuses the existing Clos FIB and path selection, then enqueues by
`(physical ingress port, physical egress port)` into VoQs. Each egress arbitrates
among its non-empty ingress VoQs with deterministic round-robin at packet
boundaries and serves at the configured physical egress rate. Control packets
use an explicit priority class without bypassing serialization. Buffer limits,
drop accounting, and any pause behavior are properties of this switch model and
must not be confused with endpoint resequencing capacity.

## Reproducibility and acceptance gates

Every reported run records the resolved profile dimensions, link rates, fixed
latencies, MTU, `Delta`, `delta`, control margin/RTT, PRBS identity and seeds,
Tomahawk 3 scheduler policy, topology, workload hash, simulator commit, and
ATLAHS commit.

Tests are layered:

1. exact invariants: exact-rational ideal max-min fairness followed by the
   declared componentwise whole-bps floor, no manifold-internal queue or
   load-dependent latency, aggregate endpoint packet service curves,
   declaration gate,
   resequencer window/tick rules, node-wide serialization, and deterministic
   seeded replay;
2. trend regressions: packetized NN approaches fluid NN as `M` decreases,
   PRBS aggregate variance decreases with sender count, RB release suppresses
   incast burst peaks, CN stays near NN for the validated paper workload, and
   conventional reactive transports degrade more strongly under large incast;
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
