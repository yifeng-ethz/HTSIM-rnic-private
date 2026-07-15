# PR 5: progressive Dragonfly routing and comparison contract

## Decision

PR 5 adds a native Dragonfly study for `rnic-ss`, but it does not replace the
existing 64-node Clos study.

The Clos study remains the controlled comparison because all physical profiles
use the same graph, endpoint rate, workload, and propagation model. A direct
`DCQCN/Clos` versus `rnic-ss/Dragonfly` result changes the congestion mechanism,
switch behavior, topology, hop distribution, and bisection structure at once.
Such a result may be reported as a native whole-system comparison, but it cannot
attribute the difference to congestion control or adaptive routing alone.

## Required experiment matrix

| Study | DCQCN | `rnic-ss` | Supported conclusion |
|---|---|---|---|
| Controlled Clos | Existing ordered Clos routing | Existing ordered Clos routing | Controlled whole-stack comparison |
| Dragonfly common-route control | Deterministic minimal, flow-sticky | The identical deterministic minimal route map | Congestion-management comparison on one Dragonfly |
| Dragonfly routing ablation | Deterministic minimal | Deterministic minimal and progressive adaptive | Incremental adaptive-routing effect |
| Dragonfly native mode | Best valid deterministic ordered mode | Deterministic ordered; unordered progressive as sensitivity | Labeled native-system result |

Packet-by-packet spraying is not assigned to DCQCN/RoCE because it would change
its ordering contract. If an adaptive DCQCN sensitivity is added, it must bind a
route at flow establishment or at an explicitly safe flowlet boundary.

## Initial topology

Use the checked-in balanced `p2a4h2` Dragonfly:

- two hosts per router;
- four routers per group;
- two global links per router;
- nine groups and 72 physical endpoints;
- 400 Gbit/s host, local, and global links;
- 25 ns host/local propagation, 500 ns global propagation, and 500 ns switch
  latency.

The 64-rank paper workload cannot be represented by a maximal, nondegenerate
64-endpoint Dragonfly under `N = p * a * (a*h + 1)`. Run the workload on 64 of
the 72 physical endpoints. Distribute the eight idle endpoints across all nine
groups; do not leave one complete group empty, because that creates an
unrealistically attractive intermediate group.

Use a nine-placement ensemble that rotates which group has eight active ranks
and which individual endpoint is idle in the other groups. Every comparator in
one paired run uses the identical placement. Logical rank blocks rotate with
the full group so fixed roles such as rank 0 visit every group exactly once. A
separate 72-active-rank workload is the preferred topology-saturation study.

## Fidelity target

The target is the progressive-routing mechanism described by US 9,137,143 B2,
not a claim of cycle-accurate Aries or proprietary Slingshot reproduction.

The implementation must provide:

1. persistent per-packet routing control, UP/DOWN episode, physically reached
   intermediate group, pending/authenticated LN roots, current-episode
   divergence, expected router, previous output port, and dependency phase;
2. global non-minimal (GN), global minimal (GM), local non-minimal (LN), and
   local minimal (LM) logical table sets;
3. at least one minimal and one safe non-minimal path, with support for
   presenting two candidates from each class;
4. two unique candidates per class when two physical entries exist; a single
   legal entry may be presented twice without inventing a second physical link;
5. near-end output pressure, a far-end receive-buffer estimate derived from
   credits and messages in flight, and explicitly advertised downstream
   pressure;
6. programmable remapping into four-bit components and four-bit saturating
   composition;
7. configurable six-bit minimal/non-minimal shifts and additive biases;
8. minimal preference on a cross-class tie and seeded reproducible same-class
   selection;
9. a proved dependency mapping: the pure full-mesh core currently uses eight
   strictly increasing logical hop phases and does not claim they are the
   patent's three physical VCs;
10. current-router link-alive filtering plus physically delayed route-table and
    safe-intermediate updates; and
11. deterministic hashed routing for ordered traffic.

The following must not be inherited from the legacy `UGAL_L` implementation:

- reselecting a random Valiant hop every time the helper is called;
- scoring a path different from the path subsequently followed;
- choosing the destination group as an intermediate;
- selecting a failed output and relying on periodic packet loss;
- treating the current output queue as complete neighboring congestion; or
- omitting route phase and VC/dependency state.

