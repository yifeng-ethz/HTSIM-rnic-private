#!/usr/bin/env python3
"""Render data-backed study figures for the NERSC-style presentation."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Sequence


COLORS = {
    "rnic-cn": "#0066B3",
    "dcqcn": "#4A4A4A",
    "rnic-ss": "#7C3AED",
    "rnic-nn": "#159D82",
    "rnic-nn-fluid": "#E68A00",
}
LABELS = {
    "rnic-cn": "rnic-cn / ns-tm3",
    "dcqcn": "DCQCN / ns-tm3",
    "rnic-ss": "rnic-ss / ns-rosetta",
    "rnic-nn": "rnic-nn / manifold",
    "rnic-nn-fluid": "rnic-nn-fluid / manifold",
}


def matplotlib_modules():
    import matplotlib.pyplot as plt
    import numpy as np

    return plt, np


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError(f"{path} contains no data rows")
    return rows


def style_axes(axis) -> None:
    axis.spines["top"].set_visible(False)
    axis.spines["right"].set_visible(False)
    axis.spines["left"].set_color("#BFC5CC")
    axis.spines["bottom"].set_color("#BFC5CC")
    axis.grid(True, axis="y", color="#E7E9EC", linewidth=0.8)
    axis.tick_params(labelsize=9, colors="#40464D")


def save_figure(fig, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=220, bbox_inches="tight", facecolor="white")
    fig.clf()


def plot_incast(summary_csv: Path, output: Path) -> None:
    plt, _ = matplotlib_modules()
    rows = read_rows(summary_csv)
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["scheme"] in {"dcqcn", "rnic-cn"}:
            grouped[row["scheme"]].append(row)
    if set(grouped) != {"dcqcn", "rnic-cn"}:
        raise ValueError("incast plot requires paired dcqcn and rnic-cn rows")

    fig, axis = plt.subplots(figsize=(11.2, 4.4))
    for scheme in ("rnic-cn", "dcqcn"):
        ordered = sorted(grouped[scheme], key=lambda row: int(row["flow_count"]))
        x = [int(row["flow_count"]) for row in ordered]
        mean = [float(row["payload_goodput_mean_gbps"]) for row in ordered]
        low = [float(row["payload_goodput_low_gbps"]) for row in ordered]
        high = [float(row["payload_goodput_high_gbps"]) for row in ordered]
        axis.plot(
            x,
            mean,
            color=COLORS[scheme],
            marker="o",
            linewidth=2.4,
            markersize=5,
            label=LABELS[scheme],
        )
        axis.fill_between(x, low, high, color=COLORS[scheme], alpha=0.14)
    axis.axhline(400, color="#9AA0A6", linestyle="--", linewidth=1)
    axis.text(63.5, 404, "400 Gbit/s wire rate", ha="right", va="bottom", fontsize=8, color="#697078")
    axis.set_xscale("log", base=2)
    axis.set_xticks((2, 4, 8, 16, 32, 64), labels=("2", "4", "8", "16", "32", "64"))
    axis.set_xlim(1.8, 70)
    axis.set_ylim(bottom=0)
    axis.set_xlabel("simultaneous directed flows", fontsize=10)
    axis.set_ylabel("receiver payload goodput (Gbit/s)", fontsize=10)
    axis.legend(frameon=False, loc="best", fontsize=9)
    style_axes(axis)
    fig.tight_layout()
    save_figure(fig, output)


def plot_cdf(
    summary_csv: Path,
    output: Path,
    profiles: Sequence[str],
    log_x: bool,
) -> None:
    plt, _ = matplotlib_modules()
    rows = read_rows(summary_csv)
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["profile"] in profiles:
            grouped[row["profile"]].append(row)
    if set(grouped) != set(profiles):
        raise ValueError(
            f"CDF is missing profiles: {sorted(set(profiles) - set(grouped))}"
        )

    fig, axis = plt.subplots(figsize=(11.2, 4.6))
    for profile in profiles:
        ordered = sorted(grouped[profile], key=lambda row: int(row["grid_fct_ps"]))
        x = [int(row["grid_fct_ps"]) / 1_000_000_000 for row in ordered]
        mean = [float(row["cdf_mean"]) for row in ordered]
        low = [float(row["cdf_low"]) for row in ordered]
        high = [float(row["cdf_high"]) for row in ordered]
        if log_x:
            selected = [index for index, value in enumerate(x) if value > 0]
            x = [x[index] for index in selected]
            mean = [mean[index] for index in selected]
            low = [low[index] for index in selected]
            high = [high[index] for index in selected]
        axis.step(
            x,
            mean,
            where="post",
            color=COLORS[profile],
            linewidth=2.1,
            label=LABELS[profile],
        )
        axis.fill_between(
            x,
            low,
            high,
            step="post",
            color=COLORS[profile],
            alpha=0.12,
        )
    if log_x:
        axis.set_xscale("log")
    axis.set_ylim(0, 1.015)
    axis.set_xlabel("flow completion time (ms)", fontsize=10)
    axis.set_ylabel("mean empirical CDF", fontsize=10)
    axis.legend(frameon=False, loc="lower right", fontsize=8.5)
    style_axes(axis)
    axis.grid(True, which="both", axis="x", color="#F0F1F2", linewidth=0.6)
    fig.tight_layout()
    save_figure(fig, output)


def plot_jct(
    profile_summary_csv: Path,
    output: Path,
    profiles: Sequence[str],
) -> None:
    plt, np = matplotlib_modules()
    rows = {row["profile"]: row for row in read_rows(profile_summary_csv)}
    missing = set(profiles) - set(rows)
    if missing:
        raise ValueError(f"JCT plot is missing profiles: {sorted(missing)}")
    means = [float(rows[profile]["jct_ps_mean"]) / 1_000_000_000 for profile in profiles]
    sigma = [float(rows[profile]["jct_ps_sigma"]) / 1_000_000_000 for profile in profiles]
    x = np.arange(len(profiles))
    fig, axis = plt.subplots(figsize=(11.2, 4.6))
    axis.bar(
        x,
        means,
        yerr=sigma,
        capsize=4,
        color=[COLORS[profile] for profile in profiles],
        alpha=0.9,
        width=0.68,
    )
    axis.set_xticks(x, [LABELS[profile].split(" /")[0] for profile in profiles])
    axis.set_ylabel("collective JCT (ms)", fontsize=10)
    style_axes(axis)
    for index, value in enumerate(means):
        axis.text(index, value + max(means) * 0.025, f"{value:.1f}", ha="center", va="bottom", fontsize=8.5)
    axis.set_ylim(0, max(means[index] + sigma[index] for index in range(len(means))) * 1.17)
    fig.tight_layout()
    save_figure(fig, output)


def plot_scan(
    scan_summary_csv: Path,
    scan_manifest_csv: Path,
    output: Path,
) -> None:
    plt, np = matplotlib_modules()
    summary = {row["profile"]: row for row in read_rows(scan_summary_csv)}
    manifest_rows = read_rows(scan_manifest_csv)
    descriptors: dict[str, tuple[str, str]] = {}
    for row in manifest_rows:
        if (row.get("status") or "complete").strip() != "complete":
            continue
        descriptors[row["profile"]] = (row["scan_axis"], row["scan_value"])
    if "default" not in summary:
        raise ValueError("scan summary lacks the declared default")
    default_p99 = float(summary["default"]["p99_fct_ps_mean"])
    default_jct = float(summary["default"]["jct_ps_mean"])
    axes = [
        "q_hi_mib",
        "q_lo_over_q_hi",
        "telemetry_delay_us",
        "path_hysteresis_ns",
        "credit_quantum_packets",
        "buffer_mib",
    ]
    titles = {
        "q_hi_mib": "Q_hi (MiB)",
        "q_lo_over_q_hi": "Q_lo / Q_hi",
        "telemetry_delay_us": "extra telemetry (µs)",
        "path_hysteresis_ns": "path hysteresis (ns)",
        "credit_quantum_packets": "credit quantum (packets)",
        "buffer_mib": "shared buffer (MiB)",
    }
    default_values = {
        "q_hi_mib": "4",
        "q_lo_over_q_hi": "0.50",
        "telemetry_delay_us": "0",
        "path_hysteresis_ns": "0",
        "credit_quantum_packets": "4",
        "buffer_mib": "64",
    }
    fig, panels = plt.subplots(2, 3, figsize=(11.2, 5.5))
    for panel, axis_name in zip(panels.flat, axes):
        points = []
        for profile, (candidate_axis, value) in descriptors.items():
            if candidate_axis == axis_name:
                points.append((float(value), value, summary[profile]))
        points.append(
            (
                float(default_values[axis_name]),
                default_values[axis_name],
                summary["default"],
            )
        )
        points.sort()
        if not points:
            raise ValueError(f"scan has no {axis_name} points")
        positions = np.arange(len(points))
        p99 = [float(row["p99_fct_ps_mean"]) / default_p99 for _, _, row in points]
        jct = [float(row["jct_ps_mean"]) / default_jct for _, _, row in points]
        panel.plot(positions, p99, marker="o", color=COLORS["rnic-ss"], label="p99 FCT")
        panel.plot(positions, jct, marker="s", color="#0066B3", label="JCT")
        panel.axhline(1.0, color="#9AA0A6", linestyle="--", linewidth=0.9)
        panel.set_xticks(positions, [value for _, value, _ in points])
        panel.set_title(titles[axis_name], fontsize=9.5, fontweight="bold")
        panel.set_ylim(bottom=0)
        style_axes(panel)
    panels[0, 0].set_ylabel("normalized to default", fontsize=9)
    panels[1, 0].set_ylabel("normalized to default", fontsize=9)
    handles, labels = panels[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, loc="upper center", ncol=2, fontsize=8.5)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save_figure(fig, output)


def read_state_trace(path: Path) -> list[dict[str, object]]:
    required = (
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
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != required:
            raise ValueError(f"{path} does not use the common state-trace schema")
        rows: list[dict[str, object]] = []
        for raw in reader:
            if raw["effective_rate_bps"] == "":
                raise ValueError(
                    f"{path} has no effective rate for {raw['event']}"
                )
            rows.append(
                {
                    "time_ps": int(raw["time_ps"]),
                    "flow_id": int(raw["flow_id"]),
                    "source": int(raw["source"]),
                    "destination": int(raw["destination"]),
                    "event": raw["event"],
                    "effective_rate_bps": int(raw["effective_rate_bps"]),
                }
            )
    if not rows:
        raise ValueError(f"{path} contains no state rows")
    if any(
        later["time_ps"] < earlier["time_ps"]
        for earlier, later in zip(rows, rows[1:])
    ):
        raise ValueError(f"{path} has decreasing timestamps")
    return rows


def _rate_series_by_flow(
    rows: Sequence[dict[str, object]],
) -> dict[int, list[tuple[int, int]]]:
    grouped: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for row in rows:
        flow_id = int(row["flow_id"])
        point = (int(row["time_ps"]), int(row["effective_rate_bps"]))
        if grouped[flow_id] and grouped[flow_id][-1][1] == point[1]:
            # ACK/control progress does not manufacture rate samples.
            continue
        grouped[flow_id].append(point)
    return grouped


def _windowed_step(
    points: Sequence[tuple[int, int]], lower_ps: int, upper_ps: int
) -> tuple[list[float], list[float]]:
    if lower_ps >= upper_ps:
        raise ValueError("rate window must have positive width")
    state = 0
    selected: list[tuple[int, int]] = []
    for time_ps, rate_bps in points:
        if time_ps <= lower_ps:
            state = rate_bps
        elif time_ps <= upper_ps:
            selected.append((time_ps, rate_bps))
        else:
            break
    timeline = [(lower_ps, state), *selected]
    if timeline[-1][0] != upper_ps:
        timeline.append((upper_ps, timeline[-1][1]))
    return (
        [time_ps / 1_000_000_000 for time_ps, _ in timeline],
        [rate_bps / 1_000_000_000 for _, rate_bps in timeline],
    )


def plot_join_dynamics(
    dcqcn_trace_csv: Path,
    rnic_cn_trace_csv: Path,
    workload_metadata_json: Path,
    output: Path,
) -> None:
    plt, _ = matplotlib_modules()
    traces = {
        "dcqcn": read_state_trace(dcqcn_trace_csv),
        "rnic-cn": read_state_trace(rnic_cn_trace_csv),
    }
    metadata = json.loads(workload_metadata_json.read_text(encoding="utf-8"))
    sources = [int(value) for value in metadata["source_nodes"]]
    joins_ps = [int(value) * 1000 for value in metadata["join_time_ns"]]
    ideal_exits_ps = [
        int(value) * 1000
        for value in metadata["ideal_target_completion_time_ns"]
    ]
    interval_ps = int(metadata["join_interval_ns"]) * 1000
    if len(sources) != 8 or len(joins_ps) != 8:
        raise ValueError("join dynamics plot requires the declared eight flows")

    flow_sources = {
        scheme: {
            int(row["flow_id"]): int(row["source"])
            for row in rows
            if row["event"] == "flow-start"
        }
        for scheme, rows in traces.items()
    }
    series = {
        scheme: _rate_series_by_flow(rows)
        for scheme, rows in traces.items()
    }
    completion_events = {
        scheme: sorted(
            (
                int(row["time_ps"]),
                int(row["flow_id"]),
            )
            for row in rows
            if row["event"] == "completion"
        )
        for scheme, rows in traces.items()
    }
    if any(len(events) != 8 for events in completion_events.values()):
        raise ValueError("each state trace must contain eight completions")

    palette = plt.get_cmap("tab10")
    fig, panels = plt.subplots(2, 2, figsize=(11.2, 7.0))
    max_completion_ps = max(
        events[-1][0] for events in completion_events.values()
    )
    overall_upper_ps = max(ideal_exits_ps[-1] + interval_ps,
                           max_completion_ps + interval_ps // 2)
    for panel, scheme in zip(panels[0], ("dcqcn", "rnic-cn")):
        for index, flow_id in enumerate(sorted(series[scheme], key=lambda value: flow_sources[scheme][value])):
            x, y = _windowed_step(series[scheme][flow_id], 0, overall_upper_ps)
            source = flow_sources[scheme][flow_id]
            panel.step(
                x,
                y,
                where="post",
                linewidth=1.35,
                color=palette(index),
                label=f"source {source}",
            )
        for join_ps in joins_ps[1:]:
            panel.axvline(join_ps / 1_000_000_000,
                          color="#D7DBDF", linewidth=0.55)
        panel.set_title(LABELS[scheme], fontsize=10, fontweight="bold")
        panel.set_xlabel("simulation time (ms)", fontsize=9)
        panel.set_ylabel("effective sender rate (Gbit/s)", fontsize=9)
        panel.set_xlim(0, overall_upper_ps / 1_000_000_000)
        panel.set_ylim(bottom=0)
        panel.legend(frameon=False, fontsize=6.8, ncol=2, loc="upper right")
        style_axes(panel)

    join_center_ps = joins_ps[1]
    join_lower_ps = join_center_ps - interval_ps // 2
    join_upper_ps = join_center_ps + interval_ps // 2
    join_panel = panels[1, 0]
    line_styles = {sources[0]: "-", sources[1]: "--"}
    for scheme in ("dcqcn", "rnic-cn"):
        for flow_id, source in flow_sources[scheme].items():
            if source not in line_styles:
                continue
            x, y = _windowed_step(
                series[scheme][flow_id], join_lower_ps, join_upper_ps
            )
            x = [value - join_center_ps / 1_000_000_000 for value in x]
            join_panel.step(
                x,
                y,
                where="post",
                color=COLORS[scheme],
                linestyle=line_styles[source],
                linewidth=2.0,
                label=f"{LABELS[scheme].split(' /')[0]}, source {source}",
            )
    join_panel.axvline(0, color="#9AA0A6", linestyle=":", linewidth=1)
    join_panel.set_title("1 → 2 flow join (aligned to flow 1 start)", fontsize=9.5, fontweight="bold")
    join_panel.set_xlabel("time from second join (ms)", fontsize=9)
    join_panel.set_ylabel("effective sender rate (Gbit/s)", fontsize=9)
    join_panel.set_ylim(bottom=0)
    join_panel.legend(frameon=False, fontsize=6.8, loc="best")
    style_axes(join_panel)

    exit_panel = panels[1, 1]
    for scheme in ("dcqcn", "rnic-cn"):
        events = completion_events[scheme]
        center_ps = events[-2][0]
        final_two = {events[-2][1], events[-1][1]}
        for flow_id in sorted(final_two, key=lambda value: flow_sources[scheme][value]):
            source = flow_sources[scheme][flow_id]
            x, y = _windowed_step(
                series[scheme][flow_id],
                center_ps - interval_ps // 2,
                center_ps + interval_ps // 2,
            )
            x = [value - center_ps / 1_000_000_000 for value in x]
            role = "exiting" if flow_id == events[-2][1] else "remaining"
            exit_panel.step(
                x,
                y,
                where="post",
                color=COLORS[scheme],
                linestyle="--" if role == "exiting" else "-",
                linewidth=2.0,
                label=f"{LABELS[scheme].split(' /')[0]}, {role} (src {source})",
            )
    exit_panel.axvline(0, color="#9AA0A6", linestyle=":", linewidth=1)
    exit_panel.set_title("2 → 1 flow exit (aligned per scheme)", fontsize=9.5, fontweight="bold")
    exit_panel.set_xlabel("time from penultimate completion (ms)", fontsize=9)
    exit_panel.set_ylabel("effective sender rate (Gbit/s)", fontsize=9)
    exit_panel.set_ylim(bottom=0)
    exit_panel.legend(frameon=False, fontsize=6.8, loc="best")
    style_axes(exit_panel)
    fig.tight_layout()
    save_figure(fig, output)


def parse_profiles(value: str) -> tuple[str, ...]:
    profiles = tuple(item.strip() for item in value.split(",") if item.strip())
    unknown = set(profiles) - set(COLORS)
    if not profiles or unknown:
        raise argparse.ArgumentTypeError(f"unknown or empty profiles: {sorted(unknown)}")
    return profiles


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    incast = subparsers.add_parser("incast")
    incast.add_argument("--summary", type=Path, required=True)
    incast.add_argument("--output", type=Path, required=True)
    cdf = subparsers.add_parser("cdf")
    cdf.add_argument("--summary", type=Path, required=True)
    cdf.add_argument("--output", type=Path, required=True)
    cdf.add_argument("--profiles", type=parse_profiles, required=True)
    cdf.add_argument("--log-x", action="store_true")
    jct = subparsers.add_parser("jct")
    jct.add_argument("--summary", type=Path, required=True)
    jct.add_argument("--output", type=Path, required=True)
    jct.add_argument("--profiles", type=parse_profiles, required=True)
    scan = subparsers.add_parser("scan")
    scan.add_argument("--summary", type=Path, required=True)
    scan.add_argument("--manifest", type=Path, required=True)
    scan.add_argument("--output", type=Path, required=True)
    dynamics = subparsers.add_parser("join-dynamics")
    dynamics.add_argument("--dcqcn-trace", type=Path, required=True)
    dynamics.add_argument("--rnic-cn-trace", type=Path, required=True)
    dynamics.add_argument("--metadata", type=Path, required=True)
    dynamics.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "incast":
        plot_incast(args.summary, args.output)
    elif args.command == "cdf":
        plot_cdf(args.summary, args.output, args.profiles, args.log_x)
    elif args.command == "jct":
        plot_jct(args.summary, args.output, args.profiles)
    elif args.command == "scan":
        plot_scan(args.summary, args.manifest, args.output)
    elif args.command == "join-dynamics":
        plot_join_dynamics(
            args.dcqcn_trace,
            args.rnic_cn_trace,
            args.metadata,
            args.output,
        )
    else:
        raise AssertionError(args.command)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
