# One C3232C: Tomahawk1 model and ASU design

Use four independently addressed 25 GbE ports with a receiver-authorized
wire-byte calendar. Apply the proposed 256 KiB DATA cap separately to each
physical egress queue. Start with a 32 us allocation window W, 8 ns scheduler
tick, 95 percent DATA wire budget and 1500-byte IP MTU; compare 9000-byte MTU
when throughput matters more than control blocking and fine allocation.
These are design candidates evaluated by the paired study, not calibrated
hardware bounds or a globally optimal configuration.

The model is an additive packet-level component. It uses existing Packet,
Route, EventList, exact wire serializer, allocator and PRBS seams. It does
not replace the default runtime, define a new application ABI, or turn a
synthetic UDP experiment into a complete rnic-cn or RoCE transport.

## Installed switch and evidence classes

The inspected switch is Cisco Nexus C3232C, NX-OS 7.0(3)I7(6). Read-only
inspection finds configured and operational Cut-Through, four active output
QoS groups, strict-priority group 3, DATA in group 0 with 100 percent remaining
bandwidth, and dynamic queue limit 6 (alpha 1/2). PFC is inactive and no WRED
policy is attached. PTP is disabled; the clock reports NTP. PTP show commands
are unavailable while disabled, so that inspection does not test timestamp
operation. Existing packet/drop counters are historical, not calibration of
this model.

A temporary unattached custom queuing policy accepted `queue-limit 262144
bytes` and was removed. No interface policy was replaced or saved. Therefore
the 256 KiB cap is a proposed setting, not the switch's current cap, and its
effective cell rounding is unknown.

