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
delay 1000000 ps.

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
   changed.
