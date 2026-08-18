# ss-dragonfly load discrimination: results

The scored state is **13 genuine-risk rows PASS, 0 FAIL, 0 VOID in
the evaluation of record, plus 8 registered rows whose PASS verdicts
are entailed by the genuine-risk rows and are therefore recorded but
not counted as independent evidence, plus 2 derived rows reported
unscored as registered**. The honest denominator is 13, not the 21
delivered verdicts: applying the freeze's own entailment rule
(recurring defect class 1), the four B-configuration CONTROL rows are
entailed by CT-4's byte identity of the exact artifacts they are read
from together with their A counterparts, DP-2 in both configurations
is entailed by DP-3's in-band first-drop instant, the DP-3 ordering
row is entailed by its two disjoint band rows, and DP-1 B is entailed
by DP-1 A under the freeze's registered open-loop identity argument
(review finding F2; an earlier version of this file headlined all 21
as scored). No registered row value, band, or direction changed at
any point; the corrections are confined to evaluation scaffolding and
framing prose and are recorded in the corrections sections. The first
runner invocation was VOIDED by a mis-registered guard and is
reported below, never scored. This is capability plus sanity evidence
for the HTSIM-29 and HTSIM-30 load harness; it is not a Merlin
calibration claim, and no statement here is about which buffer value
the Merlin fabric has.

## The headline, exactly as registered

The two configurations, identical except the shared buffer (A 4 MiB,
B 1 MiB), a pair the wave-19 evidence class could not separate:

- At the capture-shaped CONTROL load (two closed-loop flows to
  distinct destination ports with the measured endpoint floors as
  declared think times, each stack far below a port), A and B are
  **byte-identical**: bins CSV and chunk CSV equal byte for byte,
  stdout equal with the invocation's `topology=` token masked (CT-4).
- Under the saturating DISCRIMINATE load (the same two source stacks
  paced at 130 Gbit/s each into ONE destination port, 1.3x the
  egress), the same pair produces **different registered outcomes**:
  first drop at 560255151 ps (A) against 140891951 ps (B), both
  inside their registered bands with the registered ordering (DP-3),
  and 348 more drops in B than A, inside the registered [300, 400]
  band and exactly the disclosed 348-packet prediction (DP-4).

The fabric is load-bearing in the discriminate cell and only there.
The scope of the novelty claim, corrected by the adversarial review
(finding F1, recorded as correction 3): the separation itself does
not require the new capabilities. The legacy line-rate degree-2
incast also saturates one egress on these instances and separates the
pair with the same 348-packet drop difference, because the
excess-drop difference is the buffer delta over the wire size,
independent of the offered rate, and shared-port placement was always
expressible (both legacy patterns aim every flow at one receiver).
What the wave-19 evidence class could not do, and what the new
capabilities add, is expressing the capture-shaped regime at all:
sub-line-rate offered load, per-flow start and think-time
declarations, and distinct-destination-port placement. That is what
makes the CONTROL cell, and with it the registered
indistinguishable-then-separated pair of verdicts, expressible in one
pre-registered design; an earlier version of this file claimed the
separation was "carried entirely by the new capabilities", which was
wrong.

## Freeze integrity and chronology

| Step | Identity | Note |
|---|---|---|
| golden lock | commit `2d503cd` | legacy invocations byte-locked before any load code |
| freeze | commit `f205e46` | expectations and both instances committed before any load-harness code existed |
| harness | commit `3bb127c` | sources, CLI, fixtures (F7 to F12 first runs, all exact), runner; correction 1 recorded pre-run |
| run 1 | 2026-08-18 02:35 CEST, voided | conservation guard fired on the CONTROL cell; diagnosed as a guard arithmetic slip, correction 2 recorded |
| run 2 | 2026-08-18 02:37 CEST | identical cell parameters, corrected guard; every registered verdict delivered |
| run 3 (evaluation of record) | 2026-08-18 fix round | identical cell parameters under the review-amended checker (per-flow DP-1 clause evaluated, entailment classification, DP-6 looseness disclosure); every artifact SHA-256-identical to run 2's despite the rebuilt binary |

Chronology disclosures, no stronger than the evidence:

- The freeze predates the harness code, so no load-capable invocation
  of any kind preceded it. The fixtures F7 to F12 ran first (ctest,
  post-freeze) and every hand value was exact.
