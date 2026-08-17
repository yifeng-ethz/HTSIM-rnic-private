# The Slingshot-class dragonfly fabric (ss-dragonfly)

Status: hosted, calibration pending. This wave lands the physical
dragonfly fabric with Rosetta-style switches and progressive adaptive
routing, deterministic fixtures, and sanity harnesses. No accuracy or
fidelity claim against the PSI Merlin Slingshot fabric is made here;
that claim waits for the calibration wave against the NCCL completion
captures being taken on Merlin in parallel.

Components:

| File | Role |
| --- | --- |
| `ss_dragonfly_fabric.{h,cpp}` | Physical fabric: geometry, switches, cables, per-hop routing, congestion transports, route-state ownership |
| `main_ss_dragonfly.cpp` | Sanity-study driver (`htsim_ss_dragonfly`) with open-loop nn-style endpoints |
| `dragonfly_progressive_routing.{h,cpp}` | Pure progressive table core (ported from codex/rnic-dragonfly-routing) |
| `dragonfly_progressive_congestion.{h,cpp}` | Pure causal three-signal congestion core (same port) |
| `dragonfly_route_state_store.{h,cpp}` | Packet route-state sidecar (same port) |
| `ns_rosetta_switch.{h,cpp}` | Input-buffered request/grant switch model (already on main) |
| `topologies/ss_dragonfly/*.topo` | Topology instances for the `-topo` dialect |

## Sources

Every mechanism below maps to a source that was fetched and read for
this note. Where the public record fixes a mechanism's structure but
not its constants, the constant is a named calibrated abstraction in
the registry further down, never an invented mechanism.

1. Mike Parker, Steve Scott, Albert Cheng, John Kim, "Progressive
   adaptive routing in a dragonfly processor interconnect network",
   US 9,137,143 B2, granted 2015-09-15.
   https://patents.google.com/patent/US9137143B2/en
   Read for: the four table families (global/local, minimal/
   non-minimal); per-hop progressive lookup; the four-bit congestion
   value composed by three-input four-bit unsigned saturating addition
   of a remapped near-end value, a remapped far-end estimate (traffic
   sent but beyond the channel round trip), and a remapped downstream
   value; the per-routing-class bias pair (two-bit shift plus six-bit
   additive); adaptive control types 0..3; root detect for local
   non-minimal routing; the diverged bit on local minimal entries.
2. Daniele De Sensi, Salvatore Di Girolamo, Kim H. McMahon, Duncan
   Roweth, Torsten Hoefler, "An In-Depth Analysis of the Slingshot
   Interconnect", SC'20. arXiv:2008.08886,
   https://arxiv.org/abs/2008.08886
   Read for: Rosetta (section II-A): 64 ports at 200 Gbit/s,
   request/grant virtual-output-queued switching, request-queue credits
   as the adaptive-routing congestion estimate, end-to-end ACKs
   tracking outstanding packets per endpoint pair, 350 ns mean switch
   latency (300 to 400 ns); topology (II-B): dragonfly default, 16
   endpoints per switch over copper, intra-group full electrical
   connectivity, inter-group full optical connectivity, diameter three
   switch-to-switch hops; adaptive routing (II-C): up to four minimal
   and non-minimal candidate paths, congestion estimated from the total
   depth of the request queues of each output port, congestion state
   exchanged with neighboring switches by piggybacking on
   acknowledgments (about four bytes reverse per forward packet), bias
   toward minimal paths; congestion control (II-D): per-endpoint-pair
   tracking, contributor-selective stiff backpressure, and the
   observation that adaptive routing cannot relieve endpoint (last-hop)
   congestion.
3. John Kim, William J. Dally, Steve Scott, Dennis Abts,
   "Technology-Driven, Highly-Scalable Dragonfly Topology", ISCA'08.
   https://dl.acm.org/doi/10.1109/ISCA.2008.19 (open copy read:
   https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/34926.pdf)
   Read for: the standard parameters (section 3.1): p terminals, a
   routers per group, h global channels per router, router radix
   k = p + a + h - 1, up to g = ah + 1 groups, N = ap(ah + 1), balance
   condition a = 2p = 2h; routing (sections 4.1, 4.2): MIN, Valiant
   non-minimal through a random intermediate group, UGAL-L/UGAL-G
   choosing by queue length times hop count; virtual-channel deadlock
   assignment (Figure 7).
4. Greg Faanes, Abdulla Bataineh, Duncan Roweth, Tom Court, Edwin
   Froese, Bob Alverson, Tim Johnson, Joe Kopnick, Mike Higgins, James
   Reinhard, "Cray Cascade: a scalable HPC system based on a Dragonfly
   network", SC'12. DOI 10.1109/SC.2012.39.
   Citation-integrity note: the SC'12 full text is paywalled and only
   its bibliographic record and abstract were readable for this note,
   so no mechanism sentence below is anchored to it. The shipped
   Cascade/Aries mechanism details are instead anchored to the same
   group's public whitepaper, which was fetched and read in full:
