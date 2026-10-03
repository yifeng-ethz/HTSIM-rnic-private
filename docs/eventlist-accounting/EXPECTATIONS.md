# EventList pending-traffic accounting expectations

This is a pre-implementation freeze proposal. No correction or new test has
been run under this document yet. The correction is confined to the generic
EventList and generic tests; it does not establish a transport, topology,
hardware-memory, or unrestricted campaign guarantee.

## Source and observed defect

The proposed base is public main
`3fe2b400d0d376dbf8ad97ba5aa7f2bd223ca58c`, verified from the public remote
on 2026-10-03. Its EventList bytes equal those at
`1aa7b9d33227cc55e6620c9fc3c8942aad90ea68`.

| Base input | SHA-256 |
| --- | --- |
| `htsim/sim/eventlist.cpp` | `c2c5c744cb7ff755d1ba9f7b0334e605e229d14c24419296e1606088f0376a66` |
| `htsim/sim/eventlist.h` | `4c285b39793a354cfd4f4e4dc11d70451dbf005df958dfb52b7916373c81ec1b` |
| `htsim/sim/eventlist_microphase_test.cpp` | `c983fe6eff14bff1036568f69387f2b408c6d80570f43cb0bb70ce8b18ca8eab` |
| `htsim/sim/CMakeLists.txt` | `1d6a9d5f69d6b2aa8d4dd8ec6fb76f1e854433e49924e8ddab6d2cd179ebd9c7` |

At `eventlist.cpp:103-111`, plain insertion adds a pending record without
incrementing `_trafficeventcount`. Dispatch at lines 79-82 and each successful
cancellation at lines 135-180 decrement traffic records. Handle insertion at
lines 115-126 increments. Thus one plain traffic insertion followed by one
dispatch leaves the queue empty but the signed counter at -1. Repetition can
reach signed underflow even with only one live pending record. This defect is
already present in commit `896cc76`; the later next-event-time addition did
not introduce it.

The header calls the field the number of events that are not loggers/samplers.
All checked-in `isTraffic()` overrides are constant: five RNIC runtime
overrides return true, thirteen logger overrides plus Clock and the TM3 ingress
arbiter return false, and the EventSource default returns true. A repository
search finds no `setTraffic` API or mutable classification setter. Accounting
continues to require a stable classification while a source has pending
records. Changing classification or destroying a still-pending source remains
outside the supported contract; this patch does not silently snapshot or
reclassify records.

## Corrected contract and preserved policies

`trafficEventCount()` remains an `int` with the same signature. In a valid
state it equals the multiplicity of pending map entries whose source is
traffic, in the range `[0, INT_MAX]`. It is a live-record count, independent
of how many events have previously executed or how far simulation time has
advanced. Immediate TriggerTarget work is not a scheduled EventSource record
and does not contribute to this count.

| Operation | Accounting and ownership expectation |
| --- | --- |
| Accepted plain, relative, or handle traffic insertion | Check representability before insertion; increment once only after successful map insertion. If insertion allocation throws, the counter is unchanged. |
| Accepted nontraffic insertion | Insert the record without changing the count. |
| Rejected insertion | No pending record or counter change; handle insertion returns `nullHandle()`. |
| Dispatch | Check and remove exactly the selected entry, decrement once, then update time and execute the callback. The executing entry is absent and uncharged during the callback; recursive insertions are new pending ownership. |
| `cancelPendingSource` | Remove only the first matching record in map order; duplicate records remain charged. An absent source is a no-op. |
| `cancelPendingSourceByTime` | Remove only one matching record at that exact time; unrelated same-time records remain. An absent required timer retains its existing abort contract. |
| `cancelPendingSourceByHandle` | A still-valid handle identifies exactly one record, including duplicate source/time records. The caller still owns handle validity; expired or erased iterators remain unsupported. |
| Reschedule | Preserve the existing cancel-one then plain-insert behavior. A new time rejected by the end-time filter removes the old record without replacing it. This operation is not being promoted to transactional replacement. |
| Trigger dispatch | Remove the trigger from its own queue and call it at `now()`. Its later source insertions count normally. |

The existing end-time filter remains exclusive: when the configured end time
is nonzero, an insertion is accepted only for `when < endtime`. Changing the
end time does not retroactively prune queued records. Time is still monotone,
equal-time callbacks and immediate triggers remain legal, and same-time
ordering is not given a new scheduling guarantee.

The two insertion policies intentionally remain distinct. Plain insertion
accepts nontraffic sources within the end-time filter. Handle insertion accepts
a nontraffic source only when there is pending traffic or `now() == 0`.
Correcting plain accounting can make that existing handle predicate true when
plain traffic is actually pending. No new global stop-on-quiescence rule is
introduced; a plain self-rescheduling nontraffic timer can still require a
finite end time or explicit cancellation.

Traffic insertion at `INT_MAX` throws `std::overflow_error` before inserting
or exposing a handle. Traffic insertion from a negative count and traffic
removal from a count <= 0 throw `std::logic_error` before changing the pending
map, counter, clock, triggers, or invoking a callback. These checks are active
in release builds. Successfully dispatched callbacks that themselves throw
remain removed and uncharged; this patch does not roll back callback effects.
Counter guards do not make generic queues or caller callbacks transactional if
they mutate their own state before requesting a timer.

## Frozen meaningful checks

