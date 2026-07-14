# RNIC multi-baseline experiments

This experiment uses a 64-rank recursive-doubling all-reduce graph.  Each rank
exchanges one 32 MiB payload with a distinct XOR partner in each of six phases.
The message size follows the default `32M` payload in NVIDIA `nccl-tests`
`all_reduce_perf`; the selected recursive-doubling graph is an explicit open
simulation workload and is not presented as NCCL's proprietary runtime choice.

Two schedules contain exactly the same directed transfers and rank placement:

- `flat`: all 384 transfers are dependency-free and become eligible at time
  zero.  This is the per-flow FCT/tail experiment, not an all-reduce JCT claim.
- `dag`: phase `k+1` at each rank waits for both the send and receive of phase
  `k`.  The final GOAL completion is the collective JCT experiment.

Generate one paired seed and convert it to the packed GOAL format with:

```sh
python3 experiments/rnic_multibaseline/generate_allreduce_goal.py \
  --seed 1 --mode flat \
  --output /tmp/allreduce-flat-seed1.goal \
  --metadata /tmp/allreduce-flat-seed1.json

cmake --build /path/to/htsim-build --target htsim_goal_txt2bin
/path/to/htsim-build/htsim_goal_txt2bin \
  -i /tmp/allreduce-flat-seed1.goal \
  -o /tmp/allreduce-flat-seed1.bin
```

Use identical seeds, payloads, topology, link rates, propagation, packet extent,
and buffer parameters across all baselines.  A seed changes only the logical-rank
to physical-node permutation and each protocol's explicitly seeded stochastic
components.

## Statistical plot contract

For each protocol and seed, build an empirical FCT CDF from all directed flows.
Evaluate every seed CDF on one common FCT grid.  The presentation line is the
pointwise mean CDF and the shaded band is mean plus or minus one population
standard deviation, clipped to `[0, 1]`.  Report seed count and the independent
unit explicitly; flows within one seed share queues and are not independent
replicates.

## Completion analysis

Arrange simulator outputs as `ROOT/PROFILE/SEED/flowsInfo.csv`, with every
profile containing the same paired seed names.  Then run:

```sh
python3 experiments/rnic_multibaseline/analyze_fct.py cdf \
  --input-root /path/to/results \
  --output-dir /path/to/analysis
```

The analyzer accepts both the RNIC completion schema
`start_time_ps,completion_time_ps,fct_ps` and HTSIM's legacy nanosecond
`flowsInfo.csv` headers.  Picoseconds are the output unit.  Nanosecond inputs
are multiplied by exactly 1000; sub-nanosecond RNIC values are never rounded.
Explicit
`--flow-id-column`, `--payload-column`, `--start-column`, `--end-column`, and
`--fct-column` overrides are available for another exporter.  Use
`--input-time-unit ps` or `ns` when overridden column names do not carry their
unit.  It fails before writing an analysis if profiles have different seed
sets, a run has a missing or duplicate flow, payload bytes differ, timestamps
are inconsistent, or an explicit FCT disagrees with `end-start`.

The output schemas are:

- `fct_cdf_summary.csv`: `profile`, common `grid_fct_ps`, pointwise
  `cdf_mean`, population `cdf_sigma`, clipped `cdf_low`/`cdf_high`,
  `seed_count`, and `flow_count_per_seed`.  This is the flagship line-and-band
  plot input.
- `fct_cdf_by_seed.csv`: every individual seed ECDF on the same grid, retained
  for audit and optional thin-line overlays.
- `fct_run_summary.csv`: one row per profile/seed with flow and payload counts,
  nearest-rank p50/p95/p99/p99.9 FCT, and `jct_ps`.  JCT is the maximum absolute
  completion timestamp in that run.
- `fct_profile_summary.csv`: seed-level mean, population sigma, and mean ± sigma
  for each reported percentile and JCT.  FCT/JCT lower bounds are clipped at
  zero.
- `fct_analysis_manifest.json`: formula definitions plus the path and SHA-256
  digest of every input CSV.

The common CDF grid is zero plus the exact sorted union of every observed FCT;
the tool does not interpolate or manufacture samples.

## Slingshot-like parameter scan tables

Record each scan run in a CSV manifest with these required columns:

```text
profile,seed,completion_csv,q_hi_bytes,q_lo_bytes,telemetry_delay_ns,credit_quantum_packets,buffer_bytes
```

`completion_csv` may be absolute or relative to the manifest.  Every parameter
combination must contain the same paired seeds, and all completion files must
have the same flow/payload contract.  Generate plotting inputs with:

```sh
python3 experiments/rnic_multibaseline/analyze_fct.py scan \
  --manifest /path/to/scan.csv \
  --output-dir /path/to/scan-analysis
```

`parameter_scan_runs.csv` retains per-seed statistics.
`parameter_scan_summary.csv` has one row per full parameter tuple with
seed-level mean and population sigma.  `parameter_scan_plot.csv` is tidy
long-form data with one row per parameter tuple and metric, suitable for
faceting or filtering hidden debug slides; its metric values use picoseconds.
All three include the derived
hysteresis gap and the high/low thresholds as fractions of the configured
buffer.  The analyzer requires `Q_hi > Q_lo`, a positive packet-credit quantum
and buffer, and `Q_hi <= buffer`; it never inserts unstated scan defaults.

Run the synthetic analysis tests with:

```sh
cd experiments/rnic_multibaseline
python3 -m unittest -v test_analyze_fct.py
```
