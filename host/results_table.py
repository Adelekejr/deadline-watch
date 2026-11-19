#!/usr/bin/env python3
"""Build the RESULTS.md tables from the real logs in docs/results/.

    python3 host/results_table.py docs/results

Every number comes from the CSV black-box logs (and, for the pump's dose
error, from the pump's own console capture *-pump.out). Nothing is typed
in by hand.

Definitions
- judged: cycles whose deadline has passed (completed + starved/skipped)
- miss %: misses / judged
- streak: longest run of consecutive misses (x period = outage length)
- attack window (pump runs): supervisor time 4.0 s .. 24.0 s, when
  scripts/demo.sh runs dw-rogue (it starts ~1 s + 3 s after the
  supervisor and runs DUR-10 = 20 s; approximate to process start-up)
- supervisor stall: longest gap between consecutive log rows while tasks
  are active. The supervisor runs at prio 30 and logs every 10 ms cycle,
  so a gap of seconds means the whole guest stopped (a QEMU/TCG stall),
  not a scheduling problem the guard could fix.
- reaction: time from the first critical-task miss inside the attack
  window to the supervisor's first THROTTLE or SAFE_STATE row
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render import parse_log  # noqa: E402

ATTACK = (4000.0, 24000.0)


def task_stats(rows, task, window=None):
    judged = misses = streak = cur = 0
    for r in rows:
        if r["thread"] != task or r["event"] not in ("CYCLE", "MISS"):
            continue
        if window and not (window[0] <= r["t_ms"] < window[1]):
            continue
        judged += 1
        missed = r["event"] == "MISS" or r["lateness_us"] > 0
        if missed:
            misses += 1
            cur += 1
            streak = max(streak, cur)
        else:
            cur = 0
    return judged, misses, streak


def critical(rows):
    for r in rows:
        if r["event"] == "REGISTER" and r["detail"].startswith("critical"):
            return r["thread"]
    return "control"


def first(rows, event, after=0.0):
    for r in rows:
        if r["event"] == event and r["t_ms"] >= after:
            return r
    return None


def pump_dose(out_path):
    """(worst dose error %, last dose error %, alarm seen) from *-pump.out."""
    worst = last = None
    alarm = False
    if not os.path.exists(out_path):
        return None, None, False
    for line in open(out_path, errors="replace"):
        m = re.search(r"dose error\s+([+-]?\d+\.\d+)%", line)
        if m:
            v = float(m.group(1))
            last = v
            worst = v if worst is None else min(worst, v)
        if "ALARM" in line:
            alarm = True
    return worst, last, alarm


def stall_ms(rows):
    end = None
    for r in rows:
        if r["event"] in ("UNREGISTER", "LOST", "END"):
            end = r["t_ms"]
            break
    gap, prev = 0.0, None
    for r in rows:
        if prev is None and r["event"] != "CYCLE":
            continue                    # start at the first judged cycle
        if end is not None and r["t_ms"] > end:
            break
        if prev is not None:
            gap = max(gap, r["t_ms"] - prev)
        prev = r["t_ms"]
    return gap


def pct(m, j):
    return f"{100.0 * m / j:.1f} %" if j else "n/a"


def main(d):
    print("### Local mode (one process; 30 s; control 10 ms @20, rogue @25)\n")
    print("| Run | Control misses | Longest streak | Sensor misses | Enforcement | Supervisor stall |")
    print("|---|---|---|---|---|---|")
    for path in sorted(glob.glob(os.path.join(d, "local-*.csv"))):
        rows = parse_log(path)
        name = os.path.basename(path)[6:-4]
        j, m, s = task_stats(rows, "control")
        sj, sm, _ = task_stats(rows, "sensor")
        acts = [f"{r['event']} @{r['t_ms'] / 1000:.2f}s" for r in rows
                if r["event"] in ("THROTTLE", "ESCALATE", "SAFE_STATE")]
        print(f"| {name} | {m}/{j} ({pct(m, j)}) | {s} ({s * 10} ms) | "
              f"{sm}/{sj} ({pct(sm, sj)}) | {', '.join(acts) or 'none'} | "
              f"{stall_ms(rows):.0f} ms |")

    print("\n### Pump scenarios (three processes; supervisor, dw-pump, dw-rogue)\n")
    print("| Run | pump-dose misses, whole run | ...during attack window | "
          "Longest streak | Action (reaction) | Worst / final dose error | Pump alarm | Supervisor stall |")
    print("|---|---|---|---|---|---|---|---|")
    for path in sorted(glob.glob(os.path.join(d, "pump-*.csv"))):
        rows = parse_log(path)
        name = os.path.basename(path)[5:-4]
        task = critical(rows)
        j, m, s = task_stats(rows, task)
        aj, am, _ = task_stats(rows, task, ATTACK)
        act = None
        for ev in ("THROTTLE", "SAFE_STATE"):
            r = first(rows, ev)
            if r and (act is None or r["t_ms"] < act["t_ms"]):
                act = r
        react = ""
        if act:
            fm = next((r for r in rows if r["thread"] == task and r["t_ms"] >= ATTACK[0]
                       and (r["event"] == "MISS" or (r["event"] == "CYCLE" and r["lateness_us"] > 0))), None)
            if fm and fm["t_ms"] <= act["t_ms"]:
                react = f" ({act['t_ms'] - fm['t_ms']:.0f} ms after first miss)"
            act_s = f"{act['event']} @{act['t_ms'] / 1000:.2f}s{react}"
            if act["t_ms"] < ATTACK[0]:
                act_s += " (before the rogue started)"
        else:
            act_s = "none"
        worst, last, alarm = pump_dose(path[:-4] + "-pump.out")
        dose = (f"{worst:+.1f} % / {last:+.1f} %" if worst is not None else "n/a")
        print(f"| {name} | {m}/{j} ({pct(m, j)}) | {am}/{aj} ({pct(am, aj)}) | "
              f"{s} ({s * 10} ms) | {act_s} | {dose} | {'yes' if alarm else 'no'} | "
              f"{stall_ms(rows):.0f} ms |")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "docs/results")
