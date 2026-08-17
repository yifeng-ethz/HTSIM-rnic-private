# ss-dragonfly sanity studies: results against pre-registered expectations

Runs of 2026-08-17, one `run_sanity.py` invocation (51 check rows in
`summary.csv`; per-interval receiver goodput CSVs, repeat CSVs, and
manifests for every arm). Artifacts live outside Git in the bulk run
directory `/data3/yifeng/simllm-dev/wave18-runs/w18a/ss_dragonfly_sanity/`.
Binary: `htsim_ss_dragonfly` from the worktree build at the fixture
commit of branch codex/ss_dragonfly_fabric. The registered predictions
are in [expectations.md](expectations.md), committed before the first
run. Everything here is sanity evidence for the hosted fabric, not
calibration: `evidence=sanity-not-calibration`,
`calibration_status=hosted-pending-merlin-calibration`.

Verdict: **41 of 51 check rows pass, including every determinism guard
(byte-identical repeat invocations of all ten arms), every conservation
guard, saturation at exactly 196.92 Gbit/s (the C_p payload ceiling) in
every steady arm, the degree-1 line-rate check, the drops-onset checks,
and the endpoint-limit equality S1.5 at all four degrees. The 6 FAILs
are one phenomenon registered too optimistically: deterministic
admission-phase capture under open-loop overload. The 4 VOIDs plus one
vacuous PASS are a registration slip in the join-epoch bin arithmetic.
No FAIL is a fabric defect; each feeds the calibration wave.**

## Harness corrections before the scored run

Two harness defects were found and fixed between the first (voided)
invocation and the scored run; neither changed a registered band:

1. The default drain window was written as 50000000 ps (50 us) against
   the intended 50 ms; a saturated receiver-leaf buffer needs about
   171 us to drain, so overloaded arms failed their quiescence guard at
   the artificial cutoff. The default is now 50 ms. (The first
   diagnosis chased a phantom slow drain for some time; the fabric
   was draining at exact line rate throughout, one grant per 166400 ps.)
2. The CSV `goodput_bps` column overflowed uint64 for bins above about
   2.3 MB; it now uses 128-bit intermediate arithmetic. The
   `delivered_payload_bytes` column, which every registered check
   consumes, was always exact.

## S1 incast ladder

- S1.1 saturation: PASS at every degree and both routing arms; steady
  aggregate is 196.92 Gbit/s, the exact payload ceiling.
- S1.2 degree-1 line rate: PASS both arms (196.61 Gbit/s minimum
  steady bin, zero drops).
- S1.3 overload drops without collapse: PASS at every N >= 2 (for
  example 11008 drops at N = 2, 81101 at N = 8, aggregate goodput
  never leaving the band).
- S1.4 fairness/no-starvation: FAIL at N = 2, 4, 8 in both arms, and
  the failure is total starvation of specific flows, not mild skew
  (N = 2: flow 0 delivered 49.2 MB, flow 1 only 4.1 MB, all of it
  before the buffer first filled). Mechanism, from the drain traces:
  the fabric's fairness lives in the request/grant service arbitration
  (VoQ round robin), which shares perfectly while competing VoQs are
  occupied, but ADMISSION to the full shared buffer is first-arrival
  at the instant a grant frees space. With perfectly synchronized
  line-rate open-loop sources, the arrival phases are deterministic,
  one contender systematically wins the freed slot, the losers' VoQs
  drain to empty and never refill. The registered band assumed service
  fairness would surface as flow fairness; under open-loop overload it
  cannot. This is exactly the regime the real interconnect's
  per-endpoint-pair congestion control exists for (De Sensi et al.,
  SC'20, section II-D), and rnic-ss's pair-selective backpressure is
  the corresponding hosted mechanism, still Clos-only this wave. The
  FAILs stand as registered and become a calibration-wave input:
  fabric-level fairness claims need either endpoint control on the
  fabric or desynchronized offered load.
- S1.5 endpoint-limit equality: PASS at every degree; adaptive and
  minimal aggregates are identical to the reported precision. The
  hosted fabric echoes the published boundary that adaptive routing
  cannot relieve last-hop endpoint congestion.
- Route-class note: at N = 8 the adaptive arm delivered 2462 minimal,
  577 non-minimal, and 12020 undecided (same-router) packets with
  maximum router hops 6, so the non-minimal machinery is genuinely
  exercised under load. A separate labeling subtlety: under same-group
  congestion the ported core's self-root local-non-minimal entry (a
  zero-congestion root-detect candidate) wins against a loaded minimal
  entry and relabels physically identical one-hop paths as
  NonMinimal; no physical route changes. Recorded for the calibration
  wave's class-share metrics.

## S2 step-wise join

- Determinism, conservation: PASS.
- S2.1 staircase-top and the S2.2 epoch-share checks: VOID by
  registration slip. The registered epochs are 250 us at 100 us bins;
  the runner's settled window (from two bins after each join to the
  epoch end) therefore contains no complete bin, so the checks had an
  empty evaluation set. S2.2-flow0-steps-down "passed" vacuously over
  the same empty windows and must be read as VOID, not PASS.
- What the binned series itself shows (reported descriptively since
  the registered windows voided): flow 0 holds 196.9 Gbit/s alone;
  within one bin of the second flow joining the two settle at
  98.3/98.6 Gbit/s, an exact halving; the same clean halving repeats
  when flow 2 joins. Each junior flow is then starved out by the
  admission-phase capture within about 200 us (flow 1 fades by bin 5,
  flow 2 by bin 10, flow 3 never admits), after which flow 0
  re-monopolizes. The join transient the study was designed to show
  is present and crisp; the long-run shares repeat the S1.4 finding.
- S2.4 drops onset: PASS (zero drops in the single-flow epoch, 15916
  after).

## Standing conclusions

1. The fabric is deterministic end to end: all ten arms byte-identical
   on repeat invocation, and the whole run is reproducible from the
   committed seeds.
2. Goodput accounting is exact: saturation sits on the arithmetic
   payload ceiling and no bin ever exceeds it.
3. Grant/service arbitration is fair; open-loop overload admission is
   not, and deterministically so. Fabric-fairness statements must wait
   for endpoint congestion control over the dragonfly (calibration
   wave) or use desynchronized load.
4. Adaptive routing shifts real traffic non-minimally under load and
   buys nothing at a pure endpoint bottleneck, matching the public
   record's mechanism boundary.
