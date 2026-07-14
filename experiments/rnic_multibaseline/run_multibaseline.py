#!/usr/bin/env python3
"""Run paired 64-node RNIC/DCQCN experiments with strict provenance checks.

The runner owns workload generation, GOAL conversion, scheme-specific command
construction, logs, manifests, and completion validation.  It never accepts a
zero process exit as sufficient evidence of success: the completion CSV must
exactly match every directed ``send`` in the generated GOAL.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
import shlex
import subprocess
import sys
import time
from collections import Counter
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Mapping, Sequence

import generate_allreduce_goal
import generate_join_exit_goal


SCHEMA_VERSION = "rnic-multibaseline-run-v2"
SCHEMES = ("dcqcn", "rnic-cn", "rnic-ss", "rnic-nn", "rnic-nn-fluid")
WORKLOADS = ("flat32m", "dag32m", "join-exit")
RNIC_SCHEMES = frozenset(SCHEMES) - {"dcqcn"}
PHYSICAL_SCHEMES = frozenset({"dcqcn", "rnic-cn", "rnic-ss"})

REQUIRED_COMPLETION_COLUMNS = (
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

STATE_TRACE_SCHEMES = frozenset({"dcqcn", "rnic-cn", "rnic-ss"})
REQUIRED_STATE_TRACE_COLUMNS = (
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

RANK_RE = re.compile(r"^rank\s+(\d+)\s*\{$")
SEND_RE = re.compile(
    r"^[^:]+:\s+send\s+(\d+)b\s+to\s+(\d+)\s+tag\s+(\d+)\b"
)


class OrchestrationError(RuntimeError):
    """Raised when execution or provenance violates the experiment contract."""


@dataclass(frozen=True, order=True)
class Transfer:
    source: int
    destination: int
    tag: int
    payload_bytes: int


@dataclass(frozen=True)
class ToolPaths:
    goal_converter: Path
    rnic_simulator: Path
    dcqcn_simulator: Path


@dataclass(frozen=True)
class ExperimentParameters:
    node_count: int = 64
    link_bps: int = 400_000_000_000
    max_wire_packet_bytes: int = 4160
    data_header_bytes: int = 64
    physical_buffer_bytes: int = 64 << 20
    nn_propagation_ps: int = 2_000_000
    cn_control_deadline_ps: int = 10_000_000
    cn_margin_ppm: int = 900_000
    cn_control_wire_bytes: int = 64
    cn_ring_window_ps: int = 4_096_000
    cn_ring_tick_ps: int = 16_000
    cn_ring_capacity_bytes: int = 1 << 20
    cn_maximum_retransmissions: int = 8
    cn_retransmission_rto_ps: int = 50_000_000_000
    ss_control_wire_bytes: int = 64
    ss_q_hi_bytes: int = 4 << 20
    ss_q_lo_bytes: int = 2 << 20
    ss_telemetry_delay_ps: int = 0
    ss_path_hysteresis_ps: int = 0
    ss_sample_age_ps: int = 10_000_000
    ss_credit_quantum_packets: int = 4
    ss_window_packets: int = 128
    ss_rto_ps: int = 50_000_000_000
    ss_max_retransmissions: int = 8
    ss_max_credit_ahead_bytes: int = 64 << 20
    ss_routing: str = "ordered"
    dcqcn_ecn_kmin_bytes: int = 65_536
    dcqcn_ecn_kmax_bytes: int = 655_360
    dcqcn_ecn_pmax_ppm: int = 250_000
    dcqcn_pfc_low_bytes: int = 520_000
    dcqcn_pfc_high_bytes: int = 720_000
    dcqcn_egress_buffer_bytes: int = 4 << 20
    dcqcn_min_rate_bps: int = 100_000_000
    dcqcn_silent_rto_us: int = 50_000

    def validate(self) -> None:
        positive = {
            key: value
            for key, value in asdict(self).items()
            if key != "dcqcn_ecn_kmin_bytes"
            and key != "ss_q_lo_bytes"
            and key != "ss_telemetry_delay_ps"
            and key != "ss_path_hysteresis_ps"
            and key != "ss_routing"
        }
        for name, value in positive.items():
            if not isinstance(value, int) or value <= 0:
                raise OrchestrationError(f"{name} must be a positive integer")
        if self.node_count != 64:
            raise OrchestrationError("this experiment contract requires 64 nodes")
        if self.cn_margin_ppm > 1_000_000:
            raise OrchestrationError("cn_margin_ppm must be in 1..1000000")
        if self.data_header_bytes >= self.max_wire_packet_bytes:
            raise OrchestrationError("DATA header must be smaller than wire packet")
        if not 0 <= self.ss_q_lo_bytes < self.ss_q_hi_bytes:
            raise OrchestrationError("rnic-ss requires 0 <= Q_lo < Q_hi")
        if self.ss_q_hi_bytes > self.physical_buffer_bytes:
            raise OrchestrationError("rnic-ss Q_hi exceeds the physical buffer")
        if not (
            0 <= self.dcqcn_ecn_kmin_bytes
            < self.dcqcn_ecn_kmax_bytes
            < self.dcqcn_egress_buffer_bytes
            <= self.physical_buffer_bytes
            and 0 < self.dcqcn_pfc_low_bytes < self.dcqcn_pfc_high_bytes
            < self.physical_buffer_bytes
        ):
            raise OrchestrationError(
                "DCQCN thresholds must satisfy per-egress "
                "0 <= Kmin < Kmax < egress-buffer <= shared-buffer and "
                "per-ingress 0 < PFC-low < PFC-high < shared-buffer"
            )
        if not 0 < self.dcqcn_ecn_pmax_ppm <= 1_000_000:
            raise OrchestrationError("DCQCN RED Pmax must be in 1..1000000 ppm")
        if self.dcqcn_min_rate_bps > self.link_bps:
            raise OrchestrationError("DCQCN minimum rate exceeds the link rate")
        if self.ss_routing not in {"ordered", "unordered"}:
            raise OrchestrationError("ss_routing must be ordered or unordered")


@dataclass(frozen=True)
class WorkloadArtifact:
    name: str
    seed_key: str
    goal_path: Path
    metadata_path: Path
    binary_path: Path
    goal_text: str
    metadata: Mapping[str, object]
    transfers: tuple[Transfer, ...]

    @property
    def goal_sha256(self) -> str:
        return sha256_bytes(self.goal_text.encode("utf-8"))


@dataclass(frozen=True)
class CompletionRow:
    profile: str
    flow_id: int
    transfer: Transfer
    start_time_ps: int
    completion_time_ps: int
    fct_ps: int


@dataclass(frozen=True)
class CompletionSummary:
    rows: tuple[CompletionRow, ...]

    @property
    def flow_contract(self) -> tuple[tuple[int, int], ...]:
        return tuple(sorted((row.flow_id, row.transfer.payload_bytes) for row in self.rows))

    @property
    def full_contract(self) -> tuple[tuple[int, Transfer], ...]:
        return tuple(sorted((row.flow_id, row.transfer) for row in self.rows))


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
    except OSError as exc:
        raise OrchestrationError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(content, encoding="utf-8")
    os.replace(temporary, path)


def atomic_write_json(path: Path, value: object) -> None:
    atomic_write_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def parse_uint(text: str | None, context: str) -> int:
    try:
        value = int(text, 10)
    except (TypeError, ValueError) as exc:
        raise OrchestrationError(f"{context}: expected an integer, got {text!r}") from exc
    if value < 0:
        raise OrchestrationError(f"{context}: value must be nonnegative")
    return value


def parse_seed_selection(text: str) -> tuple[int, ...]:
    seeds: set[int] = set()
    for raw_token in text.split(","):
        token = raw_token.strip()
        if not token:
            raise argparse.ArgumentTypeError("seed selection contains an empty item")
        if "-" in token:
            pieces = token.split("-")
            if len(pieces) != 2 or not all(piece.isdigit() for piece in pieces):
                raise argparse.ArgumentTypeError(f"invalid seed range {token!r}")
            begin, end = (int(piece, 10) for piece in pieces)
            if begin > end:
                raise argparse.ArgumentTypeError(f"descending seed range {token!r}")
            seeds.update(range(begin, end + 1))
        else:
            if not token.isdigit():
                raise argparse.ArgumentTypeError(f"invalid seed {token!r}")
            seeds.add(int(token, 10))
    if not seeds:
        raise argparse.ArgumentTypeError("at least one seed is required")
    if max(seeds) > (1 << 64) - 1:
        raise argparse.ArgumentTypeError("seed exceeds uint64_t")
    return tuple(sorted(seeds))


def positive_int(text: str) -> int:
    try:
        value = int(text, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a positive integer") from exc
    if value <= 0:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def parse_name_selection(
    text: str, allowed: Sequence[str], label: str
) -> tuple[str, ...]:
    if text.strip() == "all":
        return tuple(allowed)
    selected: list[str] = []
    for raw in text.split(","):
        value = raw.strip()
        if value not in allowed:
            raise argparse.ArgumentTypeError(
                f"unknown {label} {value!r}; expected {', '.join(allowed)} or all"
            )
        if value not in selected:
            selected.append(value)
    if not selected:
        raise argparse.ArgumentTypeError(f"at least one {label} is required")
    return tuple(selected)


def expected_transfers(goal_text: str) -> tuple[Transfer, ...]:
    current_rank: int | None = None
    transfers: list[Transfer] = []
    for line_number, raw in enumerate(goal_text.splitlines(), start=1):
        line = raw.strip()
        rank = RANK_RE.fullmatch(line)
        if rank:
            current_rank = int(rank.group(1), 10)
            continue
        if line == "}":
            current_rank = None
            continue
        sent = SEND_RE.match(line)
        if sent:
            if current_rank is None:
                raise OrchestrationError(
                    f"GOAL line {line_number}: send appears outside a rank block"
                )
            payload, destination, tag = (int(value, 10) for value in sent.groups())
            transfers.append(Transfer(current_rank, destination, tag, payload))
    if not transfers:
        raise OrchestrationError("GOAL contains no directed send operations")
    duplicates = [item for item, count in Counter(transfers).items() if count != 1]
    if duplicates:
        raise OrchestrationError(f"GOAL has duplicate directed transfers: {duplicates[:3]}")
    return tuple(sorted(transfers))


def generate_workload(
    name: str, seed: int, output_root: Path
) -> WorkloadArtifact:
    if name in {"flat32m", "dag32m"}:
        mode = "flat" if name == "flat32m" else "dag"
        goal_text = generate_allreduce_goal.build_goal(
            64, 32 * 1024 * 1024, seed, mode
        )
        phase_count = 6
        metadata: dict[str, object] = {
            "schema": "rnic-allreduce-goal-v1",
            "algorithm": "recursive-doubling",
            "mode": mode,
            "rank_count": 64,
            "phase_count": phase_count,
            "payload_bytes_per_transfer": 32 * 1024 * 1024,
            "directed_flow_count": 64 * phase_count,
            "rank_mapping_seed": seed,
            "rank_to_physical_node": generate_allreduce_goal.rank_mapping(64, seed),
        }
        seed_key = f"seed-{seed}"
    elif name == "join-exit":
        goal_text, generated = generate_join_exit_goal.build_goal(
            generate_join_exit_goal.DEFAULT_NODE_COUNT,
            generate_join_exit_goal.DEFAULT_SOURCES,
            generate_join_exit_goal.DEFAULT_DESTINATION,
            generate_join_exit_goal.DEFAULT_INTERVAL_NS,
            generate_join_exit_goal.DEFAULT_LINK_BPS,
        )
        metadata = dict(generated)
        seed_key = "shared"
    else:
        raise OrchestrationError(f"unknown workload {name!r}")

    goal_hash = sha256_bytes(goal_text.encode("utf-8"))
    metadata.update(
        {
            "orchestration_schema": SCHEMA_VERSION,
            "workload_name": name,
            "goal_sha256": goal_hash,
        }
    )
    directory = output_root / "workloads" / name / seed_key
    return WorkloadArtifact(
        name=name,
        seed_key=seed_key,
        goal_path=directory / "workload.goal",
        metadata_path=directory / "workload.json",
        binary_path=directory / "workload.bin",
        goal_text=goal_text,
        metadata=metadata,
        transfers=expected_transfers(goal_text),
    )


def build_converter_command(
    converter: Path, goal_path: Path, output_path: Path
) -> list[str]:
    return [str(converter), "-i", str(goal_path), "-o", str(output_path)]


def build_dcqcn_command(
    executable: Path,
    goal_binary: Path,
    completion_csv: Path,
    topology: Path,
    seed: int,
    parameters: ExperimentParameters,
    state_trace_csv: Path | None = None,
) -> list[str]:
    command = [
        str(executable),
        "-goal", str(goal_binary),
        "-topology", str(topology),
        "-completion_csv", str(completion_csv),
        "-goal_rank_mapping", "gpu-rank",
        "-seed", str(seed),
        "-link_bps", str(parameters.link_bps),
        "-max_wire_packet_bytes", str(parameters.max_wire_packet_bytes),
        "-data_header_bytes", str(parameters.data_header_bytes),
        "-shared_buffer_bytes", str(parameters.physical_buffer_bytes),
        "-egress_buffer_bytes", str(parameters.dcqcn_egress_buffer_bytes),
        "-ecn_kmin_bytes", str(parameters.dcqcn_ecn_kmin_bytes),
        "-ecn_kmax_bytes", str(parameters.dcqcn_ecn_kmax_bytes),
        "-ecn_pmax_ppm", str(parameters.dcqcn_ecn_pmax_ppm),
        "-pfc_low_bytes", str(parameters.dcqcn_pfc_low_bytes),
        "-pfc_high_bytes", str(parameters.dcqcn_pfc_high_bytes),
        "-silent_rto_us", str(parameters.dcqcn_silent_rto_us),
        "-dcqcn_min_rate_bps", str(parameters.dcqcn_min_rate_bps),
    ]
    if state_trace_csv is not None:
        command += ["-state_trace_csv", str(state_trace_csv)]
    return command


def build_rnic_command(
    executable: Path,
    scheme: str,
    goal_binary: Path,
    completion_csv: Path,
    topology: Path,
    seed: int,
    parameters: ExperimentParameters,
    state_trace_csv: Path | None = None,
) -> list[str]:
    if scheme not in RNIC_SCHEMES:
        raise OrchestrationError(f"{scheme!r} is not an RNIC profile")
    command = [
        str(executable),
        "-goal", str(goal_binary),
        "-completion_csv", str(completion_csv),
        "-goal_rank_mapping", "gpu-rank",
        "-nodes", str(parameters.node_count),
        "-linkspeed_bps", str(parameters.link_bps),
        "-rnic_profile", scheme,
    ]
    if scheme != "rnic-nn-fluid":
        command += [
            "-rnic_max_wire_bytes", str(parameters.max_wire_packet_bytes),
            "-rnic_data_header_bytes", str(parameters.data_header_bytes),
        ]
    if scheme == "rnic-cn":
        command += [
            "-topo", str(topology),
            "-rnic_cn_prbs_seed", str(seed),
            "-rnic_cn_control_deadline_ps", str(parameters.cn_control_deadline_ps),
            "-rnic_cn_margin_ppm", str(parameters.cn_margin_ppm),
            "-rnic_cn_control_wire_bytes", str(parameters.cn_control_wire_bytes),
            "-rnic_cn_ring_window_ps", str(parameters.cn_ring_window_ps),
            "-rnic_cn_ring_tick_ps", str(parameters.cn_ring_tick_ps),
            "-rnic_cn_ring_capacity_bytes", str(parameters.cn_ring_capacity_bytes),
            "-rnic_cn_max_retransmissions",
            str(parameters.cn_maximum_retransmissions),
            "-rnic_cn_retransmission_rto_ps",
            str(parameters.cn_retransmission_rto_ps),
            "-rnic_cn_ns_tm3_buffer_bytes", str(parameters.physical_buffer_bytes),
        ]
        if state_trace_csv is not None:
            command += ["-rnic_cn_state_trace_csv", str(state_trace_csv)]
    elif scheme == "rnic-ss":
        command += [
            "-topo", str(topology),
            "-rnic_ss_control_wire_bytes", str(parameters.ss_control_wire_bytes),
            "-rnic_ss_ns_rosetta_buffer_bytes", str(parameters.physical_buffer_bytes),
            "-rnic_ss_q_hi_bytes", str(parameters.ss_q_hi_bytes),
            "-rnic_ss_q_lo_bytes", str(parameters.ss_q_lo_bytes),
            "-rnic_ss_telemetry_delay_ps", str(parameters.ss_telemetry_delay_ps),
            "-rnic_ss_path_hysteresis_ps", str(parameters.ss_path_hysteresis_ps),
            "-rnic_ss_sample_age_ps", str(parameters.ss_sample_age_ps),
            "-rnic_ss_credit_quantum_packets", str(parameters.ss_credit_quantum_packets),
            "-rnic_ss_window_packets", str(parameters.ss_window_packets),
            "-rnic_ss_rto_ps", str(parameters.ss_rto_ps),
            "-rnic_ss_max_retransmissions", str(parameters.ss_max_retransmissions),
            "-rnic_ss_max_credit_ahead_bytes", str(parameters.ss_max_credit_ahead_bytes),
            "-rnic_ss_routing_seed", str(seed),
            "-rnic_ss_routing", parameters.ss_routing,
        ]
        if state_trace_csv is not None:
            command += ["-rnic_ss_state_trace_csv", str(state_trace_csv)]
    else:
        if state_trace_csv is not None:
            raise OrchestrationError(
                f"state tracing is not implemented for RNIC profile {scheme!r}"
            )
        command += [
            "-rnic_nn_propagation_ps", str(parameters.nn_propagation_ps),
        ]
    return command


def build_simulator_command(
    tools: ToolPaths,
    scheme: str,
    goal_binary: Path,
    completion_csv: Path,
    topology: Path,
    seed: int,
    parameters: ExperimentParameters,
    state_trace_csv: Path | None = None,
) -> list[str]:
    if scheme == "dcqcn":
        return build_dcqcn_command(
            tools.dcqcn_simulator,
            goal_binary,
            completion_csv,
            topology,
            seed,
            parameters,
            state_trace_csv,
        )
    return build_rnic_command(
        tools.rnic_simulator,
        scheme,
        goal_binary,
        completion_csv,
        topology,
        seed,
        parameters,
        state_trace_csv,
    )


def require_tool(path: Path, label: str) -> None:
    if not path.is_file():
        raise OrchestrationError(f"{label} is missing: {path}")
    if not os.access(path, os.X_OK):
        raise OrchestrationError(f"{label} is not executable: {path}")


def run_logged(
    command: Sequence[str],
    stdout_path: Path,
    stderr_path: Path,
    cwd: Path,
    timeout_seconds: float | None,
) -> tuple[int, float]:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    try:
        with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
            "w", encoding="utf-8"
        ) as stderr:
            completed = subprocess.run(
                list(command),
                cwd=cwd,
                stdout=stdout,
                stderr=stderr,
                check=False,
                timeout=timeout_seconds,
            )
    except subprocess.TimeoutExpired as exc:
        raise OrchestrationError(
            f"command timed out after {timeout_seconds}s: {shlex.join(command)}"
        ) from exc
    return completed.returncode, time.monotonic() - started


def _write_or_verify(path: Path, expected: str, force: bool) -> str:
    if path.exists():
        actual = path.read_text(encoding="utf-8")
        if actual == expected:
            return "reused"
        if not force:
            raise OrchestrationError(
                f"existing generated artifact differs from contract: {path}; use --force"
            )
    atomic_write_text(path, expected)
    return "generated"


def materialize_workload(
    artifact: WorkloadArtifact,
    tools: ToolPaths,
    repo_root: Path,
    force: bool,
    timeout_seconds: float | None,
) -> None:
    metadata_text = json.dumps(artifact.metadata, indent=2, sort_keys=True) + "\n"
    goal_action = _write_or_verify(artifact.goal_path, artifact.goal_text, force)
    _write_or_verify(artifact.metadata_path, metadata_text, force)
    print(f"{goal_action.upper():9s} {artifact.goal_path}")

    manifest_path = artifact.binary_path.with_name("conversion_manifest.json")
    partial_binary = artifact.binary_path.with_name("workload.bin.partial")
    command = build_converter_command(
        tools.goal_converter, artifact.goal_path, partial_binary
    )
    signature = {
        "schema": SCHEMA_VERSION,
        "goal_sha256": artifact.goal_sha256,
        "converter": str(tools.goal_converter),
        "converter_sha256": sha256_file(tools.goal_converter),
        "command": command,
    }
    if artifact.binary_path.exists() and manifest_path.exists() and not force:
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise OrchestrationError(
                f"invalid conversion manifest {manifest_path}: {exc}"
            ) from exc
        if (
            manifest.get("status") == "complete"
            and manifest.get("signature") == signature
            and manifest.get("binary_sha256") == sha256_file(artifact.binary_path)
            and artifact.binary_path.stat().st_size > 0
        ):
            print(f"REUSED    {artifact.binary_path}")
            return
        raise OrchestrationError(
            f"converted GOAL provenance mismatch: {artifact.binary_path}; use --force"
        )
    if not force and any(
        managed.exists()
        for managed in (artifact.binary_path, partial_binary, manifest_path)
    ):
        raise OrchestrationError(
            f"partial converted workload state near {artifact.binary_path}; use --force"
        )

    for managed in (artifact.binary_path, partial_binary, manifest_path):
        if force and managed.exists():
            managed.unlink()
    stdout_path = artifact.binary_path.with_name("converter.stdout.log")
    stderr_path = artifact.binary_path.with_name("converter.stderr.log")
    started_at = utc_now()
    try:
        return_code, duration = run_logged(
            command, stdout_path, stderr_path, repo_root, timeout_seconds
        )
    except OrchestrationError as exc:
        atomic_write_json(
            manifest_path,
            {
                "schema": SCHEMA_VERSION,
                "status": "failed",
                "signature": signature,
                "started_at": started_at,
                "finished_at": utc_now(),
                "error": str(exc),
                "stdout_log": str(stdout_path),
                "stderr_log": str(stderr_path),
            },
        )
        raise
    if return_code != 0:
        atomic_write_json(
            manifest_path,
            {
                "schema": SCHEMA_VERSION,
                "status": "failed",
                "signature": signature,
                "return_code": return_code,
                "started_at": started_at,
                "finished_at": utc_now(),
                "duration_seconds": duration,
                "error": f"converter exited {return_code}",
                "stdout_log": str(stdout_path),
                "stderr_log": str(stderr_path),
            },
        )
        raise OrchestrationError(
            f"GOAL converter exited {return_code}; see {stderr_path}"
        )
    if not partial_binary.is_file() or partial_binary.stat().st_size == 0:
        error = OrchestrationError(
            f"GOAL converter returned success without a nonempty output: {partial_binary}"
        )
        atomic_write_json(
            manifest_path,
            {
                "schema": SCHEMA_VERSION,
                "status": "failed",
                "signature": signature,
                "return_code": return_code,
                "started_at": started_at,
                "finished_at": utc_now(),
                "duration_seconds": duration,
                "error": str(error),
                "stdout_log": str(stdout_path),
                "stderr_log": str(stderr_path),
            },
        )
        raise error
    os.replace(partial_binary, artifact.binary_path)
    atomic_write_json(
        manifest_path,
        {
            "schema": SCHEMA_VERSION,
            "status": "complete",
            "signature": signature,
            "binary_sha256": sha256_file(artifact.binary_path),
            "return_code": return_code,
            "started_at": started_at,
            "finished_at": utc_now(),
            "duration_seconds": duration,
            "stdout_log": str(stdout_path),
            "stderr_log": str(stderr_path),
        },
    )
    print(f"CONVERTED {artifact.binary_path}")


def read_completion_csv(
    path: Path, scheme: str, expected: Iterable[Transfer]
) -> CompletionSummary:
    try:
        handle = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise OrchestrationError(f"cannot read completion CSV {path}: {exc}") from exc
    with handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise OrchestrationError(f"{path}: missing CSV header")
        if len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise OrchestrationError(f"{path}: duplicate CSV headers")
        missing = [name for name in REQUIRED_COMPLETION_COLUMNS if name not in reader.fieldnames]
        if missing:
            raise OrchestrationError(f"{path}: missing columns {', '.join(missing)}")
        rows: list[CompletionRow] = []
        seen_flow_ids: set[int] = set()
        for line_number, raw in enumerate(reader, start=2):
            context = f"{path}:{line_number}"
            if raw["profile"] != scheme:
                raise OrchestrationError(
                    f"{context}: profile {raw['profile']!r} does not match {scheme!r}"
                )
            flow_id = parse_uint(raw["flow_id"], f"{context} flow_id")
            if flow_id in seen_flow_ids:
                raise OrchestrationError(f"{context}: duplicate flow_id {flow_id}")
            seen_flow_ids.add(flow_id)
            transfer = Transfer(
                parse_uint(raw["source"], f"{context} source"),
                parse_uint(raw["destination"], f"{context} destination"),
                parse_uint(raw["tag"], f"{context} tag"),
                parse_uint(raw["payload_bytes"], f"{context} payload_bytes"),
            )
            start = parse_uint(raw["start_time_ps"], f"{context} start_time_ps")
            completion = parse_uint(
                raw["completion_time_ps"], f"{context} completion_time_ps"
            )
            fct = parse_uint(raw["fct_ps"], f"{context} fct_ps")
            if completion < start or completion - start != fct:
                raise OrchestrationError(
                    f"{context}: inconsistent start/completion/FCT timestamps"
                )
            rows.append(CompletionRow(scheme, flow_id, transfer, start, completion, fct))
    if not rows:
        raise OrchestrationError(f"{path}: completion CSV has no flows")
    actual = Counter(row.transfer for row in rows)
    wanted = Counter(expected)
    if actual != wanted:
        missing = list((wanted - actual).elements())
        unexpected = list((actual - wanted).elements())
        raise OrchestrationError(
            f"{path}: completion transfer set mismatch; "
            f"missing={missing[:3]} unexpected={unexpected[:3]} "
            f"expected_count={sum(wanted.values())} actual_count={sum(actual.values())}"
        )
    return CompletionSummary(tuple(sorted(rows, key=lambda row: row.flow_id)))


def validate_state_trace_csv(
    path: Path, completion: CompletionSummary
) -> int:
    """Validate the sparse common state-trace schema against completed flows."""
    try:
        handle = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise OrchestrationError(f"cannot read state trace CSV {path}: {exc}") from exc
    completed = {row.flow_id: row for row in completion.rows}
    seen_events: dict[int, set[str]] = {
        flow_id: set() for flow_id in completed
    }
    previous_time = -1
    row_count = 0
    with handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != REQUIRED_STATE_TRACE_COLUMNS:
            raise OrchestrationError(
                f"{path}: state trace header must be exactly "
                + ",".join(REQUIRED_STATE_TRACE_COLUMNS)
            )
        for line_number, raw in enumerate(reader, start=2):
            context = f"{path}:{line_number}"
            time_ps = parse_uint(raw["time_ps"], f"{context} time_ps")
            if time_ps < previous_time:
                raise OrchestrationError(
                    f"{context}: trace timestamp regressed from {previous_time}"
                )
            previous_time = time_ps
            flow_id = parse_uint(raw["flow_id"], f"{context} flow_id")
            if flow_id not in completed:
                raise OrchestrationError(
                    f"{context}: unknown completed flow_id {flow_id}"
                )
            flow = completed[flow_id]
            source = parse_uint(raw["source"], f"{context} source")
            destination = parse_uint(
                raw["destination"], f"{context} destination"
            )
            if (source, destination) != (
                flow.transfer.source,
                flow.transfer.destination,
            ):
                raise OrchestrationError(
                    f"{context}: endpoints disagree with completion CSV"
                )
            event = raw["event"]
            if not event or any(character in event for character in "\r\n,"):
                raise OrchestrationError(f"{context}: invalid event token")
            seen_events[flow_id].add(event)
            for column in (
                "configured_rate_bps",
                "effective_rate_bps",
                "new_packets_sent",
                "rtx_packets_sent",
                "acked_packets",
            ):
                if raw[column] != "":
                    parse_uint(raw[column], f"{context} {column}")
            if raw["alpha"] != "":
                try:
                    alpha = float(raw["alpha"])
                except ValueError as exc:
                    raise OrchestrationError(
                        f"{context}: invalid alpha {raw['alpha']!r}"
                    ) from exc
                if not math.isfinite(alpha) or not 0.0 <= alpha <= 1.0:
                    raise OrchestrationError(
                        f"{context}: alpha must be finite and in [0, 1]"
                    )
            if raw["paused"] not in {"", "true", "false"}:
                raise OrchestrationError(
                    f"{context}: paused must be blank, true, or false"
                )
            if raw["paused"] == "true" and raw["effective_rate_bps"] != "0":
                raise OrchestrationError(
                    f"{context}: paused sender must have zero effective rate"
                )
            row_count += 1
    if row_count == 0:
        raise OrchestrationError(f"{path}: state trace has no rows")
    for flow_id, events in seen_events.items():
        missing = {"flow-start", "completion"} - events
        if missing:
            raise OrchestrationError(
                f"{path}: flow {flow_id} is missing trace events {sorted(missing)}"
            )
    return row_count


def _run_signature(
    command: Sequence[str],
    tools: ToolPaths,
    scheme: str,
    artifact: WorkloadArtifact,
    topology: Path,
    parameters: ExperimentParameters,
) -> dict[str, object]:
    executable = tools.dcqcn_simulator if scheme == "dcqcn" else tools.rnic_simulator
    signature: dict[str, object] = {
        "schema": SCHEMA_VERSION,
        "scheme": scheme,
        "workload": artifact.name,
        "workload_seed_key": artifact.seed_key,
        "goal_sha256": artifact.goal_sha256,
        "goal_binary_sha256": sha256_file(artifact.binary_path),
        "executable": str(executable),
        "executable_sha256": sha256_file(executable),
        "command": list(command),
        "parameters": asdict(parameters),
    }
    if scheme in PHYSICAL_SCHEMES:
        signature["topology"] = str(topology)
        signature["topology_sha256"] = sha256_file(topology)
    return signature


def run_one(
    repo_root: Path,
    output_root: Path,
    tools: ToolPaths,
    topology: Path,
    artifact: WorkloadArtifact,
    scheme: str,
    seed: int,
    parameters: ExperimentParameters,
    force: bool,
    timeout_seconds: float | None,
) -> CompletionSummary:
    run_directory = output_root / "results" / artifact.name / scheme / f"seed-{seed}"
    completion = run_directory / "flowsInfo.csv"
    partial = run_directory / "flowsInfo.partial.csv"
    trace_requested = (
        artifact.name == "join-exit" and scheme in STATE_TRACE_SCHEMES
    )
    state_trace = run_directory / "stateTrace.csv"
    state_trace_partial = run_directory / "stateTrace.partial.csv"
    stdout_path = run_directory / "stdout.log"
    stderr_path = run_directory / "stderr.log"
    manifest_path = run_directory / "run_manifest.json"
    command = build_simulator_command(
        tools,
        scheme,
        artifact.binary_path,
        partial,
        topology,
        seed,
        parameters,
        state_trace_partial if trace_requested else None,
    )
    signature = _run_signature(
        command, tools, scheme, artifact, topology, parameters
    )

    if completion.exists() and manifest_path.exists() and not force:
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise OrchestrationError(f"invalid run manifest {manifest_path}: {exc}") from exc
        if (
            manifest.get("status") == "complete"
            and manifest.get("signature") == signature
            and manifest.get("completion_sha256") == sha256_file(completion)
            and (
                not trace_requested
                or (
                    state_trace.is_file()
                    and manifest.get("state_trace_sha256")
                    == sha256_file(state_trace)
                )
            )
        ):
            result = read_completion_csv(completion, scheme, artifact.transfers)
            if trace_requested:
                validate_state_trace_csv(state_trace, result)
            print(f"REUSED    {artifact.name}/{scheme}/seed-{seed}")
            return result
        raise OrchestrationError(
            f"run provenance mismatch: {run_directory}; use --force"
        )
    if not force and any(
        managed.exists()
        for managed in (
            completion,
            partial,
            state_trace,
            state_trace_partial,
            manifest_path,
        )
    ):
        raise OrchestrationError(f"partial run state: {run_directory}; use --force")

    run_directory.mkdir(parents=True, exist_ok=True)
    for managed in (
        completion,
        partial,
        state_trace,
        state_trace_partial,
        stdout_path,
        stderr_path,
        manifest_path,
    ):
        if force and managed.exists():
            managed.unlink()
    started_at = utc_now()
    atomic_write_json(
        manifest_path,
        {
            "schema": SCHEMA_VERSION,
            "status": "running",
            "signature": signature,
            "seed": seed,
            "started_at": started_at,
        },
    )
    print(f"RUN       {artifact.name}/{scheme}/seed-{seed}")
    return_code: int | None = None
    duration: float | None = None
    try:
        return_code, duration = run_logged(
            command, stdout_path, stderr_path, repo_root, timeout_seconds
        )
        if return_code != 0:
            raise OrchestrationError(
                f"{scheme} seed {seed} exited {return_code}; see {stderr_path}"
            )
        if not partial.is_file():
            raise OrchestrationError(
                f"{scheme} seed {seed} returned success without {partial}"
            )
        result = read_completion_csv(partial, scheme, artifact.transfers)
        state_trace_rows: int | None = None
        if trace_requested:
            if not state_trace_partial.is_file():
                raise OrchestrationError(
                    f"{scheme} seed {seed} returned success without "
                    f"{state_trace_partial}"
                )
            state_trace_rows = validate_state_trace_csv(
                state_trace_partial, result
            )
        os.replace(partial, completion)
        if trace_requested:
            os.replace(state_trace_partial, state_trace)
    except Exception as exc:
        atomic_write_json(
            manifest_path,
            {
                "schema": SCHEMA_VERSION,
                "status": "failed",
                "signature": signature,
                "seed": seed,
                "started_at": started_at,
                "finished_at": utc_now(),
                "error": str(exc),
                "return_code": return_code,
                "duration_seconds": duration,
                "stdout_log": str(stdout_path),
                "stderr_log": str(stderr_path),
            },
        )
        if isinstance(exc, OrchestrationError):
            raise
        raise OrchestrationError(
            f"unexpected failure in {artifact.name}/{scheme}/seed-{seed}: {exc}"
        ) from exc

    atomic_write_json(
        manifest_path,
        {
            "schema": SCHEMA_VERSION,
            "status": "complete",
            "signature": signature,
            "seed": seed,
            "flow_count": len(result.rows),
            "completion_csv": str(completion),
            "completion_sha256": sha256_file(completion),
            "state_trace_csv": str(state_trace) if trace_requested else None,
            "state_trace_sha256": sha256_file(state_trace)
            if trace_requested
            else None,
            "state_trace_rows": state_trace_rows,
            "stdout_log": str(stdout_path),
            "stderr_log": str(stderr_path),
            "return_code": return_code,
            "started_at": started_at,
            "finished_at": utc_now(),
            "duration_seconds": duration,
        },
    )
    print(f"COMPLETE  {artifact.name}/{scheme}/seed-{seed} flows={len(result.rows)}")
    return result


def validate_paired_results(
    results: Mapping[tuple[str, int, str], CompletionSummary],
    workloads: Sequence[str],
    seeds: Sequence[int],
    schemes: Sequence[str],
) -> None:
    for workload in workloads:
        baseline_by_seed: dict[int, CompletionSummary] = {}
        for seed in seeds:
            selected = [results[(workload, seed, scheme)] for scheme in schemes]
            baseline = selected[0]
            baseline_by_seed[seed] = baseline
            for other in selected[1:]:
                if other.full_contract != baseline.full_contract:
                    raise OrchestrationError(
                        f"{workload} seed {seed}: schemes produced mismatched "
                        "flow-ID/transfer contracts"
                    )
        first_seed = seeds[0]
        first_contract = baseline_by_seed[first_seed].flow_contract
        for seed in seeds[1:]:
            if baseline_by_seed[seed].flow_contract != first_contract:
                raise OrchestrationError(
                    f"{workload}: seed {seed} has a different flow-ID/payload "
                    f"contract from paired seed {first_seed}"
                )


def dry_run_plan(
    output_root: Path,
    tools: ToolPaths,
    topology: Path,
    parameters: ExperimentParameters,
    workloads: Sequence[str],
    seeds: Sequence[int],
    schemes: Sequence[str],
) -> None:
    seen_artifacts: set[tuple[str, str]] = set()
    for workload in workloads:
        for seed in seeds:
            artifact = generate_workload(workload, seed, output_root)
            artifact_key = (artifact.name, artifact.seed_key)
            if artifact_key not in seen_artifacts:
                seen_artifacts.add(artifact_key)
                partial = artifact.binary_path.with_name("workload.bin.partial")
                print(f"DRY-RUN  generate/reuse {artifact.goal_path}")
                print(
                    "DRY-RUN  "
                    + shlex.join(
                        build_converter_command(
                            tools.goal_converter, artifact.goal_path, partial
                        )
                    )
                )
            for scheme in schemes:
                run_directory = (
                    output_root / "results" / workload / scheme / f"seed-{seed}"
                )
                command = build_simulator_command(
                    tools,
                    scheme,
                    artifact.binary_path,
                    run_directory / "flowsInfo.partial.csv",
                    topology,
                    seed,
                    parameters,
                    run_directory / "stateTrace.partial.csv"
                    if workload == "join-exit"
                    and scheme in STATE_TRACE_SCHEMES
                    else None,
                )
                print("DRY-RUN  " + shlex.join(command))


def orchestrate(
    repo_root: Path,
    output_root: Path,
    tools: ToolPaths,
    topology: Path,
    parameters: ExperimentParameters,
    workloads: Sequence[str],
    seeds: Sequence[int],
    schemes: Sequence[str],
    *,
    dry_run: bool = False,
    force: bool = False,
    timeout_seconds: float | None = None,
) -> None:
    parameters.validate()
    if dry_run:
        dry_run_plan(output_root, tools, topology, parameters, workloads, seeds, schemes)
        return
    require_tool(tools.goal_converter, "GOAL converter")
    if any(scheme in RNIC_SCHEMES for scheme in schemes):
        require_tool(tools.rnic_simulator, "RNIC simulator")
    if "dcqcn" in schemes:
        require_tool(tools.dcqcn_simulator, "DCQCN simulator")
    if any(scheme in PHYSICAL_SCHEMES for scheme in schemes) and not topology.is_file():
        raise OrchestrationError(f"physical topology is missing: {topology}")

    materialized: set[tuple[str, str]] = set()
    results: dict[tuple[str, int, str], CompletionSummary] = {}
    for workload in workloads:
        for seed in seeds:
            artifact = generate_workload(workload, seed, output_root)
            artifact_key = (artifact.name, artifact.seed_key)
            if artifact_key not in materialized:
                materialize_workload(
                    artifact, tools, repo_root, force, timeout_seconds
                )
                materialized.add(artifact_key)
            for scheme in schemes:
                results[(workload, seed, scheme)] = run_one(
                    repo_root,
                    output_root,
                    tools,
                    topology,
                    artifact,
                    scheme,
                    seed,
                    parameters,
                    force,
                    timeout_seconds,
                )
    validate_paired_results(results, workloads, seeds, schemes)
    atomic_write_json(
        output_root / "experiment_manifest.json",
        {
            "schema": SCHEMA_VERSION,
            "status": "complete",
            "created_at": utc_now(),
            "repo_root": str(repo_root),
            "output_root": str(output_root),
            "workloads": list(workloads),
            "schemes": list(schemes),
            "seeds": list(seeds),
            "topology": str(topology),
            "topology_sha256": sha256_file(topology)
            if any(scheme in PHYSICAL_SCHEMES for scheme in schemes)
            else None,
            "tools": {name: str(path) for name, path in asdict(tools).items()},
            "parameters": asdict(parameters),
            "run_count": len(results),
        },
    )


def build_parser(repo_root: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=repo_root / "build")
    parser.add_argument("--goal-converter", type=Path)
    parser.add_argument("--rnic-bin", type=Path)
    parser.add_argument("--dcqcn-bin", type=Path)
    parser.add_argument(
        "--topology",
        type=Path,
        default=repo_root / "experiments/rnic_multibaseline/topologies/clos_64_400g.topo",
    )
    parser.add_argument("--seeds", type=parse_seed_selection, default=parse_seed_selection("1-5"))
    parser.add_argument(
        "--schemes",
        type=lambda value: parse_name_selection(value, SCHEMES, "scheme"),
        default=SCHEMES,
    )
    parser.add_argument(
        "--workloads",
        type=lambda value: parse_name_selection(value, WORKLOADS, "workload"),
        default=WORKLOADS,
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--timeout-seconds", type=float, default=0.0)
    parser.add_argument("--ss-routing", choices=("ordered", "unordered"), default="ordered")
    parser.add_argument(
        "--cn-margin-ppm",
        type=positive_int,
        default=ExperimentParameters().cn_margin_ppm,
        help=(
            "rnic-cn receiver grant ceiling in ppm of the access link; "
            "the paper default is 900000"
        ),
    )
    parser.add_argument(
        "--cn-retransmission-rto-ps",
        type=positive_int,
        default=ExperimentParameters().cn_retransmission_rto_ps,
        help=(
            "bounded rnic-cn sender watchdog measured from the physical "
            "end of retry serialization"
        ),
    )
    parser.add_argument(
        "--dcqcn-egress-buffer-bytes",
        type=positive_int,
        default=ExperimentParameters().dcqcn_egress_buffer_bytes,
        help=(
            "ns-tm3 queued-VoQ admission cap for each physical egress; "
            "the switch-wide shared pool remains physical_buffer_bytes"
        ),
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parents[2]
    parser = build_parser(repo_root)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds < 0:
        parser.error("--timeout-seconds must be finite and nonnegative")
    build_dir = args.build_dir.resolve()
    tools = ToolPaths(
        goal_converter=(args.goal_converter or build_dir / "htsim_goal_txt2bin").resolve(),
        rnic_simulator=(args.rnic_bin or build_dir / "datacenter/htsim_rnic").resolve(),
        dcqcn_simulator=(args.dcqcn_bin or build_dir / "datacenter/htsim_dcqcn_atlahs").resolve(),
    )
    parameters = ExperimentParameters(
        ss_routing=args.ss_routing,
        cn_margin_ppm=args.cn_margin_ppm,
        cn_retransmission_rto_ps=args.cn_retransmission_rto_ps,
        dcqcn_egress_buffer_bytes=args.dcqcn_egress_buffer_bytes,
    )
    try:
        orchestrate(
            repo_root=repo_root,
            output_root=args.output_root.resolve(),
            tools=tools,
            topology=args.topology.resolve(),
            parameters=parameters,
            workloads=args.workloads,
            seeds=args.seeds,
            schemes=args.schemes,
            dry_run=args.dry_run,
            force=args.force,
            timeout_seconds=args.timeout_seconds or None,
        )
    except OrchestrationError as exc:
        print(f"run_multibaseline: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
