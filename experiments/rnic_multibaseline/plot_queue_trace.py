#!/usr/bin/env python3
"""Plot an exact event-boundary ns-tm3 egress queue trace.

The input is produced by the optional rnic-cn queue observer.  Rows are kept in
file order because several queue transitions may occur at the same picosecond.
The upper panel shows instantaneous buffered and total backlog bytes; the lower
panel converts the same backlog to q/C delay and compares it with the Ring-CAM
delay window.
"""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


PS_PER_SECOND = 1_000_000_000_000
PS_PER_US = 1_000_000


class TraceError(ValueError):
    pass


@dataclass(frozen=True)
class TracePoint:
    time_ps: int
    transition: str
    buffered_bytes: int
    in_service_bytes: int
    backlog_bytes: int
    flow_id: int
    packet_id: int


def nonnegative_int(text: str) -> int:
    value = int(text, 10)
    if value < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return value


def positive_int(text: str) -> int:
    value = int(text, 10)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def read_port_trace(
    path: Path, tier: str, switch_id: int, egress_id: int
) -> list[TracePoint]:
    required = {
        "time_ps",
        "tier",
        "switch_id",
        "egress_id",
        "transition",
        "flow_id",
        "packet_id",
        "egress_buffered_bytes",
        "egress_in_service_bytes",
        "egress_backlog_bytes",
    }
    try:
        handle = path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise TraceError(f"cannot read trace {path}: {exc}") from exc

    points: list[TracePoint] = []
    with handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise TraceError(f"{path}: missing CSV header")
        missing = sorted(required - set(reader.fieldnames))
        if missing:
            raise TraceError(f"{path}: missing columns {', '.join(missing)}")
        previous_time = -1
        for row_number, row in enumerate(reader, start=2):
            if (
                row["tier"] != tier
                or int(row["switch_id"]) != switch_id
                or int(row["egress_id"]) != egress_id
            ):
                continue
            point = TracePoint(
                time_ps=int(row["time_ps"]),
                transition=row["transition"],
                buffered_bytes=int(row["egress_buffered_bytes"]),
                in_service_bytes=int(row["egress_in_service_bytes"]),
                backlog_bytes=int(row["egress_backlog_bytes"]),
                flow_id=int(row["flow_id"]),
                packet_id=int(row["packet_id"]),
            )
            if point.time_ps < previous_time:
                raise TraceError(
                    f"{path}:{row_number}: selected-port time moved backwards"
                )
            if min(
                point.buffered_bytes,
                point.in_service_bytes,
                point.backlog_bytes,
            ) < 0:
                raise TraceError(f"{path}:{row_number}: negative queue extent")
            if point.backlog_bytes != (
                point.buffered_bytes + point.in_service_bytes
            ):
                raise TraceError(
                    f"{path}:{row_number}: backlog is not buffered+in-service"
                )
            points.append(point)
            previous_time = point.time_ps
    if not points:
        raise TraceError(
            f"{path}: no rows for {tier}:{switch_id}:egress:{egress_id}"
        )
    return points


def queue_delay_ps(backlog_bytes: int, link_bps: int) -> float:
    return backlog_bytes * 8 * PS_PER_SECOND / link_bps


def packet_queue_interval_ps(
    points: Sequence[TracePoint], packet_id: int
) -> tuple[int, int]:
    enqueues = [
        point.time_ps
        for point in points
        if point.packet_id == packet_id and point.transition == "enqueue"
    ]
    dequeues = [
        point.time_ps
        for point in points
        if point.packet_id == packet_id and point.transition == "dequeue"
    ]
    if len(enqueues) != 1 or len(dequeues) != 1:
        raise TraceError(
            f"packet {packet_id}: expected exactly one enqueue and dequeue "
            "on the selected port"
        )
    if dequeues[0] < enqueues[0]:
        raise TraceError(f"packet {packet_id}: dequeue precedes enqueue")
    return enqueues[0], dequeues[0]


