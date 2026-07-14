#!/usr/bin/env python3
from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

import analyze_fct


PS_FLOW_HEADERS = (
    "profile",
    "flow_id",
    "source",
    "destination",
    "tag",
    "payload_bytes",
    "start_time_ps",
    "completion_time_ps",
    "fct_ps",
)
NS_FLOW_HEADERS = (
    "srcNode_dstNode_flowId",
    "flowSizeBytes",
    "startTimeNs",
    "endTimeNs",
    "fctNs",
)


def write_flows(
    path: Path,
    fcts: dict[str, int],
    payload: int = 32 * 1024 * 1024,
    time_unit: str = "ps",
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if time_unit not in {"ps", "ns"}:
        raise ValueError(time_unit)
    headers = PS_FLOW_HEADERS if time_unit == "ps" else NS_FLOW_HEADERS
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=headers)
        writer.writeheader()
        for flow_id, fct in sorted(fcts.items()):
            if time_unit == "ps":
                writer.writerow(
                    {
                        "profile": "fixture",
                        "flow_id": flow_id,
                        "source": 0,
                        "destination": 1,
                        "tag": 0,
                        "payload_bytes": payload,
                        "start_time_ps": 0,
                        "completion_time_ps": fct,
                        "fct_ps": fct,
                    }
                )
            else:
                writer.writerow({
                    "srcNode_dstNode_flowId": flow_id,
                    "flowSizeBytes": payload,
                    "startTimeNs": 0,
                    "endTimeNs": fct,
                    "fctNs": fct,
                })


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


