#!/usr/bin/env python3
"""Run the absolute checks declared by an HTSIM validation plan."""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

COMPLETION_RE = re.compile(
    r"^Flow\s+(?P<name>\S+).*?\bfinished at\s+"
    r"(?P<fct>[0-9]+(?:\.[0-9]+)?)\b"
)
MESSAGE_COUNT_RE = re.compile(r"\btotal messages\s+(?P<count>[0-9]+)\b")


@dataclass
class Experiment:
    matrix: Path
    name: str = ""
    binary: str = "./htsim_eqds"
    parameters: list[str] = field(default_factory=list)
    target_tail_fct: float = 0.0
    target_fcts: dict[str, float] = field(default_factory=dict)

    def command(self) -> list[str]:
        command = shlex.split(self.binary)
        if not command:
            raise ValueError(f"{self.matrix}: empty binary command")
        command.extend(("-tm", str(self.matrix)))
        for parameter in self.parameters:
            command.extend(shlex.split(parameter))
        return command


def parse_plan(path: Path) -> list[Experiment]:
    lines = path.read_text().splitlines()
    experiments = []
    index = 0
    while index < len(lines):
        matrix_text = lines[index].strip()
        index += 1
        if not matrix_text or matrix_text.startswith("#"):
            continue
        if matrix_text.startswith("!"):
            raise ValueError(
                f"{path}:{index}: found parameters without a traffic matrix"
            )
        experiment = Experiment(matrix=Path(matrix_text))
        while index < len(lines) and lines[index].startswith("!"):
            directive = lines[index][1:].strip()
            line_number = index + 1
            index += 1
            key, separator, value = directive.partition(" ")
            value = value.strip()
            if not separator or not value:
                raise ValueError(f"{path}:{line_number}: malformed directive")
            if key == "Binary":
                experiment.binary = value
            elif key == "Param":
                experiment.parameters.append(value)
            elif key == "tailFCT":
                experiment.target_tail_fct = float(value)
            elif key == "FCT":
                fields = value.split()
                if len(fields) != 2:
                    raise ValueError(f"{path}:{line_number}: malformed FCT target")
                experiment.target_fcts[fields[0]] = float(fields[1])
            elif key == "Experiment":
                experiment.name = value
            else:
                raise ValueError(f"{path}:{line_number}: unknown directive {key!r}")
        experiments.append(experiment)
    if not experiments:
        raise ValueError(f"{path}: validation plan contains no experiments")
    return experiments


def connection_count(matrix: Path) -> int:
    counts = []
    for line in matrix.read_text().splitlines():
        fields = line.split()
        if fields and fields[0] == "Connections":
            if len(fields) != 2:
                raise ValueError(f"{matrix}: malformed Connections row")
            counts.append(int(fields[1]))
    if len(counts) != 1 or counts[0] < 0:
        raise ValueError(f"{matrix}: expected one nonnegative Connections row")
    return counts[0]


def run_experiment(experiment: Experiment, *, debug: bool, dry_run: bool) -> bool:
    if not experiment.matrix.is_file():
        print(f"[FAIL] Cannot find traffic matrix {experiment.matrix}")
        return False
    try:
        expected_connections = connection_count(experiment.matrix)
        command = experiment.command()
    except (OSError, ValueError) as error:
        print(f"[FAIL] Invalid experiment {experiment.name!r}: {error}")
        return False

    print(f"\nExperiment: {experiment.name}")
    print("==========================================")
    print("Running", shlex.join(command))
    if debug or dry_run:
        print("Expected connections:", expected_connections)
        print("Target tail FCT:", experiment.target_tail_fct)
        print("Target per-flow FCT:", experiment.target_fcts)
    if dry_run:
        return True

    try:
        process = subprocess.run(command, check=False, capture_output=True, text=True)
    except OSError as error:
        print(f"[FAIL] Could not start simulator command: {error}")
        return False
    if process.returncode != 0:
        print(f"[FAIL] Simulator command exited with status {process.returncode}")
        if process.stderr:
            print(process.stderr.rstrip())
        return False

    completed_connections = 0
    fcts = []
    summary = None
    passed = True
    for line in process.stdout.splitlines():
        completion = COMPLETION_RE.search(line)
        if completion is not None:
            fct = float(completion.group("fct"))
            fcts.append(fct)
            message_count = MESSAGE_COUNT_RE.search(line)
            completed_connections += (
                int(message_count.group("count")) if message_count is not None else 1
            )
            flow_name = completion.group("name")
            if flow_name in experiment.target_fcts:
                target = experiment.target_fcts[flow_name]
                if fct <= target:
                    print(
                        f"[PASS] FCT {fct} us for flow {flow_name} "
                        f"is at or below the target of {target} us"
                    )
                else:
                    print(
                        f"[FAIL] FCT {fct} us for flow {flow_name} "
                        f"is above the target of {target} us"
                    )
                    passed = False
        if "New:" in line and "Rtx:" in line:
            summary = line

    if not fcts:
        print(
            "[FAIL] No completed flows: zero flows completed while "
            f"{expected_connections} were expected"
        )
        passed = False
    else:
        tail = max(fcts)
        minimum = min(fcts)
        if experiment.target_tail_fct > 0:
            if tail <= experiment.target_tail_fct:
                print(
                    f"[PASS] Tail FCT {tail} us is at or below the target of "
                    f"{experiment.target_tail_fct} us"
                )
            else:
                print(
                    f"[FAIL] Tail FCT {tail} us is above the target of "
                    f"{experiment.target_tail_fct} us"
                )
                passed = False
        if minimum <= 0:
            print("[FAIL] FCT spread has a nonpositive minimum")
            passed = False
        else:
            print(f"FCT Spread {minimum} -> {tail} ratio {tail / minimum}")

    if completed_connections == expected_connections:
        print(f"[PASS] Connection count {completed_connections}")
    else:
        print(
            f"[FAIL] Total connections in connection matrix was "
            f"{expected_connections} but only {completed_connections} finished"
        )
        passed = False

    if summary is None:
        print("[FAIL] Simulator did not emit a New/Rtx packet summary")
        passed = False
    else:
        print("Summary:", summary)
    return passed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("plan", nargs="?", type=Path, default=Path("validate.txt"))
    parser.add_argument("-debug", action="store_true")
    parser.add_argument("-dryrun", action="store_true")
    args = parser.parse_args()
    print(f"Using {args.plan} as experiment plan")
    try:
        experiments = parse_plan(args.plan)
    except (OSError, ValueError) as error:
        print(f"[FAIL] Cannot read validation plan: {error}")
        return 1
    failed = [
        experiment
        for experiment in experiments
        if not run_experiment(experiment, debug=args.debug, dry_run=args.dryrun)
    ]
    if failed:
        print(f"\n[FAIL] {len(failed)} of {len(experiments)} experiments failed in {args.plan}")
        for experiment in failed:
            print(f"[FAIL]   {experiment.name or experiment.matrix}")
        return 1
    print(f"\n[PASS] {len(experiments)} experiments passed in {args.plan}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
