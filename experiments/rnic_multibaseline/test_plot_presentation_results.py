#!/usr/bin/env python3

from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

import plot_presentation_results as plots


TRACE_FIELDS = (
    "time_ps",
    "flow_id",
    "source",
    "destination",
    "event",
    "configured_rate_bps",
    "effective_rate_bps",
    "alpha",
    "paused",
    "new_packets_sent",
    "rtx_packets_sent",
    "acked_packets",
)


class JoinDynamicsTraceTest(unittest.TestCase):
    def test_sparse_rate_series_does_not_turn_ack_progress_into_samples(self) -> None:
        rows = [
            {
                "time_ps": 0,
                "flow_id": 7,
                "source": 0,
                "destination": 63,
                "event": "flow-start",
                "effective_rate_bps": 400,
            },
            {
                "time_ps": 10,
                "flow_id": 7,
                "source": 0,
                "destination": 63,
                "event": "ack-progress",
                "effective_rate_bps": 400,
            },
            {
                "time_ps": 20,
                "flow_id": 7,
                "source": 0,
                "destination": 63,
                "event": "pause",
                "effective_rate_bps": 0,
            },
        ]
        series = plots._rate_series_by_flow(rows)
        self.assertEqual(series[7], [(0, 400), (20, 0)])
        x, y = plots._windowed_step(series[7], 5, 25)
        self.assertEqual(x, [5e-9, 2e-8, 2.5e-8])
        self.assertEqual(y, [4e-7, 0.0, 0.0])

    def test_reader_requires_effective_rate_and_common_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.csv"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=TRACE_FIELDS)
                writer.writeheader()
                writer.writerow(
                    {
                        "time_ps": 0,
                        "flow_id": 1,
                        "source": 0,
                        "destination": 63,
                        "event": "flow-start",
                        "configured_rate_bps": 400,
                        "effective_rate_bps": 400,
                        "alpha": "",
                        "paused": "",
                        "new_packets_sent": 0,
                        "rtx_packets_sent": 0,
                        "acked_packets": 0,
                    }
                )
            rows = plots.read_state_trace(path)
            self.assertEqual(rows[0]["event"], "flow-start")
            self.assertEqual(rows[0]["effective_rate_bps"], 400)


if __name__ == "__main__":
    unittest.main()
