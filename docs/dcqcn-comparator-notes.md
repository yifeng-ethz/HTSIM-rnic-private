# DCQCN comparator notes (comparator-realism ruling, 2026-08-04)

The DCQCN/RoCEv2 comparator exists to be beaten honestly. With the default
configuration (32 MiB switch buffers, deep ECN thresholds, PFC on) the
mixed all-to-all gives it a free ride: a few hundred ECN marks, a couple
of PFC pauses, zero drops and zero RTOs, which understates every cost the
design actually pays in production fabrics. This note records the
maintainer's realism ruling: the two comparator modes, the mlx5 research
conclusion behind the recovery model, and the measurement contract.

## The two modes

- **ECN+PFC (default, `-pfc on`).** The historical behavior: per-ingress
  PFC meters pause the upstream data class between the low and high
  thresholds, so the fabric is lossless and recovery is rare. This is the
  vendor-recommended deployment and remains the default so existing runs
  reproduce bit-for-bit.
- **ECN-only (`-pfc off`).** PFC is disabled entirely; when the ns-tm3
  shared pool or an egress domain overflows, the switch drops the packet
  through the existing counted drop path, and recovery is the transport's
  job. This is the deployment many operators actually run to avoid PFC
  storms, and it is the configuration under which the comparator must be
  measured against small buffers.

## Loss recovery fidelity (mlx5 research conclusion)

RoCEv2 NICs of the ConnectX generation recover with go-back-N: a NACK for
an out-of-order PSN rewinds the send edge to the cumulative
acknowledgment, and a silent tail loss is recovered by the retransmission
timeout. That stays this comparator's default (`-recovery gbn`).

ConnectX-6 Dx introduced a limited form of selective repeat: the receiver
tracks a small, fixed window of out-of-order arrivals and requests exactly
the missing PSN; when reordering or loss runs beyond the tracking
resources, the NIC falls back to the go-back-N rewind. `-recovery sr`
models that shape with a fixed tracking window (`-sr_window_packets`,
default 64, bounded by the receiver's compile-time tracking capacity), one
selective NACK per open hole, and an escalation to the go-back-N NACK the
moment a successor lands beyond the window.

In hardware the rate machine is coupled to recovery: a retransmission
event cuts the sending rate like a congestion notification does. The
comparator therefore applies the same alpha-based multiplicative cut the
DCQCN rate processor uses for a CNP on every loss recovery event, i.e. a
NACK-triggered go-back-N rewind, a selective retransmission, and a silent
RTO all induce the AIMD-style drop. `-loss_rate_cut off` isolates this
coupling for sensitivity runs; it is on by default.

## Measurement contract (PFC storm observability)

In ECN+PFC mode the manifest reports, with no behavior change:

- `dcqcn_pfc_pause_frames` and `dcqcn_pfc_resume_frames`, in total, per
  switch, and per ingress port;
- `dcqcn_pfc_paused_wall_ps`, the cumulative wall time each port held its
  upstream paused;
- `dcqcn_pfc_max_cascade_depth`: pause frames carry their cascade depth,
  a pause emitted while one of the emitting switch's egresses is itself
  paused counts as that egress depth plus one, and the manifest reports
  the deepest chain observed. Depth one is a root pause; anything deeper
  is head-of-line blocking spreading upstream, the precursor of a PFC
  storm.

Per-switch and per-port lines are emitted only for switches and ports
with pause activity. The counter line also reports `loss_rate_cuts` so a
recovery-coupled rate collapse is visible without the state trace.