5. Bob Alverson, Edwin Froese, Larry Kaplan, Duncan Roweth, "Cray XC
   Series Network", Cray Inc. whitepaper WP-Aries01-1112.
   https://www.alcf.anl.gov/files/CrayXCNetwork.pdf (read via the
   Internet Archive copy of the same document)
   Read for: each tile's four route-table sets, local minimal and
   non-minimal and global minimal and non-minimal, and the routing
   pipeline generating two minimal and two non-minimal candidates per
   packet (pages 13 and 14); the three load metrics, the load
   distribution to neighbors every ten cycles, and the programmable
   minimal/non-minimal bias mechanism (page 14); selection of the
   lightest-load path "using a combination of downstream link load,
   estimated far-end link load and near-end link load", Valiant
   non-minimal routing through an in-group root, and the
   deterministic-hash routing control mode for ordered traffic
   (page 17).

## Mechanism-to-source map

| Mechanism in this fabric | Source anchor |
| --- | --- |
| Dragonfly terms p, a, h, g; canonical maximum size g = a*h + 1; local full mesh; one global connection per group pair | ISCA'08 section 3.1; SC'20 section II-B |
| One input-buffered request/grant switch per router; whole-packet VoQ admission against a shared buffer | SC'20 section II-A (ns-rosetta model documented in docs/rnic-switch-models) |
| Switch pipeline delay before the route decision, default 350 ns | SC'20 section II-A latency measurement |
| Progressive per-hop lookup: every switch re-decides from its own state; a local choice commits nothing end to end; a crossed global hop fixes the intermediate group | US 9,137,143 B2; WP-Aries01-1112 page 17 |
| Candidate families GM, GN, LM, LN with two minimal plus two non-minimal candidates | US 9,137,143 B2; WP-Aries01-1112 pages 13 and 14; SC'20 section II-C (up to four candidate paths) |
| Congestion score = remapped four-bit near-end + far-end estimate + downstream, saturating addition | US 9,137,143 B2; WP-Aries01-1112 page 17 |
| Near-end signal from the local output's waiting plus in-service bytes | SC'20 section II-C (request-queue depth per output port) |
| Far-end resident estimate = bytes sent minus bytes still on the wire minus credits returned, causally updated | US 9,137,143 B2 (far-end tracking beyond round trip); ported causal congestion core |
| Downstream load advertised by the neighbor at high frequency, consumed only at its physical arrival boundary, expiring at a maximum age | WP-Aries01-1112 page 14 (every ten cycles); SC'20 section II-C (neighbor exchange piggybacked on ACKs) |
| Per-class bias as shift plus additive; minimal wins ties; default bias prefers minimal | US 9,137,143 B2 (two-bit shift, six-bit additive); SC'20 section II-C (bias toward minimal) |
| Non-minimal = Valiant through a safe intermediate group (or in-group root) | ISCA'08 section 4.1; WP-Aries01-1112 page 17 |
| Deterministic flow-stable hash for ordered traffic; per-packet adaptive for unordered | WP-Aries01-1112 page 17; SC'20 section II-E (per-class ordering) |
| Endpoint-pair outstanding tracking and pair-selective backpressure (the hosted rnic-ss endpoint on the Clos) | SC'20 sections II-A and II-D |
| Adaptive routing cannot relieve endpoint congestion (registered expectation S1.5 of the sanity studies) | SC'20 section II-D |

## Calibrated abstraction registry

Each entry names the parameter, its default, and why it is an
abstraction. The calibration wave replaces defaults with
capture-derived values where discovery permits.

1. `near_end_bytes_per_level` (8320 B), `far_end_bytes_per_level`
   (8320 B), `downstream_bytes_per_level` (33280 B): the patent fixes
   the four-bit remap structure but publishes no thresholds; uniform
   steps of two and eight maximum packets are chosen for study
   sensitivity.
2. `advertisement_period_ps` (100 ns default; 10 us in the study
   fabric): Aries advertised every ten cycles and Rosetta piggybacks on
   ACK traffic; Slingshot's actual cadence is unpublished, so the
   exchange is modeled as a periodic per-link advertisement.
3. `advertisement_delay_ps` (300 ns default; 1 us in the study
   fabric): the physical delay of the load information path; the
   congestion core rejects consumption before this boundary.
4. Advertisement scope, switch-aggregate waiting bytes: SC'20 names
   the per-output request-queue depth as the switch-local estimate but
   does not publish the granularity advertised between neighbors; this
   fabric advertises the neighbor's switch-wide waiting bytes on every
   directed link.
