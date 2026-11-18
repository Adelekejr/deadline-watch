# DeadlineWatch on QNX 8.0 — the Adaptive Partitioning finding and the redesign

## The finding

**Adaptive Partitioning (APS) does not exist in QNX OS 8.0.** The
DeadlineWatch design as originally briefed — create CPU partitions, join
threads to them, change budgets at runtime, query partition statistics —
cannot run on SDP 8.0 or the QEMU Quick Start Target Image. Verified three
independent ways in the official 8.0 documentation (full notes with all
sources: `~/workspace/hackathon-research/qnx-aps-apis.md`):

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

Survives: the idea, the demo, and the category. A runtime supervisor that
watches real-time threads, enforces their deadlines while the system is
under attack, and keeps a black-box record is still buildable on 8.0 —
only the enforcement mechanism changes. The demo (0 missed deadlines with
the guard on vs. 40%+ with it off) is unchanged in shape, and the entry
stays in Low-level Software.

## The 8.0 redesign (what this repo builds)

- **Scheduling:** strict-priority `SCHED_FIFO`. Supervisor at the highest
  priority, control loop next, sensor below it, rogue above the control
  loop (so it starves the loop when the guard is off).
- **Measurement:** application-measured, not read from a partition API.
  Each thread reports completion with a pulse carrying its cycle index;
  the supervisor timestamps arrival against the deadline
  (`start + (cycle+1) * period`, `CLOCK_MONOTONIC`). A 1 ms watchdog
  timer pulse catches starvation — a cycle whose deadline passes with no
  pulse at all is a miss.
- **Enforcement:** when the control loop misses twice in a row, the
  supervisor demotes every rogue thread to a priority below the control
  loop (`pthread_setschedparam()`), so the loop preempts the rogue and
  recovers. If it still misses, the rogue drops to the minimum priority
  (escalation, once). The rogue stays demoted for the rest of the run.
  The "safe state" is: rogue demoted + a THROTTLE record in the log.
- **Multi-CPU QEMU:** the guest defaults to 8 CPUs, and one rogue thread
  can only hog one CPU. So the rogue is one thread *per online CPU*
  (detected at runtime), all at the same high priority, each burning a
  configurable share of every 10 ms window. That is what makes the
  starvation real on an 8-CPU guest.
- **Log:** black-box CSV (`deadline-watch.csv`), one row per cycle plus
  supervisor events; `host/render.py` turns it into the jitter graph.

## Why not QNX 7.1, and why not switch to BudgetCheck

- **7.1 is not an option for this contest.** The hackathon requires a
  project that "runs on or targets QNX 8.0", the free QNX Everywhere
  licence is for SDP 8.0, and the QEMU image is 8.0. A 7.1 build with real
  APS would be a different project on a different OS version.
- **BudgetCheck (8.4) stays the fallback, not the pick.** The supervisor
  path is not genuinely blocked — only its APS mechanism is. The core
  idea and the strongest demo survive the redesign, so switching would
  discard the best demo for a marginal buildability gain. Switch only if
  the 8.0 supervisor path hits a wall during the skeleton/enforcement
  days — for example, if real-time priorities cannot be set in the guest,
  or the priority-demotion demo does not produce a clean contrast.

## Honest score revision

The original 8.6 assumed APS as the mechanism. Without kernel budgets
the "Best use of QNX-specific Technologies" bonus angle is weaker and
contest fit drops a notch. Revised estimate for the redesigned entry:
**~8.2–8.4** — roughly tied with BudgetCheck (8.4, higher buildability).
The demo score (the strongest element) is unchanged: a guarantee holding
under attack is still what the judge watches.

## Open questions (to close once the SDP is installed)

- `SCHED_SPORADIC` is listed as a scheduling policy on the 8.0
  `sched_setscheduler()` page. Its budget/replenishment semantics were
  not verified — it could be the closest 8.0 analogue to a CPU budget
  for the rogue, and is worth one experiment after the priority version
  works. It is NOT used in the skeleton.
- Priority numeric range: use `sched_get_priority_min/max()` at runtime
  (the code does this); the classic "1–255" range is not verified for
  8.0.
- How a host-cross-compiled binary gets into the running QSTI guest
  (no SSH/scp steps in the official QSTI docs; self-hosted compile in
  the guest is the documented route). See README.md.
- Which QEMU version Ubuntu 24.04's apt provides, and whether stock apt
  QEMU boots the QSTI (the guide recommends building QEMU 10.1.2).
- `ClockPeriod()` set-ability on 8.0 (not relied on; the 1 ms watchdog
  uses a timer, not the system tick).
