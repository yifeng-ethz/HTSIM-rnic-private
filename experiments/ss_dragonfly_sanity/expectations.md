# ss-dragonfly sanity studies: pre-registered expectations

Written and frozen before the first run of either study, in the style
of the simllm dcqcn_micro pre-registration. These studies are sanity
evidence for the hosted ss-dragonfly fabric mechanisms, not
calibration; every artifact they produce carries
`evidence=sanity-not-calibration` and
`calibration_status=hosted-pending-merlin-calibration`. The Merlin
comparison happens in the calibration wave against the NCCL completion
captures, not here.

Both studies run on `topologies/ss_dragonfly/p2a2h1g3_200g.topo`
(p = 2, a = 2, h = 1, g = 3; 12 hosts; all links 200 Gbit/s) with
open-loop line-rate sources in the rnic-nn spirit: no congestion
control, fixed 4160 B wire packets carrying 4096 B payload. The
payload-rate ceiling of one 200 Gbit/s link is therefore
C_p = 200 * 4096 / 4160 = 196.923 Gbit/s. Receiver goodput is binned
per 100 us interval per flow by the harness
(`htsim_ss_dragonfly`), written as CSV to the bulk output directory
`/data3/yifeng/simllm-dev/wave18-runs/w18a/` (never committed).

The rnic-nn endpoint itself is topology-free by construction, so the
harness uses open-loop endpoints of the same character (packetized,
uncontrolled, deterministic) attached to the fabric; the study
artifacts name them nn-style endpoints. DCQCN arms were assessed and
skipped as not cheap: the DCQCN comparator's RoCE/ECN machinery is
wired to the ns-tm3 Clos, not to this fabric, and porting it is not a
sanity-study sized change.

## S1: incast-degree ladder

Command shape, degrees N in {1, 2, 4, 8}, both routing arms:

```text
htsim_ss_dragonfly -topo p2a2h1g3_200g.topo -pattern incast
    -receiver 0 -degree N -duration_ps 2000000000 -bin_ps 100000000
    -routing adaptive|minimal -out incast_N_<arm>.csv
```

Senders are hosts 1..N (deterministic), so the ladder mixes same-router,
same-group, and remote-group senders. All contention is at the receiver
host link: this is endpoint congestion in the SC'20 sense.

Registered directions and shapes:

- S1.1 (saturation): for every N >= 1, aggregate receiver goodput in
  steady bins (from the fifth bin on) is at least 0.90 * C_p, and never
  exceeds C_p times 1.001 in any bin (conservation).
- S1.2 (degree-1 line rate): at N = 1 the single flow holds at least
  0.99 * C_p in steady bins and dropped_packets = 0.
- S1.3 (overload drops without collapse): for N >= 2 the open-loop
  offered load is N times the sink capacity, so sustained
  admission drops at the receiver leaf are expected
  (dropped_packets > 0) while aggregate goodput stays in the S1.1
  band. No congestion-collapse dip below the band is expected at any
  N because drops happen at admission, not after multi-hop resource
  consumption on these short paths.
- S1.4 (approximate fairness): for N in {2, 4}, each flow's steady-bin
  goodput lies within [0.6, 1.4] * (C_p / N). At N = 8, senders share
  four ingress directions unevenly (two hosts per router), so only the
  aggregate band is registered, plus the direction that no flow
  starves (every flow above 0.02 * C_p in steady bins).
- S1.5 (adaptive equals minimal under endpoint congestion): the
  adaptive and minimal arms produce aggregate steady-bin goodput within
  2 percent of each other at every N. This is the fabric echoing the
  published mechanism boundary: adaptive routing cannot relieve
  last-hop endpoint congestion (De Sensi et al., SC'20, section II-D
  motivates Slingshot's endpoint congestion control with exactly this).

## S2: step-wise flow join

Command shape, adaptive routing:

```text
htsim_ss_dragonfly -topo p2a2h1g3_200g.topo -pattern join
    -receiver 0 -degree 4 -join_interval_ps 250000000
    -duration_ps 1500000000 -bin_ps 100000000 -routing adaptive
    -out join_adaptive.csv
```

Senders are hosts 2, 4, 6, 8 (routers r1, r2, r3, r4; groups 0, 1, 1,
2), joining at t = 0, 250, 500, 750 us. All flows are open loop, so
the fabric's request/grant arbitration, not a congestion controller,
sets the shares.

Registered directions and shapes:

- S2.1 (staircase top): aggregate receiver goodput reaches at least
  0.90 * C_p within two bins of the first join and stays in the S1.1
  band for the rest of the run.
- S2.2 (share steps): within two bins after the k-th join
  (k = 2, 3, 4 active flows), every active flow's per-bin goodput is
  within [0.45, 1.55] * (C_p / k), and the earlier flows step DOWN on
  each join (flow 0's steady share strictly decreases across the four
  join epochs). The band is wider than S1.4 because the receiver
  leaf's ingress-level round robin arithmetically parks one flow at
  exactly 1.5 * C_p / 3 during the three-flow epoch (two of the three
  flows share one fabric ingress), and packet quantization wobbles
  around that point.
- S2.3 (no starvation, no oscillation): after the last join, per-flow
  goodput in consecutive steady bins varies by less than 20 percent of
  C_p / 4 for every flow.
- S2.4 (drops onset): dropped_packets = 0 while only flow 0 is active;
  drops accumulate only after the second flow joins.

## Determinism guard (both studies)

Running any arm twice with the identical command produces byte-identical
CSV and manifest output. A mismatch is fatal.

## Fatal guards (both studies)

Any of the following voids the affected run rather than scoring it:
harness nonzero exit, quiescence failure at drain, delivered = 0, a
conservation violation (any bin above C_p * 1.001 aggregate), or a
determinism-guard mismatch. Voided runs are reported as voided with
the guard that fired; they are never silently rerun with different
parameters.

## Corrections

None recorded.