5. `maximum_downstream_age_ps` (400 ns default; 40 us in the study
   fabric, a 100x override): staleness bound after which an unrefreshed
   advertisement contributes zero; unpublished. The ported congestion
   core additionally carries its own internal default of 1 us for the
   same knob. The F6 congestion-response fixture depends on the
   study-fabric value: its probe-B advertisement is 10 us old at
   consumption and would already be expired at the header default (see
   the fixture-expectations corrections).
6. `adaptive_biases` (minimal shift 0 add 0, non-minimal shift 1
   add 1, all four classes): the structure is the patent's, the values
   are ours; the shift-1 default mirrors UGAL's hop-count weighting of
   ISCA'08 section 4.2 and the SC'20 minimal bias.
7. `switch_pipeline_ps` (350 ns): SC'20 reports the mean; the
   distribution is not modeled.
8. `rosetta_shared_buffer_bytes` (4 MiB in the study fabric): Rosetta
   buffer sizing is unpublished.
9. Virtual channels: the routing core's eight per-hop dependency labels
   are recorded on the route state but do not key physical VoQs; this
   store-and-forward fabric admits whole packets against a finite
   shared buffer and never blocks a link on far-side credit, so the
   labels are route-legality annotations, not modeled wormhole VCs
   (carried over from the ported integration plan's carve-out).
10. Control-plane transport: advertisements do not contend with DATA
    on the modeled links; SC'20 quantifies the real reverse overhead at
    about four bytes per forward packet, which this wave abstracts
    away.
11. The Merlin-shaped instance
    (`topologies/ss_dragonfly/merlin_a100_5node_4x200g.topo`) is
    UNVERIFIED as a whole: it is the smallest fabric consistent with
    what five-node capture discovery can determine (per-port rate, port
    count, coarse latency), namely all twenty 200G ports on one
    Rosetta-class switch. Its latency and buffer values are fixture
    defaults pending capture evidence.

## What this wave does not claim or contain

- No rnic-ss (or rnic-cn, or DCQCN) composition over the dragonfly
  fabric; the rnic driver names that seam and rejects dragonfly
  `-topo` files until the calibration wave. The hosted rnic-ss endpoint
  runs on the controlled two-tier ns-rosetta Clos.
- No link or router failure modeling in the physical fabric; the ported
  routing core carries the causal failure machinery, but the fabric
  treats a routing drop decision as a hard error this wave.
- No claim that the study fabric's constants match Merlin. Sanity
  artifacts are labeled `evidence=sanity-not-calibration`.
- General g < a*h + 1 global-link arrangements are unsupported until
  the canonical core grows a proved general connector map; supported
  instances are the canonical maximum-size dragonfly and the
  single-switch degenerate instance.

## Determinism

The fabric introduces no wall-clock or unseeded randomness: per-packet
route hashes derive from the configured `routing_seed`, the flow, and
the packet sequence through splitmix64; candidate sampling and
tie-breaks in the ported core are functions of the route hash;
advertisement emission and consumption are periodic in simulated time.
Identical invocations of `htsim_ss_dragonfly` produce byte-identical
CSVs and manifests (registered as a fatal guard in the sanity
expectations).

## Evidence map

- Deterministic fixtures with pre-registered exact values:
  `docs/ss-dragonfly-fabric/fixture-expectations.md` and
  `ss_dragonfly_fabric_test.cpp`.
- Sanity studies with pre-registered directions, shapes, and fatal
  guards: `experiments/ss_dragonfly_sanity/`.
- The ported progressive core's own exhaustive table tests:
  `dragonfly_progressive_routing_test.cpp` and neighbors.

## Commit chronology disclosures

Branch history is append-only, so commit-message inaccuracies are
corrected here rather than by rewriting:

- The deferred-failure boundary (`deferFailure` behind the noexcept
  switch observation) landed with the fabric commit 9233fcd; the
  fixture commit 791ddfd's message credits it to itself, which is
  wrong. 791ddfd introduced the construction-time advertisement-cadence
  anchoring and the origin-relative fixture harness.
- dc02db1's verbatim claim is exactly true for its own scope and no
  wider: the twelve dragonfly core files it adds are blob-identical to
  their origin/codex/rnic-dragonfly-routing counterparts. The adapted
  ported files (shared endpoint-pair header, RnicWideInteger
  arithmetic) arrived later with their adaptations disclosed in
  966e2d1.
- 9233fcd's full-ctest gate was vacuous for that commit's own roughly
  1100 new fabric lines: no test exercised them until the fixtures
  arrived in 791ddfd. The byte-identity gate it reported was real and
  covered the legacy binaries.
