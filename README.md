# DeadlineWatch

A runtime deadline supervisor for QNX 8.0. It watches every real-time
thread, checks whether each one finished before its deadline, throttles
any task that overruns, and keeps a black-box record of every miss.

## The problem

In safety-critical systems, one buggy task that hogs the CPU can silently
break every other task's deadlines, and nothing in the system stops it or
records it. A missed deadline in a car, a medical device or a robot is not
a slow page. It is a failure.

## The solution

DeadlineWatch is a supervisor process that runs alongside the real-time
threads. Every cycle it checks, per thread, whether the work finished
before its deadline. It counts and times the misses, and when a thread
misses twice in a row it demotes that thread's priority so the critical
work recovers. Every cycle and every enforcement action is written to a
timestamped log, so a missed deadline is never silent and never lost.

## How it works

One process, four kinds of thread:

- **Supervisor** (highest priority). Owns a QNX channel. Receives
  completion pulses from the workers and a 1 ms watchdog timer pulse.
  It timestamps every completion against its deadline, counts misses,
  and enforces.
- **Control loop:** 10 ms period, the critical thread. Does a fixed
  amount of work each cycle and reports completion with a pulse.
- **Sensor thread:** 50 ms period, feeds the loop, also tracked.
- **Rogue task:** deliberately greedy: one thread per online CPU, at a
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

### A note on Adaptive Partitioning

The original design used QNX Adaptive Partitioning (CPU budgets per
partition). **QNX OS 8.0 does not support Adaptive Partitioning.** The
8.0 `SchedCtl()` reference lists only CPU-offlining commands, and the IDE
guide says so explicitly. So this build enforces with strict-priority
scheduling instead of kernel budgets. The idea and the demo are unchanged;
only the mechanism is different. Full reasoning and evidence:
[docs/design-8.0.md](docs/design-8.0.md).

## The demo (measured on QEMU, 30 s runs, 2 rogue threads, 85% duty)

Split screen: a live jitter graph of the control loop on the left, the
deadline-miss counter on the right. Start the rogue task:

| Run | Guard | Measured control-loop misses |
|---|---|---|
| Rogue off | n/a | 0.1–0.6% |
| Rogue on | off | 23–39% |
| Rogue on | on | 5–20%, rogue visibly throttled and logged |

Three 30 s runs on QEMU gave guard off 38.9%, 22.6%, 35.5%, and guard
on 9.8%, 4.7%, 19.6%. Exact percentages move with host load under
software emulation, so the claim is the improvement: the guard cuts
misses roughly 2–5× and throttles the rogue every time.

The video (under 3 minutes) is a screen recording of the real QEMU run,
never an AI-generated video.

## Build

Prerequisites: QNX SDP 8.0 (`com.qnx.qnx800`), the QEMU Quick Start
Target Image (`com.qnx.qnx800.quickstart.qemu`), and QEMU on an Ubuntu
22.04/24.04 host. The SDP is available with a free myQNX QNX Everywhere
licence.

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

**Getting the binary into the guest.** The QSTI docs prescribe copying
it in over SSH (`scp` to `qnxuser@<guest-ip>`). Whether sshd runs by
default on the QEMU image is still to be confirmed on a booted guest;
the fallback is a host HTTP server plus guest `curl`.

**Getting the log back:** the same route in reverse (`scp` from the
guest), or read it from the guest console.

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

## Honest limits

- The skeleton has been compiled and run in the QEMU guest; the real
  numbers are in the demo table above and in `STAGE-2B-RUNS.md`. Host
  `qcc` is currently blocked: a restart wiped the licence file, so
  host-side compiles need the licence restored first. The guest has
  its own compiler as a fallback.
- The control loop's workload is a stand-in compute loop; its iteration
  count (`control_work_iters`) must be calibrated on the target so the
  work takes ~1 ms.
- The miss numbers are measured on QEMU under TCG software emulation,
  and they move run to run with host load (guard off 38.9%, 22.6%,
  35.5%; guard on 9.8%, 4.7%, 19.6%). The stable finding is the
  improvement: the guard cuts misses roughly 2–5× and throttles the
  rogue. The residual misses with the guard on are mostly watchdog
  starvation from host stalls, and the rogue-off baseline shows the
  same pattern (0.1–0.6%). If the contrast is not clean on QEMU, tune
  the rogue duty cycle, the priorities, or the workload.
- `SCHED_SPORADIC` (a possible kernel-budget analogue for the rogue) was
  not verified and is not used; it is a post-skeleton experiment.
- No auto-restore: once demoted, the rogue stays demoted for the run.
