# Physical switch models

This directory documents the switch layer added in incremental PR 3 of 5.
The switch layer is independent of the physical endpoint work in PR 2. It uses
only existing HTSIM packet, route, queue, pipe, event, and FatTree interfaces
plus one shared endpoint-pair value type.

The PR adds two selectable Clos switch models:

- `ns-tm3`: output-oriented virtual-output-queue traffic management;
- `ns-rosetta`: input-buffered request/grant matching.

Both are behavioral models for controlled simulation experiments. They define
packet timing, arbitration, buffering, accounting, and route boundaries. They
are not cycle-accurate implementations of commercial hardware.

## Scope

| Component | Role |
| --- | --- |
| `fat_tree_switch_model.{h,cpp}` | Defines stable model names |
| `fat_tree_switch_factory.{h,cpp}` | Constructs the legacy, ns-tm3, or ns-rosetta switch |
| `fat_tree_switch.h` | Exposes physical-ingress and switch-pipeline seams |
| `ns_tm3_switch.{h,cpp}` | Implements shared-memory egress VoQs and output serialization |
| `ns_tm3_dcqcn_policy.{h,cpp}` | Adds opt-in deterministic ECN and link-local PFC behavior |
| `ns_rosetta_switch.{h,cpp}` | Implements input-buffered request/grant arbitration |
| `rnic_ss_endpoint_pair.h` | Gives switch accounting and later RNIC-SS control one pair identity |
| `fat_tree_topology.{h,cpp}` | Builds physical ingress adapters and model-owned serializers |

The adjacent tests exercise these components directly and through constructed
two-tier and three-tier FatTree topologies.

## Packet path through a modeled switch

A physical route has an explicit switch boundary:

```text
upstream egress serializer
          |
   propagation pipe
          |
physical ingress adapter
          |
 configured switch delay
          |
 switch-owned arbitration and buffer
          |
 local egress serializer
          |
   propagation pipe
```

The physical ingress adapter records which cable delivered the packet. The
packet then traverses the existing `FatTreeSwitch` pipeline delay. When that
delay expires, the model resolves the selected local egress from the explicit
route or from the existing FatTree FIB.

A DATA or ordinary routed control packet cannot call a model-owned egress
serializer directly. The serializer accepts only the packet that its owning
switch has authorized for dispatch. This prevents a route from bypassing
buffer admission or arbitration. Link-local ns-tm3 PAUSE/RESUME delivery is
the deliberate exception: an enabled PFC policy sends those frames directly
to the upstream egress serializer.

## Rules shared by both models

### Priority classes

The models map HTSIM priorities into three strict classes:

```text
class 0: PRIO_HI
class 1: PRIO_MID
class 2: PRIO_LO and PRIO_NONE
```

The highest class with an eligible request is considered first. A paused
ns-tm3 DATA class is ineligible, as is a Rosetta request from an input already
matched or busy. Serialization is non-preemptive, so a newly arrived
high-priority packet cannot interrupt a lower-priority packet already on the
wire.

### One physical serializer per egress

Each physical output owns one serializer. Two different outputs can transmit
concurrently, but one output cannot overlap packets. The serializer holds only
the packet currently on the wire; waiting packets remain in switch-owned
queues.

### Buffer occupancy and backlog

Waiting wire bytes consume the switch-wide shared buffer. When a packet starts
serialization, its bytes leave that buffer.

```text
buffered bytes = bytes waiting in switch-owned queues
in-service bytes = bytes in the output serializer
backlog bytes = buffered bytes + in-service bytes
```

This distinction matters for queue telemetry. A just-dequeued packet produces
zero buffered bytes when it was the last waiter, but the output still has
backlog until serialization completes.

Both models require a positive shared-buffer capacity. Admission drops a whole
packet if the packet does not fit; no partial packet is stored.

## ns-tm3

### Queue structure

`NsTm3Switch` keeps a VoQ for each:

```text
(physical egress, priority class, physical ingress)
```

Separating queues by egress prevents a blocked destination from holding a
packet for a free destination behind it. Separating by ingress preserves the
input identity needed by arbitration and optional PFC accounting.

For one input with packets for outputs 0 and 1:

```text
output 0 busy: packet A waits in VoQ(input, output 0)
output 1 free: packet B can start on output 1
```

Packet B does not wait behind packet A merely because both arrived on the same
physical input.

### Default output arbitration

When an output becomes free, ns-tm3:

