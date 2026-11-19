#!/usr/bin/env python3
"""Render a DeadlineWatch black-box log (CSV) into a summary + jitter graph,
or follow a run LIVE.

Usage:
    python3 host/render.py deadline-watch.csv [--out jitter.png] [--task NAME]
    python3 host/render.py --synthetic [--out /tmp/dw-jitter-test.png]
    python3 host/render.py --self-test

Live view while the run is going (the supervisor flushes the log at least
once a second). Stream the guest's log over the QEMU SSH port forward:
    ssh -p 2222 qnxuser@127.0.0.1 tail -f deadline-watch.csv \
        | python3 host/render.py --follow -
or follow a file that is being copied/synced in:
    python3 host/render.py --follow live.csv
The live view redraws a per-second deadline-miss bar chart for every
watched task plus the enforcement events (THROTTLE, ESCALATE, SAFE_STATE).

Log format (see src/log.c):
    t_ms,thread,event,cycle,lateness_us,worst_us,detail
A cycle is a miss exactly when lateness_us > 0 (CYCLE rows), plus any
MISS rows (watchdog starvation). No third-party packages are required:
matplotlib is used for the PNG only when installed; otherwise an ASCII
histogram is printed.
"""
import argparse
import os
import random
import sys
import time


EVENTS_SHOWN = ("THROTTLE", "ESCALATE", "SAFE_STATE", "RECOVER", "REGISTER",
                "UNREGISTER", "LOST")


def parse_line(line):
    """One CSV row -> dict, or None for comments / malformed lines."""
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = line.split(",", 6)
    if len(parts) < 7:
        return None
    try:
        return {
            "t_ms": float(parts[0]),
            "thread": parts[1],
            "event": parts[2],
            "cycle": int(parts[3]),
            "lateness_us": int(parts[4]),
            "worst_us": int(parts[5]),
            "detail": parts[6],
        }
    except ValueError:
        return None


def parse_log(path):
    rows = []
    with open(path, newline="") as f:
        for line in f:
            r = parse_line(line)
            if r is not None:
                rows.append(r)
    return rows


def critical_task(rows):
    """The task to plot: a task registered as critical (system mode),
    else the local-mode control loop."""
    for r in rows:
        if r["event"] == "REGISTER" and r["detail"].startswith("critical"):
            return r["thread"]
    return "control"


def summarise(rows):
    stats = {}
    throttles = []
    streak = {}
    for r in rows:
        if r["event"] in ("CYCLE", "MISS"):
            missed = r["event"] == "MISS" or r["lateness_us"] > 0
            cur = streak.get(r["thread"], 0) + 1 if missed else 0
            streak[r["thread"]] = cur
            s0 = stats.setdefault(r["thread"], {
                "cycles": 0, "misses": 0, "worst": 0, "total": 0})
            s0["max_streak"] = max(s0.get("max_streak", 0), cur)
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
            s["starved"] = s.get("starved", 0) + 1
            s["worst"] = max(s["worst"], r["lateness_us"])
    return stats, throttles


def print_summary(rows, stats, throttles):
    print("DeadlineWatch log summary")
    print("=========================")
    for thread, s in sorted(stats.items()):
        # Miss % over cycles judged = completed (CYCLE) + starved/skipped (MISS).
        judged = s["cycles"] + s.get("starved", 0)
        miss_pct = 100.0 * s["misses"] / judged if judged else 0.0
        mean = s["total"] / s["cycles"] if s["cycles"] else 0
        print(f"{thread}: {judged} cycles judged ({s['cycles']} completed), "
              f"{s['misses']} misses ({miss_pct:.1f}%), longest miss streak "
              f"{s.get('max_streak', 0)}, worst {s['worst']} us, "
              f"mean {mean:.0f} us")
    if throttles:
        print("throttle events at t = " +
              ", ".join(f"{t:.0f} ms" for t in throttles))
    else:
        print("throttle events: none")
    for r in rows:
        if r["event"] == "SAFE_STATE":
            print(f"SAFE STATE at t = {r['t_ms']:.0f} ms: {r['detail']}")
    rogues = [r for r in rows if r["event"] == "ROGUE"]
    if rogues:
        print("last rogue row: " + rogues[-1]["detail"])


