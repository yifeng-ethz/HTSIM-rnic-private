# rnic-cn algorithm book

Living specification for the collective-network endpoint, kept next to the
implementation on purpose: when code and book disagree, the book is the
design authority and the code carries the burden of proof. Maintainer
corrections land here first; implementation PRs cite the section they
implement.

## 1. The control loop (canonical design, 2026-08-03 maintainer correction)

One algorithm for all traffic. There are no modes, no leases, no waits.

- **Continuous-flow abstraction.** Between a sender and a receiver there is
  conceptually one continuous flow. A DECLARE opens it, a RETIRE closes it;
  in between, membership is a stable observable.
- **DECLARE carries nflow (ppm of one flow).** A sender with a small work
  queue entry declares proportionally less: nflow such that its fair byte
  allocation in the coming delay windows equals what it actually has to
  send. A bulk sender declares one whole flow. Same rule, one code path.
  As implemented: with B0 the margin-derated bytes of one control round
  trip (2 * K), a transfer of W wire bytes below B0 declares
  ceil(W * 1e6 / B0) ppm, clamped to [1, one flow].
- **DECLAREs never expire.** There is no lease, no expiry timer, and no
  refresh re-DECLARE. Membership changes only through DECLARE (join) and
  RETIRE (leave, after the final ledger is fully resequenced). A repeat
  DECLARE for an active flow is an idempotent no-op that re-sends the
  current window feedback; a repeat for retired membership is counted and
  ignored, never a fault.
- **Feedback rides the resequenced stream.** The ACK of every forward
  packet that passes the resequencing buffer carries the receiver's
  membership count n_hat (in ppm) and the shared wire rate
  margin * C * 1e6 / n_hat_ppm, frozen once per dwnd window (section 1.1).
  Each sender scales the shared rate by its own declared nflow fraction, so
  the allocations of all members sum to margin * C exactly.
- **Startup does not wait.** The join gate (ACCEPT effective-time wait) is
  removed completely. A flow's first data window is the first window its
  packets can physically reach the receiver (dispatch + one_way), at its
  full reservation-ledger allocation immediately; there is no ramping and
  no negative feedback control anywhere (section 1.2). On a symmetric
  topology with a deterministic control loop the reservation timing is
  computable, so this is a reservation of future bandwidth, not a gamble.

### 1.1 The dwnd on the RTT timeline

The dwnd sits on the RTT timeline and the offsets are whole windows, not
noise, because RTT and dwnd are comparable.

- **Receiver-anchored windows.** The receiver (bottleneck authority)
  divides time into windows of length dwnd = K, the one-way control
  deadline: W(k) = [k * K, (k + 1) * K) on the receiver clock. At each
  window boundary it freezes a snapshot (n_hat_ppm, shared wire_rate). The
  ACK of every packet resequenced during W(k) carries that frozen snapshot
  plus the window it governs. The receiver never attaches the instantaneous
  live count: all ACKs within one window carry the same number, which is
  what makes the observable stable.
- **Sender-side deterministic mapping.** A packet launched by the sender at
  local t arrives at the receiver one_way = K later on the deterministic
  topology. Feedback observed in receiver window k returns to the sender
  one_way after that. The rate derived from snapshot k governs the sender
  transmissions that arrive at the receiver during window k + 2 onward
  (shift = ceil(RTT / dwnd) = 2). The sender keeps its own window clock
  offset earlier by one_way, so packets launched in sender-window j arrive
  within receiver-window j, and it applies snapshot k exactly at its window
  boundary j = k + 2, never mid-window. On the wire, the grant's
  effective_time_ps carries the governed receiver boundary (k + 2) * K; the
  sender converts to local time by subtracting one_way.

```
sender clock      t                t + RTT              t + 1.5 RTT
                  |                |                    |
     launch ------+                +-- feedback back    +-- new rate governs
                                       at sender            launches from here
receiver clock         t + 0.5 RTT                 t + 1.5 RTT
                       |                           |
     resequence -------+-- snapshot of window k    +-- boundary (k + 2) * K:
                           attached to every ACK       arrivals paced by
                           of window k                 snapshot k
```

- **Invariant.** Within any receiver window, every active sender paces from
  one common snapshot taken exactly two windows earlier, so the aggregate
  arrival rate is margin * C computed on membership that is two windows
  stale, which the margin absorbs for churn up to (1 - margin) of the
  membership per two windows.