1. selects the highest eligible non-empty priority class;
2. compares the head packet of every non-empty ingress VoQ in that class;
3. selects the oldest enqueue timestamp;
4. resolves an equal-timestamp tie by the lowest ingress ID.

This is the default `OldestHeadFirst` mode. It models one output FIFO across
the heads of its ingress VoQs without reintroducing cross-output head-of-line
blocking.

`IngressRoundRobin` is retained as an explicit sensitivity. It gives each
non-empty ingress one whole-packet turn for an output. Arbitration mode and the
per-egress buffer cap can change only while no packet is buffered, in the
switch pipeline, or being serialized.

### Shared and per-egress limits

The switch has one physical shared pool and a separate cap for bytes mapped to
any single egress. By default the egress cap equals the shared capacity.

```text
shared capacity: 400 bytes
egress cap:       200 bytes

egress 0 waiting: 200 bytes -> full for that egress
egress 1 waiting: 100 bytes -> allowed
shared occupancy: 300 bytes
```

A shared-pool drop and a per-egress-domain drop have separate counters. Bytes
already in serialization are in neither admission domain.

### Queue observations

An optional `NsTm3QueueObserver` receives exact event-boundary records for:

- enqueue;
- dequeue into serialization;
- drop;
- serialization completion.

Each record contains switch and port identity, packet identity, egress
buffered bytes, in-service bytes, total egress backlog, and switch-wide
occupancy. The observer cannot change scheduling.

## Optional ns-tm3 DCQCN policy

The base ns-tm3 model constructs no ECN/PFC policy. `configure_dcqcn_policy`
enables it explicitly for a switch.

### Deterministic ECN marking

At packet selection, the policy applies a linear RED probability between
`kmin` and `kmax`, capped by `pmax`. Only RoCE packets are marked.

The decision is derived from:

- the configured seed;
- switch congestion-domain identity;
- flow ID and RoCE sequence;
- ingress and egress IDs.

The pseudo-random sample does not depend on allocator reuse or unrelated
packet IDs. Given the same identities and observed egress occupancy, the mark
decision repeats exactly. Occupancy itself still follows the simulated packet
event sequence.

### Link-local PFC

PFC meters admitted low-priority bytes independently for each physical
ingress. Crossing the high threshold emits PAUSE; draining to the low
threshold emits RESUME.

The reverse control path has one dedicated link-local serializer per physical
ingress, followed by the real cable propagation delay learned from the
packet's explicit route. This serializer is an intentional approximation: it
does not contend with reverse DATA or other reverse control traffic.

A received PAUSE state blocks only the low-priority class and takes effect at
a packet boundary. High- and middle-priority traffic remain eligible.

The selected FatTree model must still use a non-lossless topology queue mode.
The switch-local policy is configured separately; it does not reuse HTSIM's
legacy lossless queue implementation.

## ns-rosetta

### Input-buffered VoQs

`NsRosetta` stores packets in queues indexed by:

```text
(physical input, physical output, priority class)
```

Each free output requests one eligible input. Each free input accepts at most
one output request. A matched input stays occupied until its granted packet
finishes output serialization.

### Request/grant rounds

Arbitration proceeds as follows:

1. each free output chooses the highest class with an eligible input request;
2. within that class, the output requests an ingress using round-robin state;
3. an ingress receiving several requests accepts the highest-priority class;
4. equal-class output requests are accepted using per-ingress round-robin;
5. matched inputs and outputs are removed from the current round;
6. proposal rounds continue until no additional match is possible.

For two inputs and two free outputs, the switch may dispatch two packets at
once when they use different inputs and outputs. One input cannot drive both
outputs during the same serialization interval.

Arbitration runs when a packet enters the switch buffer and when a serializer
completes. It does not wait to batch every arrival that shares the same
simulation timestamp.

### Pair accounting

Rosetta tracks queued and in-service bytes by directed endpoint pair as well
as by input and output. `RnicSsEndpointPair` is defined in a small shared
header so later RNIC-SS control and the switch use the same identity without a
translation table.

An optional `NsRosettaBacklogObserver` reports switch-local transitions and
pair contributions. It does not send telemetry packets, update a sender, or
read state from another switch.

### Path-load samples

`sample_path_load(egress)` returns a snapshot of one local output:

- requesting ingress count;
- queued packet and byte counts;
- bytes currently in serialization;
- ordered-route reservations;
- switch-wide occupancy and capacity;
- backlog delay at the output's link rate.

Backlog delay includes the residual time of the packet already in service,
not its original whole-packet serialization time.

### Ordered-route reservations

