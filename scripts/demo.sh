#!/bin/sh
# DeadlineWatch demo runner — run INSIDE the QNX 8.0 guest, as root, from
# the repo directory (binaries built with `make` or `make QCC=cc QCC_TARGET=`).
#
#   sh scripts/demo.sh local     # the three original in-process runs
#   sh scripts/demo.sh pump      # cross-process infusion-pump scenarios A-C
#   sh scripts/demo.sh all
#
# Every run writes its black-box log and console output to docs/results/
# so the evidence can be committed. Nothing here has been run on QNX yet
# by this version of the code; see docs/RESULTS.md.
set -u
OUT=docs/results
DUR=${DUR:-30}
mkdir -p "$OUT"

run_local() {
    echo "== local: rogue off (baseline)"
    ./deadline-watch --rogue=off --duration="$DUR" --log="$OUT/local-rogue-off.csv" \
        > "$OUT/local-rogue-off.out" 2>&1
    echo "== local: rogue on, guard off"
    ./deadline-watch --guard=off --duration="$DUR" --log="$OUT/local-guard-off.csv" \
        > "$OUT/local-guard-off.out" 2>&1
    echo "== local: rogue on, guard on"
    ./deadline-watch --guard=on --duration="$DUR" --log="$OUT/local-guard-on.csv" \
        > "$OUT/local-guard-on.out" 2>&1
    tail -n 6 "$OUT"/local-*.out
}

# One cross-process scenario: supervisor + pump + rogue as THREE processes.
#   $1 name  $2 supervisor flags  $3 rogue flags
scenario() {
    name=$1; sup=$2; rog=$3
    echo "== pump scenario $name: deadline-watch $sup | dw-rogue $rog"
    ./deadline-watch --mode=system --duration=$((DUR + 4)) $sup \
        --log="$OUT/pump-$name.csv" > "$OUT/pump-$name-supervisor.out" 2>&1 &
    sup_pid=$!
    sleep 1
    ./dw-pump --duration="$DUR" > "$OUT/pump-$name-pump.out" 2>&1 &
    pump_pid=$!
    sleep 3                                   # healthy infusion first
    ./dw-rogue --duration=$((DUR - 6)) $rog > "$OUT/pump-$name-rogue.out" 2>&1 &
    wait $pump_pid
    wait $sup_pid
    tail -n 3 "$OUT/pump-$name-pump.out"
    tail -n 6 "$OUT/pump-$name-supervisor.out"
}

run_pump() {
    # A: guard off — the rogue starves the dose loop: under-dosing grows.
    scenario A-guard-off "--guard=off" "--register=on"
    # B: guard on — the rogue registered as demotable: THROTTLE, dose recovers.
    scenario B-guard-on "--guard=on" "--register=on"
    # C: guard on, the rogue is unknown and cannot be demoted: SAFE_STATE,
    #    the pump stops its motor and latches an alarm (fail-safe).
    scenario C-safe-state "--guard=on" "--register=off"
}

case "${1:-all}" in
    local) run_local ;;
    pump)  run_pump ;;
    all)   run_local; run_pump ;;
    *) echo "usage: $0 local|pump|all"; exit 2 ;;
esac