- Run 1's DISCRIMINATE rows already showed the scored run's values
  (the run log is retained: injected 17980/17980, dropped 3685/4033,
  first drops 560255151/140891951, difference 348); its CONTROL rows
  were never evaluated because the guard voided the cell first. Run
  1's CSV artifacts were overwritten in place by run 2 (the runner
  writes fixed file names), so byte-level run 1 evidence is the
  retained verdict log only; the equality of every manifest-derived
  number between the two logs is repeat-determinism evidence across
  invocations, and within run 2 every invocation's registered repeat
  guard (two runs, byte compare) held.
- Run 2 executed under binary SHA-256 `1a332ad8...`; run 3, the
  evaluation of record, under the fix-round rebuild `9324cfb3...`
  (behavior-neutral hardening only), with every primary artifact
  verified SHA-256-identical to run 2's before and after. Instance
  hashes A `c81298d1...`, B `4372520e...` (now also machine-enforced
  by the `SsDragonflyInstanceLock.*` ctest cases); the recorded
  expectations SHA-256 `c3d5ff55...` in run 3's `summary.json` ties
  to the corrections-1-and-2 state of the expectations file, because
  correction 1's F4 amendment and correction 3 are prose recorded
  after run 3 and change no evaluated quantity. Manifests live in
  `summary.json` in the bulk run directory
  `/data3/yifeng/simllm-dev/wave20-runs/w20a/discrimination/`.

## Fatal guards

