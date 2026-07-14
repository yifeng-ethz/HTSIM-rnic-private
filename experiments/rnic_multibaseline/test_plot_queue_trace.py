#!/usr/bin/env python3
from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

import plot_queue_trace


HEADERS = (
    "time_ps",
    "tier",
    "switch_id",
    "ingress_id",
    "egress_id",
    "transition",
    "priority",
    "flow_id",
    "packet_id",
    "packet_bytes",
    "egress_buffered_bytes",
    "egress_in_service_bytes",
    "egress_backlog_bytes",
    "shared_buffer_occupancy_bytes",
)


class QueueTraceTest(unittest.TestCase):
    def test_filters_one_port_and_preserves_same_time_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "trace.csv"
            rows = [
                (100, "leaf", 7, 2, 4, "enqueue", 2, 1, 10, 100, 100, 0, 100, 100),
                (100, "spine", 0, 0, 1, "enqueue", 2, 2, 11, 100, 100, 0, 100, 100),
                (100, "leaf", 7, 2, 4, "dequeue", 2, 1, 10, 100, 0, 100, 100, 0),
                (200, "leaf", 7, 2, 4, "serialization-complete", 2, 1, 10, 100, 0, 0, 0, 0),
            ]
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(HEADERS)
                writer.writerows(rows)

            points = plot_queue_trace.read_port_trace(path, "leaf", 7, 4)
            self.assertEqual([point.time_ps for point in points], [100, 100, 200])
            self.assertEqual(
                [point.transition for point in points],
                ["enqueue", "dequeue", "serialization-complete"],
            )
            self.assertEqual(points[1].backlog_bytes, 100)

    def test_queue_delay_uses_exact_rate_conversion(self) -> None:
        self.assertEqual(
            plot_queue_trace.queue_delay_ps(200_000, 400_000_000_000),
            4_000_000,
        )

    def test_finds_exact_tagged_packet_queue_interval(self) -> None:
        points = [
            plot_queue_trace.TracePoint(100, "enqueue", 100, 0, 100, 7, 11),
            plot_queue_trace.TracePoint(200, "enqueue", 200, 0, 200, 8, 12),
            plot_queue_trace.TracePoint(350, "dequeue", 100, 100, 200, 7, 11),
        ]
        self.assertEqual(
            plot_queue_trace.packet_queue_interval_ps(points, 11),
            (100, 350),
        )
        with self.assertRaises(plot_queue_trace.TraceError):
            plot_queue_trace.packet_queue_interval_ps(points, 99)


if __name__ == "__main__":
    unittest.main()
