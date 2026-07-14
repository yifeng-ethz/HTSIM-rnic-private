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
