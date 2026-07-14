#!/usr/bin/env python3
from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_multibaseline as runner
import run_rnic_ss_scan as scan


class RnicSsScanTest(unittest.TestCase):
    def test_one_factor_points_are_unique_and_preserve_unscanned_defaults(self) -> None:
        base = runner.ExperimentParameters()
        points = scan.scan_points(base)
        self.assertEqual(len(points), 16)
        self.assertEqual(len({point.label for point in points}), len(points))
        default = next(point for point in points if point.label == "default")
        self.assertEqual(default.parameters, base)

        qhi = next(point for point in points if point.label == "qhi-1m")
        self.assertEqual(qhi.parameters.ss_q_hi_bytes, 1 << 20)
        self.assertEqual(qhi.parameters.ss_q_lo_bytes, 1 << 19)
        self.assertEqual(qhi.parameters.ss_credit_quantum_packets, 4)

        hysteresis = next(
            point for point in points if point.label == "hysteresis-500ns"
        )
        self.assertEqual(hysteresis.parameters.ss_path_hysteresis_ps, 500_000)
        self.assertEqual(hysteresis.parameters.ss_telemetry_delay_ps, 0)

        invalid_buffer = next(
            point for point in points if point.label == "buffer-16m"
        )
        self.assertEqual(invalid_buffer.expected_status, "expected-invalid")
        self.assertIn("below", invalid_buffer.expected_reason)
        valid_buffer = next(
            point for point in points if point.label == "buffer-32m"
        )
        self.assertEqual(valid_buffer.expected_status, "complete")
        self.assertEqual(valid_buffer.expected_reason, "")

    def test_every_requested_debug_axis_is_present(self) -> None:
        axes = {point.axis for point in scan.scan_points(runner.ExperimentParameters())}
        self.assertEqual(
            axes,
            {
                "default",
                "q_hi_mib",
                "q_lo_over_q_hi",
                "telemetry_delay_us",
                "path_hysteresis_ns",
                "credit_quantum_packets",
                "buffer_mib",
            },
        )

    def test_expected_invalid_point_is_manifested_without_launching(self) -> None:
        base = runner.ExperimentParameters()
        points = (
            scan.ScanPoint("valid", "default", "default", base),
            scan.ScanPoint(
                "invalid",
                "buffer_mib",
                "16",
                base,
                "expected-invalid",
                "deliberate bound rejection",
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            topology = root / "topology.topo"
            topology.write_text("test\n", encoding="utf-8")
            tools = runner.ToolPaths(
                root / "converter", root / "rnic", root / "dcqcn"
            )
            artifact = mock.Mock()
            with (
                mock.patch.object(scan, "scan_points", return_value=points),
                mock.patch.object(runner, "require_tool"),
                mock.patch.object(
                    runner, "generate_workload", return_value=artifact
                ) as generate,
                mock.patch.object(runner, "materialize_workload") as materialize,
                mock.patch.object(runner, "run_one") as run_one,
            ):
                manifest = scan.run_scan(
                    root,
                    root / "output",
                    tools,
                    topology,
                    (7,),
                    None,
                    False,
                )

            self.assertEqual(generate.call_count, 1)
            self.assertEqual(materialize.call_count, 1)
            self.assertEqual(run_one.call_count, 1)
            with manifest.open(encoding="utf-8", newline="") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 2)
            invalid = next(row for row in rows if row["profile"] == "invalid")
            self.assertEqual(invalid["status"], "expected-invalid")
            self.assertEqual(invalid["completion_csv"], "")
            self.assertEqual(invalid["reason"], "deliberate bound rejection")


if __name__ == "__main__":
    unittest.main()
