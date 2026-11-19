# Results

All numbers here were measured on **QNX 8.0 (`QNX qnxqemu 8.0.0 x86pc
x86_64`) in QEMU with TCG software emulation**, by the
local-mode build of DeadlineWatch (supervisor + control + sensor + rogue
threads in one process). Nothing is projected or simulated.

> **Status of this page.** The local-mode numbers below come from the
> 18 Nov 2026 runs of the *previous* version of the code (before the
> cross-process/system mode, safe state and rogue duty fix were added).
> The raw CSV logs from those runs were kept on the build machine and are
> not in this repository. Everything marked **TO RE-RECORD** must be run
> again on QNX with this version (`sh scripts/demo.sh all`) and the logs
> committed to [`docs/results/`](results/).

## Set-up of the recorded runs

- 30 s per run, control loop 10 ms @ prio 20, sensor 50 ms @ prio 15,
  supervisor @ prio 30, watchdog tick 1 ms, grace 2 ms.
- Rogue: 2 threads @ prio 25, set to 85 % duty of
  every 10 ms window, demoted to prio 5 on THROTTLE, then to the minimum
  on ESCALATE.
- Note: the previous code computed the rogue's burn time in whole
  milliseconds, so "85 %" was really **80 %** (8 ms of 10). This version
  computes it in microseconds; the next recording will be a true 85 %.
- Binary compiled in the guest with `cc -Wall -Wextra -O2` (no warnings).
- Miss = completion after the deadline, a skipped cycle, or a watchdog
  starvation (no completion by deadline + 2 ms). In every run, CYCLE
  rows + MISS rows = 3000 = 30 s / 10 ms.

## Control-loop deadline misses (recorded)

| Run | Guard | Control misses | Sensor misses | Throttle |
|---|---|---|---|---|
| v1 rogue on | off | 870 of 2234 cycles (38.9 %), worst 148.6 ms | 12 (2.0 %) | none |
| v1 rogue on | on | 272 of 2765 (9.8 %), worst 46.5 ms | 0 (0.0 %) | 1, at t = 23 ms; rogue 25 → 5 → 1 |
| v1 rogue off | on | 19 of 2982 (0.6 %), worst 130.0 ms | 2 (0.3 %) | 1 (a since-fixed bug: it fired with no rogue) |
| v2 rogue on | off | 561 of 2477 (22.6 %), worst 185.5 ms | 7 (1.2 %) | none |
| v2 rogue on | on | 135 of 2886 (4.7 %), worst 70.7 ms | 1 (0.2 %) | 1, at t = 22 ms; rogue 25 → 5 → 1 |
| v2 rogue off | on | 4 of 2997 (0.1 %), worst 31.1 ms | 0 (0.0 %) | none |

Two more guard-off / guard-on pairs gave 35.5 % / 19.6 % and 54.3 % /
20.5 %. Across the four pairs: **guard off 22.6–54.3 %, guard on
4.7–20.5 %, rogue off 0.1–0.6 %.**

## How to read these numbers

- **The guard works every time:** it throttles the rogue within the first
  ~25 ms and cuts control-loop misses roughly 2–5×. The sensor (lower
  priority, not enforced) also improves.
- **The absolute percentages move run to run.** Under TCG, the guest is
  a process on a shared host: when the host stalls, the guest's clock
  keeps running while no guest thread runs, and deadlines expire with no
  thread at fault. Most residual guard-on misses are watchdog starvation
  (236 of 272 in v1), and the rogue-off baseline shows the same pattern
  with no rogue at all. So the honest claim is the improvement, not a
  fixed percentage, and not zero.
- Real hardware should give cleaner numbers; none has been tested.

## TO RE-RECORD with this version (on QNX)

Run `sh scripts/demo.sh all` in the guest (as root). It writes the logs
and console output into `docs/results/`. Then fill in:

| Scenario | Expected shape | Measured |
|---|---|---|
| local, rogue off | under 1 % misses | TO RE-RECORD |
| local, guard off | tens of % misses | TO RE-RECORD |
| local, guard on | several-fold fewer, THROTTLE logged | TO RE-RECORD |
| pump A: 3 processes, guard off | pump dose error grows negative (under-dosing) | TO RE-RECORD |
| pump B: guard on, rogue registered as demotable | THROTTLE demotes the other process; dose error stops growing | TO RE-RECORD |
| pump C: guard on, rogue unregistered | nothing to demote → SAFE_STATE; pump stops motor, latches alarm | TO RE-RECORD |
| pump C': as C plus `--demote-pid=<rogue pid>` | THROTTLE of an uncooperative process by pid | TO RE-RECORD |

Also capture: one screenshot of the live supervisor view, one of
`render.py --follow` on the host, and the PNG from
`python3 host/render.py docs/results/pump-B-guard-on.csv --out docs/results/pump-B.png`.