def write_plot(
    points: Sequence[TracePoint],
    output: Path,
    link_bps: int,
    ring_window_ps: int,
    title: str,
    zoom_radius_us: float | None,
    tagged_packet_id: int | None = None,
) -> dict[str, object]:
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import FuncFormatter
    except ImportError as exc:
        raise TraceError("matplotlib is required to render the queue plot") from exc

    peak_index = max(range(len(points)), key=lambda index: points[index].backlog_bytes)
    peak = points[peak_index]
    origin_ps = points[0].time_ps
    times_us = [(point.time_ps - origin_ps) / PS_PER_US for point in points]
    buffered_kib = [point.buffered_bytes / 1024 for point in points]
    backlog_kib = [point.backlog_bytes / 1024 for point in points]
    delays_us = [queue_delay_ps(point.backlog_bytes, link_bps) / PS_PER_US for point in points]

    fig, (queue_axis, delay_axis) = plt.subplots(
        2,
        1,
        figsize=(10.8, 6.2),
        sharex=True,
        gridspec_kw={"height_ratios": (1.45, 1)},
        constrained_layout=True,
    )
    queue_axis.step(times_us, backlog_kib, where="post", label="backlog (VoQ + in flight)", linewidth=1.4)
    queue_axis.step(times_us, buffered_kib, where="post", label="buffered VoQ", linewidth=1.0)
    queue_axis.set_ylabel("Instantaneous queue (KiB)")
    queue_axis.grid(axis="y", linewidth=0.45, alpha=0.35)
    queue_axis.legend(frameon=False, loc="upper right")

    delay_axis.step(times_us, delays_us, where="post", label="q/C", linewidth=1.35)
    delay_axis.axhline(
        ring_window_ps / PS_PER_US,
        color="tab:red",
        linestyle="--",
        linewidth=1.0,
        label=f"Ring-CAM Δ = {ring_window_ps / PS_PER_US:.3f} µs",
    )
    delay_axis.set_ylabel("Queue delay q/C (µs)")
    delay_axis.set_xlabel("Time from first selected-port event (µs)")
    delay_axis.grid(axis="y", linewidth=0.45, alpha=0.35)
    delay_axis.legend(frameon=False, loc="upper right")

    tagged_interval: tuple[int, int] | None = None
    if tagged_packet_id is not None:
        tagged_interval = packet_queue_interval_ps(points, tagged_packet_id)
        tagged_start_us = (tagged_interval[0] - origin_ps) / PS_PER_US
        tagged_end_us = (tagged_interval[1] - origin_ps) / PS_PER_US
        tagged_wait_us = (tagged_interval[1] - tagged_interval[0]) / PS_PER_US
        for axis in (queue_axis, delay_axis):
            axis.axvspan(
                tagged_start_us,
                tagged_end_us,
                color="tab:purple",
                alpha=0.10,
                linewidth=0,
            )
        delay_axis.annotate(
            f"tagged DATA wait = {tagged_wait_us:.5f} µs",
            xy=((tagged_start_us + tagged_end_us) / 2, 0),
            xytext=(0, 10),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    peak_time_us = times_us[peak_index]
    peak_delay_us = delays_us[peak_index]
    queue_axis.scatter([peak_time_us], [backlog_kib[peak_index]], s=22, zorder=4)
    queue_axis.annotate(
        f"peak {peak.backlog_bytes:,} B\nq/C {peak_delay_us:.4f} µs",
        (peak_time_us, backlog_kib[peak_index]),
        xytext=(8, 8),
        textcoords="offset points",
        fontsize=9,
    )
    if zoom_radius_us is not None:
        left = max(times_us[0], peak_time_us - zoom_radius_us)
        right = min(times_us[-1], peak_time_us + zoom_radius_us)
        if right > left:
            delay_axis.set_xlim(left, right)

    fig.suptitle(title)
    queue_axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=180)
    plt.close(fig)

    return {
        "event_count": len(points),
        "first_time_ps": points[0].time_ps,
        "last_time_ps": points[-1].time_ps,
        "peak_time_ps": peak.time_ps,
        "peak_transition": peak.transition,
        "peak_flow_id": peak.flow_id,
        "peak_packet_id": peak.packet_id,
        "peak_buffered_bytes": peak.buffered_bytes,
        "peak_in_service_bytes": peak.in_service_bytes,
        "peak_backlog_bytes": peak.backlog_bytes,
        "peak_queue_delay_ps": queue_delay_ps(peak.backlog_bytes, link_bps),
        "ring_window_ps": ring_window_ps,
        "crosses_ring_window": queue_delay_ps(peak.backlog_bytes, link_bps)
        > ring_window_ps,
        **(
            {
                "tagged_packet_id": tagged_packet_id,
                "tagged_enqueue_ps": tagged_interval[0],
                "tagged_dequeue_ps": tagged_interval[1],
                "tagged_queue_wait_ps": tagged_interval[1]
                - tagged_interval[0],
            }
            if tagged_interval is not None
            else {}
        ),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary-json", type=Path)
    parser.add_argument("--tier", choices=("leaf", "spine", "core"), required=True)
    parser.add_argument("--switch-id", type=nonnegative_int, required=True)
    parser.add_argument("--egress-id", type=nonnegative_int, required=True)
    parser.add_argument("--link-bps", type=positive_int, required=True)
    parser.add_argument("--ring-window-ps", type=positive_int, default=4_096_000)
    parser.add_argument(
        "--tagged-packet-id",
        type=nonnegative_int,
        help="shade this packet's enqueue-to-dequeue residence interval",
    )
    parser.add_argument(
        "--zoom-radius-us",
        type=float,
        help="show only this many microseconds on either side of peak backlog",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.zoom_radius_us is not None and args.zoom_radius_us <= 0:
        raise TraceError("--zoom-radius-us must be positive")
    points = read_port_trace(args.input, args.tier, args.switch_id, args.egress_id)
    port = f"{args.tier}:{args.switch_id}:egress:{args.egress_id}"
    summary = write_plot(
        points,
        args.output,
        args.link_bps,
        args.ring_window_ps,
        f"ns-tm3 instantaneous egress queue — {port}",
        args.zoom_radius_us,
        args.tagged_packet_id,
    )
    summary.update(
        {
            "schema": "rnic-ns-tm3-queue-plot-v1",
            "input": str(args.input.resolve()),
            "port": port,
            "link_bps": args.link_bps,
        }
    )
    if args.summary_json:
        args.summary_json.parent.mkdir(parents=True, exist_ok=True)
        args.summary_json.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
