# The ss-dragonfly load harness (HTSIM-29, HTSIM-30)

Status: capability plus sanity evidence. The rate-controlled and
closed-loop sources and the multi-receiver cells below close the two
harness gaps the wave-19 calibration study registered (HTSIM-29,
HTSIM-30 in the simllm backends registry): the open-loop sources
injected at exactly the host link rate, so host-stack-bound offered
load was inexpressible, and both shipped patterns sent every flow to
one receiver host, so the distinct-destination-port shape of every
captured Merlin incast and join family could not couple through one
switch in one invocation. Nothing here makes a Merlin calibration
claim; artifacts keep the `evidence=sanity-not-calibration` label.

Components:

| File | Role |
| --- | --- |
| `ss_dragonfly_load.{h,cpp}` | Paced and closed-loop sources, flow-spec parsing, chunk accounting, delivery/drop dispatch |
| `main_ss_dragonfly.cpp` | CLI seam: `-source`, `-flow`, `-offered_bps`, `-think_ps`, `-chunk_payload_bytes`, `-chunk_out` |
| `ss_dragonfly_load_test.cpp` | Pre-registered fixtures F7 to F12 plus mechanism and parsing checks |
| `tests/golden/ss_dragonfly/` | Byte-locked legacy invocations proving the default-off contract |
| `experiments/ss_dragonfly_load_discrimination/` | The registered discrimination demonstration |

## Pacing semantics (HTSIM-29, rate-controlled arm)

A paced source offers load at a declared rate R below or at the host
link rate. Packet k enters the host injection queue at

```text
t_k = start_ps + ceil(k * wire_bytes * 8 * 10^12 / R)
```

with the ceiling taken over the total elapsed product in 128-bit
integer arithmetic, so the schedule has no cumulative rounding drift:
whenever the product divides exactly, t_k is exact (fixture F8 pins
the difference against a fixed-increment scheduler, which would
already be two picoseconds late by the third packet at 30 Gbit/s).
R is configuration, never a fitted constant; rates above the host
link rate are rejected so the host queue never accumulates and the
declared rate is the offered rate on the wire.

## Closed-loop semantics (HTSIM-29, closed-loop greedy arm)

A closed-loop source injects one chunk of
`N = ceil(chunk_payload_bytes / (wire_bytes - header_bytes))` packets
greedily at line-rate serialization spacing, then waits for the
chunk's completion (the delivery instant of its last-delivered
packet) plus a declared think time before starting the next chunk:

```text
inject(chunk c+1, first packet) = completion(chunk c) + think_ps
```

No injection happens at or after `duration_ps`; a chunk whose
injection the stop time truncates never completes. A physical drop on
a closed-loop flow is a fatal harness error: no retransmission is
modeled, so a drop would stall the loop silently, and the harness
fails loudly instead (paced cells are the tool for drop regimes).

## The think-time seam for the measured endpoint floor

The think time is exactly where the measured per-chunk endpoint
host-stack floor from the Merlin capture dataset plugs in, as
configuration. The wave-19 study separated every captured pair's
chunk life into a fabric term and an endpoint floor
`H_time = measured p50 - T_fab_lat`; feeding that floor back as
`think_ps` makes the closed-loop cadence

```text
period = F + think_ps
```

with F the greedy chunk service time of the instance, which
reproduces the pair's measured per-chunk p50 by construction of the
anchor arithmetic (fixtures F9 and F10 pin 1596855280 ps and
2198644280 ps for the two anchors on the declared Merlin-shaped
instance; the equality with the measured p50s is the seam working as
declared, not a calibration result). The floors stay configuration:
different captures, or the TRAF-53 endpoint-dynamics work, can
replace them without touching this repo.

## Multi-receiver cells (HTSIM-30)

`-pattern explicit` takes repeatable flow declarations

```text
-flow src=H,dst=H[,start_ps=PS][,offered_bps=BPS][,think_ps=PS]
```

with pairwise-distinct (src, dst) pairs and per-flow overrides of the
invocation globals. This expresses, in one invocation through one
switch: the two-source distinct-destination-port incast shape of the
captured i2 family (fixture F10), a staggered join on the
single-switch instance via per-flow start times, which the shipped
join pattern structurally rejects there (fixture F11), and shared-
egress cells where the fabric is deliberately made load-bearing (the
discrimination experiment). The shipped incast and join patterns are
untouched; explicit cells are a third pattern, not a change to them.

## Chunk accounting

With `-chunk_payload_bytes B`, chunk c covers packet sequences
[c*N, (c+1)*N) and completes when all N are delivered, whatever the
delivery order (multi-switch adaptive routing may reorder).
`-chunk_out FILE` writes `flow_id,chunk_index,first_injection_ps,
completion_ps` rows sorted by flow then chunk. The load manifest
lines report per-flow injected packets, completed chunks, and the
first physical drop instant (`first_drop_ps`), which the fabric now
exposes through an optional drop observer.

## Determinism and the default-off contract

The sources add no randomness: pacing and chunk gating are integer
picosecond arithmetic over declared configuration, and per-packet
route hashes keep the fabric's seeded splitmix64 rule. Identical
invocations produce byte-identical CSVs and manifests (the
discrimination experiment's repeat guard enforces this per run).

Every new capability defaults off: an invocation without the new
options takes exactly the legacy code path, and the
`SsDragonflyFrozen.*` ctest cases replay three wave-18-era command
lines against golden captures produced by the pre-load-harness
binary, so any byte drift of a legacy invocation fails the suite.

## What remains for an rnic-ss closed-loop composition

This wave deliberately does not touch rnic-ss behavior. The
closed-loop source is an endpoint abstraction local to the sanity
harness: think time is a constant per flow, there is no
retransmission, no congestion response, and no endpoint-pair
outstanding tracking. A future rnic-ss composition over this fabric
would replace the constant think time with the endpoint's own
dynamics (the TRAF-53 term: convergence transients, source-identity
asymmetry, burst-versus-sustained variability), carry real
retransmission so drop regimes need not be fatal, and route the
rnic-ss endpoint-pair backpressure through the same delivery
feedback seam (`SsDragonflyLoadDispatch` is the single place where
deliveries return to sources today). Until that lands, the rnic-ss
endpoint's hosting and validity claims are unchanged by this wave.

## Evidence map

- Pre-registered fixtures with exact hand-derived values: F7 to F12
  in `docs/ss-dragonfly-fabric/fixture-expectations.md` (load-harness
  section), asserted by `ss_dragonfly_load_test.cpp`; all six passed
  on their first run with zero corrections.
- Byte-identity of legacy invocations: `SsDragonflyFrozen.*` golden
  tests.
- The registered discrimination demonstration:
  `experiments/ss_dragonfly_load_discrimination/` (expectations
  frozen before the harness existed; results in that directory's
  RESULTS.md).