def plot_ascii(rows, task="control"):
    """Coarse jitter-over-time picture using only the standard library.
    MISS rows (watchdog starvation) are plotted too: they are most of
    the misses in a guard-off run, and their lateness is the largest."""
    ctrl = [(r["t_ms"], r["lateness_us"]) for r in rows
            if r["thread"] == task and r["event"] in ("CYCLE", "MISS")]
    if not ctrl:
        print(f"(no {task} CYCLE rows to plot)")
        return
    t0, t1 = ctrl[0][0], ctrl[-1][0]
    span = max(t1 - t0, 1.0)
    cols = 60
    buckets = [None] * cols
    for t, late in ctrl:
        i = min(int((t - t0) / span * cols), cols - 1)
        buckets[i] = late if buckets[i] is None else max(buckets[i], late)
    worst = max(v for v in buckets if v is not None)
    print(f"\n{task} worst lateness per time bucket (us; >0 = missed deadline)")
    print(f"run: {t0:.0f} ms .. {t1:.0f} ms; worst {worst} us")
    for i, v in enumerate(buckets):
        if v is None:
            bar = ""
        else:
            # Scale: 40 chars for the worst bucket; negative lateness shows as '.'.
            n = int(abs(v) / max(worst, 1) * 40) if worst > 0 else 0
            bar = ("#" * n) if v > 0 else ("." * max(n, 1))
        print(f"{i * span / cols + t0:7.0f} ms |{bar} {v if v is not None else ''}")


def plot_png(rows, out, throttles, task="control"):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping PNG (ASCII plot above).")
        return False
    ctrl = [(r["t_ms"] / 1000.0, r["lateness_us"]) for r in rows
            if r["thread"] == task and r["event"] in ("CYCLE", "MISS")]
    if not ctrl:
        print(f"(no {task} CYCLE rows to plot)")
        return False
    xs, ys = zip(*ctrl)
    plt.figure(figsize=(10, 4))
    plt.plot(xs, ys, ".", markersize=2)
    plt.axhline(0, color="red", linewidth=1, label="deadline (0 us)")
    for i, t in enumerate(throttles):
        plt.axvline(t / 1000.0, color="green", linewidth=1,
                    linestyle="--", label="throttle" if i == 0 else None)
    for i, r in enumerate([r for r in rows if r["event"] == "SAFE_STATE"]):
        plt.axvline(r["t_ms"] / 1000.0, color="black", linewidth=1.5,
                    label="safe state" if i == 0 else None)
    plt.xlabel("time (s)")
    plt.ylabel(f"{task} lateness (us)")
    plt.title(f"DeadlineWatch — {task} lateness over time")
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


