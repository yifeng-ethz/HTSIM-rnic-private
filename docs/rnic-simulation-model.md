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
| `rnic-nn` | packets | null-network manifold with fixed propagation | instantaneous centralized packetized max-min | centrally feasible packet slots; PRBS may randomize ties |
| `rnic-nn-fluid` | fluid bytes | null-network manifold with fixed propagation | instantaneous centralized fluid max-min | continuous service; no packet pacer |

CN always means **collective network**. Do not use `cc` as a profile or mode
name because it is conventionally read as congestion control. Spell Tomahawk 3
out in class, file, and manifest names; `TM3` is acceptable only as explanatory
shorthand in prose.

The implementation keeps the following dimensions independent internally:

- `RnicFabricModel`: `Tomahawk3Clos` or `NullNetworkManifold`;
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

## Network-calculus contract

For cumulative arrivals `R`, an arrival curve `alpha` satisfies

```text
R(t) - R(s) <= alpha(t - s), for all s <= t.
```

A fixed-rate, fixed-latency endpoint edge offers the fluid service curve

```text
beta_C,L(t) = C [t - L]^+.
```

For packet size `M`, packetized service is

```text
beta^M_C,L(t) = M floor(C [t - L]^+ / M),
beta_C,L+M/C(t) <= beta^M_C,L(t) <= beta_C,L(t).
```

Every active allocation must satisfy both endpoint-edge constraints:

```text
sum(rate_f for f sourced at s) <= C_up(s)
sum(rate_f for f destined to d) <= C_down(d).
```

The central oracle uses progressive filling to produce a max-min fair feasible
allocation. A symmetric `N:1` incast therefore assigns `C_down/N` to every
active flow (subject to any smaller source-edge or demand limit).

## Packetized null-network manifold

The null-network manifold abstracts routing, switches, internal links,
congestion domains, and internal queues. It retains physical packets, endpoint
access-link serialization, and a configurable fixed propagation latency.

At every flow join or leave event, the central oracle recomputes the max-min
allocation at the same simulation time. A packet is admitted to a centrally
selected feasible source/destination slot and then enters a constant-delay
in-flight calendar. The manifold never builds a congestion queue, drops a
packet, applies backpressure, or adds load-dependent latency.

Independent sender PRBS streams at aggregate load exactly equal to a downlink
capacity would create a stochastic `rho = 1` destination queue. That would
contradict the null-network definition. Consequently, `rnic-nn` uses a central
collision-free packet-slot scheduler. It may use deterministic PRBS state to
choose among equally feasible matchings or flow ties, but it must never schedule
two packets that violate a source or destination access-link slot. This is the
packetized counterpart of max-min service, not a best-effort fabric.

For synchronized equal-size `N:1` flows, packetized round-robin has a useful
legacy reference ledger. With flow size `S`, full packet size `M`,
`P = ceil(S/M)`, final packet size `R = S - (P - 1)M`, and `U = S/C`, the paper's
named source-charged, non-overlapped convention gives ordered completion times

```text
T_i = L + U + (N(P - 1)M + iR)/C.
```

The physical engine pipelines source serialization, fixed-delay transit, and
destination serialization whenever causality permits, so it must not be forced
to reproduce that non-overlapped ledger. Tests retain the formula with its
convention named, then independently enforce capacity lower bounds and the
single-packet slack implied by the packetized service curve.

## Fluid null-network manifold

`rnic-nn-fluid` applies the same instantaneous max-min allocation and endpoint
constraints to continuous bytes. A rate change splits an in-progress transfer
at the change time, accounts service under the old rate, and reschedules its
completion under the new rate. Propagation remains fixed. There are no packets,
PRBS slots, acknowledgements, resequencing, loss recovery, or quantization.

This profile is the `M -> 0` idealization used to separate packetization effects
from topology and control-loop effects.

## Collective-network control

`rnic-cn` has no central oracle and does not read global simulator flow state.
Its startup sequence is:

1. a flow sends a declaration before any DATA;
2. the receiver-side controller counts the declared active flow;
3. DATA remains hard-gated until an ACCEPT grant returns in band;
4. the sender moves directly from zero to `margin * C_b / N_hat`;
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

The `rnic-cn` sender schedules one wire packet opportunity at a time across all
eligible flows on an `RnicTxPort`. For flow grant `r_i` and access-link capacity
`C_access`, the selector chooses flow `i` with probability `r_i/C_access` and an
idle outcome with the remaining probability. Selecting a flow advances its
head pointer and consumes exactly one serialized packet opportunity.

Those probabilities apply directly to equal wire quanta. For a short final
packet or mixed packet sizes, byte-deficit accounting corrects the lottery so
the long-run **byte** rate remains `r_i`; the implementation must not silently
turn a byte-rate grant into a packet-count share.

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
block an unrelated eligible packet. After release, the one physical receiver
serializer and per-flow delivery FSM account for packet quantization and
reliability.

For an aggregate arrival envelope `alpha`, release satisfies the conservative
bound

```text
R_rb(t) - R_rb(s) <= alpha(t - s + delta) + M.
```

The configured buffer must cover at least peak admitted bytes over one window
plus a packet edge:

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

1. exact invariants: max-min feasibility/fairness, no null-manifold queue or
   load-dependent latency, packet service-curve slack, declaration gate,
   resequencer window/tick rules, node-wide serialization, and deterministic
   seeded replay;
2. trend regressions: packetized null approaches fluid null as `M` decreases,
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
cannot universally match full-capacity null service: its asymptotic rate tax is
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
