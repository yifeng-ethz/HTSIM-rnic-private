# Retry timeout lookup: frozen identity expectations

HTSIM-40's protected-control path exercises the existing data retry engine.
Cancelling the deadlines for one acknowledged logical packet must not require
scanning unrelated packets. Add a lookup index into the existing deadline
queue. The deadline queue remains the timing authority; the index contains
only references to its entries and never schedules or advances a packet.

This change has no intended modeled behavior. Its exact oracle is the
completion CSV and failure class of the prior control-headroom implementation
at commit 48851d161de81cf155aed65a3420c740abe53875. The index must preserve every
packet time, random draw, byte count and completion order. Per-packet timeout
supersession, cancellation through an attempt number, duplicate attempts and
due-time removal must leave the index consistent with the authoritative queue.
No retry limit, deadline, buffer size, receiver rule or default changes.

Before running the indexed implementation, freeze these comparisons: widths
8, 16, 32, 64 at 400 and 200 Gbit/s, recovery none and headroom; pipeline
depths 2, 4, 8 on both attachments at both existing oversubscription levels,
with expert widths 0, 8, 32. Reuse the control_recovery_v1 inputs unchanged.
Every prior completed CSV must be byte-identical, including newly completed
width-64 headroom cells. Prior protocol failures must retain their failure
class. Operationally timed-out attempts contain no modeled outcome and cannot
serve as a latency oracle; retain their inputs and wall-time record.

Native tests exercise multiple packet ranges and attempt numbers, including
cancellation of one packet while others remain armed, a later sender timeout,
and quiescent index emptiness. Existing timeout and duplicate-recovery fixtures
remain mandatory. Queue/index conservation is fatal and unscored. No fatal
guard is survivable. The original control study's physical floors, unbounded
physical ceilings and predictive bands remain frozen and unchanged. Elapsed
host execution time is a performance diagnostic, never a network metric.
