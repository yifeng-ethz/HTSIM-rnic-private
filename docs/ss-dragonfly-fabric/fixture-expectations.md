# ss-dragonfly deterministic fixture expectations

Pre-registered before the first run of each fixture, per the standing
validation discipline: every exact value below was derived by hand from
the canonical topology construction, the progressive table structure of
US 9137143 B2, and the configured physical constants, not by running the
fixture and copying its output. If a fixture disagrees with a value
here, the disagreement is investigated first; a correction to this file
is allowed only when the code is shown correct and the hand derivation
misread the canonical tables, and any such correction must be recorded
in the "corrections" section at the bottom, never silently.

## Fabric under test

`topologies/ss_dragonfly/p2a2h1g3_200g.topo`: p = 2, a = 2, h = 1,
g = 3; routers r0..r5; groups {r0,r1}, {r2,r3}, {r4,r5}; host H sits on
router H/2. Router port map: port 0 = local link to the group sibling,
port 1 = global link. Global cables from the canonical construction
(local index L targets group (G + L + 1) mod 3):

```text
r0 -- r3   (group 0 to group 1)
r1 -- r4   (group 0 to group 2)
r2 -- r5   (group 1 to group 2)
```

Physical constants: every link class 200 Gbit/s; wire packet 4160 B
(payload 4096 B), so one serialization is exactly 166400 ps; host and
local propagation 300000 ps; global propagation 1000000 ps; switch
pipeline 350000 ps; shared buffer 4 MiB; near/far quantizer step
8320 B; downstream step 33280 B; advertisement period 10000000 ps with
delay 1000000 ps; maximum downstream age 40000000 ps (added by
correction 3 below; the F6 verdict depends on it).

An uncontended stage-by-stage delivery time is the sum of: host queue
serialization, host propagation, then per switch (pipeline, egress
serialization, link propagation), with the request/grant match adding
zero time on an idle switch.

## F1: same-router delivery

host0 to host1 (both on r0), one packet injected at t = 0, Adaptive0.

- Delivered at exactly 166400 + 300000 + 350000 + 166400 + 300000
  = 1282800 ps.
- router_hops 0, global_hops 0.
- route_class Undecided (delivery at the injection router selects no
  candidate, so no class is ever assigned).

## F2: same-group delivery

host0 (r0) to host2 (r1), one packet at t = 0, Adaptive0.

- Decision sequence: (r0, output port 0), then delivered at r1.
- route_class Minimal, router_hops 1, global_hops 0.
- Delivered at exactly 2099200 ps
  (166400 + 300000 + 350000 + 166400 + 300000 + 350000 + 166400
  + 300000).

## F3: minimal inter-group, connector router

host0 (r0) to host6 (r3), one packet at t = 0, Adaptive0.

- Decision sequence: (r0, output port 1), then delivered at r3.
- route_class Minimal, router_hops 1, global_hops 1.
- Delivered at exactly 2799200 ps
  (166400 + 300000 + 350000 + 166400 + 1000000 + 350000 + 166400
  + 300000).

## F4: minimal inter-group with a target-group local hop

host0 (r0) to host4 (r2), one packet at t = 0, Adaptive0.

- Decision sequence: (r0, port 1), (r3, port 0), delivered at r2.
- route_class Minimal, router_hops 2, global_hops 1.
- Delivered at exactly 3615600 ps
  (166400 + 300000 + 350000 + 166400 + 1000000 + 350000 + 166400
  + 300000 + 350000 + 166400 + 300000).

## F5: deterministic non-minimal inter-group

host0 (r0) to host4 (r2), one packet at t = 0,
DeterministicNonMinimal. The only legal intermediate group from r0 is
group 2, reached through the sibling's global link.

- Decision sequence: (r0, port 0), (r1, port 1), (r4, port 0),
  (r5, port 1), delivered at r2.