class CompletionAnalysisTest(unittest.TestCase):
    def test_native_headers_and_exact_common_grid_cdf(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "input"
            output = Path(temporary) / "output"
            fixtures = {
                ("dcqcn", "seed-1"): ({"f0": 10, "f1": 20}, "ns"),
                ("dcqcn", "seed-2"): ({"f0": 20, "f1": 30}, "ns"),
                ("rnic-cn", "seed-1"): ({"f0": 15001, "f1": 25001}, "ps"),
                ("rnic-cn", "seed-2"): ({"f0": 25001, "f1": 35001}, "ps"),
            }
            for (profile, seed), (fcts, unit) in fixtures.items():
                write_flows(
                    root / profile / seed / "flowsInfo.csv", fcts, time_unit=unit
                )

            runs = analyze_fct.discover_runs(root, "flowsInfo.csv")
            analyze_fct.write_cdf_analysis(runs, output)

            summary = read_rows(output / "fct_cdf_summary.csv")
            grid = sorted({int(row["grid_fct_ps"]) for row in summary})
            self.assertEqual(grid, [0, 10000, 15001, 20000, 25001, 30000, 35001])
            dcqcn_at_20 = next(
                row
                for row in summary
                if row["profile"] == "dcqcn" and row["grid_fct_ps"] == "20000"
            )
            self.assertAlmostEqual(float(dcqcn_at_20["cdf_mean"]), 0.75)
            self.assertAlmostEqual(float(dcqcn_at_20["cdf_sigma"]), 0.25)
            self.assertAlmostEqual(float(dcqcn_at_20["cdf_low"]), 0.50)
            self.assertAlmostEqual(float(dcqcn_at_20["cdf_high"]), 1.00)

            run_summary = read_rows(output / "fct_run_summary.csv")
            first = next(
                row
                for row in run_summary
                if row["profile"] == "dcqcn" and row["seed"] == "seed-1"
            )
            self.assertEqual(first["p50_fct_ps"], "10000")
            self.assertEqual(first["p95_fct_ps"], "20000")
            self.assertEqual(first["p99_fct_ps"], "20000")
            self.assertEqual(first["p99_9_fct_ps"], "20000")
            self.assertEqual(first["jct_ps"], "20000")
            rnic_first = next(
                row
                for row in run_summary
                if row["profile"] == "rnic-cn" and row["seed"] == "seed-1"
            )
            self.assertEqual(rnic_first["p50_fct_ps"], "15001")

    def test_rejects_payload_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_flows(root / "dcqcn" / "seed-1" / "flowsInfo.csv", {"f0": 10})
            write_flows(
                root / "rnic-cn" / "seed-1" / "flowsInfo.csv",
                {"f0": 10},
                payload=16,
            )
            with self.assertRaisesRegex(analyze_fct.AnalysisError, "payload"):
                analyze_fct.discover_runs(root, "flowsInfo.csv")

    def test_rejects_flow_set_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_flows(
                root / "dcqcn" / "seed-1" / "flowsInfo.csv",
                {"f0": 10, "f1": 20},
            )
            write_flows(
                root / "rnic-cn" / "seed-1" / "flowsInfo.csv",
                {"f0": 10},
            )
            with self.assertRaisesRegex(analyze_fct.AnalysisError, "missing flow IDs"):
                analyze_fct.discover_runs(root, "flowsInfo.csv")

    def test_rejects_unpaired_seed_sets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_flows(root / "dcqcn" / "seed-1" / "flowsInfo.csv", {"f0": 10})
            write_flows(root / "dcqcn" / "seed-2" / "flowsInfo.csv", {"f0": 10})
            write_flows(root / "rnic-cn" / "seed-1" / "flowsInfo.csv", {"f0": 10})
            with self.assertRaisesRegex(analyze_fct.AnalysisError, "unpaired seeds"):
                analyze_fct.discover_runs(root, "flowsInfo.csv")

    def test_rejects_inconsistent_explicit_fct(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "flowsInfo.csv"
            write_flows(path, {"f0": 10})
            rows = read_rows(path)
            rows[0]["completion_time_ps"] = "11"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=PS_FLOW_HEADERS)
                writer.writeheader()
                writer.writerows(rows)
            with self.assertRaisesRegex(analyze_fct.AnalysisError, "disagrees"):
                analyze_fct.read_completion_csv(path)


class ParameterScanTest(unittest.TestCase):
    def test_parameter_scan_tidy_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "scan.csv"
            output = root / "output"
            rows: list[dict[str, object]] = []
            for q_hi, seed, fcts in (
                (8000, "seed-1", {"f0": 10, "f1": 20}),
                (8000, "seed-2", {"f0": 20, "f1": 30}),
                (12000, "seed-1", {"f0": 15, "f1": 25}),
                (12000, "seed-2", {"f0": 25, "f1": 35}),
            ):
                completion = root / f"q{q_hi}" / seed / "flowsInfo.csv"
                write_flows(completion, fcts)
                rows.append(
                    {
                        "profile": "rnic-ss",
                        "seed": seed,
                        "completion_csv": completion.relative_to(root),
                        "q_hi_bytes": q_hi,
                        "q_lo_bytes": 4000,
                        "telemetry_delay_ns": 100,
                        "path_hysteresis_ns": 250,
                        "credit_quantum_packets": 8,
                        "buffer_bytes": 16000,
                    }
                )
            with manifest.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle, fieldnames=analyze_fct.SCAN_MANIFEST_COLUMNS
                )
                writer.writeheader()
                writer.writerows(rows)

            scan_runs = analyze_fct.read_scan_manifest(manifest)
            analyze_fct.write_parameter_scan(scan_runs, output)

            summary = read_rows(output / "parameter_scan_summary.csv")
            self.assertEqual(len(summary), 2)
            first = next(row for row in summary if row["q_hi_bytes"] == "8000")
            self.assertEqual(first["hysteresis_gap_bytes"], "4000")
            self.assertEqual(first["path_hysteresis_ns"], "250")
            self.assertAlmostEqual(float(first["q_hi_fraction_buffer"]), 0.5)
            self.assertAlmostEqual(float(first["p50_fct_ps_mean"]), 15.0)
            self.assertAlmostEqual(float(first["p50_fct_ps_sigma"]), 5.0)
            self.assertAlmostEqual(float(first["jct_ps_mean"]), 25.0)

            plot = read_rows(output / "parameter_scan_plot.csv")
            self.assertEqual(len(plot), 2 * len(analyze_fct.SUMMARY_METRICS))
            self.assertEqual(
                {row["metric"] for row in plot},
                {"p50_fct", "p95_fct", "p99_fct", "p99_9_fct", "jct"},
            )

    def test_scan_skips_explicit_expected_invalid_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completion = root / "flowsInfo.csv"
            write_flows(completion, {"f0": 10})
            manifest = root / "scan.csv"
            rows = (
                {
                    "profile": "default",
                    "seed": "seed-1",
                    "status": "complete",
                    "reason": "",
                    "completion_csv": completion.name,
                    "q_hi_bytes": 4000,
                    "q_lo_bytes": 2000,
                    "telemetry_delay_ns": 0,
                    "path_hysteresis_ns": 0,
                    "credit_quantum_packets": 4,
                    "buffer_bytes": 64000,
                },
                {
                    "profile": "buffer-16m",
                    "seed": "seed-1",
                    "status": "expected-invalid",
                    "reason": "below analytical envelope",
                    "completion_csv": "",
                    "q_hi_bytes": 4000,
                    "q_lo_bytes": 2000,
                    "telemetry_delay_ns": 0,
                    "path_hysteresis_ns": 0,
                    "credit_quantum_packets": 4,
                    "buffer_bytes": 16000,
                },
            )
            with manifest.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle, fieldnames=analyze_fct.SCAN_MANIFEST_COLUMNS
                )
                writer.writeheader()
                writer.writerows(rows)

            scan_runs = analyze_fct.read_scan_manifest(manifest)
            self.assertEqual(len(scan_runs), 1)
            self.assertEqual(scan_runs[0].run.profile, "default")

    def test_scan_rejects_invalid_hysteresis(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            completion = root / "flowsInfo.csv"
            write_flows(completion, {"f0": 10})
            manifest = root / "scan.csv"
            with manifest.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle, fieldnames=analyze_fct.SCAN_MANIFEST_COLUMNS
                )
                writer.writeheader()
                writer.writerow(
                    {
                        "profile": "rnic-ss",
                        "seed": "seed-1",
                        "completion_csv": completion.name,
                        "q_hi_bytes": 4000,
                        "q_lo_bytes": 4000,
                        "telemetry_delay_ns": 100,
                        "path_hysteresis_ns": 250,
                        "credit_quantum_packets": 8,
                        "buffer_bytes": 16000,
                    }
                )
            with self.assertRaisesRegex(analyze_fct.AnalysisError, "greater"):
                analyze_fct.read_scan_manifest(manifest)


if __name__ == "__main__":
    unittest.main()
