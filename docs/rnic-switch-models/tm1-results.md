# Single C3232C component results

The independently audited packet model executes 920 configurations with no
fatal accounting, capacity or physical-floor violations. Four senders each
provide 1 MiB to one receiver. The sweep covers four policies, four 25G ports
versus one 100G port, MTU1500/9000, W8/16/32/64 us, 8/64 ns ticks, 90/95
percent wire allocation, three seeds, explicit collisions/hash mappings,
half rates, static/dynamic admission and cell/XPE accounting sensitivities.

The paired SimLLM study is `examples/asu_tm1_single_switch_v1`. Expectations
were committed before implementation at 33a44d6d, with independent pre-run
accounting clarification 3d75af59. Binary SHA256 is
`885789d2f53f5e8bcc9c432b5a5226d38b9a7b1ba0ebc629dfcab96a72040cc5`.
Raw CSV, exact commands, source hashes and manifests are external build
artifacts. This is a synthetic UDP component experiment with a preinstalled
calendar, not a distributed rnic-cn controller, hardware calibration or
RC/DCQCN comparison. See [the model and design](tm1.md).

## Selected operating point

Balanced explicit destination lanes, W=32 us, tick=8 ns, DATA allocation
95 percent, seed=1 and proposed per-DATA-queue static cap 1260 cells:

| Ports | IP MTU | Pacing | Complete 4 MiB? | Makespan us | Max DATA cells | Drops |
| :-- | --: | :-- | :-- | --: | --: | --: |
| 4x25 | 1500 | Deterministic quota | Yes | 380.056160 | 18 | 0 |
| 4x25 | 9000 | Deterministic quota | Yes | 371.040160 | 88 | 0 |
| 1x100 | 1500 | Deterministic quota | Yes | 379.014400 | 42 | 0 |
| 1x100 | 9000 | Deterministic quota | Yes | 360.212160 | 108 | 0 |
| 4x25 | 1500 | Independent PRBS lottery | Yes | 400.920160 | 232 | 0 |
| 4x25 | 9000 | Independent PRBS lottery | Yes | 403.192160 | 528 | 0 |
| 4x25 | 1500 | Unpaced | No | Last delivery 168.117600 | 1258 | 1564 |
| 4x25 | 9000 | Unpaced | No | Last delivery 166.522080 | 1248 | 244 |

Bounded-quota PRBS matches the deterministic calendar's selected four-lane
makespans and maximum occupancy. It supplies no measured benefit in this
balanced equal-weight case. Independent lotteries have seed-dependent tails.
Four MACs do not produce additional interior routes, and do not universally
improve randomness or throughput over one 100G MAC.

Dropped-run times describe the last delivered packet. They are not task
completion times and cannot be used to claim that unpaced traffic is faster.
All static cases remain within the proposed cap. The dynamic comparison
reaches 3360 DATA cells because its separate alpha profile has no 256 KiB
hard cap. Each sensitivity uses explicit uncalibrated pool/mapping inputs.

## W and MTU choice

| IP MTU | W8 us | W16 us | W32 us | W64 us |
| --: | --: | --: | --: | --: |
| 1500 | 380.248160 | 380.184160 | 380.056160 | 377.816160 |
| 9000 | 371.040160 | 371.040160 | 371.040160 | 362.208160 |

These are deterministic four-lane makespans in us at 95 percent and 8 ns
ticks. W64 is fastest among these evaluated advance-known bulk cases.
Choose W32 for the initial interactive prototype: its small-frame penalty
versus W64 is 2.24 us, and it halves the allocation horizon. The candidate
19 us declaration/allocation/grant/arm lead still fits one future W32 window.
Choose W64 with jumbo frames for long bulk phases when coarser response is
acceptable. This sweep does not measure unpredictable demand arrival and
does not establish a global optimum.

Choose MTU1500 first for smaller nonpreemptive control blocking and easier
small-operation accounting. MTU9000 improves bulk efficiency but blocks
priority traffic for up to 2.89216 us per active 25G frame, rather than
0.49216 us. The one-100G calendar completes the selected jumbo workload in
360.21216 us, faster than four 25G lanes at the same W. Keep packetization,
per-port caps and aggregate storage budgets explicit in such comparisons.

## Physics and validation

One sender's 1 MiB is 731 packets/1123138 wire bytes at MTU1500, or
118 packets/1060612 bytes at MTU9000. The aggregate 100G serialization floors
are 359.40416 and 339.39584 us before fixed forward terms. The runner checks
the stronger maximum actual receiver-lane byte floor on four 25G ports.
Every generated extent matches an independent packetization formula.

Three frozen relation families pass in 22 parameterized instances: pure wire
serialization floors double exactly at half rate; jumbo lowers wire overhead
for identical requested bytes; and a forced one-lane collision allocates
23.75 Gb/s instead of 95 Gb/s. Loaded half-rate makespan ratios range from
1.9603 to 2.0280 because fixed delays, tick rounding and whole-packet window
carry remain. A four-port collision never gains service through hash entropy.

The complete native suite passes 549 tests, including 14 TM1-focused tests,
legacy frozen behavior and exception cleanup. Native test counts, fatal
guards, wire oracles, behavioral families and configuration counts are
separate evidence classes. The DATA sweep injects no HIGH control traffic;
priority/nonpreemption is unit-test evidence, and K=8 us still requires a
policed control envelope and measured timing qualification.

Remaining work is registered in the paired backend module: HTSIM-43 owns
NX-OS/cut-through/XPE calibration; HTSIM-44 owns connection to the existing
RNIC runtime for a matched ECN/DCQCN RC experiment. These results close
neither hardware validation nor application TTFT/TPOT integration.
