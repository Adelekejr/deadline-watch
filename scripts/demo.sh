#!/bin/sh
# DeadlineWatch demo runner — run INSIDE the QNX 8.0 guest, as root, from
# the directory holding deadline-watch, dw-pump, dw-rogue and config/.
#
#   sh scripts/demo.sh local     # in-process runs (rogue off / guard off / guard on)
#   sh scripts/demo.sh pump      # cross-process infusion-pump scenarios A, B, C, C'
#   sh scripts/demo.sh all
#   sh scripts/demo.sh cpid      # only scenario C' (--demote-pid)
#
# Environment: DUR (seconds per run, default 30), REPS (repetitions of
# each rogue-on run, default 3), DUTY (rogue duty for the pump
# scenarios, default 100 = a process stuck in a busy loop), LOCAL_DUTY
# (rogue duty for local mode, default 100; 85 was the old default and
# leaves enough slack on a 4-vCPU guest that the loop barely misses).
#
# Logs (*.csv) and console captures (*.out) go to docs/results/.
set -u
OUT=docs/results
DUR=${DUR:-30}
REPS=${REPS:-3}
DUTY=${DUTY:-100}
LOCAL_DUTY=${LOCAL_DUTY:-100}
mkdir -p "$OUT"

local_run() {   # $1 name  $2.. flags
    name=$1; shift
    echo "== local $name: deadline-watch --rogue-duty=$LOCAL_DUTY $*"
    ./deadline-watch --duration="$DUR" --rogue-duty="$LOCAL_DUTY" \
        --log="$OUT/local-$name.csv" "$@" \
        > "$OUT/local-$name.out" 2>&1
    grep -E "^control|^sensor|throttle events" "$OUT/local-$name.out"
}

run_local() {
    local_run rogue-off --rogue=off
    r=1
    while [ $r -le "$REPS" ]; do
        local_run "d$LOCAL_DUTY-guard-off-r$r" --guard=off
        local_run "d$LOCAL_DUTY-guard-on-r$r" --guard=on
        r=$((r + 1))
    done
}

# One cross-process scenario: supervisor, pump and rogue as THREE processes.
#   $1 name  $2 supervisor flags  $3 rogue flags
# Timeline: supervisor at 0 s, pump at 1 s, rogue from 4 s for DUR-10 s,
# so every run has healthy, attacked and recovery phases.
scenario() {
    name=$1; sup=$2; rog=$3
    echo "== pump $name: deadline-watch $sup | dw-rogue --duty=$DUTY $rog"
    ./deadline-watch --mode=system --duration=$((DUR + 6)) $sup \
        --log="$OUT/pump-$name.csv" > "$OUT/pump-$name-supervisor.out" 2>&1 &
    sup_pid=$!
    sleep 1
    ./dw-pump --duration="$DUR" > "$OUT/pump-$name-pump.out" 2>&1 &
    pump_pid=$!
    sleep 3
    ./dw-rogue --duty="$DUTY" --duration=$((DUR - 10)) $rog \
        > "$OUT/pump-$name-rogue.out" 2>&1
    wait $pump_pid
    wait $sup_pid
    grep -E "^pump|throttle events" "$OUT/pump-$name-supervisor.out"
    tail -n 1 "$OUT/pump-$name-pump.out"
}

# C': the rogue does not register; the supervisor is told its pid.
scenario_pid() {
    name=$1
    echo "== pump $name: deadline-watch --demote-pid=<rogue> | dw-rogue --register=off"
    ./dw-rogue --duty="$DUTY" --register=off --start-delay=4 \
        --duration=$((DUR - 10)) > "$OUT/pump-$name-rogue.out" 2>&1 &
    rog_pid=$!
    ./deadline-watch --mode=system --duration=$((DUR + 6)) --guard=on \
        --demote-pid=$rog_pid --log="$OUT/pump-$name.csv" \
        > "$OUT/pump-$name-supervisor.out" 2>&1 &
    sup_pid=$!
    sleep 1
    ./dw-pump --duration="$DUR" > "$OUT/pump-$name-pump.out" 2>&1 &
    pump_pid=$!
    wait $rog_pid
    wait $pump_pid
    wait $sup_pid
    grep -E "^pump|throttle events" "$OUT/pump-$name-supervisor.out"
    tail -n 1 "$OUT/pump-$name-pump.out"
}

run_pump() {
    r=1
    while [ $r -le "$REPS" ]; do
        # A: guard off: the rogue starves the dose loop; under-dosing grows.
        scenario "A-guard-off-r$r" "--guard=off" "--register=on"
        # B: guard on: the rogue registered as demotable: THROTTLE, dose recovers.
        scenario "B-guard-on-r$r" "--guard=on" "--register=on"
        # C: guard on, rogue unregistered: nothing to demote: SAFE_STATE.
        scenario "C-safe-state-r$r" "--guard=on" "--register=off"
        # C': as C, but the supervisor is given the rogue's pid.
        scenario_pid "Cpid-demote-pid-r$r"
        r=$((r + 1))
    done
}

run_cpid() {     # only C' (after a --demote-pid change)
    r=1
    while [ $r -le "$REPS" ]; do
        scenario_pid "Cpid-demote-pid-r$r"
        r=$((r + 1))
    done
}

case "${1:-all}" in
    local) run_local ;;
    pump)  run_pump ;;
    cpid)  run_cpid ;;
    all)   run_local; run_pump ;;
    *) echo "usage: $0 local|pump|cpid|all"; exit 2 ;;
esac