- route_class NonMinimal, router_hops 4, global_hops 2.
- Delivered at exactly 5948400 ps
  (166400 + 300000 + 350000 + 166400 + 300000 + 350000 + 166400
  + 1000000 + 350000 + 166400 + 300000 + 350000 + 166400 + 1000000
  + 350000 + 166400 + 300000).

## F6: congestion response through the delayed advertisement

Background: host0 to host6 and host1 to host7 from t = 0 until
t = 30000000 ps, open loop at line rate, pinned to their minimal path
with DeterministicMinimal control. Both cross r0's global egress (2C
offered into C), so r0's waiting bytes grow at close to 25 bytes per ns
while the flood runs, and r0's switch-wide occupancy is what the
r1-to-r0 downstream advertisement will carry.

Probe A: one packet host2 (r1) to host4 (r2) at t = 5000000 ps,
Adaptive0. The first advertisement is emitted at the 10000000 ps tick
and consumed at 11000000 ps, so probe A decides with downstream = 0 on
every candidate, near-end 0 and far-end 0 at r1 (the flood does not
touch r1's egresses). Minimal score 0 beats the non-minimal score
(0 << 1) + 1 = 1:

- Probe A decision sequence: (r1, port 0), (r0, port 1), (r3, port 0),
  delivered at r2. route_class Minimal, router_hops 3, global_hops 1.
- Probe A is delayed behind the r0 global backlog but delivered before
  t = 20000000 ps, with zero drops.

Probe B: one identical packet at t = 20000000 ps. By then the
11000000 ps advertisement has installed r0's waiting bytes (about
230 KB at the 10000000 ps observation, well above two downstream
quantizer steps of 33280 B) as the downstream component of r1's
port 0. The minimal adjusted score is now at least 2, which loses to
the non-minimal score of 1:

- Probe B decision sequence: (r1, port 1), (r4, port 0), (r5, port 1),
  delivered at r2. route_class NonMinimal, router_hops 3,
  global_hops 2.
- Probe B is delivered in under 6000000 ps after its injection.

Whole-fixture accounting, running the event list to 70000000 ps (the
r0 global backlog peaks near 175 packets when the flood stops and needs
about 29000000 ps to drain):

- background packets injected: exactly 181 per flow (source ticks at
  k * 166400 ps for k = 0..180 are all below 30000000 ps), 362 in
  total, 364 with the probes;
- delivered = injected, dropped = 0 (peak r0 occupancy stays around
  750 KB, far below the 4 MiB shared buffer);
- advertisements consumed at least 12 (one full 12-directed-link round);
- the route-state store and every link transport are quiescent at the
  end.

Fatal guards (any one voids the fixture rather than scoring it): a
drop, probe A not Minimal, probe B not NonMinimal, a routing-core drop
decision, or a quiescence failure.

## Load-harness fixtures (HTSIM-29 and HTSIM-30)

Registered before any load-harness code was written or run, per the
same discipline as F1 to F6. These fixtures pin the rate-controlled
and closed-loop source semantics and the multi-receiver distinct-port
cells. Every value below is derived by hand from the declared source
semantics and the stage sums already verified by F1 to F6 and by the
wave-19 exact oracles (simllm study merlin_ss_fabric_calibration_v1,
EX-1 to EX-3). Capability and sanity evidence only; no calibration
claim.

### Registered source semantics

The load sources are configuration-driven; declared rates and think
times are configuration, never fitted constants. All arithmetic is
integer picoseconds; 128-bit intermediates where products can exceed
64 bits.

- Paced source at declared offered rate R (bits per second): packet k
  (k = 0, 1, ...) enters the host injection queue at
  `t_k = start_ps + ceil(k * wire_bytes * 8 * 10^12 / R)`.
  The ceiling is taken over the total elapsed product, not per
  interval, so the schedule carries no cumulative rounding drift:
  whenever `k * wire_bytes * 8 * 10^12` is divisible by R, `t_k` is
  exact. R must satisfy 0 < R <= host link rate. No injection at or
  after `duration_ps`.
