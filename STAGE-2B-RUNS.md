# DeadlineWatch stage 2b — live guest runs (2026-11-18, ~07:00–07:10 WAT)

All numbers below are REAL, from black-box logs produced by runs in the
QNX 8.0 guest under QEMU/TCG. No numbers are invented or projected.

## Guest access (guest LEFT RUNNING, detached)

- QEMU PID 4488, inside tmux session `qnx` (started with `setsid tmux
  new-session -d -s qnx ...`, survives exec-session teardown).
- Serial console: `tmux attach -t qnx` (or `tmux capture-pane -t qnx -p`);
  console log (partial, from pipe-pane): `/home/hatch/qnx-console.log`
- SSH: `sshpass -p <guest password> ssh -p 2222 -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null qnxuser@127.0.0.1` (the stock QEMU
  image's default password; not printed here)
- Guest: `QNX qnxqemu 8.0.0 2026/06/05-16:26:03EDT x86pc x86_64`,
  IP 10.0.2.15 (vtnet0), sshd LISTEN on tcp4+tcp6 :22.
- QEMU monitor socket: `/tmp/qnx-mon.sock`
  (`echo quit | socat - UNIX-CONNECT:/tmp/qnx-mon.sock` kills it abruptly;
  prefer a clean guest `shutdown`).
- Boot took ~7 min to the login prompt under TCG. QEMU was reinstalled
  first (`sudo apt-get install -y qemu-system-x86 sshpass`) — a restart had
  wiped it again.
- Disk verified intact before boot: `output/disk-qemu` = 50,826,834,432 bytes.

## Hello-world

- Host `qcc` was BLOCKED at the time (the QNX licence file had been
  wiped by a restart; `qcc` exited 129 "no valid license"). It has
  since been restored and host `qcc` works again.
- Workaround that worked: the guest HAS its own compiler
  (`/system/bin/cc`, `gcc`, `g++`). `hello.c` was scp'd in, compiled
  in-guest with `cc`, and ran. Exact output:
  `Hello from QNX 8.0 (x86_64)!`

## Binary transfer (TESTED, worked first try)

- Method: **scp via sshpass** (no HTTP fallback needed).
  `sshpass -p <guest password> scp -P 2222 -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null deadline-watch
  deadline-watch.conf qnxuser@127.0.0.1:~/`
  then `chmod +x ~/deadline-watch`. Verified in guest:
  `ELF shared object, 64-bit LSB x86-64, dynamic
  (/usr/lib/ldqnx-64.so.2)`, 32,312 bytes, `--help` prints the usage.
- Guest copies: `~/deadline-watch`, `~/deadline-watch.conf`,
  `~/rogue-off.csv`, `~/guard-on.csv`, `~/rogue-off-baseline.csv`,
  `~/hello`, `~/hello.c`.
- Host copies of the logs: `host/logs/{rogue-off,guard-on,
  rogue-off-baseline}.csv` (scp'd back the same way).

## Runs (each 30 s, 2 rogue threads, 85% duty, prio 25 → demote 5)

Config file note: the binary prints `unknown key 'rogue_threads'`,
`unknown key 'guard_on'`, `unknown key 'rogue_on'` for
`config/deadline-watch.conf` — those keys are ignored by this build.
All runs below set the mode explicitly with `--guard=` / `--rogue=`
flags, so the runs are as labelled anyway. (Minor bug to fix: config
parser vs shipped config keys disagree.)

### Run 1 — rogue ON, guard OFF (`--guard=off --rogue=on`, log rogue-off.csv)
- control: 2234 cycles, **870 misses (38.9%)**, worst 148,602 us, mean −4,658 us
- sensor: 590 cycles, 12 misses (2.0%), worst 108,649 us
- throttle events: 0, rogue final priority: 25
- Miss split: 104 late completions (CYCLE) + 766 watchdog starvation (MISS).
- Matches the README's "40%+" expectation for guard-off.

### Run 2 — rogue ON, guard ON (`--guard=on --rogue=on`, log guard-on.csv)
- control: 2765 cycles, **272 misses (9.8%)**, worst 46,522 us, mean −8,039 us
- sensor: 600 cycles, **0 misses (0.0%)**
- throttle events: 1 (at t=23 ms), rogue demoted 25 → 5, then ESCALATE → 1
- Miss split: 36 late completions + 236 watchdog starvation.
- Misses CONTINUE after the throttle (234 watchdog + 36 cycle after t=23 ms).

### Run 3 — rogue OFF baseline (`--guard=on --rogue=off`, log rogue-off-baseline.csv)
- control: 2982 cycles, **19 misses (0.6%)**, worst 130,018 us, mean −7,941 us
- sensor: 598 cycles, 2 misses (0.3%)
- throttle events: 1 (at t=22 ms!) even with 0 rogue threads — the
  supervisor demotes "the rogue" on 2 consecutive control misses whether
  or not a rogue exists (cosmetic logic quirk).

## Honest reading

- The guard WORKS: 38.9% → 9.8% control misses, sensor 2.0% → 0.0%,
  throttle logged, rogue demoted to prio 1.
- It does NOT reach the README's "0" under TCG: the residual 9.8% is
  mostly watchdog starvation (236/272), and the rogue-off baseline shows
  the same pattern (18 watchdog misses, worst spike 130 ms) with no rogue
  at all — i.e. TCG/host stalls freeze the guest while its virtual clock
  keeps ticking, so deadlines expire with no thread at fault. The demo
  story should say "guard cuts misses ~4× and throttles the rogue" rather
  than "0 misses", unless a longer/calibrated run on real hardware shows 0.
- `host/render.py` confirms the same summaries (its "worst" differs
  slightly — it takes max over CYCLE rows only, the binary includes MISS
  rows: e.g. rogue-off worst 141,283 us vs binary 148,602 us).
- Fewer cycles than the ideal 3000/30 s in runs 1–2 (2234, 2765) vs 2982
  in the baseline — the loop runs slower than wall-clock under TCG.

## Bug-fix verification pass (2026-11-18, ~07:11–07:15 WAT)

Two fixes applied by the parent, verified here against the fixed code
running in the guest. Host `qcc` is still blocked (licence file gone
after a restart — NOT restored, out of scope), so the fixed sources were
compiled IN-GUEST.

### In-guest compile (exact command that worked)

Sources scp'd to `~/dw2/src/` (+ `config/deadline-watch.conf` to
`~/dw2/config/`), md5-verified against the host copies, then:

```
cd ~/dw2 && cc -Wall -Wextra -O2 -o deadline-watch src/supervisor.c src/threads.c src/config.c src/log.c
```

First try, no warnings, no link flags needed (QNX libc carries pthread).
Binary: 32,096 bytes, ELF 64-bit, `--help` prints the usage. Guest
copies: `~/dw2/deadline-watch`, `~/dw2/{guard-on-v2,guard-off-v2,
rogue-off-v2,flip-test}.csv` (+ `.out`/`.err` captures).

### Fix 1 — config parser strips inline `#` comments: VERIFIED

- No-flag run (shipped config drives the mode), log `guard-on-v2.csv`:
  stdout printed `config: guard=on rogue=on duration=30s
  log=guard-on-v2.csv`, and **stderr was completely empty** — no
  `unknown key` warnings. The old build printed `unknown key
  'rogue_threads'`, `'guard_on'`, `'rogue_on'` here.
- The no-flag run alone cannot prove the file is read: the compiled-in
  defaults are identical to the shipped config values. So a flipped
  copy was tested: `guard_on = 0`, `duration_s = 5` in `/tmp/flip.conf`,
  run with `--config=/tmp/flip.conf`. Printed `config: guard=off
  rogue=on duration=5s`, ran 5 s, summary `guard: off, duration: 5s`
  (log `flip-test.csv`: 426 control cycles, 79 misses, 18.5%).
  The file values are genuinely honoured.

### Fix 2 — no THROTTLE/ESCALATE with no rogue: VERIFIED

- Rogue-off 30 s run (`--guard=on --rogue=off`, log `rogue-off-v2.csv`):
  summary `throttle events: 0`, and the log contains **0 THROTTLE rows
  and 0 ESCALATE rows** (the only lines matching are the `# events:`
  header comment listing possible event names). The old build logged 1
  THROTTLE at t=22 ms with 0 rogue threads.
- Guard-off v2 log also has 0 THROTTLE / 0 ESCALATE rows, as expected.

### Re-runs (each 30 s, 2 rogue threads, 85% duty, prio 25) — v2 numbers

Logs: `host/logs/{guard-off-v2,guard-on-v2,rogue-off-v2}.csv`.
Numbers below are the binary's own SUMMARY rows (same accounting as
the v1 runs: cycles = completed CYCLE rows; misses = late completions
+ watchdog MISS rows; in every run CYCLE rows + MISS rows = 3000).

