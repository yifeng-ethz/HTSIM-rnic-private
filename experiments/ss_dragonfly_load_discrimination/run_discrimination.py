#!/usr/bin/env python3
"""Run the pre-registered ss-dragonfly load-discrimination experiment.

Executes both cells (CONTROL and DISCRIMINATE) of expectations.md under
both buffer configurations (A = 4 MiB, B = 1 MiB), every invocation
twice (repeat determinism guard), sequentially, and evaluates every
registered row mechanically. Bulk outputs go to the output directory,
which must live outside the repository. Capability plus sanity
evidence; no Merlin calibration claim.

Usage:
    python3 run_discrimination.py --binary PATH/TO/htsim_ss_dragonfly \
        --out-dir /data3/yifeng/simllm-dev/wave20-runs/w20a/discrimination
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent

CONFIGS = {
    "A": HERE / "merlin_shape_buffer4mib.topo",
    "B": HERE / "merlin_shape_buffer1mib.topo",
}

BIN_PS = 100_000_000
CONTROL_DURATION_PS = 200_000_000_000
DISCRIMINATE_DURATION_PS = 5_000_000_000

# Registered constants (expectations.md, hand arithmetic).
CHUNK_SERVICE_F_PS = 340_417_280
FLOW0_PERIOD_PS = 2_198_644_280
FLOW1_PERIOD_PS = 1_596_855_280
CONTROL_INJECTED = 203_546
CONTROL_CHUNKS = {0: 91, 1: 126}
DISCRIMINATE_INJECTED = 17_980
FIRST_DROP_BAND = {"A": (500_000_000, 620_000_000), "B": (125_000_000, 155_000_000)}
DROP_DIFF_BAND = (300, 400)
BIN_AGGREGATE_BAND = (2_450_355, 2_487_544)
QUANTIZATION_BOUND = 2_487_544
SHARE_BAND = (0.4, 0.6)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def control_command(binary: Path, topo: Path, out_csv: Path, chunk_csv: Path) -> list[str]:
    return [
        str(binary), "-topo", str(topo), "-pattern", "explicit",
        "-routing", "adaptive", "-wire_bytes", "9038", "-header_bytes", "90",
        "-bin_ps", str(BIN_PS), "-duration_ps", str(CONTROL_DURATION_PS),
        "-source", "closed", "-chunk_payload_bytes", "8388608",
        "-flow", "src=8,dst=5,think_ps=1858227000",
        "-flow", "src=16,dst=6,think_ps=1256438000",
        "-chunk_out", str(chunk_csv), "-out", str(out_csv),
    ]


def discriminate_command(binary: Path, topo: Path, out_csv: Path) -> list[str]:
    return [
        str(binary), "-topo", str(topo), "-pattern", "explicit",
        "-routing", "adaptive", "-wire_bytes", "9038", "-header_bytes", "90",
        "-bin_ps", str(BIN_PS), "-duration_ps", str(DISCRIMINATE_DURATION_PS),
        "-source", "paced", "-offered_bps", "130000000000",
        "-flow", "src=8,dst=5",
        "-flow", "src=16,dst=5,start_ps=278092",
        "-out", str(out_csv),
    ]


def manifest_value(stdout: str, key: str) -> str:
    for token in stdout.split():
        if token.startswith(key + "="):
            return token.split("=", 1)[1]
    raise RuntimeError(f"manifest lacks {key}")


def mask_topology_token(stdout: str) -> str:
    """CT-4 correction 1 as amended: the manifest echoes the invocation's
    topology path, which differs between configurations by construction.
    The comparison tokenizes stdout on whitespace and rejoins with single
    spaces, so line and spacing structure is normalized as well, and the
    topology= token is replaced; the masked token is the only content
    difference the CT-4 verdict tolerates."""
    return " ".join("topology=MASKED" if token.startswith("topology=") else token
                    for token in stdout.split())


def load_bins(path: Path) -> dict[int, dict[int, int]]:
    bins: dict[int, dict[int, int]] = defaultdict(dict)
    with path.open() as handle:
        for row in csv.DictReader(handle):
            bins[int(row["bin_start_ps"])][int(row["flow_id"])] = int(
                row["delivered_payload_bytes"])
    return dict(bins)


def load_chunks(path: Path) -> dict[int, list[tuple[int, int, int]]]:
    chunks: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    with path.open() as handle:
        for row in csv.DictReader(handle):
            chunks[int(row["flow_id"])].append(
                (int(row["chunk_index"]), int(row["first_injection_ps"]),
                 int(row["completion_ps"])))
    for rows in chunks.values():
        rows.sort()
    return dict(chunks)


class Checks:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str]] = []
        self.failed = 0
        self.voided = 0

    def record(self, name: str, passed: bool, detail: str) -> None:
        self.rows.append((name, "PASS" if passed else "FAIL", detail))
        if not passed:
            self.failed += 1

    def fatal(self, name: str, detail: str) -> None:
        self.rows.append((name, "VOID", detail))
        self.voided += 1

    def derived(self, name: str, detail: str) -> None:
        self.rows.append((name, "DERIVED", detail))


def run_cell(binary: Path, out_dir: Path, cell: str, config: str,
             checks: Checks) -> dict | None:
    """Runs one (cell, config) twice; returns parsed artifacts or None on a
    fatal guard."""
    topo = CONFIGS[config]
    artifacts = {}
    for repeat in (1, 2):
        stem = f"{cell}_{config}_r{repeat}"
        out_csv = out_dir / f"{stem}.csv"
        chunk_csv = out_dir / f"{stem}.chunks.csv"
        if cell == "control":
            command = control_command(binary, topo, out_csv, chunk_csv)
        else:
            command = discriminate_command(binary, topo, out_csv)
        result = subprocess.run(command, capture_output=True, text=True)
        (out_dir / f"{stem}.stdout").write_text(result.stdout)
        (out_dir / f"{stem}.cmd").write_text(" ".join(command) + "\n")
        if result.returncode != 0:
            checks.fatal(f"FG-exit {cell} {config} r{repeat}",
                         f"exit {result.returncode}: {result.stderr.strip()}")
            return None
        artifacts[repeat] = {
            "stdout": result.stdout,
            "csv_bytes": out_csv.read_bytes(),
            "chunk_bytes": chunk_csv.read_bytes() if cell == "control" else b"",
        }
    if (artifacts[1]["csv_bytes"] != artifacts[2]["csv_bytes"]
            or artifacts[1]["stdout"] != artifacts[2]["stdout"]
            or artifacts[1]["chunk_bytes"] != artifacts[2]["chunk_bytes"]):
        checks.fatal(f"FG-determinism {cell} {config}",
                     "repeat outputs are not byte-identical")
        return None

    stdout = artifacts[1]["stdout"]
    parsed = {
        "stdout": stdout,
        "csv_bytes": artifacts[1]["csv_bytes"],
        "chunk_bytes": artifacts[1]["chunk_bytes"],
        "injected": int(manifest_value(stdout, "injected")),
        "delivered": int(manifest_value(stdout, "delivered")),
        "dropped": int(manifest_value(stdout, "dropped")),
        "first_drop": manifest_value(stdout, "first_drop_ps"),
        "bins": load_bins(out_dir / f"{cell}_{config}_r1.csv"),
    }
    if cell == "control":
        parsed["chunks"] = load_chunks(out_dir / f"{cell}_{config}_r1.chunks.csv")
    if parsed["delivered"] == 0:
        checks.fatal(f"FG-delivered {cell} {config}", "delivered = 0")
        return None
    # Conservation guard, corrected scope (correction 2): every flow's
    # per-bin payload is bounded by its destination port's quantization
    # bound, and the cell aggregate by that bound times the cell's number
    # of distinct destination ports (CONTROL has two, DISCRIMINATE one).
    destination_ports = 2 if cell == "control" else 1
    worst_flow = max((value for flows in parsed["bins"].values()
                      for value in flows.values()), default=0)
    worst = max((sum(flows.values()) for flows in parsed["bins"].values()),
                default=0)
    if (worst_flow > QUANTIZATION_BOUND
            or worst > destination_ports * QUANTIZATION_BOUND):
        checks.fatal(
            f"FG-conservation {cell} {config}",
            f"worst per-flow bin {worst_flow} B (bound {QUANTIZATION_BOUND}), "
            f"worst aggregate bin {worst} B "
            f"(bound {destination_ports * QUANTIZATION_BOUND})")
        return None
    return parsed


def check_control(config: str, parsed: dict, checks: Checks) -> None:
    checks.record(
        f"CT-1 {config} exact accounting",
        parsed["injected"] == CONTROL_INJECTED
        and parsed["delivered"] == CONTROL_INJECTED and parsed["dropped"] == 0,
        f"injected={parsed['injected']} delivered={parsed['delivered']} "
        f"dropped={parsed['dropped']} (registered {CONTROL_INJECTED}, 0 drops)")
    counts = {flow: len(rows) for flow, rows in parsed["chunks"].items()}
    checks.record(f"CT-2 {config} chunk counts", counts == CONTROL_CHUNKS,
                  f"counts={counts} (registered {CONTROL_CHUNKS})")
    for flow, period in ((0, FLOW0_PERIOD_PS), (1, FLOW1_PERIOD_PS)):
        rows = parsed["chunks"].get(flow, [])
        first_ok = bool(rows) and rows[0][2] == CHUNK_SERVICE_F_PS
        deltas_ok = all(
            rows[i][2] - rows[i - 1][2] == period for i in range(1, len(rows)))
        checks.record(
            f"CT-3 {config} flow {flow} cadence",
            first_ok and deltas_ok,
            f"chunk0={rows[0][2] if rows else 'none'} every delta == {period}: "
            f"{deltas_ok}")


def flow_injected_counts(stdout: str) -> dict[int, int]:
    """Per-flow injected packets from the harness's flow manifest lines."""
    counts: dict[int, int] = {}
    for line in stdout.splitlines():
        if not line.startswith("[ss-dragonfly flow]"):
            continue
        tokens = dict(token.split("=", 1) for token in line.split()
                      if "=" in token)
        counts[int(tokens["flow"])] = int(tokens["injected_packets"])
    return counts


