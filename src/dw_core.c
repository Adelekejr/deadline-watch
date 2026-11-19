/*
 * DeadlineWatch — deadline accounting and enforcement policy (pure C).
 *
 * Deadline accounting: cycle k of a task with period P has deadline
 * epoch + (k+1)*P. A cycle is a miss when its completion report arrives
 * after its deadline, when the task skips it, or when the watchdog finds
 * it overdue (deadline + grace) with no report at all (starvation). Each
 * cycle is judged exactly once: `expected_cycle` only moves forward.
 *
 * Enforcement ladder (guard on), driven by CRITICAL tasks only:
 *   1. THROTTLE   a critical task misses miss_threshold (2) cycles in a
 *                 row: demote every enforcement target below the
 *                 critical tasks.
 *   2. ESCALATE   it still misses 2*miss_threshold in a row after that:
 *                 demote the targets to the minimum priority (once).
 *   3. SAFE_STATE enforcement is exhausted (escalated, or there is
 *                 nothing to demote) and a critical task is still not
 *                 meeting its deadlines: it has failed continuously for
 *                 safe_after_ms, OR (if safe_miss_pct > 0) it missed at
 *                 least safe_miss_pct % of its cycles in the last 1 s
 *                 window. Also when a critical task's process dies.
 *                 The mechanism then commands the critical tasks into
 *                 their safe state (infusion-pump example: stop the
 *                 motor, latch an alarm). Latched for the rest of the
 *                 run, like a real alarm.
 * RECOVER is logged once the triggering critical task is clean for
 * recover_cycles (50) cycles after a THROTTLE. Targets stay demoted.
 */
#include <stdio.h>
#include <string.h>

#include "dw_core.h"

int64_t dw_diff_us(const struct timespec *a, const struct timespec *b)
{
    return (int64_t)(b->tv_sec - a->tv_sec) * 1000000L
         + (b->tv_nsec - a->tv_nsec) / 1000L;
}

