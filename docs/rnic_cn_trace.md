# Collective packet and queue observations

The optional collective trace records physical packets, switch visits and
receiver delivery through the direct `htsim_rnic` execution path. The switch
and collective runtime own timing. Trace output is an observation of those
owners and never supplies scheduling input.

## Invocation

Add `-rnic_cn_trace_dir` to an `rnic-cn` command. The input and output parent
directory must already exist; the trace destination and its `.tmp` sibling
must be absent.

```sh
build/datacenter/htsim_rnic -goal run/step.bin -topo run/clos.topo \
  -rnic_profile rnic-cn -linkspeed_bps 400000000000 \
  -completion_csv run/completion.csv -rnic_cn_trace_dir run/trace
```

Other profiles reject the option. Omitting it preserves the timing path.
Tracing does not enable control headroom, DATA probes or initial-send budgets;
those selections remain independent.

## Observation contract

The format is `rnic-cn-trace-v1`. Every observation has `schema`, a globally
dense `sequence`, and `observed_at_ps`. Merge the four tables by sequence.
Physical service timestamps may precede the observation that reports them;
observation time is not a substitute for service start or logical eligibility.

| Table | Authority projected |
|---|---|
| `flows.csv` | Requested endpoints, tag, payload and exact packetization, with separate 64-bit flow and 32-bit PacketFlow identities |
| `packets.csv` | Physical packet and lifecycle IDs, route, source service, DATA extent and attempt, or control metadata |
| `queues.csv` | Enqueue, service start/end and drop on a physical switch ingress/egress, with base-buffer and in-service observations |
| `events.csv` | Endpoint arrival/consumption, admission, receiver release/service, ordered delivery, completion and retry authorization |

The complete column headers live with the writer in
`htsim/sim/datacenter/rnic_collective_trace.cpp`. A retransmission is a new
physical packet joined to the same logical flow and packet index. A packet
created before route installation is joined by lifecycle; tracing never
allocates its ID or chooses its route early.

A switch queue wait is service start minus enqueue. Receiver holding is
logical release minus actual arrival. Receiver queue wait is service start
minus logical release. These are different quantities. On-time DATA can
arrive later and spend correspondingly less time in the receiver store,
leaving release unchanged. Summing waits across visits gives work accounting,
not automatically an additive flow or request latency contribution.

Delivery rows identify both the previous logically delivered packet and the
receiver completion that triggered progress. An earlier missing extent can
release later packets that have already completed receiver service. The final
logical packet index alone therefore does not identify the cause of completion.

## Finalization and evidence

Output is written under the `.tmp` sibling and atomically installed after
verified physical quiescence. `manifest.csv` records table counts, the complete
sequence range, packet/flow counts and the verification timestamp. The native
stdout manifest reports the same `physical_quiescence_time_ps` with tracing
enabled or disabled. Write and finalization failures reject successful exit;
partial output remains diagnostic evidence only. An accepted consumer also
requires successful process exit and its complete completion CSV.

The paired simLLM `pp_rail_contention_v2` study owns the frozen experiment,
strict trace audit and request projection. Native unit cases are component
evidence. Neither trace availability nor a packet timeline constitutes
hardware calibration or a serving-latency result.