def check_discriminate(config: str, parsed: dict, checks: Checks) -> None:
    # DP-1's registered clause includes the per-flow split (8990 per
    # flow); evaluate both halves (review finding F5: the first scored
    # run checked only the fabric-wide total).
    per_flow = flow_injected_counts(parsed["stdout"])
    checks.record(
        f"DP-1 {config} exact pacing oracle",
        parsed["injected"] == DISCRIMINATE_INJECTED
        and per_flow == {0: DISCRIMINATE_INJECTED // 2, 1: DISCRIMINATE_INJECTED // 2},
        f"injected={parsed['injected']} per_flow={per_flow} "
        f"(registered {DISCRIMINATE_INJECTED} total, 8990 per flow)")
    checks.record(f"DP-2 {config} drops occur", parsed["dropped"] > 0,
                  f"dropped={parsed['dropped']}")
    band = FIRST_DROP_BAND[config]
    first_drop = (int(parsed["first_drop"])
                  if parsed["first_drop"] != "none" else None)
    checks.record(
        f"DP-3 {config} first-drop band",
        first_drop is not None and band[0] <= first_drop <= band[1],
        f"first_drop_ps={parsed['first_drop']} (registered {band})")
    steady = [start for start in parsed["bins"]
              if start >= BIN_PS and start + BIN_PS <= DISCRIMINATE_DURATION_PS]
    aggregate_ok = True
    share_ok = True
    aggregate_detail = ""
    share_detail = ""
    worst_share_delta = 0.0
    for start in steady:
        flows = parsed["bins"][start]
        aggregate = sum(flows.values())
        if not BIN_AGGREGATE_BAND[0] <= aggregate <= BIN_AGGREGATE_BAND[1]:
            aggregate_ok = False
            aggregate_detail = f"bin {start} aggregate {aggregate}"
        for flow in (0, 1):
            share = flows.get(flow, 0) / aggregate if aggregate else 0.0
            worst_share_delta = max(worst_share_delta, abs(share - 0.5))
            if not SHARE_BAND[0] <= share <= SHARE_BAND[1]:
                share_ok = False
                share_detail = f"bin {start} flow {flow} share {share:.3f}"
    checks.record(
        f"DP-5 {config} saturation band",
        bool(steady) and aggregate_ok,
        aggregate_detail
        or f"{len(steady)} steady bins all in {BIN_AGGREGATE_BAND}")
    # F3 disclosure: the frozen [0.4, 0.6] band is far looser than the
    # round-robin mechanism it registers, so this row was near-unfailable
    # as registered; the worst observed distance from 0.5 is reported so
    # the looseness is visible.  The frozen band itself is not retuned.
    checks.record(
        f"DP-6 {config} fairness band",
        bool(steady) and share_ok,
        share_detail
        or (f"per-flow shares in {SHARE_BAND} across {len(steady)} bins, "
            f"worst |share - 0.5| = {worst_share_delta:.4f}"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    arguments = parser.parse_args()
    out_dir = arguments.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    repo_root = HERE.parent.parent
    if str(out_dir).startswith(str(repo_root)):
        raise SystemExit("--out-dir must live outside the repository")

    manifest = {
        "binary_sha256": sha256_of(arguments.binary),
        "topologies": {name: sha256_of(path) for name, path in
                       ((name, path) for name, path in CONFIGS.items())},
        "expectations_sha256": sha256_of(HERE / "expectations.md"),
    }

    checks = Checks()
    parsed: dict[tuple[str, str], dict] = {}
    for cell in ("control", "discriminate"):
        for config in ("A", "B"):
            result = run_cell(arguments.binary, out_dir, cell, config, checks)
            if result is not None:
                parsed[(cell, config)] = result

    if ("control", "A") in parsed and ("control", "B") in parsed:
        for config in ("A", "B"):
            check_control(config, parsed[("control", config)], checks)
        a, b = parsed[("control", "A")], parsed[("control", "B")]
        checks.record(
            "CT-4 control identity A == B",
            a["csv_bytes"] == b["csv_bytes"]
            and a["chunk_bytes"] == b["chunk_bytes"]
            and mask_topology_token(a["stdout"]) == mask_topology_token(b["stdout"]),
            "bins CSV and chunk CSV byte-identical; stdout identical with the "
            "topology= invocation token masked (correction 1)")

    if ("discriminate", "A") in parsed and ("discriminate", "B") in parsed:
        for config in ("A", "B"):
            check_discriminate(config, parsed[("discriminate", config)], checks)
        a, b = parsed[("discriminate", "A")], parsed[("discriminate", "B")]
        first_a = int(a["first_drop"]) if a["first_drop"] != "none" else None
        first_b = int(b["first_drop"]) if b["first_drop"] != "none" else None
        checks.record(
            "DP-3 ordering first_drop(B) < first_drop(A)",
            first_a is not None and first_b is not None and first_b < first_a,
            f"A={first_a} B={first_b}")
        diff = b["dropped"] - a["dropped"]
        checks.record(
            "DP-4 drop difference band",
            DROP_DIFF_BAND[0] <= diff <= DROP_DIFF_BAND[1],
            f"dropped(B)-dropped(A)={diff} (registered {DROP_DIFF_BAND}, "
            "prediction 348)")
        checks.derived(
            "delivered difference (entailed, unscored)",
            f"delivered(A)-delivered(B)={a['delivered'] - b['delivered']} "
            f"== dropped(B)-dropped(A)={diff}: "
            f"{a['delivered'] - b['delivered'] == diff}")
        checks.derived(
            "bins CSVs differ across configs (entailed, unscored)",
            f"identical={a['csv_bytes'] == b['csv_bytes']}")

    # Entailment classification (review finding F2, applying the freeze's
    # own rule that entailed rows are never counted as independent
    # evidence): the B-config CONTROL rows are entailed by CT-4's byte
    # identity of the artifacts they are read from plus their A
    # counterparts; DP-2 is entailed by DP-3's in-band first drop; the
    # DP-3 ordering row is entailed by its two disjoint band rows; and
    # DP-1 B is entailed by DP-1 A under the freeze's registered
    # open-loop identity argument.
    entailed_names = {
        "CT-1 B exact accounting",
        "CT-2 B chunk counts",
        "CT-3 B flow 0 cadence",
        "CT-3 B flow 1 cadence",
        "DP-1 B exact pacing oracle",
        "DP-2 A drops occur",
        "DP-2 B drops occur",
        "DP-3 ordering first_drop(B) < first_drop(A)",
    }
    genuine_pass = sum(1 for name, verdict, _ in checks.rows
                       if verdict == "PASS" and name not in entailed_names)
    entailed_pass = sum(1 for name, verdict, _ in checks.rows
                        if verdict == "PASS" and name in entailed_names)
    derived_count = sum(1 for _, verdict, _ in checks.rows
                        if verdict == "DERIVED")

    summary = {
        "manifest": manifest,
        "rows": [{"name": name, "verdict": verdict, "detail": detail,
                  "entailed": name in entailed_names}
                 for name, verdict, detail in checks.rows],
        "failed": checks.failed,
        "voided": checks.voided,
        "genuine_risk_pass": genuine_pass,
        "entailed_pass": entailed_pass,
        "derived_rows": derived_count,
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    width = max(len(name) for name, _, _ in checks.rows)
    for name, verdict, detail in checks.rows:
        tag = " (entailed)" if name in entailed_names and verdict == "PASS" else ""
        print(f"{name:<{width}}  {verdict:7}  {detail}{tag}")
    print(f"\nfailed={checks.failed} voided={checks.voided} "
          f"genuine_risk_pass={genuine_pass} entailed_pass={entailed_pass} "
          f"derived={derived_count} rows={len(checks.rows)}")
    return 1 if checks.failed or checks.voided else 0


if __name__ == "__main__":
    sys.exit(main())
