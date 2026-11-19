#!/usr/bin/env python3
"""Graphs for docs/RESULTS.md, drawn from the real logs in docs/results/.

    python3 host/plot_scenarios.py docs/results --all        # every figure below
    python3 host/plot_scenarios.py docs/results --pump --rep r1 --out docs/results/pump-timeline-r1.png
    python3 host/plot_scenarios.py docs/results --summary     --out docs/results/pump-summary.png
    python3 host/plot_scenarios.py docs/results --local --rep r1 --out docs/results/local-timeline-r1.png

Nothing is smoothed or interpolated:
- "Missed deadlines per 100 ms" counts, for each 100 ms of the run, the
  critical loop's cycles whose deadline was missed (CSV rows MISS, or CYCLE
  with lateness_us > 0). Each cycle is placed at its own deadline slot
  (register time + cycle x period), so a bin can never exceed 10 misses for
  a 10 ms loop, even when the supervisor writes rows late after a stall.
- "Dose error" is printed once a second by the pump itself (*-pump.out;
  pump time + 1 s = supervisor time, see scripts/demo.sh). Where the pump's
  normal-priority status printer got no CPU there are no readings, and the
  line is left open rather than joined.
- Grey hatching marks emulator stalls: gaps of >= STALL_MS between rows of
  the prio-30 supervisor, which logs every 10 ms. During such a gap the
  whole QEMU guest was frozen.
- The summary bars are the mean of the 3 runs; whiskers span min..max.
  They use the same functions as host/results_table.py (RESULTS.md).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render import parse_log, critical_task  # noqa: E402
from results_table import ATTACK, task_stats, pump_dose, critical  # noqa: E402

STALL_MS = 100.0
RED, GREEN, AMBER, BLUE, GREY = "#C62828", "#2E7D32", "#E69500", "#1565C0", "#757575"

PUMP = [  # (key, short label, long label, colour)
    ("A-guard-off", "A  Guard off", "A: guard off", RED),
    ("B-guard-on", "B  Guard on, greedy program registered", "B: guard on, greedy program registered", GREEN),
    ("C-safe-state", "C  Guard on, nothing to demote", "C: guard on, nothing to demote -> safe state", AMBER),
    ("Cpid-demote-pid", "C\u2032  Guard on, --demote-pid", "C\u2032: guard on, greedy program named by --demote-pid", BLUE),
]


def style(plt):
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 10.5,
        "axes.titlesize": 11.5, "axes.titleweight": "bold", "axes.titlelocation": "left",
        "axes.labelsize": 10.5, "axes.spines.top": False, "axes.spines.right": False,
        "axes.edgecolor": "#444444", "axes.grid": True, "grid.color": "#E6E6E6",
        "grid.linewidth": 0.8, "axes.axisbelow": True, "figure.facecolor": "white",
        "axes.facecolor": "white", "savefig.facecolor": "white", "legend.frameon": False,
    })


def period_and_start(rows, task):
    for r in rows:
        if r["thread"] == task and r["event"] == "REGISTER":
            m = re.search(r"period=(\d+)ms", r["detail"])
            return (float(m.group(1)) if m else 10.0), r["t_ms"]
    first = next(r for r in rows if r["thread"] == task and r["event"] in ("CYCLE", "MISS"))
    return 10.0, first["t_ms"]


def misses_per_bin(rows, task, end_ms, bin_ms=100.0):
    period, t0 = period_and_start(rows, task)
    nb = int(end_ms // bin_ms) + 1
    bins = [0] * nb
    for r in rows:
        if r["thread"] != task or r["event"] not in ("CYCLE", "MISS"):
            continue
        if r["event"] == "MISS" or r["lateness_us"] > 0:
            b = int((t0 + (r["cycle"] + 1) * period) // bin_ms)
            if 0 <= b < nb:
                bins[b] += 1
    return [i * bin_ms / 1000.0 for i in range(nb)], bins


def stalls(rows):
    """Gaps >= STALL_MS between supervisor-written rows while the run is live."""
    i0 = next((i for i, r in enumerate(rows) if r["event"] == "CYCLE"), 0)
    ts = [r["t_ms"] for r in rows[i0:]]   # from the first judged cycle on
    end = next((r["t_ms"] for r in rows if r["event"] == "END"), ts[-1] if ts else 0)
    out = []
    for a, b in zip(ts, ts[1:]):
        if b > end:
            break
        if b - a >= STALL_MS:
            out.append((a / 1000.0, b / 1000.0))
    return out


def actions(rows):
    return [(r["t_ms"] / 1000.0, r["event"]) for r in rows
            if r["event"] in ("THROTTLE", "ESCALATE", "SAFE_STATE")]


def label_actions(ax, acts):
    """Dashed line per guard action; first of each kind labelled, labels stacked if close."""
    seen, last_t, y = set(), None, 9.6
    for t, ev in acts:
        ax.axvline(t, color="#222222", lw=1.0, ls=(0, (3, 2)))
        if ev in seen:
            continue
        seen.add(ev)
        y = y - 3.2 if last_t is not None and t - last_t < 2.0 else 9.6
        ax.text(t + 0.15, y, ev.replace("_", " "), fontsize=8.5, va="top", color="#222222")
        last_t = t


def dose_segments(path):
    """Pump console -> list of (xs, ys) runs with no gap > 1.5 s."""
    segs, xs, ys = [], [], []
    for line in open(path, errors="replace"):
        m = re.search(r"\[pump\s+([\d.]+)s\].*dose error\s+([+-]?\d+\.\d+)%", line)
        if not m:
            continue
        x = float(m.group(1)) + 1.0
        if xs and x - xs[-1] > 1.5:
            segs.append((xs, ys))
            xs, ys = [], []
        xs.append(x)
        ys.append(float(m.group(2)))
    if xs:
        segs.append((xs, ys))
    return segs


def shade(ax, stall_list, attack=True):
    if attack:
        ax.axvspan(ATTACK[0] / 1000, ATTACK[1] / 1000, color="#F3F3F3", zorder=0, lw=0)
    for a, b in stall_list:
        ax.axvspan(a, max(b, a + 0.08), facecolor="none", edgecolor="#9E9E9E",
                   hatch="////", lw=0, zorder=0.5)


def fig_pump(d, rep, out, plt):
    from matplotlib.patches import Patch
    from matplotlib.lines import Line2D
    fig = plt.figure(figsize=(12, 10.5))
    gs = fig.add_gridspec(5, 1, height_ratios=[1, 1, 1, 1, 2.9], hspace=0.6)
    end = 31.0
    dose_ax = fig.add_subplot(gs[4])
    first = None
    for i, (key, short, long_, col) in enumerate(PUMP):
        base = os.path.join(d, f"pump-{key}-{rep}")
        rows = parse_log(base + ".csv")
        task = critical(rows)
        ax = fig.add_subplot(gs[i], sharex=first) if first else fig.add_subplot(gs[i])
        first = first or ax
        st = stalls(rows)
        shade(ax, st)
        xs, ys = misses_per_bin(rows, task, end * 1000)
        ax.fill_between(xs, ys, step="post", color=col, alpha=0.85, lw=0)
        ax.set_ylim(0, 10.5)
        ax.set_yticks([0, 5, 10])
        ax.set_xlim(0, end)
        aj, am, _ = task_stats(rows, task, ATTACK)
        ax.set_title(f"{short}", color=col, pad=3)
        ax.text(1.0, 1.04, f"{100.0 * am / aj:.1f} % of deadlines missed while the greedy program ran",
                transform=ax.transAxes, ha="right", va="bottom", fontsize=10, color="#333333")
        label_actions(ax, actions(rows))
        ax.tick_params(labelbottom=False)
        if i == 0:
            ax.text(14.0, 12.6, "greedy program running (\u2248 4\u201324 s)", ha="center",
                    fontsize=9, color="#666666")
        for segs in dose_segments(base + "-pump.out"):
            dose_ax.plot(segs[0], segs[1], "-o", color=col, ms=3.5, lw=2.0)
    fig.text(0.075, 0.70, "Missed deadlines per 100 ms  (10 ms dose loop, max 10)",
             rotation=90, va="center", ha="center", fontsize=10.5)
    shade(dose_ax, [])
    dose_ax.axhline(0, color="#222222", lw=0.8)
    dose_ax.set_xlim(0, end)
    dose_ax.set_ylim(-100, 10)
    dose_ax.set_yticks(range(-100, 11, 20))
    dose_ax.set_xlabel("Time since the run started (s)")
    dose_ax.set_ylabel("Dose error (%)\n0 = delivered exactly as prescribed")
    dose_ax.set_title("Dose delivered vs prescribed (%)", pad=6)
    handles = [Line2D([], [], color=c, lw=2.2, marker="o", ms=4, label=l) for _, _, l, c in PUMP]
    handles += [Patch(facecolor="#F3F3F3", label="greedy program running"),
                Patch(facecolor="white", edgecolor="#9E9E9E", hatch="////", label="emulator stall (whole guest paused)")]
    dose_ax.legend(handles=handles, loc="lower left", fontsize=9.2, ncol=2)
    dose_ax.text(0.99, 0.47, "No dots = no reading: the pump's low-priority\nstatus printer got no CPU while it was starved.\n"
                 "C's motor was stopped on purpose (alarm raised).",
                 transform=dose_ax.transAxes, ha="right", va="bottom", fontsize=8.8, color="#555555")
    fig.suptitle("Simulated infusion pump on QNX 8.0: a greedy program vs. DeadlineWatch",
                 x=0.075, ha="left", fontsize=15, fontweight="bold", y=0.975)
    fig.text(0.075, 0.935, f"Run {rep} of 3  \u00b7  QNX 8.0 in QEMU (software emulation)  \u00b7  "
             f"data: docs/results/pump-*-{rep}.csv and *-pump.out  \u00b7  dashed line = guard action",
             fontsize=9.5, color="#555555")
    fig.subplots_adjust(left=0.12, right=0.98, top=0.89, bottom=0.06)
    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out)


def fig_summary(d, out, plt):
    reps = ["r1", "r2", "r3"]
    miss, worst = [], []
    for key, *_ in PUMP:
        ms, ws = [], []
        for rep in reps:
            base = os.path.join(d, f"pump-{key}-{rep}")
            rows = parse_log(base + ".csv")
            aj, am, _ = task_stats(rows, critical(rows), ATTACK)
            ms.append(100.0 * am / aj)
            ws.append(pump_dose(base + "-pump.out")[0])
        miss.append(ms)
        worst.append(ws)
    labels = ["A  Guard off", "B  Guard on,\nprogram registered", "C  Guard on,\nnothing to demote", "C\u2032  Guard on,\n--demote-pid"]
    cols = [c for *_, c in PUMP]
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.8))
    y = list(range(len(PUMP)))[::-1]
    for ax, data, title, xlim, fmt in (
            (axes[0], miss, "Missed deadlines while the greedy program ran (%)", (0, 112), "{:.1f} %"),
            (axes[1], worst, "Worst dose error (%)", (-112, 0), "{:+.1f} %")):
        means = [sum(v) / len(v) for v in data]
        lo = [m - min(v) for m, v in zip(means, data)]
        hi = [max(v) - m for m, v in zip(means, data)]
        ax.barh(y, means, color=cols, height=0.6, xerr=[lo, hi],
                error_kw=dict(ecolor="#333333", capsize=4, lw=1.1))
        ax.set_yticks(y)
        ax.set_yticklabels(labels)
        ax.set_xlim(*xlim)
        ax.set_title(title, pad=8)
        ax.grid(axis="y", visible=False)
        ax.axvline(0, color="#222222", lw=0.8)
        for yi, m, v in zip(y, means, data):
            edge = max(v) if xlim[0] >= 0 else min(v)
            off = 2.5 if xlim[0] >= 0 else -2.5
            ax.text(edge + off, yi, fmt.format(m), va="center",
                    ha="left" if xlim[0] >= 0 else "right", fontsize=10.5, fontweight="bold")
    axes[1].set_yticklabels([])
    axes[1].text(-50, y[2] - 0.47, "motor stopped on purpose, alarm raised", fontsize=8.6,
                 color="#555555", ha="center", va="top")
    axes[1].text(-50, y[0] - 0.47, "silent under-dose, no alarm", fontsize=8.6,
                 color="#555555", ha="center", va="top")
    fig.suptitle("Infusion-pump scenarios on QNX 8.0: mean of 3 runs (whiskers = lowest to highest run)",
                 x=0.02, ha="left", fontsize=13.5, fontweight="bold")
    fig.text(0.02, 0.9, "QNX 8.0 in QEMU (software emulation)  \u00b7  same numbers as the table in docs/RESULTS.md",
             fontsize=9.5, color="#555555")
    fig.subplots_adjust(left=0.17, right=0.98, top=0.8, bottom=0.08, wspace=0.08)
    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out)


def fig_local(d, rep, out, plt):
    runs = [("Greedy thread off", "local-rogue-off-2", GREY),
            ("Greedy thread at 100 %, guard off", f"local-d100-guard-off-{rep}", RED),
            ("Greedy thread at 100 %, guard on", f"local-d100-guard-on-{rep}", GREEN)]
    fig, axes = plt.subplots(3, 1, figsize=(12, 6.6), sharex=True)
    for ax, (label, base, col) in zip(axes, runs):
        rows = parse_log(os.path.join(d, base + ".csv"))
        task = critical_task(rows)
        shade(ax, stalls(rows), attack=False)
        xs, ys = misses_per_bin(rows, task, 31000)
        ax.fill_between(xs, ys, step="post", color=col, alpha=0.85, lw=0)
        j, m, _ = task_stats(rows, task)
        ax.set_title(label, color=col, pad=3)
        ax.text(1.0, 1.04, f"{100.0 * m / j:.1f} % of deadlines missed", transform=ax.transAxes,
                ha="right", va="bottom", fontsize=10, color="#333333")
        label_actions(ax, actions(rows))
        ax.set_ylim(0, 10.5)
        ax.set_yticks([0, 5, 10])
        ax.set_xlim(0, 31)
    axes[1].set_ylabel("Missed deadlines per 100 ms  (10 ms control loop, max 10)")
    axes[-1].set_xlabel("Time since the run started (s)")
    fig.suptitle("Local mode on QNX 8.0: one process, greedy thread vs. control loop",
                 x=0.075, ha="left", fontsize=14, fontweight="bold")
    fig.text(0.075, 0.905, f"Run {base[-2:] if 'rogue-off' not in base else rep} of 3 (rogue-off: run 2)  \u00b7  "
             "QNX 8.0 in QEMU (software emulation)  \u00b7  dashed line = guard action; hatched = emulator stall",
             fontsize=9.5, color="#555555")
    fig.subplots_adjust(left=0.08, right=0.98, top=0.84, bottom=0.09, hspace=0.45)
    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--pump", action="store_true")
    g.add_argument("--summary", action="store_true")
    g.add_argument("--local", action="store_true")
    g.add_argument("--all", action="store_true")
    ap.add_argument("--rep", default="r1")
    ap.add_argument("--out")
    a = ap.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    style(plt)
    if a.all:
        for rep in ("r1", "r2", "r3"):
            fig_pump(a.dir, rep, os.path.join(a.dir, f"pump-timeline-{rep}.png"), plt)
        fig_summary(a.dir, os.path.join(a.dir, "pump-summary.png"), plt)
        fig_local(a.dir, "r1", os.path.join(a.dir, "local-timeline-r1.png"), plt)
        return
    if not a.out:
        ap.error("--out is required unless --all")
    if a.pump:
        fig_pump(a.dir, a.rep, a.out, plt)
    elif a.summary:
        fig_summary(a.dir, a.out, plt)
    else:
        fig_local(a.dir, a.rep, a.out, plt)


if __name__ == "__main__":
    main()
