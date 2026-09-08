# Bounded control admission for the collective network

A switch can drain only one packet at a time on each output wire. When many
sources send together, their arriving bytes wait in switch memory. A small
control packet can be lost at a full memory even when it has first choice of
the next wire slot: admission happens before service priority can help it.

In the width-64 all-to-all, each of eight active receivers on a leaf has 56
remote sources. The first data packet from each of those 448 flows offers
448*4160 = 1863680 wire bytes to that leaf, already larger than the 1048576
byte shared buffer. Each complete 65536-byte flow carries 16 packets, so the
full data round aimed at that leaf offers 29818880 wire bytes. These are
arrival-work bounds, not a claim that every offered byte is buffered at one
instant: output draining, path interleaving and sender pacing determine actual
occupancy. By contrast the corresponding DECLARE wave alone is only
448*64 = 28672 bytes. It is data plus later in-band feedback and retirement
traffic sharing memory that exposes control to loss, not an isolated 64-byte
message exceeding the buffer. The pinned width-64 exits establish that the
admission envelope is exceeded. The study records switch loss evidence.

## Hardware basis and chosen policy

Real remote direct memory access network interface controllers (RNICs) use
reliable transport responses and timeouts, and fabrics can protect selected
traffic with separate scheduling classes and buffer reservations. NVIDIA's
[RoCE configuration](https://docs.nvidia.com/networking-ethernet-software/cumulus-linux/Layer-1-and-Switch-Ports/Quality-of-Service/RDMA-over-Converged-Ethernet-RoCE/)
assigns congestion notification packets (CNPs) strict priority and describes
priority flow control (PFC) for lossless data traffic. Its
[buffer configuration reference](https://docs.nvidia.com/networking-ethernet-software/nvue-reference/Set-and-Unset-Commands/QoS/)
also exposes reserved buffer space and shared headroom. These sources support
class separation as a mechanism; they do not claim all control packets are
lossless or identify our reserve with a measured switch allocation.

Implement policy B: `-rnic_cn_control_recovery headroom`. The default `none`
retains fatal control loss. Each physical output receives an additive reserve
for the seven collective controls: DECLARE, ACCEPT, GRANT_UPDATE, GAP_NACK,
GAP_RESOLVED, RETIRE and NFLOW_UPDATE. DATA never uses that reserve. Only a
control that would fail the original shared-pool or egress-domain admission
may enter the reserve. This models bounded class protection, without adding
an endpoint retry protocol or learning loss through simulator side tables.

A reserved packet keeps its single physical lifecycle, existing priority,
route, queue order and serialization. Every receive path is unchanged: no
replayed control can duplicate membership, grants or retirement because no
extra packet is created. Arbitrary link corruption and exhausted control
reserve still invoke the existing fatal lifecycle path. This is admission
loss prevention, not recovery of an already destroyed packet.

## Storage authority and sizing

Let B be the unchanged shared buffer, E the unchanged per-egress base cap,
H the reserve per egress, F the number of flows whose outstanding controls
can converge on one egress, M their maximum outstanding controls per flow,
and C the control wire bytes. Require H >= F*M*C. Report the conditional
admitted fan-in floor(H/(M*C)). The initial enabled configuration uses
H=131072, M=32 and C=64, admitting F=64 per egress. M is an explicit envelope
assumption, not a runtime limit on the protocol. The 16-packet study flows
have 16 feedback messages plus membership and recovery traffic; M=32 allows
additional controls but does not prove a bound under arbitrary repeated loss.

With N physical outputs, provision total switch storage B+N*H. The base
pool and each egress base occupancy exclude packets charged to the reserve.
An ordinary admission uses exactly the old B and E comparisons. A rescued
control charges only its output's reserve, with occupancy at most H; it
releases that charge when selected for serialization. Total physical occupancy
includes both kinds of storage. Reserve packets never migrate into the base
pool as it drains. Thus no data packet sees a larger or smaller configured
admission threshold. Subsequent traffic may change because a formerly lost
control now arrives; that is the intended physical consequence.

Flags: `-rnic_cn_control_recovery none|headroom`,
`-rnic_cn_control_headroom_bytes H`, and
`-rnic_cn_control_messages_per_flow M`. Sizing values are positive, bounded
against physical occupancy overflow, and accepted only on the collective
profile. Parameter overrides without headroom are rejected rather than
silently ignored. The default selection adds no buffer and preserves the
fatal behavior of the pin.

The configuration manifest prints selection, base buffer, reserve per output,
M, C, admitted fan-in and the formula identifier. At termination it prints
headroom admissions separately for all seven control kinds, their sum, peak
reserve occupancy, remaining occupancy and unchanged switch drop diagnostics.
Counts are switch admissions: a control rescued at two switches counts twice.
They are not endpoint retries or recovered application messages.

## Frozen validation contract

Before implementing or running, freeze native capacity and traffic sweeps:
base buffers 64 and 128 bytes, reserves 64 and 128 bytes, every control kind,
both shared-pool and egress-domain overflow. Inject the same admission-loss
condition with the reserve off and on. Off must drop the control; on must
consume it exactly once without a FABRIC_DROP. Exhausted reserve must drop.
DATA must not use headroom even if it is otherwise empty. Draining must leave
all occupancy zero. Sizing is exact floor division, including 63/64-byte
boundaries, M=1 and M=32, C=64 and C=96, and overflow rejection.

Native no-loss fixtures compare complete packet arrival identity/order/time;
driver fixtures compare byte-identical rendered completion CSV with none and
headroom. The off path must retain existing native tests and command behavior.
These are structural and exact-oracle checks, not behavioral score points.

The simllm control_recovery_v1 freeze governs the wide-incast experiments:
widths 8/16/32/64, 400/200 Gbit/s, none/headroom, identical ideal references,
and the six prior pipeline failures. The old completed CSVs must stay
byte-identical. Enabled cells must quiesce. Expected off-mode loss is identity
evidence, with no valid latency. No violated fatal guard is survivable.
Physical floors precede measurements; latency ceilings are unbounded because
finite storage protection alone does not bound control or retry delay.