- **Window accounting is sent <= reserved, with an explicit deficit
  carry.** A window's residue (reservation minus the integer packets sent)
  carries to the next window; a packet launches only when the accumulated
  reservation covers its full wire bytes. A packet is never sent against a
  partial-window remainder.
- **Pacing basis.** Every cadence derives from the allocated service rate,
  never the physical line rate: a whole flow alone paces 4160 B packets at
  4160 * 8 / (0.9 * C), and a 64 B flag displaces the calendar by 512 / r
  where r is the flow's allocated rate.

### 1.2 The resequencing window D and the bandwidth jitter product (maintainer-ruled)

D is not a tuning knob and is never shrunk for latency. Where classical
congestion control is sized by the bandwidth delay product, this design is
sized by the bandwidth jitter product (BJP): the only jitter (queueing
delay variation) in the system comes from FIFO buildup upstream of the
resequencing buffer, i.e. one FIFO at the receiving RNIC and possibly
several inside network switches on the path. For a given switch family the
FIFO depth is a generational constant, so jitter times bandwidth is a
constant, and

    D = Q_upstream / C

with Q_upstream the summed upstream FIFO depth. D must exceed the upstream
jitter; the resequencing buffer (PIFO) must be at least the upstream queue
size, which makes the resequencer itself lossless. The wait is independent
of where in the window a hit arrives: a hit arriving at the beginning of
its window experienced no jitter and waits the longest in the PIFO, a hit
delayed by the maximum jitter arrives just in time; release at ETA + D is
therefore the deterministic contract, and the constant +D on every flow's
completion is designed behavior, not overhead. Comparisons against
baselines that carry no resequencing discipline (rnic-nn) must treat D as
a known constant offset.

D is derived, never picked. With MTU_wire the maximum wire packet, S_max
the maximum sender count, C the bottleneck capacity and n_paths the number
of equal-cost paths through each intermediate stage, the upstream FIFO
depths are bounded by

    Q_final = (S_max - 1) * MTU_wire          (bottleneck egress / RX FIFO)
    Q_mid   = ceil(S_max / n_paths) * MTU_wire (per intermediate stage)

    D = (Q_final + n_mid_stages * Q_mid) * 8 / C

For the 64-node 400G two-tier reference Clos (S_max = 64, n_paths = 8,
two intermediate stages, MTU_wire = 4160 B): D = (63 + 16) * 4160 * 8 /
400e9 = 6.573 us. The historical 4.096 us default sits below this provable
bound and was only empirically sufficient thanks to PRBS scrambling; runs
must use the derived value unless the operator explicitly overrides it
with a bound of their own.

Egress-constrained resequencers: in the RNIC the resequencer output is
effectively unconstrained (delivery to memory); inside a network switch it
is bounded by the egress port bandwidth. When summed ingress exceeds
egress, the buffer can only replay the burst shape: the hit
lifetime-in-system profile at egress equals the ingress profile stretched
or shrunk by the ratio of summed ingress to egress bandwidth. With
well-implemented PRBS pacing, timestamp collisions should not produce
egress congestion in the first place.

### 1.3 The deterministic reservation ledger (decision C2, maintainer-ruled)

Cold start is full deterministic: no ramping and no negative feedback
control anywhere. Where full ex-ante knowledge is awkward, fixed topology
constants (bottleneck capacity C, maximum sender count S_max) are the
sanctioned fallback, known to both sender and receiver as run constants.

- **Shared reservation ledger.** One ledger per receiver, shared by senders
  and receiver inside the runtime. It models the ex-ante declaration
  schedule (the control plane) that the deterministic-loop design assumes:
  at DECLARE dispatch the sender registers (flow_id, nflow_ppm) and reads
  back its allocation; the receiver retires the entry when membership
  retirement commits. Sender launch schedules and receiver admission
  windows derive from the same ledger, so they agree by construction and
  recovery events are impossible in a valid run.
- **Allocation rule.** Per receiver window, flow i receives
  B(f_i) = B0 * f_i / sum(f_j over ledger-registered flows for that
  window), with B0 = margin * C * K / 8 bytes, an explicit deficit carry,
  and rotating discrete full-packet slots whenever a share is smaller than
  one packet. Tie-break order for same-window registrations: receiver
  window epoch, then nominal ETA tick, then seeded permutation of source
  rank, then source rank, then flow sequence; never actual arrival time.
