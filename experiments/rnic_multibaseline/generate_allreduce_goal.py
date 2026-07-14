#!/usr/bin/env python3
"""Generate paired flat and DAG recursive-doubling all-reduce GOAL schedules.

The communication graph is fixed across modes.  ``flat`` removes application
dependencies so every point-to-point transfer is eligible at time zero; ``dag``
requires each rank to finish both directions of phase k before phase k+1.
Physical RNIC serialization and network control remain active in both modes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
from pathlib import Path


def positive_power_of_two(text: str) -> int:
    value = int(text)
    if value < 2 or value & (value - 1):
        raise argparse.ArgumentTypeError("must be a power of two >= 2")
    return value


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def rank_mapping(rank_count: int, seed: int) -> list[int]:
    mapping = list(range(rank_count))
    random.Random(seed).shuffle(mapping)
    return mapping


def build_goal(rank_count: int, payload_bytes: int, seed: int, mode: str) -> str:
    physical = rank_mapping(rank_count, seed)
    phase_count = int(math.log2(rank_count))
    per_rank: list[list[str]] = [[] for _ in range(rank_count)]

    for logical_rank in range(rank_count):
        src = physical[logical_rank]
        label_index = 1
        previous_send = ""
        previous_recv = ""
        for phase in range(phase_count):
            partner = physical[logical_rank ^ (1 << phase)]
            send_label = f"l{label_index}"
            label_index += 1
            recv_label = f"l{label_index}"
            label_index += 1
            tag = phase

            per_rank[src].append(
                f"{send_label}: send {payload_bytes}b to {partner} tag {tag} "
                "cpu 0 nic 0"
            )
            per_rank[src].append(
                f"{recv_label}: recv {payload_bytes}b from {partner} tag {tag} "
                "cpu 0 nic 0"
            )
            if mode == "dag" and phase > 0:
                per_rank[src].append(f"{send_label} requires {previous_send}")
                per_rank[src].append(f"{send_label} requires {previous_recv}")
                per_rank[src].append(f"{recv_label} requires {previous_send}")
                per_rank[src].append(f"{recv_label} requires {previous_recv}")
            previous_send = send_label
            previous_recv = recv_label

    lines = [f"num_ranks {rank_count}", ""]
    for rank, operations in enumerate(per_rank):
        lines.append(f"rank {rank} {{")
        lines.extend(operations)
        lines.append("}")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ranks", type=positive_power_of_two, default=64)
    parser.add_argument("--bytes", type=positive_int, default=32 * 1024 * 1024)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--mode", choices=("flat", "dag"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()

    goal = build_goal(args.ranks, args.bytes, args.seed, args.mode)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(goal, encoding="utf-8")

    if args.metadata:
        phase_count = int(math.log2(args.ranks))
        metadata = {
            "schema": "rnic-allreduce-goal-v1",
            "algorithm": "recursive-doubling",
            "mode": args.mode,
            "rank_count": args.ranks,
            "phase_count": phase_count,
            "payload_bytes_per_transfer": args.bytes,
            "directed_flow_count": args.ranks * phase_count,
            "rank_mapping_seed": args.seed,
            "rank_to_physical_node": rank_mapping(args.ranks, args.seed),
            "goal_sha256": hashlib.sha256(goal.encode("utf-8")).hexdigest(),
        }
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )


if __name__ == "__main__":
    main()
