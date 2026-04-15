#!/usr/bin/env python3
"""
Blog-friendly plots for Phanes benchmark results.

Usage:
    python3 benchmark/plot_results.py results.json
    python3 benchmark/plot_results.py results.json --out benchmark_plots
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import numpy as np
except ImportError:
    sys.exit("Error: install matplotlib and numpy first: pip install matplotlib numpy")


# ---------------------------------------------------------------------------
# Global style
# ---------------------------------------------------------------------------

plt.rcParams.update(
    {
        "figure.facecolor": "white",
        "axes.facecolor": "white",
        "savefig.facecolor": "white",
        "axes.edgecolor": "#333333",
        "axes.labelcolor": "#222222",
        "xtick.color": "#222222",
        "ytick.color": "#222222",
        "text.color": "#111111",
        "font.size": 11,
        "axes.titlesize": 14,
        "axes.titleweight": "bold",
        "axes.labelsize": 12,
        "legend.fontsize": 10,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
    }
)

TAB10 = plt.cm.tab10.colors  # type: ignore[attr-defined]
FIG_W, FIG_H = 10, 5
DPI = 180


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

def load_benchmarks(path: str) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)

    raw = [b for b in data["benchmarks"] if b.get("run_type") == "iteration"]
    if raw:
        return raw

    return [b for b in data["benchmarks"] if b["name"].endswith("_mean")]


def parse_name(name: str) -> tuple[str, list[int]]:
    for sfx in ("_mean", "_median", "_stddev", "_cv"):
        if name.endswith(sfx):
            name = name[: -len(sfx)]
            break

    parts = name.split("/")
    args: list[int] = []
    for p in parts[1:]:
        try:
            args.append(int(p))
        except ValueError:
            pass
    return parts[0], args


def to_ns(b: dict) -> float:
    t = float(b["real_time"])
    unit = b.get("time_unit", "ns")
    return t * {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}.get(unit, 1.0)


def safe_items_per_second(b: dict) -> float:
    try:
        return float(b.get("items_per_second", 0.0))
    except (TypeError, ValueError):
        return 0.0


def median_ns(rows: list[dict]) -> float:
    vals = [to_ns(b) for b in rows]
    return float(np.median(vals)) if vals else 0.0


def best_unit(values_ns: list[float]) -> tuple[float, str]:
    mx = max(values_ns) if values_ns else 1.0
    for divisor, label in ((1e9, "s"), (1e6, "ms"), (1e3, "μs")):
        if mx >= divisor:
            return divisor, label
    return 1.0, "ns"


# ---------------------------------------------------------------------------
# Style helpers
# ---------------------------------------------------------------------------

def human_formatter(decimals: int = 2):
    def _fmt(x, _pos=None):
        if abs(x) >= 100:
            return f"{x:.0f}"
        if abs(x) >= 10:
            return f"{x:.1f}"
        return f"{x:.{decimals}f}"
    return ticker.FuncFormatter(_fmt)


def disable_offset(ax: plt.Axes) -> None:
    ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.yaxis.get_major_formatter().set_useOffset(False)
    ax.yaxis.get_major_formatter().set_scientific(False)


def style_time_axis(ax: plt.Axes, decimals: int = 2) -> None:
    disable_offset(ax)
    ax.yaxis.set_major_formatter(human_formatter(decimals))


def style_count_axis(ax: plt.Axes, decimals: int = 2) -> None:
    ax.yaxis.set_major_formatter(human_formatter(decimals))


def style_axes(ax: plt.Axes, title: str, xlabel: str, ylabel: str) -> None:
    ax.set_title(title, pad=12)
    ax.set_xlabel(xlabel, labelpad=8)
    ax.set_ylabel(ylabel, labelpad=8)
    ax.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.22, zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def save_fig(fig: plt.Figure, dest: Path) -> None:
    fig.tight_layout()
    fig.savefig(dest, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved  {dest}")


# ---------------------------------------------------------------------------
# Plot generators
# ---------------------------------------------------------------------------

def plot_analyzer_scalability(grouped: dict[str, list[dict]], out: Path) -> None:
    keys = [
        "BM_FileStats",
        "BM_DirectoryMetrics",
        "BM_EmptyDirs",
        "BM_ExtensionStats",
        "BM_RecentFiles",
        "BM_Summary",
        "BM_DirectoryStats",
    ]

    series: dict[str, tuple[list[int], list[float]]] = {}
    for key in keys:
        if key not in grouped:
            continue
        pts = sorted(
            ((parse_name(b["name"])[1][0], to_ns(b)) for b in grouped[key]),
            key=lambda p: p[0],
        )
        if pts:
            series[key.replace("BM_", "")] = ([p[0] for p in pts], [p[1] for p in pts])

    if not series:
        return

    all_ns = [v for _, ys in series.values() for v in ys]
    divisor, unit = best_unit(all_ns)
    norm = {k: (xs, [y / divisor for y in ys]) for k, (xs, ys) in series.items()}

    fig, ax = plt.subplots(figsize=(11, 6))
    for i, (label, (xs, ys)) in enumerate(norm.items()):
        ax.plot(xs, ys, marker="o", markersize=7, linewidth=2.5, color=TAB10[i % 10], label=label)

    ax.legend(framealpha=0.9, ncols=2)
    style_axes(ax, "Analyzer — Algorithm Scalability", "Number of directories", f"Time ({unit})")
    style_time_axis(ax)
    save_fig(fig, out / "analyzer_scalability.png")


def plot_topn_performance(grouped: dict[str, list[dict]], out: Path) -> None:
    series: dict[str, tuple[list[int], list[float]]] = {}
    for key in ("BM_LargestNFiles", "BM_LargestNDirs"):
        if key not in grouped:
            continue
        pts = sorted(
            ((parse_name(b["name"])[1][0], to_ns(b)) for b in grouped[key]),
            key=lambda p: p[0],
        )
        if pts:
            series[key.replace("BM_", "")] = ([p[0] for p in pts], [p[1] for p in pts])

    if not series:
        return

    all_ns = [v for _, ys in series.values() for v in ys]
    divisor, unit = best_unit(all_ns)
    norm = {k: (xs, [y / divisor for y in ys]) for k, (xs, ys) in series.items()}

    fig, ax = plt.subplots(figsize=(9.5, 5.8))
    for i, (label, (xs, ys)) in enumerate(norm.items()):
        ax.plot(xs, ys, marker="o", markersize=7, linewidth=2.5, color=TAB10[i % 10], label=label)

    ax.legend(framealpha=0.9)
    style_axes(ax, "Analyzer — Top-N Query Performance", "N (number of results)", f"Time ({unit})")
    style_time_axis(ax)
    save_fig(fig, out / "topn_performance.png")


def plot_builder_scheduler(grouped: dict[str, list[dict]], out: Path) -> None:
    panels_needed = any(
        k in grouped
        for k in (
            "BM_BuildTree_ThreadOverhead",
            "BM_BuildTree_Granularity",
            "BM_BuildTree_Flat",
            "BM_BuildTree_Nested",
            "BM_BuildTree_Balanced",
            "BM_BuildTree_Skewed",
        )
    )
    if not panels_needed:
        return

    fig, axes = plt.subplots(2, 2, figsize=(13.5, 9))
    fig.suptitle("Builder — Parallel Scanner Scheduler Analysis", fontsize=16, fontweight="bold", y=0.98)
    (ax_overhead, ax_granularity), (ax_topology, ax_skew) = axes

    if "BM_BuildTree_ThreadOverhead" in grouped:
        pts = sorted(
            ((parse_name(b["name"])[1][0], to_ns(b)) for b in grouped["BM_BuildTree_ThreadOverhead"]),
            key=lambda p: p[0],
        )
        if pts:
            divisor, unit = best_unit([p[1] for p in pts])
            ax_overhead.plot(
                [p[0] for p in pts],
                [p[1] / divisor for p in pts],
                marker="o",
                markersize=7,
                color=TAB10[2],
                linewidth=2.5,
            )
            style_axes(ax_overhead, "A. Thread-Pool Overhead Floor", "Directories (1 file each)", f"Time ({unit})")
            style_time_axis(ax_overhead)

    if "BM_BuildTree_Granularity" in grouped:
        pts = sorted(
            ((parse_name(b["name"])[1][0], to_ns(b)) for b in grouped["BM_BuildTree_Granularity"]),
            key=lambda p: p[0],
        )
        if pts:
            divisor, unit = best_unit([p[1] for p in pts])
            ax_granularity.plot(
                [p[0] for p in pts],
                [p[1] / divisor for p in pts],
                marker="s",
                markersize=7,
                color=TAB10[4],
                linewidth=2.5,
            )
            style_axes(ax_granularity, "B. Task-Granularity Sweep", "Directories / tasks", f"Time ({unit})")
            style_time_axis(ax_granularity)

    flat_times = [to_ns(b) for b in grouped.get("BM_BuildTree_Flat", [])]
    nested_times = [to_ns(b) for b in grouped.get("BM_BuildTree_Nested", [])]
    if flat_times or nested_times:
        labels, vals, colors = [], [], []
        if flat_times:
            labels.append("Flat\n(all tasks at start)")
            vals.append(float(np.median(flat_times)))
            colors.append(TAB10[0])
        if nested_times:
            labels.append("Nested\n(tasks emerge in waves)")
            vals.append(float(np.median(nested_times)))
            colors.append(TAB10[1])

        divisor, unit = best_unit(vals)
        norm_vals = [v / divisor for v in vals]
        x = np.arange(len(labels))
        bars = ax_topology.bar(x, norm_vals, color=colors, width=0.5, edgecolor="white", zorder=3)
        ax_topology.set_xticks(x)
        ax_topology.set_xticklabels(labels)
        for bar, v in zip(bars, norm_vals):
            ax_topology.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() * 1.02,
                f"{v:.3f}",
                ha="center",
                va="bottom",
                fontsize=10,
            )
        style_axes(ax_topology, "C. Static vs Dynamic Task Generation", "Tree topology", f"Median time ({unit})")
        style_time_axis(ax_topology, 3)

    balanced_times = [to_ns(b) for b in grouped.get("BM_BuildTree_Balanced", [])]
    skewed_times = [to_ns(b) for b in grouped.get("BM_BuildTree_Skewed", [])]
    if balanced_times or skewed_times:
        labels, vals, colors = [], [], []
        if balanced_times:
            labels.append("Balanced\n(100 dirs × 100 files)")
            vals.append(float(np.median(balanced_times)))
            colors.append(TAB10[0])
        if skewed_times:
            labels.append("Skewed\n(1 dir × 800 files\n+ 100 dirs × 2 files)")
            vals.append(float(np.median(skewed_times)))
            colors.append(TAB10[5])

        divisor, unit = best_unit(vals)
        norm_vals = [v / divisor for v in vals]
        x = np.arange(len(labels))
        bars = ax_skew.bar(x, norm_vals, color=colors, width=0.5, edgecolor="white", zorder=3)
        ax_skew.set_xticks(x)
        ax_skew.set_xticklabels(labels)
        for bar, v in zip(bars, norm_vals):
            ax_skew.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() * 1.02,
                f"{v:.3f}",
                ha="center",
                va="bottom",
                fontsize=10,
            )
        style_axes(ax_skew, "D. Balanced vs Skewed Workload", "Workload distribution", f"Median time ({unit})")
        style_time_axis(ax_skew, 3)

    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out / "builder_scheduler.png", dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved  {out / 'builder_scheduler.png'}")


def plot_thread_scaling(grouped: dict[str, list[dict]], out: Path) -> None:
    if "BM_BuildTree_ThreadScaling" not in grouped:
        return

    pts = sorted(
        (
            (parse_name(b["name"])[1][0], to_ns(b), safe_items_per_second(b))
            for b in grouped["BM_BuildTree_ThreadScaling"]
        ),
        key=lambda p: p[0],
    )
    if not pts:
        return

    threads = [p[0] for p in pts]
    times_ns = [p[1] for p in pts]
    ips = [p[2] for p in pts]

    divisor, unit = best_unit(times_ns)

    fig, (ax_time, ax_ips) = plt.subplots(1, 2, figsize=(12.5, 5.5))
    fig.suptitle("Builder — Thread-Count Scaling", fontsize=16, fontweight="bold", y=0.98)

    ax_time.plot(threads, [t / divisor for t in times_ns], marker="o", markersize=7, color=TAB10[2], linewidth=2.5)
    style_axes(ax_time, "Scan Time vs Thread Count", "Threads", f"Time ({unit})")
    style_time_axis(ax_time, 3)

    ips_m = [i / 1e6 for i in ips]
    ax_ips.plot(threads, ips_m, marker="s", markersize=7, color=TAB10[4], linewidth=2.5, label="measured")

    if ips and ips[0] > 0:
        ideal = [(threads[i] / threads[0]) * ips[0] / 1e6 for i in range(len(threads))]
        ax_ips.plot(threads, ideal, linestyle="--", color="#888888", linewidth=1.5, label="ideal linear")
        ax_ips.legend(framealpha=0.9)

    style_axes(ax_ips, "Throughput vs Thread Count", "Threads", "Files/s (millions)")
    style_count_axis(ax_ips, 4)

    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out / "thread_scaling.png", dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved  {out / 'thread_scaling.png'}")


def plot_deque_microbench(grouped: dict[str, list[dict]], out: Path) -> None:
    has_data = any(
        k in grouped for k in ("BM_Deque_PushPop_Fresh", "BM_Deque_PushPop_Steady", "BM_Deque_StealContention")
    )
    if not has_data:
        return

    fig, (ax_pp, ax_steal) = plt.subplots(1, 2, figsize=(12.5, 5.5))
    fig.suptitle("LockFreeDeque — Microbenchmarks", fontsize=16, fontweight="bold", y=0.98)

    plotted_any = False
    for key, label, color in (
        ("BM_Deque_PushPop_Fresh", "Fresh (includes alloc)", TAB10[0]),
        ("BM_Deque_PushPop_Steady", "Steady (pre-grown)", TAB10[2]),
    ):
        if key not in grouped:
            continue
        pts = sorted(
            ((parse_name(b["name"])[1][0], safe_items_per_second(b)) for b in grouped[key]),
            key=lambda p: p[0],
        )
        if not pts:
            continue

        ns = [p[0] for p in pts]
        vals = [p[1] / 1e6 for p in pts]
        ax_pp.plot(ns, vals, marker="o", markersize=7, color=color, linewidth=2.5, label=label)
        plotted_any = True

    if plotted_any:
        ax_pp.set_xscale("log", base=2)
        ax_pp.legend(framealpha=0.9)
        style_axes(ax_pp, "Owner push+pop throughput", "Batch size N", "Operations/s (millions)")
        style_count_axis(ax_pp, 2)

    if "BM_Deque_StealContention" in grouped:
        pts = sorted(
            ((parse_name(b["name"])[1][0], safe_items_per_second(b)) for b in grouped["BM_Deque_StealContention"]),
            key=lambda p: p[0],
        )
        thieves = [p[0] for p in pts]
        ips = [p[1] / 1e6 for p in pts]

        ax_steal.plot(thieves, ips, marker="o", markersize=7, color=TAB10[5], linewidth=2.5)
        if ips:
            ax_steal.axhline(ips[0], linestyle="--", color="#888888", linewidth=1.5, label="0-thief baseline")
            ax_steal.legend(framealpha=0.9)

        style_axes(ax_steal, "Owner throughput under steal contention", "Number of thieves", "Owner ops/s (millions)")
        style_count_axis(ax_steal, 4)

    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out / "deque_microbench.png", dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved  {out / 'deque_microbench.png'}")


def plot_false_sharing(grouped: dict[str, list[dict]], out: Path) -> None:
    packed = grouped.get("BM_FalseSharing_Packed", [])
    padded = grouped.get("BM_FalseSharing_Padded", [])
    if not packed and not padded:
        return

    labels, vals, colors = [], [], []
    if packed:
        labels.append("Packed")
        vals.append(median_ns(packed))
        colors.append(TAB10[3])
    if padded:
        labels.append("Padded")
        vals.append(median_ns(padded))
        colors.append(TAB10[2])

    divisor, unit = best_unit(vals)
    norm_vals = [v / divisor for v in vals]

    fig, ax = plt.subplots(figsize=(7.5, 5.5))
    x = np.arange(len(labels))
    bars = ax.bar(x, norm_vals, color=colors, width=0.55, edgecolor="white", zorder=3)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    for bar, v in zip(bars, norm_vals):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() * 1.02,
            f"{v:.3f}",
            ha="center",
            va="bottom",
            fontsize=10,
        )

    style_axes(ax, "False Sharing Benchmark", "Counter layout", f"Median time ({unit})")
    style_time_axis(ax, 3)
    save_fig(fig, out / "false_sharing.png")


def plot_overview(grouped: dict[str, list[dict]], out: Path) -> None:
    order = [
        ("FileStats", "BM_FileStats", "Analyzer"),
        ("DirMetrics", "BM_DirectoryMetrics", "Analyzer"),
        ("EmptyDirs", "BM_EmptyDirs", "Analyzer"),
        ("ExtStats", "BM_ExtensionStats", "Analyzer"),
        ("RecentFiles", "BM_RecentFiles", "Analyzer"),
        ("Summary", "BM_Summary", "Analyzer"),
        ("DirStats", "BM_DirectoryStats", "Analyzer"),
        ("LargestNFiles", "BM_LargestNFiles", "Analyzer"),
        ("LargestNDirs", "BM_LargestNDirs", "Analyzer"),
        ("BuildOverhead", "BM_BuildTree_ThreadOverhead", "Builder"),
        ("BuildGranularity", "BM_BuildTree_Granularity", "Builder"),
        ("BuildFlat", "BM_BuildTree_Flat", "Builder"),
        ("BuildNested", "BM_BuildTree_Nested", "Builder"),
        ("BuildBalanced", "BM_BuildTree_Balanced", "Builder"),
        ("BuildSkewed", "BM_BuildTree_Skewed", "Builder"),
        ("ThreadScaling", "BM_BuildTree_ThreadScaling", "Builder"),
        ("DequeFresh", "BM_Deque_PushPop_Fresh", "Deque"),
        ("DequeSteady", "BM_Deque_PushPop_Steady", "Deque"),
        ("DequeSteal", "BM_Deque_StealContention", "Deque"),
        ("FalsePacked", "BM_FalseSharing_Packed", "Deque"),
        ("FalsePadded", "BM_FalseSharing_Padded", "Deque"),
    ]

    component_color = {
        "Analyzer": TAB10[0],
        "Builder": TAB10[2],
        "Deque": TAB10[6],
    }

    labels, medians, colors = [], [], []
    for label, key, family in order:
        if key not in grouped:
            continue
        m = median_ns(grouped[key])
        if m <= 0:
            continue
        labels.append(label)
        medians.append(m / 1e6)  # ms
        colors.append(component_color[family])

    if not labels:
        return

    fig, ax = plt.subplots(figsize=(14, 6))
    x = np.arange(len(labels))
    ax.bar(x, medians, color=colors, width=0.72, edgecolor="white", zorder=3)

    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=35, ha="right")
    ax.set_yscale("log")
    ax.set_ylabel("Median time (ms, log scale)")
    ax.set_title("Phanes — Benchmark Overview", pad=12)
    ax.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.22, zorder=0, which="both")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    import matplotlib.patches as mpatches
    ax.legend(
        handles=[
            mpatches.Patch(color=TAB10[0], label="Analyzer"),
            mpatches.Patch(color=TAB10[2], label="Builder"),
            mpatches.Patch(color=TAB10[6], label="Deque / False Sharing"),
        ],
        framealpha=0.9,
        loc="upper left",
    )

    save_fig(fig, out / "overview.png")


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot phanes Google Benchmark JSON results.")
    parser.add_argument("results", help="Path to benchmark JSON (--benchmark_out)")
    parser.add_argument("--out", default="benchmark_plots", help="Output directory")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print(f"Loading {args.results} …")
    benchmarks = load_benchmarks(args.results)
    print(f"  {len(benchmarks)} benchmark entries loaded")

    grouped: dict[str, list[dict]] = defaultdict(list)
    for b in benchmarks:
        name, _ = parse_name(b["name"])
        grouped[name].append(b)

    print("\nGenerating charts …")
    plot_overview(grouped, out)
    plot_analyzer_scalability(grouped, out)
    plot_topn_performance(grouped, out)
    plot_builder_scheduler(grouped, out)
    plot_thread_scaling(grouped, out)
    plot_deque_microbench(grouped, out)
    plot_false_sharing(grouped, out)

    print(f"\nDone. Open {out}/ to view the charts.")


if __name__ == "__main__":
    main()