- **No ramping.** A flow's first data window is the first window its
  packets can physically reach the receiver, at full ledger allocation
  immediately. The windowed k to k + 2 snapshot machinery of section 1.1 is
  the in-band transport of ledger state; it is feedforward, not feedback,
  and rates only ever change at window boundaries to ledger values.
- **Flag-only traffic (zero-data doorbells).** If an open flow exists
  between the same endpoints, the flag rides that flow's reservation (64 B
  charged to it, deferring data if the window is full); otherwise it uses a
  permanently reserved per-source bootstrap control slot sized from S_max
  and C, with no DECLARE, no membership, and no RETIRE.
- **Review-derived bounds.** The self-consistent fractional minimum is
  f >= S * N / (B0 - S) for control extent S and N registered flows;
  reservation domains are per-receiver; the reverse ACK calendar is
  reserved with the bound 64 * N_k + X_k <= C_rev * K / 8 per window.

### 1.4 Sender egress composition and the RTT rebalancer (maintainer-ruled)

Per-receiver ledgers alone cannot see a sender's port: fifteen receivers
can jointly grant one sender more than its egress, it launches late, and
packets land beyond ETA + D (observed deterministically in the mixed
all-to-all). The composition rule:

- **The sender knows its output bandwidth.** When pending work spans
  n_dest destinations whose granted sum could exceed the sender's egress
  capacity within a dwnd, the sender declares from its own fair share: the
  initial declaration toward each destination requests rate
  C_egress / n_dest (as a flow fraction, clamped to one flow), so the
  sender's own declarations never oversubscribe its port and no receiver
  grant is claimed that cannot be used.
- **Hunger is detected locally.** A destination may be oversubscribed
  (many senders targeting it) and grant less than the fair-share request.
  The sender sees this directly in the returned window snapshots (granted
  rate versus requested); the shortfall is idle egress capacity, i.e.
  sender egress bandwidth waste.
- **A slower loop rebalances at RTT granularity.** Once per RTT (2K), the
  sender redistributes the detected slack to destinations that were fully
  granted (no oversubscription observed), raising their declarations via
  NFLOW_UPDATE. This outer loop is deliberately negative feedback, scoped
  narrowly to reclaiming sender-egress waste; the per-window fast path
  stays feedforward and rates still change only at window boundaries to
  ledger values. Convergence is deterministic: same snapshots, same
  reallocation, every run.
- **NFLOW_UPDATE wire semantics** (previously an open definition, now
  load-bearing): a flag packet carrying (flow_id, new nflow_ppm),
  resequenced like every other packet, taking effect in the ledger and the
  window snapshots at the boundary of the window after its resequenced
  arrival. It changes a declaration's magnitude only; membership identity,
  join and retire semantics are untouched. A repeat with the same value is
  an idempotent no-op.

## 2. The impossible triangle: deterministic, line-rate, lossless

- **Per-packet spraying, not ECMP.** rnic-nn and rnic-cn spray packets
  across all equal paths (per-packet path round robin), so static
  flow-hash collisions are structurally impossible; the resequencing buffer
  (Ring-CAM, deterministic ETA admission, in-order release) restores
  per-flow ordering at the receiver. This is what lets the fabric be driven
  flat, where flow-hashed designs strand capacity on collided rails.
- **Line rate by reservation accounting.** Rates are restored to
  dwnd-boundary granularity, and within any receiver window the sum of
  sending rates equals the frozen reservation, margin * C on membership two
  windows stale. The bottleneck therefore runs at its reserved line rate by
  construction; the only surrendered capacity is the margin, which is
  precisely the headroom that absorbs two windows of membership churn.
- **Lossless by design, without PFC.** Loss is never a signal. Unlike
  loss-signal transports, and unlike ECN-based control (which still drops
  or leans on PFC), the per-dwnd invariant, i.e. bytes sent in a window
  never exceed bytes reserved for it, bounds every queue by construction,
  so packets are never dropped and PFC is never needed.
- **The triangle.** Determinism, line rate, and losslessness held
  simultaneously. Loss-signal designs give up losslessness, ECN designs
  give up determinism and still shed capacity to convergence, PFC-lossless
  designs give up line rate to head-of-line blocking. The comparators in
  this repo exist to exhibit exactly those sacrifices; DCQCN over
  single-rail ECMP is the expected-fail reference on all three corners.

## 3. Why the observable is stable

The receiver resequences every arrival (Ring-CAM admission by deterministic
ETA, in-order release). Membership mutations are serialized by the same
resequenced stream that carries data, and rate feedback is frozen once per
dwnd window, so every ACKed packet reflects a consistent (n_hat, C)
snapshot. Two consequences:

- No sender can act on a stale rate for longer than one dwnd beyond the
  deterministic two-window pipeline.
- Any two senders' views of n_hat for the same window are identical by
  construction; only the in-flight membership events of the last two
  windows differ, which the margin absorbs.

## 4. As-implemented discrepancies (burn-down record)

| # | Implementation before 2026-08-03 | Book | Status |
|---|---|---|---|
| D1 | ACCEPT join gate: sender waits until join_not_before = t0 + K + 2*dwnd before DATA | No wait at all; first window starts at the full ledger allocation | done (declare-and-go, ledger startup) |
| D2 | Leases: lease_expiry = m + K + 2*dwnd, LeaseExpired phase, refresh re-DECLARE path | DECLAREs never expire; delete the whole mechanism | behavior removed; vestigial wire fields remain (grant feedback_deadline_ps / lease_expiry_ps pinned to 0, kept for wire-format stability; removal tracked here) |
| D3 | Rate feedback only on one marked DATA packet per delay window | Feedback on the ACK of every resequenced forward packet, carrying the window-frozen snapshot of section 1.1 | done (windowed snapshots, k + 2 boundary effect) |
| D4 | fractional nflow behind a CLI flag (-rnic_cn_fractional_nflow) | One algorithm, no mode switch; the fractional rule is the only rule | done (flag removed, rule unconditional) |
| D5 | n_hat integer flow counts (pre-2026-08-03) | nflow and n_hat in ppm of a flow | done (ppm-native controller, bit-identical for whole flows) |

Evidence that D1/D2 were wrong as shipped: with the control deadline
calibrated to the deterministic topology (4.5 us instead of the loose
10 us default), the stock runtime aborted deterministically (20/20) with
"re-DECLARE targets inactive membership" on a 32-pair permutation at
1 MiB: a lease expired mid-flow, the refresh re-DECLARE raced the flow's
own retirement, and the receiver hard-threw. Under the book's design the
race cannot exist because neither the lease nor the refresh exists.

## 5. Validation contract

The acceptance bar for this endpoint (maintainer, 2026-08-03): per-flow
FCT within 2x of rnic-nn on descending flow-size sweeps (permutation and
incast), usually within 20 percent once this book's short-flow behavior is
implemented. DCQCN is the expected-fail comparator and is not mitigated;
rnic-nn / rnic-nn-fluid are the baselines all runs normalize to.

The observable form of the per-dwnd invariant is that Ring-CAM late
admissions and gap NACKs are zero in every valid run, including startup,
so any nonzero count is a hard failure, not noise.

## 6. Mechanism backlog

Numbered gaps between this book and the current implementation. Items 1
through 8 come from the external review (the review's full 12-item corner
case list is held in the maintainer's review thread; the items below are
the ones already adjudicated); items 9 and 10 are open wire-semantic
definitions awaiting the maintainer.

1. Rotating discrete full-packet slots for shares smaller than one packet
   (the runtime currently rate-paces; sub-packet shares rely on the pacer's
   whole-packet launches rather than an explicit slot calendar).
2. Ledger-derived Ring-CAM ETAs (admission windows still derive from
   launch time plus calibrated no-load transit, not from the ledger slot
   schedule; transient join overlap is absorbed by margin headroom instead
   of being impossible by construction).
3. Same-window registration tie-break order (receiver window epoch, ETA
   tick, seeded permutation, source rank, flow sequence) as executable
   arbitration rather than batch-registration order.
4. Explicit per-window byte ledgers with deficit carry in the TX port (the
   PRBS pacer's whole-packet launches at the allocated rate approximate
   this; the carry is not yet an auditable counter).
5. Enforcement of the fractional minimum f >= S * N / (B0 - S).
6. Reservation of the reverse ACK calendar with the bound
   64 * N_k + X_k <= C_rev * K / 8, plus its violation counter.
7. Ledger-vs-snapshot skew accounting for staggered joins: an incumbent
   paces its old allocation until boundary k + 2 while the joiner already
   holds its ledger share; bound and document the transient overlap.
8. Flag-only bootstrap control slots (zero-data doorbells still traverse
   DECLARE/RETIRE membership today).
9. Open (maintainer): DECLARE+data piggyback wire semantics for small
   WQEs.
10. Open (maintainer): a distinct nflow-update flag packet for mid-flow
    WQE arrivals, since re-DECLAREs no longer exist.
