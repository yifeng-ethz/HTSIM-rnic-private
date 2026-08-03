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
- **DECLAREs never expire.** There is no lease, no expiry timer, and no
  refresh re-DECLARE. Membership changes only through DECLARE (join) and
  RETIRE (leave, after the final ledger is fully resequenced).
- **Feedback rides the resequenced stream.** The ACK of every forward
  packet that passes the resequencing buffer carries the receiver's current
  membership count n_hat (in ppm) and the bottleneck wire capacity C (known
  from the declared topology). n_hat decreases continuously as flows retire
  and steps up when a resequenced DECLARE admits a joiner. Because feedback
  is attached to every resequenced packet, the sender's observable is
  stable and fresh within one delay window (dwnd) at all times.
- **Deterministic dwnd accounting.** Sender-side send windows are indexed
  from the DECLARE dispatch time in units of dwnd. The feedback carried by
  ACKs arriving during window k governs the byte budget of window k+1:
  fair_bytes(k+1) = margin * C * nflow_own / n_hat_ppm * dwnd / 8. The
  sender scrambles packet launch times inside each window (the shared PRBS
  pacer), which is what makes "never oversend, never undersend within a
  dwnd" hold exactly rather than on average.
- **Startup does not wait.** The join gate (ACCEPT effective-time wait) is
  removed completely. The sender starts transmitting in its first window at
  its declared fraction of the granted bottleneck share; the resequencing
  buffer absorbs the at-most-one-round-trip of optimism, and the first
  returning ACK carries the true n_hat that governs the next window. On a
  symmetric topology with a deterministic control loop the reservation
  timing is computable, so this is a reservation of future bandwidth, not a
  gamble.

## 2. Why the observable is stable

The receiver resequences every arrival (Ring-CAM admission by deterministic
ETA, in-order release). Membership mutations are serialized by the same
resequenced stream that carries data, so every ACKed packet reflects a
consistent (n_hat, C) snapshot. Two consequences:

- No sender can act on a stale rate for longer than one dwnd.
- Any two senders' views of n_hat for the same window differ by at most the
  in-flight membership events of that window, which the margin absorbs.

## 3. As-implemented discrepancies (to be burned down)

| # | Implementation today | Book | Status |
|---|---|---|---|
| D1 | ACCEPT join gate: sender waits until join_not_before = t0 + K + 2*dwnd before DATA | No wait at all; first window starts at the declared fraction | open |
| D2 | Leases: lease_expiry = m + K + 2*dwnd, LeaseExpired phase, refresh re-DECLARE path | DECLAREs never expire; delete the whole mechanism | open |
| D3 | Rate feedback only on one marked DATA packet per delay window | Feedback on the ACK of every resequenced forward packet | open |
| D4 | fractional nflow behind a CLI flag (-rnic_cn_fractional_nflow) | One algorithm, no mode switch; the fractional rule is the only rule | open |
| D5 | n_hat integer flow counts (pre-2026-08-03) | nflow and n_hat in ppm of a flow | done (ppm-native controller, bit-identical for whole flows) |

Evidence that D1/D2 are wrong as shipped: with the control deadline
calibrated to the deterministic topology (4.5 us instead of the loose
10 us default), the stock runtime aborts deterministically (20/20) with
"re-DECLARE targets inactive membership" on a 32-pair permutation at
1 MiB: a lease expires mid-flow, the refresh re-DECLARE races the flow's
own retirement, and the receiver hard-throws. Under the book's design the
race cannot exist because neither the lease nor the refresh exists.

## 4. Validation contract

The acceptance bar for this endpoint (maintainer, 2026-08-03): per-flow
FCT within 2x of rnic-nn on descending flow-size sweeps (permutation and
incast), usually within 20 percent once this book's short-flow behavior is
implemented. DCQCN is the expected-fail comparator and is not mitigated;
rnic-nn / rnic-nn-fluid are the baselines all runs normalize to.
