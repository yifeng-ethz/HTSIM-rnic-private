# Single-leaf and two-tier Clos topology primitive

`RnicBreakoutTopology` builds cached explicit htsim Packet/Route paths through
independent source lanes, an ingress propagation/processing pipe, a finite
switch egress lane and an egress propagation pipe. Configuration supplies
endpoint count, lanes, rate, queue capacities and delays. Each direction has
its own source and egress serializer. There is no extra aggregate serializer
or unlimited egress service. Destinations bind to one stable PacketSink across
lanes. Drain routed packets before destroying the topology and sinks.

The optional `endpoints_per_leaf` and `spines` fields enable a two-tier
leaf/spine Clos. Both fields default to zero, preserving the single-leaf
paths and resource catalog. A same-leaf route uses the same two serializers
and delays. An inter-leaf route adds a leaf-to-spine serializer, a
spine-to-destination-leaf serializer, two inter-switch propagation delays and
two switch processing delays. These are actual finite packet queues and
Pipes. No scalar topology multiplier prices the packet a second time.

`route()` takes an explicit optional spine index separately from both
physical lane indices. Leaf uplinks are shared by `(leaf, spine, lane)`;
spine downlinks are shared by `(spine, leaf, lane)`. Opposite directions have
independent serializers. Queue inspection names each physical resource.
The application owns spine selection, declared forwarding groups and any
source pacing. These primitives do not infer a physical switch hash or
guarantee diversity. Four endpoints per leaf and four spines have equal
aggregate downlink and uplink capacity at the configured common lane rate;
two spines have 2:1 oversubscription and one has 4:1.

With 1538 wire bytes, 100000 ps propagation per link and 200000 ps processing
per switch, unloaded same-leaf/inter-leaf transit is exactly
1384320/2968640 ps at 25 Gb/s per lane, and 2860800/5921600 ps at 10 Gb/s.
The source-already-serialized suffix removes exactly one serializer.
`noQueueTransitPs(bytes, source, destination)` includes the selected path's
serializer and delay count; the older endpoint-independent overload retains
its single-leaf meaning. Queue occupancy uses the explicit storage charge,
which can differ from wire bytes. These model inputs require independent
hardware qualification before serving as physical bounds.

`RnicFinitePriorityQueue` serves HIGH, MID then LOW with strict priority at
packet boundaries. Service is nonpreemptive and occupancy includes the active
frame. Capacity uses the caller's required stored/charged-byte function independently
of wire serialization. Occupancy includes the active frame; callers must map
actual switch allocation units before using a physical SRAM capacity. Overflow drops the arriving whole frame, calls an optional observation
hook before freeing it and increments a counter. Packet size is serialized as
wire bytes; callers must include the overhead their experiment promises.
The exact rational serializer retains fractional boundaries across a busy
period instead of adding a rounding tick for every packet.

`hashedRoute` requires an explicit function mapping endpoint pair and UDP
source port to an index in an explicitly supplied forwarding group.
Group members name reachable physical destination lanes according to the
experiment's routing and addressing configuration. A breakout cable creates
independent physical ports, not an ECMP group; a one-member group remains on
its sole lane for every source port. A constant tuple can collide; entropy is
not a diversity guarantee, and an unknown physical hash provides no bounded
interior service proof. Lane selection never changes an endpoint's protocol
identity. Callers that already serialized the source can request a route
suffix that omits exactly that source queue.

This primitive installs no transport, reservation authority, global clock,
memory controller or application ABI. It is below existing runtime/port
seams. Independent tests check exact unloaded transit, simultaneous lanes,
source suffixes, explicit hash collisions and overflow identities,
nonpreemptive priority and rational serialization.

Finite capacity bounds admitted stored occupancy, not a low-priority frame's
residence under continuing higher-priority arrivals. A queue drain calculation
assumes no new arrivals. Timed DATA service needs a policed control envelope
and positive residual service or an explicit class reservation calendar.
The continuing-control test holds occupancy below a fixed capacity while
delaying DATA beyond capacity divided by link rate; extending the high-priority
stream extends that delay. Strict priority alone provides no DATA deadline.
