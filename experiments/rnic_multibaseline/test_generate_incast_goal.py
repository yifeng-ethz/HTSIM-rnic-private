#!/usr/bin/env python3
from __future__ import annotations

import unittest

import generate_incast_goal


class IncastGoalTest(unittest.TestCase):
    def test_remote_widths_have_distinct_sources(self) -> None:
        for width in (2, 4, 8, 16, 32):
            goal, metadata = generate_incast_goal.build_goal(
                64, width, 32 * 1024 * 1024, 7
            )
            self.assertEqual(goal.count(" send "), width)
            self.assertEqual(goal.count(" recv "), width)
            self.assertEqual(metadata["distinct_source_count"], width)
            self.assertNotIn(metadata["destination_node"], metadata["source_nodes"])

    def test_sixty_four_flows_stay_on_sixty_four_physical_nodes(self) -> None:
        goal, metadata = generate_incast_goal.build_goal(
            64, 64, 32 * 1024 * 1024, 11
        )
        self.assertEqual(goal.count(" send "), 64)
        self.assertEqual(metadata["distinct_source_count"], 63)
        self.assertEqual(len(metadata["source_nodes"]), 64)
        self.assertEqual(len(set(metadata["source_nodes"])), 63)
        self.assertNotIn(metadata["destination_node"], metadata["source_nodes"])

    def test_seed_replays_and_changes_placement_only(self) -> None:
        first_goal, first = generate_incast_goal.build_goal(64, 8, 1024, 3)
        replay_goal, replay = generate_incast_goal.build_goal(64, 8, 1024, 3)
        other_goal, other = generate_incast_goal.build_goal(64, 8, 1024, 4)
        self.assertEqual(first_goal, replay_goal)
        self.assertEqual(first, replay)
        self.assertNotEqual(first_goal, other_goal)
        self.assertEqual(first["flow_count"], other["flow_count"])
        self.assertEqual(first["payload_bytes_per_flow"], other["payload_bytes_per_flow"])


if __name__ == "__main__":
    unittest.main()
