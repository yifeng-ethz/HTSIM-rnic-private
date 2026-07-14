#!/usr/bin/env python3
"""Run paired simultaneous incasts and summarize receiver payload goodput."""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Sequence

import generate_incast_goal
import run_multibaseline as runner


SCHEMA_VERSION = "rnic-incast-sweep-v1"


def generate_artifact(
    output_root: Path, width: int, payload_bytes: int, seed: int
) -> runner.WorkloadArtifact:
    goal, metadata = generate_incast_goal.build_goal(
        64, width, payload_bytes, seed
    )
    metadata = dict(metadata)
    metadata.update(
        {
            "orchestration_schema": SCHEMA_VERSION,
            "workload_name": f"incast-{width}",
        }
    )
    directory = output_root / "workloads" / f"incast-{width}" / f"seed-{seed}"
    return runner.WorkloadArtifact(
        name=f"incast-{width}",
        seed_key=f"seed-{seed}",
        goal_path=directory / "workload.goal",
        metadata_path=directory / "workload.json",
        binary_path=directory / "workload.bin",
        goal_text=goal,
        metadata=metadata,
        transfers=runner.expected_transfers(goal),
    )


def payload_goodput_gbps(summary: runner.CompletionSummary) -> float:
    begin = min(row.start_time_ps for row in summary.rows)
    finish = max(row.completion_time_ps for row in summary.rows)
    if finish <= begin:
        raise runner.OrchestrationError("incast completion interval is not positive")
    payload_bytes = sum(row.transfer.payload_bytes for row in summary.rows)
    return payload_bytes * 8.0 * 1_000.0 / (finish - begin)


def write_summary(
    output_root: Path,
    results: dict[tuple[int, int, str], runner.CompletionSummary],
    widths: Sequence[int],
    seeds: Sequence[int],
    schemes: Sequence[str],
) -> None:
    rows: list[dict[str, object]] = []
    for width in widths:
        for seed in seeds:
            for scheme in schemes:
                summary = results[(width, seed, scheme)]
                rows.append(
                    {
                        "scheme": scheme,
                        "flow_count": width,
                        "distinct_source_count": min(width, 63),
                        "seed": seed,
                        "payload_goodput_gbps": f"{payload_goodput_gbps(summary):.12g}",
                        "last_completion_ps": max(
                            row.completion_time_ps for row in summary.rows
                        ),
                    }
                )
    runs_path = output_root / "incast_goodput_runs.csv"
    runs_path.parent.mkdir(parents=True, exist_ok=True)
    with runs_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=tuple(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    grouped: list[dict[str, object]] = []
    for scheme in schemes:
        for width in widths:
            values = [
                payload_goodput_gbps(results[(width, seed, scheme)])
                for seed in seeds
            ]
            mean = statistics.fmean(values)
            sigma = statistics.pstdev(values)
            grouped.append(
                {
                    "scheme": scheme,
                    "flow_count": width,
                    "distinct_source_count": min(width, 63),
                    "seed_count": len(values),
                    "payload_goodput_mean_gbps": f"{mean:.12g}",
                    "payload_goodput_sigma_gbps": f"{sigma:.12g}",
                    "payload_goodput_low_gbps": f"{max(0.0, mean - sigma):.12g}",
                    "payload_goodput_high_gbps": f"{mean + sigma:.12g}",
                }
            )
    with (output_root / "incast_goodput_summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=tuple(grouped[0]))
        writer.writeheader()
        writer.writerows(grouped)


def run(
    repo_root: Path,
    output_root: Path,
    tools: runner.ToolPaths,
    topology: Path,
    widths: Sequence[int],
    payload_bytes: int,
    seeds: Sequence[int],
    schemes: Sequence[str],
    parameters: runner.ExperimentParameters,
    force: bool = False,
    timeout_seconds: float | None = None,
) -> None:
    parameters.validate()
    runner.require_tool(tools.goal_converter, "GOAL converter")
    if any(scheme in runner.RNIC_SCHEMES for scheme in schemes):
        runner.require_tool(tools.rnic_simulator, "RNIC simulator")
    if "dcqcn" in schemes:
        runner.require_tool(tools.dcqcn_simulator, "DCQCN simulator")
    if any(scheme in runner.PHYSICAL_SCHEMES for scheme in schemes):
        if not topology.is_file():
            raise runner.OrchestrationError(f"topology is missing: {topology}")

    results: dict[tuple[int, int, str], runner.CompletionSummary] = {}
    for width in widths:
        for seed in seeds:
            artifact = generate_artifact(output_root, width, payload_bytes, seed)
            runner.materialize_workload(
                artifact, tools, repo_root, force, timeout_seconds
            )
            paired: dict[tuple[str, int, str], runner.CompletionSummary] = {}
            for scheme in schemes:
                summary = runner.run_one(
                    repo_root,
                    output_root,
                    tools,
                    topology,
                    artifact,
                    scheme,
                    seed,
                    parameters,
                    force,
                    timeout_seconds,
                )
                results[(width, seed, scheme)] = summary
                paired[(artifact.name, seed, scheme)] = summary
            runner.validate_paired_results(
                paired, (artifact.name,), (seed,), schemes
            )
    write_summary(output_root, results, widths, seeds, schemes)
    runner.atomic_write_json(
        output_root / "incast_sweep_manifest.json",
        {
            "schema": SCHEMA_VERSION,
            "widths": list(widths),
            "width_definition": "directed flow count; width 64 has 63 distinct remote sources",
            "payload_bytes_per_flow": payload_bytes,
            "seeds": list(seeds),
            "schemes": list(schemes),
            "topology": str(topology),
            "parameters": asdict(parameters),
            "goodput_formula": "sum(payload_bytes)*8/(max(completion)-min(start))",
        },
    )


def parse_widths(text: str) -> tuple[int, ...]:
    widths = runner.parse_seed_selection(text)
    if any(width not in generate_incast_goal.DEFAULT_WIDTHS for width in widths):
        raise argparse.ArgumentTypeError(
            "widths must be selected from 2,4,8,16,32,64"
        )
    return widths


def main(argv: Sequence[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--topology", type=Path, default=repo_root / "experiments/rnic_multibaseline/topologies/clos_64_400g.topo")
    parser.add_argument("--widths", type=parse_widths, default=generate_incast_goal.DEFAULT_WIDTHS)
    parser.add_argument("--bytes", type=generate_incast_goal.positive_int, default=generate_incast_goal.DEFAULT_PAYLOAD_BYTES)
    parser.add_argument("--seeds", type=runner.parse_seed_selection, default=(1,))
    parser.add_argument("--schemes", type=lambda value: runner.parse_name_selection(value, runner.SCHEMES, "scheme"), default=runner.SCHEMES)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--timeout", type=float)
    args = parser.parse_args(argv)
    tools = runner.ToolPaths(
        args.build_dir / "htsim_goal_txt2bin",
        args.build_dir / "datacenter/htsim_rnic",
        args.build_dir / "datacenter/htsim_dcqcn_atlahs",
    )
    try:
        run(
            repo_root,
            args.output_root,
            tools,
            args.topology,
            args.widths,
            args.bytes,
            args.seeds,
            args.schemes,
            runner.ExperimentParameters(),
            args.force,
            args.timeout,
        )
    except (runner.OrchestrationError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
