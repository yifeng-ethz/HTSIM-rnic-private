#!/usr/bin/env python3

from __future__ import annotations

import csv
import contextlib
import io
import json
import os
import tempfile
import textwrap
import unittest
from pathlib import Path

import run_multibaseline as runner


class SelectionAndGoalTest(unittest.TestCase):
    def test_seed_ranges_are_sorted_and_deduplicated(self) -> None:
        self.assertEqual(runner.parse_seed_selection("3,1-2,2"), (1, 2, 3))
        with self.assertRaisesRegex(Exception, "descending"):
            runner.parse_seed_selection("3-1")

    def test_flat_goal_has_the_exact_384_transfer_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            artifact = runner.generate_workload(
                "flat32m", 7, Path(temporary)
            )
        self.assertEqual(len(artifact.transfers), 384)
        self.assertEqual(
            {transfer.payload_bytes for transfer in artifact.transfers},
            {32 * 1024 * 1024},
        )
        self.assertEqual(artifact.metadata["rank_mapping_seed"], 7)

    def test_dag_goal_preserves_flat_transfers_and_adds_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            flat = runner.generate_workload("flat32m", 7, root)
            dag = runner.generate_workload("dag32m", 7, root)
        self.assertEqual(flat.transfers, dag.transfers)
        self.assertEqual(dag.metadata["mode"], "dag")
        self.assertEqual(dag.seed_key, "seed-7")
        self.assertIn(" requires ", dag.goal_text)
        self.assertNotIn(" requires ", flat.goal_text)

    def test_join_exit_is_seed_independent_and_has_eight_flows(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            first = runner.generate_workload("join-exit", 1, Path(temporary))
            second = runner.generate_workload("join-exit", 99, Path(temporary))
        self.assertEqual(first.seed_key, "shared")
        self.assertEqual(first.goal_sha256, second.goal_sha256)
        self.assertEqual(len(first.transfers), 8)


class CommandBuilderTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tools = runner.ToolPaths(
            Path("/tools/txt2bin"), Path("/tools/rnic"), Path("/tools/dcqcn")
        )
        self.parameters = runner.ExperimentParameters()

    def test_dcqcn_builder_pins_topology_packet_and_seed_contract(self) -> None:
        command = runner.build_simulator_command(
            self.tools,
            "dcqcn",
            Path("/w.bin"),
            Path("/flows.csv"),
            Path("/clos.topo"),
            17,
            self.parameters,
        )
        self.assertEqual(command[0], "/tools/dcqcn")
        self.assertEqual(command[command.index("-seed") + 1], "17")
        self.assertEqual(command[command.index("-topology") + 1], "/clos.topo")
        self.assertEqual(
            command[command.index("-max_wire_packet_bytes") + 1], "4160"
        )
        self.assertEqual(command[command.index("-ecn_kmin_bytes") + 1], "65536")
        self.assertEqual(command[command.index("-ecn_kmax_bytes") + 1], "655360")
        self.assertEqual(command[command.index("-ecn_pmax_ppm") + 1], "250000")
        self.assertEqual(
            command[command.index("-shared_buffer_bytes") + 1], "67108864"
        )
        self.assertEqual(
            command[command.index("-egress_buffer_bytes") + 1], "4194304"
        )
        self.assertEqual(
            command[command.index("-dcqcn_min_rate_bps") + 1], "100000000"
        )
        self.assertNotIn("-ecn_threshold_bytes", command)

    def test_dcqcn_egress_domain_bounds_ecn_not_ingress_pfc(self) -> None:
        with self.assertRaisesRegex(
            runner.OrchestrationError, "per-egress"
        ):
            runner.ExperimentParameters(
                dcqcn_egress_buffer_bytes=655_360
            ).validate()
        with self.assertRaisesRegex(
            runner.OrchestrationError, "per-egress"
        ):
            runner.ExperimentParameters(
                dcqcn_egress_buffer_bytes=(64 << 20) + 1
            ).validate()

        # PFC meters a physical ingress and may therefore have a threshold
        # above the independent per-egress admission cap.
        runner.ExperimentParameters(
            dcqcn_egress_buffer_bytes=2 << 20,
            dcqcn_pfc_low_bytes=3 << 20,
            dcqcn_pfc_high_bytes=4 << 20,
        ).validate()

        with self.assertRaisesRegex(
            runner.OrchestrationError, "per-ingress"
        ):
            runner.ExperimentParameters(
                dcqcn_pfc_low_bytes=32 << 20,
                dcqcn_pfc_high_bytes=64 << 20,
            ).validate()

    def test_cn_margin_is_a_fraction_of_link_capacity(self) -> None:
        with self.assertRaisesRegex(
            runner.OrchestrationError, "cn_margin_ppm"
        ):
            runner.ExperimentParameters(cn_margin_ppm=1_000_001).validate()

    def test_study_cli_records_explicit_dcqcn_egress_domain(self) -> None:
        parser = runner.build_parser(Path("/repo"))
        args = parser.parse_args(
            [
                "--output-root",
                "/results",
                "--dcqcn-egress-buffer-bytes",
                "2097152",
                "--cn-margin-ppm",
                "850000",
                "--cn-retransmission-rto-ps",
                "25000000000",
            ]
        )
        self.assertEqual(args.dcqcn_egress_buffer_bytes, 2 << 20)
        self.assertEqual(args.cn_margin_ppm, 850_000)
        self.assertEqual(args.cn_retransmission_rto_ps, 25_000_000_000)

    def test_rnic_profiles_receive_only_applicable_flags(self) -> None:
        common = (
            self.tools,
            Path("/w.bin"),
            Path("/flows.csv"),
            Path("/clos.topo"),
            23,
            self.parameters,
        )
        cn = runner.build_simulator_command(common[0], "rnic-cn", *common[1:])
        ss = runner.build_simulator_command(common[0], "rnic-ss", *common[1:])
        nn = runner.build_simulator_command(common[0], "rnic-nn", *common[1:])
        fluid = runner.build_simulator_command(
            common[0], "rnic-nn-fluid", *common[1:]
        )
        self.assertIn("-rnic_cn_prbs_seed", cn)
        self.assertEqual(
            cn[cn.index("-rnic_cn_max_retransmissions") + 1], "8"
        )
        self.assertEqual(
            cn[cn.index("-rnic_cn_retransmission_rto_ps") + 1],
            "50000000000",
        )
        self.assertNotIn("-rnic_ss_routing_seed", cn)
        self.assertIn("-rnic_ss_routing_seed", ss)
        self.assertEqual(ss[ss.index("-rnic_ss_routing") + 1], "ordered")
        self.assertEqual(ss[ss.index("-rnic_ss_window_packets") + 1], "128")
        self.assertNotIn("-topo", nn)
        self.assertIn("-rnic_nn_propagation_ps", nn)
        self.assertNotIn("-rnic_max_wire_bytes", fluid)
        self.assertIn("-rnic_nn_propagation_ps", fluid)

    def test_join_exit_state_trace_flags_are_profile_specific(self) -> None:
        dcqcn = runner.build_simulator_command(
            self.tools,
            "dcqcn",
            Path("/w.bin"),
            Path("/flows.csv"),
            Path("/clos.topo"),
            1,
            self.parameters,
            Path("/state.csv"),
        )
        cn = runner.build_simulator_command(
            self.tools,
            "rnic-cn",
            Path("/w.bin"),
            Path("/flows.csv"),
            Path("/clos.topo"),
            1,
            self.parameters,
            Path("/state.csv"),
        )
        ss = runner.build_simulator_command(
            self.tools,
            "rnic-ss",
            Path("/w.bin"),
            Path("/flows.csv"),
            Path("/clos.topo"),
            1,
            self.parameters,
            Path("/state.csv"),
        )
        self.assertEqual(
            dcqcn[dcqcn.index("-state_trace_csv") + 1], "/state.csv"
        )
        self.assertEqual(
            cn[cn.index("-rnic_cn_state_trace_csv") + 1], "/state.csv"
        )
        self.assertEqual(
            ss[ss.index("-rnic_ss_state_trace_csv") + 1], "/state.csv"
        )
        with self.assertRaisesRegex(runner.OrchestrationError, "not implemented"):
            runner.build_simulator_command(
                self.tools,
                "rnic-nn",
                Path("/w.bin"),
                Path("/flows.csv"),
                Path("/clos.topo"),
                1,
                self.parameters,
                Path("/state.csv"),
            )


def write_completion(
    path: Path,
    scheme: str,
    transfers: tuple[runner.Transfer, ...],
    omit_last: bool = False,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=runner.REQUIRED_COMPLETION_COLUMNS
        )
        writer.writeheader()
        selected = transfers[:-1] if omit_last else transfers
        for flow_id, transfer in enumerate(selected):
            writer.writerow(
                {
                    "profile": scheme,
                    "flow_id": flow_id,
                    "source": transfer.source,
                    "destination": transfer.destination,
                    "tag": transfer.tag,
                    "payload_bytes": transfer.payload_bytes,
                    "start_time_ps": 0,
                    "completion_time_ps": 100 + flow_id,
                    "fct_ps": 100 + flow_id,
                }
            )


class CompletionValidationTest(unittest.TestCase):
    def test_state_trace_requires_common_schema_and_flow_lifecycle(self) -> None:
        transfer = runner.Transfer(0, 2, 0, 100)
        completion = runner.CompletionSummary(
            (runner.CompletionRow("dcqcn", 7, transfer, 0, 100, 100),)
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.csv"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle, fieldnames=runner.REQUIRED_STATE_TRACE_COLUMNS
                )
                writer.writeheader()
                common = {
                    "flow_id": 7,
                    "source": 0,
                    "destination": 2,
                    "configured_rate_bps": 100,
                    "effective_rate_bps": 100,
                    "alpha": "",
                    "paused": "false",
                    "new_packets_sent": 0,
                    "rtx_packets_sent": 0,
                    "acked_packets": 0,
                }
                writer.writerow({**common, "time_ps": 0, "event": "flow-start"})
                writer.writerow({**common, "time_ps": 100, "event": "completion"})
            self.assertEqual(
                runner.validate_state_trace_csv(path, completion), 2
            )
    def test_partial_success_csv_is_rejected(self) -> None:
        transfers = (
            runner.Transfer(0, 2, 0, 100),
            runner.Transfer(1, 2, 1, 100),
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "flows.csv"
            write_completion(path, "dcqcn", transfers, omit_last=True)
            with self.assertRaisesRegex(runner.OrchestrationError, "mismatch"):
                runner.read_completion_csv(path, "dcqcn", transfers)

    def test_inconsistent_fct_is_rejected(self) -> None:
        transfer = (runner.Transfer(0, 2, 0, 100),)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "flows.csv"
            write_completion(path, "rnic-cn", transfer)
            with path.open(encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            rows[0]["fct_ps"] = "99"
            with path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(
                    handle, fieldnames=runner.REQUIRED_COMPLETION_COLUMNS
                )
                writer.writeheader()
                writer.writerows(rows)
            with self.assertRaisesRegex(runner.OrchestrationError, "inconsistent"):
                runner.read_completion_csv(path, "rnic-cn", transfer)

    def test_paired_schemes_must_keep_flow_ids_and_transfers_aligned(self) -> None:
        transfer = runner.Transfer(0, 2, 0, 100)
        dcqcn = runner.CompletionSummary(
            (runner.CompletionRow("dcqcn", 0, transfer, 0, 100, 100),)
        )
        rnic = runner.CompletionSummary(
            (runner.CompletionRow("rnic-nn", 1, transfer, 0, 100, 100),)
        )
        with self.assertRaisesRegex(runner.OrchestrationError, "mismatched"):
            runner.validate_paired_results(
                {
                    ("join-exit", 1, "dcqcn"): dcqcn,
                    ("join-exit", 1, "rnic-nn"): rnic,
                },
                ("join-exit",),
                (1,),
                ("dcqcn", "rnic-nn"),
            )


class EndToEndOrchestrationTest(unittest.TestCase):
    def make_tool(self, path: Path, body: str) -> Path:
        path.write_text("#!/usr/bin/env python3\n" + body, encoding="utf-8")
        path.chmod(0o755)
        return path

    def test_fake_tools_produce_valid_reusable_paired_runs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            counter = root / "invocations.txt"
            converter = self.make_tool(
                root / "converter.py",
                textwrap.dedent(
                    f"""
                    import pathlib, shutil, sys
                    args = sys.argv[1:]
                    shutil.copyfile(args[args.index('-i') + 1], args[args.index('-o') + 1])
                    with pathlib.Path({str(counter)!r}).open('a') as handle:
                        handle.write('converter\\n')
                    """
                ),
            )
            simulator = self.make_tool(
                root / "simulator.py",
                textwrap.dedent(
                    f"""
                    import csv, pathlib, re, sys
                    args = sys.argv[1:]
                    scheme = (
                        args[args.index('-rnic_profile') + 1]
                        if '-rnic_profile' in args else 'dcqcn'
                    )
                    goal = pathlib.Path(args[args.index('-goal') + 1]).read_text()
                    output = pathlib.Path(args[args.index('-completion_csv') + 1])
                    rank = None
                    sends = []
                    for raw in goal.splitlines():
                        line = raw.strip()
                        match = re.fullmatch(r'rank\\s+(\\d+)\\s*\\{{', line)
                        if match:
                            rank = int(match.group(1))
                        elif line == '}}':
                            rank = None
                        else:
                            sent = re.match(
                                r'^[^:]+:\\s+send\\s+(\\d+)b\\s+to\\s+(\\d+)'
                                r'\\s+tag\\s+(\\d+)\\b',
                                line,
                            )
                            if sent:
                                sends.append((
                                    rank, int(sent.group(2)), int(sent.group(3)),
                                    int(sent.group(1)),
                                ))
                    output.parent.mkdir(parents=True, exist_ok=True)
                    fields = {runner.REQUIRED_COMPLETION_COLUMNS!r}
                    with output.open('w', newline='') as handle:
                        writer = csv.writer(handle)
                        writer.writerow(fields)
                        for flow_id, (source, destination, tag, payload) in enumerate(sends):
                            writer.writerow([
                                scheme, flow_id, source, destination, tag,
                                payload, 0, 100 + flow_id, 100 + flow_id,
                            ])
                    trace_flag = (
                        '-state_trace_csv' if '-state_trace_csv' in args
                        else '-rnic_cn_state_trace_csv'
                        if '-rnic_cn_state_trace_csv' in args
                        else '-rnic_ss_state_trace_csv'
                        if '-rnic_ss_state_trace_csv' in args else None
                    )
                    if trace_flag is not None:
                        trace = pathlib.Path(args[args.index(trace_flag) + 1])
                        trace.parent.mkdir(parents=True, exist_ok=True)
                        trace_fields = {runner.REQUIRED_STATE_TRACE_COLUMNS!r}
                        with trace.open('w', newline='') as handle:
                            writer = csv.writer(handle)
                            writer.writerow(trace_fields)
                            for flow_id, (source, destination, tag, payload) in enumerate(sends):
                                writer.writerow([
                                    0, flow_id, source, destination, 'flow-start',
                                    400000000000, 400000000000, '', 'false',
                                    0, 0, 0,
                                ])
                            for flow_id, (source, destination, tag, payload) in enumerate(sends):
                                writer.writerow([
                                    100 + flow_id, flow_id, source, destination,
                                    'completion', 400000000000, 400000000000,
                                    '', 'false', 1, 0, 1,
                                ])
                    with pathlib.Path({str(counter)!r}).open('a') as handle:
                        handle.write(scheme + '\\n')
                    """
                ),
            )
            topology = root / "clos.topo"
            topology.write_text("test topology\n", encoding="utf-8")
            tools = runner.ToolPaths(converter, simulator, simulator)
            output = root / "output"
            kwargs = dict(
                repo_root=root,
                output_root=output,
                tools=tools,
                topology=topology,
                parameters=runner.ExperimentParameters(),
                workloads=("join-exit",),
                seeds=(1,),
                schemes=("dcqcn", "rnic-nn"),
            )
            runner.orchestrate(**kwargs)
            first_invocations = counter.read_text(encoding="utf-8").splitlines()
            self.assertEqual(first_invocations, ["converter", "dcqcn", "rnic-nn"])
            runner.orchestrate(**kwargs)
            self.assertEqual(
                counter.read_text(encoding="utf-8").splitlines(),
                first_invocations,
            )
            manifest = json.loads(
                (output / "experiment_manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["run_count"], 2)
            for scheme in ("dcqcn", "rnic-nn"):
                self.assertTrue(
                    (output / "results/join-exit" / scheme / "seed-1/flowsInfo.csv").is_file()
                )
            trace = output / "results/join-exit/dcqcn/seed-1/stateTrace.csv"
            self.assertTrue(trace.is_file())
            run_manifest = json.loads(
                (trace.parent / "run_manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(run_manifest["state_trace_rows"], 16)
            self.assertEqual(
                run_manifest["state_trace_sha256"], runner.sha256_file(trace)
            )

    def test_stale_partial_conversion_requires_force(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            converter = self.make_tool(root / "converter.py", "raise SystemExit(0)\n")
            artifact = runner.generate_workload("join-exit", 1, root / "output")
            partial = artifact.binary_path.with_name("workload.bin.partial")
            partial.parent.mkdir(parents=True, exist_ok=True)
            partial.write_bytes(b"stale")
            with self.assertRaisesRegex(runner.OrchestrationError, "partial converted"):
                runner.materialize_workload(
                    artifact,
                    runner.ToolPaths(converter, converter, converter),
                    root,
                    False,
                    None,
                )

    def test_converter_success_without_output_writes_failed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            converter = self.make_tool(root / "converter.py", "raise SystemExit(0)\n")
            artifact = runner.generate_workload("join-exit", 1, root / "output")
            with self.assertRaisesRegex(runner.OrchestrationError, "without a nonempty"):
                runner.materialize_workload(
                    artifact,
                    runner.ToolPaths(converter, converter, converter),
                    root,
                    False,
                    None,
                )
            manifest = json.loads(
                artifact.binary_path.with_name("conversion_manifest.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(manifest["status"], "failed")
            self.assertEqual(manifest["return_code"], 0)

    def test_dry_run_does_not_touch_output_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "absent"
            with contextlib.redirect_stdout(io.StringIO()):
                runner.orchestrate(
                    repo_root=root,
                    output_root=output,
                    tools=runner.ToolPaths(
                        Path("/missing/c"), Path("/missing/r"), Path("/missing/d")
                    ),
                    topology=Path("/missing/topology"),
                    parameters=runner.ExperimentParameters(),
                    workloads=("join-exit",),
                    seeds=(1,),
                    schemes=("dcqcn",),
                    dry_run=True,
                )
            self.assertFalse(output.exists())

    def test_nonzero_simulator_exit_writes_failed_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            converter = self.make_tool(
                root / "converter.py",
                textwrap.dedent(
                    """
                    import shutil, sys
                    args = sys.argv[1:]
                    shutil.copyfile(args[args.index('-i') + 1], args[args.index('-o') + 1])
                    """
                ),
            )
            simulator = self.make_tool(
                root / "simulator.py",
                "import sys\nprint('deliberate failure', file=sys.stderr)\nraise SystemExit(7)\n",
            )
            topology = root / "clos.topo"
            topology.write_text("test topology\n", encoding="utf-8")
            output = root / "output"
            with self.assertRaisesRegex(runner.OrchestrationError, "exited 7"):
                runner.orchestrate(
                    repo_root=root,
                    output_root=output,
                    tools=runner.ToolPaths(converter, simulator, simulator),
                    topology=topology,
                    parameters=runner.ExperimentParameters(),
                    workloads=("join-exit",),
                    seeds=(1,),
                    schemes=("dcqcn",),
                )
            run_directory = output / "results/join-exit/dcqcn/seed-1"
            manifest = json.loads(
                (run_directory / "run_manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["status"], "failed")
            self.assertEqual(manifest["return_code"], 7)
            self.assertIn(
                "deliberate failure",
                (run_directory / "stderr.log").read_text(encoding="utf-8"),
            )
            self.assertFalse((run_directory / "flowsInfo.csv").exists())


if __name__ == "__main__":
    unittest.main()
