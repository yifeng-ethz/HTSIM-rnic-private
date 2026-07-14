#!/usr/bin/env python3
"""Generate the eight-flow 5 ms join / finite-byte exit workload.

Flow i joins at i*5 ms.  Payloads are derived from ideal destination-link
processor sharing so that, after all eight flows are active, flows complete in
reverse join order at 5 ms intervals.  Flow 0, which joined first, is therefore
the final flow to complete.  The simulator still determines the actual
completion time; no stop event or exit timer is emitted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from fractions import Fraction
from pathlib import Path


DEFAULT_SOURCES = (0, 8, 16, 24, 32, 40, 48, 56)
DEFAULT_DESTINATION = 63
DEFAULT_NODE_COUNT = 64
DEFAULT_INTERVAL_NS = 5_000_000
DEFAULT_LINK_BPS = 400_000_000_000


def positive_int(text: str) -> int:
    value = int(text, 10)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def ideal_schedule(
    flow_count: int, interval_ns: int, link_bps: int
) -> tuple[list[int], list[int], list[int]]:
    """Return join ns, target completion ns, and ceil-sized payload bytes."""

    joins = [index * interval_ns for index in range(flow_count)]
    # The last join is at (N - 1) intervals.  Begin the target completion
    # sequence one interval later, but retire in reverse join order so the
    # first/long flow is the final survivor.
    exits = [
        (2 * flow_count - 1 - index) * interval_ns
        for index in range(flow_count)
    ]
    events = sorted(set(joins + exits))
    byte_rate = Fraction(link_bps, 8)
    payloads: list[int] = []
    for flow in range(flow_count):
        service = Fraction(0, 1)
        for begin, end in zip(events, events[1:]):
            if begin < joins[flow] or end > exits[flow]:
                continue
            active = sum(
                1
                for candidate in range(flow_count)
                if joins[candidate] <= begin < exits[candidate]
            )
            if active <= 0:
                raise AssertionError("ideal schedule has an empty service interval")
            service += byte_rate * Fraction(end - begin, 1_000_000_000) / active
        payloads.append(math.ceil(service))
    return joins, exits, payloads


def build_goal(
    node_count: int,
    sources: tuple[int, ...],
    destination: int,
    interval_ns: int,
    link_bps: int,
) -> tuple[str, dict[str, object]]:
    if len(set(sources)) != len(sources):
        raise ValueError("sources must be distinct")
    if destination in sources:
        raise ValueError("destination cannot also be a source")
    if any(node < 0 or node >= node_count for node in (*sources, destination)):
        raise ValueError("source or destination is outside the node domain")

    joins, exits, payloads = ideal_schedule(len(sources), interval_ns, link_bps)
    per_rank: list[list[str]] = [[] for _ in range(node_count)]
    for flow, (source, join_ns, payload_bytes) in enumerate(
        zip(sources, joins, payloads)
    ):
        send_label = "l1" if join_ns == 0 else "l2"
        if join_ns > 0:
            per_rank[source].append(f"l1: calc {join_ns}")
        per_rank[source].append(
            f"{send_label}: send {payload_bytes}b to {destination} tag {flow} "
            "cpu 0 nic 0"
        )
        if join_ns > 0:
            per_rank[source].append(f"{send_label} requires l1")
        per_rank[destination].append(
            f"l{flow + 1}: recv {payload_bytes}b from {source} tag {flow} "
            "cpu 0 nic 0"
        )

    lines = [f"num_ranks {node_count}", ""]
    for rank, operations in enumerate(per_rank):
        lines.append(f"rank {rank} {{")
        lines.extend(operations)
        lines.append("}")
        lines.append("")
    goal = "\n".join(lines)

    metadata: dict[str, object] = {
        "schema": "rnic-join-exit-goal-v2",
        "node_count": node_count,
        "source_nodes": list(sources),
        "destination_node": destination,
        "flow_count": len(sources),
        "join_interval_ns": interval_ns,
        "join_time_ns": joins,
        "ideal_target_completion_time_ns": exits,
        "ideal_exit_flow_order": sorted(
            range(len(sources)), key=lambda flow: (exits[flow], flow)
        ),
        "payload_bytes": payloads,
        "payload_rule": (
            "ceil integral of C/N(t) from join to target completion; "
            "actual completion is network-driven with no exit timer"
        ),
        "ideal_destination_link_bps": link_bps,
        "overall_plot_window_ns": [0, max(exits) + interval_ns],
        "join_zoom_window_ns": [joins[1] - interval_ns // 2, joins[1] + interval_ns // 2],
        # At the penultimate target completion only flows 0 and 1 remain;
        # this window is the requested two-to-one divergence zoom.
        "exit_zoom_window_ns": [
            sorted(exits)[-2] - interval_ns // 2,
            sorted(exits)[-2] + interval_ns // 2,
        ],
        "goal_sha256": hashlib.sha256(goal.encode("utf-8")).hexdigest(),
    }
    return goal, metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--interval-ns", type=positive_int, default=DEFAULT_INTERVAL_NS)
    parser.add_argument("--link-bps", type=positive_int, default=DEFAULT_LINK_BPS)
    args = parser.parse_args()

    goal, metadata = build_goal(
        DEFAULT_NODE_COUNT,
        DEFAULT_SOURCES,
        DEFAULT_DESTINATION,
        args.interval_ns,
        args.link_bps,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(goal, encoding="utf-8")
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
