# RNIC-CN endpoint

This directory documents the collective-network endpoint added in incremental
PR 4 of 5. It combines the physical RNIC machinery from PR 2 with the
`ns-tm3` switch model from PR 3.

`rnic-cn` is an explicit-rate protocol. A receiver counts the active senders
that target it, computes one wire-rate share, and returns that rate to each
sender over the simulated network. A new sender waits while existing senders
learn the lower rate. Existing senders update independently; they do not wait
for a receiver-wide activation event.

## Scope

| Component | Role |
| --- | --- |
| `rnic_collective_control.{h,cpp}` | Receiver membership, explicit-rate calculation, sender gate, epochs, and leases |
| `rnic_collective_packet.{h,cpp}` | Typed DATA and control packets with lifecycle accounting |
| `datacenter/rnic_collective_route.{h,cpp}` | Explicit physical paths through a two-tier `ns-tm3` Clos |
| `datacenter/rnic_collective_network_runtime.{h,cpp}` | End-to-end packet, control, resequencing, recovery, and retirement runtime |
| `eventlist.{h,cpp}` | Exact-timestamp settling query used before endpoint decisions |
| `datacenter/fat_tree_topology.{h,cpp}` | Read-only link-speed accessor used by transit calibration |

The runtime implements `AtlahsFlowRuntime`, but this PR does not connect it to
an executable or command-line profile. That integration remains a later
change.

## Packet path

Every flow follows this path:

```text
sender flow
    |
DECLARE -> ACCEPT -> two-window DATA gate
    |
node-wide PRBS selector
    |
one shared sender serializer
    |
explicit route through a two-tier ns-tm3 Clos
    |
receiver Ring-CAM
    |
one shared receiver serializer
    |
delivery ledger and RETIRE
```

DATA and control packets use the same sender wire. Control has strict priority
at a packet boundary, but it cannot preempt a packet already being serialized.
Every switch hop and propagation pipe in the route is explicit.

## Receiver rate calculation

Each `DECLARE` carries `nflow`. The receiver stores the contribution of
every active flow and computes:

```text
N_hat = sum(active nflow values)
wire_rate = floor(margin * bottleneck_wire_capacity / N_hat)
```

The default margin is 0.9. With a 100-Gbit/s receiver:

```text
one active contribution:  N_hat = 1 -> 90 Gbit/s
two active contributions: N_hat = 2 -> 45 Gbit/s each
four active contributions: N_hat = 4 -> 22.5 Gbit/s each
```

Rates are whole bits per second and are rounded down. The calculation uses only
`nflow`; collective identity and expected fan-in are not carried by the wire
packet and cannot affect the result.

Receiver membership changes are transactional. Declarations or retirements
observed at the same simulator timestamp are evaluated together so their
result does not depend on event insertion order. Later membership changes wait
until the current join gate opens. This serialization affects receiver state
only; it does not make incumbent rate updates synchronous.

## Joining with a two-`dwnd` gate

Let:

- `t0` be the time the receiver observes a new `DECLARE`;
- `K` be the configured worst-case one-way control delivery time;
- `D` be the Ring-CAM delay window, `dwnd`.

The new sender receives an `ACCEPT` containing the current epoch, explicit
rate, and:

```text
accept_delivery_deadline = t0 + K
join_not_before          = t0 + K + 2D
```

The sender's DATA rate remains zero until `join_not_before`. An `ACCEPT`
that misses its delivery deadline invalidates the run.

Existing senders do not receive one common update. A DATA packet selected for
rate feedback carries a mark. After that packet crosses the receiver
resequencer, the receiver returns a `GRANT_UPDATE` over the reverse physical
path. The sender applies the new rate when that packet arrives.

For example, suppose one sender has 90 Gbit/s and a second sender joins:

```text
before the marked ACK: incumbent = 90 Gbit/s, joiner = 0
after the marked ACK:  incumbent = 45 Gbit/s, joiner = 0
after the join gate:   incumbent = 45 Gbit/s, joiner = 45 Gbit/s
```

Different incumbents may step down at different times. Capacity can be unused
during this interval, which is safe because the joiner is still gated.

## Why the delay is two windows

Each active flow has one deterministic pseudo-random target position per delay
window. The first selected fresh DATA packet whose ETA reaches that target is
marked. A target near the start of one window followed by a target near the end
of the next window creates a gap approaching `2D`. Waiting only one window
would allow the joiner to start before an incumbent's next feedback could
return.

Missed empty windows are skipped. They do not accumulate marks that would make
several consecutive packets carry catch-up feedback after a sender resumes.

PRBS selection gives rate shares but does not guarantee that every low-rate
sender emits a DATA packet in a finite window. The lease handles that case:

1. the stale sender closes its DATA gate and clears its old rate;
2. it sends an idempotent `DECLARE` over the normal control path;
3. the receiver leaves membership unchanged and returns the current rate;
4. the sender resumes only after that reply arrives.