class LiveView:
    """Incremental state for --follow: fed raw text chunks, keeps
    per-task per-second miss counts and the enforcement event list."""

    def __init__(self, window_s=30):
        self.window_s = window_s
        self.partial = ""
        self.rows = 0
        self.t_ms = 0.0
        self.tasks = {}      # name -> {"cycles","misses","worst","sec":{s:[miss,total]}}
        self.events = []
        self.finished = False

    def feed(self, text):
        data = self.partial + text
        lines = data.split("\n")
        self.partial = lines.pop()          # keep an incomplete last line
        for line in lines:
            r = parse_line(line)
            if r is not None:
                self.add(r)

    def add(self, r):
        self.rows += 1
        self.t_ms = max(self.t_ms, r["t_ms"])
        ev = r["event"]
        if ev in ("CYCLE", "MISS"):
            t = self.tasks.setdefault(r["thread"], {
                "cycles": 0, "misses": 0, "worst": 0, "sec": {}})
            sec = int(r["t_ms"] // 1000)
            b = t["sec"].setdefault(sec, [0, 0])
            b[1] += 1
            if ev == "CYCLE":
                t["cycles"] += 1
            late = r["lateness_us"]
            if ev == "MISS" or late > 0:
                t["misses"] += 1
                b[0] += 1
            t["worst"] = max(t["worst"], late)
        elif ev in EVENTS_SHOWN:
            self.events.append(r)
        elif ev == "END":
            self.finished = True

    def render(self):
        out = []
        now_s = int(self.t_ms // 1000)
        out.append(f"DeadlineWatch LIVE  t = {self.t_ms / 1000:6.1f} s  "
                   f"rows = {self.rows}" + ("  (run ended)" if self.finished else ""))
        out.append("")
        first = max(0, now_s - self.window_s + 1)
        for name, t in sorted(self.tasks.items()):
            total = sum(v[1] for v in t["sec"].values())
            pct = 100.0 * t["misses"] / total if total else 0.0
            out.append(f"{name:<16} misses {t['misses']:6d}  ({pct:5.1f}% of "
                       f"{total} judged)  worst {t['worst'] / 1000:7.1f} ms")
            for s_ in range(first, now_s + 1):
                miss, tot = t["sec"].get(s_, [0, 0])
                frac = miss / tot if tot else 0.0
                bar = "#" * int(round(frac * 40))
                out.append(f"   {s_:4d}s |{bar:<40}| {100 * frac:5.1f}%")
            out.append("")
        if self.events:
            out.append("events:")
            for e in self.events[-8:]:
                out.append(f"  {e['t_ms'] / 1000:7.2f} s  {e['event']:<10} "
                           f"{e['thread']}: {e['detail']}")
        return "\n".join(out)


def follow(path, interval=1.0, once=False):
    """Tail a growing log (or '-' for stdin) and redraw the live view."""
    view = LiveView()
    src = sys.stdin if path == "-" else open(path, newline="")
    try:
        while True:
            if path == "-":
                line = src.readline()
                if not line:
                    break
                view.feed(line)
                # Redraw at most once per interval while streaming.
                now = time.monotonic()
                if now - getattr(follow, "_last", 0) < interval:
                    continue
                follow._last = now
            else:
                chunk = src.read()
                if chunk:
                    view.feed(chunk)
            if sys.stdout.isatty():
                sys.stdout.write("\x1b[H\x1b[2J")
            print(view.render(), flush=True)
            if once or view.finished:
                break
            if path != "-":
                time.sleep(interval)
    except KeyboardInterrupt:
        pass
    finally:
        if src is not sys.stdin:
            src.close()
    print(view.render())
    return 0


def self_test():
    """Deterministic checks of the parser, summary and live view against
    hand-built logs. Exits non-zero on failure. No QNX needed."""
    import tempfile
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    lines = [
        "# DeadlineWatch black-box log",
        "# mode=system guard=on rogue=off duration=5s",
        "0.000,supervisor,START,0,0,0,run begins",
        "1.000,pump-dose,REGISTER,0,0,0,critical pid=4100 tid=2 period=10ms",
        "11.000,pump-dose,CYCLE,0,-500,0,",          # on time
        "21.500,pump-dose,CYCLE,1,1500,1500,",        # late -> miss
        "33.000,pump-dose,MISS,2,2000,2000,starved",  # starved -> miss
        "33.000,supervisor,THROTTLE,2,0,2000,pump-dose missed 2 in a row: demoted 2 of 2 offender threads to prio 5",
        "1041.000,pump-dose,CYCLE,103,-800,2000,",
        "1500.000,supervisor,SAFE_STATE,150,0,2000,critical task pump-dose lost (pid 4100): safe state latched; operator alarm",
        "bad,row,that,does,not,parse,!",
        "1600.000,supervisor,END,0,0,0,run ends",
    ]
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
        f.write("\n".join(lines) + "\n")
        path = f.name
    try:
        rows = parse_log(path)
        check(len(rows) == 9, f"parsed {len(rows)} rows, expected 9")
        stats, throttles = summarise(rows)
        d = stats.get("pump-dose", {})
        check(d.get("cycles") == 3, f"pump-dose cycles {d.get('cycles')} != 3")
        check(d.get("misses") == 2, f"pump-dose misses {d.get('misses')} != 2")
        check(d.get("worst") == 2000, f"pump-dose worst {d.get('worst')} != 2000")
        check(throttles == [33.0], f"throttles {throttles}")
        check(critical_task(rows) == "pump-dose", "critical task from REGISTER")

        # Live view fed in awkward chunks (split mid-line) must agree.
        text = "\n".join(lines) + "\n"
        lv = LiveView()
        for i in range(0, len(text), 7):
            lv.feed(text[i:i + 7])
        t = lv.tasks.get("pump-dose", {})
        check(t.get("misses") == 2, f"live misses {t.get('misses')} != 2")
        check(t.get("sec", {}).get(0) == [2, 3], f"live second 0 {t.get('sec', {}).get(0)}")
        check([e["event"] for e in lv.events] == ["REGISTER", "THROTTLE", "SAFE_STATE"],
              f"live events {[e['event'] for e in lv.events]}")
        check(lv.finished, "live view saw END")
        check("SAFE_STATE" in lv.render(), "live render shows the safe state")

        # Synthetic local-mode log still renders end to end.
        synth = synthetic_log(path)
        srows = parse_log(synth)
        sstats, sthr = summarise(srows)
        check(sstats["control"]["cycles"] == 3000, "synthetic: 3000 control cycles")
        check(len(sthr) == 1, "synthetic: one throttle")
    finally:
        os.unlink(path)

    if fails:
        for m in fails:
            print("FAIL: " + m)
        return 1
    print("render.py self-test: all checks passed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="black-box log CSV to render")
    ap.add_argument("--out", default="host/jitter.png",
                    help="PNG output path (default host/jitter.png)")
    ap.add_argument("--task", default=None,
                    help="task to plot (default: the critical task)")
    ap.add_argument("--synthetic", action="store_true",
                    help="generate a fake log and render it (no QNX needed)")
    ap.add_argument("--follow", metavar="PATH",
                    help="live view of a growing log ('-' = stdin)")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="live view redraw interval in seconds")
    ap.add_argument("--self-test", action="store_true",
                    help="run the parser/summary self-test (no QNX needed)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.follow:
        return follow(args.follow, args.interval)

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
    task = args.task or critical_task(rows)
    print_summary(rows, stats, throttles)
    plot_ascii(rows, task)
    plot_png(rows, args.out, throttles, task)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
