# Devpost submission — ready to paste

> Fill-in notes for the submitter (delete before pasting): the video link,
> the measured numbers for the pump scenarios, and the screenshots must
> come from real QNX runs of THIS version (see docs/RESULTS.md, "TO
> RE-RECORD"). Do not paste a number that has not been measured.

**Project name:** DeadlineWatch

**Category:** Robotics & Medical

**Tagline (≤ 200 chars):**
A runtime deadline guard for QNX 8.0: it catches a critical control loop
missing its deadlines, demotes the process starving it, and drives the
device to a safe state if that fails.

**Built with:** QNX SDP 8.0, QNX OS 8.0 (QEMU x86_64 target image), qcc
(GCC), C11, QNX message passing (channels, pulses, name_attach),
SCHED_FIFO, Python 3 (host log viewer).

---

## Inspiration

QNX's promise is not just that code runs, but *when*. In an infusion
pump or a surgical robot, a control loop that runs late is a failure,
not a slowdown. Yet the most common way it happens is mundane: some
other task, a logger, a UI, a telemetry job, ends up at the wrong
priority or in a loop, and quietly eats the CPU. Nothing notices, and
nothing records it. We wanted a component a device team could drop in
that *watches the deadlines themselves*, steps in, and leaves evidence.

Our first design used QNX Adaptive Partitioning to shrink the
offender's CPU budget. Reading the 8.0 documentation we found that APS
is not supported in QNX OS 8.0, so we redesigned the enforcement around
what 8.0 does provide: strict-priority scheduling and kernel calls that
can change another process's thread priority.

## What it does

DeadlineWatch is a supervisor that runs at the highest priority and
judges every cycle of every registered loop against its deadline:

- A loop reports "cycle *k* done"; the supervisor timestamps arrival
  against `epoch + (k+1) × period`. A 1 ms watchdog timer catches cycles
  that never report at all (starvation). Measurement is independent of
  the loop being measured.
- When a **critical** loop misses twice in a row, the supervisor
  **demotes the offending threads** below it, even in another process.
  If the loop keeps missing, offenders drop to the minimum priority.
- If that still isn't enough, or the offender can't be identified, or
  the critical process dies, it commands a **safe state**, and boosts the
  critical thread just below itself so the thread actually gets the CPU
  to carry it out.
- Every cycle and action goes to a **black-box CSV log**, viewable live
  from the host.

The demo device is a **simulated syringe infusion pump** (`dw-pump`): a
10 ms dose loop with a motor speed limit, so missed cycles show up as
measurable under-dosing, and a safe state that stops the motor and
latches an alarm. A separate runaway process (`dw-rogue`) at a higher
priority plays the bug.

Three scenarios, each with three separate QNX processes:
1. Guard off: the dose error grows.
2. Guard on: the supervisor demotes the rogue process and the dose loop
   recovers.
3. Guard on, rogue not identifiable: the pump is driven to its safe
   state instead of silently under-dosing.

## How we built it

- C11 for QNX 8.0, built with `qcc -Vgcc_ntox86_64`, run in the QNX 8.0
  QEMU target image.
- **Policy vs mechanism.** `src/dw_core.c` (pure C, no QNX calls)
  decides when a cycle is a miss and when to throttle, escalate or go to
  the safe state. It has 39 host unit checks. `src/supervisor.c` carries
  those decisions out with QNX kernel calls.
- **QNX message passing all the way through.** In-process workers use
  pulses. Other processes find the supervisor with `name_open()` and
  talk to it with synchronous `MsgSend()`, and the reply carries
  commands back (run / safe state). The supervisor's channel is
  fixed-priority, so a low-priority client can never drag the guard's
  own priority down. Sender identity comes from the kernel
  (`struct _msg_info`), not from the client.
- **Enforcement across processes** with `SchedSet(pid, tid, …)`.
- Host tooling: `host/render.py` gives a summary, a jitter plot, and a
  live `--follow` view streamed over SSH from the guest.

## Where to verify the QNX use (file:line)

| What | File:line |
|---|---|
| Fixed-priority channel + disconnect notification | `src/supervisor.c:700-703` (`ChannelCreate`) |
| Publish the service name | `src/supervisor.c:593-599` (`dispatch_create_channel`, `name_attach`) |
| Receive loop: pulses, `_IO_CONNECT` handshake, messages | `src/supervisor.c:419-486` (`MsgReceive`, `MsgReply`, `MsgError`) |
| Client death → LOST / safe state | `src/supervisor.c:322` (`_PULSE_CODE_DISCONNECT`, `ConnectDetach`), `:473` |
| Kernel-reported pid/tid at registration | `src/supervisor.c:219-264` (`struct _msg_info`) |
| Demote a thread in another process | `src/supervisor.c:61` (`SchedSet`) |
| Demote an uncooperative process by pid | `src/supervisor.c:69-84` (`SchedSet` tid walk) |
| Rescue boost of critical threads | `src/supervisor.c:127-146` (`SchedSet` / `pthread_setschedparam`) |
| 1 ms watchdog delivered as a pulse | `src/supervisor.c:732-734` (`SIGEV_PULSE_INIT`, `timer_create`) |
| Client side: `name_open` + `MsgSend` | `src/dw_client.c:27`, `:45`, `:78` |
| In-process completion pulses | `src/threads.c:55` (`MsgSendPulse`) |
| Pump safe state on supervisor command / supervisor loss | `examples/pump.c:97-101` |
| Build for QNX | `Makefile` (`qcc -Vgcc_ntox86_64`) |
| Why no APS on 8.0 (with doc links) | `docs/design-8.0.md` |

## Challenges we ran into

- **Adaptive Partitioning isn't in QNX 8.0.** We had to redesign the
  enforcement after confirming it in three places in the 8.0 docs.
- **Emulation noise.** Under QEMU/TCG, host stalls freeze the guest while
  its clock runs, so deadlines expire with no thread at fault. Our
  numbers vary run to run (guard off 22.6–54.3 %, guard on 4.7–20.5 %
  control-loop misses in local mode), so we report the improvement
  (2–5× fewer misses, rogue throttled every time) rather than a single
  percentage, and set the safe-state thresholds above the stalls.
- **A starved thread can't obey a command.** Telling the pump to stop is
  useless if the pump never runs. So the safe state also boosts the
  critical thread's priority so it can act.
- **Priority inheritance.** A plain QNX channel makes the receiver take
  the sender's priority, which would have let a low-priority client
  pull the guard below the offender. We use a fixed-priority channel.

## Accomplishments that we're proud of

- A clear, measured improvement on real QNX 8.0 (local mode): control
  loop misses cut several-fold, every run, with the action logged.
- Guarding across process boundaries with nothing but QNX primitives,
  and a client API small enough to add to an existing loop in four
  lines.
- A safe-state path that still works when the offender can't be
  demoted.

## What we learned

How QNX's priority-driven message passing really behaves (inheritance,
pulses vs messages, disconnect notification); how much timing noise
emulation adds; and that a guard is only as good as its evidence, which
is why the black box came first.

