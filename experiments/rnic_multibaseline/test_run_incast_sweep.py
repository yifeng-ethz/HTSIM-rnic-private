#!/usr/bin/env python3
from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

import run_incast_sweep as sweep
import run_multibaseline as runner


class IncastSweepTest(unittest.TestCase):
    def test_goodput_uses_last_receiver_completion(self) -> None:
        transfer = runner.Transfer(0, 2, 0, 1_000)
        other = runner.Transfer(1, 2, 1, 1_000)
        summary = runner.CompletionSummary(
            (
                runner.CompletionRow(
                    "rnic-cn", 0, transfer, 10, 1_000_010, 1_000_000
                ),
                runner.CompletionRow(
                    "rnic-cn", 1, other, 10, 2_000_010, 2_000_000
                ),
            )
        )
        self.assertAlmostEqual(sweep.payload_goodput_gbps(summary), 8.0)

    def test_summary_records_sixty_four_flow_source_distinction(self) -> None:
        rows = []
        for flow in range(64):
            transfer = runner.Transfer(flow % 63, 63, flow, 1000)
            rows.append(
                runner.CompletionRow("rnic-nn", flow, transfer, 0, 1000, 1000)
            )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sweep.write_summary(
                root,
                {(64, 1, "rnic-nn"): runner.CompletionSummary(tuple(rows))},
                (64,),
                (1,),
                ("rnic-nn",),
            )
            with (root / "incast_goodput_summary.csv").open(
                encoding="utf-8", newline=""
            ) as handle:
                result = next(csv.DictReader(handle))
        self.assertEqual(result["flow_count"], "64")
        self.assertEqual(result["distinct_source_count"], "63")

    def test_width_parser_rejects_unreported_points(self) -> None:
        self.assertEqual(sweep.parse_widths("2,64"), (2, 64))
        with self.assertRaises(Exception):
            sweep.parse_widths("3")


if __name__ == "__main__":
    unittest.main()