[Cisco identifies Nexus 3200 as Broadcom Tomahawk](https://blogs.cisco.com/datacenter/3-reasons-10-million-is-a-big-number).
[The C3232C datasheet](https://www.cisco.com/c/en/us/products/collateral/switches/nexus-3232c-switch/datasheet-c78-734883.html)
lists 32 100G ports, 4x25G breakout, a nominal 16 MB buffer and approximately
450 ns switching latency. The exact die revision has not been read. The
behavioral model therefore represents first-generation Tomahawk, not an
asserted die stepping.

## TM1 accounting and limits

| Quantity | Model value | Status |
| :-- | --: | :-- |
| Packet cell | 208 bytes | Broadcom TM1 constant |
| Advertised cells per XPE | 20165 | SDK constant, 4194320 bytes |
| XPE domains | 4 | Cisco confirms nominal 4 MB per XPE |
| Physical cells per XPE | 23040 | Distinct physical quantity, not available shared capacity |
| CFAP reserve | 720 physical cells | Already distinct from advertised capacity; not subtracted again |
| DATA static cap | floor(262144/208) = 1260 cells | Conservative proposed rounding, 262080 bytes |
| Control cap | floor(8192/208) cells | Study design, not installed setting |
| Internal overhead | 0 or 64 bytes | Sensitivity; 64 is an SDK accounting constant |
| Unavailable pool cells | 0 or 4096 per domain | Sensitivity, not measured reservation |
| XPE accounting mask | Explicit 4-bit mask | Uncalibrated port mapping; charge each selected domain conservatively |
| Dynamic threshold | alpha = 1/2 | Separate mode matching observed alpha, not a static cap |
| Allocation granularity | Whole frames | Admission, grant and launch never consume fractions of a packet |

[Broadcom's TM1 header](https://github.com/Broadcom-Network-Switching-Software/OpenBCM/blob/master/sdk-6.5.27/include/soc/tomahawk.h#L642)
defines these cell counts, four service pools, eight ingress priority groups,
16 internal priorities and a 64-byte packet-header constant.
[Its initialization](https://github.com/Broadcom-Network-Switching-Software/OpenBCM/blob/master/sdk-6.5.27/src/soc/esw/tomahawk.c#L814)
also establishes ten UC and ten MC hardware queues per ordinary port. Those
ASIC resources do not imply ten NX-OS-configurable QoS classes. The model
uses the DATA and control classes needed by this experiment, rather than
claiming all hardware queue features are deployed.

[The XPE topology](https://github.com/Broadcom-Network-Switching-Software/OpenBCM/blob/master/sdk-6.5.27/src/soc/esw/tomahawk.c#L1694)
is XPE0: ingress pipes 0/3 to egress pipes 0/1; XPE1: 0/3 to 2/3;
XPE2: 1/2 to 0/1; XPE3: 1/2 to 2/3. Actual Cisco lane-to-pipe mapping,
pool configuration, minimum guarantees, descriptor limits, ingress admission
and cut-through reservations require hardware calibration. Full charge in
each selected model domain is a conservative sensitivity, not a claim that
every actual unicast packet is duplicated in SRAM. The model does not let one
queue borrow a fictional unrestricted 16 MB pool.

For each selected XPE, admission checks whole-frame cells against available
pool cells. Static mode additionally checks the class cap. Dynamic mode checks
the prospective shared queue occupancy against alpha times remaining pool
cells after admission. With no reserved minimum this idealized rule gives an
isolated alpha=1/2 queue approximately one third of an otherwise free pool.
The comparison boundary is an explicit model choice, not measured ASIC timing.
[Cisco documents dynamic threshold factors and queue limits](https://www.cisco.com/c/en/us/td/docs/switches/datacenter/nexus9000/sw/7-x/qos/configuration/guide/b_Cisco_Nexus_9000_Series_NX-OS_Quality_of_Service_Configuration_Guide_7x/configuring_queuing_and_scheduling.html).

The packet path is source MAC serialization, cable, complete-frame admission,
450 ns processing eligibility, output class FIFO, one nonpreemptive output
serializer and cable. Occupancy retains the transmitting packet until its
final bit. Retaining the full active frame conservatively delays its storage
release in this model. Complete-frame admission and the 450 ns eligibility
offset are approximations, not universal upper bounds on actual occupancy,
loss or latency. Live cut-through latency is not reproduced exactly; the
datasheet number does not identify head-cell forwarding, egress-speed mismatch
or congested cut-through behavior. Report simulated latency as model latency.

## What four ports change

Each 25G lane has its own MAC, addressing, source service, receiver egress
queue and lane grant. Four 256 KiB DATA caps give about 1 MiB of aggregate
nominal DATA queue allowance across those four queues, subject to XPE/pool
constraints. They do not give four times the cap to one flow on one port.
DATA and PTP/control caps must be accounted separately; a DATA cap alone does
not bound total port occupancy across every class.

Do not rely on unknown switch hashing to choose these lanes. Use four
explicit destination addresses or a verified forwarding group, and bind all
lanes to one logical ASU endpoint. Breakout creates physical links; it does
not create four interior ECMP routes through this one switch. One fixed tuple
uses one ordinary ECMP member. Random UDP source ports can collide and only
help when more reachable forwarding members exist.

Four independent PRBS streams can decorrelate scheduling opportunities if
seeds and eligible sets differ. They do not increase Shannon entropy of a
single physical path or guarantee better queue tails. A 100G MAC can perform
the same deterministic PRBS selection over the same flow candidates and has
one quarter the packet serialization time. The advantage of four MACs is
parallel port service and four congestion domains. The cost is lane skew,
more metadata, clock-domain boundaries and possible reorder.

Implement per-destination, per-lane sender VoQs before the MACs, bounded
descriptor/storage credit per operation, and a receiver calendar. This keeps
a blocked operation from consuming every sender buffer. Do not call this
switch a distributed ingress-VoQ fabric: the modeled TM1 scheduling boundary
is shared-memory admission followed by physical egress queues.

## Calendar, known demand, tick and deadline

Known ASU byte counts permit advance declarations or a preinstalled schedule.
A receiver freezes a future window's demand, allocates wire budgets and sends
an immutable epoch, lane map, authorized packet count/sequence range and
ready-time contract. For every physical source lane s and receiver lane d:

`sum_i r(i,s) <= margin*C_s` and `sum_i r(i,d) <= margin*C_d`.

Compute integer packet counts using actual wire lengths, carrying fractional
byte deficits. Schedule eligible full packets by rotating deficit service;
PRBS may permute legal opportunities inside the remaining quotas. PRBS alone
is a lottery with statistical tails. It is not a hard per-window allocation
or a lossless-buffer proof. A central oracle calendar in this experiment is
explicitly preinstalled, not distributed message delivery. Its destination
planning clock constrains joint-resource launch opportunities; it is not
proof of a receiver-arrival window budget when frames have different lengths
or source/destination rates. Actual modeled output queues still enforce
physical service and expose any resulting backlog.

At 25G, W=32 us contains 100000 wire bytes at full rate and 95000 at
95 percent. Four equal senders have 23750 bytes each per receiving lane,
roughly 15 full 1500-MTU frames or two full 9000-MTU frames with deficit carry.
Smaller W improves response granularity but makes jumbo packet allocation
coarse; larger W accumulates more setup delay and burst exposure. An 8 ns
tick is 1.6 percent of a 1500-MTU frame time and 0.28 percent of a jumbo
frame time. It is a hardware implementation candidate, not a PTP accuracy
claim. An 8 ns tick requires a qualified 125 MHz timebase or equivalent
timestamp implementation; the existing 100 MHz management clock has a
10 ns period. Keep those domains explicit and repeat the sweep at a chosen
native hardware tick before firmware signoff. Round launch eligibility
upwards; retain exact serializer fractions.

In the reference book `dwnd=K` is a one-way control deadline. This design
names the allocation window W separately. With bounded declaration delivery
K_d, allocation work L_c, grant delivery K_g, arm work L_a and guard G:

`Lead = K_d + L_c + K_g + L_a + G`,
`future_windows = ceil(Lead/W)` for boundary-only declarations.

A candidate reserved control service K_d=K_g=8 us, L_c=L_a=1 us, G=1 us
gives Lead=19 us and one future 32 us window. Measure these bounds before
promising them. Announcing data at least Lead early hides control setup;
knowing its byte count does not make the data ready or bypass PCIe/replay
storage credits. Late declarations move to a later window; broken promises
leave their reserved holes visible rather than silently reallocating them.

The candidate 8 us deadline requires a control arrival envelope, not merely
an 8 KiB storage cap. An illustrative per-output aggregate envelope is a
1024-wire-byte burst and no more than 1 percent of lane rate, including PTP
and all ASU control. Four compact 192-wire-byte grants per 32 us consume
0.768 percent of a 25G lane; PTP and other records must fit the remainder.
With that envelope enforced at both source and switch control service, two
25G nonpreemptive stages each budget at most one 9038-byte DATA frame plus
1024 bytes of control burst. A conservative residual-rate approximation
gives `2*(2892.16 ns + 1024*8/(0.99*25e9)) + 650 ns`, about 7.10 us before
remaining timestamp/implementation error. This makes 8 us a plausible target,
not a proven current bound. An unpoliced full 8 KiB control backlog is a
different experiment and can exceed it. The 920-case DATA sweep contains
no injected control workload; only native tests exercise priority blocking.

The reference runtime's shared ledger can affect other senders at DECLARE
dispatch before any control packet arrives. This is ex-ante oracle knowledge.
Its zero-gap behavior does not prove a distributed hardware implementation.
ACKs emitted throughout a window can also arrive after the activation
boundary. For late-window ACK feedback use enough future windows to cover
the complete round trip, or send the snapshot in a reserved boundary-control
slot. Do not copy an ideal two-window shift without checking causality.

## MTU and resequencing

| IP MTU | Full wire bytes | 25G frame time | 100G frame time | UDP experiment payload |
| --: | --: | --: | --: | --: |
| 1500 | 1538 | 492.16 ns | 123.04 ns | 1436 bytes |
| 9000 | 9038 | 2892.16 ns | 723.04 ns | 8936 bytes |

Wire extent includes 14-byte Ethernet header, 4-byte FCS, 8-byte preamble/SFD
and 12-byte IFG. Cell charge excludes the final 20 bytes of those wire costs.
The synthetic payload uses IPv4/UDP plus a 36-byte experiment header. These
payload sizes are not RoCE MTU definitions. At full frames the payload/wire
efficiencies are about 93.37 and 98.87 percent. At 95 percent wire allocation
the corresponding application ceilings are about 88.70 and 93.93 Gb/s over
four 25G lanes, before tails, window rounding, control, PCIe and memory limits.

Use 1500 first for tight control blocking, small operations and initial
firmware verification. Enable 9000 end to end for long bulk phases only after
the receiver ring, replay storage and lane-ordering contract cover full jumbo
frames. IP MTU is not the same as an RDMA QP's path MTU; use the supported
verbs path MTU for the standard RC comparison.

The byte-cap approximation gives about 83.9 us on each 25G lane and
21.0 us at 100G. It is not a formal cell-cap drain ceiling because the wire
contains preamble/IFG that is not stored. For full study packets with zero
internal overhead, 1260 cells hold at most 157 full 1500-MTU packets or 28
full 9000-MTU packets, whose wire drains are 77.26912 and 80.98048 us at 25G.
For arbitrary admitted packet lengths, a conservative one-cell maximum of
228 wire bytes gives 91.9296 us before other control backlog. Use actual
admitted wire bytes and a service envelope for a formal bound.
A finite cap does not itself bound DATA residence under
unlimited HIGH traffic. A tighter calendar-derived jitter bound needs a
proved burst envelope and positive residual DATA service. With four sources
each contributing at most one simultaneous frame to a balanced lane, three
frames of FIFO buildup are 1.47648 us (1500) or 8.67648 us (9000); continuous
independent lotteries do not establish that one-frame burst premise.

Size receiver reorder storage from the proved aggregate jitter envelope,
not a chosen average delay. Keep four physical storage banks and one global
sequence head per logical peer to preserve the ASU ordering contract. Merge
the bank heads using authorized global release timestamps/ranges. Independent
physical-rail sequence heads would change object assembly/visibility rules
and require a separate contract change. Across four lanes a conservative
256 KiB allowance per lane already
means roughly 1 MiB of upstream DATA allowance, plus endpoint queues, replay,
descriptors and guard. Missing-packet deadlines require exact granted slots
or a window-tail uncertainty term; an absent packet cannot provide its own
transmit timestamp. Keep control on a separate sequence space.

## PTP and standard ECN comparison

PTP packets can share the dedicated control class (strict group 3), while
DATA uses group 0. Classify both UDP 319 and 320, and the chosen ASU control
traffic, explicitly. Police their aggregate envelope. Classification alone
does not enable the switch clock or hardware timestamps. Nonpreemptive DATA
can block newly arriving PTP by up to one DATA frame, 492.16 or 2892.16 ns
at 25G; strict priority cannot remove that term. Verify whether event
timestamps are taken before/after the queue and how correction fields are
handled on every real endpoint.

[The C3232C standards table](https://www.cisco.com/c/en/us/products/collateral/switches/nexus-3232c-switch/datasheet-c78-734883.html)
lists IEEE 1588 boundary-clock support.
[The applicable 7.x PTP guide](https://www.cisco.com/c/en/us/td/docs/switches/datacenter/nexus9000/sw/7-x/system_management/configuration/guide/b_Cisco_Nexus_9000_Series_NX-OS_System_Management_Configuration_Guide_7x/b_Cisco_Nexus_9000_Series_NX-OS_System_Management_Configuration_Guide_7x_chapter_0100.html)
describes UDP boundary-clock operation, with Ethernet PTP transport and
transparent-clock operation unsupported for this platform in the applicable
guide.
[The platform scalability guide](https://www.cisco.com/c/en/us/td/docs/switches/datacenter/nexus3232and3264/sw/7x/scalability/guide/b_Cisco_Nexus_3232C_3264Q_Verified_Scalability_703I71.pdf)
verifies 10G PTP port counts. Exact 25G breakout timestamp behavior is not
established by those counts and remains a required live qualification.

[Cisco's queuing guide](https://www.cisco.com/c/en/us/td/docs/switches/datacenter/nexus9000/sw/7-x/qos/configuration/guide/b_Cisco_Nexus_9000_Series_NX-OS_Quality_of_Service_Configuration_Guide_7x/configuring_queuing_and_scheduling.html)
documents WRED with ECN in a queuing class; the C3232C platform readme directs
users to these N9K guides. The
[exact I7(6) unsupported-feature list](https://www.cisco.com/c/en/us/td/docs/switches/datacenter/nexus9000/sw/7-x/release/notes/70376_nxos_rn.pdf) does not
exclude ECN. This is documented capability, not a measured CE/CNP loop.
The guide prohibits WRED and explicit tail-drop configuration in the same
class. Therefore use separate experimental policies: static-cap calendar
and WRED/ECN with measured dynamic/physical admission. Do not assume
`queue-limit 262144 bytes` plus `random-detect ... ecn` is a supported pair.

For the RC comparator retain a stable packet path for each QP, enable ECT,
verify CE at the receiver and CNP at the sender, then measure DCQCN rate,
retries, completion time, fairness and drops. Compare lossless ECN+PFC and
lossy ECN without PFC as separate profiles with the same wire workload.
[NVIDIA documents both RoCE modes](https://docs.nvidia.com/networking-ethernet-software/cumulus-linux-54/Layer-1-and-Switch-Ports/Quality-of-Service/RDMA-over-Converged-Ethernet-RoCE/).
RC provides reliable ordered message delivery; it does not imply that the
fabric never drops a packet. Arbitrary packet spraying can trigger endpoint
reorder/retry costs. The synthetic UDP calendar does not execute RC, CNP,
DCQCN or retries and cannot produce a valid standard-RDMA performance claim.

## One switch, Clos and other chips

One switch is sufficient for receiver allocation, many-to-one congestion,
four-lane scheduling, queue caps, PTP isolation and ECN experiments. Simulate
four senders and one receiver, even if the current physical bench has fewer
independent active endpoints. A loopback or two FPGA functions are not extra
physical NICs. The model endpoint count is a declared experiment input.

A real two-tier Clos is needed only to measure interior multipath, uplink
oversubscription and stage contention. The smallest meaningful redundant
example is two leaves plus two spines: four switches total, so three more
than the current bench. Two leaves plus one spine has three switches but
only one spine path. Buying more switches is not a prerequisite for the
single-switch allocation design. Start with the current bench and emulate
the larger topology using the existing breakout/Clos primitive.

If distributed switch VoQs with credit-based fabric service are a hard
requirement, compare a Jericho-class design.
[Broadcom's Jericho2 description](https://investors.broadcom.com/news-releases/news-release-details/broadcom-announces-jericho2-production-silicon-availability-0)
explicitly provides end-to-end VoQ and fabric credits. For modern in-switch
spraying/adaptive load balancing, choose a platform whose vendor explicitly
supports those features, such as the newer
[Arista 7060X6 family](https://www.arista.com/assets/data/pdf/Datasheets/7060X6-Datasheet.pdf), then verify
the exact software and RC endpoint interoperability. The original 7060CX32S
is also Tomahawk1; its shared chip generation does not imply every EOS and
NX-OS feature is identical.
[Arista identifies the original platform as Tomahawk](https://www.arista.com/assets/data/pdf/Whitepapers/Arista-Solutions-Cloud-Providers-WP.pdf).

## Reference provenance

The rnic-cn design authority is `docs/rnic-cn-endpoint/algorithm_book.md`;
read its reservation ledger, sender egress composition, deficit accounting
and acknowledged incomplete mechanisms. An older internal timing memo was
reviewed as conceptual background, as requested. Its original document is
kept outside this repository. The proposal borrows calibrated sender-defined
eligibility and bounded endpoint resequencing, while using the current book
for allocation semantics. It does not adopt old timestamp field widths,
fixed tick counts or controller constants.

For an arrival envelope `A(t+h)-A(t) <= sigma+rho*h` and guaranteed service
`C*(h-T)^+`, with rho no larger than C, backlog is bounded by
`sigma+rho*T` and delay by `T+sigma/C`. C here is bytes per second when
sigma is in bytes. A receiver bank sized for jitter J needs at least
`C_lane*J + Lmax`, plus descriptor/clock/endpoint allowances. Independent
eligibility release does not create link capacity: simultaneous eligibility
timestamps still need serialized service. Use `arrival-ETA` to measure
upstream jitter; residence until `ETA+J` decreases as that jitter increases.
The current experiment does not execute the endpoint resequencer or prove
this envelope for a deployed controller.

The paired study lives at `examples/asu_tm1_single_switch_v1` in SimLLM and
freezes expectations in commit 33a44d6d before this implementation, with
independent pre-run accounting clarification 3d75af59. Run
configs, CSV metrics and manifests identify inputs and binary hashes; hardware
qualification and full application TTFT/TPOT integration are separate evidence.
