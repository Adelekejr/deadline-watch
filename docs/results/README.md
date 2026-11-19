# docs/results — raw evidence from QNX runs

This folder is where real run logs are committed (the top-level
`.gitignore` ignores `*.csv` everywhere else, but not here).

`sh scripts/demo.sh all`, run inside the QNX 8.0 guest, writes:

- `local-{rogue-off,guard-off,guard-on}.csv` / `.out`
- `pump-{A-guard-off,B-guard-on,C-safe-state}.csv`
- `pump-*-{supervisor,pump,rogue}.out`

**Currently empty: TO RE-RECORD.** The CSV logs behind the numbers in
[`../RESULTS.md`](../RESULTS.md) (18 Nov 2026 runs of the previous
version) were not committed and are not available here.
