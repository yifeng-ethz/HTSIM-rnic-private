# RNIC source provenance and acceptance targets

This document records where the RNIC simulator's behavioral contract comes
from. It is an interpretation guide, not a copy of the sources. The executable
contract is [rnic-simulation-model.md](rnic-simulation-model.md).

## Source precedence

When sources disagree, use the first applicable source in this order:

1. decisions from the current RNIC migration discussion, as recorded in
   `rnic-simulation-model.md` and reviewed code;
2. the time-regulated transport patent;
3. the current SIGCOMM paper and its appendices;
4. the network-calculus book and repository notes;
5. archived simulators, experiments, and presentation plots.

The lower-priority artifacts provide provenance and trend evidence. They are
not golden implementations, parameter sets, or exact-output fixtures.

## Local source index

All paths below are local to the development machine.

| Source | Path | Relevant pointers |
| --- | --- | --- |
| Current simulator contract | `/Users/yifengw/Documents/bp/HTSIM/docs/rnic-simulation-model.md` | Source precedence; public profiles; network-calculus contract; CN control; PRBS; Ring-CAM; Tomahawk 3 Clos |
| Patent | `/Users/yifengw/Documents/sigcomm/doc/patent-summary-time-regulated-transport.pdf` | pp. 3--5, Sections 3.3--3.5 and Figure 2: timestamp window and release; p. 9, Equations 9--13 and Appendix B: envelope restoration and PRBS; pp. 11--12, Appendix C.1: superseded rate-control design; p. 14, Appendices E--F: PRBS aggregation and shared sender pacer |
| Current paper | `/Users/yifengw/Documents/sigcomm/paper/main.pdf` | pp. 2--3, Sections 2.1--2.2: physical null manifold and packet ledger; pp. 4--6, Sections 3--4 and Figures 1--3: datapath, RB, direct grant and startup; pp. 8--9, Section 5 and Figure 7: spray/RB scaling; p. 10, Section 6.3 and Figure 8: Ring-CAM; pp. 12--13, Section 8: RTT claims excluded below; pp. 16--17, Appendix B: service curves, packetization and RB bounds |
| Paper source for precise anchors | `/Users/yifengw/Documents/sigcomm/paper/sections/02-motivation.tex` | Section 2.1 at line 62: null/empty networks, axioms and max-min convention; Section 2.2 at line 166: packet ledger |
| Paper source for direct grant | `/Users/yifengw/Documents/sigcomm/paper/sections/03-design.tex` | Section 3.1 at line 49 and Figure 2: RB semantics; Section 3.2 at line 108, Equation 1 and Figure 3: `margin * C_b / N_hat`, declaration and ACCEPT gate |
| Paper source for startup | `/Users/yifengw/Documents/sigcomm/paper/sections/04-flowdyn.tex` | Section 4.1 at line 66 and Section 4.2 at line 92: zero-to-grant startup and join timing |
| Paper source for fabric pairing | `/Users/yifengw/Documents/sigcomm/paper/sections/05-codesign.tex` | Subsections at lines 70, 123, 143 and 165; Figure 7: ECMP/spray/RB comparisons |
| Paper source for Ring-CAM | `/Users/yifengw/Documents/sigcomm/paper/sections/06-loss-recovery.tex` | Section 6.3 at line 144 and Figure 8 |
| Paper appendix source | `/Users/yifengw/Documents/sigcomm/paper/sections/appendix-null-properties.tex` | Sections B.1--B.3 at lines 17, 40 and 57: fluid/packet service and ledger; B.4--B.5 at lines 85 and 111: RB envelope and storage |
| Formal startup appendix | `/Users/yifengw/Documents/sigcomm/paper-src-notes/appendix-r1-theory.tex` | Lines 193--211: no uncounted DATA and aggregate-grant safety |
| Network-calculus book | `/Users/yifengw/Documents/sigcomm/papers/netCalBook-LeBoudec-Thiran.pdf` | Sections 1.2 and 1.5: arrival/service curves and shaping; Section 2.4.3: dampers; Section 5.1, PDF p. 173/book p. 155: the book's distinct transparent/null system |
| Network-calculus notes | `/Users/yifengw/Documents/sigcomm/papers/netcal-notes.md` | Repository mapping from Sections 1.2, 1.5 and 2.4.3 to the RNIC model |
| NERSC presentation | `/Users/yifengw/Documents/sigcomm/slides/collective-network-intro-vinay-nersc-new.pptx` | Slide 3: standard switch and TM3+ spraying statement; slide 4: historical FCT, TTOP and incast trends |
| Slide-4 testbench | `/Users/yifengw/Documents/sigcomm/experiments/cn_vs_dcqcn_testbench/README.md` | Lines 10--20: topology and historical CN model; lines 24--47: results; lines 49--57: caveats |
| Ring-CAM presentation | `/Users/yifengw/Documents/sigcomm/doc/slides-ringcam-resequencer-ieee-rt26.pdf` | Slides 9--11: arrival-write/timestamp-read and depth; slides 12--14: burst/storage/stacking trends; slides 15--16: prototype configuration and occupancy |
| Legacy alleged TM3 model | `/Users/yifengw/Documents/bp/htsim_clos_voq/README.md` | Lines 28--29 explicitly describe a lossless-input/lossless-output abstraction rather than a true Tomahawk 3 model |