| run | control cycles | control misses | sensor misses | throttle |
|---|---|---|---|---|
| guard OFF (v2) | 2477 | 561 (22.6%), worst 185,525 us | 7 (1.2%) | 0 |
| guard ON (v2) | 2886 | 135 (4.7%), worst 70,724 us | 1 (0.2%) | 1 (t=22 ms), rogue 25→5→1 |
| rogue OFF baseline (v2) | 2997 | 4 (0.1%), worst 31,059 us | 0 (0.0%) | 0 |

v1 → v2 comparison (same 30 s, same settings):

| run | v1 control misses | v2 control misses |
|---|---|---|
| guard OFF | 870 (38.9%) | 561 (22.6%) |
| guard ON | 272 (9.8%) | 135 (4.7%) |
| rogue OFF baseline | 19 (0.6%) | 4 (0.1%) |

**The numbers moved — all three down, by roughly half.** This is NOT
explained by the fixes: fix 1 only changes config parsing (the v1 runs
set the mode with flags, so parsing was irrelevant to their numbers)
and fix 2 only suppresses a log event. The plausible cause is host
load under TCG varying run to run: the residual misses are mostly
watchdog starvation from host stalls, and the v2 runs completed more
cycles (2477/2886/2997 vs 2234/2765/2982), i.e. the guest got more CPU
this time. The 5 s flip-test guard-off snippet agrees (18.5%).
The guard still works — 22.6% → 4.7%, about 5× fewer misses — but the
absolute percentages are NOT stable run to run under TCG. The demo
story should quote the improvement (guard cuts misses several-fold and
throttles the rogue), not a fixed percentage. The README demo table
currently shows the v1 numbers (38.9% / 9.8% / 0.6%) and is stale
against these v2 runs — it needs a decision, not a silent overwrite.