The ordinary feedback path remains one ACK per marked DATA packet. Re-DECLARE
is the fail-closed recovery path when no marked packet renewed the lease.

## Epochs, deadlines, and leases

Each rate reply contains:

- flow identity;
- receiver membership epoch;
- `N_hat` and explicit wire rate;
- receiver generation time;
- physical delivery deadline;
- lease expiry;
- whether the reply acknowledges marked DATA or an idempotent re-DECLARE.

For a marked packet released by the receiver at time `m`:

```text
feedback_deadline = m + K
lease_expiry      = m + K + 2D
```

The sender rejects a reply that arrives before its receiver generation time,
after its delivery deadline, or at or after lease expiry. It ignores older
epochs. Within one epoch, a reordered older reply cannot move the generation
time or lease backward. An exact duplicate has no second effect; a later reply
may extend the lease.

At an exact lease boundary, physical arrivals and ACKs are processed first. A
same-time valid renewal therefore makes the old expiry record stale. Any lease
that remains stale closes before a join gate opens or DATA is selected.

## Exact timestamp order

The endpoint runtime settles all other HTSIM sources queued at a timestamp
before it makes a scheduling decision. Its order at that timestamp is:

1. launch frames whose source serialization has completed;
2. process physical endpoint arrivals;
3. advance Ring-CAM and receiver serializers;
4. make due gap-recovery decisions;
5. process lease expiry and any fail-closed re-DECLARE;
6. open due join gates;
7. apply the next receiver membership change;
8. dispatch queued control;
9. dispatch DATA if no control used the boundary.

This ordering makes an ACK arriving exactly at its deadline timely and prevents
DATA from observing a partially settled timestamp.

## Wire objects

The profile uses seven packet kinds:

| Kind | Direction | Purpose |
| --- | --- | --- |
| `DATA` | sender to receiver | Payload, ETA, final ledger, retry attempt, optional rate mark |
| `DECLARE` | sender to receiver | Initial membership or fail-closed rate refresh |
| `ACCEPT` | receiver to sender | Initial explicit rate and join gate |
| `GRANT_UPDATE` | receiver to sender | Marked-DATA ACK or re-DECLARE reply |
| `GAP_NACK` | receiver to sender | Request one exact missing logical packet |
| `GAP_RESOLVED` | receiver to sender | Close one previously requested retry range |
| `RETIRE` | sender to receiver | Publish the final ledger and end membership |

DATA is low priority. All controls are high priority. Packet metadata is
immutable after construction, and every packet has exactly one terminal
lifecycle observation: endpoint consumption or fabric drop.

## Ring-CAM and deterministic recovery

The sender stamps each DATA packet with:

```text
ETA = source serialization end + calibrated no-queue transit
```

The calibration includes the physical path after the source serializer and
depends on the packet's exact wire extent. The runtime currently requires a
two-tier `ns-tm3` Clos.

Ring-CAM admits a packet when its observed arrival lies in:

```text
ETA <= arrival <= ETA + D
```

Admitted packets are released in timestamp order and then use the one shared
receiver serializer. A packet arriving after `ETA + D`, or a detected
sequence gap, triggers an exact-range `GAP_NACK`. Retransmissions use the
same source grant and PRBS opportunity as fresh DATA. Duplicate NACKs,
duplicate DATA, and reordered closure packets are idempotent. Retry attempts
and sender timeouts are bounded by configuration.

Completion fires only when the receiver's payload, wire-byte, and packet
ledgers all match the sender's final ledger. Membership is removed after
`RETIRE`, complete delivery, and recovery closure.

## Assumptions and non-goals

This endpoint requires:

- a homogeneous two-tier Clos using `ns-tm3`;
- endpoint capacity equal to the physical host-link rate;
- deterministic packet-specific no-queue transit calibration;
- positive control deadline, delay window, and retry bounds;
- enough Ring-CAM capacity for the configured bandwidth-delay envelope.

This PR intentionally excludes:

- ATLAHS CLI, runtime factory, workload driver, and executable wiring;
- CSV telemetry, plotting, traces, and experiment scripts;
- RNIC-SS, DCQCN, ns-rosetta endpoint integration, and Dragonfly routing;
- public paper profiles and manifest generation.

Those pieces can be reviewed after the endpoint protocol and its invariants are
accepted.

## Build and test

From the repository root:

```sh
cmake -S htsim/sim -B build -DENABLE_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R 'EventListMicrophase|RnicCollective|RnicSenderGrantGate'
```

The focused tests cover rate arithmetic, transactional membership,
two-window admission, independent incumbent updates, stale and reordered ACKs,
lease refresh, typed packet lifecycles, physical route calibration, Ring-CAM
delivery, deterministic recovery, retirement, and exact same-time ordering.
