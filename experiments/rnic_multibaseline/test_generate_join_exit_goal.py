#!/usr/bin/env python3
from __future__ import annotations

import unittest
from fractions import Fraction

import generate_join_exit_goal


class JoinExitGoalTest(unittest.TestCase):
    def test_schedule_joins_and_exits_in_requested_order(self) -> None:
        joins, exits, payloads = generate_join_exit_goal.ideal_schedule(
            8, 5_000_000, 400_000_000_000
        )
        self.assertEqual(joins, [index * 5_000_000 for index in range(8)])
        self.assertEqual(exits, [(8 + index) * 5_000_000 for index in range(8)])
        self.assertEqual(payloads[0], payloads[-1])
        self.assertEqual(payloads[1], payloads[-2])
        self.assertLess(payloads[3], payloads[2])
        self.assertEqual(payloads[3], payloads[4])

        # Ceil rounding adds less than one byte relative to the exact
        # processor-sharing integral, far below one physical packet.
        byte_rate = Fraction(400_000_000_000, 8)
        for payload in payloads:
            self.assertGreater(payload, 0)
            self.assertLess(Fraction(payload, 1) / byte_rate, Fraction(1, 10))

    def test_goal_uses_compute_only_for_join_and_never_for_exit(self) -> None:
        goal, metadata = generate_join_exit_goal.build_goal(
            generate_join_exit_goal.DEFAULT_NODE_COUNT,
            generate_join_exit_goal.DEFAULT_SOURCES,
            generate_join_exit_goal.DEFAULT_DESTINATION,
            generate_join_exit_goal.DEFAULT_INTERVAL_NS,
            generate_join_exit_goal.DEFAULT_LINK_BPS,
        )
        self.assertEqual(goal.count(" send "), 8)
        self.assertEqual(goal.count(" recv "), 8)
        self.assertEqual(goal.count(" calc "), 7)
        self.assertEqual(goal.count(" requires "), 7)
        self.assertNotIn("stop", goal.lower())
        self.assertEqual(metadata["join_time_ns"][7], 35_000_000)
        self.assertEqual(metadata["ideal_target_completion_time_ns"][0], 40_000_000)
        self.assertEqual(metadata["ideal_target_completion_time_ns"][7], 75_000_000)
        self.assertEqual(metadata["exit_zoom_window_ns"], [67_500_000, 72_500_000])
        self.assertEqual(metadata["payload_bytes"][0], 679_464_286)


if __name__ == "__main__":
    unittest.main()
