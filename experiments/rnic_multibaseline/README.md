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

Use identical seeds, payloads, topology, link rates, propagation, packet
extent, and switch-wide physical-pool capacity across physical baselines.
Record any model-specific congestion-domain admission cap separately rather
than calling it the physical pool.  A seed changes only the logical-rank to
physical-node permutation and each protocol's explicitly seeded stochastic
components.

## Join / finite-byte exit dynamics

The dynamics workload uses eight incast sources, one per leaf, and node 63 as
the destination.  Flow `i` joins at `i * 5 ms`.  Its payload is the ceiling of
the ideal 400-Gbit/s processor-sharing service integral from its join until its
target completion.  Consequently all eight flows are active before any target
completion; flows then finish in reverse join order at 5 ms intervals, with
flow 0 (the first joiner) deliberately completing last.
These are byte-limited flows: there is no stop event or exit timer, so every
protocol determines its own actual completion time.

```sh
python3 experiments/rnic_multibaseline/generate_join_exit_goal.py \
  --output /tmp/join-exit.goal \
  --metadata /tmp/join-exit.json

/path/to/htsim_goal_txt2bin \
  -i /tmp/join-exit.goal \
  -o /tmp/join-exit.bin
```

The metadata records the overall and 1-to-2-flow join windows.  The plotted
2-to-1 exit is aligned to each protocol's actual penultimate completion, not
to the ideal target timestamp, so a long DCQCN tail cannot silently move out
of the zoom.

## Simultaneous incast sweep

`run_incast_sweep.py` runs paired finite 32 MiB incasts with 2, 4, 8, 16,
32, and 64 directed flows.  Every send is eligible at time zero.  With one
destination inside a fixed 64-node topology, the 64-flow point necessarily has
63 distinct remote source RNICs and one additional flow from an existing
source; plots must therefore label the x axis as **flow count**, not claim 64
distinct senders.  Receiver payload goodput is
`sum(payload bytes) * 8 / (last completion - first start)`.

```sh
python3 experiments/rnic_multibaseline/run_incast_sweep.py \
  --build-dir /path/to/htsim-build \
  --output-root /path/to/incast-results \
  --seeds 1-8 \
  --schemes dcqcn,rnic-cn \
  --dcqcn-egress-buffer-bytes 4194304
```

The canonical DCQCN stress keeps the `ns-tm3` switch-wide shared pool at
64 MiB and separately caps queued VoQ occupancy mapped to any one physical
egress at 4 MiB.  The packet in egress serialization is not buffer-resident.
Admission requires space in both domains, and the simulator reports which
domain caused every drop.  The 4 MiB value is a declared behavioral-model
parameter, not a claim about Tomahawk 3 hardware and not a relabeling of the
64 MiB switch-wide pool.  Both values are recorded in the run command,
signature, and manifests.  A seed-1 calibration over 1, 2, 4, and 8 MiB
selected 4 MiB because 1--2 MiB dropped before modest fan-in had converged,
whereas 8 MiB produced no loss through 64 flows; this is an experiment stress
choice, not a hardware-sizing inference.

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
profile,seed,status,reason,completion_csv,q_hi_bytes,q_lo_bytes,telemetry_delay_ns,path_hysteresis_ns,credit_quantum_packets,buffer_bytes
```

`status=complete` requires a `completion_csv`, which may be absolute or relative
to the manifest.  `status=expected-invalid` requires an empty `completion_csv`
and a nonempty reason; the analyzer validates the parameters but excludes that
row from statistics and plotting.  The 16 MiB `rnic-ss` buffer point uses this
status because it is below the checked analytical envelope.  The analytical
predicate admits 32 MiB, but seeds 1 and 3 observed physical loss in the
canonical four-seed campaign at model commit `0fa128d`; the runner therefore also records
32 MiB as `expected-invalid` and excludes all four paired seeds rather than
averaging only the successful runs.  The canonical executed lossless point is
64 MiB.  Every executed parameter combination must contain the same paired
seeds, and all completion files must have the same flow/payload contract.
Generate plotting inputs with:

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

The reproducible hidden-slide sweep is one-factor-at-a-time around the declared
default.  It covers `Q_hi`, `Q_lo/Q_hi`, extra telemetry delay, path-selection
hysteresis, returned credit quantum, and shared buffer without pretending that
the open values are proprietary Rosetta parameters:

```sh
python3 experiments/rnic_multibaseline/run_rnic_ss_scan.py \
  --build-dir /path/to/htsim-build \
  --output-root /path/to/scan-results \
  --seeds 1-4
