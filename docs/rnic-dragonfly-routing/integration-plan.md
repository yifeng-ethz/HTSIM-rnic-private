# Production integration plan

Provenance: authored on origin/codex/rnic-dragonfly-routing (commit
2efd81b) and ported unchanged onto the ss-dragonfly fabric branch
(codex/ss_dragonfly_fabric, not yet main-reachable) as the standing
integration contract. The ss-dragonfly fabric wave landed the first stage of this
plan (the Rosetta-style dragonfly wrapper with post-pipeline lookup,
topology-owned route state, and modeled advertisement delivery) as
`ss_dragonfly_fabric.{h,cpp}`; deviations this wave took, such as the
sideband advertisement transport and VC labels without VC-keyed VoQs,
are recorded as named calibrated abstractions in
docs/ss-dragonfly-fabric/README.md. The remaining stages, including
`rnic-ss` hosting over the fabric, stay future work for the
calibration wave.

The standalone routing and congestion cores deliberately have no dependency on
legacy `DragonflySwitch`, `Packet`, or `EventList`. Production integration must
preserve their causal and route-state guarantees instead of adding a new mode
to the legacy `UGAL_L` branch.

## Ownership and files

### Canonical topology and policy

`dragonfly_progressive_routing.{h,cpp}` remains the single source for canonical
router/port geometry, GM/GN/LM/LN behavior, route state, phases, and candidate
selection. `DragonflyTopology` should construct its links from
`DragonflyCanonicalTopology::ports()` rather than duplicating connector
arithmetic.

`DragonflyTopology` additionally owns:

- core-port to physical-egress maps;
- physical ingress objects and cached one-hop routes;
- a selected member of `DragonflyEndpointPlacementEnsemble`;
- the progressive-routing object;
- per-port congestion transports and link-alive state; and
- the packet route-state store.

The legacy `dragonfly_switch.{h,cpp}` remains unchanged for its existing
`MINIMAL`, `VALIANT`, `UGAL_L`, and `SOURCE` modes.

### Packet state

Use `dragonfly_route_state_store.{h,cpp}`. The topology-owned store holds one
`DragonflyRouteState` for each live `Packet*`, guarded by packet ID, source,
destination, and optional ordered-binding identity. Each forwarding decision
records both the expected next router and the physical output used to reach it;
the next switch validates both before mutating the state.

Lifecycle:

1. the fabric inserts state before source injection;
2. each progressive switch requires and mutates that state;
3. destination delivery erases it before forwarding to the endpoint;
4. every switch, link, and admission drop erases it before `Packet::free()`;
5. pointer reuse with an uncleared entry is a hard error; and
6. ordered bindings accept deterministic controls only and remain retained at
   zero live packets until the flow owner explicitly releases them; and
7. quiescence requires both packet entries and retained bindings to be empty.

Do not add route state to base `Packet` and do not encode it in `_pathid` or
`_direction`; those fields cannot hold the intermediate group, root,
divergence, phase, and VC state.

### Rosetta traffic manager

Extract the topology-independent request/grant machinery currently embedded in
`NsRosetta` into `rosetta_traffic_manager.{h,cpp}`. It owns shared-buffer
accounting, ingress VoQs, arbitration, serializer state, and observations.

`NsRosetta` becomes the unchanged-behavior FatTree wrapper around this core.
Add `dragonfly_rosetta_switch.{h,cpp}` as the Dragonfly wrapper. Its order is:

1. physical-ingress arrival;
2. modeled switch-pipeline delay;
3. progressive route lookup;
4. Rosetta admission and request/grant;
5. selected output serialization; and
6. one-hop propagation.

The lookup must occur after the switch-pipeline delay. The legacy
`DragonflySwitch` chooses before its `CallbackPipe` delay, which would hide an
advertisement arriving during that interval. The selected output is
revalidated again at serialization because a link may fail while the packet is
waiting in a VoQ.

### RNIC fabric boundary

Add `rnic_ss_fabric.{h,cpp}` with a topology-neutral `RnicSsPhysicalFabric`
interface for endpoint binding, packet preparation, injection, queue
observation, congestion domains, no-load diameter, and topology-specific
envelope validation.