## Invariants and their provenance

| Simulator invariant | Primary support | Authoritative interpretation |
| --- | --- | --- |
| CN means **collective network**, never congestion control. Public profiles are `rnic-cn`, `rnic-nn`, and `rnic-nn-fluid`. | Current discussion and simulator contract | Do not expose `rnic-cc` or retain legacy `rnic-null` naming. |
| Every CN flow declares before DATA; DATA is ineligible until an in-band ACCEPT; the sender then steps directly from zero to the returned grant. | Paper pp. 4--6, Figure 3 and Sections 3.2, 4.1--4.2; formal appendix lines 193--211 | No slow start, rate probing, additive ramp, lookback estimate, or out-of-band oracle in `rnic-cn`. |
| A receiver grant is the wire rate `margin * C_b / N_hat`; the default margin is `0.9`. | Paper p. 4, Equation 1, `03-design.tex:108` | `C_b` is physical wire capacity. Payload goodput is accounted separately; historical experiments with another margin cannot override this equation. |
| CN's PRBS pacer is one shared sender-side scheduler over per-flow heads; ETA is stamped after physical dispatch. | Patent pp. 9 and 14, Appendices B and F; paper p. 4, Figure 1 | Independent per-node reproducible streams preserve source independence. Mixed extents use size-normalized fixed-point event hazards so grants remain wire-byte rates; equal extents preserve the legacy sequence exactly. |
| RB admission and release are driven by packet eligibility time, not FIFO arrival or PSN. Missing/late packets cannot head-of-line block unrelated eligible packets. | Patent pp. 3--5, Sections 3.3--3.5 and Figure 2; paper p. 4, Figure 2 | Admit within the configured timestamp window and release at quantized `ETA + Delta`; use modular timestamp comparisons. |
| RB restores the dispatch envelope within one tick/packet edge; shared storage scales with admitted peak rate times `Delta`, not flow count. | Patent p. 9, Equations 9--13; paper p. 17, Sections B.4--B.5; Ring-CAM slides 9--14 | Enforce `W_RB >= R_in_peak * Delta + MTU`; separate early, late, and capacity violations. |
| `rnic-nn` has finite source/destination links, fixed propagation, instantaneous centralized max-min allocation, and no internal congestion queue, loss, backpressure, reorder, or variable delay. | Current discussion; paper pp. 2--3, Section 2.1; Appendix B.1/B.3 | Use a central collision-free source/destination packet calendar. Global state need not be broadcast as per-sender events. |
| Packetized NN retains exact packet sizes and bounded serialization quantization. | Paper p. 3, Section 2.2; Appendix B.1/B.3 | The final short packet is not padded or charged as a full MTU. Packetization is not congestion. |
| `rnic-nn-fluid` uses the same edge constraints and fixed propagation but continuous service. | Current discussion; paper p. 16, Sections B.1 and B.3 | No packets, PRBS, ACKs, Ring-CAM, or quantization; joins/leaves change max-min rates at the same timestamp. |
| Every Clos tier uses the Tomahawk 3 behavioral model with true ingress/egress VoQs. | Current discussion; NERSC slide 3 only supports TM3+ as a spraying-capable example | Exact switch scheduling is a declared simulator contract, not a reverse-engineered ASIC claim. |
| Network-calculus checks precede plot matching. | Paper Appendix B; book Sections 1.2, 1.5 and 2.4.3 | Enforce edge feasibility, packet service-curve slack, fixed-delay causality, RB envelope, and buffer bounds before trend comparisons. |

## Trend acceptance targets

These are directional/statistical tests, not exact-output tests:

- On the reproducible 40-node slide-4 workload, CN FCT remains tightly
  clustered near the appropriate NN reference, while the reactive baseline has
  a materially wider high-percentile tail.
- CN completion time remains roughly flat as incast width grows; the reactive
  baseline worsens with large fan-in.
- CN receiver goodput is fan-in-stable at the configured grant ceiling and has
  no switch drops in its validated operating region. With `margin = 0.9`, the
  ceiling is not full line rate.
- Packet spray plus RB preserves both capacity and delivery order. Per-flow
  hashing can waste capacity through collisions; spray without RB exposes
  reorder/burstiness.
- Legal switch jitter or timing compression increases burst peaks before RB;
  after RB, the release histogram/envelope follows sender dispatch shifted by
  `Delta`, within tick and packet-edge allowances.
- Ring-CAM occupancy remains bounded by the bandwidth--jitter product and does
  not scale with flow count merely because fan-in increases.
- Independent sender PRBS streams reduce aggregate phase alignment and
  short-window variance as sender count grows. PRBS alone is not expected to
  undo burstiness introduced by switches.
- Packetized NN converges toward fluid NN as MTU decreases. Their difference is
  bounded deterministic serialization/quantization, not a stochastic or
  load-dependent manifold queue.