```

Run the synthetic analysis tests with:

```sh
cd experiments/rnic_multibaseline
python3 -m unittest -v test_analyze_fct.py
```

## Paired experiment runner

`run_multibaseline.py` generates or verifies the flat 32 MiB, dependency-gated
32 MiB DAG, and join/exit GOAL artifacts, converts them with the checked-in
converter target, and runs the selected paired seeds through DCQCN and the four
RNIC profiles.  The `flat32m` and `dag32m` schedules have the same transfer
multiset and rank placement; only the phase dependencies differ.  Every run
gets a completion CSV, stdout/stderr logs, and a provenance manifest.  A run
is rejected if the process fails, the CSV is partial, timestamps disagree, or
the completed transfer set differs from the GOAL.

The join/exit runs additionally request an event-driven `stateTrace.csv` for
DCQCN, `rnic-cn`, and `rnic-ss`.  It is buffered by the simulator and installed
atomically only after physical quiescence.  The runner validates and hashes the
common schema (`time_ps`, flow/endpoints, event, configured/effective rate, alpha,
pause state, and packet counters).  ACK progress remains real trace evidence
but is collapsed when plotting unchanged rate state; no periodic rate samples
are synthesized.

The same three physical profiles also produce `goodputTrace.csv`. Successful
logical payload delivery is counted exactly once at the receiver: newly
accepted in-order DATA for DCQCN, the first SACK-scoreboard acceptance for
`rnic-ss`, and the post-Ring-CAM in-order delivery ledger for `rnic-cn`.
Duplicates and failed attempts therefore contribute no bytes. Rows are sparse,
simulation-epoch-aligned bins with schema
`bin_start_ps,bin_end_ps,flow_id,source,destination,delivered_payload_bytes,goodput_bps`;
`goodput_bps` is the integer floor of `bytes * 8 * 10^12 / bin_width_ps`.
The join/exit runner defaults to 10-us bins and accepts
`--goodput-trace-bin-ps` for an explicit alternative. It verifies ordering,
alignment, the derived rate, endpoints, and exact per-flow payload totals,
then records the path, row count, bin width, and SHA-256 in the v3 run manifest.
Both simulator CLIs keep tracing off unless a path and a positive bin width
are supplied together, and install the CSV atomically only after quiescence.

For `rnic-ss`, the effective rate is sender-local evidence rather than a
central allocation.  It is the access-link rate before pair-selective
backpressure, zero while a new credit epoch has no returned service sample,
and then cumulative serviced wire bytes divided by the physical CREDIT-arrival
interval from that epoch.  Each active source flow on the same endpoint pair
receives an equal share of this observational pair rate.  `flow-start`, source
active-set changes, BP transitions, changed service-rate observations, and
completion are traced; forwarding and control decisions do not read the trace.

DCQCN and `rnic-cn` use the fixed `ns-tm3` Clos; `rnic-ss` uses the same Clos
with `ns-rosetta`.  The `rnic-nn` and `rnic-nn-fluid` profiles remain
topology-free manifold baselines and receive only their fixed propagation
delay.  The ordered `rnic-ss` default uses the 128-entry SACK scoreboard/window
needed by the rounded one-control-RTT allocation at 400 Gbit/s.

The pinned 400-Gbit/s DCQCN comparator uses seeded linear RED
(`Kmin=65,536 B`, `Kmax=655,360 B`, `Pmax=0.25`), PFC XON/XOFF thresholds of
`520,000/720,000 B`, and a 100-Mbit/s minimum per-flow rate.  The RED and rate
floor values preserve the earlier repository study's 400G scaling, while the
RP/NP equations and 50/55-us timers remain the SIGCOMM'15 algorithm.  All of
these values, the 64 MiB switch-wide shared pool, and the 4 MiB per-egress
admission domain are emitted in each run manifest; the older deterministic
single-threshold ECN mode is not the canonical comparison.

These occupancies are distinct physical domains. RED and the admission/drop
cap observe queued VoQ bytes at one physical egress. PFC XON/XOFF observes
admitted low-priority bytes at one physical ingress, while the 64 MiB pool is
switch-wide. The experiment keeps the numerical PFC thresholds below the
4 MiB egress cap as a pinned stress choice, but validation does not pretend
that ingress PFC and egress RED are one ordered occupancy axis.

```sh
python3 experiments/rnic_multibaseline/run_multibaseline.py \
  --build-dir /path/to/htsim-build \
  --output-root /path/to/results \
  --seeds 1-10 \
  --schemes all \
  --workloads all \
  --goodput-trace-bin-ps 10000000
```

Render the overall, 1-to-2 join, and actual 2-to-1 exit rate panels from one
paired seed with:

```sh
python3 experiments/rnic_multibaseline/plot_presentation_results.py \
  join-dynamics \
  --dcqcn-trace /path/to/results/results/join-exit/dcqcn/seed-1/stateTrace.csv \
  --rnic-cn-trace /path/to/results/results/join-exit/rnic-cn/seed-1/stateTrace.csv \
  --metadata /path/to/results/workloads/join-exit/shared/workload.json \
  --output /path/to/join-exit-dynamics.png
```

Use `--dry-run` to print every conversion and simulator command without
creating output files.  Existing workloads and completed runs are reused only
when their hashes, commands, parameters, and manifests match exactly;
`--force` is required to replace a mismatched managed artifact.  The default
Slingshot-like comparison uses ordered endpoint-pair routing; select the
unordered sensitivity case explicitly with `--ss-routing unordered`.

Run its unit and fake-tool integration tests with:

```sh
cd experiments/rnic_multibaseline
python3 -m unittest -v \
  test_run_multibaseline.py test_plot_presentation_results.py
```
