# DeadlineWatch on QNX 8.0 — the Adaptive Partitioning finding and the redesign

## The finding

**Adaptive Partitioning (APS) does not exist in QNX OS 8.0.** The
DeadlineWatch design as originally briefed — create CPU partitions, join
threads to them, change budgets at runtime, query partition statistics —
cannot run on SDP 8.0 or the QEMU Quick Start Target Image. Verified three
independent ways in the official 8.0 documentation:

1. IDE User Guide, Target Navigator (8.0): *"QNX OS 8.0 does not support
   adaptive partitioning; if your target is based on 8.0, then APS can't be
   configured and these controls will be ineffective."*
   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.ide.userguide/topic/target_navigator.html
2. IDE User Guide, APS View (8.0): the same note verbatim.
3. C Library Reference, `SchedCtl()` (8.0): only three commands exist —
   `SCHED_PROCESSOR_ONLINE`, `SCHED_PROCESSOR_OFFLINE`,
   `SCHED_PROCESSORS_STATUS` (CPU offlining). No `SCHED_APS_*` commands,
   and the synopsis includes only `<sys/neutrino.h>` — no
   `<sys/sched_aps.h>`. Independently re-checked 18 Nov 2026.
   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/schedctl.html

The 7.x `SCHED_STOP_APP` / `SCHED_CONT_APP` commands (stop a whole
application — a possible rogue-throttle) are also gone from 8.0. No
replacement CPU-budgeting feature is named anywhere in the 8.0 docs read.

## What this kills, and what survives

Killed: the mechanism. No partitions, no partition budgets, no
`SCHED_APS_MODIFY_PARTITION` throttle, no partition statistics.

Survives: the idea and the demo. A runtime supervisor that watches
real-time work, enforces deadlines while the system is under attack, and
keeps a black-box record is still buildable on 8.0; only the enforcement
mechanism changes. The entry is in the **Robotics & Medical** category
(real-time applications where timing, safety, or reliability is
critical), framed around a simulated infusion-pump dose loop.

## The 8.0 design (what this repo builds)

- **Policy vs mechanism.** `src/dw_core.c` decides *when* a cycle is a
  miss and *when* to act; it has no QNX calls and is unit-tested on the
  host (`tests/test_core.c`). `src/supervisor.c` carries the decisions out
  with QNX calls.
- **Scheduling:** strict-priority `SCHED_FIFO`. Supervisor highest (30),
  critical loop next (20), tracked sensor below it (15), offender above
  the critical loop (25) so it starves the loop when the guard is off.
- **IPC:** one supervisor channel created with
  `ChannelCreate(_NTO_CHF_FIXED_PRIORITY | ...)`, so receiving from a
  low-priority client never lowers the supervisor's priority.
  - *local mode:* worker threads report with `MsgSendPulse()`.
  - *system mode:* the channel is published with
    `dispatch_create_channel()` + `name_attach("deadline-watch")`. Other
    processes `name_open()` it and use `MsgSend()` (REGISTER / BEAT /
    PROGRESS). The supervisor takes the sender's pid/tid from the
    kernel's `struct _msg_info`, never from the message, and gets
    `_PULSE_CODE_DISCONNECT` when a client dies (`_NTO_CHF_DISCONNECT`).
- **Measurement:** cycle *k* of a task with period *P* has deadline
  `epoch + (k+1)P` on `CLOCK_MONOTONIC` (system-wide, so a client's epoch
  is the supervisor's registration timestamp). A 1 ms `timer_create()`
  pulse (`SIGEV_PULSE_INIT`) is the watchdog: a cycle whose deadline plus
  2 ms grace passes with no report is a starvation miss.
- **Enforcement ladder** (critical tasks only):
  1. THROTTLE: 2 consecutive misses → demote every offender thread below
     the critical loop. Local threads: `pthread_setschedparam()`.
     Other processes: `SchedSet(pid, tid, ...)`. Unregistered processes
     named with `--demote-pid`: every thread id that exists in them.
  2. ESCALATE: 4 more consecutive misses → offenders to the minimum
     priority.
  3. SAFE_STATE: enforcement exhausted (or nothing can be demoted) and
     the critical task still fails, for 300 ms continuously or 50 % of a
     1 s window, or its process died → the next BEAT reply carries
     `DW_CMD_SAFE_STATE`, and the critical threads are boosted just
     below the supervisor (`SchedSet`) so they can run their safe-state
     code even if the offender could not be demoted. Latched, like a
     real alarm.
- **Multi-CPU QEMU:** the guest defaults to 8 CPUs, and one offender
  thread can only hog one CPU, so the rogue runs one thread per online
  CPU.
- **Log:** black-box CSV, one row per cycle plus every supervisor event,
  flushed at least once a second; `host/render.py` renders it after the
  run or live (`--follow`).

## Limits of the 8.0 mechanism (stated plainly)

- Priority demotion is not a CPU budget: a demoted offender still runs
  whenever higher-priority work is idle. That is the intended behaviour
  for best-effort work, but it is not the hard isolation APS gave on 7.x.
- A thread that is starved cannot act on any command. The rescue boost
  addresses that for registered critical threads; a real medical device
  would still pair this with an independent hardware watchdog.
- `--demote-pid` walks thread ids 1..256 with `SchedSet()` and treats
  ESRCH as "no such thread"; a process with sparse thread ids above
  that range would be missed.

## Open questions

- `SCHED_SPORADIC` is listed on the 8.0 `sched_setscheduler()` page. It
  could be the closest 8.0 analogue to a per-thread CPU budget for
  offenders. Not used yet.
- Priority numeric range: the code queries
  `sched_get_priority_min/max()` at runtime and clamps.
- QEMU/TCG host stalls (up to ~185 ms observed) freeze the guest while
  its clock keeps running, which produces misses with no thread at
  fault. Real hardware (e.g. a Raspberry Pi 5) would give cleaner
  numbers.