Evaluation of record (run 3; artifacts byte-identical to run 2's):
none fired. All eight invocations (2 cells x 2 configs x 2 repeats)
exited 0 with quiescent fabrics; every repeat pair was
byte-identical; no per-flow bin exceeded the single-port quantization
bound 2487544 B and no cell aggregate exceeded its port-count bound.

Run 1: FG-conservation fired on CONTROL A and CONTROL B under the
frozen single-port-bound-per-cell wording and voided both; correction
2 records why that wording was arithmetically wrong for a
two-destination-port cell (worst observed bin 4957192 B = 554 * 8948,
exactly two full ports; per-flow worst 2478596 B = 277 packets, under
the per-port bound everywhere). The fabric conserves port by port;
the guard, not the fabric, was wrong.

## Rows: 13 genuine-risk, 8 entailed, 2 derived

CONTROL (values shown once; every B-configuration verdict is entailed
by CT-4 plus its A counterpart and counts as recorded, not as
independent evidence):

| Row | Registered | Observed | Verdict |
|---|---|---|---|
| CT-1 A exact accounting | injected = delivered = 203546, dropped = 0 | 203546, 203546, 0 | PASS |
| CT-2 A chunk counts | flow 0 = 91, flow 1 = 126 | 91, 126 | PASS |
| CT-3 A flow 0 cadence | chunk 0 at 340417280 ps, all 90 deltas exactly 2198644280 ps | exact | PASS |
| CT-3 A flow 1 cadence | chunk 0 at 340417280 ps, all 125 deltas exactly 1596855280 ps | exact | PASS |
| CT-4 A/B identity | bins CSV, chunk CSV byte-identical; stdout identical with `topology=` masked (correction 1 as amended) | identical | PASS |
| CT-1/2/3 B (4 rows) | same values as A | same | PASS, entailed |

DISCRIMINATE (DP-1 B, DP-2 A and B, and the DP-3 ordering row are
entailed as classified in the headline):

| Row | Registered | Observed A | Observed B | Verdict |
|---|---|---|---|---|
| DP-1 A pacing oracle | injected = 17980, 8990 per flow | 17980, {8990, 8990} | | PASS |
| DP-1 B pacing oracle | identical to A (open-loop argument) | | 17980, {8990, 8990} | PASS, entailed |
| DP-2 drops occur | dropped > 0 | 3685 | 4033 | PASS, entailed by DP-3 |
| DP-3 first-drop bands | A in [500, 620] us, B in [125, 155] us | 560255151 ps | 140891951 ps | PASS (2 rows) |
| DP-3 ordering | B < A | | | PASS, entailed by the disjoint bands |
| DP-4 drop difference | dropped(B) - dropped(A) in [300, 400], prediction 348 | 348 | | PASS |
| DP-5 saturation band | every steady bin aggregate in [2450355, 2487544] B | all 49 bins | all 49 bins | PASS |
| DP-6 fairness band | per-flow share in [0.4, 0.6] every steady bin | holds | holds | PASS, near-unfailable as registered |

DP-1's per-flow clause (8990 per flow) went unevaluated in the run-2
checker, which compared only the fabric-wide total (review finding
F5); the evaluation of record checks both halves from the per-flow
manifest lines.

DP-6 looseness disclosure (review finding F3): the frozen [0.4, 0.6]
band is about 55 times looser than the round-robin mechanism it
registers. The worst observed distance from an even split across all
196 evaluated flow-bins in both configurations is 0.0018, so the row
as registered could realistically only have failed under a gross
arbitration defect; its PASS is weak evidence by construction. The
frozen band is disclosed, not retuned.

Derived rows, reported and never scored, per the freeze's entailment
listing: delivered(A) - delivered(B) = 348 = dropped(B) - dropped(A)
(conservation plus DP-1), and the discriminate bins CSVs differ
between A and B (entailed by DP-3/DP-4 plus conservation).

## Physical sanity

The napkin models disclosed at the freeze land almost exactly: net
buffer fill at 7.5 wire bytes per ns predicts first drops near
559.24 us plus startup for A and 139.81 us for B; observed 560.255
and 140.892 us, within 0.8 percent of prediction with the startup of
order the 1.7 us pipeline-and-propagation entry. The drop-count
difference equals the fill-time-difference arithmetic exactly (348).
The saturated egress delivers 276 or 277 packets per 100-us bin in
every steady bin (serialization-exact), and the request/grant round
robin holds both flows within [0.4, 0.6] of every steady bin under
the declared half-gap stagger. The closed-loop control reproduces,
over 91 and 126 chunks, cadences equal to the two wave-19 measured
per-pair p50s by construction of the think-time seam; that is the
seam working as declared, not calibration.

## What this closes and what it does not

Closed here (backend side): the HTSIM-29 capability (declared-rate
paced and per-chunk closed-loop sources with the endpoint think-time
seam) and the HTSIM-30 capability (distinct-destination-port
multi-flow cells in one invocation, including the captured i2 shape
and a single-switch join schedule), with deterministic fixtures and
this pre-registered demonstration holding, in one registered design,
the capture-shaped regime where the buffer pair is indistinguishable
and a load-bearing regime where it separates (the corrected scope of
the novelty claim is in the headline section and correction 3).
Not established: any Merlin calibration statement (no measured Merlin
quantity is compared against a simulated one here), fabric-model
discrimination at the captured loads (the CONTROL cell reconfirms its
absence), shared-port capture families (uncaptured, TRAF-52), and
endpoint dynamics (TRAF-53). Registry closure of HTSIM-29/HTSIM-30
happens in a paired simllm change per the standing convention, not in
this repo.

## Post-specified corrections (adversarial review round)

Recorded after the review of the published record. No registered row
value, band, or direction changed; the registered artifacts of the
evaluation of record are byte-identical to run 2's.

1. F1, the novelty over-claim: an earlier version of this file and
   two frozen prose passages claimed the separation was carried
   entirely by the new capabilities and that the old harness could
   not make the fabric bind. Disproved by the review's
   counter-demonstration (legacy line-rate degree-2 incast separates
   the same pair with the same rate-independent 348-packet drop
   difference); corrected in the headline section here and recorded
   as correction 3 in expectations.md. The genuinely new
   capabilities: sub-line-rate pacing, per-flow start and think-time
   declarations, distinct-destination-port placement.
2. F2, the tally: the 21-row headline counted 8 rows whose verdicts
   are entailed by other rows, against the freeze's own entailment
   rule. The published denominator is now 13 genuine-risk rows, with
   the 8 entailed rows recorded as delivered-but-derived; the runner
   prints and stores the same classification.
3. F3, DP-6 looseness: disclosed above; the frozen band stays.
4. F4, correction 1's scope: the CT-4 stdout comparison normalizes
   all whitespace, not only the masked token; correction 1 now states
   what the code does.
5. F5, DP-1's per-flow clause: previously unevaluated; the
   evaluation of record evaluates it (8990 per flow, both
   configurations, PASS).

## Reproduction

```bash
cmake -S htsim/sim -B <build-dir> -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON
cmake --build <build-dir>
ctest --test-dir <build-dir>
python3 experiments/ss_dragonfly_load_discrimination/run_discrimination.py \
    --binary <build-dir>/datacenter/htsim_ss_dragonfly \
    --out-dir <bulk-dir>
```

Determinism makes the reproduction exact: identical binaries produce
byte-identical CSVs, manifests, and verdicts.
