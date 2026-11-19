# docs/results — raw evidence from QNX runs

Real logs recorded on 19 Nov 2026 on QNX 8.0 in QEMU (TCG software
emulation, no KVM; see `vm-setup.txt`). The top-level `.gitignore`
ignores `*.csv` everywhere else, but not here. `../RESULTS.md` is
generated from these files with `python3 host/results_table.py docs/results`.

`sh scripts/demo.sh all` (or `local`, `pump`, `cpid`), run as root inside
the QNX 8.0 guest, writes:

- `local-{rogue-off-N,d100-guard-{off,on}-rN,d85-guard-{off,on}-rN}.csv` / `.out`
- `pump-{A-guard-off,B-guard-on,C-safe-state,Cpid-demote-pid}-rN.csv`
- `pump-*-rN-{supervisor,pump,rogue}.out` (console of each process)
- `console-*.txt` (one-line summary per run), `run-*.txt` (timestamps, guest clock in UTC)

Other files:

- `pump-timeline-r{1,2,3}.png`, `pump-summary.png`, `local-timeline-r1.png`:
  graphs made by `python3 host/plot_scenarios.py docs/results --all`;
  `pump-B-guard-on-r1-lateness.png` by `host/render.py`.
- `captures/`: console screenshots rendered from the `.out` files.
- `pre-fix/`: the C′ run that exposed the `--demote-pid` new-thread bug,
  kept as evidence.
- `guest-uname.txt`, `guest-pidin-info.txt`, `vm-setup.txt`: the guest and VM.
