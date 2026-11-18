# DeadlineWatch

A runtime deadline supervisor for **QNX 8.0** (QEMU). It watches every
real-time thread, isolates any task that overruns its deadline, and keeps
a black-box record — while the system is under attack.

**Contest:** QNX Global Hackathon · **Category:** Low-level Software ·
**Prize pool:** $50,000+ · **Deadline:** 15 Dec 2026, 14:00 ET (20:00 WAT) ·
**Site:** https://qnx.devpost.com/

> **Status — Stage 1 skeleton (18 Nov 2026).** The supervisor, the three
> threads and the black-box log are written. The code has **not yet been
> compiled against an installed SDP** — the myQNX QNX Everywhere licence
> was requested 18 Nov and is pending deployment. Nothing here has run on
> QEMU yet, and the demo numbers below are targets, not measurements.

## The problem

In safety-critical systems, one buggy task that hogs the CPU can silently
break every other task's deadlines — and nothing in the system stops it or
records it. A missed deadline in a car, a medical device or a robot is not
a slow page. It is a failure.

## How it works

One process, four kinds of thread:

- **Supervisor** (highest priority). Owns a QNX channel. Receives
  completion pulses from the workers and a 1 ms watchdog timer pulse.
  It timestamps every completion against its deadline, counts misses,
  and enforces.
- **Control loop** — 10 ms period, the critical thread. Does a fixed
  amount of work each cycle and reports completion with a pulse.
- **Sensor thread** — 50 ms period, feeds the loop, also tracked.
- **Rogue task** — deliberately greedy: one thread per online CPU, at a
  priority *above* the control loop, burning a configurable share of
  every 10 ms window. With the guard off it starves the loop.

**Deadline accounting.** The deadline of cycle *k* is
`start + (k+1) * period`. A cycle is a miss when its completion pulse
arrives after its deadline, or when the watchdog finds it overdue with no
pulse at all (starvation). Each cycle is counted at most once.

**Enforcement.** When the control loop misses twice in a row, the
supervisor demotes every rogue thread to a priority *below* the control
loop (`pthread_setschedparam()`), so the loop preempts the rogue and
recovers. If it still misses, the rogue drops to the minimum priority.
The rogue stays demoted for the rest of the run. The safe state is:
rogue demoted + a THROTTLE record in the black-box log.

**The log.** Every cycle and every supervisor action goes to
`deadline-watch.csv` (thread, event, cycle, signed lateness in µs, worst
overrun, detail). `host/render.py` turns it into a summary and a jitter
graph.

### A note on Adaptive Partitioning — read this first

The original design used QNX Adaptive Partitioning (CPU budgets per
partition). **QNX OS 8.0 does not support Adaptive Partitioning** — the
8.0 `SchedCtl()` reference lists only CPU-offlining commands, and the IDE
guide says so explicitly. So this build enforces with strict-priority
scheduling instead of kernel budgets. The idea, the demo and the category
are unchanged; only the mechanism is different. Full reasoning, evidence
and the score revision: [docs/design-8.0.md](docs/design-8.0.md).

## The demo (target numbers — to be measured on QEMU)

Split screen: a live jitter graph of the control loop on the left, the
deadline-miss counter on the right. Start the rogue task:

| Run | Guard | Expected control-loop misses |
|---|---|---|
| Rogue off | — | 0 |
| Rogue on | off | 40%+ |
| Rogue on | on | 0, rogue visibly throttled and logged |

The video (under 3 minutes) is a screen recording of the real QEMU run —
never an AI-generated video.

## Build

Prerequisites: a deployed myQNX QNX Everywhere licence, QNX SDP 8.0
(`com.qnx.qnx800`), the QEMU Quick Start Target Image
(`com.qnx.qnx800.quickstart.qemu`), and QEMU on an Ubuntu 22.04/24.04
host. Full install sequence: `~/workspace/hackathon-research/qnx-aps-apis.md`
(kept out of this repo; it is build research, not submission material).

```sh
source ~/qnx800/qnxsdp-env.sh
make            # builds ./deadline-watch for x86_64 (qcc -Vgcc_ntox86_64)
```

## Run (in the QEMU guest, as root)

```sh
./deadline-watch                      # guard on, rogue on, 30 s
./deadline-watch --guard=off          # measure only: expect 40%+ misses
./deadline-watch --rogue=off          # control-only run: expect 0 misses
./deadline-watch --duration=60 --log=run2.csv
```

Real-time priorities require root in the guest; the program fails loudly
if it cannot set them, instead of running a misleading demo. Tune the
run with `config/deadline-watch.conf` or flags (`--rogue-duty=N`,
`--rogue-threads=N`, `--log=PATH`); see `--help`.

**Getting the binary into the guest is still open.** The stock QEMU image
has no working sshd, and the official QSTI docs do not give SSH/scp steps
for a host-cross-compiled binary; the documented route is compiling
self-hosted inside the guest (QNX Developer Desktop). The setup step will
confirm which route works — either building the binary into the image or
compiling it in the guest — before the demo is recorded.

**Getting the log back:** same open item as the binary (no sshd in the
stock image). Planned: a small TCP helper. Until then, the log can be read
from the guest console.

## Render the log (on the host)

```sh
python3 host/render.py deadline-watch.csv --out jitter.png
make render-test   # no QNX needed: renders a synthetic log
```

`render.py` needs only Python 3. matplotlib is used for the PNG when
installed; without it, it prints an ASCII jitter plot.

## File layout

```
deadline-watch/
  README.md
  LICENSE                  # MIT
  Makefile                 # plain qcc wrapper (qcc -Vgcc_ntox86_64)
  config/
    deadline-watch.conf    # periods, priorities, rogue duty, modes
  src/
    dw.h                   # shared types (config, stats, context)
    supervisor.c           # main, monitor loop, enforcement, summary
    threads.c              # control loop, sensor, rogue
    config.c               # config file + command-line parsing
    log.c                  # black-box CSV log
  host/
    render.py              # log -> summary + jitter graph
  docs/
    design-8.0.md          # why there is no Adaptive Partitioning in 8.0
```

## Submission

- Public repo with an open-source licence (MIT, this repo).
- Text description: what it does, why it matters, how it uses QNX
  (strict-priority scheduling, pulses, `ClockCycles()`/`CLOCK_MONOTONIC`
  measurement, watchdog semantics).
- Demo video under 3 minutes on YouTube (public), recorded from the real
  QEMU run.
- Build/run instructions: QNX 8.0, target QEMU, toolchain
  `qcc -Vgcc_ntox86_64` (GCC 12.x).
- One category: Low-level Software.

## Honest limits (Stage 1)

- Not yet compiled against SDP 8.0; signatures are from the 8.0 docs and
  must be verified against `$QNX_TARGET/usr/include` once installed.
- The control loop's workload is a stand-in compute loop; its iteration
  count (`control_work_iters`) must be calibrated on the target so the
  work takes ~1 ms.
- The 40%+ / 0 miss numbers are targets. If the contrast is not clean on
  QEMU, tune the rogue duty cycle, the priorities, or the workload —
  that is the Days 9–10 job in the plan.
- `SCHED_SPORADIC` (a possible kernel-budget analogue for the rogue) was
  not verified and is not used; it is a post-skeleton experiment.
- No auto-restore: once demoted, the rogue stays demoted for the run.
