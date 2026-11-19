# DeadlineWatch

**A runtime deadline guard for QNX 8.0.** It watches the real-time loops
that registered with it, in its own process or in any other process on
the system, judges every cycle against its deadline, demotes the
offender when a critical loop starts missing, commands a safe state when
that is not enough, and keeps a black-box record of every miss and every
action.

Category: **Robotics & Medical**. Demo scenario: a simulated syringe
infusion pump whose 10 ms dose loop is starved by a runaway process.

## The problem

In a medical device or a robot, one buggy task that hogs the CPU can
silently break another task's deadlines. A missed deadline in an
infusion pump is not a slow page: the motor is not commanded on time, the
dose drifts, and usually nothing records why. QNX gives you the tools to
prevent this (priorities, message passing, precise timers), but nothing
watches whether the deadlines are actually being met at run time.

## What it does

- **Measures** each registered loop independently of the loop itself:
  the loop reports "cycle *k* done", the supervisor timestamps arrival
  against `epoch + (k+1) × period`, and a 1 ms watchdog timer catches
  cycles that never report (starvation).
- **Enforces** a ladder when a *critical* loop misses:
  1. **THROTTLE** after 2 consecutive misses: demote every offender
     thread below the critical loop.
  2. **ESCALATE** after 4 more: offenders to the minimum priority.
  3. **SAFE_STATE** if the critical loop still fails (300 ms continuously
     or 50 % of a 1 s window) or nothing can be demoted, or its process
     dies: tell the critical loop to go to its safe state, and boost it
     just below the supervisor so it gets the CPU to do so. Latched, like
     a real alarm.
- **Records** every cycle and every action in a CSV black box, flushed at
  least once a second, so a run can be watched live and survives Ctrl-C.

## Two modes

**Local mode** (default, `./deadline-watch`): one process with the
supervisor, a 10 ms control loop, a 50 ms sensor and a deliberately
greedy rogue (one thread per online CPU, priority above the control
loop). Both modes were recorded on QNX 8.0 in QEMU (see Results).

**System mode** (`./deadline-watch --mode=system`): the supervisor
publishes the name `deadline-watch` and guards **other processes**. Any
program can opt in with the small client library
([`src/dw_client.h`](src/dw_client.h)):

```c
dw_client_t c;
dw_client_register(&c, "pump-dose", DW_KIND_CRITICAL, 10, 5000);
for (long k = 0; ; ) {
    do_one_control_cycle();
    if (dw_client_beat(&c, k) == DW_CMD_SAFE_STATE)
        enter_safe_state();              /* stop motor, latch alarm */
    k = dw_client_wait_next(&c, k);      /* sleep to the next period */
}
```

Best-effort processes register as `DW_KIND_DEMOTABLE` (the integrator's
contract: "this may be demoted"). A process that does not cooperate at
all can still be demoted with `--demote-pid=PID`. Two example programs
are included:

- [`examples/pump.c`](examples/pump.c) → `dw-pump`: a **simulated**
  syringe infusion pump (not a medical device). 10 ms critical dose loop
  with a motor speed limit, so starved cycles become visible
  **under-dosing**; 50 ms pressure sensor; on safe state it stops the
  motor and latches an alarm. It also fails safe if the supervisor
  disappears.
- [`examples/rogue.c`](examples/rogue.c) → `dw-rogue`: a separate runaway
  process at priority 25, one thread per CPU, 100 % duty by default
  (`--duty=` to change), registered or not
  (`--register=off`).

## How it uses QNX 8.0

| QNX feature | Where |
|---|---|
| Channels + pulses, `MsgReceive` loop | `src/supervisor.c` (`dw_supervisor_run`) |
| `_NTO_CHF_FIXED_PRIORITY` so low-priority clients can't drag the supervisor's priority down | `src/supervisor.c` (`main`, `ChannelCreate`) |
| `name_attach` / `name_open` service discovery, `MsgSend` request/reply | `src/supervisor.c` (`dw_start_system`), `src/dw_client.c` |
| Sender pid/tid from the kernel (`struct _msg_info`), not the client | `src/supervisor.c` (`dw_handle_register`) |
| `_PULSE_CODE_DISCONNECT` when a client dies → LOST / SAFE_STATE | `src/supervisor.c` (`dw_handle_disconnect`) |
| `SchedSet(pid, tid, …)` to demote or boost a thread in another process | `src/supervisor.c` (`dw_target_set_prio`, `dw_demote_pid`, `dw_rescue_boost`) |
| `timer_create` + `SIGEV_PULSE_INIT`: 1 ms watchdog as a pulse | `src/supervisor.c` (`main`) |
| `SCHED_FIFO` priorities, `pthread_setschedparam` | `src/supervisor.c`, `src/threads.c`, examples |

**No Adaptive Partitioning.** The original design used APS CPU budgets.
QNX OS 8.0 does not support APS (8.0 `SchedCtl()` lists only CPU
offlining commands, and the IDE guide says so). Enforcement is therefore
by strict-priority scheduling. Details: [docs/design-8.0.md](docs/design-8.0.md).

## Results (QNX 8.0 in QEMU with TCG software emulation, not real hardware)

Recorded on 19 Nov 2026, 30 s per run, 3 runs per scenario, rogue at
100 % duty on 4 threads. Mean across the 3 runs, range in brackets. All
numbers are generated from the committed logs by `host/results_table.py`.