## Physical information timing

Neighbor congestion does not become visible instantaneously. A testable update
must travel through the modeled advertisement or returning-credit path before a
router may consume it. The far-end estimate separates flits still in flight
from flits inferred to be resident at the remote input.

For the endpoint-returned `rnic-ss` sample, `observed_at_ps` is the time of the
last real switch grant accumulated by the DATA packet. Receiver arrival is not
the observation time. The runtime records age at both receiver and sender so an
age-boundary regression can detect accidental timestamp refreshing.

## Implementation sequence

1. Add a pure progressive-routing core with congestion composition, tables,
   candidate selection, persistent state, phase rules, failure filtering, and
   deterministic behavior.
2. Add a canonical Dragonfly graph/route substrate with explicit physical port
   identities and persistent route descriptors.
3. Add a Dragonfly Rosetta traffic-manager adapter. Reuse the request/grant VoQ
   machinery; do not duplicate it into an unrelated queue implementation.
4. Put the physical fabric and route policy behind the RNIC runtime boundary so
   the current Clos behavior remains unchanged.
5. Add the common deterministic route mode before enabling progressive routing.
6. Add experiment runner support for 72 physical endpoints, 64 active-rank
   placement, and separate workload, routing, and protocol seeds.
7. Run focused routing tests, physical hotspot tests, and paired workloads before
   generating presentation evidence.

## Required validation

### Pure routing tests

- two unique candidates per class and deterministic replay;
- minimal wins an equal adjusted score;
- remote congestion changes the winner while local queues remain equal;
- in-flight flits are not counted as remote-buffered flits;
- an advertisement cannot affect a decision before physical arrival;
- a physically entered intermediate group remains fixed;
- a local GN choice remains adaptive-UP, and only the first physical GN global
  crossing fixes the intermediate group;
- intermediate entry compares GM with LN, target entry compares LM with LN,
  and LN root-detect ends non-minimal eligibility in that group;
- minimal routes use at most one global link and non-minimal routes at most two;
- dependency phase never regresses and the enumerated dependency graph is
  acyclic;
- restricted GM and the LM diverged condition cannot reverse a completed route
  dimension;
- dead links and unsafe intermediate groups are excluded;
- all-invalid candidates produce the declared drop result; and
- deterministic packets retain a stable path.

### End-to-end tests

- within-group delivery;
- balanced inter-group permutation;
- a single global-link hotspot with unrelated bystanders;
- congestion beyond the source router with equal source-local queues;
- ordered deterministic routing without reordering;
- unordered adaptive routing with SACK recovery;
- all-to-all or collective traffic;
- zero loops, hop-budget violations, unexplained drops, and dependency-phase
  regressions; and
- failure invalidation after the control update physically arrives; before that
  boundary, a remote source is allowed to have stale route-table knowledge.

Destination incast alone is not an adaptive-routing validation because every
candidate eventually shares the destination endpoint link.

## Buffer and DCQCN controls

Match endpoint rate, physical graph, active-rank placement, propagation, packet
extent, and buffer budget within the Dragonfly study. Report both bytes and
queue time `q/C`, plus total network memory, unloaded latency, normalized
slowdown, total byte-hops, global byte-hops, path stretch, and control bytes.

A single priority-wide PFC class on a cyclic Dragonfly can create a pause
dependency cycle. A Dragonfly DCQCN baseline must either use route-phase-separated
VC/PFC state or be explicitly configured and labeled as a lossy ECN/DCQCN
variant. Reusing the Clos PFC assumptions without a dependency analysis is not
an acceptable control.

## Presentation rule

Keep the current controlled Clos slides. Add Dragonfly results as a separate
chapter or paired panel after the implementation and validation gates pass. Do
not overwrite Clos traces with Dragonfly traces or combine cross-topology curves
under one causal claim.

Any slide updated after the telemetry fix or Dragonfly integration must be
rebuilt from complete manifests, regenerated metrics, and fresh plots. The
exported PowerPoint must then pass overflow checks and full-slide visual review.
