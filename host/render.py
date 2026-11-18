#!/usr/bin/env python3
"""Render a DeadlineWatch black-box log (CSV) into a summary + jitter graph.

Usage:
    python3 host/render.py deadline-watch.csv [--out jitter.png]
    python3 host/render.py --synthetic [--out /tmp/dw-jitter-test.png]

Log format (see src/log.c):
    t_ms,thread,event,cycle,lateness_us,worst_us,detail
A cycle is a miss exactly when lateness_us > 0 (CYCLE rows), plus any
MISS rows (watchdog starvation). No third-party packages are required:
matplotlib is used for the PNG only when installed; otherwise an ASCII
histogram is printed.
"""
import argparse
import random
import sys


def parse_log(path):
    rows = []
    with open(path, newline="") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", 6)
            if len(parts) < 7:
                continue
            try:
                rows.append({
                    "t_ms": float(parts[0]),
                    "thread": parts[1],
                    "event": parts[2],
                    "cycle": int(parts[3]),
                    "lateness_us": int(parts[4]),
                    "worst_us": int(parts[5]),
                    "detail": parts[6],
                })
            except ValueError:
                continue
    return rows


def summarise(rows):
    stats = {}
    throttles = []
    for r in rows:
        if r["event"] == "THROTTLE":
            throttles.append(r["t_ms"])
        if r["event"] == "CYCLE":
            s = stats.setdefault(r["thread"], {
                "cycles": 0, "misses": 0, "worst": 0, "total": 0})
            s["cycles"] += 1
            s["total"] += r["lateness_us"]
            s["worst"] = max(s["worst"], r["lateness_us"])
            if r["lateness_us"] > 0:
                s["misses"] += 1
        elif r["event"] == "MISS":
            s = stats.setdefault(r["thread"], {
                "cycles": 0, "misses": 0, "worst": 0, "total": 0})
            s["misses"] += 1
            s["worst"] = max(s["worst"], r["lateness_us"])
    return stats, throttles


def print_summary(rows, stats, throttles):
    print("DeadlineWatch log summary")
    print("=========================")
    for thread, s in sorted(stats.items()):
        miss_pct = 100.0 * s["misses"] / s["cycles"] if s["cycles"] else 0.0
        mean = s["total"] / s["cycles"] if s["cycles"] else 0
        print(f"{thread}: {s['cycles']} cycles, {s['misses']} misses "
              f"({miss_pct:.1f}%), worst {s['worst']} us, mean {mean:.0f} us")
    if throttles:
        print("throttle events at t = " +
              ", ".join(f"{t:.0f} ms" for t in throttles))
    else:
        print("throttle events: none")
    rogues = [r for r in rows if r["event"] == "ROGUE"]
    if rogues:
        print("last rogue row: " + rogues[-1]["detail"])


def plot_ascii(rows):
    """Coarse jitter-over-time picture using only the standard library."""
    ctrl = [(r["t_ms"], r["lateness_us"]) for r in rows
            if r["thread"] == "control" and r["event"] == "CYCLE"]
    if not ctrl:
        print("(no control CYCLE rows to plot)")
        return
    t0, t1 = ctrl[0][0], ctrl[-1][0]
    span = max(t1 - t0, 1.0)
    cols = 60
    buckets = [None] * cols
    for t, late in ctrl:
        i = min(int((t - t0) / span * cols), cols - 1)
        buckets[i] = late if buckets[i] is None else max(buckets[i], late)
    worst = max(v for v in buckets if v is not None)
    print("\ncontrol-loop worst lateness per time bucket (us; >0 = missed deadline)")
    print(f"run: {t0:.0f} ms .. {t1:.0f} ms; worst {worst} us")
    for i, v in enumerate(buckets):
        if v is None:
            bar = ""
        else:
            # Scale: 40 chars for the worst bucket; negative lateness shows as '.'.
            n = int(abs(v) / max(worst, 1) * 40) if worst > 0 else 0
            bar = ("#" * n) if v > 0 else ("." * max(n, 1))
        print(f"{i * span / cols + t0:7.0f} ms |{bar} {v if v is not None else ''}")


def plot_png(rows, out, throttles):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping PNG (ASCII plot above).")
        return False
    ctrl = [(r["t_ms"] / 1000.0, r["lateness_us"]) for r in rows
            if r["thread"] == "control" and r["event"] == "CYCLE"]
    if not ctrl:
        print("(no control CYCLE rows to plot)")
        return False
    xs, ys = zip(*ctrl)
    plt.figure(figsize=(10, 4))
    plt.plot(xs, ys, ".", markersize=2)
    plt.axhline(0, color="red", linewidth=1, label="deadline (0 us)")
    for t in throttles:
        plt.axvline(t / 1000.0, color="green", linewidth=1,
                    linestyle="--", label="throttle")
    plt.xlabel("time (s)")
    plt.ylabel("control-loop lateness (us)")
    plt.title("DeadlineWatch — control-loop lateness over time")
    plt.legend(loc="upper right")
    plt.tight_layout()
    plt.savefig(out)
    print(f"wrote {out}")
    return True


def synthetic_log(path):
    """A fake 30 s log in the real format, so the script can be tested
    without QNX. First 10 s: guard off, ~30% misses. Then a THROTTLE
    row, then 20 s with ~0 misses."""
    rng = random.Random(42)
    lines = [
        "# DeadlineWatch black-box log",
        "# format: t_ms,thread,event,cycle,lateness_us,worst_us,detail",
        "# guard=off rogue=on duration=30s",
        "0.000,supervisor,START,0,0,0,run begins",
    ]
    worst = 0
    for cycle in range(3000):
        t = cycle * 10.0
        if cycle < 1000:
            late = int(rng.gauss(800, 1500))
        else:
            late = int(rng.gauss(-200, 150))
        worst = max(worst, late)
        lines.append(f"{t:.3f},control,CYCLE,{cycle},{late},{worst},")
        if cycle % 5 == 0:
            sl = int(rng.gauss(-100, 80))
            lines.append(f"{t:.3f},sensor,CYCLE,{cycle // 5},{sl},{max(sl, 0)},")
        if cycle == 1000:
            lines.append(f"{t:.3f},supervisor,THROTTLE,{cycle},0,{worst},"
                         "rogue demoted to prio 5 (8 of 8 threads)")
        if cycle % 100 == 0:
            rate = 800 if cycle < 1000 else 120
            lines.append(f"{t:.3f},rogue,ROGUE,{cycle * 8},0,0,"
                         f"rogue windows/s={rate} prio={25 if cycle < 1000 else 5}")
    lines.append("30000.000,supervisor,END,0,0,0,run ends")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", nargs="?", help="black-box log CSV to render")
    ap.add_argument("--out", default="host/jitter.png",
                    help="PNG output path (default host/jitter.png)")
    ap.add_argument("--synthetic", action="store_true",
                    help="generate a fake log and render it (no QNX needed)")
    args = ap.parse_args(argv)

    path = args.log
    if args.synthetic:
        path = path or "/tmp/dw-synthetic.csv"
        synthetic_log(path)
        print(f"generated synthetic log: {path}")
    if not path:
        ap.error("give a log file, or use --synthetic")
    rows = parse_log(path)
    if not rows:
        print(f"no rows parsed from {path}", file=sys.stderr)
        return 1
    stats, throttles = summarise(rows)
    print_summary(rows, stats, throttles)
    plot_ascii(rows)
    plot_png(rows, args.out, throttles)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
