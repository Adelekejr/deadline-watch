#!/usr/bin/env python3
"""One figure comparing pump scenarios from the real logs in docs/results/.

    python3 host/plot_scenarios.py docs/results --rep r1 --out docs/results/pump-scenarios-r1.png
    python3 host/plot_scenarios.py docs/results --local --rep r1 --out docs/results/local-d100-r1.png

Top: critical-loop misses per 100 ms (from the CSV black box).
Bottom (pump only): dose error over time, from the pump's own console
capture (*-pump.out; pump time + 1 s = supervisor time, see demo.sh).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render import parse_log, critical_task  # noqa: E402


def misses_per_bin(rows, task, bin_ms=100.0):
    bins = {}
    for r in rows:
        if r["thread"] != task or r["event"] not in ("CYCLE", "MISS"):
            continue
        if r["event"] == "MISS" or r["lateness_us"] > 0:
            b = int(r["t_ms"] // bin_ms)
            bins[b] = bins.get(b, 0) + 1
    end = max((r["t_ms"] for r in rows), default=0)
    xs = [i * bin_ms / 1000.0 for i in range(int(end // bin_ms) + 1)]
    return xs, [bins.get(i, 0) for i in range(len(xs))]


def actions(rows):
    return [(r["t_ms"] / 1000.0, r["event"]) for r in rows
            if r["event"] in ("THROTTLE", "ESCALATE", "SAFE_STATE")]


def dose(path):
    xs, ys = [], []
    for line in open(path, errors="replace"):
        m = re.search(r"\[pump\s+([\d.]+)s\].*dose error\s+([+-]?\d+\.\d+)%", line)
        if m:
            x = float(m.group(1)) + 1.0
            if xs and x - xs[-1] > 1.5:     # printer starved: show the gap
                xs.append(float("nan"))
                ys.append(float("nan"))
            xs.append(x)
            ys.append(float(m.group(2)))
    return xs, ys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--rep", default="r1")
    ap.add_argument("--local", action="store_true")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if a.local:
        runs = [("rogue off", "local-rogue-off-2"),
                ("rogue 100 %, guard off", f"local-d100-guard-off-{a.rep}"),
                ("rogue 100 %, guard on", f"local-d100-guard-on-{a.rep}")]
        fig, ax = plt.subplots(1, 1, figsize=(10, 3.6))
        axes = [ax]
    else:
        runs = [("A: guard off", f"pump-A-guard-off-{a.rep}"),
                ("B: guard on (rogue registered)", f"pump-B-guard-on-{a.rep}"),
                ("C: guard on (rogue unknown) -> SAFE_STATE", f"pump-C-safe-state-{a.rep}"),
                ("C': guard on, --demote-pid", f"pump-Cpid-demote-pid-{a.rep}")]
        fig, axes = plt.subplots(2, 1, figsize=(10, 6.4), sharex=True)
    colors = ["tab:red", "tab:green", "tab:orange", "tab:blue"]
    for (label, base), col in zip(runs, colors):
        rows = parse_log(os.path.join(a.dir, base + ".csv"))
        task = critical_task(rows)
        xs, ys = misses_per_bin(rows, task)
        axes[0].step(xs, ys, where="post", color=col, label=label, lw=1.2)
        for t, ev in actions(rows):
            axes[0].axvline(t, color=col, ls="--", lw=0.8)
        if not a.local:
            px, py = dose(os.path.join(a.dir, base + "-pump.out"))
            axes[1].plot(px, py, "o-", color=col, ms=3, label=label)
    crit = "control loop (10 ms)" if a.local else "pump-dose loop (10 ms)"
    axes[0].set_ylabel(f"misses / 100 ms\n{crit}")
    axes[0].set_ylim(-0.5, 11)
    axes[0].legend(loc="upper right", fontsize=8)
    title = ("DeadlineWatch on QNX 8.0 (QEMU x86_64, TCG) - " +
             ("local mode" if a.local else "infusion-pump scenarios") +
             f", run {a.rep}; dashed = THROTTLE / SAFE_STATE")
    axes[0].set_title(title, fontsize=10)
    if not a.local:
        axes[1].axhline(0, color="k", lw=0.6)
        axes[1].set_ylabel("dose error (%)\nprescribed vs delivered")
        axes[1].text(0.5, 0.02, "gaps = the pump's status printer (normal priority) got no CPU",
                     transform=axes[1].transAxes, ha="center", fontsize=8, color="gray")
        axes[1].legend(loc="lower left", fontsize=8)
    axes[-1].set_xlabel("supervisor time (s)")
    fig.tight_layout()
    fig.savefig(a.out, dpi=110)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
