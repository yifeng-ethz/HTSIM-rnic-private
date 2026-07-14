#!/usr/bin/env python3
"""Run a paired one-factor rnic-ss sensitivity scan for hidden debug slides."""

from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Sequence

import run_multibaseline as runner


MIB = 1 << 20
ANALYTICAL_INVALID_BUFFER_REASON = (
    "16 MiB is below the canonical 64-node controlled-Clos analytical "
    "network-calculus envelope"
)
OBSERVED_UNSAFE_BUFFER_REASON = (
    "observed-invalid: 32 MiB is excluded from paired analysis after seeds 1 "
    "and 3 observed physical loss in the canonical four-seed campaign at "
    "model commit 0fa128d; "
    "64 MiB remains the canonical lossless buffer"
)


@dataclass(frozen=True)
class ScanPoint:
    label: str
    axis: str
    value: str
    parameters: runner.ExperimentParameters
    expected_status: str = "complete"
    expected_reason: str = ""


def scan_points(base: runner.ExperimentParameters) -> tuple[ScanPoint, ...]:
    points: list[ScanPoint] = [ScanPoint("default", "default", "default", base)]

    for q_hi_mib in (1, 2, 8):
        points.append(
            ScanPoint(
                f"qhi-{q_hi_mib}m",
                "q_hi_mib",
                str(q_hi_mib),
                replace(
                    base,
                    ss_q_hi_bytes=q_hi_mib * MIB,
                    ss_q_lo_bytes=q_hi_mib * MIB // 2,
                ),
            )
        )
    for numerator, label in ((1, "25"), (3, "75")):
        points.append(
            ScanPoint(
                f"qlo-r{label}",
                "q_lo_over_q_hi",
                f"0.{label}",
                replace(
                    base,
                    ss_q_lo_bytes=base.ss_q_hi_bytes * numerator // 4,
                ),
            )
        )
    for delay_us in (1, 2, 4):
        points.append(
            ScanPoint(
                f"telemetry-{delay_us}us",
                "telemetry_delay_us",
                str(delay_us),
                replace(base, ss_telemetry_delay_ps=delay_us * 1_000_000),
            )
        )
    for hysteresis_ns in (500, 1000, 2000):
        points.append(
            ScanPoint(
                f"hysteresis-{hysteresis_ns}ns",
                "path_hysteresis_ns",
                str(hysteresis_ns),
                replace(base, ss_path_hysteresis_ps=hysteresis_ns * 1000),
            )
        )
    for quantum in (1, 16):
        points.append(
            ScanPoint(
                f"credit-{quantum}",
                "credit_quantum_packets",
                str(quantum),
                replace(base, ss_credit_quantum_packets=quantum),
            )
        )
    for buffer_mib in (16, 32):
        expected_reason = (
            ANALYTICAL_INVALID_BUFFER_REASON
            if buffer_mib == 16
            else OBSERVED_UNSAFE_BUFFER_REASON
        )
        points.append(
            ScanPoint(
                f"buffer-{buffer_mib}m",
                "buffer_mib",
                str(buffer_mib),
                replace(base, physical_buffer_bytes=buffer_mib * MIB),
                "expected-invalid",
                expected_reason,
            )
        )

    labels = [point.label for point in points]
    if len(labels) != len(set(labels)):
        raise AssertionError("scan labels are not unique")
    for point in points:
        point.parameters.validate()
        if point.expected_status not in {"complete", "expected-invalid"}:
            raise AssertionError("scan point has an unknown expected status")
        if (point.expected_status == "expected-invalid") != bool(
            point.expected_reason
        ):
            raise AssertionError(
                "expected-invalid scan points require one explicit reason"
            )
    return tuple(points)


def run_scan(
    repo_root: Path,
    output_root: Path,
    tools: runner.ToolPaths,
    topology: Path,
    seeds: Sequence[int],
    timeout_seconds: float | None,
    force: bool,
) -> Path:
    runner.require_tool(tools.goal_converter, "GOAL converter")
    runner.require_tool(tools.rnic_simulator, "RNIC simulator")
    if not topology.is_file():
        raise runner.OrchestrationError(f"topology is missing: {topology}")

    manifest_rows: list[dict[str, object]] = []
    for point in scan_points(runner.ExperimentParameters()):
        point_root = output_root / "points" / point.label
        for seed in seeds:
            completion: str | Path = ""
            if point.expected_status == "complete":
                artifact = runner.generate_workload("flat32m", seed, point_root)
                runner.materialize_workload(
                    artifact, tools, repo_root, force, timeout_seconds
                )
                runner.run_one(
                    repo_root,
                    point_root,
                    tools,
                    topology,
                    artifact,
                    "rnic-ss",
                    seed,
                    point.parameters,
                    force,
                    timeout_seconds,
                )
                completion = (
                    point_root
                    / "results/flat32m/rnic-ss"
                    / f"seed-{seed}/flowsInfo.csv"
                ).resolve()
            manifest_rows.append(
                {
                    "profile": point.label,
                    "seed": f"seed-{seed}",
                    "status": point.expected_status,
                    "reason": point.expected_reason,
                    "completion_csv": completion,
                    "q_hi_bytes": point.parameters.ss_q_hi_bytes,
                    "q_lo_bytes": point.parameters.ss_q_lo_bytes,
                    "telemetry_delay_ns": point.parameters.ss_telemetry_delay_ps // 1000,
                    "path_hysteresis_ns": point.parameters.ss_path_hysteresis_ps // 1000,
                    "credit_quantum_packets": point.parameters.ss_credit_quantum_packets,
                    "buffer_bytes": point.parameters.physical_buffer_bytes,
                    "scan_axis": point.axis,
                    "scan_value": point.value,
                }
            )

    manifest_path = output_root / "rnic_ss_scan_manifest.csv"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "profile",
        "seed",
        "status",
        "reason",
        "completion_csv",
        "q_hi_bytes",
        "q_lo_bytes",
        "telemetry_delay_ns",
        "path_hysteresis_ns",
        "credit_quantum_packets",
        "buffer_bytes",
        "scan_axis",
        "scan_value",
    )
    with manifest_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(manifest_rows)
    return manifest_path


def main(argv: Sequence[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--topology", type=Path, default=repo_root / "experiments/rnic_multibaseline/topologies/clos_64_400g.topo")
    parser.add_argument("--seeds", type=runner.parse_seed_selection, default=(1, 2, 3, 4))
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    tools = runner.ToolPaths(
        args.build_dir / "htsim_goal_txt2bin",
        args.build_dir / "datacenter/htsim_rnic",
        args.build_dir / "datacenter/htsim_dcqcn_atlahs",
    )
    try:
        manifest = run_scan(
            repo_root,
            args.output_root,
            tools,
            args.topology,
            args.seeds,
            args.timeout,
            args.force,
        )
    except (runner.OrchestrationError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
