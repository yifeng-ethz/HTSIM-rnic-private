#!/usr/bin/env python3
"""Run the pre-registered ss-dragonfly sanity studies.

Executes the incast-degree ladder (S1) and the step-wise flow join (S2)
of expectations.md on the small canonical dragonfly, with the
determinism guard, and evaluates every registered check mechanically.
Bulk outputs (per-interval receiver goodput CSVs, manifests,
summary.csv) go to the output directory, which must live outside the
repository. Sanity evidence only; calibration pending.

Usage:
    python3 run_sanity.py --binary PATH/TO/htsim_ss_dragonfly \
        --out-dir /data3/yifeng/simllm-dev/wave18-runs/w18a/ss_dragonfly_sanity
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOPO = (HERE / ".." / ".." / "htsim" / "sim" / "datacenter" / "topologies" /
        "ss_dragonfly" / "p2a2h1g3_200g.topo").resolve()

BIN_PS = 100_000_000
DURATION_PS = 2_000_000_000
JOIN_DURATION_PS = 1_500_000_000
JOIN_INTERVAL_PS = 250_000_000
C_PAYLOAD_BPS = 200_000_000_000 * 4096 // 4160
STEADY_FIRST_BIN = 4
LADDER = (1, 2, 4, 8)


def run(binary: Path, out_csv: Path, arguments: list[str]) -> str:
    command = [str(binary), "-topo", str(TOPO), "-out", str(out_csv)] + arguments
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"FATAL guard: harness exit {result.returncode} for {command}:\n"
            f"{result.stderr}")
    return result.stdout


def load_bins(path: Path) -> dict[int, dict[int, int]]:
    bins: dict[int, dict[int, int]] = defaultdict(dict)
    with path.open() as handle:
        for row in csv.DictReader(handle):
            bins[int(row["bin_start_ps"])][int(row["flow_id"])] = int(
                row["delivered_payload_bytes"])
    return dict(bins)


def bin_bps(payload_bytes: int) -> int:
    return payload_bytes * 8 * 1_000_000_000_000 // BIN_PS


def manifest_value(manifest: str, key: str) -> str:
    for token in manifest.split():
        if token.startswith(key + "="):
            return token.split("=", 1)[1]
    raise RuntimeError(f"manifest lacks {key}")


class Checks:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []
        self.failed = 0

    def record(self, name: str, passed: bool, detail: str) -> None:
        self.rows.append((name, "PASS" if passed else "FAIL", detail))
        if not passed:
            self.failed += 1

    def fatal(self, name: str, detail: str) -> None:
        self.rows.append((name, "VOID", detail))
        self.failed += 1


def steady_bins(bins: dict[int, dict[int, int]], start_ps: int,
                stop_ps: int) -> list[dict[int, int]]:
    selected = []
    for bin_start in sorted(bins):
        if start_ps <= bin_start and bin_start + BIN_PS <= stop_ps:
            selected.append(bins[bin_start])
    return selected


def check_conservation(checks: Checks, label: str,
                       bins: dict[int, dict[int, int]]) -> None:
    for bin_start, per_flow in bins.items():
        aggregate = bin_bps(sum(per_flow.values()))
        if aggregate > C_PAYLOAD_BPS * 1.001:
            checks.fatal(f"{label}.conservation",
                         f"bin {bin_start} aggregate {aggregate} bps")
            return
    checks.record(f"{label}.conservation", True, "no bin above C_p")


def run_study_pair(binary: Path, out_dir: Path, name: str,
                   arguments: list[str], checks: Checks) -> tuple[
                       dict[int, dict[int, int]], str]:
    first_csv = out_dir / f"{name}.csv"
    manifest = run(binary, first_csv, arguments)
    (out_dir / f"{name}.manifest.txt").write_text(manifest)
    repeat_csv = out_dir / f"{name}.repeat.csv"
    repeat_manifest = run(binary, repeat_csv, arguments)
    identical = (first_csv.read_bytes() == repeat_csv.read_bytes()
                 and manifest == repeat_manifest)
    if not identical:
        checks.fatal(f"{name}.determinism", "repeat invocation differed")
    else:
        checks.record(f"{name}.determinism", True, "byte-identical repeat")
    return load_bins(first_csv), manifest


def study_incast(binary: Path, out_dir: Path, checks: Checks) -> None:
    aggregates: dict[str, dict[int, float]] = {"adaptive": {}, "minimal": {}}
    for arm in ("adaptive", "minimal"):
        for degree in LADDER:
            name = f"incast_{degree}_{arm}"
            arguments = [
                "-pattern", "incast", "-receiver", "0", "-degree", str(degree),
                "-duration_ps", str(DURATION_PS), "-bin_ps", str(BIN_PS),
                "-routing", arm,
            ]
            bins, manifest = run_study_pair(binary, out_dir, name, arguments,
                                            checks)
            check_conservation(checks, name, bins)
            steady = steady_bins(bins, STEADY_FIRST_BIN * BIN_PS, DURATION_PS)
            if not steady:
                checks.fatal(f"{name}.delivered", "no steady bins")
                continue
            per_bin_aggregate = [bin_bps(sum(b.values())) for b in steady]
            mean_aggregate = sum(per_bin_aggregate) / len(per_bin_aggregate)
            aggregates[arm][degree] = mean_aggregate
            checks.record(
                f"{name}.S1.1-saturation",
                mean_aggregate >= 0.90 * C_PAYLOAD_BPS,
                f"steady aggregate {mean_aggregate / 1e9:.2f} Gbps")
            dropped = int(manifest_value(manifest, "dropped"))
            if degree == 1:
                flow_bins = [bin_bps(b.get(0, 0)) for b in steady]
                checks.record(
                    f"{name}.S1.2-line-rate",
                    min(flow_bins) >= 0.99 * C_PAYLOAD_BPS and dropped == 0,
                    f"min steady {min(flow_bins) / 1e9:.2f} Gbps, "
                    f"dropped {dropped}")
            else:
                checks.record(f"{name}.S1.3-drops", dropped > 0,
                              f"dropped {dropped}")
            if degree in (2, 4):
                fair = C_PAYLOAD_BPS / degree
                worst = ""
                ok = True
                for flow in range(degree):
                    flow_mean = sum(bin_bps(b.get(flow, 0))
                                    for b in steady) / len(steady)
                    if not 0.6 * fair <= flow_mean <= 1.4 * fair:
                        ok = False
                        worst = f"flow {flow} at {flow_mean / 1e9:.2f} Gbps"
                checks.record(f"{name}.S1.4-fairness", ok,
                              worst or "all flows in the band")
            if degree == 8:
                ok = True
                worst = ""
                for flow in range(degree):
                    flow_mean = sum(bin_bps(b.get(flow, 0))
                                    for b in steady) / len(steady)
                    if flow_mean < 0.02 * C_PAYLOAD_BPS:
                        ok = False
                        worst = f"flow {flow} at {flow_mean / 1e9:.3f} Gbps"
                checks.record(f"{name}.S1.4-no-starvation", ok,
                              worst or "all flows above the floor")
    for degree in LADDER:
        if degree in aggregates["adaptive"] and degree in aggregates["minimal"]:
            adaptive = aggregates["adaptive"][degree]
            minimal = aggregates["minimal"][degree]
            checks.record(
                f"incast_{degree}.S1.5-endpoint-limit",
                abs(adaptive - minimal) <= 0.02 * max(adaptive, minimal),
                f"adaptive {adaptive / 1e9:.2f} vs minimal "
                f"{minimal / 1e9:.2f} Gbps")


def study_join(binary: Path, out_dir: Path, checks: Checks) -> None:
    name = "join_adaptive"
    arguments = [
        "-pattern", "join", "-receiver", "0", "-degree", "4",
        "-join_interval_ps", str(JOIN_INTERVAL_PS),
        "-duration_ps", str(JOIN_DURATION_PS), "-bin_ps", str(BIN_PS),
        "-routing", "adaptive",
    ]
    bins, manifest = run_study_pair(binary, out_dir, name, arguments, checks)
    check_conservation(checks, name, bins)

    ramp = steady_bins(bins, 2 * BIN_PS, JOIN_INTERVAL_PS)
    if ramp:
        aggregate = max(bin_bps(sum(b.values())) for b in ramp)
        checks.record(f"{name}.S2.1-staircase-top",
                      aggregate >= 0.90 * C_PAYLOAD_BPS,
                      f"first-epoch peak {aggregate / 1e9:.2f} Gbps")
    flow0_epoch_means = []
    for k in range(1, 5):
        epoch_start = (k - 1) * JOIN_INTERVAL_PS
        epoch_stop = min(k * JOIN_INTERVAL_PS, JOIN_DURATION_PS)
        settled = steady_bins(bins, epoch_start + 2 * BIN_PS, epoch_stop)
        if not settled:
            checks.fatal(f"{name}.S2.2-epoch{k}", "no settled bins")
            continue
        fair = C_PAYLOAD_BPS / k
        ok = True
        worst = ""
        for flow in range(k):
            flow_mean = sum(bin_bps(b.get(flow, 0))
                            for b in settled) / len(settled)
            if not 0.45 * fair <= flow_mean <= 1.55 * fair:
                ok = False
                worst = f"flow {flow} at {flow_mean / 1e9:.2f} Gbps"
        checks.record(f"{name}.S2.2-epoch{k}-shares", ok,
                      worst or f"{k} active flows in the band")
        flow0_epoch_means.append(
            sum(bin_bps(b.get(0, 0)) for b in settled) / len(settled))
    checks.record(
        f"{name}.S2.2-flow0-steps-down",
        all(later < earlier for earlier, later in
            zip(flow0_epoch_means, flow0_epoch_means[1:])),
        "flow 0 epoch means " +
        ", ".join(f"{m / 1e9:.2f}" for m in flow0_epoch_means))
    tail = steady_bins(bins, 3 * JOIN_INTERVAL_PS + 2 * BIN_PS,
                       JOIN_DURATION_PS)
    if len(tail) >= 2:
        ok = True
        worst = ""
        for flow in range(4):
            series = [bin_bps(b.get(flow, 0)) for b in tail]
            wobble = max(abs(second - first)
                         for first, second in zip(series, series[1:]))
            if wobble > 0.2 * C_PAYLOAD_BPS / 4:
                ok = False
                worst = f"flow {flow} wobble {wobble / 1e9:.2f} Gbps"
        checks.record(f"{name}.S2.3-stability", ok, worst or "settled")
    checks.record(f"{name}.S2.4-drop-onset",
                  int(manifest_value(manifest, "dropped")) > 0,
                  f"dropped {manifest_value(manifest, 'dropped')}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    arguments = parser.parse_args()
    arguments.out_dir.mkdir(parents=True, exist_ok=True)

    checks = Checks()
    study_incast(arguments.binary, arguments.out_dir, checks)
    study_join(arguments.binary, arguments.out_dir, checks)

    summary = arguments.out_dir / "summary.csv"
    with summary.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["check", "verdict", "detail"])
        writer.writerows(checks.rows)
    for name, verdict, detail in checks.rows:
        print(f"[{verdict}] {name}: {detail}")
    print(f"summary: {summary}")
    print("evidence=sanity-not-calibration "
          "calibration_status=hosted-pending-merlin-calibration")
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