## What's next for DeadlineWatch

- Run on real hardware (Raspberry Pi 5) for clean timing numbers.
- Find offenders without registration by scanning `/proc` thread
  priorities and CPU time.
- Try `SCHED_SPORADIC` as a per-thread CPU budget for offenders, the
  closest 8.0 analogue to APS.
- Per-task policies from a config file (thresholds, safe-state actions)
  and a hardware-watchdog hand-off.

## Build and run (QNX version, target, toolchain)

QNX SDP 8.0 / QNX OS 8.0; target: QEMU x86_64 Quick Start Target Image;
toolchain: `qcc -Vgcc_ntox86_64`. Steps: see the README "Build" and
"Run" sections; `sh scripts/demo.sh all` runs every scenario.

---

# 3-minute video script (screen recording of the real QEMU run only)

Rules to respect: under 3:00, public on YouTube, shows it running on
QNX 8.0, no copyrighted music or third-party trademarks without
permission. No AI-generated footage.

Layout for the main shots: left half, the guest console over SSH
(`deadline-watch` live view on top, `dw-pump` output below, e.g. two
tmux panes); right half, the host `render.py --follow` view.

| Time | Shot | Voice-over (suggested) |
|---|---|---|
| 0:00–0:15 | Title card: "DeadlineWatch: a deadline guard for QNX 8.0". Then `uname -a` in the guest showing QNX 8.0. | "In an infusion pump, a control loop that runs late isn't slow. It's a failure. DeadlineWatch watches those deadlines on QNX 8.0 and steps in." |
| 0:15–0:35 | Diagram (one slide): supervisor ↔ pump ↔ rogue, three processes, `name_open` / `MsgSend`, ladder THROTTLE → ESCALATE → SAFE STATE. | "Three separate processes. The pump's 10 ms dose loop reports every cycle to the supervisor, which judges it against its deadline." |
| 0:35–1:05 | **Scenario A, guard off.** Start the supervisor `--guard=off`, then `dw-pump`; the dose error stays near 0 %. Start `dw-rogue`: the misses/s bars fill and the dose error climbs negative. | "Here's a runaway process at a higher priority. With the guard off, the dose loop starves, and the pump falls behind the prescription." |
| 1:05–1:45 | **Scenario B, guard on.** Same steps. As the rogue starts: THROTTLE line, offenders@prio drops 25 → 5, the bars empty, the dose error stops growing. Briefly show the log rows REGISTER, MISS, THROTTLE. | "Guard on. Two misses in a row, and the supervisor demotes the rogue's threads in the other process with SchedSet. The pump recovers, and every step is in the black box." |
| 1:45–2:20 | **Scenario C, rogue unregistered.** `dw-rogue --register=off`. Nothing to demote: `*** SAFE STATE ***` in the supervisor; the pump prints "ALARM: SAFE STATE (motor stopped by guard)". | "If the offender can't be identified, the guard doesn't hope for the best. It boosts the pump's thread so it can act, and commands the safe state: motor stopped, alarm latched." |
| 2:20–2:40 | Host: `render.py` summary + jitter PNG for A vs B; quick flash of the measured local-mode table. | "Measured on QNX 8.0 in QEMU: the guard cuts control-loop misses two to five times, every run." |
| 2:40–2:55 | Code tour (fast): `dw_client` four-line loop; `src/supervisor.c` `ChannelCreate(_NTO_CHF_FIXED_PRIORITY)`, `name_attach`, `SchedSet`. | "It's all QNX primitives: fixed-priority channels, name_attach, pulses, SchedSet. Any loop can opt in with four lines." |
| 2:55–3:00 | End card: repo URL, "Robotics & Medical". | "DeadlineWatch. Open source, MIT." |

Recording checklist:
- Build this version in the guest; run `sh scripts/demo.sh all` once
  first and commit `docs/results/` (evidence + real numbers for the
  voice-over).
- Record at 1080p; enlarge the terminal font; keep each scenario's
  console output on screen long enough to read.
- With the guard off, everything below priority 25 (including the SSH
  shell and the pump's status printer) gets only the rogue's 15 % slack,
  so the terminal can lag. That is the failure being demonstrated;
  narrate it rather than cut it.
- If a scenario does not behave as the script says, say what actually
  happened, or tune it (`--safe-miss-pct`, `--duty`) and re-record. Do
  not edit numbers.
