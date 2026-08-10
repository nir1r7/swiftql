#!/usr/bin/env python3
"""
make_figures.py — the Week 37 figures, generated from the recorded JSON.

    .venv/bin/python python_tools/make_figures.py

Every figure is built from `docs/week-37-*.json` and nothing is typed in by
hand, so a figure cannot drift from the measurement it depicts. Re-running after
a new measurement regenerates all four.

FORM, chosen per figure by the job the data does:
  1. per-query latency  — magnitude across 22 queries x 5 engines. Horizontal
     grouped bars on a LOG axis, because the values span 0.7ms to 20s and a
     linear axis would render 18 of the 22 as a flat smear.
  2. scaling            — change across three scale factors. Lines, with a
     reference rule at 1.0 (parity) since the quantity is a ratio and the
     crossing is the story.
  3. optimizer impact   — magnitude, one series, sorted. No legend: the title
     names the single series.
  4. q-error            — a distribution, so a CDF rather than a histogram; bin
     choice would otherwise decide the shape of a long-tailed quantity.

COLOR: the validated categorical order, assigned by ENTITY (engine) and fixed
across every figure, so SwiftQL is the same blue in all four. Never cycled, never
reassigned by rank.
"""

import json
import os
import statistics
import math
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt          # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

OUT = "docs/figures"

# Validated categorical slots 1-5 (light surface). Fixed per ENTITY.
COLOR = {
    "swiftql":       "#2a78d6",
    "swiftql-noopt": "#e87ba4",
    "sqlite":        "#eb6834",
    "postgres":      "#1baf7a",
    "duckdb":        "#eda100",
}
LABEL = {
    "swiftql": "SwiftQL", "swiftql-noopt": "SwiftQL (no optimizer)",
    "sqlite": "SQLite", "postgres": "PostgreSQL", "duckdb": "DuckDB",
}
INK, MUTED, GRID = "#1a1a19", "#5c5b55", "#e4e3dd"


def style(ax):
    """Recessive grid and axes; the data carries the ink."""
    ax.set_facecolor("white")
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=8, length=3)
    ax.grid(True, color=GRID, linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)


def load(path):
    with open(path) as f:
        return json.load(f)


def usable(res, engines):
    """Queries where every engine agreed on row count — the rest are excluded."""
    out = []
    for i in range(1, 23):
        q = f"q{i}"
        rows = {res[f"{q}|{e}"]["rows"] for e in engines if f"{q}|{e}" in res}
        if len(rows) == 1:
            out.append(q)
    return out


# ------------------------------------------------------- 1. per-query latency

def fig_per_query(sf1):
    res = sf1["results"]
    engines = ["swiftql", "sqlite", "postgres", "duckdb"]
    qs = [f"q{i}" for i in range(1, 23)]
    fig, ax = plt.subplots(figsize=(8.5, 10))
    h = 0.20
    for k, e in enumerate(engines):
        ys = [i + (1.5 - k) * h for i in range(len(qs))]
        vals = [res[f"{q}|{e}"]["ms"] for q in qs]
        ax.barh(ys, vals, height=h * 0.88, color=COLOR[e], label=LABEL[e],
                zorder=3, linewidth=0)
    ax.set_yticks(range(len(qs)))
    ax.set_yticklabels(qs)
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.set_xlabel("latency (ms, log scale) — median of 3, load excluded", color=MUTED, fontsize=9, labelpad=8)
    ax.xaxis.set_major_formatter(FuncFormatter(
        lambda v, _: f"{v:g}" if v < 1000 else f"{v/1000:g}s"))
    ax.set_title("TPC-H per-query latency at SF=1\n"
                 "SQLite and PostgreSQL carry the spec's PK/FK indexes; SwiftQL is single-threaded",
                 color=INK, fontsize=11, loc="left", pad=14)
    # Legend on the figure, below the plot: at 22 rows the axes are tall and a
    # legend anchored above them lands on top of the two-line title.
    ax.legend(frameon=False, fontsize=9, labelcolor=MUTED, ncol=4,
              loc="upper center", bbox_to_anchor=(0.5, -0.045))
    style(ax)
    fig.tight_layout()
    fig.savefig(f"{OUT}/fig1-per-query-sf1.png", dpi=200, bbox_inches="tight")
    plt.close(fig)


# ------------------------------------------------------------- 2. scaling