- Closed-loop greedy source: a chunk is
  `N = ceil(chunk_payload_bytes / (wire_bytes - header_bytes))`
  packets. Chunk 0's first packet enters the queue at `start_ps`;
  within a chunk, packets follow at the line-rate serialization
  spacing `ser = ceil(wire_bytes * 8 * 10^12 / host_rate)` (greedy,
  exactly the open-loop spacing). Chunk c+1's first packet enters at
  `completion(chunk c) + think_ps`, where completion is the instant
  the last of the chunk's N packets is delivered. The declared
  think time is the seam where a measured per-chunk endpoint floor
  plugs in. No injection at or after `duration_ps`; a chunk whose
  injection was truncated by `duration_ps` never completes. A
  physical drop on a closed-loop flow is a fatal harness error: no
  retransmission is modeled, so a drop would silently stall the loop.
- Chunk accounting (paced or closed): chunk c covers packet sequences
  [c*N, (c+1)*N); it completes when all N are delivered, whatever the
  delivery order.
- Explicit multi-receiver cells: repeatable per-flow declarations
  `src, dst, start_ps, offered_bps, think_ps` with pairwise-distinct
  (src, dst) pairs; distinct destination ports are the registered
  HTSIM-30 shape.

### Load-fixture fabric

Merlin-shaped single-switch instance exactly as
`topologies/ss_dragonfly/merlin_a100_5node_4x200g.topo` (p = 20,
one router, host links 200 Gbit/s, host propagation 300000 ps,
switch pipeline 350000 ps, shared buffer 4 MiB) with the wave-19
capture framing: wire 9038 B, header 90 B, payload 8948 B.

Derived constants, hand arithmetic:

- `ser = 9038 * 8 * 10^12 / 200e9 = 361520 ps` (exact division).
- Uncontended stage sum ingress to delivery
  `D = ser + 300000 + 350000 + ser + 300000 = 1673040 ps`
  (the 9038-byte analog of F1's 1282800).
- Chunk 8 MiB: `N = ceil(8388608 / 8948) = 938`
  (937 * 8948 = 8384276 < 8388608 <= 938 * 8948 = 8393224).
- Greedy chunk service from first injection to completion on an idle
  fabric: `F = (N - 1) * ser + D = 937 * 361520 + 1673040
  = 340417280 ps`, which equals the wave-19 EX-1 hand value, as it
  must (same sum).

Host numbering for the i2-mirror cells (host = node * 4 + port over
gpu101..gpu105): host 8 = gpu103 port 0, host 16 = gpu105 port 0,
hosts 5 and 6 = gpu102 ports 1 and 2.

### F7: paced solo, exact chunk-completion sequence

host 16 to host 6, paced at R = 100e9 (half line rate), chunk 8 MiB,
start 0, duration 2100000000 ps, Adaptive0.

- `t_k = k * 723040` exactly (9038 * 8 * 10^12 / 100e9 = 723040, no
  remainder).
- Pacing gap 723040 >= ser, so every packet finds every stage idle:
  delivery `d_k = t_k + 1673040`.
- Chunk c completes at `d_(938(c+1) - 1)
  = (938(c+1) - 1) * 723040 + 1673040`:
  - chunk 0: 937 * 723040 + 1673040 = 677488480 + 1673040
    = 679161520 ps;
  - chunk 1: 1875 * 723040 + 1673040 = 1355700000 + 1673040
    = 1357373040 ps;
  - chunk 2: 2813 * 723040 + 1673040 = 2033911520 + 1673040
    = 2035584560 ps.
  - Consecutive deltas exactly 938 * 723040 = 678211520 ps.
- Injected packets: t_k < 2100000000 holds for k <= 2904
  (2904 * 723040 = 2099708160; 2905 * 723040 = 2100431200), so
  exactly 2905 injected, 2905 delivered, 0 dropped; chunk 3 (needs
  k through 3751) never completes: exactly 3 recorded completions.

### F8: paced non-divisible rate, exact ceiling schedule

host 16 to host 6, paced at R = 30e9, chunk 26844 B (exactly 3
packets), start 0, duration 15000000 ps, Adaptive0.

- Ideal spacing 9038 * 8 * 10^12 / 30e9 = 7230400 / 3
  = 2410133 and 1/3 ps, so the ceiling rule is observable:
  t_0..t_6 = 0, 2410134, 4820267, 7230400, 9640534, 12050667,
  14460800. Every third packet lands exactly on k * 7230400 / 3
  with no accumulated drift (a fixed-increment scheduler would put
  packet 3 at 3 * 2410134 = 7230402, two picoseconds late; this row
  discriminates the two rules).
- Deliveries `d_k = t_k + 1673040`: 1673040, 4083174, 6493307,
  8903440, 11313574, 13723707, 16133840.
- Chunk completions: chunk 0 at d_2 = 6493307 ps; chunk 1 at
  d_5 = 13723707 ps.
- Injected: t_6 = 14460800 < 15000000 <= t_7 = 16870934
  (ceil(50612800 / 3) = 16870934), so exactly 7 injected, 7
  delivered, 0 dropped, 2 completed chunks.

### F9: closed-loop solo, exact cadence under a declared think time

host 16 to host 6, closed loop, chunk 8 MiB, think
T = 1256438000 ps, start 0, duration 3600000000 ps, Adaptive0.

The declared T is the wave-19 measured per-chunk endpoint floor of
the gpu105 to gpu102 anchor (H_time = 1256.438 us), used here as
configuration to demonstrate the seam; the fixture would hold for any
declared T.

- Chunk c completes at `C_c = (c + 1) * F + c * T`:
  - chunk 0: 340417280 ps;
  - chunk 1: 680834560 + 1256438000 = 1937272560 ps;
  - chunk 2: 1021251840 + 2512876000 = 3534127840 ps.
- Chunk first injections: I_0 = 0; I_1 = C_0 + T = 1596855280 ps;
  I_2 = C_1 + T = 3193710560 ps.
- The completion period is F + T = 1596855280 ps. By construction of
  the wave-19 anchor arithmetic (H_time = measured p50 - F) this
  equals the measured solo steady-window p50 of that pair,
  1596855 ns; that is the seam working as declared, not a
  calibration result of this fixture.
- Chunk 3 would start at C_2 + T = 4790565840 >= duration, so it
  never starts: exactly 3 * 938 = 2814 injected, 2814 delivered,
  0 dropped, 3 completed chunks (chunk 2's last injection is at
  I_2 + 937 * ser = 3532454800 < duration).

### F10: two flows to distinct ports, exact independence (i2 mirror)

flow 0: host 8 to host 5, closed loop, T0 = 1858227000 ps (the
wave-19 gpu103 to gpu102 anchor floor); flow 1: host 16 to host 6,
closed loop, T1 = 1256438000 ps. Both chunk 8 MiB, start 0, duration
4750000000 ps, Adaptive0. Distinct source ports, distinct
destination ports, one switch: the never-exercised
no-cross-flow-interference premise, exercised.

- Registered independence: each flow's completion sequence equals its
  solo formula exactly, with zero cross-flow displacement:
  - flow 0 (period F + T0 = 2198644280 ps, which is the gpu103
    anchor's measured p50 by the same seam construction):
    340417280, 2539061560, 4737705840 ps;
  - flow 1: identical to F9: 340417280, 1937272560, 3534127840 ps.
- flow 0 chunk 3 would start at 4737705840 + 1858227000 >= duration;
  flow 1 chunk 3 at 4790565840 >= duration: neither starts. Exactly
  2814 + 2814 = 5628 injected, 5628 delivered, 0 dropped, 3
  completed chunks per flow.

### F11: staggered join on the single switch, exact join-unharmed

flow 0: host 8 to host 5, paced at 100e9, start 0; flow 1: host 16
to host 6, paced at 100e9, start 500000000 ps. Chunk 8 MiB, duration
2600000000 ps, Adaptive0. This is the join schedule the shipped join
pattern cannot express on a single-switch instance.

- flow 0 completions: exactly F7's values 679161520, 1357373040,
  2035584560 ps (the joiner displaces nothing: exact join-unharmed).
- flow 1 completions: F7's values shifted by its start:
  1179161520, 1857373040, 2535584560 ps.
- Injected: flow 0 exactly 3596 (k <= 3595: 3595 * 723040
  = 2599328800 < 2600000000 <= 3596 * 723040); flow 1 exactly 2905
  (same count as F7, shifted threshold); 0 dropped; 3 completed
  chunks per flow.

### F12: paced inter-group flow on the study fabric

Study fabric and framing of F1 to F6 (p2a2h1g3, 4160 B wire, 4096 B
payload, ser = 166400 ps). host 0 to host 6 (the F3 path through
r0's global egress), paced at R = 100e9, chunk 40960 B (exactly 10
packets), start 0, duration 6400000 ps, Adaptive0.

- `t_k = k * 332800` exactly; with zero congestion every decision is
  Minimal (F3's score arithmetic), router_hops 1, global_hops 1 on
  every delivery.
- `d_k = t_k + 2799200` (F3's registered stage sum).
- Chunk completions: chunk 0 at 9 * 332800 + 2799200 = 5794400 ps;
  chunk 1 at 19 * 332800 + 2799200 = 9122400 ps (the second lands in
  the drain window, which is registered as allowed: completions may
  postdate duration_ps).
- Injected: exactly 20 (t_19 = 6323200 < 6400000 <= t_20 = 6656000);
  20 delivered, 0 dropped, 2 completed chunks.

## Corrections

1. F6 background routing control (recorded after the first F6 run,
   before any verdict was published). The registration described the
   background as "open loop at line rate" and its arithmetic assumed
   the flood stays on the r0 global egress, but did not name the
   background's routing control; the first fixture implementation used
   Adaptive0 for the background, and the adaptive background
   self-balanced away from the hotspot exactly as the mechanism should
   (its own minimal score exceeded the non-minimal bias within a few
   packets), so the registered backlog never formed and probe B stayed
   minimal. The fabric behavior is correct; the fixture now pins the
   background to DeterministicMinimal, which is the scenario the
   registered arithmetic described. No registered expected value was
   changed. The observed self-balancing is itself mechanism evidence
   and is noted in the results.
2. Fixture time origin (recorded with the first full-suite run). The
   registered times are offsets from each fixture's injection origin;
   the shared test event list never rewinds between fixtures, so the
   harness measures deliveries relative to its construction origin, and
   the fabric anchors its advertisement cadence at construction time
   rather than absolute zero. No registered expected value was
   changed. Clarification (adversarial review round): the
   construction-time anchoring sentence above describes a change
   introduced with the fixture commit 791ddfd, not a property the
   fabric already had; before that commit the first tick was scheduled
   at the absolute period.
3. Missing constant in the registered constants list (adversarial
   review round). The maximum downstream age was omitted from the
   constants paragraph and from the probe-B freshness derivation,
   which silently relied on it: at probe B the installed advertisement
   is 10000000 ps old, fresh only because the study fabric sets
   maximum_downstream_age_ps to 40000000 ps. At the fabric header
   default of 400000 ps, or the ported congestion core's internal
   default of 1000000 ps, the advertisement would already be expired
   and F6 would fail. The constant is now listed; the registered
   expected values themselves are unchanged, and the override is also
   disclosed in the design note's abstraction registry entry 5.