Before the first DATA packet of a newly bound ordered endpoint pair reaches
its source-leaf switch, the route selector may reserve one DATA envelope on
the chosen egress. The reservation contributes to local path pressure but not
to shared-buffer occupancy because no DATA bytes have arrived.

The first matching low-priority packet consumes the reservation. The switch
does not inspect an RNIC-SS packet-kind field at this boundary; the later
runtime must put ACK, credit, and backpressure traffic in a higher priority
class so those packets cannot consume a DATA reservation. A pair may have only
one live reservation, and arrival on a different egress is rejected.

Rosetta has no PFC mode. Congestion response and physical telemetry transport
belong to the later RNIC-SS runtime PR.

## Clos topology integration

`FatTreeSwitchModel` has three stable manifest names:

```text
default
ns-tm3
ns-rosetta
```

`default` constructs the existing `FatTreeSwitch` and preserves the legacy
queue path. Selecting either new model makes the topology:

- construct the corresponding switch at every configured Clos tier;
- replace output queues with model-owned physical serializers;
- create a distinct physical ingress adapter for every incoming cable;
- include those ingress adapters in generated routes;
- derive shared capacity from the tier's configured up/down queue sizes unless
  an explicit model capacity override is set.

File-backed queue sizes are preserved during topology initialization because
they provide the fallback shared capacity. An explicit capacity override must
be positive.

Legacy LOSSLESS, LOSSLESS_INPUT, and LOSSLESS_INPUT_ECN topology queue modes
are rejected for ns-tm3 and ns-rosetta. Combining a custom switch buffer with
a second legacy lossless buffering layer would make capacity and PAUSE
semantics ambiguous.

This PR exposes the model enum and C++ configuration methods. Profile parsing,
CLI selection, runtime assembly, and experiment configuration are deferred.

## Rejected states

The implementation rejects states that would bypass or change the model
mid-flight, including:

- packets entering without a physical ingress adapter;
- routes whose selected egress is not owned by the current switch;
- direct non-PFC calls into a model-owned serializer;
- nonpositive shared capacity;
- ns-tm3 egress caps outside `(0, shared capacity]`;
- ns-tm3 policy or arbitration changes while affected packets are active;
- PAUSE packets or PAUSE topology modes where unsupported;
- duplicate or mismatched Rosetta route reservations;
- buffer, timestamp, and reservation arithmetic overflow.

## Assumptions and non-goals

- Packets are served as non-preemptive whole wire packets.
- The existing FatTree FIB remains the source of next-hop decisions.
- Observers expose local state and do not participate in arbitration.
- The models capture the stated queueing contracts, not internal ASIC cycles.
- The PR does not add an RNIC-CN or RNIC-SS congestion controller.
- The PR does not add route-selection policy at the sender.
- The PR does not add drivers, command-line options, GOAL integration, traces,
  workloads, plotting, or experiment scripts.
- The PR does not change the default switch selected by existing simulations.

## Test map

`fat_tree_switch_factory_test.cpp` covers stable names, default preservation,
model construction, and independence from queue configuration.

`ns_tm3_switch_test.cpp` covers VoQ separation, both arbitration modes,
concurrent outputs, pipeline delay, strict priority, shared and per-egress
limits, accounting, observers, invalid route bypass, Clos FIB behavior,
physical ingress wiring, and queue-mode rejection.

`ns_tm3_dcqcn_policy_test.cpp` covers deterministic RED endpoints and replay,
opt-in behavior, independent ingress PFC meters, reverse PAUSE/RESUME timing,
and control progress while DATA is paused.

`ns_rosetta_switch_test.cpp` covers request/grant matching, priority,
round-robin state, shared-buffer drops, local path samples, residual service,
pair observations, ordered-route reservations, topology construction, and
PAUSE rejection.

## Review order

1. This README for scope and invariants.
2. `fat_tree_switch_model.{h,cpp}` and `fat_tree_switch_factory.{h,cpp}`.
3. The two additions to `fat_tree_switch.h`.
4. `rnic_ss_endpoint_pair.h`.
5. `ns_tm3_switch.h`, then `ns_tm3_switch.cpp`.
6. `ns_tm3_dcqcn_policy.h`, then `ns_tm3_dcqcn_policy.cpp`.
7. `ns_rosetta_switch.h`, then `ns_rosetta_switch.cpp`.
8. The focused `fat_tree_topology.{h,cpp}` changes.
9. The four adjacent test files.
10. `datacenter/CMakeLists.txt`.