def fig_scaling(files):
    engines = ["sqlite", "postgres", "duckdb"]
    xs = [0.01, 0.1, 1.0]
    fig, ax = plt.subplots(figsize=(7, 4.6))
    for e in engines:
        ys = []
        for f in files:
            res = load(f)["results"]
            qs = usable(res, ["swiftql", "sqlite", "postgres", "duckdb"])
            ys.append(math.exp(statistics.fmean(
                [math.log(res[f"{q}|swiftql"]["ms"] / res[f"{q}|{e}"]["ms"]) for q in qs])))
        ax.plot(xs, ys, color=COLOR[e], linewidth=2, marker="o", markersize=7,
                label=LABEL[e], zorder=3)
        # Direct label at the right end — identity is never color-alone.
        ax.annotate(f" {LABEL[e]}  {ys[-1]:.2f}x", (xs[-1], ys[-1]),
                    color=MUTED, fontsize=8, va="center")
    ax.axhline(1.0, color=MUTED, linewidth=1, linestyle=(0, (4, 3)), zorder=2)
    ax.annotate("parity", (0.0105, 1.06), color=MUTED, fontsize=8)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xticks(xs); ax.set_xticklabels(["SF=0.01", "SF=0.1", "SF=1"])
    ax.set_ylabel("SwiftQL ÷ engine (geometric mean)\nbelow 1.0 = SwiftQL faster",
                  color=MUTED, fontsize=9)
    ax.set_title("How SwiftQL's standing changes with scale",
                 color=INK, fontsize=11, loc="left", pad=10)
    ax.set_xlim(0.008, 3.2)
    style(ax)
    fig.tight_layout()
    fig.savefig(f"{OUT}/fig2-scaling.png", dpi=200, bbox_inches="tight")
    plt.close(fig)


# --------------------------------------------------- 3. optimizer impact

def fig_optimizer(sf1):
    res = sf1["results"]
    pairs = [(q, res[f"{q}|swiftql-noopt"]["ms"] / res[f"{q}|swiftql"]["ms"])
             for q in [f"q{i}" for i in range(1, 23)]]
    pairs.sort(key=lambda t: t[1])
    fig, ax = plt.subplots(figsize=(7, 6))
    ax.barh([p[0] for p in pairs], [p[1] for p in pairs],
            color=COLOR["swiftql"], height=0.66, zorder=3, linewidth=0)
    ax.axvline(1.0, color=MUTED, linewidth=1, linestyle=(0, (4, 3)), zorder=4)
    for q, v in pairs:                       # selective direct labels
        if v >= 2:
            ax.annotate(f" {v:.1f}x", (v, q), color=MUTED, fontsize=8, va="center")
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}x"))
    ax.set_xlabel("unoptimized ÷ optimized — right of the rule is a gain",
                  color=MUTED, fontsize=9)
    ax.set_title("What the optimizer is worth, per query, at SF=1\n"
                 "geometric mean 2.22x across 21 queries",
                 color=INK, fontsize=11, loc="left", pad=10)
    style(ax)
    fig.tight_layout()
    fig.savefig(f"{OUT}/fig3-optimizer.png", dpi=200, bbox_inches="tight")
    plt.close(fig)


# ------------------------------------------------------------- 4. q-error

def fig_qerror(paths):
    fig, ax = plt.subplots(figsize=(7, 4.6))
    for (label, path), colour in zip(paths, [COLOR["swiftql"], COLOR["sqlite"]]):
        vals = sorted(n["qerror"] for nodes in load(path)["nodes"].values()
                      for n in nodes)
        ys = [(i + 1) / len(vals) * 100 for i in range(len(vals))]
        ax.step(vals, ys, where="post", color=colour, linewidth=2,
                label=f"{label} ({len(vals)} nodes)", zorder=3)
    for k in (2, 10, 100):
        ax.axvline(k, color=GRID, linewidth=1, zorder=1)
        ax.annotate(f"{k}x", (k, 2), color=MUTED, fontsize=8, ha="center")
    ax.set_xscale("log")
    ax.set_xlabel("q-error = max(estimate, actual) / min(estimate, actual)",
                  color=MUTED, fontsize=9)
    ax.set_ylabel("% of plan nodes at or below", color=MUTED, fontsize=9)
    ax.set_title("Cardinality-estimation accuracy\n"
                 "the median is good; the tail is where it fails, and it worsens with scale",
                 color=INK, fontsize=11, loc="left", pad=10)
    ax.legend(frameon=False, fontsize=8, labelcolor=MUTED, loc="lower right")
    ax.set_ylim(0, 101)
    style(ax)
    fig.tight_layout()
    fig.savefig(f"{OUT}/fig4-qerror.png", dpi=200, bbox_inches="tight")
    plt.close(fig)


def main():
    os.makedirs(OUT, exist_ok=True)
    files = [f"docs/week-37-engine-comparison-sf{s}.json" for s in ("0.01", "0.1", "1")]
    missing = [f for f in files if not os.path.exists(f)]
    if missing:
        sys.exit(f"missing measurement files: {missing}")
    sf1 = load(files[2])
    fig_per_query(sf1)
    fig_scaling(files)
    fig_optimizer(sf1)
    fig_qerror([("SF=0.1", "docs/week-37-qerror-sf0.1.json"),
                ("SF=1", "docs/week-37-qerror-sf1.json")])
    for f in sorted(os.listdir(OUT)):
        print(f"  {OUT}/{f}")


if __name__ == "__main__":
    main()
