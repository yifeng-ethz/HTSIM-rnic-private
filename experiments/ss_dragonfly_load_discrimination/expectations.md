# ss-dragonfly load discrimination: pre-registered expectations

Written and frozen before the load-harness code existed and before
the first run of any cell below. This experiment is the registered
discrimination demonstration for the HTSIM-29 and HTSIM-30 load
harness: one pre-registered setting in which the fabric is
load-bearing and two fabric configurations that the wave-19
calibration evidence could not distinguish produce different
registered outcomes. It is capability plus sanity evidence for the
harness; it is not a Merlin calibration claim, and every artifact
carries `evidence=sanity-not-calibration`.

Context being closed, stated precisely: the wave-19 study (simllm
examples/merlin_ss_fabric_calibration_v1, RESULTS.md discrimination
statement) established that at the captured Merlin loads, each stack
under a fifth of a port, any fabric model above roughly 5 GB/s per
port yields identical verdicts; in particular the shared-buffer size
is invisible there (solo-cell occupancy stays at the in-service
packet scale). The harness could not express sub-line-rate offered
load or distinct-receiver-port multi-flow cells, so no cell could
make the fabric bind. This experiment uses exactly the new
capabilities to (a) reproduce that indistinguishability as a control
and (b) exhibit the discrimination the old harness could not.

## Configurations and instances

Two committed instances, identical except the shared buffer:

| Configuration | File | Shared buffer |
| --- | --- | --- |
| A | `merlin_shape_buffer4mib.topo` | 4194304 B |
| B | `merlin_shape_buffer1mib.topo` | 1048576 B |