void dw_add_ms(const struct timespec *base, long ms, struct timespec *out)
{
    out->tv_sec = base->tv_sec + ms / 1000;
    out->tv_nsec = base->tv_nsec + (ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

static void dw_core_log(dw_core_t *c, const struct timespec *now,
                        const char *who, const char *event, long cycle,
                        long late, long worst, const char *detail)
{
    if (c->log)
        c->log(c->log_user, now, who, event, cycle, late, worst, detail);
}

void dw_core_set_safe_miss_pct(dw_core_t *c, int pct)
{
    c->safe_miss_pct = (pct > 0 && pct <= 100) ? pct : 0;
}

void dw_core_init(dw_core_t *c, int guard_on, int safe_after_ms,
                  dw_log_fn log, void *log_user)
{
    memset(c, 0, sizeof(*c));
    c->guard_on = guard_on;
    c->miss_threshold = 2;
    c->recover_cycles = 50;
    c->grace_ms = 2;
    c->safe_after_ms = safe_after_ms > 0 ? safe_after_ms : 0;
    c->trigger_task = -1;
    c->log = log;
    c->log_user = log_user;
}

int dw_core_add_task(dw_core_t *c, const char *name, int role, int period_ms,
                     const struct timespec *epoch)
{
    if (period_ms < 1 || !name ||
        (role != DW_ROLE_CRITICAL && role != DW_ROLE_TRACKED))
        return -1;
    for (int i = 0; i < DW_MAX_TASKS; i++) {
        dw_task_t *t = &c->task[i];
        if (t->active || t->name[0])
            continue;   /* slots are never reused: log rows stay unambiguous */
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", name);
        t->role = role;
        t->period_ms = period_ms;
        t->epoch = *epoch;
        t->active = 1;
        return i;
    }
    return -1;
}

static void dw_deadline(const dw_task_t *t, long cycle, struct timespec *out)
{
    dw_add_ms(&t->epoch, (cycle + 1) * t->period_ms, out);
}

static int dw_core_safe(dw_core_t *c, int ti, const struct timespec *now,
                        const char *why)
{
    (void)now;
    if (c->safe_state || !c->guard_on ||
        (c->safe_after_ms == 0 && c->safe_miss_pct == 0))
        return DW_ACT_NONE;   /* already latched, measure-only, or disabled */
    c->safe_state = 1;
    c->safe_reason = why;
    c->trigger_task = ti;
    c->trigger_cycle = ti >= 0 ? c->task[ti].expected_cycle : 0;
    return DW_ACT_SAFE_STATE;
}

/* 1 s tumbling window of judged cycles per task; the miss-rate safe
   criterion is evaluated when a window closes. */
static int dw_core_window(dw_core_t *c, int ti, int missed,
                          const struct timespec *now)
{
    dw_task_t *t = &c->task[ti];
    long win = 1000L / t->period_ms;
    if (win < 1)
        win = 1;
    t->win_judged++;
    if (missed)
        t->win_missed++;
    if (t->win_judged < win)
        return DW_ACT_NONE;
    int acts = DW_ACT_NONE;
    if (c->guard_on && t->role == DW_ROLE_CRITICAL && c->safe_miss_pct > 0 &&
        (c->escalated || c->targets == 0) &&
        t->win_missed * 100 >= (long)c->safe_miss_pct * t->win_judged)
        acts = dw_core_safe(c, ti, now, "miss-rate");
    t->win_judged = t->win_missed = 0;
    return acts;
}

/* Called after every miss of task ti. */
static int dw_core_check(dw_core_t *c, int ti, const struct timespec *now)
{
    dw_task_t *t = &c->task[ti];
    if (!c->guard_on || t->role != DW_ROLE_CRITICAL || c->safe_state)
        return DW_ACT_NONE;
    if (t->consecutive_misses < c->miss_threshold)
        return DW_ACT_NONE;

    long failing_ms = t->consecutive_misses * (long)t->period_ms;
    int exhausted = c->escalated || c->targets == 0;

    if (c->targets > 0 && !c->throttled) {
        c->throttled = 1;
        c->recovered = 0;
        c->throttle_events++;
        c->trigger_task = ti;
        c->trigger_cycle = t->expected_cycle;
        t->consecutive_misses = 0;   /* fresh count for the escalation check */
        t->clean_cycles = 0;
        return DW_ACT_THROTTLE;
    }
    if (c->targets > 0 && !c->escalated &&
        t->consecutive_misses >= 2L * c->miss_threshold) {
        c->escalated = 1;
        c->trigger_task = ti;
        c->trigger_cycle = t->expected_cycle;
        return DW_ACT_ESCALATE;
    }
    if (exhausted && c->safe_after_ms > 0 && failing_ms >= c->safe_after_ms)
        return dw_core_safe(c, ti, now, "continuous-failure");
    return DW_ACT_NONE;
}

static int dw_core_miss(dw_core_t *c, int ti, long cycle,
                        const struct timespec *deadline,
                        const struct timespec *now, const char *why)
{
    dw_task_t *t = &c->task[ti];
    long late = (long)dw_diff_us(deadline, now);
    t->misses++;
    t->consecutive_misses++;
    if (++t->streak > t->max_consecutive)
        t->max_consecutive = t->streak;
    t->clean_cycles = 0;
    if (late > t->worst_lateness_us)
        t->worst_lateness_us = late;
    dw_core_log(c, now, t->name, "MISS", cycle, late, t->worst_lateness_us, why);
    int acts = dw_core_check(c, ti, now);
    return acts | dw_core_window(c, ti, 1, now);
}

int dw_core_report(dw_core_t *c, int ti, long cycle, const struct timespec *now)
{
    if (ti < 0 || ti >= DW_MAX_TASKS || !c->task[ti].active || cycle < 0)
        return DW_ACT_NONE;
    dw_task_t *t = &c->task[ti];
    int acts = DW_ACT_NONE;

    /* A report for a cycle that has not started yet (more than one period
       ahead of the clock) is bogus: a buggy client could otherwise make
       the loop below log millions of "skipped" misses at the supervisor's
       top priority. Integer-only bound, so a huge value cannot overflow
       the deadline arithmetic. */
    int64_t elapsed_ms = dw_diff_us(&t->epoch, now) / 1000;
    if (elapsed_ms < 0 || cycle > elapsed_ms / t->period_ms + 1)
        return DW_ACT_NONE;

    /* Cycles skipped before this one. The watchdog usually catches them
       first; this is the net. */
    while (t->expected_cycle < cycle) {
        struct timespec d;
        dw_deadline(t, t->expected_cycle, &d);
        acts |= dw_core_miss(c, ti, t->expected_cycle, &d, now, "skipped");
        t->expected_cycle++;
    }
    /* Stale report for a cycle already judged as a miss: ignore it, and
       do not let its lateness leak into worst_lateness_us (the summary
       must be derivable from the logged rows). */
    if (cycle < t->expected_cycle)
        return acts;

    struct timespec d;
    dw_deadline(t, cycle, &d);
    long late = (long)dw_diff_us(&d, now);
    t->cycles++;
    t->total_lateness_us += late;
    if (late > t->worst_lateness_us)
        t->worst_lateness_us = late;
    dw_core_log(c, now, t->name, "CYCLE", cycle, late, t->worst_lateness_us, "");
    t->expected_cycle = cycle + 1;

    if (late > 0) {
        t->misses++;
        t->consecutive_misses++;
        if (++t->streak > t->max_consecutive)
            t->max_consecutive = t->streak;
        t->clean_cycles = 0;
        acts |= dw_core_check(c, ti, now);
        acts |= dw_core_window(c, ti, 1, now);
    } else {
        acts |= dw_core_window(c, ti, 0, now);
        t->consecutive_misses = 0;
        t->streak = 0;
        t->clean_cycles++;
        if (c->throttled && !c->recovered && !c->safe_state &&
            ti == c->trigger_task && t->clean_cycles >= c->recover_cycles) {
            char detail[96];
            c->recovered = 1;
            snprintf(detail, sizeof(detail),
                     "%s clean for %d cycles; targets stay demoted",
                     t->name, c->recover_cycles);
            dw_core_log(c, now, "supervisor", "RECOVER", cycle, 0,
                        t->worst_lateness_us, detail);
        }
    }
    return acts;
}

int dw_core_tick(dw_core_t *c, const struct timespec *now)
{
    int acts = DW_ACT_NONE;
    for (int ti = 0; ti < DW_MAX_TASKS; ti++) {
        dw_task_t *t = &c->task[ti];
        if (!t->active)
            continue;
        for (int guard = 0; guard < 100000; guard++) {
            struct timespec d, g;
            dw_deadline(t, t->expected_cycle, &d);
            dw_add_ms(&d, c->grace_ms, &g);
            if (dw_diff_us(&g, now) <= 0)
                break;  /* not overdue yet */
            acts |= dw_core_miss(c, ti, t->expected_cycle, &d, now, "starved");
            t->expected_cycle++;
        }
    }
    return acts;
}

int dw_core_lost(dw_core_t *c, int ti, const struct timespec *now)
{
    if (ti < 0 || ti >= DW_MAX_TASKS || !c->task[ti].active)
        return DW_ACT_NONE;
    dw_task_t *t = &c->task[ti];
    t->active = 0;
    t->gone = 2;
    dw_core_log(c, now, t->name, "LOST", t->expected_cycle, 0,
                t->worst_lateness_us, "task's process disconnected");
    if (t->role == DW_ROLE_CRITICAL)
        return dw_core_safe(c, ti, now, "lost");
    return DW_ACT_NONE;
}

void dw_core_unregister(dw_core_t *c, int ti, const struct timespec *now)
{
    if (ti < 0 || ti >= DW_MAX_TASKS || !c->task[ti].active)
        return;
    dw_task_t *t = &c->task[ti];
    t->active = 0;
    t->gone = 1;
    dw_core_log(c, now, t->name, "UNREGISTER", t->expected_cycle, 0,
                t->worst_lateness_us, "clean exit");
}
