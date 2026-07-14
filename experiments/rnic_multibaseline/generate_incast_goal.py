#!/usr/bin/env python3
"""Generate simultaneous finite-byte incasts for the 64-node Clos.

The width is the number of directed flows, not necessarily the number of
distinct source RNICs.  A 64-node topology has at most 63 remote sources for
one destination, so the 64-flow point uses 63 distinct sources and a second
flow from one of those RNICs.  This keeps the physical node count fixed and
records the distinction explicitly instead of implying a 65th endpoint.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import generate_allreduce_goal


DEFAULT_NODE_COUNT = 64
DEFAULT_PAYLOAD_BYTES = 32 * 1024 * 1024
DEFAULT_WIDTHS = (2, 4, 8, 16, 32, 64)


def positive_int(text: str) -> int:
    value = int(text, 10)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def placement(node_count: int, width: int, seed: int) -> tuple[list[int], int]:
    if node_count < 3:
        raise ValueError("incast requires at least two sources and one destination")
    if width < 1 or width > node_count:
        raise ValueError("flow width must be in [1, node_count]")
    shuffled = generate_allreduce_goal.rank_mapping(node_count, seed)
    destination = shuffled[-1]
    remote_sources = shuffled[:-1]
    sources = [remote_sources[index % len(remote_sources)] for index in range(width)]
    return sources, destination


def build_goal(
    node_count: int, width: int, payload_bytes: int, seed: int
) -> tuple[str, dict[str, object]]:
    if payload_bytes <= 0:
        raise ValueError("payload must be positive")
    sources, destination = placement(node_count, width, seed)
    per_rank: list[list[str]] = [[] for _ in range(node_count)]
    label_number = [1] * node_count
    for tag, source in enumerate(sources):
        send_label = f"l{label_number[source]}"
        label_number[source] += 1
        recv_label = f"l{label_number[destination]}"
        label_number[destination] += 1
        per_rank[source].append(
            f"{send_label}: send {payload_bytes}b to {destination} tag {tag} "
            "cpu 0 nic 0"
        )
        per_rank[destination].append(
            f"{recv_label}: recv {payload_bytes}b from {source} tag {tag} "
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
        "schema": "rnic-incast-goal-v1",
        "node_count": node_count,
        "flow_count": width,
        "distinct_source_count": len(set(sources)),
        "source_nodes": sources,
        "destination_node": destination,
        "payload_bytes_per_flow": payload_bytes,
        "start_rule": "all sends are eligible at time zero",
        "placement_seed": seed,
        "width_label": "directed flow count",
        "goal_sha256": hashlib.sha256(goal.encode("utf-8")).hexdigest(),
    }
    return goal, metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=positive_int, required=True)
    parser.add_argument("--bytes", type=positive_int, default=DEFAULT_PAYLOAD_BYTES)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    args = parser.parse_args()
    goal, metadata = build_goal(
        DEFAULT_NODE_COUNT, args.width, args.bytes, args.seed
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
