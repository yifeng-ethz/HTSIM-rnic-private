# Bounded initial DATA and tail recovery

A receiver can observe a missing packet when later DATA or RETIRE exposes its
sequence gap. A lost retry has no such guarantee: the receiver has already
requested those bytes, and another packet need not reveal that the retry was
also lost. The sender therefore retains a physical timeout for that exact
logical extent. Repeated probes can themselves be lost when they arrive
during the same congested burst, so an optional exponential schedule spaces
later probes progressively farther apart.

## Interface

The collective `rnic-cn` profile accepts these independent selections:

| Selection | Meaning |
|---|---|
| `-rnic_cn_data_recovery none` | Existing receiver gap recovery and long sender timeout; default |
| `-rnic_cn_data_recovery deadline` | Constant tail-probe interval after a dispatched retry |
| `-rnic_cn_data_recovery exponential` | Tail-probe interval doubles with each successive retry of the extent |
| `-rnic_cn_retry_probe_windows m` | Positive base multiple of the configured control deadline, default 4; requires enabled DATA recovery |
| `-rnic_cn_initial_window_bytes U` and `-rnic_cn_initial_window_fan_in F` | Paired optional pre-grant DATA budget and declared positive fan-in bound |

The default has neither an initial limit nor a prompt tail probe. The
initial-window pair may be selected independently of DATA recovery. All
selections use `RnicCollectiveNetworkRuntime`, behind `AtlahsFlowRuntime`.
The paired simllm interface is `HtsimRnicConfig`.

## Initial grant and storage envelope

Before an in-band nonempty receiver grant reaches a sender, the flow may
serialize at most U DATA wire bytes. Both originals and retries count.
An indivisible next packet waits if it cannot fit the remaining budget.
Configuration requires F*U <= B for the declared shared leaf pool, whose
configured capacity B stays unchanged. F is an input assumption about flows
converging on that pool. It does not dynamically read switch occupancy or
establish a bound for every other traversed pool.

An empty initial receiver snapshot schedules an ACCEPT at the next control
boundary when the flow needs a grant to progress. U=0 therefore progresses
without manufacturing DATA. A full-flow budget stays dormant in a no-loss
run; a receiver-observed gap schedules a grant if recovery would otherwise
be held behind that budget. ACCEPT, negative acknowledgements and resolution
controls retain their physical routes, serializers and finite storage.
The separate control-headroom option protects their admission.

## Probe timing and terminal resolution

Let P=m*K, where K is the control deadline. After the source physically
serializes retry a, the next probe's eligibility is its serialization end
plus P under `deadline`, or P*2^(a-1) under `exponential`. These probes use the
normal per-flow retransmission head and source lottery. A receiver negative
acknowledgement still requests the next retry immediately. Backoff is local
to the logical extent; another flow's receipt cannot reset it.

The maximum retry count R remains unchanged. A probe is armed only for a<R.
The final retry keeps the existing long timeout, starting at its own physical
serialization end, and permanent loss fails there without sending retry R+1.
Exponential configuration rejects interval overflow and a legacy timeout
that would preempt the largest probe interval.

A physical GAP_RESOLVED terminally closes the sender's logical extent. An
older successful copy can therefore cancel a newer queued probe. Cancellation
removes only that extent's queued retries and timers; already serialized or
routed copies still drain. Repeated resolution after retirement is idempotent.
Probe packet and wire-byte counters advance at physical dispatch, so a
cancelled queued probe contributes neither.

The distinction between speculative probes and loss declarations, and
increasing probe intervals under repeated loss, follows the mechanism in
[RFC 9002 sections 6.2 and 6.2.1](https://www.rfc-editor.org/rfc/rfc9002.html#section-6.2).
The configured deadline is a model input, not a measured round-trip-time
estimate or a calibration of any remote direct memory access network
interface controller (RNIC) firmware.

## Late receive admission

An authenticated retry of a receiver-owned missing extent may use recovery
admission after the normal reorder window expires. Its carried timestamp
and bytes stay intact. Release occurs at the first receive tick at or after
actual arrival, through the existing finite Ring-CAM reorder store and the
same receive serializer as ordinary DATA. Ring-CAM occupancy is charged until
logical release; pending serialization retains its existing accounting domain.
Full recovery storage rejects the copy and issues a physical negative
acknowledgement. Original DATA retains strict window admission.

The manifest reports the selected recovery and backoff, initial sizing,
terminal timeout rule, physical probe packets and bytes, late admissions,
budget holds and initial ACCEPT dispatches. Quiescence validates all packet,
retry, timer, control and receive-storage lifecycle state. These are modeled
mechanism checks; the paired simllm study separately checks consumer latency,
physical byte floors, exact disabled identities and the limits of calibration.
