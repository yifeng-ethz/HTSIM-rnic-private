#!/usr/bin/env python3
"""Validate and summarize paired multi-baseline completion experiments.

The ``cdf`` command consumes this directory layout::

    INPUT_ROOT/
      PROFILE/
        SEED/
          flowsInfo.csv

Every profile must contain the same seed names.  Every run must contain exactly
the same flow identifiers and payload bytes.  Empirical CDFs are evaluated on
the union of all observed FCTs, plus zero, so every profile and seed is sampled
on one exact common grid.

The ``scan`` command consumes an explicit CSV manifest.  It produces tidy
per-run and seed-aggregated tables for Q_hi/Q_lo, telemetry-delay,
packet-credit-quantum, and buffer sensitivity plots.  It never supplies
defaults for the scanned parameters: the manifest is the experiment record.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


SCHEMA_VERSION = "rnic-multibaseline-fct-analysis-v2"

FLOW_ID_ALIASES = (
    "flow_id",
    "flowId",
    "flowid",
    "srcNode_dstNode_flowId",
)
PAYLOAD_ALIASES = (
    "payload_bytes",
    "flow_size_bytes",
    "flowSizeBytes",
    "flow_size",
)
START_PS_ALIASES = ("start_time_ps", "startTimePs", "flowStartTimePs")
END_PS_ALIASES = (
    "completion_time_ps",
    "end_time_ps",
    "endTimePs",
    "completionTimePs",
)
FCT_PS_ALIASES = ("fct_ps", "fctPs", "flow_completion_time_ps")
START_NS_ALIASES = ("start_time_ns", "startTimeNs", "flowStartTimeNs")
END_NS_ALIASES = (
    "end_time_ns",
    "endTimeNs",
    "completion_time_ns",
    "completionTimeNs",
)
FCT_NS_ALIASES = ("fct_ns", "fctNs", "flow_completion_time_ns")

QUANTILES = (
    ("p50_fct_ps", 0.50),
    ("p95_fct_ps", 0.95),
    ("p99_fct_ps", 0.99),
    ("p99_9_fct_ps", 0.999),
)
SUMMARY_METRICS = tuple(name for name, _ in QUANTILES) + ("jct_ps",)

SCAN_PARAMETER_COLUMNS = (
    "q_hi_bytes",
    "q_lo_bytes",
    "telemetry_delay_ns",
    "credit_quantum_packets",
    "buffer_bytes",
)
SCAN_MANIFEST_COLUMNS = (
    "profile",
    "seed",
    "completion_csv",
    *SCAN_PARAMETER_COLUMNS,
)


class AnalysisError(ValueError):
    """Raised when inputs violate the paired-experiment contract."""


@dataclass(frozen=True)
class ColumnOverrides:
    flow_id: str | None = None
    payload: str | None = None
    start: str | None = None
    end: str | None = None
    fct: str | None = None
    input_time_unit: str | None = None


@dataclass(frozen=True)
class FlowRecord:
    flow_id: str
    payload_bytes: int
    start_time_ps: int
    end_time_ps: int
    fct_ps: int


@dataclass(frozen=True)
class Run:
    profile: str
    seed: str
    completion_csv: Path
    flows: tuple[FlowRecord, ...]


@dataclass(frozen=True)
class ScanParameters:
    q_hi_bytes: int
    q_lo_bytes: int
    telemetry_delay_ns: int
    credit_quantum_packets: int
    buffer_bytes: int

    @property
    def hysteresis_gap_bytes(self) -> int:
        return self.q_hi_bytes - self.q_lo_bytes

    @property
    def q_hi_fraction_buffer(self) -> float:
        return self.q_hi_bytes / self.buffer_bytes

    @property
    def q_lo_fraction_buffer(self) -> float:
        return self.q_lo_bytes / self.buffer_bytes

    def values(self) -> tuple[int, int, int, int, int]:
        return (
            self.q_hi_bytes,
            self.q_lo_bytes,
            self.telemetry_delay_ns,
            self.credit_quantum_packets,
            self.buffer_bytes,
        )


@dataclass(frozen=True)
class ScanRun:
    run: Run
    parameters: ScanParameters


def _parse_nonnegative_int(value: str, field: str, context: str) -> int:
    text = value.strip()
    try:
        parsed = int(text, 10)
    except ValueError as exc:
        raise AnalysisError(f"{context}: {field} must be an integer, got {value!r}") from exc
    if parsed < 0:
        raise AnalysisError(f"{context}: {field} must be nonnegative, got {parsed}")
    return parsed


def _select_column(
    headers: Sequence[str],
    override: str | None,
    aliases: Sequence[str],
    logical_name: str,
) -> str | None:
    if override:
        if override not in headers:
            raise AnalysisError(
                f"requested {logical_name} column {override!r} is absent; "
                f"available columns: {', '.join(headers)}"
            )
        return override
    for candidate in aliases:
        if candidate in headers:
            return candidate
    return None


def _select_time_column(
    headers: Sequence[str],
    override: str | None,
    ps_aliases: Sequence[str],
    ns_aliases: Sequence[str],
    logical_name: str,
    override_unit: str | None,
) -> tuple[str | None, str | None]:
    """Select a timestamp column and return its declared physical unit."""

    if override:
        if override not in headers:
            raise AnalysisError(
                f"requested {logical_name} column {override!r} is absent; "
                f"available columns: {', '.join(headers)}"
            )
        inferred_unit: str | None = None
        if override in ps_aliases or override.lower().endswith("_ps"):
            inferred_unit = "ps"
        elif override in ns_aliases or override.lower().endswith("_ns"):
            inferred_unit = "ns"
        if override_unit and inferred_unit and override_unit != inferred_unit:
            raise AnalysisError(
                f"requested {logical_name} column {override!r} declares {inferred_unit}, "
                f"which conflicts with --input-time-unit {override_unit}"
            )
        unit = inferred_unit or override_unit
        if unit is None:
            raise AnalysisError(
                f"cannot infer the unit of overridden {logical_name} column {override!r}; "
                "supply --input-time-unit ps or ns"
            )
        return override, unit

    matches = [
        *((column, "ps") for column in ps_aliases if column in headers),
        *((column, "ns") for column in ns_aliases if column in headers),
    ]
    if len(matches) > 1:
        raise AnalysisError(
            f"ambiguous {logical_name} columns {', '.join(column for column, _ in matches)}; "
            f"select one with --{logical_name}-column"
        )
    return matches[0] if matches else (None, None)


def _time_as_ps(value: str, field: str, unit: str, context: str) -> int:
    parsed = _parse_nonnegative_int(value, field, context)
    if unit == "ps":
        return parsed
    if unit == "ns":
        return parsed * 1000
    raise AnalysisError(f"{context}: unsupported time unit {unit!r}")


def read_completion_csv(
    path: Path, overrides: ColumnOverrides | None = None
) -> tuple[FlowRecord, ...]:
    """Read one completion CSV and losslessly normalize time fields to integer ps."""

    overrides = overrides or ColumnOverrides()
    try:
        handle = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise AnalysisError(f"cannot read completion CSV {path}: {exc}") from exc

    with handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise AnalysisError(f"{path}: missing CSV header")
        headers = [header.strip() for header in reader.fieldnames]
        if len(headers) != len(set(headers)):
            raise AnalysisError(f"{path}: duplicate CSV column names")
        if headers != reader.fieldnames:
            reader.fieldnames = headers

        flow_column = _select_column(
            headers, overrides.flow_id, FLOW_ID_ALIASES, "flow identifier"
        )
        payload_column = _select_column(
            headers, overrides.payload, PAYLOAD_ALIASES, "payload"
        )
        start_column, start_unit = _select_time_column(
            headers,
            overrides.start,
            START_PS_ALIASES,
            START_NS_ALIASES,
            "start",
            overrides.input_time_unit,
        )
        end_column, end_unit = _select_time_column(
            headers,
            overrides.end,
            END_PS_ALIASES,
            END_NS_ALIASES,
            "end",
            overrides.input_time_unit,
        )
        fct_column, fct_unit = _select_time_column(
            headers,
            overrides.fct,
            FCT_PS_ALIASES,
            FCT_NS_ALIASES,
            "fct",
            overrides.input_time_unit,
        )

        if flow_column is None:
            raise AnalysisError(
                f"{path}: no flow identifier column; expected one of "
                f"{', '.join(FLOW_ID_ALIASES)}"
            )
        if payload_column is None:
            raise AnalysisError(
                f"{path}: no payload column; expected one of {', '.join(PAYLOAD_ALIASES)}"
            )
        if fct_column is None and (start_column is None or end_column is None):
            raise AnalysisError(
                f"{path}: need an FCT column or both start and end timestamp columns"
            )

        flows: list[FlowRecord] = []
        seen: set[str] = set()
        for row_number, row in enumerate(reader, start=2):
            context = f"{path}:{row_number}"
            flow_id = (row.get(flow_column) or "").strip()
            if not flow_id:
                raise AnalysisError(f"{context}: empty flow identifier")
            if flow_id in seen:
                raise AnalysisError(f"{context}: duplicate flow identifier {flow_id!r}")
            seen.add(flow_id)

            payload = _parse_nonnegative_int(
                row.get(payload_column) or "", payload_column, context
            )
            start = (
                _time_as_ps(
                    row.get(start_column) or "", start_column, start_unit, context
                )
                if start_column and start_unit
                else None
            )
            end = (
                _time_as_ps(row.get(end_column) or "", end_column, end_unit, context)
                if end_column and end_unit
                else None
            )
            fct = (
                _time_as_ps(row.get(fct_column) or "", fct_column, fct_unit, context)
                if fct_column and fct_unit
                else None
            )

            if fct is None:
                assert start is not None and end is not None
                if end < start:
                    raise AnalysisError(f"{context}: end timestamp precedes start timestamp")
                fct = end - start
            elif start is not None and end is not None:
                if end < start:
                    raise AnalysisError(f"{context}: end timestamp precedes start timestamp")
                if end - start != fct:
                    raise AnalysisError(
                        f"{context}: FCT {fct} ps disagrees with end-start "
                        f"({end - start} ps)"
                    )
            elif start is not None:
                end = start + fct
            elif end is not None:
                if end < fct:
                    raise AnalysisError(
                        f"{context}: completion timestamp {end} ps is smaller than FCT {fct} ps"
                    )
                start = end - fct
            else:
                start = 0
                end = fct

            assert start is not None and end is not None
            flows.append(FlowRecord(flow_id, payload, start, end, fct))

    if not flows:
        raise AnalysisError(f"{path}: completion CSV contains no flow rows")
    return tuple(sorted(flows, key=lambda flow: flow.flow_id))


def discover_runs(
    input_root: Path,
    completion_name: str,
    overrides: ColumnOverrides | None = None,
) -> list[Run]:
    if not input_root.is_dir():
        raise AnalysisError(f"input root is not a directory: {input_root}")

    runs: list[Run] = []
    profile_dirs = sorted(
        path for path in input_root.iterdir() if path.is_dir() and not path.name.startswith(".")
    )
    if not profile_dirs:
        raise AnalysisError(f"{input_root}: no profile directories found")

    for profile_dir in profile_dirs:
        seed_dirs = sorted(
            path
            for path in profile_dir.iterdir()
            if path.is_dir() and not path.name.startswith(".")
        )
        if not seed_dirs:
            raise AnalysisError(f"{profile_dir}: no seed directories found")
        for seed_dir in seed_dirs:
            completion_csv = seed_dir / completion_name
            if not completion_csv.is_file():
                raise AnalysisError(
                    f"{seed_dir}: expected completion file {completion_name!r}"
                )
            runs.append(
                Run(
                    profile=profile_dir.name,
                    seed=seed_dir.name,
                    completion_csv=completion_csv,
                    flows=read_completion_csv(completion_csv, overrides),
                )
            )
    validate_paired_runs(runs)
    return runs


def _flow_payload_map(run: Run) -> dict[str, int]:
    return {flow.flow_id: flow.payload_bytes for flow in run.flows}


def _describe_flow_mismatch(reference: Run, candidate: Run) -> str:
    expected = _flow_payload_map(reference)
    actual = _flow_payload_map(candidate)
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    changed = sorted(
        flow_id
        for flow_id in set(expected) & set(actual)
        if expected[flow_id] != actual[flow_id]
    )
    details: list[str] = []
    if missing:
        details.append(f"missing flow IDs {missing[:5]!r}")
    if extra:
        details.append(f"extra flow IDs {extra[:5]!r}")
    if changed:
        flow_id = changed[0]
        details.append(
            f"payload for {flow_id!r} is {actual[flow_id]}, expected {expected[flow_id]}"
        )
    return "; ".join(details) or "flow contract differs"


def validate_flow_contract(runs: Sequence[Run]) -> None:
    if not runs:
        raise AnalysisError("no runs supplied")
    reference = runs[0]
    expected = _flow_payload_map(reference)
    for candidate in runs[1:]:
        if _flow_payload_map(candidate) != expected:
            raise AnalysisError(
                f"flow/payload contract mismatch between "
                f"{reference.profile}/{reference.seed} and "
                f"{candidate.profile}/{candidate.seed}: "
                f"{_describe_flow_mismatch(reference, candidate)}"
            )


def validate_paired_runs(runs: Sequence[Run]) -> None:
    if not runs:
        raise AnalysisError("no runs supplied")
    seeds_by_profile: dict[str, set[str]] = {}
    for run in runs:
        profile_seeds = seeds_by_profile.setdefault(run.profile, set())
        if run.seed in profile_seeds:
            raise AnalysisError(f"duplicate run for {run.profile}/{run.seed}")
        profile_seeds.add(run.seed)

    reference_profile = sorted(seeds_by_profile)[0]
    reference_seeds = seeds_by_profile[reference_profile]
    for profile, seeds in sorted(seeds_by_profile.items()):
        if seeds != reference_seeds:
            missing = sorted(reference_seeds - seeds)
            extra = sorted(seeds - reference_seeds)
            raise AnalysisError(
                f"unpaired seeds for profile {profile!r} relative to "
                f"{reference_profile!r}: missing={missing!r}, extra={extra!r}"
            )
    validate_flow_contract(runs)


def empirical_quantile(sorted_values: Sequence[int], probability: float) -> int:
    """Return inverse-ECDF (nearest-rank) quantile."""

    if not sorted_values:
        raise AnalysisError("cannot compute a quantile of an empty sample")
    if not 0.0 <= probability <= 1.0:
        raise AnalysisError(f"invalid quantile probability {probability}")
    index = max(0, math.ceil(probability * len(sorted_values)) - 1)
    return sorted_values[index]


def run_statistics(run: Run) -> dict[str, int]:
    fcts = sorted(flow.fct_ps for flow in run.flows)
    result = {
        "flow_count": len(run.flows),
        "payload_bytes_total": sum(flow.payload_bytes for flow in run.flows),
    }
    for name, probability in QUANTILES:
        result[name] = empirical_quantile(fcts, probability)
    # JCT is the latest absolute completion timestamp in the simulation epoch.
    result["jct_ps"] = max(flow.end_time_ps for flow in run.flows)
    return result


def _population_mean_sigma(values: Sequence[float | int]) -> tuple[float, float]:
    if not values:
        raise AnalysisError("cannot summarize an empty sample")
    float_values = [float(value) for value in values]
    return statistics.fmean(float_values), statistics.pstdev(float_values)


def _format_float(value: float) -> str:
    return format(value, ".12g")


def _write_csv(path: Path, fieldnames: Sequence[str], rows: Iterable[Mapping[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="raise")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_cdf_analysis(runs: Sequence[Run], output_dir: Path) -> None:
    validate_paired_runs(runs)
    ordered_runs = sorted(runs, key=lambda run: (run.profile, run.seed))
    grid = sorted({0, *(flow.fct_ps for run in ordered_runs for flow in run.flows)})

    run_rows: list[dict[str, object]] = []
    for run in ordered_runs:
        stats = run_statistics(run)
        run_rows.append(
            {
                "profile": run.profile,
                "seed": run.seed,
                "completion_csv": str(run.completion_csv.resolve()),
                **stats,
            }
        )
    run_fields = (
        "profile",
        "seed",
        "completion_csv",
        "flow_count",
        "payload_bytes_total",
        *SUMMARY_METRICS,
    )
    _write_csv(output_dir / "fct_run_summary.csv", run_fields, run_rows)

    profile_rows: list[dict[str, object]] = []
    for profile in sorted({run.profile for run in ordered_runs}):
        rows = [row for row in run_rows if row["profile"] == profile]
        profile_row: dict[str, object] = {
            "profile": profile,
            "seed_count": len(rows),
            "flow_count_per_seed": rows[0]["flow_count"],
            "payload_bytes_per_seed": rows[0]["payload_bytes_total"],
        }
        for metric in SUMMARY_METRICS:
            mean, sigma = _population_mean_sigma([int(row[metric]) for row in rows])
            profile_row[f"{metric}_mean"] = _format_float(mean)
            profile_row[f"{metric}_sigma"] = _format_float(sigma)
            profile_row[f"{metric}_low"] = _format_float(max(0.0, mean - sigma))
            profile_row[f"{metric}_high"] = _format_float(mean + sigma)
        profile_rows.append(profile_row)
    profile_fields = [
        "profile",
        "seed_count",
        "flow_count_per_seed",
        "payload_bytes_per_seed",
    ]
    for metric in SUMMARY_METRICS:
        profile_fields.extend(
            (f"{metric}_mean", f"{metric}_sigma", f"{metric}_low", f"{metric}_high")
        )
    _write_csv(output_dir / "fct_profile_summary.csv", profile_fields, profile_rows)

    sorted_fcts_by_run = {
        (run.profile, run.seed): sorted(flow.fct_ps for flow in run.flows)
        for run in ordered_runs
    }

    def cdf_by_seed_rows() -> Iterable[Mapping[str, object]]:
        # Stream this potentially large audit table instead of materializing
        # O(profile * seed * grid) dictionaries in memory.
        for run in ordered_runs:
            sorted_fcts = sorted_fcts_by_run[(run.profile, run.seed)]
            denominator = len(sorted_fcts)
            for fct_ps in grid:
                cdf = bisect.bisect_right(sorted_fcts, fct_ps) / denominator
                yield {
                    "profile": run.profile,
                    "seed": run.seed,
                    "grid_fct_ps": fct_ps,
                    "cdf": _format_float(cdf),
                }

    _write_csv(
        output_dir / "fct_cdf_by_seed.csv",
        ("profile", "seed", "grid_fct_ps", "cdf"),
        cdf_by_seed_rows(),
    )

    flow_count = len(ordered_runs[0].flows)
    runs_by_profile = {
        profile: [run for run in ordered_runs if run.profile == profile]
        for profile in sorted({run.profile for run in ordered_runs})
    }

    def cdf_summary_rows() -> Iterable[Mapping[str, object]]:
        for profile, profile_runs in runs_by_profile.items():
            for fct_ps in grid:
                values = [
                    bisect.bisect_right(
                        sorted_fcts_by_run[(run.profile, run.seed)], fct_ps
                    )
                    / flow_count
                    for run in profile_runs
                ]
                mean, sigma = _population_mean_sigma(values)
                yield {
                    "profile": profile,
                    "grid_fct_ps": fct_ps,
                    "cdf_mean": _format_float(mean),
                    "cdf_sigma": _format_float(sigma),
                    "cdf_low": _format_float(max(0.0, mean - sigma)),
                    "cdf_high": _format_float(min(1.0, mean + sigma)),
                    "seed_count": len(values),
                    "flow_count_per_seed": flow_count,
                }

    _write_csv(
        output_dir / "fct_cdf_summary.csv",
        (
            "profile",
            "grid_fct_ps",
            "cdf_mean",
            "cdf_sigma",
            "cdf_low",
            "cdf_high",
            "seed_count",
            "flow_count_per_seed",
        ),
        cdf_summary_rows(),
    )

    manifest = {
        "schema": SCHEMA_VERSION,
        "analysis": "paired-seed empirical FCT CDF",
        "canonical_time_unit": "picoseconds",
        "cdf_grid": "zero plus sorted union of all observed fct_ps values",
        "cdf_band": "pointwise mean +/- one population standard deviation, clipped to [0,1]",
        "quantile": "inverse empirical CDF (nearest-rank)",
        "jct_ps": "maximum absolute completion_time_ps within a run",
        "profiles": sorted({run.profile for run in ordered_runs}),
        "seeds": sorted({run.seed for run in ordered_runs}),
        "inputs": [
            {
                "profile": run.profile,
                "seed": run.seed,
                "path": str(run.completion_csv.resolve()),
                "sha256": _sha256(run.completion_csv),
            }
            for run in ordered_runs
        ],
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "fct_analysis_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def _validate_scan_parameters(parameters: ScanParameters, context: str) -> None:
    if parameters.q_hi_bytes <= parameters.q_lo_bytes:
        raise AnalysisError(
            f"{context}: q_hi_bytes must be greater than q_lo_bytes "
            f"({parameters.q_hi_bytes} <= {parameters.q_lo_bytes})"
        )
    if parameters.credit_quantum_packets <= 0:
        raise AnalysisError(f"{context}: credit_quantum_packets must be positive")
    if parameters.buffer_bytes <= 0:
        raise AnalysisError(f"{context}: buffer_bytes must be positive")
    if parameters.q_hi_bytes > parameters.buffer_bytes:
        raise AnalysisError(
            f"{context}: q_hi_bytes cannot exceed buffer_bytes "
            f"({parameters.q_hi_bytes} > {parameters.buffer_bytes})"
        )


def validate_scan_runs(scan_runs: Sequence[ScanRun]) -> None:
    if not scan_runs:
        raise AnalysisError("no parameter-scan runs supplied")
    validate_flow_contract([scan_run.run for scan_run in scan_runs])

    seen: set[tuple[str, str, tuple[int, int, int, int, int]]] = set()
    seeds_by_config: dict[tuple[str, tuple[int, int, int, int, int]], set[str]] = {}
    for scan_run in scan_runs:
        context = f"{scan_run.run.profile}/{scan_run.run.seed}"
        _validate_scan_parameters(scan_run.parameters, context)
        run_key = (scan_run.run.profile, scan_run.run.seed, scan_run.parameters.values())
        if run_key in seen:
            raise AnalysisError(f"duplicate parameter-scan run {run_key!r}")
        seen.add(run_key)
        config_key = (scan_run.run.profile, scan_run.parameters.values())
        seeds_by_config.setdefault(config_key, set()).add(scan_run.run.seed)

    reference_key = sorted(seeds_by_config)[0]
    reference_seeds = seeds_by_config[reference_key]
    for key, seeds in sorted(seeds_by_config.items()):
        if seeds != reference_seeds:
            raise AnalysisError(
                f"unpaired parameter-scan seeds for {key!r} relative to "
                f"{reference_key!r}: expected {sorted(reference_seeds)!r}, "
                f"got {sorted(seeds)!r}"
            )


def read_scan_manifest(
    manifest_path: Path, overrides: ColumnOverrides | None = None
) -> list[ScanRun]:
    try:
        handle = manifest_path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise AnalysisError(f"cannot read scan manifest {manifest_path}: {exc}") from exc

    scan_runs: list[ScanRun] = []
    seen: set[tuple[str, str, tuple[int, int, int, int, int]]] = set()
    with handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise AnalysisError(f"{manifest_path}: missing CSV header")
        missing = [column for column in SCAN_MANIFEST_COLUMNS if column not in reader.fieldnames]
        if missing:
            raise AnalysisError(
                f"{manifest_path}: missing required columns {', '.join(missing)}"
            )
        for row_number, row in enumerate(reader, start=2):
            context = f"{manifest_path}:{row_number}"
            profile = (row.get("profile") or "").strip()
            seed = (row.get("seed") or "").strip()
            completion_text = (row.get("completion_csv") or "").strip()
            if not profile or not seed or not completion_text:
                raise AnalysisError(
                    f"{context}: profile, seed, and completion_csv must be nonempty"
                )
            completion_csv = Path(completion_text)
            if not completion_csv.is_absolute():
                completion_csv = manifest_path.parent / completion_csv
            parameters = ScanParameters(
                **{
                    column: _parse_nonnegative_int(row.get(column) or "", column, context)
                    for column in SCAN_PARAMETER_COLUMNS
                }
            )
            _validate_scan_parameters(parameters, context)
            key = (profile, seed, parameters.values())
            if key in seen:
                raise AnalysisError(
                    f"{context}: duplicate profile/seed/parameter combination {key!r}"
                )
            seen.add(key)
            scan_runs.append(
                ScanRun(
                    Run(
                        profile=profile,
                        seed=seed,
                        completion_csv=completion_csv,
                        flows=read_completion_csv(completion_csv, overrides),
                    ),
                    parameters,
                )
            )
    if not scan_runs:
        raise AnalysisError(f"{manifest_path}: scan manifest contains no rows")

    validate_scan_runs(scan_runs)
    return scan_runs


def _scan_parameter_row(parameters: ScanParameters) -> dict[str, object]:
    return {
        "q_hi_bytes": parameters.q_hi_bytes,
        "q_lo_bytes": parameters.q_lo_bytes,
        "hysteresis_gap_bytes": parameters.hysteresis_gap_bytes,
        "q_hi_fraction_buffer": _format_float(parameters.q_hi_fraction_buffer),
        "q_lo_fraction_buffer": _format_float(parameters.q_lo_fraction_buffer),
        "telemetry_delay_ns": parameters.telemetry_delay_ns,
        "credit_quantum_packets": parameters.credit_quantum_packets,
        "buffer_bytes": parameters.buffer_bytes,
    }


SCAN_OUTPUT_PARAMETER_FIELDS = (
    "q_hi_bytes",
    "q_lo_bytes",
    "hysteresis_gap_bytes",
    "q_hi_fraction_buffer",
    "q_lo_fraction_buffer",
    "telemetry_delay_ns",
    "credit_quantum_packets",
    "buffer_bytes",
)


def write_parameter_scan(scan_runs: Sequence[ScanRun], output_dir: Path) -> None:
    validate_scan_runs(scan_runs)
    ordered = sorted(
        scan_runs,
        key=lambda item: (item.run.profile, item.parameters.values(), item.run.seed),
    )

    run_rows: list[dict[str, object]] = []
    for scan_run in ordered:
        run_rows.append(
            {
                "profile": scan_run.run.profile,
                "seed": scan_run.run.seed,
                "completion_csv": str(scan_run.run.completion_csv.resolve()),
                **_scan_parameter_row(scan_run.parameters),
                **run_statistics(scan_run.run),
            }
        )
    _write_csv(
        output_dir / "parameter_scan_runs.csv",
        (
            "profile",
            "seed",
            "completion_csv",
            *SCAN_OUTPUT_PARAMETER_FIELDS,
            "flow_count",
            "payload_bytes_total",
            *SUMMARY_METRICS,
        ),
        run_rows,
    )

    grouped: dict[tuple[str, tuple[int, int, int, int, int]], list[dict[str, object]]] = {}
    parameter_objects: dict[
        tuple[str, tuple[int, int, int, int, int]], ScanParameters
    ] = {}
    for row, scan_run in zip(run_rows, ordered):
        key = (scan_run.run.profile, scan_run.parameters.values())
        grouped.setdefault(key, []).append(row)
        parameter_objects[key] = scan_run.parameters

    summary_rows: list[dict[str, object]] = []
    plot_rows: list[dict[str, object]] = []
    for key in sorted(grouped):
        profile, _ = key
        rows = grouped[key]
        parameters = parameter_objects[key]
        common = {
            "profile": profile,
            **_scan_parameter_row(parameters),
            "seed_count": len(rows),
            "flow_count_per_seed": rows[0]["flow_count"],
            "payload_bytes_per_seed": rows[0]["payload_bytes_total"],
        }
        summary: dict[str, object] = dict(common)
        for metric in SUMMARY_METRICS:
            mean, sigma = _population_mean_sigma([int(row[metric]) for row in rows])
            low = max(0.0, mean - sigma)
            high = mean + sigma
            summary[f"{metric}_mean"] = _format_float(mean)
            summary[f"{metric}_sigma"] = _format_float(sigma)
            summary[f"{metric}_low"] = _format_float(low)
            summary[f"{metric}_high"] = _format_float(high)
            plot_rows.append(
                {
                    "profile": profile,
                    **_scan_parameter_row(parameters),
                    "seed_count": len(rows),
                    "metric": metric.removesuffix("_ps"),
                    "mean_ps": _format_float(mean),
                    "sigma_ps": _format_float(sigma),
                    "low_ps": _format_float(low),
                    "high_ps": _format_float(high),
                }
            )
        summary_rows.append(summary)

    summary_fields = [
        "profile",
        *SCAN_OUTPUT_PARAMETER_FIELDS,
        "seed_count",
        "flow_count_per_seed",
        "payload_bytes_per_seed",
    ]
    for metric in SUMMARY_METRICS:
        summary_fields.extend(
            (f"{metric}_mean", f"{metric}_sigma", f"{metric}_low", f"{metric}_high")
        )
    _write_csv(output_dir / "parameter_scan_summary.csv", summary_fields, summary_rows)
    _write_csv(
        output_dir / "parameter_scan_plot.csv",
        (
            "profile",
            *SCAN_OUTPUT_PARAMETER_FIELDS,
            "seed_count",
            "metric",
            "mean_ps",
            "sigma_ps",
            "low_ps",
            "high_ps",
        ),
        plot_rows,
    )


def _column_overrides_from_args(args: argparse.Namespace) -> ColumnOverrides:
    return ColumnOverrides(
        flow_id=args.flow_id_column,
        payload=args.payload_column,
        start=args.start_column,
        end=args.end_column,
        fct=args.fct_column,
        input_time_unit=args.input_time_unit,
    )


def _add_column_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--flow-id-column")
    parser.add_argument("--payload-column")
    parser.add_argument("--start-column")
    parser.add_argument("--end-column")
    parser.add_argument("--fct-column")
    parser.add_argument(
        "--input-time-unit",
        choices=("ps", "ns"),
        help="unit for overridden time columns whose names do not declare ps or ns",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    cdf = subparsers.add_parser("cdf", help="summarize paired profile/seed completion CSVs")
    cdf.add_argument("--input-root", type=Path, required=True)
    cdf.add_argument("--output-dir", type=Path, required=True)
    cdf.add_argument("--completion-name", default="flowsInfo.csv")
    _add_column_arguments(cdf)

    scan = subparsers.add_parser("scan", help="summarize an explicit parameter scan")
    scan.add_argument("--manifest", type=Path, required=True)
    scan.add_argument("--output-dir", type=Path, required=True)
    _add_column_arguments(scan)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        overrides = _column_overrides_from_args(args)
        if args.command == "cdf":
            runs = discover_runs(args.input_root, args.completion_name, overrides)
            write_cdf_analysis(runs, args.output_dir)
        elif args.command == "scan":
            scan_runs = read_scan_manifest(args.manifest, overrides)
            write_parameter_scan(scan_runs, args.output_dir)
        else:  # pragma: no cover - argparse enforces the command set.
            parser.error(f"unknown command {args.command!r}")
    except AnalysisError as exc:
        parser.exit(2, f"error: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
