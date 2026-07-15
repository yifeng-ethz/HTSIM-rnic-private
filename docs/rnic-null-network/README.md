# RNIC null-network models

This directory documents two deliberately simple RNIC baselines:

- `rnic-nn-fluid` treats traffic as continuous fluid.
- `rnic-nn` sends packet-sized quanta through a deterministic slot calendar.

Both models answer the same question: **what completion time would endpoint
sharing alone produce if the network fabric added no congestion of its own?**
They are useful reference models for separating endpoint limits from effects
caused by switches, queues, routing, loss, or congestion control.

Here, “null network” means *topology-free*: there are no modeled switches or
internal fabric links. It does not mean zero delay or unlimited bandwidth.
Endpoint capacity is still finite, and every delivery still has a fixed
propagation delay.

| | `rnic-nn-fluid` | `rnic-nn` |
| --- | --- | --- |
| Service | Continuous payload service | Whole DATA packets in fixed reservation slots |
| Packet overhead | Not modeled | Headers consume wire capacity |
| Timing | Abstract service, then propagation | Source serialization, propagation, then destination serialization |
| Allocation change | Takes effect immediately | Takes effect at the next unreserved slot |

## Shared model

Every node has one sending link and one receiving link. A flow consumes capacity
on its source's sending link and its destination's receiving link. Flows with no
shared endpoint do not interfere with one another.

The models recompute an unweighted max-min fair allocation whenever the active
flow set changes. In plain language, all flows grow at the same rate until an
endpoint becomes full. Flows using that endpoint stop growing, while unrelated
flows may continue to increase. An optional demand cap can stop one flow at a
lower rate. The allocator and model primitives support these caps; the current
`AtlahsFlowRuntime` adapters submit uncapped flows.

For example, suppose every endpoint runs at 100 Gbit/s and these flows are
active:

```text
A -> X
A -> Y
B -> X
```

The first two flows share A's sending link, and the first and third share X's
receiving link. Max-min fairness gives each flow 50 Gbit/s. Both contested
endpoints are then exactly full.

The allocator solves the continuous fair rates with exact rational arithmetic.
Executable rates are whole bits per second and are rounded down. A fractional
remainder stays idle instead of being awarded according to flow ID.

Both baselines intentionally omit:

- switch queues and internal fabric links;
- routing and path selection;
- packet loss, ECN, PFC, and backpressure;
- acknowledgements, retransmission, and receiver resequencing;
- PRBS pacing and physical-switch behavior.

A fixed propagation delay is still applied, so delivery happens after service
rather than at the service-completion timestamp.

## Fluid model: `rnic-nn-fluid`

The fluid model serves every active flow continuously at its current fair rate.
When a flow starts or finishes, the rates are recomputed and service continues
with the new allocation.

Imagine pouring water through pipes: the model tracks how much service remains,
not individual packets or a source serializer. If a flow finishes service at
time `t`, its delivery completion is `t + propagation_delay`.

This model is the cleanest ideal reference because it has no packet boundaries.
Its main implementation is:

```text
htsim/sim/rnic_fluid_manifold.{h,cpp}
htsim/sim/rnic_fluid_manifold_runtime.{h,cpp}
```

## Packetized (slot-quantized) model: `rnic-nn`

The packetized model uses the same fair rates but makes service packet-sized. It
divides time into wire-quantum reservations and selects a collision-free set of
flows in each slot. A node can participate in at most one selected transmission
as a sender and at most one as a receiver in that slot.

Continuing the example above, `A -> Y` cannot share a source slot with `A -> X`,
and `B -> X` cannot share a destination slot with `A -> X`. However, `A -> Y`
and `B -> X` may run in the same slot because they use different sending and
receiving endpoints.

Each flow accumulates signed scheduling credit from its fair grant. The calendar
uses that credit to choose packets deterministically while preserving endpoint
capacity. New flows can receive service in their first positive-rate slot, and
surviving flows keep their credit across allocation epochs.

The fair grant is measured in **wire** bits per second. DATA headers consume
part of that grant, so application payload goodput can be lower than the
reported wire rate.

A reservation always occupies one full configured wire quantum. A short final
packet keeps that reservation envelope, while its actual source and destination
serialization intervals use its exact wire length. This makes packet timing
deterministic without pretending that the final packet contains padding data.
For example, with a 10-byte maximum wire packet and a 2-byte DATA header, an
18-byte payload becomes wire packets of 10, 10, and 4 bytes. The 4-byte tail
still owns one full reservation slot, but each serializer handles only 4 bytes.

For each selected packet, the timeline is source serialization, fixed
propagation, and destination serialization. The flow's completion callback
fires only after its final packet finishes destination serialization. Once a
slot is committed it is not rewritten; a changed active set or fair grant is
used when the next unreserved slot is planned.

The main implementation is:

```text
htsim/sim/rnic_packetized_manifold.{h,cpp}
htsim/sim/rnic_packetized_manifold_runtime.{h,cpp}
```

## Runtime boundary

Both models implement the small `AtlahsFlowRuntime` interface. A caller:

1. calls `setup()` with the number of nodes and a completion callback;
2. submits a flow with `send()`;
3. advances the normal HTSIM event list;
4. receives exactly one completion callback after delivery.

The runtime adapters own their scheduling and report pending work until every
delivery event has drained. They do not depend on the ATLAHS command-line
driver, workload generators, trace writers, or experiment scripts.

## Choosing between the models

Use `rnic-nn-fluid` when the desired reference is ideal continuous sharing. Use
`rnic-nn` when packet boundaries and collision-free endpoint scheduling matter,
but switch and transport effects should still be absent.

The two models should not be expected to have identical completion times for
small flows. Quantization can leave a flow waiting for its next eligible slot;
that difference is the intended cost of packet-sized service.

## Build and test

From the repository root:

```sh
cmake -S htsim/sim -B build -DENABLE_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The focused tests cover exact max-min allocation, endpoint conflicts,
deterministic slot selection, short final packets, allocation changes, delivery
timing, completion callbacks, invalid input, and arithmetic overflow.

Physical RNIC profile implementations, switch models, ATLAHS driver
integration, telemetry, and experiments are intentionally outside this change
and will be introduced in separate pull requests.
