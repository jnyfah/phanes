#!/usr/bin/env python3
"""
Plot Phanes benchmark results.

Usage:
    python3 benchmark/plot_results.py results.json
    python3 benchmark/plot_results.py results.json --out benchmark_plots
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path
import sys

try:
    import matplotlib.patches as mpatches
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import numpy as np
except ImportError:
    sys.exit("Error: install matplotlib and numpy first:  pip install matplotlib numpy")


# ---------------------------------------------------------------------------
# Global style
# ---------------------------------------------------------------------------

plt.rcParams.update({
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
})

TAB10 = plt.cm.tab10.colors  # type: ignore[attr-defined]
DPI = 180


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

def load_benchmarks(path: str) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    raw = [b for b in data["benchmarks"] if b.get("run_type") == "iteration"]
    return raw if raw else [b for b in data["benchmarks"] if b["name"].endswith("_mean")]


def parse_name(name: str) -> tuple[str, list[int]]:
    for sfx in ("_mean", "_median", "_stddev", "_cv"):
        if name.endswith(sfx):
            name = name[: -len(sfx)]
            break
    parts = name.split("/")
    return parts[0], [int(p) for p in parts[1:] if p.isdigit()]


def to_ns(b: dict) -> float:
    t = float(b["real_time"])
    unit = b.get("time_unit", "ns")
    return t * {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}.get(unit, 1.0)


def items_per_second(b: dict) -> float:
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


def collect_pts(grouped: dict, key: str, y_fn=to_ns) -> list[tuple[int, float]]:
    """Return (x_arg, y_value) pairs sorted by x for a benchmark group."""
    return sorted(
        ((parse_name(b["name"])[1][0], y_fn(b)) for b in grouped.get(key, [])),
        key=lambda p: p[0],
    )


# ---------------------------------------------------------------------------
# Style helpers
# ---------------------------------------------------------------------------

def _human_fmt(decimals: int = 2):
    def _fmt(x, _pos=None):
        if abs(x) >= 100:
            return f"{x:.0f}"
        if abs(x) >= 10:
            return f"{x:.1f}"
        return f"{x:.{decimals}f}"
    return ticker.FuncFormatter(_fmt)


def style_axes(ax: plt.Axes, title: str, xlabel: str, ylabel: str) -> None:
    ax.set_title(title, pad=12)
    ax.set_xlabel(xlabel, labelpad=8)
    ax.set_ylabel(ylabel, labelpad=8)
    ax.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.22, zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def style_time_axis(ax: plt.Axes, decimals: int = 2) -> None:
    ax.ticklabel_format(axis="y", style="plain", useOffset=False)
    ax.yaxis.get_major_formatter().set_useOffset(False)
    ax.yaxis.get_major_formatter().set_scientific(False)
    ax.yaxis.set_major_formatter(_human_fmt(decimals))


def style_count_axis(ax: plt.Axes, decimals: int = 2) -> None:
    ax.yaxis.set_major_formatter(_human_fmt(decimals))


def save_fig(fig: plt.Figure, dest: Path, *, rect=(0, 0, 1, 1)) -> None:
    fig.tight_layout(rect=rect)
    fig.savefig(dest, dpi=DPI, bbox_inches="tight")
    plt.close(fig)
    print(f"  saved  {dest}")


# ---------------------------------------------------------------------------
# Plot generators
# ---------------------------------------------------------------------------

def plot_thread_scaling(grouped: dict, out: Path) -> None:
    pts = collect_pts(grouped, "BM_BuildTree_ThreadScaling")
    ips_pts = collect_pts(grouped, "BM_BuildTree_ThreadScaling", y_fn=items_per_second)
    if not pts:
        return

    threads = [p[0] for p in pts]
    times_ns = [p[1] for p in pts]
    ips = [p[1] for p in ips_pts]
    divisor, unit = best_unit(times_ns)

    fig, (ax_time, ax_ips) = plt.subplots(1, 2, figsize=(12.5, 5.5))
    fig.suptitle("Builder — Thread-Count Scaling", fontsize=16, fontweight="bold", y=0.98)

    ax_time.plot(threads, [t / divisor for t in times_ns], marker="o", markersize=7,
                 color=TAB10[2], linewidth=2.5)
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

    save_fig(fig, out / "thread_scaling.png", rect=(0, 0, 1, 0.96))


def plot_deque_microbench(grouped: dict, out: Path) -> None:
    has_data = any(k in grouped for k in (
        "BM_Deque_PushPop_Fresh", "BM_Deque_PushPop_Steady", "BM_Deque_StealContention",
    ))
    if not has_data:
        return

    fig, (ax_pp, ax_steal) = plt.subplots(1, 2, figsize=(12.5, 5.5))
    fig.suptitle("LockFreeDeque — Microbenchmarks", fontsize=16, fontweight="bold", y=0.98)

    plotted_any = False
    for key, label, color in (
        ("BM_Deque_PushPop_Fresh",  "Fresh (includes alloc)", TAB10[0]),
        ("BM_Deque_PushPop_Steady", "Steady (pre-grown)",     TAB10[2]),
    ):
        pts = collect_pts(grouped, key, y_fn=items_per_second)
        if not pts:
            continue
        ax_pp.plot([p[0] for p in pts], [p[1] / 1e6 for p in pts],
                   marker="o", markersize=7, color=color, linewidth=2.5, label=label)
        plotted_any = True

    if plotted_any:
        ax_pp.set_xscale("log", base=2)
        ax_pp.legend(framealpha=0.9)
        style_axes(ax_pp, "Owner push+pop throughput", "Batch size N", "Operations/s (millions)")
        style_count_axis(ax_pp, 2)

    pts = collect_pts(grouped, "BM_Deque_StealContention", y_fn=items_per_second)
    if pts:
        thieves = [p[0] for p in pts]
        ips = [p[1] / 1e6 for p in pts]
        ax_steal.plot(thieves, ips, marker="o", markersize=7, color=TAB10[5], linewidth=2.5)
        ax_steal.axhline(ips[0], linestyle="--", color="#888888", linewidth=1.5, label="0-thief baseline")
        ax_steal.legend(framealpha=0.9)
        style_axes(ax_steal, "Owner throughput under steal contention", "Number of thieves", "Owner ops/s (millions)")
        style_count_axis(ax_steal, 4)

    save_fig(fig, out / "deque_microbench.png", rect=(0, 0, 1, 0.96))


def plot_overview(grouped: dict, out: Path) -> None:
    order = [
        ("FileStats",     "BM_FileStats",               "Analyzer"),
        ("DirMetrics",    "BM_DirectoryMetrics",        "Analyzer"),
        ("EmptyDirs",     "BM_EmptyDirs",               "Analyzer"),
        ("ExtStats",      "BM_ExtensionStats",          "Analyzer"),
        ("RecentFiles",   "BM_RecentFiles",             "Analyzer"),
        ("Summary",       "BM_Summary",                 "Analyzer"),
        ("DirStats",      "BM_DirectoryStats",          "Analyzer"),
        ("LargestNFiles", "BM_LargestNFiles",           "Analyzer"),
        ("LargestNDirs",  "BM_LargestNDirs",            "Analyzer"),
        ("BuildOverhead", "BM_BuildTree_ThreadOverhead", "Builder"),
        ("Granularity",   "BM_BuildTree_Granularity",   "Builder"),
        ("BuildFlat",     "BM_BuildTree_Flat",          "Builder"),
        ("BuildNested",   "BM_BuildTree_Nested",        "Builder"),
        ("BuildBalanced", "BM_BuildTree_Balanced",      "Builder"),
        ("BuildSkewed",   "BM_BuildTree_Skewed",        "Builder"),
        ("ThreadScaling", "BM_BuildTree_ThreadScaling", "Builder"),
        ("DequeFresh",    "BM_Deque_PushPop_Fresh",    "Deque"),
        ("DequeSteady",   "BM_Deque_PushPop_Steady",  "Deque"),
        ("DequeSteal",    "BM_Deque_StealContention",  "Deque"),
        ("FalsePacked",   "BM_FalseSharing_Packed",    "Deque"),
        ("FalsePadded",   "BM_FalseSharing_Padded",    "Deque"),
    ]
    component_color = {"Analyzer": TAB10[0], "Builder": TAB10[2], "Deque": TAB10[6]}

    labels, medians, colors = [], [], []
    for label, key, family in order:
        m = median_ns(grouped.get(key, []))
        if m > 0:
            labels.append(label)
            medians.append(m / 1e6)
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
    ax.legend(handles=[
        mpatches.Patch(color=TAB10[0], label="Analyzer"),
        mpatches.Patch(color=TAB10[2], label="Builder"),
        mpatches.Patch(color=TAB10[6], label="Deque / False Sharing"),
    ], framealpha=0.9, loc="upper left")
    save_fig(fig, out / "overview.png")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Plot Phanes benchmark results.")
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
        grouped[parse_name(b["name"])[0]].append(b)

    print("\nGenerating charts …")
    plot_overview(grouped, out)
    plot_thread_scaling(grouped, out)
    plot_deque_microbench(grouped, out)
    print(f"\nDone. Open {out}/ to view the charts.")


if __name__ == "__main__":
    main()