Historical slide-4 values and paper Figure 7 values may be plotted for context,
but failed final-digit comparisons are investigations rather than regressions.

## Conflicts and non-golden evidence

| Artifact or claim | Conflict | Required treatment |
| --- | --- | --- |
| NERSC slide 4, including 387.7 Gbit/s incast | The backing harness uses `margin=0.985`, deterministic per-receiver ETA-slot staggering, and an out-of-order-tolerant receiver (`cn_vs_dcqcn_testbench/README.md:17-20`). This is not the current shared RNIC/PRBS/Ring-CAM model. | Reproduce its flat-vs-collapsing and tight-tail-vs-broad-tail trends only. Do not target 387.7 Gbit/s with the current `0.9` grant margin. |
| Paper Figure 7 CN/NN ratios near 1.003--1.012 | A sustained receiver-bound CN flow with a `0.9` grant ceiling asymptotically pays approximately `1/0.9` relative to full-rate NN. | Use flat CN-vs-rising-reactive shape and bounded RB state. Near-one historical ratios are not universal golden values. |
| Patent Appendix C.1 explicit-rate controller | It contains legacy soft-window tokens, service estimation, smoothing, ramping, stochastic probes, and payback. | Superseded by the conversation and paper's declaration/ACCEPT/direct-grant path. ETA calibration probes elsewhere in the patent are distinct from rate probes. |
| Legacy `htsim_clos_voq` “TM-3/VoQ” | Its README admits it is HTsim lossless-input plus lossless-output behavior, not a true VoQ traffic manager or proprietary TM3 model. No primary source specifies exact TM3 arbitration. | Do not preserve its queue behavior or numbers as golden. Record the new behavioral scheduler explicitly in every manifest. |
| Paper Section 8 RTT independence | The current implementation is not expected to remain correct under heterogeneous control-loop RTTs; the discussion places that case outside this paper/model scope. | Do not make mixed-RTT fairness an acceptance gate. Reject, constrain, or clearly label such runs as out of scope. |
| Paper Section 2.2 packet ledger | The closed form uses a named source-charged, non-overlapped accounting convention. A physical event engine may pipeline source serialization, propagation, and destination serialization. | Keep the ledger as an explicitly named analytical reference, not an event-timing golden trace. Enforce capacity and packet service-curve bounds instead. |
| Paper Axiom A2 endpoint-local knowledge | `rnic-nn` deliberately uses a centralized oracle and global rate table. | The current discussion wins for NN profiles; avoid event-expensive broadcasts when a central table preserves the same instantaneous allocation. |
| Network-calculus book's “null system” | Book Section 5.1 uses a transparent `delta_0` system, while the project NN retains finite edge rates, fixed latency, and optional packetization. | Do not equate the terminology. Use the project's physical null-manifold definition. |
| Ring-CAM slide wording about finishing the earliest timestamp | Taken literally, it could imply waiting for entries that never arrived. | Interpret it over present/ready entries only; the patent's no-HOL rule is authoritative. |

## Required run-manifest claims

Every published or compared run must record enough information to distinguish
the model from the non-golden artifacts above:

- [ ] resolved profile and each orthogonal dimension: fabric, traffic,
  controller, pacer, and RB mode;
- [ ] simulator commit, ATLAHS commit, dirty-worktree state, build mode, and run
  identifier;
- [ ] workload identity/hash, trace provenance, flow count, random seed set, and
  whether each scale point is measured or synthetic;
- [ ] topology, Clos tier count, routing/spray policy, endpoint and fabric link
  rates, oversubscription, MTU/header bytes, and fixed cable/switch latency;
- [ ] full Tomahawk 3 behavioral contract: VoQ key, arbitration policy,
  priorities, buffer limits, ECN/PFC/drop behavior, and whether any legacy
  approximation is active;
- [ ] CN declaration timing, ACCEPT/control RTT, bottleneck definition,
  `margin`, active-flow counting/retirement rule, and any intentionally hidden
  startup RTT;
- [ ] PRBS algorithm/version, polynomial or generator identity, packet quantum,
  global seed, node-seed derivation, tie-breaking, and mixed-size
  size-normalized fixed-point policy (including preferred scale and deterministic
  fallback rule);
- [ ] Ring-CAM `Delta`, release tick `delta`, timestamp width/wrap rule,
  calibrated transit, capacity, shared-vs-per-flow scope, release/admission event
  order, and early/late/capacity violation counters;
- [ ] NN oracle update semantics, max-min solver/tie rule, source/destination
  edge constraints, packet-slot matching policy, final-packet accounting, fixed
  in-flight delay, and confirmation that the manifold has no congestion queue;
- [ ] fluid-NN rate-change accounting and whether propagation is charged before,
  during, or after continuous service;
- [ ] metric definitions and measurement windows, especially FCT/JCT/TTOP,
  payload-vs-wire goodput, slowdown denominator, warm-up, and drain interval;
- [ ] comparator implementation and parameters, including DCQCN/PFC/ECN/RTO
  settings when used;
- [ ] which results are exact invariant checks, trend checks, or historical
  context, plus the cited source/profile and any deviation explanation.
