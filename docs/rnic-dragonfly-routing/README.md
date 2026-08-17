# The progressive Dragonfly routing core

Provenance: this document and the core it describes were authored on
origin/codex/rnic-dragonfly-routing (commit 2efd81b, the original PR 5
of 5) and were ported to main by the ss-dragonfly fabric wave. The
"Current integration boundary" section below is updated to the ported
state; the physical fabric that now consumes this core is documented in
docs/ss-dragonfly-fabric/README.md and is hosted with calibration
pending.

## Scope

This core is the tested Dragonfly routing layer for the `rnic-ss`
study. It does not change the existing Clos experiment and it does not
reuse the legacy `UGAL_L` implementation.

The target is the public progressive-routing mechanism described by
[US 9,137,143 B2](https://patents.google.com/patent/US9137143B2/en). It is not a
cycle-accurate Aries model and it does not claim to reproduce proprietary HPE
Slingshot thresholds, tables, or router microarchitecture.

The first checked topology is the repository's balanced `p2a4h2` Dragonfly:

- 2 hosts per router;
- 4 routers per group;
- 2 global links per router;
- 9 groups, 36 routers, and 72 physical endpoints; and
- one bidirectional global connection for every pair of groups.

## Why the Clos result remains

Changing from Clos to Dragonfly changes the physical graph, path lengths,
global-link capacity, propagation delay, switch count, and adaptive-routing
opportunities. A direct `DCQCN/Clos` versus `rnic-ss/Dragonfly` plot therefore
cannot isolate congestion control.

The paper study keeps both views:

| View | DCQCN | `rnic-ss` | What it measures |
|---|---|---|---|
| Controlled Clos | Existing ordered Clos | Existing ordered Clos | Whole-stack comparison on one graph |
| Dragonfly common route | Deterministic minimal | The same deterministic minimal route map | Congestion management on one Dragonfly |
| Dragonfly routing ablation | Deterministic minimal | Minimal versus progressive adaptive | Incremental routing benefit |
| Dragonfly native | Best valid deterministic ordered mode | Deterministic ordered; unordered progressive sensitivity | Labeled native-system behavior |

DCQCN is not given packet-by-packet spraying because RoCE depends on ordered
delivery. An adaptive DCQCN sensitivity must be flow-sticky or use an
explicitly safe flowlet boundary.

## Progressive route decision

Adaptive arbitration occurs at each input queue while the packet is still
routing up. The candidate families depend on the packet's current group and
whether a local dimension has already been traversed:

| Current context | Two minimal slots | Two non-minimal slots |
|---|---|---|
| Remote source, first lookup | GM full | GN |
| Later source-group lookup | GM restricted | GN hierarchy constrained to a current global output |
| Intermediate-group lookup | GM full first, then GM restricted | LN |
| Target or same-group lookup | LM when minimally eligible | LN |
| After LN root-detect | GM or LM only | None |

A local adaptive GN or GM choice is not an end-to-end commitment. The next
input queue performs another legal arbitration. A global GN crossing fixes the
intermediate group because the packet has physically entered it. An LN lookup
may also return the patent's self/root-detect entry. For a non-self result, the
decision records a pending one-hop root; the state returned by the core is the
state for the next input. That input validates the expected router, previous
router, and previous output port before root-detect ends uprouting for the
group.

The canonical topology sometimes has only one legal physical output in a
class. In that case the same GM entry occupies both minimal slots. When at
least two physical outputs exist, the two candidates use different output
ports. This is a simulator abstraction: the product tables may contain
weighted duplicate entries that map to one physical output.

Each candidate receives a congestion value composed from three four-bit
signals:

```text
near-end output pressure
+ far-end resident estimate
+ downstream advertised pressure
= four-bit saturating congestion
```

The routing class then applies a configurable left shift and a six-bit
additive bias. The lowest adjusted value wins. A minimal candidate wins a tie
against a non-minimal candidate. Same-class ties follow deterministic seeded
candidate order.

For example, suppose both source-local output queues are empty. Before a
neighbor advertisement arrives, the minimal and non-minimal scores are tied,
so the minimal route wins. If a physically delayed advertisement later reports
pressure beyond the minimal output, the near-end queues are still equal but
the non-minimal candidate can win. The test suite exercises this exact event
boundary. A second test makes the first local choice GN and the next restricted
lookup GM, proving that a local GN output does not freeze a route class.

## Causal congestion information

The implementation separates bytes into three states:

```text
sent but not credited = outstanding
sent but not yet arrived at neighbor = in flight
estimated resident at neighbor = outstanding - in flight
```

A byte on the link is therefore never counted as already buffered at the
far-end input. Credit return is rejected if it would acknowledge bytes that
are still in flight.

Downstream advertisements carry:

- a monotonically increasing sequence;
- the time at which the neighbor made the observation;
- the earliest time at which the message can physically arrive; and
- the advertised waiting-byte count.

The router cannot consume an advertisement before its arrival boundary.
Reordered sequences and observations older than the installed sample are
ignored. Installed downstream pressure also has a configured maximum age; it
is valid through the exact age boundary and contributes zero one picosecond
later if no refresh arrives.

Physical link state is separate from routing-table knowledge. A router filters
its own failed output immediately. A failure at another router changes this
router's destination/safe-group table only after a sequenced route update has
crossed its configured physical-delay boundary.

The current files model these semantics without an `EventList` dependency so
they can be exhaustively tested. The Dragonfly Rosetta adapter must deliver
advertisements through modeled serialization and propagation before calling
the consume operation.

## Persistent route state

The packet stores its routing control, UP/DOWN episode, expected current router,
previous router and output port, global crossings, physically reached
intermediate group, pending/authenticated LN roots, and logical dependency
phase. The state deliberately does not turn a local adaptive table choice into
a whole-route commitment.

The four logical table families are explicit:

- GN: select a safe non-minimal intermediate group;
- GM: move minimally toward the next target group;
- LN: select or reach a non-minimal local root; and
- LM: finish legal minimal movement inside the target group.

The patent's concrete topology has two ordered local dimensions and describes
three physical VCs. This study uses a canonical local full mesh, which collapses
those dimensions. To retain a mechanically checkable dependency proof, the
pure core assigns one strictly increasing logical dependency phase per router
hop. These eight labels are not a claim of eight product VC buffers. Production
integration must either preserve them or supply a new dimension-aware mapping
and dependency proof.

The patent's LM `diverged` bit is explicit as well. A marked LM entry is
excluded from adaptive target-group uprouting but remains legal after an LN
root transition and for deterministic downrouting. The default full-mesh table
does not mark its direct one-hop LM entry, but tests program the bit and verify
both sides of the rule.

Exhaustive tests cover every ordered source/destination router pair in the
36-router topology. They exercise both deterministic controls and all four
adaptive controls under biases that force each available route class:

- a minimal inter-group route uses one global hop and at most three router
  hops; and
- a non-minimal inter-group route uses two global hops and at most eight router
  hops.

## Failure behavior

Candidate filtering reads only the current router's directed output state. It
never preflights a remote connector's live port. A stale source may therefore
send one packet over a live local hop toward a connector whose global output
has failed; that connector declares the drop. After the physical route update
arrives, new source lookups exclude the unavailable destination or unsafe
intermediate group.

An intermediate-group safety update means that the control plane has verified
the group's required local and global escape reachability. If a physically
committed output fails later, forwarding reports a declared drop rather than
teleporting or silently changing a physically reached intermediate. Higher-level
retry remains responsible for recovery.

## Ordered and adaptive modes

The routing controls are:

- four adaptive classes, each with independent minimal/non-minimal biases;
- deterministic minimal; and
- deterministic non-minimal.

Deterministic modes depend on a flow-stable route hash and topology, not
congestion. They provide the stable path required by ordered traffic and the
common-route Dragonfly comparison. Ordered state-store bindings reject adaptive
controls and reject changes to endpoints, control, or route hash. Adaptive mode
may choose a different path for every packet and is used only when the endpoint
transport permits reordering.

## Current integration boundary

Implemented and tested on main after the port:

- canonical Dragonfly geometry and stable port identities;
- GM, GN, LM, and LN table behavior;
- per-input progressive sampling, route-class biasing, and tie rules;
- persistent physical-boundary and dependency-phase state;
- a topology-owned, identity-checked packet route-state sidecar;
- causally delayed failure and safe-intermediate filtering;
- four-bit remapping and expiring causal near/far/downstream signals;
- the complete nine-placement 64-of-72 endpoint and logical-role rotation;
- the RNIC-SS load-observation timestamp correction (carried inside the
  rnic-ss hosting port); and
- landed by the ss-dragonfly fabric wave: the Rosetta-style switch
  adapter performing the route lookup after the modeled switch-pipeline
  delay, fabric-owned packet route-state plumbing, and modeled
  downstream-advertisement delivery
  (`ss_dragonfly_fabric.{h,cpp}`, hosted with calibration pending).

Still not enabled in a production simulator executable:

- route-state plumbing for `rnic-ss` and the ordered DCQCN control over
  the dragonfly fabric (the rnic profiles stay on the controlled Clos
  until the Merlin calibration wave);
- physical credit and advertisement packets that contend with DATA (the
  fabric models the advertisement path as a delayed sideband, recorded
  as a named calibrated abstraction);
- phase-separated buffering/PFC treatment for a lossless DCQCN
  baseline; and
- wiring the balanced 64-active-rank placement into GOAL/runtime
  endpoint binding.

Until those gates are complete, the core generates fabric sanity
evidence only. The controlled Clos remains the comparator evidence.

## Suggested review order

1. `docs/ss-dragonfly-fabric/README.md`
2. `dragonfly_progressive_routing.h`
3. `dragonfly_progressive_routing.cpp`
4. `dragonfly_progressive_congestion.h`
5. `dragonfly_progressive_congestion.cpp`
6. `dragonfly_progressive_routing_test.cpp`
7. `dragonfly_progressive_congestion_test.cpp`
8. `dragonfly_route_state_store.{h,cpp}` and its test
9. `dragonfly_endpoint_placement.{h,cpp}` and its test
10. `ss_dragonfly_fabric.{h,cpp}` and its fixtures
