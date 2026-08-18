# ss-dragonfly load discrimination: results

The scored state is **all 21 scored rows PASS, 0 FAIL, 0 VOID in the
scored invocation, after one recorded correction to the evaluation
scaffolding (correction 2); the first runner invocation was VOIDED by
that mis-registered guard and is reported below, never scored**. No
registered row value, band, or direction changed at any point: the
registered-versus-corrected tally is 21 registered scored rows, 21
delivered verdicts on exactly the registered values and bands, plus 2
corrections confined to how the evaluator compares artifacts (guard
scope, stdout token), plus 2 derived rows reported unscored as
registered. This is capability plus sanity evidence for the HTSIM-29
and HTSIM-30 load harness; it is not a Merlin calibration claim, and
no statement here is about which buffer value the Merlin fabric has.

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

The fabric is load-bearing in the discriminate cell and only there,
which is precisely the condition the wave-19 study identified as
missing for fabric-model discrimination; the separation is carried
entirely by the new capabilities (declared sub-line-rate pacing,
explicit distinct-port and shared-port flow placement).

## Freeze integrity and chronology

| Step | Identity | Note |
|---|---|---|
| golden lock | commit `2d503cd` | legacy invocations byte-locked before any load code |
| freeze | commit `f205e46` | expectations and both instances committed before any load-harness code existed |
| harness | commit `3bb127c` | sources, CLI, fixtures (F7 to F12 first runs, all exact), runner; correction 1 recorded pre-run |
| run 1 | 2026-08-18 02:35 CEST, voided | conservation guard fired on the CONTROL cell; diagnosed as a guard arithmetic slip, correction 2 recorded |
| run 2 (scored) | 2026-08-18 02:37 CEST | identical cell parameters, corrected guard; all 21 scored rows pass |

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
- The scored run's manifest: binary SHA-256 `1a332ad8...`, instance
  hashes A `c81298d1...`, B `4372520e...`, corrected expectations
  SHA-256 `c3d5ff55...`, in `summary.json` in the bulk run directory
  `/data3/yifeng/simllm-dev/wave20-runs/w20a/discrimination/`.

## Fatal guards

Scored run: none fired. All eight invocations (2 cells x 2 configs x
2 repeats) exited 0 with quiescent fabrics; every repeat pair was
byte-identical; no per-flow bin exceeded the single-port quantization
bound 2487544 B and no cell aggregate exceeded its port-count bound.

Run 1: FG-conservation fired on CONTROL A and CONTROL B under the
frozen single-port-bound-per-cell wording and voided both; correction
2 records why that wording was arithmetically wrong for a
two-destination-port cell (worst observed bin 4957192 B = 554 * 8948,
exactly two full ports; per-flow worst 2478596 B = 277 packets, under
the per-port bound everywhere). The fabric conserves port by port;
the guard, not the fabric, was wrong.

## Scored rows

CONTROL (both configurations identical; values shown once):

| Row | Registered | Observed | Verdict |
|---|---|---|---|
| CT-1 exact accounting | injected = delivered = 203546, dropped = 0 | 203546, 203546, 0 | PASS (A and B) |
| CT-2 chunk counts | flow 0 = 91, flow 1 = 126 | 91, 126 | PASS (A and B) |
| CT-3 flow 0 cadence | chunk 0 at 340417280 ps, all 90 deltas exactly 2198644280 ps | exact | PASS (A and B) |
| CT-3 flow 1 cadence | chunk 0 at 340417280 ps, all 125 deltas exactly 1596855280 ps | exact | PASS (A and B) |
| CT-4 A/B identity | bins CSV, chunk CSV byte-identical; stdout identical with `topology=` masked (correction 1) | identical | PASS |

DISCRIMINATE:

| Row | Registered | Observed A | Observed B | Verdict |
|---|---|---|---|---|
| DP-1 pacing oracle | injected = 17980, identical in A and B | 17980 | 17980 | PASS |
| DP-2 drops occur | dropped > 0 | 3685 | 4033 | PASS |
| DP-3 first-drop bands | A in [500, 620] us, B in [125, 155] us, B < A | 560255151 ps | 140891951 ps | PASS |
| DP-4 drop difference | dropped(B) - dropped(A) in [300, 400], prediction 348 | 348 | | PASS |
| DP-5 saturation band | every steady bin aggregate in [2450355, 2487544] B | all 49 bins | all 49 bins | PASS |
| DP-6 fairness band | per-flow share in [0.4, 0.6] every steady bin | holds | holds | PASS |

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
this pre-registered demonstration that the capabilities make fabric
configurations discriminable exactly when the fabric is load-bearing.
Not established: any Merlin calibration statement (no measured Merlin
quantity is compared against a simulated one here), fabric-model
discrimination at the captured loads (the CONTROL cell reconfirms its
absence), shared-port capture families (uncaptured, TRAF-52), and
endpoint dynamics (TRAF-53). Registry closure of HTSIM-29/HTSIM-30
happens in a paired simllm change per the standing convention, not in
this repo.

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