Add one CMake-discovered GoogleTest target, `eventlist_accounting_test`, with
18 directed groups. Each ordinary mutation is checked against an independent
scan of `getPendingSources()` and expected identity/time/multiplicity. Callback
observations and clock observations supplement the counter, rather than using
the counter as its own oracle.

| Group | Frozen stimulus and expected outcome |
| --- | --- |
| 1. Startup nontraffic handle | At initially empty time zero, the existing handle predicate accepts a nontraffic event; count remains zero through dispatch. |
| 2. Mixed insertion multiplicity | Plain, relative and handle insertion, including the same traffic source/time more than once, contribute once per actual record; nontraffic contributes zero. |
| 3. Source cancellation | Three records for one source at two times plus an unrelated record; cancel removes only the earliest matching record. Absent-source cancellation changes nothing. |
| 4. Time cancellation | Duplicate matching records and an unrelated source at the same timestamp; exactly one matching record is removed. |
| 5. Handle cancellation | Erase the second of identical source/time entries using its actual handle; the first and unrelated entries survive with their handles valid. |
| 6. Reschedule | Replace one of multiple records, then exercise a rejected end-time replacement; assert the preserved cancel-one behavior and exact remaining timestamps. |
| 7. End-time boundary | Plain, relative and handle attempts before, at and after an exclusive end time; rejected attempts leave map, count, time and callbacks unchanged. An already queued event survives a later end-time change. |
| 8. Nontraffic policy | After positive time with no traffic, plain nontraffic insertion succeeds and handle nontraffic insertion rejects. Pending plain traffic enables the existing handle predicate; its removal disables it. |
| 9. Recursive same-time traffic | The selected record is absent and count zero inside its callback, which installs plain and handle descendants at the same time plus nontraffic work. All descendants settle with exact counts. |
| 10. Immediate triggers | Duplicate immediate triggers do not count; a trigger recursively schedules traffic and a trigger descendant at the same time. Queue membership, count and time are independently asserted. |
| 11. Real Queue/Pipe and sampler | Eight real packets traverse a generic 8 Gb/s Queue and 2 us Pipe while a finite nontraffic sampler runs. Assert all packet arrivals, causal service/propagation times, and actual pending-source counts across recursive Queue/Pipe callbacks. |
| 12. Long balanced lifetime | A source executes 65,536 self-rescheduled traffic callbacks spaced 100 ms apart, exceeding one second while keeping at most one traffic record live. Count is zero before each new callback insertion and returns to zero at drain. This is a finite generic diagnostic, not a serving workload. |
| 13. Throwing callback | A dispatched traffic callback throws while a later event remains queued; the dispatched entry stays removed, the later record stays charged, and time reflects the actual dispatch. |
| 14. Plain overflow | Seed the counter at `INT_MAX` through the test peer and attempt real plain traffic insertion; catch overflow and verify identical pending identities, time, trigger observations and count. |
| 15. Handle overflow | The same boundary through handle insertion; no handle or pending record is exposed on failure. |
| 16. Removal underflow | Seed count zero and `INT_MIN` with one real traffic record and exercise dispatch plus each of the three cancellation methods. Logic errors preserve the actual entry, handle, clock and callback count; restore the seed before normal cleanup. |
| 17. Adjacent counter limits | Controlled `INT_MAX-1` insertion/removal returns exactly to its seed, nontraffic work does not consume a count slot, and negative-count traffic insertion rejects without mutation. |
| 18. Mixed operation sequence | A fixed deterministic 512-operation sequence across eight traffic/nontraffic sources uses duplicate times, all cancellation APIs, rescheduling and dispatch; an independent pending-entry ledger and raw-map census verify every operation and final drain. |

Boundary seeding uses one narrowly named friend test peer; it adds no public
runtime mutator or reset API. The peer restores synthetic counter seeds before
ordinary cleanup. Normal tests start with a drained singleton and reset only
empty-list test clock/end-time state, so test ordering does not depend on a
previous test's final simulated time. Overflow tests do not allocate billions
of map entries or present their synthetic seed as a valid physical census.

After this expectations-only commit, first build an isolated public-API-only
reproducer against unchanged base sources: insert one plain traffic record,
observe raw queue size one but count zero, dispatch once, then observe an empty
queue but count -1. It uses no friend injector or modified header. Archive its
source hashes, command, exit and mismatch under an ignored build directory.
Then implement the correction and
require all 18 groups, the existing full CTest suite, and the 18 focused groups
with unsuppressed AddressSanitizer/UndefinedBehaviorSanitizer. Archive exact
source, compiler/link inputs and outputs; preserve any failures. Windows CI is
required before upstream merge, not claimed by a local Linux result.

## Owned scope and remaining work

The intended patch owns only this document, `htsim/sim/eventlist.cpp`,
`htsim/sim/eventlist.h`, `htsim/sim/eventlist_accounting_test.cpp`, and the
single test-target addition in `htsim/sim/CMakeLists.txt`. Existing test bytes,
other scheduling policies, packet models, transport profiles and topology
code are unchanged. Any resulting completion or admission differences are
reported honestly against the unchanged base.

This correction does not fix the process-global Logged registry, make stale
iterator handles safe, bound pending-source memory, or validate arbitrary
runtime subclasses that change traffic classification while queued. The
signed counter has an explicit live capacity; a balanced lifetime no longer
consumes it. Simulator time and other identity/counter limits still apply.
Adoption by an external pinned consumer requires its own source-pin and
compatibility review; no existing evidence is reinterpreted under new bytes.