- `RnicSsClosFabric` wraps the existing `rnic_ss_route` four-of-eight Clos
  behavior.
- `RnicSsDragonflyFabric` injects at the source router and initializes the
  topology-owned route state. Routing then happens at Dragonfly switches.
- `RnicSsRuntime` consumes only `RnicSsPhysicalFabric`, not concrete
  `FatTreeTopology` or `NsRosetta` pointers.
- The runtime factory owns topology, fabric, and runtime in destruction-safe
  order.

The CLI and manifest must name fabric, endpoint placement, routing control,
route seed, adaptive bias, quantizers, advertisement delay, credit delay, and
link-failure seed separately.

## Virtual channels and deadlock boundary

The route core's eight values are strictly increasing logical hop-dependency
phases for the collapsed one-local-dimension topology. They are not the
patent's concrete three-physical-VC allocation. Production Rosetta VoQs must
initially be keyed by:

```text
(traffic class, VC, physical ingress, physical egress)
```

VC is independent of the existing three packet priorities.

- every physical router hop strictly advances the logical phase;
- buffer or credit resources must be separated or have reserved capacity; and
- control traffic needs reserved progress when DATA consumes the shared pool.

A later reduction to three physical VCs is allowed only with a dimension-aware
mapping and a newly enumerated channel-dependency proof. If per-VC capacity
affects results, report both the conservative mapping and the reduced mapping
as a sensitivity rather than attributing the difference to routing.

Generate the complete `(directed link, VC)` channel-dependency graph for
`p2a4h2` and assert that it is acyclic. A single priority-wide PFC class is not
valid on cyclic Dragonfly. The DCQCN control either needs phase-separated
VC/PFC with a dependency proof or must be labeled as a lossy ECN/DCQCN mode.

If the final simulator is store-and-forward without hop-level blocking credit,
describe these VCs as route-legality/dependency labels rather than a
cycle-accurate wormhole implementation.

## Physical congestion transport

Add `dragonfly_congestion_transport.{h,cpp}` around
`DragonflyProgressiveCongestion`, one instance per directed output.

- Near-end pressure comes from local waiting and in-service bytes.
- Far-end pressure is outstanding sent bytes minus bytes still in flight.
- Downstream pressure is installed only when a neighbor advertisement arrives.
- Message departure, link arrival, input/VC release, credit return, and
  advertisement arrival are separate modeled events.
- Same-time ordering processes a credit or advertisement arriving exactly at
  the lookup timestamp before route lookup.
- `DragonflyProgressiveRouting::setPortCongestion()` is updated only by local
  events or physically delivered control information.

Add a parallel sequenced route-update transport for destination availability
and intermediate-group safety. A physical failure changes the failed router's
own directed port immediately; no other router may update its table until that
control message arrives. Do not scan another connector's live link state.

The manifest records advertisement and credit bytes and whether they use a
sideband or contend with DATA.

## Integration gates

Before a Dragonfly performance plot is accepted:

1. Clos `ns-rosetta` behavior and tests remain unchanged after extraction.
2. Same-group, minimal inter-group, and non-minimal packets follow the exact
   router and port sequence produced by the pure core.
3. Route state is released after delivery, admission drop, link drop, retry,
   and pooled-pointer reuse.
4. Per-VC isolation, control progress, and the channel-dependency graph pass.
5. Equal source queues plus a remote hotspot change selection only after the
   advertisement physically arrives.
6. Credit changes far-end occupancy only after its modeled return.
7. Ordered deterministic traffic has zero reordering; adaptive controls are
   rejected for ordered bindings; unordered adaptive
   traffic completes through SACK recovery.
8. ACK, backpressure, and credit packets initially use deterministic minimal
   routing.
9. The 72-endpoint fabric maps 64 active ranks across all nine groups, rotates
   the eight idle endpoints, and rotates every logical rank role through all
   nine groups over the placement ensemble.
10. DCQCN and `rnic-ss` share the exact graph, placement, link parameters,
    buffer budget, workload, and deterministic-minimal route map before the
    adaptive-routing ablation is enabled.