| Pump scenario (3 processes) | Dose-loop misses during the 20 s attack | Longest outage | Guard action | Final dose error |
|---|---|---|---|---|
| A: guard off | 99.6 % (99.4–99.8) | 20 s, the whole attack | none | −59.6 %, worst −85 %, **no alarm** |
| B: guard on, rogue registered as demotable | 0.7 % (0.2–1.5) | 30–150 ms | THROTTLE 9–10 ms after the first miss | 0.0 % (worst −3.4 %) |
| C: guard on, nothing it can demote | 1.9 % (1.8–1.9) | 310 ms | SAFE_STATE 290 ms after the first miss (3/3): motor stopped, alarm latched | −89.9 % (therapy stopped on purpose, with an alarm) |
| C′: guard on, uncooperative rogue named by `--demote-pid` | 0.5 % (0.3–0.7) | 40–60 ms | THROTTLE by pid, new rogue threads re-demoted | 0.0 % (worst −1.6 %) |

Local mode (one process): guard off 100 % control-loop misses (3/3
runs); guard on 0.3–0.8 % with THROTTLE at 30 ms; rogue off 0.1–0.7 %.

Emulator stalls cause some misses with no thread at fault (one guest-wide
stall lasted 37.7 s and spoiled a run; it is kept in the logs). These
numbers show the mechanism working on the real QNX kernel; they are not
real-hardware timing. Full tables, graphs, method and problems seen:
[docs/RESULTS.md](docs/RESULTS.md).

## Build

Target: **QNX 8.0** (SDP 8.0, `com.qnx.qnx800`), **QEMU** x86_64 Quick
Start Target Image (`com.qnx.qnx800.quickstart.qemu`), toolchain
**`qcc -Vgcc_ntox86_64`** (GCC). Free with a myQNX QNX Everywhere licence.

```sh
source ~/qnx800/qnxsdp-env.sh
make                          # deadline-watch, dw-pump, dw-rogue
# or inside the guest, which ships a compiler:
make QCC=cc QCC_TARGET=
```

Copy the binaries and `config/` into the guest with `scp` over the QEMU
port forward (`scp -P 2222 … qnxuser@127.0.0.1:~/`).

## Run (in the QEMU guest, as root)

Real-time priorities need root; the programs fail loudly instead of
running a misleading demo.

```sh
# Local mode
./deadline-watch                     # guard on, rogue on, 30 s
./deadline-watch --guard=off         # measure only
./deadline-watch --rogue=off         # baseline

# System mode: three separate processes
./deadline-watch --mode=system --duration=40 &
./dw-pump --duration=30 &
sleep 3; ./dw-rogue --duration=20                    # registered: gets THROTTLED
#        ./dw-rogue --register=off --duration=20     # unknown: pump goes to SAFE STATE
#   or:  ./deadline-watch --mode=system --demote-pid=<rogue pid>

# Everything above, logs saved to docs/results/:
sh scripts/demo.sh all
```

Every second the supervisor prints a live view (guard state, offender
priority, a per-task misses-per-second bar), and `dw-pump` prints
prescribed vs delivered volume and dose error. `--help` lists every
option; `config/deadline-watch.conf` holds the defaults.

## Watch and render the log (host)

```sh
# live, while the run is going:
ssh -p 2222 qnxuser@127.0.0.1 tail -f deadline-watch.csv | python3 host/render.py --follow -
# after the run: summary + jitter plot (PNG if matplotlib is installed)
python3 host/render.py deadline-watch.csv --out jitter.png
```

## Host checks (no QNX needed)

```sh
make host-check
```

Compiles every C file with `gcc -Wall -Wextra -Werror` against stub QNX
headers (`tests/qnx-stubs/`, declarations only), runs 43 unit checks of
the deadline policy (`tests/test_core.c`: miss accounting, the
THROTTLE → ESCALATE → SAFE_STATE ladder timing, recovery, lost
clients), and runs the `render.py` self-test. This proves the code is
warning-free and the policy logic is right; it does **not** prove it
runs on QNX.

## File layout

```
src/dw_core.[ch]     deadline accounting + enforcement policy (pure C, host-tested)
src/supervisor.c     main, MsgReceive loop, enforcement mechanism, system mode
src/threads.c        local mode: control, sensor, rogue threads
src/config.c         config file + flags        src/log.c   black-box CSV
src/dw_proto.h       system-mode message protocol
src/dw_client.[ch]   client library for other processes
examples/pump.c      simulated infusion pump (critical client)
examples/rogue.c     separate runaway process (offender)
scripts/demo.sh      runs all scenarios in the guest, saves logs
host/render.py       summary, jitter plot, live --follow view, --self-test
tests/               policy unit tests + stub QNX headers for host compile
docs/                design-8.0.md, RESULTS.md, results/
```

## Honest limits

- Everything has been run on QNX 8.0, but only in QEMU with TCG
  software emulation on a shared host. No real hardware has been tested.
- Priority demotion is not a CPU budget (that was APS, gone in 8.0).
- A fully starved thread cannot act on any command. The rescue boost
  helps registered critical threads; a real device would also have an
  independent hardware watchdog.
- The pump is a teaching simulation, not medical software.
- QEMU/TCG stalls cause misses with no thread at fault. Most were under
  ~200 ms (the 300 ms safe-state threshold is above that), but two
  guest-wide stalls of 6.2 s and 37.7 s were seen, plus one guest hang
  that needed a VM reset. See docs/RESULTS.md.
- The live view costs the supervisor one short `write()` per second;
  use `--live=off` for the cleanest timing measurements. (The recorded
  runs had the live view on.)
- Workload sizes (`control_work_iters`, `--work`) are stand-ins; calibrate
  on the target so the control work takes about 1 ms.

MIT licence.