Merlin shape p = 20, one Rosetta-class switch, 200 Gbit/s host
links, 300000 ps host propagation, 350000 ps pipeline; every scored
invocation passes `-wire_bytes 9038 -header_bytes 90` (the wave-19
capture framing: payload 8948 B per packet, ser = 361520 ps at line
rate). Derived constants, hand arithmetic (all verified against the
wave-19 EX oracles' stage sums):

- payload ceiling per port `C_p = 200e9 * 8948 / 9038` bits per
  second = 24751051117.5 B/s, i.e. 2475105.5 B per 100 us bin;
- 100-us bin packet quantization bound: at most
  ceil(100000000 / 361520) + 1 = 278 packets = 2487544 B per bin;
- uncontended stage sum D = 1673040 ps; greedy 8 MiB chunk service
  F = 340417280 ps; chunk packets N = 938.

## Cells

Both cells run under both configurations, every invocation twice
(repeat determinism guard), sequentially, with the binary built from
this branch. Common flags:
`-pattern explicit -routing adaptive -wire_bytes 9038
-header_bytes 90 -bin_ps 100000000`.

### Cell CONTROL, capture-shaped load, fabric not load-bearing

The i2-mirror shape: two closed-loop flows from distinct source
nodes to distinct destination ports of gpu102, with the measured
per-chunk endpoint floors from the wave-19 anchors as the declared
think times (configuration, not fit):

```text
-source closed -chunk_payload_bytes 8388608
-flow src=8,dst=5,think_ps=1858227000
-flow src=16,dst=6,think_ps=1256438000
-duration_ps 200000000000 -chunk_out <file>
```

Offered load per flow is one chunk per (F + T) period:
flow 0 period 2198644280 ps, flow 1 period 1596855280 ps, i.e.
about 3.8 and 5.3 GB/s against a 24.75 GB/s port; the fabric is not
the binding stage, exactly the captured regime.

### Cell DISCRIMINATE, saturating shared egress, fabric load-bearing

The same two source stacks paced at a declared sub-line rate but
aimed at ONE destination port, so the offered aggregate
2 * 130 Gbit/s = 1.3x the 200 Gbit/s egress makes the fabric bind:

```text
-source paced -offered_bps 130000000000
-flow src=8,dst=5
-flow src=16,dst=5,start_ps=278092
-duration_ps 5000000000
```

The 278092 ps stagger on flow 1 is half the pacing gap
(gap = 7230400/13 = 556184.6 ps), declared so the two paced streams
interleave instead of colliding on identical event timestamps; the
wave-18/19 record already analyzed synchronized open-loop overload as
its own artifact regime and this cell is not that regime.

No chunk accounting in this cell: under sustained drops chunks would
never complete, so the chunk observable is registered as
inapplicable here.

## Registered rows

Verdict classes: exact (integer equality), signed (registered
inequality or band with its direction stated before any run).
Derived-not-scored rows are listed so their entailment is on the
record before running (recurring defect class 1: entailed rows are
never scored as independent evidence).

### CONTROL rows

- CT-1 exact, per configuration: injected = delivered = 203546,
  dropped = 0. Hand derivation: flow 0 starts chunks at
  c * 2198644280 < 2e11 for c = 0..90 (91 chunks, last fully
  injected by 198216729440 ps), flow 1 at c * 1596855280 < 2e11 for
  c = 0..125 (126 chunks, last fully injected by 199945654240 ps);
  (91 + 126) * 938 = 203546.
- CT-2 exact, per configuration: chunks completed, flow 0 = 91,
  flow 1 = 126.
- CT-3 exact, per configuration: flow 0 chunk 0 completes at
  340417280 ps and every consecutive completion delta is exactly
  2198644280 ps (90 deltas); flow 1 chunk 0 at 340417280 ps and
  every delta exactly 1596855280 ps (125 deltas). This is the
  no-cross-flow-interference premise at scale, over 200 ms and both
  flows, not just the three-chunk fixture window.
- CT-4 identity, the control of the discrimination claim:
  configuration A and configuration B produce byte-identical bins
  CSV, chunk CSV, and stdout for this cell. The buffer knob is
  invisible at capture-shaped load.
- Derived-not-scored: last completions flow 0 at 198218402480 ps,
  flow 1 at 199947327280 ps (entailed by CT-2 plus CT-3).

### DISCRIMINATE rows

- DP-1 exact, per configuration: injected = 17980 (8990 per flow;
  flow 0 injects at ceil(k * 7230400 / 13) < 5e9 for k = 0..8989,
  t_8989 = 4999543508; flow 1 at 278092 + the same schedule,
  t_8989 = 4999821600; both k = 8990 values cross 5e9). Identical
  in A and B: pacing is open loop, drops do not gate injection.
- DP-2 signed, per configuration: dropped > 0 (offered 1.3x egress).
- DP-3 signed, the first discrimination row: first_drop_ps(B) <
  first_drop_ps(A), with bands
  first_drop_ps(A) in [500000000, 620000000] and
  first_drop_ps(B) in [125000000, 155000000].
  Point predictions, disclosed: net buffer fill rate is
  2 * 16.25 - 25 = 7.5 B/ns of wire bytes, so about
  4194304 / 7.5 = 559240 ns (A) and 1048576 / 7.5 = 139810 ns (B)
  plus a startup of order 2 us.
- DP-4 signed, the second discrimination row:
  dropped(B) - dropped(A) in [300, 400] packets, positive direction
  registered. Point prediction, disclosed: the excess-drop window is
  the fill-time difference 419430 ns at drop rate 7.5 / 9038
  packets per ns, about 348 packets; end effects cancel because both
  configurations share the identical injection schedule and the same
  post-fill steady state.
- DP-5 signed, per configuration, saturation without collapse: every
  bin with bin_start >= 100000000 and bin_end <= 5000000000 has
  aggregate delivered payload in [2450355, 2487544] B (0.99 * C_p
  per bin up to the 278-packet quantization bound; the egress is
  continuously busy from the first packets because the offered rate
  exceeds the service rate from t = 0).
- DP-6 signed, per configuration, same bins: each flow's delivered
  payload lies in [0.4, 0.6] of the bin aggregate (the switch grants
  round-robin over the two ingress VoQs of the shared egress).
- Derived-not-scored: delivered(A) - delivered(B) =
  dropped(B) - dropped(A) (conservation plus DP-1); the bins CSVs of
  A and B differ (entailed by DP-3/DP-4 plus conservation: different
  drop counts force different delivered totals, hence at least one
  differing bin row).

### The registered discrimination statement

If CT-4 passes (A and B indistinguishable at capture-shaped load)
and DP-3 and DP-4 pass (A and B produce different registered
outcomes under the saturating cell), then the demonstration holds:
the same configuration pair that the wave-19 evidence class could
not separate is separated by a cell in which the fabric is
load-bearing, and the separation is carried by the new offered-load
and multi-receiver capabilities (sub-line-rate pacing, explicit
distinct-port and shared-port flow placement). Nothing here claims
which buffer value Merlin has; that would need fabric-side
measurements the capture does not contain.

## Fatal guards, void and never scored

Any violation voids the affected run rather than scoring it: nonzero
harness exit; quiescence failure at drain; delivered = 0; a
conservation violation (any bin, either cell, above the 2487544 B
aggregate quantization bound); a repeat-determinism mismatch (any
invocation not byte-identical across its two runs). Voided runs are
reported as voided with the guard named; they are never silently
rerun with different parameters.

## Observables and their sources

Per-flow per-bin delivered payload from the bins CSV; injected,
delivered, dropped from the manifest; first_drop_ps from the load
manifest line the harness prints when load capabilities are active
(registered here as part of the harness contract); chunk completions
from the chunk CSV. Bulk outputs live under
`/data3/yifeng/simllm-dev/wave20-runs/w20a/` and are never
committed; the run script records binary and topology SHA-256 per
run.

## Chronology disclosures

The wave-19 anchors (think times 1858227000 and 1256438000 ps, the
framing, C_p, F, and the quantization bound) are published,
byte-locked measured quantities and derived constants; nothing about
them is blind here. The napkin fill-time and drop-count models above
are disclosed predictions computed before any run. The genuinely
blind quantities are the simulated outcomes of every row; no cell of
this experiment, and no invocation of the load-capable harness at
all, has run before this freeze (the harness does not exist at
freeze time). The three-chunk fixture analogs of CT-3's formulas
(F9 to F11 in docs/ss-dragonfly-fabric/fixture-expectations.md) are
registered in the same change and will run first as ctest fixtures;
CT-3 extends them to 91 and 126 chunks over 200 ms, which no fixture
covers.

## Corrections

Any correction is recorded here with what changed and why, never
silently.

1. CT-4 stdout comparison scope (recorded before the first run of any
   cell, found by desk-checking the runner against the harness's
   manifest format). The registered CT-4 text demands byte-identical
   stdout between configurations A and B, but the harness's first
   manifest line echoes the invocation's topology file path
   (`topology=...`), and the two configurations are by construction
   different files, so a literal stdout comparison would fail on the
   invocation-identity token alone and carry no information about the
   buffer knob's visibility. CT-4's stdout clause is therefore
   evaluated with the manifest's `topology=` token masked; the bins
   CSV and chunk CSV comparisons stay literal byte identity. No
   registered numeric value, band, or direction changed.
   Amendment (adversarial review round, finding F4): the implemented
   comparison is wider than the sentence above states. It tokenizes
   stdout on whitespace and rejoins with single spaces, so newline and
   spacing structure is normalized as well as the token being masked;
   the masked token is the only content difference the verdict
   tolerates, but byte-level line structure is not compared. This
   amendment states what the code does; the code was not changed.
2. Conservation-guard bound for the multi-receiver cell (recorded
   after the first runner invocation, which the guard voided; the
   fabric is correct and the registered guard arithmetic was wrong).
   The frozen guard applied the single-port 100-us quantization bound
   (278 packets, 2487544 B) to "any bin, either cell", but the
   CONTROL cell delivers to TWO distinct destination ports, and
   closed-loop chunks arrive as line-rate bursts (938 packets over
   about 339 us), so bins where the two flows' bursts overlap
   legitimately carry up to two ports' worth of packets. The voided
   first run's worst bin held 4957192 B = 554 * 8948, exactly two
   full ports, and per flow (per destination port) no bin anywhere
   exceeded 2478596 B = 277 packets, so serialization conservation
   holds port by port and the registered bound mis-scoped a per-port
   ceiling to a two-port aggregate. Corrected guard, the physically
   derived form: every flow's per-bin delivered payload is bounded by
   its destination port's quantization bound (2487544 B), and a
   cell's per-bin aggregate is bounded by that bound times its number
   of distinct destination ports (CONTROL 4975088 B, DISCRIMINATE
   unchanged at 2487544 B because both flows share one port). No
   registered row value, band, or direction changed; the voided first
   invocation is reported in RESULTS.md and the cells were rerun with
   identical parameters under the corrected guard (the wave-18 F6
   correction is the precedent for this handling).
3. Old-harness over-claim in two frozen prose passages (recorded in
   the adversarial review round, finding F1; the registered rows,
   bands, and directions are untouched and this corrects framing
   prose only). The introduction's sentence "The harness could not
   express sub-line-rate offered load or distinct-receiver-port
   multi-flow cells, so no cell could make the fabric bind" and the
   discrimination statement's clause "the separation is carried by
   the new offered-load and multi-receiver capabilities
   (sub-line-rate pacing, explicit distinct-port and shared-port flow
   placement)" both overstate. Disproved by the review's
   counter-demonstration: the LEGACY line-rate degree-2 incast (both
   flows to one receiver, a shape and load the pre-load-harness
   binary always expressed; the wave-18 F6 flood is exactly that
   regime) run against these two instances also saturates the shared
   egress and separates them with the same 348-packet drop
   difference, because the excess-drop difference is the buffer delta
   over the wire size (3145728 / 9038 = 348.05), independent of the
   offered rate. So the old harness could make the fabric bind and
   could separate this pair, and shared-port placement is not new
   (both legacy patterns aim every flow at one receiver). The
   corrected claims: (a) the pair is inseparable by the WAVE-19
   EVIDENCE CLASS, that is, at capture-shaped loads, which the old
   harness could not express; (b) the genuinely new capabilities are
   sub-line-rate pacing, per-flow start and think-time declarations,
   and DISTINCT-destination-port placement; (c) what the
   demonstration shows is one registered design expressing both the
   capture-shaped regime (where the pair is indistinguishable, the
   control) and a load-bearing regime (where it separates), with the
   control cell expressible only through the new capabilities.
